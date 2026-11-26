#include <rex/ppc/context.h>

#if defined(LARECOMP_HAS_GPU) && !defined(REXGLUE_HAS_XEO3_TARGET)
#include "gpu/cascade_fit.h"
#include "gpu/cascade_schedule_rules.h"
#include "gpu/side_views.h"
#include "hooks_internal.h"
#include "larecomp_guest.h"
#include "settings/larecomp_settings.h"

namespace cf = mcla::gpu::cascade_fit;
namespace cs = mcla::gpu::cascade_schedule;

namespace {

constexpr uint32_t kShadowCullContext = 0x829019E4;
constexpr uint32_t kFarSlice = 160;
constexpr uint32_t kTileFar = 84;
constexpr uint32_t kTileZShift = 100;
constexpr auto kMaxReadbackAge = std::chrono::milliseconds(100);

struct State {
  uint32_t manager = 0;
  uint32_t renderer = 0;
  uint32_t texels = 0;
  uint32_t tile = 0;
  uint32_t basis = 0;
  unsigned index = 0;
  bool use_depth = false;
  bool stable = false;
  float stock_slice = 0.0f;
  float slice = 0.0f;
  double margin = 0.0;
  bool slice_written = false;
  bool fitting = false;
  cf::DepthSample sample;
  cf::Box box;
  std::array<float, 4> fars{};
  std::array<bool, 4> padded{};
};
thread_local State state;

cf::Vec ReadVec(const uint8_t* base, uint32_t address) {
  return {ReadGuestF32(base, address), ReadGuestF32(base, address + 4),
          ReadGuestF32(base, address + 8)};
}

void WriteDirection(uint8_t* base, uint32_t address, const cf::Vec& direction) {
  for (unsigned i = 0; i < 3; ++i) WriteGuestF32(base, address + i * 4, float(direction[i]));
  WriteGuestF32(base, address + 12, 0.0f);
}

}  // namespace

bool cf::BeginFrame(uint32_t manager, uint32_t renderer, uint32_t texels, bool census) {
  const bool enhanced = manager && texels > 640;
  const bool use_depth = enhanced && larecomp::settings::mcla_gpu_shadow_visible_far.Get();
  const bool stable = enhanced && larecomp::settings::mcla_gpu_shadow_stable_fit.Get();
  if (manager != state.manager || renderer != state.renderer || texels != state.texels ||
      use_depth != state.use_depth || stable != state.stable) {
    depth_epoch.fetch_add(1);
    state = {};
  }
  state.manager = manager;
  state.renderer = renderer;
  state.texels = texels;
  state.use_depth = use_depth;
  state.stable = stable;
  state.tile = state.basis = 0;
  state.fitting = false;
  state.sample = use_depth ? LatestDepth() : DepthSample{};
  depth_wanted.store(use_depth, std::memory_order_release);
  mcla::gpu::BeginCascadeDensity(census && manager, texels);
  return use_depth || stable || (census && manager);
}

void cf::PlanCascade(uint8_t* base, uint32_t cascade, uint32_t camera, uint32_t basis,
                     const cs::TileAddresses& addresses, cs::View& view) {
  state.tile = 0;
  if (!base || !state.manager || !view.valid || !addresses.Valid()) return;
  const float stock_slice = view.fit[0];
  const float nominal = ReadGuestF32(base, addresses.tile + kTileFar);
  const float stock = stock_slice * nominal;
  const float backshift = ReadGuestF32(base, addresses.tile + kTileZShift);
  if (!(stock > 1.0f && stock < 512.0f && backshift >= 0.0f && backshift <= 16.0f)) return;
  const unsigned index = cascade - 1;
  float inner = 0.0f;
  if (state.use_depth && index != 0) {
    const auto inner_addresses = cs::ReadTileAddresses(state.manager, uint8_t(index),
        [base](uint32_t address) { return ReadGuestU32(base, address); });
    if (inner_addresses.Valid()) inner = stock_slice * ReadGuestF32(base, inner_addresses.tile + kTileFar);
    if (!std::isfinite(inner)) return;
  }
  float fitted_far = stock;
  const auto& sample = state.sample;
  const bool fresh = state.use_depth && sample.cut == view.cut &&
      sample.epoch == depth_epoch.load() && Clock::now() - sample.recorded <= kMaxReadbackAge;
  if (fresh && cascade > 1) {
    const Vec eye{view.eye[0], view.eye[1], view.eye[2]};
    const Vec forward{-view.forward[0], -view.forward[1], -view.forward[2]};
    const Vec up = ReadVec(base, camera + 16);
    if (std::abs(Length(up) - 1.0) < 0.01) {
      const double required =
          VisibleFar(sample.pose, {eye, forward, Unit(up)}, sample.max_depth, stock);
      fitted_far = SelectFar(stock, inner, required + backshift, state.fars[index]);
    }
  }
  state.fars[index] = fitted_far;
  state.tile = addresses.tile;
  state.basis = basis;
  state.index = index;
  state.stock_slice = stock_slice;
  state.slice = fitted_far < stock ? fitted_far / nominal : stock_slice;
  view.stable_far = state.stable ? fitted_far + backshift : 0.0f;
  state.margin = cascade >= 3 ? FitMargin(view.stable_far) : kOneFrameTravel;
  view.fit[0] = state.slice;
  // Roll moves the cone's corners even with an unchanged forward vector.
  view.up = {ReadGuestF32(base, camera + 16), ReadGuestF32(base, camera + 20),
             ReadGuestF32(base, camera + 24)};
  if (state.stable) view.valid &= cs::Normalize(view.up);
}

void Patch_CascadeConeBegin(PPCRegister& r3, PPCRegister& r4, PPCRegister& r7) {
  state.box = {};
  state.fitting = state.tile && r3.u32 == state.manager &&
                  r4.u32 == state.tile && r7.u32 == state.basis;
  if (!state.fitting) return;
  state.padded[state.index] = state.stable || state.slice != state.stock_slice;
  auto* base = larecomp::guest::Membase();
  if (!base || state.slice == state.stock_slice) return;
  WriteGuestF32(base, state.manager + kFarSlice, state.slice);
  state.slice_written = true;
}

void Patch_CascadeConeReady(PPCRegister& r1, PPCRegister& r29, PPCRegister& r31) {
  if (!state.fitting || r31.u32 != state.tile || r29.u32 != state.basis) return;
  auto* base = larecomp::guest::Membase();
  if (!base || !cs::GuestPointer(r1.u32)) return;
  // sub_825F4FB8 has consumed FarSlice and written the near and far rings on
  // the caller's stack. Neither the tune nor the other cascades inherit it.
  if (state.slice_written) {
    WriteGuestF32(base, state.manager + kFarSlice, state.stock_slice);
    state.slice_written = false;
  }
  if (!state.stable) return;
  cf::Corners corners;
  for (unsigned i = 0; i < corners.size(); ++i) {
    corners[i] = ReadVec(base, r1.u32 + 128 + i * 16);
  }
  const auto sphere = cf::EncloseCone(corners);
  const cf::Vec sun = ReadVec(base, state.basis + 32);
  if (!(std::abs(cf::Length(sun) - 1.0) < 0.01)) return;
  const auto basis = cf::WorldBasis(cf::Unit(sun));
  state.box = cf::Snap(sphere, basis, state.texels, state.margin);
  if (!(state.box.half > 0.0)) return;
  WriteDirection(base, state.basis, basis.right);
  WriteDirection(base, state.basis + 16, basis.up);
  WriteDirection(base, state.basis + 32, basis.sun);
}

// CHECK: confirm the viewport, tile matrix and collector planes use the same
// box on guest codegen, and that the snap stays on the host atlas's texels.
void Patch_CascadeBox(PPCRegister& f6, PPCRegister& f7, PPCRegister& f8,
                      PPCRegister& f9, PPCRegister& f10, PPCRegister& f11) {
  if (!(state.box.half > 0.0)) return;
  const auto bounds = cf::Bounds(state.box);
  f6.f64 = bounds[0];
  f7.f64 = bounds[1];
  f8.f64 = bounds[2];
  f9.f64 = bounds[3];
  f10.f64 = bounds[4];
  f11.f64 = bounds[5];
  state.box = {};
}

void Patch_CascadeDensity(PPCRegister& f2, PPCRegister& f4) {
  if (!state.fitting) return;
  mcla::gpu::NoteCascadeDensity(state.index, state.fars[state.index], f2.f64, f4.f64);
  state.fitting = false;
}

// Only pad the cull copy. The boxes in the tile and the collector planes
// derived by the copy retain their exact raster bounds. The separate copy
// starts with five consecutive world-space planes per cascade (16-byte rows).
// CHECK: test which frame consumes this copy and watch a caster across a cut
// or a Far band that grows as the road opens.
void Patch_CascadeCullMargin(PPCRegister& r20) {
  if (r20.u32 != 3 || (!state.stable && !state.use_depth)) return;
  auto* base = larecomp::guest::Membase();
  if (!base) return;
  const uint32_t context = ReadGuestU32(base, kShadowCullContext);
  if (!cs::GuestPointer(context)) return;
  const uint32_t planes = ReadGuestU32(base, context + 336);
  if (!cs::GuestPointer(planes)) return;
  for (unsigned tile = 0; tile < state.padded.size(); ++tile) {
    if (!state.padded[tile]) continue;
    for (unsigned plane = 0; plane < 5; ++plane) {
      const uint32_t row = planes + (tile * 5 + plane) * 16;
      const auto normal = ReadVec(base, row);
      const float w = ReadGuestF32(base, row + 12);
      if (std::isfinite(w) && std::isfinite(cf::Dot(normal, normal))) {
        WriteGuestF32(base, row + 12, cf::PadPlane(normal, w, cf::kOneFrameTravel));
      }
    }
  }
}
#else
void Patch_CascadeConeBegin(PPCRegister&, PPCRegister&, PPCRegister&) {}
void Patch_CascadeConeReady(PPCRegister&, PPCRegister&, PPCRegister&) {}
void Patch_CascadeBox(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&) {}
void Patch_CascadeDensity(PPCRegister&, PPCRegister&) {}
void Patch_CascadeCullMargin(PPCRegister&) {}
#endif
