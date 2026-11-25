#pragma once
// ===========================================================================
// MCLA GPU renderer -- one shared low resolution rear view: the rules that need no renderer (MIRROR-VIEW)
// ===========================================================================
// perf/MIRROR-VIEW.md is the design. In one picture: the game's rear view mirror phase (0x1000000, hooks/rear_view.cpp) already draws the city and the racers behind the player from the MAIN camera's
// eye, turned half a circle (sub_82307F28 negates the camera's right and forward rows). Instead of stamping that picture into a 229x44 rectangle of the main target, the host sends its draws
// into a small target of its own, and two kinds of reader use the finished image:
//   - the HUD mirror: one quad into the rectangle, as the stamp was (the same picture, a clean place for it in the frame);
//   - the car's two side mirrors on cards that cannot ray trace (MIRROR-RT is for the ones that can): the glass draws again with a pixel shader that finds, for each pixel of the glass, the
//     ray the glass reflects to the eye and reads the picture where that ray goes (UvForMirrorPixel).
// This header is the arithmetic of the second kind and of the target's size, as pure functions on plain numbers, so the design's error tables and sizes come from code with a test that needs
// no game (mirror_view_rules_test.cpp). Nothing here touches the renderer.
//
// THE RIG. The shared view's camera sits at the eye E of the main camera (a cockpit camera: the driver's head; a chase camera: behind the car), looks backwards and keeps the world's up. In car
// coordinates (x forward, y left, z up) its forward is -x, its up +z and its right +y (a camera that looks backwards has the car's left on its right: the lookup by direction below needs no flip,
// a mirror image is a lookup by the reflected direction).
// THE PARALLAX. The glass is a small way ahead of and beside the eye (about 0.8 to 1.4 m), so the ray a mirror pixel sends back leaves from the glass, not from the eye. A rig at the eye sees a
// point X = P + t r (P on the glass, r the reflected direction, t the distance beyond the glass) along X - E, not along r. UvForMirrorPixel looks up the direction (P - E_view) + t0 r: exact for
// things t0 metres away (kAssumedDistance), off by ParallaxError for the rest. A later step can read t from the view's own depth instead of assuming it (the design's package MV5).
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "mirror_draws.h"
#include "mirror_view_link.h"

namespace mcla::gpu::mirror_view {

// ---------------------------------------------------------------------------
// vectors
// ---------------------------------------------------------------------------

struct V3 {
  double x = 0.0, y = 0.0, z = 0.0;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline double Length(V3 a) { return std::sqrt(Dot(a, a)); }
inline V3 Normalize(V3 a) {
  const double l = Length(a);
  return l > 1e-12 ? a * (1.0 / l) : V3{};
}

// The ray a flat mirror sends back: `d` the unit ray from the eye to the glass, `n` the glass's unit normal (facing the eye).
inline V3 Reflect(V3 d, V3 n) { return d - n * (2.0 * Dot(d, n)); }

// The eye mirrored in the glass's plane: where a camera would stand to see through the glass what the mirror shows.
inline V3 MirroredEye(V3 eye, V3 glass_point, V3 n) { return eye - n * (2.0 * Dot(eye - glass_point, n)); }

// ---------------------------------------------------------------------------
// the rig
// ---------------------------------------------------------------------------

struct Rig {
  V3 eye;                                     // the view's origin (the main camera's eye when the view was drawn)
  V3 forward{-1.0, 0.0, 0.0};                 // looks backwards
  V3 up{0.0, 0.0, 1.0};
  V3 right{0.0, 1.0, 0.0};                    // forward x up, the car's left
  double tan_half_h = 1.0;                    // tan of half the horizontal field of view
  double tan_half_v = 0.5;                    // tan of half the vertical field of view
};

// A direction (from the rig's eye) as the picture's coordinates: u 0 at the left edge to 1 at the right, v 0 at the top to 1 at the bottom. False when it points behind the camera.
struct Uv {
  double u = 0.0, v = 0.0;
  bool ok = false;
};
inline Uv ProjectDirection(const Rig& rig, V3 dir) {
  Uv out;
  const double f = Dot(dir, rig.forward);
  if (!(f > 1e-9)) {
    return out;
  }
  const double x = Dot(dir, rig.right) / f / rig.tan_half_h;  // -1 .. 1
  const double y = Dot(dir, rig.up) / f / rig.tan_half_v;
  out.u = 0.5 + 0.5 * x;
  out.v = 0.5 - 0.5 * y;
  out.ok = true;
  return out;
}

// Whether the coordinates are inside the picture (a lookup outside it is clamped by the sampler: the edge's colour).
inline bool Inside(const Uv& uv) { return uv.ok && uv.u >= 0.0 && uv.u <= 1.0 && uv.v >= 0.0 && uv.v <= 1.0; }

inline constexpr double kAssumedDistance = 20.0;  // metres beyond the glass at which the lookup is exact (CHECK: the look, MIRROR-VIEW.md section 5)

// The picture's coordinates for one pixel of a side mirror: `eye` the eye now, `glass` the point of the glass the pixel is on, `n` its unit normal (facing the eye). The reflected ray leaves the
// glass; the rig is at rig.eye, which is the eye of the frame the view was drawn in (equal to `eye` for a view drawn this frame, a little behind it for a held one).
inline Uv UvForMirrorPixel(const Rig& rig, V3 eye, V3 glass, V3 n, double distance = kAssumedDistance) {
  const V3 d = Normalize(glass - eye);
  const V3 r = Reflect(d, n);
  return ProjectDirection(rig, (glass - rig.eye) + r * distance);
}

// The angle (radians) between where the lookup goes for a thing `t` metres beyond the glass and where the rig really sees that thing: the picture's error for it.
inline double ParallaxError(const Rig& rig, V3 eye, V3 glass, V3 n, double t, double distance = kAssumedDistance) {
  const V3 d = Normalize(glass - eye);
  const V3 r = Reflect(d, n);
  const V3 sampled = Normalize((glass - rig.eye) + r * distance);
  const V3 seen = Normalize((glass - rig.eye) + r * t);
  return std::acos(std::clamp(Dot(sampled, seen), -1.0, 1.0));
}

// ---------------------------------------------------------------------------
// what the view has to cover
// ---------------------------------------------------------------------------

// The angular extent, in the rig, of a set of directions: the half angles (radians) from the rig's forward, horizontal and vertical, that hold all of them with the view centred on forward.
struct Extent {
  double half_h = 0.0;
  double half_v = 0.0;
  bool ok = false;
};
inline Extent ExtentOf(const Rig& rig, const V3* dirs, int count) {
  Extent e;
  for (int i = 0; i < count; ++i) {
    const double f = Dot(dirs[i], rig.forward);
    if (!(f > 1e-9)) {
      return Extent{};
    }
    e.half_h = std::max(e.half_h, std::atan(std::fabs(Dot(dirs[i], rig.right)) / f));
    e.half_v = std::max(e.half_v, std::atan(std::fabs(Dot(dirs[i], rig.up)) / f));
  }
  e.ok = count > 0;
  return e;
}

// ---------------------------------------------------------------------------
// the view's size
// ---------------------------------------------------------------------------

enum class Class : uint8_t { kLow, kMid, kHigh };

struct Plan {
  uint32_t width = 0;
  uint32_t height = 0;
  double hfov_deg = 0.0;
  double vfov_deg = 0.0;
  uint32_t every = 1;  // drawn every Nth frame (the others hold the picture)
  bool valid() const { return width != 0 && height != 0; }
};

inline constexpr double kMinHorizontalFov = 40.0;   // degrees
inline constexpr double kMaxHorizontalFov = 110.0;
inline constexpr double kMinVerticalFov = 20.0;
inline constexpr double kMaxVerticalFov = 45.0;

inline constexpr double PixelsPerDegree(Class c) { return c == Class::kHigh ? 10.0 : c == Class::kMid ? 7.0 : 5.0; }
inline constexpr uint32_t MaxWidth(Class c) { return c == Class::kHigh ? 768u : c == Class::kMid ? 512u : 384u; }
inline constexpr uint32_t Cadence(Class c) { return c == Class::kHigh ? 1u : 2u; }

inline uint32_t RoundUp16(double v) { return uint32_t(std::ceil(v / 16.0)) * 16u; }

// The vertical field (degrees) of a stamp of `aspect` (width over height) that shows `hfov_deg` across.
inline double StampVerticalFov(double hfov_deg, double aspect) {
  const double pi = 3.14159265358979323846;
  return 2.0 * std::atan(std::tan(hfov_deg * 0.5 * pi / 180.0) / aspect) * 180.0 / pi;
}

// The size of the shared view. `side_hfov_deg` and `side_vfov_deg` are what the side mirrors need, centred on straight back (ExtentOf; 0 when they are not asked for), `hud_hfov_deg` what the
// HUD mirror wants across its stamp of `hud_aspect` (229 by 44 units: 5.2; 0 when it is off): the view's field is the larger of the two, the side mirrors' with a margin on each side for the
// parallax of near things, within the limits above; the picture's pixels per degree and cap are the class's, and the height follows from the two fields so the pixels are square. Nothing
// asked for: no view (an invalid Plan).
inline Plan PlanView(Class c, double side_hfov_deg, double side_vfov_deg, double hud_hfov_deg, double hud_aspect = 229.0 / 44.0, double margin_deg = 5.0) {
  Plan p;
  if (!(side_hfov_deg > 0.0) && !(hud_hfov_deg > 0.0)) {
    return p;
  }
  const double side_h = side_hfov_deg > 0.0 ? side_hfov_deg + 2.0 * margin_deg : 0.0;
  const double side_v = side_vfov_deg > 0.0 ? side_vfov_deg + 2.0 * margin_deg : 0.0;
  const double hud_v = hud_hfov_deg > 0.0 && hud_aspect > 0.0 ? StampVerticalFov(std::min(hud_hfov_deg, kMaxHorizontalFov), hud_aspect) : 0.0;
  p.hfov_deg = std::clamp(std::max(side_h, hud_hfov_deg), kMinHorizontalFov, kMaxHorizontalFov);
  p.vfov_deg = std::clamp(std::max({side_v, hud_v, kMinVerticalFov}), kMinVerticalFov, kMaxVerticalFov);
  const double w = std::min(double(MaxWidth(c)), PixelsPerDegree(c) * p.hfov_deg);
  p.width = RoundUp16(w);
  const double tan_h = std::tan(p.hfov_deg * 0.5 * 3.14159265358979323846 / 180.0);
  const double tan_v = std::tan(p.vfov_deg * 0.5 * 3.14159265358979323846 / 180.0);
  p.height = RoundUp16(double(p.width) * tan_v / tan_h);
  p.every = Cadence(c);
  return p;
}

// The memory the view holds: the multisampled colour and depth it is drawn into, and the single sample colour the readers sample (and, for the depth aware lookup of package MV5, a single sample
// depth). Bytes a pixel are the host's formats' (CHECK: the main colour's host format).
inline uint64_t HeldBytes(const Plan& p, uint32_t samples, uint32_t colour_bytes, uint32_t depth_bytes, bool resolved_depth) {
  if (!p.valid()) {
    return 0;
  }
  const uint64_t px = uint64_t(p.width) * p.height;
  uint64_t total = px * samples * (colour_bytes + depth_bytes);
  if (samples > 1) {
    total += px * colour_bytes;  // the resolved colour
  }
  if (resolved_depth) {
    total += px * 4;
  }
  return total;
}

// ---------------------------------------------------------------------------
// the redirect: a draw of the phase, in the rectangle's pixels, becomes a draw into the whole view
// ---------------------------------------------------------------------------

struct Rect {
  double x = 0.0, y = 0.0, w = 0.0, h = 0.0;
};

// A viewport (or scissor) in the guest's pixels of the main target, as a viewport of the view's own pixels: the rectangle maps to the whole view, a viewport inside it keeps its place relative to
// it, one that reaches past it is clamped to the view (a draw of the phase with a window of its own cannot leave the view).
inline Rect RemapToView(const Rect& mirror, const Rect& in, uint32_t view_w, uint32_t view_h) {
  Rect out;
  if (!(mirror.w > 0.0) || !(mirror.h > 0.0) || view_w == 0 || view_h == 0) {
    return out;
  }
  const double sx = double(view_w) / mirror.w;
  const double sy = double(view_h) / mirror.h;
  double x0 = (in.x - mirror.x) * sx;
  double y0 = (in.y - mirror.y) * sy;
  double x1 = (in.x + in.w - mirror.x) * sx;
  double y1 = (in.y + in.h - mirror.y) * sy;
  x0 = std::clamp(x0, 0.0, double(view_w));
  x1 = std::clamp(x1, 0.0, double(view_w));
  y0 = std::clamp(y0, 0.0, double(view_h));
  y1 = std::clamp(y1, 0.0, double(view_h));
  out.x = x0;
  out.y = y0;
  out.w = std::max(0.0, x1 - x0);
  out.h = std::max(0.0, y1 - y0);
  return out;
}

// The part of the shared view (u0, v0, u1, v1) the HUD stamp shows when the view's field is wider than the stamp wants: the central crop that has the wanted horizontal field and the stamp's
// aspect. The crop is never larger than the view.
struct Crop {
  double u0 = 0.0, v0 = 0.0, u1 = 1.0, v1 = 1.0;
};
inline Crop HudCrop(const Plan& view, double hud_hfov_deg, double stamp_aspect) {
  Crop c;
  if (!view.valid() || !(hud_hfov_deg > 0.0) || !(stamp_aspect > 0.0)) {
    return c;
  }
  const double pi = 3.14159265358979323846;
  const double tan_view_h = std::tan(view.hfov_deg * 0.5 * pi / 180.0);
  const double tan_view_v = std::tan(view.vfov_deg * 0.5 * pi / 180.0);
  const double tan_hud_h = std::tan(std::min(hud_hfov_deg, view.hfov_deg) * 0.5 * pi / 180.0);
  const double tan_hud_v = tan_hud_h / stamp_aspect;
  const double wu = std::min(1.0, tan_hud_h / tan_view_h);
  const double hv = std::min(1.0, tan_hud_v / tan_view_v);
  c.u0 = 0.5 - 0.5 * wu;
  c.u1 = 0.5 + 0.5 * wu;
  c.v0 = 0.5 - 0.5 * hv;
  c.v1 = 0.5 + 0.5 * hv;
  return c;
}

// ===========================================================================
// MV1: the HOST VIEW (perf/MIRROR-VIEW.md sections 2.1, 2.2, 2.5 and 9): what the renderer decides about the draws between the guest's two markers, the view's size for the HUD's rectangle, how long
// the view lives and the maps of a draw's viewport and scissor into it. All pure: mirror_view.h and scene.cpp only ask these functions and do what they answer.
// ===========================================================================

// ---------------------------------------------------------------------------
// the markers and the stamp flag: the interface between the guest's hooks and the host (mirror_view_link.h, the one header both sides use)
// ---------------------------------------------------------------------------
// kMarkerBegin / kMarkerEnd, IsMarker, SetStampWanted / StampWanted are in mirror_view_link.h, included above. The rectangle is NOT in a marker: the guest publishes it through mirror_draws::Publish
// before the phase runs (TickRearView), also for a phase that no HUD mirror asked for.

// ---------------------------------------------------------------------------
// the host's per frame request to the guest (MirrorView::BeginFrame publishes it, once a frame: the guest half reads it, hooks/rear_view.cpp)
// ---------------------------------------------------------------------------

struct RequestFacts {
  bool enabled = false;    // mcla_gpu_mirror_view is on (MirrorView::enabled)
  bool blocked = false;    // the shaders failed or the images could not be made in the last attempts: the same word as BeginFacts::blocked
  bool side_need = false;  // MV3's flag: the glass pair found and wide enough on the picture (mirror_glass::SideNeed())
};

// What the host says to the guest this frame. `redirect` is the host's promise that a phase the guest marks goes into the view: without it the guest runs no side-only phase and keeps the game's own
// camera-step clear (a phase nobody catches would draw an unframed mirror into the main picture). The side mirrors' need counts only with it (the guest ignores it otherwise too). The field and the
// cadence are left at "none asked for" until the glass overlay (MV4) and the class table (MV6) want them: the game's own field stays, the view is redrawn every frame.
inline Request RequestFor(const RequestFacts& f) {
  Request r;
  r.redirect = f.enabled && !f.blocked;
  r.side_need = r.redirect && f.side_need;
  return r;
}

// ---------------------------------------------------------------------------
// the phase: what the draws between the markers do
// ---------------------------------------------------------------------------

enum class PhaseState : uint8_t {
  kIdle,         // no phase open: every draw is the main target's
  kRedirect,     // the phase is open and the view takes its draws and its begin clear
  kPassThrough,  // the phase is open and the switch is on, but the view cannot run (no rectangle, no plan, the images failed): the game's own mirror, as without the switch
};

struct BeginFacts {
  bool enabled = false;     // mcla_gpu_mirror_view is on
  bool rect_valid = false;  // the guest published the rectangle (mirror_draws::Current().valid())
  bool plan_valid = false;  // a size for the view is there (PlanForRect)
  bool blocked = false;     // the images could not be made in the last attempts (video memory): wait for the next change before asking again
};

// The phase of the frame in progress. Begin and End come from the markers, FrameStart from Scene::BeginFrame (a phase never lasts past the frame: a lost end closes there, and is counted).
struct PhaseTracker {
  PhaseState state = PhaseState::kIdle;
  uint32_t phases = 0;      // begins seen
  uint32_t redirected = 0;  // begins that redirect
  uint32_t completed = 0;   // redirected phases whose end came
  uint32_t unbalanced = 0;  // a begin inside a phase, an end outside one, a phase open at the frame's start

  PhaseState Begin(const BeginFacts& f) {
    if (state != PhaseState::kIdle) {
      ++unbalanced;  // the end of the last one was lost: this begin starts a new phase
    }
    ++phases;
    const bool redirect = f.enabled && f.rect_valid && f.plan_valid && !f.blocked;
    state = redirect ? PhaseState::kRedirect : PhaseState::kPassThrough;
    redirected += redirect ? 1u : 0u;
    return state;
  }
  // True when the end closed a redirected phase: the view's picture is whole now.
  bool End() {
    if (state == PhaseState::kIdle) {
      ++unbalanced;
      return false;
    }
    const bool whole = state == PhaseState::kRedirect;
    completed += whole ? 1u : 0u;
    state = PhaseState::kIdle;
    return whole;
  }
  void FrameStart() {
    if (state != PhaseState::kIdle) {
      ++unbalanced;
      state = PhaseState::kIdle;
    }
  }
  bool redirecting() const { return state == PhaseState::kRedirect; }
};

// ---------------------------------------------------------------------------
// the draw: where it goes
// ---------------------------------------------------------------------------

// What the renderer knows of a prepared draw that decides its route. The view's framebuffer has ONE colour and a depth, at the formats and samples the phase's first draw named (a pipeline can only be bound
// to a framebuffer of its own formats and samples).
struct DrawFacts {
  uint32_t color_count = 0;      // the pipeline key's colour targets
  bool has_depth = false;        // the key has a depth format
  bool planes_attached = false;  // the key carries the G-buffer's planes as colour targets after the game's one
  uint32_t color_format = 0;     // plume::RenderFormat of colour 0
  uint32_t depth_format = 0;     // plume::RenderFormat of the depth
  uint32_t samples = 1;          // the key's sample count
};
struct ViewFormats {
  bool assigned = false;  // a draw of this phase has named them
  uint32_t color_format = 0;
  uint32_t depth_format = 0;
  uint32_t samples = 1;
};
enum class Route : uint8_t {
  kMain,  // drawn into the main target as the game asked (no phase, or a pass-through one)
  kView,  // drawn into the view
  kDrop,  // in a redirected phase and not drawable into the view: left out, counted, never drawn into the main target
};
inline Route RouteDraw(PhaseState state, const DrawFacts& f, const ViewFormats& view) {
  if (state != PhaseState::kRedirect) {
    return Route::kMain;
  }
  if (f.color_count != 1 || !f.has_depth || f.planes_attached) {
    return Route::kDrop;
  }
  if (view.assigned && (view.color_format != f.color_format || view.depth_format != f.depth_format || view.samples != f.samples)) {
    return Route::kDrop;
  }
  return Route::kView;
}

// ---------------------------------------------------------------------------
// the view's size for the rectangle (the HUD mirror at the game's own field of view)
// ---------------------------------------------------------------------------

// The game's mirror camera takes its aspect from the rectangle (sub_82307F28: the ints at pass+8 and +12), and the draws' viewport is that rectangle, so the picture in the view has the rectangle's aspect:
// the width is the class's (MaxWidth: 384, 512, 768) and the height follows, to an even number. The field of view stays the game's (the owner kept it for the HUD mirror); package MV2 sets field and aspect
// for the side mirrors and PlanView's table takes over there. hfov_deg and vfov_deg stay 0: not known.
inline Plan PlanForRect(Class c, double rect_w, double rect_h) {
  Plan p;
  if (!(rect_w >= 1.0) || !(rect_h >= 1.0)) {
    return p;
  }
  p.width = MaxWidth(c);
  const double h = double(p.width) * rect_h / rect_w;
  p.height = std::max(2u, uint32_t(std::lround(h / 2.0)) * 2u);
  p.every = Cadence(c);
  return p;
}

// `auto` per class (MIRROR-VIEW.md 2.7): Low keeps the game's own mirrors until the numbers are in. The debug switch of MV1 forces the view on, whatever the class says.
inline constexpr bool AutoWanted(Class c) { return c != Class::kLow; }

// ---------------------------------------------------------------------------
// the view's life
// ---------------------------------------------------------------------------

inline constexpr int64_t kIdleDropMs = 3000;   // the images go after about 3 s without a phase asking for them
inline constexpr uint32_t kMaxHeldFrames = 8;  // a stamp is drawn from a picture up to this many frames old (the guest's every-Nth-frame cadence holds it 1 to 3)

struct Lifetime {
  bool held = false;  // the images exist
  bool ever_wanted = false;
  int64_t last_wanted_ms = 0;  // the last begin of a redirected phase
  void Wanted(int64_t now_ms) {
    ever_wanted = true;
    last_wanted_ms = now_ms;
  }
  bool ShouldDrop(int64_t now_ms) const { return held && (!ever_wanted || now_ms - last_wanted_ms > kIdleDropMs); }
};

// Whether a stamp is drawn at the end-of-scene copy of a frame: a picture is whole, it was finished `age_frames` frames ago (0: this frame) and a rectangle is up now.
inline constexpr bool StampDue(bool whole, uint32_t age_frames, bool rect_valid, bool stamp_wanted) {
  return whole && rect_valid && stamp_wanted && age_frames <= kMaxHeldFrames;
}

// ---------------------------------------------------------------------------
// the maps of a draw's viewport and scissor into the view
// ---------------------------------------------------------------------------

struct IRect {
  int32_t left = 0, top = 0, right = 0, bottom = 0;
  bool empty() const { return right <= left || bottom <= top; }
  bool operator==(const IRect&) const = default;
};

// The guest rectangle in the host pixels of the target the draws would have gone to (mirror_draws::HostEdge: the same edges MatchesViewport compares).
inline IRect HostRectOf(const mirror_draws::Rect& r, uint32_t twentieths) {
  IRect out;
  if (!r.valid() || twentieths == 0) {
    return out;
  }
  out.left = int32_t(mirror_draws::HostEdge(r.x, twentieths));
  out.top = int32_t(mirror_draws::HostEdge(r.y, twentieths));
  out.right = int32_t(mirror_draws::HostEdge(int64_t(r.x) + r.w, twentieths));
  out.bottom = int32_t(mirror_draws::HostEdge(int64_t(r.y) + r.h, twentieths));
  return out;
}

// A viewport in host pixels of the main target as a viewport of the view: the rectangle is the whole view, a viewport that is part of it or more than it keeps its place and size in proportion. NOT clamped:
// the NDC of the draw's vertices is taken against the viewport, so a viewport changed in shape moves the picture; the scissor is what cuts.
inline Rect MapViewport(const IRect& mirror, const Rect& in, uint32_t view_w, uint32_t view_h) {
  Rect out;
  if (mirror.empty() || view_w == 0 || view_h == 0) {
    return out;
  }
  const double mw = double(mirror.right - mirror.left);
  const double mh = double(mirror.bottom - mirror.top);
  out.x = (in.x - mirror.left) * double(view_w) / mw;  // the product first: whole edges stay whole (no 48.999... from a scale taken before)
  out.y = (in.y - mirror.top) * double(view_h) / mh;
  out.w = in.w * double(view_w) / mw;
  out.h = in.h * double(view_h) / mh;
  return out;
}

// A scissor in host pixels of the main target as the view's: the same map, the edges rounded outwards and cut to the view (a draw of the phase can never touch pixels outside it).
inline IRect MapScissor(const IRect& mirror, const IRect& in, uint32_t view_w, uint32_t view_h) {
  IRect out;
  if (mirror.empty() || view_w == 0 || view_h == 0) {
    return out;
  }
  const double mw = double(mirror.right - mirror.left);
  const double mh = double(mirror.bottom - mirror.top);
  const auto clamp_to = [](double v, uint32_t hi) { return int32_t(std::clamp(v, 0.0, double(hi))); };
  out.left = clamp_to(std::floor(double(in.left - mirror.left) * double(view_w) / mw), view_w);
  out.top = clamp_to(std::floor(double(in.top - mirror.top) * double(view_h) / mh), view_h);
  out.right = clamp_to(std::ceil(double(in.right - mirror.left) * double(view_w) / mw), view_w);
  out.bottom = clamp_to(std::ceil(double(in.bottom - mirror.top) * double(view_h) / mh), view_h);
  if (out.right < out.left) out.right = out.left;
  if (out.bottom < out.top) out.bottom = out.top;
  return out;
}

}  // namespace mcla::gpu::mirror_view
