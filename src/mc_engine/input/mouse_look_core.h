#pragma once
// The pure parts of mouse look and keyboard steering (KBM-DESIGN.md phase P5): no SDK, no window, no guest memory, so
// larecomp_mouse_look_test (mouse_look_test.cpp) can run them without the game. The runtime half is mouse_look.cpp (relative mouse mode,
// the window listener, the gate's facts), camera_look.cpp (what the look does to the camera) and keyboard_native.cpp (the steering ramp's feed).
//
//  DeltaSum      the mouse's raw motion: the UI thread adds every motion event's dx / dy, the game thread takes the sum with exchange(0).
//                Fixed point (1/1024 of a count), so fractions add up and the two axes are plain atomics.
//  Gate          whether relative mouse mode should be on: only while the keyboard and mouse are the active device, the window has focus, a car
//                is on the road (no menu, overlay, map or phone) and mouse look is wanted. It lets go at once on every hard reason (a controller
//                took over, focus lost, a menu or overlay opened, the setting turned off) and waits a moment before it takes hold or lets go on
//                a soft one (a HUD that skipped a beat), so that the pointer does not flap.
//  LookState     the view angles the mouse has built up, and how they come back behind the car after a short idle (the game's own lookaround:
//                LookaroundDelayTime, then LookaroundDecayRate, camera_look.h).
//  Steer ramp    full lock builds up over a short time while a steering key is held and returns to centre when it is let go, so keyboard
//                steering is not a 0 / 100 % switch.
//
// ---------------------------------------------------------------------------------------------------------------------------------------
// THE SENSITIVITY RULE (kLookRadiansPerCount, cam_freelook_sens)
//   The view turns `sens` radians for every RAW mouse count, the number the mouse itself reports (SDL's relative motion: unaccelerated, the
//   same on every Windows pointer-speed setting). The old look took the cursor's pixel movement, and at Windows' default pointer speed (6 of 11,
//   "enhance pointer precision" off) one count is one pixel; so the default stays 0.0035 rad per count (0.2 degrees) and the same movement of the
//   hand turns the view as far as before at default sensitivity, whatever the mouse's DPI is. The rows' values (0.0015 ... 0.01) keep their
//   meaning, so the launcher's list needs no change. What the default means on the desk, for a 360 degree turn (1795 counts):
//       400 DPI  11.4 cm      800 DPI  5.7 cm      1600 DPI  2.8 cm      3200 DPI  1.4 cm
//   and a quick glance to the side (90 degrees, 449 counts) is 1.4 cm at 800 DPI, 0.7 cm at 1600 DPI. A mouse with a different DPI is a
//   different feel by exactly its DPI ratio; the row is the answer (LOOK SENS: 0.0015 ... 0.01), not a guess at the hardware.
//   If Windows' pointer acceleration was on, the old look turned further for a fast flick than for a slow one; raw counts are the same for both.
// ---------------------------------------------------------------------------------------------------------------------------------------

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace larecomp::mouselook {

constexpr double kLookRadiansPerCount = 0.0035;

// Radians the view turns for `counts` raw counts at sensitivity `sens` (radians per count).
inline double RadiansFor(double counts, double sens) { return counts * sens; }

// ---------------------------------------------------------------------------------------------------------------------------------------
// The raw delta sum
// ---------------------------------------------------------------------------------------------------------------------------------------
class DeltaSum {
 public:
  static constexpr double kScale = 1024.0;

  // UI thread: one motion event's dx / dy (counts; fractional when the window's pixel density is not 1).
  void Add(float dx, float dy) {
    if (dx != 0.0f) x_.fetch_add(static_cast<int64_t>(std::llround(double(dx) * kScale)), std::memory_order_relaxed);
    if (dy != 0.0f) y_.fetch_add(static_cast<int64_t>(std::llround(double(dy) * kScale)), std::memory_order_relaxed);
  }
  // Game thread: everything added since the last Take.
  void Take(float& dx, float& dy) {
    dx = static_cast<float>(double(x_.exchange(0, std::memory_order_relaxed)) / kScale);
    dy = static_cast<float>(double(y_.exchange(0, std::memory_order_relaxed)) / kScale);
  }
  void Clear() {
    x_.store(0, std::memory_order_relaxed);
    y_.store(0, std::memory_order_relaxed);
  }

 private:
  std::atomic<int64_t> x_{0};
  std::atomic<int64_t> y_{0};
};

// ---------------------------------------------------------------------------------------------------------------------------------------
// The gate: should relative mouse mode be on?
// ---------------------------------------------------------------------------------------------------------------------------------------
struct Facts {
  bool wanted = false;          // mouse look is switched on (cam_freelook) and the camera is asking
  bool keyboard_mouse = false;  // the active device is the keyboard and mouse (input_state.h)
  bool focused = false;         // the game window has the focus
  bool driving = false;         // a car is on the road: the race HUD is updating and the game is not paused
  bool menu = false;            // a menu screen is up (the pause menu, the garage, a pop-up, the map's movie ...)
  bool phone = false;           // the Sidekick phone is up
  bool map = false;             // the full map has the pointer
  bool overlay = false;         // an overlay (F4, a browser) wants the mouse
  bool drag = false;            // off the road (a menu, the garage, the pause menu, the photo mode) with a mouse button held: the mouse is the right stick
};

enum class Why : uint8_t { On = 0, Off, Overlay, NoFocus, PadInUse, Menu, Map, Phone, NotDriving, Drag };

// The first reason that says no (hard reasons first, NotDriving last: it is the soft one).
// A drag off the road needs no look to be wanted and no car: only the hard reasons (an overlay, the focus, a controller, the map, the phone) stop it.
inline Why Decide(const Facts& f) {
  if (f.drag && f.focused && f.keyboard_mouse && !f.overlay && !f.map && !f.phone) return Why::Drag;
  if (!f.wanted) return Why::Off;
  if (f.overlay) return Why::Overlay;
  if (!f.focused) return Why::NoFocus;
  if (!f.keyboard_mouse) return Why::PadInUse;
  if (f.menu) return Why::Menu;
  if (f.map) return Why::Map;
  if (f.phone) return Why::Phone;
  if (!f.driving) return Why::NotDriving;
  return Why::On;
}

inline const char* WhyText(Why w) {
  switch (w) {
    case Why::On: return "the keyboard and mouse are in use and a car is on the road";
    case Why::Off: return "mouse look is off";
    case Why::Overlay: return "an overlay wants the mouse";
    case Why::NoFocus: return "the window lost the focus";
    case Why::PadInUse: return "a controller is in use";
    case Why::Menu: return "a menu is up";
    case Why::Map: return "the map has the pointer";
    case Why::Phone: return "the Sidekick phone is up";
    case Why::NotDriving: return "no car is being driven";
    case Why::Drag: return "a mouse button is held off the road: the mouse drags the right stick";
  }
  return "?";
}

class Gate {
 public:
  // How long everything must say yes before relative mode is taken, and how long a soft "no" (NotDriving) is ridden out.
  static constexpr int64_t kEngageDelayMs = 150;
  static constexpr int64_t kSoftReleaseMs = 500;

  // Once per frame with the facts and a monotonic clock in milliseconds. Returns whether relative mode should be on.
  bool Step(const Facts& f, int64_t now_ms) {
    why_ = Decide(f);
    if (why_ == Why::Drag) {
      // A drag starts with the button: no delay, or the first stretch of it would be lost.
      soft_since_ = -1;
      want_since_ = -1;
      on_ = true;
      return true;
    }
    if (why_ == Why::On) {
      soft_since_ = -1;
      if (!on_) {
        if (want_since_ < 0) want_since_ = now_ms;
        if (now_ms - want_since_ >= kEngageDelayMs) on_ = true;
      }
      return on_;
    }
    want_since_ = -1;
    if (why_ == Why::NotDriving && on_) {
      if (soft_since_ < 0) soft_since_ = now_ms;
      if (now_ms - soft_since_ < kSoftReleaseMs) return true;
    }
    soft_since_ = -1;
    on_ = false;
    return false;
  }
  Why why() const { return why_; }
  bool on() const { return on_; }

 private:
  bool on_ = false;
  int64_t want_since_ = -1;  // since when everything has said yes
  int64_t soft_since_ = -1;  // since when only the soft reason has said no
  Why why_ = Why::Off;
};

// ---------------------------------------------------------------------------------------------------------------------------------------
// The look angles
// ---------------------------------------------------------------------------------------------------------------------------------------
struct LookParams {
  float sens = float(kLookRadiansPerCount);  // radians per raw count
  bool invert_x = false;
  bool invert_y = false;
  float return_delay = 0.35f;  // seconds without motion before the view starts easing back (the game's LookaroundDelayTime)
  float return_rate = 4.0f;    // 1 / seconds: the fraction of the remaining angle that comes back per second (LookaroundDecayRate)
  float yaw_limit = 3.14159f;
  float pitch_limit = 0.7f;
};

struct LookState {
  float yaw = 0.0f;
  float pitch = 0.0f;
  float idle = 0.0f;  // seconds since the mouse last moved
};

// A yaw limit this close to pi means no limit at all: the view spins freely instead of stopping behind the car.
constexpr double kPiD = 3.14159265358979323846;
constexpr float kYawAllRound = 3.14f;

// Motion under this many counts in a frame is not "the player is looking around": a raw sensor does not jitter, so one count is real motion.
constexpr float kMoveCounts = 0.5f;

// One camera tick. `engaged` = relative mode is on and the mouse is steering. dx / dy are the raw counts since the last tick, dt the seconds.
inline void StepLook(LookState& s, bool engaged, float dx, float dy, float dt, const LookParams& p) {
  const float ease = 1.0f - std::exp(-p.return_rate * dt);  // frame-rate independent
  if (!engaged) {
    // Whatever took the mouse away (an overlay, lost focus, a controller, the setting) should not leave the view parked to one side.
    s.yaw -= s.yaw * ease;
    s.pitch -= s.pitch * ease;
    s.idle = 0.0f;
    return;
  }
  if (p.invert_x) dx = -dx;
  if (p.invert_y) dy = -dy;
  const bool moving = (std::fabs(dx) + std::fabs(dy)) >= kMoveCounts;
  if (moving) {
    s.idle = 0.0f;
    // Displacement, not a rate: the counts are already per tick, scaling them by dt would make one movement mean different amounts at different frame rates.
    s.yaw += float(RadiansFor(dx, p.sens));
    s.pitch += float(RadiansFor(dy, p.sens));
  } else {
    s.idle += dt;
    if (s.idle >= p.return_delay) {
      s.yaw -= s.yaw * ease;
      s.pitch -= s.pitch * ease;
      // Below a twentieth of a degree there is nothing left to see, and an exponential never quite arrives on its own.
      if (std::fabs(s.yaw) < 0.001f) s.yaw = 0.0f;
      if (std::fabs(s.pitch) < 0.001f) s.pitch = 0.0f;
    }
  }
  if (p.yaw_limit >= kYawAllRound) {
    // All the way round: keep turning, past the back and on; the angle stays in [-pi, pi] so the return eases back the short way.
    s.yaw = float(std::remainder(double(s.yaw), 2.0 * kPiD));
  } else {
    s.yaw = std::clamp(s.yaw, -p.yaw_limit, p.yaw_limit);
  }
  s.pitch = std::clamp(s.pitch, -p.pitch_limit, p.pitch_limit);
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// The mouse as the right stick, off the road
// ---------------------------------------------------------------------------------------------------------------------------------------
// The garage's turntable camera, the pause menu's way into the photo mode (the right stick pushed sideways: PHOTO_CAM_MOVE_LT / _RT) and the
// photo mode's own camera all read the RIGHT STICK, which a keyboard player does not have. While a mouse button is held there, the mouse's
// speed becomes a stick deflection: still = centred, `full_counts_s` raw counts per second = full lock, and anything that moves starts at
// `start` so that it clears the game's own dead zones. The garage's camera (sub_82251FD0) is the strictest reader measured: a 0.3 dead zone
// rescaled to 0..1 (sub_8260C5C8), and then anything at or under 0.1 of that counts as still -- so a stick under about 0.37 does nothing at
// all there. `start` = 0.55 lands a crawl at about 0.36 after that rescale.
// The mouse's events do not arrive in every frame (a 125 Hz mouse against a 144 Hz screen), so a frame without motion keeps the last target
// for `hold_s` before it counts as still; the value eases towards the target with time constant `smooth_s`.
struct DragParams {
  float full_counts_s = 900.0f;  // raw counts per second for full lock (about 3 cm/s at 800 DPI)
  float start = 0.55f;           // the deflection of the slightest motion
  float smooth_s = 0.04f;        // easing time constant
  float hold_s = 0.06f;          // a gap between mouse events this short is not a stop
  bool invert_x = false;
  bool invert_y = false;
};

struct DragStick {
  float x = 0.0f;   // -1 .. 1, positive is right
  float y = 0.0f;   // -1 .. 1, positive is UP (the pad's convention; the mouse's dy grows downwards)
  float tx = 0.0f;  // the last target, held across a short gap between events
  float ty = 0.0f;
  float idle = 0.0f;  // seconds since the last motion
};

inline float DragTarget(float counts, float dt, const DragParams& p) {
  if (!(dt > 0.0f) || counts == 0.0f) return 0.0f;
  const float speed = std::fabs(counts) / dt;
  const float m = std::min(1.0f, p.start + (1.0f - p.start) * speed / std::max(p.full_counts_s, 1.0f));
  return counts > 0.0f ? m : -m;
}

// One frame. `held` = the drag is on (a button is down, relative mode is on); off, the stick is centred at once, so a released button never
// leaves the camera turning.
inline void StepDrag(DragStick& s, bool held, float dx, float dy, float dt, const DragParams& p) {
  if (!held) {
    s = DragStick{};
    return;
  }
  dt = std::clamp(dt, 0.0f, 0.1f);
  if (p.invert_x) dx = -dx;
  if (p.invert_y) dy = -dy;
  if (dx != 0.0f || dy != 0.0f) {
    s.idle = 0.0f;
    s.tx = DragTarget(dx, dt, p);
    s.ty = DragTarget(-dy, dt, p);
  } else {
    s.idle += dt;
    if (s.idle > p.hold_s) {
      // A stop is a stop: centred at once, so the camera does not drift on after the hand has.
      s = DragStick{};
      s.idle = p.hold_s;
      return;
    }
  }
  const float a = p.smooth_s > 0.0f ? 1.0f - std::exp(-dt / p.smooth_s) : 1.0f;
  s.x += (s.tx - s.x) * a;
  s.y += (s.ty - s.y) * a;
  if (std::fabs(s.x) < 0.02f) s.x = 0.0f;
  if (std::fabs(s.y) < 0.02f) s.y = 0.0f;
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// Keyboard steering
// ---------------------------------------------------------------------------------------------------------------------------------------
struct SteerParams {
  float rise_s = 0.40f;    // seconds from centre to full lock while a steering key is held
  float return_s = 0.25f;  // seconds from full lock back to centre when it is let go (and while turning through centre to the other side)
};

// -1 left, 0 none or both (they cancel, the old key pair's rule), +1 right.
inline int SteerDirection(bool left, bool right) { return (right ? 1 : 0) - (left ? 1 : 0); }

// One step of the ramp: `value` (-1 full left .. +1 full right) moves towards `dir` at 1 / rise_s per second, towards the centre at 1 / return_s per second when the key
// is let go, and through the centre to the other side at the return rate until it crosses and then at the rise rate. A tap is a small correction, a held key sweeps to full lock.
// dt is clamped to 0.1 s so that a stall is not a jump.
inline float StepSteer(float value, int dir, float dt, const SteerParams& p) {
  dt = std::clamp(dt, 0.0f, 0.1f);
  const float rise = 1.0f / std::max(p.rise_s, 0.01f);
  const float back = 1.0f / std::max(p.return_s, 0.01f);
  if (dir == 0) {
    const float step = back * dt;
    return value > 0.0f ? std::max(value - step, 0.0f) : std::min(value + step, 0.0f);
  }
  const float d = float(dir);
  if (value * d < 0.0f) {  // on the other side of the centre: come back at the return rate first
    const float to_centre = std::fabs(value) / back;
    if (dt <= to_centre) return value + d * back * dt;
    return d * std::min(rise * (dt - to_centre), 1.0f);  // through the centre and on at the rise rate
  }
  const float next = value + d * rise * dt;
  return std::clamp(next, -1.0f, 1.0f);
}

// A stick axis as the game's pad state holds it (-32767 .. 32767; positive is right).
inline int16_t SteerToAxis(float v) {
  const long a = std::lround(double(std::clamp(v, -1.0f, 1.0f)) * 32767.0);
  return static_cast<int16_t>(a);
}

// The key steering and the pad's stick on the same axis: the larger magnitude wins (the game's own rule for two sources of one output).
inline int16_t MergeAxis(int16_t stick, int16_t key) {
  const int s = stick < 0 ? -int(stick) : int(stick);
  const int k = key < 0 ? -int(key) : int(key);
  return k > s ? key : stick;
}

}  // namespace larecomp::mouselook
