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

TemporalMotion::~TemporalMotion() {
  Shutdown();
}

void TemporalMotion::Initialize(Device& device, BindlessHeap& heap, HostPasses& passes,
                                PassTiming& pass_timing) {
  device_ = &device;
  heap_ = &heap;
  passes_ = &passes;
  pass_timing_ = &pass_timing;
  mask_.Initialize(passes, pass_timing);
  failed_ = false;
  initialized_ = true;
}

void TemporalMotion::Shutdown() {
  // The GPU is idle and the heap goes later: the slots are not handed back (PostAa::Shutdown).
  for (HostImage& image : depth_) {
    image = HostImage{};
  }
  motion_ = HostImage{};
  table_ = HostImage{};
  mask_.Shutdown();
  mask_recorded_ = false;
  table_pipeline_.reset();
  depth_pipeline_.reset();
  motion_pipeline_.reset();
  table_shader_.reset();
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
  passes_->ReleaseImage(table_);
  mask_.Release();
  mask_recorded_ = false;
  recorded_ = false;
}

uint64_t TemporalMotion::HeldBytes() const {
  uint64_t bytes = mask_.HeldBytes();
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
  if (motion_pipeline_) {
    return true;
  }
  if (failed_ || !initialized_) {
    return false;
  }
  // The host pass layout (host_passes.h) with 96 bytes of push constants instead of 48: the four
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
  const HostShaderBlob table = TemporalVehicleTablePixelShader();
  motion_shader_ = passes_->CreateShader(motion.data, motion.size, motion.name);
  depth_shader_ = passes_->CreateShader(depth.data, depth.size, depth.name);
  table_shader_ = passes_->CreateShader(table.data, table.size, table.name);
  bool ok = motion_layout_ && motion_shader_ && depth_shader_;
  if (ok) {
    motion_pipeline_ = passes_->CreatePipeline(*motion_layout_, *motion_shader_, kMotionFormat,
                                               "temporal motion vectors");
    depth_pipeline_ = passes_->CreatePipeline(*depth_shader_, kDepthFormat, "temporal depth copy");
    ok = motion_pipeline_ && depth_pipeline_;
  }
  if (ok) {
    // The vehicle table's rows are drawn with the host pass layout as it is (up to 64 bytes of push
    // constants). Without it the vectors are the camera's for every pixel, as before the vehicles.
    if (table_shader_) {
      table_pipeline_ =
          passes_->CreatePipeline(*table_shader_, kTableFormat, "temporal vehicle table");
    }
    if (!table_pipeline_) {
      REXLOG_ERROR("{} the vehicle table's pass could not be made: no vehicle vectors this session "
                   "(the camera's motion for every pixel)",
                   kPrefix);
    }
  }
  if (!ok) {
    failed_ = true;
    motion_pipeline_.reset();
    REXLOG_ERROR("{} the passes could not be made (layout, shaders or pipelines): no motion "
                 "vectors this session",
                 kPrefix);
    return false;
  }
  REXLOG_INFO("{} passes ready: depth {}, vectors {} (render size), vehicle table {}", kPrefix,
              "R32_FLOAT", "R16G16_FLOAT", table_pipeline_ ? "yes" : "NO");
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

  pass_timing_->Mark(list, "temporal motion vectors");
  RecordMotion(list, f, has_previous, stencil, table_slot);

  // The shadow decals' mask, when the Scene drew any (it needs no previous frame).
  mask_recorded_ = mask_.Resolve(list, rw, rh, stencil);

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
                                  bool has_previous, uint32_t stencil, uint32_t table) {
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
  if (has_previous) {
    float m[16];
    ToFloats(temporal::CameraToPreviousPixels(*f.camera, *f.previous), m);
    SetColumns(push, m);
  } else {
    push.mode = 2;  // zero: the history starts again
  }

  // The same sequence as HostPasses::DrawInto, with this pass's own layout.
  // CHECK: 96 bytes of push constants for the vertex and the pixel stage on both backends (the
  // resolve memory's fused pass already uses 72; Vulkan guarantees 128, D3D12 has 64 dwords of root
  // signature for 24 of them and the five tables) and the host layout's four texture sets and
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
  k.mask = mask_recorded_ ? mask_.mask() : nullptr;
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
  const std::string mask = mask_.Summary();
  if (!mask.empty()) {
    out += (out.empty() ? "" : " | ") + mask;
  }
  return out;
}

}  // namespace mcla::gpu
