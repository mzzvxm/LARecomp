// Ultrawide: the aspect ratio the game is shown at. See ultrawide.h.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include "ultrawide.h"
#include "settings/immortal.h"

#include "settings/larecomp_settings.h"
#include <rex/kernel/title_runtime.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "mc_engine/hooks/hooks.h"
#include "mc_engine/hooks/hooks_internal.h"
#include "larecomp_log.h"
#include "mc_engine/menus/swf_dom.h"
#if defined(LARECOMP_HAS_GPU)
#include "gpu/gpu.h"
#endif


namespace mc::ultrawide {

namespace {

// The window's size, width << 32 | height; 0 until the app says.
std::atomic<uint64_t> g_window{0};
// The aspect ratio in force, between kBaseAspect and kMaxAspect: the frame's (frame_fit.h).
std::atomic<double> g_aspect{kBaseAspect};
// The choice in force (mc::frame::Choice), -1 until the first Recompute.
std::atomic<int> g_choice{-1};
// The setting hud_width, 0 to 100: from Init and its change callback; read by the guest hooks that name
// the HUD's pieces and by the renderer (HudWidthFraction).
std::atomic<int> g_hud_width{0};

// The layout in force and what the last setting or window changed in it (frame_fit.h Tracker): one
// writer at a time, no cvar is read or set while this is held.
std::mutex g_layout_mutex;
mc::frame::Tracker g_tracker;
// What the log last said: a window dragged to a new size changes the aspect ratio at every step.
double g_logged_aspect = 0.0;
bool g_logged_boxed = false;
bool g_logged_wide = false;
bool g_logged_tall = false;

// The setting's text, as the last change left it (a change callback is given the new value; reading
// the cvar inside the callback is not safe, it may run with the registry locked).
std::mutex g_ask_mutex;
std::string& g_ask = larecomp::Leak<std::string>();
bool g_ask_known = false;  // Init ran

// The SDK presenter (the Xenia renderer) fits the 1280x720 picture into the window with black bars
// unless present_letterbox is off, when it stretches the picture over the whole window: what a
// widened picture needs. The value given to it last (-1: none yet, so the first window size sets it
// whatever an old config file says); the Plume renderer has its own present.
int g_xenia_stretch = -1;

bool PlumeRenderer() {
#if defined(LARECOMP_HAS_GPU)
  return mcla::gpu::Enabled();
#else
  return false;
#endif
}

// The setting's text as a choice. Anything that is none of them is AUTO. The Xenia renderer's present
// fills the window with the picture and has no frame to draw it in: it always follows the window.
mc::frame::Choice ChoiceOf(std::string_view ask) {
  mc::frame::Choice choice = mc::frame::Choice::kAuto;
  if (!mc::frame::ParseChoice(ask, &choice)) {
    choice = mc::frame::Choice::kAuto;
  }
  return PlumeRenderer() ? choice : mc::frame::Choice::kAuto;
}

// The narrowest picture the renderer shows: a portrait 9:16 on the native renderer; the game's 16:9 on the
// Xenia one, whose present stretches the picture over the window and has no vertical squeeze for the UI, so
// a window taller than 16:9 keeps 16:9 with bars above and below there, as it always did.
double MinAspect() { return PlumeRenderer() ? mc::frame::kAspectMin : mc::frame::kAspect16x9; }

// The window's size arrives (SetupPresentation) before Init does: the cvar has its value by then.
std::string Ask() {
  {
    std::lock_guard<std::mutex> lock(g_ask_mutex);
    if (g_ask_known) {
      return g_ask;
    }
  }
  return larecomp::settings::aspect_ratio.Get();
}

// The choice in force, for a caller that has no setting text (no registry read once Recompute has run).
mc::frame::Choice ChoiceNow() {
  const int choice = g_choice.load(std::memory_order_relaxed);
  return choice >= 0 ? mc::frame::Choice(choice) : ChoiceOf(Ask());
}

// The setting or the window changed: the layout is worked out again, and what the change touches is
// done at once: the cameras' aspect ratio in guest memory (the camera hooks and the renderer read
// DisplayAspect()), the log. The present, the render scale and the mouse read the frame themselves
// (FrameFor) at their next frame, so a live change of the setting needs nothing more. Never called
// with the cvar registry locked except from the setting's own change callback, which takes `ask`
// from the callback's argument.
void Recompute(std::string_view ask) {
  const uint64_t window = g_window.load(std::memory_order_acquire);
  const uint32_t window_w = uint32_t(window >> 32);
  const uint32_t window_h = uint32_t(window & 0xFFFFFFFFu);
  const mc::frame::Choice choice = ChoiceOf(ask);
  uint32_t change = 0;
  mc::frame::Layout layout;
  bool log = false;
  {
    std::lock_guard<std::mutex> lock(g_layout_mutex);
    change = g_tracker.Update(choice, window_w, window_h, MinAspect());
    layout = g_tracker.layout();
    g_aspect.store(layout.aspect, std::memory_order_release);
    g_choice.store(int(choice), std::memory_order_relaxed);
    if (change & (mc::frame::kChoiceChanged | mc::frame::kAspectChanged)) {
      log = (change & mc::frame::kChoiceChanged) != 0 || std::abs(layout.aspect - g_logged_aspect) > 0.02 ||
            layout.wide != g_logged_wide || layout.tall != g_logged_tall || layout.boxed() != g_logged_boxed;
      if (log) {
        g_logged_aspect = layout.aspect;
        g_logged_boxed = layout.boxed();
        g_logged_wide = layout.wide;
        g_logged_tall = layout.tall;
      }
    }
  }
  if (change & mc::frame::kAspectChanged) {
    ApplyAspectRatioPatch();
  }
  if (log) {
    LARECOMP_APP_INFO(
        "ultrawide: the aspect ratio is {:.4f}:1 (setting \"{}\", window {}x{}): the frame is {}x{} at ({}, {}), "
        "bars {} left {} right {} above {} below{}",
        layout.aspect, mc::frame::ChoiceName(layout.choice), window_w, window_h, layout.frame.w,
        layout.frame.h, layout.frame.x, layout.frame.y, layout.bar_left(), layout.bar_right(), layout.bar_top(),
        layout.bar_bottom(),
        layout.wide ? "" : layout.tall ? ", taller than 16:9: the vertical view grows (Vert+)" : ", the game's own 16:9");
  }
}

}  // namespace

void SetWindowSize(uint32_t width, uint32_t height) {
  if (width == 0 || height == 0) {
    return;
  }
  g_window.store(uint64_t(width) << 32 | height, std::memory_order_release);
  Recompute(Ask());
  // The Xenia renderer's present follows the picture. Here, on the UI thread outside any cvar
  // callback, where setting another cvar is safe.
  // CHECK: the Xenia renderer in a 21:9 window: the picture fills it (present_letterbox off) and
  // goes back to black bars when the window is made 16:9 again.
  if (!PlumeRenderer()) {
    const int stretch = Active() ? 1 : 0;
    if (stretch != g_xenia_stretch) {
      g_xenia_stretch = stretch;
      larecomp::cvar::SetFlagByName("present_letterbox", stretch ? "false" : "true");
    }
  }
}

double DisplayAspect() { return g_aspect.load(std::memory_order_acquire); }

double UiWidthFraction() { return mc::frame::UiFraction(DisplayAspect()); }

double UiHeightFraction() { return mc::frame::UiHeightFraction(DisplayAspect()); }

double CameraFov(double fov_degrees) { return mc::frame::VertPlusFov(fov_degrees, DisplayAspect()); }

double VertPlusScale() { return mc::frame::VertPlusTan(DisplayAspect()); }

mc::frame::Rect FrameFor(uint32_t window_width, uint32_t window_height) {
  // The setting as it is now, the window as the caller measured it (not the app's last report: the
  // renderer's first frames and the map's mouse have their own sizes, and the start of the renderer comes
  // before the app's first report).
  return mc::frame::FrameOf(window_width, window_height,
                            mc::frame::AspectOf(ChoiceNow(), window_width, window_height, MinAspect()));
}

mc::frame::Rect FrameRect() {
  const uint64_t window = g_window.load(std::memory_order_acquire);
  return FrameFor(uint32_t(window >> 32), uint32_t(window & 0xFFFFFFFFu));
}

double HudWidthFraction() {
  const int percent = g_hud_width.load(std::memory_order_relaxed);
  return percent > 0 && Active() ? double(percent) / 100.0 : 0.0;
}

float MarkerStageX(float stage_x) {
  if (!Wide()) {
    return stage_x;
  }
  // The UI is squeezed about the middle of the stage by k (gpu/scene.cpp, UiSqueezeOf): a clip at
  // x lands at 640 + (x - 640) * k. The game worked x out from the widened camera
  // (x = 640 + 640 * ndc_x), which is where the 3D point is once the picture is stretched, so the
  // clip is placed at the x the squeeze maps back to it.
  const float k = float(UiWidthFraction());
  return kStageHalfWidth + (stage_x - kStageHalfWidth) / k;
}

float MarkerStageY(float stage_y) {
  if (!Tall()) {
    return stage_y;
  }
  // The same as MarkerStageX turned round: the UI is squeezed about the middle of the stage's height by
  // k = UiHeightFraction() (gpu/scene.cpp), a clip at y lands at 360 + (y - 360) * k. The game worked y out
  // (y = 360 - 360 * ndc_y) from the Vert+ camera, which is where the 3D point is once the picture is
  // stretched to the frame, so the clip is placed at the y the squeeze maps back to it.
  const float k = float(UiHeightFraction());
  return kStageHalfHeight + (stage_y - kStageHalfHeight) / k;
}

namespace {
// The map's own 16:9 constant (0x8201FAC8) and the half of the 25 degree vertical field of view it
// goes with (0.218166 radians, 0x8208AC60: sub_826206B0 reads it as the same half angle).
// CHECK: both numbers against the constants in the guest image (1.7778 and 0.218166).
constexpr double kMapBaseAspect = 1.7778;
constexpr double kMapHalfFov = 0.218166;
}  // namespace

double MapVisibleHalfFov() {
  const double aspect = DisplayAspect();
  if (!mc::frame::IsTall(aspect)) {
    return kMapHalfFov;
  }
  // Vert+: the camera's half vertical angle grows as tan(half') = tan(half) * (16/9) / aspect, and the
  // depth of the ground the function works out from it grows with it (tan(tilt +- half)).
  return std::atan(std::tan(kMapHalfFov) * mc::frame::VertPlusTan(aspect));
}

double MapVisibleAspect() {
  const double aspect = DisplayAspect();
  if (mc::frame::IsTall(aspect)) {
    // The width of the box is tan(f27 * half) * ...: with the half angle grown (MapVisibleHalfFov) the
    // value that keeps f27 * half the game's own keeps the box as wide as the game has it at 16:9, which
    // is the width the Vert+ camera shows.
    return kMapBaseAspect * kMapHalfFov / MapVisibleHalfFov();
  }
  if (!(aspect > kBaseAspect * 1.001)) {
    return kMapBaseAspect;
  }
  // The game's half width at 16:9 is tan(kMapBaseAspect * kMapHalfFov). The same share of a picture
  // that is aspect / kBaseAspect times as wide is that times the ratio; the game's formula is
  // inverted for the aspect ratio that gives it.
  const double half_width_16x9 = std::tan(kMapBaseAspect * kMapHalfFov);
  return std::atan(half_width_16x9 * (aspect / kBaseAspect)) / kMapHalfFov;
}

namespace {
// A world-icon bracket is open (begin sent, end not yet). The end is sent even if the picture has
// stopped being wide in between (a window resized inside the draw), so the renderer's state closes.
std::atomic<bool> g_world_icons_open{false};
}  // namespace

void NoteWorldIcons(bool begin) {
#if defined(LARECOMP_HAS_GPU)
  if (!PlumeRenderer()) {
    return;
  }
  if (begin) {
    if (!Active()) {
      return;
    }
    g_world_icons_open.store(true, std::memory_order_relaxed);
    mcla::gpu::OnGuestSceneComposited(kUiWorldBegin);
  } else if (g_world_icons_open.exchange(false, std::memory_order_relaxed)) {
    mcla::gpu::OnGuestSceneComposited(kUiWorldEnd);
  }
#else
  (void)begin;
#endif
}

// --- the HUD's corner pieces (the setting hud_width; ultrawide.h says how they were found) ----------------

namespace {

enum class Side : int8_t { kLeft = -1, kRight = 1 };
// Which edge a piece goes to in a TALL picture: the top one or the bottom one, from where its clip sits on the
// 1280x720 stage (y above or below the middle, 360).
enum class Vert : int8_t { kTop = -1, kBottom = 1 };
// Which of the two HUD movies a piece is of.
enum class HudMovie : uint8_t { kGauge, kMinimap };

struct Piece {
  const char* name;  // the instance's name, a child of the clip named "HUD"
  Side side;
  Vert vert;
  HudMovie movie;
};

// hud.xsf: the two lists of the script's widescreen branch (the action block at 0xB982: `_x` + 135 for
// the right pieces, - 135 for the left ones), and `chat`; hudmap.xsf: the minimap's four clips.
// CHECK: `chat` (no offset in the script) is a left piece: that its text box does not span the middle.
// The top or bottom edge of a tall picture is the clip's place on the stage (its origin's y before the
// script's offsets, from the movies: info 630, glows 625, speedometer 584 and the minimap's miniBG 484, ring 573
// are in the lower half, so bottom; posmeter 79, playerList 90, delivery 75, CTF_score 96, flagTime 85,
// flagsDelivered 73, bomb 110, flag 105, lapmeter 80, laptime 128, totaltime 49 and chat 104 in the upper
// half, so top).
constexpr Piece kPieces[] = {
    {"info", Side::kRight, Vert::kBottom, HudMovie::kGauge},
    {"glows", Side::kRight, Vert::kBottom, HudMovie::kGauge},
    {"speedometer", Side::kRight, Vert::kBottom, HudMovie::kGauge},
    {"posmeter", Side::kRight, Vert::kTop, HudMovie::kGauge},
    {"playerList", Side::kRight, Vert::kTop, HudMovie::kGauge},
    {"delivery", Side::kRight, Vert::kTop, HudMovie::kGauge},
    {"CTF_score", Side::kRight, Vert::kTop, HudMovie::kGauge},
    {"flagTime", Side::kLeft, Vert::kTop, HudMovie::kGauge},
    {"flagsDelivered", Side::kLeft, Vert::kTop, HudMovie::kGauge},
    {"bomb", Side::kLeft, Vert::kTop, HudMovie::kGauge},
    {"flag", Side::kLeft, Vert::kTop, HudMovie::kGauge},
    {"lapmeter", Side::kLeft, Vert::kTop, HudMovie::kGauge},
    {"laptime", Side::kLeft, Vert::kTop, HudMovie::kGauge},
    {"totaltime", Side::kLeft, Vert::kTop, HudMovie::kGauge},
    {"chat", Side::kLeft, Vert::kTop, HudMovie::kGauge},
    {"miniBG", Side::kLeft, Vert::kBottom, HudMovie::kMinimap},
    {"ring", Side::kLeft, Vert::kBottom, HudMovie::kMinimap},
    {"ringNorth", Side::kLeft, Vert::kBottom, HudMovie::kMinimap},
    {"renderMe", Side::kLeft, Vert::kBottom, HudMovie::kMinimap},
};
constexpr size_t kPieceCount = sizeof(kPieces) / sizeof(kPieces[0]);
static_assert(kPieceCount <= 32, "the log's bit mask is 32 bits");

// The Flash player's instance (rage::swfINSTANCE), as hooks/swf_dom.cpp has it.
constexpr uint32_t kInstAlpha = 79;        // u8, 255 opaque
constexpr uint32_t kInstParent = 184;      // the parent instance, 0 for the movie's root
constexpr uint32_t kInstFirstChild = 188;
constexpr uint32_t kInstNext = 192;        // the next child of the parent
constexpr uint32_t kInstName = 196;        // char*, the name the movie gave the instance, or 0

bool PlausibleEa(uint32_t ea) { return ea >= 0x10000u && ea < 0xC0000000u; }

// The guest string at `ea` is `text`.
bool GuestNameIs(const uint8_t* base, uint32_t ea, const char* text) {
  if (!PlausibleEa(ea)) {
    return false;
  }
  const char* s = reinterpret_cast<const char*>(base + ea);
  for (size_t i = 0;; ++i) {
    if (s[i] != text[i]) {
      return false;
    }
    if (text[i] == '\0') {
      return true;
    }
  }
}

// The guest string at `ea`, up to 31 characters of printable text ("" if it is not).
std::string GuestName(const uint8_t* base, uint32_t ea) {
  std::string out;
  if (!PlausibleEa(ea)) {
    return out;
  }
  const char* s = reinterpret_cast<const char*>(base + ea);
  for (size_t i = 0; i < 31 && s[i] != '\0'; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x20 || c > 0x7E) {
      return std::string();
    }
    out.push_back(char(c));
  }
  return out;
}

// The guest memory the hooks read: the base is the same for the whole run.
const uint8_t* GuestBase() {
  static std::atomic<const uint8_t*> cached{nullptr};
  const uint8_t* base = cached.load(std::memory_order_relaxed);
  if (!base) {
    if (rex::kernel::TitleRuntime* runtime = REX_RUNTIME()) {
      base = runtime->GetMembase().GetBase();
      cached.store(base, std::memory_order_relaxed);
    }
  }
  return base;
}

// What the log has said: one line for each piece found (the bit is the table's index), one for each other
// child of the HUD clip (the names, at most 16: what stays in the middle), and one for the minimap's
// pieces when the expanded map keeps them in the middle.
std::atomic<uint32_t> g_pieces_logged{0};
std::mutex g_kept_mutex;
std::vector<std::string>& g_kept_logged = larecomp::Leak<std::vector<std::string>>();
std::atomic<bool> g_expanded_logged{false};
constexpr size_t kMaxKeptLogged = 16;

// The first child of `parent` with this name, or 0.
uint32_t FindChildNamed(const uint8_t* base, uint32_t parent, const char* name) {
  uint32_t child = ReadGuestU32(base, parent + kInstFirstChild);
  for (int guard = 0; PlausibleEa(child) && guard < 256; ++guard) {
    if (GuestNameIs(base, ReadGuestU32(base, child + kInstName), name)) {
      return child;
    }
    child = ReadGuestU32(base, child + kInstNext);
  }
  return 0;
}

// The piece the draw of this instance is the whole of, or null: a child of a clip named "HUD" whose name is
// in the table, and, for the minimap, while the minimap shows (the clip `ring` is not transparent: in the
// expanded map the script hides it and the other three).
const Piece* PieceOf(const uint8_t* base, uint32_t instance) {
  if (!PlausibleEa(instance)) {
    return nullptr;
  }
  const uint32_t parent = ReadGuestU32(base, instance + kInstParent);
  if (!PlausibleEa(parent) || !GuestNameIs(base, ReadGuestU32(base, parent + kInstName), "HUD")) {
    return nullptr;
  }
  const uint32_t name_ea = ReadGuestU32(base, instance + kInstName);
  for (size_t i = 0; i < kPieceCount; ++i) {
    const Piece& piece = kPieces[i];
    if (!GuestNameIs(base, name_ea, piece.name)) {
      continue;
    }
    if (piece.movie == HudMovie::kMinimap) {
      const uint32_t ring = FindChildNamed(base, parent, "ring");
      if (!ring || base[ring + kInstAlpha] == 0) {
        if (!g_expanded_logged.exchange(true, std::memory_order_relaxed)) {
          LARECOMP_APP_INFO(
              "ultrawide: the minimap's pieces stay in the middle while its ring is transparent "
              "(the expanded map, or the HUD hidden)");
        }
        return nullptr;
      }
    }
    if (!(g_pieces_logged.fetch_or(1u << i, std::memory_order_relaxed) & (1u << i))) {
      LARECOMP_APP_INFO("ultrawide: HUD piece \"{}\" ({}) is anchored to the {} edge",
                        piece.name, piece.movie == HudMovie::kGauge ? "hud.swf" : "hudmap.swf",
                        Tall() ? (piece.vert == Vert::kTop ? "top" : "bottom")
                               : (piece.side == Side::kLeft ? "left" : "right"));
    }
    return &piece;
  }
  // Another child of the HUD clip: stays in the middle with the rest of the HUD. The first few are named
  // in the log, so that a piece the lists missed shows up in a test.
  const std::string name = GuestName(base, name_ea);
  if (!name.empty()) {
    std::lock_guard<std::mutex> lock(g_kept_mutex);
    if (g_kept_logged.size() < kMaxKeptLogged &&
        std::find(g_kept_logged.begin(), g_kept_logged.end(), name) == g_kept_logged.end()) {
      g_kept_logged.push_back(name);
      LARECOMP_APP_INFO("ultrawide: HUD clip \"{}\" stays in the middle ({} of {} logged)", name,
                        g_kept_logged.size(), kMaxKeptLogged);
    }
  }
  return nullptr;
}

// The instance whose draw this thread has told the renderer about, 0 when none: the bracket is closed by
// the exit of that same draw, and a draw inside a piece is no piece of its own.
thread_local uint32_t t_open_piece = 0;

// --- the title screen's prompt bar ---------------------------------------------------------------------
//
// The title's "PRESS START" (white text and the Start button on a dark bar, bottom right) is the movie the UI
// manager has as PROMPTMOVIE, the game's shared prompt bar (a census at 3440x1440, 2026-11-12: of the movies
// with a prompt bar, PROMPTMOVIE's `allprompts` is the one drawn straight onto the screen; PAUSEMOVIE's is
// inside the pause menu's card, NAVSYSMOVIE's and POPUPMOVIE's belong to their own panels). It sits at the
// screen's right edge on a wide picture (the bottom edge on a tall one), as far from it as on a 16:9 screen,
// whatever hud_width is (the owner, 2026-11-12: centred, it looked out of place under the wide title view). Only until the game is
// entered (NoteGameEntered: the player's control map is first built, after the title and the sign-in steps;
// the driving HUD's movie is no sign of it, it is drawn hidden from the first title frame): later prompts of
// the same movie belong to screens whose menus stay in the middle.
// CHECK: that nothing but the prompt bar is drawn by PROMPTMOVIE before the game is entered (the sign-in and
// profile steps included), and that after quitting to the title the bar staying in the middle is acceptable.

std::atomic<bool> g_game_entered{false};
std::atomic<bool> g_title_prompt_logged{false};

// `instance` is PROMPTMOVIE's `screen` (the root's child that holds the whole bar: the player draws a movie's
// root's children through sub_825FB348, not the root itself) and the game has not been entered yet.
bool IsTitlePrompt(const uint8_t* base, uint32_t instance) {
  if (g_game_entered.load(std::memory_order_relaxed) ||
      !GuestNameIs(base, ReadGuestU32(base, instance + kInstName), "screen")) {
    return false;
  }
  const uint32_t parent = ReadGuestU32(base, instance + kInstParent);
  if (!PlausibleEa(parent) || ReadGuestU32(base, parent + kInstParent) != 0) {
    return false;  // (only a root's `screen`, a few a frame, gets the lookup)
  }
  larecomp::swf::Movie movie;
  return larecomp::swf::FindMovie(base, "PROMPTMOVIE", movie) && movie.root == parent;
}

}  // namespace

void NoteGameEntered() {
  if (!g_game_entered.exchange(true, std::memory_order_relaxed) &&
      g_title_prompt_logged.load(std::memory_order_relaxed)) {
    LARECOMP_APP_INFO("ultrawide: the game is entered: the prompt bar stays in the middle from now on");
  }
}

void NoteUiPiece(uint32_t instance, bool begin) {
#if defined(LARECOMP_HAS_GPU)
  if (!begin) {
    // The end is sent even if the picture has stopped being wide, or the setting is 0, in between (a
    // window resized or a setting changed inside the draw): the renderer's state closes.
    if (t_open_piece != 0 && t_open_piece == instance) {
      t_open_piece = 0;
      mcla::gpu::OnGuestSceneComposited(kUiPieceEnd);
    }
    return;
  }
  // The hot path: this runs for every instance the Flash player draws (the menus have hundreds).
  if (t_open_piece != 0 || !Active() || !PlumeRenderer()) {
    return;
  }
  const uint8_t* base = GuestBase();
  if (!base) {
    return;
  }
  if (!g_game_entered.load(std::memory_order_relaxed)) {
    if (IsTitlePrompt(base, instance)) {
      if (!g_title_prompt_logged.exchange(true, std::memory_order_relaxed)) {
        LARECOMP_APP_INFO("ultrawide: the title's prompt bar (PROMPTMOVIE) is at the {} edge until the game is entered",
                          Tall() ? "bottom" : "right");
      }
      t_open_piece = instance;
      mcla::gpu::OnGuestSceneComposited(Tall() ? kUiEdgeBottom : kUiEdgeRight);
      return;
    }
  }
  if (g_hud_width.load(std::memory_order_relaxed) == 0) {
    return;
  }
  const Piece* piece = PieceOf(base, instance);
  if (!piece) {
    return;
  }
  t_open_piece = instance;
  if (Tall()) {
    mcla::gpu::OnGuestSceneComposited(piece->vert == Vert::kTop ? kUiPieceTop : kUiPieceBottom);
  } else {
    mcla::gpu::OnGuestSceneComposited(piece->side == Side::kLeft ? kUiPieceLeft : kUiPieceRight);
  }
#else
  (void)instance;
  (void)begin;
#endif
}

// From InitHooks, once, before any guest code: the setting is read, and a change of it (the F4
// panel, the launcher's config) is followed while the game runs.
void Init() {
  {
    std::lock_guard<std::mutex> lock(g_ask_mutex);
    g_ask = larecomp::settings::aspect_ratio.Get();
    g_ask_known = true;
  }
  g_hud_width.store(std::clamp<int32_t>(larecomp::settings::hud_width.Get(), 0, 100), std::memory_order_relaxed);
  larecomp::cvar::RegisterChangeCallback("hud_width", [](std::string_view, std::string_view value) {
    int32_t percent = 0;
    const char* end = value.data() + value.size();
    if (std::from_chars(value.data(), end, percent).ec != std::errc()) {
      return;
    }
    percent = std::clamp<int32_t>(percent, 0, 100);
    if (g_hud_width.exchange(percent, std::memory_order_relaxed) != percent) {
      LARECOMP_APP_INFO("ultrawide: hud_width is {}%{}", percent,
                        Active() ? "" : " (the picture is 16:9: nothing moves)");
    }
  });
  larecomp::cvar::RegisterChangeCallback("aspect_ratio", [](std::string_view, std::string_view value) {
    {
      std::lock_guard<std::mutex> lock(g_ask_mutex);
      g_ask = std::string(value);
    }
    Recompute(value);
  });
  // The aspect ratio the window's size (SetupPresentation) and the setting made, to the guest.
  Recompute(Ask());
  ApplyAspectRatioPatch();
}

}  // namespace mc::ultrawide

// --- guest hooks (larecomp_config.toml, [[midasm_hook]]) -------------------------------------------

// sub_82289530 (the floating gamertag over a car, a Flash clip of the HUD movie): after
// `fmuls f7,f9,f13` (0x82289808) f7 is the clip's x on the 1280 stage, (ndc_x + 1) * 0.5 * 1280 from
// the player camera's projected position; the next instruction stores it as the movie's
// "gamertag_x". The y needs nothing: the UI squeeze is horizontal.
// CHECK: the stage is the game's 1280-wide screen with x from the left edge, so that the clip is
// where the car is on the widened picture with the 3D view at 2560x1080 and 3840x1080 (a name tag
// over a car at the edge of the picture, online or in a LAN lobby race).
void Patch_UiMarkerX_82289808(PPCRegister& f7) {
  if (mc::ultrawide::Wide()) {
    f7.f64 = double(mc::ultrawide::MarkerStageX(float(f7.f64)));
  }
}

// The same tag's y, in a TALL picture: sub_82289530 stores the clip's y as `fmuls f8,f10,f12` at 0x82289810
// (f10 = 1 - (ndc_y + 1) * 0.5, f12 = 720), the next instruction stores it as "gamertag_y". The UI is squeezed
// vertically there, so the tag is given the y the squeeze maps back onto the car (MarkerStageY).
// CHECK: a name tag over a car at the top or the bottom of a 4:3 or 16:10 picture (online or LAN only).
void Patch_UiMarkerY_82289810(PPCRegister& f8) {
  if (mc::ultrawide::Tall()) {
    f8.f64 = double(mc::ultrawide::MarkerStageY(float(f8.f64)));
  }
}

// sub_82236558 (draws the world icons queued this frame: for each of the entries of 96 bytes it
// draws a 3D movie with the camera's view and projection, in the UI pass after the scene was
// composed): the hook at its first instruction (0x82236558) and the one before its last
// instruction (0x822365B0, the jump to the register restore), the only way out.
// CHECK: that the racer, checkpoint, place and other world icons of a race all come through this
// list; an icon drawn on the render thread straight away (the same icon functions, 0x8223B5D8 and
// its neighbours, draw at once when the thread flag at [r13]+72 is set) is not inside the bracket.
void Patch_UiWorldIcons_Begin() { mc::ultrawide::NoteWorldIcons(true); }
void Patch_UiWorldIcons_End() { mc::ultrawide::NoteWorldIcons(false); }

// sub_825FB348 (the Flash player's draw of one instance and, recursively, its children, ultrawide.h "the HUD's
// corner pieces"): the entry hook (r3 = the instance) and the one at the exit, 0x825FB698, the label every path
// of the function ends at (the instance is `mr r30,r3` all through, and the exit's `addi r1,r1,144` is the
// first instruction of the epilogue). The hot path is the first line of NoteUiPiece: it runs for every instance
// the player draws, a few hundred a frame in the menus.
// CHECK: that r30 still holds the instance at 0x825FB698 on every path (the code reads it so), and that
// midasm hooks at a function's entry and at an epilogue label work as the world icons' do (0x82236558, 0x822365B0).
void Patch_UiPiece_Begin(PPCRegister& r3) { mc::ultrawide::NoteUiPiece(r3.u32, true); }
void Patch_UiPiece_End(PPCRegister& r30) { mc::ultrawide::NoteUiPiece(r30.u32, false); }

// sub_826206B0 (the nav map: the ground the map camera sees, as a box in world x and z for the cursor
// and the map's edges): 0x82620700 is `lfs f27,-1336(r11)`, the load of the map's 16:9 aspect ratio
// (0x8201FAC8) on the widescreen branch; the 4:3 default at 0x826206E0 stays. f27 is the only place
// the function uses the aspect ratio: half width = tan(f27 * 12.5 degrees) * distance * 0.85.
// CHECK: that the nav map's cursor reaches the left and right edges of a 2560x1080 and a 3840x1080
// picture, that the icons out there are placed (mouse and stick), and that nothing moves at 16:9.
bool Patch_MapVisibleAspect_82620700(PPCRegister& f27) {
  if (!mc::ultrawide::Active()) {
    return false;
  }
  f27.f64 = double(float(mc::ultrawide::MapVisibleAspect()));
  return true;
}

// The same function's half vertical field of view: 0x82620708 is `lfs f31,-21408(r11)`, 12.5 degrees in radians
// (0.218166), which the box's depth is worked from (tan(tilt +- f31) times the distance, and f27 * f31 for its
// width). In a TALL picture the camera's half angle grows (Vert+), so this does too, and the hook above gave
// f27 the value that keeps the width the game's; the cursor's box then covers the taller picture. After the
// load, so f31 is the one the function uses from here on. A no-op unless the picture is tall.
// CHECK: that the nav map's cursor reaches the top and bottom edges of a 1920x1200 and a 1440x1080 picture
// (stick), that the icons there are placed, and that nothing moves at 16:9 or in an ultrawide picture.
void Patch_MapVisibleHalfFov_82620708(PPCRegister& f31) {
  if (mc::ultrawide::Tall()) {
    f31.f64 = double(float(mc::ultrawide::MapVisibleHalfFov()));
  }
}

// sub_822C0320 (the scene camera update): 0x822C0918 is `lfs f0,-28432(r8)`, the load of 0x820390F0
// (1.77778) on the branch that sets an ORTHOGRAPHIC projection (sub_8217CC80: left/right = -/+ half
// height * 1.77778, bottom/top = -/+ half height, half height = the variable's value * 0.5) in place
// of sub_822E5E10's perspective one. The branch is taken when the developer variable "orthoheight"
// (the object at 0x828747E4, named by the string at 0x8203916C) is not -1, its default, so it is a
// debug view and not reached in play. The wider half width keeps the vertical size and shows more at
// the sides, as the perspective cameras do; the next instruction (0x822C091C) is not touched.
// This is the only load of 0x820390F0 in the generated code. CHECK: that it is an aspect ratio (the
// vertical extent is not rescaled anywhere else) and that nothing but a console command or a mod can
// set "orthoheight", so this cannot be looked at in a normal run.
bool Patch_AspectRatio_822C0918(PPCRegister& f0) {
  if (!mc::ultrawide::Wide()) {
    return false;  // (a debug view: a tall picture keeps the game's width, the height is the variable's)
  }
  f0.f64 = double(float(mc::ultrawide::DisplayAspect()));
  return true;
}

// sub_822C0320 (the scene camera update), the branch that takes the pose and the field of view from
// the cutscene director's camera (the engine's own cutscenes; 0x822C0474..0x822C0510, taken when the
// in-game override flag r19 is 0, the director object at 0x8286D8E8 exists and sub_823CDC90, "is
// the camera active", says yes). The vertical field of view it hands on (80(r1): the perspective of
// sub_822E5E10 after Patch_FOVScale) is worked out there as
//   f12 = (mcCineCamera + 104, radians) * 57.2958 / sub_82171600(the player's viewport)   [0x822C04E8, fdivs f12,f28,f1]
// where sub_82171600 returns the aspect ratio the last Perspective call stored in the viewport
// (+700; sub_822E5E10 makes that call with the aspect ratio the four camera hooks above widen). The
// authored angle is taken to be horizontal, as sub_8221BE98 does for the map's camera flight: its
// division by the aspect ratio makes the vertical angle. With the widened aspect ratio in f1 the
// vertical angle shrinks by 16:9 over the aspect ratio and a cutscene is zoomed into the middle of
// the wide picture. The hook runs before the division and gives it 16:9 whenever the picture is
// not 16:9 (wider, or taller: there the Vert+ rewrite of the camera builder follows), so the vertical
// angle is the one the game has at 16:9, and the wide picture (its Perspective
// aspect ratio is still the widened one) shows more at the sides, as the gameplay cameras do. f1 is
// the return value of the call before: nothing reads it after the division (the next uses are
// loads or the result of a later call). Only this branch divides by it: the gameplay cameras take
// their vertical angle from the camera rig (sub_822AFA30, no aspect ratio) and are not touched.
// CHECK: that f1 here is the viewport's aspect ratio and that the cine camera's value at +104 is a
// horizontal angle in radians: a cutscene (`--tour cutscene`) at 2560x1080 and 3840x1080 must
// show the vertical framing of the 1920x1080 one with more at the sides. The 13 other callers of
// sub_82171600 (return addresses 0x82172308, 0x82172618, 0x82172858, 0x8217CB84, 0x82206F84,
// 0x8223FB50, 0x822FB5E4, 0x82306D10, 0x82320258, 0x8249B688, 0x825F5038, 0x82725E0C, 0x82725F08)
// were not read for an effect on the picture: 0x82320258 (sub_8231FE30, `fdivs f11,f30,f1`, a
// frustum) and 0x825F5038 (`fmuls f9,f1,f31`) use it as a factor. That a cutscene is never
// split-screen: the hook gives every viewport 16:9.
void Patch_CutsceneFovAspect_822C04E8(PPCRegister& f1) {
  if (mc::ultrawide::Active()) {
    f1.f64 = double(float(mc::ultrawide::kBaseAspect));
  }
}
#endif  // REXGLUE_HAS_XEO3_TARGET
