// A check of the event latch and the mouse keys (kbm_input.h) that needs no game.
//   cmake --build <dir> --target larecomp_kbm_input_test, then run it (exit 0 = passed).
// Or by hand: clang++ -std=c++20 -I src/mc_engine src/mc_engine/input/kbm_input_test.cpp -o kbm_input_test

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
    (void)k.Poll();
    k.Down(kW, true);  // auto-repeat
    k.Down(kW, true);
    CHECK(k.Held(kW));
    // a repeat brings back a key that ReleaseAll let go of (an overlay swallowed its key-up)
    k.ReleaseAll();
    CHECK(!k.Poll().Any());
    k.Down(kW, true);
    CHECK(k.Poll().Test(kW));
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

// Every mouse key has its key cap, the picture the live button prompts paint when a binding names it (KBM-RULES (c)): src/keycaps/<name in lower case>.png, 64 x 64,
// and <name>_w.png, 64 x 32 (the round Start / Back slots); the cap is found by the key's canonical name, which is also what the player's spellings map to (Mouse1 draws lmb.png).
// The files are read from the source tree next to this test (a build that moved the test elsewhere skips the check and says so).
bool PngSize(const std::filesystem::path& path, uint32_t* w, uint32_t* h) {
    std::ifstream f(path, std::ios::binary);
    unsigned char head[24] = {};
    if (!f.read(reinterpret_cast<char*>(head), sizeof(head))) return false;
    static const unsigned char kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    for (int i = 0; i < 8; ++i)
        if (head[i] != kSig[i]) return false;
    if (head[12] != 'I' || head[13] != 'H' || head[14] != 'D' || head[15] != 'R') return false;
    *w = (uint32_t(head[16]) << 24) | (uint32_t(head[17]) << 16) | (uint32_t(head[18]) << 8) | head[19];
    *h = (uint32_t(head[20]) << 24) | (uint32_t(head[21]) << 16) | (uint32_t(head[22]) << 8) | head[23];
    return true;
}

std::string LowerName(const char* name) {
    std::string out;
    for (const char* c = name; *c; ++c) out += (*c >= 'A' && *c <= 'Z') ? char(*c - 'A' + 'a') : *c;
    return out;
}

void TestMouseKeyCaps() {
    const std::filesystem::path dir = std::filesystem::path(__FILE__).parent_path() / ".." / ".." / "keycaps";
    if (!std::filesystem::exists(dir / "a.png")) {
        std::printf("kbm_input_test: the key caps folder %s was not found next to the test; the mouse cap check is skipped\n", dir.string().c_str());
        return;
    }
    for (const MouseKey& m : kMouseKeys) {
        const std::string stem = LowerName(m.name);
        uint32_t w = 0, h = 0;
        CHECK(PngSize(dir / (stem + ".png"), &w, &h) && w == 64 && h == 64);
        w = h = 0;
        CHECK(PngSize(dir / (stem + "_w.png"), &w, &h) && w == 64 && h == 32);
    }
    // The player's other spellings land on the canonical name, so the same cap is drawn.
    const struct {
        const char* spelling;
        const char* stem;
    } kSpellings[] = {{"Mouse1", "lmb"}, {"LButton", "lmb"}, {"Mouse2", "rmb"}, {"RButton", "rmb"}, {"Mouse3", "mmb"}, {"MButton", "mmb"}, {"Mouse4", "mb4"}, {"XButton1", "mb4"},
                      {"X1", "mb4"},    {"Mouse5", "mb5"},   {"XButton2", "mb5"}, {"X2", "mb5"},      {"MouseWheelUp", "wheelup"}, {"MouseWheelDown", "wheeldown"}};
    for (const auto& sp : kSpellings) {
        const MouseKey* m = FindMouseKeyByName(sp.spelling);
        CHECK(m != nullptr && LowerName(m->name) == sp.stem);
    }
}

// The menu's buttons as keys (the mouse in the menus, KBM-DESIGN.md P6): ids and scancodes that nothing else uses, found both ways, and a press that reaches exactly one poll.
void TestMenuKeys() {
    CHECK(kMenuKeyCount == 6);
    std::set<int> ids;
    std::set<int> scancodes;
    for (int i = 0; i < kMenuKeyCount; ++i) {
        const MenuKeyDef& k = kMenuKeys[i];
        CHECK(int(k.key) == i);  // the table is in enum order: MenuKeyId indexes it
        CHECK(MenuKeyId(k.key) == k.id);
        CHECK(FindMenuKeyById(k.id) == &k);
        CHECK(ids.insert(k.id).second && scancodes.insert(k.scancode).second);
        // Windows VK numbers nothing uses (0x97..0x9F); the SDK's VirtualKey has 0x88..0x8F as the gamepad navigation keys and 0xA0 as left shift.
        CHECK(k.id >= 0x97 && k.id <= 0x9F);
        // Right after the mouse keys' scancodes, inside the codes the probe found free (P0d: 67-9C), clear of the paddles (0x7B-0x7E), the camera (0x7F) and the mouse (0x80-0x86).
        CHECK(k.scancode >= 0x87 && k.scancode <= 0x9C);
        // not a mouse key
        CHECK(FindMouseKeyById(k.id) == nullptr && FindMouseKeyByScancode(k.scancode) == nullptr);
        CHECK(FindMouseKeyByName(k.name) == nullptr);
    }
    CHECK(FindMenuKeyById(0x01) == nullptr && FindMenuKeyById(0x0D) == nullptr && FindMenuKeyById(0x8E) == nullptr && FindMenuKeyById(0x96) == nullptr);
    // A press is one poll: the latch's pulse, then nothing; two presses in the same poll are one.
    KeyLatch latch;
    latch.Pulse(MenuKeyId(MenuKey::Accept));
    latch.Pulse(MenuKeyId(MenuKey::Accept));
    KeySnapshot a = latch.Poll();
    CHECK(a.Test(MenuKeyId(MenuKey::Accept)) && !a.Test(MenuKeyId(MenuKey::Back)));
    KeySnapshot b = latch.Poll();
    CHECK(!b.Test(MenuKeyId(MenuKey::Accept)));
    // Different buttons in one poll are all there.
    latch.Pulse(MenuKeyId(MenuKey::Down));
    latch.Pulse(MenuKeyId(MenuKey::Right));
    KeySnapshot c = latch.Poll();
    CHECK(c.Test(MenuKeyId(MenuKey::Down)) && c.Test(MenuKeyId(MenuKey::Right)) && !c.Test(MenuKeyId(MenuKey::Up)));
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
    TestMouseKeyCaps();
    TestMenuKeys();
    std::printf("kbm_input_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
