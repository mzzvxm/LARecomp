// MCLA GPU renderer -- capture of the guest's D3D calls. See guest_capture.h.
//
// Design followed: reeot hooks/state.cpp and gpu/draw.cpp (BSD-3-Clause) -- a
// draw packet built from copies of the device's state windows, constants
// deduplicated against the dirty masks and verified against the bank. The
// windows, offsets and event set here are this game's; no code was copied.

#include "guest_capture.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <new>

#include <rex/cvar.h>
#include <rex/graphics/format/ucode.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>
#endif

#include "cycles.h"
#include "draw_decode.h"
#include "guest_memory.h"
#include "native_gfx/guest/guest_fence.h"
#include "occlusion.h"
#include "textures.h"
#include "native_gfx/guest/guest_resources.h"
#include "native_gfx/shader_identity.h"

REXCVAR_DECLARE(bool, mcla_gpu_edram_aliasing);

REXCVAR_DEFINE_BOOL(
    mcla_gpu_readable_cache, true, "MCLA/GPU",
    "Ask the guest heap whether a guest range is readable once a guest frame per 4 KiB page "
    "instead of once a draw: the capture checks about ten ranges a draw, and each check takes "
    "the heap's lock (about 0.2 ms a frame at 2,600 draws). The pages found readable are "
    "forgotten at every present. A page the guest unmaps within the frame and that a later draw "
    "still points at would be read as before the unmapping (the memory stays mapped, so it "
    "cannot fault); off asks the heap at every check, as before.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace mcla::gpu {

namespace nfx = mcla::native_gfx;

namespace {

// Whether a pixel shader can kill pixels: a kill opcode -- vector
// kKillEq..kKillNe, scalar kKillsEq..kKillsOne -- in an ALU instruction of any
// exec clause. The walk is the SDK's Shader::AnalyzeUcode (control flow pairs
// up to the first clause's instructions) and the test its
// GatherAluInstructionInformation (kAluOpChangedStatePixelKill), made on the
// opcodes themselves: the opcode tables live in the SDK's sources. A clause
// beyond the microcode counts as killing: the EDRAM detector may report too
// much, never too little.
bool ProbePixelKill(const uint8_t* bytes, uint32_t size) {
  namespace ucode = rex::graphics::ucode;
  const uint32_t dword_count = size / 4;
  const auto load = [&](uint32_t first, uint32_t* out) {
    for (uint32_t k = 0; k < 3; ++k) {
      out[k] = LoadBe32(bytes + 4 * (first + k));
    }
  };
  uint32_t cf_bound = dword_count / 3;  // control flow pairs, 3 dwords each
  for (uint32_t pair = 0; pair < cf_bound; ++pair) {
    uint32_t dwords[3];
    load(pair * 3, dwords);
    ucode::ControlFlowInstruction cf[2];
    ucode::UnpackControlFlowInstructions(dwords, cf);
    for (const ucode::ControlFlowInstruction& c : cf) {
      uint32_t address = 0;
      uint32_t count = 0;
      uint32_t sequence = 0;
      switch (c.opcode()) {
        case ucode::ControlFlowOpcode::kExec:
        case ucode::ControlFlowOpcode::kExecEnd:
          address = c.exec.address();
          count = c.exec.count();
          sequence = c.exec.sequence();
          break;
        case ucode::ControlFlowOpcode::kCondExec:
        case ucode::ControlFlowOpcode::kCondExecEnd:
        case ucode::ControlFlowOpcode::kCondExecPredClean:
        case ucode::ControlFlowOpcode::kCondExecPredCleanEnd:
          address = c.cond_exec.address();
          count = c.cond_exec.count();
          sequence = c.cond_exec.sequence();
          break;
        case ucode::ControlFlowOpcode::kCondExecPred:
        case ucode::ControlFlowOpcode::kCondExecPredEnd:
          address = c.cond_exec_pred.address();
          count = c.cond_exec_pred.count();
          sequence = c.cond_exec_pred.sequence();
          break;
        default:
          continue;
      }
      cf_bound = std::min(cf_bound, address);
      for (uint32_t k = 0; k < count; ++k) {
        if ((sequence >> (2 * k)) & 1u) {
          continue;  // a fetch
        }
        if ((address + k + 1) * 3 > dword_count) {
          return true;
        }
        uint32_t alu_dwords[3];
        load((address + k) * 3, alu_dwords);
        ucode::AluInstruction alu;
        static_assert(sizeof(alu) == sizeof(alu_dwords));
        std::memcpy(&alu, alu_dwords, sizeof(alu));
        switch (alu.vector_opcode()) {
          case ucode::AluVectorOpcode::kKillEq:
          case ucode::AluVectorOpcode::kKillGt:
          case ucode::AluVectorOpcode::kKillGe:
          case ucode::AluVectorOpcode::kKillNe:
            return true;
          default:
            break;
        }
        switch (alu.scalar_opcode()) {
          case ucode::AluScalarOpcode::kKillsEq:
          case ucode::AluScalarOpcode::kKillsGt:
          case ucode::AluScalarOpcode::kKillsGe:
          case ucode::AluScalarOpcode::kKillsNe:
          case ucode::AluScalarOpcode::kKillsOne:
            return true;
          default:
            break;
        }
      }
    }
  }
  return false;
}

constexpr uint32_t kMaxEventDepth = 4;
// Every offset the capture reads from a device lies below this.
constexpr uint32_t kDeviceReadEnd = guest::kEventWindowEnd;

struct EventSnapshot {
  GuestEvent event = GuestEvent::kClear;
  uint32_t device = 0;
  uint32_t flags = 0;  // EventFlags found before the call
  uint32_t stack[2] = {};
  uint32_t nested_at_begin = 0;
  uint64_t before_cycles = 0;
  EventState state;
};

// Per guest thread: the pre-state of the events in progress on it (an EndTiling
// holds its own while each of its resolves takes the next slot).
thread_local std::unique_ptr<std::array<EventSnapshot, kMaxEventDepth>> t_snapshots;
thread_local uint32_t t_depth = 0;
thread_local uint32_t t_end_tiling_depth = 0;
thread_local uint32_t t_nested_resolves = 0;

inline const uint8_t* GuestBytes(const uint8_t* base, uint32_t address) {
  return rex::memory::GuestPtr<const uint8_t*>(const_cast<uint8_t*>(base), address);
}

inline const uint8_t* DeviceBytes(const uint8_t* base, uint32_t device, uint32_t offset) {
  return GuestBytes(base, device + offset);
}

inline uint32_t DeviceU32(const uint8_t* base, uint32_t device, uint32_t offset) {
  return LoadBe32(DeviceBytes(base, device, offset));
}

inline uint64_t DeviceU64(const uint8_t* base, uint32_t device, uint32_t offset) {
  return uint64_t(DeviceU32(base, device, offset)) << 32 | DeviceU32(base, device, offset + 4);
}

// TSC cycles since `since` (a cycles::Now). The capture times itself twice per
// draw; steady_clock reads QueryPerformanceCounter, about 20 ns a call, the TSC a
// few. The frame's total is turned into time by the code that reads it
// (cycles::ToNs), as the render thread's own per-draw timers are.
uint64_t ElapsedCycles(uint64_t since) {
  const uint64_t now = cycles::Now();
  return now > since ? now - since : 0;
}

// A draw is timed one time in kDrawTimingEvery and its cycles are counted that many times. The
// two TSC reads of a draw cost an estimated 10 to 15 ns together (RDTSC is a slow instruction
// on AMD), about a tenth of a draw's whole capture, and they only feed the capture figure of the
// validation window and of the frame trace. The phase is per guest thread (the guest render
// thread makes nearly every draw), so it takes no lock and shares no cache line. Clears,
// resolves and tiling calls happen about fifty times a frame and are timed every time.
// CHECK: not built or run. That figure is now an estimate from 1 draw in 16 (about 275 timed
// draws a frame at 4,400); its mean over a window should match the old one within a few
// percent. The gain (about 0.04 ms a frame) is read from the profile, not measured.
constexpr uint32_t kDrawTimingEvery = 16;
static_assert((kDrawTimingEvery & (kDrawTimingEvery - 1)) == 0, "the phase test is a mask");
thread_local uint32_t t_draw_timing_phase = 0;

inline bool TimeThisDraw() {
  return (++t_draw_timing_phase & (kDrawTimingEvery - 1)) == 0;
}

// The memo slot of a shader object: the top 10 bits of a multiplicative hash of its address
// (kMemoSlots entries a kind). A vertex shader drawn with no pixel shader can select its second
// variant, so the same object has two entries and the second one's slot flips the lowest bit.
inline uint32_t MemoSlot(uint32_t object, bool vertex, bool ps_null) {
  return ((object * 2654435761u) >> 22) ^ ((vertex && ps_null) ? 1u : 0u);
}

// memcmp(a, b, kConstantBlockBytes) == 0 without a call into vcruntime140: the
// compiler does not expand it, and a draw compares a few blocks (0.03 ms a frame in the
// profile, mostly the call).
inline bool SameBlock(const uint8_t* a, const uint8_t* b) {
  static_assert(guest::kConstantBlockBytes == 64, "four 16-byte compares");
#if defined(_M_X64) || defined(__x86_64__)
  const auto load = [](const uint8_t* p, int k) {
    return _mm_loadu_si128(reinterpret_cast<const __m128i*>(p) + k);
  };
  const __m128i e0 = _mm_cmpeq_epi8(load(a, 0), load(b, 0));
  const __m128i e1 = _mm_cmpeq_epi8(load(a, 1), load(b, 1));
  const __m128i e2 = _mm_cmpeq_epi8(load(a, 2), load(b, 2));
  const __m128i e3 = _mm_cmpeq_epi8(load(a, 3), load(b, 3));
  return _mm_movemask_epi8(_mm_and_si128(_mm_and_si128(e0, e1), _mm_and_si128(e2, e3))) == 0xFFFF;
#else
  return std::memcmp(a, b, guest::kConstantBlockBytes) == 0;
#endif
}

// The deltas of one bank: blocks whose bytes differ from the shadow, among the
// ones the dirty mask names -- or among all of them when verifying. A block
// whose bit is set but whose bytes did not change is not sent.
void CollectDeltas(const uint8_t* bank, const uint8_t* shadow, bool shadow_valid, uint64_t dirty,
                   bool verify, std::array<uint8_t, guest::kConstantBlocksPerBank>& out,
                   uint32_t& count, uint32_t& masked, uint32_t& unmasked) {
  count = 0;
  if (shadow_valid && !verify) {
    // Only a block the mask names can be sent, so visit those alone, lowest
    // block first (bit 63 is block 0) as the full walk below does, instead of
    // testing all 64 bits of both banks at every draw.
    static_assert(guest::kConstantBlocksPerBank == 64, "one mask bit per block");
    for (uint64_t bits = dirty; bits != 0;) {
      const uint32_t i = uint32_t(std::countl_zero(bits));
      bits &= ~(uint64_t(1) << (63 - i));
      const uint32_t at = i * guest::kConstantBlockBytes;
      if (SameBlock(bank + at, shadow + at)) {
        continue;
      }
      out[count++] = uint8_t(i);
      ++masked;
    }
    return;
  }
  for (uint32_t i = 0; i < guest::kConstantBlocksPerBank; ++i) {
    const bool named = ((dirty >> (63 - i)) & 1u) != 0;
    if (shadow_valid && !named && !verify) {
      continue;
    }
    const uint32_t at = i * guest::kConstantBlockBytes;
    if (shadow_valid && SameBlock(bank + at, shadow + at)) {
      continue;
    }
    out[count++] = uint8_t(i);
    if (!shadow_valid || named) {
      ++masked;
    } else {
      ++unmasked;
    }
  }
}

}  // namespace

GuestCapture::GuestCapture(CommandStream& stream, bool verify_constants)
    : stream_(stream),
      verify_constants_(verify_constants),
      page_cache_(REXCVAR_GET(mcla_gpu_readable_cache)) {
  scratch_.reserve(nfx::kMaxUcodeBytes);
}

void GuestCapture::Disable() {
  if (!enabled_.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  if (page_cache_) {
    const uint64_t hits = cache_hits_.load(std::memory_order_relaxed);
    const uint64_t asks = cache_heap_asks_.load(std::memory_order_relaxed);
    REXLOG_INFO("[gpu] readable cache: {} of {} guest range checks answered from the frame's "
                "pages, {} asked the heap",
                hits, hits + asks, asks);
  }
  const uint64_t memo_hits = memo_hits_.load(std::memory_order_relaxed);
  const uint64_t memo_misses = memo_misses_.load(std::memory_order_relaxed);
  REXLOG_INFO("[gpu] shader memo: {} of {} shader identity lookups answered from the frame's "
              "memo, {} looked up in full",
              memo_hits, memo_hits + memo_misses, memo_misses);
}

// ---------------------------------------------------------------------------
// guest reads
// ---------------------------------------------------------------------------

bool GuestCapture::CopyGuest(const uint8_t* base, uint32_t address, void* out, uint32_t bytes) {
  if (address < 0x1000u || bytes == 0 || !Readable(address, bytes)) {
    ++frame_.unreadable;
    return false;
  }
  std::memcpy(out, GuestBytes(base, address), bytes);
  return true;
}

// IsGuestRangeReadable, except that a range lying on pages that an earlier
// check found readable is not asked again. Each check takes the heap's lock and
// walks its page table; a draw makes about ten, mostly on the same few pages (a
// shader object and its sub-object, a declaration and its elements), and from
// one draw to the next mostly on the same pages again (the same shaders and
// declarations).
// Two memories, both of 4 KiB pages: no heap has a smaller page (they are
// 4 KiB, 64 KiB or 16 MiB) and a protection is per heap page, so a page that
// lies inside a checked range is as readable as the range.
//  - verified_: the runs found during the current GatherDraw. Gone when it
//    returns, so a change of the guest's protections between draws is seen by
//    the next draw's first check.
//  - frame_pages_ (mcla_gpu_readable_cache): the pages found during the guest
//    frame, forgotten at the present. The heap's answer is the guest's own
//    protection bits of the page, and the host memory behind a page stays
//    mapped after the guest releases it (BaseHeap::Decommit and Release leave
//    the host pages alone), so an answer that is a few draws old can only
//    differ for a draw that points at memory the guest freed within the frame.
// Only ranges of up to kMaxPages pages inside one 16 MiB region are kept in
// frame_pages_. Heaps meet only on 16 MiB boundaries (the last one ends at
// 0xFFD00000 with nothing behind it, so a page past its end is never found
// readable), which makes a page-by-page answer the answer for the range
// (QueryRangeAccess refuses a range that crosses a heap's end).
bool GuestCapture::Readable(uint32_t address, uint64_t size) {
  if (address < 0x1000u || size == 0 || uint64_t(address) + size > 0x100000000ull) {
    return nfx::IsGuestRangeReadable(address, size);
  }
  const uint32_t first = address >> 12;
  const uint32_t last = uint32_t((uint64_t(address) + size - 1) >> 12);
  if (verified_.active) {
    for (uint32_t i = 0; i < verified_.count; ++i) {
      if (verified_.first[i] <= first && last <= verified_.last[i]) {
        return true;
      }
    }
  }
  const bool keep = page_cache_ && last - first < FramePages::kMaxPages &&
                    (first >> 12) == (last >> 12);
  const uint64_t generation = uint64_t(frame_pages_.generation) << 32;
  const auto slot_of = [](uint32_t page) { return (page * 2654435761u) >> 21; };
  static_assert(FramePages::kSlots == (1u << 11), "slot_of keeps the top 11 bits");
  if (keep) {
    uint32_t page = first;
    while (page <= last && frame_pages_.slot[slot_of(page)] == (generation | page)) {
      ++page;
    }
    if (page > last) {
      cache_hits_.store(cache_hits_.load(std::memory_order_relaxed) + 1,
                        std::memory_order_relaxed);
      return true;
    }
  }
  if (page_cache_) {
    cache_heap_asks_.store(cache_heap_asks_.load(std::memory_order_relaxed) + 1,
                           std::memory_order_relaxed);
  }
  if (!nfx::IsGuestRangeReadable(address, size)) {
    return false;
  }
  if (keep) {
    for (uint32_t page = first; page <= last; ++page) {
      frame_pages_.slot[slot_of(page)] = generation | page;
    }
  }
  if (verified_.active) {
    const uint32_t slot = verified_.next++ % VerifiedPages::kRuns;
    verified_.first[slot] = first;
    verified_.last[slot] = last;
    verified_.count = std::min(verified_.count + 1, VerifiedPages::kRuns);
  }
  return true;
}

void GuestCapture::NextFramePages() {
  if (++frame_pages_.generation == 0) {
    // After 4 billion frames: no slot may match a generation that comes round again.
    std::fill(std::begin(frame_pages_.slot), std::end(frame_pages_.slot), uint64_t{0});
    vs_memo_.fill(IdentityMemo{});
    ps_memo_.fill(IdentityMemo{});
    frame_pages_.generation = 1;
  }
}

GuestCapture::DeviceState& GuestCapture::StateFor(uint32_t device) {
  if (last_state_ != nullptr && last_state_->device == device) {
    return *last_state_;
  }
  for (auto& state : devices_) {
    if (state->device == device) {
      last_state_ = state.get();
      return *state;
    }
  }
  devices_.push_back(std::make_unique<DeviceState>());
  devices_.back()->device = device;
  last_state_ = devices_.back().get();
  if (devices_.size() > 1 && !reported_device_count_) {
    // Expected only if the game's command-buffer cache records on its own
    // device; OnGuestFrameEnd keeps that cache off.
    reported_device_count_ = true;
    REXLOG_WARN("[gpu] capture: a second D3D device 0x{:08X} is drawing (first was 0x{:08X})",
                device, devices_.front()->device);
  }
  return *devices_.back();
}

GuestCapture::ShaderRead GuestCapture::ReadPixelShader(const uint8_t* base, uint32_t ps_object) {
  // sub_82424670: sub = ps + u32(ps+64); address = u32(sub+40) + u32(ps+24);
  // size = u32(sub+44).
  ShaderRead r;
  r.object = ps_object;
  uint8_t object[guest::kPsSubObject + 4];
  if (!CopyGuest(base, ps_object, object, sizeof(object))) {
    return r;
  }
  const uint32_t sub = ps_object + LoadBe32(object + guest::kPsSubObject);
  uint8_t fields[8];
  if (!CopyGuest(base, sub + guest::kPsUcodeAddress, fields, sizeof(fields))) {
    return r;
  }
  r.address = LoadBe32(fields) + LoadBe32(object + guest::kPsUcodeBase);
  r.size = LoadBe32(fields + 4);
  r.readable = r.size != 0 && r.size <= nfx::kMaxUcodeBytes && r.address >= 0x1000u &&
               Readable(r.address, r.size);
  if (!r.readable) {
    ++frame_.unreadable;
  }
  return r;
}

GuestCapture::ShaderRead GuestCapture::ReadVertexShader(const uint8_t* base, uint32_t device,
                                                        uint32_t ps_object, uint32_t* variant) {
  // sub_82424670: variant 1 only with no PS bound and u32(vs+872) & 0x20;
  // off = u32(vs + 896 + 8*variant); address = u32(vs+off+872) + u32(vs+32);
  // size = u32(vs+off+876).
  ShaderRead r;
  *variant = 0;
  r.object = DeviceU32(base, device, guest::kDevVertexShader);
  if (r.object == 0) {
    return r;
  }
  // Only the bytes the driver reads: variant 1's entry is read only when it is
  // the one in use, as sub_82424670 does, so a smaller object ending at a page
  // boundary is not reported unreadable for nothing.
  uint8_t object[guest::kVsVariantTable + 4];
  if (!CopyGuest(base, r.object, object, sizeof(object))) {
    return r;
  }
  if (ps_object == 0 &&
      (LoadBe32(object + guest::kVsVariantFlags) & guest::kVsVariantFlagHasDepthOnly) != 0) {
    *variant = 1;
  }
  uint32_t off = LoadBe32(object + guest::kVsVariantTable);
  if (*variant == 1) {
    uint8_t entry[4];
    if (!CopyGuest(base, r.object + guest::kVsVariantTable + guest::kVsVariantStride, entry, 4)) {
      return r;
    }
    off = LoadBe32(entry);
  }
  if (off == 0) {
    return r;
  }
  uint8_t fields[8];
  if (!CopyGuest(base, r.object + off + guest::kVsUcodeAddress, fields, sizeof(fields))) {
    return r;
  }
  r.address = LoadBe32(fields) + LoadBe32(object + guest::kVsUcodeBase);
  r.size = LoadBe32(fields + 4);
  r.readable = r.size != 0 && r.size <= nfx::kMaxUcodeBytes && r.address >= 0x1000u &&
               Readable(r.address, r.size);
  if (!r.readable) {
    ++frame_.unreadable;
  }
  return r;
}

bool GuestCapture::ShaderFromMemo(uint32_t object, bool vertex, bool ps_null, ShaderRead& shader,
                                  uint32_t* variant) {
  const IdentityMemo& memo = (vertex ? vs_memo_ : ps_memo_)[MemoSlot(object, vertex, ps_null)];
  if (memo.object != object || memo.generation != frame_pages_.generation ||
      memo.ps_null != (vertex && ps_null)) {
    return false;
  }
  memo_hits_.store(memo_hits_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  shader.object = object;
  shader.address = memo.address;
  shader.size = memo.size;
  shader.identity = memo.identity;
  shader.computed_fetch_index = memo.computed_fetch_index;
  shader.kills_pixels = memo.kills_pixels;
  shader.readable = true;  // the first resolve of this frame read the microcode
  if (variant != nullptr) {
    *variant = memo.variant;
  }
  return true;
}

void GuestCapture::ResolveIdentity(const uint8_t* base, ShaderRead& shader, bool vertex,
                                   bool ps_null, uint32_t variant) {
  IdentityMemo& memo =
      (vertex ? vs_memo_ : ps_memo_)[MemoSlot(shader.object, vertex, ps_null)];
  static_assert(kMemoSlots == (1u << 10), "the slot is the top 10 bits of the product");
  const uint32_t generation = frame_pages_.generation;
  if (memo.object == shader.object && memo.generation == generation &&
      memo.address == shader.address && memo.size == shader.size &&
      memo.ps_null == (vertex && ps_null)) {
    memo_hits_.store(memo_hits_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    shader.identity = memo.identity;
    shader.computed_fetch_index = memo.computed_fetch_index;
    shader.kills_pixels = memo.kills_pixels;
    return;
  }
  memo_misses_.store(memo_misses_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  ResolveIdentityFull(base, shader, vertex);
  memo.object = shader.object;
  memo.generation = generation;
  memo.address = shader.address;
  memo.size = shader.size;
  memo.identity = shader.identity;
  memo.computed_fetch_index = shader.computed_fetch_index;
  memo.kills_pixels = shader.kills_pixels;
  memo.ps_null = vertex && ps_null;
  memo.variant = uint8_t(vertex ? variant : 0);
}

void GuestCapture::ResolveIdentityFull(const uint8_t* base, ShaderRead& shader, bool vertex) {
  const uint8_t* ucode = GuestBytes(base, shader.address);
  const uint64_t key = uint64_t(shader.address) << 32 | shader.size;
  auto found = identities_.find(key);
  if (found != identities_.end()) {
    IdentityEntry& e = found->second;
    for (auto it = e.raw.rbegin(); it != e.raw.rend(); ++it) {
      if (std::memcmp(it->data(), ucode, shader.size) == 0) {
        shader.identity = e.identity;
        shader.computed_fetch_index = e.computed_fetch_index;
        shader.kills_pixels = e.kills_pixels;
        return;
      }
    }
    // New bytes at a known place: a vfetch repatch for another declaration
    // keeps the normalized form; anything else is a different shader.
    scratch_.assign(ucode, ucode + shader.size);
    nfx::NormalizeVertexFetches(scratch_.data(), scratch_.size());
    if (scratch_ == e.normalized) {
      if (e.raw.size() >= kMaxRawVariants) {
        e.raw.erase(e.raw.begin());
      }
      e.raw.emplace_back(ucode, ucode + shader.size);
      ++frame_.identity_renormalized;
      shader.identity = e.identity;
      shader.computed_fetch_index = e.computed_fetch_index;
      shader.kills_pixels = e.kills_pixels;
      return;
    }
    identities_.erase(found);
  } else {
    scratch_.assign(ucode, ucode + shader.size);
    nfx::NormalizeVertexFetches(scratch_.data(), scratch_.size());
  }
  if (identities_.size() >= kMaxIdentityEntries) {
    // A cache, so dropping it is always correct; it only costs re-hashing what
    // is still bound. Reached only if the game keeps loading microcode at new
    // addresses for a very long session.
    REXLOG_WARN("[gpu] capture: identity cache reached {} entries; cleared", identities_.size());
    identities_.clear();
  }
  IdentityEntry e;
  e.raw.emplace_back(ucode, ucode + shader.size);
  e.normalized = scratch_;
  e.identity = nfx::Fnv1a64(scratch_.data(), scratch_.size());
  e.computed_fetch_index = vertex && nfx::ProbeComputedVertexFetchIndex(ucode, shader.size);
  // Only the EDRAM detector asks.
  e.kills_pixels = !vertex && REXCVAR_GET(mcla_gpu_edram_aliasing) &&
                   ProbePixelKill(ucode, shader.size);
  shader.identity = e.identity;
  shader.computed_fetch_index = e.computed_fetch_index;
  shader.kills_pixels = e.kills_pixels;
  identities_.emplace(key, std::move(e));
  ++frame_.identity_new;
}

// ---------------------------------------------------------------------------
// draws
// ---------------------------------------------------------------------------

GuestCallToken GuestCapture::BeforeDraw(const uint8_t* base, uint32_t device) {
  GuestCallToken token;
  if (!enabled() || base == nullptr || device < 0x1000u) {
    return token;
  }
  // The original zeroes both masks while flushing them.
  token.vs_dirty = DeviceU64(base, device, guest::kDevVsConstantDirty);
  token.ps_dirty = DeviceU64(base, device, guest::kDevPsConstantDirty);
  token.active = true;
  return token;
}

void GuestCapture::GatherDraw(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                              DrawCapture& c) {
  // Readable() remembers the pages it found readable until this call returns.
  verified_.count = 0;
  verified_.next = 0;
  verified_.active = true;
  struct Deactivate {
    VerifiedPages& pages;
    ~Deactivate() { pages.active = false; }
  } deactivate{verified_};
  // A shader the guest frame's memo already has (nearly every draw: 97% of the lookups in the
  // profiled roam and drive) is taken from it without reading the shader object, its sub-object
  // or the microcode, and without asking the heap about any of them; the rest read the guest's
  // objects and resolve the identity, which stores the memo entry.
  const uint32_t ps_object = DeviceU32(base, device, guest::kDevPixelShader);
  const bool ps_null = ps_object == 0;
  if (ps_null) {
    c.flags |= kDrawNoPixelShader;
  } else {
    if (!ShaderFromMemo(ps_object, false, false, c.ps, nullptr)) {
      c.ps = ReadPixelShader(base, ps_object);
      if (c.ps.readable) {
        ResolveIdentity(base, c.ps, false, false, 0);
      }
    }
    if (c.ps.readable) {
      if (c.ps.kills_pixels) {
        c.flags |= kDrawPsKills;
      }
    } else {
      c.flags |= kDrawPsUnreadable;
    }
  }
  const uint32_t vs_object = DeviceU32(base, device, guest::kDevVertexShader);
  if (vs_object == 0 || !ShaderFromMemo(vs_object, true, ps_null, c.vs, &c.vs_variant)) {
    c.vs = ReadVertexShader(base, device, ps_object, &c.vs_variant);
    if (c.vs.readable) {
      ResolveIdentity(base, c.vs, true, ps_null, c.vs_variant);
    }
  }
  if (c.vs.readable) {
    if (c.vs.computed_fetch_index) {
      c.flags |= kDrawVsComputedFetchIndex;
    }
  } else {
    c.flags |= kDrawVsUnreadable;
  }

  c.decl_object = DeviceU32(base, device, guest::kDevVertexDeclaration);
  if (c.decl_object != 0) {
    uint8_t count_bytes[4];
    if (!CopyGuest(base, c.decl_object + nfx::kDeclOffsetElementCount, count_bytes, 4)) {
      c.flags |= kDrawDeclUnreadable;
    } else {
      const uint32_t count = LoadBe32(count_bytes);
      if (count > guest::kMaxDeclElements) {
        c.flags |= kDrawDeclTooLong;
      } else if (count != 0 &&
                 !Readable(c.decl_object + nfx::kDeclOffsetElements,
                           count * guest::kDeclElementBytes)) {
        c.flags |= kDrawDeclUnreadable;
        ++frame_.unreadable;
      } else {
        c.decl_count = count;
      }
    }
  }

  if (c.kind == DrawKind::kIndexed) {
    c.index_object = DeviceU32(base, device, guest::kDevIndexBuffer);
    uint8_t object[guest::kIndexBufferBytes];
    if (c.index_object != 0 && CopyGuest(base, c.index_object, object, sizeof(object))) {
      c.index_dword0 = LoadBe32(object);
      c.index_address = LoadBe32(object + guest::kIndexBufferAddressOffset);
    } else {
      c.flags |= kDrawIndexUnreadable;
    }
  }

  DeviceState& state = StateFor(device);
  c.state = &state;
  CollectDeltas(DeviceBytes(base, device, guest::kDevVsConstantBank), state.vs_shadow.data(),
                state.shadows_valid, token.vs_dirty, verify_constants_, c.vs_blocks,
                c.vs_block_count, frame_.constant_blocks_masked, frame_.constant_blocks_unmasked);
  CollectDeltas(DeviceBytes(base, device, guest::kDevPsConstantBank), state.ps_shadow.data(),
                state.shadows_valid, token.ps_dirty, verify_constants_, c.ps_blocks,
                c.ps_block_count, frame_.constant_blocks_masked, frame_.constant_blocks_unmasked);
  const uint32_t check_every = verify_constants_ ? kBankCheckInterval : kBankCheckIntervalQuiet;
  if (state.check_next || ++state.draws_since_check >= check_every) {
    c.bank_check = true;
    state.check_next = false;
    state.draws_since_check = 0;
  }
}

void GuestCapture::WriteDraw(const uint8_t* base, uint32_t device, const DrawCapture& c,
                             uint8_t* payload) {
  const DrawRecordLayout layout =
      ComputeDrawLayout(c.decl_count, c.vs_block_count, c.ps_block_count, c.inline_bytes);
  // Default-initialized: every member but the two windows has an initializer
  // (zero), and the windows are copied whole below.
  auto* r = new (payload) DrawRecord;
  r->device = device;
  r->kind = c.kind;
  r->vs_variant = uint8_t(c.vs_variant);
  r->decl_count = uint16_t(c.decl_count);
  r->primitive = c.primitive;
  r->count = c.count;
  r->start = c.start;
  r->base_vertex = c.base_vertex;
  r->inline_stride = c.inline_stride;
  r->inline_bytes = c.inline_bytes;
  r->flags = c.flags;
  r->index_object = c.index_object;
  r->index_dword0 = c.index_dword0;
  r->index_address = c.index_address;
  r->vs_object = c.vs.object;
  r->ps_object = c.ps.object;
  r->vs_ucode_address = c.vs.address;
  r->vs_ucode_size = c.vs.size;
  r->ps_ucode_address = c.ps.address;
  r->ps_ucode_size = c.ps.size;
  r->vs_identity = c.vs.identity;
  r->ps_identity = c.ps.identity;
  r->decl_object = c.decl_object;
  r->vs_delta_count = uint16_t(c.vs_block_count);
  r->ps_delta_count = uint16_t(c.ps_block_count);
  r->viz_query_index = *DeviceBytes(base, device, guest::kDevVizQueryIndex);
  r->viz_query_id = *DeviceBytes(base, device, guest::kDevVizQueryTable + r->viz_query_index);
  r->device_flags = DeviceU32(base, device, guest::kDevFlags);
  // One translation of the table's address serves its words (GuestPtr compares and adds the
  // 0xE0 heap's offset at every call; the table lies inside the device object).
  const uint8_t* colors = DeviceBytes(base, device, guest::kDevColorSurfaces);
  for (uint32_t i = 0; i < 4; ++i) {
    r->surfaces[i] = LoadBe32(colors + 4 * i);
  }
  r->surfaces[4] = DeviceU32(base, device, guest::kDevDepthSurface);
  {
    // The size of the image the draw's targets hold: inside a predicated
    // tiling section the section's extent (the surfaces declare one tile),
    // otherwise the declared size of the surface that sizes them -- its header
    // read when that surface changes. As EventImageSizeBits for the events.
    const uint8_t tiling_flags = *DeviceBytes(base, device, guest::kDevFlags);
    bool tiling = (tiling_flags & 0x10) != 0;
    if (!tiling && (tiling_flags & 0x20) != 0) {
      tiling = true;
      for (uint32_t i = 0; i < guest::kSurfaceCount; ++i) {
        if (r->surfaces[i] != 0 &&
            DeviceU32(base, device, guest::kDevSurfaceCache + 4 * i) != r->surfaces[i]) {
          tiling = false;
        }
      }
    }
    if (tiling) {
      r->target_size_bits = SizeBitsOf(DeviceU32(base, device, guest::kDevTilingExtent),
                                       DeviceU32(base, device, guest::kDevTilingExtent + 4));
    } else {
      DeviceState& sized = *c.state;
      const uint32_t surface = r->surfaces[0] ? r->surfaces[0] : r->surfaces[4];
      if (surface != sized.sized_surface) {
        sized.sized_surface = surface;
        sized.sized_surface_bits = 0;
        if (surface >= 0x1000u &&
            nfx::IsGuestRangeReadable(surface, guest::kSurfaceHeaderBytes)) {
          sized.sized_surface_bits =
              LoadBe32(GuestBytes(base, surface + guest::kSurfaceSizeBitsOffset));
        }
      }
      r->target_size_bits = sized.sized_surface_bits;
    }
  }
  const uint8_t* samplers = DeviceBytes(base, device, guest::kDevSamplerTextures);
  for (uint32_t i = 0; i < guest::kSamplerCount; ++i) {
    r->sampler_textures[i] = LoadBe32(samplers + 4 * i);
  }
  std::memcpy(r->stream_strides, DeviceBytes(base, device, guest::kDevStreamStrides),
              sizeof(r->stream_strides));
  std::memcpy(r->fetch, DeviceBytes(base, device, guest::kDevFetchShadow), sizeof(r->fetch));
  std::memcpy(r->registers, DeviceBytes(base, device, guest::kRegisterWindowBegin),
              sizeof(r->registers));

  if (c.decl_count != 0) {
    std::memcpy(payload + layout.decl, GuestBytes(base, c.decl_object + nfx::kDeclOffsetElements),
                c.decl_count * guest::kDeclElementBytes);
  }

  DeviceState& state = *c.state;
  const uint8_t* vs_bank = DeviceBytes(base, device, guest::kDevVsConstantBank);
  const uint8_t* ps_bank = DeviceBytes(base, device, guest::kDevPsConstantBank);
  std::memcpy(payload + layout.vs_indices, c.vs_blocks.data(), c.vs_block_count);
  std::memcpy(payload + layout.ps_indices, c.ps_blocks.data(), c.ps_block_count);
  for (uint32_t k = 0; k < c.vs_block_count; ++k) {
    const uint32_t at = c.vs_blocks[k] * guest::kConstantBlockBytes;
    std::memcpy(payload + layout.vs_blocks + k * guest::kConstantBlockBytes, vs_bank + at,
                guest::kConstantBlockBytes);
    std::memcpy(state.vs_shadow.data() + at, vs_bank + at, guest::kConstantBlockBytes);
  }
  for (uint32_t k = 0; k < c.ps_block_count; ++k) {
    const uint32_t at = c.ps_blocks[k] * guest::kConstantBlockBytes;
    std::memcpy(payload + layout.ps_blocks + k * guest::kConstantBlockBytes, ps_bank + at,
                guest::kConstantBlockBytes);
    std::memcpy(state.ps_shadow.data() + at, ps_bank + at, guest::kConstantBlockBytes);
  }
  state.shadows_valid = true;
  if (c.bank_check) {
    r->flags |= kDrawBankCheck;
    r->bank_check_vs = XXH3_64bits(vs_bank, guest::kConstantBankBytes);
    r->bank_check_ps = XXH3_64bits(ps_bank, guest::kConstantBankBytes);
  }
}

void GuestCapture::CountRecord(uint32_t payload_bytes) {
  ++frame_.records;
  frame_.bytes += AlignRecord(uint32_t(sizeof(RecordHeader)) + payload_bytes);
}

void GuestCapture::AfterDraw(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                             DrawKind kind, uint32_t primitive, uint32_t count, uint32_t start,
                             int32_t base_vertex) {
  if (!token.active || !enabled()) {
    return;
  }
  const bool timed = TimeThisDraw();
  const uint64_t t0 = timed ? cycles::Now() : 0;
  CommandStream::Writer writer(stream_);  // the producer lock: the capture's one lock
  if (pending_.active && pending_.device == device) {
    FlushPendingInline(writer, base, /*terminated=*/false);
  }
  DrawCapture c;
  c.kind = kind;
  c.primitive = primitive;
  c.count = count;
  c.start = start;
  c.base_vertex = base_vertex;
  GatherDraw(token, base, device, c);
  const DrawRecordLayout layout =
      ComputeDrawLayout(c.decl_count, c.vs_block_count, c.ps_block_count, 0);
  uint8_t* payload = writer.Begin(RecordType::kDraw, layout.payload_bytes);
  if (payload) {
    WriteDraw(base, device, c, payload);
    writer.Commit(Publication::kBatched);
    CountRecord(layout.payload_bytes);
    ++frame_.draws;
  }
  if (timed) {
    frame_.capture_cycles += ElapsedCycles(t0) * kDrawTimingEvery;
  }
}

void GuestCapture::AfterBeginVertices(const GuestCallToken& token, const uint8_t* base,
                                      uint32_t device, uint32_t primitive, uint32_t count,
                                      uint32_t stride, uint32_t buffer) {
  if (!token.active || !enabled()) {
    return;
  }
  const bool timed = TimeThisDraw();
  const uint64_t t0 = timed ? cycles::Now() : 0;
  CommandStream::Writer writer(stream_);  // the lock; no record is begun through it here
  if (pending_.active) {
    // A second Begin before End on the same device is a guest bug; on another
    // device it is a second thread. Either way the first is closed here.
    FlushPendingInline(writer, base, /*terminated=*/false);
  }
  const uint64_t bytes = uint64_t(count) * stride;
  // The whole record has to fit, and it is checked before anything is gathered:
  // gathering updates the constant shadows, which must only happen for a record
  // that will reach the stream. The fixed part and the largest variable parts
  // are bounded, so this bound is exact enough to decide up front.
  const uint64_t worst = uint64_t(ComputeDrawLayout(guest::kMaxDeclElements,
                                                    guest::kConstantBlocksPerBank,
                                                    guest::kConstantBlocksPerBank, 0)
                                      .payload_bytes) +
                         bytes + kRecordAlignment;
  if (worst > stream_.max_payload()) {
    REXLOG_ERROR("[gpu] capture: inline draw of {} vertices x {} bytes exceeds the stream's "
                 "record limit; not captured",
                 count, stride);
    if (timed) {
      frame_.capture_cycles += ElapsedCycles(t0) * kDrawTimingEvery;
    }
    return;
  }
  DrawCapture c;
  c.kind = DrawKind::kInline;
  c.primitive = primitive;
  c.count = count;
  c.inline_stride = stride;
  c.inline_bytes = uint32_t(bytes);
  GatherDraw(token, base, device, c);
  const DrawRecordLayout layout =
      ComputeDrawLayout(c.decl_count, c.vs_block_count, c.ps_block_count, c.inline_bytes);
  pending_.payload.assign(layout.payload_bytes, 0);
  WriteDraw(base, device, c, pending_.payload.data());
  pending_.active = true;
  pending_.device = device;
  pending_.buffer = buffer;
  if (timed) {
    frame_.capture_cycles += ElapsedCycles(t0) * kDrawTimingEvery;
  }
}

void GuestCapture::AfterEndVertices(const uint8_t* base, uint32_t device) {
  if (!enabled()) {
    return;
  }
  const bool timed = TimeThisDraw();
  const uint64_t t0 = timed ? cycles::Now() : 0;
  CommandStream::Writer writer(stream_);
  if (pending_.active && pending_.device == device) {
    FlushPendingInline(writer, base, /*terminated=*/true);
  }
  if (timed) {
    frame_.capture_cycles += ElapsedCycles(t0) * kDrawTimingEvery;
  }
}

void GuestCapture::FlushPendingInline(CommandStream::Writer& writer, const uint8_t* base,
                                      bool terminated) {
  pending_.active = false;
  auto* r = reinterpret_cast<DrawRecord*>(pending_.payload.data());
  const DrawRecordLayout layout =
      ComputeDrawLayout(r->decl_count, r->vs_delta_count, r->ps_delta_count, r->inline_bytes);
  if (!terminated) {
    // Copied anyway and flagged: what the caller had written by now is all
    // there is, and the render thread decides what an unterminated draw is.
    r->flags |= kDrawInlineUnterminated;
    ++frame_.inline_unterminated;
  }
  if (r->inline_bytes != 0 &&
      !CopyGuest(base, pending_.buffer, pending_.payload.data() + layout.inline_data,
                 r->inline_bytes)) {
    r->flags |= kDrawInlineUnreadable;
  }
  uint8_t* payload = writer.Begin(RecordType::kDraw, uint32_t(pending_.payload.size()));
  if (payload) {
    std::memcpy(payload, pending_.payload.data(), pending_.payload.size());
    writer.Commit(Publication::kBatched);
    CountRecord(uint32_t(pending_.payload.size()));
    ++frame_.draws;
  }
}

// ---------------------------------------------------------------------------
// clear, resolve, tiling
// ---------------------------------------------------------------------------

GuestCallToken GuestCapture::BeforeEvent(const uint8_t* base, uint32_t device, GuestEvent event,
                                         uint32_t stack_pointer) {
  GuestCallToken token;
  if (!enabled() || base == nullptr || device < 0x1000u) {
    return token;
  }
  if (t_depth >= kMaxEventDepth) {
    static bool reported = false;
    if (!reported) {
      reported = true;
      REXLOG_ERROR("[gpu] capture: guest events nested deeper than {}; the inner one is not "
                   "captured",
                   kMaxEventDepth);
    }
    return token;
  }
  const uint64_t t0 = cycles::Now();
  if (!t_snapshots) {
    t_snapshots = std::make_unique<std::array<EventSnapshot, kMaxEventDepth>>();
  }
  EventSnapshot& s = (*t_snapshots)[t_depth];
  s.event = event;
  s.device = device;
  s.flags = 0;
  s.stack[0] = s.stack[1] = 0;
  std::memcpy(s.state.pre, DeviceBytes(base, device, guest::kEventWindowBegin),
              guest::kEventWindowBytes);
  for (uint32_t i = 0; i < guest::kSurfaceCount; ++i) {
    const uint32_t pointer =
        LoadBe32(s.state.pre + guest::kDevColorSurfaces + 4 * i - guest::kEventWindowBegin);
    std::memset(s.state.surfaces[i], 0, guest::kSurfaceHeaderBytes);
    if (pointer != 0 &&
        (pointer < 0x1000u || !nfx::IsGuestRangeReadable(pointer, guest::kSurfaceHeaderBytes))) {
      s.flags |= kEventSurfaceUnreadable;
    } else if (pointer != 0) {
      std::memcpy(s.state.surfaces[i], GuestBytes(base, pointer), guest::kSurfaceHeaderBytes);
    }
  }
  if (event == GuestEvent::kResolve) {
    const uint32_t first = stack_pointer + guest::kResolveStencilStackOffset;
    const uint32_t bytes = guest::kResolveParamsStackOffset + 4 - guest::kResolveStencilStackOffset;
    if (stack_pointer >= 0x1000u && nfx::IsGuestRangeReadable(first, bytes)) {
      const uint8_t* stack = GuestBytes(base, first);
      s.stack[0] = LoadBe32(stack);
      s.stack[1] = LoadBe32(stack + guest::kResolveParamsStackOffset -
                            guest::kResolveStencilStackOffset);
    } else {
      s.flags |= kEventStackUnreadable;
    }
    if (t_end_tiling_depth > 0) {
      s.flags |= kEventInsideEndTiling;
    }
  }
  if (event == GuestEvent::kTilingEnd) {
    s.nested_at_begin = t_nested_resolves;
    ++t_end_tiling_depth;
  }
  s.before_cycles = ElapsedCycles(t0);
  ++t_depth;
  token.snapshot = t_depth;
  token.active = true;
  return token;
}

namespace {

// The snapshot a token names, popped. Null (and the stack repaired) when the
// token is not the innermost event of this thread -- a hook pairing bug.
EventSnapshot* PopSnapshot(const GuestCallToken& token, GuestEvent expected) {
  if (!token.active || token.snapshot == 0 || !t_snapshots) {
    return nullptr;
  }
  if (token.snapshot != t_depth || (*t_snapshots)[token.snapshot - 1].event != expected) {
    REXLOG_ERROR("[gpu] capture: event bracket mismatch (token {}, depth {}); dropping the event",
                 token.snapshot, t_depth);
    t_depth = token.snapshot - 1;
    return nullptr;
  }
  --t_depth;
  return &(*t_snapshots)[token.snapshot - 1];
}

// The event record's state: the snapshot taken before the call, window R now.
void CopyEventState(const EventSnapshot& s, const uint8_t* base, uint32_t device,
                    EventState& state, uint32_t* flags) {
  std::memcpy(state.pre, s.state.pre, sizeof(state.pre));
  std::memcpy(state.surfaces, s.state.surfaces, sizeof(state.surfaces));
  std::memcpy(state.post, DeviceBytes(base, device, guest::kRegisterWindowBegin),
              sizeof(state.post));
  *flags |= s.flags;
}

}  // namespace

void GuestCapture::AfterClear(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                              uint32_t caller, uint32_t flags, uint32_t rect, uint32_t color,
                              float z, uint32_t stencil) {
  EventSnapshot* s = PopSnapshot(token, GuestEvent::kClear);
  if (!s || !enabled()) {
    return;
  }
  const uint64_t t0 = cycles::Now();
  CommandStream::Writer writer(stream_);
  if (pending_.active && pending_.device == device) {
    FlushPendingInline(writer, base, /*terminated=*/false);
  }
  uint8_t* payload = writer.Begin(RecordType::kClear, sizeof(ClearRecord));
  if (payload) {
    auto* r = new (payload) ClearRecord();
    r->device = device;
    r->caller = caller;
    r->clear_flags = flags;
    r->z = z;
    r->stencil = stencil;
    r->rect_ptr = rect;
    r->color_ptr = color;
    if (rect != 0) {
      r->flags |= CopyGuest(base, rect, r->rect, sizeof(r->rect)) ? kEventHasRect
                                                                    : kEventRectUnreadable;
    }
    if (color != 0) {
      r->flags |= CopyGuest(base, color, r->color, sizeof(r->color)) ? kEventHasColor
                                                                       : kEventColorUnreadable;
    }
    CopyEventState(*s, base, device, r->state, &r->flags);
    writer.Commit(Publication::kBatched);
    CountRecord(sizeof(ClearRecord));
  }
  frame_.capture_cycles += s->before_cycles + ElapsedCycles(t0);
}

void GuestCapture::AfterResolve(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                                uint32_t caller, uint32_t flags, uint32_t source_rect,
                                uint32_t dest_texture, uint32_t dest_point, uint32_t dest_level,
                                uint32_t dest_slice, uint32_t clear_color, float clear_z) {
  EventSnapshot* s = PopSnapshot(token, GuestEvent::kResolve);
  if (!s) {
    return;
  }
  if ((s->flags & kEventInsideEndTiling) != 0) {
    ++t_nested_resolves;
  }
  if (!enabled()) {
    return;
  }
  const uint64_t t0 = cycles::Now();
  CommandStream::Writer writer(stream_);
  if (pending_.active && pending_.device == device) {
    FlushPendingInline(writer, base, /*terminated=*/false);
  }
  uint8_t* payload = writer.Begin(RecordType::kResolve, sizeof(ResolveRecord));
  if (payload) {
    auto* r = new (payload) ResolveRecord();
    r->device = device;
    r->caller = caller;
    r->resolve_flags = flags;
    r->src_rect_ptr = source_rect;
    r->dest_ptr = dest_texture;
    r->dest_point_ptr = dest_point;
    r->dest_level = dest_level;
    r->dest_slice = dest_slice;
    r->clear_color_ptr = clear_color;
    r->clear_z = clear_z;
    r->clear_stencil = s->stack[0];
    r->params_ptr = s->stack[1];
    if (source_rect != 0) {
      r->flags |= CopyGuest(base, source_rect, r->src_rect, sizeof(r->src_rect))
                      ? kEventHasRect
                      : kEventRectUnreadable;
    }
    if (dest_point != 0) {
      r->flags |= CopyGuest(base, dest_point, r->dest_point, sizeof(r->dest_point))
                      ? kEventHasPoint
                      : kEventPointUnreadable;
    }
    if (clear_color != 0) {
      r->flags |= CopyGuest(base, clear_color, r->clear_color, sizeof(r->clear_color))
                      ? kEventHasColor
                      : kEventColorUnreadable;
    }
    if (dest_texture != 0) {
      r->flags |= CopyGuest(base, dest_texture, r->dest_header, sizeof(r->dest_header))
                      ? kEventHasDest
                      : kEventDestUnreadable;
    }
    if (r->flags & kEventHasDest) {
      NoteResolveDestination(r->dest_header);
    }
    CopyEventState(*s, base, device, r->state, &r->flags);
    writer.Commit(Publication::kBatched);
    CountRecord(sizeof(ResolveRecord));
  }
  frame_.capture_cycles += s->before_cycles + ElapsedCycles(t0);
}

void GuestCapture::NoteResolveDestination(const uint8_t* header) {
  uint32_t dwords[6];
  TextureHeaderFetch(header, dwords);
  const TextureFetch fetch = ReadTextureFetch(dwords);
  if (!fetch.is_texture) {
    return;
  }
  uint32_t ranges[2][2];
  TextureGuestRanges(fetch.key, ranges);
  for (const auto& range : ranges) {
    if (range[1] == 0) {
      continue;
    }
    const std::pair<uint32_t, uint32_t> span{range[0], range[0] + range[1]};
    if (std::find(resolve_destinations_.begin(), resolve_destinations_.end(), span) ==
        resolve_destinations_.end()) {
      resolve_destinations_.push_back(span);
    }
  }
}

void GuestCapture::AfterBeginTiling(const GuestCallToken& token, const uint8_t* base,
                                    uint32_t device, uint32_t caller, uint32_t flags,
                                    uint32_t tile_count, uint32_t rects, uint32_t clear_color,
                                    float clear_z, uint32_t clear_stencil) {
  EventSnapshot* s = PopSnapshot(token, GuestEvent::kTilingBegin);
  if (!s || !enabled()) {
    return;
  }
  const uint64_t t0 = cycles::Now();
  CommandStream::Writer writer(stream_);
  if (pending_.active && pending_.device == device) {
    FlushPendingInline(writer, base, /*terminated=*/false);
  }
  uint8_t* payload = writer.Begin(RecordType::kTilingBegin, sizeof(TilingBeginRecord));
  if (payload) {
    auto* r = new (payload) TilingBeginRecord();
    r->device = device;
    r->caller = caller;
    r->tiling_flags = flags;
    r->tile_count = tile_count;
    r->rects_ptr = rects;
    r->clear_color_ptr = clear_color;
    r->clear_z = clear_z;
    r->clear_stencil = clear_stencil;
    const uint32_t tiles = std::min(tile_count, guest::kMaxTiles);
    if (rects != 0 && tiles != 0) {
      r->flags |= CopyGuest(base, rects, r->rects, tiles * 16) ? kEventHasRects
                                                               : kEventRectUnreadable;
    }
    if (clear_color != 0) {
      r->flags |= CopyGuest(base, clear_color, r->clear_color, sizeof(r->clear_color))
                      ? kEventHasColor
                      : kEventColorUnreadable;
    }
    CopyEventState(*s, base, device, r->state, &r->flags);
    writer.Commit(Publication::kBatched);
    CountRecord(sizeof(TilingBeginRecord));
  }
  frame_.capture_cycles += s->before_cycles + ElapsedCycles(t0);
}

void GuestCapture::AfterEndTiling(const GuestCallToken& token, const uint8_t* base,
                                  uint32_t device, uint32_t caller, uint32_t flags,
                                  uint32_t rects, uint32_t dest_texture, uint32_t clear_color,
                                  float clear_z, uint32_t clear_stencil, uint32_t params) {
  EventSnapshot* s = PopSnapshot(token, GuestEvent::kTilingEnd);
  if (!s) {
    return;
  }
  if (t_end_tiling_depth > 0) {
    --t_end_tiling_depth;
  }
  const uint32_t nested = t_nested_resolves - s->nested_at_begin;
  if (!enabled()) {
    return;
  }
  const uint64_t t0 = cycles::Now();
  CommandStream::Writer writer(stream_);
  if (pending_.active && pending_.device == device) {
    FlushPendingInline(writer, base, /*terminated=*/false);
  }
  uint8_t* payload = writer.Begin(RecordType::kTilingEnd, sizeof(TilingEndRecord));
  if (payload) {
    auto* r = new (payload) TilingEndRecord();
    r->device = device;
    r->caller = caller;
    r->resolve_flags = flags;
    r->rects_ptr = rects;
    r->dest_ptr = dest_texture;
    r->clear_color_ptr = clear_color;
    r->clear_z = clear_z;
    r->clear_stencil = clear_stencil;
    r->params_ptr = params;
    r->nested_resolves = nested;
    // The tile count EndTiling iterates is the device's, as BeginTiling left it.
    const uint32_t tiles = std::min(
        LoadBe32(s->state.pre + guest::kDevTileCount - guest::kEventWindowBegin), guest::kMaxTiles);
    if (rects != 0 && tiles != 0) {
      r->flags |= CopyGuest(base, rects, r->rects, tiles * 16) ? kEventHasRects
                                                               : kEventRectUnreadable;
    }
    if (clear_color != 0) {
      r->flags |= CopyGuest(base, clear_color, r->clear_color, sizeof(r->clear_color))
                      ? kEventHasColor
                      : kEventColorUnreadable;
    }
    if (dest_texture != 0) {
      r->flags |= CopyGuest(base, dest_texture, r->dest_header, sizeof(r->dest_header))
                      ? kEventHasDest
                      : kEventDestUnreadable;
    }
    CopyEventState(*s, base, device, r->state, &r->flags);
    writer.Commit(Publication::kBatched);
    CountRecord(sizeof(TilingEndRecord));
  }
  frame_.capture_cycles += s->before_cycles + ElapsedCycles(t0);
}

// ---------------------------------------------------------------------------
// present
// ---------------------------------------------------------------------------

void GuestCapture::Present(const uint8_t* base, uint32_t device, uint32_t front_buffer,
                           int64_t target_qpc) {
  if (!enabled()) {
    return;
  }
  uint64_t sequence = 0;
  {
    const uint64_t t0 = cycles::Now();
    CommandStream::Writer writer(stream_);
    if (pending_.active && pending_.device == device) {
      FlushPendingInline(writer, base, /*terminated=*/false);
    }
    uint8_t* payload = writer.Begin(RecordType::kPresent, sizeof(PresentRecord));
    if (payload) {
      auto* r = new (payload) PresentRecord();
      r->device = device;
      r->front_buffer = front_buffer;
      r->target_qpc = target_qpc;
      if (front_buffer != 0 &&
          !CopyGuest(base, front_buffer, r->front_buffer_header, sizeof(r->front_buffer_header))) {
        r->flags |= kPresentFrontBufferUnreadable;
      }
      // What the Swap programs the display with besides the front buffer: the fetch's swizzle and signs, and the gamma table.
      uint8_t control[4];
      if (CopyGuest(base, device + guest::kDevSwapControl, control, sizeof(control))) {
        r->swap_control = LoadBe32(control);
      } else {
        r->flags |= kPresentSwapControlUnreadable;
      }
      if (!CopyGuest(base, device + guest::kDevGammaRamp, r->gamma_ramp, sizeof(r->gamma_ramp))) {
        r->flags |= kPresentGammaRampUnreadable;
      }
      CountRecord(sizeof(PresentRecord));
      frame_.capture_cycles += ElapsedCycles(t0);
      r->stats = frame_;
      sequence = writer.Commit(Publication::kWaited);
    }
    frame_ = GuestFrameStats{};
    NextFramePages();
    for (auto& state : devices_) {
      state->check_next = true;
    }
  }
  stream_.WaitExecuted(sequence);
}

// ---------------------------------------------------------------------------
// fences
// ---------------------------------------------------------------------------

void GuestCapture::Fence(uint32_t device, uint32_t value) {
  if (!enabled()) {
    return;
  }
  const uint64_t t0 = cycles::Now();
  // Not a draw-state event: a pending inline draw stays open. A fence kicked
  // between BeginVertices and EndVertices was inserted before that draw's
  // packet, so it does not cover the draw and goes ahead of its record.
  CommandStream::Writer writer(stream_);
  uint8_t* payload = writer.Begin(RecordType::kFence, sizeof(FenceRecord));
  if (!payload) {
    return;
  }
  auto* r = new (payload) FenceRecord();
  r->device = device;
  r->value = value;
  CountRecord(sizeof(FenceRecord));
  ++frame_.fences;
  frame_.capture_cycles += ElapsedCycles(t0);
  writer.Commit(Publication::kNow);
}

bool GuestCapture::FenceCatchUp(const uint8_t* base, uint32_t device) {
  if (!enabled() || device == 0) {
    return false;
  }
  // Read with the producer lock held: a record whose writer finished before
  // the increment this read sees is ahead of the catch-up in the stream.
  CommandStream::Writer writer(stream_);
  uint8_t issued_be[4];
  if (!CopyGuest(base, device + nfx::kDevFenceIssuedOffset, issued_be, sizeof(issued_be))) {
    return false;
  }
  const uint32_t issued = LoadBe32(issued_be);
  if (issued == 0) {
    return false;  // ring not set up yet; sub_82412A60 starts issued at 3, retired at 1
  }
  uint8_t* payload = writer.Begin(RecordType::kFence, sizeof(FenceRecord));
  if (!payload) {
    return false;
  }
  auto* r = new (payload) FenceRecord();
  r->device = device;
  r->value = issued - 2;  // the newest fence inserted so far
  CountRecord(sizeof(FenceRecord));
  ++frame_.fences;
  writer.Commit(Publication::kNow);
  return true;
}

// ---------------------------------------------------------------------------
// locks (M5.1)
// ---------------------------------------------------------------------------

bool GuestCapture::Lock(uint32_t resource, uint32_t level, uint32_t address, uint32_t size) {
  if (!enabled() || size == 0) {
    return false;
  }
  const uint32_t start = GuestMemory::ToPhysical(address);
  const uint64_t end = uint64_t(start) + size;
  uint64_t sequence = 0;
  {
    CommandStream::Writer writer(stream_);
    bool destination = false;
    for (const auto& [s, e] : resolve_destinations_) {
      if (s < end && start < e) {
        destination = true;
        break;
      }
    }
    if (!destination) {
      return false;
    }
    // Not a draw-state event, like a fence: a pending inline draw stays open.
    uint8_t* payload = writer.Begin(RecordType::kLock, sizeof(LockRecord));
    if (!payload) {
      return false;
    }
    auto* r = new (payload) LockRecord();
    r->resource = resource;
    r->level = level;
    r->address = start;
    r->size = size;
    CountRecord(sizeof(LockRecord));
    sequence = writer.Commit(Publication::kWaited);
  }
  // The render thread writes the GPU's image of the range into memory before
  // the CPU reads it (M5.1).
  stream_.WaitExecuted(sequence);
  return true;
}

// ---------------------------------------------------------------------------
// the scene's composition (M6c.0)
// ---------------------------------------------------------------------------

void GuestCapture::Composite(uint32_t bucket) {
  if (!enabled()) {
    return;
  }
  const uint64_t t0 = cycles::Now();
  // Not a draw-state event, like a fence: a pending inline draw stays open (none
  // is, at the end of the post object's draw).
  CommandStream::Writer writer(stream_);
  uint8_t* payload = writer.Begin(RecordType::kComposite, sizeof(CompositeRecord));
  if (!payload) {
    return;
  }
  auto* r = new (payload) CompositeRecord();
  r->bucket = bucket;
  CountRecord(sizeof(CompositeRecord));
  frame_.capture_cycles += ElapsedCycles(t0);
  writer.Commit(Publication::kBatched);
}

// ---------------------------------------------------------------------------
// occlusion queries
// ---------------------------------------------------------------------------

void GuestCapture::OcclusionEvent(const uint8_t* base, uint32_t query, bool is_begin) {
  if (!enabled() || !occlusion::Active()) {
    return;
  }
  const uint64_t t0 = cycles::Now();
  // Not a draw-state event, like a fence: a pending inline draw stays open (the sun's
  // draws are inline ones, and are emitted at their EndVertices, inside the query).
  CommandStream::Writer writer(stream_);
  uint32_t use = 0;
  if (!occlusion::OnGuestIssue(const_cast<uint8_t*>(base), query, is_begin, &use)) {
    return;
  }
  uint8_t* payload = writer.Begin(RecordType::kOcclusion, sizeof(OcclusionRecord));
  if (!payload) {
    return;
  }
  auto* r = new (payload) OcclusionRecord();
  r->query = query;
  r->use = use;
  r->end = is_begin ? 0 : 1;
  CountRecord(sizeof(OcclusionRecord));
  frame_.capture_cycles += ElapsedCycles(t0);
  writer.Commit(Publication::kBatched);
}

}  // namespace mcla::gpu
