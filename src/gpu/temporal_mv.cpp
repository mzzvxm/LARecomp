// MCLA GPU renderer -- the temporal system's passes. See temporal_mv.h.

#include "temporal_mv.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <string>

#include <rex/cvar.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>

#include "descriptors.h"
#include "device.h"
#include "format.h"
#include "host_shaders.h"
#include "pass_timing.h"
#include "texture_bytes.h"
#include "textures.h"

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[gpu] temporal mv:";
constexpr plume::RenderFormat kDepthFormat = plume::RenderFormat::R32_FLOAT;
constexpr plume::RenderFormat kMotionFormat = plume::RenderFormat::R16G16_FLOAT;

// A Mat4 as 16 floats, row-major.
void ToFloats(const temporal::Mat4& m, float out[16]) {
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      out[r * 4 + c] = float(m.m[r][c]);
    }
  }
}

// Columns x, y and w of a row-major 4x4 (row vectors: prev.x = dot(clip, column x)).
template <typename Push>
void SetColumns(Push& push, const float a[16]) {
  for (int row = 0; row < 4; ++row) {
    push.col_x[row] = a[row * 4 + 0];
    push.col_y[row] = a[row * 4 + 1];
    push.col_w[row] = a[row * 4 + 3];
  }
}

}  // namespace

TemporalMotion::~TemporalMotion() {
  Shutdown();
}

void TemporalMotion::Initialize(Device& device, BindlessHeap& heap, HostPasses& passes,
                                PassTiming& pass_timing) {
  device_ = &device;
  heap_ = &heap;
  passes_ = &passes;
  pass_timing_ = &pass_timing;
  failed_ = false;
  initialized_ = true;
}

void TemporalMotion::Shutdown() {
  // The GPU is idle and the heap goes later: the slots are not handed back (PostAa::Shutdown).
  for (HostImage& image : depth_) {
    image = HostImage{};
  }
  motion_ = HostImage{};
  depth_pipeline_.reset();
  motion_pipeline_.reset();
  depth_shader_.reset();
  motion_shader_.reset();
  motion_layout_.reset();
  recorded_ = false;
  device_ = nullptr;
  heap_ = nullptr;
  passes_ = nullptr;
  pass_timing_ = nullptr;
  initialized_ = false;
}

void TemporalMotion::Release() {
  if (!initialized_ || !passes_) {
    return;
  }
  for (HostImage& image : depth_) {
    passes_->ReleaseImage(image);
  }
  passes_->ReleaseImage(motion_);
  recorded_ = false;
}

uint64_t TemporalMotion::HeldBytes() const {
  uint64_t bytes = 0;
  const auto add = [&bytes](const HostImage& image) {
    if (image.texture) {
      bytes += TextureBytes(image.width, image.height, 1, image.format);
    }
  };
  for (const HostImage& image : depth_) {
    add(image);
  }
  add(motion_);
  return bytes;
}

bool TemporalMotion::Prepare() {
  if (motion_pipeline_) {
    return true;
  }
  if (failed_ || !initialized_) {
    return false;
  }
  // The host pass layout (host_passes.h) with 80 bytes of push constants instead of 32: the four
  // texture sets, the sampler set, the push range at (b0, space4), for both stages as there.
  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  for (uint32_t set = 0; set < 4; ++set) {
    builder.addDescriptorSet(heap_->texture_set_builder());
  }
  builder.addDescriptorSet(heap_->sampler_set_builder());
  builder.addPushConstant(0, 4, kMotionPushBytes,
                          plume::RenderShaderStageFlag::VERTEX | plume::RenderShaderStageFlag::PIXEL);
  builder.end();
  motion_layout_ = device_->CreatePipelineLayout(builder, "temporal motion layout");

  const HostShaderBlob motion = TemporalMotionPixelShader();
  const HostShaderBlob depth = TemporalCopyPixelShader();
  motion_shader_ = passes_->CreateShader(motion.data, motion.size, motion.name);
  depth_shader_ = passes_->CreateShader(depth.data, depth.size, depth.name);
  bool ok = motion_layout_ && motion_shader_ && depth_shader_;
  if (ok) {
    motion_pipeline_ = passes_->CreatePipeline(*motion_layout_, *motion_shader_, kMotionFormat,
                                               "temporal motion vectors");
    depth_pipeline_ = passes_->CreatePipeline(*depth_shader_, kDepthFormat, "temporal depth copy");
    ok = motion_pipeline_ && depth_pipeline_;
  }
  if (!ok) {
    failed_ = true;
    motion_pipeline_.reset();
    REXLOG_ERROR("{} the passes could not be made (layout, shaders or pipelines): no motion "
                 "vectors this session",
                 kPrefix);
    return false;
  }
  REXLOG_INFO("{} passes ready: depth {}, vectors {} (render size)", kPrefix, "R32_FLOAT",
              "R16G16_FLOAT");
  return true;
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

bool TemporalMotion::Record(plume::RenderCommandList* list, const Frame& f) {
  if (!list || !f.camera || !available() || !Prepare()) {
    return false;
  }
  const uint32_t rw = f.render_size[0];
  const uint32_t rh = f.render_size[1];
  if (rw == 0 || rh == 0 || f.guest_size[0] == 0 || f.guest_size[1] == 0) {
    return false;
  }
  // This frame's images are the other half of the pair; the previous frame's stay where they are.
  cur_ ^= 1u;
  HostImage& depth = depth_[cur_];
  const bool has_previous = f.previous != nullptr;
  recorded_ = false;
  if (!passes_->EnsureImage(depth, rw, rh, kDepthFormat, "temporal depth") ||
      !passes_->EnsureImage(motion_, rw, rh, kMotionFormat, "temporal motion vectors")) {
    return false;
  }

  if (rw != logged_size_[0] || rh != logged_size_[1]) {
    logged_size_[0] = rw;
    logged_size_[1] = rh;
    REXLOG_INFO("{} images at {}x{} (render size, guest {}x{}): depth R32_FLOAT (1 near, 0 far, "
                "kept), vectors R16G16_FLOAT (render pixels, previous minus current, jitter "
                "removed)",
                kPrefix, rw, rh, f.guest_size[0], f.guest_size[1]);
  }
  // CHECK: the depth product the anchor binds is the render size (the guest size at the scene's
  // scale), not a copy reduced to another scale (census CHECK 2: the host depth is 1 near, 0 far).
  CopyPush copy{};
  pass_timing_->Mark(list, "temporal depth copy");
  copy.source = f.depth_slot;
  passes_->DrawInto(list, depth, depth_pipeline_.get(), &copy, sizeof(copy));

  pass_timing_->Mark(list, "temporal motion vectors");
  RecordMotion(list, f, has_previous);

  recorded_ = true;
  recorded_frame_ = f.frame;
  motion_zero_ = !has_previous;
  return true;
}

void TemporalMotion::BindMotion(plume::RenderCommandList* list) const {
  list->setGraphicsPipelineLayout(motion_layout_.get());
  for (uint32_t set = 0; set < 4; ++set) {
    list->setGraphicsDescriptorSet(heap_->texture_set(), set);
  }
  list->setGraphicsDescriptorSet(heap_->sampler_set(), 4);
}

void TemporalMotion::RecordMotion(plume::RenderCommandList* list, const Frame& f,
                                  bool has_previous) {
  MotionPush push{};
  push.depth = f.depth_slot;
  push.jitter[0] = f.jitter[0];
  push.jitter[1] = f.jitter[1];
  push.size[0] = float(f.render_size[0]);
  push.size[1] = float(f.render_size[1]);
  push.to_render[0] = float(double(f.render_size[0]) / double(f.guest_size[0]));
  push.to_render[1] = float(double(f.render_size[1]) / double(f.guest_size[1]));
  if (has_previous) {
    float m[16];
    ToFloats(temporal::CameraToPreviousPixels(*f.camera, *f.previous), m);
    SetColumns(push, m);
  } else {
    push.mode = 2;  // zero: the history starts again
  }

  // The same sequence as HostPasses::DrawInto, with this pass's own layout.
  // CHECK: 80 bytes of push constants for the vertex and the pixel stage on both backends (the
  // resolve memory's fused pass already uses 72) and the host layout's four texture sets and
  // sampler set bound again by BindMotion; the D3D12 debug layer and Vulkan validation over this
  // pass settle it.
  HostImage& image = motion_;
  const bool first_use = image.layout == plume::RenderTextureLayout::UNKNOWN;
  if (image.layout != plume::RenderTextureLayout::COLOR_WRITE) {
    list->barriers(plume::RenderBarrierStage::GRAPHICS,
                   plume::RenderTextureBarrier(image.texture.get(),
                                               plume::RenderTextureLayout::COLOR_WRITE));
    image.layout = plume::RenderTextureLayout::COLOR_WRITE;
  }
  list->setFramebuffer(image.framebuffer.get());
  if (first_use) {
    list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
  }
  const plume::RenderViewport viewport{0.0f, 0.0f, float(image.width), float(image.height)};
  list->setViewports(&viewport, 1);
  const plume::RenderRect scissor(0, 0, int32_t(image.width), int32_t(image.height));
  list->setScissors(&scissor, 1);
  BindMotion(list);
  list->setPipeline(motion_pipeline_.get());
  list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  list->drawInstanced(3, 1, 0, 0);
  list->setFramebuffer(nullptr);
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(image.texture.get(),
                                             plume::RenderTextureLayout::SHADER_READ));
  image.layout = plume::RenderTextureLayout::SHADER_READ;
}

TemporalMotion::Kept TemporalMotion::kept() const {
  Kept k;
  if (!recorded_) {
    return k;
  }
  k.valid = true;
  k.frame = recorded_frame_;
  k.depth = &depth_[cur_];
  k.motion = &motion_;
  k.motion_is_zero = motion_zero_;
  return k;
}

}  // namespace mcla::gpu
