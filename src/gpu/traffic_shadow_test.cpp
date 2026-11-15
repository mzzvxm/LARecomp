// Check of the traffic cars' sun shadow rules (traffic_shadow.h): the setting's words and the `auto` cell per GPU
// class, the cascade phases, the limits of each mode, which vehicles are candidates, the cutoff that picks the nearest
// cars, the census over a frame's phases, and the cutoff the renderer is told. A program of its own: it needs no renderer,
// no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_traffic_shadow_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_traffic_shadow_test
// or by hand: clang++ -std=c++23 -I src/gpu src/gpu/traffic_shadow_test.cpp -o traffic_shadow_test

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "traffic_shadow.h"
#include "traffic_shadow_gates.h"

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
  // auto: near on Mid and High, off on Low (the owner's rule); the player's own choice is never changed.
  using gpu_class::GpuClass;
  Check(ModeFor(GpuClass::kLow, "auto") == Mode::kOff, "auto on Low: off");
  Check(ModeFor(GpuClass::kMid, "auto") == Mode::kNear, "auto on Mid: near");
  Check(ModeFor(GpuClass::kHigh, "auto") == Mode::kNear, "auto on High: near");
  for (GpuClass c : {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh}) {
    Check(ModeFor(c, "off") == Mode::kOff, "off stays off on every class");
    Check(ModeFor(c, "near") == Mode::kNear, "near stays near on every class (a Low-class player may ask for it)");
    Check(ModeFor(c, "all") == Mode::kAll, "all stays all on every class");
  }
  // The row's label for auto.
  Check(gpu_class::AutoLabelFor(GpuClass::kLow, gpu_class::Effect::kTrafficShadows) == "AUTO (OFF)", "the row says AUTO (OFF) on Low");
  Check(gpu_class::AutoLabelFor(GpuClass::kHigh, gpu_class::Effect::kTrafficShadows) == "AUTO (NEAR)", "the row says AUTO (NEAR) on High");
  Check(gpu_class::CvarName(gpu_class::Effect::kTrafficShadows) == "mcla_gpu_traffic_shadows", "the cvar's name");
}

void Phases() {
  Check(CascadeIndex(0x20) == 1 && CascadeIndex(0x40) == 2 && CascadeIndex(0x80) == 3 && CascadeIndex(0x100) == 4, "the four cascades in order");
  for (uint32_t p : {0u, 0x1u, 0x2u, 0x10u, 0x200u, 0x400u, 0x1000u, 0x2000u, 0x4000u, 0x10000u, 0x1000000u, 0x60u}) {
    Check(CascadeIndex(p) == 0, "phase " + Num("%.0f", double(p)) + " is not a cascade (the collector 0x1000 and the night phase 0x400 included)");
  }
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
  Check(kAmbientClientVtable == 0x820D8C0Cu && kAmbientBodyPhase == 0x8000u, "the guest constants of the traffic client");
  Check(CascadeIndex(kAmbientBodyPhase) == 0, "the body phase the draw is told is no cascade (the hook is not re-entered on it)");
}

AmbientFlags GoodItem() {
  AmbientFlags f;
  f.enabled = 1;
  f.car_flags = 0x40;
  f.model_count = 3;
  f.lod = 0;
  f.item_flags = 0x80;
  f.visible = 4;
  return f;
}

void AmbientItems() {
  AmbientFlags f = GoodItem();
  Check(AmbientRejectBits(f) == 0 && AmbientEligible(f), "a traffic car the draw would draw is eligible");
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
  f.visible = 0;
  Check(AmbientRejectBits(f) == kAmbientRejectNotVisible, "not seen by the main view");
  f.visible = 0xFB;
  Check(AmbientRejectBits(f) == kAmbientRejectNotVisible, "only bit 4 of the visible byte counts");
  f.visible = 5;
  Check(AmbientRejectBits(f) == 0, "bit 4 with others set: seen");
  f = GoodItem();
  f.lod = -1;
  Check(AmbientRejectBits(f) == kAmbientRejectLod, "a negative LOD is not drawn");
  f.item_flags = 0xA0;
  Check(AmbientRejectBits(f) == 0, "item +556 bit 0x20 skips the LOD test, as the draw does");
  f = GoodItem();
  f.item_flags = 0x00;
  Check(AmbientRejectBits(f) == kAmbientRejectNoBody, "no 0x80 in item +556: nothing to draw");
  f = GoodItem();
  f.enabled = 0;
  f.visible = 0;
  f.lod = -2;
  Check(AmbientRejectBits(f) == (kAmbientRejectEnabled | kAmbientRejectNotVisible | kAmbientRejectLod), "several tests: several bits");
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
  for (const char* part : {"mode off", "no memory", "vehicle ptr", "ineligible", "+593 clear", "+6478 set", "+6467 set", "+123 set", "+116 zero", "+120 zero",
                           "entity ptr", "camera ptr", "distance not finite", "reached Decide", "phase loop", "mask widened", "item ptr", "car ptr", "buffer",
                           "+36 zero", "car +500", "models<3", "not visible", "LOD<0", "no body", "position"}) {
    Check(line.find(part) != std::string::npos, std::string("the line names `") + part + "`");
  }
}

void LogScheduleRules() {
  LogSchedule schedule;
  const int64_t s = 1'000'000'000;
  Check(!schedule.Due(0, 0) && !schedule.Due(1999, 5 * s), "no line before 2000 calls, however late");
  Check(schedule.Due(2000, 6 * s), "the first line at the 2000th call");
  Check(!schedule.Due(2001, 6 * s + 1) && !schedule.Due(50000, 20 * s), "none within 30 s of it");
  Check(schedule.Due(50001, 36 * s), "the next one 30 s after the first");
  Check(!schedule.Due(60000, 40 * s) && !schedule.Due(60001, 65 * s), "and none until 30 s after that (66 s)");
  Check(schedule.Due(60002, 66 * s), "the third at +30 s");
  LogSchedule late;
  Check(late.Due(9000, 100 * s) && !late.Due(9001, 101 * s), "the first line is not waiting for a time, only for the calls");
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

}  // namespace

int main() {
  SettingAndClasses();
  Phases();
  LimitsOfTheModes();
  Candidates();
  VehicleRejects();
  AmbientClientMask();
  AmbientItems();
  AmbientPosition();
  MixedCensus();
  ValueCountsAndGates();
  LogScheduleRules();
  Cutoff();
  CensusOverFrames();
  CascadeLimit();
  SinglePhaseFrames();
  NonCascadePhases();
  ResetAndNearAllSwitch();
  Published_();
  CostRule();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
