#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — texture / sampler binding.
// See texture_binding.h for the fetch-slot -> descriptor-index chain.

#include "texture_binding.h"

#include <cstdio>
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
// Handing the register straight over is NOT correct on its own, though.
// Measured in-game: it turns the night sky dome brick red, and the sky's
// colour comes from those very LUTs, so red and blue ended up swapped where
// they were right before.
//
// The SDK's own (Xenia-derived) cache composes the guest swizzle with a
// PER-FORMAT host swizzle -- src/graphics/pipeline/texture/cache.cpp
// GuestToHostSwizzle, fed by D3D12TextureCache::GetHostFormatSwizzle -- and
// for k_8_8_8_8 that host half is the identity, so there the guest swizzle
// does apply verbatim. What differs here is the SOURCE: a resource handed back
// by the render-target bridge was rendered by this runtime and is already in
// host channel order, so a guest swizzle authored for Xenos-ordered memory
// must not be applied to it. Only a texture decoded out of guest memory can
// want it. That is the split this applies, and it is still behind
// mcla_native_gfx_texture_swizzle (default off) until it is checked against a
// known-good frame rather than reasoned about.
uint32_t ShaderComponentMappingForSwizzle(uint32_t swizzle, TextureSource source) {
  if (!REXCVAR_GET(mcla_native_gfx_texture_swizzle) ||
      source != TextureSource::kGuestDecode) {
    return D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  }
  for (uint32_t i = 0; i < 4; ++i) {
    if (((swizzle >> (i * 3)) & 0x7u) > 5u) {
      // 6 and 7 are reserved; a fetch constant that carries them is not
      // something to guess at.
      return D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    }
  }
  return (swizzle & 0xFFFu) | (1u << 12);
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
  desc.Shader4ComponentMapping = ShaderComponentMappingForSwizzle(fetch.swizzle, source);
  desc.Texture2D.MipLevels = 1;
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
                            uint32_t out_capacity) {
  if (out_count) {
    *out_count = 0;
  }
  const uint32_t shadow = dev + kDevFetchShadowOffset;
  for (uint32_t slot = 0; slot < kFetchConstantSlotCount; ++slot) {
    const uint32_t ea = shadow + slot * kFetchGroupDwords * 4;
    uint32_t d[6];
    for (uint32_t i = 0; i < 6; ++i) {
      d[i] = R32(base, ea + 4 * i);
    }
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
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET
