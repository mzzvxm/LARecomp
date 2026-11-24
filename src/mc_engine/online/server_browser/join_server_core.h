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
// RESULT (opjrev-pcli, 2026-11-22, with this wait in): the join began 10 s after Start, the box was long gone (it held the join back for
// 2.5 s) and the join failed the same way: goto(QuickMatchSearching) 0, then 1 on the retry, no socket, reason=no_session. So the box
// was not the cause; SearchGate below is the next step.
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
    // The F6 browser's join (browser_join.cpp, kStarting) uses the same gate, except that a join that has seen no message box needs no
    // quiet time: a click is not a launch at Start. Once a box was up it waits for it and then for kQuietMs, as Update does. True = the
    // join may go on. Call Update or Allows on one gate, not both.
    bool Allows(bool message_up, int64_t now_ms) {
        const bool quiet = Update(message_up, now_ms);
        return quiet || (!saw_ && !message_up);
    }
    bool SawMessage() const { return saw_; }
    // How long the join has been held back by a message box, as of `now_ms` (0 when none was seen): for the log line.
    int64_t HeldMs(int64_t now_ms) const { return saw_ ? now_ms - first_up_ms_ : 0; }

private:
    bool saw_ = false;
    int64_t first_up_ms_ = 0;
    int64_t clear_since_ms_ = -1;
};

// ------------------------------------------------------------------------------------ can the game start a search now ---
//
// WHY. The loopback runs of 2026-11-21/22 split cleanly: every join that began 8 to 10 s after Start (all of the --join_server runs:
// ojold-cli, ojnew-cli, opjold-pcli, opjnew-pcli, and opjrev-pcli even with the message box gone) was dropped by the game: goto(LAN) 1,
// goto(QuickMatchSearching) 0 with no search command, a retry that returned 1 and showed the SEARCHING panel, and no socket ever bound
// (no "[mcla-online] peer registered (port N)" after the goto, so the relay never heard the game). Every join that began about 45 s or more
// after Start (the pause-menu scenes) bound its search socket 17 ms after the goto and hosted.
//
// What the game's own search start asks (read in the generated code of native\tree-t11: sub_8226C038, the net manager's quick match, >
// sub_82298F60 > sub_824FD470; the net manager is *0x8286D800). The start returns 0 without a socket, and without a word to anyone, when
//   - the search manager's busy word (*(netmgr+4292)+0) is set, or
//   - the search object (that manager + 528) reports a search in progress (sub_824FBD50: *(object+1436) is a record whose first word is 1), or
//   - for a System Link search (the net mode word *(options+2352) is above 0 after the goto into the LAN page) the search object's
//     handle (*(object+0)) is 0, or
//   - the player's slot (*(netmgr+4284) + 10832 * the local player's index) is not signed in: bit 0x80 of the byte at slot+108
//     (sub_824E70E0), or (for a Live search) not signed in to Live, bit 0x40 (sub_824E70F0).
// None of it is visible in the logs of those runs, and the game's profile sync (14 statistics reads, the prizes fetch, "[rmr] ... identity
// created" and "[tourney] no prizes" 30 s after Start) is still running at the moment of the early joins. SearchFacts is those words;
// SearchGate holds the join until the ones that can be true before the goto hold for a moment, and gives up after a minute so that a
// reading that is wrong in some game build cannot keep the join from trying. The search object's handle is logged but not required: the
// constructor (sub_824FC5A0) stores 0 in it and no store of it was found in the code read, so it may only be set by the page's own entry.
// CHECK: none of this was run. The words were read from the generated code, not watched; the join logs them (join_server.cpp) at the gate,
// every 250 ms to 1 s for the first 30 s of a join with the debug setting online_diag on, and once when the join ends in a failure. If the
// first run after this shows the gate open at once and the search still not starting, the cause is not among these words: read the
// samples to see which of them change at the moment the search is entered (net_mode first).
//
// The clock is handed in, so the test needs no game.
struct SearchFacts {
    bool read = false;             // every word below was read
    const char* failed_at = "";    // when !read: the first read that failed, for the log
    uint32_t search_flag = 0;      // the search manager's busy word, *(netmgr+4292)+0
    uint32_t handle = 0;           // the search object's handle, *(search manager+528)+0 (logged, not required: see SearchGate)
    uint32_t busy_record = 0;      // *(search object+1436): the record sub_824FBD50 looks at (0 before the first search)
    uint32_t busy_state = 0;       // that record's first word: 1 = a search in progress
    uint32_t player_slot = 0;      // the local player's index (the slot is *(netmgr+4284) + 10832 * this)
    uint32_t player_flags = 0;     // the byte at slot+108: 0x80 signed in, 0x40 signed in to Live
    int32_t net_mode = 0;          // *(*(*(netmgr+4304))+2352): above 0 = a System Link search (read before the goto into the page: usually 0)
    uint32_t session_state = 0;    // the net session manager's +696: 3 once the game's quick match has started a search
};

inline bool SearchBusy(const SearchFacts& f) { return f.busy_record != 0 && f.busy_state == 1; }
inline bool PlayerSignedIn(const SearchFacts& f) { return (f.player_flags & 0x80u) != 0; }
inline bool PlayerOnLive(const SearchFacts& f) { return (f.player_flags & 0x40u) != 0; }

// What can be true before the goto into the net page: nothing is running and the player is signed in. (net_mode and the handle are set
// by the page's own entry, as far as the code read says; the Live bit is not needed for a System Link search.)
inline bool SearchReady(const SearchFacts& f) {
    return f.read && f.search_flag == 0 && !SearchBusy(f) && PlayerSignedIn(f);
}

// The first thing that stands in the way, in the words the log uses ("" when nothing does).
inline std::string SearchMissing(const SearchFacts& f) {
    if (!f.read) return std::string("not readable: ") + (f.failed_at ? f.failed_at : "?");
    if (f.search_flag != 0) return "a search is already marked as running";
    if (SearchBusy(f)) return "the search object is busy";
    if (!PlayerSignedIn(f)) return "the player is not signed in to the network layer yet";
    return "";
}

namespace detail {
inline std::string Hex(uint32_t v) {
    static const char* digits = "0123456789ABCDEF";
    std::string s = "0x";
    bool started = false;
    for (int shift = 28; shift >= 0; shift -= 4) {
        const uint32_t d = (v >> shift) & 0xFu;
        if (d != 0 || started || shift == 0) {
            s += digits[d];
            started = true;
        }
    }
    return s;
}
}  // namespace detail

// One line of words for the log.
inline std::string DescribeSearchFacts(const SearchFacts& f) {
    using detail::Hex;
    if (!f.read) return std::string("not readable: ") + (f.failed_at ? f.failed_at : "?");
    return "search_flag=" + Hex(f.search_flag) + " busy=" + (SearchBusy(f) ? "1" : "0") + " (record " + Hex(f.busy_record) + " state " +
           Hex(f.busy_state) + ") handle=" + Hex(f.handle) + " player " + std::to_string(f.player_slot) + " flags " + Hex(f.player_flags) +
           " (signed_in " + (PlayerSignedIn(f) ? "1" : "0") + ", live " + (PlayerOnLive(f) ? "1" : "0") + ") net_mode=" +
           std::to_string(f.net_mode) + " session_state=" + std::to_string(f.session_state) + " ready=" + (SearchReady(f) ? "1" : "0");
}

class SearchGate {
public:
    static constexpr int64_t kStableMs = 500;       // the words must hold this long without a break
    static constexpr int64_t kMaxWaitMs = 60 * 1000;  // then the join goes on whatever they say: the game's own answer is the last word

    enum class Verdict {
        kWait,    // not yet
        kReady,   // they hold: go on
        kGaveUp,  // kMaxWaitMs passed: go on anyway (the caller logs what was missing)
    };

    // One call per tick while the join is otherwise ready.
    Verdict Update(const SearchFacts& f, int64_t now_ms) {
        if (first_ms_ < 0) first_ms_ = now_ms;
        if (SearchReady(f)) {
            if (ready_since_ms_ < 0) ready_since_ms_ = now_ms;
            return now_ms - ready_since_ms_ >= kStableMs ? Verdict::kReady : Verdict::kWait;
        }
        ready_since_ms_ = -1;
        if (now_ms - first_ms_ >= kMaxWaitMs) return Verdict::kGaveUp;
        return Verdict::kWait;
    }
    // How long the gate has been looking, as of `now_ms` (0 before the first call): for the log line.
    int64_t WaitedMs(int64_t now_ms) const { return first_ms_ < 0 ? 0 : now_ms - first_ms_; }

private:
    int64_t first_ms_ = -1;
    int64_t ready_since_ms_ = -1;
};

// ------------------------------------------------------------------------------------------- the pause menu comes first ---
//
// WHY (opjrev-pcli against opjrev-ps-lan and opjrev-ps-live, 2026-11-22, one exe: fix/online-review 043ea9e3). The search gate opened at once
// (search_flag 0, busy 0, signed in 1, ready 1), goto(LAN) returned 1 and set net_mode 1, and goto(QuickMatchSearching) still returned 0; the
// retry returned 1 and posted the search panel (flash command E51), and the game never started the search (no socket, no REGISTER at the
// relay, no_session after 45 s). The joins that worked, from the same exe, began with the pause screen, the paused flag and the pause movie
// up (the log line "join asked for: pause screen 1, paused flag 1, pause movie 1, activity free roam"): the browser's join found the pause
// menu open, closed it the way CONTINUE does ("closed first, then the goto", the live scene) or searched from the page it stood on (the lan
// scene), and the game searched 17 ms after the goto. The failing join began with no pause menu ("... pause screen 0, paused flag 0, pause
// movie 0"), in a game that had never opened it; the game also booted online. Nothing else the logs show differs once the message box and
// the start-up work are out of the way.
//
// So --join_server does what the working runs had done before their join: it opens the pause menu the way a player does (Start, in free
// roam), waits until it is up, and only then files its join, which finds the pause menu open and does what the working live scene's did: the
// pause gate (browser_join_core.h) gives a menu found open its settle time, closes it, waits for the game to run again, and goes to the net
// menu and the search from free roam. The press is the pad's Start (input/synthetic_start.h), put into the state of the game's own poll: the
// same reading the game makes of a player's Start, and the one the test harness's pad uses in the runs that worked.
// CHECK: not run in a game. That the pause menu opened this way is the one the working runs had, and that it is what lets the search start,
// is the reading of the logs above (hypothesis (b) of JOIN-SERVER-ROOT.md); the run says.
//
// This is the order of those steps, with the clock handed in, so that the test needs no game. Per tick, with what the caller sees:
//   up         the pause menu is on screen (the pause movie is the UI's current one and a menu screen is up);
//   can_press  nothing stands in the way of a Start press: the game is in free roam and no message box is on screen (the box takes the
//              input first, and a join under it was never started).
// The step is kPress when a Start press should be made now, kReady once the menu has stayed up for kShownMs (its page is loaded and has
// been drawn: closing a menu in its first frames meets the movie's load), kGaveUp when it never came (the join then goes on as it did
// before this step, from free roam: the caller logs it), kWait otherwise. A press that did not open the menu within kAttemptMs is made
// again, kMaxPresses in all.
class PauseOpener {
public:
    enum class Step {
        kWait,    // not yet
        kPress,   // press Start now
        kReady,   // the menu is up and has settled: file the join
        kGaveUp,  // it did not open: file the join anyway
    };
    static constexpr int64_t kShownMs = 1500;         // the menu must stay up this long before the join is filed
    static constexpr int64_t kAttemptMs = 3000;       // a press that opened nothing is repeated after this long
    static constexpr int kMaxPresses = 3;
    static constexpr int64_t kMaxWaitMs = 30 * 1000;  // from the first call: the whole step is given up after this

    // One call per tick while the step runs.
    Step Update(bool up, bool can_press, int64_t now_ms) {
        if (first_ms_ < 0) first_ms_ = now_ms;
        if (up) {
            if (up_since_ms_ < 0) up_since_ms_ = now_ms;
            return now_ms - up_since_ms_ >= kShownMs ? Step::kReady : Step::kWait;
        }
        up_since_ms_ = -1;  // closed again (the player's own close, a flicker): the time it must stay up starts over
        if (now_ms - first_ms_ >= kMaxWaitMs) return Step::kGaveUp;
        if (presses_ >= kMaxPresses) return now_ms >= next_press_ms_ ? Step::kGaveUp : Step::kWait;
        if (!can_press) return Step::kWait;
        if (presses_ == 0 || now_ms >= next_press_ms_) {
            ++presses_;
            next_press_ms_ = now_ms + kAttemptMs;
            return Step::kPress;
        }
        return Step::kWait;
    }
    int Presses() const { return presses_; }
    // How long the step has been going, as of `now_ms` (0 before the first call): for the log line.
    int64_t WaitedMs(int64_t now_ms) const { return first_ms_ < 0 ? 0 : now_ms - first_ms_; }

private:
    int64_t first_ms_ = -1;
    int64_t up_since_ms_ = -1;
    int64_t next_press_ms_ = 0;
    int presses_ = 0;
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
