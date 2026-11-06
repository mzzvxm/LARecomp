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
#include "textures.h"

REXCVAR_DEFINE_STRING(
    mcla_gpu_temporal_view, "off", "MCLA/GPU",
    "Development view of what the temporal groundwork keeps (needs mcla_gpu_temporal), drawn over "
    "the whole picture: off, mv (motion vectors: mid grey is no motion), depth (as stored: near is "
    "bright) or stencil (the vehicle ID of the game's draws: black for the world, a colour for 1 to "
    "23, white for 24 and above). Live.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(
    mcla_gpu_temporal_view_range, 4.0, "MCLA/GPU",
    "With mcla_gpu_temporal_view = mv: the vector length in render pixels that is full colour. "
    "Smaller shows smaller errors. Live.")
    .range(0.01, 256.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(
    mcla_gpu_temporal_vehicles, false, "MCLA/GPU",
    "Debug: the motion vectors of the pixels that carry a vehicle ID use that vehicle's own matrix "
    "from the game's constants (their meaning is not proven yet); off, every pixel moves with the "
    "camera. Needs mcla_gpu_temporal. Live.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_UINT32(
    mcla_gpu_temporal_check, 0, "MCLA/GPU",
    "Development check of the motion vectors (needs mcla_gpu_temporal): every N-th frame, 16 x 9 "
    "sample pixels are read back and the vectors are compared with the world point seen there "
    "projected by the previous frame's view-projection, and the previous frame's depth and colour "
    "are read where the vectors point; one '[gpu] temporal mv: check' line each, the first 40. 0 "
    "is off; the colour history costs a full-screen copy a frame around each check. Live.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[gpu] temporal mv:";
constexpr plume::RenderFormat kDepthFormat = plume::RenderFormat::R32_FLOAT;
constexpr plume::RenderFormat kMotionFormat = plume::RenderFormat::R16G16_FLOAT;
constexpr plume::RenderFormat kColorFormat = plume::RenderFormat::R16G16B16A16_FLOAT;
constexpr plume::RenderFormat kIdsFormat = plume::RenderFormat::R8_UNORM;
constexpr plume::RenderFormat kCheckFormat = plume::RenderFormat::R32G32B32A32_FLOAT;

const char* ViewName(TemporalMotion::View view) {
  switch (view) {
    case TemporalMotion::View::kOff: return "off";
    case TemporalMotion::View::kMotion: return "motion vectors";
    case TemporalMotion::View::kDepth: return "depth";
    case TemporalMotion::View::kStencil: return "vehicle IDs (stencil)";
  }
  return "?";
}

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

double Mean(double sum, uint32_t n) {
  return n != 0 ? sum / double(n) : 0.0;
}

double Percent(uint32_t n, uint32_t of) {
  return of != 0 ? 100.0 * double(n) / double(of) : 0.0;
}

// The view distance a stored depth (1 near, 0 far) stands for under a lens: z_ndc = 1 - depth,
// z = near far / (far - z_ndc (far - near)). Zero when the value is outside the frustum.
double ViewDistance(double stored, double near_plane, double far_plane) {
  const double z_ndc = 1.0 - stored;
  const double denominator = far_plane - z_ndc * (far_plane - near_plane);
  if (!(z_ndc >= 0.0 && z_ndc <= 1.0) || !(denominator > 0.0)) {
    return 0.0;
  }
  return near_plane * far_plane / denominator;
}

}  // namespace

uint32_t TemporalMotion::StencilChannel(const uint32_t fetch[6], uint32_t* host_swizzle) {
  rex::graphics::xenos::xe_gpu_texture_fetch_t constant;
  static_assert(sizeof(constant) == 6 * sizeof(uint32_t));
  std::memcpy(&constant, fetch, sizeof(constant));
  if (host_swizzle) {
    *host_swizzle = 0;
  }
  if (constant.type != rex::graphics::xenos::FetchConstantType::kTexture) {
    return 4;
  }
  // The very swizzle the texture cache gives the view the anchor binds (textures.cpp Bind2D).
  const uint16_t host = GuestToHostSwizzle(
      uint16_t(constant.swizzle),
      format::TextureHost(uint32_t(rex::graphics::xenos::TextureFormat::k_8_8_8_8)).host_swizzle);
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

TemporalMotion::View TemporalMotion::ReadView() {
  std::string value = REXCVAR_GET(mcla_gpu_temporal_view);
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  if (value == "mv" || value == "motion") {
    return View::kMotion;
  }
  if (value == "depth") {
    return View::kDepth;
  }
  if (value == "stencil" || value == "ids") {
    return View::kStencil;
  }
  return View::kOff;
}

TemporalMotion::~TemporalMotion() {
  Shutdown();
}

void TemporalMotion::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap,
                                HostPasses& passes, PassTiming& pass_timing,
                                plume::RenderFormat target_format) {
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  passes_ = &passes;
  pass_timing_ = &pass_timing;
  target_format_ = target_format;
  failed_ = false;
  initialized_ = true;
}

void TemporalMotion::Shutdown() {
  ++generation_;
  // The GPU is idle and the heap goes later: the slots are not handed back (PostAa::Shutdown).
  for (HostImage& image : depth_) {
    image = HostImage{};
  }
  for (HostImage& image : color_) {
    image = HostImage{};
  }
  motion_ = HostImage{};
  ids_ = HostImage{};
  check_ = HostImage{};
  for (Readback& r : readbacks_) {
    r.buffer.reset();
    r.busy = false;
  }
  debug_pipeline_.reset();
  debug_shader_.reset();
  check_pipeline_.reset();
  ids_pipeline_.reset();
  color_pipeline_.reset();
  depth_pipeline_.reset();
  motion_pipeline_.reset();
  check_shader_.reset();
  ids_shader_.reset();
  color_shader_.reset();
  depth_shader_.reset();
  motion_shader_.reset();
  motion_layout_.reset();
  recorded_ = false;
  ids_recorded_ = false;
  color_recorded_[0] = color_recorded_[1] = false;
  device_ = nullptr;
  frames_ = nullptr;
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
  for (HostImage& image : color_) {
    passes_->ReleaseImage(image);
  }
  passes_->ReleaseImage(motion_);
  passes_->ReleaseImage(ids_);
  passes_->ReleaseImage(check_);
  recorded_ = false;
  ids_recorded_ = false;
  color_recorded_[0] = color_recorded_[1] = false;
  // Readbacks still in flight judge a copy of what they need and then free themselves.
}

void TemporalMotion::ReadSettings() {
  view_ = ReadView();
  vehicles_ = REXCVAR_GET(mcla_gpu_temporal_vehicles);
  check_every_ = REXCVAR_GET(mcla_gpu_temporal_check);
  if (view_ != reported_view_ || vehicles_ != reported_vehicles_ ||
      check_every_ != reported_check_) {
    reported_view_ = view_;
    reported_vehicles_ = vehicles_;
    reported_check_ = check_every_;
    REXLOG_INFO("{} view: {}; vehicle matrices {}; check {}", kPrefix, ViewName(view_),
                vehicles_ ? "used for the pixels of their IDs (debug)" : "not used",
                check_every_ != 0 ? std::format("every {} frames", check_every_)
                                  : std::string("off"));
  }
}

bool TemporalMotion::Prepare() {
  if (motion_pipeline_) {
    return true;
  }
  if (failed_ || !initialized_) {
    return false;
  }
  // The host pass layout (host_passes.h) with 96 bytes of push constants instead of 32: the four
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
  const HostShaderBlob depth = TemporalCopyPixelShader(0);
  const HostShaderBlob color = TemporalCopyPixelShader(1);
  const HostShaderBlob ids = TemporalCopyPixelShader(2);
  const HostShaderBlob check = TemporalCheckPixelShader();
  motion_shader_ = passes_->CreateShader(motion.data, motion.size, motion.name);
  depth_shader_ = passes_->CreateShader(depth.data, depth.size, depth.name);
  color_shader_ = passes_->CreateShader(color.data, color.size, color.name);
  ids_shader_ = passes_->CreateShader(ids.data, ids.size, ids.name);
  check_shader_ = passes_->CreateShader(check.data, check.size, check.name);
  bool ok = motion_layout_ && motion_shader_ && depth_shader_ && color_shader_ && ids_shader_ &&
            check_shader_;
  if (ok) {
    motion_pipeline_ = passes_->CreatePipeline(*motion_layout_, *motion_shader_, kMotionFormat,
                                               "temporal motion vectors");
    depth_pipeline_ = passes_->CreatePipeline(*depth_shader_, kDepthFormat, "temporal depth copy");
    color_pipeline_ =
        passes_->CreatePipeline(*color_shader_, kColorFormat, "temporal colour copy");
    ids_pipeline_ = passes_->CreatePipeline(*ids_shader_, kIdsFormat, "temporal vehicle IDs");
    check_pipeline_ =
        passes_->CreatePipeline(*check_shader_, kCheckFormat, "temporal check samples");
    ok = motion_pipeline_ && depth_pipeline_ && color_pipeline_ && ids_pipeline_ &&
         check_pipeline_;
  }
  if (!ok) {
    failed_ = true;
    motion_pipeline_.reset();
    REXLOG_ERROR("{} the passes could not be made (layout, shaders or pipelines): no motion "
                 "vectors this session",
                 kPrefix);
    return false;
  }
  REXLOG_INFO("{} passes ready: depth {}, vectors {}, IDs {}, colour history {} (render size)", kPrefix,
              "R32_FLOAT", "R16G16_FLOAT", "R8_UNORM", "R16G16B16A16_FLOAT");
  return true;
}

bool TemporalMotion::PrepareDebug() {
  if (debug_pipeline_) {
    return true;
  }
  if (debug_failed_ || !initialized_) {
    return false;
  }
  const HostShaderBlob blob = TemporalDebugPixelShader();
  debug_shader_ = passes_->CreateShader(blob.data, blob.size, blob.name);
  if (debug_shader_) {
    debug_pipeline_ = passes_->CreatePipeline(*debug_shader_, target_format_, "temporal debug view");
  }
  if (!debug_pipeline_) {
    debug_failed_ = true;
    debug_shader_.reset();
    REXLOG_ERROR("{} the debug view pass could not be made", kPrefix);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

bool TemporalMotion::Record(plume::RenderCommandList* list, const Frame& f) {
  ReadSettings();
  if (!list || !f.camera || !available() || !Prepare()) {
    return false;
  }
  const uint32_t rw = f.render_size[0];
  const uint32_t rh = f.render_size[1];
  if (rw == 0 || rh == 0 || f.guest_size[0] == 0 || f.guest_size[1] == 0) {
    return false;
  }
  // This frame's images are the other half of the pair; the previous frame's stay where they are.
  const uint32_t before = cur_;
  cur_ ^= 1u;
  HostImage& depth = depth_[cur_];
  const bool has_previous = f.previous != nullptr;
  const bool history = has_previous && recorded_ && recorded_frame_ + 1 == f.frame &&
                       depth_[before].texture && depth_[before].width == rw &&
                       depth_[before].height == rh;
  recorded_ = false;
  ids_recorded_ = false;
  if (!passes_->EnsureImage(depth, rw, rh, kDepthFormat, "temporal depth") ||
      !passes_->EnsureImage(motion_, rw, rh, kMotionFormat, "temporal motion vectors")) {
    ++counters_.failed;
    return false;
  }

  if (rw != logged_size_[0] || rh != logged_size_[1]) {
    logged_size_[0] = rw;
    logged_size_[1] = rh;
    REXLOG_INFO("{} images at {}x{} (render size, guest {}x{}): depth R32_FLOAT (1 near, 0 far, "
                "kept), vectors R16G16_FLOAT (render pixels, previous minus current, jitter "
                "removed){}",
                kPrefix, rw, rh, f.guest_size[0], f.guest_size[1],
                check_every_ != 0 ? ", colour history R16G16B16A16_FLOAT around the checks" : "");
  }
  // CHECK: the depth and stencil products the anchor binds are the render size (the guest size at
  // the scene's scale), not a copy reduced to another scale; the depth view (mcla_gpu_temporal_view
  // = depth) shows it, and shows the host value's direction (census CHECK 2).
  // The stencil byte is where the view's swizzle puts the product's red, not `.x`: the product of
  // the depth resolve into k_8_8_8_8 holds the raw dword depth24 << 8 | stencil as RGBA8 (R the
  // stencil), and the anchor's view of it is swizzled ZYXW, so `.x` is the depth's middle byte
  // (which is what made the first stencil view stripes over the whole world). The vehicle ID is
  // the byte >> 2.
  // CHECK: the swizzle the log names below (ZYXW, the fork's finding, component 2); the stencil
  // view (IDs in colour: black world, coloured cars) settles it.
  uint32_t host_swizzle = 0;
  const uint32_t stencil_channel = StencilChannel(f.stencil_fetch, &host_swizzle);
  stencil_ref_ = StencilRef(f.stencil_slot, stencil_channel);
  if (host_swizzle != reported_stencil_swizzle_) {
    reported_stencil_swizzle_ = host_swizzle;
    if (stencil_channel < 4) {
      REXLOG_INFO("{} the stencil view's host swizzle is {:#05x}: the stencil byte (the product's "
                  "red) is its component {}",
                  kPrefix, host_swizzle, stencil_channel);
    } else {
      REXLOG_ERROR("{} the stencil view's host swizzle is {:#05x}, which reads no red: no vehicle "
                   "IDs (every pixel is the world)",
                   kPrefix, host_swizzle);
    }
  }
  CopyPush copy{};
  pass_timing_->Mark(list, "temporal depth copy");
  copy.source = f.depth_slot;
  passes_->DrawInto(list, depth, depth_pipeline_.get(), &copy, sizeof(copy));

  pass_timing_->Mark(list, "temporal motion vectors");
  RecordMotion(list, f, has_previous);

  if (view_ == View::kStencil &&
      passes_->EnsureImage(ids_, rw, rh, kIdsFormat, "temporal vehicle IDs")) {
    pass_timing_->Mark(list, "temporal vehicle IDs");
    copy.source = stencil_ref_;
    passes_->DrawInto(list, ids_, ids_pipeline_.get(), &copy, sizeof(copy));
    ids_recorded_ = true;
  }

  // The checks: this frame's colour is copied around them (a check needs the frame before's too).
  const bool color_before = color_recorded_[before] && color_frame_[before] + 1 == f.frame &&
                            color_[before].texture && color_[before].width == rw &&
                            color_[before].height == rh;
  const bool check_due = check_every_ != 0 && history && color_before &&
                         records_ % check_every_ == 0;
  const bool copy_color = check_every_ != 0 &&
                          (check_due || (records_ + 1) % check_every_ == 0);
  color_recorded_[cur_] = false;
  if (copy_color &&
      passes_->EnsureImage(color_[cur_], rw, rh, kColorFormat, "temporal colour history")) {
    pass_timing_->Mark(list, "temporal colour copy");
    copy.source = f.color_slot;
    passes_->DrawInto(list, color_[cur_], color_pipeline_.get(), &copy, sizeof(copy));
    color_recorded_[cur_] = true;
    color_frame_[cur_] = f.frame;
  }
  if (check_due && color_recorded_[cur_]) {
    RecordCheck(list, f);
  }

  recorded_ = true;
  recorded_frame_ = f.frame;
  motion_zero_ = !has_previous;
  ++records_;
  ++counters_.frames;
  counters_.zero_frames += !has_previous;
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
  push.stencil = stencil_ref_;
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

  // The same sequence as HostPasses::DrawInto, with this pass's own layout and more than one draw.
  // CHECK: 96 bytes of push constants for the vertex and the pixel stage on both backends (the
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
  if (has_previous && vehicles_) {
    // Debug: each vehicle ID whose matrix is usable draws again over its own pixels.
    for (uint32_t k = 1; k < temporal::kVehicleMatrices; ++k) {
      if (!(f.camera->previous_valid & (1u << k))) {
        continue;
      }
      MotionPush vehicle = push;
      vehicle.mode = 1;
      vehicle.id = k;
      SetColumns(vehicle, f.camera->clip_to_previous_pixels[k]);
      list->setGraphicsPushConstants(0, &vehicle, 0, sizeof(vehicle));
      list->drawInstanced(3, 1, 0, 0);
      ++counters_.vehicle_draws;
    }
  }
  list->setFramebuffer(nullptr);
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(image.texture.get(),
                                             plume::RenderTextureLayout::SHADER_READ));
  image.layout = plume::RenderTextureLayout::SHADER_READ;
}

// ---------------------------------------------------------------------------
// the check
// ---------------------------------------------------------------------------

bool TemporalMotion::EnsureReadbacks() {
  if (readbacks_failed_) {
    return false;
  }
  for (Readback& r : readbacks_) {
    if (r.buffer) {
      continue;
    }
    r.buffer = device_->CreateBuffer(
        plume::RenderBufferDesc::ReadbackBuffer(uint64_t(kCheckRowBytes) * kCheckRows),
        "temporal check readback");
    if (!r.buffer) {
      readbacks_failed_ = true;
      REXLOG_ERROR("{} the check's readback buffer could not be made: no checks", kPrefix);
      return false;
    }
  }
  return true;
}

void TemporalMotion::RecordCheck(plume::RenderCommandList* list, const Frame& f) {
  if (!EnsureReadbacks()) {
    return;
  }
  Readback& r = readbacks_[next_readback_];
  if (r.busy) {
    return;  // every readback is still in flight: this check waits for the next round
  }
  if (!passes_->EnsureImage(check_, kCheckImageWidth, kCheckRows, kCheckFormat,
                            "temporal check samples")) {
    return;
  }
  const uint32_t index = next_readback_;
  next_readback_ = (next_readback_ + 1) % kReadbacks;
  const uint32_t before = cur_ ^ 1u;

  pass_timing_->Mark(list, "temporal check");
  CheckPush push{};
  push.depth = depth_[cur_].slot;
  push.stencil = stencil_ref_;
  push.motion = motion_.slot;
  push.prev_depth = depth_[before].slot;
  push.color = color_[cur_].slot;
  push.prev_color = color_[before].slot;
  push.delta[0] = f.previous_jitter[0] - f.jitter[0];
  push.delta[1] = f.previous_jitter[1] - f.jitter[1];
  passes_->DrawInto(list, check_, check_pipeline_.get(), &push, sizeof(push), true);
  // CHECK: an R32G32B32A32_FLOAT render target copied to a readback buffer with a placed footprint
  // whose row width is in texels (the flicker probe does it for 8 bits per channel); DrawInto moves
  // the image from COPY_SOURCE back to COLOR_WRITE next time.
  list->barriers(plume::RenderBarrierStage::COPY,
                 plume::RenderTextureBarrier(check_.texture.get(),
                                             plume::RenderTextureLayout::COPY_SOURCE));
  check_.layout = plume::RenderTextureLayout::COPY_SOURCE;
  list->copyTextureRegion(
      plume::RenderTextureCopyLocation::PlacedFootprint(
          r.buffer.get(), kCheckFormat, kCheckImageWidth, kCheckRows, 1, kCheckImageWidth, 0),
      plume::RenderTextureCopyLocation::Subresource(check_.texture.get()));
  r.busy = true;

  CheckJob job;
  job.frame = f.frame;
  job.render_size[0] = f.render_size[0];
  job.render_size[1] = f.render_size[1];
  job.guest_size[0] = f.guest_size[0];
  job.guest_size[1] = f.guest_size[1];
  job.to_render[0] = double(f.render_size[0]) / double(f.guest_size[0]);
  job.to_render[1] = double(f.render_size[1]) / double(f.guest_size[1]);
  job.jitter[0] = f.jitter[0];
  job.jitter[1] = f.jitter[1];
  job.view_projection_inverse = f.camera->view_projection_inverse;
  job.previous_view_projection = f.previous->view_projection;
  job.previous_near = f.previous->near_plane;
  job.previous_far = f.previous->far_plane;
  job.vehicle_slots = vehicles_ ? (f.camera->previous_valid & ~1u) : 0u;
  const uint64_t generation = generation_;
  frames_->RetireAction([this, index, job, generation] {
    if (generation == generation_) {
      Process(index, job);
    }
  });
}

void TemporalMotion::Process(uint32_t readback, const CheckJob& job) {
  Readback& r = readbacks_[readback];
  r.busy = false;
  const void* mapped = r.buffer ? r.buffer->map() : nullptr;
  if (!mapped) {
    REXLOG_ERROR("{} the check of frame {} could not be read back", kPrefix, job.frame);
    return;
  }
  Judge(job, static_cast<const float*>(mapped));
  r.buffer->unmap();
  ++counters_.checks;
}

// The two judgements of the check in double precision (temporal_mv.h): the vectors against the world
// point seen at each sample pixel, and the previous frame's depth and colour where the vectors point.
void TemporalMotion::Judge(const CheckJob& job, const float* rows) {
  const double width = double(job.render_size[0]);
  const double height = double(job.render_size[1]);
  uint32_t samples = 0;
  uint32_t sky = 0;
  uint32_t vehicle = 0;
  uint32_t skipped = 0;
  uint32_t within_tolerance = 0;
  double error_sum = 0.0;
  double error_max = 0.0;
  double length_sum = 0.0;
  double length_max = 0.0;
  uint32_t small = 0;
  double small_sum = 0.0;
  double small_max = 0.0;
  uint32_t in_view = 0;
  uint32_t depth_agrees = 0;
  uint32_t colour = 0;
  double warp_sum = 0.0;
  double still_sum = 0.0;
  uint32_t warp_within[3] = {};   // 2, 8 and 16 levels
  uint32_t still_within[3] = {};
  constexpr double kLevels[3] = {2.0, 8.0, 16.0};
  constexpr double kTolerance = 0.05;

  for (uint32_t j = 0; j < kCheckRows; ++j) {
    const float* row = reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(rows) +
                                                      size_t(j) * kCheckRowBytes);
    for (uint32_t i = 0; i < kCheckColumns; ++i) {
      const float* a = row + size_t(2 * i) * 4;
      const float* b = a + 4;
      const double depth = a[0];
      const int id = int(a[1] + 0.5f);
      if (!(depth > 0.0)) {
        ++sky;  // cleared depth: the far plane, no surface to project
        continue;
      }
      if (id > 0 && id < 32 && (job.vehicle_slots & (1u << id))) {
        ++vehicle;  // drawn with the game's own matrix: not this check's
        continue;
      }
      // The pixel the shader sampled (integer arithmetic on both sides).
      const uint32_t px = ((2 * i + 1) * job.render_size[0]) / 32;
      const uint32_t py = ((2 * j + 1) * job.render_size[1]) / 18;
      const double pos[2] = {double(px) + 0.5 - job.jitter[0], double(py) + 0.5 - job.jitter[1]};
      const double ndc[4] = {2.0 * pos[0] / width - 1.0, 1.0 - 2.0 * pos[1] / height, 1.0 - depth,
                             1.0};
      // The world point: unproject with this frame's view-projection inverse (row vectors).
      double world[4];
      for (int c = 0; c < 4; ++c) {
        world[c] = 0.0;
        for (int r = 0; r < 4; ++r) {
          world[c] += ndc[r] * job.view_projection_inverse.m[r][c];
        }
      }
      if (!(std::fabs(world[3]) > 1e-30)) {
        ++skipped;
        continue;
      }
      const double point[4] = {world[0] / world[3], world[1] / world[3], world[2] / world[3], 1.0};
      // ... and the previous frame's view-projection of it.
      double previous[4];
      for (int c = 0; c < 4; ++c) {
        previous[c] = 0.0;
        for (int r = 0; r < 4; ++r) {
          previous[c] += point[r] * job.previous_view_projection.m[r][c];
        }
      }
      if (!(std::fabs(previous[3]) > 1e-9)) {
        ++skipped;
        continue;
      }
      // Guest pixels in the pixel-edge convention, then render pixels.
      const double guest[2] = {(previous[0] / previous[3] + 1.0) * double(job.guest_size[0]) * 0.5,
                               (1.0 - previous[1] / previous[3]) * double(job.guest_size[1]) * 0.5};
      const double expected[2] = {std::clamp(guest[0] * job.to_render[0] - pos[0], -width, width),
                                  std::clamp(guest[1] * job.to_render[1] - pos[1], -height, height)};
      const double mv[2] = {a[2], a[3]};
      const double error = std::hypot(mv[0] - expected[0], mv[1] - expected[1]);
      const double length = std::hypot(expected[0], expected[1]);
      ++samples;
      error_sum += error;
      error_max = std::max(error_max, error);
      within_tolerance += error <= kTolerance;
      length_sum += length;
      length_max = std::max(length_max, length);
      if (length <= kSmallVector) {
        ++small;
        small_sum += error;
        small_max = std::max(small_max, error);
      }

      // The reprojection: the previous frame's depth where the vector points against the depth that
      // point should have there; where they agree (no disocclusion, no edge) the colour errors count.
      if (b[1] < 0.5f || b[2] < 0.0f) {
        continue;  // the vector points out of the image
      }
      ++in_view;
      const double expected_depth = 1.0 - previous[2] / previous[3];
      const double want = ViewDistance(expected_depth, job.previous_near, job.previous_far);
      const double have = ViewDistance(double(b[0]), job.previous_near, job.previous_far);
      if (want > 0.0 && have > 0.0 && std::fabs(have - want) <= 0.02 * want) {
        ++depth_agrees;
        ++colour;
        warp_sum += b[2];
        still_sum += b[3];
        for (int level = 0; level < 3; ++level) {
          warp_within[level] += b[2] <= kLevels[level];
          still_within[level] += b[3] <= kLevels[level];
        }
      }
    }
  }

  if (check_lines_ >= kMaxCheckLines) {
    return;
  }
  ++check_lines_;
  REXLOG_INFO(
      "{} check of frame {}: vectors against the world through the previous view-projection: {} "
      "samples ({} sky, {} vehicle pixels, {} unusable), error mean {:.4f} max {:.4f} px, {:.0f}% "
      "within {} px; vectors up to {:.0f} px: {} samples, mean {:.4f} max {:.4f}; scene motion mean "
      "{:.2f} max {:.2f} px | reprojection: {} samples where the vector points into the previous "
      "image, its depth agrees (2%) for {} ({:.0f}%); colour error of those, in levels of 255: with "
      "the vectors mean {:.2f} ({:.0f}% within 2, {:.0f}% within 8, {:.0f}% within 16), without "
      "them mean {:.2f} ({:.0f}%, {:.0f}%, {:.0f}%){}",
      kPrefix, job.frame, samples, sky, vehicle, skipped, Mean(error_sum, samples), error_max,
      Percent(within_tolerance, samples), kTolerance, kSmallVector, small, Mean(small_sum, small),
      small_max, Mean(length_sum, samples), length_max, in_view, depth_agrees,
      Percent(depth_agrees, in_view), Mean(warp_sum, colour), Percent(warp_within[0], colour),
      Percent(warp_within[1], colour), Percent(warp_within[2], colour), Mean(still_sum, colour),
      Percent(still_within[0], colour), Percent(still_within[1], colour),
      Percent(still_within[2], colour),
      check_lines_ == kMaxCheckLines ? " (the last of these lines)" : "");
}

// ---------------------------------------------------------------------------
// the debug view and the kept images
// ---------------------------------------------------------------------------

void TemporalMotion::DrawDebug(plume::RenderCommandList* list, uint32_t width, uint32_t height) {
  if (!available() || !recorded_ || !list || width == 0 || height == 0) {
    return;
  }
  const View view = ReadView();
  const HostImage* image = nullptr;
  uint32_t mode = 0;
  switch (view) {
    case View::kOff:
      return;
    case View::kMotion:
      image = &motion_;
      mode = 0;
      break;
    case View::kDepth:
      image = &depth_[cur_];
      mode = 1;
      break;
    case View::kStencil:
      image = ids_recorded_ ? &ids_ : nullptr;  // made with the next frame after the switch
      mode = 2;
      break;
  }
  if (!image || !image->texture || image->width == 0 || image->height == 0 || !PrepareDebug()) {
    return;
  }
  // Letterboxed like the present: the largest rectangle of the image's aspect, centred.
  const double fit = std::min(double(width) / image->width, double(height) / image->height);
  const uint32_t fw = std::max(uint32_t(std::lround(image->width * fit)), 1u);
  const uint32_t fh = std::max(uint32_t(std::lround(image->height * fit)), 1u);
  const uint32_t fx = (width - std::min(fw, width)) / 2;
  const uint32_t fy = (height - std::min(fh, height)) / 2;
  const plume::RenderViewport viewport{float(fx), float(fy), float(fw), float(fh)};
  list->setViewports(&viewport, 1);
  const plume::RenderRect scissor(int32_t(fx), int32_t(fy), int32_t(fx + fw), int32_t(fy + fh));
  list->setScissors(&scissor, 1);
  passes_->Bind(list);
  list->setPipeline(debug_pipeline_.get());
  DebugPush push{};
  push.source = image->slot;
  push.mode = mode;
  push.origin[0] = float(fx);
  push.origin[1] = float(fy);
  push.scale[0] = float(image->width) / float(fw);
  push.scale[1] = float(image->height) / float(fh);
  push.range = float(REXCVAR_GET(mcla_gpu_temporal_view_range));
  list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  list->drawInstanced(3, 1, 0, 0);
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

TemporalMotion::Counters TemporalMotion::TakeCounters() {
  const Counters c = counters_;
  counters_ = Counters{};
  return c;
}

}  // namespace mcla::gpu
