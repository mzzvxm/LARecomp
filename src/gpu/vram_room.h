#pragma once
// ===========================================================================
// MCLA GPU renderer -- "is there room in video memory for this?" (pure rules)
// ===========================================================================
// What a creation that cannot be trimmed for asks before it is made, with no renderer, no SDK and no
// GPU, so that vram_room_test.cpp checks it alone:
//
//   HasRoom        the line vram_budget.h keeps (VideoMemoryBudget::HasRoomFor is this): the use plus
//                  what would be made must stay under kHighPercent of the OS budget, the share where
//                  the caches start releasing what they hold. Frame generation (frame_generation.cpp)
//                  and the FidelityFX upscaler (temporal_ffx.cpp) ask it before their contexts, so that
//                  making one never makes the response release textures and targets to fit it.
//   UpscalerNeed   what an upscaler context asks for: the runtime's own figure for the context plus the
//                  image it writes into (RGBA16F at the output's size). 0 when the context's size is
//                  not known: nothing is then refused for it (the creation itself is the judge, as it
//                  was before this check).
//   ScaledEstimate the runtime has no way to say a context's size for sizes it has not made one for,
//                  except through a query that a provider may not answer; the last context it made and
//                  reported on, scaled by the pixels, is the answer then (frame generation does the same).
//   RefusalBook    what was refused for want of room, per request, and until when: the first wait is
//                  kFirstWaitMs, each time the same request is refused again after its wait the next one
//                  is twice as long, up to kMaxWaitMs, so that a card that stays full is not asked every
//                  frame and a freed one is noticed within a minute.
//   FitKind        the upscaler the settings can have when some are refused for want of room: FSR 4
//                  falls to FSR 3.1, which falls to none.
//
// Threading: none of this has state beyond what a caller keeps in a RefusalBook.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace mcla::gpu::vram_room {

// The share of the budget in use where the caches start releasing (vram_budget.h kHighPercent).
inline constexpr uint32_t kHighPercent = 90;

// `extra` more bytes keep the use under the line. True when the adapter reports no budget (0): there is
// nothing to compare with, and a creation that fails is that adapter's only sign.
constexpr bool HasRoom(uint64_t used, uint64_t budget, uint64_t extra,
                       uint32_t high_percent = kHighPercent) {
  return budget == 0 || (used + extra) * 100 < budget * high_percent;
}

// R16G16B16A16_FLOAT: what the upscaler writes its picture into (temporal_backend.h TemporalInputs).
inline constexpr uint64_t kOutputBytesPerPixel = 8;

constexpr uint64_t OutputImageBytes(uint32_t width, uint32_t height) {
  return uint64_t(width) * height * kOutputBytesPerPixel;
}

// The video memory an upscaler context for an output of `out_w` x `out_h` asks for, `context_bytes`
// being the runtime's figure for the context; 0 while that is not known.
constexpr uint64_t UpscalerNeed(uint64_t context_bytes, uint32_t out_w, uint32_t out_h) {
  return context_bytes == 0 ? 0 : context_bytes + OutputImageBytes(out_w, out_h);
}

// The last measurement scaled to `pixels`; 0 when there is none.
constexpr uint64_t ScaledEstimate(uint64_t measured_bytes, uint64_t measured_pixels,
                                  uint64_t pixels) {
  if (measured_bytes == 0 || measured_pixels == 0 || pixels == 0) {
    return 0;
  }
  return measured_bytes * pixels / measured_pixels;
}

// ---- what was refused, and until when -----------------------------------------------------------

inline constexpr uint64_t kFirstWaitMs = 5000;
inline constexpr uint64_t kMaxWaitMs = 60000;

constexpr uint64_t NextWaitMs(uint64_t previous_ms) {
  return previous_ms == 0 ? kFirstWaitMs : std::min(previous_ms * 2, kMaxWaitMs);
}

// A few requests at a time (the running slot's and the prepared one's, plus the lower upscaler's that a
// refusal makes the caller try): the oldest entry makes room for a new one. `Key` needs operator==.
template <class Key, size_t kCapacity = 8>
class RefusalBook {
 public:
  // The request was refused at `now_ms`: it is blocked for the next wait of its series. Returns that
  // wait (ms).
  uint64_t Refuse(const Key& key, uint64_t now_ms) {
    Entry* entry = Find(key);
    if (!entry) {
      entry = Take();
      entry->key = key;
      entry->used = true;
      entry->wait_ms = 0;
    }
    entry->wait_ms = NextWaitMs(entry->wait_ms);
    entry->until_ms = now_ms + entry->wait_ms;
    entry->order = ++counter_;
    return entry->wait_ms;
  }
  // Whether the request is waiting out a refusal at `now_ms`. After the wait it is not: the next
  // creation is asked for again, and refused again (Refuse: a longer wait) or made (Clear).
  bool Blocked(const Key& key, uint64_t now_ms) const {
    const Entry* entry = Find(key);
    return entry && now_ms < entry->until_ms;
  }
  // Whether the request has been refused and not made since (its wait may be over).
  bool Known(const Key& key) const { return Find(key) != nullptr; }
  // Milliseconds left of the request's wait; 0 when it is not blocked.
  uint64_t LeftMs(const Key& key, uint64_t now_ms) const {
    const Entry* entry = Find(key);
    return entry && now_ms < entry->until_ms ? entry->until_ms - now_ms : 0;
  }
  // The request was made: its series is over.
  void Clear(const Key& key) {
    if (Entry* entry = Find(key)) {
      *entry = Entry{};
    }
  }
  void ClearAll() { entries_ = {}; }
  size_t Count() const {
    size_t n = 0;
    for (const Entry& e : entries_) {
      n += e.used ? 1 : 0;
    }
    return n;
  }

 private:
  struct Entry {
    Key key{};
    bool used = false;
    uint64_t wait_ms = 0;
    uint64_t until_ms = 0;
    uint64_t order = 0;
  };
  Entry* Find(const Key& key) {
    for (Entry& e : entries_) {
      if (e.used && e.key == key) {
        return &e;
      }
    }
    return nullptr;
  }
  const Entry* Find(const Key& key) const {
    for (const Entry& e : entries_) {
      if (e.used && e.key == key) {
        return &e;
      }
    }
    return nullptr;
  }
  Entry* Take() {
    Entry* oldest = &entries_[0];
    for (Entry& e : entries_) {
      if (!e.used) {
        return &e;
      }
      if (e.order < oldest->order) {
        oldest = &e;
      }
    }
    *oldest = Entry{};
    return oldest;
  }
  std::array<Entry, kCapacity> entries_{};
  uint64_t counter_ = 0;
};

// ---- the upscaler that fits ------------------------------------------------------------------------

// The upscalers as gpu.h's enum numbers them (temporal.cpp static_asserts it).
inline constexpr uint8_t kKindOff = 0;
inline constexpr uint8_t kKindFsr31 = 1;
inline constexpr uint8_t kKindFsr4 = 2;
inline constexpr uint8_t kKindDlss = 3;

// The next upscaler down when one does not fit: FSR 4 (its context is the larger one) to FSR 3.1, which
// and every other one to none.
constexpr uint8_t LowerKind(uint8_t kind) {
  return kind == kKindFsr4 ? kKindFsr31 : kKindOff;
}

// `kind`, or the first upscaler below it that can run and is not refused for want of room; none when
// there is not one. `refused(kind)` and `can_run(kind)` are the caller's.
template <class RefusedFn, class CanRunFn>
constexpr uint8_t FitKind(uint8_t kind, RefusedFn&& refused, CanRunFn&& can_run) {
  while (kind != kKindOff) {
    if (can_run(kind) && !refused(kind)) {
      return kind;
    }
    kind = LowerKind(kind);
  }
  return kKindOff;
}

}  // namespace mcla::gpu::vram_room
