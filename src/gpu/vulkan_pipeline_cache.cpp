#include "vulkan_pipeline_cache.h"

#include <exception>
#include <fstream>
#include <vector>

#include <plume_render_interface.h>
#include <rex/logging/log_category.h>

#include "os/file_replace.h"

namespace mcla::gpu {

namespace {

// The current Plume pin predates the cache API. The pick adds virtual methods on RenderDevice.
template <typename RenderDevice>
bool GetCacheData(RenderDevice& device, std::vector<uint8_t>& data) {
  if constexpr (requires { device.getPipelineCacheData(data); }) {
    return device.getPipelineCacheData(data);
  }
  return false;
}

template <typename RenderDevice>
bool LoadCacheData(RenderDevice& device, std::span<const uint8_t> data) {
  if constexpr (requires { device.loadPipelineCacheData(data.data(), data.size()); }) {
    return device.loadPipelineCacheData(data.data(), data.size());
  }
  return false;
}

}  // namespace

bool VulkanPipelineCache::Initialize(plume::RenderDevice& device) {
  namespace rules = vulkan_pipeline_cache;
  try {
    // Ask the fresh cache for its header. It identifies the device that Plume actually opened.
    // CHECK: build with the Plume pick and confirm a second Vulkan launch loads the saved blob.
    std::vector<uint8_t> data;
    if (!GetCacheData(device, data)) {
      return false;
    }
    const auto identity = rules::ReadIdentity(data);
    if (!identity) {
      return false;
    }
    identity_ = *identity;
    // Same cache directory as the D3D12 pipeline library. Resolve it before workers start.
    std::error_code ec;
    const auto folder = std::filesystem::absolute("cache", ec);
    if (ec) {
      return false;
    }
    path_ = folder / rules::FileName(identity_);

    std::ifstream file(path_, std::ios::binary | std::ios::ate);
    if (!file) {
      return true;  // cold start
    }
    const std::streamoff size = file.tellg();
    if (size < 0 || !rules::ValidSize(uint64_t(size))) {
      REXLOG_WARN("[gpu] Vulkan pipeline cache: ignoring a file of {} bytes", size);
      return true;
    }
    data.resize(size_t(size));
    file.seekg(0);
    const bool read = bool(file.read(reinterpret_cast<char*>(data.data()), size));
    file.close();
    // Plume merges the validated blob into its device cache. This is before any pipeline
    // creation, so the destination of vkMergePipelineCaches is externally synchronized.
    if (read && rules::ValidBlob(data, identity_) && LoadCacheData(device, data)) {
      loaded_bytes_ = data.size();
    } else {
      REXLOG_WARN("[gpu] Vulkan pipeline cache: ignoring unreadable or incompatible data");
    }
  } catch (const std::exception& error) {
    REXLOG_WARN("[gpu] Vulkan pipeline cache: could not load data ({})", error.what());
  }
  return !path_.empty();
}

bool VulkanPipelineCache::Save(plume::RenderDevice& device, bool lost) {
  namespace rules = vulkan_pipeline_cache;
  namespace files = larecomp::os;
  // Take the count before the snapshot. A creation concurrent with the snapshot remains
  // eligible for the exit save even if it finishes before vkGetPipelineCacheData returns.
  const uint64_t created = created_.load(std::memory_order_relaxed);
  if (!rules::ShouldSave(lost, created, last_saved_)) {
    return !lost && (loaded() || last_saved_ != 0);
  }

  std::filesystem::path temp;
  try {
    std::vector<uint8_t> data;
    // The pick leaves the cache flags at zero. Vulkan synchronizes graphics and compute
    // creation and vkGetPipelineCacheData. Plume retries VK_INCOMPLETE if the blob grows.
    // CHECK: time the parallel precache and the first frames on real Vulkan drivers.
    if (!GetCacheData(device, data) || !rules::ValidBlob(data, identity_)) {
      REXLOG_WARN("[gpu] Vulkan pipeline cache: could not get a valid snapshot");
      return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    if (ec) {
      REXLOG_WARN("[gpu] Vulkan pipeline cache: could not create the cache folder ({})", ec.message());
      return false;
    }
    temp = files::TempPathFor(path_);
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    file.close();
    if (!file) {
      std::filesystem::remove(temp, ec);
      REXLOG_WARN("[gpu] Vulkan pipeline cache: could not write the snapshot");
      return false;
    }

    std::string why;
    const auto moved = files::MoveIntoPlace(temp, path_, &why);
    if (moved == files::MoveResult::kFailed) {
      REXLOG_WARN("[gpu] Vulkan pipeline cache: could not publish the snapshot ({})", why);
      return false;
    }
    if (moved == files::MoveResult::kMoved) {
      saved_bytes_ += data.size();
    }
    last_saved_ = created;
    return true;
  } catch (const std::exception& error) {
    if (!temp.empty()) {
      std::error_code ec;
      std::filesystem::remove(temp, ec);
    }
    REXLOG_WARN("[gpu] Vulkan pipeline cache: could not save data ({})", error.what());
    return false;
  }
}

void VulkanPipelineCache::LogSummary() const {
  // Plume gives no hit feedback. This counts successful graphics and compute creations
  // on a device that loaded a cache with a payload.
  const uint64_t warm_creations = vulkan_pipeline_cache::Warm(loaded_bytes_)
                                      ? created_.load(std::memory_order_relaxed)
                                      : 0;
  REXLOG_INFO("[gpu] Vulkan pipeline cache {}: {} bytes loaded, {} pipelines created with a warm "
              "cache, {} bytes saved",
              path_.filename().string(), loaded_bytes_, warm_creations, saved_bytes_);
}

}  // namespace mcla::gpu
