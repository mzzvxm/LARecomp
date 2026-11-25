// MCLA GPU renderer -- the shared screen products: the extraction of the depth chain, the normal image and the bounce light's colour chain (SS-2). See screen_shared.h and screen_extract.h.
//
// This file is the AO's recording moved here: the passes are the AO's shaders (host_shaders.h AmbientOcclusionPixelShader's passes 0, 1, 2, 11, 12, 13, 27, 28), the layout is the AO's (the host's
// pass layout with a push block of 128 bytes), the push blocks are built by screen_extract.h from the AO's frame block, and the order and the pass timing labels are the AO's. What is different is
// who owns the images and the pipelines, and who calls: ao.cpp calls RecordDepth, RecordNormals and RecordColourPre at the places it recorded these passes from.

#include "screen_shared.h"

#include <algorithm>
#include <format>
#include <string>
#include <utility>

#include <rex/logging/log_category.h>

#include "descriptors.h"
#include "device.h"
#include "host_shaders.h"
#include "pass_timing.h"

namespace mcla::gpu {

namespace {

using screen::extract::Kind;

constexpr const char* kPrefix = "[screen]";

constexpr const char* kKindNames[size_t(Kind::kCount)] = {
    "screen depth", "screen depth ms", "screen depth mip", "screen normal", "screen normal ms", "screen colour chain", "screen colour chain ms", "screen colour chain mip",
    "screen depth + hiz", "screen depth ms + hiz", "screen depth mip + hiz",
};

// The format each pass writes (the AO's: ao.cpp MakePipeline).
plume::RenderFormat FormatOf(Kind kind) {
  switch (kind) {
    case Kind::kDepth:
    case Kind::kDepthMs: return plume::RenderFormat::R32_FLOAT;
    case Kind::kMip: return plume::RenderFormat::R16_FLOAT;
    case Kind::kNormal:
    case Kind::kNormalMs: return plume::RenderFormat::R16G16_UNORM;
    case Kind::kColour:
    case Kind::kColourMs:
    case Kind::kColourMip: return plume::RenderFormat::R16G16B16A16_FLOAT;
    case Kind::kDepthHiz:
    case Kind::kDepthMsHiz: return plume::RenderFormat::R32_FLOAT;
    case Kind::kMipHiz: return plume::RenderFormat::R16_FLOAT;
    case Kind::kCount: break;
  }
  return plume::RenderFormat::UNKNOWN;
}

uint64_t KeyOf(Kind kind) {
  // The Hi-Z is variant 0 and the colour capture's passes 1 to 3: the extraction's start at 16.
  return HostPrecache::Key(HostPrecache::kScreenShared, 16u + uint32_t(kind));
}

}  // namespace

// ---------------------------------------------------------------------------
// the layout, the shaders, the pipelines
// ---------------------------------------------------------------------------

void ScreenShared::InitializeChain(Device& device, BindlessHeap& heap) {
  device_ = &device;
  heap_ = &heap;
  chain_listed_ = 0;
  chain_failed_ = 0;
  // The host pass layout (host_passes.h) with a push block of 128 bytes instead of 64: the four texture sets, the sampler set, the push range at (b0, space4), for both stages, as the AO's own
  // (ao.cpp Initialize): the AO family's shaders declare AoPush there.
  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  for (uint32_t set = 0; set < 4; ++set) {
    builder.addDescriptorSet(heap.texture_set_builder());
  }
  builder.addDescriptorSet(heap.sampler_set_builder());
  builder.addPushConstant(0, 4, sizeof(AoPush), plume::RenderShaderStageFlag::VERTEX | plume::RenderShaderStageFlag::PIXEL);
  builder.end();
  wide_layout_ = device.CreatePipelineLayout(builder, "screen extract layout");
  bool ok = wide_layout_ != nullptr;
  for (uint32_t i = 0; i < screen::extract::kFirstFusedKind && ok; ++i) {
    const HostShaderBlob blob = AmbientOcclusionPixelShader(screen::extract::PassOf(Kind(i)));
    chain_shaders_[i] = passes_->CreateShader(blob.data, blob.size, blob.name);
    ok = chain_shaders_[i] != nullptr;
  }
  if (!ok) {
    for (auto& shader : chain_shaders_) {
      shader.reset();
    }
    wide_layout_.reset();
    chain_failed_ = screen::extract::kAllKinds;
    REXLOG_ERROR("{} the extraction's layout or a shader could not be made: the ambient occlusion's depth chain, normals and colour chain are recorded by its own passes this session", kPrefix);
    return;
  }
  // The fused passes (SS-3) are an addition: one that cannot be made leaves the Hi-Z to its own passes (the owner never fuses) and costs the extraction nothing.
  for (uint32_t i = screen::extract::kFirstFusedKind; i < uint32_t(Kind::kCount); ++i) {
    const HostShaderBlob blob = ScreenFusedDepthPixelShader(screen::extract::FusedPassOf(Kind(i)));
    chain_shaders_[i] = passes_->CreateShader(blob.data, blob.size, blob.name);
    if (!chain_shaders_[i]) {
      chain_failed_ |= screen::extract::Bit(Kind(i));
      REXLOG_ERROR("{} the fused depth pass '{}' could not be made: the Hi-Z's first levels are made by its own passes this session", kPrefix, kKindNames[i]);
    }
  }
}

void ScreenShared::ShutdownChain() {
  mrt_.clear();  // the GPU is idle (Shutdown): the framebuffers go with nothing to wait for
  for (auto& pipeline : chain_pipelines_) {
    pipeline.reset();
  }
  for (auto& shader : chain_shaders_) {
    shader.reset();
  }
  wide_layout_.reset();
  for (HostImage& image : depth_) {
    image = HostImage{};
  }
  depth_full_ = HostImage{};
  depth_prev_ = HostImage{};
  normal_ = HostImage{};
  normal_full_ = HostImage{};
  frame_depth0_ = nullptr;
  chain_listed_ = 0;
  chain_failed_ = 0;
  device_ = nullptr;
  heap_ = nullptr;
}

std::unique_ptr<plume::RenderPipeline> ScreenShared::MakeChainPipeline(Kind kind) {
  const size_t k = size_t(kind);
  if (!chain_shaders_[k] || !wide_layout_) {
    return nullptr;
  }
  if (screen::extract::IsFused(kind)) {
    // Two render targets: the depth chain's image and the Hi-Z's level of its size (R32_FLOAT).
    const plume::RenderFormat formats[2] = {FormatOf(kind), plume::RenderFormat::R32_FLOAT};
    return passes_->CreateMrtPipeline(*wide_layout_, *chain_shaders_[k], formats, 2, kKindNames[k]);
  }
  return passes_->CreatePipeline(*wide_layout_, *chain_shaders_[k], FormatOf(kind), plume::RenderBlendDesc::Copy(), 1, kKindNames[k]);
}

plume::RenderPipeline* ScreenShared::ChainPipeline(Kind kind) {
  const size_t k = size_t(kind);
  if (chain_pipelines_[k]) {
    return chain_pipelines_[k].get();
  }
  const uint32_t bit = screen::extract::Bit(kind);
  if ((chain_failed_ & bit) != 0 || !chain_shaders_[k] || !wide_layout_ || !passes_) {
    return nullptr;
  }
  if ((chain_listed_ & bit) == 0) {
    return nullptr;  // ChainReady lists it first
  }
  HostPrecache& precache = passes_->precache();
  if (!precache.Ready(KeyOf(kind))) {
    return nullptr;  // a frame never waits for a compile
  }
  chain_pipelines_[k] = precache.Acquire(KeyOf(kind), kKindNames[k], [this, kind] { return MakeChainPipeline(kind); });
  if (!chain_pipelines_[k]) {
    chain_failed_ |= bit;
    REXLOG_ERROR("{} the pipeline '{}' could not be made: no ambient occlusion in that configuration this session", kPrefix, kKindNames[k]);
  }
  return chain_pipelines_[k].get();
}

uint32_t ScreenShared::ListChainAtStart(uint32_t kinds) {
  if (!passes_ || !wide_layout_) {
    return 0;
  }
  HostPrecache& precache = passes_->precache();
  uint32_t added = 0;
  std::string names;
  for (uint32_t i = 0; i < uint32_t(Kind::kCount); ++i) {
    const Kind kind = Kind(i);
    const uint32_t bit = screen::extract::Bit(kind);
    if ((kinds & bit) == 0 || (chain_listed_ & bit) != 0 || (chain_failed_ & bit) != 0 || chain_pipelines_[i] || !chain_shaders_[i]) {
      continue;
    }
    chain_listed_ |= bit;  // ChainReady / ChainPipeline take it from the precache like one it listed itself
    if (precache.Add(KeyOf(kind), kKindNames[i], [this, kind] { return MakeChainPipeline(kind); })) {
      ++added;
      names += std::format("{}{}", names.empty() ? "" : ", ", kKindNames[i]);
    }
  }
  if (added != 0) {
    precache.Note("the shared screen products' depth and colour chain", added);
    REXLOG_INFO("{} the depth chain's and colour chain's pipelines the start's choices use are in the boot precache: {} ({})", kPrefix, added, names);
  }
  return added;
}

bool ScreenShared::ChainReady(uint32_t kinds) {
  if (!passes_ || !wide_layout_) {
    return false;
  }
  // The first frame that wants a pass lists it with the host precache (made on a worker of its own, the AO's way: HostPrecache::AddLive); the frames go without until it is made.
  std::vector<HostPrecache::Item> fresh;
  std::string names;
  for (uint32_t i = 0; i < uint32_t(Kind::kCount); ++i) {
    const Kind kind = Kind(i);
    const uint32_t bit = screen::extract::Bit(kind);
    if ((kinds & bit) == 0 || (chain_listed_ & bit) != 0 || (chain_failed_ & bit) != 0 || chain_pipelines_[i] || !chain_shaders_[i]) {
      continue;
    }
    chain_listed_ |= bit;
    names += std::format("{}{}", names.empty() ? "" : ", ", kKindNames[i]);
    fresh.push_back({KeyOf(kind), kKindNames[i], [this, kind] { return MakeChainPipeline(kind); }});
  }
  if (!fresh.empty()) {
    const uint32_t count = passes_->precache().AddLive("screen products", std::move(fresh));
    REXLOG_INFO("{} the depth chain, normal image or colour chain is wanted: {} pipelines ({}) are made in the background first, and the passes are recorded from the frame they are done", kPrefix,
                count, names);
  }
  bool ready = true;
  for (uint32_t i = 0; i < uint32_t(Kind::kCount); ++i) {
    if ((kinds & screen::extract::Bit(Kind(i))) != 0) {
      ready = ChainPipeline(Kind(i)) != nullptr && ready;
    }
  }
  return ready;
}

void ScreenShared::DrawWide(plume::RenderCommandList* list, HostImage& image, const plume::RenderPipeline* pipeline, const AoPush& push) {
  // The same sequence as AmbientOcclusion::DrawPass and HostPasses::DrawInto, with the 128 byte layout: the push block for the vertex and the pixel stage on both backends, the host layout's
  // texture sets and sampler set bound again.
  const bool first_use = image.layout == plume::RenderTextureLayout::UNKNOWN;
  if (image.layout != plume::RenderTextureLayout::COLOR_WRITE) {
    list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(image.texture.get(), plume::RenderTextureLayout::COLOR_WRITE));
    image.layout = plume::RenderTextureLayout::COLOR_WRITE;
  }
  list->setFramebuffer(image.framebuffer.get());
  if (first_use) {
    // A new colour target is initialised before it is drawn into (D3D12 debug layer id 1422).
    list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
  }
  const plume::RenderViewport viewport{0.0f, 0.0f, float(image.width), float(image.height)};
  list->setViewports(&viewport, 1);
  const plume::RenderRect scissor(0, 0, int32_t(image.width), int32_t(image.height));
  list->setScissors(&scissor, 1);
  list->setGraphicsPipelineLayout(wide_layout_.get());
  for (uint32_t set = 0; set < 4; ++set) {
    list->setGraphicsDescriptorSet(heap_->texture_set(), set);
  }
  list->setGraphicsDescriptorSet(heap_->sampler_set(), 4);
  list->setPipeline(pipeline);
  list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  list->drawInstanced(3, 1, 0, 0);
  list->setFramebuffer(nullptr);
  list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(image.texture.get(), plume::RenderTextureLayout::SHADER_READ));
  image.layout = plume::RenderTextureLayout::SHADER_READ;
}

plume::RenderFramebuffer* ScreenShared::MrtFramebufferFor(const HostImage& first, const HostImage& second) {
  // An entry stands while both images it names do: the serials (HostImage::serial) of the images that can be a target now are the live ones (a texture's address can be another texture's later).
  const auto live = [this](uint64_t serial) {
    if (serial == 0) {
      return false;
    }
    for (const HostImage& image : depth_) {
      if (image.serial == serial) return true;
    }
    if (depth_prev_.serial == serial) return true;
    for (const HostImage& image : hiz_) {
      if (image.serial == serial) return true;
    }
    return false;
  };
  for (size_t i = 0; i < mrt_.size();) {
    if (!live(mrt_[i].serial[0]) || !live(mrt_[i].serial[1])) {
      passes_->RetireFramebuffer(std::move(mrt_[i].framebuffer));
      mrt_.erase(mrt_.begin() + std::ptrdiff_t(i));
    } else {
      ++i;
    }
  }
  if (first.serial == 0 || second.serial == 0 || !first.texture || !second.texture) {
    return nullptr;
  }
  for (const MrtFramebuffer& entry : mrt_) {
    if (entry.serial[0] == first.serial && entry.serial[1] == second.serial) {
      return entry.framebuffer.get();
    }
  }
  const plume::RenderTexture* targets[2] = {first.texture.get(), second.texture.get()};
  MrtFramebuffer made;
  made.serial[0] = first.serial;
  made.serial[1] = second.serial;
  made.framebuffer = passes_->CreateMrtFramebuffer(targets, 2, "screen depth + hiz");
  if (!made.framebuffer) {
    if (!mrt_failed_logged_) {
      mrt_failed_logged_ = true;
      REXLOG_ERROR("{} the framebuffer of the fused depth pass could not be made: the Hi-Z's first levels are made by its own passes while that lasts (asked again every frame)", kPrefix);
    }
    return nullptr;
  }
  mrt_.push_back(std::move(made));
  return mrt_.back().framebuffer.get();
}

void ScreenShared::PurgeMrt() {
  if (!passes_) {
    mrt_.clear();
    return;
  }
  for (MrtFramebuffer& entry : mrt_) {
    passes_->RetireFramebuffer(std::move(entry.framebuffer));
  }
  mrt_.clear();
}

bool ScreenShared::DrawWideMrt(plume::RenderCommandList* list, HostImage& first, HostImage& second, const plume::RenderPipeline* pipeline, const AoPush& push) {
  if (!first.texture || !second.texture || first.width != second.width || first.height != second.height) {
    return false;
  }
  plume::RenderFramebuffer* framebuffer = MrtFramebufferFor(first, second);
  if (!framebuffer) {
    return false;
  }
  // DrawWide's sequence with two targets: both in the colour write layout, a new one cleared before it is drawn into (D3D12 debug layer id 1422), the push block for both stages, both left readable.
  const bool first_use[2] = {first.layout == plume::RenderTextureLayout::UNKNOWN, second.layout == plume::RenderTextureLayout::UNKNOWN};
  plume::RenderTextureBarrier before[2];
  uint32_t moves = 0;
  if (first.layout != plume::RenderTextureLayout::COLOR_WRITE) {
    before[moves++] = plume::RenderTextureBarrier(first.texture.get(), plume::RenderTextureLayout::COLOR_WRITE);
    first.layout = plume::RenderTextureLayout::COLOR_WRITE;
  }
  if (second.layout != plume::RenderTextureLayout::COLOR_WRITE) {
    before[moves++] = plume::RenderTextureBarrier(second.texture.get(), plume::RenderTextureLayout::COLOR_WRITE);
    second.layout = plume::RenderTextureLayout::COLOR_WRITE;
  }
  if (moves != 0) {
    list->barriers(plume::RenderBarrierStage::GRAPHICS, before, moves);
  }
  list->setFramebuffer(framebuffer);
  for (uint32_t target = 0; target < 2; ++target) {
    if (first_use[target]) {
      list->clearColor(target, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
    }
  }
  const plume::RenderViewport viewport{0.0f, 0.0f, float(first.width), float(first.height)};
  list->setViewports(&viewport, 1);
  const plume::RenderRect scissor(0, 0, int32_t(first.width), int32_t(first.height));
  list->setScissors(&scissor, 1);
  list->setGraphicsPipelineLayout(wide_layout_.get());
  for (uint32_t set = 0; set < 4; ++set) {
    list->setGraphicsDescriptorSet(heap_->texture_set(), set);
  }
  list->setGraphicsDescriptorSet(heap_->sampler_set(), 4);
  list->setPipeline(pipeline);
  list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  list->drawInstanced(3, 1, 0, 0);
  list->setFramebuffer(nullptr);
  const plume::RenderTextureBarrier after[2] = {plume::RenderTextureBarrier(first.texture.get(), plume::RenderTextureLayout::SHADER_READ),
                                                plume::RenderTextureBarrier(second.texture.get(), plume::RenderTextureLayout::SHADER_READ)};
  list->barriers(plume::RenderBarrierStage::GRAPHICS, after, 2);
  first.layout = plume::RenderTextureLayout::SHADER_READ;
  second.layout = plume::RenderTextureLayout::SHADER_READ;
  return true;
}

// ---------------------------------------------------------------------------
// the images
// ---------------------------------------------------------------------------

bool ScreenShared::EnsureDepth(const screen::extract::Frame& frame) {
  if (!passes_) {
    return false;
  }
  // The names, formats and sizes are the AO's own (they were its images): the log, the memory tools and the census read the same.
  bool ok = passes_->EnsureImage(depth_[0], frame.traced[0], frame.traced[1], plume::RenderFormat::R32_FLOAT, "ao depth");
  if (ok && frame.levels) {
    ok = passes_->EnsureImage(depth_[1], frame.level[0][0], frame.level[0][1], plume::RenderFormat::R16_FLOAT, "ao depth 1/2") &&
         passes_->EnsureImage(depth_[2], frame.level[1][0], frame.level[1][1], plume::RenderFormat::R16_FLOAT, "ao depth 1/4");
  } else if (!frame.levels) {
    // Only the contact shadows run: they read level 0. What an occlusion that has been turned off left (two coarser levels, about 5 MB at 4K) goes back.
    for (uint32_t i = 1; i < screen::kDepthLevels; ++i) {
      if (depth_[i].texture) {
        passes_->ReleaseImage(depth_[i]);
      }
    }
  }
  // The contact shadows' own full size depth while the occlusion is traced at half size and they are not (Plan::SplitContact, which implies the apply).
  if (ok && frame.full) {
    ok = passes_->EnsureImage(depth_full_, frame.render[0], frame.render[1], plume::RenderFormat::R32_FLOAT, "ao contact depth");
  } else if (!frame.full && depth_full_.texture) {
    passes_->ReleaseImage(depth_full_);
  }
  return ok;
}

bool ScreenShared::EnsureDepthHistory(uint32_t width, uint32_t height) {
  return passes_ && passes_->EnsureImage(depth_prev_, width, height, plume::RenderFormat::R32_FLOAT, "ssgi depth history");
}

void ScreenShared::ReleaseDepth() {
  if (!passes_) {
    return;
  }
  PurgeMrt();
  for (HostImage& image : depth_) {
    passes_->ReleaseImage(image);
  }
  passes_->ReleaseImage(depth_full_);
  frame_depth0_ = nullptr;
}

void ScreenShared::ReleaseDepthHistory() {
  if (passes_) {
    passes_->ReleaseImage(depth_prev_);
  }
}

bool ScreenShared::EnsureNormal(uint32_t width, uint32_t height) {
  return passes_ && passes_->EnsureImage(normal_, width, height, plume::RenderFormat::R16G16_UNORM, "ao normals");
}

bool ScreenShared::EnsureNormalFull(uint32_t width, uint32_t height) {
  return passes_ && passes_->EnsureImage(normal_full_, width, height, plume::RenderFormat::R16G16_UNORM, "ao contact normals");
}

void ScreenShared::ReleaseNormalFull() {
  if (passes_ && normal_full_.texture) {
    passes_->ReleaseImage(normal_full_);
  }
}

void ScreenShared::ReleaseNormals() {
  if (!passes_) {
    return;
  }
  passes_->ReleaseImage(normal_);
  passes_->ReleaseImage(normal_full_);
}

void ScreenShared::EndDepthChain(bool trade) {
  if (trade) {
    // The depth image of this frame is the next frame's depth history: the two trade places now that nothing of this frame reads level 0 again (the next frame's depth pass writes the other).
    std::swap(depth_[0], depth_prev_);
  }
  frame_depth0_ = trade ? &depth_prev_ : &depth_[0];
}

// ---------------------------------------------------------------------------
// the passes
// ---------------------------------------------------------------------------

uint32_t ScreenShared::RecordDepthFused(plume::RenderCommandList* list, const screen::extract::Frame& frame, const AoPush& level0, plume::RenderPipeline* mip, uint32_t stencil_slot, bool source_bits,
                                        bool multisampled) {
  // SS-3: the Hi-Z's first levels are written by the depth chain's own passes (screen_extract.h "the fused Hi-Z"). The depth chain's levels 0 to 2 are recorded HERE when this returns more than 0, and the
  // caller records them when it returns 0. What the frame needs: the Hi-Z wanted, the setting, the fused pipelines made and the Hi-Z's own pass (it records the levels above the fused ones), and the
  // images; anything missing and the plain passes are recorded (and Publish makes the whole chain the separate way).
  using screen::extract::Kind;
  if (!hiz_fused_ || (plan_.make & screen::ProductBit(screen::kHizMin)) == 0) {
    return 0;
  }
  const uint32_t hiz_levels = screen::HizLevelCount(frame.traced[0], frame.traced[1]);
  const uint32_t fused = screen::extract::FusedHizLevels(frame, hiz_levels);
  if (fused == 0) {
    return 0;
  }
  const Kind depth_kind = multisampled ? Kind::kDepthMsHiz : Kind::kDepthHiz;
  if (!ChainReady(screen::extract::Bit(depth_kind) | (fused >= 2 ? screen::extract::Bit(Kind::kMipHiz) : 0u))) {
    return 0;  // listed with the host precache, made in the background: the frames go without
  }
  plume::RenderPipeline* tail = HizPipeline();
  uint32_t levels = 0;
  if (!tail || !EnsureHizImages(frame.traced[0], frame.traced[1], &levels) || levels != hiz_levels) {
    return 0;
  }
  plume::RenderPipeline* depth_hiz = ChainPipeline(depth_kind);
  plume::RenderPipeline* mip_hiz = fused >= 2 ? ChainPipeline(Kind::kMipHiz) : nullptr;
  if (!depth_hiz || (fused >= 2 && !mip_hiz)) {
    return 0;
  }
  // Level 0: the depth pass with the Hi-Z's level 0 beside it. When the draw cannot be made the caller records the plain passes and the Hi-Z is not started.
  if (!DrawWideMrt(list, depth_[0], hiz_[0], depth_hiz, screen::extract::DepthHizPush(level0, frame, stencil_slot, source_bits))) {
    return 0;
  }
  uint32_t made = 1;
  if (frame.levels) {
    // Level 1: the mean of the four level 0 texels and the Hi-Z's level 1 of the same four (slots.w = slots.z: one set of loads); level 2: the mean of the four level 1 texels and the Hi-Z's level 2 of
    // the Hi-Z's level 1. A draw that cannot be made is the plain pass (and the Hi-Z goes on from the level below it with its own pass).
    const AoPush mip1 = screen::extract::DepthMipHizPush(level0, frame, 1, depth_[0].slot, depth_[0].slot);
    if (fused >= 2 && DrawWideMrt(list, depth_[1], hiz_[1], mip_hiz, mip1)) {
      made = 2;
    } else {
      DrawWide(list, depth_[1], mip, screen::extract::DepthMipPush(level0, frame, 1, depth_[0].slot));
    }
    const AoPush mip2 = screen::extract::DepthMipHizPush(level0, frame, 2, depth_[1].slot, hiz_[1].slot);
    if (made == 2 && fused >= 3 && DrawWideMrt(list, depth_[2], hiz_[2], mip_hiz, mip2)) {
      made = 3;
    } else {
      DrawWide(list, depth_[2], mip, screen::extract::DepthMipPush(level0, frame, 2, depth_[1].slot));
    }
  }
  // The levels above, behind the passes that made the ones below them (the label 'screen hiz' stands for them, as it did for the whole chain).
  if (made < hiz_levels) {
    pass_timing_->Mark(list, "screen hiz");
    RecordHizPasses(list, nullptr, made, hiz_levels, tail);
  }
  LogHiz(hiz_levels, frame.traced[0], frame.traced[1]);
  return hiz_levels;
}

void ScreenShared::RecordDepth(plume::RenderCommandList* list, const screen::extract::Frame& frame, const AoPush& base, uint32_t stencil_slot, bool source_bits, bool multisampled) {
  plume::RenderPipeline* depth = ChainPipeline(multisampled ? Kind::kDepthMs : Kind::kDepth);  // made: the AO asked ChainReady before it took the frame
  plume::RenderPipeline* mip = frame.levels ? ChainPipeline(Kind::kMip) : nullptr;
  if (!depth || (frame.levels && !mip) || !list) {
    return;
  }
  pass_timing_->Mark(list, "ao depth");
  // The depth target's stencil plane (the vehicles' IDs): the pass marks the vehicles' pixels. At half size with the G-buffer's normals the pass also records which sample of each 2x2 block it
  // took (slots2.w; hlsl/ao_depth_ps.hlsl, ao_gbuffer_normal.hlsli).
  const AoPush level0 = screen::extract::DepthPush(base, frame, stencil_slot, source_bits);
  // SS-3: with the Hi-Z wanted the same passes also write its first levels, and the levels above them follow (RecordDepthFused records the depth chain's levels 0 to 2 itself then).
  hiz_made_ = RecordDepthFused(list, frame, level0, mip, stencil_slot, source_bits, multisampled);
  if (hiz_made_ == 0) {
    DrawWide(list, depth_[0], depth, level0);
    if (frame.levels) {
      // The coarser levels.
      DrawWide(list, depth_[1], mip, screen::extract::DepthMipPush(level0, frame, 1, depth_[0].slot));
      DrawWide(list, depth_[2], mip, screen::extract::DepthMipPush(level0, frame, 2, depth_[1].slot));
    }
  }
  if (frame.full) {
    // The occlusion is traced at half size and the contact shadows keep the picture's: their march and filter read this full size linear depth (the depth pass again, without the half size
    // flag; the vehicle bit is in it as in level 0).
    pass_timing_->Mark(list, "ao contact depth");
    DrawWide(list, depth_full_, depth, screen::extract::DepthFullPush(base, frame, stencil_slot));
  }
}

void ScreenShared::RecordNormals(plume::RenderCommandList* list, const screen::extract::Frame& frame, const AoPush& base, const screen::extract::NormalRecipe& recipe) {
  plume::RenderPipeline* pipeline = ChainPipeline(recipe.plane_samples > 1 ? Kind::kNormalMs : Kind::kNormal);  // made: the AO asked ChainReady when it planned the normals
  if (!pipeline || !list) {
    return;
  }
  // The traced size image, from the depth chain's level 0 (it carries the vehicle mark and, at half size, the source of each pixel). Push fields: hlsl/ao_normal_ps.hlsl.
  pass_timing_->Mark(list, "ao normals");
  const AoPush push = screen::extract::NormalPush(base, frame, recipe, depth_[0].slot);
  DrawWide(list, normal_, pipeline, push);
  if (frame.full && normal_full_.texture) {
    // The contact shadows keep the picture's size beside a half size occlusion, with their own depth: their normal image is from that (no half size block to choose from).
    pass_timing_->Mark(list, "ao contact normals");
    DrawWide(list, normal_full_, pipeline, screen::extract::NormalFullPush(push, frame, depth_full_.slot));
  }
}

void ScreenShared::RecordColourPre(plume::RenderCommandList* list, const screen::extract::Frame& frame, const AoPush& base, EdramTargets::Target& colour,
                                   const screen::extract::ColourRecipe& recipe) {
  const bool ms = colour.host_samples > 1;
  plume::RenderPipeline* level0 = ChainPipeline(ms ? Kind::kColourMs : Kind::kColour);
  plume::RenderPipeline* mip = ChainPipeline(Kind::kColourMip);
  if (!level0 || !mip || !list) {
    return;
  }
  // The colour target is read as a texture: after the opaque draws it is in its write layout; the apply pass (or the game's next draw) moves it back.
  edram_->Transition(list, colour, plume::RenderTextureLayout::SHADER_READ);
  pass_timing_->Mark(list, "ssgi colour chain");
  // Level 0: the colour target's mean over the pixels and samples a texel covers, each sample capped at a multiple of the frame's ambient + sun (ao_color_ps.hlsl).
  DrawWide(list, colour_pre_[0], level0, screen::extract::ColourLevel0Push(base, frame, recipe));
  // The coarser levels, one pass each, as the depth chain's.
  DrawWide(list, colour_pre_[1], mip, screen::extract::ColourMipPush(base, frame, 1, colour_pre_[0].slot));
  DrawWide(list, colour_pre_[2], mip, screen::extract::ColourMipPush(base, frame, 2, colour_pre_[1].slot));
  pre_made_ = true;
}

}  // namespace mcla::gpu
