// A check of the pure part of the live button prompts (prompt_glyphs.h): the slot tables, the menu / road context, which art each glyph family puts in each
// slot, the PlayStation sources and their override, and the reading back of the game's own DXT5 bitmaps. Needs no game.
//   cmake --build <dir> --target larecomp_prompt_glyphs_test, then run it (exit 0 = passed).
// Or by hand: clang++ -std=c++20 -I src/mc_engine src/mc_engine/prompt_glyphs_test.cpp -o prompt_glyphs_test

#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "prompt_glyphs.h"

namespace {

using namespace larecomp::prompts;

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

// the default bindings of keyboard_native.cpp, as the cvars hold them
const std::map<std::string, std::string> kDefaults = {
    {"kb_ui_accept", "Return,Space"}, {"kb_ui_back", "Escape,Backspace"}, {"kb_ui_x", "X"},       {"kb_ui_y", "Y"},
    {"kb_ui_lb", "Q"},                {"kb_ui_rb", "E"},                  {"kb_ui_lt", "PageUp"}, {"kb_ui_rt", "PageDown"},
    {"kb_ui_start", "Return,Escape"}, {"kb_ui_select", "Tab"},            {"kb_drive_handbrake", "Space"},
    {"kb_drive_weight", "CapsLock"},  {"kb_drive_nitro", "Shift,X"},      {"kb_drive_headlights", "H"},
    {"kb_drive_camera", "V"},         {"kb_drive_brake", "S,Down"},       {"kb_drive_gas", "W,Up"},
    {"kb_drive_pause", ""},           {"kb_drive_map", "M"},
};

std::vector<std::string> Split(const std::string& list) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t comma = list.find(',', pos);
        if (comma == std::string::npos) comma = list.size();
        if (comma > pos) out.push_back(list.substr(pos, comma - pos));
        pos = comma + 1;
    }
    return out;
}

// the keyboard_native.cpp's rule: the first key of the list that has a cap, else the first key
std::string KeyOfDefaults(std::string_view cvar, const std::set<std::string>& caps) {
    const auto it = kDefaults.find(std::string(cvar));
    if (it == kDefaults.end()) return {};
    return PickKey(Split(it->second), [&](const std::string& k) { return caps.count(k) != 0; });
}

const std::set<std::string> kAllCaps = {"Return", "Space", "Escape", "Backspace", "X", "Y", "Q", "E", "PageUp", "PageDown", "Tab", "CapsLock", "Shift", "H", "V", "S", "Down", "W", "Up", "M"};

Art For(Family f, Context c, Slot s, bool bitmaps = true, bool files = true, const std::set<std::string>& caps = kAllCaps,
        const std::vector<std::pair<Slot, std::string>>& ov = {}) {
    return Decide(
        f, c, s, ov, [&](const std::string&) { return bitmaps; }, [&](const std::string&) { return files; },
        [&](std::string_view cvar) { return KeyOfDefaults(cvar, caps); });
}

void TestSlotTables() {
    CHECK(kSlotCount == 11);
    for (int i = 0; i < kSlotCount; ++i) {
        CHECK(int(kSlots[i].slot) == i);
        CHECK(kSlots[i].width == 64);
        CHECK(kSlots[i].height == 64 || kSlots[i].height == 32);
        CHECK(!kMenuCvar[i].empty());
        CHECK(kMenuCvar[i].substr(0, 6) == "kb_ui_");
        CHECK(kRoadCvar[i].empty() || kRoadCvar[i].substr(0, 9) == "kb_drive_");
        CHECK(int(kPsSources[i].slot) == i);
        CHECK(kPsSources[i].file != nullptr);
    }
    // the round Start and Back glyphs are the 64x32 bitmaps, everything else 64x64
    CHECK(Info(Slot::Start).height == 32 && Info(Slot::Back).height == 32);
    CHECK(Info(Slot::Phone).height == 64);
    Slot s = Slot::A;
    CHECK(SlotFromName("LB", &s) && s == Slot::LB);
    CHECK(SlotFromName("rt", &s) && s == Slot::RT);
    CHECK(SlotFromName("Phone", &s) && s == Slot::Phone);
    CHECK(!SlotFromName("L1", &s));
    CHECK(!SlotFromName("", &s));
}

void TestContext() {
    CHECK(ContextFor(false, false) == Context::Menu);  // the title and the sign-in: the menu's mapper answers
    CHECK(ContextFor(false, true) == Context::Menu);
    CHECK(ContextFor(true, true) == Context::Menu);    // a pause menu, a garage, a message box
    CHECK(ContextFor(true, false) == Context::Drive);  // the road
}

void TestKeyCvars() {
    // in a menu every slot uses its menu cvar only
    for (int i = 0; i < kSlotCount; ++i) {
        const auto m = KeyCvars(Slot(i), Context::Menu);
        CHECK(m[0] == kMenuCvar[i]);
        CHECK(m[1].empty());
    }
    // on the road: the road's cvar first, then the menu's; slots without a road key stay on the menu's
    auto a = KeyCvars(Slot::A, Context::Drive);
    CHECK(a[0] == "kb_drive_handbrake" && a[1] == "kb_ui_accept");
    auto x = KeyCvars(Slot::X, Context::Drive);
    CHECK(x[0] == "kb_drive_nitro" && x[1] == "kb_ui_x");
    auto lb = KeyCvars(Slot::LB, Context::Drive);
    CHECK(lb[0] == "kb_ui_lb" && lb[1].empty());
    auto phone = KeyCvars(Slot::Phone, Context::Drive);
    CHECK(phone[0] == "kb_ui_select" && phone[1].empty());
    auto start = KeyCvars(Slot::Start, Context::Drive);
    CHECK(start[0] == "kb_drive_pause" && start[1] == "kb_ui_start");
}

void TestPickKey() {
    const std::set<std::string> some = {"Space"};
    CHECK(PickKey(Split("Return,Space"), [&](const std::string& k) { return some.count(k) != 0; }) == "Space");  // the first one that has a cap
    CHECK(PickKey(Split("Return,Escape"), [&](const std::string&) { return false; }) == "Return");              // none has: the first (the blank cap)
    CHECK(PickKey(Split(""), [&](const std::string&) { return true; }).empty());                                // no key at all: the art stays
}

void TestXbox() {
    // the Xbox family is the original art, in every context, with or without the other sources
    for (int i = 0; i < kSlotCount; ++i) {
        CHECK(For(Family::Xbox, Context::Menu, Slot(i)) == Art{});
        CHECK(For(Family::Xbox, Context::Drive, Slot(i)).kind == ArtKind::Original);
    }
}

void TestKeyboardContexts() {
    // the menu: the menu keys
    CHECK((For(Family::Keyboard, Context::Menu, Slot::A) == Art{ArtKind::KeyCap, "Return"}));
    CHECK((For(Family::Keyboard, Context::Menu, Slot::B) == Art{ArtKind::KeyCap, "Escape"}));
    CHECK((For(Family::Keyboard, Context::Menu, Slot::X) == Art{ArtKind::KeyCap, "X"}));
    CHECK((For(Family::Keyboard, Context::Menu, Slot::LB) == Art{ArtKind::KeyCap, "Q"}));
    CHECK((For(Family::Keyboard, Context::Menu, Slot::Back) == Art{ArtKind::KeyCap, "Tab"}));
    CHECK((For(Family::Keyboard, Context::Menu, Slot::Start) == Art{ArtKind::KeyCap, "Return"}));
    // the road: the driving keys of the buttons that have one, the menu keys of the rest
    CHECK((For(Family::Keyboard, Context::Drive, Slot::A) == Art{ArtKind::KeyCap, "Space"}));
    CHECK((For(Family::Keyboard, Context::Drive, Slot::B) == Art{ArtKind::KeyCap, "CapsLock"}));
    CHECK((For(Family::Keyboard, Context::Drive, Slot::X) == Art{ArtKind::KeyCap, "Shift"}));  // "Shift,X": the first with a cap
    CHECK((For(Family::Keyboard, Context::Drive, Slot::Y) == Art{ArtKind::KeyCap, "H"}));
    CHECK((For(Family::Keyboard, Context::Drive, Slot::RB) == Art{ArtKind::KeyCap, "V"}));
    CHECK((For(Family::Keyboard, Context::Drive, Slot::LT) == Art{ArtKind::KeyCap, "S"}));
    CHECK((For(Family::Keyboard, Context::Drive, Slot::RT) == Art{ArtKind::KeyCap, "W"}));
    CHECK((For(Family::Keyboard, Context::Drive, Slot::Back) == Art{ArtKind::KeyCap, "M"}));
    CHECK((For(Family::Keyboard, Context::Drive, Slot::LB) == Art{ArtKind::KeyCap, "Q"}));
    CHECK((For(Family::Keyboard, Context::Drive, Slot::Start) == Art{ArtKind::KeyCap, "Return"}));  // kb_drive_pause is empty: the menu's Start key
    CHECK((For(Family::Keyboard, Context::Drive, Slot::Phone) == Art{ArtKind::KeyCap, "Tab"}));      // the phone answers through the menu's Back key on the road too
    // the same slot differs between the contexts exactly where the road has its own key
    int differ = 0;
    for (int i = 0; i < kSlotCount; ++i)
        if (For(Family::Keyboard, Context::Menu, Slot(i)) != For(Family::Keyboard, Context::Drive, Slot(i))) ++differ;
    CHECK(differ == 8);  // A B X Y RB LT RT Back; LB, Start and the phone do not
    // a key with no cap in the folder is still drawn (the blank cap); an empty binding leaves the original art
    const std::set<std::string> none;
    CHECK((For(Family::Keyboard, Context::Menu, Slot::X, true, true, none) == Art{ArtKind::KeyCap, "X"}));
    CHECK((Decide(Family::Keyboard, Context::Menu, Slot::X, {}, [](const std::string&) { return true; }, [](const std::string&) { return true; },
                  [](std::string_view) { return std::string(); }) == Art{}));
}

void TestPlayStation() {
    // the faces: the game's own bitmaps; the same in a menu and on the road
    CHECK((For(Family::PlayStation, Context::Menu, Slot::A) == Art{ArtKind::GameBitmap, "shared_24.dds"}));  // cross
    CHECK((For(Family::PlayStation, Context::Menu, Slot::B) == Art{ArtKind::GameBitmap, "shared_13.dds"}));  // circle
    CHECK((For(Family::PlayStation, Context::Menu, Slot::X) == Art{ArtKind::GameBitmap, "shared_15.dds"}));  // square
    CHECK((For(Family::PlayStation, Context::Menu, Slot::Y) == Art{ArtKind::GameBitmap, "shared_36.dds"}));  // triangle
    for (int i = 0; i < kSlotCount; ++i) CHECK(For(Family::PlayStation, Context::Menu, Slot(i)) == For(Family::PlayStation, Context::Drive, Slot(i)));
    // the shoulders, Options and Share are not identified in the dictionary: the shipped pictures
    CHECK((For(Family::PlayStation, Context::Menu, Slot::LB) == Art{ArtKind::File, "ps_l1"}));
    CHECK((For(Family::PlayStation, Context::Menu, Slot::RB) == Art{ArtKind::File, "ps_r1"}));
    CHECK((For(Family::PlayStation, Context::Menu, Slot::LT) == Art{ArtKind::File, "ps_l2"}));
    CHECK((For(Family::PlayStation, Context::Menu, Slot::RT) == Art{ArtKind::File, "ps_r2"}));
    CHECK((For(Family::PlayStation, Context::Menu, Slot::Start) == Art{ArtKind::File, "ps_options"}));
    CHECK((For(Family::PlayStation, Context::Menu, Slot::Back) == Art{ArtKind::File, "ps_share"}));
    CHECK((For(Family::PlayStation, Context::Menu, Slot::Phone) == Art{ArtKind::File, "ps_share"}));
    // a face whose bitmap is not in the dictionary falls back to its picture
    CHECK((For(Family::PlayStation, Context::Menu, Slot::A, false, true) == Art{ArtKind::File, "ps_cross"}));
    // nothing at all: the original art stays
    CHECK((For(Family::PlayStation, Context::Menu, Slot::LB, false, false) == Art{}));
    CHECK((For(Family::PlayStation, Context::Menu, Slot::A, false, false) == Art{}));
}

void TestPsOverride() {
    const auto ov = ParsePsMap(" LB=shared_8.dds, rb = shared_32.dds ,Back=shared_5.dds,nonsense,Q=shared_1.dds,A=");
    CHECK(ov.size() == 3);
    CHECK(ov[0].first == Slot::LB && ov[0].second == "shared_8.dds");
    CHECK(ov[1].first == Slot::RB && ov[1].second == "shared_32.dds");
    CHECK(ov[2].first == Slot::Back && ov[2].second == "shared_5.dds");
    CHECK(ParsePsMap("").empty());
    // the override wins over the table and the file when the bitmap is in the dictionary
    CHECK((For(Family::PlayStation, Context::Menu, Slot::LB, true, true, kAllCaps, ov) == Art{ArtKind::GameBitmap, "shared_8.dds"}));
    CHECK((For(Family::PlayStation, Context::Menu, Slot::A, true, true, kAllCaps, ov) == Art{ArtKind::GameBitmap, "shared_24.dds"}));  // not overridden
    // ... and is ignored when it names a bitmap the dictionary does not hold
    const Art skipped = Decide(
        Family::PlayStation, Context::Menu, Slot::LB, ov, [](const std::string& n) { return n != "shared_8.dds"; }, [](const std::string&) { return true; },
        [](std::string_view) { return std::string(); });
    CHECK((skipped == Art{ArtKind::File, "ps_l1"}));
}

void TestArtIdentity() {
    CHECK((Art{ArtKind::KeyCap, "Return"} == Art{ArtKind::KeyCap, "Return"}));
    CHECK((Art{ArtKind::KeyCap, "Return"} != Art{ArtKind::File, "Return"}));
    CHECK(Describe(Art{}) == "original");
    CHECK(Describe(Art{ArtKind::GameBitmap, "shared_24.dds"}) == "game:shared_24.dds");
    CHECK(Describe(Art{ArtKind::KeyCap, "Tab"}) == "key:Tab");
}

// ---- the DXT5 reading ----

// Builds a standard BC3 block by hand and returns it the way the guest holds it (pairs swapped).
void Guest(const uint8_t standard[16], uint8_t out[16]) { SwapGuestPairs(standard, out); }

void TestDecodeFlatBlocks() {
    // opaque red: alpha endpoints 255 and 255, every index 0; colour endpoints pure red, every index 0
    uint8_t std_block[16] = {255, 255, 0, 0, 0, 0, 0, 0, 0x00, 0xF8, 0x00, 0xF8, 0, 0, 0, 0};
    uint8_t guest[16];
    Guest(std_block, guest);
    uint8_t px[64];
    DecodeBc3GuestBlock(guest, px);
    for (int i = 0; i < 16; ++i) {
        CHECK(px[i * 4 + 0] == 255);
        CHECK(px[i * 4 + 1] == 0);
        CHECK(px[i * 4 + 2] == 0);
        CHECK(px[i * 4 + 3] == 255);
    }
    // fully transparent white: alpha 0 / 0, colour white
    uint8_t clear[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0};
    Guest(clear, guest);
    DecodeBc3GuestBlock(guest, px);
    CHECK(px[3] == 0 && px[0] == 255 && px[1] == 255 && px[2] == 255);
}

void TestDecodeIndices() {
    // alpha 255 / 0 (eight levels: 255, 0, 218, 182, 145, 109, 72, 36) with the index of pixel i = i % 8; colour red / blue with the index of pixel i = i % 4
    uint8_t blk[16] = {255, 0};
    uint64_t abits = 0;
    for (int i = 0; i < 16; ++i) abits |= uint64_t(i % 8) << (3 * i);
    for (int i = 0; i < 6; ++i) blk[2 + i] = uint8_t((abits >> (8 * i)) & 0xFF);
    blk[8] = 0x00;  // c0 = 0xF800 (red)
    blk[9] = 0xF8;
    blk[10] = 0x1F;  // c1 = 0x001F (blue)
    blk[11] = 0x00;
    uint32_t cbits = 0;
    for (int i = 0; i < 16; ++i) cbits |= uint32_t(i % 4) << (2 * i);
    for (int i = 0; i < 4; ++i) blk[12 + i] = uint8_t((cbits >> (8 * i)) & 0xFF);
    uint8_t guest[16];
    Guest(blk, guest);
    uint8_t px[64];
    DecodeBc3GuestBlock(guest, px);
    const int want_alpha[8] = {255, 0, 218, 182, 145, 109, 72, 36};
    for (int i = 0; i < 16; ++i) CHECK(px[i * 4 + 3] == want_alpha[i % 8]);
    // colours: index 0 red, 1 blue, 2 two thirds red, 3 one third red
    CHECK(px[0] == 255 && px[1] == 0 && px[2] == 0);       // pixel 0, index 0
    CHECK(px[4] == 0 && px[5] == 0 && px[6] == 255);       // pixel 1, index 1
    CHECK(px[8] == 170 && px[9] == 0 && px[10] == 85);     // pixel 2, index 2: (2*255+0)/3, (2*0+255)/3
    CHECK(px[12] == 85 && px[13] == 0 && px[14] == 170);   // pixel 3, index 3
    // the six-level alpha mode (a0 <= a1) has transparent and opaque entries at the end
    uint8_t six[16] = {10, 200, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint64_t sbits = 0;
    sbits |= uint64_t(6) << 0;  // pixel 0: index 6 = 0
    sbits |= uint64_t(7) << 3;  // pixel 1: index 7 = 255
    sbits |= uint64_t(2) << 6;  // pixel 2: index 2 = (4*10 + 1*200)/5 = 48
    for (int i = 0; i < 6; ++i) six[2 + i] = uint8_t((sbits >> (8 * i)) & 0xFF);
    Guest(six, guest);
    DecodeBc3GuestBlock(guest, px);
    CHECK(px[3] == 0 && px[7] == 255 && px[11] == 48);
}

void TestDecodeSurface() {
    // 8x8: four blocks, each a flat colour (red, green, blue, white), alpha opaque
    const uint16_t colours[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
    std::vector<uint8_t> blocks;
    for (uint16_t c : colours) {
        uint8_t blk[16] = {255, 255, 0, 0, 0, 0, 0, 0, uint8_t(c & 0xFF), uint8_t(c >> 8), uint8_t(c & 0xFF), uint8_t(c >> 8), 0, 0, 0, 0};
        uint8_t guest[16];
        Guest(blk, guest);
        blocks.insert(blocks.end(), guest, guest + 16);
    }
    const std::vector<uint8_t> rgba = DecodeBc3GuestSurface(blocks, 8, 8);
    CHECK(rgba.size() == 8 * 8 * 4);
    auto at = [&](int x, int y, int c) { return rgba[(size_t(y) * 8 + x) * 4 + c]; };
    CHECK(at(0, 0, 0) == 255 && at(0, 0, 1) == 0);    // block 0 (top left): red
    CHECK(at(3, 3, 0) == 255 && at(3, 3, 1) == 0);
    CHECK(at(4, 0, 1) == 255 && at(4, 0, 0) == 0);    // block 1 (top right): green
    CHECK(at(0, 4, 2) == 255 && at(0, 4, 0) == 0);    // block 2 (bottom left): blue
    CHECK(at(7, 7, 0) == 255 && at(7, 7, 1) == 255 && at(7, 7, 2) == 255);  // block 3: white
    CHECK(at(7, 7, 3) == 255);
    // a wrong byte count or size gives nothing
    CHECK(DecodeBc3GuestSurface(blocks, 16, 8).empty());
    CHECK(DecodeBc3GuestSurface(blocks, 6, 8).empty());
    CHECK(DecodeBc3GuestSurface({}, 8, 8).empty());
}

}  // namespace

// The screens of the first P4 run (2026-11-15), as the three facts the game gives: the pause menu is on the form stack; the garage, its EXIT question and the full map are not, but
// FlashNavigator's current movie is GARAGEMOVIE / NAVSYSMOVIE; a road pop-up is a layer. The road has none of them, the phone is not one of them.
UiSignals Ui(bool form, bool movie, bool layer) {
    UiSignals s;
    s.form = form;
    s.movie = movie;
    s.layer = layer;
    return s;
}

void TestUiSignals() {
    const bool entered = true;
    // every screen the run visited
    CHECK(ContextFor(entered, Ui(false, false, false)) == Context::Drive);  // free roam: the HUD, and the phone's message over it
    CHECK(ContextFor(entered, Ui(true, false, false)) == Context::Menu);   // the pause menu (the run's one right answer)
    CHECK(ContextFor(entered, Ui(false, true, false)) == Context::Menu);   // the garage menu: GARAGEMOVIE, no form (FAILED in the run)
    CHECK(ContextFor(entered, Ui(false, true, true)) == Context::Menu);    // the EXIT question in the garage (FAILED in the run)
    CHECK(ContextFor(entered, Ui(false, true, false)) == Context::Menu);   // the full map: NAVSYSMOVIE (the caps were S W / CAPS / SHIFT / H)
    CHECK(ContextFor(entered, Ui(false, false, true)) == Context::Menu);   // a pop-up on the road
    CHECK(ContextFor(entered, Ui(true, true, true)) == Context::Menu);     // all of them
    // before the game is entered nothing matters: the menu's mapper answers
    for (int bits = 0; bits < 8; ++bits) CHECK(ContextFor(false, Ui(bits & 1, bits & 2, bits & 4)) == Context::Menu);
    // the old two-argument form is the same rule
    CHECK(ContextFor(true, false) == ContextFor(true, Ui(false, false, false)));
    CHECK(ContextFor(true, true) == ContextFor(true, Ui(true, false, false)));
    // any() and equality
    CHECK(!Ui(false, false, false).any());
    CHECK(Ui(false, false, true).any() && Ui(false, true, false).any() && Ui(true, false, false).any());
    CHECK(Ui(true, false, true) == Ui(true, false, true));
    CHECK(!(Ui(true, false, true) == Ui(true, true, true)));
    // the road's own movies are not screens: the HUD
    CHECK(IsRoadMovieName("HUDMOVIE"));
    CHECK(IsRoadMovieName("HUDMAPMOVIE"));
    CHECK(IsRoadMovieName("HUD"));
    CHECK(!IsRoadMovieName("GARAGEMOVIE"));
    CHECK(!IsRoadMovieName("NAVSYSMOVIE"));
    CHECK(!IsRoadMovieName("PAUSEMOVIE"));
    CHECK(!IsRoadMovieName("POPUPMOVIE"));
    CHECK(IsRoadMovieName("SIDEKICKMOVIE"));   // the phone's message comes over the road (if the statechart ever selects it, it is not a screen)
    CHECK(!IsRoadMovieName("PROMPTMOVIE"));    // the prompt bar is drawn over every screen: not listed, so a statechart screen that selects it still counts
    CHECK(!IsRoadMovieName(""));               // an entry the movie map does not know is a screen of unknown name
    CHECK(!IsRoadMovieName("HU"));
    // the reason text of the log line
    CHECK(DescribeUi(Ui(false, false, false), "") == "none");
    CHECK(DescribeUi(Ui(true, false, false), "") == "form");
    CHECK(DescribeUi(Ui(false, true, false), "GARAGEMOVIE") == "movie GARAGEMOVIE");
    CHECK(DescribeUi(Ui(false, true, false), "") == "movie");
    CHECK(DescribeUi(Ui(false, true, true), "GARAGEMOVIE") == "movie GARAGEMOVIE+layer");
    CHECK(DescribeUi(Ui(true, true, true), "NAVSYSMOVIE") == "form+movie NAVSYSMOVIE+layer");
}

// The context follows the game live: the keyboard's caps are repainted exactly when the context turns, and only for the keyboard family. A walk through the run's screens.
void TestContextFollowsLive() {
    struct Step {
        const char* what;
        Family family;
        bool entered;
        UiSignals ui;
        bool repaint;  // the poll that sees this repaints
    };
    bool painted_menu = true;  // the first repaint of a run (the title: the menu's)
    const Step steps[] = {
        {"the title, keyboard", Family::Keyboard, false, Ui(false, false, false), false},   // menu context painted already
        {"in free roam", Family::Keyboard, true, Ui(false, false, false), true},            // the road's keys
        {"the same poll again", Family::Keyboard, true, Ui(false, false, false), false},
        {"the pause menu opens", Family::Keyboard, true, Ui(true, false, false), true},
        {"...a movie too", Family::Keyboard, true, Ui(true, true, false), false},           // still the menu's: nothing to paint
        {"the pause menu closes", Family::Keyboard, true, Ui(false, false, false), true},
        {"the map opens", Family::Keyboard, true, Ui(false, true, false), true},
        {"the map closes", Family::Keyboard, true, Ui(false, false, false), true},
        {"the garage menu", Family::Keyboard, true, Ui(false, true, false), true},
        {"the EXIT question over it", Family::Keyboard, true, Ui(false, true, true), false},
        {"a pad is used: Xbox art", Family::Xbox, true, Ui(false, true, true), false},       // no context for the pad's art
        {"back on the road with the pad", Family::Xbox, true, Ui(false, false, false), false},
    };
    for (const Step& st : steps) {
        const bool need = ContextNeedsRepaint(st.family, st.entered, st.ui, painted_menu);
        if (need != st.repaint) std::printf("  context step '%s': repaint %d, wanted %d\n", st.what, need ? 1 : 0, st.repaint ? 1 : 0);
        CHECK(need == st.repaint);
        // the real code repaints (and records the context) whenever the family or the context needs it; here only a context repaint is modelled
        if (need) painted_menu = (ContextFor(st.entered, st.ui) == Context::Menu);
    }
    // the repaint that a change of family makes records the context too (RepaintPrompts): so a key after the pad's stretch does not need a second one
    painted_menu = (ContextFor(true, Ui(false, false, false)) == Context::Menu);  // the repaint for the family change, on the road
    CHECK(!ContextNeedsRepaint(Family::Keyboard, true, Ui(false, false, false), painted_menu));
    // the pad's families never repaint for a context
    CHECK(!ContextNeedsRepaint(Family::Xbox, true, Ui(true, true, true), false));
    CHECK(!ContextNeedsRepaint(Family::PlayStation, true, Ui(false, false, false), true));
    CHECK(!ContextNeedsRepaint(Family::PlayStation, true, Ui(true, false, false), false));
    // the keys each screen gets: the map's zoom is PageUp / PageDown and its exit Escape, not S / W / CAPS (what the run painted); the garage's back is Escape
    const Art map_lt = For(Family::Keyboard, ContextFor(true, Ui(false, true, false)), Slot::LT);
    CHECK((map_lt == Art{ArtKind::KeyCap, "PageUp"}));
    const Art map_b = For(Family::Keyboard, ContextFor(true, Ui(false, true, false)), Slot::B);
    CHECK((map_b == Art{ArtKind::KeyCap, "Escape"}));
    const Art road_b = For(Family::Keyboard, ContextFor(true, Ui(false, false, false)), Slot::B);
    CHECK((road_b == Art{ArtKind::KeyCap, "CapsLock"}));
    // the phone: the same cap on the road and in a menu (the Back / Select key: it answers through the menu's mapper)
    CHECK((For(Family::Keyboard, Context::Drive, Slot::Phone) == For(Family::Keyboard, Context::Menu, Slot::Phone)));
    CHECK((For(Family::Keyboard, Context::Drive, Slot::Phone) == Art{ArtKind::KeyCap, "Tab"}));
}


int main() {
    TestSlotTables();
    TestContext();
    TestUiSignals();
    TestKeyCvars();
    TestPickKey();
    TestXbox();
    TestKeyboardContexts();
    TestContextFollowsLive();
    TestPlayStation();
    TestPsOverride();
    TestArtIdentity();
    TestDecodeFlatBlocks();
    TestDecodeIndices();
    TestDecodeSurface();
    std::printf("prompt_glyphs_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
