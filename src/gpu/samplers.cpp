// MCLA GPU renderer -- samplers from the guest's texture fetch constants. See
// samplers.h.
//
// Mapping followed: Xenia's D3D12TextureCache::GetSamplerParameters and
// WriteSampler (src/graphics/d3d12/texture_cache.cpp in the ReXGlue SDK;
// Copyright Ben Vanik, the Xenia authors, BSD-3-Clause). Reimplemented over
// Plume's sampler description.

#include "samplers.h"

#include <algorithm>
#include <cfloat>
#include <cstring>

#include <rex/graphics/xenos.h>
#include <rex/logging.h>

#include "descriptors.h"
#include "device.h"

namespace mcla::gpu {

namespace xe = rex::graphics::xenos;

namespace {

plume::RenderTextureAddressMode AddressMode(uint32_t clamp) {
  switch (xe::ClampMode(clamp)) {
    case xe::ClampMode::kRepeat: return plume::RenderTextureAddressMode::WRAP;
    case xe::ClampMode::kMirroredRepeat: return plume::RenderTextureAddressMode::MIRROR;
    case xe::ClampMode::kClampToEdge: return plume::RenderTextureAddressMode::CLAMP;
    case xe::ClampMode::kMirrorClampToEdge: return plume::RenderTextureAddressMode::MIRROR_ONCE;
    // No clamp-to-half-edge on the host; no Direct3D 9 equivalent either (Xenia).
    case xe::ClampMode::kClampToHalfway: return plume::RenderTextureAddressMode::CLAMP;
    case xe::ClampMode::kMirrorClampToHalfway: return plume::RenderTextureAddressMode::MIRROR_ONCE;
    case xe::ClampMode::kClampToBorder: return plume::RenderTextureAddressMode::BORDER;
    case xe::ClampMode::kMirrorClampToBorder: return plume::RenderTextureAddressMode::MIRROR_ONCE;
  }
  return plume::RenderTextureAddressMode::WRAP;
}

}  // namespace

uint64_t Samplers::Description::Key() const {
  uint64_t k = 0;
  k |= uint64_t(clamp[0]);
  k |= uint64_t(clamp[1]) << 3;
  k |= uint64_t(clamp[2]) << 6;
  k |= uint64_t(mag_linear) << 9;
  k |= uint64_t(min_linear) << 10;
  k |= uint64_t(mip_linear) << 11;
  k |= uint64_t(mip_base_map) << 12;
  k |= uint64_t(aniso) << 13;
  k |= uint64_t(border) << 16;
  k |= uint64_t(mip_min) << 18;
  k |= uint64_t(mip_max) << 22;
  k |= uint64_t(uint16_t(lod_bias) & 0x3FFu) << 26;
  return k;
}

Samplers::~Samplers() {
  Shutdown();
}

void Samplers::Initialize(Device& device, BindlessHeap& heap) {
  device_ = &device;
  heap_ = &heap;
}

void Samplers::Shutdown() {
  slots_.clear();
  samplers_.clear();
  device_ = nullptr;
  heap_ = nullptr;
}

uint32_t Samplers::Get(const uint32_t fetch_dwords[6]) {
  xe::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, fetch_dwords, sizeof(fetch));

  Description d;
  d.clamp[0] = uint8_t(fetch.clamp_x);
  d.clamp[1] = uint8_t(fetch.clamp_y);
  d.clamp[2] = uint8_t(fetch.clamp_z);
  const xe::TextureFilter mip_filter = fetch.mip_filter;
  d.mip_base_map = mip_filter == xe::TextureFilter::kBaseMap;
  xe::AnisoFilter aniso = std::min(fetch.aniso_filter, xe::AnisoFilter::kMax_16_1);
  d.aniso = uint8_t(aniso);
  if (aniso != xe::AnisoFilter::kDisabled) {
    d.mag_linear = d.min_linear = d.mip_linear = 1;
  } else {
    d.mag_linear = fetch.mag_filter == xe::TextureFilter::kLinear;
    d.min_linear = fetch.min_filter == xe::TextureFilter::kLinear;
    d.mip_linear = mip_filter == xe::TextureFilter::kLinear;
  }
  d.border = uint8_t(fetch.border_color);
  d.mip_min = uint8_t(fetch.mip_min_level);
  d.mip_max = uint8_t(fetch.mip_max_level);
  d.lod_bias = int16_t(fetch.lod_bias);

  const uint64_t key = d.Key();
  const auto it = slots_.find(key);
  if (it != slots_.end()) {
    return it->second;
  }

  plume::RenderSamplerDesc desc;
  desc.magFilter = d.mag_linear ? plume::RenderFilter::LINEAR : plume::RenderFilter::NEAREST;
  desc.minFilter = d.min_linear ? plume::RenderFilter::LINEAR : plume::RenderFilter::NEAREST;
  desc.mipmapMode = d.mip_linear ? plume::RenderMipmapMode::LINEAR : plume::RenderMipmapMode::NEAREST;
  desc.addressU = AddressMode(d.clamp[0]);
  desc.addressV = AddressMode(d.clamp[1]);
  desc.addressW = AddressMode(d.clamp[2]);
  desc.anisotropyEnabled = aniso != xe::AnisoFilter::kDisabled;
  desc.maxAnisotropy = desc.anisotropyEnabled ? (1u << (uint32_t(aniso) - 1)) : 1u;
  desc.mipLODBias = float(d.lod_bias) * (1.0f / 32.0f);
  switch (xe::BorderColor(d.border)) {
    case xe::BorderColor::k_ABGR_White:
      desc.borderColor = plume::RenderBorderColor::OPAQUE_WHITE;
      break;
    case xe::BorderColor::k_ABGR_Black:
      desc.borderColor = plume::RenderBorderColor::TRANSPARENT_BLACK;
      break;
    default:
      // The YCbCr blacks have no host border colour; transparent black is the
      // closest, and the case is reported.
      desc.borderColor = plume::RenderBorderColor::TRANSPARENT_BLACK;
      if (reported_ < 8) {
        ++reported_;
        REXLOG_WARN("[gpu] sampler: border colour {} has no host equivalent; transparent black "
                    "is used",
                    uint32_t(d.border));
      }
      break;
  }
  desc.minLOD = float(d.mip_min);
  if (d.mip_base_map) {
    // Xenia: only the base level, keeping magnification vs minification.
    desc.maxLOD = desc.minLOD + (desc.anisotropyEnabled ? 0.0f : 0.25f);
    desc.mipmapMode = plume::RenderMipmapMode::NEAREST;
  } else {
    desc.maxLOD = float(std::max(d.mip_min, d.mip_max));
  }

  std::unique_ptr<plume::RenderSampler> sampler = device_->CreateSampler(desc, "guest sampler");
  uint32_t slot = kInvalidBindlessSlot;
  if (sampler) {
    slot = heap_->AddSampler(*sampler);
  }
  if (slot == kInvalidBindlessSlot) {
    if (reported_ < 8) {
      ++reported_;
      REXLOG_ERROR("[gpu] sampler for fetch {:08X} {:08X} could not be made; linear wrap is used",
                   fetch_dwords[3], fetch_dwords[4]);
    }
    slots_.emplace(key, kSamplerLinearWrap);
    return kSamplerLinearWrap;
  }
  samplers_.push_back(std::move(sampler));
  slots_.emplace(key, slot);
  return slot;
}

}  // namespace mcla::gpu
