// MCLA GPU renderer -- capture of the guest's D3D calls. See guest_capture.h.
//
// Design followed: reeot hooks/state.cpp and gpu/draw.cpp (BSD-3-Clause) -- a
// draw packet built from copies of the device's state windows, constants
// deduplicated against the dirty masks and verified against the bank. The
// windows, offsets and event set here are this game's; no code was copied.

#include "guest_capture.h"

#include <algorithm>
#include <cstring>
#include <new>

#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include "native_gfx/guest/guest_fence.h"
#include "native_gfx/guest/guest_resources.h"
#include "native_gfx/shader_identity.h"

namespace mcla::gpu {

namespace nfx = mcla::native_gfx;

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kMaxEventDepth = 4;
// Every offset the capture reads from a device lies below this.
constexpr uint32_t kDeviceReadEnd = guest::kEventWindowEnd;

struct EventSnapshot {
  GuestEvent event = GuestEvent::kClear;
  uint32_t device = 0;
  uint32_t flags = 0;  // EventFlags found before the call
  uint32_t stack[2] = {};
  uint32_t nested_at_begin = 0;
  uint64_t before_ns = 0;
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

uint64_t ElapsedNs(Clock::time_point since) {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - since).count());
}

// The deltas of one bank: blocks whose bytes differ from the shadow, among the
// ones the dirty mask names -- or among all of them when verifying. A block
// whose bit is set but whose bytes did not change is not sent.
void CollectDeltas(const uint8_t* bank, const uint8_t* shadow, bool shadow_valid, uint64_t dirty,
                   bool verify, std::array<uint8_t, guest::kConstantBlocksPerBank>& out,
                   uint32_t& count, uint32_t& masked, uint32_t& unmasked) {
  count = 0;
  for (uint32_t i = 0; i < guest::kConstantBlocksPerBank; ++i) {
    const bool named = ((dirty >> (63 - i)) & 1u) != 0;
    if (shadow_valid && !named && !verify) {
      continue;
    }
    const uint32_t at = i * guest::kConstantBlockBytes;
    if (shadow_valid && std::memcmp(bank + at, shadow + at, guest::kConstantBlockBytes) == 0) {
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
    : stream_(stream), verify_constants_(verify_constants) {
  scratch_.reserve(nfx::kMaxUcodeBytes);
}

// ---------------------------------------------------------------------------
// guest reads
// ---------------------------------------------------------------------------

bool GuestCapture::CopyGuest(const uint8_t* base, uint32_t address, void* out, uint32_t bytes) {
  if (address < 0x1000u || bytes == 0 || !nfx::IsGuestRangeReadable(address, bytes)) {
    ++frame_.unreadable;
    return false;
  }
  std::memcpy(out, GuestBytes(base, address), bytes);
  return true;
}

GuestCapture::DeviceState& GuestCapture::StateFor(uint32_t device) {
  for (auto& state : devices_) {
    if (state->device == device) {
      return *state;
    }
  }
  devices_.push_back(std::make_unique<DeviceState>());
  devices_.back()->device = device;
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
               nfx::IsGuestRangeReadable(r.address, r.size);
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
               nfx::IsGuestRangeReadable(r.address, r.size);
  if (!r.readable) {
    ++frame_.unreadable;
  }
  return r;
}

void GuestCapture::ResolveIdentity(const uint8_t* base, ShaderRead& shader, bool vertex) {
  const uint8_t* ucode = GuestBytes(base, shader.address);
  const uint64_t key = uint64_t(shader.address) << 32 | shader.size;
  auto found = identities_.find(key);
  if (found != identities_.end()) {
    IdentityEntry& e = found->second;
    for (auto it = e.raw.rbegin(); it != e.raw.rend(); ++it) {
      if (std::memcmp(it->data(), ucode, shader.size) == 0) {
        shader.identity = e.identity;
        shader.computed_fetch_index = e.computed_fetch_index;
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
  shader.identity = e.identity;
  shader.computed_fetch_index = e.computed_fetch_index;
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
  const uint32_t ps_object = DeviceU32(base, device, guest::kDevPixelShader);
  if (ps_object == 0) {
    c.flags |= kDrawNoPixelShader;
  } else {
    c.ps = ReadPixelShader(base, ps_object);
    if (c.ps.readable) {
      ResolveIdentity(base, c.ps, false);
    } else {
      c.flags |= kDrawPsUnreadable;
    }
  }
  c.vs = ReadVertexShader(base, device, ps_object, &c.vs_variant);
  if (c.vs.readable) {
    ResolveIdentity(base, c.vs, true);
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
                 !nfx::IsGuestRangeReadable(c.decl_object + nfx::kDeclOffsetElements,
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
  CollectDeltas(DeviceBytes(base, device, guest::kDevVsConstantBank), state.vs_shadow.data(),
                state.shadows_valid, token.vs_dirty, verify_constants_, c.vs_blocks,
                c.vs_block_count, frame_.constant_blocks_masked, frame_.constant_blocks_unmasked);
  CollectDeltas(DeviceBytes(base, device, guest::kDevPsConstantBank), state.ps_shadow.data(),
                state.shadows_valid, token.ps_dirty, verify_constants_, c.ps_blocks,
                c.ps_block_count, frame_.constant_blocks_masked, frame_.constant_blocks_unmasked);
  if (state.check_next || ++state.draws_since_check >= kBankCheckInterval) {
    c.bank_check = true;
    state.check_next = false;
    state.draws_since_check = 0;
  }
}

void GuestCapture::WriteDraw(const uint8_t* base, uint32_t device, const DrawCapture& c,
                             uint8_t* payload) {
  const DrawRecordLayout layout =
      ComputeDrawLayout(c.decl_count, c.vs_block_count, c.ps_block_count, c.inline_bytes);
  auto* r = new (payload) DrawRecord();
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
  for (uint32_t i = 0; i < 4; ++i) {
    r->surfaces[i] = DeviceU32(base, device, guest::kDevColorSurfaces + 4 * i);
  }
  r->surfaces[4] = DeviceU32(base, device, guest::kDevDepthSurface);
  for (uint32_t i = 0; i < guest::kSamplerCount; ++i) {
    r->sampler_textures[i] = DeviceU32(base, device, guest::kDevSamplerTextures + 4 * i);
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

  DeviceState& state = StateFor(device);
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
  const auto t0 = Clock::now();
  std::lock_guard lock(mutex_);
  if (pending_.active && pending_.device == device) {
    FlushPendingInline(base, device, /*terminated=*/false);
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
  {
    CommandStream::Writer writer(stream_);
    uint8_t* payload = writer.Begin(RecordType::kDraw, layout.payload_bytes);
    if (payload) {
      WriteDraw(base, device, c, payload);
      writer.Commit(Publication::kBatched);
      CountRecord(layout.payload_bytes);
      ++frame_.draws;
    }
  }
  frame_.capture_ns += ElapsedNs(t0);
}

void GuestCapture::AfterBeginVertices(const GuestCallToken& token, const uint8_t* base,
                                      uint32_t device, uint32_t primitive, uint32_t count,
                                      uint32_t stride, uint32_t buffer) {
  if (!token.active || !enabled()) {
    return;
  }
  const auto t0 = Clock::now();
  std::lock_guard lock(mutex_);
  if (pending_.active) {
    // A second Begin before End on the same device is a guest bug; on another
    // device it is a second thread. Either way the first is closed here.
    FlushPendingInline(base, pending_.device, /*terminated=*/false);
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
    frame_.capture_ns += ElapsedNs(t0);
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
  frame_.capture_ns += ElapsedNs(t0);
}

void GuestCapture::AfterEndVertices(const uint8_t* base, uint32_t device) {
  if (!enabled()) {
    return;
  }
  const auto t0 = Clock::now();
  std::lock_guard lock(mutex_);
  if (pending_.active && pending_.device == device) {
    FlushPendingInline(base, device, /*terminated=*/true);
  }
  frame_.capture_ns += ElapsedNs(t0);
}

void GuestCapture::FlushPendingInline(const uint8_t* base, uint32_t device, bool terminated) {
  (void)device;
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
  CommandStream::Writer writer(stream_);
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
  const auto t0 = Clock::now();
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
  s.before_ns = ElapsedNs(t0);
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
  const auto t0 = Clock::now();
  std::lock_guard lock(mutex_);
  if (pending_.active && pending_.device == device) {
    FlushPendingInline(base, device, /*terminated=*/false);
  }
  CommandStream::Writer writer(stream_);
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
  frame_.capture_ns += s->before_ns + ElapsedNs(t0);
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
  const auto t0 = Clock::now();
  std::lock_guard lock(mutex_);
  if (pending_.active && pending_.device == device) {
    FlushPendingInline(base, device, /*terminated=*/false);
  }
  CommandStream::Writer writer(stream_);
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
    CopyEventState(*s, base, device, r->state, &r->flags);
    writer.Commit(Publication::kBatched);
    CountRecord(sizeof(ResolveRecord));
  }
  frame_.capture_ns += s->before_ns + ElapsedNs(t0);
}

void GuestCapture::AfterBeginTiling(const GuestCallToken& token, const uint8_t* base,
                                    uint32_t device, uint32_t caller, uint32_t flags,
                                    uint32_t tile_count, uint32_t rects, uint32_t clear_color,
                                    float clear_z, uint32_t clear_stencil) {
  EventSnapshot* s = PopSnapshot(token, GuestEvent::kTilingBegin);
  if (!s || !enabled()) {
    return;
  }
  const auto t0 = Clock::now();
  std::lock_guard lock(mutex_);
  if (pending_.active && pending_.device == device) {
    FlushPendingInline(base, device, /*terminated=*/false);
  }
  CommandStream::Writer writer(stream_);
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
  frame_.capture_ns += s->before_ns + ElapsedNs(t0);
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
  const auto t0 = Clock::now();
  std::lock_guard lock(mutex_);
  if (pending_.active && pending_.device == device) {
    FlushPendingInline(base, device, /*terminated=*/false);
  }
  CommandStream::Writer writer(stream_);
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
  frame_.capture_ns += s->before_ns + ElapsedNs(t0);
}

// ---------------------------------------------------------------------------
// present
// ---------------------------------------------------------------------------

void GuestCapture::Present(const uint8_t* base, uint32_t device, uint32_t front_buffer) {
  if (!enabled()) {
    return;
  }
  uint64_t sequence = 0;
  {
    const auto t0 = Clock::now();
    std::lock_guard lock(mutex_);
    if (pending_.active && pending_.device == device) {
      FlushPendingInline(base, device, /*terminated=*/false);
    }
    CommandStream::Writer writer(stream_);
    uint8_t* payload = writer.Begin(RecordType::kPresent, sizeof(PresentRecord));
    if (payload) {
      auto* r = new (payload) PresentRecord();
      r->device = device;
      r->front_buffer = front_buffer;
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
      frame_.capture_ns += ElapsedNs(t0);
      r->stats = frame_;
      sequence = writer.Commit(Publication::kWaited);
    }
    frame_ = GuestFrameStats{};
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
  const auto t0 = Clock::now();
  std::lock_guard lock(mutex_);
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
  frame_.capture_ns += ElapsedNs(t0);
  writer.Commit(Publication::kNow);
}

bool GuestCapture::FenceCatchUp(const uint8_t* base, uint32_t device) {
  if (!enabled() || device == 0) {
    return false;
  }
  std::lock_guard lock(mutex_);
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

}  // namespace mcla::gpu
