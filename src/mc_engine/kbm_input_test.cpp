// A check of the event latch, the mouse keys and the neutral pad rule (kbm_input.h) that needs no game.
//   cmake --build <dir> --target larecomp_kbm_input_test, then run it (exit 0 = passed).
// Or by hand: clang++ -std=c++20 -I src/mc_engine src/mc_engine/kbm_input_test.cpp -o kbm_input_test

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "kbm_input.h"

namespace {

using namespace larecomp::kbm;

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const char* what, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAILED line %d: %s\n", line, what);
    }
}
#define CHECK(cond) Check((cond), #cond, __LINE__)

constexpr int kW = 0x57;      // VK_W
constexpr int kEnter = 0x0D;  // VK_RETURN
constexpr int kShift = 0x10;

std::vector<int> Ids(const KeySnapshot& s) {
    std::vector<int> out;
    s.ForEach([&](int id) { out.push_back(id); });
    return out;
}

// ---- the latch ----

void TestHeldKeys() {
    KeyLatch k;
    CHECK(!k.Poll().Any());
    k.Down(kW, false);
    CHECK(k.Held(kW));
    CHECK(k.Poll().Test(kW));
    CHECK(k.Poll().Test(kW));  // still held: every poll says so
    k.Down(kShift, false);
    CHECK((Ids(k.Poll()) == std::vector<int>{kShift, kW}));
    k.Up(kW);
    CHECK(!k.Held(kW));
    CHECK(!k.Poll().Test(kW));
    CHECK(k.Poll().Test(kShift));
    k.Up(kShift);
    CHECK(!k.Poll().Any());
}

// The quick tap: down and up both between two polls is still seen, once.
void TestTapBetweenPolls() {
    KeyLatch k;
    k.Down(kEnter, false);
    k.Up(kEnter);
    CHECK(!k.Held(kEnter));
    CHECK(k.Poll().Test(kEnter));   // the tap
    CHECK(!k.Poll().Test(kEnter));  // used up
    // two taps between two polls are one press as far as the poll can tell, and still seen
    k.Down(kEnter, false);
    k.Up(kEnter);
    k.Down(kEnter, false);
    k.Up(kEnter);
    CHECK(k.Poll().Test(kEnter));
    CHECK(!k.Poll().Test(kEnter));
    // a tap while the key is held by another down: the held state wins
    k.Down(kEnter, false);
    CHECK(k.Poll().Test(kEnter));
    CHECK(k.Poll().Test(kEnter));
    k.Up(kEnter);
    CHECK(!k.Poll().Test(kEnter));
}

void TestRepeatIsHeldButNotAPress() {
    KeyLatch k;
    k.Down(kW, false);
    CHECK(k.PressCount() == 1);
    (void)k.Poll();
    k.Down(kW, true);  // auto-repeat
    k.Down(kW, true);
    CHECK(k.PressCount() == 1);
    CHECK(k.Held(kW));
    // a repeat brings back a key that ReleaseAll let go of (an overlay swallowed its key-up)
    k.ReleaseAll();
    CHECK(!k.Poll().Any());
    k.Down(kW, true);
    CHECK(k.Poll().Test(kW));
    CHECK(k.PressCount() == 1);
    // but a repeat alone is not a tap: let go, nothing is left in the latch
    k.Up(kW);
    CHECK(!k.Poll().Any());
}

void TestReleaseAllClearsEverything() {
    KeyLatch k;
    k.Down(kW, false);
    k.Down(kShift, false);
    k.Up(kShift);  // a tap waiting
    k.Pulse(kKeyWheelUp);
    k.ReleaseAll();
    CHECK(!k.Held(kW));
    CHECK(!k.Poll().Any());
}

void TestWheelPulse() {
    KeyLatch k;
    k.Pulse(kKeyWheelUp);
    k.Pulse(kKeyWheelDown);
    const KeySnapshot s = k.Poll();
    CHECK(s.Test(kKeyWheelUp) && s.Test(kKeyWheelDown));
    CHECK(!k.Held(kKeyWheelUp));
    CHECK(!k.Poll().Any());  // exactly one poll
    CHECK(k.PressCount() == 2);
}

void TestBounds() {
    KeyLatch k;
    k.Down(0, false);
    k.Down(255, false);
    k.Down(-3, false);
    k.Down(1000, false);
    k.Up(0);
    k.Pulse(255);
    CHECK(!k.Poll().Any());
    CHECK(k.PressCount() == 0);
    k.Down(254, false);
    CHECK(k.Poll().Test(254));
    CHECK(!KeySnapshot{}.Test(0) && !KeySnapshot{}.Test(300) && !KeySnapshot{}.Test(-1));
}

void TestSnapshotOrder() {
    KeyLatch k;
    for (int id : {200, 3 + 1, 64, 63, 128, 254, 65}) k.Down(id, false);
    const std::vector<int> want{4, 63, 64, 65, 128, 200, 254};
    CHECK(Ids(k.Poll()) == want);
}

// A producer thread taps a key N times (down and up at once, from another thread than the polls), waiting after each tap until the consumer has
// seen it and then seen the key up again; the consumer polls as fast as it can. Every tap must be seen, once.
void TestNoTapIsLostAcrossThreads() {
    KeyLatch k;
    constexpr int kTaps = 20000;
    std::atomic<int> rises{0};
    std::atomic<int> falls{0};
    std::atomic<bool> done{false};
    std::thread consumer([&] {
        bool was = false;
        while (!done.load()) {
            const bool now = k.Poll().Test(kW);
            if (now && !was) rises.fetch_add(1);
            if (!now && was) falls.fetch_add(1);
            was = now;
        }
    });
    for (int i = 1; i <= kTaps; ++i) {
        k.Down(kW, false);
        k.Up(kW);
        while (rises.load() < i) std::this_thread::yield();
        while (falls.load() < i) std::this_thread::yield();
    }
    done.store(true);
    consumer.join();
    CHECK(rises.load() == kTaps);
    CHECK(falls.load() == kTaps);
    CHECK(k.PressCount() == uint32_t(kTaps));
}

// ---- the mouse keys ----

void TestMouseKeyTable() {
    std::set<int> ids;
    std::set<int> scancodes;
    std::set<std::string> names;
    for (const MouseKey& m : kMouseKeys) {
        CHECK(ids.insert(m.id).second);
        CHECK(scancodes.insert(m.scancode).second);
        CHECK(names.insert(m.name).second);
        CHECK(m.id > 0 && m.id < 255);
        // not the paddles' 0x7B-0x7E, not the camera key 0x7F, above them and below the keyboard's own codes the probe found free (P0d: 67-9C)
        CHECK(m.scancode >= 0x80 && m.scancode <= 0x9C);
        CHECK(FindMouseKeyById(m.id) == &m);
        CHECK(FindMouseKeyByScancode(m.scancode) == &m);
        CHECK(FindMouseKeyByName(m.name) == &m);
    }
    CHECK(kMouseKeyCount == 7);
    // the ids that no real key event produces: Windows VK 1 2 4 5 6 are the mouse buttons, 7 and 0x0A are undefined; 3 (Cancel), 8 (Back) and 9 (Tab) are keys
    CHECK(FindMouseKeyById(0x03) == nullptr && FindMouseKeyById(0x08) == nullptr && FindMouseKeyById(0x09) == nullptr);
    CHECK(FindMouseKeyById(0x0D) == nullptr && FindMouseKeyById(0x57) == nullptr);
    CHECK(FindMouseKeyByScancode(0x7B) == nullptr && FindMouseKeyByScancode(0x7F) == nullptr);
    CHECK(kKeyWheelUp == 0x07 && kKeyWheelDown == 0x0A);
}

void TestMouseKeyNames() {
    CHECK(FindMouseKeyByName("lmb") != nullptr && FindMouseKeyByName("lmb")->id == 0x01);
    CHECK(FindMouseKeyByName("LButton") == FindMouseKeyByName("LMB"));
    CHECK(FindMouseKeyByName("rbutton") == FindMouseKeyByName("RMB"));
    CHECK(FindMouseKeyByName("XButton1") == FindMouseKeyByName("MB4"));
    CHECK(FindMouseKeyByName("x2") == FindMouseKeyByName("MB5"));
    CHECK(FindMouseKeyByName("Mouse3") == FindMouseKeyByName("MMB"));
    CHECK(FindMouseKeyByName("mousewheelup") == FindMouseKeyByName("WheelUp"));
    CHECK(FindMouseKeyByName("WHEELDOWN")->scancode == 0x86);
    // names of real keys are not mouse keys (X is the key X, the X button is X1)
    CHECK(FindMouseKeyByName("X") == nullptr && FindMouseKeyByName("Shift") == nullptr && FindMouseKeyByName("Return") == nullptr);
    CHECK(FindMouseKeyByName("") == nullptr && FindMouseKeyByName("LMB ") == nullptr && FindMouseKeyByName("Mouse") == nullptr);
}

// ---- the neutral pad ----

// A keyboard player: no pad ever, the SDK says "not connected" all the time. The guest is told "connected" always, however many keys.
void TestKeyboardOnlyNeverPauses() {
    NeutralPadRule r;
    CHECK(r.State() == PadSeen::kNever);
    for (uint32_t presses = 0; presses < 50; ++presses) {
        CHECK(r.Decide(false, presses) == PadAnswer::kNeutral);
        CHECK(r.Decide(false, presses) == PadAnswer::kNeutral);  // caps and state ask in turn
    }
    CHECK(r.State() == PadSeen::kNever);
}

// A pad is here: the SDK's answer stands, so the neutral pad never mixes with a real one.
void TestRealPadStands() {
    NeutralPadRule r;
    CHECK(r.Decide(true, 0) == PadAnswer::kSdk);
    CHECK(r.State() == PadSeen::kHere);
    CHECK(r.Decide(true, 7) == PadAnswer::kSdk);
}

// The pad was here and is gone: the game is told the truth (it pauses), and goes on being told while the player does not touch the keyboard.
void TestUnplugIsReported() {
    NeutralPadRule r;
    CHECK(r.Decide(true, 5) == PadAnswer::kSdk);
    CHECK(r.Decide(false, 5) == PadAnswer::kSdk);  // the unplug: not connected, as it is
    CHECK(r.State() == PadSeen::kGone);
    for (int i = 0; i < 100; ++i) CHECK(r.Decide(false, 5) == PadAnswer::kSdk);  // frames go by, no key
    CHECK(r.State() == PadSeen::kGone);
}

// Keys held down before the unplug, or a pad tap, are not "a key since": only a press after the unplug takes the keyboard over.
void TestKeyAfterUnplugTakesOver() {
    NeutralPadRule r;
    CHECK(r.Decide(true, 12) == PadAnswer::kSdk);
    CHECK(r.Decide(false, 12) == PadAnswer::kSdk);
    CHECK(r.Decide(false, 12) == PadAnswer::kSdk);
    CHECK(r.Decide(false, 13) == PadAnswer::kNeutral);  // Enter on CONTINUE
    CHECK(r.State() == PadSeen::kNever);
    CHECK(r.Decide(false, 13) == PadAnswer::kNeutral);  // and it stays
    CHECK(r.Decide(false, 14) == PadAnswer::kNeutral);
}

// Presses made BEFORE the pad was lost are the baseline, including a wrapped counter.
void TestBaselineIsTheLossMoment() {
    NeutralPadRule r;
    CHECK(r.Decide(true, 0xFFFFFFFEu) == PadAnswer::kSdk);
    CHECK(r.Decide(false, 0xFFFFFFFEu) == PadAnswer::kSdk);
    CHECK(r.Decide(false, 0xFFFFFFFFu) == PadAnswer::kNeutral);
    NeutralPadRule w;
    CHECK(w.Decide(true, 0xFFFFFFFFu) == PadAnswer::kSdk);
    CHECK(w.Decide(false, 0xFFFFFFFFu) == PadAnswer::kSdk);
    CHECK(w.Decide(false, 0u) == PadAnswer::kNeutral);  // wrapped: still different
}

// After the keyboard took over, a pad that comes back is the SDK's again, and losing it again is a new pause.
void TestPadComesBackAndGoesAgain() {
    NeutralPadRule r;
    CHECK(r.Decide(true, 0) == PadAnswer::kSdk);
    CHECK(r.Decide(false, 0) == PadAnswer::kSdk);
    CHECK(r.Decide(false, 1) == PadAnswer::kNeutral);
    CHECK(r.Decide(true, 1) == PadAnswer::kSdk);
    CHECK(r.State() == PadSeen::kHere);
    CHECK(r.Decide(false, 1) == PadAnswer::kSdk);  // lost again: told again
    CHECK(r.State() == PadSeen::kGone);
    CHECK(r.Decide(false, 1) == PadAnswer::kSdk);
    CHECK(r.Decide(false, 2) == PadAnswer::kNeutral);
}

// The pad is back before any key: the pause the unplug caused ends the way it would without the rule.
void TestPadReturnsBeforeAnyKey() {
    NeutralPadRule r;
    CHECK(r.Decide(true, 3) == PadAnswer::kSdk);
    CHECK(r.Decide(false, 3) == PadAnswer::kSdk);
    CHECK(r.Decide(true, 3) == PadAnswer::kSdk);
    CHECK(r.State() == PadSeen::kHere);
    CHECK(r.Decide(false, 3) == PadAnswer::kSdk);
    CHECK(r.State() == PadSeen::kGone);
}

// Caps and state both ask every frame with the same inputs: asking twice, in either order, gives the same answer.
void TestAskingTwiceIsTheSame() {
    NeutralPadRule a, b;
    const bool script[][2] = {{true, false}, {false, false}, {false, true}, {false, false}, {true, false}, {false, false}, {false, true}};
    uint32_t presses = 0;
    for (const auto& step : script) {
        if (step[1]) ++presses;
        const PadAnswer a1 = a.Decide(step[0], presses);
        const PadAnswer a2 = a.Decide(step[0], presses);
        const PadAnswer b1 = b.Decide(step[0], presses);
        CHECK(a1 == a2);
        CHECK(a1 == b1);
    }
}

// The latch and the rule together: a key tapped in the pause menu after an unplug.
void TestLatchAndRule() {
    KeyLatch k;
    NeutralPadRule r;
    CHECK(r.Decide(true, k.PressCount()) == PadAnswer::kSdk);
    k.Down(kShift, false);  // held before the unplug (the player was boosting)
    CHECK(r.Decide(false, k.PressCount()) == PadAnswer::kSdk);
    k.Down(kShift, true);  // auto-repeat of that key: not a press
    CHECK(r.Decide(false, k.PressCount()) == PadAnswer::kSdk);
    k.Pulse(kKeyWheelDown);  // a mouse wheel notch is
    CHECK(r.Decide(false, k.PressCount()) == PadAnswer::kNeutral);
}

}  // namespace

int main() {
    TestHeldKeys();
    TestTapBetweenPolls();
    TestRepeatIsHeldButNotAPress();
    TestReleaseAllClearsEverything();
    TestWheelPulse();
    TestBounds();
    TestSnapshotOrder();
    TestNoTapIsLostAcrossThreads();
    TestMouseKeyTable();
    TestMouseKeyNames();
    TestKeyboardOnlyNeverPauses();
    TestRealPadStands();
    TestUnplugIsReported();
    TestKeyAfterUnplugTakesOver();
    TestBaselineIsTheLossMoment();
    TestPadComesBackAndGoesAgain();
    TestPadReturnsBeforeAnyKey();
    TestAskingTwiceIsTheSame();
    TestLatchAndRule();
    std::printf("kbm_input_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
