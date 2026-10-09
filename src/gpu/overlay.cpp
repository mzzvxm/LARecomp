// MCLA GPU renderer -- the SDK overlays. See overlay.h.
//
// Contract followed: the SDK's own D3D12 immediate drawer
// (src/ui/d3d12/d3d12_immediate_drawer.cpp) for blend state, the four
// samplers, the line/triangle pipelines, draw semantics (indexed draws add
// base_vertex; non-indexed draws start at base_vertex) and the scissor
// conversion (ImmediateDrawer::ScissorToRenderTarget). Threading model: this
// renderer's; reblue
// imgui_overlay_drawer.cpp and reeot imgui_overlay.cpp (BSD-3-Clause) record
// on the UI thread instead.

#include "overlay.h"

#include <cstddef>
#include <cstring>
#include <format>
#include <string>

#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/graphics_util.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include "descriptors.h"
#include "device.h"
#include "frame.h"
#include "host_shaders.h"
#include "upload.h"

namespace mcla::gpu {

namespace {

// Matches OverlayPush in hlsl/overlay_common.hlsli.
struct OverlayPushConstants {
  float scale[2];
  float translate[2];
  uint32_t texture;
  uint32_t sampler_index;
};
static_assert(sizeof(OverlayPushConstants) == 24);
static_assert(sizeof(rex::ui::ImmediateVertex) == 20);

// Frames kept for reuse by the channel; enough for one being recorded, one
// published and one retained, plus slack.
constexpr size_t kMaxFreeFrames = 4;

constexpr uint64_t AlignUp(uint64_t v, uint64_t alignment) {
  return (v + alignment - 1) & ~(alignment - 1);
}

// ImmediateDrawer::ScissorToRenderTarget against the back buffer the frame is
// drawn into, which during a resize is not the size it was recorded for.
bool ScissorToPixels(const OverlayDraw& draw, float space_width, float space_height,
                     uint32_t target_width, uint32_t target_height, plume::RenderRect& out) {
  if (!draw.scissor) {
    out = plume::RenderRect(0, 0, int32_t(target_width), int32_t(target_height));
    return target_width && target_height;
  }
  const float width = float(target_width);
  const float height = float(target_height);
  const float scale_x = width / space_width;
  const float scale_y = height / space_height;
  const float x0 = rex::clamp_float(draw.scissor_left * scale_x, 0.0f, width);
  const float y0 = rex::clamp_float(draw.scissor_top * scale_y, 0.0f, height);
  const float x1 = rex::clamp_float(draw.scissor_right * scale_x, x0, width);
  const float y1 = rex::clamp_float(draw.scissor_bottom * scale_y, y0, height);
  // Top-left includes .5, bottom-right excludes it (16.8 fixed point).
  const int32_t ix0 = (rex::ui::FloatToD3D11Fixed16p8(x0) + 127) >> 8;
  const int32_t iy0 = (rex::ui::FloatToD3D11Fixed16p8(y0) + 127) >> 8;
  const int32_t ix1 = (rex::ui::FloatToD3D11Fixed16p8(x1) + 127) >> 8;
  const int32_t iy1 = (rex::ui::FloatToD3D11Fixed16p8(y1) + 127) >> 8;
  out = plume::RenderRect(ix0, iy0, ix1, iy1);
  return ix1 > ix0 && iy1 > iy0;
}

}  // namespace

// ---------------------------------------------------------------------------
// OverlayFrame
// ---------------------------------------------------------------------------

bool OverlayFrame::References(uint32_t texture_id) const {
  for (const OverlayDraw& draw : draws) {
    if (draw.texture_id == texture_id) {
      return true;
    }
  }
  return false;
}

void OverlayFrame::Clear() {
  sequence = 0;
  width = 0.0f;
  height = 0.0f;
  vertices.clear();
  indices.clear();
  draws.clear();
}

// ---------------------------------------------------------------------------
// OverlayChannel
// ---------------------------------------------------------------------------

uint32_t OverlayChannel::CreateTexture(uint32_t width, uint32_t height, bool linear, bool repeat,
                                       const uint8_t* rgba) {
  OverlayTextureUpload upload;
  upload.width = width;
  upload.height = height;
  upload.linear = linear;
  upload.repeat = repeat;
  const size_t bytes = size_t(width) * size_t(height) * 4;
  if (rgba && bytes) {
    // Copied outside the lock: a font atlas is megabytes.
    upload.rgba.assign(rgba, rgba + bytes);
  }
  std::lock_guard lock(mutex_);
  upload.id = next_texture_id_++;
  const uint32_t id = upload.id;
  if (closed_) {
    return id;
  }
  if (!rgba || !bytes) {
    REXLOG_ERROR("[gpu] overlay texture {}: {}x{} with {} pixel data; it will not be drawn", id,
                 width, height, rgba ? "empty" : "no");
    return id;
  }
  uploads_.push_back(std::move(upload));
  return id;
}

void OverlayChannel::DestroyTexture(uint32_t id) {
  std::lock_guard lock(mutex_);
  if (!closed_) {
    deaths_.push_back(id);
  }
}

std::unique_ptr<OverlayFrame> OverlayChannel::AcquireFrame() {
  std::unique_ptr<OverlayFrame> frame;
  {
    std::lock_guard lock(mutex_);
    if (!free_frames_.empty()) {
      frame = std::move(free_frames_.back());
      free_frames_.pop_back();
    }
  }
  if (!frame) {
    frame = std::make_unique<OverlayFrame>();
  }
  frame->Clear();
  return frame;
}

void OverlayChannel::Publish(std::unique_ptr<OverlayFrame> frame) {
  std::lock_guard lock(mutex_);
  frame->sequence = ++published_;
  if (latest_ && free_frames_.size() < kMaxFreeFrames) {
    free_frames_.push_back(std::move(latest_));
  }
  latest_ = std::move(frame);
}

void OverlayChannel::Take(OverlayIntake& out) {
  std::lock_guard lock(mutex_);
  out.uploads.swap(uploads_);
  out.deaths.swap(deaths_);
  out.frame = std::move(latest_);
}

bool OverlayChannel::PeekNewFrame(bool* has_draws) {
  std::lock_guard lock(mutex_);
  if (!latest_) {
    return false;
  }
  *has_draws = !latest_->empty();
  return true;
}

void OverlayChannel::Recycle(std::unique_ptr<OverlayFrame> frame) {
  std::lock_guard lock(mutex_);
  if (frame && free_frames_.size() < kMaxFreeFrames) {
    free_frames_.push_back(std::move(frame));
  }
}

void OverlayChannel::Close() {
  std::lock_guard lock(mutex_);
  closed_ = true;
  uploads_.clear();
  deaths_.clear();
  latest_.reset();
  free_frames_.clear();
}

// ---------------------------------------------------------------------------
// OverlayDrawer (UI thread)
// ---------------------------------------------------------------------------

OverlayDrawer::OverlayDrawer(std::shared_ptr<OverlayChannel> channel)
    : channel_(std::move(channel)) {}

std::unique_ptr<rex::ui::ImmediateTexture> OverlayDrawer::CreateTexture(
    uint32_t width, uint32_t height, rex::ui::ImmediateTextureFilter filter, bool is_repeated,
    const uint8_t* data) {
  const uint32_t id = channel_->CreateTexture(
      width, height, filter == rex::ui::ImmediateTextureFilter::kLinear, is_repeated, data);
  return std::make_unique<OverlayTexture>(width, height, id, channel_);
}

void OverlayDrawer::Begin(rex::ui::UIDrawContext& ui_draw_context, float space_width,
                          float space_height) {
  // The base resolves a non-positive coordinate space to render-target pixels;
  // its accessors return what it settled on.
  ImmediateDrawer::Begin(ui_draw_context, space_width, space_height);
  frame_ = channel_->AcquireFrame();
  frame_->width = coordinate_space_width();
  frame_->height = coordinate_space_height();
  batch_open_ = false;
}

void OverlayDrawer::BeginDrawBatch(const rex::ui::ImmediateDrawBatch& batch) {
  if (!frame_) {
    return;
  }
  batch_vertex_base_ = uint32_t(frame_->vertices.size());
  batch_index_base_ = uint32_t(frame_->indices.size());
  batch_vertex_count_ = (batch.vertices && batch.vertex_count > 0) ? uint32_t(batch.vertex_count)
                                                                   : 0;
  batch_indexed_ = batch.indices != nullptr;
  batch_index_count_ = (batch_indexed_ && batch.index_count > 0) ? uint32_t(batch.index_count) : 0;
  // Both pointers are valid only during this batch (ImGui's draw lists).
  frame_->vertices.insert(frame_->vertices.end(), batch.vertices,
                          batch.vertices + batch_vertex_count_);
  if (batch_index_count_) {
    frame_->indices.insert(frame_->indices.end(), batch.indices,
                           batch.indices + batch_index_count_);
  }
  batch_open_ = true;
}

void OverlayDrawer::Draw(const rex::ui::ImmediateDraw& draw) {
  if (!batch_open_ || !frame_ || draw.count <= 0) {
    return;
  }
  OverlayDraw d;
  d.count = uint32_t(draw.count);
  d.lines = draw.primitive_type == rex::ui::ImmediatePrimitiveType::kLines;
  d.indexed = batch_indexed_;
  bool in_range;
  if (d.indexed) {
    in_range = draw.index_offset >= 0 &&
               uint64_t(draw.index_offset) + d.count <= batch_index_count_;
    d.first_index = batch_index_base_ + uint32_t(draw.index_offset);
  } else {
    in_range = draw.base_vertex >= 0 &&
               uint64_t(draw.base_vertex) + d.count <= batch_vertex_count_;
  }
  if (!in_range) {
    if (!reported_bad_draw_) {
      reported_bad_draw_ = true;
      REXLOG_ERROR("[gpu] overlay draw outside its batch dropped: {} {} from {} (batch has {} "
                   "vertices, {} indices)",
                   d.count, d.indexed ? "indices" : "vertices",
                   d.indexed ? draw.index_offset : draw.base_vertex, batch_vertex_count_,
                   batch_index_count_);
    }
    return;
  }
  d.base_vertex = int32_t(batch_vertex_base_) + draw.base_vertex;
  // Every ImmediateTexture in this mode comes from CreateTexture above.
  d.texture_id = draw.texture ? static_cast<const OverlayTexture*>(draw.texture)->id()
                              : kOverlayNoTexture;
  d.scissor = draw.scissor;
  d.scissor_left = draw.scissor_left;
  d.scissor_top = draw.scissor_top;
  d.scissor_right = draw.scissor_right;
  d.scissor_bottom = draw.scissor_bottom;
  frame_->draws.push_back(d);
}

void OverlayDrawer::EndDrawBatch() {
  batch_open_ = false;
}

void OverlayDrawer::End() {
  if (frame_) {
    channel_->Publish(std::move(frame_));
  }
  batch_open_ = false;
  ImmediateDrawer::End();
  ++ended_passes_;
}

void OverlayDrawer::PublishEmpty(float width, float height) {
  std::unique_ptr<OverlayFrame> frame = channel_->AcquireFrame();
  frame->width = width;
  frame->height = height;
  channel_->Publish(std::move(frame));
}

// ---------------------------------------------------------------------------
// OverlayHost
// ---------------------------------------------------------------------------

OverlayHost::OverlayHost(rex::ui::WindowedAppContext& app_context, rex::ui::Window& window,
                         rex::ui::ImGuiDrawer& imgui_drawer, OverlayDrawer& drawer)
    : app_context_(app_context),
      window_(&window),
      imgui_drawer_(&imgui_drawer),
      drawer_(&drawer) {}

void OverlayHost::RequestPass() {
  if (!connected_.load(std::memory_order_acquire) ||
      pass_pending_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  std::shared_ptr<OverlayHost> self = shared_from_this();
  if (!app_context_.CallInUIThreadDeferred([self] { self->RunPass(); })) {
    // The UI loop no longer takes work (the app is quitting).
    pass_pending_.store(false, std::memory_order_release);
  }
}

void OverlayHost::RunPass() {
  if (connected_.load(std::memory_order_acquire)) {
    const uint32_t width = window_->GetActualPhysicalWidth();
    const uint32_t height = window_->GetActualPhysicalHeight();
    // Minimised: nothing to lay out against; the retained frame stays.
    if (width && height) {
      const uint64_t ended = drawer_->ended_passes();
      rex::ui::AppUIDrawContext context(width, height);
      imgui_drawer_->Draw(context);
      if (drawer_->ended_passes() == ended) {
        // No dialog: ImGuiDrawer::Draw returned before Begin. Without this the
        // last frame that had a dialog would stay retained.
        drawer_->PublishEmpty(float(width), float(height));
      }
    }
  }
  pass_pending_.store(false, std::memory_order_release);
}

void OverlayHost::Disconnect() {
  // The SDK objects die on the UI thread after OnShutdown (rex_app.cpp
  // OnDestroy), which is where this runs; RunPass runs on that same thread, so
  // a pass either finished before this or sees connected_ false. The pointers
  // themselves are left alone: nothing reads them once disconnected.
  connected_.store(false, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// OverlayRenderer (render thread)
// ---------------------------------------------------------------------------

OverlayRenderer::~OverlayRenderer() {
  Shutdown();
}

bool OverlayRenderer::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap,
                                 UploadRing& upload, std::shared_ptr<OverlayChannel> channel,
                                 plume::RenderFormat target_format) {
  if (device_) {
    return true;
  }
  if (!channel) {
    REXLOG_ERROR("[gpu] overlay renderer: no channel from the overlay drawer");
    return false;
  }
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  upload_ = &upload;
  channel_ = std::move(channel);
  if (!CreatePipelines(target_format)) {
    Shutdown();
    return false;
  }
  return true;
}

void OverlayRenderer::Shutdown() {
  if (channel_) {
    channel_->Close();
  }
  // The GPU is idle (Renderer::Shutdown drained the ring): no retirement.
  for (auto& [id, texture] : textures_) {
    texture.view.reset();
    texture.texture.reset();
  }
  textures_.clear();
  failed_textures_.clear();
  reported_missing_.clear();
  pending_deaths_.clear();
  intake_ = OverlayIntake{};
  retained_.reset();
  triangle_pipeline_.reset();
  line_pipeline_.reset();
  vertex_shader_.reset();
  pixel_shader_.reset();
  layout_.reset();
  channel_.reset();
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
  upload_ = nullptr;
}

bool OverlayRenderer::CreatePipelines(plume::RenderFormat target_format) {
  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(false, true);
  builder.addDescriptorSet(heap_->texture_set_builder());  // set 0: t0, space0
  builder.addDescriptorSet(heap_->sampler_set_builder());  // set 1: s0, space1
  builder.addPushConstant(0, 2, sizeof(OverlayPushConstants),
                          plume::RenderShaderStageFlag::VERTEX |
                              plume::RenderShaderStageFlag::PIXEL);  // b0, space2
  builder.end();
  layout_ = device_->CreatePipelineLayout(builder, "overlay layout");

  const HostShaderBlob vs = OverlayVertexShader();
  const HostShaderBlob ps = OverlayPixelShader();
  vertex_shader_ = device_->CreateShader(vs.data, vs.size, "main", vs.name);
  pixel_shader_ = device_->CreateShader(ps.data, ps.size, "main", ps.name);
  if (!layout_ || !vertex_shader_ || !pixel_shader_) {
    return false;
  }

  const plume::RenderInputSlot input_slot(0, sizeof(rex::ui::ImmediateVertex));
  const plume::RenderInputElement input_elements[] = {
      plume::RenderInputElement("POSITION", 0, 0, plume::RenderFormat::R32G32_FLOAT, 0,
                                offsetof(rex::ui::ImmediateVertex, x)),
      plume::RenderInputElement("TEXCOORD", 0, 1, plume::RenderFormat::R32G32_FLOAT, 0,
                                offsetof(rex::ui::ImmediateVertex, u)),
      plume::RenderInputElement("COLOR", 0, 2, plume::RenderFormat::R8G8B8A8_UNORM, 0,
                                offsetof(rex::ui::ImmediateVertex, color)),
  };

  plume::RenderBlendDesc blend;
  blend.blendEnabled = true;
  blend.srcBlend = plume::RenderBlend::SRC_ALPHA;
  blend.dstBlend = plume::RenderBlend::INV_SRC_ALPHA;
  blend.blendOp = plume::RenderBlendOperation::ADD;
  blend.srcBlendAlpha = plume::RenderBlend::ONE;
  blend.dstBlendAlpha = plume::RenderBlend::ONE;
  blend.blendOpAlpha = plume::RenderBlendOperation::ADD;

  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = layout_.get();
  desc.vertexShader = vertex_shader_.get();
  desc.pixelShader = pixel_shader_.get();
  desc.depthClipEnabled = true;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.fillMode = plume::RenderFillMode::SOLID;
  desc.multisampling.sampleCount = plume::RenderSampleCount::COUNT_1;
  desc.renderTargetFormat[0] = target_format;
  desc.renderTargetBlend[0] = blend;
  desc.renderTargetCount = 1;
  desc.inputSlots = &input_slot;
  desc.inputSlotsCount = 1;
  desc.inputElements = input_elements;
  desc.inputElementsCount = static_cast<uint32_t>(std::size(input_elements));

  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  triangle_pipeline_ = device_->CreateGraphicsPipeline(desc, "overlay triangles");
  desc.primitiveTopology = plume::RenderPrimitiveTopology::LINE_LIST;
  line_pipeline_ = device_->CreateGraphicsPipeline(desc, "overlay lines");
  return triangle_pipeline_ && line_pipeline_;
}

bool OverlayRenderer::HasNewContent() {
  if (!channel_) {
    return false;
  }
  bool new_has_draws = false;
  if (!channel_->PeekNewFrame(&new_has_draws)) {
    return false;
  }
  const bool retained_has_draws = retained_ && !retained_->empty();
  return new_has_draws || retained_has_draws;
}

void OverlayRenderer::Prepare(plume::RenderCommandList* list, uint32_t slot) {
  if (!channel_ || !list) {
    return;
  }
  channel_->Take(intake_);
  for (const OverlayTextureUpload& upload : intake_.uploads) {
    CreateTexture(list, slot, upload);
  }
  intake_.uploads.clear();
  pending_deaths_.insert(pending_deaths_.end(), intake_.deaths.begin(), intake_.deaths.end());
  intake_.deaths.clear();
  if (intake_.frame) {
    if (retained_) {
      channel_->Recycle(std::move(retained_));
    }
    retained_ = std::move(intake_.frame);
  }
  ReleaseDeadTextures();
}

void OverlayRenderer::CreateTexture(plume::RenderCommandList* list, uint32_t slot,
                                    const OverlayTextureUpload& upload) {
  const uint32_t width = upload.width;
  const uint32_t height = upload.height;
  const std::string name = std::format("overlay texture {} ({}x{})", upload.id, width, height);
  const uint64_t row_bytes = uint64_t(width) * 4;
  if (upload.rgba.size() < row_bytes * height) {
    REXLOG_ERROR("[gpu] {}: {} bytes of pixels for {} needed", name, upload.rgba.size(),
                 row_bytes * height);
    failed_textures_.insert(upload.id);
    return;
  }

  auto texture = device_->CreateTexture(
      plume::RenderTextureDesc::Texture2D(width, height, 1, plume::RenderFormat::R8G8B8A8_UNORM),
      name.c_str());
  if (!texture) {
    failed_textures_.insert(upload.id);
    return;
  }
  plume::RenderTextureViewDesc view_desc =
      plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::R8G8B8A8_UNORM);
  view_desc.mipLevels = 1;
  auto view = device_->CreateTextureView(*texture, view_desc, name.c_str());
  if (!view) {
    failed_textures_.insert(upload.id);
    return;
  }

  const uint64_t pitch = AlignUp(row_bytes, kTextureCopyPitchAlignment);
  const UploadSpan staging = upload_->Allocate(slot, pitch * height, kTextureCopyOffsetAlignment);
  if (!staging) {
    REXLOG_ERROR("[gpu] {}: no upload memory for {} bytes", name, pitch * height);
    failed_textures_.insert(upload.id);
    return;
  }
  for (uint32_t y = 0; y < height; ++y) {
    std::memcpy(staging.cpu + y * pitch, upload.rgba.data() + y * row_bytes, row_bytes);
  }
  list->barriers(plume::RenderBarrierStage::COPY,
                 plume::RenderTextureBarrier(texture.get(), plume::RenderTextureLayout::COPY_DEST));
  list->copyTextureRegion(
      plume::RenderTextureCopyLocation::Subresource(texture.get()),
      plume::RenderTextureCopyLocation::PlacedFootprint(staging.buffer,
                                                        plume::RenderFormat::R8G8B8A8_UNORM, width,
                                                        height, 1, uint32_t(pitch / 4),
                                                        staging.offset));
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(texture.get(), plume::RenderTextureLayout::SHADER_READ));

  const uint32_t bindless = heap_->AllocateTexture(*texture, *view);
  if (bindless == kInvalidBindlessSlot) {
    // Already named by the copy on the open list: retired, not destroyed.
    failed_textures_.insert(upload.id);
    frames_->Retire(std::move(view));
    frames_->Retire(std::move(texture));
    return;
  }

  GpuTexture gpu;
  gpu.texture = std::move(texture);
  gpu.view = std::move(view);
  gpu.slot = bindless;
  gpu.sampler = upload.linear ? (upload.repeat ? kSamplerLinearWrap : kSamplerLinearClamp)
                              : (upload.repeat ? kSamplerPointWrap : kSamplerPointClamp);
  textures_.emplace(upload.id, std::move(gpu));
  REXLOG_INFO("[gpu] {}: slot {}, {} {}", name, bindless, upload.linear ? "linear" : "point",
              upload.repeat ? "wrap" : "clamp");
}

void OverlayRenderer::ReleaseTexture(GpuTexture& texture) {
  // The slot first, so the descriptor is null before the texture dies.
  heap_->ReleaseTexture(texture.slot, TextureDimension::k2D);
  frames_->Retire(std::move(texture.view));
  frames_->Retire(std::move(texture.texture));
}

void OverlayRenderer::ReleaseDeadTextures() {
  size_t kept = 0;
  for (const uint32_t id : pending_deaths_) {
    if (retained_ && retained_->References(id)) {
      // The retained frame is drawn again until a newer one replaces it.
      pending_deaths_[kept++] = id;
      continue;
    }
    if (auto it = textures_.find(id); it != textures_.end()) {
      ReleaseTexture(it->second);
      textures_.erase(it);
    }
    failed_textures_.erase(id);
    reported_missing_.erase(id);
  }
  pending_deaths_.resize(kept);
}

uint32_t OverlayRenderer::Draw(plume::RenderCommandList* list, uint32_t slot,
                               uint32_t target_width, uint32_t target_height) {
  if (!list || !retained_ || retained_->empty() || !target_width || !target_height) {
    return 0;
  }
  const OverlayFrame& frame = *retained_;
  if (!(frame.width > 0.0f) || !(frame.height > 0.0f)) {
    return 0;
  }

  const uint64_t vertex_bytes = frame.vertices.size() * sizeof(rex::ui::ImmediateVertex);
  const uint64_t index_bytes = frame.indices.size() * sizeof(uint16_t);
  UploadSpan vertices;
  UploadSpan indices;
  if (vertex_bytes) {
    vertices = upload_->Allocate(slot, vertex_bytes, 16);
  }
  if (index_bytes) {
    indices = upload_->Allocate(slot, index_bytes, 16);
  }
  if ((vertex_bytes && !vertices) || (index_bytes && !indices)) {
    REXLOG_ERROR("[gpu] overlay frame dropped: no upload memory for {} + {} bytes", vertex_bytes,
                 index_bytes);
    return 0;
  }
  if (vertices) {
    std::memcpy(vertices.cpu, frame.vertices.data(), vertex_bytes);
  }
  if (indices) {
    std::memcpy(indices.cpu, frame.indices.data(), index_bytes);
  }

  list->setViewports(plume::RenderViewport(0.0f, 0.0f, float(target_width), float(target_height)));
  list->setGraphicsPipelineLayout(layout_.get());
  list->setGraphicsDescriptorSet(heap_->texture_set(), 0);
  list->setGraphicsDescriptorSet(heap_->sampler_set(), 1);
  const plume::RenderInputSlot input_slot(0, sizeof(rex::ui::ImmediateVertex));
  if (vertices) {
    const plume::RenderVertexBufferView view(vertices.reference(), uint32_t(vertex_bytes));
    list->setVertexBuffers(0, &view, 1, &input_slot);
  }
  if (indices) {
    const plume::RenderIndexBufferView view(indices.reference(), uint32_t(index_bytes),
                                            plume::RenderFormat::R16_UINT);
    list->setIndexBuffer(&view);
  }

  OverlayPushConstants push;
  push.scale[0] = 2.0f / frame.width;
  push.scale[1] = -2.0f / frame.height;
  push.translate[0] = -1.0f;
  push.translate[1] = 1.0f;

  const plume::RenderPipeline* bound = nullptr;
  uint32_t recorded = 0;
  for (const OverlayDraw& draw : frame.draws) {
    if (!vertices || (draw.indexed && !indices)) {
      continue;
    }
    plume::RenderRect scissor;
    if (!ScissorToPixels(draw, frame.width, frame.height, target_width, target_height, scissor)) {
      continue;
    }
    push.texture = kOverlayNoTexture;
    push.sampler_index = kSamplerPointClamp;
    if (draw.texture_id != kOverlayNoTexture) {
      const auto it = textures_.find(draw.texture_id);
      if (it == textures_.end()) {
        // A failed creation was logged when it failed; anything else is a
        // texture the frame should not be able to name.
        if (!failed_textures_.count(draw.texture_id) &&
            reported_missing_.insert(draw.texture_id).second) {
          REXLOG_ERROR("[gpu] overlay draw names texture {}, which does not exist; skipped",
                       draw.texture_id);
        }
        continue;
      }
      push.texture = it->second.slot;
      push.sampler_index = it->second.sampler;
    }
    const plume::RenderPipeline* pipeline =
        draw.lines ? line_pipeline_.get() : triangle_pipeline_.get();
    if (pipeline != bound) {
      list->setPipeline(pipeline);
      bound = pipeline;
    }
    list->setScissors(scissor);
    list->setGraphicsPushConstants(0, &push);
    if (draw.indexed) {
      list->drawIndexedInstanced(draw.count, 1, draw.first_index, draw.base_vertex, 0);
    } else {
      list->drawInstanced(draw.count, 1, uint32_t(draw.base_vertex), 0);
    }
    ++recorded;
  }
  if (recorded && !reported_first_frame_) {
    reported_first_frame_ = true;
    REXLOG_INFO("[gpu] overlay: first frame drawn ({} draws, {} vertices, {}x{} space on {}x{})",
                recorded, frame.vertices.size(), frame.width, frame.height, target_width,
                target_height);
  }
  return recorded;
}

}  // namespace mcla::gpu
