#pragma once

#include <array>
#include <memory>

#include "cascade_fit.h"
#include "edram.h"
#include "frame.h"

namespace mcla::gpu {

class Device;
class HostPasses;
class PassTiming;
namespace temporal {
struct Camera;
}

class ShadowFitDepth {
 public:
  void Record(Device& device, FrameRing& frames, HostPasses& passes, EdramTargets& edram, PassTiming& timing,
              plume::RenderCommandList* list, EdramTargets::Target& source,
              uint32_t width, uint32_t height, uint64_t frame);
  void NoteCamera(uint64_t frame, const temporal::Camera& camera);
  void Shutdown();

 private:
  struct Slot {
    std::unique_ptr<plume::RenderBuffer> output;
    std::unique_ptr<plume::RenderBuffer> readback;
    std::unique_ptr<plume::RenderDescriptorSet> set;
    uint64_t bytes = 0;
    uint64_t frame = 0;
    bool busy = false;
    cascade_fit::DepthSample sample{};
    std::array<double, 4> lens{};
  };

  bool Prepare(Device& device, HostPasses& passes, unsigned shader);
  bool PrepareSlot(Device& device, Slot& slot, uint64_t bytes);
  void Readback(Slot& slot, uint32_t token);

  std::array<Slot, kFramesInFlight> slots_{};
  plume::RenderDescriptorSetBuilder builder_;
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::array<std::unique_ptr<plume::RenderShader>, 2> shaders_;
  std::array<std::unique_ptr<plume::RenderPipeline>, 2> pipelines_;
  std::array<bool, 2> listed_{};
  std::array<bool, 2> failed_{};
};

}  // namespace mcla::gpu
