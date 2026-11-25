#pragma once

#include <atomic>
#include <filesystem>

#include "vulkan_pipeline_cache_rules.h"

namespace plume {
struct RenderDevice;
}

namespace mcla::gpu {

// Owned by Device. Initialize before pipeline creation, save from the autosave worker or
// after joining the builders. There is only one writer at a time for this device.
class VulkanPipelineCache {
 public:
  bool Initialize(plume::RenderDevice& device);
  bool Save(plume::RenderDevice& device, bool lost);
  void LogSummary() const;

  bool loaded() const { return loaded_bytes_ != 0; }
  void NotePipelineCreated() { created_.fetch_add(1, std::memory_order_relaxed); }

 private:
  std::filesystem::path path_;
  vulkan_pipeline_cache::Identity identity_;
  uint64_t loaded_bytes_ = 0;
  uint64_t saved_bytes_ = 0;
  uint64_t last_saved_ = 0;
  std::atomic<uint64_t> created_{0};
};

}  // namespace mcla::gpu
