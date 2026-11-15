#pragma once
//
// Button prompts that follow the device, live, on every screen (KBM-DESIGN.md phase P4). The pure part: which art goes into which glyph
// bitmap, for which glyph family and which context, and how the game's own PlayStation bitmaps are read back. No game memory, no
// files and no cvar is touched here (keyboard_native.cpp does the painting); the unit test is prompt_glyphs_test.cpp.
//
// WHY PAINTING AND NOT THE `platform` FLAG (KBM-P0, E1): flipping the movies' platform flag does not change a prompt that is already on
// screen (the garage menu and a pop-up showed 0.00% change), but painting over the glyph bitmaps in the texture dictionary changes every
// screen on the next frame (the texture cache is write-watched: LIVE, 0.36% of the band). So the flag is pinned to Xbox (0), every
// movie draws the Xbox bitmaps, and what those bitmaps hold is the family's art:
//
//   Xbox         the game's own art, put back from the copy taken when the dictionary was scanned
//   PlayStation  the faces from the game's own PlayStation bitmaps (shared_24 cross, 13 circle, 15 square, 36 triangle: 32x32, scaled up to
//                the 64x64 slot); L1 R1 L2 R2, Options and Share from the game's own bitmaps when the table below names them, else from the
//                CC0 pictures shipped next to the key caps (src/keycaps/ps_*.png, Kenney's Input Prompts)
//   Keyboard     key caps (src/keycaps), the key of the menu's binding while a menu is open, of the driving binding on the road
//
// Slots are the Xbox bitmaps (keyboard_native.cpp kGlyphs lists the dictionary names of each, and the sizes): the movies hold separate copies
// of a glyph, so one slot is one or two dictionary textures.
//

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "input_state.h"

namespace larecomp::prompts {

using Family = larecomp::input::GlyphFamily;

enum class Slot : uint8_t { A, B, X, Y, LB, RB, LT, RT, Start, Back, Phone };
constexpr int kSlotCount = 11;

struct SlotInfo {
  Slot slot;
  const char* name;
  uint32_t width;   // of the bitmap the movies draw it from
  uint32_t height;
};

inline constexpr SlotInfo kSlots[kSlotCount] = {
    {Slot::A, "A", 64, 64},      {Slot::B, "B", 64, 64},       {Slot::X, "X", 64, 64},         {Slot::Y, "Y", 64, 64},
    {Slot::LB, "LB", 64, 64},    {Slot::RB, "RB", 64, 64},     {Slot::LT, "LT", 64, 64},       {Slot::RT, "RT", 64, 64},
    {Slot::Start, "Start", 64, 32}, {Slot::Back, "Back", 64, 32}, {Slot::Phone, "Phone", 64, 64},
};

inline const SlotInfo& Info(Slot s) { return kSlots[size_t(s)]; }

inline bool SlotFromName(std::string_view name, Slot* out) {
  for (const SlotInfo& s : kSlots) {
    if (name.size() != std::char_traits<char>::length(s.name)) continue;
    bool same = true;
    for (size_t i = 0; i < name.size(); ++i) {
      const char a = name[i] >= 'a' && name[i] <= 'z' ? char(name[i] - 32) : name[i];
      const char b = s.name[i] >= 'a' && s.name[i] <= 'z' ? char(s.name[i] - 32) : s.name[i];
      same = same && a == b;
    }
    if (same) {
      *out = s.slot;
      return true;
    }
  }
  return false;
}

// ---- context: menu keys or driving keys --------------------------------------------------------------------------------------------

enum class Context : uint8_t { Menu, Drive };

// The keyboard's prompts show the key of the menu while a menu (a pause menu, a garage, a message box) is open, and before the game is entered (the title and the sign-in:
// the mapper that answers there is the menu's); on the road they show the driving key. The pad is the same in both.
inline Context ContextFor(bool game_entered, bool menu_open) { return (game_entered && !menu_open) ? Context::Drive : Context::Menu; }

// "A menu is open" is not one fact in this game. The first P4 run (2026-11-15) showed it: the pause menu is an mcForm on mcMainForm's open-form stack, but the garage's menu, its EXIT
// question and the full map are not on that stack at all, and the keyboard's prompts there were the driving keys (CAPS for B, S / W for the zoom) while the screens answer to the menu's
// keys. Three independent facts are read from the game (keyboard_native.cpp UiSignalsOf), and any one of them makes the context the menu's:
//
//   form    the open-form stack of mcMainForm is not empty (the pause menu and every list screen made from mcPerspectiveForm: OPTIONS, CONTROLLER ...)
//   movie   FlashNavigator's current movie is set. The UI statechart's `movie` command (the log's `[flash-cmd] type=3 'NAME'`) selects the movie a screen draws into and stores it at
//           navigator + 263952 (sub_8221EEC0: the name "NULL" clears it, any other name is looked up in the movie map): PAUSEMOVIE, GARAGEMOVIE, NAVSYSMOVIE (the full map) ... and
//           `NULL` again on ENTERNORMAL, the road. The garage, the EXIT question inside it (the same movie), the map and every other statechart screen are covered by this one, with no
//           list of names to keep up to date. A name that starts with HUD or SIDEKICK is the road's own (kRoadMoviePrefixes) and does not count.
//   layer   FlashNavigator's FIFO of modal message boxes is not empty (navigator + 263992: a pop-up that takes the input before anything else)
//
// Not on the list on purpose: the Sidekick phone. Its message comes over the road and does not stop the driving; its one button is answered through the menu's mapper (the Back /
// Select key), so its glyph shows kb_ui_select in both contexts (kMenuCvar / kRoadCvar below) and the road's context stays the road's.
struct UiSignals {
  bool form = false;
  bool movie = false;
  bool layer = false;
  bool any() const { return form || movie || layer; }
  bool operator==(const UiSignals& o) const { return form == o.form && movie == o.movie && layer == o.layer; }
};

inline Context ContextFor(bool game_entered, const UiSignals& ui) { return ContextFor(game_entered, ui.any()); }

// Does the key caps' context have to be painted again? Only the keyboard family has two sets (the menu's and the road's); `painted_menu` = the context they were last painted for.
inline bool ContextNeedsRepaint(Family family, bool game_entered, const UiSignals& ui, bool painted_menu) {
  return family == Family::Keyboard && (ContextFor(game_entered, ui) == Context::Menu) != painted_menu;
}

// A movie name that belongs to the road, not to a screen: HUDMOVIE and the like (FlashNavigator names HUDMOVIE in explicit commands, `type=1 'HUDMOVIE' 'playerlist'`; the HUD's two
// movies are HUDMAPMOVIE and the gauge) and the Sidekick phone, whose message comes over the road. None of them is a screen the keyboard has to leave the road for.
inline constexpr std::string_view kRoadMoviePrefixes[] = {"HUD", "SIDEKICK"};
inline bool IsRoadMovieName(std::string_view name) {
  for (std::string_view prefix : kRoadMoviePrefixes)
    if (name.size() >= prefix.size() && name.substr(0, prefix.size()) == prefix) return true;
  return false;
}

// The reason text of the log line and of the report: "form", "movie GARAGEMOVIE", "layer" joined by "+", "none" when nothing is open (the road).
inline std::string DescribeUi(const UiSignals& ui, std::string_view movie_name) {
  std::string out;
  auto add = [&](const std::string& part) {
    if (!out.empty()) out += '+';
    out += part;
  };
  if (ui.form) add("form");
  if (ui.movie) add(movie_name.empty() ? std::string("movie") : "movie " + std::string(movie_name));
  if (ui.layer) add("layer");
  return out.empty() ? std::string("none") : out;
}

// The cvar that holds the key of a slot's prompt. The menu's table has a key for every slot (the Phone's accept is the Back button, so the phone shows the menu's Back key in
// both contexts: a Flash movie answers through the menu's mapper). The road's table has one for the buttons that do something on the road; an empty name or an empty value
// there means "the menu's key".
inline constexpr std::string_view kMenuCvar[kSlotCount] = {
    "kb_ui_accept", "kb_ui_back", "kb_ui_x", "kb_ui_y", "kb_ui_lb", "kb_ui_rb", "kb_ui_lt", "kb_ui_rt", "kb_ui_start", "kb_ui_select", "kb_ui_select",
};
inline constexpr std::string_view kRoadCvar[kSlotCount] = {
    "kb_drive_handbrake",  // A: the handbrake
    "kb_drive_weight",     // B: weight transfer
    "kb_drive_nitro",      // X: nitro
    "kb_drive_headlights", // Y: headlights
    "",                    // LB: nothing of its own on the road
    "kb_drive_camera",     // RB: the camera
    "kb_drive_brake",      // LT: the brake
    "kb_drive_gas",        // RT: the gas
    "kb_drive_pause",      // Start: pause (empty by default: the menu's Start key then)
    "kb_drive_map",        // Back: the map
    "",                    // Phone: the menu's Back key
};

// The cvars to try, in order, for `slot` in `ctx`: at most two (the road's own, then the menu's).
inline std::array<std::string_view, 2> KeyCvars(Slot slot, Context ctx) {
  const std::string_view menu = kMenuCvar[size_t(slot)];
  const std::string_view road = kRoadCvar[size_t(slot)];
  if (ctx == Context::Drive && !road.empty()) return {road, menu};
  return {menu, std::string_view()};
}

// The first key of a binding list ("Return,Space") that has a cap, else the first key (drawn as the blank cap), else nothing (the art stays). `has_cap(name)` asks the folder.
template <class HasCap>
std::string PickKey(const std::vector<std::string>& keys, HasCap&& has_cap) {
  for (const std::string& k : keys)
    if (has_cap(k)) return k;
  return keys.empty() ? std::string() : keys.front();
}

// ---- the art of a slot ----------------------------------------------------------------------------------------------------------------------

enum class ArtKind : uint8_t { Original, GameBitmap, File, KeyCap };

struct Art {
  ArtKind kind = ArtKind::Original;
  std::string id;  // GameBitmap: the dictionary texture ("shared_24.dds"); File: the stem in the key cap folder ("ps_l1"); KeyCap: the key name ("Return")
  bool operator==(const Art& o) const { return kind == o.kind && id == o.id; }
  bool operator!=(const Art& o) const { return !(*this == o); }
};

inline std::string Describe(const Art& a) {
  switch (a.kind) {
    case ArtKind::Original: return "original";
    case ArtKind::GameBitmap: return "game:" + a.id;
    case ArtKind::File: return "file:" + a.id;
    case ArtKind::KeyCap: return "key:" + a.id;
  }
  return "?";
}

// Where the PlayStation art of a slot comes from. `bitmap` = the game's own PlayStation bitmap in the dictionary (null = not identified yet); `file` = the stem of the picture
// shipped next to the key caps (Kenney's Input Prompts, CC0), used when the bitmap is not named or not in the dictionary. A 64x32 slot reads "<stem>_w.png" (the key caps' rule).
//
// WHAT THE GAME HAS AND WHAT IT DOES NOT (KBM-P4 run of 2026-11-15, the dictionary dump drawn on perf\out\KBM-P4\P4-glyphs.png): the game's own PlayStation art is the four faces only
// (shared_24 cross, 13 circle, 15 square, 36 triangle, 32x32: the ones keyboard_native.cpp has always used for the PlayStation iconIDs 50..53). It has NO PlayStation shoulder, trigger,
// Start or Back bitmap: the 3 unlisted 64x64 bitmaps (shared_8, 32, 34) and the 4 unlisted 64x32 ones (shared_5, 28, 54, 55) are other UI art (a grey ball, a grey tile, a race flag; a
// play arrow, two grey buttons, a curved arrow: kUnrelatedBitmaps lists them for the sheet), and the 128x128 ones are the d-pad and the sticks. So the shoulders, Options and Create are
// the shipped pictures in src\keycaps\ps_*.png, which is the right answer and not a stop-gap: L1 R1 L2 R2 are Kenney's Input Prompts (CC0) cut to the 64x64 slot; Options and Create
// on the round 64x32 slots are a light disc cut out like the game's own Back / Start discs (shared_31 / 33: the same size, the same grey, the symbol is the window), made by
// perf\scripts\kbm\make_ps_prompts.py from Kenney's PlayStation 5 "options" (the three bars) and "create" (the button with its rays) pictograms. The debug cvar prompts_ps_map can still
// point a slot at a dictionary bitmap, to try one without a build.
inline constexpr std::string_view kUnrelatedBitmaps[] = {"shared_5.dds", "shared_8.dds", "shared_28.dds", "shared_32.dds", "shared_34.dds", "shared_54.dds", "shared_55.dds"};
struct PsSource {
  Slot slot;
  const char* bitmap;
  const char* file;
};

inline constexpr PsSource kPsSources[kSlotCount] = {
    {Slot::A, "shared_24.dds", "ps_cross"},   {Slot::B, "shared_13.dds", "ps_circle"},  {Slot::X, "shared_15.dds", "ps_square"},
    {Slot::Y, "shared_36.dds", "ps_triangle"}, {Slot::LB, nullptr, "ps_l1"},            {Slot::RB, nullptr, "ps_r1"},
    {Slot::LT, nullptr, "ps_l2"},              {Slot::RT, nullptr, "ps_r2"},            {Slot::Start, nullptr, "ps_options"},
    {Slot::Back, nullptr, "ps_share"},         {Slot::Phone, nullptr, "ps_share"},
};

// "LB=shared_8.dds, RB=shared_32.dds" -> [(LB, "shared_8.dds"), ...]; unknown slots and empty names are skipped.
inline std::vector<std::pair<Slot, std::string>> ParsePsMap(std::string_view text) {
  std::vector<std::pair<Slot, std::string>> out;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t comma = text.find(',', pos);
    if (comma == std::string_view::npos) comma = text.size();
    std::string_view item = text.substr(pos, comma - pos);
    pos = comma + 1;
    while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.remove_prefix(1);
    while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) item.remove_suffix(1);
    const size_t eq = item.find('=');
    if (eq == std::string_view::npos) continue;
    std::string_view slot_name = item.substr(0, eq);
    std::string_view bitmap = item.substr(eq + 1);
    while (!slot_name.empty() && slot_name.back() == ' ') slot_name.remove_suffix(1);
    while (!bitmap.empty() && bitmap.front() == ' ') bitmap.remove_prefix(1);
    Slot slot = Slot::A;
    if (bitmap.empty() || !SlotFromName(slot_name, &slot)) continue;
    out.emplace_back(slot, std::string(bitmap));
  }
  return out;
}

// What the PlayStation family shows in `slot`: the game's bitmap (the override of prompts_ps_map first, then the table) when `has_bitmap(name)` says the dictionary holds it, else
// the shipped file when `has_file(stem)` says it is there, else nothing (the original art stays).
template <class HasBitmap, class HasFile>
Art PlayStationArt(Slot slot, const std::vector<std::pair<Slot, std::string>>& overrides, HasBitmap&& has_bitmap, HasFile&& has_file) {
  for (const auto& o : overrides)
    if (o.first == slot && has_bitmap(o.second)) return {ArtKind::GameBitmap, o.second};
  const PsSource& src = kPsSources[size_t(slot)];
  if (src.bitmap && has_bitmap(std::string(src.bitmap))) return {ArtKind::GameBitmap, src.bitmap};
  if (has_file(std::string(src.file))) return {ArtKind::File, src.file};
  return {};
}

// What `family` shows in `slot` in `ctx`. `key_of(cvar)` = the key a binding cvar gives ("" = none).
template <class HasBitmap, class HasFile, class KeyOf>
Art Decide(Family family, Context ctx, Slot slot, const std::vector<std::pair<Slot, std::string>>& overrides, HasBitmap&& has_bitmap, HasFile&& has_file, KeyOf&& key_of) {
  switch (family) {
    case Family::Xbox: return {};
    case Family::PlayStation: return PlayStationArt(slot, overrides, has_bitmap, has_file);
    case Family::Keyboard: {
      for (std::string_view cvar : KeyCvars(slot, ctx)) {
        if (cvar.empty()) continue;
        const std::string key = key_of(cvar);
        if (!key.empty()) return {ArtKind::KeyCap, key};
      }
      return {};
    }
  }
  return {};
}

// ---- the game's own bitmaps, read back ---------------------------------------------------------------------------------------------------
//
// The dictionary's glyphs are DXT5 (BC3), fmt 20, endian 8-in-16: every 16 bytes of a block are byte-swapped in pairs in the guest's memory, and the blocks of a small texture are
// tiled. keyboard_native.cpp reads the blocks of a texture out at their tiled offsets, in linear block order (row by row), so what arrives here is `blocks`: w/4 * h/4 blocks of
// 16 bytes, still swapped. (The painter writes in the other direction with modloader's CompressBlocks + TileXbox360, which ends in the same swap.)

// One block -> 16 RGBA pixels, row by row. Colour is always the four-colour mode (BC2 / BC3 never use the 3-colour + transparent one).
inline void DecodeBc3GuestBlock(const uint8_t* guest, uint8_t out_rgba[64]) {
  uint8_t b[16];
  for (int i = 0; i < 8; ++i) {
    b[2 * i] = guest[2 * i + 1];
    b[2 * i + 1] = guest[2 * i];
  }
  // alpha: two endpoints and 16 three-bit indices
  const int a0 = b[0], a1 = b[1];
  int pal[8];
  pal[0] = a0;
  pal[1] = a1;
  if (a0 > a1) {
    for (int k = 1; k <= 6; ++k) pal[1 + k] = ((7 - k) * a0 + k * a1) / 7;
  } else {
    for (int k = 1; k <= 4; ++k) pal[1 + k] = ((5 - k) * a0 + k * a1) / 5;
    pal[6] = 0;
    pal[7] = 255;
  }
  uint64_t abits = 0;
  for (int i = 0; i < 6; ++i) abits |= uint64_t(b[2 + i]) << (8 * i);
  // colour: two RGB565 endpoints and 16 two-bit indices
  const uint16_t c0 = uint16_t(b[8] | (b[9] << 8)), c1 = uint16_t(b[10] | (b[11] << 8));
  auto expand = [](uint16_t v, int rgb[3]) {
    const int r = (v >> 11) & 31, g = (v >> 5) & 63, bl = v & 31;
    rgb[0] = (r << 3) | (r >> 2);
    rgb[1] = (g << 2) | (g >> 4);
    rgb[2] = (bl << 3) | (bl >> 2);
  };
  int col[4][3];
  expand(c0, col[0]);
  expand(c1, col[1]);
  for (int c = 0; c < 3; ++c) {
    col[2][c] = (2 * col[0][c] + col[1][c]) / 3;
    col[3][c] = (col[0][c] + 2 * col[1][c]) / 3;
  }
  const uint32_t cbits = uint32_t(b[12]) | (uint32_t(b[13]) << 8) | (uint32_t(b[14]) << 16) | (uint32_t(b[15]) << 24);
  for (int i = 0; i < 16; ++i) {
    const int ci = int((cbits >> (2 * i)) & 3);
    out_rgba[i * 4 + 0] = uint8_t(col[ci][0]);
    out_rgba[i * 4 + 1] = uint8_t(col[ci][1]);
    out_rgba[i * 4 + 2] = uint8_t(col[ci][2]);
    out_rgba[i * 4 + 3] = uint8_t(pal[(abits >> (3 * i)) & 7]);
  }
}

// `blocks` (w/4 * h/4 blocks, linear, as read from the guest) -> w x h RGBA8, tightly packed; empty when the size or the byte count does not fit.
inline std::vector<uint8_t> DecodeBc3GuestSurface(const std::vector<uint8_t>& blocks, uint32_t w, uint32_t h) {
  if (w == 0 || h == 0 || (w % 4) || (h % 4)) return {};
  const uint32_t bw = w / 4, bh = h / 4;
  if (blocks.size() != size_t(bw) * bh * 16) return {};
  std::vector<uint8_t> rgba(size_t(w) * h * 4, 0);
  uint8_t px[64];
  for (uint32_t by = 0; by < bh; ++by) {
    for (uint32_t bx = 0; bx < bw; ++bx) {
      DecodeBc3GuestBlock(blocks.data() + (size_t(by) * bw + bx) * 16, px);
      for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 4; ++x)
          for (uint32_t c = 0; c < 4; ++c) rgba[(size_t(by * 4 + y) * w + (bx * 4 + x)) * 4 + c] = px[(y * 4 + x) * 4 + c];
    }
  }
  return rgba;
}

// The standard (unswapped) form of a block, for the test and for anything that wants to feed the decoder a block it built by hand.
inline void SwapGuestPairs(const uint8_t in[16], uint8_t out[16]) {
  for (int i = 0; i < 8; ++i) {
    out[2 * i] = in[2 * i + 1];
    out[2 * i + 1] = in[2 * i];
  }
}

}  // namespace larecomp::prompts
