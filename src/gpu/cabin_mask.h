#pragma once

#include <chrono>
#include <memory>
#include <unordered_map>
#include <vector>

#include "car_frame.h"
#include "host_passes.h"
#include "mirror_glass_rules.h"

namespace mcla::gpu {

class PassTiming;

// A coverage image made by replaying rigid, opaque cabin geometry through
// the mirror aperture. Overlay reads it in aperture (glass model) space.
class CabinMask {
 public:
  void Shutdown();
  struct Result {
    uint32_t slot;
    const char* reason;
  };
  Result Record(plume::RenderCommandList& list, Device& device, HostPasses& passes, PassTiming& timing,
                const car_frame::Frame& frame, const car_frame::Draw& mirror, const mirror_glass::Result& glass);

 private:
  struct Part {
    size_t draw;
    uint64_t mesh;
    uint32_t variant;
    temporal::Mat4 to_mirror;
  };

  bool Prepare(Device& device, HostPasses& passes);
  std::unique_ptr<plume::RenderPipeline> MakePipeline(uint32_t variant) const;
  plume::RenderPipeline* Pipeline(uint32_t variant);

  Device* device_ = nullptr;
  HostPasses* passes_ = nullptr;
  HostImage image_;
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::unique_ptr<plume::RenderShader> vertex_, pixel_;
  struct PipelineEntry {
    std::unique_ptr<plume::RenderPipeline> pipeline;
    bool resolved = false;
  };
  std::unordered_map<uint32_t, PipelineEntry> pipelines_;
  std::vector<Part> parts_;
  std::vector<Part> cached_parts_;
  std::chrono::steady_clock::time_point updated_{};
  mirror_view::V3 last_eye_;
  uint64_t last_glass_ = 0;
  bool failed_ = false;
};

}  // namespace mcla::gpu
