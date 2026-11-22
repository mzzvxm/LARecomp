// MCLA GPU renderer -- the city hit data's probe. See city_hit_probe.h.

#include "city_hit_probe.h"

#include <cstring>
#include <format>
#include <string>
#include <utility>

#include <rex/logging/log_category.h>

#include "descriptors.h"
#include "device.h"
#include "frame.h"
#include "host_shaders.h"

namespace mcla::gpu {

namespace {
constexpr const char* kPrefix = "[city hit]";
constexpr uint32_t kGroup = 64;                    // the shader's thread group
constexpr uint64_t kReadbackEveryFrames = 240;     // the result is read back this often (the pass runs every frame it is on)
}  // namespace

CityHitProbe::~CityHitProbe() = default;

void CityHitProbe::Shutdown() {
  ++generation_;
  set_.reset();
  counters_.reset();
  readback_.reset();
  pipeline_.reset();
  shader_.reset();
  layout_.reset();
  made_ = false;
  failed_ = false;
  busy_ = false;
  buffer_bytes_ = 0;
  held_bytes_ = 0;
}

bool CityHitProbe::Make(Device& device, HostPasses& passes, BindlessHeap& heap, const plume::RenderDescriptorSetDesc& scene_desc) {
  if (made_) return true;
  if (failed_) return false;
  output_builder_.begin();
  output_builder_.addReadWriteByteAddressBuffer(0);  // u0: the result
  output_builder_.end();
  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  builder.addDescriptorSet(heap.texture_set_builder());  // space 0 (not read)
  builder.addDescriptorSet(output_builder_);              // space 1
  builder.addDescriptorSet(scene_desc);                   // space 2: the ray tracing set (CITY_HIT_SPACE)
  builder.addPushConstant(0, 3, sizeof(Push), plume::RenderShaderStageFlag::COMPUTE);
  builder.end();
  layout_ = device.CreatePipelineLayout(builder, "city hit probe layout");
  const HostShaderBlob blob = CityHitProbeShader();
  shader_ = layout_ ? passes.CreateShader(blob.data, blob.size, blob.name) : nullptr;
  pipeline_ = shader_ ? device.CreateComputePipeline(plume::RenderComputePipelineDesc(layout_.get(), shader_.get(), kGroup, 1, 1), "city hit probe") : nullptr;
  if (!pipeline_) {
    failed_ = true;
    REXLOG_ERROR("{} the probe's layout, shader or pipeline could not be made: no probe this session", kPrefix);
    return false;
  }
  made_ = true;
  return true;
}

bool CityHitProbe::EnsureBuffers(Device& device, uint32_t slots) {
  const uint64_t bytes = city_hit::ProbeBufferBytes(slots);
  if (counters_ && buffer_bytes_ >= bytes) return true;
  const uint64_t made = (bytes + 4095) / 4096 * 4096;
  std::unique_ptr<plume::RenderBuffer> counters =
      device.CreateBuffer(plume::RenderBufferDesc::DefaultBuffer(made, plume::RenderBufferFlag::STORAGE | plume::RenderBufferFlag::UNORDERED_ACCESS), "city hit probe result");
  std::unique_ptr<plume::RenderBuffer> readback = device.CreateBuffer(plume::RenderBufferDesc::ReadbackBuffer(made), "city hit probe readback");
  if (!counters || !readback) return false;
  std::unique_ptr<plume::RenderDescriptorSet> set = device.CreateDescriptorSet(output_builder_, 1, 0, "city hit probe output set");
  if (!set) return false;
  set->setBuffer(0, counters.get(), made);
  set_ = std::move(set);
  counters_ = std::move(counters);
  readback_ = std::move(readback);
  buffer_bytes_ = made;
  held_bytes_ = 2 * made;
  return true;
}

bool CityHitProbe::Record(Device& device, FrameRing& frames, HostPasses& passes, BindlessHeap& heap, const plume::RenderDescriptorSetDesc& scene_desc, plume::RenderCommandList* list,
                          plume::RenderDescriptorSet* scene_set, const std::shared_ptr<city_hit::PoolBook>& book, uint32_t material0, uint32_t texture0, uint64_t frame) {
  if (!list || !scene_set || !book || !book->ready()) return false;
  const uint32_t slots = book->table().slots();
  if (slots == 0 || !Make(device, passes, heap, scene_desc) || !EnsureBuffers(device, slots)) return false;
  const Push push = {{slots, 0, 0, 0}};
  list->barriers(plume::RenderBarrierStage::COMPUTE, plume::RenderBufferBarrier(counters_.get(), plume::RenderBufferAccess::READ | plume::RenderBufferAccess::WRITE));
  list->setComputePipelineLayout(layout_.get());
  list->setComputeDescriptorSet(heap.texture_set(), 0);
  list->setComputeDescriptorSet(set_.get(), 1);
  list->setComputeDescriptorSet(scene_set, 2);
  list->setPipeline(pipeline_.get());
  list->setComputePushConstants(0, &push, 0, sizeof(push));
  list->dispatch((slots + kGroup - 1) / kGroup, 1, 1);
  ++passes_;
  if (!busy_ && frame >= next_readback_) {
    const uint64_t bytes = city_hit::ProbeBufferBytes(slots);
    list->barriers(plume::RenderBarrierStage::COPY, plume::RenderBufferBarrier(counters_.get(), plume::RenderBufferAccess::READ));
    list->copyBufferRegion(plume::RenderBufferReference(readback_.get(), 0), plume::RenderBufferReference(counters_.get(), 0), bytes);
    busy_ = true;
    next_readback_ = frame + kReadbackEveryFrames;
    // What the host expects is what it holds NOW: the blocks of this frame's table (the pass reads the table as it was copied for this frame).
    frames.RetireAction([this, frame, generation = generation_, slots, material0, texture0, expected = book->ProbeList(), book]() mutable {
      Process(frame, generation, slots, material0, texture0, std::move(expected), book);
    });
  }
  return true;
}

void CityHitProbe::Process(uint64_t frame, uint64_t generation, uint32_t slots, uint32_t material0, uint32_t texture0, std::vector<city_hit::ProbeBlock> expected,
                           std::shared_ptr<city_hit::PoolBook> book) {
  busy_ = false;
  if (generation != generation_ || !readback_) return;
  const void* mapped = readback_->map();
  if (!mapped) {
    REXLOG_ERROR("{} probe of frame {}: the readback could not be mapped", kPrefix, frame);
    return;
  }
  const size_t word_count = size_t(city_hit::ProbeBufferBytes(slots) / 4);
  std::vector<uint32_t> words(word_count);
  std::memcpy(words.data(), mapped, word_count * 4);
  readback_->unmap();
  ++readbacks_;
  // A block that was evicted or replaced after the pass was recorded is not judged: the slot's block is still the one that was expected when the book holds the same checksum.
  const city_hit::ProbeVerdict v = city_hit::JudgeProbe(words.data(), words.size(), slots, material0, texture0, expected, [&](uint32_t slot, uint32_t checksum) {
    const city_hit::Block& b = book->block_at_slot(slot);
    return b.present && b.flags != 0 && b.checksum == checksum;
  });
  const bool good = v.header_ok && v.bad_checksum == 0 && v.bad_row == 0 && v.checked != 0 && v.good == v.checked;
  bad_ += good ? 0 : 1;
  const std::string book_check = book->Verify();
  if (good && book_check.empty()) {
    REXLOG_INFO("{} probe of frame {}: {} of {} slots read through the GPU, all {} good ({} evicted or replaced since: not judged); the pool's book is sound", kPrefix, frame, v.checked, slots, v.good,
                v.stale_skipped);
  } else {
    REXLOG_WARN("{} probe of frame {}: {} slots checked, {} good, {} with other bytes in the pool than were copied, {} with another sector row, {} not judged, header {}{}{}{}", kPrefix, frame, v.checked,
                v.good, v.bad_checksum, v.bad_row, v.stale_skipped, v.header_ok ? "ok" : "WRONG", v.first_problem.empty() ? "" : "; ", v.first_problem,
                book_check.empty() ? "" : std::format("; THE BOOK IS NOT SOUND: {}", book_check));
  }
}

}  // namespace mcla::gpu
