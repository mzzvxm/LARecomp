#pragma once
// ===========================================================================
// MCLA GPU renderer -- the passive LightManager's rules: today's producers collected into one LightFrame, and the checks that the frame says what the consumers use
// (perf/LIGHT-UNIFY.md 7.1 step U2, package LU-2)
// ===========================================================================
// Pure: no renderer, no guest, no SDK, no GPU (light_manager_test.cpp holds it to the real structs of the headlight rig, the shadow replay and the volumetric light). Everything
// that has to be decided lives here so that the renderer's half (light_manager.{h,cpp}) is only the glue: where the producers' outputs are read, the upload and the log.
//
//   SlotTable     a light keeps its index in LightFrame::lights while it lives ("slot-stable"): id -> slot, freed when the id was not seen in a whole guest frame.
//   ShadowCarry   what headlight_rig::SetLocalShadows does with the replay's maps: a copy made at the trigger, with the car key of each light of the set it was made for and the
//                 atlas's slot; read for the next frame's set through the rig's own remap (headlight_rules::RemapShadowViews, handed in as a function, so the manager follows
//                 whatever branch it is built on).
//   Collector     the LightFrame of a guest frame, built in stages as the producers have their data: the headlights of the rig at BeginFrame (ids, slots, velocity), the shadow
//                 views at BeginFrame (the last trigger's, moved to this set) and again at the trigger (this frame's), the street lamps at the volume's fill (the ones the volume's
//                 picker chose, found in the game's list: the passive manager does not pick; LU-3 moves the pick here).
//   SelfCheck     the counts of what was compared with what and the first thing that differed.
//   Check*        the comparisons: the frame against the headlight block the lit shaders read (672 B, or 1072 B on feat/hl-per-lamp), against the volume's inputs (lamps, headlights,
//                 shadow views) and against its own invariants (unique ids, slots kept, rankings, the GPU twin's round trip, the id formulas). Templates over "any type with those
//                 members" (the convention of light_convert.h), so this header needs no producer's header.
//
// NOTHING HERE WRITES TO A PRODUCER OR READS A CONSUMER'S INPUT BACK INTO ONE: the consumers still read their own producers and the manager only looks. See light_manager.h for what
// it costs and how it is switched on (mcla_gpu_lights_census).
// ===========================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <iterator>
#include <string>
#include <type_traits>
#include <vector>

#include "light_convert.h"
#include "light_frame.h"
#include "light_id.h"
#include "light_laws.h"

namespace larecomp::gpu::light_manager {

inline constexpr uint32_t kRigLights = 4;              // the headlights a frame (headlight_rig_rules.h kMaxLights)
inline constexpr uint32_t kMaxVolumeLamps = 48;        // the volume's lamps (volumetrics_types.h kMaxLamps)
inline constexpr uint32_t kNoView = 0xFFFFFFFFu;       // headlight_rig_rules.h kNoView
inline constexpr uint32_t kNoAtlasSlot = 0xFFFFFFFFu;  // headlight_rig_rules.h kNoSlot, volumetrics_types.h kNoSlot
// A light's velocity is the step between two guest frames over the scene clock's seconds, and 0 over a gap or a jump (a cut, a menu, a teleport): the rig's tracker believes no step
// over 5 m a frame, the volume treats a camera step over 25 m as a cut.
inline constexpr double kMaxVelocityGapSeconds = 0.25;
inline constexpr float kMaxVelocityStepMetres = 25.0f;

// ---------------------------------------------------------------------------
// slots
// ---------------------------------------------------------------------------

// id -> slot. A slot stays with its id while the id is seen in each guest frame; an id not seen in a whole frame loses its slot at End() (so a light that comes back a frame later
// may be put elsewhere: nothing keys by the slot across more than a frame, everything persistent keys by the id).
class SlotTable {
 public:
  void Clear() {
    ids_.fill(kNoLightId);
    touched_.fill(false);
    added_ = kept_ = gone_ = overflow_ = 0;
  }
  // A guest frame starts: nothing has been seen yet.
  void Begin() {
    touched_.fill(false);
    added_ = kept_ = gone_ = 0;
  }
  // The slot of `id`: the one it holds, else the lowest slot no id holds, else a slot whose id was not seen in this frame (before it is freed), else kNoLightSlot.
  uint32_t Slot(uint64_t id) {
    if (id == kNoLightId) {
      return kNoLightSlot;
    }
    for (uint32_t i = 0; i < kMaxLights; ++i) {
      if (ids_[i] == id) {
        if (!touched_[i]) {
          ++kept_;
        }
        touched_[i] = true;
        return i;
      }
    }
    for (uint32_t i = 0; i < kMaxLights; ++i) {
      if (ids_[i] == kNoLightId) {
        return Take(i, id);
      }
    }
    for (uint32_t i = 0; i < kMaxLights; ++i) {
      if (!touched_[i]) {
        return Take(i, id);
      }
    }
    ++overflow_;
    return kNoLightSlot;
  }
  // The frame is over: the ids not seen lose their slots.
  void End() {
    for (uint32_t i = 0; i < kMaxLights; ++i) {
      if (ids_[i] != kNoLightId && !touched_[i]) {
        ids_[i] = kNoLightId;
        ++gone_;
      }
    }
  }
  uint64_t IdAt(uint32_t slot) const { return slot < kMaxLights ? ids_[slot] : kNoLightId; }
  uint32_t added() const { return added_; }  // ids that got a slot in this frame
  uint32_t kept() const { return kept_; }    // ids that kept the slot they had
  uint32_t gone() const { return gone_; }    // ids freed at the last End()
  uint32_t overflow() const { return overflow_; }

 private:
  uint32_t Take(uint32_t slot, uint64_t id) {
    ids_[slot] = id;
    touched_[slot] = true;
    ++added_;
    return slot;
  }
  std::array<uint64_t, kMaxLights> ids_{};
  std::array<bool, kMaxLights> touched_{};
  uint32_t added_ = 0, kept_ = 0, gone_ = 0, overflow_ = 0;
};

// ---------------------------------------------------------------------------
// the replay's maps as the rig keeps them
// ---------------------------------------------------------------------------

// headlight_rig::SetLocalShadows: at the replay's trigger the rig copies the frame's views (when there are any and the atlas has a slot) with the car key of each light of its set,
// and later moves them to the lights of whatever set reads them. The same bookkeeping, kept apart so that the manager does not depend on the rig's private copy.
template <class LocalShadowFrameT>
class ShadowCarry {
 public:
  void Clear() {
    has_ = false;
    frame_ = LocalShadowFrameT{};
    atlas_ = ShadowAtlasInfo{};
    key_count_ = 0;
  }
  // At the trigger (and whenever the producer gives its maps up: `frame` null). `keys` = the car key of each light of the set the maps were made for.
  void Set(const LocalShadowFrameT* frame, const ShadowAtlasInfo& atlas, const uint32_t* keys, uint32_t key_count) {
    Clear();
    if (frame != nullptr && frame->count != 0 && atlas.slot != kNoLightSlot) {
      has_ = true;
      frame_ = *frame;
      atlas_ = atlas;
      key_count_ = std::min<uint32_t>(key_count, kRigLights);
      for (uint32_t i = 0; i < key_count_; ++i) {
        keys_[i] = keys[i];
      }
    }
  }
  bool has() const { return has_; }
  const LocalShadowFrameT& frame() const { return frame_; }
  const ShadowAtlasInfo& atlas() const { return atlas_; }
  // What the replay hands the rig (local_shadow.cpp HandOver): its views and atlas are handed over when the atlas is valid and there is a view, else nothing.
  static bool Published(const LocalShadowFrameT& frame, const ShadowAtlasInfo& atlas) { return atlas.valid && frame.count != 0; }
  // Whether what the replay publishes now is not what the carry holds: another view or tile, another atlas slot, or maps given up (or newly made). A resolve that is not the replay's
  // trigger finds it false; the trigger finds it true unless the maps are the same as the last frame's (then the recording says so: the glue's `recorded`).
  bool Differs(const LocalShadowFrameT& frame, const ShadowAtlasInfo& atlas) const {
    const bool published = Published(frame, atlas);
    if (published != has_) {
      return true;
    }
    return published && (std::memcmp(&frame, &frame_, sizeof(frame)) != 0 || atlas.slot != atlas_.slot);
  }
  // The views for the set whose lights have these car keys: `remap(frame, old_keys, old_count, new_keys, new_count)` is headlight_rules::RemapShadowViews. Empty when there are none.
  template <class RemapFn>
  LocalShadowFrameT ForSet(const uint32_t* new_keys, uint32_t new_count, RemapFn&& remap) const {
    if (!has_) {
      return LocalShadowFrameT{};
    }
    return remap(frame_, keys_.data(), key_count_, new_keys, new_count);
  }

 private:
  bool has_ = false;
  LocalShadowFrameT frame_{};
  ShadowAtlasInfo atlas_{};
  std::array<uint32_t, kRigLights> keys_{};
  uint32_t key_count_ = 0;
};

// ---------------------------------------------------------------------------
// the collector
// ---------------------------------------------------------------------------

// Where a lamp of the frame came from.
enum class LampSource : uint8_t {
  kList = 0,         // the game's list has a record at that place and the volume's lamp is what that record makes
  kListDiffers = 1,  // the list has a record at that place but the volume's lamp is not what it makes (the check says so); the record is used
  kUnlisted = 2,     // no record (the core's test lamp, a lamp fading out whose record is gone): the Light is read back from the volume's lamp
};

// The multiplier the volume's picker folded into a lamp's colour: the brightest channel's ratio, 1 for a lamp with no colour (nothing to tell).
template <class VolLampT>
inline float InferVolumeWeight(const VolLampT& projected_at_one, const VolLampT& volume) {
  const float p = std::max({projected_at_one.colour[0], projected_at_one.colour[1], projected_at_one.colour[2]});
  const float v = std::max({volume.colour[0], volume.colour[1], volume.colour[2]});
  if (!(p > 0.0f)) {
    return 1.0f;
  }
  return std::clamp(v / p, 0.0f, 1.0f);
}

// Whether two volume lamps are the same lamp: every field but the colour (the picker's weight is in it) and the shadow views (their indices are another table's).
template <class VolLampT>
inline bool SameLampShape(const VolLampT& a, const VolLampT& b) {
  return std::memcmp(a.pos, b.pos, sizeof(a.pos)) == 0 && std::memcmp(a.dir, b.dir, sizeof(a.dir)) == 0 && std::memcmp(&a.range, &b.range, sizeof(float)) == 0 &&
         std::memcmp(&a.cos_outer, &b.cos_outer, sizeof(float)) == 0 && std::memcmp(&a.cos_inner, &b.cos_inner, sizeof(float)) == 0 &&
         std::memcmp(&a.pad[0], &b.pad[0], sizeof(float)) == 0;
}

class Collector {
 public:
  // The rig's lights in the rig's own order (the order `CurrentHeadlights()` and the headlight block use): which slot of the frame each is in.
  struct Rig {
    uint32_t count = 0;
    std::array<uint32_t, kRigLights> slot{};
  };
  // What a frame was made of, for the census.
  struct Stats {
    uint32_t headlights = 0, player = 0, duplicate_ids = 0;
    uint32_t lamps = 0, listed = 0, from_list = 0, differs = 0, unlisted = 0, lamps_dropped = 0;
    uint32_t slots_kept = 0, slots_added = 0, slots_gone = 0, slot_overflow = 0;
    uint32_t shadowed = 0, views = 0;
  };

  void Reset() {
    slots_.Clear();
    frame_ = LightFrame{};
    prev_ = LightFrame{};
    have_prev_ = false;
    rig_ = Rig{};
    lamp_count_ = 0;
    list_ids_.clear();
    stats_ = Stats{};
    generation_ = 1;
    budget_key_ = {};
  }

  // ---- a guest frame ----

  // The frame opens: the rig's set of this frame (it was decided at the last present). `now_s` is the scene clock in seconds.
  template <class HeadlightSetT>
  void BeginFrame(uint32_t frame_serial, double now_s, const HeadlightSetT& set) {
    slots_.Begin();
    frame_ = LightFrame{};
    frame_.frame = frame_serial;
    frame_.generation = generation_;
    rig_ = Rig{};
    lamp_count_ = 0;
    stats_ = Stats{};
    now_s_ = now_s;
    const double dt = now_s - prev_time_s_;
    const bool velocity_ok = have_prev_ && dt > 1.0e-4 && dt <= kMaxVelocityGapSeconds;
    for (uint32_t i = 0; i < uint32_t(set.count) && i < kRigLights; ++i) {
      Light l = LightFromHeadlight(set.lights[i]);
      // Two lights of one car (the rig's keys are unique: this is the guard that keeps the ids so, too).
      for (uint32_t n = 0; frame_.Find(l.id) >= 0; ++n) {
        l.id = light_id::Make(light_id::kDomainCar, l.car_key, 3u + n);
        ++stats_.duplicate_ids;
      }
      if (velocity_ok) {
        const int32_t p = prev_.Find(l.id);
        if (p >= 0) {
          float step[3];
          float step2 = 0.0f;
          for (int c = 0; c < 3; ++c) {
            step[c] = l.pos[c] - prev_.lights[uint32_t(p)].pos[c];
            step2 += step[c] * step[c];
          }
          if (step2 <= kMaxVelocityStepMetres * kMaxVelocityStepMetres) {
            for (int c = 0; c < 3; ++c) {
              l.vel[c] = float(double(step[c]) / dt);
            }
          }
        }
      }
      const uint32_t slot = Place(l);
      if (slot == kNoLightSlot) {
        continue;
      }
      rig_.slot[rig_.count++] = slot;
      ++stats_.headlights;
      stats_.player += (l.flags & kLightFlagPlayer) != 0 ? 1u : 0u;
    }
    RebuildOrder();
  }

  // The shadow views as the producer made them, already moved to this frame's set (`local` null: none), and the atlas they are in. Called at BeginFrame (the last trigger's maps) and
  // again at the trigger (this frame's).
  template <class LocalShadowFrameT>
  void SetShadows(const LocalShadowFrameT* local, const ShadowAtlasInfo& atlas) {
    std::array<uint64_t, kRigLights> ids{};
    for (uint32_t i = 0; i < rig_.count; ++i) {
      ids[i] = frame_.lights[rig_.slot[i]].id;
    }
    if (local != nullptr && local->count != 0) {
      frame_.shadow = ShadowFrameFromLocal(*local, atlas, ids.data(), rig_.count);
    } else {
      frame_.shadow = ShadowFrame{};
      frame_.shadow.atlas = atlas;
    }
    BindShadowRefs(&frame_);
    stats_.shadowed = 0;
    for (uint32_t i = 0; i < frame_.count; ++i) {
      stats_.shadowed += frame_.lights[i].shadow.kind == ShadowKind::kAtlas ? 1u : 0u;
    }
    stats_.views = frame_.shadow.count;
    RebuildOrder();
  }

  // What the budgets and the atlas were this frame: a change moves `generation` (what was cached from an older one is dropped: light_frame.h).
  void NoteBudgets(uint32_t atlas_slot, uint32_t atlas_w, uint32_t atlas_h, uint32_t grid_lamps, uint32_t grid_headlights) {
    const std::array<uint32_t, 5> key = {atlas_slot, atlas_w, atlas_h, grid_lamps, grid_headlights};
    if (key != budget_key_) {
      budget_key_ = key;
      ++generation_;
    }
    frame_.generation = generation_;
  }

  // The street lamps, at the volume's fill: `picked` = the volume's lamps of this frame (`picked_count` of them), `list` = the game's lamp list (a copy of the newest the hook table
  // gave: `list_changed` when it is another copy than the last call's). Each picked lamp is found in the list by its place (the id), turned into a Light by the one conversion and
  // given the weight the volume's picker folded into its colour; one the list has no record for is read back from the volume's lamp.
  template <class GameLampT, class VolLampT>
  void SetLamps(const GameLampT* list, uint32_t list_count, bool list_changed, const VolLampT* picked, uint32_t picked_count) {
    if (list_changed || list_ids_.size() != list_count) {
      list_ids_.resize(list_count);
      for (uint32_t j = 0; j < list_count; ++j) {
        list_ids_[j] = light_id::LampId(list[j].pos);
      }
    }
    stats_.listed = list_count;
    stats_.lamps = stats_.from_list = stats_.differs = stats_.unlisted = stats_.lamps_dropped = 0;
    for (uint32_t k = 0; k < lamp_count_; ++k) {
      frame_.lights[lamp_slot_[k]] = Light{};  // a second fill in the same frame replaces the first's lamps
    }
    lamp_count_ = 0;
    for (uint32_t k = 0; k < picked_count && k < kMaxVolumeLamps; ++k) {
      const VolLampT& v = picked[k];
      const uint64_t id = light_id::LampId(v.pos);
      Light light;
      LampSource source = LampSource::kUnlisted;
      int32_t first = -1;
      bool found = false;
      for (uint32_t j = 0; j < list_count && !found; ++j) {
        if (list_ids_[j] != id) {
          continue;
        }
        first = first < 0 ? int32_t(j) : first;
        const Light candidate = LightFromGameLamp(list[j]);
        if (SameLampShape(ToVolLamp<VolLampT>(candidate), v)) {
          light = candidate;
          source = LampSource::kList;
          found = true;
        }
      }
      if (!found && first >= 0) {
        light = LightFromGameLamp(list[uint32_t(first)]);
        source = LampSource::kListDiffers;
        found = true;
      }
      if (found) {
        light.vol_weight = InferVolumeWeight(ToVolLamp<VolLampT>(light), v);
      } else {
        light = LightFromVolLamp(v, LightLaw::kGameLamp, LightSource::kLamp);
        light.shadow.reach = light_laws::LampShadowReach(light.law_a);
      }
      light.id = id;
      if (frame_.Find(id) >= 0) {
        ++stats_.lamps_dropped;  // the same place twice
        continue;
      }
      const uint32_t slot = Place(light);
      if (slot == kNoLightSlot) {
        ++stats_.lamps_dropped;  // no room
        continue;
      }
      lamp_slot_[lamp_count_] = slot;
      lamp_source_[lamp_count_] = source;
      lamp_input_[lamp_count_] = k;
      ++lamp_count_;
      ++stats_.lamps;
      stats_.from_list += source == LampSource::kList ? 1u : 0u;
      stats_.differs += source == LampSource::kListDiffers ? 1u : 0u;
      stats_.unlisted += source == LampSource::kUnlisted ? 1u : 0u;
    }
    BindShadowRefs(&frame_);  // a lamp has no view (yet): kNone, as a hole in the frame's own table would say
    RebuildOrder();
  }

  // LU-4b: the street lamps the manager's OWN pick chose for the volume (volumetrics_lamp_pick.h: PickResult::records and weights), `count` of them in the picker's order. Each is the
  // list's record made a Light by the one conversion, with the weight the picker folded into its colour exactly (no division to infer it: ToVolLamp of the Light is then the picker's
  // VolLamp bit for bit), one lamp a place, `listed` = the records the pick was given. Replaces what an earlier call of this or of SetLamps put in the frame.
  template <class GameLampT>
  void SetPickedLamps(const GameLampT* records, const float* weights, uint32_t count, uint32_t listed) {
    stats_.listed = listed;
    stats_.lamps = stats_.from_list = stats_.differs = stats_.unlisted = stats_.lamps_dropped = 0;
    for (uint32_t k = 0; k < lamp_count_; ++k) {
      frame_.lights[lamp_slot_[k]] = Light{};
    }
    lamp_count_ = 0;
    for (uint32_t k = 0; k < count && k < kMaxVolumeLamps; ++k) {
      Light light = LightFromGameLamp(records[k]);
      light.vol_weight = std::clamp(weights[k], 0.0f, 1.0f);
      if (frame_.Find(light.id) >= 0) {
        ++stats_.lamps_dropped;  // the same place twice
        continue;
      }
      const uint32_t slot = Place(light);
      if (slot == kNoLightSlot) {
        ++stats_.lamps_dropped;  // no room
        continue;
      }
      lamp_slot_[lamp_count_] = slot;
      lamp_source_[lamp_count_] = LampSource::kList;
      lamp_input_[lamp_count_] = k;
      ++lamp_count_;
      ++stats_.lamps;
      ++stats_.from_list;
    }
    BindShadowRefs(&frame_);
    RebuildOrder();
  }

  // The guest frame is over: the ids not seen lose their slots, this frame becomes the one the next compares with.
  void EndFrame() {
    slots_.End();
    stats_.slots_kept = slots_.kept();
    stats_.slots_added = slots_.added();
    stats_.slots_gone = slots_.gone();
    stats_.slot_overflow = slots_.overflow();
    prev_ = frame_;
    prev_time_s_ = now_s_;
    have_prev_ = true;
  }

  const LightFrame& frame() const { return frame_; }
  const LightFrame& previous() const { return prev_; }
  bool has_previous() const { return have_prev_; }
  const Rig& rig() const { return rig_; }
  const Stats& stats() const { return stats_; }
  uint32_t lamp_count() const { return lamp_count_; }
  // The slot, the source and the place in the volume's picked lamps of the k-th lamp of the frame.
  uint32_t lamp_slot(uint32_t k) const { return k < lamp_count_ ? lamp_slot_[k] : kNoLightSlot; }
  uint32_t lamp_input(uint32_t k) const { return k < lamp_count_ ? lamp_input_[k] : 0u; }
  LampSource lamp_source(uint32_t k) const { return k < lamp_count_ ? lamp_source_[k] : LampSource::kUnlisted; }
  const SlotTable& slots() const { return slots_; }
  LightFrame& mutable_frame_for_tests() { return frame_; }

 private:
  // The light in the slot its id holds (kNoLightSlot: no room); the frame's count follows.
  uint32_t Place(const Light& l) {
    const uint32_t slot = slots_.Slot(l.id);
    if (slot != kNoLightSlot) {
      frame_.lights[slot] = l;
      frame_.count = std::max(frame_.count, slot + 1);
    }
    return slot;
  }
  // order[] and role_count of the frame: headlights in the rig's order are the surface role's; the volume's is the headlights, then the lamps in the order the volume's picker gave them;
  // the shadow role's is the lights with views, in the rig's order. (LU-3 ranks; passive, the producers' own orders stand.)
  void RebuildOrder() {
    frame_.order = {};
    frame_.role_count = {};
    const auto push = [&](LightRole role, uint32_t slot) {
      uint8_t& n = frame_.role_count[uint32_t(role)];
      if (n < kMaxLights) {
        frame_.order[uint32_t(role)][n++] = uint8_t(slot);
      }
    };
    for (uint32_t i = 0; i < rig_.count; ++i) {
      const uint32_t slot = rig_.slot[i];
      const Light& l = frame_.lights[slot];
      if ((l.roles & kRoleMaskSurface) != 0) {
        push(LightRole::kSurface, slot);
      }
      if ((l.roles & kRoleMaskVolume) != 0) {
        push(LightRole::kVolume, slot);
      }
      if ((l.roles & kRoleMaskShadow) != 0 && l.shadow.kind == ShadowKind::kAtlas) {
        push(LightRole::kShadow, slot);
      }
    }
    for (uint32_t k = 0; k < lamp_count_; ++k) {
      const uint32_t slot = lamp_slot_[k];
      const Light& l = frame_.lights[slot];
      if ((l.roles & kRoleMaskSurface) != 0) {
        push(LightRole::kSurface, slot);
      }
      if ((l.roles & kRoleMaskVolume) != 0) {
        push(LightRole::kVolume, slot);
      }
      if ((l.roles & kRoleMaskShadow) != 0 && l.shadow.kind == ShadowKind::kAtlas) {
        push(LightRole::kShadow, slot);
      }
    }
    uint32_t count = 0;
    for (uint32_t i = 0; i < kMaxLights; ++i) {
      if (frame_.lights[i].id != kNoLightId) {
        count = i + 1;
      }
    }
    frame_.count = count;
  }

  SlotTable slots_;
  LightFrame frame_{};
  LightFrame prev_{};
  bool have_prev_ = false;
  double now_s_ = 0.0;
  double prev_time_s_ = 0.0;
  Rig rig_{};
  uint32_t lamp_count_ = 0;
  std::array<uint32_t, kMaxVolumeLamps> lamp_slot_{};
  std::array<LampSource, kMaxVolumeLamps> lamp_source_{};
  std::array<uint32_t, kMaxVolumeLamps> lamp_input_{};
  std::vector<uint64_t> list_ids_;
  Stats stats_{};
  uint32_t generation_ = 1;
  std::array<uint32_t, 5> budget_key_{};
};

// ---------------------------------------------------------------------------
// the GPU buffer
// ---------------------------------------------------------------------------

// What a frame writes once into its buffer (light_frame.h GpuLightBuffer): the header and the packed lights in slot order, the rows past `count` zero.
inline void BuildGpuLightBuffer(const LightFrame& frame, GpuLightBuffer* out) {
  *out = GpuLightBuffer{};
  out->header.count = frame.count;
  out->header.frame = frame.frame;
  out->header.generation = frame.generation;
  for (uint32_t i = 0; i < frame.count && i < kMaxLights; ++i) {
    if (frame.lights[i].id != kNoLightId) {
      out->lights[i] = PackGpuLight(frame.lights[i]);
    }
  }
}

// ---------------------------------------------------------------------------
// the self-check
// ---------------------------------------------------------------------------

enum class Check : uint32_t {
  kBlockBefore = 0,   // the headlight block (feature 9's) at the frame's start: the lights of this set, the maps of the last trigger moved to it
  kBlockAfter,        // ... and after the trigger: this frame's maps
  kVolumeLamps,       // the volume's street lamps
  kVolumeHeadlights,  // the volume's headlights
  kVolumeViews,       // and the shadow views they read
  kFrame,             // the frame's own invariants
  kBlockBytes,        // LU-4a: the projection of the frame (light_project.h) against the rig's block, byte for byte (at the frame's start and after the trigger)
  kVolumeBytes,       // LU-4b: the volume's light inputs made from the frame against the legacy producer's, byte for byte
  kShadowLights,      // LU-4c: the lights the shadow replay picks from, made from the frame against the rig's set, bit for bit
  kCount
};
inline constexpr uint32_t kCheckCount = uint32_t(Check::kCount);
inline const char* CheckName(Check c) {
  switch (c) {
    case Check::kBlockBefore: return "block at start";
    case Check::kBlockAfter: return "block after trigger";
    case Check::kVolumeLamps: return "volume lamps";
    case Check::kVolumeHeadlights: return "volume headlights";
    case Check::kVolumeViews: return "volume views";
    case Check::kFrame: return "frame";
    case Check::kBlockBytes: return "block bytes";
    case Check::kVolumeBytes: return "volume bytes";
    case Check::kShadowLights: return "shadow lights";
    case Check::kCount: break;
  }
  return "?";
}

// How many checks were made and how many differed, over a window (a census line) and over the run, and the first difference of each.
class SelfCheck {
 public:
  struct Window {
    std::array<uint64_t, kCheckCount> checks{};
    std::array<uint64_t, kCheckCount> bad{};
    std::string first;  // the first difference of the window ("<check>: what")
    uint64_t Checks() const {
      uint64_t n = 0;
      for (uint64_t c : checks) {
        n += c;
      }
      return n;
    }
    uint64_t Bad() const {
      uint64_t n = 0;
      for (uint64_t c : bad) {
        n += c;
      }
      return n;
    }
  };
  void Pass(Check k) {
    ++window_.checks[uint32_t(k)];
    ++total_checks_;
  }
  // Records a difference; true when it should be logged at once (the first few of each kind in the whole run).
  bool Fail(Check k, const std::string& what) {
    const uint32_t i = uint32_t(k);
    ++window_.checks[i];
    ++window_.bad[i];
    ++total_checks_;
    ++total_bad_;
    if (window_.first.empty()) {
      window_.first = std::string(CheckName(k)) + ": " + what;
    }
    return ++logged_[i] <= kLogEach;
  }
  void Note(Check k, const std::string& fail) {
    if (fail.empty()) {
      Pass(k);
    } else if (Fail(k, fail) && on_fail_) {
      on_fail_(k, fail);
    }
  }
  Window TakeWindow() {
    Window w = std::move(window_);
    window_ = Window{};
    return w;
  }
  uint64_t total_checks() const { return total_checks_; }
  uint64_t total_bad() const { return total_bad_; }
  // Called for a difference that should be logged at once.
  void SetOnFail(void (*fn)(Check, const std::string&)) { on_fail_ = fn; }

 private:
  static constexpr uint32_t kLogEach = 3;
  Window window_;
  std::array<uint32_t, kCheckCount> logged_{};
  uint64_t total_checks_ = 0, total_bad_ = 0;
  void (*on_fail_)(Check, const std::string&) = nullptr;
};

namespace detail {

inline bool SameBits(const void* a, const void* b, size_t bytes) { return std::memcmp(a, b, bytes) == 0; }
inline bool SameFloat(float a, float b) { return SameBits(&a, &b, sizeof(float)); }
// Within a few units of the last place: a weight found by dividing and multiplied again is not bit for bit the picker's own.
inline bool NearFloat(float a, float b, float rel = 1.0e-6f) { return std::fabs(a - b) <= rel * std::max({std::fabs(a), std::fabs(b), 1.0e-30f}); }
// Adds one finding to the text of a check (at most three: the first is what the census shows).
inline void Add(std::string* fail, const std::string& what) {
  if (fail->size() < 360) {
    *fail += fail->empty() ? what : "; " + what;
  }
}

}  // namespace detail

// The headlight block (headlight_rig_rules.h HeadlightConstants, 672 B or 1072 B) against the frame. The block's lights are the rig's, in the rig's order: light i of the block is the
// frame's light in `rig.slot[i]`, row for row bit for bit (position + reach, axis + cos outer, colour + cos inner), its reach box, and each light's shadow views by content (the
// block's `shadow_of` / `shadow_of_b` index its own table, the frame's `ShadowRef` another's) and the atlas's slot and sampler, which the block carries only when a light reads a view.
template <class BlockT>
inline void CheckHeadlightBlock(const LightFrame& frame, const Collector::Rig& rig, const BlockT& b, Check kind, SelfCheck* sc) {
  using detail::Add;
  using detail::SameBits;
  std::string fail;
  const uint32_t n = std::min<uint32_t>(rig.count, kRigLights);
  const uint32_t block_n = b.header[0] > 0.0f ? uint32_t(b.header[0]) : 0u;
  if (block_n != n) {
    Add(&fail, std::format("the block holds {} lights, the frame {}", block_n, n));
    sc->Note(kind, fail);
    return;
  }
  for (uint32_t i = 0; i < n; ++i) {
    const Light& l = frame.lights[rig.slot[i]];
    const float* a = b.lights[3 * i];
    const float* d = b.lights[3 * i + 1];
    const float* c = b.lights[3 * i + 2];
    const bool ok = SameBits(a, l.pos, 12) && SameBits(a + 3, &l.range, 4) && SameBits(d, l.dir, 12) && SameBits(d + 3, &l.cos_outer, 4) && SameBits(c, l.color, 12) &&
                    SameBits(c + 3, &l.cos_inner, 4);
    if (!ok) {
      Add(&fail, std::format("light {} (id {:x}) differs from its block rows: pos ({:.3f} {:.3f} {:.3f}) vs ({:.3f} {:.3f} {:.3f}), colour {:.4f} vs {:.4f}", i, l.id, l.pos[0],
                             l.pos[1], l.pos[2], a[0], a[1], a[2], l.color[0], c[0]));
    }
  }
  // The reach box: the shader's early out (HeadlightRig MakeConstants: every light's position minus and plus its reach; an empty set a box with lo above hi).
  float lo[3] = {1.0e30f, 1.0e30f, 1.0e30f};
  float hi[3] = {-1.0e30f, -1.0e30f, -1.0e30f};
  for (uint32_t i = 0; i < n; ++i) {
    const Light& l = frame.lights[rig.slot[i]];
    for (int k = 0; k < 3; ++k) {
      lo[k] = i == 0 ? l.pos[k] - l.range : std::min(lo[k], l.pos[k] - l.range);
      hi[k] = i == 0 ? l.pos[k] + l.range : std::max(hi[k], l.pos[k] + l.range);
    }
  }
  if (!SameBits(b.bounds_lo, lo, 12) || !SameBits(b.bounds_hi, hi, 12)) {
    Add(&fail, "the reach box differs");
  }
  // The shadow views by content.
  bool block_reads_views = false;
  for (uint32_t i = 0; i < n; ++i) {
    const Light& l = frame.lights[rig.slot[i]];
    uint32_t want = l.shadow.kind == ShadowKind::kAtlas ? l.shadow.view_count : 0u;
    uint32_t got[2] = {b.shadow_of[i], kNoView};
    if constexpr (requires { b.shadow_of_b[i]; }) {
      got[1] = b.shadow_of_b[i];
    } else {
      want = std::min(want, 1u);  // the 672 byte block has one view a light
    }
    const uint32_t got_count = (got[0] != kNoView ? 1u : 0u) + (got[0] != kNoView && got[1] != kNoView ? 1u : 0u);
    block_reads_views = block_reads_views || got[0] != kNoView;
    if (got_count != want) {
      Add(&fail, std::format("light {} reads {} views in the block, the frame says {}", i, got_count, want));
      continue;
    }
    for (uint32_t e = 0; e < got_count; ++e) {
      const uint32_t index = got[e];
      const uint32_t mine = uint32_t(l.shadow.first_view) + e;
      if (index >= std::size(b.shadow) || mine >= kMaxLightShadowViews) {
        Add(&fail, std::format("light {} names view {} / {} out of range", i, index, mine));
      } else if (!SameBits(&b.shadow[index], &frame.shadow.views[mine], sizeof(LocalShadowView))) {
        Add(&fail, std::format("light {}'s view {} differs from the frame's entry {}", i, e, mine));
      }
    }
  }
  const uint32_t want_slot = block_reads_views ? frame.shadow.atlas.slot : kNoAtlasSlot;
  const uint32_t want_sampler = block_reads_views ? frame.shadow.atlas.sampler : kNoAtlasSlot;
  if (b.atlas[0] != want_slot || b.atlas[1] != want_sampler) {
    Add(&fail, std::format("the block's atlas is slot {} sampler {}, the frame's {} / {}", b.atlas[0], b.atlas[1], want_slot, want_sampler));
  }
  sc->Note(kind, fail);
}

// What the volume's check needs to know that its inputs do not say.
struct VolumeCheckSpec {
  bool lamps_on = true;      // the volume reads the game's lamps (mcla_gpu_vol_inputs bit 1 and no test lamp instead)
  bool heads_on = true;      // ... and the rig's headlights
  uint32_t first_lamp = 0;   // the lamps before it were in the volume's inputs before the producer's fill (the core's test lamp): the producer's are from here on
};

// The volume's inputs (volumetrics_types.h VolInputs) against the frame. Lamps: each is the frame's light at its place (the id), through the one projection ToVolLamp, field for field
// bit for bit but for the colour, which carries the picker's weight (within a millionth); a lamp with no record in the game's list is counted, not compared. Headlights: the rig's
// lights in the rig's order, in the volume's units, bit for bit; the shadow views: each light's by content, and the atlas's slot and size when a light reads one. `usable` is the
// volume's own filter of a published view (volumetrics_headlights.h UsableView): a view it drops is not expected to be read.
template <class VolInputsT, class UsableFn>
inline void CheckVolumeInputs(const LightFrame& frame, const Collector& c, const VolInputsT& in, const VolumeCheckSpec& spec, UsableFn&& usable, SelfCheck* sc) {
  using detail::Add;
  using detail::NearFloat;
  using detail::SameBits;
  using VolLampT = std::remove_cvref_t<decltype(in.lamps[0])>;
  using VolViewT = std::remove_cvref_t<decltype(in.shadow_views[0])>;

  if (spec.lamps_on) {
    std::string fail;
    const uint32_t first = std::min(spec.first_lamp, uint32_t(in.lamp_count));
    if (c.stats().lamps_dropped != 0) {
      Add(&fail, std::format("{} of the volume's lamps had no slot or repeated a place", c.stats().lamps_dropped));
    }
    for (uint32_t k = 0; k < c.lamp_count(); ++k) {
      const uint32_t at = first + c.lamp_input(k);
      if (at >= in.lamp_count) {
        Add(&fail, std::format("lamp {} is the volume's {}, which it does not have", k, at));
        continue;
      }
      const VolLampT& v = in.lamps[at];
      const uint32_t slot = c.lamp_slot(k);
      if (slot == kNoLightSlot) {
        Add(&fail, std::format("lamp {} has no slot", k));
        continue;
      }
      const Light& l = frame.lights[slot];
      if (l.id != light_id::LampId(v.pos)) {
        Add(&fail, std::format("lamp {} is slot {} with another id", k, slot));
        continue;
      }
      if (c.lamp_source(k) == LampSource::kUnlisted) {
        continue;  // read back from the volume's own lamp: nothing independent to compare it with
      }
      const VolLampT p = ToVolLamp<VolLampT>(l);
      bool colour_ok = true;
      for (int i = 0; i < 3; ++i) {
        colour_ok = colour_ok && NearFloat(p.colour[i], v.colour[i]);
      }
      if (!SameLampShape(p, v) || !colour_ok) {
        Add(&fail, std::format("lamp {} at ({:.2f} {:.2f} {:.2f}) differs from what its list record makes: colour {:.5f} vs {:.5f}, range {:.3f} vs {:.3f}, near^2 {:.4f} vs {:.4f}{}", k,
                               v.pos[0], v.pos[1], v.pos[2], p.colour[0], v.colour[0], p.range, v.range, p.pad[0], v.pad[0],
                               c.lamp_source(k) == LampSource::kListDiffers ? " (the record at that place is not the picker's)" : ""));
      }
    }
    // Every lamp the producer put in is in the frame (the ones before it are the core's).
    if (in.lamp_count > first && c.lamp_count() + c.stats().lamps_dropped != in.lamp_count - first) {
      Add(&fail, std::format("the volume has {} lamps from the producer, the frame {} (+ {} dropped)", in.lamp_count - first, c.lamp_count(), c.stats().lamps_dropped));
    }
    sc->Note(Check::kVolumeLamps, fail);
  }

  if (spec.heads_on) {
    std::string fail;
    const uint32_t want = std::min<uint32_t>(c.rig().count, uint32_t(in.headlights.size()));
    if (in.headlight_count != want) {
      Add(&fail, std::format("the volume has {} headlights, the rig {}", in.headlight_count, want));
    }
    for (uint32_t j = 0; j < in.headlight_count && j < want; ++j) {
      const Light& l = frame.lights[c.rig().slot[j]];
      const VolLampT p = ToVolLamp<VolLampT>(l);
      const VolLampT& v = in.headlights[j];
      bool colour_ok = true;
      for (int i = 0; i < 3; ++i) {
        colour_ok = colour_ok && SameBits(&p.colour[i], &v.colour[i], sizeof(float));
      }
      if (!SameLampShape(p, v) || !colour_ok) {
        Add(&fail, std::format("headlight {} differs from the frame's light: colour {:.5f} vs {:.5f}, near^2 {:.3f} vs {:.3f}", j, p.colour[0], v.colour[0], p.pad[0], v.pad[0]));
      }
    }
    sc->Note(Check::kVolumeHeadlights, fail);

    // The views each headlight reads.
    std::string views;
    bool any = false;
    for (uint32_t j = 0; j < in.headlight_count && j < want; ++j) {
      const Light& l = frame.lights[c.rig().slot[j]];
      // What the frame expects the volume to read: the light's entries, as the volume's views, those it finds usable.
      std::array<VolViewT, 2> expect{};
      uint32_t expect_count = 0;
      if (l.shadow.kind == ShadowKind::kAtlas) {
        for (uint32_t e = 0; e < l.shadow.view_count && e < 2; ++e) {
          const VolViewT v = ToVolShadowView<VolViewT>(frame.shadow.views[uint32_t(l.shadow.first_view) + e]);
          if (usable(v)) {
            expect[expect_count++] = v;
          }
        }
      }
      std::array<int32_t, 2> got = {in.headlights[j].shadow_view, -1};
      if constexpr (requires { in.headlights[j].shadow_view_b; }) {
        got[1] = in.headlights[j].shadow_view_b;
      }
      uint32_t got_count = 0;
      for (const int32_t index : got) {
        if (index < 0) {
          continue;
        }
        if (uint32_t(index) >= in.shadow_view_count || uint32_t(index) >= in.shadow_views.size()) {
          Add(&views, std::format("headlight {} names view {} of {}", j, index, in.shadow_view_count));
          continue;
        }
        if (got_count < expect_count && !SameBits(&in.shadow_views[uint32_t(index)], &expect[got_count], sizeof(VolViewT))) {
          Add(&views, std::format("headlight {}'s view {} differs from the frame's", j, got_count));
        }
        ++got_count;
      }
      if (got_count != expect_count) {
        Add(&views, std::format("headlight {} reads {} views, the frame expects {}", j, got_count, expect_count));
      }
      any = any || got_count != 0;
    }
    const uint32_t want_slot = any ? frame.shadow.atlas.slot : kNoAtlasSlot;
    if (in.shadow_atlas_slot != want_slot) {
      Add(&views, std::format("the volume's atlas slot is {}, the frame's {}", in.shadow_atlas_slot, want_slot));
    } else if (any && (!detail::SameFloat(in.shadow_atlas_size[0], float(frame.shadow.atlas.width)) || !detail::SameFloat(in.shadow_atlas_size[1], float(frame.shadow.atlas.height)))) {
      Add(&views, std::format("the volume's atlas is {}x{}, the frame's {}x{}", in.shadow_atlas_size[0], in.shadow_atlas_size[1], frame.shadow.atlas.width, frame.shadow.atlas.height));
    }
    sc->Note(Check::kVolumeViews, views);
  }
}

// The frame against itself and against the one before it: the ids are unique and the formulas that make them hold, a light seen in two frames in a row keeps its slot, the rankings
// name lights that have the role, a light's views are the frame's and are its own, every number is a number, and what the GPU gets (PackGpuLight) is what the light says.
inline void CheckFrameInvariants(const LightFrame& frame, const LightFrame* previous, SelfCheck* sc) {
  using detail::Add;
  std::string fail;
  const auto finite = [](const float* p, int n) {
    for (int i = 0; i < n; ++i) {
      if (!std::isfinite(p[i])) {
        return false;
      }
    }
    return true;
  };
  if (frame.count > kMaxLights) {
    Add(&fail, "count over the capacity");
    sc->Note(Check::kFrame, fail);
    return;
  }
  for (uint32_t i = 0; i < frame.count; ++i) {
    const Light& l = frame.lights[i];
    if (l.id == kNoLightId) {
      continue;
    }
    for (uint32_t j = 0; j < i; ++j) {
      if (frame.lights[j].id == l.id) {
        Add(&fail, std::format("slots {} and {} hold the same id", j, i));
      }
    }
    if (previous != nullptr) {
      const int32_t p = previous->Find(l.id);
      if (p >= 0 && uint32_t(p) != i) {
        Add(&fail, std::format("id {:x} moved from slot {} to {}", l.id, p, i));
      }
    }
    if (l.source == LightSource::kHeadlight && l.id != light_id::CarLampId(l.car_key, light_id::CarLamp::kPair) && l.id != light_id::Make(light_id::kDomainCar, l.car_key, 3)) {
      Add(&fail, std::format("slot {}: a headlight's id is not its car's", i));
    }
    if (l.source == LightSource::kLamp && l.id != light_id::LampId(l.pos)) {
      Add(&fail, std::format("slot {}: a lamp's id is not its place's", i));
    }
    if (!finite(l.pos, 3) || !finite(l.dir, 3) || !finite(l.color, 3) || !finite(l.vel, 3) || !std::isfinite(l.range) || !std::isfinite(l.cos_outer) ||
        !std::isfinite(l.cos_inner) || !std::isfinite(l.law_a) || !(l.vol_weight >= 0.0f && l.vol_weight <= 1.0f)) {
      Add(&fail, std::format("slot {}: a number is not a number, or the weight is outside 0..1", i));
    }
    if (l.shadow.kind == ShadowKind::kAtlas) {
      const uint32_t end = uint32_t(l.shadow.first_view) + l.shadow.view_count;
      if (l.shadow.view_count < 1 || l.shadow.view_count > 2 || end > frame.shadow.count || end > kMaxLightShadowViews) {
        Add(&fail, std::format("slot {}: views {}..{} of {}", i, l.shadow.first_view, end, frame.shadow.count));
      } else {
        for (uint32_t e = l.shadow.first_view; e < end; ++e) {
          if (frame.shadow.view_light[e] != l.id || frame.shadow.views[e].valid == 0) {
            Add(&fail, std::format("slot {}: view {} is not its own or not valid", i, e));
          }
        }
      }
    }
    // The GPU twin carries everything the pass reads, bit for bit.
    const Light round = UnpackGpuLight(PackGpuLight(l));
    const bool same = detail::SameBits(round.pos, l.pos, 12) && detail::SameBits(round.dir, l.dir, 12) && detail::SameBits(round.color, l.color, 12) &&
                      detail::SameFloat(round.range, l.range) && detail::SameFloat(round.cos_outer, l.cos_outer) && detail::SameFloat(round.cos_inner, l.cos_inner) &&
                      detail::SameFloat(round.law_a, l.law_a) && round.kind == l.kind && round.source == l.source && round.law == l.law && round.roles == l.roles &&
                      round.flags == (l.flags & kLightFlagsGpuMask) && round.shadow.kind == l.shadow.kind && round.shadow.view_count == l.shadow.view_count &&
                      round.shadow.first_view == l.shadow.first_view && detail::SameFloat(round.shadow.weight, l.shadow.weight);
    if (!same) {
      Add(&fail, std::format("slot {}: the GPU twin does not hold what the light says", i));
    }
  }
  for (uint32_t r = 0; r < kRoleCount; ++r) {
    const uint32_t n = frame.role_count[r];
    if (n > kMaxLights) {
      Add(&fail, std::format("role {} counts {} lights", r, n));
      continue;
    }
    for (uint32_t k = 0; k < n; ++k) {
      const uint32_t slot = frame.order[r][k];
      if (slot >= frame.count || frame.lights[slot].id == kNoLightId || (frame.lights[slot].roles & RoleBit(LightRole(r))) == 0) {
        Add(&fail, std::format("role {} rank {} is slot {}, which is not a light of that role", r, k, slot));
      }
    }
  }
  sc->Note(Check::kFrame, fail);
}

}  // namespace larecomp::gpu::light_manager
