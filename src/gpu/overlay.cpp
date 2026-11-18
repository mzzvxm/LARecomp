// MCLA GPU renderer -- the SDK overlays. See overlay.h.
//
// Contract followed: the blend state, the four samplers and the scissor conversion of the SDK's own D3D12
// immediate drawer (the 0.10 src/ui/d3d12/d3d12_immediate_drawer.cpp, now gone), and Dear ImGui's own backends
// for what a draw command and a texture request mean (ImDrawCmd, ImTextureData). Threading model: this
// renderer's; reblue imgui_overlay_drawer.cpp and reeot imgui_overlay.cpp (BSD-3-Clause) record on the UI
// thread instead.

#include <atomic>
#include "overlay.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <string>

#include <imgui.h>

#include <rex/logging/log_category.h>
#include <rex/math.h>
#include <rex/ui/ui_system.h>
#include <rex/overlay_renderer.h>

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
  // overlay_hdr_ps only (hdr.h HdrLevels): the UI's level on an HDR10 back buffer.
  float hdr_white;
  float hdr_peak;
  float hdr_knee;
  uint32_t hdr_flags;
};
static_assert(sizeof(OverlayPushConstants) == 40);
// The pipeline reads ImGui's vertices as they are.
static_assert(sizeof(ImDrawVert) == sizeof(OverlayVertex));
static_assert(offsetof(ImDrawVert, pos) == offsetof(OverlayVertex, x));
static_assert(offsetof(ImDrawVert, uv) == offsetof(OverlayVertex, u));
static_assert(offsetof(ImDrawVert, col) == offsetof(OverlayVertex, color));
static_assert(sizeof(ImDrawIdx) == sizeof(uint16_t), "the overlay's index buffer is 16-bit");

// Frames kept for reuse by the channel; enough for one being recorded, one
// published and one retained, plus slack.
constexpr size_t kMaxFreeFrames = 4;

constexpr uint64_t AlignUp(uint64_t v, uint64_t alignment) {
  return (v + alignment - 1) & ~(alignment - 1);
}

// A float as the 16.8 fixed point the rasterizer snaps a scissor to (Xenia's FloatToD3D11Fixed16p8; the SDK keeps
// its copy private since 0.11). https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
int32_t FloatToD3D11Fixed16p8(float f32) {
  // n == NaN || n.unbiasedExponent < -f-1 -> 0 . 0
  if (!(std::abs(f32) >= 1.0f / 512.0f)) {
    return 0;
  }
  // n >= (2^(i-1)-2^-f) -> 2^(i-1)-1 . 2^f-1
  if (f32 >= 32768.0f - 1.0f / 256.0f) {
    return (1 << 23) - 1;
  }
  // n <= -2^(i-1) -> -2^(i-1) . 0
  if (f32 <= -32768.0f) {
    return -32768 * 256;
  }
  uint32_t f32_bits;
  std::memcpy(&f32_bits, &f32, sizeof(f32_bits));
  // The mantissa plus the hidden bit, with the sign applied as a two's complement.
  int32_t result = int32_t((f32_bits & ((1u << 23) - 1)) | (1u << 23));
  if ((f32_bits >> 31) != 0) {
    result = -result;
  }
  // extraBits = (mantissa - f) - n.unbiasedExponent (guaranteed to be >= 0).
  const int32_t exponent = int32_t((f32_bits >> 23) & 255) - 127;
  const uint32_t extra_bits = uint32_t(15 - exponent);
  if (extra_bits) {
    // Round to nearest-even, then shift right sign extending.
    result = int32_t(uint32_t(result) + (1u << (extra_bits - 1)) - 1 + ((uint32_t(result) >> extra_bits) & 1));
    result >>= extra_bits;
  }
  return result;
}

// The clip rectangle of a draw against the back buffer the frame is drawn into, which during a resize is not the
// size it was recorded for.
bool ScissorToPixels(const OverlayDraw& draw, float space_width, float space_height,
                     uint32_t target_width, uint32_t target_height, plume::RenderRect& out) {
  const float width = float(target_width);
  const float height = float(target_height);
  const float scale_x = width / space_width;
  const float scale_y = height / space_height;
  const float x0 = rex::clamp_float(draw.scissor_left * scale_x, 0.0f, width);
  const float y0 = rex::clamp_float(draw.scissor_top * scale_y, 0.0f, height);
  const float x1 = rex::clamp_float(draw.scissor_right * scale_x, x0, width);
  const float y1 = rex::clamp_float(draw.scissor_bottom * scale_y, y0, height);
  // Top-left includes .5, bottom-right excludes it (16.8 fixed point).
  const int32_t ix0 = (FloatToD3D11Fixed16p8(x0) + 127) >> 8;
  const int32_t iy0 = (FloatToD3D11Fixed16p8(y0) + 127) >> 8;
  const int32_t ix1 = (FloatToD3D11Fixed16p8(x1) + 127) >> 8;
  const int32_t iy1 = (FloatToD3D11Fixed16p8(y1) + 127) >> 8;
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

void OverlayChannel::ReplaceTexture(uint32_t id, uint32_t width, uint32_t height, bool linear,
                                    bool repeat, const uint8_t* rgba) {
  OverlayTextureUpload upload;
  upload.id = id;
  upload.width = width;
  upload.height = height;
  upload.linear = linear;
  upload.repeat = repeat;
  const size_t bytes = size_t(width) * size_t(height) * 4;
  if (!rgba || !bytes) {
    return;
  }
  upload.rgba.assign(rgba, rgba + bytes);
  std::lock_guard lock(mutex_);
  if (closed_) {
    return;
  }
  // An upload for an id that is already queued (two updates inside one render frame) replaces it: the render
  // thread would otherwise make a texture and retire it in the same frame.
  for (OverlayTextureUpload& queued : uploads_) {
    if (queued.id == id) {
      queued = std::move(upload);
      return;
    }
  }
  uploads_.push_back(std::move(upload));
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
// OverlayCapture (UI thread)
// ---------------------------------------------------------------------------

namespace {

// The pixels of an ImGui texture as RGBA8, from the 32-bit RGBA or the 8-bit alpha format.
bool ExpandTexturePixels(const ImTextureData& tex, std::vector<uint8_t>& rgba) {
  if (!tex.Pixels || tex.Width <= 0 || tex.Height <= 0) {
    return false;
  }
  const size_t count = size_t(tex.Width) * size_t(tex.Height);
  if (tex.Format == ImTextureFormat_RGBA32) {
    rgba.assign(tex.Pixels, tex.Pixels + count * 4);
    return true;
  }
  // Alpha8 (ImGui's font atlas can be one): white with the stored coverage, as ImGui's own backends expand it.
  rgba.resize(count * 4);
  for (size_t i = 0; i < count; ++i) {
    rgba[i * 4 + 0] = 255;
    rgba[i * 4 + 1] = 255;
    rgba[i * 4 + 2] = 255;
    rgba[i * 4 + 3] = tex.Pixels[i];
  }
  return true;
}

}  // namespace

// CHECK: ImGui 1.92 grows the font atlas by making a new ImTextureData and sending the old one WantDestroy; both paths end in the channel (a new id,
// a death). A glyph burst re-sends the whole atlas (ReplaceTexture): if the log shows many "overlay texture" lines or the frame time moves while text
// is typed, send the update rectangles instead (copy them into the existing texture between two barriers).
void OverlayCapture::ServeTextures(ImDrawData& draw_data) {
  ++pass_;
  if (draw_data.Textures) {
    std::vector<uint8_t> rgba;
    for (ImTextureData* tex : *draw_data.Textures) {
      if (!tex) {
        continue;
      }
      auto known = tracked_.find(tex);
      // A texture this capture does not know that claims to be done (made before it, or an object address that was
      // reused): ask ImGui to send it again.
      if (known == tracked_.end() && tex->Status == ImTextureStatus_OK) {
        tex->SetStatus(ImTextureStatus_WantCreate);
      }
      switch (tex->Status) {
        case ImTextureStatus_WantCreate: {
          if (known != tracked_.end()) {
            channel_->DestroyTexture(known->second.id);  // a new object at an old address
            tracked_.erase(known);
          }
          uint32_t id = 0;
          if (ExpandTexturePixels(*tex, rgba)) {
            id = channel_->CreateTexture(uint32_t(tex->Width), uint32_t(tex->Height), /*linear=*/true,
                                         /*repeat=*/false, rgba.data());
          }
          if (id == 0) {
            REXLOG_ERROR("[gpu] overlay texture {}: no pixels to create it from; it will not be drawn",
                         tex->UniqueID);
            break;
          }
          tex->SetTexID(static_cast<ImTextureID>(id));
          tex->SetStatus(ImTextureStatus_OK);
          tracked_[tex] = Tracked{id, pass_};
          break;
        }
        case ImTextureStatus_WantUpdates: {
          // The whole texture again, not the rectangles: the render thread makes a new GPU texture (a frame in
          // flight may still read the old one), so there is nothing to patch. Glyphs arrive in a few bursts.
          if (known != tracked_.end() && ExpandTexturePixels(*tex, rgba)) {
            channel_->ReplaceTexture(known->second.id, uint32_t(tex->Width), uint32_t(tex->Height),
                                     /*linear=*/true, /*repeat=*/false, rgba.data());
            known->second.seen = pass_;
          }
          tex->SetStatus(ImTextureStatus_OK);
          break;
        }
        case ImTextureStatus_WantDestroy: {
          if (known != tracked_.end()) {
            channel_->DestroyTexture(known->second.id);
            tracked_.erase(known);
          }
          tex->SetStatus(ImTextureStatus_Destroyed);
          break;
        }
        default:
          if (known != tracked_.end()) {
            known->second.seen = pass_;
          }
          break;
      }
    }
  }
  // A texture that left the list without a destroy request (a dialog unregistered it): its GPU copy goes too.
  for (auto it = tracked_.begin(); it != tracked_.end();) {
    if (it->second.seen != pass_) {
      channel_->DestroyTexture(it->second.id);
      it = tracked_.erase(it);
    } else {
      ++it;
    }
  }
}

void OverlayCapture::RenderOverlays(ImDrawData& draw_data) {
  called_ = true;
  ServeTextures(draw_data);
  const float width = draw_data.DisplaySize.x;
  const float height = draw_data.DisplaySize.y;
  if (!(width > 0.0f) || !(height > 0.0f) || draw_data.CmdListsCount <= 0) {
    PublishEmpty();
    return;
  }
  std::unique_ptr<OverlayFrame> frame = channel_->AcquireFrame();
  frame->width = width;
  frame->height = height;
  frame->vertices.reserve(size_t(draw_data.TotalVtxCount));
  frame->indices.reserve(size_t(draw_data.TotalIdxCount));
  const ImVec2 origin = draw_data.DisplayPos;
  for (const ImDrawList* list : draw_data.CmdLists) {
    const uint32_t vertex_base = uint32_t(frame->vertices.size());
    const uint32_t index_base = uint32_t(frame->indices.size());
    frame->vertices.insert(frame->vertices.end(),
                           reinterpret_cast<const OverlayVertex*>(list->VtxBuffer.Data),
                           reinterpret_cast<const OverlayVertex*>(list->VtxBuffer.Data) + list->VtxBuffer.Size);
    frame->indices.insert(frame->indices.end(), list->IdxBuffer.Data, list->IdxBuffer.Data + list->IdxBuffer.Size);
    for (const ImDrawCmd& cmd : list->CmdBuffer) {
      // A callback is a backend-specific drawing hook (ImDrawCallback_ResetRenderState has nothing to reset here);
      // none of LARecomp's dialogs sets one.
      if (cmd.UserCallback || cmd.ElemCount == 0) {
        continue;
      }
      OverlayDraw draw;
      draw.count = cmd.ElemCount;
      draw.first_index = index_base + cmd.IdxOffset;
      draw.base_vertex = int32_t(vertex_base) + int32_t(cmd.VtxOffset);
      const ImTextureID texture = cmd.GetTexID();
      draw.texture_id = texture == ImTextureID_Invalid ? kOverlayNoTexture : uint32_t(texture);
      draw.scissor_left = cmd.ClipRect.x - origin.x;
      draw.scissor_top = cmd.ClipRect.y - origin.y;
      draw.scissor_right = cmd.ClipRect.z - origin.x;
      draw.scissor_bottom = cmd.ClipRect.w - origin.y;
      frame->draws.push_back(draw);
    }
  }
  channel_->Publish(std::move(frame));
}

void OverlayCapture::PublishEmpty() {
  channel_->Publish(channel_->AcquireFrame());
}

// ---------------------------------------------------------------------------
// OverlayHost
// ---------------------------------------------------------------------------

OverlayHost::OverlayHost(rex::ui::IUISystem& ui, std::shared_ptr<OverlayChannel> channel)
    : ui_(&ui), capture_(std::move(channel)) {}

void OverlayHost::RequestPass() {
  if (!connected_.load(std::memory_order_acquire) ||
      pass_pending_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  std::shared_ptr<OverlayHost> self = shared_from_this();
  // Dropped (with one warning from the UI system) once the UI system has stopped: pass_pending_ then stays set,
  // which is right, nothing asks again after Disconnect.
  ui_->PostTask([self] { self->RunPass(); });
}

void OverlayHost::RunPass() {
  if (connected_.load(std::memory_order_acquire)) {
    capture_.Begin();
    // On the UI thread RunSynchronous runs inline, so this builds the frame now and RenderOverlays (the capture)
    // follows on this thread; with no dialog open it returns without calling it.
    ui_->DrawOverlays(capture_);
    if (!capture_.called()) {
      // No dialog: the last frame that had one must not stay retained.
      capture_.PublishEmpty();
    }
  }
  pass_pending_.store(false, std::memory_order_release);
}

void OverlayHost::Disconnect() {
  // The UI system stops after OnStop/OnShutdown, where this runs; RunPass runs on that same thread, so a pass
  // either finished before this or sees connected_ false. The pointer itself is left alone: nothing reads it once
  // disconnected.
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
                                 plume::RenderFormat target_format, bool hdr) {
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
  if (!CreateShared() || !BuildTarget(target_format, hdr)) {
    Shutdown();
    return false;
  }
  UseTarget(hdr);
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
  triangle_pipeline_ = nullptr;
  for (Target& target : targets_) {
    target = Target{};
  }
  vertex_shader_.reset();
  layout_.reset();
  channel_.reset();
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
  upload_ = nullptr;
}

bool OverlayRenderer::CreateShared() {
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
  vertex_shader_ = device_->CreateShader(vs.data, vs.size, "main", vs.name);
  return layout_ && vertex_shader_;
}

bool OverlayRenderer::BuildTarget(plume::RenderFormat target_format, bool hdr) {
  Target& slot = targets_[hdr ? 1 : 0];
  if (slot.built) {
    return true;
  }
  if (!device_ || !layout_ || !vertex_shader_) {
    return false;
  }
  Target made;
  const HostShaderBlob ps = hdr ? OverlayHdrPixelShader() : OverlayPixelShader();
  made.pixel_shader = device_->CreateShader(ps.data, ps.size, "main", ps.name);
  if (!made.pixel_shader) {
    return false;
  }

  const plume::RenderInputSlot input_slot(0, sizeof(OverlayVertex));
  const plume::RenderInputElement input_elements[] = {
      plume::RenderInputElement("POSITION", 0, 0, plume::RenderFormat::R32G32_FLOAT, 0,
                                offsetof(OverlayVertex, x)),
      plume::RenderInputElement("TEXCOORD", 0, 1, plume::RenderFormat::R32G32_FLOAT, 0,
                                offsetof(OverlayVertex, u)),
      plume::RenderInputElement("COLOR", 0, 2, plume::RenderFormat::R8G8B8A8_UNORM, 0,
                                offsetof(OverlayVertex, color)),
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
  desc.pixelShader = made.pixel_shader.get();
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
  made.triangle_pipeline = device_->CreateGraphicsPipeline(desc, "overlay triangles");
  if (!made.triangle_pipeline) {
    return false;
  }
  made.built = true;
  slot = std::move(made);
  return true;
}

void OverlayRenderer::UseTarget(bool hdr) {
  Target& target = targets_[hdr ? 1 : 0];
  if (!target.built) {
    return;
  }
  hdr_ = hdr;
  triangle_pipeline_ = target.triangle_pipeline.get();
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
    // Can repeat every frame while the cause lasts: the first 8 are logged.
    static std::atomic<uint32_t> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
      REXLOG_ERROR("[gpu] {}: no upload memory for {} bytes", name, pitch * height);
    }
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
  // The same id again (ImGui's font atlas got new glyphs): the texture the frames in flight read is retired, and
  // the id names the new one from the next draw on.
  if (auto old = textures_.find(upload.id); old != textures_.end()) {
    ReleaseTexture(old->second);
    textures_.erase(old);
  }
  failed_textures_.erase(upload.id);

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

  const uint64_t vertex_bytes = frame.vertices.size() * sizeof(OverlayVertex);
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
    // Can repeat every frame while the cause lasts: the first 8 are logged.
    static std::atomic<uint32_t> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
      REXLOG_ERROR("[gpu] overlay frame dropped: no upload memory for {} + {} bytes", vertex_bytes,
                   index_bytes);
    }
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
  const plume::RenderInputSlot input_slot(0, sizeof(OverlayVertex));
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
  push.hdr_white = hdr_levels_.white;
  push.hdr_peak = hdr_levels_.peak;
  push.hdr_knee = hdr_levels_.knee;
  push.hdr_flags = hdr_levels_.flags;

  const plume::RenderPipeline* bound = nullptr;
  uint32_t recorded = 0;
  for (const OverlayDraw& draw : frame.draws) {
    if (!vertices || !indices) {
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
    const plume::RenderPipeline* pipeline = triangle_pipeline_;
    if (pipeline != bound) {
      list->setPipeline(pipeline);
      bound = pipeline;
    }
    list->setScissors(scissor);
    list->setGraphicsPushConstants(0, &push);
    list->drawIndexedInstanced(draw.count, 1, draw.first_index, draw.base_vertex, 0);
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
