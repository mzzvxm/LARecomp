#include "car_rt_denoise_v2.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <unordered_map>

#include <rex/logging/log_category.h>

#include "car_rt_scene.h"
#include "descriptors.h"
#include "device.h"
#include "host_precache.h"
#include "host_shaders.h"
#include "texture_bytes.h"

namespace mcla::gpu {
namespace {

namespace rules = car_rt_denoise_v2;
using Rect = car_rt_refl::PaintRect;
using Layout = plume::RenderTextureLayout;
namespace Stage = plume::RenderBarrierStage;

constexpr uint32_t kGroup = 8;
constexpr const char* kName = "car rt denoise v2";
constexpr auto kFlags = plume::RenderTextureFlag::STORAGE | plume::RenderTextureFlag::UNORDERED_ACCESS;
constexpr auto kValue = plume::RenderFormat::R16G16B16A16_FLOAT;
constexpr auto kFeature = plume::RenderFormat::R32G32B32A32_FLOAT;
constexpr auto kMotionBytes = car_rt_rules::kMaxInstances * 64u;
static_assert(car_rt_rules::kMaxInstances <= 4096);

uint64_t Key() { return HostPrecache::Key(HostPrecache::kCarRtRefl, 9); }

void Transition(plume::RenderCommandList& list, HostImage& image, Layout layout,
                plume::RenderBarrierStages stage = Stage::COMPUTE) {
  list.barriers(stage, plume::RenderTextureBarrier(image.texture.get(), layout));
  image.layout = layout;
}

bool SameGrid(const CarRtDenoiseV2::Frame& a, const CarRtDenoiseV2::Frame& b) {
  return a.width == b.width && a.height == b.height && a.render_width == b.render_width &&
         a.render_height == b.render_height && a.divisor == b.divisor;
}

}  // namespace

void CarRtDenoiseV2::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes) {
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  passes_ = &passes;
  output_builder_.begin();
  output_builder_.addReadWriteTexture(0);
  output_builder_.addReadWriteTexture(1);
  output_builder_.end();
  motion_builder_.begin();
  motion_builder_.addByteAddressBuffer(0);
  motion_builder_.end();

  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(false, false);
  builder.addDescriptorSet(heap.texture_set_builder());
  builder.addDescriptorSet(output_builder_);
  builder.addDescriptorSet(motion_builder_);
  builder.addPushConstant(0, 3, sizeof(Push), plume::RenderShaderStageFlag::COMPUTE);
  builder.end();
  layout_ = device.CreatePipelineLayout(builder, kName);
  const auto blob = CarRtDenoiseV2Shader();
  shader_ = passes.CreateShader(blob.data, blob.size, blob.name);
  listed_ = failed_ = false;
}

std::unique_ptr<plume::RenderPipeline> CarRtDenoiseV2::MakePipeline() {
  if (!device_ || !layout_ || !shader_) return nullptr;
  return device_->CreateComputePipeline({layout_.get(), shader_.get(), kGroup, kGroup, 1}, kName);
}

void CarRtDenoiseV2::Precache(bool startup) {
  if (listed_ || !passes_ || !layout_ || !shader_) return;
  auto make = [this] { return MakePipeline(); };
  if (startup) {
    passes_->precache().Add(Key(), kName, make);
  } else {
    passes_->precache().AddLive(kName, {{Key(), kName, make}});
  }
  listed_ = true;
}

bool CarRtDenoiseV2::Prepare(const Frame& frame, const CarRtView& view, bool paint, bool glass) {
  if (!passes_ || !frames_ || !layout_ || !shader_ || failed_) return false;
  if (!paint && paint_.values[0].texture) ReleaseHistory(paint_);
  if (!glass && glass_.values[0].texture) ReleaseHistory(glass_);
  Precache(false);
  if (!pipeline_) {
    if (!passes_->precache().Ready(Key())) return false;
    pipeline_ = passes_->precache().Acquire(Key(), kName, [this] { return MakePipeline(); });
    if (!pipeline_) {
      REXLOG_ERROR("[car rt refl] V2 denoise pipeline unavailable");
      failed_ = true;
      return false;
    }
  }
  const bool previous = rules::IsConsecutive(frame_.number, frame.number, frame.reset, SameGrid(frame_, frame));
  frame_ = frame;
  return UploadMotion(view, previous) &&
         passes_->EnsureImage(guide_, frame.width, frame.height, kFeature, "car rt v2 trace guide", kFlags) &&
         passes_->EnsureImage(scratch_, frame.width, frame.height, kValue, "car rt v2 scratch", kFlags);
}

bool CarRtDenoiseV2::UploadMotion(const CarRtView& view, bool previous) {
  if (!frames_ || !device_ || view.denoise_instances.empty() ||
      view.denoise_instances.size() > car_rt_rules::kMaxInstances) return false;
  // Invalidate previous indices on any failed prepare before publishing this frame.
  previous_.swap(current_);
  current_.clear();
  temporal::Mat4 camera = view.camera_view;
  if (view.camera_flip_z) {
    for (auto& row : camera.m) row[2] = -row[2];
  }
  temporal::Mat4 to_world;
  if (!temporal::Inverse(camera, &to_world)) return false;
  current_.reserve(view.denoise_instances.size());
  for (const auto& item : view.denoise_instances) {
    MotionInstance& instance = current_.emplace_back();
    instance.mesh = item.mesh_key;
    instance.mask = item.mask;
    instance.view = rules::InstanceMatrix(item.transform);
    const double* translation = instance.view.m[3];
    for (int axis = 0; axis < 3; ++axis) {
      instance.origin[axis] = translation[0] * to_world.m[0][axis] + translation[1] * to_world.m[1][axis] +
                              translation[2] * to_world.m[2][axis] + to_world.m[3][axis];
    }
  }
  MotionSlot& slot = motion_[frames_->slot()];
  if (!slot.buffer) {
    slot.buffer = device_->CreateBuffer(plume::RenderBufferDesc::UploadBuffer(kMotionBytes, plume::RenderBufferFlag::STORAGE), "car rt v2 motion rows");
    if (!slot.buffer) return false;
  }
  if (!slot.set) {
    slot.set = device_->CreateDescriptorSet(motion_builder_, 1, 0, "car rt v2 motion set");
    if (!slot.set) return false;
    slot.set->setBuffer(0, slot.buffer.get(), kMotionBytes);
  }

  // Three affine rows followed by the previous instance index plus one.
  // An unmatched or ambiguous part has a zero index and uses spatial filtering.
  using Row = std::array<uint32_t, 16>;
  std::vector<Row> rows(current_.size());
  if (previous) {
    std::unordered_multimap<uint64_t, uint32_t> by_mesh;
    by_mesh.reserve(previous_.size());
    for (uint32_t i = 0; i < previous_.size(); ++i) by_mesh.emplace(previous_[i].mesh, i);
    std::vector<uint32_t> claims(previous_.size());
    for (uint32_t i = 0; i < current_.size(); ++i) {
      const MotionInstance& now = current_[i];
      float best = std::numeric_limits<float>::infinity();
      float next = best;
      uint32_t match = 0;
      const auto [first, end] = by_mesh.equal_range(now.mesh);
      for (auto it = first; it != end; ++it) {
        const MotionInstance& old = previous_[it->second];
        if (old.mask != now.mask) continue;
        const float distance = float(std::hypot(now.origin[0] - old.origin[0], now.origin[1] - old.origin[1], now.origin[2] - old.origin[2]));
        if (distance < best) {
          next = best;
          best = distance;
          match = it->second;
        } else {
          next = std::min(next, distance);
        }
      }
      if (!rules::UniqueMatch(best, next)) continue;
      temporal::Mat4 transform;
      if (!rules::PreviousInstance(now.view, previous_[match].view, transform)) continue;
      for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 4; ++row) rows[i][column * 4 + row] = std::bit_cast<uint32_t>(float(transform.m[row][column]));
      }
      rows[i][12] = match + 1u;
      ++claims[match];
    }
    for (auto& row : rows) {
      if (row[12] && claims[row[12] - 1u] != 1u) row[12] = 0;
    }
  }
  const plume::RenderRange no_read(0, 0);
  void* mapped = slot.buffer->map(0, &no_read);
  if (!mapped) return false;
  std::memcpy(mapped, rows.data(), rows.size() * sizeof(Row));
  slot.buffer->unmap(0, nullptr);
  instance_count_ = uint32_t(rows.size());
  return true;
}

bool CarRtDenoiseV2::EnsureSet(Output& output, const HostImage& image, const HostImage* features) {
  if (!device_ || !image.texture || !features || !features->texture) return false;
  if (output.set && output.image == image.serial && output.features == features->serial) return true;
  RetireSet(output);
  output.set = device_->CreateDescriptorSet(output_builder_, 2, 0, "car rt v2 output set");
  if (!output.set) return false;
  output.set->setTexture(0, image.texture.get(), Layout::GENERAL, image.view.get());
  output.set->setTexture(1, features->texture.get(), Layout::GENERAL, features->view.get());
  output.image = image.serial;
  output.features = features->serial;
  return true;
}

bool CarRtDenoiseV2::EnsureHistory(History& history, HostImage& trace, uint32_t width, uint32_t height) {
  if (!passes_ || !width || !height) return false;
  for (uint32_t i = 0; i < 2; ++i) {
    const uint64_t before = history.features[i].serial;
    if (!passes_->EnsureImage(history.values[i], width, height, kValue, "car rt v2 history", kFlags) ||
        !passes_->EnsureImage(history.features[i], width, height, kFeature, "car rt v2 features", kFlags) ||
        !EnsureSet(history.history_set[i], history.values[i], &history.features[i])) {
      ReleaseHistory(history);
      return false;
    }
    if (history.features[i].serial != before) history.valid = false;
  }
  if (!EnsureSet(history.trace_set, trace, &guide_) || !EnsureSet(history.scratch_set, scratch_, &guide_)) {
    ReleaseHistory(history);
    return false;
  }
  return true;
}

bool CarRtDenoiseV2::PreparePaint(HostImage& trace) {
  return EnsureHistory(paint_, trace, frame_.width, frame_.height);
}

bool CarRtDenoiseV2::PrepareGlass(HostImage& trace, Rect rect) {
  if (rect.Empty()) return false;
  const auto capacity = [](uint32_t size, uint32_t old, uint32_t max) {
    return std::min(max, std::max(old, std::bit_ceil(size)));
  };
  return EnsureHistory(glass_, trace,
                       capacity(uint32_t(rect.Width()), glass_.values[0].width, frame_.width),
                       capacity(uint32_t(rect.Height()), glass_.values[0].height, frame_.height));
}

void CarRtDenoiseV2::RecordPaint(plume::RenderCommandList* list, HostImage& trace, const HostImage& aux,
                                  const car_rt_refl::PaintRects& regions) {
  const uint32_t out = 1u - paint_.index;
  const Rect full{0, 0, int32_t(frame_.width), int32_t(frame_.height)};
  const auto dirty = car_rt_refl::PaintDispatch(paint_.coverage[out], paint_.features[out].serial, regions, full, true);
  if (list && aux.texture) {
    // The caller has already transitioned aux to SHADER_READ.
    Record(list, paint_, trace, aux.slot, full, dirty, false);
  }
}

void CarRtDenoiseV2::RecordGlass(plume::RenderCommandList* list, HostImage& trace, Rect rect) {
  Record(list, glass_, trace, 0, rect, std::span(&rect, 1), true);
}

void CarRtDenoiseV2::Record(plume::RenderCommandList* list, History& history, HostImage& trace, uint32_t aux,
                             Rect rect, std::span<const Rect> regions, bool glass) {
  if (!list || !frames_ || !heap_) return;
  const uint32_t old = history.index;
  const uint32_t out = 1u - old;
  const auto signature = glass ? frame_.glass_signature : frame_.paint_signature;
  const auto previous_signature = glass ? history.frame.glass_signature : history.frame.paint_signature;
  const bool have_last = history.valid && rules::IsConsecutive(history.frame.number, frame_.number, frame_.reset,
                                                               signature == previous_signature && SameGrid(frame_, history.frame));
  auto write_rect = [](int32_t (&dest)[4], Rect r) {
    dest[0] = r.x0;
    dest[1] = r.y0;
    dest[2] = r.x1;
    dest[3] = r.y1;
  };
  Push push{};
  push.slots[0] = trace.slot;
  push.slots[1] = guide_.slot;
  push.slots[2] = have_last ? history.values[old].slot : trace.slot;
  push.slots[3] = have_last ? history.features[old].slot : guide_.slot;
  push.extent[0] = frame_.width;
  push.extent[1] = frame_.height;
  push.extent[2] = frame_.render_width;
  push.extent[3] = frame_.render_height;
  write_rect(push.rect, rect);
  write_rect(push.previous_rect, history.rect);
  push.lens[0] = frame_.tan_x;
  push.lens[1] = frame_.tan_y;
  push.lens[2] = history.frame.tan_x;
  push.lens[3] = history.frame.tan_y;
  push.config[1] = frame_.divisor;
  push.config[2] = (glass ? 1u : 0u) | (have_last ? 2u : 0u);
  push.config[3] = aux;
  push.trace[0] = frame_.cone;
  push.trace[1] = float(instance_count_);

  const auto dispatch = [&](const Output& output) {
    list->setComputeDescriptorSet(output.set.get(), 1);
    for (Rect region : regions) {
      write_rect(push.dispatch_rect, region);
      list->setComputePushConstants(0, &push, 0, sizeof(push));
      list->dispatch(car_rt_opp_refl::GroupsX(region), car_rt_opp_refl::GroupsY(region), 1);
    }
  };
  // CHECK: validate trace -> temporal -> spatial barriers and live switches on both APIs.
  Transition(*list, trace, Layout::SHADER_READ);
  Transition(*list, guide_, Layout::SHADER_READ);
  Transition(*list, history.values[out], Layout::GENERAL);
  Transition(*list, history.features[out], Layout::GENERAL);
  list->setComputePipelineLayout(layout_.get());
  list->setComputeDescriptorSet(heap_->texture_set(), 0);
  list->setComputeDescriptorSet(motion_[frames_->slot()].set.get(), 2);
  list->setPipeline(pipeline_.get());
  dispatch(history.history_set[out]);
  Transition(*list, history.values[out], Layout::SHADER_READ);
  Transition(*list, history.features[out], Layout::SHADER_READ);
  Transition(*list, guide_, Layout::GENERAL);

  const HostImage* input = &history.values[out];
  push.slots[1] = history.features[out].slot;
  push.slots[2] = push.slots[3] = history.features[out].slot;
  const uint32_t iterations = rules::Iterations(glass);
  for (uint32_t i = 0; i < iterations; ++i) {
    const bool writes_trace = rules::WritesTrace(i, iterations);
    HostImage& output = writes_trace ? trace : scratch_;
    Transition(*list, output, Layout::GENERAL);
    push.slots[0] = input->slot;
    push.config[0] = i + 1u;
    dispatch(writes_trace ? history.trace_set : history.scratch_set);
    Transition(*list, output, Layout::SHADER_READ, i + 1u == iterations ? Stage::GRAPHICS_AND_COMPUTE : Stage::COMPUTE);
    input = &output;
  }
  history.frame = frame_;
  history.rect = rect;
  history.index = out;
  history.valid = true;
  if (frame_.report && frame_.number % 60u == 0u) {
    REXLOG_INFO("[car denoise v2] {} frame {}", glass ? "glass" : "paint", frame_.number);
  }
}

void CarRtDenoiseV2::RetireSet(Output& output) {
  if (frames_ && output.set) frames_->Retire(std::move(output.set));
  output = {};
}

void CarRtDenoiseV2::ReleaseHistory(History& history) {
  if (!passes_) return;
  RetireSet(history.trace_set);
  RetireSet(history.scratch_set);
  for (uint32_t i = 0; i < 2; ++i) {
    RetireSet(history.history_set[i]);
    passes_->ReleaseImage(history.values[i]);
    passes_->ReleaseImage(history.features[i]);
  }
  history = {};
}

void CarRtDenoiseV2::Release() {
  if (!passes_ || (!guide_.texture && !scratch_.texture)) return;
  ReleaseHistory(paint_);
  ReleaseHistory(glass_);
  passes_->ReleaseImage(guide_);
  passes_->ReleaseImage(scratch_);
  previous_.clear();
  current_.clear();
  frame_ = {};
}

void CarRtDenoiseV2::Shutdown() {
  paint_ = {};
  glass_ = {};
  guide_ = {};
  scratch_ = {};
  motion_ = {};
  pipeline_.reset();
  shader_.reset();
  layout_.reset();
  previous_.clear();
  current_.clear();
  frame_ = {};
  passes_ = nullptr;
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
}

uint64_t CarRtDenoiseV2::HeldBytes() const {
  uint64_t bytes = 0;
  for (const HostImage* image : {&guide_, &scratch_, &paint_.values[0], &paint_.values[1], &paint_.features[0], &paint_.features[1],
                                 &glass_.values[0], &glass_.values[1], &glass_.features[0], &glass_.features[1]}) {
    if (image->texture) bytes += TextureBytes(image->width, image->height, 1, image->format);
  }
  for (const auto& slot : motion_) {
    if (slot.buffer) bytes += kMotionBytes;
  }
  return bytes;
}

}  // namespace mcla::gpu
