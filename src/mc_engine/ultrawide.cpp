// Ultrawide: the aspect ratio the game is shown at. See ultrawide.h.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include "ultrawide.h"

#include <rex/cvar.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <string_view>

#include "hooks/hooks.h"
#include "hooks/hooks_internal.h"
#include "larecomp_log.h"
#if defined(LARECOMP_HAS_GPU)
#include "gpu/gpu.h"
#endif

REXCVAR_DECLARE(std::string, aspect_ratio);  // hooks/graphics.cpp

namespace mc::ultrawide {

namespace {

// The window's size, width << 32 | height; 0 until the app says.
std::atomic<uint64_t> g_window{0};
// The aspect ratio in force, between kBaseAspect and kMaxAspect.
std::atomic<double> g_aspect{kBaseAspect};

// The setting's text, as the last change left it (a change callback is given the new value; reading
// the cvar inside the callback is not safe, it may run with the registry locked).
std::mutex g_ask_mutex;
std::string g_ask;
bool g_ask_known = false;  // Init ran

// The SDK presenter (the Xenia renderer) fits the 1280x720 picture into the window with black bars
// unless present_letterbox is off, when it stretches the picture over the whole window: what a
// widened picture needs. The value given to it last (-1: none yet, so the first window size sets it
// whatever an old config file says); the Plume renderer has its own present.
int g_xenia_stretch = -1;
// The aspect ratio the log last printed: a window dragged to a new size changes it at every step.
double g_logged_aspect = 0.0;

// The fixed choices of the setting. 16:10 is below the game's own shape: the window then keeps the
// 16:9 picture with black bars above and below, as 16:9 does.
double FixedAspect(std::string_view ask) {
  if (ask == "21:9") return 7.0 / 3.0;
  if (ask == "32:9") return kMaxAspect;
  return kBaseAspect;
}

double Choose(std::string_view ask, uint64_t window) {
  const uint32_t width = uint32_t(window >> 32);
  const uint32_t height = uint32_t(window & 0xFFFFFFFFu);
  double aspect = kBaseAspect;
  if (ask == "auto" || ask.empty()) {
    if (width != 0 && height != 0) {
      aspect = double(width) / double(height);
    }
  } else {
    aspect = FixedAspect(ask);
  }
  aspect = std::clamp(aspect, kBaseAspect, kMaxAspect);
  // A window a few pixels off 16:9 (a frame, a scaling factor's rounding) is 16:9: nothing to
  // widen, and the picture keeps its place.
  if (aspect < kBaseAspect * 1.005) {
    aspect = kBaseAspect;
  }
  return aspect;
}

// The setting or the window changed. Never called with the cvar registry locked except from the
// setting's own change callback, which takes `ask` from the callback's argument.
void Recompute(std::string_view ask) {
  const uint64_t window = g_window.load(std::memory_order_acquire);
  const double aspect = Choose(ask, window);
  const double before = g_aspect.exchange(aspect, std::memory_order_acq_rel);
  if (std::abs(aspect - before) > 1e-4) {
    if (std::abs(aspect - g_logged_aspect) > 0.02 || (aspect == kBaseAspect) != (g_logged_aspect == kBaseAspect)) {
      g_logged_aspect = aspect;
      LARECOMP_APP_INFO("ultrawide: the aspect ratio is {:.4f}:1 (setting \"{}\", window {}x{}){}",
                        aspect, ask, uint32_t(window >> 32), uint32_t(window & 0xFFFFFFFFu),
                        aspect > kBaseAspect * 1.001 ? "" : ", the game's own 16:9");
    }
    ApplyAspectRatioPatch();
  }
}

// The window's size arrives (SetupPresentation) before Init does: the cvar has its value by then.
std::string Ask() {
  {
    std::lock_guard<std::mutex> lock(g_ask_mutex);
    if (g_ask_known) {
      return g_ask;
    }
  }
  return REXCVAR_GET(aspect_ratio);
}

bool PlumeRenderer() {
#if defined(LARECOMP_HAS_GPU)
  return mcla::gpu::Enabled();
#else
  return false;
#endif
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
      rex::cvar::SetFlagByName("present_letterbox", stretch ? "false" : "true");
    }
  }
}

double DisplayAspect() { return g_aspect.load(std::memory_order_acquire); }

double UiWidthFraction() {
  const double aspect = DisplayAspect();
  return aspect > kBaseAspect * 1.001 ? kBaseAspect / aspect : 1.0;
}

float MarkerStageX(float stage_x) {
  if (!Active()) {
    return stage_x;
  }
  // The UI is squeezed about the middle of the stage by k (gpu/scene.cpp, UiSqueezeOf): a clip at
  // x lands at 640 + (x - 640) * k. The game worked x out from the widened camera
  // (x = 640 + 640 * ndc_x), which is where the 3D point is once the picture is stretched, so the
  // clip is placed at the x the squeeze maps back to it.
  const float k = float(UiWidthFraction());
  return kStageHalfWidth + (stage_x - kStageHalfWidth) / k;
}

double MapVisibleAspect() {
  // The map's own 16:9 constant (0x8201FAC8) and the half of the 25 degree vertical field of view it
  // goes with (0.218166 radians, 0x8208AC60: sub_826206B0 reads it as the same half angle).
  // CHECK: both numbers against the constants in the guest image (1.7778 and 0.218166).
  constexpr double kMapBaseAspect = 1.7778;
  constexpr double kMapHalfFov = 0.218166;
  const double aspect = DisplayAspect();
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

// From InitHooks, once, before any guest code: the setting is read, and a change of it (the F4
// panel, the launcher's config) is followed while the game runs.
void Init() {
  {
    std::lock_guard<std::mutex> lock(g_ask_mutex);
    g_ask = REXCVAR_GET(aspect_ratio);
    g_ask_known = true;
  }
  rex::cvar::RegisterChangeCallback("aspect_ratio", [](std::string_view, std::string_view value) {
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
  if (mc::ultrawide::Active()) {
    f7.f64 = double(mc::ultrawide::MarkerStageX(float(f7.f64)));
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
  if (!mc::ultrawide::Active()) {
    return false;
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
// wider, so the vertical angle is the one the game has at 16:9, and the wide picture (its Perspective
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
