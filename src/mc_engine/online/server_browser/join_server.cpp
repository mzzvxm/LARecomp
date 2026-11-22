#include "join_server.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "discord_rpc/discord_rpc.h"  // RpcInFreeRoam: the game's state machine is in Cruising
#include "mc_engine/input/synthetic_start.h"  // PressPadStart: the pause menu is opened the way a player does
#include "mc_engine/keyboard_native.h"           // MenuScreenUp, CurrentMovieName: the pause menu is on screen
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
    kOpenPause,  // the pause menu is opened the way a player does, before the join is filed (join_server_core.h, PauseOpener)
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
js::QuietGate g_quiet;        // no message box on screen for a short while (the join waits for it, join_server_core.h)
bool g_quiet_logged = false;  // the "message box is still up" line was written
js::SearchGate g_search_gate;  // the game can start a search (the words its own start asks about, join_server_core.h, SearchGate)
bool g_search_wait_logged = false;  // the "cannot start a search yet" line was written
Clock::time_point g_join_started;   // the browser's join was requested: the probe samples count from here
int64_t g_probe_due_ms = 0;         // the next probe sample, in ms after g_join_started
js::PauseOpener g_pause_opener;     // the pause menu is opened before the join is filed (join_server_core.h, PauseOpener)

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
    g_join_started = Clock::now();
    g_probe_due_ms = 0;
    RequestJoin(req);
    g_phase = Phase::kJoining;
    SetState(js::State::kConnecting, "", "Joining " + js::Describe(g_target) + ".");
}

int64_t SteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

// The pause menu is on screen: the UI's current movie is the pause movie and a menu screen is up (keyboard_native's tracker, as of the
// last input poll: the same facts the prompts' "screen -> form+movie PAUSEMOVIE" log line is made of). Any thread.
bool PauseMenuShown() {
    if (!larecomp::keyboard::GameEntered() || !larecomp::keyboard::MenuScreenUp()) return false;
    char movie[32] = {};
    larecomp::keyboard::CurrentMovieName(movie, sizeof(movie));
    return std::strcmp(movie, "PAUSEMOVIE") == 0;
}

// The step between the worker's answer and the browser's join: the pause menu is opened the way a player does (Start in free roam), and
// the join is filed once it has been up for a moment (join_server_core.h, PauseOpener). The join then finds the menu open, closes it the
// way CONTINUE does and goes to the net menu and the search from free roam: the sequence of the pause-menu joins that worked.
void OpenPauseTick() {
    const int64_t now_ms = SteadyMs();
    // Free roam and no message box: a Start press goes nowhere under a box, and not in a race or a load. (A join in a session the game is
    // in already is refused by the browser's own join.)
    const bool can_press = WorldReady() && RpcInFreeRoam() && !ModalMessageUp();
    const bool up = PauseMenuShown();
    const js::PauseOpener::Step step = g_pause_opener.Update(up, can_press, now_ms);
    switch (step) {
    case js::PauseOpener::Step::kPress:
        LARECOMP_APP_INFO("[join_server] opening the pause menu the way a player does, before the join (Start, press {} of up to {}): the pause menu was open "
                          "in every join that worked",
                          g_pause_opener.Presses(), js::PauseOpener::kMaxPresses);
        larecomp::input::PressPadStart();
        break;
    case js::PauseOpener::Step::kReady:
    case js::PauseOpener::Step::kGaveUp: {
        if (step == js::PauseOpener::Step::kReady)
            LARECOMP_APP_INFO("[join_server] the pause menu is open (after {} ms, {} press(es)): the join starts from it", g_pause_opener.WaitedMs(now_ms),
                              g_pause_opener.Presses());
        else
            LARECOMP_APP_WARN("[join_server] the pause menu did not open ({} press(es), {} ms; free roam {}): the join goes on without it",
                              g_pause_opener.Presses(), g_pause_opener.WaitedMs(now_ms), can_press ? "and no message box" : "not reached or a message box up");
        const std::shared_ptr<Job> job = std::move(g_job);
        g_job.reset();
        if (job) StartJoin(*job);
        break;
    }
    case js::PauseOpener::Step::kWait:
        break;
    }
}

const char* JoinPhaseName(JoinPhase p) {
    switch (p) {
    case JoinPhase::kIdle: return "idle";
    case JoinPhase::kGoingOnline: return "going online";
    case JoinPhase::kStarting: return "starting";
    case JoinPhase::kSearching: return "searching";
    case JoinPhase::kJoining: return "joining";
    case JoinPhase::kFailed: return "failed";
    }
    return "?";
}

// The words the game's search start asks about, sampled while the browser's join runs (debug setting online_diag, which the loopback chain
// sets for this run): every 250 ms for the first 6 s after the join was requested (the goto into the page and into the search are in there),
// then every second up to 30 s. The sample at which net_mode, session_state or the busy words change is the moment the game took or
// dropped the search (join_server_core.h, SearchFacts). Guest thread.
void ProbeSearch(JoinPhase phase) {
    if (!larecomp::settings::online_diag.Get()) return;
    const int64_t since = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_join_started).count();
    if (since < g_probe_due_ms || since > 30000) return;
    g_probe_due_ms = since + (since < 6000 ? 250 : 1000);
    LARECOMP_APP_INFO("[join_server] probe +{} ms ({}): {}", since, JoinPhaseName(phase), js::DescribeSearchFacts(ReadSearchFacts()));
}

}  // namespace

void JoinServerTick() {
    if (g_phase == Phase::kUnread) ReadArgument();
    if (g_open_browser && WorldReady()) {
        g_open_browser = false;
        ServerBrowserOpen();
    }
    switch (g_phase) {
    case Phase::kWaitCity: {
        // The city AND the player's car (WorldReady), and the game's own state machine in "Cruising": not the title screen, not the
        // loading screen after Start. The measured crash of a join that began at the title (browser_join_core.h) was the first
        // of these missing; the second keeps the join from a state the machine leaves for the net menu half-built.
        // RpcInFreeRoam is guest code (the "Cruising" activation hook), so it is the same on every renderer, the hidden XENIA one
        // (gpu.driver d3d12/vulkan) included. It is false only in the link stubs of the XEO3 DLL (the Microsoft emulator's target,
        // REXGLUE_HAS_XEO3_TARGET, not buildable since rexglue 0.11): its Patch_DeltaTimePre is empty, so this tick never runs there
        // and --join_server is never read, rather than waiting out kCityWait.
        // And no message box on screen: the first one after Start is still up when the machine reaches "Cruising", and a join that
        // went into the net menu under it never got its search going (join_server_core.h, QuietGate).
        const bool in_city = WorldReady() && RpcInFreeRoam();
        if (!in_city) {
            g_quiet = js::QuietGate{};  // a load or the title again: the quiet time counts from the city's return
            g_search_gate = js::SearchGate{};
        }
        const bool message_up = in_city && ModalMessageUp();
        const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
        // And the game able to start a search: the join that began 8 to 10 s after Start was dropped by the game with no search socket,
        // while every join 45 s or more after Start searched (join_server_core.h, SearchGate). Checked once the screen is quiet.
        bool ready = false;
        if (in_city && g_quiet.Update(message_up, now_ms)) {
            const js::SearchFacts facts = ReadSearchFacts();
            const js::SearchGate::Verdict verdict = g_search_gate.Update(facts, now_ms);
            if (verdict == js::SearchGate::Verdict::kWait) {
                if (!g_search_wait_logged) {
                    g_search_wait_logged = true;
                    LARECOMP_APP_INFO("[join_server] the city is up after {} s, but the game cannot start a search yet ({}): the join waits for it: {}",
                                      std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - g_started).count(),
                                      js::SearchMissing(facts), js::DescribeSearchFacts(facts));
                }
            } else {
                ready = true;
                if (verdict == js::SearchGate::Verdict::kGaveUp)
                    LARECOMP_APP_WARN("[join_server] the game could not start a search for {} s ({}): asking anyway: {}",
                                      g_search_gate.WaitedMs(now_ms) / 1000, js::SearchMissing(facts), js::DescribeSearchFacts(facts));
                else if (g_search_wait_logged)
                    LARECOMP_APP_INFO("[join_server] the game can start a search now (the join waited {} ms for it): {}", g_search_gate.WaitedMs(now_ms),
                                      js::DescribeSearchFacts(facts));
            }
        }
        if (ready) {
            const int64_t held_ms = g_quiet.HeldMs(now_ms);
            LARECOMP_APP_INFO("[join_server] the city is up after {} s{}: asking {}",
                              std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - g_started).count(),
                              g_quiet.SawMessage() ? " (a message box held the join back for " + std::to_string(held_ms) + " ms)" : std::string(),
                              g_target.Address());
            g_job = std::make_shared<Job>();
            g_resolve_started = Clock::now();
            std::thread(Resolve, g_job, g_target).detach();
            g_phase = Phase::kResolving;
            SetState(js::State::kConnecting, "", "Asking " + g_target.Address() + ".");
        } else if (Clock::now() - g_started > kCityWait) {
            if (in_city && g_quiet.SawMessage())
                FailWith(js::reason::kMessageBox);
            else
                FailWith(js::reason::kWorldTimeout,
                         std::to_string(std::chrono::duration_cast<std::chrono::minutes>(kCityWait).count()));
        } else if (message_up && !g_quiet_logged) {
            g_quiet_logged = true;
            LARECOMP_APP_INFO("[join_server] the city is up, but a message box is still on screen: the join waits for it to close");
        }
        break;
    }

    case Phase::kResolving:
        if (g_job && g_job->done.load(std::memory_order_acquire)) {
            if (g_job->ok) {
                // The relay answered. Before the join is filed the pause menu is opened, as every join that worked had it open; the
                // answer stays in g_job until then.
                g_pause_opener = js::PauseOpener{};
                g_phase = Phase::kOpenPause;
            } else {
                const std::shared_ptr<Job> job = std::move(g_job);
                g_job.reset();
                FailWith(job->why, job->detail);
            }
        } else if (g_job && Clock::now() - g_resolve_started > kResolveLimit) {
            g_job.reset();  // the worker holds its own reference and its answer is dropped
            LARECOMP_APP_WARN("[join_server] asking {} took more than {} minutes: the join is cancelled", g_target.Address(),
                              std::chrono::duration_cast<std::chrono::minutes>(kResolveLimit).count());
            FailWith(js::reason::kServerUnreachable);
        }
        break;

    case Phase::kOpenPause:
        OpenPauseTick();
        break;

    case Phase::kJoining: {
        // The browser's join runs; failure and the end come through JoinServerNoteFinish.
        const JoinStatus st = GetJoinStatus();
        ProbeSearch(st.phase);
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
        // A join that ended without a search: the words the game's search start asks about, as they are now (the join's own log lines
        // say what was asked of the game; this says what the game saw).
        LARECOMP_APP_INFO("[join_server] the search facts when the join failed ({}): {}", reason, js::DescribeSearchFacts(ReadSearchFacts()));
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
