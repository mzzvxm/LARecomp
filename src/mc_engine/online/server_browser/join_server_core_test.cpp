// A check that needs no game and no server: the --join_server argument, the room match and the status file text
// (join_server_core.h). Build:
//   cmake --build <build dir> --target larecomp_join_server_test, then run it; exit 0 = passed.
//
// LOCAL TRIO CHECK of the join itself (main session, one game at a time; needs the relay, a hoster and the save mzlate):
//   larecomp.exe --join_server=127.0.0.1:31000,room:Hoster
//   1. The log has, in order: [join_server] ... waiting for the city; ... state=connecting; [browser] join Hoster @ ...;
//      [browser] goto(LAN) ...; [browser] the game is joining the picked room; [join_server] DONE.
//      the log folder's join_server.txt ends with state=done.
//   2. No such host: state=failed reason=room_not_found after 45 s, the browser opens with the sentence in red.
//   3. Relay down: state=failed reason=server_unreachable. A required mod missing: reason=mods_missing, nothing is joined.
//   4. Open the pause menu while the city loads and leave it open: the join closes it (browser_join_core.h), then joins.
//      A fresh game: the log has "[join_server] the city is up, but a message box is still on screen" (the box after Start) and then
//      "the city is up after N s (a message box held the join back for M ms): asking", with the goto into the search returning 1 and
//      the relay seeing the game host (REGISTER ... host=1) when nothing is there to join.
//      A fresh game, the log also has "[join_server] the city is up, but the game cannot start a search yet (...)" while the game's
//      start-up work is going on, then "the game can start a search now"; with --online_diag=true a "[join_server] probe +N ms" line every
//      250 ms to 1 s (the words of SearchFacts) for the first 30 s of the join, and one line "the search facts when it ended" at a failure.
//   5. larecomp.toml has no join_server / mcla_online_server / mcla_online_enable line afterwards (the argument is for one run).
//   6. --join_server=127.0.0.1:31000 (Cruise) and ,race: with a Cruise / Race session hosted by the second game, and with none
//      (the game hosts one itself).

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "join_server_core.h"

namespace js = larecomp::browser::joinserver;

namespace {

int g_checks = 0;
int g_failed = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failed;                                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
        }                                                                              \
    } while (0)

js::ParseResult P(const char* text) { return js::Parse(text); }

void TestParseServer() {
    {
        const auto r = P("127.0.0.1:31000");
        CHECK(r.ok && r.target.host == "127.0.0.1" && r.target.port == 31000 && r.target.mode == js::Mode::kCruise);
        CHECK(r.target.Address() == "127.0.0.1:31000");
    }
    {
        const auto r = P("play.example.org");  // no port: the relay's default
        CHECK(r.ok && r.target.host == "play.example.org" && r.target.port == 31000);
    }
    {
        const auto r = P("  host-1.example:4242  ");
        CHECK(r.ok && r.target.host == "host-1.example" && r.target.port == 4242);
    }
    CHECK(P("h:1").ok && P("h:65535").ok);
    CHECK(!P("h:0").ok && !P("h:65536").ok && !P("h:99999").ok && !P("h:123456").ok);
    CHECK(!P("h:").ok);
    CHECK(!P("h:12a").ok);
    CHECK(!P("h:-1").ok);
    CHECK(!P("").ok && !P("   ").ok);
    CHECK(!P(",cruise").ok);                // no host
    CHECK(!P(":31000").ok);
    CHECK(!P("a b:31000").ok);               // a space in the host
    CHECK(!P("host/path").ok);
    CHECK(!P("-host").ok && !P(".host").ok);
    CHECK(!P("::1").ok && !P("fe80::1:31000").ok);  // IPv6 is not taken
    CHECK(!P("host:31000:1").ok);
    const auto bad = P("h:99999");
    CHECK(!bad.error.empty());
}

void TestParseMode() {
    CHECK(P("h:1,cruise").ok && P("h:1,cruise").target.mode == js::Mode::kCruise);
    CHECK(P("h:1,CRUISE").target.mode == js::Mode::kCruise);
    CHECK(P("h:1,race").target.mode == js::Mode::kRace);
    CHECK(P("h:1, Race ").target.mode == js::Mode::kRace);
    {
        const auto r = P("h:1,room:Hoster");
        CHECK(r.ok && r.target.mode == js::Mode::kRoom && r.target.room == "Hoster");
    }
    {
        const auto r = P("h:1,ROOM: Some Player ");  // spaces inside a gamertag stay, the ends go
        CHECK(r.ok && r.target.mode == js::Mode::kRoom && r.target.room == "Some Player");
    }
    {
        const auto r = P("h:1,Hoster");  // a bare word is a gamertag (or a room name)
        CHECK(r.ok && r.target.mode == js::Mode::kRoom && r.target.room == "Hoster");
    }
    {
        const auto r = P("h:1,room:cruise");  // a player called Cruise: the prefix says so
        CHECK(r.ok && r.target.mode == js::Mode::kRoom && r.target.room == "cruise");
    }
    {
        const auto r = P("h:1,Night Race");  // not the word race: a name
        CHECK(r.ok && r.target.mode == js::Mode::kRoom && r.target.room == "Night Race");
    }
    CHECK(!P("h:1,").ok);
    CHECK(!P("h:1,   ").ok);
    CHECK(!P("h:1,room:").ok);
    CHECK(!P("h:1,room:   ").ok);
    CHECK(!P("h:1,a\x01z").ok);                       // a control character
    CHECK(!P("h:1,room:0123456789012345678901234567890123").ok);  // 34 characters
    CHECK(P("h:1,room:01234567890123456789012345678901").ok);     // 32
    CHECK(!P("h:1,room:a\nb").ok);
}

void TestDescribe() {
    CHECK(js::Describe(P("h:7").target) == "Cruise on h:7");
    CHECK(js::Describe(P("h:7,race").target) == "Race on h:7");
    CHECK(js::Describe(P("h:7,room:Pat").target) == "Pat's room on h:7");
    CHECK(js::Word(P("h:7").target) == "cruise");
    CHECK(js::Word(P("h:7,race").target) == "race");
    CHECK(js::Word(P("h:7,room:Pat").target) == "room:Pat");
}

std::vector<js::RoomInfo> Rooms() {
    return {
        {101, "Alice", "Night Cruise", 2, 8},
        {102, "Bob", "Bob's Race", 8, 8},   // full
        {103, "Carol", "Open Cruise", 1, 8},
        {104, "dave", "alice", 1, 8},       // a room NAME that equals another host's gamertag
        {105, "Erin", "Shared", 1, 8},
        {106, "Frank", "shared", 1, 8},     // two rooms of the same name
    };
}

void TestFindRoom() {
    const auto rooms = Rooms();
    {
        const auto r = js::FindRoom(rooms, "Carol");
        CHECK(r.status == js::FindStatus::kFound && rooms[r.index].host_vip == 103);
    }
    {
        const auto r = js::FindRoom(rooms, "carol");  // case ignored
        CHECK(r.status == js::FindStatus::kFound && rooms[r.index].host_vip == 103);
    }
    {
        const auto r = js::FindRoom(rooms, "  Carol ");
        CHECK(r.status == js::FindStatus::kFound && rooms[r.index].host_vip == 103);
    }
    {
        const auto r = js::FindRoom(rooms, "Bob");  // found, full
        CHECK(r.status == js::FindStatus::kFull && rooms[r.index].host_vip == 102);
    }
    {
        // "alice" is Alice's gamertag and dave's room name: the host wins
        const auto r = js::FindRoom(rooms, "alice");
        CHECK(r.status == js::FindStatus::kFound && rooms[r.index].host_vip == 101);
    }
    {
        const auto r = js::FindRoom(rooms, "Open Cruise");  // by room name when no host has it
        CHECK(r.status == js::FindStatus::kFound && rooms[r.index].host_vip == 103);
    }
    CHECK(js::FindRoom(rooms, "Shared").status == js::FindStatus::kAmbiguous);
    CHECK(js::FindRoom(rooms, "Carl").status == js::FindStatus::kNotFound);       // a part of a name never matches
    CHECK(js::FindRoom(rooms, "Carol2").status == js::FindStatus::kNotFound);
    CHECK(js::FindRoom(rooms, "").status == js::FindStatus::kNotFound);
    CHECK(js::FindRoom({}, "Carol").status == js::FindStatus::kNotFound);
    {
        // an unknown size (max_players 0) is not "full"
        std::vector<js::RoomInfo> one = {{9, "Zed", "z", 5, 0}};
        CHECK(js::FindRoom(one, "Zed").status == js::FindStatus::kFound);
    }
    {
        // the same gamertag twice (a host seen on two pages): ambiguous, not a silent pick
        std::vector<js::RoomInfo> two = {{1, "Zed", "a", 1, 8}, {2, "zed", "b", 1, 8}};
        CHECK(js::FindRoom(two, "Zed").status == js::FindStatus::kAmbiguous);
    }
}

void TestStatusFile() {
    js::Status s;
    s.state = js::State::kFailed;
    s.server = "127.0.0.1:31000";
    s.target = "room:Hoster";
    s.reason = js::reason::kRoomNotFound;
    s.message = js::FailureMessage(s.reason, P("127.0.0.1:31000,room:Hoster").target);
    const std::string text = js::FormatStatus(s, 1763750000);
    CHECK(text ==
          "join_server_status=1\n"
          "state=failed\n"
          "server=127.0.0.1:31000\n"
          "target=room:Hoster\n"
          "reason=room_not_found\n"
          "message=No room hosted by Hoster was found on 127.0.0.1:31000.\n"
          "updated=1763750000\n");

    // A value can never start another line.
    js::Status evil;
    evil.state = js::State::kDone;
    evil.message = "ok\nstate=failed\r\nreason=x";
    const std::string t2 = js::FormatStatus(evil, 1);
    CHECK(t2.find("\nstate=failed") == std::string::npos);
    CHECK(t2.find("state=done\n") != std::string::npos);
    int lines = 0;
    for (char c : t2) lines += c == '\n';
    CHECK(lines == 7);

    CHECK(std::string(js::StateName(js::State::kWaiting)) == "waiting");
    CHECK(std::string(js::StateName(js::State::kConnecting)) == "connecting");
    CHECK(std::string(js::StateName(js::State::kSearching)) == "searching");
    CHECK(std::string(js::StateName(js::State::kJoining)) == "joining");
    CHECK(std::string(js::StateName(js::State::kDone)) == "done");
    CHECK(std::string(js::StateName(js::State::kFailed)) == "failed");
    CHECK(js::IsFinal(js::State::kDone) && js::IsFinal(js::State::kFailed));
    CHECK(!js::IsFinal(js::State::kWaiting) && !js::IsFinal(js::State::kJoining));
}

// The quiet screen: the join starts only after no message box has been up for kQuietMs (the first box after Start is still up when the
// machine reaches "Cruising", about 8 s in, and goes at about 9 to 10 s).
void TestQuietGate() {
    constexpr int64_t q = js::QuietGate::kQuietMs;
    {
        // No box ever: the quiet time counts from the first look.
        js::QuietGate g;
        CHECK(!g.Update(false, 1000));
        CHECK(!g.Update(false, 1000 + q - 1));
        CHECK(g.Update(false, 1000 + q));
        CHECK(g.Update(false, 1000 + q + 5000));
        CHECK(!g.SawMessage());
        CHECK(g.HeldMs(99999) == 0);
    }
    {
        // The measured case: the box is up when the city is, goes ~1.5 s later; the join waits for it and then for the quiet time.
        js::QuietGate g;
        int64_t t = 0;
        for (; t < 1500; t += 16) CHECK(!g.Update(true, t));
        CHECK(g.SawMessage());
        CHECK(!g.Update(false, t));            // the box just went
        CHECK(!g.Update(false, t + q - 1));    // not long enough yet
        CHECK(g.Update(false, t + q));         // quiet
        CHECK(g.HeldMs(t + q) == t + q);       // held back since the first look (t = 0 there)
    }
    {
        // A second box right behind the first one: still one wait, and the quiet time restarts.
        js::QuietGate g;
        CHECK(!g.Update(true, 0));
        CHECK(!g.Update(false, 100));
        CHECK(!g.Update(false, 100 + q - 100));
        CHECK(!g.Update(true, 100 + q - 50));   // the next box opens before the quiet time is over
        CHECK(!g.Update(false, 100 + q));       // gone again: the count restarts here
        CHECK(!g.Update(false, 100 + 2 * q - 1));
        CHECK(g.Update(false, 100 + 2 * q));
    }
    {
        // A box that never goes: never quiet.
        js::QuietGate g;
        bool ever = false;
        for (int64_t t = 0; t < 60 * 60 * 1000; t += 1000) ever = ever || g.Update(true, t);
        CHECK(!ever && g.SawMessage());
    }
    {
        // A fresh gate (the city was lost and came back) forgets the old clear time.
        js::QuietGate g;
        CHECK(!g.Update(false, 0));
        g = js::QuietGate{};
        CHECK(!g.Update(false, 10 * q));
        CHECK(g.Update(false, 11 * q));
    }
}

// The search gate: the join goes on once the words the game's own search start asks for hold for a moment, or after a minute whatever
// they say. The measured case: a fresh game, the words not true until the profile sync is over, about 30 s after Start.
js::SearchFacts Facts(bool signed_in, bool handle = true, uint32_t flag = 0, uint32_t busy_record = 0, uint32_t busy_state = 0) {
    js::SearchFacts f;
    f.read = true;
    f.search_flag = flag;
    f.handle = handle ? 0x2F001230u : 0u;
    f.busy_record = busy_record;
    f.busy_state = busy_state;
    f.player_slot = 0;
    f.player_flags = signed_in ? 0xC0u : 0u;
    return f;
}

void TestSearchGate() {
    using V = js::SearchGate::Verdict;
    constexpr int64_t stable = js::SearchGate::kStableMs;
    constexpr int64_t cap = js::SearchGate::kMaxWaitMs;

    // The words, one by one.
    CHECK(js::SearchReady(Facts(true)));
    CHECK(!js::SearchReady(Facts(false)));
    CHECK(!js::SearchReady(Facts(true, false)));                    // no handle
    CHECK(!js::SearchReady(Facts(true, true, 1)));                  // a search is marked as running
    CHECK(!js::SearchReady(Facts(true, true, 0, 0x2F004000u, 1)));  // the search object is busy
    CHECK(js::SearchReady(Facts(true, true, 0, 0x2F004000u, 2)));   // its last search is over
    CHECK(js::SearchReady(Facts(true, true, 0, 0u, 1)));            // no record yet: a state word of 1 means nothing
    {
        js::SearchFacts f = Facts(true);
        f.player_flags = 0x80;  // signed in, not on Live: a System Link search needs no more
        CHECK(js::SearchReady(f) && js::PlayerSignedIn(f) && !js::PlayerOnLive(f));
        f.read = false;
        f.failed_at = "the net manager pointer";
        CHECK(!js::SearchReady(f));
        CHECK(js::SearchMissing(f) == "not readable: the net manager pointer");
    }
    CHECK(js::SearchMissing(Facts(true)).empty());
    CHECK(js::SearchMissing(Facts(false)) == "the player is not signed in to the network layer yet");
    CHECK(js::SearchMissing(Facts(true, false)) == "the search object has no handle yet");
    CHECK(js::SearchMissing(Facts(true, true, 1)) == "a search is already marked as running");
    CHECK(js::SearchMissing(Facts(true, true, 0, 1, 1)) == "the search object is busy");

    {
        // Ready from the first look: goes on after the stable time, not before.
        js::SearchGate g;
        CHECK(g.Update(Facts(true), 1000) == V::kWait);
        CHECK(g.Update(Facts(true), 1000 + stable - 1) == V::kWait);
        CHECK(g.Update(Facts(true), 1000 + stable) == V::kReady);
        CHECK(g.WaitedMs(1000 + stable) == stable);
    }
    {
        // The measured case: not ready until 30 s after Start, ready after that.
        js::SearchGate g;
        int64_t t = 0;
        bool went = false;
        for (; t < 30000; t += 16) went = went || g.Update(Facts(false), t) != V::kWait;
        CHECK(!went);
        CHECK(g.Update(Facts(true), t) == V::kWait);
        CHECK(g.Update(Facts(true), t + stable) == V::kReady);
    }
    {
        // A flicker restarts the stable time.
        js::SearchGate g;
        CHECK(g.Update(Facts(true), 0) == V::kWait);
        CHECK(g.Update(Facts(true), stable - 10) == V::kWait);
        CHECK(g.Update(Facts(false), stable - 5) == V::kWait);
        CHECK(g.Update(Facts(true), stable) == V::kWait);
        CHECK(g.Update(Facts(true), 2 * stable - 1) == V::kWait);
        CHECK(g.Update(Facts(true), 2 * stable) == V::kReady);
    }
    {
        // Never ready: gives up at the cap, not before. (A reading that is wrong in some build must not keep the join from trying.)
        js::SearchGate g;
        CHECK(g.Update(Facts(false), 0) == V::kWait);
        CHECK(g.Update(Facts(false), cap - 1) == V::kWait);
        CHECK(g.Update(Facts(false), cap) == V::kGaveUp);
        CHECK(g.Update(Facts(false), cap + 5000) == V::kGaveUp);
    }
    {
        // Memory that cannot be read counts as not ready, and also ends at the cap.
        js::SearchGate g;
        js::SearchFacts unread;
        unread.failed_at = "the player's slot";
        CHECK(g.Update(unread, 100) == V::kWait);
        CHECK(g.Update(unread, 100 + cap) == V::kGaveUp);
    }
    {
        // Ready for the stable time just as the cap passes: goes on as ready, not as "gave up".
        js::SearchGate g;
        CHECK(g.Update(Facts(false), 0) == V::kWait);
        CHECK(g.Update(Facts(true), cap - stable) == V::kWait);
        CHECK(g.Update(Facts(true), cap) == V::kReady);
    }
    {
        // A fresh gate (the city was lost and came back) forgets the old wait.
        js::SearchGate g;
        CHECK(g.Update(Facts(false), 0) == V::kWait);
        g = js::SearchGate{};
        CHECK(g.WaitedMs(99999) == 0);
        CHECK(g.Update(Facts(false), cap) == V::kWait);
    }
    // The log line.
    {
        js::SearchFacts f = Facts(true);
        f.net_mode = 1;
        f.session_state = 3;
        const std::string d = js::DescribeSearchFacts(f);
        CHECK(d.find("search_flag=0x0") != std::string::npos);
        CHECK(d.find("handle=0x2F001230") != std::string::npos);
        CHECK(d.find("flags 0xC0 (signed_in 1, live 1)") != std::string::npos);
        CHECK(d.find("net_mode=1 session_state=3 ready=1") != std::string::npos);
        CHECK(d.find('\n') == std::string::npos);
        js::SearchFacts u;
        u.failed_at = "the net manager pointer";
        CHECK(js::DescribeSearchFacts(u) == "not readable: the net manager pointer");
        CHECK(js::DescribeSearchFacts(Facts(false)).find("signed_in 0") != std::string::npos);
    }
}

void TestMessages() {
    const js::Target t = P("h:5,room:Pat").target;
    for (const char* why : {js::reason::kBadArgument, js::reason::kOnlineUnavailable, js::reason::kServerUnreachable,
                            js::reason::kServerTooOld, js::reason::kModsMissing, js::reason::kRoomNotFound, js::reason::kRoomFull, js::reason::kMessageBox,
                            js::reason::kRoomAmbiguous, js::reason::kWorldTimeout, js::reason::kOnlineFailed,
                            js::reason::kPauseOpen, js::reason::kMenuUnavailable, js::reason::kHostNoAnswer,
                            js::reason::kRoomGone}) {
        const std::string m = js::FailureMessage(why, t, "x");
        CHECK(!m.empty() && m.back() == '.');
        CHECK(m.find('\n') == std::string::npos);
    }
    CHECK(js::FailureMessage(js::reason::kModsMissing, t, "A, B") == "The server h:5 needs mods this game does not have: A, B.");
    CHECK(js::FailureMessage(js::reason::kWorldTimeout, t, "20") == "The game did not reach the city in 20 minutes, so the join was cancelled.");
    CHECK(js::FailureMessage(js::reason::kMessageBox, t) == "A message box in the game stayed open, so the join was cancelled.");
    CHECK(std::string(js::reason::kMessageBox) == "message_box");
    CHECK(js::FailureMessage("something_new", t, "text") == "text");
}

}  // namespace

int main() {
    TestParseServer();
    TestParseMode();
    TestDescribe();
    TestFindRoom();
    TestStatusFile();
    TestMessages();
    TestQuietGate();
    TestSearchGate();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
