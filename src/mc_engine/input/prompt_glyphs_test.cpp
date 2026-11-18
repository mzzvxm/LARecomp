// A check of the pure part of the live button prompts (prompt_glyphs.h, prompt_art.h): the slot tables, the menu / road context, which art each glyph family puts in each
// slot, the PlayStation sources and their override, the reading back of the game's own DXT5 bitmaps, and the rule that fits a replacement into the ink of the glyph it
// replaces (ink box measuring, aspect fit, the tonal scheme, the numeric check). Needs no game.
//   cmake --build <dir> --target larecomp_prompt_glyphs_test, then run it (exit 0 = passed).
// Or by hand: clang++ -std=c++20 -I src/mc_engine src/mc_engine/input/prompt_glyphs_test.cpp -o prompt_glyphs_test

#include <cmath>
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
    {"kb_drive_weight", "Control"},   {"kb_drive_nitro", "Shift,X,LMB"},  {"kb_drive_headlights", "H"},
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

const std::set<std::string> kAllCaps = {"Return", "Space", "Escape", "Backspace", "X", "Y", "Q", "E", "PageUp", "PageDown", "Tab", "CapsLock", "Control", "Shift", "H", "V", "S", "Down", "W", "Up", "M"};

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
    CHECK((For(Family::Keyboard, Context::Drive, Slot::B) == Art{ArtKind::KeyCap, "Control"}));
    CHECK((For(Family::Keyboard, Context::Drive, Slot::X) == Art{ArtKind::KeyCap, "Shift"}));  // "Shift,X,LMB": the first with a cap (the mouse button has none)
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
    CHECK((road_b == Art{ArtKind::KeyCap, "Control"}));
    // the phone: the same cap on the road and in a menu (the Back / Select key: it answers through the menu's mapper)
    CHECK((For(Family::Keyboard, Context::Drive, Slot::Phone) == For(Family::Keyboard, Context::Menu, Slot::Phone)));
    CHECK((For(Family::Keyboard, Context::Drive, Slot::Phone) == Art{ArtKind::KeyCap, "Tab"}));
}


// ---- prompt_art.h: the rule that fits a replacement into the ink of the glyph it replaces -----------------------------------------------------------------

void FillRect(Bitmap& b, int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t bl, uint8_t a) {
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            uint8_t* p = &b.rgba[(size_t(y) * b.width + size_t(x)) * 4];
            p[0] = r;
            p[1] = g;
            p[2] = bl;
            p[3] = a;
        }
    }
}

bool Near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

constexpr double kPhoneLight = 155.0;  // the Sidekick phone's bar (prompt_glyphs.h kPhoneBackdrop)
constexpr double kDark = 24.0;         // the prompt bars of the menus

// A thin dark ring (2.6 px) with a dark arrow, on transparent, one ink colour: the simplest line art (box 13..50 of 64).
Bitmap ThinRing() {
    Bitmap b = MakeBitmap(64, 64);
    DrawRing(b, InkBox{13, 13, 50, 50}, 2.6);
    FillRect(b, 27, 27, 34, 36, 0, 0, 0, 255);
    const double ink[3] = {66.0, 66.0, 66.0};
    Tint(b, ink);
    return b;
}

// The Sidekick phone's own accept glyph as the dump of the first run holds it (sidekick_106_64x64.rgba): fully opaque, a LIGHT disc (231) with a dark rim (115, 3 px) and a dark arrow
// (66), box 12..51 x 14..48. The movie draws it darkened (about x0.69, which is why it shows on the bar as a dark ring and a dark arrow with the bar showing through the disc).
Bitmap PhoneOriginal() {
    Bitmap b = MakeBitmap(64, 64);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const double px = double(x) + 0.5 - 32.0, py = double(y) + 0.5 - 31.5;
            const bool outer = (px / 20.0) * (px / 20.0) + (py / 17.5) * (py / 17.5) <= 1.0;
            const bool inner = (px / 17.0) * (px / 17.0) + (py / 14.5) * (py / 14.5) <= 1.0;
            if (!outer) continue;
            const uint8_t v = inner ? 231 : 115;
            FillRect(b, x, y, x, y, v, v, v, 255);
        }
    }
    FillRect(b, 26, 27, 35, 36, 66, 66, 66, 255);  // the arrow
    return b;
}

// The shipped Create picture: a white pill, tall (12 x 36 of 24 x 44), on transparent.
Bitmap Pill() {
    Bitmap b = MakeBitmap(24, 44);
    FillRect(b, 6, 4, 17, 39, 255, 255, 255, 255);
    return b;
}

// A key cap picture: an opaque white cap (60 x 52 at 2..61, 6..57 of 64 x 64) with a 2 px black border and a black block for the key name.
Bitmap CapPicture() {
    Bitmap b = MakeBitmap(64, 64);
    FillRect(b, 2, 6, 61, 57, 0, 0, 0, 255);
    FillRect(b, 4, 8, 59, 55, 255, 255, 255, 255);
    FillRect(b, 22, 26, 40, 36, 0, 0, 0, 255);
    return b;
}

void TestInkMeasure() {
    Bitmap b = MakeBitmap(20, 10);
    CHECK(!MeasureInk(b).valid());  // nothing drawn: no ink, no box
    CHECK(!MeasureInk(Bitmap{}).valid());
    FillRect(b, 3, 2, 12, 7, 200, 100, 50, 255);
    Ink ink = MeasureInk(b);
    CHECK(ink.valid());
    CHECK((ink.box == InkBox{3, 2, 12, 7}));
    CHECK(ink.box.width() == 10 && ink.box.height() == 6 && ink.pixels == 60);
    CHECK(Near(ink.solidity, 1.0, 1e-9));
    CHECK(Near(ink.rgb[0], 200, 1e-6) && Near(ink.rgb[1], 100, 1e-6) && Near(ink.rgb[2], 50, 1e-6));
    CHECK(Near(ink.luma, 0.299 * 200 + 0.587 * 100 + 0.114 * 50, 1e-6));
    // the threshold: alpha 32 is not ink, 33 is (the soft edge of a scaled picture stays out of the box, the body does not)
    Bitmap edge = MakeBitmap(10, 10);
    FillRect(edge, 4, 4, 4, 4, 255, 255, 255, 32);
    CHECK(!MeasureInk(edge).valid());
    FillRect(edge, 4, 4, 4, 4, 255, 255, 255, 33);
    CHECK(MeasureInk(edge).valid());
    // the colour is weighted by alpha: a solid red pixel and a faint (85 / 255) blue one
    Bitmap two = MakeBitmap(4, 1);
    FillRect(two, 0, 0, 0, 0, 255, 0, 0, 255);
    FillRect(two, 3, 0, 3, 0, 0, 0, 255, 85);
    const Ink tw = MeasureInk(two);
    CHECK((tw.box == InkBox{0, 0, 3, 0}));
    CHECK(Near(tw.rgb[0], 255.0 * 1.0 / (1.0 + 1.0 / 3.0), 1e-6) && Near(tw.rgb[2], 255.0 * (1.0 / 3.0) / (1.0 + 1.0 / 3.0), 1e-6));
    // a hole inside the box lowers the solidity (a ring is about a third of its box)
    const Ink ring = MeasureInk(ThinRing());
    CHECK((ring.box == InkBox{13, 13, 50, 50}));
    CHECK(ring.solidity > 0.2 && ring.solidity < 0.45);
    CHECK(Near(ring.luma, 66.0, 0.01));
    CHECK(Near(ring.line_rgb[0], 66.0, 0.01) && Near(ring.mark_rgb[2], 66.0, 0.01) && ring.line_alpha == 255.0);  // one ink colour: the dark band and the marks are all of it
    // the contrast against a backdrop: dark ink on the phone's light bar, light ink on a dark one; |luma - backdrop|, alpha weighted
    CHECK(Near(InkContrast(ThinRing(), kPhoneLight), 155.0 - 66.0, 0.01));
    Bitmap white = MakeBitmap(8, 8);
    FillRect(white, 0, 0, 7, 7, 255, 255, 255, 255);
    CHECK(Near(InkContrast(white, kDark), 255.0 - 24.0, 0.01));
    CHECK(Near(InkContrast(MakeBitmap(8, 8), kDark), 0.0, 1e-9));
    CHECK(Near(MeasureOriginal(white, kDark).contrast, 231.0, 0.01));
}

void TestInkColoursAsEncoded() {
    // the phone's bitmap: a light disc, a dark rim, a dark arrow. Its ink colours as the bitmap holds them: the mean says light, the dark band is the rim and the arrow, the marks are the arrow
    const Ink orig = MeasureOriginal(PhoneOriginal(), kPhoneLight);
    CHECK((orig.box == InkBox{12, 14, 51, 48}));
    CHECK(orig.luma > 150.0);                                                  // the mean ink is light: a luma guess on it says "light on dark"
    CHECK(Near(orig.mark_rgb[0], 66.0, 0.5) && Near(orig.mark_luma, 66.0, 0.5));  // the marks: the arrow
    CHECK(orig.line_rgb[0] > 90.0 && orig.line_rgb[0] < 115.0);                // the dark band: the rim (115) and the arrow (66), the rim's pixels the most
    CHECK(orig.line_alpha == 255.0 && orig.peak_alpha == 255.0);
    CHECK(Near(orig.stroke, 3.0, 0.4));                                        // the rim's width, not the disc's (the disc is lighter than the dark band)
    CHECK(IsDarkOnLight(orig, kPhoneLight));                                   // its marks are far darker than the bar
    CHECK(!IsDarkOnLight(orig, kDark));                                        // on a dark bar nothing is 40 darker
    // a tab on a dark bar: a light body (231) with dark letters (40): the marks are dark too, and still no line art (the bar is dark: kToneGap)
    Bitmap tab = MakeBitmap(64, 64);
    FillRect(tab, 14, 14, 52, 50, 231, 231, 231, 255);
    FillRect(tab, 24, 24, 40, 40, 40, 40, 40, 255);
    CHECK(!IsDarkOnLight(MeasureOriginal(tab, kDark), kDark));
    // a flat picture: every ink pixel is both the dark band and the marks
    Bitmap flat = MakeBitmap(10, 10);
    FillRect(flat, 2, 2, 7, 7, 120, 130, 140, 255);
    const Ink f = MeasureInk(flat);
    CHECK(Near(f.line_rgb[1], 130.0, 1e-6) && Near(f.mark_rgb[2], 140.0, 1e-6));
}

void TestBoxWithin() {
    const InkBox outer{13, 13, 50, 50};
    CHECK(BoxWithin(InkBox{13, 13, 50, 50}, outer, 0));
    CHECK(BoxWithin(InkBox{14, 20, 40, 50}, outer, 0));
    CHECK(!BoxWithin(InkBox{12, 13, 50, 50}, outer, 0));
    CHECK(BoxWithin(InkBox{12, 12, 51, 51}, outer, 1));  // the slack of one pixel on every side
    CHECK(!BoxWithin(InkBox{11, 13, 50, 50}, outer, 1));
    CHECK(!BoxWithin(InkBox{15, 1, 47, 62}, outer, 1));  // the first run's PlayStation Create button: the whole bitmap
    CHECK(!BoxWithin(InkBox{}, outer, 1) && !BoxWithin(outer, InkBox{}, 1));  // no ink on either side is no answer
}

void TestFitSize() {
    CHECK(FitSize(30, 30, 61, 61).w == 61 && FitSize(30, 30, 61, 61).h == 61);  // a 32x32 face into the 61 pixel ink box of a ball: exact, not 60 for a float's sake
    const Size tall = FitSize(20, 40, 32, 32);
    CHECK(tall.w == 16 && tall.h == 32);  // the aspect is kept: the tall side decides
    const Size wide = FitSize(60, 52, 38, 38);
    CHECK(wide.w == 38 && wide.h == 32);  // a TAB cap in the phone's ring box: rounded down, never over
    const Size thin = FitSize(100, 1, 10, 10);
    CHECK(thin.w == 10 && thin.h == 1);  // at least one pixel
    CHECK(FitSize(0, 5, 10, 10).w == 0 && FitSize(5, 5, 0, 10).h == 0);
    for (int sw = 3; sw < 90; sw += 7) {
        for (int sh = 2; sh < 90; sh += 11) {
            const Size r = FitSize(sw, sh, 38, 29);
            CHECK(r.w >= 1 && r.w <= 38 && r.h >= 1 && r.h <= 29);  // never over the box
            CHECK(r.w == 38 || r.h == 29 || r.w == 1 || r.h == 1);   // and it fills one of the box's sides
        }
    }
}

void TestResample() {
    Bitmap block = MakeBitmap(8, 8);
    FillRect(block, 0, 0, 7, 7, 10, 120, 240, 255);
    const Bitmap small = Resample(block, 4, 4);
    CHECK(small.width == 4 && small.height == 4);
    bool same = true;
    for (size_t i = 0; i < 16; ++i) same = same && small.rgba[i * 4 + 0] == 10 && small.rgba[i * 4 + 1] == 120 && small.rgba[i * 4 + 2] == 240 && small.rgba[i * 4 + 3] == 255;
    CHECK(same);  // a flat picture stays flat
    const Bitmap big = Resample(Resample(block, 2, 2), 8, 8);
    CHECK(big.rgba[(3 * 8 + 3) * 4 + 0] == 10 && big.rgba[(3 * 8 + 3) * 4 + 3] == 255);
    // a transparent half does not tint the edge: red on the left, nothing (colour 0) on the right
    Bitmap half = MakeBitmap(4, 1);
    FillRect(half, 0, 0, 1, 0, 255, 0, 0, 255);
    const Bitmap h2 = Resample(half, 2, 1);
    CHECK(h2.rgba[0] == 255 && h2.rgba[3] == 255);  // the left pixel
    CHECK(h2.rgba[7] == 0);                          // the right one is empty
    Bitmap three = MakeBitmap(3, 1);
    FillRect(three, 0, 0, 0, 0, 255, 0, 0, 255);
    const Bitmap t1 = Resample(three, 1, 1);
    CHECK(t1.rgba[0] == 255 && t1.rgba[1] == 0 && t1.rgba[3] == 85);  // a third of it is red: alpha a third, colour still red
    CHECK(Resample(Bitmap{}, 4, 4).empty() && Resample(block, 0, 4).empty());
}

void TestStrokeWidth() {
    Bitmap ring = MakeBitmap(64, 64);
    DrawRing(ring, InkBox{10, 10, 53, 53}, 3.0);
    CHECK(Near(StrokeWidth(ring, InkBox{10, 10, 53, 53}), 3.0, 0.35));
    Bitmap thin = MakeBitmap(64, 64);
    DrawRing(thin, InkBox{10, 10, 53, 53}, 1.5);
    CHECK(Near(StrokeWidth(thin, InkBox{10, 10, 53, 53}), 1.5, 0.35));
    CHECK(Near(MeasureInk(ThinRing()).stroke, 2.6, 0.4));  // the arrow in the middle does not count: the first run from the left is the ring's
    CHECK(Near(MeasureInk(PhoneOriginal()).stroke, 3.0, 0.4));  // the rim of a light disc: only the dark band counts, not the disc inside it
    Bitmap rect = MakeBitmap(20, 10);
    FillRect(rect, 3, 2, 12, 7, 255, 255, 255, 255);
    CHECK(Near(StrokeWidth(rect, InkBox{3, 2, 12, 7}), 10.0, 1e-9));  // a solid picture gives its width: the caller clamps it
    CHECK(StrokeWidth(MakeBitmap(8, 8), InkBox{0, 0, 7, 7}) == 0.0);
    CHECK(StrokeWidth(rect, InkBox{}) == 0.0 && StrokeWidth(rect, InkBox{0, 0, 99, 99}) == 0.0);
}

void TestDrawRingAndDilate() {
    Bitmap canvas = MakeBitmap(64, 64);
    DrawRing(canvas, InkBox{13, 13, 50, 50}, 2.6);
    const Ink ink = MeasureInk(canvas);
    CHECK((ink.box == InkBox{13, 13, 50, 50}));  // the outer edge touches the box and stays in it
    CHECK(AlphaAt(canvas, 32, 32) == 0);         // hollow
    CHECK(AlphaAt(canvas, 13, 32) > 200);        // the left side of the ring
    Bitmap dot = MakeBitmap(9, 9);
    FillRect(dot, 4, 4, 4, 4, 0, 0, 0, 255);
    const Bitmap grown = DilateAlpha(dot, 2);
    CHECK(AlphaAt(grown, 4, 4) == 255 && AlphaAt(grown, 6, 4) == 255 && AlphaAt(grown, 4, 2) == 255);
    CHECK(AlphaAt(grown, 7, 4) == 0 && AlphaAt(grown, 0, 0) == 0);
    CHECK(DilateAlpha(dot, 0).rgba == dot.rgba);
    // the outline band of a silhouette: after erosions only the inside is left
    Bitmap sq = MakeBitmap(10, 10);
    FillRect(sq, 1, 1, 8, 8, 255, 255, 255, 255);
    const std::vector<uint8_t> inner = InnerOfSilhouette(sq, 2);
    CHECK(inner[size_t(1) * 10 + 1] == 0 && inner[size_t(2) * 10 + 2] == 0);  // the two outermost rings are gone
    CHECK(inner[size_t(3) * 10 + 3] == 1 && inner[size_t(5) * 10 + 5] == 1);
    CHECK(InnerOfSilhouette(sq, 0)[size_t(1) * 10 + 1] == 1);
}

void TestFitSolid() {
    // a light-on-dark original (a tab on a dark prompt bar): the replacement keeps its own tones and goes into the original's box, centred, keeping its aspect
    Bitmap light = MakeBitmap(64, 64);
    FillRect(light, 10, 8, 41, 39, 200, 200, 200, 255);
    const Ink orig = MeasureOriginal(light, kDark);
    CHECK(!IsDarkOnLight(orig, kDark));
    Bitmap src = MakeBitmap(24, 44);
    FillRect(src, 2, 2, 21, 41, 255, 255, 255, 255);  // 20 x 40 of ink in a transparent margin
    const Fitted f = FitArt(src, Shape::Pictogram, orig, kDark, 64, 64);
    CHECK(f.look == Look::Solid && f.bitmap.width == 64 && f.bitmap.height == 64);
    const Ink got = MeasureInk(f.bitmap);
    CHECK((got.box == InkBox{18, 8, 33, 39}));  // 16 x 32: the tall side fills the box's height, centred across its width
    CHECK(Near(got.luma, 255.0, 0.01));          // its own tones
    const FitCheck check = CheckFit(orig, f.bitmap, kDark);
    CHECK(check.look == Look::Solid && check.box_ok && check.tone_ok && check.ok());
    // a picture much bigger than the bitmap still ends inside the box
    Bitmap huge = MakeBitmap(128, 128);
    FillRect(huge, 0, 0, 127, 127, 255, 255, 255, 255);
    CHECK((MeasureInk(FitArt(huge, Shape::Cap, orig, kDark, 64, 64).bitmap).box == InkBox{10, 8, 41, 39}));
    // a small one is brought up to the box (the game's 32 x 32 faces fill the 64 x 64 slot)
    Bitmap face = MakeBitmap(32, 32);
    FillRect(face, 1, 1, 30, 30, 90, 90, 120, 255);
    Bitmap ball = MakeBitmap(64, 64);
    FillRect(ball, 1, 1, 61, 61, 60, 160, 20, 255);
    const Ink ball_ink = MeasureOriginal(ball, kDark);
    CHECK((MeasureInk(FitArt(face, Shape::Pictogram, ball_ink, kDark, 64, 64).bitmap).box == InkBox{1, 1, 61, 61}));
    // no ink in the original (nothing to measure): the whole canvas is the box
    CHECK((MeasureInk(FitArt(src, Shape::Pictogram, Ink{}, kDark, 64, 64).bitmap).box == InkBox{16, 0, 47, 63}));  // 20 x 40 fitted into 64 x 64 is 32 x 64
    // a source without ink gives nothing
    CHECK(FitArt(MakeBitmap(8, 8), Shape::Cap, orig, kDark, 64, 64).bitmap.empty());
    // the transparent surround carries the art's colour, not black (a block compressor and the GPU's filter read it)
    CHECK(f.bitmap.rgba[3] == 0 && f.bitmap.rgba[0] == 255 && f.bitmap.rgba[1] == 255);
}

uint8_t RgbAt(const Bitmap& b, int x, int y, int c) { return b.rgba[(size_t(y) * b.width + size_t(x)) * 4 + size_t(c)]; }

void TestFitLineOnThePhone() {
    const Ink orig = MeasureOriginal(PhoneOriginal(), kPhoneLight);
    CHECK(IsDarkOnLight(orig, kPhoneLight));

    // PlayStation: a ring on the original's box with the Create picture inside; the ring in the colour of the original's rim, the symbol in the colour of its arrow, the original's stroke
    const Fitted ps = FitArt(Pill(), Shape::Pictogram, orig, kPhoneLight, 64, 64);
    CHECK(ps.look == Look::Line && ps.bitmap.width == 64 && ps.bitmap.height == 64);
    const Ink ps_ink = MeasureInk(ps.bitmap);
    CHECK((ps_ink.box == InkBox{12, 14, 51, 48}));  // the ring IS the box: the symbol is inside it
    CHECK(AlphaAt(ps.bitmap, 32, 22) == 255 && AlphaAt(ps.bitmap, 32, 32) == 255);  // the symbol sits in the middle of the ring (the pill's body)
    CHECK(AlphaAt(ps.bitmap, 20, 32) == 0 && AlphaAt(ps.bitmap, 24, 32) == 0);      // the ring's hollow is open around it: transparent, the bar shows through, as through the disc
    CHECK(Near(RgbAt(ps.bitmap, 32, 30, 0), orig.mark_rgb[0], 1.0));                // the symbol: the arrow's own colour (it is what the movie darkens)
    CHECK(Near(RgbAt(ps.bitmap, 13, 31, 0), orig.line_rgb[0], 1.0) && AlphaAt(ps.bitmap, 13, 31) == 255);  // the ring: the rim's own colour, full alpha
    const FitCheck c1 = CheckFit(orig, ps.bitmap, kPhoneLight);
    CHECK(c1.look == Look::Line && c1.box_ok && c1.tone_ok && c1.ok());
    CHECK(c1.colour_diff <= kColourTolerance && Near(c1.alpha, c1.original_alpha, kAlphaTolerance));

    // keyboard: the cap's outline and its name at the ring's size; the outline in the rim's colour and the ring's weight, the name in the arrow's colour
    const Fitted kb = FitArt(CapPicture(), Shape::Cap, orig, kPhoneLight, 64, 64);
    CHECK(kb.look == Look::Line);
    const Ink kb_ink = MeasureInk(kb.bitmap);
    CHECK(BoxWithin(kb_ink.box, orig.box, kBoxSlack));
    CHECK(kb_ink.box.width() >= 38 && kb_ink.box.height() >= 30 && kb_ink.box.height() < kb_ink.box.width());  // the 60 x 52 cap keeps its aspect in the 40 x 35 box
    CHECK(AlphaAt(kb.bitmap, 32, 31) == 255 && Near(RgbAt(kb.bitmap, 32, 31, 0), orig.mark_rgb[0], 1.0));  // the key name block is in the middle, in the arrow's colour
    CHECK(AlphaAt(kb.bitmap, 20, 22) == 0);                                                                // the cap's face is open (transparent): outline and name only
    CHECK(Near(RgbAt(kb.bitmap, 13, 31, 0), orig.line_rgb[0], 1.0) && AlphaAt(kb.bitmap, 13, 31) > 200);  // the outline's left side: the rim's colour, thickened to about the ring's weight
    const FitCheck c2 = CheckFit(orig, kb.bitmap, kPhoneLight);
    CHECK(c2.look == Look::Line && c2.box_ok && c2.tone_ok && c2.ok());
    // no fill anywhere: the interior of a line art is transparent, so the bar shows through exactly where it shows through the original's disc once the movie has darkened it
    int opaque_inside = 0;
    for (int y = 20; y < 44; ++y)
        for (int x = 18; x < 24; ++x) opaque_inside += AlphaAt(kb.bitmap, x, y) > 0 ? 1 : 0;
    CHECK(opaque_inside == 0);
}

void TestLineArtKeepsTheOriginalsEncoding() {
    // an original whose strokes are semi-transparent and tinted (alpha 200, colour 90,80,70): the replacement's strokes have that colour and reach that alpha, not 255 and not a grey of our own
    Bitmap orig_bmp = MakeBitmap(64, 64);
    DrawRing(orig_bmp, InkBox{13, 13, 50, 50}, 2.6);
    const double tint[3] = {90.0, 80.0, 70.0};
    Tint(orig_bmp, tint);
    ScaleAlpha(orig_bmp, 200.0 / 255.0);
    const Ink orig = MeasureOriginal(orig_bmp, kPhoneLight);
    CHECK(orig.line_alpha == 200.0 && Near(orig.line_rgb[1], 80.0, 0.01));
    const Fitted ps = FitArt(Pill(), Shape::Pictogram, orig, kPhoneLight, 64, 64);
    CHECK(ps.look == Look::Line);
    CHECK(MeasureInk(ps.bitmap).peak_alpha == 200.0);
    CHECK(Near(RgbAt(ps.bitmap, 32, 30, 0), 90.0, 1.0) && Near(RgbAt(ps.bitmap, 32, 30, 1), 80.0, 1.0) && Near(RgbAt(ps.bitmap, 32, 30, 2), 70.0, 1.0));
    CHECK(CheckFit(orig, ps.bitmap, kPhoneLight).ok());
    const Fitted kb = FitArt(CapPicture(), Shape::Cap, orig, kPhoneLight, 64, 64);
    CHECK(MeasureInk(kb.bitmap).peak_alpha == 200.0 && CheckFit(orig, kb.bitmap, kPhoneLight).ok());
    // Over() mixes colours by alpha (the symbol over the ring): half a red over an opaque blue is purple, fully opaque
    Bitmap dst = MakeBitmap(2, 1);
    FillRect(dst, 0, 0, 1, 0, 0, 0, 255, 255);
    Bitmap src = MakeBitmap(1, 1);
    FillRect(src, 0, 0, 0, 0, 255, 0, 0, 128);
    Over(dst, src, 0, 0);
    CHECK(Near(RgbAt(dst, 0, 0, 0), 128.0, 1.0) && Near(RgbAt(dst, 0, 0, 2), 127.0, 1.0) && AlphaAt(dst, 0, 0) == 255 && RgbAt(dst, 1, 0, 2) == 255);
    Bitmap clear = MakeBitmap(1, 1);
    Over(clear, src, 0, 0);
    CHECK(RgbAt(clear, 0, 0, 0) == 255 && AlphaAt(clear, 0, 0) == 128);  // over nothing: the source as it is
}

void TestCheckFitCatchesTheFirstRun() {
    const Ink orig = MeasureOriginal(PhoneOriginal(), kPhoneLight);
    // the first run's PlayStation Create button: a white pill over the whole bitmap (box 15..47 x 1..62), white where the original is dark grey
    Bitmap first_ps = MakeBitmap(64, 64);
    FillRect(first_ps, 15, 1, 47, 62, 255, 255, 255, 255);
    const FitCheck bad = CheckFit(orig, first_ps, kPhoneLight);
    CHECK(bad.look == Look::Line && !bad.box_ok && !bad.tone_ok && !bad.ok());  // it spills out of the ring's box and its ink colour is 150 levels off
    CHECK(bad.colour_diff > 100.0);
    // the second run's: inside the box, but still white (the box is right, the encoding is not)
    const Fitted right = FitArt(Pill(), Shape::Pictogram, orig, kPhoneLight, 64, 64);
    Bitmap white = right.bitmap;
    const double ink_white[3] = {255.0, 255.0, 255.0};
    Tint(white, ink_white);
    const FitCheck second = CheckFit(orig, white, kPhoneLight);
    CHECK(second.box_ok && !second.tone_ok && !second.ok());
    // a lighter grey (a good 36 levels off) fails, a few levels off passes
    Bitmap lighter = right.bitmap;
    const double grey_140[3] = {140.0, 140.0, 140.0};
    Tint(lighter, grey_140);
    CHECK(CheckFit(orig, lighter, kPhoneLight).box_ok && !CheckFit(orig, lighter, kPhoneLight).tone_ok);
    Bitmap close = right.bitmap;
    Tint(close, orig.line_rgb);
    CHECK(CheckFit(orig, close, kPhoneLight).ok());
    // the alpha has to match too: half transparent strokes are not the original's
    Bitmap faint = right.bitmap;
    ScaleAlpha(faint, 0.5);
    const FitCheck half = CheckFit(orig, faint, kPhoneLight);
    CHECK(half.box_ok && !half.tone_ok);
    // the first run's TAB cap: opaque, 60 pixels wide, a light face
    const FitCheck cap = CheckFit(orig, CapPicture(), kPhoneLight);
    CHECK(!cap.box_ok && !cap.ok());
    // a solid glyph on a dark bar: only the box and the side of the backdrop are checked, the brightness is free (a white cap on a green ball)
    Bitmap ball = MakeBitmap(64, 64);
    FillRect(ball, 1, 1, 61, 61, 60, 160, 20, 255);
    const Ink ball_ink = MeasureOriginal(ball, kDark);
    Bitmap cap_white = MakeBitmap(64, 64);
    FillRect(cap_white, 3, 3, 60, 60, 255, 255, 255, 255);
    const FitCheck solid = CheckFit(ball_ink, cap_white, kDark);
    CHECK(solid.look == Look::Solid && solid.box_ok && solid.tone_ok && solid.ok());
    CHECK(solid.ratio() > 1.2);  // not within 20%, and not asked to be
    Bitmap one_over = MakeBitmap(64, 64);
    FillRect(one_over, 0, 0, 62, 62, 255, 255, 255, 255);
    CHECK(CheckFit(ball_ink, one_over, kDark).box_ok);  // box 0..62 against 1..61: the one pixel of slack
    Bitmap two_over = MakeBitmap(64, 64);
    FillRect(two_over, 0, 0, 63, 63, 255, 255, 255, 255);
    CHECK(!CheckFit(ball_ink, two_over, kDark).box_ok);  // box 0..63: two pixels over on the far side
    Bitmap dark_cap = MakeBitmap(64, 64);
    FillRect(dark_cap, 3, 3, 60, 60, 4, 4, 4, 255);
    CHECK(!CheckFit(ball_ink, dark_cap, kDark).tone_ok);  // black on a dark bar: on the other side of it from the ball
}

void TestSlotBackdrops() {
    for (int i = 0; i < kSlotCount; ++i) {
        if (Slot(i) == Slot::Phone) continue;
        CHECK(kSlots[i].backdrop == kDarkBackdrop);  // every prompt bar is dark
    }
    CHECK(Info(Slot::Phone).backdrop == kPhoneBackdrop && kPhoneBackdrop > 100.0 && kDarkBackdrop < 50.0);
    CHECK(ShapeOf(ArtKind::KeyCap) == Shape::Cap && ShapeOf(ArtKind::File) == Shape::Pictogram && ShapeOf(ArtKind::GameBitmap) == Shape::Pictogram);
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
    TestInkMeasure();
    TestInkColoursAsEncoded();
    TestBoxWithin();
    TestFitSize();
    TestResample();
    TestStrokeWidth();
    TestDrawRingAndDilate();
    TestFitSolid();
    TestFitLineOnThePhone();
    TestLineArtKeepsTheOriginalsEncoding();
    TestCheckFitCatchesTheFirstRun();
    TestSlotBackdrops();
    std::printf("prompt_glyphs_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
