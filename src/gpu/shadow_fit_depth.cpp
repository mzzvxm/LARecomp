#include "shadow_fit_depth.h"

#include <bit>
#include <cstring>
#include <mutex>

#include <rex/logging/log_category.h>

#include "cascade_schedule.h"
#include "device.h"
#include "host_passes.h"
#include "host_shaders.h"
#include "pass_timing.h"
#include "settings/immortal.h"
#include "settings/larecomp_settings.h"
#include "temporal_camera.h"

namespace mcla::gpu {
namespace cf = cascade_fit;
namespace {

struct Mailbox {
  std::mutex mutex;
  cf::DepthSample latest;
};
Mailbox& DepthMailbox() {
  static larecomp::Immortal<Mailbox> mailbox;
  return *mailbox;
}

constexpr uint32_t kResultBytes = 2 * sizeof(uint32_t);
constexpr uint32_t kTile = 32;
constexpr uint32_t kThreads = 8;
struct Push {
  uint32_t width, height, cols, tiles, samples, token, finish;
};
constexpr uint64_t PipelineKey(unsigned shader) {
  return HostPrecache::Key(HostPrecache::kAmbientOcclusion, 0x400u + shader);
}

}  // namespace

cf::DepthSample cf::LatestDepth() {
  auto& mailbox = DepthMailbox();
  std::lock_guard lock(mailbox.mutex);
  return mailbox.latest;
}

bool ShadowFitDepth::Prepare(Device& device, HostPasses& passes, unsigned shader) {
  if (failed_[shader]) return false;
  if (!layout_) {
    builder_.begin();
    builder_.addTexture(0);
    builder_.addReadWriteByteAddressBuffer(1);
    builder_.end();
    plume::RenderPipelineLayoutBuilder layout;
    layout.begin(false, false);
    layout.addDescriptorSet(builder_);
    layout.addPushConstant(0, 1, sizeof(Push), plume::RenderShaderStageFlag::COMPUTE);
    layout.end();
    layout_ = device.CreatePipelineLayout(layout, "sun shadow visible depth layout");
  }
  auto& precache = passes.precache();
  const auto key = PipelineKey(shader);
  const auto blob = ShadowFitDepthShader(shader != 0);
  if (pipelines_[shader]) return true;
  const auto make = [this, &device, shader, name = blob.name] {
    return device.CreateComputePipeline(
        plume::RenderComputePipelineDesc(layout_.get(), shaders_[shader].get(), kThreads, kThreads, 1),
        name);
  };
  if (!listed_[shader]) {
    shaders_[shader] = layout_ ? passes.CreateShader(blob.data, blob.size, blob.name) : nullptr;
    if (!shaders_[shader]) {
      failed_[shader] = true;
      return false;
    }
    listed_[shader] = true;
    precache.AddLive("sun shadow visible depth", {{key, blob.name, make}});
  }
  if (!precache.Ready(key)) return false;
  pipelines_[shader] = precache.Acquire(key, blob.name, make);
  failed_[shader] = !pipelines_[shader];
  return !failed_[shader];
}

bool ShadowFitDepth::PrepareSlot(Device& device, Slot& slot, uint64_t bytes) {
  if (slot.busy) return false;
  if (!slot.set) slot.set = device.CreateDescriptorSet(builder_, 2, 0, "sun shadow depth set");
  if (!slot.readback) {
    slot.readback = device.CreateBuffer(plume::RenderBufferDesc::ReadbackBuffer(kResultBytes),
                                        "sun shadow depth readback");
  }
  if (slot.bytes < bytes) {
    const auto flags = plume::RenderBufferFlag::STORAGE | plume::RenderBufferFlag::UNORDERED_ACCESS;
    auto output = device.CreateBuffer(plume::RenderBufferDesc::DefaultBuffer(bytes, flags),
                                      "sun shadow depth reduction");
    if (!output) return false;
    slot.output = std::move(output);
    slot.bytes = bytes;
  }
  return slot.set && slot.readback && slot.output;
}

void ShadowFitDepth::Record(Device& device, FrameRing& frames, HostPasses& passes, EdramTargets& edram,
                           PassTiming& timing,
                           plume::RenderCommandList* list, EdramTargets::Target& source,
                           uint32_t width, uint32_t height, uint64_t frame) {
  if (!list || width == 0 || height == 0 || !source.texture || !source.sample_view) return;
  const unsigned shader = source.host_samples > 1;
  const uint32_t cols = (width + kTile - 1) / kTile;
  const uint32_t rows = (height + kTile - 1) / kTile;
  const uint32_t tiles = cols * rows;
  Slot& slot = slots_[frames.slot()];
  if (!Prepare(device, passes, shader) ||
      !PrepareSlot(device, slot, kResultBytes + uint64_t(tiles) * sizeof(uint32_t))) return;

  const uint32_t token = uint32_t(frame) + 1u;
  slot.frame = frame;
  slot.sample = {{}, cf::Clock::now(), 0.0, cf::depth_epoch.load(),
                 cascade_schedule::camera_cut.load(std::memory_order_relaxed)};
  slot.lens = {};

  using Layout = plume::RenderTextureLayout;
  namespace Stage = plume::RenderBarrierStage;
  timing.Mark(list, "sun shadow visible depth");
  edram.Transition(list, source, Layout::SHADER_READ);
  slot.set->setTexture(0, source.texture.get(), Layout::SHADER_READ, source.sample_view.get());
  slot.set->setBuffer(1, slot.output.get(), slot.bytes);
  list->setComputePipelineLayout(layout_.get());
  list->setComputeDescriptorSet(slot.set.get(), 0);
  list->setPipeline(pipelines_[shader].get());
  Push push{width, height, cols, tiles, source.host_samples, token, 0};
  const plume::RenderBufferBarrier rw(slot.output.get(),
      plume::RenderBufferAccess::READ | plume::RenderBufferAccess::WRITE);
  list->barriers(Stage::COMPUTE, rw);
  list->setComputePushConstants(0, &push, 0, sizeof(push));
  list->dispatch(cols, rows, 1);
  list->barriers(Stage::COMPUTE, rw);
  push.finish = 1;
  list->setComputePushConstants(0, &push, 0, sizeof(push));
  list->dispatch(1, 1, 1);
  // CHECK: verify the depth view and the UAV/copy/readback barriers on both APIs.
  list->barriers(Stage::COPY, plume::RenderBufferBarrier(slot.output.get(), plume::RenderBufferAccess::READ));
  list->copyBufferRegion(plume::RenderBufferReference(slot.readback.get(), 0),
                         plume::RenderBufferReference(slot.output.get(), 0), kResultBytes);
  slot.busy = true;
  frames.RetireAction([this, index = frames.slot(), token] { Readback(slots_[index], token); });
}

void ShadowFitDepth::NoteCamera(uint64_t frame, const temporal::Camera& camera) {
  for (auto& slot : slots_) {
    if (!slot.busy || slot.frame != frame) continue;
    for (unsigned axis = 0; axis < 3; ++axis) {
      slot.sample.pose.eye[axis] = camera.position[axis];
      slot.sample.pose.forward[axis] = camera.forward[axis];
      slot.sample.pose.up[axis] = camera.up[axis];
    }
    const auto& p = camera.projection;
    slot.lens = {p.m[2][2], p.m[2][3], p.m[3][2], p.m[3][3]};
  }
}

void ShadowFitDepth::Readback(Slot& slot, uint32_t token) {
  slot.busy = false;
  if (!slot.readback || slot.sample.epoch != cf::depth_epoch.load()) return;
  const void* map = slot.readback->map();
  if (!map) return;
  uint32_t value[2];
  std::memcpy(value, map, sizeof(value));
  slot.readback->unmap();
  if (value[0] != token) return;
  slot.sample.max_depth = cf::LinearDepth(std::bit_cast<float>(value[1]), slot.lens);
  if (larecomp::settings::mcla_gpu_shadow_ledger.Get() && slot.frame % 120 == 0) {
    REXLOG_INFO("[shadow fit] frame {}: visible depth maximum {:.2f} m, readback age {:.1f} ms",
                slot.frame, slot.sample.max_depth,
                std::chrono::duration<double, std::milli>(cf::Clock::now() - slot.sample.recorded).count());
  }
  auto& mailbox = DepthMailbox();
  std::lock_guard lock(mailbox.mutex);
  if (slot.sample.recorded > mailbox.latest.recorded) mailbox.latest = slot.sample;
}

void ShadowFitDepth::Shutdown() {
  for (auto& slot : slots_) slot = {};
  for (auto& shader : shaders_) shader.reset();
  for (auto& pipeline : pipelines_) pipeline.reset();
  layout_.reset();
  listed_ = {};
  failed_ = {};
  cf::depth_epoch.fetch_add(1);
}

}  // namespace mcla::gpu
