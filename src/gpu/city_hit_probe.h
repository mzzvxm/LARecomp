#pragma once
// ===========================================================================
// MCLA GPU renderer -- the city hit data's probe (CITY-RT M2 package H3)
// ===========================================================================
// mcla_gpu_city_hit_probe (debug): once a frame, after the top level and the hit data's descriptors are written (CarRtScene), a compute pass (hlsl/city_hit_probe_cs.hlsl) reads the pool, the
// sector table, the material table and the texture table THROUGH THE DESCRIPTOR SET, the way the hit shading will, and writes a sampled checksum of every valid slot's block, the row's flags and
// triangle count, and the tables' first words. Every 240 frames the result is read back and compared with what the host copied (city_hit_pool_rules.h JudgeProbe); the log says
// "[city hit] probe of frame N: M slots read through the GPU, K good, ..." and a WARN line names the first thing that differs. This is the proof of the pool's offsets, alignments and bindings on
// each backend, before any consumer exists; it costs one thread a slot (878 at most) reading a few thousand words each, once a frame, and is never on by default.
// ===========================================================================

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <plume_render_interface.h>

#include "city_hit_pool_rules.h"
#include "host_passes.h"

namespace mcla::gpu {

class BindlessHeap;
class Device;
class FrameRing;

class CityHitProbe {
 public:
  CityHitProbe() = default;
  ~CityHitProbe();
  CityHitProbe(const CityHitProbe&) = delete;
  CityHitProbe& operator=(const CityHitProbe&) = delete;

  // Records the pass on `list` (outside any render pass). `scene_set` is the ray tracing set of the frame slot (its t6..t9 are written), `scene_desc` its description (the layout's third set).
  // `material0` and `texture0` are the first word of the material table and the first slot of the texture table as the host has them. False: the pass could not be made (logged once).
  bool Record(Device& device, FrameRing& frames, HostPasses& passes, BindlessHeap& heap, const plume::RenderDescriptorSetDesc& scene_desc, plume::RenderCommandList* list,
              plume::RenderDescriptorSet* scene_set, const std::shared_ptr<city_hit::PoolBook>& book, uint32_t material0, uint32_t texture0, uint64_t frame);
  void Shutdown();
  uint64_t HeldBytes() const { return held_bytes_; }

 private:
  struct Push {
    uint32_t slots[4];
  };
  static_assert(sizeof(Push) == 16);
  bool Make(Device& device, HostPasses& passes, BindlessHeap& heap, const plume::RenderDescriptorSetDesc& scene_desc);
  bool EnsureBuffers(Device& device, uint32_t slots);
  void Process(uint64_t frame, uint64_t generation, uint32_t slots, uint32_t material0, uint32_t texture0, std::vector<city_hit::ProbeBlock> expected,
               std::shared_ptr<city_hit::PoolBook> book);

  plume::RenderDescriptorSetBuilder output_builder_;
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::unique_ptr<plume::RenderShader> shader_;
  std::unique_ptr<plume::RenderPipeline> pipeline_;
  std::unique_ptr<plume::RenderDescriptorSet> set_;
  std::unique_ptr<plume::RenderBuffer> counters_;
  std::unique_ptr<plume::RenderBuffer> readback_;
  uint64_t buffer_bytes_ = 0;
  uint64_t held_bytes_ = 0;
  bool made_ = false;
  bool failed_ = false;
  bool busy_ = false;
  uint64_t next_readback_ = 0;
  uint64_t generation_ = 1;
  uint64_t passes_ = 0, readbacks_ = 0, bad_ = 0;
};

}  // namespace mcla::gpu
