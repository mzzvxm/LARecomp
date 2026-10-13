#pragma once
// ===========================================================================
// MCLA GPU renderer -- samplers from the guest's texture fetch constants
// ===========================================================================
// The sampling state lives in the
// texture fetch constant (xe_gpu_texture_fetch_t, rex/graphics/xenos.h): clamp
// modes, min/mag/mip filters, anisotropy, LOD bias, the mip range and the
// border colour. The mapping is Xenia's D3D12 texture cache
// (D3D12TextureCache::GetSamplerParameters / WriteSampler in the SDK,
// BSD-3-Clause) with two differences: the translated shaders cannot add the
// fetch constant's LOD bias themselves, so it goes into the sampler; and the
// instruction-level filter overrides of a tfetch are not honoured, because the
// generated HLSL samples with the bound sampler (the fetch constant's filters
// are used).
//
// Each distinct description gets one sampler and one bindless sampler slot for
// the life of the renderer (27 distinct in the measured frames).
//
// Threading: render thread only.
// ===========================================================================

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>

namespace mcla::gpu {

class BindlessHeap;
class Device;

class Samplers {
 public:
  Samplers() = default;
  ~Samplers();
  Samplers(const Samplers&) = delete;
  Samplers& operator=(const Samplers&) = delete;

  void Initialize(Device& device, BindlessHeap& heap);
  // The GPU must be idle; before the BindlessHeap shuts down.
  void Shutdown();

  // The bindless sampler slot for the texture fetch constant `fetch` (six
  // host-order dwords), with the mip range the texture really has: `mip_min`
  // and `mip_max` as texture_util::GetSubresourcesFromFetchConstant leaves
  // them (clamped to the size, the base excluded when it is absent), which
  // the raw fetch fields are not. Falls back to the fixed
  // linear-wrap sampler, logged, when one cannot be made.
  uint32_t Get(const uint32_t fetch[6], uint32_t mip_min, uint32_t mip_max);

  size_t size() const { return slots_.size(); }

 private:
  struct Description {
    uint8_t clamp[3] = {};
    uint8_t mag_linear = 0, min_linear = 0, mip_linear = 0, mip_base_map = 0;
    uint8_t aniso = 0;           // xenos::AnisoFilter, 0 = off
    uint8_t border = 0;          // xenos::BorderColor
    uint8_t mip_min = 0, mip_max = 0;
    int16_t lod_bias = 0;        // 5 fractional bits
    uint64_t Key() const;
  };

  Device* device_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  std::unordered_map<uint64_t, uint32_t> slots_;  // description key -> bindless slot
  std::vector<std::unique_ptr<plume::RenderSampler>> samplers_;
  uint32_t reported_ = 0;
};

}  // namespace mcla::gpu
