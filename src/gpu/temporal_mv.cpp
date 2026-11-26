// MCLA GPU renderer -- the temporal system's passes. See temporal_mv.h.

#include "temporal_mv.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <string>

#include "settings/larecomp_settings.h"
#include <rex/xenos/fetch.h>
#include <rex/xenos/format.h>
#include <rex/logging/log_category.h>

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
// The vehicle table (vehicle_motion.h): a row a stencil ID, four texels a row (the three columns and
// the usable flag, hlsl/temporal_vehicle_table_ps.hlsl).
constexpr plume::RenderFormat kTableFormat = plume::RenderFormat::R32G32B32A32_FLOAT;
constexpr uint32_t kTableWidth = 4;
constexpr uint32_t kTableRows = temporal::kVehicleIds;

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

uint32_t TemporalMotion::StencilChannel(const uint32_t fetch[6], uint32_t* host_swizzle) {
  if (host_swizzle) {
    *host_swizzle = 0;
  }
  const TextureFetch constant = ReadTextureFetch(fetch);
  if (!constant.is_texture) {
    return 4;
  }
  // The very swizzle the texture cache gives the view the anchor binds (textures.cpp Bind2D).
  const uint16_t host = GuestToHostSwizzle(
      constant.swizzle,
      format::TextureHost(uint32_t(rex::xenos::TextureFormat::k_8_8_8_8)).host_swizzle);
  if (host_swizzle) {
    *host_swizzle = host;
  }
  // The depth resolve into k_8_8_8_8 puts the stencil in the product's red (resolve_ps.hlsl).
  for (uint32_t component = 0; component < 4; ++component) {
    if (((host >> (3 * component)) & 7u) == 0u) {
      return component;
    }
  }
  return 4;
}

TemporalMotion::~TemporalMotion() {
  Shutdown();
}

void TemporalMotion::Initialize(HostPasses& passes, PassTiming& pass_timing) {
  passes_ = &passes;
  pass_timing_ = &pass_timing;
  masks_.Initialize(passes, pass_timing);
  failed_ = false;
  initialized_ = true;
}

void TemporalMotion::Shutdown() {
  // The GPU is idle and the heap goes later: the slots are not handed back (PostAa::Shutdown).
  for (HostMrtPair& targets : fused_targets_) {
    targets = {};
  }
  for (HostImage& image : depth_) {
    image = HostImage{};
  }
  motion_ = HostImage{};
  table_ = HostImage{};
  masks_.Shutdown();
  mask_recorded_ = false;
  table_pipeline_.reset();
  depth_pipeline_.reset();
  motion_pipeline_.reset();
  fused_pipeline_.reset();
  table_shader_.reset();
  depth_shader_.reset();
  motion_shader_.reset();
  fused_shader_.reset();
  motion_layout_.reset();
  fused_pipeline_failed_ = false;
  recorded_ = false;
  passes_ = nullptr;
  pass_timing_ = nullptr;
  initialized_ = false;
}

void TemporalMotion::Release() {
  if (!initialized_ || !passes_) {
    return;
  }
  for (HostMrtPair& targets : fused_targets_) {
    targets.Release(*passes_);
  }
  for (HostImage& image : depth_) {
    passes_->ReleaseImage(image);
  }
  passes_->ReleaseImage(motion_);
  passes_->ReleaseImage(table_);
  masks_.Release();
  mask_recorded_ = false;
  recorded_ = false;
}

uint64_t TemporalMotion::HeldBytes() const {
  uint64_t bytes = masks_.HeldBytes();
  const auto add = [&bytes](const HostImage& image) {
    if (image.texture) {
      bytes += TextureBytes(image.width, image.height, 1, image.format);
    }
  };
  for (const HostImage& image : depth_) {
    add(image);
  }
  add(motion_);
  add(table_);
  return bytes;
}

bool TemporalMotion::Prepare() {
  if (motion_layout_) {
    return true;
  }
  if (failed_ || !initialized_ || !passes_) {
    return false;
  }
  motion_layout_ = passes_->CreateLayout(kMotionPushBytes, "temporal motion layout");

  if (!motion_layout_) {
    failed_ = true;
    REXLOG_ERROR("{} the motion layout could not be made", kPrefix);
    return false;
  }
  const HostShaderBlob table = TemporalVehicleTablePixelShader();
  table_shader_ = passes_->CreateShader(table.data, table.size, table.name);
  if (table_shader_) {
    table_pipeline_ = passes_->CreatePipeline(*table_shader_, kTableFormat, "temporal vehicle table");
  }
  if (!table_pipeline_) {
    REXLOG_ERROR("{} the vehicle table's pass could not be made: using camera vectors", kPrefix);
  }
  REXLOG_INFO("{} passes ready: depth {}, vectors {} (render size), vehicle table {}", kPrefix,
              "R32_FLOAT", "R16G16_FLOAT", table_pipeline_ ? "yes" : "NO");
  return true;
}

bool TemporalMotion::PrepareSeparate() {
  if (motion_pipeline_ && depth_pipeline_) {
    return true;
  }
  const HostShaderBlob motion = TemporalMotionPixelShader();
  const HostShaderBlob depth = TemporalCopyPixelShader();
  motion_shader_ = passes_->CreateShader(motion.data, motion.size, motion.name);
  depth_shader_ = passes_->CreateShader(depth.data, depth.size, depth.name);
  if (motion_shader_ && depth_shader_) {
    motion_pipeline_ = passes_->CreatePipeline(*motion_layout_, *motion_shader_, kMotionFormat,
                                               "temporal motion vectors");
    depth_pipeline_ = passes_->CreatePipeline(*depth_shader_, kDepthFormat, "temporal depth copy");
  }
  if (!motion_pipeline_ || !depth_pipeline_) {
    failed_ = true;
    REXLOG_ERROR("{} the separate depth and motion passes could not be made", kPrefix);
    return false;
  }
  return true;
}

bool TemporalMotion::PrepareFused() {
  if (fused_pipeline_ || fused_pipeline_failed_) {
    return bool(fused_pipeline_);
  }
  const HostShaderBlob blob = TemporalMotionDepthPixelShader();
  fused_shader_ = passes_->CreateShader(blob.data, blob.size, blob.name);
  if (fused_shader_) {
    const plume::RenderFormat formats[] = {kMotionFormat, kDepthFormat};
    fused_pipeline_ = passes_->CreateMrtPipeline(*motion_layout_, *fused_shader_, formats, 2,
                                                "temporal depth + motion vectors");
  }
  fused_pipeline_failed_ = !fused_pipeline_;
  if (fused_pipeline_failed_) {
    REXLOG_WARN("{} using separate depth and motion passes after MRT pipeline failure", kPrefix);
  }
  return bool(fused_pipeline_);
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

bool TemporalMotion::Record(plume::RenderCommandList* list, const Frame& f, bool upscaling, bool lean,
                            bool verify) {
  if (!list || !f.camera || !pass_timing_ || !available() || !Prepare()) {
    return false;
  }
  const uint32_t rw = f.render_size[0];
  const uint32_t rh = f.render_size[1];
  if (rw == 0 || rh == 0 || f.guest_size[0] == 0 || f.guest_size[1] == 0) {
    return false;
  }
  // This frame's images are the other half of the pair; the previous frame's stay where they are.
  const uint32_t next = cur_ ^ 1u;
  HostImage& depth = depth_[next];
  const bool has_previous = f.previous != nullptr;
  recorded_ = false;
  if (motion_.width != rw || motion_.height != rh) {
    for (HostMrtPair& targets : fused_targets_) {
      targets.Release(*passes_);
    }
  } else if (depth.width != rw || depth.height != rh) {
    fused_targets_[next].Release(*passes_);
  }
  if (!passes_->EnsureImage(depth, rw, rh, kDepthFormat, "temporal depth") ||
      !passes_->EnsureImage(motion_, rw, rh, kMotionFormat, "temporal motion vectors")) {
    return false;
  }
  // CHECK: compare both attachments at the anchor on D3D12 and Vulkan, including a reset frame.
  const bool fused = lean && PrepareFused() &&
                     fused_targets_[next].Ensure(*passes_, motion_, depth,
                                                 "temporal depth + motion framebuffer");
  const bool separate = !fused || verify;
  if (separate && !PrepareSeparate()) {
    return false;
  }
  cur_ = next;

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
  if (separate) {
    const CopyPush copy{f.depth_slot};
    pass_timing_->Mark(list, "temporal depth copy");
    passes_->DrawInto(list, depth, depth_pipeline_.get(), &copy, sizeof(copy));
  }

  // The stencil view the vehicles' IDs are read from: the component that holds the stencil byte comes
  // from the fetch constant, not from a pinned swizzle.
  uint32_t host_swizzle = 0;
  const uint32_t channel = StencilChannel(f.stencil_fetch, &host_swizzle);
  const uint32_t stencil = StencilRef(f.stencil_slot, channel);
  if (host_swizzle != logged_swizzle_) {
    logged_swizzle_ = host_swizzle;
    if (channel > 3) {
      REXLOG_WARN("{} the stencil view's host swizzle is {:#x}: no component reads the product's red, "
                  "so there are no vehicle IDs and no vehicle vectors (the census of the real game "
                  "found 0x60a, the stencil in component 2)",
                  kPrefix, host_swizzle);
    } else {
      REXLOG_INFO("{} the stencil view's host swizzle is {:#x}: the stencil byte is component {} (the "
                  "census of the real game found 0x60a, component 2)",
                  kPrefix, host_swizzle, channel);
    }
  }
  uint32_t table_slot = kNoTable;
  if (has_previous) {
    const temporal::VehicleStats& s = f.vehicle_stats;
    ++counts_.frames;
    if (s.asked != 0) {
      ++counts_.with_ids;
      counts_.ids += s.asked;
      counts_.taken += s.taken;
      counts_.not_sane += s.not_sane;
      counts_.not_rigid += s.not_rigid;
      counts_.not_finite += s.not_finite;
      counts_.max_ids = std::max(counts_.max_ids, s.asked);
    }
    counts_.no_world += s.no_world;
    if (channel <= 3 && RecordVehicleTable(list, f)) {
      table_slot = table_.slot;
      ++counts_.tables;
    }
  }

  RecordMotion(list, f, has_previous, stencil, table_slot, fused, separate);

  // The masks of the draws the Scene drew again (they need no previous frame).
  mask_recorded_ = upscaling && masks_.Resolve(list, rw, rh, lean, verify);

  recorded_ = true;
  recorded_frame_ = f.frame;
  motion_zero_ = !has_previous;
  return true;
}

bool TemporalMotion::RecordVehicleTable(plume::RenderCommandList* list, const Frame& f) {
  if (f.vehicles.valid == 0 || !table_pipeline_ ||
      !passes_->EnsureImage(table_, kTableWidth, kTableRows, kTableFormat,
                            "temporal vehicle table")) {
    return false;
  }
  // One draw a vehicle over the four texels of its row (hlsl/temporal_vehicle_table_ps.hlsl); the
  // image is cleared first, so the row of an ID nobody drew reads as not usable.
  pass_timing_->Mark(list, "temporal vehicle table");
  HostImage& image = table_;
  if (image.layout != plume::RenderTextureLayout::COLOR_WRITE) {
    list->barriers(plume::RenderBarrierStage::GRAPHICS,
                   plume::RenderTextureBarrier(image.texture.get(),
                                               plume::RenderTextureLayout::COLOR_WRITE));
    image.layout = plume::RenderTextureLayout::COLOR_WRITE;
  }
  list->setFramebuffer(image.framebuffer.get());
  list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
  passes_->Bind(list);
  list->setPipeline(table_pipeline_.get());
  for (uint32_t id = 1; id < kTableRows; ++id) {
    if (!(f.vehicles.valid & (1u << id))) {
      continue;
    }
    static_assert(sizeof(TablePush) == sizeof(f.vehicles.columns[0]));
    TablePush push{};
    std::memcpy(&push, f.vehicles.columns[id], sizeof(push));
    const plume::RenderViewport viewport{0.0f, float(id), float(kTableWidth), 1.0f};
    list->setViewports(&viewport, 1);
    const plume::RenderRect scissor(0, int32_t(id), int32_t(kTableWidth), int32_t(id + 1));
    list->setScissors(&scissor, 1);
    list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
    list->drawInstanced(3, 1, 0, 0);
  }
  list->setFramebuffer(nullptr);
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(image.texture.get(),
                                             plume::RenderTextureLayout::SHADER_READ));
  image.layout = plume::RenderTextureLayout::SHADER_READ;
  return true;
}

void TemporalMotion::RecordMotion(plume::RenderCommandList* list, const Frame& f,
                                  bool has_previous, uint32_t stencil, uint32_t table, bool fused,
                                  bool separate) {
  MotionPush push{};
  push.depth = f.depth_slot;
  push.stencil = stencil;
  push.table = table;
  push.guard = kGuardShare * float(std::min(f.render_size[0], f.render_size[1]));
  push.jitter[0] = f.jitter[0];
  push.jitter[1] = f.jitter[1];
  push.size[0] = float(f.render_size[0]);
  push.size[1] = float(f.render_size[1]);
  push.to_render[0] = float(double(f.render_size[0]) / double(f.guest_size[0]));
  push.to_render[1] = float(double(f.render_size[1]) / double(f.guest_size[1]));
  if (has_previous && f.previous && f.camera) {
    float m[16];
    ToFloats(temporal::CameraToPreviousPixels(*f.camera, *f.previous), m);
    SetColumns(push, m);
  } else {
    push.mode = 2;  // zero: the history starts again
  }

  if (separate) {
    pass_timing_->Mark(list, "temporal motion vectors");
    passes_->DrawInto(list, motion_, motion_pipeline_.get(), &push, sizeof(push), false,
                      motion_layout_.get());
  }
  if (fused) {
    pass_timing_->Mark(list, "temporal depth + motion vectors");
    passes_->DrawMrtPair(*list, *fused_targets_[cur_].framebuffer, motion_, depth_[cur_],
                        *fused_pipeline_, &push, sizeof(push), motion_layout_.get());
  }
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
  k.mask = mask_recorded_ ? masks_.mask(UpscalerMasks::Layer::kReactive) : nullptr;
  k.transparency = mask_recorded_ ? masks_.mask(UpscalerMasks::Layer::kTransparency) : nullptr;
  return k;
}

std::string TemporalMotion::Summary() const {
  std::string out;
  const VehicleCounts& c = counts_;
  if (c.with_ids != 0) {
    out = std::format("vehicle vectors: {} of {} frames had vehicle IDs (up to {} a frame): {} IDs "
                      "asked, {} taken, left out: not sane {}, not rigid {}, not finite {}; frames "
                      "whose c69 was not usable {}; tables drawn in {} frames",
                      c.with_ids, c.frames, c.max_ids, c.ids, c.taken, c.not_sane, c.not_rigid,
                      c.not_finite, c.no_world, c.tables);
  }
  const std::string mask = masks_.Summary();
  if (!mask.empty()) {
    out += (out.empty() ? "" : " | ") + mask;
  }
  return out;
}

}  // namespace mcla::gpu
