#pragma once
// ===========================================================================
// MCLA GPU renderer -- the probe field's owner: the rules that need no renderer (package G1 of perf/CITY-RT-M3.md, sections 3.1, 3.3, 6, 7)
// ===========================================================================
// gi_frame.h (package G0) FIXES what the field is: the cascades' table, the atlases' layout, the toroidal addressing, the lookup. This file is what the field's OWNER (gi_field.h,
// the Plume side) decides with those, as plain functions and small classes with no device, so that gi_field_rules_test.cpp checks them alone:
//
//   THE MEMORY OF A CELL       MemoryOf(budget): the bytes the three textures and the state buffer take (what G1 allocates), what G2's ray buffers and G3's / G5's images are
//                              estimated to add (what they will allocate: the design's totals, M3 section 6), per (ray tracing tier, level) cell (Cells()).
//   THE ORDER OF LOSS          ChooseFit: when video memory is short the field steps down, per-pixel images first, then the fourth cascade, then the third (M3 section 6: "the
//                              effect never fails the renderer"; M2's texture cap sits between the fourth and the third cascade and is not this file's). A step is asked of the
//                              caller's room test (vram_room.h HasRoom), and a refused step waits before it is asked again (vram_room::RefusalBook).
//   TOROIDAL ADDRESSING        FieldState keeps each cascade's window (the lattice coordinate of its minimum corner) and moves it with the camera in whole cells: Move() returns the
//                              boxes of coordinates that entered a window (their slots held the data of coordinates that left it: they are CLEARED on the GPU, ResetList) and marks
//                              them DIRTY. A step over kCutMetres, or a request (the Director's hour, the garage exit, a race start), is a cut: every probe is cleared and dirty,
//                              the fade starts again from 0 and the next kCutWarmupFrames frames carry kCutWarmupBudgetScale times the probe budget (BudgetScale).
//   DIRTY MARKING              the CPU keeps one bit a probe (DirtySet): the probes whose data is new or stale and that the update (G2) traces FIRST, without history. G2's scheduler
//                              takes them with TakeDirty (nearest the camera first) and marks the world boxes that changed (MarkWorldBox: a sector that was just built). The
//                              GPU's state word (below) has a NEW bit as well, for the debug view; the CPU's bit is the scheduler's.
//   THE VALIDITY MAPS          the state word a probe has in the state buffer (one uint a probe, all cascades in one RWByteAddressBuffer, StateIndex), and the rule that turns it
//                              into the weight map's byte (ProbeWeight): inside geometry or outside the safe range or never traced = 0, then a ramp over the first updates. G2 writes
//                              the state (updates, the inside flag, the back face share: GifStateAfterUpdate); G1's weights pass (hlsl/gi_field_weights_cs.hlsl) writes the weight map
//                              from it, once a frame after G2 (the reset writes 0 to the probes it clears, which is what the pass would make of them).
//   THE FADE                   FadeState: the field's weight in the picture (GiFrame::fade): 0 while the field is not ready (a cut, a new layout, most of the finest cascade still
//                              dirty), rising over kFadeInSeconds once it is, falling over kFadeOutSeconds when it is not usable (a crossfade with the screen-space bounce light).
//
// THE FRAME'S ORDER (gi_field.h): BeginFrame (CPU: the fit, the textures, Move, the fade, the GiFrame) -> RecordMaintenance (the reset of the exposed probes on the GPU) -> G2's trace and
// blend (the textures and the state buffer are in the GENERAL layout) -> RecordFinish (the weights pass, the textures to the readers' layout, the frame is published).
//
// Everything here is plain data and pure functions (render thread only where a class has state). The header includes nothing of Plume; gi_frame.h is the frozen interface and is not
// edited. Numbers marked `G1a` are first guesses (values, changeable without a version); `COST PASS` as in gi_frame.h.
// ===========================================================================

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "gi_frame.h"
#include "vram_room.h"

namespace mcla::gpu::gi_field {

inline constexpr uint32_t kFormatVersion = 1;  // the state word, the reset list and the weight rule (GIF_FORMAT_VERSION in hlsl/gi_field_common.hlsli)
inline constexpr uint64_t kMiB = 1024ull * 1024ull;

// ---------------------------------------------------------------------------
// the state word of a probe (the state buffer: one uint a probe)
// ---------------------------------------------------------------------------
//   bits  0..7   updates   how many traces were blended into the probe since it was cleared (saturating at 255): 0 = never traced (weight 0)
//   bit   8      inside    G2's classification: the probe is inside geometry (switched off: no update, weight 0)
//   bit   9      new       set by the reset (the probe was cleared), cleared by G2's blend: the first trace has no history. The CPU's DirtySet is the scheduler's own record
//   bits 10..17  back      G2: the share of the last update's rays that hit a back face, 0..255 = 0..100% (the validity debug view shows it; the 30% rule is G2's)
//   bits 18..31  reserved  G2's (the frame of the last update, the nearest hit): the reset writes 0 there

inline constexpr uint32_t kStateUpdatesMask = 0xFFu;
inline constexpr uint32_t kStateUpdatesMax = 255u;
inline constexpr uint32_t kStateInside = 1u << 8;
inline constexpr uint32_t kStateNew = 1u << 9;
inline constexpr uint32_t kStateBackShift = 10;
inline constexpr uint32_t kStateBackMask = 0xFFu << kStateBackShift;
inline constexpr uint32_t kStateAfterReset = kStateNew;  // what the reset writes: never traced, new, not inside

constexpr uint32_t StateUpdates(uint32_t state) { return state & kStateUpdatesMask; }
constexpr bool StateInside(uint32_t state) { return (state & kStateInside) != 0u; }
constexpr bool StateNew(uint32_t state) { return (state & kStateNew) != 0u; }
constexpr uint32_t StateBack(uint32_t state) { return (state & kStateBackMask) >> kStateBackShift; }
constexpr uint32_t MakeState(uint32_t updates, bool inside, bool is_new, uint32_t back = 0) {
  return (updates > kStateUpdatesMax ? kStateUpdatesMax : updates) | (inside ? kStateInside : 0u) | (is_new ? kStateNew : 0u) | ((back > 255u ? 255u : back) << kStateBackShift);
}
// One more update blended: the count rises (saturating), the new bit goes.
constexpr uint32_t StateAfterUpdate(uint32_t state) {
  const uint32_t updates = StateUpdates(state);
  return (state & ~(kStateUpdatesMask | kStateNew)) | (updates < kStateUpdatesMax ? updates + 1u : kStateUpdatesMax);
}

// The weight map's rule (gi_frame.h: 0 inside geometry, never traced, outside the safe range, to 1 converged): the first trace of a probe has 64 rays and no history, so it is not
// trusted at once: half after the first update, whole after the second. G1a: the first runs (the canyon, the leak scene) decide; it is a value.
inline constexpr float kWeightRampUpdates = 2.0f;

constexpr float ProbeWeight(uint32_t state, bool in_range) {
  if (!in_range || StateInside(state)) return 0.0f;
  const float w = float(StateUpdates(state)) / kWeightRampUpdates;
  return w < 0.0f ? 0.0f : w > 1.0f ? 1.0f : w;
}
// The probe is inside the range the structures stand in (M1's SafeRange from the camera): a probe farther than this has weight 0, a safe range of 0 is no field. Float arithmetic as the
// weights pass does it.
inline bool ProbeInRange(const gi::Vec3& probe, const gi::Vec3& camera, float safe_range_m) {
  if (!(safe_range_m > 0.0f)) return false;
  const gi::Vec3 d = probe - camera;
  return std::sqrt(gi::Dot(d, d)) <= safe_range_m;
}
// The byte the R8_UNORM weight map stores for w (the GPU's conversion: round to nearest).
constexpr uint32_t WeightByte(float w) { return uint32_t(w * 255.0f + 0.5f); }

// ---------------------------------------------------------------------------
// the slot of a probe and the probe of a slot
// ---------------------------------------------------------------------------

// The inverse of gi::ProbeIndex.
constexpr gi::Slot SlotOfIndex(uint32_t index, const gi::Dims& d) { return {index % d.x, (index / d.x) % d.y, index / (d.x * d.y)}; }
// The one coordinate of the window [origin, origin + dims) that is stored at `slot` (toroidal: slot = c mod dims).
constexpr gi::Coord CoordOfSlot(const gi::Coord& origin, const gi::Dims& d, const gi::Slot& slot) {
  return {origin.x + int32_t(gi::Wrap(int32_t(slot.x) - origin.x, d.x)), origin.y + int32_t(gi::Wrap(int32_t(slot.y) - origin.y, d.y)),
          origin.z + int32_t(gi::Wrap(int32_t(slot.z) - origin.z, d.z))};
}
constexpr gi::Coord CoordOfIndex(const gi::Coord& origin, const gi::Dims& d, uint32_t index) { return CoordOfSlot(origin, d, SlotOfIndex(index, d)); }
constexpr uint32_t IndexOfCoord(const gi::Coord& c, const gi::Dims& d) { return gi::ProbeIndex(gi::SlotOf(c, d), d); }

// ---------------------------------------------------------------------------
// where each cascade's probes are in the state buffer
// ---------------------------------------------------------------------------

struct ProbeTable {
  uint32_t cascades = 0;
  std::array<uint32_t, gi::kMaxCascades + 1> base = {};  // base[c] = the first probe of cascade c; base[cascades] = the total
  constexpr uint32_t Total() const { return base[cascades]; }
};

constexpr ProbeTable TableOf(const gi::Budget& b) {
  ProbeTable t;
  t.cascades = b.cascade_count;
  uint32_t total = 0;
  for (uint32_t c = 0; c < b.cascade_count; ++c) {
    t.base[c] = total;
    total += b.cascade[c].dims.Probes();
  }
  for (uint32_t c = b.cascade_count; c <= gi::kMaxCascades; ++c) t.base[c] = total;
  return t;
}
// The word of probe `probe` (gi::ProbeIndex order) of cascade `cascade` in the state buffer.
constexpr uint32_t StateIndex(const ProbeTable& t, uint32_t cascade, uint32_t probe) { return t.base[cascade] + probe; }
constexpr uint64_t StateBytes(const ProbeTable& t) { return uint64_t(t.Total()) * 4u; }

// ---------------------------------------------------------------------------
// the memory of a cell
// ---------------------------------------------------------------------------

// What G2 and G3 / G5 will allocate on top of what this owner does, from M3 section 6's totals (every number COST PASS / G1a): the trace results of a frame's rays (the radiance, the
// distance: 16 bytes a ray) and the apply's images (the traced size's irradiance and its upsample). The per-pixel pass's images (G5: the denoiser's history and moments) are what is
// left of the design's total for the extended layout.
inline constexpr uint64_t kRayRecordBytes = 16;
constexpr uint64_t ApplyReserveBytes(gi::Layout layout) {
  switch (layout) {
    case gi::Layout::kLean: return 1 * kMiB;
    case gi::Layout::kStandard: return 11 * kMiB;
    case gi::Layout::kExtended: return 11 * kMiB;
    case gi::Layout::kNone: break;
  }
  return 0;
}

struct Memory {
  uint64_t irradiance = 0;      // the RGBA16F atlas
  uint64_t moments = 0;         // the RG16F atlas
  uint64_t weight = 0;          // the R8 weight map
  uint64_t state = 0;           // the state buffer
  uint64_t update_reserve = 0;  // G2's ray buffers (an estimate: rays a frame x kRayRecordBytes)
  uint64_t apply_reserve = 0;   // G3's images (an estimate)
  uint64_t pixel_reserve = 0;   // G5's per-pixel images (an estimate; the extended layout with its pixel images only)
  constexpr uint64_t Field() const { return irradiance + moments + weight + state; }  // what this owner allocates
  constexpr uint64_t Total() const { return Field() + update_reserve + apply_reserve + pixel_reserve; }
};

// The memory of a budget's field (cascades as the budget has them: a derived budget with fewer cascades has less). `pixel_images`: the per-pixel pass's images are counted (only the extended
// layout has any: the rest of the design's total).
constexpr Memory MemoryOf(const gi::Budget& b, bool pixel_images) {
  Memory m;
  if (b.layout == gi::Layout::kNone || b.cascade_count == 0) return m;
  const uint64_t atlas = uint64_t(gi::AtlasWidth(b)) * gi::AtlasHeight(b);
  m.irradiance = atlas * gi::TexelBytes(gi::kIrradianceFormat);
  m.moments = atlas * gi::TexelBytes(gi::kMomentsFormat);
  m.weight = uint64_t(gi::WeightWidth(b)) * gi::WeightHeight(b) * gi::TexelBytes(gi::kWeightFormat);
  m.state = StateBytes(TableOf(b));
  m.update_reserve = uint64_t(gi::BudgetRaysPerFrame(b)) * kRayRecordBytes;
  m.apply_reserve = ApplyReserveBytes(b.layout);
  if (pixel_images && b.layout == gi::Layout::kExtended) {
    const uint64_t design = uint64_t(b.cost.vram_mib) * kMiB;
    const uint64_t others = m.Field() + m.update_reserve + m.apply_reserve;
    m.pixel_reserve = design > others ? design - others : 0;
  }
  return m;
}

// The cells of the (ray tracing tier, level) table: what each runs and what it holds, for the report and the census.
struct Cell {
  gi::Rung rung = gi::Rung::kNone;
  gi::Level level = gi::Level::kOff;
  gi::Layout layout = gi::Layout::kNone;
  Memory memory;
};
constexpr std::array<Cell, 9> Cells() {
  std::array<Cell, 9> out = {};
  uint32_t i = 0;
  for (const gi::Rung rung : {gi::Rung::kNone, gi::Rung::kWeak, gi::Rung::kStrong}) {
    for (const gi::Level level : {gi::Level::kOff, gi::Level::kHigh, gi::Level::kUltra}) {
      const gi::Layout layout = gi::LayoutFor(rung, level);
      out[i++] = {rung, level, layout, MemoryOf(gi::BudgetOf(layout), gi::PixelPassAllowed(rung, layout))};
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// the order of loss
// ---------------------------------------------------------------------------

enum class Step : uint8_t {
  kFull,             // the layout as the cell says
  kNoPixelImages,    // the extended layout without the per-pixel pass's images (the pass is off)
  kNoFourthCascade,  // the extended layout's first three cascades
  kNoThirdCascade,   // the first two cascades (the standard layout, or the extended one at its worst)
  kNone,             // no field: the screen-space bounce light stays the source
};
constexpr const char* StepName(Step step) {
  switch (step) {
    case Step::kFull: return "full";
    case Step::kNoPixelImages: return "without the per-pixel images";
    case Step::kNoFourthCascade: return "without the fourth cascade";
    case Step::kNoThirdCascade: return "without the third cascade";
    case Step::kNone: return "none";
  }
  return "?";
}

// The next step down for a layout (the steps that do not apply to it are skipped): extended: full, no pixel images, no fourth cascade, no third, none; standard: full, no third cascade,
// none; lean: full, none. kNone stays.
constexpr Step NextStep(gi::Layout layout, Step step) {
  switch (step) {
    case Step::kFull: return layout == gi::Layout::kExtended ? Step::kNoPixelImages : layout == gi::Layout::kStandard ? Step::kNoThirdCascade : Step::kNone;
    case Step::kNoPixelImages: return Step::kNoFourthCascade;
    case Step::kNoFourthCascade: return Step::kNoThirdCascade;
    case Step::kNoThirdCascade: return Step::kNone;
    case Step::kNone: break;
  }
  return Step::kNone;
}
// How many cascades a step keeps of a layout's budget.
constexpr uint32_t CascadesAt(const gi::Budget& b, Step step) {
  switch (step) {
    case Step::kFull:
    case Step::kNoPixelImages: return b.cascade_count;
    case Step::kNoFourthCascade: return std::min(b.cascade_count, 3u);
    case Step::kNoThirdCascade: return std::min(b.cascade_count, 2u);
    case Step::kNone: break;
  }
  return 0;
}

// A budget with its first `n` cascades (the atlas rows stack, so dropping the last ones only shortens the atlases).
constexpr gi::Budget WithCascades(const gi::Budget& in, uint32_t n) {
  gi::Budget b = in;
  b.cascade_count = std::min(n, in.cascade_count);
  for (uint32_t c = b.cascade_count; c < gi::kMaxCascades; ++c) b.cascade[c] = gi::CascadeSpec{};
  if (b.cascade_count == 0) b.layout = gi::Layout::kNone;
  return b;
}

struct Fit {
  gi::Layout wanted = gi::Layout::kNone;  // what the cell asked for
  Step step = Step::kNone;                // what is made
  gi::Budget budget;                      // the cascades that are made (budget.layout = wanted; cascade_count 0 for none)
  bool pixel_images = false;              // G5 may make its images
  Memory memory;
  constexpr bool On() const { return step != Step::kNone && budget.cascade_count > 0; }
  // The field is smaller than the cell asked for.
  constexpr bool Reduced() const { return wanted != gi::Layout::kNone && step != Step::kFull; }
  constexpr bool operator==(const Fit& o) const { return wanted == o.wanted && step == o.step; }
};

constexpr Fit FitAt(gi::Layout wanted, Step step) {
  Fit f;
  f.wanted = wanted;
  f.step = wanted == gi::Layout::kNone ? Step::kNone : step;
  const gi::Budget& full = gi::BudgetOf(wanted);
  f.budget = f.step == Step::kNone ? gi::Budget{} : WithCascades(full, CascadesAt(full, f.step));
  if (f.step != Step::kNone) f.budget.layout = wanted;
  f.pixel_images = f.step == Step::kFull && gi::PixelPassAllowed(gi::Rung::kStrong, wanted);
  f.memory = MemoryOf(f.budget, f.pixel_images);
  return f;
}

constexpr Fit RequestedFit(gi::Layout layout) {
  return FitAt(layout, layout == gi::Layout::kNone ? Step::kNone : Step::kFull);
}

constexpr bool FixedFitPressure(const Fit& fit, uint64_t held, uint64_t used, uint64_t budget) {
  const uint64_t total = fit.memory.Total();
  return !vram_room::HasRoom(used, budget, total > held ? total - held : 0u);
}

// The first step whose total `fits(bytes)` (the room test the caller supplies: vram_room.h HasRoom against the budget, with what the field already holds taken off), from the full
// layout down; none when nothing fits. `skip(step)` (optional: a step that is waiting out a refusal) is passed over.
template <class FitsFn, class SkipFn>
constexpr Fit ChooseFit(gi::Layout wanted, FitsFn&& fits, SkipFn&& skip) {
  if (wanted == gi::Layout::kNone) return FitAt(gi::Layout::kNone, Step::kNone);
  Step step = Step::kFull;
  while (step != Step::kNone) {
    const Fit f = FitAt(wanted, step);
    if (!skip(step) && fits(f.memory.Total())) return f;
    step = NextStep(wanted, step);
  }
  return FitAt(wanted, Step::kNone);
}
template <class FitsFn>
constexpr Fit ChooseFit(gi::Layout wanted, FitsFn&& fits) {
  return ChooseFit(wanted, fits, [](Step) { return false; });
}

// ---------------------------------------------------------------------------
// the dirty set
// ---------------------------------------------------------------------------

// One bit a probe of a cascade (gi::ProbeIndex order): the probes whose data is new or stale.
class DirtySet {
 public:
  void Resize(uint32_t probes) {
    size_ = probes;
    count_ = 0;
    bits_.assign((size_t(probes) + 63) / 64, 0ull);
  }
  uint32_t Size() const { return size_; }
  uint32_t Count() const { return count_; }
  bool Test(uint32_t i) const { return i < size_ && ((bits_[i >> 6] >> (i & 63)) & 1ull) != 0; }
  // True when the probe was not dirty before.
  bool Mark(uint32_t i) {
    if (i >= size_ || Test(i)) return false;
    bits_[i >> 6] |= 1ull << (i & 63);
    ++count_;
    return true;
  }
  // True when the probe was dirty.
  bool Clear(uint32_t i) {
    if (!Test(i)) return false;
    bits_[i >> 6] &= ~(1ull << (i & 63));
    --count_;
    return true;
  }
  void MarkAll() {
    for (uint32_t i = 0; i < size_; ++i) Mark(i);
  }
  void ClearAll() {
    std::fill(bits_.begin(), bits_.end(), 0ull);
    count_ = 0;
  }
  // The dirty probes' indices in increasing order, for iteration.
  template <class Fn>
  void ForEach(Fn&& fn) const {
    for (size_t w = 0; w < bits_.size(); ++w) {
      uint64_t bits = bits_[w];
      while (bits) {
        const uint32_t b = uint32_t(std::countr_zero(bits));
        bits &= bits - 1;
        fn(uint32_t(w * 64 + b));
      }
    }
  }

 private:
  std::vector<uint64_t> bits_;
  uint32_t size_ = 0;
  uint32_t count_ = 0;
};

// ---------------------------------------------------------------------------
// the reset list: what the GPU clears when a window moves (hlsl/gi_field_reset_cs.hlsl; hlsl/gi_field_common.hlsli GifResetProbe is the twin)
// ---------------------------------------------------------------------------
// The coordinates that entered a window hold, in their slots, what the coordinates that left it had: the reset writes the NEW probe's state (kStateAfterReset: never traced), its
// weight (0), its irradiance tile (0: no light, no sky) and its moments tile (the cascade's maximum distance: open), so that nothing of the old probe is read before it is traced.
// A list is up to kResetListMaxEntries boxes (3 a cascade: ExposedBoxes), each box [lo, lo + extent) of lattice coordinates of one cascade; the GPU dispatches one 10 x 10 group for
// each probe of all the boxes in order (the group's number g: the entry whose `first` <= g < first + the entry's probes).

inline constexpr uint32_t kResetListMaxEntries = gi::kMaxCascades * 3;
inline constexpr uint32_t kResetEntryWords = 8;   // cascade, lo x y z, extent x y z, first
inline constexpr uint32_t kResetHeaderWords = 4;  // entry count, total probes, 0, 0
inline constexpr uint32_t kResetListWords = kResetHeaderWords + kResetListMaxEntries * kResetEntryWords;
inline constexpr uint32_t kResetListBytes = kResetListWords * 4;
inline constexpr uint32_t kResetGroupsX = 256;  // the dispatch is (kResetGroupsX, ceil(total / kResetGroupsX), 1): g = y * kResetGroupsX + x

struct Exposure {
  uint32_t cascade = 0;
  gi::Box box;
};

struct ResetList {
  std::array<uint32_t, kResetListWords> words = {};
  uint32_t entries = 0;
  uint32_t total = 0;  // the probes of all the entries (the groups the GPU needs)
  uint32_t GroupsY() const { return (total + kResetGroupsX - 1) / kResetGroupsX; }
};

// More than kResetListMaxEntries are dropped (the callers cannot make more: three boxes a cascade).
inline ResetList MakeResetList(const Exposure* exposures, uint32_t count) {
  ResetList list;
  uint32_t first = 0;
  for (uint32_t i = 0; i < count && list.entries < kResetListMaxEntries; ++i) {
    const gi::Box& b = exposures[i].box;
    if (b.Empty()) continue;
    uint32_t* w = &list.words[kResetHeaderWords + list.entries * kResetEntryWords];
    w[0] = exposures[i].cascade;
    w[1] = uint32_t(b.lo.x);
    w[2] = uint32_t(b.lo.y);
    w[3] = uint32_t(b.lo.z);
    w[4] = uint32_t(b.hi.x - b.lo.x);
    w[5] = uint32_t(b.hi.y - b.lo.y);
    w[6] = uint32_t(b.hi.z - b.lo.z);
    w[7] = first;
    first += uint32_t(b.Count());
    ++list.entries;
  }
  list.total = first;
  list.words[0] = list.entries;
  list.words[1] = list.total;
  return list;
}

// Which probe group g of a list is (the C++ twin of GifResetProbe).
struct ResetProbe {
  bool valid = false;
  uint32_t cascade = 0;
  gi::Coord coord;
};
inline ResetProbe ResetProbeOf(const ResetList& list, uint32_t g) {
  ResetProbe out;
  if (g >= list.total) return out;
  for (uint32_t e = 0; e < list.entries; ++e) {
    const uint32_t* w = &list.words[kResetHeaderWords + e * kResetEntryWords];
    if (g >= w[7] && g - w[7] < w[4] * w[5] * w[6]) {
      const uint32_t local = g - w[7];
      out.valid = true;
      out.cascade = w[0];
      out.coord = {int32_t(w[1]) + int32_t(local % w[4]), int32_t(w[2]) + int32_t((local / w[4]) % w[5]), int32_t(w[3]) + int32_t(local / (w[4] * w[5]))};
      return out;
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// the field's CPU state: windows, dirty sets, cuts
// ---------------------------------------------------------------------------

// A cut the callers ask for: a teleport (the garage exit, a race start, the Director's camera) clears everything because the camera is somewhere else; a relight (the hour jumped: the Director's
// hour, a mission start's, a skip) clears everything because the light is, the camera staying where it is. Both are cuts for the data (every probe cleared and dirty, the fade from 0, the
// warm-up budget, the pixel pass's history reset); the difference is the report's flag (cut or relight: the log says "hour jump" for the second). The relight does NOT keep the old data: a
// probe that kept it would blend the first new trace in with the jump hysteresis (gi::kHysteresisJump, 0.85: 85% of the old hour's light left after it), and the traces after it
// only at 0.85 while the probe's mean luminance has moved by more than the update's jump test (25% and four standard errors: a day to night jump does, 17:00 to 17:40 does not), else at 0.97 (about 33
// traces). At High the warm-up traces the finest cascade every frame (12 traces: 14% of the old light left) but the coarser two only 6 and 3 times (38% and 61%), then once in 8 and 16 frames:
// seconds of the old hour's light in the far field. A cleared probe has no history: its first trace is the new light.
enum class CutKind : uint8_t { kNone, kTeleport, kRelight };

// What the integration hands the field for a frame (gi_integration.cpp BeginFrame): a cut the GAME asked for (cut_request.h: the trailer toolkit's camera cuts on purpose, the first frame of a shot) is a
// teleport, so every probe and the pixel pass's history start again even where the camera's step is under gi::kCutMetres (a shot that starts 50 m away, or a turn only); no request is no cut.
// An hour jump the clock and the light both saw (RelightDetector) is a relight, and a teleport asked in the same frame wins (it clears the same and moves the windows).
constexpr CutKind CutFor(bool game_cut, bool hour_jump = false) { return game_cut ? CutKind::kTeleport : hour_jump ? CutKind::kRelight : CutKind::kNone; }

// ---------------------------------------------------------------------------
// the hour jump (CITY-RT-REVIEW M4)
// ---------------------------------------------------------------------------
// When the time of day jumps (the Director's hour, a mission start that sets it, a skip, the settings' hour slider) every probe holds the light of the old hour. The signal needs two
// witnesses, because each alone fails in a way that cannot be told from here:
//   THE CLOCK  world_clock.h HourJumps(): the hook that runs the lighting update sees the hour of every frame's start, and a step over world_clock::kHourJumpHours (0.5 h, above the largest step
//              normal play makes in a frame) is a jump, whoever set the hour (the game thread: it may run a few frames ahead of the render thread, so the clock can name the jump before the draws
//              carry the new light, or after).
//   THE LIGHT  the sun's world direction of the draws (GiIntegration::LatchPoseAndLight, PS c16): the light the probes are traced with is the draws', and a relight that came before it
//              would spend the warm-up's 12 frames of 4 x the budget on the old hour. A step of the direction over kRelightSunStepCos between two triggers is a step of the light. Alone it
//              would also fire on a light set that changes with no hour behind it (a tunnel, the garage).
// The two must come within kRelightWindowFrames frames of each other, in either order; a jump is spent when the relight fires, so one jump fires once (a second step of the sun in the
// window has no clock jump to pair with). The largest step of the sun in a frame of normal play is 0.104 h * 15 degrees = 1.6 degrees (world_clock.h); kRelightSunStepCos is cos(3 degrees),
// a step that the clock's own test has already called a jump needs only to be seen in the light, not to be large. A lightning flash changes the sun's colour, not its direction: it does
// not count. An hour jump that does not move the sun by 3 degrees (a few minutes of the day, or a night that the sky keeps still) leaves the light where it was: nothing to relight.
inline constexpr uint32_t kRelightWindowFrames = 30;
inline constexpr double kRelightSunStepCos = 0.9986295347545738;  // cos(3 degrees)

class RelightDetector {
 public:
  // Once a frame, from the play view's trigger: `frame` (the scene's serial, rising), `clock_jumps` (world_clock::HourJumps()), the sun's direction toward the sun (any length; `have_sun`
  // false when the draws gave none). True once for each jump that both witnesses saw.
  bool Observe(uint64_t frame, uint32_t clock_jumps, bool have_sun, const double to_sun[3]) {
    if (!have_clock_) {
      have_clock_ = true;
      seen_jumps_ = clock_jumps;  // the jumps before the first look are not this field's
    } else if (clock_jumps != seen_jumps_) {
      seen_jumps_ = clock_jumps;
      jump_frame_ = frame;
      have_jump_ = true;
    }
    if (have_sun) {
      const double length = std::sqrt(to_sun[0] * to_sun[0] + to_sun[1] * to_sun[1] + to_sun[2] * to_sun[2]);
      if (std::isfinite(length) && length > 1.0e-6) {
        const double n[3] = {to_sun[0] / length, to_sun[1] / length, to_sun[2] / length};
        if (have_sun_ && n[0] * sun_[0] + n[1] * sun_[1] + n[2] * sun_[2] < kRelightSunStepCos) {
          step_frame_ = frame;
          have_step_ = true;
        }
        for (int a = 0; a < 3; ++a) sun_[a] = n[a];
        have_sun_ = true;
      }
    }
    if (have_jump_ && have_step_ && (jump_frame_ > step_frame_ ? jump_frame_ - step_frame_ : step_frame_ - jump_frame_) <= kRelightWindowFrames) {
      have_jump_ = have_step_ = false;
      return true;
    }
    // What can no longer meet its other witness is dropped.
    if (have_jump_ && frame > jump_frame_ && frame - jump_frame_ > kRelightWindowFrames) have_jump_ = false;
    if (have_step_ && frame > step_frame_ && frame - step_frame_ > kRelightWindowFrames) have_step_ = false;
    return false;
  }
  void Reset() { *this = RelightDetector{}; }

 private:
  bool have_clock_ = false;
  uint32_t seen_jumps_ = 0;
  bool have_jump_ = false;
  uint64_t jump_frame_ = 0;
  bool have_sun_ = false;
  double sun_[3] = {0.0, 0.0, 0.0};
  bool have_step_ = false;
  uint64_t step_frame_ = 0;
};

struct MoveReport {
  bool running = false;
  bool fresh = false;       // the first Move after Start (every probe is new)
  bool cut = false;         // the data of every probe was dropped (the camera jumped or a teleport was asked for)
  bool relight = false;     // the data of every probe was dropped because the light changed (an hour jump), the camera did not jump
  uint32_t moved = 0;       // cascades whose window moved
  uint64_t exposed = 0;     // probes cleared and marked dirty by this move
  std::array<Exposure, kResetListMaxEntries> exposures = {};
  uint32_t exposure_count = 0;
  ResetList Reset() const { return MakeResetList(exposures.data(), exposure_count); }
};

class FieldState {
 public:
  // (Re)starts for `budget` with the camera at `camera`: the windows are centred on it, every probe is new and dirty. `Move` then reports the whole windows as exposed.
  void Start(const gi::Budget& budget, const double camera[3]) {
    budget_ = budget;
    table_ = TableOf(budget);
    for (uint32_t c = 0; c < gi::kMaxCascades; ++c) {
      cascades_[c] = Cascade{};
      if (c < budget.cascade_count) {
        cascades_[c].origin = gi::OriginFor(camera, budget.cascade[c], budget.y_centre_offset_m);
        cascades_[c].dirty.Resize(budget.cascade[c].dims.Probes());
      }
    }
    for (int a = 0; a < 3; ++a) last_camera_[a] = camera[a];
    running_ = budget.cascade_count > 0;
    fresh_ = true;
    pending_ = CutKind::kNone;
    warmup_left_ = 0;
    ++starts_;
  }
  void Stop() {
    running_ = false;
    budget_ = gi::Budget{};
    table_ = ProbeTable{};
    for (Cascade& c : cascades_) c = Cascade{};
    pending_ = CutKind::kNone;
    warmup_left_ = 0;
  }
  bool running() const { return running_; }
  const gi::Budget& budget() const { return budget_; }
  const ProbeTable& table() const { return table_; }
  uint32_t starts() const { return starts_; }

  // Takes effect at the next Move. A teleport wins over a relight.
  void RequestCut(CutKind kind) {
    if (kind == CutKind::kTeleport || (kind == CutKind::kRelight && pending_ == CutKind::kNone)) pending_ = kind;
  }

  // The camera is at `camera` (world metres): the windows follow in whole cells, what entered them is cleared and marked dirty; a step over kCutMetres is a cut. Called once a frame.
  MoveReport Move(const double camera[3]) {
    MoveReport r;
    r.running = running_;
    if (!running_) return r;
    if (warmup_left_ > 0) --warmup_left_;
    r.fresh = fresh_;
    const double dx = camera[0] - last_camera_[0], dy = camera[1] - last_camera_[1], dz = camera[2] - last_camera_[2];
    const bool jumped = !fresh_ && std::sqrt(dx * dx + dy * dy + dz * dz) > double(gi::kCutMetres);
    const bool cut = !fresh_ && (jumped || pending_ == CutKind::kTeleport);
    const bool relight = !fresh_ && !cut && pending_ == CutKind::kRelight;
    pending_ = CutKind::kNone;
    for (int a = 0; a < 3; ++a) last_camera_[a] = camera[a];
    for (uint32_t c = 0; c < budget_.cascade_count; ++c) {
      Cascade& k = cascades_[c];
      const gi::CascadeSpec& spec = budget_.cascade[c];
      const gi::Coord origin = gi::OriginFor(camera, spec, budget_.y_centre_offset_m);
      const bool moved = !(origin == k.origin);
      if (fresh_ || cut || relight) {
        k.origin = origin;
        const gi::Box whole = k.Window(spec.dims);
        Expose(r, c, whole);
        k.dirty.MarkAll();
        r.moved += moved ? 1u : 0u;
        continue;
      }
      if (moved) {
        gi::Box boxes[3];
        const uint32_t n = gi::ExposedBoxes(k.origin, origin, spec.dims, boxes);
        k.origin = origin;
        for (uint32_t i = 0; i < n; ++i) {
          Expose(r, c, boxes[i]);
          MarkBox(c, boxes[i]);
        }
        ++r.moved;
      }
    }
    r.cut = cut;
    r.relight = relight;
    // The warm-up: the frame of a fresh start, a cut or a relight and the kCutWarmupFrames - 1 after it carry the larger budget.
    if (fresh_ || cut || relight) warmup_left_ = gi::kCutWarmupFrames;
    fresh_ = false;
    return r;
  }

  // The windows (and the dirty sets) as they are now.
  gi::Coord origin(uint32_t c) const { return cascades_[c].origin; }
  gi::Box Window(uint32_t c) const { return cascades_[c].Window(budget_.cascade[c].dims); }
  const DirtySet& dirty(uint32_t c) const { return cascades_[c].dirty; }
  uint32_t DirtyCount(uint32_t c) const { return cascades_[c].dirty.Count(); }
  uint32_t TotalDirty() const {
    uint32_t n = 0;
    for (uint32_t c = 0; c < budget_.cascade_count; ++c) n += cascades_[c].dirty.Count();
    return n;
  }
  // The share of cascade c's probes that are dirty, 0..1 (0 for no such cascade).
  float DirtyShare(uint32_t c) const {
    if (c >= budget_.cascade_count || cascades_[c].dirty.Size() == 0) return 0.0f;
    return float(cascades_[c].dirty.Count()) / float(cascades_[c].dirty.Size());
  }

  // The probe budget's scale for this frame: kCutWarmupBudgetScale for kCutWarmupFrames frames after a cut or a fresh start, else 1 (G2's scheduler multiplies its probes by it).
  float BudgetScale() const { return warmup_left_ > 0 ? gi::kCutWarmupBudgetScale : 1.0f; }
  uint32_t WarmupFramesLeft() const { return warmup_left_; }

  // Marks the probes of cascade c in the box of lattice coordinates [box.lo, box.hi) that are in its window. Returns how many were newly marked.
  uint32_t MarkBox(uint32_t c, const gi::Box& box) {
    if (c >= budget_.cascade_count) return 0;
    Cascade& k = cascades_[c];
    const gi::Dims& d = budget_.cascade[c].dims;
    const gi::Box win = k.Window(d);
    const gi::Box b = {{std::max(box.lo.x, win.lo.x), std::max(box.lo.y, win.lo.y), std::max(box.lo.z, win.lo.z)},
                       {std::min(box.hi.x, win.hi.x), std::min(box.hi.y, win.hi.y), std::min(box.hi.z, win.hi.z)}};
    if (b.Empty()) return 0;
    uint32_t marked = 0;
    for (int32_t z = b.lo.z; z < b.hi.z; ++z)
      for (int32_t y = b.lo.y; y < b.hi.y; ++y)
        for (int32_t x = b.lo.x; x < b.hi.x; ++x) marked += k.dirty.Mark(IndexOfCoord({x, y, z}, d)) ? 1u : 0u;
    return marked;
  }

  // A world box [lo, hi] (metres) changed (a sector was built: its geometry is new to every probe that can see it): marks the probes of every cascade within `margin_m` of it, all
  // cascades or only `cascade` (>= 0). margin_m < 0: kMomentsMaxSpacings spacings of each cascade (the distance its moments reach). Returns how many were newly marked.
  uint32_t MarkWorldBox(const double lo[3], const double hi[3], int cascade = -1, double margin_m = -1.0) {
    uint32_t marked = 0;
    for (uint32_t c = 0; c < budget_.cascade_count; ++c) {
      if (cascade >= 0 && uint32_t(cascade) != c) continue;
      const float s = budget_.cascade[c].spacing_m;
      const double m = margin_m >= 0.0 ? margin_m : double(gi::kMomentsMaxSpacings) * double(s);
      const gi::Box b = {{gi::CellOf(lo[0] - m, s), gi::CellOf(lo[1] - m, s), gi::CellOf(lo[2] - m, s)},
                         {gi::CellOf(hi[0] + m, s) + 1, gi::CellOf(hi[1] + m, s) + 1, gi::CellOf(hi[2] + m, s) + 1}};
      marked += MarkBox(c, b);
    }
    return marked;
  }
  void MarkAll(uint32_t c) {
    if (c < budget_.cascade_count) cascades_[c].dirty.MarkAll();
  }

  // G2's scheduler: up to `max` dirty probes of cascade c, the nearest to the lattice coordinate `ref` first (Chebyshev distance in cells, then the index), written to `out`. Does not
  // clear them (Take does).
  uint32_t CollectDirty(uint32_t c, uint32_t max, const gi::Coord& ref, uint32_t* out) const {
    if (c >= budget_.cascade_count || max == 0) return 0;
    const Cascade& k = cascades_[c];
    const gi::Dims& d = budget_.cascade[c].dims;
    std::vector<std::pair<uint32_t, uint32_t>> keys;  // (distance, index)
    keys.reserve(k.dirty.Count());
    k.dirty.ForEach([&](uint32_t i) {
      const gi::Coord p = CoordOfIndex(k.origin, d, i);
      const uint32_t dist = uint32_t(std::max({std::abs(p.x - ref.x), std::abs(p.y - ref.y), std::abs(p.z - ref.z)}));
      keys.push_back({dist, i});
    });
    const uint32_t n = std::min<uint32_t>(max, uint32_t(keys.size()));
    std::partial_sort(keys.begin(), keys.begin() + n, keys.end());
    for (uint32_t i = 0; i < n; ++i) out[i] = keys[i].second;
    return n;
  }
  // The same, and the probes are no longer dirty (they are going to be traced now).
  uint32_t TakeDirty(uint32_t c, uint32_t max, const gi::Coord& ref, uint32_t* out) {
    const uint32_t n = CollectDirty(c, max, ref, out);
    for (uint32_t i = 0; i < n; ++i) cascades_[c].dirty.Clear(out[i]);
    return n;
  }
  bool NoteTraced(uint32_t c, uint32_t probe) { return c < budget_.cascade_count && cascades_[c].dirty.Clear(probe); }

  // The frame the owner publishes from: the table with the windows as they are now (gi::MakeFrame with the origins kept by Move: they equal OriginFor of the last camera).
  gi::GiFrame Frame(const double camera[3]) const {
    gi::GiFrame f = gi::MakeFrame(budget_, camera);
    for (uint32_t c = 0; c < budget_.cascade_count; ++c) f.cascades[c].origin = cascades_[c].origin;
    return f;
  }

 private:
  struct Cascade {
    gi::Coord origin;
    DirtySet dirty;
    gi::Box Window(const gi::Dims& d) const { return {origin, {origin.x + int32_t(d.x), origin.y + int32_t(d.y), origin.z + int32_t(d.z)}}; }
  };
  void Expose(MoveReport& r, uint32_t c, const gi::Box& box) {
    if (box.Empty() || r.exposure_count >= kResetListMaxEntries) return;
    r.exposures[r.exposure_count++] = {c, box};
    r.exposed += box.Count();
  }

  gi::Budget budget_;
  ProbeTable table_;
  std::array<Cascade, gi::kMaxCascades> cascades_ = {};
  double last_camera_[3] = {0, 0, 0};
  bool running_ = false;
  bool fresh_ = false;
  CutKind pending_ = CutKind::kNone;
  uint32_t warmup_left_ = 0;
  uint32_t starts_ = 0;
};

// ---------------------------------------------------------------------------
// the fade
// ---------------------------------------------------------------------------

// The field is ready to show once the finest cascade is mostly traced: the dirty share must fall to the first number to start, and rise past the second to stop (a camera moving at
// speed exposes a few percent a frame: never near it). G1a / COST PASS.
inline constexpr float kReadyDirtyShare = 0.25f;
inline constexpr float kNotReadyDirtyShare = 0.5f;

struct FadeState {
  float fade = 0.0f;
  bool ready = false;

  // A cut, a new layout, the textures made again: the field has nothing to show.
  void Reset() {
    fade = 0.0f;
    ready = false;
  }
  // Advances by `dt_seconds`. `usable`: the field may be in the picture (a layout that is on, textures that exist, a play view, a safe range); `dirty_share`: the finest cascade's.
  // Returns the fade (0..1).
  float Step(double dt_seconds, bool usable, float dirty_share) {
    if (!usable) {
      ready = false;
    } else if (!ready && dirty_share <= kReadyDirtyShare) {
      ready = true;
    } else if (ready && dirty_share > kNotReadyDirtyShare) {
      ready = false;
    }
    const float dt = float(std::clamp(dt_seconds, 0.0, 1.0));
    if (usable && ready) {
      fade = std::min(1.0f, fade + dt / gi::kFadeInSeconds);
    } else {
      fade = std::max(0.0f, fade - dt / gi::kFadeOutSeconds);
    }
    return fade;
  }
};

// ---------------------------------------------------------------------------
// what the owner shows of the field in the census
// ---------------------------------------------------------------------------

inline std::string MemoryText(const Memory& m) {
  const auto mib = [](uint64_t bytes) { return double(bytes) / double(kMiB); };
  char buffer[256];
  std::snprintf(buffer, sizeof(buffer), "%.1f MiB (atlases %.1f + %.1f, weights %.2f, state %.2f; the update, the apply and the per-pixel pass will add about %.1f + %.1f + %.1f)",
                mib(m.Field()), mib(m.irradiance), mib(m.moments), mib(m.weight), mib(m.state), mib(m.update_reserve), mib(m.apply_reserve), mib(m.pixel_reserve));
  return buffer;
}

// ---------------------------------------------------------------------------
// the bindings of the owner's passes (gi_field.cpp builds the layouts from these; gi_field_rules_test.cpp reads the three .hlsl files and checks their registers against them)
// ---------------------------------------------------------------------------
// The compute passes (reset, weights): ONE descriptor set at space 0, one set for each frame slot (the frame rows and the reset list are written by the host each frame). The binding
// numbers are unique inside the set (Vulkan numbers t and u registers together).
inline constexpr uint32_t kBindRows = 0;        // t0  ByteAddressBuffer: the frame's rows (gi_frame.h GpuGiFrame, 320 bytes)
inline constexpr uint32_t kBindResetList = 1;   // t1  ByteAddressBuffer: the reset list (ResetList, 400 bytes)
inline constexpr uint32_t kBindIrradiance = 2;  // u2  RWTexture2D<float4> RGBA16F
inline constexpr uint32_t kBindMoments = 3;     // u3  RWTexture2D<float2> RG16F
inline constexpr uint32_t kBindWeight = 4;      // u4  RWTexture2D<float> R8_UNORM
inline constexpr uint32_t kBindState = 5;       // u5  RWByteAddressBuffer: the state words
// The view pass (a pixel shader): the bindless textures at space 0, the samplers at space 1, this set at space 2, the push block (96 bytes) at (b0, space 3).
inline constexpr uint32_t kViewBindRows = 0;    // t0  ByteAddressBuffer: the frame's rows
inline constexpr uint32_t kViewBindState = 1;   // t1  ByteAddressBuffer: the state words (a read only view of the state buffer)
inline constexpr uint32_t kViewPushBytes = 96;
inline constexpr uint32_t kViewPushSpace = 3;
// The thread groups the compute passes are written for.
inline constexpr uint32_t kResetGroupSize = gi::kProbeTile;  // 10 x 10 threads: one probe's tile
inline constexpr uint32_t kWeightsGroupSize = 64;            // 64 x 1 threads: 64 probes
constexpr uint32_t WeightsGroups(uint32_t probes) { return (probes + kWeightsGroupSize - 1) / kWeightsGroupSize; }

// ---------------------------------------------------------------------------
// the debug views the owner draws (mcla_gpu_rt_gi_view, hlsl/gi_field_view_ps.hlsl)
// ---------------------------------------------------------------------------
// gi::View's own values for the one the owner draws (kProbes: the probes' positions, by validity), and two appended here (gi_frame.h allows appended view modes; it is not edited): the
// probes as spheres shaded by what the field holds, in each probe's own atlas tile. The other views of gi::View are the apply's (G3): a per-pixel lookup needs the G-buffer.
inline constexpr uint32_t kViewProbes = uint32_t(gi::View::kProbes);  // 4: sphere colour = the probe's state and weight
inline constexpr uint32_t kViewIrradianceSpheres = 7;                // the sphere shows the irradiance atlas in its own normal's direction
inline constexpr uint32_t kViewSkySpheres = 8;                       // ... the sky visibility (alpha) in it
static_assert(uint32_t(gi::View::kCount) == 7, "gi_frame.h appended a view mode: check kViewIrradianceSpheres and kViewSkySpheres against it");
constexpr bool IsOwnView(uint32_t view) { return view == kViewProbes || view == kViewIrradianceSpheres || view == kViewSkySpheres; }

// The spheres' radius in spacings (a probe sits in the centre of a cell of one spacing, so under 0.5 keeps every sphere inside its own cell: the traversal's first hit is the nearest).
inline constexpr float kSphereRadiusSpacings = 0.16f;
// The view draws probes within this many metres of the camera and never farther than the scene's depth.
inline constexpr float kViewMaxDistanceM = 200.0f;

// ---------------------------------------------------------------------------
// the static checks (what the design promises, evaluated at compile time: the functions above are constant expressions)
// ---------------------------------------------------------------------------

static_assert(MemoryOf(gi::BudgetOf(gi::Layout::kStandard), false).Field() == 59228160ull && MemoryOf(gi::BudgetOf(gi::Layout::kLean), false).Field() == 4935680ull &&
                  MemoryOf(gi::BudgetOf(gi::Layout::kExtended), false).Field() == 78970880ull,
              "the field's bytes: the atlases, the weight map and the state buffer of each layout");
static_assert(Cells()[8].layout == gi::Layout::kExtended && Cells()[8].memory.Total() == 190ull * kMiB && Cells()[0].memory.Total() == 0 && Cells()[8].memory.Total() == Cells()[5].memory.Total(),
              "both RT tiers use the same Ultra field");
static_assert(FitAt(gi::Layout::kExtended, Step::kNoFourthCascade).budget.cascade_count == 3 && !FitAt(gi::Layout::kExtended, Step::kNoFourthCascade).pixel_images &&
                  FitAt(gi::Layout::kExtended, Step::kFull).pixel_images && FitAt(gi::Layout::kStandard, Step::kNoThirdCascade).budget.cascade_count == 2,
              "the steps of the order of loss");
static_assert(ChooseFit(gi::Layout::kStandard, [](uint64_t bytes) { return bytes <= 70 * kMiB; }).step == Step::kNoThirdCascade &&
                  ChooseFit(gi::Layout::kExtended, [](uint64_t) { return true; }).step == Step::kFull && !ChooseFit(gi::Layout::kLean, [](uint64_t) { return false; }).On(),
              "the room test picks the first step that fits");
static_assert(ProbeWeight(MakeState(1, false, false), true) == 0.5f && ProbeWeight(MakeState(2, true, false), true) == 0.0f && ProbeWeight(kStateAfterReset, true) == 0.0f,
              "the weight rule");
static_assert(CoordOfSlot({-3, 5, 7}, {32, 16, 32}, gi::SlotOf({-3 + 20, 5 + 3, 7 + 31}, {32, 16, 32})) == gi::Coord{17, 8, 38} && IndexOfCoord(CoordOfIndex({-40, 2, 9}, {32, 16, 32}, 12345), {32, 16, 32}) == 12345,
              "a slot's coordinate and a coordinate's slot");
static_assert(kResetListWords == 100 && kResetListBytes == 400 && kViewPushBytes == 96 && kBindState == 5 && WeightsGroups(65536) == 1024, "the list's and the groups' sizes");

}  // namespace mcla::gpu::gi_field
