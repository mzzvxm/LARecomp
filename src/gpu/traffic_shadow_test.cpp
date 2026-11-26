// Check of the traffic cars' sun shadow rules (traffic_shadow.h): the setting's words and the `auto` cell per GPU
// class, the cascade phases, the limits of each mode, which vehicles are candidates, the cutoff that picks the nearest
// cars, the census over a frame's phases, the cutoff the renderer is told, and (the second run's findings) the main
// viewport the distances are measured from, the check of a distance against the item's own view depth, the distance
// bands and the three log lines. A program of its own: it needs no renderer,
// no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_traffic_shadow_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_traffic_shadow_test
// The turn of the shadow (traffic_shadow_pose.h: the World matrix of a raised traffic car's cascade draw, the headings, the turn rate) is
// checked here as well.
// or by hand: clang++ -std=c++23 -I src/gpu src/gpu/traffic_shadow_test.cpp -o traffic_shadow_test

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "gpu_class.h"
#include "traffic_shadow.h"
#include "traffic_shadow_gates.h"
#include "traffic_shadow_pose.h"

using namespace mcla::gpu;
using namespace mcla::gpu::traffic_shadow;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

std::string Num(const char* format, double a) {
  char buffer[160];
  std::snprintf(buffer, sizeof(buffer), format, a);
  return buffer;
}

// --- the setting ---------------------------------------------------------------------------------------------------

void SettingAndClasses() {
  Check(ParseMode("near") == Mode::kNear && ParseMode("all") == Mode::kAll && ParseMode("off") == Mode::kOff, "the three words");
  Check(ParseMode("") == Mode::kOff && ParseMode("auto") == Mode::kOff && ParseMode("Near") == Mode::kOff && ParseMode("on") == Mode::kOff,
        "anything else is off (auto is resolved before)");
  Check(ModeName(Mode::kNear) == "near" && ModeName(Mode::kAll) == "all" && ModeName(Mode::kOff) == "off", "the names");
  using gpu_class::GpuClass;
  Check(ModeFor("auto") == Mode::kNear, "legacy auto uses Enhanced");
  Check(ModeFor("off") == Mode::kOff, "explicit off");
  Check(ModeFor("near") == Mode::kNear, "explicit near");
  Check(ModeFor("all") == Mode::kAll, "explicit all");
  // The row's label for auto.
  Check(gpu_class::AutoLabelFor(GpuClass::kLow, gpu_class::Effect::kTrafficShadows) == "AUTO (NEAR)", "the row says AUTO (NEAR) on Low");
  Check(gpu_class::AutoLabelFor(GpuClass::kHigh, gpu_class::Effect::kTrafficShadows) == "AUTO (NEAR)", "the row says AUTO (NEAR) on High");
  Check(gpu_class::CvarName(gpu_class::Effect::kTrafficShadows) == "mcla_gpu_traffic_shadows", "the cvar's name");
}

void Phases() {
  Check(CascadeIndex(0x20) == 1 && CascadeIndex(0x40) == 2 && CascadeIndex(0x80) == 3 && CascadeIndex(0x100) == 4, "the four cascades in order");
  for (uint32_t p : {0u, 0x1u, 0x2u, 0x10u, 0x200u, 0x400u, 0x1000u, 0x2000u, 0x4000u, 0x10000u, 0x1000000u, 0x60u}) {
    Check(CascadeIndex(p) == 0, "phase " + Num("%.0f", double(p)) + " is not a cascade (the collector 0x1000 and the night phase 0x400 included)");
  }
}

void Reach() {
  constexpr float far_edges[] = {15.0f, 31.0f, 80.0f, 160.0f};
  constexpr float tile_centres[][2] = {{0.25f, 0.25f}, {0.25f, 0.75f}, {0.75f, 0.25f}, {0.75f, 0.75f}};
  constexpr float length = 6.0f;
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float eye[] = {100.0f, 0.0f, 30.0f};
  for (int cascade = 1; cascade <= 4; ++cascade) {
    const float far = far_edges[cascade - 1];
    Check(!BeyondCascadeFar(cascade, far + length, length), "the split includes a car straddling the edge");
    Check(BeyondCascadeFar(cascade, far + length + 0.001f, length), "each split uses its own far edge");
    Check(!BeyondCascadeFar(cascade, far + length + 0.001f, 2.0f * length), "longer cars retain the split");
    Check(!BeyondCascadeFar(cascade, far + length + 0.001f, length, 1.2f), "the guest can extend the split");
    Check(!BeyondCascadeFar(cascade, far, length, 0.5f), "a smaller guest split keeps the coarse test conservative");

    const float scale = 0.25f / far;
    const auto& centre = tile_centres[cascade - 1];
    AtlasMatrixUV matrix{};
    matrix[0][0] = scale;
    matrix[2][1] = -scale;  // the light-space V axis flips as in the atlas transform
    matrix[3] = {centre[0] - scale * eye[0], centre[1] + scale * eye[2]};
    const auto bounds = MakeCascadeReach(cascade, matrix, eye);
    Check(bounds.valid, "the current cascade's quadrant and nonzero translation are accepted");
    const float just_inside[] = {eye[0] + far + length, 1.0f, eye[2]};
    const float just_inside_v[] = {eye[0], 1.0f, eye[2] - far - length};
    const float outside_u[] = {eye[0] + far + length + 1.0f, 1.0f, eye[2]};
    const float outside_v[] = {eye[0], 1.0f, eye[2] - far - length - 1.0f};
    const float same_shadow_ray[] = {eye[0], 200.0f, eye[2]};
    const float bad_position[] = {outside_u[0], 1.0f, nan};
    Check(!bounds.MissesTile(just_inside, length), "touching the padded light-space edge retains the car");
    Check(!bounds.MissesTile(just_inside_v, length), "touching the vertical padded edge retains the car");
    Check(bounds.MissesTile(outside_u, length), "car fully outside the horizontal edge is skipped");
    Check(bounds.MissesTile(outside_v, length), "car fully outside the vertical edge is skipped");
    Check(!bounds.MissesTile(same_shadow_ray, length), "a distant caster along a light ray can still reach the tile");
    Check(!bounds.MissesTile(outside_v, nan) && !bounds.MissesTile(bad_position, length), "unknown caster bounds draw");
    Check(!MakeCascadeReach(cascade % 4 + 1, matrix, eye).valid, "the wrong tile fails open");
    matrix[0][1] = scale;
    matrix[3][1] -= scale * eye[0];
    Check(!MakeCascadeReach(cascade, matrix, eye).valid, "non-orthogonal light-space axes fail open");
  }
  Check(!BeyondCascadeFar(0, 200.0f, length) && !BeyondCascadeFar(5, 200.0f, length), "non-cascade phases have no reach cull");
  Check(!BeyondCascadeFar(1, nan, length) && !BeyondCascadeFar(1, 200.0f, length, 0.0f), "unknown view data fails open");
  const CascadeReach unknown{};
  Check(!unknown.MissesTile(eye, length), "an unavailable matrix keeps the original draw");
}

void LimitsOfTheModes() {
  const Limits off = LimitsFor(Mode::kOff, 0, 0, 0);
  Check(off.radius == 0.0f && off.cars == 0, "off: nothing casts");
  const Limits near = LimitsFor(Mode::kNear, 0, 0, 0);
  Check(near.radius == kNearRadius && near.cars == kNearCars && near.cascades == 4, "near: its own radius and count, all four cascades");
  const Limits all = LimitsFor(Mode::kAll, 0, 0, 0);
  Check(all.radius == kAllRadius && all.cars == kAllCars && all.cascades == 4, "all: its own radius and count");
  Check(all.radius > near.radius && all.cars > near.cars, "all reaches farther and takes more cars than near");
  Check(kNearCars <= kMaxCars && kAllCars <= kMaxCars, "no mode takes more than the most a setting may");
  // The debug overrides: 0 or less keeps the mode's value; a value replaces it, within the bounds.
  const Limits over = LimitsFor(Mode::kNear, 25.0, 9, 2);
  Check(over.radius == 25.0f && over.cars == 9 && over.cascades == 2, "the overrides replace the mode's values");
  const Limits big = LimitsFor(Mode::kAll, 99999.0, 999, 9);
  Check(big.radius == 1000.0f && big.cars == kMaxCars && big.cascades == 4, "the overrides are bounded (1000 m, the most cars, four cascades)");
  const Limits neg = LimitsFor(Mode::kNear, -5.0, -1, -2);
  Check(neg.radius == kNearRadius && neg.cars == kNearCars && neg.cascades == 4, "a negative override keeps the mode's value");
  Check(LimitsFor(Mode::kOff, 50.0, 8, 2).radius == 0.0f, "off stays off whatever the overrides say");
}

// --- which vehicles ------------------------------------------------------------------------------------------------

void Candidates() {
  VehicleFlags traffic;
  traffic.casts_in_game = 0;
  traffic.side_views = 1;
  traffic.visible = 1;
  traffic.enabled = 1;
  Check(Eligible(traffic), "a traffic car (+593 set, +594 clear, seen by the main view) is a candidate");
  VehicleFlags player = traffic;
  player.casts_in_game = 1;
  Check(!Eligible(player), "the player's car (+594 set) is not: the game draws it already");
  VehicleFlags no_side = traffic;
  no_side.side_views = 0;
  Check(!Eligible(no_side), "a vehicle without +593 (slot -1) is not a shadow caster at all");
  VehicleFlags hidden = traffic;
  hidden.hidden = 1;
  Check(!Eligible(hidden), "the game's own hidden byte (+6478) keeps a car out, as in its own condition");
  VehicleFlags off_screen = traffic;
  off_screen.visible = 0;
  Check(!Eligible(off_screen), "a car the main view does not see (+116 = 0) does not use a place: sub_8235FA90 would not draw it");
  VehicleFlags disabled = traffic;
  disabled.disabled = 1;
  Check(!Eligible(disabled), "+6467 set: nothing is drawn for it");
  VehicleFlags ghost = traffic;
  ghost.ghost = 1;
  Check(!Eligible(ghost), "+123 set: nothing is drawn for it");
  VehicleFlags not_enabled = traffic;
  not_enabled.enabled = 0;
  Check(!Eligible(not_enabled), "+120 = 0: nothing is drawn for it");

  const float eye[3] = {0.0f, 1.5f, 0.0f};
  const float at[3] = {3.0f, 1.5f, 4.0f};
  Check(std::fabs(Distance(eye, at) - 5.0f) < 1e-6f, "the distance is the Euclidean one (3-4-5)");
  const float nan_at[3] = {std::nanf(""), 0.0f, 0.0f};
  Check(!std::isfinite(Distance(eye, nan_at)), "a NaN position gives a distance that is not finite (the hook skips it)");
}

// --- the vehicles' reject bits ---------------------------------------------------------------------------------------

void VehicleRejects() {
  VehicleFlags ok;
  ok.side_views = 1;
  ok.visible = 1;
  ok.enabled = 1;
  Check(VehicleRejectBits(ok) == 0 && Eligible(ok), "an eligible vehicle has no reject bit");
  VehicleFlags f = ok;
  f.side_views = 0;
  Check(VehicleRejectBits(f) == kRejectSideViews, "+593 clear: its own bit");
  f = ok;
  f.hidden = 1;
  Check(VehicleRejectBits(f) == kRejectHidden, "+6478: its own bit");
  f = ok;
  f.disabled = 1;
  Check(VehicleRejectBits(f) == kRejectDisabled, "+6467: its own bit");
  f = ok;
  f.ghost = 1;
  Check(VehicleRejectBits(f) == kRejectGhost, "+123: its own bit");
  f = ok;
  f.visible = 0;
  Check(VehicleRejectBits(f) == kRejectNotVisible, "+116: its own bit");
  f = ok;
  f.enabled = 0;
  Check(VehicleRejectBits(f) == kRejectNotEnabled, "+120: its own bit");
  f = ok;
  f.hidden = 1;
  f.visible = 0;
  Check(VehicleRejectBits(f) == (kRejectHidden | kRejectNotVisible), "two flags: two bits");
  // The bits agree with Eligible() for every combination of the six flags (casts_in_game aside: the hook has returned before).
  bool agree = true;
  for (int mask = 0; mask < 64; ++mask) {
    VehicleFlags g;
    g.side_views = (mask & 1) ? 0 : 1;
    g.hidden = (mask & 2) ? 1 : 0;
    g.disabled = (mask & 4) ? 1 : 0;
    g.ghost = (mask & 8) ? 1 : 0;
    g.visible = (mask & 16) ? 0 : 1;
    g.enabled = (mask & 32) ? 0 : 1;
    agree = agree && ((VehicleRejectBits(g) == 0) == Eligible(g)) && ((VehicleRejectBits(g) == 0) == (mask == 0));
  }
  Check(agree, "reject bits and Eligible() agree on all 64 combinations");
}

// --- the traffic's route: the ambient client's mask, its item draw ----------------------------------------------------

void AmbientClientMask() {
  Check(kCascadeMask == 0x1E0u, "the four cascade phases as a mask");
  const uint32_t stock = 0x01118002u;  // mcAmbientCarRenderClass's mask as the game builds it (the mirror bit on)
  for (uint32_t phase : kCascadePhases) {
    Check((stock & phase) == 0, Num("the stock mask has no cascade (phase %.0f)", phase));
    Check(WidenedClientMask(stock, phase, true) == (stock | phase), Num("on: the phase is added (%.0f)", phase));
    Check((WidenedClientMask(stock, phase, true) & phase) == phase, "the loop's `mask & phase` is then non-zero");
    Check(WidenedClientMask(stock, phase, false) == stock, "off: the mask is the game's");
  }
  // Only the cascades: the collector, the colour pass, the rain phase and the others keep the game's mask.
  for (uint32_t phase : {0x1u, 0x2u, 0x4u, 0x8u, 0x10u, 0x200u, 0x400u, 0x1000u, 0x2000u, 0x4000u, 0x8000u, 0x10000u, 0x100000u, 0x1000000u}) {
    Check(WidenedClientMask(stock, phase, true) == stock, Num("a phase that is no cascade changes nothing (%.0f)", phase));
  }
  // Each phase is widened on its own: the mask the next iteration sees is the game's again.
  const uint32_t one = WidenedClientMask(stock, 0x20u, true);
  Check((one & 0x40u) == 0 && (one & 0x80u) == 0 && (one & 0x100u) == 0, "widening for one cascade leaves the others out");
  Check(kAmbientClientVtable == 0x820D8C0Cu && kAmbientBodyPhase == 0x100000u, "the guest constants of the traffic client");
  Check(kAmbientBodyPhase != 0x8000u && kAmbientBodyPhase != 0x10000u && kAmbientBodyPhase != 0x1000000u,
        "the body phase is not the lights' (0x8000), the LOD update's (0x10000) or the mirror's (0x1000000)");
  Check((0x01118002u & kAmbientBodyPhase) != 0, "the body phase is one the stock mask has (the item draw knows it)");
  Check(CascadeIndex(kAmbientBodyPhase) == 0, "the body phase the draw is told is no cascade (the hook is not re-entered on it)");
}

AmbientFlags GoodItem() {
  AmbientFlags f;
  f.enabled = 1;
  f.car_flags = 0x40;
  f.model_count = 3;
  f.lod = 0;
  return f;
}

void AmbientItems() {
  AmbientFlags f = GoodItem();
  Check(AmbientRejectBits(f) == 0 && AmbientEligible(f), "a traffic car the body phase would draw is eligible");
  f = GoodItem();
  f.enabled = 0;
  Check(AmbientRejectBits(f) == kAmbientRejectEnabled, "item +36 zero");
  f = GoodItem();
  f.car_flags = 0x60;
  Check(AmbientRejectBits(f) == kAmbientRejectCarFlags, "car +500 with 0x20 (a child): turned away");
  f.car_flags = 0xC0;
  Check(AmbientRejectBits(f) == kAmbientRejectCarFlags, "car +500 with 0x80: turned away");
  f.car_flags = 0x00;
  Check(AmbientRejectBits(f) == kAmbientRejectCarFlags, "car +500 without 0x40: turned away");
  f.car_flags = 0x44;
  Check(AmbientRejectBits(f) == 0, "other car +500 bits do not matter");
  f = GoodItem();
  f.model_count = 2;
  Check(AmbientRejectBits(f) == kAmbientRejectModels, "fewer than 3 models");
  f.model_count = -1;
  Check(AmbientRejectBits(f) == kAmbientRejectModels, "no model block (-1): the same bit");
  f = GoodItem();
  f.lod = -1;
  Check(AmbientRejectBits(f) == kAmbientRejectLod, "a negative LOD (outside the main view) is not drawn");
  f.lod = -2147483647 - 1;
  Check(AmbientRejectBits(f) == kAmbientRejectLod, "the most negative LOD: the same bit");
  // The body phase (0x100000) has no near-car exemption (the lights' phase 0x8000 has one) and does not look at the visible
  // byte (+557 + buffer, bit 4 = lights): only the LOD, the item and the car decide.
  f = GoodItem();
  f.lod = 1;
  Check(AmbientRejectBits(f) == 0, "LOD 1 is drawn");
  f.lod = 2;
  Check(AmbientRejectBits(f) == kAmbientRejectLodCoarse, "LOD 2, the coarsest: the body phase returns for it");
  f.lod = int32_t(kAmbientCoarsestLod);
  Check(AmbientRejectBits(f) == kAmbientRejectLodCoarse, "the coarsest LOD constant is 2");
  f.lod = 3;
  Check(AmbientRejectBits(f) == 0, "a LOD above 2 does not exist in the game; the draw's own test is `== 2`, so it passes");
  f = GoodItem();
  f.enabled = 0;
  f.lod = -2;
  f.model_count = 0;
  Check(AmbientRejectBits(f) == (kAmbientRejectEnabled | kAmbientRejectModels | kAmbientRejectLod), "several tests: several bits");
  // Every combination of the three plain flags and the LOD classes: the bits and AmbientEligible() agree.
  bool agree = true;
  for (int mask = 0; mask < 32; ++mask) {
    AmbientFlags g = GoodItem();
    g.enabled = (mask & 1) ? 0 : 1;
    g.car_flags = (mask & 2) ? 0x00 : 0x40;
    g.model_count = (mask & 4) ? 2 : 3;
    g.lod = (mask & 8) ? -1 : ((mask & 16) ? 2 : 0);
    agree = agree && ((AmbientRejectBits(g) == 0) == AmbientEligible(g)) && ((AmbientRejectBits(g) == 0) == (mask == 0));
  }
  Check(agree, "reject bits and AmbientEligible() agree on 32 combinations");
}

// The viewports the distances are measured from (the second run: the current viewport is the cascade's own in a cascade).
void ViewportsAndEyes() {
  Check(kRendererGlobal == 0x8287E064u && kRendererMainViewports == 180u && kCurrentViewportGlobal == 0x82839704u && kViewportEye == 112u,
        "the guest constants of the viewports");
  Check(MainViewportSlot(0) == 180u && MainViewportSlot(1) == 184u && MainViewportSlot(3) == 192u,
        "the main viewport of render buffer b is the pointer at renderer + 180 + 4 b ((b + 45) * 4, as sub_827902F0 reads it)");
  Check(kAmbientBufferGlobal == 0x827E5640u, "the render buffer's global is the one the item draw and sub_82794EE8 read");
  Check(kRendererGlobal != kCurrentViewportGlobal, "the renderer and the current viewport are different globals");
  // The distance of the same car from the two eyes: the main viewport's eye near it, the cascade's far.
  const float car[3] = {100.0f, 0.0f, 50.0f};
  const float main_eye[3] = {110.0f, 1.5f, 52.0f};
  const float cascade_eye[3] = {60.0f, 95.0f, -20.0f};
  Check(Distance(main_eye, car) < kNearRadius, Num("from the main viewport's eye the car is within the near radius (%.1f m)", double(Distance(main_eye, car))));
  Check(Distance(cascade_eye, car) > 100.0f, Num("from the cascade's eye it is beyond 100 m (%.1f m): the first version's census saw nothing nearer", double(Distance(cascade_eye, car))));
}

// The census's cross-check: the distance against the item's own depth + radius (item +32).
void DepthCheck() {
  // A car 30 m straight ahead, radius 2: the item's +32 is 32.
  Check(CheckAgainstDepth(30.0f, 32.0f, 2.0f) == DepthVerdict::kConsistent, "straight ahead: distance = depth");
  // The same car 20 degrees off the axis: the distance is longer by 1 / cos(20) = 1.064.
  Check(CheckAgainstDepth(30.0f / 0.9397f, 32.0f, 2.0f) == DepthVerdict::kConsistent, "20 degrees off the axis: a little longer than the depth");
  Check(CheckAgainstDepth(30.0f * 1.9f, 32.0f, 2.0f) == DepthVerdict::kConsistent, "at the edge of a very wide view: under twice the depth");
  // The first version's case: the cascade's eye (100 m away) for a car 30 m ahead of the main one.
  Check(CheckAgainstDepth(100.0f, 32.0f, 2.0f) == DepthVerdict::kFarBeyondView, "a distance of 100 m for a depth of 30 m: the eye is wrong");
  Check(CheckAgainstDepth(225.0f, 52.0f, 2.0f) == DepthVerdict::kFarBeyondView, "225 m for a depth of 50 m: the eye is wrong");
  // Closer than the depth is impossible for a car in the frustum.
  Check(CheckAgainstDepth(10.0f, 32.0f, 2.0f) == DepthVerdict::kCloserThanDepth, "10 m for a depth of 30 m: the position is wrong");
  Check(CheckAgainstDepth(29.5f, 32.0f, 2.0f) == DepthVerdict::kConsistent, "within a metre under the depth (the frame between the two reads)");
  // No usable depth.
  Check(CheckAgainstDepth(30.0f, std::nanf(""), 2.0f) == DepthVerdict::kUnknown, "a NaN depth");
  Check(CheckAgainstDepth(std::nanf(""), 32.0f, 2.0f) == DepthVerdict::kUnknown, "a NaN distance");
  Check(CheckAgainstDepth(30.0f, 32.0f, std::numeric_limits<float>::infinity()) == DepthVerdict::kUnknown, "an infinite radius");
  Check(CheckAgainstDepth(30.0f, 1.0f, 2.0f) == DepthVerdict::kUnknown, "a car at or behind the eye's plane (depth below zero): no band");
  Check(CheckAgainstDepth(30.0f, 5.0f, 5.0f) == DepthVerdict::kUnknown, "depth exactly zero: no band");
  Check(std::string(DepthVerdictName(DepthVerdict::kConsistent)) == "consistent" && std::string(DepthVerdictName(DepthVerdict::kUnknown)) == "unknown",
        "the verdicts have names");
}

void Bands() {
  Check(DistanceBand(0.0f) == 0 && DistanceBand(19.9f) == 0, "under 20 m: band 0");
  Check(DistanceBand(20.0f) == 1 && DistanceBand(39.9f) == 1, "20 to 40 m: band 1 (the near radius ends here)");
  Check(DistanceBand(40.0f) == 2 && DistanceBand(79.9f) == 2, "40 to 80 m: band 2");
  Check(DistanceBand(80.0f) == 3 && DistanceBand(159.9f) == 3, "80 to 160 m: band 3");
  Check(DistanceBand(160.0f) == 4 && DistanceBand(1.0e6f) == 4, "160 m and more: band 4");
  Check(DistanceBand(std::nanf("")) == kDistanceBands - 1 && DistanceBand(-3.0f) == kDistanceBands - 1, "NaN and negative: the far band");
  Check(kNearRadius == 40.0f, "band 1 ends at the near radius");
  for (int b = 0; b < kDistanceBands; ++b) Check(std::string(DistanceBandName(b)).size() >= 4, "every band has a name");
}

void AmbientPosition() {
  // The render thread's matrix: ((buffer + 4) rotated left by 6) & ~63 from the car; the update thread's at +128.
  Check(AmbientMatrixOffset(true, 0) == 256u && AmbientMatrixOffset(true, 1) == 320u, "the render thread's copies are 256 and 320");
  Check(AmbientMatrixOffset(true, 2) == 384u && AmbientMatrixOffset(true, 3) == 448u, "buffers 2 and 3 are 64 bytes apart too");
  Check(AmbientMatrixOffset(false, 0) == 128u && AmbientMatrixOffset(false, 1) == 128u, "the update thread's is at +128 whatever the buffer");
  Check(AmbientBufferValid(0) && AmbientBufferValid(1) && AmbientBufferValid(3), "buffer indices 0 to 3");
  Check(!AmbientBufferValid(4) && !AmbientBufferValid(0xFFFFFFFFu) && !AmbientBufferValid(0x82000000u), "anything else is not a buffer index");
  Check(kAmbientTranslation == 48u, "the translation row of a Matrix44 is at +48");

  const float good[3] = {100.0f, 2.0f, -50.0f};
  Check(PlausiblePosition(good), "a position");
  const float nan_p[3] = {0.0f, std::nanf(""), 0.0f};
  Check(!PlausiblePosition(nan_p), "NaN is not");
  const float inf_p[3] = {0.0f, 0.0f, std::numeric_limits<float>::infinity()};
  Check(!PlausiblePosition(inf_p), "infinity is not");
  const float far_p[3] = {2.0e6f, 0.0f, 0.0f};
  Check(!PlausiblePosition(far_p), "two million metres is not");
  const float edge_p[3] = {-9.0e5f, 0.0f, 0.0f};
  Check(PlausiblePosition(edge_p), "under a million metres is");
}

// The traffic route's census: items of the traffic client and vehicles of the car-model client go through one
// Selector, nearest first, and each car casts or not by the same cutoff.
void MixedCensus() {
  Selector selector;
  const Limits limits = LimitsFor(Mode::kNear, 0.0, 0, 0);  // 4 cars within 40 m
  const uint32_t racer = 0x70000100u;
  const uint32_t traffic[6] = {0x80000010u, 0x80000020u, 0x80000030u, 0x80000040u, 0x80000050u, 0x80000060u};
  const float distance[6] = {5.0f, 9.0f, 12.0f, 15.0f, 20.0f, 30.0f};
  // Frame 1 is the census: everybody asked in the first cascade, nobody known yet (cutoff = the radius).
  bool cast_first[7];
  cast_first[0] = selector.Decide(racer, 0x20u, 7.0f, limits);
  for (int i = 0; i < 6; ++i) cast_first[i + 1] = selector.Decide(traffic[i], 0x20u, distance[i], limits);
  bool all = true;
  for (bool c : cast_first) all = all && c;
  Check(all, "the first frame casts every car within the radius (no cutoff yet)");
  for (uint32_t phase : {0x40u, 0x80u, 0x100u}) {
    selector.Decide(racer, phase, 7.0f, limits);
    for (int i = 0; i < 6; ++i) selector.Decide(traffic[i], phase, distance[i], limits);
  }
  // Frame 2: the cutoff is between the 4th (12 m) and the 5th (15 m) nearest of the 7 (5, 7, 9, 12, 15, 20, 30), 13.5 m.
  Check(selector.Decide(racer, 0x20u, 7.0f, limits), "frame 2: the racer at 7 m (second nearest) casts");
  Check(selector.Decide(traffic[0], 0x20u, 5.0f, limits), "frame 2: traffic at 5 m casts");
  Check(selector.Decide(traffic[1], 0x20u, 9.0f, limits), "frame 2: traffic at 9 m (third nearest) casts");
  Check(selector.Decide(traffic[2], 0x20u, 12.0f, limits), "frame 2: traffic at 12 m (fourth nearest) casts");
  Check(!selector.Decide(traffic[3], 0x20u, 15.0f, limits), "frame 2: traffic at 15 m (fifth) does not");
  Check(!selector.Decide(traffic[5], 0x20u, 30.0f, limits), "frame 2: traffic at 30 m does not");
  Check(selector.last().candidates == 7, "the census counted all seven cars of the first frame");
}

// --- the gate counters ----------------------------------------------------------------------------------------------

void ValueCountsAndGates() {
  ValueCounts values;
  Check(values.Text() == "{}", "no value: {}");
  values.Add(0x10);
  values.Add(0x200);
  values.Add(0x10);
  values.Add(0x10);
  Check(values.Text() == "{0x10 x3, 0x200 x1}", "values in the order first seen, with their counts: " + values.Text());
  values.Add(0xFFFFFFFFu);
  Check(values.Text().find("0xFFFFFFFF x1") != std::string::npos, "the largest value is kept apart from an empty slot");
  values.Add(0);
  Check(values.Text().find("0x0 x1") != std::string::npos, "the value 0 is a value too");
  ValueCounts many;
  for (uint32_t v = 1; v <= ValueCounts::kSlots + 5; ++v) many.Add(v);
  Check(many.Text().find("other x5") != std::string::npos, "more distinct values than slots: the rest is counted as other (" + many.Text() + ")");
  size_t first_slots = 0;
  for (uint32_t v = 1; v <= ValueCounts::kSlots; ++v) {
    char key[32];
    std::snprintf(key, sizeof(key), "0x%X x1", v);
    if (many.Text().find(key) != std::string::npos) ++first_slots;
  }
  Check(first_slots == ValueCounts::kSlots, "the first kSlots values each have a slot");

  Gates gates;
  Check(gates.Get(CarGate::kCalls) == 0 && gates.Get(AmbientGate::kLeafCalls) == 0 && gates.Calls() == 0, "a fresh set is all zero");
  for (int i = 0; i < 5; ++i) gates.Add(CarGate::kCalls);
  gates.Add(CarGate::kGameCaster);
  gates.Add(CarGate::kGameCaster);
  gates.Add(AmbientGate::kLeafCalls);
  gates.Add(AmbientGate::kLeafCascade);
  gates.Add(AmbientGate::kRemapped);
  gates.Add(AmbientGate::kEyeMain);
  gates.Add(AmbientGate::kEyeMain);
  gates.Add(AmbientGate::kEyeFallback);
  gates.Add(AmbientGate::kLodCoarse);
  gates.Add(CarGate::kEyeMain);
  gates.Add(CarGate::kReach);
  gates.Add(AmbientGate::kReach);
  gates.Add(AmbientGate::kReach);
  Check(gates.Get(CarGate::kCalls) == 5 && gates.Get(CarGate::kGameCaster) == 2, "car counters count");
  Check(gates.Get(AmbientGate::kLeafCalls) == 1 && gates.Get(AmbientGate::kRemapped) == 1 && gates.Get(AmbientGate::kDecided) == 0,
        "ambient counters count and stay apart from the car's");
  Check(gates.BumpCalls() == 1 && gates.BumpCalls() == 2 && gates.Calls() == 2, "the schedule's call counter");
  gates.car_phases().Add(0x10);
  gates.car_phases().Add(0x10);
  gates.leaf_phases().Add(0x10000);
  gates.Add(CarGate::kOtherPhase);
  gates.Add(CarGate::kOtherPhase);

  const std::string line = gates.Line("near");
  Check(line.rfind("[traffic-shadows] gates: mode near; ", 0) == 0, "the line starts with the tag, `gates:` and the mode: " + line);
  Check(line.find('\n') == std::string::npos, "one line");
  Check(line.find("calls 5, r11 set (the game's own caster) 2, phase rejected 2 r28 {0x10 x2}") != std::string::npos,
        "the car-model hook's first tests: " + line);
  Check(line.find("other phases {0x10000 x1}") != std::string::npos, "the item draw's other phases: " + line);
  Check(line.find("reached Decide 0, cast 0; traffic route") != std::string::npos, "the car route ends, the traffic route starts: " + line);
  Check(line.find("item draw calls 1 (cascade 1, other phases") != std::string::npos, "the item draw's calls: " + line);
  Check(line.find("cast 1") != std::string::npos, "the traffic route's casts: " + line);
  Check(line.find("beyond cascade reach: car-model 1, ambient 2") != std::string::npos, "reach counts both routes");
  for (const char* part : {"mode off", "no memory", "vehicle ptr", "ineligible", "+593 clear", "+6478 set", "+6467 set", "+123 set", "+116 zero", "+120 zero",
                           "entity ptr", "camera ptr", "distance not finite", "reached Decide", "phase loop", "mask widened", "item ptr", "car ptr", "buffer",
                           "+36 zero", "car +500", "models<3", "LOD<0", "LOD 2", "position", "eye main viewport", "current viewport", "renderer ptr",
                           "viewport ptr"}) {
    Check(line.find(part) != std::string::npos, std::string("the line names `") + part + "`");
  }
  Check(line.find("not visible") == std::string::npos && line.find("no body") == std::string::npos,
        "the visible byte and the lights' body flag are no longer tests of the body phase, so they are not named");
  Check(line.find("eye main viewport 2 / current viewport 1") != std::string::npos, "the traffic route's eye counters: " + line);
  Check(line.find("eye main viewport 1 / current viewport 0") != std::string::npos, "the car route's eye counters: " + line);
  Check(line.find("LOD 2 1]") != std::string::npos, "the coarsest-LOD counter: " + line);
}

// The window line: deltas since the last one; the census line: the eyes and the bands.
void WindowAndCensusLines() {
  Gates gates;
  for (int i = 0; i < 10; ++i) gates.Add(AmbientGate::kLeafCascade);
  for (int i = 0; i < 4; ++i) gates.Add(AmbientGate::kIneligible);
  for (int i = 0; i < 3; ++i) gates.Add(AmbientGate::kLod);
  gates.Add(AmbientGate::kLodCoarse);
  for (int i = 0; i < 6; ++i) {
    gates.Add(AmbientGate::kDecided);
    gates.Add(AmbientGate::kEyeMain);
  }
  gates.Add(AmbientGate::kRemapped);
  for (int i = 0; i < 5; ++i) gates.Add(AmbientGate::kDepthConsistent);
  gates.Add(AmbientGate::kDepthFar);
  gates.Add(AmbientGate::kReach);
  const std::string w1 = gates.WindowLine();
  Check(w1.rfind("[traffic-shadows] window: ", 0) == 0 && w1.find('\n') == std::string::npos, "the window line: tag, one line: " + w1);
  Check(w1.find("item draws in cascades 10, turned away 4 [+36 zero 0, car +500 0, models<3 0, LOD<0 3, LOD 2 1], reached Decide 6, cast 1") != std::string::npos,
        "the first window counts from the start: " + w1);
  Check(w1.find("eye main viewport 6 / current viewport 0") != std::string::npos, "... with the eye counters: " + w1);
  Check(w1.find("consistent 5, closer than the depth 0, beyond the view 1, unknown 0") != std::string::npos, "... and the depth check: " + w1);
  Check(w1.find("beyond cascade reach 1") != std::string::npos, "first window includes skipped draws");
  // More traffic: the next window shows only the new counts.
  for (int i = 0; i < 7; ++i) gates.Add(AmbientGate::kLeafCascade);
  gates.Add(AmbientGate::kDecided);
  gates.Add(AmbientGate::kRemapped);
  gates.Add(AmbientGate::kRemapped);
  const std::string w2 = gates.WindowLine();
  Check(w2.find("item draws in cascades 7, turned away 0 [+36 zero 0, car +500 0, models<3 0, LOD<0 0, LOD 2 0], reached Decide 1, cast 2") != std::string::npos,
        "the second window counts what is new: " + w2);
  Check(w2.find("beyond cascade reach 0") != std::string::npos, "the reach window resets with the other counters");
  const std::string w3 = gates.WindowLine();
  Check(w3.find("item draws in cascades 0, turned away 0") != std::string::npos && w3.find("reached Decide 0, cast 0") != std::string::npos,
        "a window with nothing new is all zero: " + w3);
  Check(gates.Get(AmbientGate::kLeafCascade) == 17, "the windows do not reset the totals");

  // The census line.
  Gates census;
  std::string c0 = census.CensusLine();
  Check(c0.rfind("[traffic-shadows] census: no eye seen yet", 0) == 0 && c0.find('\n') == std::string::npos, "no eye yet: " + c0);
  Check(c0.find("<20 m [candidates 0, LOD<0 0, LOD 2 0, other 0]") != std::string::npos && c0.find(">=160 m [candidates 0") != std::string::npos,
        "every band is named, all zero: " + c0);
  const float main_eye[3] = {110.0f, 1.5f, 52.0f};
  const float cascade_eye[3] = {110.0f, 101.5f, 52.0f};
  census.NoteEyes(0x82F10000u, main_eye, 0x82F20000u, cascade_eye);
  census.AddBand(BandKind::kCandidate, 12.0f);
  census.AddBand(BandKind::kCandidate, 35.0f);
  census.AddBand(BandKind::kCandidate, 35.5f);
  census.AddBand(BandKind::kLodOutside, 15.0f);
  census.AddBand(BandKind::kLodCoarse, 200.0f);
  census.AddBand(BandKind::kOther, 90.0f);
  const EyeSnapshot e = census.Eyes();
  Check(e.valid && e.main_viewport == 0x82F10000u && e.current_viewport == 0x82F20000u && e.main_eye[1] == 1.5f && e.current_eye[1] == 101.5f,
        "the eyes are stored and read back exactly");
  const std::string c1 = census.CensusLine();
  Check(c1.find("main viewport 0x82F10000, current viewport 0x82F20000") != std::string::npos, "the viewports' addresses: " + c1);
  Check(c1.find("eye of the main viewport 110.0 1.5 52.0, of the current viewport 110.0 101.5 52.0, 100.0 m apart") != std::string::npos, "the eyes and their separation: " + c1);
  Check(c1.find("<20 m [candidates 1, LOD<0 1, LOD 2 0, other 0]") != std::string::npos, "band <20 m: " + c1);
  Check(c1.find("<40 m [candidates 2, LOD<0 0, LOD 2 0, other 0]") != std::string::npos, "band <40 m: " + c1);
  Check(c1.find("<80 m [candidates 0, LOD<0 0, LOD 2 0, other 0]") != std::string::npos, "band <80 m: " + c1);
  Check(c1.find("<160 m [candidates 0, LOD<0 0, LOD 2 0, other 1]") != std::string::npos, "band <160 m: " + c1);
  Check(c1.find(">=160 m [candidates 0, LOD<0 0, LOD 2 1, other 0]") != std::string::npos, "band >=160 m: " + c1);
  Check(census.Band(BandKind::kCandidate, 1) == 2 && census.Band(BandKind::kOther, 3) == 1, "the band counters can be read");
  Check(std::string(BandKindName(BandKind::kCandidate)) == "candidates" && std::string(BandKindName(BandKind::kLodOutside)) == "LOD<0", "the kinds have names");
}

void LogScheduleRules() {
  LogSchedule schedule;
  const int64_t s = 1'000'000'000;
  Check(!schedule.Due(0, 0) && !schedule.Due(1999, 5 * s), "no line before 2000 calls, however late");
  Check(!schedule.Due(2000, 6 * s), "the 2000th call only starts the first line's delay (the world's first half second has no LODs yet)");
  Check(!schedule.Due(2300, 10 * s) && !schedule.Due(9000, 25 * s), "no line within 20 s of that");
  Check(schedule.Due(9500, 26 * s), "the first line 20 s after it");
  Check(!schedule.Due(9501, 26 * s + 1) && !schedule.Due(50000, 50 * s), "none within 30 s of the first");
  Check(schedule.Due(50001, 56 * s), "the next one 30 s after the first");
  Check(!schedule.Due(60000, 60 * s) && !schedule.Due(60001, 85 * s), "and none until 30 s after that (86 s)");
  Check(schedule.Due(60002, 86 * s), "the third at +30 s");
  LogSchedule late;
  Check(!late.Due(9000, 100 * s) && !late.Due(9001, 110 * s) && late.Due(9002, 120 * s), "the delay counts from the first time the calls were enough, not from the start of the clock");
  Check(LogSchedule::kFirstDelayNs > 10 * s, "the delay is long enough for the world to settle");
}

// --- the cutoff ----------------------------------------------------------------------------------------------------

void Cutoff() {
  Limits limits = LimitsFor(Mode::kNear, 0, 0, 0);  // 40 m, 4 cars
  Check(CutoffFor(nullptr, 0, limits) == limits.radius, "no candidates: the radius");
  const float few[] = {5.0f, 9.0f, 20.0f, 33.0f};
  Check(CutoffFor(few, 4, limits) == limits.radius, "as many candidates as places: the radius");
  const float many[] = {50.0f, 9.0f, 20.0f, 5.0f, 12.0f, 14.0f, 80.0f};
  // within 40 m: 5, 9, 12, 14, 20 (five cars for four places): between the 4th (14) and the 5th (20)
  Check(std::fabs(CutoffFor(many, 7, limits) - 17.0f) < 1e-6f, "five cars within reach for four places: halfway between the 4th and the 5th nearest");
  // exactly the nearest four are at or under the cutoff
  const float cut = CutoffFor(many, 7, limits);
  int under = 0;
  for (float d : many) under += d <= cut ? 1 : 0;
  Check(under == 4, Num("the nearest four are at or under the cutoff (%.0f)", double(under)));
  // The order of arrival does not matter.
  const float shuffled[] = {14.0f, 80.0f, 5.0f, 20.0f, 50.0f, 12.0f, 9.0f};
  Check(CutoffFor(shuffled, 7, limits) == cut, "the cutoff does not depend on the order");
  // Cars beyond the radius do not compete for the places.
  const float far_cars[] = {5.0f, 9.0f, 100.0f, 120.0f, 130.0f, 140.0f};
  Check(CutoffFor(far_cars, 6, limits) == limits.radius, "cars beyond the radius take no place: two within reach, four places: the radius");
  // The radius caps the cutoff.
  Limits tight = limits;
  tight.radius = 10.0f;
  const float spread[] = {1.0f, 2.0f, 3.0f, 4.0f, 9.5f};
  Check(CutoffFor(spread, 5, tight) <= tight.radius, "the cutoff never passes the radius");
  // Ties: both cars at the same distance are at the cutoff and both cast (one place too many, not too few).
  const float ties[] = {3.0f, 6.0f, 6.0f, 6.0f, 7.0f, 30.0f};
  const float tie_cut = CutoffFor(ties, 6, limits);
  int tie_under = 0;
  for (float d : ties) tie_under += d <= tie_cut ? 1 : 0;
  Check(tie_under >= 4 && tie_under <= 5, Num("ties at the edge: at least the four places are filled, one more at most (%.0f)", double(tie_under)));
  // NaN and infinity are far away, not nearest.
  const float bad[] = {std::nanf(""), std::numeric_limits<float>::infinity(), 4.0f, 8.0f, 12.0f, 16.0f, 20.0f};
  const float bad_cut = CutoffFor(bad, 7, limits);
  Check(std::isfinite(bad_cut) && bad_cut >= 16.0f && bad_cut <= 20.0f, "a NaN or infinite distance is ignored");
  // A mode with no radius or no cars casts nothing.
  Limits none;
  Check(CutoffFor(few, 4, none) == 0.0f, "no limits: cutoff 0");
  Limits zero_cars = limits;
  zero_cars.cars = 0;
  Check(CutoffFor(few, 4, zero_cars) == 0.0f, "no places: cutoff 0");
  // More candidates than the table holds do not overrun it.
  std::vector<float> lots(100, 10.0f);
  Check(CutoffFor(lots.data(), 100, limits) <= limits.radius, "100 candidates are handled (the first 32 are counted)");
}

// --- the census over frames ---------------------------------------------------------------------------------------

struct Car {
  uint32_t id;
  float distance;
};

// One frame of the game's draw: the four cascade phases, each over every candidate car. Returns, per car, in how
// many of the phases it cast, and the phases' pattern in `cast[car][cascade]`.
struct FrameResult {
  std::vector<std::vector<bool>> cast;
};
FrameResult RunFrame(Selector& s, const std::vector<Car>& cars, const Limits& limits, const std::vector<uint32_t>& phases) {
  FrameResult r;
  r.cast.assign(cars.size(), std::vector<bool>(phases.size(), false));
  for (size_t p = 0; p < phases.size(); ++p) {
    for (size_t i = 0; i < cars.size(); ++i) {
      r.cast[i][p] = s.Decide(cars[i].id, phases[p], cars[i].distance, limits);
    }
  }
  return r;
}
int CastingCars(const FrameResult& r) {
  int n = 0;
  for (const auto& row : r.cast) n += (!row.empty() && row[0]) ? 1 : 0;
  return n;
}
const std::vector<uint32_t> kAllPhases = {0x20, 0x40, 0x80, 0x100};

void CensusOverFrames() {
  const Limits limits = LimitsFor(Mode::kNear, 0, 0, 0);  // 40 m, 4 cars
  const std::vector<Car> cars = {{0xA1, 8.0f}, {0xA2, 15.0f}, {0xA3, 22.0f}, {0xA4, 30.0f}, {0xA5, 35.0f}, {0xA6, 38.0f}, {0xA7, 90.0f}};
  Selector s;
  Check(s.cutoff() < 0.0f, "before the first frame there is no cutoff");
  // Frame 1: no history, every car within the radius casts (the radius is the cutoff).
  const FrameResult f1 = RunFrame(s, cars, limits, kAllPhases);
  Check(CastingCars(f1) == 6 && !f1.cast[6][0], "frame 1: no census yet: every car within the radius casts, the one at 90 m does not");
  // Frame 2 begins when the phase falls back to the first one: the cutoff from frame 1's census is used.
  const FrameResult f2 = RunFrame(s, cars, limits, kAllPhases);
  Check(s.last().frame == 1, "the boundary was found when the phase fell back (one frame closed)");
  Check(CastingCars(f2) == 4, Num("frame 2: the four nearest cast (%.0f)", double(CastingCars(f2))));
  Check(f2.cast[0][0] && f2.cast[1][0] && f2.cast[2][0] && f2.cast[3][0] && !f2.cast[4][0] && !f2.cast[5][0] && !f2.cast[6][0],
        "... the cars at 8, 15, 22 and 30 m, not the ones at 35, 38 and 90");
  // Consistent across the cascades of a frame: a car casts in all four or in none.
  bool consistent = true;
  for (const auto& row : f2.cast) {
    for (bool b : row) consistent = consistent && b == row[0];
  }
  Check(consistent, "a car casts in all four cascades of a frame or in none");
  const FrameStats& st = s.last();
  Check(st.candidates == 7 && st.casting == 6 && st.nearest == 8.0f && st.farthest_cast == 38.0f, "the closed frame's stats: 7 seen, 6 cast, nearest 8 m, farthest 38 m");
  // Frame 3 closes frame 2: 7 candidates, 4 cast.
  RunFrame(s, cars, limits, kAllPhases);
  Check(s.last().casting == 4 && s.last().candidates == 7 && s.last().farthest_cast == 30.0f, "frame 2's stats: 4 cast, the farthest at 30 m");
  Check(s.cutoff() > 30.0f && s.cutoff() < 35.0f, Num("the cutoff sits between the 4th and the 5th car (%.1f m)", double(s.cutoff())));
  // The nearest car moves away: the set follows the distances, one frame late.
  std::vector<Car> moved = cars;
  moved[0].distance = 36.5f;  // the car at 8 m is now between the 5th and 6th
  RunFrame(s, moved, limits, kAllPhases);   // frame 3: still decided by frame 2's census
  const FrameResult f4 = RunFrame(s, moved, limits, kAllPhases);
  Check(CastingCars(f4) == 4 && !f4.cast[0][0] && f4.cast[4][0], "a car that moved away loses its place to the next nearest, a frame later");
  // A car within the cutoff that appears new casts at once (its distance is all it needs).
  std::vector<Car> with_new = cars;
  with_new.push_back({0xB1, 5.0f});
  const FrameResult f5 = RunFrame(s, with_new, limits, kAllPhases);
  Check(f5.cast.back()[0], "a new car within the cutoff casts in its first frame");
}

void CascadeLimit() {
  const Limits limits = LimitsFor(Mode::kAll, 0, 0, 2);  // the first two cascades only
  const std::vector<Car> cars = {{0xC1, 10.0f}, {0xC2, 60.0f}};
  Selector s;
  const FrameResult f = RunFrame(s, cars, limits, kAllPhases);
  Check(f.cast[0][0] && f.cast[0][1] && !f.cast[0][2] && !f.cast[0][3], "with two cascades the car is in the first two and not in the third and fourth");
  Check(f.cast[1][0] && f.cast[1][1] && !f.cast[1][2], "likewise for a car at 60 m (inside the radius of all)");
}

void SinglePhaseFrames() {
  // A frame with only one cascade phase (the others disabled): the frame ends when the census phase sees a car again.
  const Limits limits = LimitsFor(Mode::kNear, 0, 0, 0);
  const std::vector<Car> cars = {{0x1, 5.0f}, {0x2, 10.0f}, {0x3, 15.0f}, {0x4, 20.0f}, {0x5, 25.0f}, {0x6, 30.0f}};
  Selector s;
  RunFrame(s, cars, limits, {0x20});
  const FrameResult f2 = RunFrame(s, cars, limits, {0x20});
  Check(s.last().frame == 1, "a repeated car in the census phase closes the frame");
  Check(CastingCars(f2) == 4, "so the four nearest cast in the second frame");
  // And a frame that starts at a later phase (the first cascade is off): that phase is the census phase.
  Selector t;
  RunFrame(t, cars, limits, {0x40, 0x80});
  const FrameResult g2 = RunFrame(t, cars, limits, {0x40, 0x80});
  Check(t.last().frame == 1 && t.last().candidates == 6, "a frame that begins at the second cascade counts its cars there");
  Check(CastingCars(g2) == 4, "and picks the four nearest");
}

void NonCascadePhases() {
  const Limits limits = LimitsFor(Mode::kAll, 0, 0, 0);
  Selector s;
  Check(!s.Decide(0x1, 0x1000, 3.0f, limits), "the collector phase never casts");
  Check(!s.Decide(0x1, 0x10, 3.0f, limits) && !s.Decide(0x1, 0x400, 3.0f, limits) && !s.Decide(0x1, 0x10000, 3.0f, limits), "other phases never cast");
  Check(s.last().frame == 0 && s.last().candidates == 0, "and they are not counted as a frame or a candidate");
  Check(!s.Decide(0x1, 0x20, std::nanf(""), limits), "a car with no distance does not cast");
  Check(!s.Decide(0x2, 0x20, 500.0f, limits), "a car beyond the radius does not cast");
  Limits no_cars = limits;
  no_cars.cars = 0;
  Selector t;
  Check(!t.Decide(0x1, 0x20, 3.0f, no_cars), "a limit of no cars casts nothing");
  Limits no_radius = limits;
  no_radius.radius = 0.0f;
  Check(!t.Decide(0x1, 0x40, 3.0f, no_radius), "a radius of 0 casts nothing");
}

void ResetAndNearAllSwitch() {
  const std::vector<Car> cars = {{0x1, 5.0f}, {0x2, 10.0f}, {0x3, 15.0f}, {0x4, 20.0f}, {0x5, 25.0f}, {0x6, 30.0f}, {0x7, 80.0f}};
  Selector s;
  const Limits near = LimitsFor(Mode::kNear, 0, 0, 0);
  RunFrame(s, cars, near, kAllPhases);
  RunFrame(s, cars, near, kAllPhases);
  Check(CastingCars(RunFrame(s, cars, near, kAllPhases)) == 4, "near: four cars cast");
  s.Reset();
  Check(s.cutoff() < 0.0f && s.last().frame == 0, "a reset forgets the cutoff and the stats");
  const Limits all = LimitsFor(Mode::kAll, 0, 0, 0);
  RunFrame(s, cars, all, kAllPhases);
  const FrameResult f = RunFrame(s, cars, all, kAllPhases);
  Check(CastingCars(f) == 7, "all: every car within 150 m (seven of seven, up to sixteen)");
  // Going from all back to near without a reset: a frame's census is closed with the NEW frame's limits, so the radius and the
  // count both apply at once.
  const FrameResult g = RunFrame(s, cars, near, kAllPhases);
  Check(CastingCars(g) == 4 && !g.cast[6][0] && !g.cast[5][0] && g.cast[3][0],
        "back to near without a reset: the new limits apply at once (the four nearest, not the 80 m car)");
}

// --- what the renderer is told -----------------------------------------------------------------------------------

void Published_() {
  Check(FreshCutoff(40.0f, 0, 1'000'000'000) == 0.0f, "nothing published: 0");
  const int64_t t0 = 5'000'000'000;
  Check(FreshCutoff(40.0f, t0, t0) == 40.0f && FreshCutoff(40.0f, t0, t0 + kStaleNs) == 40.0f, "fresh up to the stale limit");
  Check(FreshCutoff(40.0f, t0, t0 + kStaleNs + 1) == 0.0f, "stale after it: 0 (the cascades stopped or the setting went off)");
  Check(FreshCutoff(40.0f, t0, t0 - 1) == 0.0f, "a stamp from the future (a clock oddity): 0");
  // Publish stores a sane value only.
  Publish(37.5f, t0);
  Check(State().cutoff.load() == 37.5f && State().stamp_ns.load() == t0, "Publish stores the cutoff and the stamp");
  Publish(std::nanf(""), t0);
  Check(State().cutoff.load() == 0.0f, "a NaN is published as 0");
  Publish(-4.0f, t0);
  Check(State().cutoff.load() == 0.0f, "a negative cutoff is published as 0");
  // CastDistance reads the clock: published now it is fresh, published long ago it is 0.
  Publish(25.0f, NowNs());
  Check(CastDistance() == 25.0f, "published now: read back");
  Publish(25.0f, NowNs() - 10 * kStaleNs);
  Check(CastDistance() == 0.0f, "published long ago: 0");
  Publish(0.0f, 0);
}

void CostRule() {
  Check(ExtraDraws(0, 4) == 0, "no car casts: no draws");
  Check(ExtraDraws(4, 4) == 4 * 4 * kDrawsPerCarPerCascade, "four cars in four cascades");
  Check(ExtraDraws(8, 2) == 8 * 2 * kDrawsPerCarPerCascade, "eight cars in two cascades");
  Check(ExtraDraws(kNearCars, 4) < ExtraDraws(kAllCars, 4), "near costs less than all");
}

// --- the turn of the shadow (traffic_shadow_pose.h) ----------------------------------------------------------------------

// A bone: rotation about y by `degrees` (a column-vector matrix: a row is an output component, the fourth column the translation).
void MakeBone(float bone[3][4], float degrees, float tx, float ty, float tz) {
  const float a = degrees / kRadiansToDegrees;
  const float c = std::cos(a), s = std::sin(a);
  const float m[3][4] = {{c, 0.0f, s, tx}, {0.0f, 1.0f, 0.0f, ty}, {-s, 0.0f, c, tz}};
  for (int r = 0; r < 3; ++r)
    for (int k = 0; k < 4; ++k) bone[r][k] = m[r][k];
}

// The game's World for the body draw: the unit axes and the root translation in row 3 (sub_827914B8 before sub_8217D918).
void MakeGameWorld(float world[4][4], float rx, float ry, float rz) {
  const float m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {rx, ry, rz, 1}};
  for (int r = 0; r < 4; ++r)
    for (int k = 0; k < 4; ++k) world[r][k] = m[r][k];
}

// What the colour pass computes for a local point: the bone, then the root translation (the game's World).
void ColourPass(const float bone[3][4], const float game_world[4][4], const float p[3], float out[3]) {
  for (int r = 0; r < 3; ++r) {
    out[r] = bone[r][0] * p[0] + bone[r][1] * p[1] + bone[r][2] * p[2] + bone[r][3] + game_world[3][r];
  }
}

// What the shadow technique computes: the point as a row vector times the World matrix (the world part of gWorldViewProj), nothing else.
void ShadowPass(const float world[4][4], const float p[3], float out[3]) {
  for (int c = 0; c < 3; ++c) out[c] = p[0] * world[0][c] + p[1] * world[1][c] + p[2] * world[2][c] + world[3][c];
}

void ShadowTurnsWithTheCar() {
  const float points[5][3] = {{0, 0, 0}, {0, 0, 2.2f}, {1.0f, 0.4f, -2.0f}, {-0.9f, 0.5f, 1.5f}, {3.0f, 1.0f, 4.0f}};
  for (float degrees : {0.0f, 15.0f, 90.0f, 135.0f, 180.0f, -37.5f, -90.0f, 270.0f}) {
    float bone[3][4], game[4][4], used[4][4];
    MakeBone(bone, degrees, 0.0f, 0.0f, 0.0f);
    MakeGameWorld(game, 120.0f, 17.5f, -340.0f);
    Check(BodyBoneWorld(game, bone, used), Num("a bone of %.1f degrees is used", degrees));
    // The fix: the shadow's point is the colour pass's point, for every point of the model.
    float worst = 0.0f;
    float worst_game = 0.0f;
    for (const auto& p : points) {
      float want[3], got[3], old[3];
      ColourPass(bone, game, p, want);
      ShadowPass(used, p, got);
      ShadowPass(game, p, old);
      for (int i = 0; i < 3; ++i) {
        worst = std::max(worst, std::fabs(want[i] - got[i]));
        worst_game = std::max(worst_game, std::fabs(want[i] - old[i]));
      }
    }
    Check(worst < 1.0e-3f, Num("the shadow's point is the colour pass's point (worst %.5f m)", worst));
    // The bug: the game's World alone leaves the model at its bind heading, so once the car is turned the shadow is elsewhere.
    if (std::fabs(WrapDegrees(degrees)) > 1.0f) {
      Check(worst_game > 0.5f, Num("the game's own World puts the shadow off the car at %.1f degrees", degrees));
    }
    // The headings: the body's, the shadow's and the game's.
    const float body = BodyYaw(bone);
    Check(YawError(body, WrapDegrees(degrees)) < 0.01f, Num("the body's heading is the rotation (%.1f)", degrees));
    Check(YawError(body, WorldYaw(used)) < 0.01f, Num("the fixed World has the body's heading (%.1f)", degrees));
    Check(WorldYaw(game) == 0.0f, "the game's World has the model's heading (0)");
    Check((std::fabs(WrapDegrees(degrees)) < 0.01f) == (YawError(body, WorldYaw(game)) < 0.01f),
          Num("only a car at the model's heading has the game's World right (%.1f)", degrees));
  }
}

void BoneRefusals() {
  float bone[3][4], game[4][4], out[4][4];
  MakeGameWorld(game, 1, 2, 3);
  MakeBone(bone, 40.0f, 0, 0, 0);
  bone[1][2] = std::numeric_limits<float>::quiet_NaN();
  Check(!BodyBoneWorld(game, bone, out), "a NaN bone is refused");
  bool same = true;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) same = same && out[r][c] == game[r][c];
  Check(same, "a refused bone leaves the game's World");
  MakeBone(bone, 40.0f, 0, 0, 0);
  for (int r = 0; r < 3; ++r) bone[r][0] = 0.0f;
  Check(!BodyBoneWorld(game, bone, out), "an axis of no length is refused");
  MakeBone(bone, 40.0f, 0, 0, 0);
  bone[0][3] = 2.0e6f;
  Check(!BodyBoneWorld(game, bone, out), "a translation beyond a million metres is refused");
  MakeBone(bone, 40.0f, 0, 0, 0);
  for (int r = 0; r < 3; ++r) bone[r][1] *= 5000.0f;
  Check(!BodyBoneWorld(game, bone, out), "an enormous axis is refused");
  // The bone's own translation adds to the root, the w column of the game's rows stays.
  MakeBone(bone, 0.0f, 0.5f, 0.25f, -0.75f);
  MakeGameWorld(game, 10, 20, 30);
  Check(BodyBoneWorld(game, bone, out) && out[3][0] == 10.5f && out[3][1] == 20.25f && out[3][2] == 29.25f && out[3][3] == 1.0f &&
            out[0][3] == 0.0f && out[1][3] == 0.0f && out[2][3] == 0.0f,
        "the bone's translation is added to the root; the w column is the game's");
}

void YawRules() {
  Check(YawOfDirection(0, 1) == 0.0f, "along +z is 0");
  Check(std::fabs(YawOfDirection(1, 0) - 90.0f) < 1.0e-4f, "along +x is 90");
  Check(std::fabs(YawOfDirection(0, -1)) > 179.99f, "along -z is 180");
  Check(std::fabs(YawOfDirection(-1, 0) + 90.0f) < 1.0e-4f, "along -x is -90");
  Check(YawOfDirection(0, 0) == 0.0f && YawOfDirection(std::numeric_limits<float>::quiet_NaN(), 1) == 0.0f, "no direction, no heading");
  Check(WrapDegrees(190.0f) == -170.0f && WrapDegrees(-190.0f) == 170.0f && WrapDegrees(180.0f) == 180.0f && WrapDegrees(-180.0f) == 180.0f &&
            WrapDegrees(720.0f) == 0.0f,
        "angles wrap into (-180, 180]");
  Check(YawError(179.0f, -179.0f) == 2.0f && YawError(10.0f, 10.0f) == 0.0f, "the error is the short way round");
  Check(std::fabs(TurnRate(10.0f, 0.0f, 0.1) - 100.0f) < 0.01f, "10 degrees in 0.1 s is 100 a second");
  Check(std::fabs(TurnRate(-179.0f, 179.0f, 0.5) - 4.0f) < 0.01f, "a turn across 180 degrees takes the short way (2 degrees in 0.5 s)");
  Check(TurnRate(10.0f, 0.0f, 0.0) == 0.0f && TurnRate(10.0f, 0.0f, 2.0) == 0.0f, "no rate for no time or for a gap over half a second");

  YawHistory history;
  Check(history.Note(0x1000, 10.0f, 1'000'000'000) == 0.0f, "the first reading has no rate");
  const float rate = history.Note(0x1000, 14.0f, 1'100'000'000);
  Check(std::fabs(rate - 40.0f) < 0.01f, Num("4 degrees in 0.1 s is 40 a second (%.3f)", rate));
  Check(history.Note(0x2000, 50.0f, 1'100'000'000) == 0.0f, "another car starts its own history");
  Check(history.Note(0x1000, 18.0f, 3'000'000'000) == 0.0f, "a car not seen for 1.9 s starts again");
  // A full table replaces in turn and keeps answering.
  for (uint32_t i = 0; i < 2 * YawHistory::kCapacity; ++i) history.Note(0x3000 + i, float(i), 5'000'000'000);
  const uint32_t last = 0x3000 + 2 * YawHistory::kCapacity - 1;
  Check(std::fabs(history.Note(last, float(2 * YawHistory::kCapacity - 1) + 1.0f, 5'100'000'000) - 10.0f) < 0.01f,
        "a full table still gives the rate of the car it kept");
  history.Reset();
  Check(history.Note(0x1000, 1.0f, 9'000'000'000) == 0.0f, "reset forgets the cars");
  Check(kYawToleranceDegrees == 2.0f, "the judged tolerance");
}

}  // namespace

int main() {
  SettingAndClasses();
  Phases();
  Reach();
  LimitsOfTheModes();
  Candidates();
  VehicleRejects();
  AmbientClientMask();
  AmbientItems();
  ViewportsAndEyes();
  DepthCheck();
  Bands();
  AmbientPosition();
  MixedCensus();
  ValueCountsAndGates();
  WindowAndCensusLines();
  LogScheduleRules();
  Cutoff();
  CensusOverFrames();
  CascadeLimit();
  SinglePhaseFrames();
  NonCascadePhases();
  ResetAndNearAllSwitch();
  Published_();
  CostRule();
  ShadowTurnsWithTheCar();
  BoneRefusals();
  YawRules();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
