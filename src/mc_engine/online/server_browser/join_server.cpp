#include "join_server.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "discord_rpc/discord_rpc.h"  // RpcInFreeRoam: the game's state machine is in Cruising
#include "settings/larecomp_settings.h"
#include "spdlog_console.h"  // LarecompLogFolder()

#include "../online_platform.h"
#include "../../modloader/modsbrowser/mod_download.h"
#include "browser_client.h"
#include "browser_join.h"
#include "browser_ui.h"
#include "join_server_core.h"
#include "larecomp_log.h"

namespace larecomp::browser {
namespace {

namespace js = joinserver;
using Clock = std::chrono::steady_clock;

// The game gets this long to reach the city (a first start that builds shaders, a slow disk): the only time limit of the join.
constexpr auto kCityWait = std::chrono::minutes(20);
// The relay's details are asked for this many times, this far apart, before the server counts as down.
constexpr int kServerTries = 3;
constexpr auto kServerRetry = std::chrono::seconds(2);
// A room that is not listed yet (the friend is still starting) is waited for this long, asking again this often.
constexpr auto kRoomWait = std::chrono::seconds(45);
constexpr auto kRoomPoll = std::chrono::seconds(3);
// The worker's whole run is 3 asks of the relay, the mods and up to 45 s for a room. One that is still going after this (a relay
// that answers a byte at a time, a name that never resolves) is given up on: the join fails and the thread ends on its own.
// CHECK: not run against a stalled relay; 3 minutes is far above the worker's own limits (about a minute) and was not measured.
constexpr auto kResolveLimit = std::chrono::minutes(3);

const char* kStatusFile = "join_server.txt";

enum class Phase {
    kUnread,     // the first tick has not looked at the argument yet
    kOff,        // no --join_server in this run (or it is not allowed here)
    kWaitCity,   // waiting for the city
    kResolving,  // the worker asks the relay
    kJoining,    // the browser's join runs; it reports through JoinServerNoteFinish
    kEnded,      // done or failed, final
};

// Guest thread only, except where a mutex says otherwise.
Phase g_phase = Phase::kUnread;
js::Target g_target;
Clock::time_point g_started;
js::Status g_status;
bool g_status_known = false;
bool g_open_browser = false;  // a failure: the browser opens with the sentence once the city is up

std::mutex g_failure_mutex;
std::string g_failure_text;  // guarded

// What the worker found out. It is written once, before `done` is set.
struct Job {
    std::atomic<bool> done{false};
    bool ok = false;
    std::string why;      // a joinserver::reason word, when !ok
    std::string detail;   // the part of the sentence only the worker knows (the mods)
    uint32_t vip = 0;     // kRoom
    std::vector<Prop> props;
    std::string label;    // "Hoster @ Server name" / the server's name
};
std::shared_ptr<Job> g_job;
Clock::time_point g_resolve_started;

int64_t UnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::filesystem::path StatusPath() { return LarecompLogFolder() / kStatusFile; }

// The file is rewritten whole beside itself and moved over the old one, so the launcher never reads half of it. A failure to write
// costs the launcher its feedback, not the join.
void WriteStatusFile() {
    try {
        std::error_code ec;
        const std::filesystem::path path = StatusPath();
        std::filesystem::create_directories(path.parent_path(), ec);
        std::filesystem::path temp = path;  // not path.string() + ".tmp": on Windows string() is the ANSI code page, which loses a
        temp += ".tmp";                      // user name outside it (Cyrillic, Chinese) and the file could never be written
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out) return;
            out << js::FormatStatus(g_status, UnixSeconds());
        }
        std::filesystem::rename(temp, path, ec);
        if (ec) LARECOMP_APP_WARN("[join_server] could not write {}: {}", path.string(), ec.message());
    } catch (const std::exception& e) {
        LARECOMP_APP_WARN("[join_server] could not write the status file: {}", e.what());
    }
}

// A new state, or the same state with other words: logged, written to the file. Final states are not left again.
void SetState(js::State state, std::string reason, std::string message) {
    if (js::IsFinal(g_status.state) && g_status_known) return;
    if (g_status_known && g_status.state == state && g_status.message == message) return;
    g_status.state = state;
    g_status.server = g_target.host.empty() ? std::string() : g_target.Address();  // empty when the argument did not parse
    g_status.target = g_target.host.empty() ? std::string() : js::Word(g_target);
    g_status.reason = std::move(reason);
    g_status.message = std::move(message);
    g_status_known = true;
    if (state == js::State::kFailed)
        LARECOMP_APP_WARN("[join_server] FAILED reason={}: {}", g_status.reason, g_status.message);
    else if (state == js::State::kDone)
        LARECOMP_APP_INFO("[join_server] DONE: {}", g_status.message);
    else
        LARECOMP_APP_INFO("[join_server] state={}: {}", js::StateName(state), g_status.message);
    WriteStatusFile();
}

void Fail(const std::string& why, const std::string& message) {
    g_phase = Phase::kEnded;
    SetState(js::State::kFailed, why, message);
    {
        std::lock_guard<std::mutex> lk(g_failure_mutex);
        g_failure_text = message;
    }
    ShowJoinFailure(message);
    g_open_browser = true;
}

// The same, with the sentence from join_server_core.h; `detail` is the part only the caller knows.
void FailWith(const std::string& why, const std::string& detail = {}) {
    Fail(why, js::FailureMessage(why, g_target, detail));
}

std::string ServerLabel(const ServerView& v) { return v.name.empty() ? v.address : v.name; }

// Waits `d` unless the game is going away (the process ends, the detached thread with it).
void Sleep(std::chrono::milliseconds d) { std::this_thread::sleep_for(d); }

// The worker: asks the relay, checks the mods, finds the room. Network and disk only: no guest memory (the room's properties are
// read by the guest thread afterwards).
void Resolve(std::shared_ptr<Job> job, js::Target t) {
    const auto finish = [&](bool ok, const char* why, std::string detail) {
        job->ok = ok;
        job->why = why ? why : "";
        job->detail = std::move(detail);
        job->done.store(true, std::memory_order_release);
    };

    ServerView v;
    for (int attempt = 0;; ++attempt) {
        v = QueryServerNow(t.host, t.port);
        if (v.answered) break;
        if (attempt + 1 >= kServerTries) return finish(false, js::reason::kServerUnreachable, {});
        Sleep(std::chrono::duration_cast<std::chrono::milliseconds>(kServerRetry));
    }
    LARECOMP_APP_INFO("[join_server] {} answers: {} players, {} rooms, ping {} ms", t.Address(), v.players, v.rooms, v.ping_ms);

    // Required mods the game does not have: joining without them is a session that cannot work. The launcher installs them
    // before the start; a server that changed since is caught here, and the browser's mods window takes it from there.
    if (v.http_port && (v.mods_required + v.mods_optional) > 0) {
        const ModCheck check = CheckServerMods(t.host, v.http_port);
        if (!check.ok) {
            LARECOMP_APP_WARN("[join_server] the mods of {} could not be read ({}); joining anyway", t.Address(), check.error);
        } else if (!check.RequiredReady()) {
            std::string names;
            for (const ServerMod& m : check.mods) {
                if (!m.required || m.state == ModState::kInstalled) continue;
                names += (names.empty() ? "" : ", ") + m.name;
            }
            return finish(false, js::reason::kModsMissing, names);
        }
    }

    job->label = ServerLabel(v);
    if (t.mode != js::Mode::kRoom) return finish(true, nullptr, {});

    if (v.legacy) return finish(false, js::reason::kServerTooOld, {});
    const auto until = Clock::now() + kRoomWait;
    for (;;) {
        std::vector<js::RoomInfo> rooms;
        rooms.reserve(v.room_list.size());
        for (const RoomView& r : v.room_list)
            rooms.push_back({r.host_vip, r.gamertag, r.name, r.cur_players, r.max_players});
        const js::FindResult found = js::FindRoom(rooms, t.room);
        if (found.status == js::FindStatus::kAmbiguous) return finish(false, js::reason::kRoomAmbiguous, {});
        if (found.status == js::FindStatus::kFull) return finish(false, js::reason::kRoomFull, {});
        if (found.status == js::FindStatus::kFound) {
            const RoomView& room = v.room_list[found.index];
            job->vip = room.host_vip;
            job->props = room.props;
            job->label = (room.gamertag.empty() ? room.name : room.gamertag) + " @ " + ServerLabel(v);
            // The relay puts this host first in the list the game's Live search gets (as the window's Join does).
            SendTargetHint(t.host, t.port, room.host_vip, 800);
            return finish(true, nullptr, {});
        }
        if (Clock::now() >= until) return finish(false, js::reason::kRoomNotFound, {});
        Sleep(std::chrono::duration_cast<std::chrono::milliseconds>(kRoomPoll));
        const ServerView again = QueryServerNow(t.host, t.port);
        if (again.answered) v = again;
    }
}

void ReadArgument() {
    g_phase = Phase::kOff;
    const std::string text = larecomp::cvar::GetFlagByName("join_server");
    if (text.empty()) {
        // No join this run: a status file of an earlier run is not this run's answer.
        std::error_code ec;
        std::filesystem::remove(StatusPath(), ec);
        return;
    }
    if (larecomp::cvar::GetFlagSource("join_server") != larecomp::cvar::Source::kCommandLine) {
        LARECOMP_APP_WARN("[join_server] ignored: --join_server is a command line option, larecomp.toml and the environment do not take it");
        std::error_code ec;
        std::filesystem::remove(StatusPath(), ec);
        return;
    }
    g_started = Clock::now();
    const js::ParseResult parsed = js::Parse(text);
    if (parsed.ok) g_target = parsed.target;
    if (!parsed.ok) {
        LARECOMP_APP_WARN("[join_server] --join_server=\"{}\" is not valid: {}", text, parsed.error);
        FailWith(js::reason::kBadArgument, parsed.error);
        return;
    }
    if (!larecomp::kOnlineAvailable) {
        LARECOMP_APP_WARN("[join_server] --join_server=\"{}\": {}", text, larecomp::kOnlineUnavailableText);
        FailWith(js::reason::kOnlineUnavailable);
        return;
    }
    g_phase = Phase::kWaitCity;
    SetState(js::State::kWaiting, "",
             "Waiting for you to press Start and the game to reach the city, to join: " + js::Describe(g_target) + " (up to " +
                 std::to_string(std::chrono::duration_cast<std::chrono::minutes>(kCityWait).count()) + " minutes).");
}

// The browser's join started by this argument: the request for the worker's answer.
void StartJoin(const Job& job) {
    JoinRequest req;
    req.label = job.label;
    req.lan = true;  // System Link, as "Play on this server" does: the path the relay has carried longest
    if (g_target.mode == js::Mode::kRoom) {
        req.host_vip = job.vip;
        bool cruise = false, lan = true;
        ClassifyRoom(job.props, &cruise, &lan);  // the game's own property names; empty until its table is filled
        req.cruise = cruise;
        req.lan = lan;
    } else {
        req.any_room = true;
        req.cruise = g_target.mode == js::Mode::kCruise;
    }
    LARECOMP_APP_INFO("[join_server] joining {}", req.label);
    RequestJoin(req);
    g_phase = Phase::kJoining;
    SetState(js::State::kConnecting, "", "Joining " + js::Describe(g_target) + ".");
}

}  // namespace

void JoinServerTick() {
    if (g_phase == Phase::kUnread) ReadArgument();
    if (g_open_browser && WorldReady()) {
        g_open_browser = false;
        ServerBrowserOpen();
    }
    switch (g_phase) {
    case Phase::kWaitCity:
        // The city AND the player's car (WorldReady), and the game's own state machine in "Cruising": not the title screen, not the
        // loading screen after Start. The measured crash of a join that began at the title (browser_join_core.h) was the first
        // of these missing; the second keeps the join from a state the machine leaves for the net menu half-built.
        if (WorldReady() && RpcInFreeRoam()) {
            LARECOMP_APP_INFO("[join_server] the city is up after {} s: asking {}",
                              std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - g_started).count(),
                              g_target.Address());
            g_job = std::make_shared<Job>();
            g_resolve_started = Clock::now();
            std::thread(Resolve, g_job, g_target).detach();
            g_phase = Phase::kResolving;
            SetState(js::State::kConnecting, "", "Asking " + g_target.Address() + ".");
        } else if (Clock::now() - g_started > kCityWait) {
            FailWith(js::reason::kWorldTimeout,
                     std::to_string(std::chrono::duration_cast<std::chrono::minutes>(kCityWait).count()));
        }
        break;

    case Phase::kResolving:
        if (g_job && g_job->done.load(std::memory_order_acquire)) {
            const std::shared_ptr<Job> job = std::move(g_job);
            g_job.reset();
            if (job->ok)
                StartJoin(*job);
            else
                FailWith(job->why, job->detail);
        } else if (g_job && Clock::now() - g_resolve_started > kResolveLimit) {
            g_job.reset();  // the worker holds its own reference and its answer is dropped
            LARECOMP_APP_WARN("[join_server] asking {} took more than {} minutes: the join is cancelled", g_target.Address(),
                              std::chrono::duration_cast<std::chrono::minutes>(kResolveLimit).count());
            FailWith(js::reason::kServerUnreachable);
        }
        break;

    case Phase::kJoining: {
        // The browser's join runs; failure and the end come through JoinServerNoteFinish.
        const JoinStatus st = GetJoinStatus();
        if (st.phase == JoinPhase::kSearching)
            SetState(js::State::kSearching, "", "Searching for " + js::Describe(g_target) + ".");
        else if (st.phase == JoinPhase::kJoining)
            SetState(js::State::kJoining, "", "Joining " + js::Describe(g_target) + ".");
        break;
    }

    default:
        break;
    }
}

void JoinServerNoteFinish(bool failed, const std::string& reason, const std::string& text) {
    if (g_phase != Phase::kJoining) return;
    if (failed) {
        // The sentences of the join's own steps; the ones that name the server come from the core.
        const bool known = reason == js::reason::kOnlineFailed || reason == js::reason::kHostNoAnswer ||
                           reason == js::reason::kPauseOpen || reason == js::reason::kMenuUnavailable ||
                           reason == js::reason::kRoomGone;
        Fail(reason, known ? js::FailureMessage(reason, g_target) : text);
        return;
    }
    g_phase = Phase::kEnded;
    SetState(js::State::kDone, "",
             g_target.mode == js::Mode::kRoom
                 ? "The game joined " + js::Describe(g_target) + "."
                 : "The game joined a session on " + g_target.Address() + ", or started one for the next player.");
}

std::string JoinServerFailureText() {
    std::lock_guard<std::mutex> lk(g_failure_mutex);
    return g_failure_text;
}

void JoinServerClearFailure() {
    std::lock_guard<std::mutex> lk(g_failure_mutex);
    g_failure_text.clear();
}

}  // namespace larecomp::browser
