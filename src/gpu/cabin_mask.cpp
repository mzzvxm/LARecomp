#include "cabin_mask.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

#include <rex/logging/log_category.h>

#include "car_shadow_map_rules.h"
#include "descriptors.h"
#include "mirror_overlay_rules.h"
#include "device.h"
#include "pass_timing.h"

#if defined(LARECOMP_GPU_HAS_D3D12)
#include "car_shadow_map_vs.hlsl.dxil.h"
#include "cabin_mask_ps.hlsl.dxil.h"
#endif
#if defined(LARECOMP_GPU_HAS_VULKAN)
#include "car_shadow_map_vs.hlsl.spirv.h"
#include "cabin_mask_ps.hlsl.spirv.h"
#endif
#include "host_shaders.h"

namespace mcla::gpu {
namespace {
namespace cab = cabin_mirror;
namespace sm = car_shadow_map;
constexpr auto kFormat = plume::RenderFormat::R8_UNORM;
constexpr uint32_t kPushBytes = 4 * 4 * sizeof(float);

uint32_t Variant(const car_frame::Draw& d) {
  const DrawGeometry& g = d.geometry;
  sm::PositionKind kind = sm::PositionKind::kNone;
  switch (d.position_format) {
    case plume::RenderFormat::R32G32B32_FLOAT:
    case plume::RenderFormat::R32G32B32A32_FLOAT: kind = sm::PositionKind::kFloat3; break;
    case plume::RenderFormat::R16G16B16A16_FLOAT: kind = sm::PositionKind::kHalf4; break;
    default: break;
  }
  if (g.count == 0 || d.position_slot >= std::min(g.slot_count, DrawGeometry::kMaxSlots) ||
      (g.topology != plume::RenderPrimitiveTopology::TRIANGLE_LIST && g.topology != plume::RenderPrimitiveTopology::TRIANGLE_STRIP) ||
      (g.indexed && !g.index_view.buffer.ref) || d.position_stride >= mirror_overlay::kMaxStride) return 0;
  const auto& vb = g.vertex_views[d.position_slot];
  return vb.buffer.ref && sm::PositionUsable(kind, d.position_offset, d.position_stride, vb.size)
      ? sm::VariantKey(kind, uint32_t(g.topology), d.position_stride) : 0;
}

double PoseDrift(const temporal::Mat4& a, const temporal::Mat4& b) {
  double drift = 0.0;
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 3; ++c) {
      drift = std::max(drift, std::abs(a.m[r][c] - b.m[r][c]));
    }
  }
  return drift;
}
}  // namespace

void CabinMask::Shutdown() {
  // MirrorOverlay stops the shared pipeline builder before calling here.
  if (passes_) passes_->ReleaseImage(image_);
  pipelines_.clear();
  vertex_.reset();
  pixel_.reset();
  layout_.reset();
  parts_.clear();
  cached_parts_.clear();
  device_ = nullptr;
  passes_ = nullptr;
  last_glass_ = 0;
  failed_ = false;
}

bool CabinMask::Prepare(Device& device, HostPasses& passes) {
  if (failed_) return false;
  if (layout_) return true;
  device_ = &device;
  passes_ = &passes;
  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(false, true);
  builder.addPushConstant(0, 0, kPushBytes, plume::RenderShaderStageFlag::VERTEX);
  builder.end();
  layout_ = device.CreatePipelineLayout(builder, "cabin mask layout");
  const HostShaderBlob vs = MCLA_GPU_HOST_SHADER_BLOB(car_shadow_map_vs);
  const HostShaderBlob ps = MCLA_GPU_HOST_SHADER_BLOB(cabin_mask_ps);
  vertex_ = device.CreateShader(vs.data, vs.size, "main", vs.name);
  pixel_ = device.CreateShader(ps.data, ps.size, "main", ps.name);
  failed_ = !layout_ || !vertex_ || !pixel_;
  if (failed_) REXLOG_ERROR("[cabin mask] the shaders or layout could not be created");
  return !failed_;
}

std::unique_ptr<plume::RenderPipeline> CabinMask::MakePipeline(uint32_t variant) const {
  if (!device_ || !layout_ || !vertex_ || !pixel_) return {};
  const plume::RenderInputSlot slot(0, sm::VariantStride(variant));
  const plume::RenderInputElement element("POSITION", 0, 0,
      sm::VariantKind(variant) == sm::PositionKind::kHalf4 ? plume::RenderFormat::R16G16B16A16_FLOAT : plume::RenderFormat::R32G32B32_FLOAT, 0, 0);
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = layout_.get();
  desc.vertexShader = vertex_.get();
  desc.pixelShader = pixel_.get();
  desc.inputSlots = &slot;
  desc.inputSlotsCount = 1;
  desc.inputElements = &element;
  desc.inputElementsCount = 1;
  desc.primitiveTopology = plume::RenderPrimitiveTopology(sm::VariantTopology(variant));
  desc.depthClipEnabled = true;
  desc.multisampling.sampleCount = plume::RenderSampleCount::COUNT_1;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.fillMode = plume::RenderFillMode::SOLID;
  desc.renderTargetCount = 1;
  desc.renderTargetFormat[0] = kFormat;
  desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
  return device_->CreateGraphicsPipeline(desc, "cabin mask");
}

plume::RenderPipeline* CabinMask::Pipeline(uint32_t variant) {
  if (!passes_) return nullptr;
  HostPrecache& builder = passes_->precache();
  const uint64_t key = HostPrecache::Key(HostPrecache::kMirrorOverlay, variant | 0x80000000u);
  const auto [it, new_variant] = pipelines_.try_emplace(variant);
  auto& entry = it->second;
  if (entry.resolved) return entry.pipeline.get();
  const auto make = [this, variant] { return MakePipeline(variant); };
  if (new_variant) {
    builder.AddLive("cabin mask", {{key, std::format("cabin mask position variant {}", variant), make}});
  } else if (builder.Ready(key)) {
    entry.pipeline = builder.Acquire(key, std::format("cabin mask position variant {}", variant), make);
    entry.resolved = true;
    if (!entry.pipeline) REXLOG_ERROR("[cabin mask] position variant {} could not be created", variant);
  }
  return entry.pipeline.get();
}

CabinMask::Result CabinMask::Record(plume::RenderCommandList& list, Device& device, HostPasses& passes, PassTiming& timing,
                                  const car_frame::Frame& frame, const car_frame::Draw& mirror, const mirror_glass::Result& glass) {
  temporal::Mat4 view_to_world, world_to_mirror;
  if (!temporal::Inverse(frame.view, &view_to_world) || !temporal::Inverse(mirror.model_to_world, &world_to_mirror)) {
    return {kInvalidBindlessSlot, "mask transform unavailable"};
  }
  const cab::V3 eye = cab::Point(cab::Point({}, view_to_world), world_to_mirror);
  const cab::V3 centre = glass.cabin_centre_model, normal = glass.cabin_normal_model;
  const cab::V3 virtual_eye = eye + cab::VirtualOffset(eye, centre, normal);
  if (!(cab::Dot(centre - virtual_eye, normal) > 0.02)) return {kInvalidBindlessSlot, "mask eye on or behind glass"};

  // CHECK: verify rear window, pillars and headrests are submitted in the
  // cockpit draw list on both APIs. Replay the submitted rigid LOD geometry
  // without materials or depth.
  parts_.clear();
  for (size_t i = 0; i < frame.draws.size(); ++i) {
    const auto& d = frame.draws[i];
    if (&d == &mirror || d.part != car_frame::Part::kBody ||
        !cab::IsBlocker(d.ps, mirror_glass::ClassOfShader(d.ps), (d.depth_word & 2u) != 0)) continue;
    const uint32_t variant = Variant(d);
    if (variant == 0) continue;
    const auto to_mirror = temporal::Mul(d.model_to_world, world_to_mirror);
    if (temporal::Finite(to_mirror)) parts_.push_back({i, d.mesh_key, variant, to_mirror});
  }

  bool changed = parts_.size() != cached_parts_.size();
  double motion = cab::Length(eye - last_eye_);
  for (size_t i = 0; !changed && i < parts_.size(); ++i) {
    const auto& p = parts_[i];
    const auto& previous = cached_parts_[i];
    changed = p.mesh != previous.mesh || p.variant != previous.variant;
    motion = std::max(motion, PoseDrift(p.to_mirror, previous.to_mirror));
  }
  const auto now = std::chrono::steady_clock::now();
  const bool valid = last_glass_ == mirror.mesh_key && image_.slot != kInvalidBindlessSlot;
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - updated_).count();
  if (!cab::Refresh(valid, changed, motion, elapsed)) return {image_.slot, nullptr};

  if (!Prepare(device, passes)) return {kInvalidBindlessSlot, "mask shaders or layout unavailable"};
  bool pending = false;
  for (const auto& p : parts_) {
    if (!Pipeline(p.variant)) {
      if (pipelines_.at(p.variant).resolved) return {kInvalidBindlessSlot, "mask pipeline failed"};
      pending = true;
    }
  }
  if (pending) return {kInvalidBindlessSlot, "mask pipeline pending"};
  if (!passes.EnsureImage(image_, cab::kMaskWidth, cab::kMaskHeight, kFormat, "cabin mask") || !image_.texture || !image_.framebuffer) {
    return {kInvalidBindlessSlot, "mask target unavailable"};
  }

  const auto projection = cab::MaskProjection(glass.aperture, centre, normal, virtual_eye);
  // CHECK: measure the refresh separately from the cached overlay on D3D12 and Vulkan.
  timing.Mark(&list, "cabin mask");
  list.barriers(plume::RenderBarrierStage::GRAPHICS,
                plume::RenderTextureBarrier(image_.texture.get(), plume::RenderTextureLayout::COLOR_WRITE));
  image_.layout = plume::RenderTextureLayout::COLOR_WRITE;
  list.setFramebuffer(image_.framebuffer.get());
  list.clearColor(0, plume::RenderColor(0, 0, 0, 0));
  const plume::RenderViewport viewport(0, 0, float(cab::kMaskWidth), float(cab::kMaskHeight));
  const plume::RenderRect scissor(0, 0, int32_t(cab::kMaskWidth), int32_t(cab::kMaskHeight));
  list.setViewports(&viewport, 1);
  list.setScissors(&scissor, 1);
  list.setGraphicsPipelineLayout(layout_.get());
  plume::RenderPipeline* bound = nullptr;
  for (const auto& p : parts_) {
    const auto& d = frame.draws[p.draw];
    const auto& g = d.geometry;
    auto* pipeline = pipelines_.at(p.variant).pipeline.get();
    if (bound != pipeline) {
      list.setPipeline(pipeline);
      bound = pipeline;
    }
    const auto clip = temporal::Mul(p.to_mirror, projection);
    float rows[4][4];
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) rows[r][c] = float(clip.m[r][c]);
    }
    list.setGraphicsPushConstants(0, rows, 0, kPushBytes);
    plume::RenderVertexBufferView vb = g.vertex_views[d.position_slot];
    vb.buffer.offset += d.position_offset;
    vb.size -= d.position_offset;
    const plume::RenderInputSlot slot(0, d.position_stride);
    list.setVertexBuffers(0, &vb, 1, &slot);
    if (g.indexed) {
      list.setIndexBuffer(&g.index_view);
      list.drawIndexedInstanced(g.count, 1, g.first, g.base_vertex, 0);
    } else {
      list.drawInstanced(g.count, 1, g.first, 0);
    }
  }
  list.setFramebuffer(nullptr);
  list.barriers(plume::RenderBarrierStage::GRAPHICS,
                plume::RenderTextureBarrier(image_.texture.get(), plume::RenderTextureLayout::SHADER_READ));
  image_.layout = plume::RenderTextureLayout::SHADER_READ;
  timing.Mark(&list, "after cabin mask");
  if (!valid || changed) REXLOG_INFO("[cabin mask] refreshed {} x {} from {} rigid draws", cab::kMaskWidth, cab::kMaskHeight, parts_.size());
  last_glass_ = mirror.mesh_key;
  last_eye_ = eye;
  updated_ = now;
  cached_parts_.swap(parts_);
  return {image_.slot, nullptr};
}

}  // namespace mcla::gpu
