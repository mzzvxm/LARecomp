#pragma once
//
// --join_server: the launch argument that joins a server by itself (join_server.cpp). Pure (no SDK, no guest, no sockets):
// join_server_core_test.cpp compiles this header alone.
//
//   --join_server=host[:port]                  Cruise on that server: any Cruise session on it, or host one for whoever comes next
//   --join_server=host[:port],cruise           the same
//   --join_server=host[:port],race             any Race session on it, or host one
//   --join_server=host[:port],room:<gamertag>  the room that player hosts (or, if nobody of that name hosts, the room of that name)
//   --join_server=host[:port],<gamertag>       the same, for a text that is not cruise / race
//
// The port is the relay's (default 31000). The game goes online on that server for this run (the same as
// --mcla_online_enable=true --mcla_online_server=host:port, command line only, never written to larecomp.toml) and, once the city
// is up and the pause menu is closed, joins as the server browser's Join button would, with no offer to accept and no 3 minute
// expiry. Everything it finds out is reported in the log ([join_server] lines) and in the log folder's join_server.txt (FormatStatus),
// which the launcher reads.
//
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace larecomp::browser::joinserver {

inline constexpr uint16_t kDefaultRelayPort = 31000;
inline constexpr size_t kMaxRoomText = 32;  // a room name on the wire is 32 bytes, a gamertag 16

enum class Mode { kCruise, kRace, kRoom };

struct Target {
    std::string host;
    uint16_t port = kDefaultRelayPort;
    Mode mode = Mode::kCruise;
    std::string room;  // kRoom: the host's gamertag or the room's name, as typed

    std::string Address() const { return host + ":" + std::to_string(port); }
};

struct ParseResult {
    bool ok = false;
    Target target;
    std::string error;  // a plain sentence, when !ok
};

namespace detail {

constexpr bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

inline std::string_view Trim(std::string_view s) {
    while (!s.empty() && IsSpace(s.front())) s.remove_prefix(1);
    while (!s.empty() && IsSpace(s.back())) s.remove_suffix(1);
    return s;
}

inline char Lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

inline bool EqualsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (Lower(a[i]) != Lower(b[i])) return false;
    return true;
}

inline bool StartsWithNoCase(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && EqualsNoCase(s.substr(0, prefix.size()), prefix);
}

// Letters, digits, '.' and '-' (what the browser accepts as a host); no IPv6 literals.
inline bool ValidHost(std::string_view host) {
    if (host.empty() || host.size() > 253 || host.front() == '.' || host.front() == '-') return false;
    for (char c : host) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
        if (!ok) return false;
    }
    return true;
}

}  // namespace detail

// The text after --join_server=. Never throws; a mistake gives ok = false and a sentence.
inline ParseResult Parse(std::string_view text) {
    using namespace detail;
    ParseResult out;
    text = Trim(text);
    if (text.empty()) {
        out.error = "the join argument is empty (use host:port, or host:port,room:<gamertag>)";
        return out;
    }
    const size_t comma = text.find(',');
    const std::string_view server = Trim(text.substr(0, comma));
    const bool has_mode = comma != std::string_view::npos;
    const std::string_view mode = has_mode ? Trim(text.substr(comma + 1)) : std::string_view();

    const size_t colon = server.find(':');
    if (colon != std::string_view::npos && server.find(':', colon + 1) != std::string_view::npos) {
        out.error = "\"" + std::string(server) + "\" is not host:port (IPv6 addresses are not supported)";
        return out;
    }
    const std::string_view host = server.substr(0, colon);
    if (!ValidHost(host)) {
        out.error = "\"" + std::string(host) + "\" is not a server name or address";
        return out;
    }
    uint32_t port = kDefaultRelayPort;
    if (colon != std::string_view::npos) {
        const std::string_view digits = server.substr(colon + 1);
        port = 0;
        for (char c : digits) {
            if (c < '0' || c > '9' || port > 65535) {
                port = 0;
                break;
            }
            port = port * 10 + uint32_t(c - '0');
        }
        if (digits.empty() || digits.size() > 5 || port == 0 || port > 65535) {
            out.error = "\"" + std::string(digits) + "\" is not a port (1 to 65535)";
            return out;
        }
    }
    out.target.host = std::string(host);
    out.target.port = uint16_t(port);

    if (!has_mode) {
        out.target.mode = Mode::kCruise;
        out.ok = true;
        return out;
    }
    if (mode.empty()) {
        out.error = "nothing after the comma (use cruise, race or room:<gamertag>)";
        return out;
    }
    std::string_view room;
    if (EqualsNoCase(mode, "cruise")) {
        out.target.mode = Mode::kCruise;
    } else if (EqualsNoCase(mode, "race")) {
        out.target.mode = Mode::kRace;
    } else {
        out.target.mode = Mode::kRoom;
        room = StartsWithNoCase(mode, "room:") ? Trim(mode.substr(5)) : mode;
        if (room.empty()) {
            out.error = "room: needs a gamertag after it";
            return out;
        }
        if (room.size() > kMaxRoomText) {
            out.error = "the gamertag or room name is longer than " + std::to_string(kMaxRoomText) + " characters";
            return out;
        }
        for (char c : room) {
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
                out.error = "the gamertag or room name has a control character";
                return out;
            }
        }
        out.target.room = std::string(room);
    }
    out.ok = true;
    return out;
}

// What the target is called in messages: "Cruise on host:port", "Race on host:port", "Hoster's room on host:port".
inline std::string Describe(const Target& t) {
    switch (t.mode) {
    case Mode::kCruise: return "Cruise on " + t.Address();
    case Mode::kRace: return "Race on " + t.Address();
    case Mode::kRoom: return t.room + "'s room on " + t.Address();
    }
    return t.Address();
}

// The word of the target in the status file: cruise, race or room:<x>.
inline std::string Word(const Target& t) {
    switch (t.mode) {
    case Mode::kCruise: return "cruise";
    case Mode::kRace: return "race";
    case Mode::kRoom: return "room:" + t.room;
    }
    return "cruise";
}

// ------------------------------------------------------------------------------------------------------------- rooms ---

struct RoomInfo {
    uint32_t host_vip = 0;
    std::string gamertag;  // the host
    std::string name;      // the room
    int cur_players = 0;
    int max_players = 0;  // 0 = unknown
};

enum class FindStatus { kFound, kNotFound, kFull, kAmbiguous };

struct FindResult {
    FindStatus status = FindStatus::kNotFound;
    size_t index = 0;  // kFound, kFull: the room
};

// The room a --join_server=host,room:<x> means: the one whose HOST's gamertag is x (a player hosts one room, so that is
// unambiguous); only when no host has that gamertag, the one whose room NAME is x. Whole text, ASCII case ignored: a part of a
// name never matches, a join must not land in someone else's room. Two hosts, or two room names, that match are kAmbiguous.
inline FindResult FindRoom(const std::vector<RoomInfo>& rooms, std::string_view query) {
    using namespace detail;
    query = Trim(query);
    FindResult out;
    if (query.empty()) return out;
    const auto pick = [&](bool by_gamertag) {
        size_t hits = 0, at = 0;
        for (size_t i = 0; i < rooms.size(); ++i) {
            if (EqualsNoCase(Trim(by_gamertag ? rooms[i].gamertag : rooms[i].name), query)) {
                ++hits;
                at = i;
            }
        }
        return std::pair<size_t, size_t>(hits, at);
    };
    std::pair<size_t, size_t> found = pick(true);
    if (found.first == 0) found = pick(false);
    if (found.first == 0) return out;
    if (found.first > 1) {
        out.status = FindStatus::kAmbiguous;
        return out;
    }
    out.index = found.second;
    const RoomInfo& r = rooms[found.second];
    out.status = (r.max_players > 0 && r.cur_players >= r.max_players) ? FindStatus::kFull : FindStatus::kFound;
    return out;
}

// ------------------------------------------------------------------------------------------------------ the quiet screen ---
//
// WHY (measured on a loopback test, 2026-11-22: the relay had nothing on it, so the game has to host). A fresh game, Start pressed at
// the title: the game's own state machine is in "Cruising" about 8 s later, but a modal message box (FlashNavigator's FIFO of them, the
// "layer" of the log's `[prompts] screen ->` lines) stays on screen until about 9.1 to 9.7 s after Start. Three --join_server runs began
// their join in that window (8.1 s, 8.6 s and 8.2 s after Start). In each: `goto(LAN)` was taken, `goto(QuickMatchSearching)` returned
// 0 (the page showed "SEARCHING" and nothing else), no search socket was opened, the relay never saw the game search or host, and the
// join ran out of time. Every join that started with no message box up searched and hosted within 20 ms of the goto (four runs: the
// goto returned 1, the game registered with the relay and hosted). A player cannot start a search under that box either: the box
// takes the input first. So --join_server waits until none has been up for a short while.
// CHECK: this is the one difference the logs show at the moment of the goto, not a traced cause. Two other things differ in those runs:
// the game's own profile sync ends 30 s after Start, and the pause screen was never opened before. If a join still does not search
// after this wait, look at those two.
//
// The clock is handed in, so the test needs no game.
class QuietGate {
public:
    static constexpr int64_t kQuietMs = 1500;  // no message box for this long: a box that closes and another that opens right behind it is one wait

    // One call per tick, with whether a message box is up and the time in ms. True once none has been up for kQuietMs.
    bool Update(bool message_up, int64_t now_ms) {
        if (message_up) {
            if (!saw_) first_up_ms_ = now_ms;
            saw_ = true;
            clear_since_ms_ = -1;
            return false;
        }
        if (clear_since_ms_ < 0) clear_since_ms_ = now_ms;
        return now_ms - clear_since_ms_ >= kQuietMs;
    }
    bool SawMessage() const { return saw_; }
    // How long the join has been held back by a message box, as of `now_ms` (0 when none was seen): for the log line.
    int64_t HeldMs(int64_t now_ms) const { return saw_ ? now_ms - first_up_ms_ : 0; }

private:
    bool saw_ = false;
    int64_t first_up_ms_ = 0;
    int64_t clear_since_ms_ = -1;
};

// ----------------------------------------------------------------------------------------------------------- status ---

enum class State {
    kWaiting,     // the game is starting: waiting for the city
    kConnecting,  // asking the server, going online, opening the game's net menu
    kSearching,   // the game is searching for the session
    kJoining,     // the game is joining it
    kDone,        // handed over: the game joined, or hosts a session for the next player
    kFailed,      // reason and message say why
};

inline const char* StateName(State s) {
    switch (s) {
    case State::kWaiting: return "waiting";
    case State::kConnecting: return "connecting";
    case State::kSearching: return "searching";
    case State::kJoining: return "joining";
    case State::kDone: return "done";
    case State::kFailed: return "failed";
    }
    return "failed";
}

// The failure reasons: stable words for the launcher (it maps them to its own text if it wants), each with the sentence the
// game shows and logs. `detail` is the part only the caller knows (a mod list, the number of minutes, the text the join said).
namespace reason {
inline constexpr const char* kBadArgument = "bad_argument";
inline constexpr const char* kOnlineUnavailable = "online_unavailable";
inline constexpr const char* kServerUnreachable = "server_unreachable";
inline constexpr const char* kServerTooOld = "server_too_old";
inline constexpr const char* kModsMissing = "mods_missing";
inline constexpr const char* kRoomNotFound = "room_not_found";
inline constexpr const char* kRoomFull = "room_full";
inline constexpr const char* kRoomAmbiguous = "room_ambiguous";
inline constexpr const char* kWorldTimeout = "world_timeout";
inline constexpr const char* kMessageBox = "message_box";  // the city is up but a message box never closed
inline constexpr const char* kOnlineFailed = "online_failed";
inline constexpr const char* kPauseOpen = "pause_open";
inline constexpr const char* kMenuUnavailable = "menu_unavailable";
inline constexpr const char* kHostNoAnswer = "host_no_answer";
inline constexpr const char* kRoomGone = "room_gone";
// A join is started from free roam only (browser_join_core.h RefuseStart): the game is somewhere the net menu cannot be entered from.
inline constexpr const char* kRaceOn = "race_on";
inline constexpr const char* kGarageOpen = "garage_open";
inline constexpr const char* kInSession = "in_session";
// The search ended and the game is in no session: it neither started one nor joined one (ojnew-cli reported "done" for a search the game never entered).
inline constexpr const char* kNoSession = "no_session";
}  // namespace reason

inline std::string FailureMessage(const std::string& why, const Target& t, const std::string& detail = {}) {
    const std::string server = t.Address();
    if (why == reason::kBadArgument) return "The join argument is not valid: " + detail + ".";
    if (why == reason::kOnlineUnavailable) return "Online play is not available on Linux yet.";
    if (why == reason::kServerUnreachable) return "The server " + server + " did not answer.";
    if (why == reason::kServerTooOld) return "The server " + server + " is too old to list its rooms, so " + t.room + "'s room cannot be found.";
    if (why == reason::kModsMissing) return "The server " + server + " needs mods this game does not have: " + detail + ".";
    if (why == reason::kRoomNotFound) return "No room hosted by " + t.room + " was found on " + server + ".";
    if (why == reason::kRoomFull) return t.room + "'s room on " + server + " is full.";
    if (why == reason::kRoomAmbiguous) return "More than one room on " + server + " is called " + t.room + ": join it by its host's gamertag.";
    if (why == reason::kWorldTimeout) return "The game did not reach the city in " + detail + " minutes, so the join was cancelled.";
    if (why == reason::kMessageBox) return "A message box in the game stayed open, so the join was cancelled.";
    if (why == reason::kOnlineFailed) return "Online play could not be switched on: the server " + server + " did not answer.";
    if (why == reason::kPauseOpen) return "The pause menu stayed open, so the join was cancelled.";
    if (why == reason::kMenuUnavailable) return "The game's online menu could not be opened.";
    if (why == reason::kHostNoAnswer) return "The room did not answer the search on " + server + ".";
    if (why == reason::kRoomGone) return "The room is gone: it closed, is full, or is in another mode.";
    if (why == reason::kRaceOn) return "A race is on: finish or quit it, then join.";
    if (why == reason::kGarageOpen) return "The garage is open: leave it, then join.";
    if (why == reason::kInSession) return "You are already in an online session: leave it from the pause menu first, then join.";
    if (why == reason::kNoSession) return "The game's search did not start or join a session.";
    return detail.empty() ? "The join failed." : detail;
}

struct Status {
    State state = State::kWaiting;
    std::string server;   // host:port
    std::string target;   // Word()
    std::string reason;   // kFailed only
    std::string message;  // a sentence for the player; kFailed, kDone (and a hint while waiting)
};

namespace detail {
// One line: newlines and control characters become spaces, so a value can never start another line.
inline std::string OneLine(std::string_view s) {
    std::string out;
    for (char c : s) out += (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) ? ' ' : c;
    return out;
}
}  // namespace detail

// The text of the log folder's join_server.txt: `key=value` lines, rewritten whole on every change.
//   join_server_status=1
//   state=waiting|connecting|searching|joining|done|failed
//   server=host:port
//   target=cruise|race|room:<x>
//   reason=<word>     (failed only; empty otherwise)
//   message=<sentence>
//   updated=<unix seconds>
// The launcher polls this while the game runs and reads it once more when the game ends: `state` done or failed is final.
inline std::string FormatStatus(const Status& s, int64_t unix_seconds) {
    std::string out;
    out += "join_server_status=1\n";
    out += std::string("state=") + StateName(s.state) + "\n";
    out += "server=" + detail::OneLine(s.server) + "\n";
    out += "target=" + detail::OneLine(s.target) + "\n";
    out += "reason=" + detail::OneLine(s.reason) + "\n";
    out += "message=" + detail::OneLine(s.message) + "\n";
    out += "updated=" + std::to_string(unix_seconds) + "\n";
    return out;
}

// True for the states the launcher takes as the end of the join.
constexpr bool IsFinal(State s) { return s == State::kDone || s == State::kFailed; }

}  // namespace larecomp::browser::joinserver
