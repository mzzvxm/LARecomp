#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — texture / sampler binding.
// See texture_binding.h for the fetch-slot -> descriptor-index chain.

#include "texture_binding.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include "../guest/guest_constants.h"
#include "../guest/guest_resources.h"
#include <rex/ui/d3d12/d3d12_util.h>

#include "context.h"
#include "texture_cache.h"

REXCVAR_DECLARE(bool, mcla_native_gfx_texture_swizzle);

REXCVAR_DECLARE(bool, mcla_native_gfx_bind_memo);

namespace mcla::native_gfx {


namespace {

// Descriptor heap sizes. Generous but bounded; the caches make reuse the
// common case, and running out is reported rather than silently wrapping.
// A full unlimited MCLA frame (scene+shadow+aux+composite, ~4000 draws) binds
// well over 2048 unique textures, so with the heap split in halves each half
// must hold a whole frame's worth. 32768 -> 16384 per half covers it with room
// to spare (shader-visible CBV/SRV/UAV heaps allow up to 1M on tier-1 hardware).
constexpr uint32_t kSrvHeapSize = 32768;
// Shader-visible SAMPLER heaps are hard-capped at 2048 total by D3D12, so this
// cannot grow; 1024 unique sampler states per frame is far more than MCLA uses.
constexpr uint32_t kSamplerHeapSize = 2048;
// Double-buffered: each native frame uses one half of the heap so the previous
// frame's descriptors (still read by the GPU) are never overwritten.
constexpr uint32_t kSrvHalf = kSrvHeapSize / 2;          // 16384 SRVs per frame
constexpr uint32_t kSamplerHalf = kSamplerHeapSize / 2;  // 1024 samplers per frame

inline uint32_t R32(const uint8_t* base, uint32_t ea) {
  if (ea < 0x1000u) {
    return 0;
  }
  uint32_t v;
  std::memcpy(&v, rex::memory::GuestPtr(const_cast<uint8_t*>(base), ea), 4);
  return __builtin_bswap32(v);
}

uint64_t Hash64(const void* data, size_t size) {
  const auto* p = static_cast<const uint8_t*>(data);
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < size; ++i) {
    h ^= p[i];
    h *= 0x100000001B3ull;
  }
  return h;
}

}  // namespace

bool TextureBinder::Initialize(D3D12Context& context) {
  if (srv_heap_) {
    return true;
  }
  D3D12_DESCRIPTOR_HEAP_DESC srv = {};
  srv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  srv.NumDescriptors = kSrvHeapSize;
  srv.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(context.device()->CreateDescriptorHeap(&srv, IID_PPV_ARGS(&srv_heap_)))) {
    REXLOG_ERROR("[native_gfx] SRV descriptor heap creation failed");
    return false;
  }
  D3D12_DESCRIPTOR_HEAP_DESC smp = {};
  smp.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
  smp.NumDescriptors = kSamplerHeapSize;
  smp.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(context.device()->CreateDescriptorHeap(&smp, IID_PPV_ARGS(&sampler_heap_)))) {
    REXLOG_ERROR("[native_gfx] sampler descriptor heap creation failed");
    srv_heap_.Reset();
    return false;
  }
  srv_increment_ = context.device()->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  sampler_increment_ =
      context.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  return true;
}

void TextureBinder::Shutdown(D3D12Context& context) {
  (void)context;
  srv_cache_.clear();
  sampler_cache_.clear();
  fallback_texture_.Reset();
  fallback_ready_ = false;
  fallback_filled_ = false;
  fallback_srv_ = 0;
  srv_heap_.Reset();
  sampler_heap_.Reset();
  srv_next_ = 0;
  sampler_next_ = 0;
  srv_base_ = 0;
  sampler_base_ = 0;
  frame_parity_ = 0;
  fallback_srv_valid_ = false;
}

void TextureBinder::BeginFrame() {
  // Flip to the other half; the previous frame's descriptors (still in flight)
  // live in the half we leave. Fresh caches so no stale pointer-keyed SRV
  // survives a resource free/recycle.
  frame_parity_ ^= 1u;
  srv_base_ = frame_parity_ * kSrvHalf;
  sampler_base_ = frame_parity_ * kSamplerHalf;
  srv_next_ = srv_base_;
  sampler_next_ = sampler_base_;
  srv_cache_.clear();
  sampler_cache_.clear();
  srv_warned_ = false;
  sampler_warned_ = false;
  // Descriptor indices are allocated out of THIS frame's half, so every index
  // the memo holds names a descriptor that is about to be overwritten.
  memo_valid_ = false;
  // The fallback texture persists, but its descriptor lives in the heap and must
  // be re-created in this frame's half; force a re-alloc on next FallbackSrv.
  fallback_srv_valid_ = false;
}

namespace {

// Typeless resources cannot be viewed with a typeless format; map each one to
// its readable member. Anything else is already a fully typed format and is
// used as created.
uint32_t SrvFormatForResource(ID3D12Resource* resource, uint32_t guest_format) {
  if (!resource) {
    return TextureFormatToDxgi(guest_format);
  }
  switch (resource->GetDesc().Format) {
    case DXGI_FORMAT_R24G8_TYPELESS:
      return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
      return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:
      return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_UNKNOWN:
      return TextureFormatToDxgi(guest_format);
    default:
      return uint32_t(resource->GetDesc().Format);
  }
}

// The Xenos fetch constant carries a 12-bit swizzle, four 3-bit selectors in
// x,y,z,w order, and its encoding is the same one D3D12 uses for
// Shader4ComponentMapping: 0..3 pick a source component, 4 forces 0, 5 forces
// 1, and the only difference is the 1<<12 flag D3D12 requires. Telemetry says
// it matters: of 1885 distinct fetches 1847 are the identity 0x688, but the 34
// k_8_8_8_8 ones carry 0x60A = (Z,Y,X,W) and account for 38% of all textured
// draws, because they are the colour-grading LUTs bound on every draw. The
// single-channel masks use 0xB68 = (X,1,1,1) and 0xA00 = (X,X,X,1).
//
// The fetch swizzle alone is NOT the answer to hand D3D12. It selects among
// the components as the GUEST stores them, and it has to be composed with a
// second swizzle that says where this runtime's own decode put each of those
// components -- exactly what the SDK does (GuestToHostSwizzle fed by
// GetHostFormatSwizzle, src/graphics/pipeline/texture/cache.cpp). Our decode
// only untiles, so the host half follows straight from the DXGI format picked
// in TextureFormatToDxgi:
//
//   k_8_8_8_8 -> R8G8B8A8, a plain copy, host half is the identity, so a
//   0x60A = (Z,Y,X,W) fetch lands verbatim. That is the night city: ColorT1
//   (slot 7) and the light-index grid (slot 11) both carry 0x60A, and dropping
//   it made every lit surface read its light colour with red and blue
//   exchanged.
//
//   k_1_5_5_5 -> B5G5R5A1, and here the format itself already exchanges red
//   and blue, because the SDK does that conversion in its load shader
//   (kLoadShaderIndexR5G5B5A1ToB5G5R5A1) and we do not. So OUR host half is
//   (Z,Y,X,W), and composing it with a 0x60A fetch cancels to the identity.
//   That is the day sky: applying 0x60A raw to it put 82% too much red in the
//   sky region (0.09303 against the emulated 0.05107).
//
// Composing gets both, which raw application cannot: measured HDR, native
// against emulated, cam 13 by day and cam 21 at 22:00.
//
// Single-channel formats take RRRR, same as the SDK, so a (X,1,1,1) or
// (X,X,X,1) mask keeps reading the one channel that exists.
// Where this runtime's decode leaves each guest component, per guest format.
// Encoded like the fetch swizzle: three bits per component, 0..3 pick a source
// component, 4 is 0 and 5 is 1.
uint32_t HostFormatSwizzle(uint32_t guest_format, bool tiled) {
  constexpr uint32_t kRgba = 0x688u;  // (X,Y,Z,W)
  constexpr uint32_t kRrrr = 0x000u;  // (X,X,X,X)
  constexpr uint32_t kBgra = 0x60Au;  // (Z,Y,X,W)
  switch (GuestTextureFormat(guest_format)) {
    case GuestTextureFormat::k_1_5_5_5:
      // B5G5R5A1_UNORM over an unconverted R5G5B5A1 payload.
      return kBgra;
    case GuestTextureFormat::k_8_8_8_8:
      // Measured discriminator, cause not yet understood: every k_8_8_8_8 in
      // this game arrives end=2 with the same 0x60A fetch swizzle, but only the
      // LINEAR ones want it applied. Those are the light data tables the
      // multi-light shaders index (ColorT 128x256, light-index grid 512x640);
      // applying it there is what makes the night and dusk city match the
      // emulated. The TILED ones -- the noise and grading surfaces -- come out
      // of the decode already in host order, and applying it again reddens the
      // sky. Composing kBgra with a 0x60A fetch cancels to the identity.
      return tiled ? kBgra : kRgba;
    case GuestTextureFormat::k_8:
    case GuestTextureFormat::k_32_FLOAT:
      return kRrrr;
    default:
      // Everything else this runtime binds is a straight component-for-
      // component DXGI match; depth arrives through the render-target path and
      // is sampled as a single channel, which kRrrr would also give.
      return kRgba;
  }
}

uint32_t ShaderComponentMappingForSwizzle(uint32_t guest_swizzle, uint32_t guest_format,
                                         bool tiled) {
  if (!REXCVAR_GET(mcla_native_gfx_texture_swizzle)) {
    return D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  }
  const uint32_t host_format_swizzle = HostFormatSwizzle(guest_format, tiled);
  uint32_t mapping = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t guest_component = (guest_swizzle >> (3u * i)) & 0x7u;
    uint32_t component;
    if (guest_component >= 4u) {
      // 6 and 7 are reserved; fold them onto 4 (zero) and 5 (one) rather than
      // handing the driver something undefined, same as the SDK.
      component = guest_component & 0x5u;
    } else {
      component = (host_format_swizzle >> (3u * guest_component)) & 0x7u;
    }
    mapping |= component << (3u * i);
  }
  return mapping | (1u << 12);
}

}  // namespace

uint32_t TextureBinder::AcquireSrv(D3D12Context& context, ID3D12Resource* resource,
                                   const TextureFetch& fetch, TextureSource source) {
  // Identity: the resource plus the fields that change how it is VIEWED.
  // `source` belongs in the key because it decides whether the guest swizzle
  // is applied at all.
  struct {
    uint64_t resource;
    uint32_t format, width, height, swizzle, source;
  } id = {uint64_t(reinterpret_cast<uintptr_t>(resource)), fetch.format, fetch.width,
          fetch.height, fetch.swizzle,      uint32_t(source)};
  const uint64_t key = Hash64(&id, sizeof(id));
  auto it = srv_cache_.find(key);
  if (it != srv_cache_.end()) {
    ++stats_.srv_hits;
    return it->second;
  }
  if (srv_next_ >= srv_base_ + kSrvHalf) {
    if (!srv_warned_) {
      REXLOG_ERROR("[native_gfx] SRV descriptor half exhausted ({} entries/frame)", kSrvHalf);
      srv_warned_ = true;
    }
    ++stats_.srv_exhausted;
    ++stats_.unresolved;
    return 0;
  }
  const uint32_t index = srv_next_++;
  D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
  // Derived from the resource, not from the fetch: a pooled depth target
  // is created TYPELESS so it can be both a DSV and an SRV, and a view
  // built from the guest format would not match it.
  desc.Format = DXGI_FORMAT(SrvFormatForResource(resource, fetch.format));
  desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  desc.Shader4ComponentMapping = ShaderComponentMappingForSwizzle(fetch.swizzle, fetch.format, fetch.tiled);
  // Every level the resource has, not just the top one. Pinning this to 1
  // hides the mip chain the texture cache now uploads: the sampler would still
  // have a mip filter and a LOD range, but nothing below level 0 to read, so
  // distant surfaces would keep aliasing. -1 means "all levels from
  // MostDetailedMip" and is equally correct for the single-level resources the
  // render-target bridge hands back.
  desc.Texture2D.MipLevels = UINT(-1);
  D3D12_CPU_DESCRIPTOR_HANDLE handle = srv_heap_->GetCPUDescriptorHandleForHeapStart();
  handle.ptr += SIZE_T(index) * srv_increment_;
  context.device()->CreateShaderResourceView(resource, &desc, handle);
  srv_cache_.emplace(key, index);
  ++stats_.srv_misses;
  return index;
}

uint32_t TextureBinder::AcquireSampler(D3D12Context& context, const SamplerDescription& s) {
  const uint64_t key = Hash64(&s, sizeof(s));
  auto it = sampler_cache_.find(key);
  if (it != sampler_cache_.end()) {
    ++stats_.sampler_hits;
    return it->second;
  }
  if (sampler_next_ >= sampler_base_ + kSamplerHalf) {
    if (!sampler_warned_) {
      REXLOG_ERROR("[native_gfx] sampler descriptor half exhausted ({} entries/frame)", kSamplerHalf);
      sampler_warned_ = true;
    }
    ++stats_.sampler_exhausted;
    ++stats_.unresolved;
    return 0;
  }
  const uint32_t index = sampler_next_++;
  D3D12_SAMPLER_DESC desc = {};
  BuildD3D12SamplerDesc(s, &desc);

  D3D12_CPU_DESCRIPTOR_HANDLE handle = sampler_heap_->GetCPUDescriptorHandleForHeapStart();
  handle.ptr += SIZE_T(index) * sampler_increment_;
  context.device()->CreateSampler(&desc, handle);
  sampler_cache_.emplace(key, index);
  ++stats_.sampler_misses;
  return index;
}

uint32_t TextureBinder::FallbackSrv(D3D12Context& context, ID3D12GraphicsCommandList* cl) {
  // The SRV descriptor lives in the heap half of the CURRENT frame and must be
  // re-created every frame (BeginFrame flips halves + clears validity). The
  // texture itself is created ONCE and reused.
  if (fallback_srv_valid_) {
    return fallback_srv_;
  }
  if (!fallback_ready_) {
    fallback_ready_ = true;  // one attempt; a failure must not retry every draw
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = 1;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    // COPY_DEST so the white pixel can be uploaded below. An UNINITIALISED
    // fallback (the previous COMMON, never-written texture) is the black-screen
    // bug: a missing fetch — the composite's 1x1 average-exposure input among
    // them — sampled heap garbage, and a near-zero garbage exposure multiplies
    // the whole scene to black in the tonemap. White (1,1,1,1) makes a missing
    // multiply-style input a pass-through instead.
    if (FAILED(context.device()->CreateCommittedResource(
            &rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&fallback_texture_)))) {
      REXLOG_ERROR("[native_gfx] fallback texture creation failed");
      fallback_ready_ = false;
      return 0;
    }
  }
  if (srv_next_ >= srv_base_ + kSrvHalf) {
    return 0;
  }
  fallback_srv_ = srv_next_++;
  fallback_srv_valid_ = true;
  D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  desc.Texture2D.MipLevels = 1;
  D3D12_CPU_DESCRIPTOR_HANDLE handle = srv_heap_->GetCPUDescriptorHandleForHeapStart();
  handle.ptr += SIZE_T(fallback_srv_) * srv_increment_;
  context.device()->CreateShaderResourceView(fallback_texture_.Get(), &desc, handle);
  // The white-pixel upload only needs to happen once (the texture persists in
  // ALL_SHADER_RESOURCE afterward); re-creating the SRV each frame does not
  // touch the pixel data. `fallback_filled_` guards the one-time copy.
  D3D12Context::UploadAlloc up;
  if (!fallback_filled_ && cl && context.AllocateUpload(256, 256, up)) {
    *reinterpret_cast<uint32_t*>(up.cpu) = 0xFFFFFFFFu;  // R8G8B8A8 white, opaque
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = fallback_texture_.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    src.pResource = up.buffer;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = up.offset;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = 1;
    src.PlacedFootprint.Footprint.Height = 1;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = 256;  // D3D12 texture copy row alignment
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER b = {};
    b.Transition.pResource = fallback_texture_.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &b);
    fallback_filled_ = true;
    REXLOG_INFO("[native_gfx] fallback texture (white) at SRV descriptor {}", fallback_srv_);
  }
  return fallback_srv_;
}

void TextureBinder::BindAll(D3D12Context& context, ID3D12GraphicsCommandList* cl,
                            const uint8_t* base, uint32_t dev, TextureCache& textures,
                            uint8_t* shared_bytes, BoundTexture* out, uint32_t* out_count,
                            uint32_t out_capacity, uint64_t volatile_guard) {
  if (out_count) {
    *out_count = 0;
  }
  const uint32_t shadow = dev + kDevFetchShadowOffset;

  // Read the whole fetch shadow up front. This is not extra work: the per-slot
  // loop below reads exactly these dwords anyway, so hoisting them only moves
  // the reads -- and it gives the memo its key for free. See the memo notes in
  // texture_binding.h for why the key is the raw shadow rather than the decoded
  // fetches: two different shadows can decode to the same TextureFetch, and
  // comparing raw bytes cannot be fooled by a field the decoder ignores today.
  uint32_t shadow_dwords[kMemoShadowDwords];
  for (uint32_t i = 0; i < kMemoShadowDwords; ++i) {
    shadow_dwords[i] = R32(base, shadow + 4 * i);
  }

  // Anything that can move a resource out from under an unchanged fetch
  // constant has to break the memo. Contents changing is fine -- the SRV still
  // names the same resource and the GPU sees the new data -- so only the
  // counters that mean "a different resource now backs this address" are here.
  const TextureCache::Stats& tc = textures.stats();
  const uint64_t guard = tc.uploads + tc.evictions + tc.bridge_refusals +
                         tc.stale_gpu_addresses + tc.decode_failures +
                         tc.unsupported_format + volatile_guard;

  const bool memo_enabled = REXCVAR_GET(mcla_native_gfx_bind_memo);
  if (memo_enabled && memo_valid_ && memo_dev_ == dev && memo_guard_ == guard &&
      std::memcmp(memo_shadow_, shadow_dwords, sizeof(shadow_dwords)) == 0) {
    if (shared_bytes) {
      std::memcpy(shared_bytes, memo_shared_, kMemoSharedBytes);
    }
    if (out && out_count) {
      const uint32_t n = memo_out_count_ < out_capacity ? memo_out_count_ : out_capacity;
      for (uint32_t i = 0; i < n; ++i) {
        out[i] = memo_out_[i];
      }
      *out_count = n;
    }
    ++stats_.memo_hits;
    return;
  }
  ++stats_.memo_misses;
  if (memo_valid_ && memo_dev_ == dev &&
      std::memcmp(memo_shadow_, shadow_dwords, sizeof(shadow_dwords)) == 0) {
    ++stats_.memo_miss_guard;  // same textures, but something moved under them
  } else {
    ++stats_.memo_miss_key;
  }
  // Refuse to store a result that contains a resource the bridge can replace.
  bool memo_storable = true;

  for (uint32_t slot = 0; slot < kFetchConstantSlotCount; ++slot) {
    const uint32_t* d = shadow_dwords + slot * kFetchGroupDwords;
    if ((d[0] & 0x3u) != 2u) {
      continue;  // not a texture fetch constant
    }
    // The fetch constant's four 2-bit sign fields and its exponent bias were
    // measured over 40.6 million slot decodes of MCLA, and the result is why
    // only ONE of the three gaps against the emulated path is worth code:
    //   3,3,3,0 gamma      32.1%   handled
    //   0,0,0,0 unsigned   55.7%   handled
    //   1,1,1,1 kSigned    12.2%   inert, see below
    //   anything else       0.0%
    //   exponent bias      always 0, so ignoring it costs nothing here
    // So the per-component decode below is safe in this title: all four fields
    // always agree.
    //
    // kSigned looked like a real gap -- TextureFormatToDxgi never sees the
    // sign, so a signed fetch would read as UNORM [0,1] where the hardware
    // gives [-1,1] -- until the FORMATS behind it were measured. Every kSigned
    // fetch in MCLA uses exactly one of:
    //   29 k_16_16_16_16_EXPAND -> R16G16B16A16_SNORM   already signed
    //   32 k_16_16_16_16_FLOAT  -> R16G16B16A16_FLOAT   signed by nature
    //   36 k_32_FLOAT           -> R32_FLOAT            signed by nature
    // None lands on a UNORM host format, so there is nothing to correct. The
    // emulated path arrives at the same place: it does not handle kSigned in
    // the shader either, only through host format choice.
    const TextureFetch fetch = DecodeTextureFetch(d);
    if (!fetch.type_valid || fetch.width == 0 || fetch.height == 0) {
      continue;
    }

    const SamplerDescription sampler = DecodeSampler(d);
    TextureSource source = TextureSource::kUnresolved;
    ID3D12Resource* resource = textures.Resolve(context, cl, base, fetch, &source);
    if (!resource) {
      ++stats_.unresolved;
      // The neutral white stand-in is a placeholder for a bind that failed.
      // Reusing it for a later draw would keep serving white after the texture
      // became resolvable, so a frame that recovers would never show it.
      memo_storable = false;
      ++stats_.memo_refused_fallback;
      // TEMP DIAG (remove after): every distinct fetch that ends up on the
      // neutral white texture, once each. The [comp] dump is capped at 40 lines
      // for the whole session, so it only ever shows the first frames -- it
      // cannot answer whether a LATER draw (the 2D start screen) is being
      // whitened by the render-target refusal.
      {
        static std::set<uint64_t> seen;
        const uint64_t id = (uint64_t(fetch.base_address) << 16) ^
                            (uint64_t(fetch.format) << 8) ^ uint64_t(slot);
        if (seen.insert(id).second) {
          if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
            std::fprintf(f, "FALLBACK slot=%u 0x%08X %ux%u f%u\n", slot, fetch.base_address,
                         fetch.width, fetch.height, fetch.format);
            std::fflush(f);
            std::fclose(f);
          }
        }
      }
      // Report the failure instead of dropping it: the caller cannot diagnose
      // a fetch it never sees, and this path leaves the SharedConstants index
      // at 0, which makes the shader sample an unrelated texture rather than
      // simply missing one.
      if (out && out_count && *out_count < out_capacity) {
        BoundTexture& b = out[(*out_count)++];
        b.fetch_slot = slot;
        b.fetch = fetch;
        b.sampler = sampler;
        b.resource = nullptr;
        b.resolved = false;
        // The slot is about to be pointed at the neutral white texture below,
        // so that -- not "unresolved" -- is what the shader actually samples.
        b.source = TextureSource::kFallback;
      }
      // Point the slot at a defined texture rather than leaving it at 0.
      if (shared_bytes) {
        const uint32_t fallback = FallbackSrv(context, cl);
        const uint32_t sampler_index = AcquireSampler(context, sampler);
        std::memcpy(shared_bytes + SharedTextureIndexByteOffset(0, slot), &fallback, 4);
        std::memcpy(shared_bytes + SharedSamplerIndexByteOffset(slot), &sampler_index, 4);
      }
      continue;
    }
    const uint32_t srv_index = AcquireSrv(context, resource, fetch, source);
    const uint32_t sampler_index = AcquireSampler(context, sampler);
    if (source == TextureSource::kRenderTargetBridge) {
      // Counted, not refused: see the memo notes in texture_binding.h. The
      // resource behind a bridge bind can change, but only when the pool
      // creates or resolves a target, and that rides in `volatile_guard`.
      ++stats_.memo_bridge_binds;
    }

    // Write the indices where the shader reads them. `slot` here is already
    // the slot the shader addresses: pixel shaders use 0..15 and vertex
    // shaders 16..31, and the device shadow uses the same numbering, so the
    // +16 offset is inherent to the slot and must NOT be added again.
    if (shared_bytes) {
      const uint32_t dimension = 0;  // 100% of MCLA's fetches are 2D
      // Bit 31 of the descriptor index carries the fetch constant's GAMMA sign
      // to the shader, which applies the Xenos piecewise-linear curve after
      // sampling. Riding on the index costs nothing and needs no change to the
      // generated tfetch call sites; a descriptor heap index never comes close
      // to 2^31, and shader_common.h masks the bit off before indexing.
      const uint32_t table_index = srv_index | (fetch.gamma ? 0x80000000u : 0u);
      std::memcpy(shared_bytes + SharedTextureIndexByteOffset(dimension, slot), &table_index, 4);
      std::memcpy(shared_bytes + SharedSamplerIndexByteOffset(slot), &sampler_index, 4);
    }

    if (out && out_count && *out_count < out_capacity) {
      BoundTexture& b = out[(*out_count)++];
      b.fetch_slot = slot;
      b.fetch = fetch;
      b.sampler = sampler;
      b.srv_descriptor_index = srv_index;
      b.sampler_descriptor_index = sampler_index;
      b.resource = resource;
      b.resolved = true;
      b.source = source;
    }
  }

  if (memo_enabled && memo_storable && shared_bytes && out && out_count) {
    memo_valid_ = true;
    memo_dev_ = dev;
    memo_guard_ = guard;
    std::memcpy(memo_shadow_, shadow_dwords, sizeof(shadow_dwords));
    std::memcpy(memo_shared_, shared_bytes, kMemoSharedBytes);
    memo_out_count_ = *out_count < kFetchConstantSlotCount ? *out_count : kFetchConstantSlotCount;
    for (uint32_t i = 0; i < memo_out_count_; ++i) {
      memo_out_[i] = out[i];
    }
  } else {
    // Do not merely skip the store: leaving the PREVIOUS memo in place would
    // let a draw two steps back be reused across this one.
    memo_valid_ = false;
    if (!memo_storable) {
      ++stats_.memo_refused_volatile;
    }
  }
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET
