#include <rex/ppc/context.h>

#if defined(LARECOMP_HAS_GPU) && !defined(REXGLUE_HAS_XEO3_TARGET)
#include <algorithm>
#include <chrono>

#include "gpu/cascade_schedule.h"
#include "gpu/cycles.h"
#include "gpu/side_views.h"
#include "gpu/world_clock.h"
#include "hooks_internal.h"
#include "larecomp_guest.h"
#include "larecomp_log.h"
#include "settings/larecomp_settings.h"

namespace cs = mcla::gpu::cascade_schedule;
namespace wc = mcla::gpu::world_clock;
namespace {

using Clock = std::chrono::steady_clock;
constexpr uint32_t kShadowManager = 0x8288D2B4;
constexpr uint32_t kCurrentViewport = 0x82839704;
constexpr unsigned kWindow = 300;

struct History {
  cs::View view;
  cs::Casters casters;
  uint64_t frame = 0;
  cs::TileAddresses addresses;
};
struct Counts {
  std::array<unsigned, size_t(cs::Reason::kCount)> reasons{};
  unsigned fits = 0;
  unsigned draws = 0;
  unsigned clears = 0;
  unsigned resolves = 0;
  unsigned held_draws = 0;
  unsigned held_clears = 0;
  unsigned held_resolves = 0;
  unsigned collector_uploads = 0;
  unsigned held_collector_uploads = 0;
  double guest_ms = 0;
  uint64_t capture_cycles = 0;
};
struct State {
  std::array<History, 4> history{};
  std::array<Counts, 4> counts{};
  cs::Casters casters;
  cs::View previous;
  Clock::time_point start{};
  Clock::time_point window_start{};
  uint64_t window_tsc = 0;
  uint64_t frame = 0;
  uint32_t manager = 0;
  uint32_t renderer = 0;
  uint32_t detail = 0;
  uint8_t held = 0;
  bool enabled = false;
  unsigned samples = 0;
  unsigned drive_samples = 0;
  double travel = 0, yaw = 0, max_travel = 0, max_yaw = 0;
  bool fourth_held = false;
};
thread_local State state;

cs::Vec ReadVec(const uint8_t* base, uint32_t address) {
  return {ReadGuestF32(base, address), ReadGuestF32(base, address + 4),
          ReadGuestF32(base, address + 8)};
}

cs::View ReadView(const uint8_t* base, uint32_t manager, uint32_t camera, uint32_t basis) {
  cs::View view;
  const uint32_t viewport = ReadGuestU32(base, kCurrentViewport);
  if (!cs::GuestPointer(viewport) || !cs::GuestPointer(camera) || !cs::GuestPointer(basis)) return view;
  view.eye = ReadVec(base, camera + 48);
  view.forward = ReadVec(base, camera + 32);
  view.heading = {view.forward[0], 0, view.forward[2]};
  view.sun = ReadVec(base, basis + 32);
  view.fit = {ReadGuestF32(base, manager + 160), ReadGuestF32(base, manager + 164),
              ReadGuestF32(base, manager + 172), ReadGuestF32(base, manager + 176),
              ReadGuestF32(base, viewport + 696)};
  view.cut = cs::camera_cut.load(std::memory_order_relaxed);
  view.hour_jump = wc::HourJumps();
  view.weather = wc::Weather();
  view.valid = std::isfinite(cs::Dot(view.eye, view.eye)) &&
               cs::Normalize(view.forward) && cs::Normalize(view.heading) && cs::Normalize(view.sun);
  for (float value : view.fit) view.valid &= std::isfinite(value);
  return view;
}

void NoteDrive(const cs::View& view) {
  if (state.previous.valid && view.valid && state.previous.cut == view.cut) {
    const double travel = std::sqrt(cs::DeltaSquared(state.previous.eye, view.eye));
    const double yaw = std::acos(std::clamp(cs::Dot(state.previous.heading, view.heading), -1.0f, 1.0f))
                       * (180.0 / 3.141592653589793);
    ++state.drive_samples;
    state.travel += travel;
    state.yaw += yaw;
    state.max_travel = std::max(state.max_travel, travel);
    state.max_yaw = std::max(state.max_yaw, yaw);
  }
  state.previous = view;
}

void Report() {
  const double frames = state.samples;
  const uint64_t cycles = mcla::gpu::cycles::Now() - state.window_tsc;
  const double ms_per_cycle = std::chrono::duration<double, std::milli>(
      Clock::now() - state.window_start).count() / double(std::max<uint64_t>(cycles, 1));
  for (unsigned i = 0; i < state.counts.size(); ++i) {
    const Counts& c = state.counts[i];
    const auto n = [&c](cs::Reason reason) { return c.reasons[size_t(reason)]; };
    LARECOMP_APP_INFO("[shadow ledger] cascade {} over {} daylight frames: fit {} hold {} "
                     "draws {} clear {} resolve {} | held draws {} clear {} resolve {} | "
                     "collector uploads {} (held {}) | guest {:.3f} ms/frame capture {:.3f} ms/frame | "
                     "refresh cold {} cadence {} travel {} turn {} light {} cut {} fit {} caster {}",
                     i + 1, state.samples, c.fits, n(cs::Reason::kHold), c.draws, c.clears, c.resolves,
                     c.held_draws, c.held_clears, c.held_resolves, c.collector_uploads, c.held_collector_uploads,
                     c.guest_ms / frames, c.capture_cycles * ms_per_cycle / frames,
                     n(cs::Reason::kCold), n(cs::Reason::kCadence), n(cs::Reason::kTravel), n(cs::Reason::kTurn),
                     n(cs::Reason::kLight), n(cs::Reason::kCut), n(cs::Reason::kFit), n(cs::Reason::kCaster));
  }
  const double pairs = std::max(state.drive_samples, 1u);
  LARECOMP_APP_INFO("[shadow ledger] drive {} camera pairs: travel mean {:.3f} max {:.3f} m/frame, "
                   "yaw mean {:.3f} max {:.3f} degrees/frame",
                   state.drive_samples, state.travel / pairs, state.max_travel, state.yaw / pairs, state.max_yaw);
  state.counts = {};
  state.samples = state.drive_samples = 0;
  state.travel = state.yaw = state.max_travel = state.max_yaw = 0;
}

}  // namespace

void cs::NoteCaster(uint32_t id, const float (&position)[3], uint8_t last_cascade) {
  if (track_casters && last_cascade >= 3) {
    state.casters.Add({id, {position[0], position[1], position[2]}, last_cascade});
  }
}

uint64_t cs::FrameSerial() { return state.frame; }

void Patch_CascadeFrame(PPCRegister& r21, PPCRegister& r27) {
  using namespace mcla::gpu;
  const bool daylight = (r21.u32 & cs::kCascadeMask) == cs::kCascadeMask;
  auto* base = larecomp::guest::Membase();
  const uint32_t manager = daylight && base ? ReadGuestU32(base, kShadowManager) : 0;
  const std::string detail = daylight ? side_views::SettingText(SideView::kShadow) : "";
  const uint32_t detail_id = detail == "ultra" ? 3 : detail == "high" ? 2 : detail == "auto" ? 1 : 0;

  if (manager != state.manager || state.renderer != r27.u32 || state.detail != detail_id) {
    state.history = {};
    state.previous = {};
  }
  state.manager = cs::GuestPointer(manager) ? manager : 0;
  state.renderer = r27.u32;
  state.detail = detail_id;
  ++state.frame;
  state.held = 0;
  state.fourth_held = false;
  state.casters.count = 0;
  state.casters.incomplete = false;
  cs::capture_cascade = 0;
  cs::time_guest = daylight && larecomp::settings::mcla_gpu_shadow_ledger.Get();
  cs::time_draws = daylight && (cs::time_guest || larecomp::settings::mcla_gpu_pass_timing.Get());
  bool enabled = daylight && state.manager && detail_id && larecomp::settings::mcla_gpu_shadow_schedule.Get();
  if (enabled) {
    const auto expected = ScaleOfLevel(side_views::LevelOf(SideView::kShadow));
    enabled = cs::atlas_ready_scale.load(std::memory_order_acquire) == expected.n;
  }
  if (state.enabled && !enabled) state.history = {};
  state.enabled = enabled;
  cs::track_casters = state.enabled;
  if (cs::time_guest) {
    if (state.samples == kWindow) Report();
    if (state.samples++ == 0) {
      state.window_start = Clock::now();
      state.window_tsc = cycles::Now();
    }
  }
}

void Patch_CascadePhaseBegin(PPCRegister& r30) {
  if (!cs::time_guest || !cs::Cascade(r30.u32)) return;
  state.start = Clock::now();
  cs::capture_cycles = cs::captured_draws = cs::captured_clears = cs::captured_resolves = 0;
}

// The fit and bind skip together. The viewport, tile matrix and planes stay
// paired with the old quadrant. Cascade 4 still populates the cull context
// from the four tiles, so a held 4 can coexist with a freshly fitted 3.
// CHECK: exercise held 4 with refreshed 3 in the cull and collector captures.
bool Patch_CascadeFit(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6) {
  const uint8_t cascade = static_cast<uint8_t>(r4.u32 + 1);
  if (r4.u32 >= 4) return false;
  cs::capture_cascade = cascade;
  if (cs::time_guest) ++state.counts[cascade - 1].fits;
  if ((!state.enabled && !cs::time_guest) || (cascade < 3 && !cs::time_guest)) return false;

  const auto* base = larecomp::guest::Membase();
  if (!base || !state.manager || r3.u32 != state.manager) return false;
  const cs::View view = ReadView(base, r3.u32, r5.u32, r6.u32);
  if (cs::time_guest && cascade == 1) NoteDrive(view);
  if (cascade < 3) return false;

  const auto addresses = cs::ReadTileAddresses(state.manager, cascade,
      [base](uint32_t address) { return ReadGuestU32(base, address); });
  History& h = state.history[cascade - 1];
  cs::Reason reason = cs::Reason::kCold;
  if (state.enabled && addresses.Valid() && h.addresses == addresses) {
    reason = cs::Decide(cascade, state.frame - h.frame, h.view, view, h.casters, state.casters);
  }

  const bool hold = reason == cs::Reason::kHold;
  if (hold) state.held = cascade;
  else if (state.enabled) h = {view, state.casters, state.frame, addresses};
  if (cs::time_guest) {
    ++state.counts[cascade - 1].reasons[size_t(reason)];
    if (hold) --state.counts[cascade - 1].fits;
  }
  return hold;
}

bool Patch_CascadeClients(PPCRegister& r30) {
  return state.held && cs::Cascade(r30.u32) == state.held;
}

bool Patch_CascadeBind(PPCRegister& r4) {
  return state.held && r4.u32 == uint32_t(state.held - 1);
}

// On 4 the jump lands before the collector constants and impostor draw.
bool Patch_CascadeResolve(PPCRegister& r4) {
  return Patch_CascadeBind(r4);
}

void Patch_CascadePhaseEnd(PPCRegister& r30) {
  const uint32_t index = r30.u32;
  if (index >= 4) return;
  state.fourth_held = state.held == 4;
  if (cs::time_guest && state.start != Clock::time_point{}) {
    Counts& c = state.counts[index];
    c.guest_ms += std::chrono::duration<double, std::milli>(Clock::now() - state.start).count();
    c.draws += cs::captured_draws;
    c.clears += cs::captured_clears;
    c.resolves += cs::captured_resolves;
    if (state.held) {
      c.held_draws += cs::captured_draws;
      c.held_clears += cs::captured_clears;
      c.held_resolves += cs::captured_resolves;
    }
    c.capture_cycles += cs::capture_cycles;
    state.start = {};
  }
  cs::capture_cascade = 0;
  state.held = 0;
}

void Patch_CascadeCollectorUpload() {
  if (!cs::time_guest) return;
  ++state.counts[3].collector_uploads;
  if (state.fourth_held) ++state.counts[3].held_collector_uploads;
}

#else
void Patch_CascadeFrame(PPCRegister&, PPCRegister&) {}
void Patch_CascadePhaseBegin(PPCRegister&) {}
bool Patch_CascadeFit(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&) { return false; }
bool Patch_CascadeClients(PPCRegister&) { return false; }
bool Patch_CascadeBind(PPCRegister&) { return false; }
bool Patch_CascadeResolve(PPCRegister&) { return false; }
void Patch_CascadePhaseEnd(PPCRegister&) {}
void Patch_CascadeCollectorUpload() {}
#endif
