// The title screen's road reflection plane: the hooks and the calls into the game. The why and the rules are in title_road_plane.h.
//
// RE map (default.xex; the addresses were read in the recompiled sub_822F3918, which sets the road's reflection constants every frame):
//
//   sub_822F3918(this, t)   builds gReflectionParams (VS c43 / PS c43): the three vectors it hands sub_8218AEF8 are on its stack. The one at r1+80 is
//                           (this+14576 the wetness, this+14580 the reflection's strength, the plane's height, the constant 1.0).
//   sub_822A39C0(holder)    tail-calls sub_821E80F8(holder, 0): "player slot 0 has a view". holder = dword_82874374 (the player manager).
//   with a player view      the height is the y of the matrix sub_822A8108(sub_822A39C8(holder)) returns (+52): the player's ground.
//   without one (the front end: title, boot menus)
//                           loc_822F3AF4: `stfs f31,88(r1)` stores f31, which holds the float at 0x82001D14 = 1.0: a plane at sea level.
//                           loc_822F3AF8: `stfs f31,92(r1)` stores the same f31 as the vector's w.
//
//   0x822F3AF4  Hook_TitlePlaneZ  (f31)  reached only by the no-player path: f31 = the plane the camera's ground says, for the store at +88
//   0x822F3AF8  Hook_TitlePlaneW  (f31)  reached by both paths: puts f31 back when the hook above changed it (the w component stays the game's)
//
// The ground is probed with the game's own segment probe (sub_82574418 on the physics level dword_828DA464, the call map_warp.cpp's teleport snap and the
// Director's query.pick make), from the camera the front-end camera hook publishes (mc_engine/camera/final_camera.h): a few rays through the
// lower part of the picture every tenth of a second, the upward-facing hits' lower quartile (title_road_plane.h). It runs on the guest thread that
// calls sub_822F3918 (a guest thread, which the call into the game needs). Nothing is written to the guest's memory but a 160-byte scratch for the
// probe's segment and result, made once.
//
// Log, tag "[title plane]": the first plane found, and then one line when it moved by more than 3 m or the state changed, at most one every 5 s.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include "settings/immortal.h"
#include <rex/kernel/title_runtime.h>
#include <rex/ppc/context.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "hooks.h"
#include "hooks_internal.h"
#include "larecomp_guest.h"
#include "larecomp_log.h"
#include "mc_engine/camera/final_camera.h"
#include "title_road_plane.h"

namespace {

using Clock = std::chrono::steady_clock;
namespace tp = larecomp::title_plane;

constexpr uint32_t kLevelPtr = 0x828DA464;        // dword: the physics level
constexpr uint32_t kProbeFn = 0x82574418;         // the segment probe
constexpr uint32_t kGuestMallocFn = 0x82130528;   // sub_82130528(bytes)
constexpr uint32_t kScratchBytes = 160;           // segment 32 + result 96, and room for the 16-byte alignment

struct State {
  bool have_plane = false;       // a plane was found at least once
  float plane = 1.0f;            // the plane that is applied (smoothed)
  float target = 1.0f;           // the last probe's
  Clock::time_point probed_at;
  bool probed_once = false;
  Clock::time_point applied_at;
  bool applied_valid = false;
  uint32_t calls = 0;            // the plane function's calls without a player view (the warm-up)
  uint32_t scratch = 0;          // guest: the segment (32) and the result (96)
  bool scratch_failed = false;
  float logged_plane = 1.0f;
  bool logged_any = false;
  Clock::time_point logged_at;
  float saved_f31 = 1.0f;        // Hook_TitlePlaneZ's f31 before it changed it, for Hook_TitlePlaneW
  bool pending = false;          // the Z hook changed f31 in this call
};
thread_local State t_state;      // the plane function runs on one guest thread; the state is its own

uint8_t* Base() { return larecomp::guest::Membase(); }

uint32_t ReadU32(uint8_t* base, uint32_t ea) {
  uint32_t v;
  std::memcpy(&v, base + ea, sizeof(v));
  return __builtin_bswap32(v);
}

float ReadF32(uint8_t* base, uint32_t ea) {
  const uint32_t bits = ReadU32(base, ea);
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

void WriteF32(uint8_t* base, uint32_t ea, float f) {
  uint32_t v;
  std::memcpy(&v, &f, sizeof(v));
  v = __builtin_bswap32(v);
  std::memcpy(base + ea, &v, sizeof(v));
}

PPCFunc* GuestFn(uint32_t addr) {
  auto* rt = REX_RUNTIME();
  if (!rt) return nullptr;
  return rt->FindFunction(addr).value_or(nullptr);
}

bool EnsureScratch(State& s) {
  if (s.scratch) return true;
  if (s.scratch_failed) return false;
  PPCFunc* fn = GuestFn(kGuestMallocFn);
  const uint32_t raw = fn ? larecomp::guest::Call<uint32_t>(fn, kScratchBytes) : 0;
  if (!raw) {
    s.scratch_failed = true;
    return false;
  }
  s.scratch = (raw + 15u) & ~15u;
  return true;
}

// The first thing the segment from `a` to `b` meets in the physics level: 1 a hit, 0 nothing, -1 the probe cannot run (no level yet, no guest thread).
int ProbeSegment(State& s, tp::Vec3 a, tp::Vec3 b, tp::Hit* out) {
  uint8_t* base = Base();
  PPCFunc* fn = GuestFn(kProbeFn);
  if (!base || !fn) return -1;
  const uint32_t level = ReadU32(base, kLevelPtr);
  if (!level || !EnsureScratch(s)) return -1;
  const uint32_t seg = s.scratch;
  const uint32_t res = s.scratch + 32;
  const float seg_values[8] = {a.x, a.y, a.z, 0.0f, b.x, b.y, b.z, 0.0f};
  for (uint32_t i = 0; i < 8; ++i) WriteF32(base, seg + 4 * i, seg_values[i]);
  std::memset(base + res, 0, 96);
  base[res + 76] = 0xFF;  // component: none
  base[res + 77] = 0xFF;
  const uint32_t hit = larecomp::guest::Call<uint32_t>(fn, level, seg, res, 0u, 0x800u, 0xFFFFFFFFu, 4u, 1u, 0u);
  if (!(hit & 0xFF)) return 0;
  out->pos = {ReadF32(base, res + 16), ReadF32(base, res + 20), ReadF32(base, res + 24)};
  out->normal = {ReadF32(base, res + 32), ReadF32(base, res + 36), ReadF32(base, res + 40)};
  return 1;
}

// One round of probes from the published camera. True with the plane the hits say.
bool ProbeGround(State& s, float* plane, size_t* ground_hits, size_t* rays) {
  mc::final_camera::Pose pose;
  double age = 0.0;
  if (!mc::final_camera::Latest(&pose, &age) || !tp::Fresh(age)) return false;
  tp::Camera cam;
  cam.valid = true;
  cam.pos = {pose.pos[0], pose.pos[1], pose.pos[2]};
  cam.right = {pose.right[0], pose.right[1], pose.right[2]};
  cam.up = {pose.up[0], pose.up[1], pose.up[2]};
  cam.back = {pose.back[0], pose.back[1], pose.back[2]};
  cam.fov_degrees = pose.fov_degrees;
  if (!tp::Usable(cam)) return false;
  std::vector<tp::Hit> hits;
  hits.reserve(tp::kRayCount);
  *rays = 0;
  for (size_t i = 0; i < tp::kRayCount; ++i) {
    const tp::Ray ray = tp::ProbeRay(cam, i);
    if (!(tp::Length(ray.dir) > 0.5f)) continue;
    const tp::Vec3 end = tp::Add(ray.origin, tp::Scale(ray.dir, tp::kRayLengthMetres));
    tp::Hit hit;
    const int r = ProbeSegment(s, ray.origin, end, &hit);
    if (r < 0) return false;  // no level, no guest thread: nothing to say this time
    ++*rays;
    if (r == 1) hits.push_back(hit);
  }
  size_t n = 0;
  for (const tp::Hit& h : hits) {
    if (tp::IsGround(h, cam.pos.y)) ++n;
  }
  *ground_hits = n;
  return tp::PlaneFromHits(hits, cam.pos.y, plane);
}

void LogPlane(State& s, float plane, size_t ground, size_t rays, bool known) {
  const auto now = Clock::now();
  const bool first = !s.logged_any;
  const bool moved = s.logged_any && std::fabs(plane - s.logged_plane) > 3.0f;
  if (!first && !moved) return;
  if (!first && now - s.logged_at < std::chrono::seconds(5)) return;
  s.logged_any = true;
  s.logged_plane = plane;
  s.logged_at = now;
  LARECOMP_APP_INFO(
      "[title plane] no player view: the road reflection's mirror plane is y = {:.2f} (the game's own is 1.00: sea level), from {} ground hits of {} rays through the picture{}",
      plane, ground, rays, known ? "" : " (held: this round found no ground)");
}

}  // namespace

// 0x822F3AF4 in sub_822F3918, before `stfs f31,88(r1)`. Only the path without a player view gets here.
void Hook_TitlePlaneZ(PPCRegister& f31) {
  State& s = t_state;
  s.pending = false;
  ++s.calls;
  const auto now = Clock::now();
  if (s.calls > tp::kWarmupCalls) {
    const bool due = !s.probed_once || std::chrono::duration<float>(now - s.probed_at).count() >= tp::kRefreshSeconds;
    if (due) {
      s.probed_once = true;
      s.probed_at = now;
      float plane = 0.0f;
      size_t ground = 0, rays = 0;
      if (ProbeGround(s, &plane, &ground, &rays)) {
        const float dt = s.applied_valid ? std::chrono::duration<float>(now - s.applied_at).count() : 0.0f;
        s.target = plane;
        s.plane = tp::Follow(s.have_plane, s.plane, plane, dt);
        s.applied_at = now;
        s.applied_valid = true;
        s.have_plane = true;
        LogPlane(s, s.plane, ground, rays, true);
      }
    }
  }
  if (!s.have_plane) return;  // nothing known: the game's own value
  s.saved_f31 = float(f31.f64);
  s.pending = true;
  f31.f64 = double(tp::Chosen(true, s.plane, s.saved_f31));
}

// 0x822F3AF8 in sub_822F3918, before `stfs f31,92(r1)`: both paths arrive here; the w component is the game's constant.
void Hook_TitlePlaneW(PPCRegister& f31) {
  State& s = t_state;
  if (!s.pending) return;
  s.pending = false;
  f31.f64 = double(s.saved_f31);
}

#endif  // REXGLUE_HAS_XEO3_TARGET
