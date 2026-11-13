// MCLA GPU renderer -- anti-aliasing of the composed scene, before the UI. See
// post.h.
//
// SMAA's lookup textures are the reference ones (thirdparty/smaa/Textures,
// AreaTex.h and SearchTex.h, MIT, Copyright (C) 2013 Jorge Jimenez, Jose I.
// Echevarria, Belen Masia, Fernando Navarro, Diego Gutierrez), uploaded as they
// are; the passes are the reference's three (thirdparty/smaa/SMAA.hlsl).

#include "post.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <chrono>
#include <cstring>
#include <format>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "descriptors.h"
#include "device.h"
#include "host_shaders.h"
#include "pass_timing.h"
#include "upload.h"

#include "smaa/AreaTex.h"
#include "smaa/SearchTex.h"

REXCVAR_DEFINE_STRING(
    mcla_gpu_aa, "off", "MCLA/GPU",
    "Anti-aliasing of the composed scene, before the UI (M6c): off, fxaa (FXAA "
    "3.11, Xenia's preset) or smaa (SMAA 1x, preset HIGH). The image the game resolves into the "
    "front buffer includes it (photos too). Live.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gpu {

namespace {

constexpr plume::RenderFormat kColorFormat = plume::RenderFormat::R8G8B8A8_UNORM;
constexpr plume::RenderFormat kEdgesFormat = plume::RenderFormat::R8G8_UNORM;
constexpr uint8_t kWriteRgb = uint8_t(plume::RenderColorWriteEnable::RED) |
                              uint8_t(plume::RenderColorWriteEnable::GREEN) |
                              uint8_t(plume::RenderColorWriteEnable::BLUE);

const char* ModeName(PostAa::Mode mode) {
  switch (mode) {
    case PostAa::Mode::kOff: return "off";
    case PostAa::Mode::kFxaa: return "fxaa";
    case PostAa::Mode::kSmaa: return "smaa";
  }
  return "?";
}

uint64_t PackKey(const EdramKey& k) {
  return uint64_t(k.base_tiles) | uint64_t(k.pitch) << 16 | uint64_t(k.depth) << 32 |
         uint64_t(k.format) << 40 | uint64_t(k.samples) << 48;
}

PostAa::Mode ReadMode() {
  std::string value = REXCVAR_GET(mcla_gpu_aa);
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  if (value == "fxaa") {
    return PostAa::Mode::kFxaa;
  }
  if (value == "smaa") {
    return PostAa::Mode::kSmaa;
  }
  return PostAa::Mode::kOff;
}

}  // namespace

PostAa::~PostAa() {
  Shutdown();
}

PostAa::Mode PostAa::FrameMode() {
  const Mode mode = ReadMode();
  if (mode != reported_mode_) {
    reported_mode_ = mode;
    REXLOG_INFO("[gpu] post: scene anti-aliasing {}", ModeName(mode));
  }
  return mode;
}

PostAa::Mode PostAa::CurrentMode() const {
  return ReadMode();
}

bool PostAa::Initialize(Device& device, BindlessHeap& heap, UploadRing& upload, HostPasses& passes,
                        EdramTargets& edram, PassTiming& pass_timing) {
  device_ = &device;
  heap_ = &heap;
  upload_ = &upload;
  passes_ = &passes;
  edram_ = &edram;
  pass_timing_ = &pass_timing;
  const HostShaderBlob luma = PostLumaPixelShader();
  const HostShaderBlob fxaa = FxaaPixelShader();
  const HostShaderBlob edges = SmaaPixelShader(1);
  const HostShaderBlob weights = SmaaPixelShader(2);
  const HostShaderBlob blend = SmaaPixelShader(3);
  luma_shader_ = passes.CreateShader(luma.data, luma.size, luma.name);
  fxaa_shader_ = passes.CreateShader(fxaa.data, fxaa.size, fxaa.name);
  smaa_edges_shader_ = passes.CreateShader(edges.data, edges.size, edges.name);
  smaa_weights_shader_ = passes.CreateShader(weights.data, weights.size, weights.name);
  smaa_blend_shader_ = passes.CreateShader(blend.data, blend.size, blend.name);
  if (!luma_shader_ || !fxaa_shader_ || !smaa_edges_shader_ || !smaa_weights_shader_ ||
      !smaa_blend_shader_) {
    return false;
  }
  // The pipelines of the mode that is on now, so that the first frame is smoothed as it was when
  // all five were made here. Nothing is made with the default (off): a mode switched on later
  // finds its pipelines made by the background build (PipelinesReady).
  if (const uint32_t pieces = ModePieces(ReadMode()); pieces != 0) {
    Pipelines made;
    if (MakePieces(pieces, &made) != 0) {
      return false;
    }
    Adopt(made);
  }
  // SMAA's lookups: made now, uploaded on first use (a list is needed).
  area_ = device.CreateTexture(plume::RenderTextureDesc::Texture2D(
                                   AREATEX_WIDTH, AREATEX_HEIGHT, 1, plume::RenderFormat::R8G8_UNORM),
                               "smaa area texture");
  search_ = device.CreateTexture(
      plume::RenderTextureDesc::Texture2D(SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1,
                                          plume::RenderFormat::R8_UNORM),
      "smaa search texture");
  if (!area_ || !search_) {
    return false;
  }
  plume::RenderTextureViewDesc area_view =
      plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::R8G8_UNORM);
  area_view.mipLevels = 1;
  area_view_ = device.CreateTextureView(*area_, area_view, "smaa area texture");
  plume::RenderTextureViewDesc search_view =
      plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::R8_UNORM);
  search_view.mipLevels = 1;
  search_view_ = device.CreateTextureView(*search_, search_view, "smaa search texture");
  if (!area_view_ || !search_view_) {
    return false;
  }
  area_slot_ = heap.AllocateTexture(*area_, *area_view_);
  search_slot_ = heap.AllocateTexture(*search_, *search_view_);
  if (area_slot_ == kInvalidBindlessSlot || search_slot_ == kInvalidBindlessSlot) {
    return false;
  }
  reported_mode_ = ReadMode();
  REXLOG_INFO("[gpu] post: scene anti-aliasing {} (mcla_gpu_aa, live)", ModeName(reported_mode_));
  return true;
}

void PostAa::Shutdown() {
  // The background build's job names the shaders below: it ends first.
  build_.Join();
  build_made_ = Pipelines{};
  build_pieces_ = 0;
  build_failed_pieces_ = 0;
  failed_pieces_ = 0;
  // The GPU is idle and the heap goes later: the slots are not handed back.
  copy_ = HostImage{};
  edges_ = HostImage{};
  weights_ = HostImage{};
  smaa_blend_pipeline_.reset();
  smaa_weights_pipeline_.reset();
  smaa_edges_pipeline_.reset();
  fxaa_pipeline_.reset();
  luma_pipeline_.reset();
  smaa_blend_shader_.reset();
  smaa_weights_shader_.reset();
  smaa_edges_shader_.reset();
  fxaa_shader_.reset();
  luma_shader_.reset();
  search_view_.reset();
  search_.reset();
  area_view_.reset();
  area_.reset();
  area_slot_ = kInvalidBindlessSlot;
  search_slot_ = kInvalidBindlessSlot;
  lookups_ready_ = false;
  device_ = nullptr;
  heap_ = nullptr;
  upload_ = nullptr;
  passes_ = nullptr;
  edram_ = nullptr;
  pass_timing_ = nullptr;
}

uint32_t PostAa::ModePieces(Mode mode) {
  switch (mode) {
    case Mode::kFxaa: return kLuma | kFxaa;
    case Mode::kSmaa: return kLuma | kEdges | kWeights | kBlend;
    case Mode::kOff: break;
  }
  return 0;
}

uint32_t PostAa::Missing(Mode mode) const {
  uint32_t missing = 0;
  const uint32_t pieces = ModePieces(mode);
  if ((pieces & kLuma) && !luma_pipeline_) missing |= kLuma;
  if ((pieces & kFxaa) && !fxaa_pipeline_) missing |= kFxaa;
  if ((pieces & kEdges) && !smaa_edges_pipeline_) missing |= kEdges;
  if ((pieces & kWeights) && !smaa_weights_pipeline_) missing |= kWeights;
  if ((pieces & kBlend) && !smaa_blend_pipeline_) missing |= kBlend;
  return missing;
}

uint32_t PostAa::MakePieces(uint32_t pieces, Pipelines* into) {
  uint32_t failed = 0;
  if (pieces & kLuma) {
    into->luma = passes_->CreatePipeline(*luma_shader_, kColorFormat, "aa copy");
    if (!into->luma) failed |= kLuma;
  }
  if (pieces & kFxaa) {
    into->fxaa = passes_->CreatePipeline(*fxaa_shader_, kColorFormat, kWriteRgb, "aa fxaa");
    if (!into->fxaa) failed |= kFxaa;
  }
  if (pieces & kEdges) {
    into->edges =
        passes_->CreatePipeline(*smaa_edges_shader_, kEdgesFormat, "aa smaa edges");
    if (!into->edges) failed |= kEdges;
  }
  if (pieces & kWeights) {
    into->weights =
        passes_->CreatePipeline(*smaa_weights_shader_, kColorFormat, "aa smaa weights");
    if (!into->weights) failed |= kWeights;
  }
  if (pieces & kBlend) {
    into->blend =
        passes_->CreatePipeline(*smaa_blend_shader_, kColorFormat, kWriteRgb, "aa smaa blend");
    if (!into->blend) failed |= kBlend;
  }
  return failed;
}

void PostAa::Adopt(Pipelines& made) {
  if (made.luma) luma_pipeline_ = std::move(made.luma);
  if (made.fxaa) fxaa_pipeline_ = std::move(made.fxaa);
  if (made.edges) smaa_edges_pipeline_ = std::move(made.edges);
  if (made.weights) smaa_weights_pipeline_ = std::move(made.weights);
  if (made.blend) smaa_blend_pipeline_ = std::move(made.blend);
}

bool PostAa::PipelinesReady(Mode mode) {
  if (build_.Collect()) {
    // The job ended: what it made joins the members, what it failed to make is not tried again.
    Adopt(build_made_);
    failed_pieces_ |= build_failed_pieces_;
    if (build_failed_pieces_ != 0) {
      REXLOG_ERROR("[gpu] post: the pipelines for scene anti-aliasing could not be made in the "
                   "background (Device logged why): {} stays off this session",
                   ModeName(mode));
    } else {
      REXLOG_INFO("[gpu] post: the background build made {} pipeline(s) for scene anti-aliasing "
                  "in {:.1f} ms",
                  std::popcount(build_pieces_), double(build_ns_) / 1.0e6);
    }
  }
  const uint32_t missing = Missing(mode);
  if (missing == 0) {
    return true;
  }
  if ((missing & failed_pieces_) != 0 || build_.Running()) {
    return false;
  }
  build_pieces_ = missing;
  build_failed_pieces_ = 0;
  build_ns_ = 0;
  REXLOG_INFO("[gpu] post: scene anti-aliasing {} was off at start: its {} pipeline(s) are made on "
              "a worker thread, the scene is not smoothed until they are",
              ModeName(mode), std::popcount(missing));
  build_.Start([this, missing] {
    const auto began = std::chrono::steady_clock::now();
    build_failed_pieces_ = MakePieces(missing, &build_made_);
    build_ns_ = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - began)
                             .count());
  });
  return false;
}

bool PostAa::Apply(plume::RenderCommandList* list, uint32_t slot, Mode mode,
                   EdramTargets::Target& target, uint32_t width, uint32_t height) {
  if (mode == Mode::kOff || !list || width == 0 || height == 0) {
    return false;
  }
  if (target.key.depth || target.key.samples != 1 || target.format != kColorFormat) {
    ++stats_.skipped_target;
    if (reported_targets_.insert(PackKey(target.key)).second) {
      REXLOG_ERROR("[gpu] post: the scene was composed into {}, not a single-sampled 8:8:8:8 "
                   "colour target; it is not anti-aliased (counted; reported once per target)",
                   DescribeEdramKey(target.key));
    }
    return false;
  }
  if (!PipelinesReady(mode)) {
    ++stats_.pending;
    return false;
  }
  const uint32_t w = target.scale.Edge(std::min(width, target.width));
  const uint32_t h = target.scale.Edge(std::min(height, target.height));
  uint32_t sample_slot = kInvalidBindlessSlot;
  uint32_t stencil_slot = kInvalidBindlessSlot;
  if (!edram_->SampleSlots(target, &sample_slot, &stencil_slot) ||
      !passes_->EnsureImage(copy_, w, h, kColorFormat, "aa copy")) {
    ++stats_.failed;
    return false;
  }
  if (mode == Mode::kSmaa &&
      ((!lookups_ready_ && !UploadLookups(list, slot)) ||
       !passes_->EnsureImage(edges_, w, h, kEdgesFormat, "aa smaa edges") ||
       !passes_->EnsureImage(weights_, w, h, kColorFormat, "aa smaa weights"))) {
    ++stats_.failed;
    return false;
  }
  // The scene, read where it was composed.
  edram_->Transition(list, target, plume::RenderTextureLayout::SHADER_READ);
  pass_timing_->Mark(list, "aa copy");
  const LumaPush luma{sample_slot};
  passes_->DrawInto(list, copy_, luma_pipeline_.get(), &luma, sizeof(luma));
  if (mode == Mode::kFxaa) {
    pass_timing_->Mark(list, "aa fxaa");
    const FxaaPush push{copy_.slot, 0, 1.0f / float(w), 1.0f / float(h)};
    DrawIntoTarget(list, target, w, h, fxaa_pipeline_.get(), &push, sizeof(push));
    ++stats_.fxaa;
    return true;
  }
  SmaaPush push{};
  push.metrics[0] = 1.0f / float(w);
  push.metrics[1] = 1.0f / float(h);
  push.metrics[2] = float(w);
  push.metrics[3] = float(h);
  pass_timing_->Mark(list, "aa smaa edges");
  push.source = copy_.slot;
  passes_->DrawInto(list, edges_, smaa_edges_pipeline_.get(), &push, sizeof(push), true);
  pass_timing_->Mark(list, "aa smaa weights");
  push.source = edges_.slot;
  push.area = area_slot_;
  push.search = search_slot_;
  passes_->DrawInto(list, weights_, smaa_weights_pipeline_.get(), &push, sizeof(push), true);
  pass_timing_->Mark(list, "aa smaa blend");
  push.source = copy_.slot;
  push.second = weights_.slot;
  DrawIntoTarget(list, target, w, h, smaa_blend_pipeline_.get(), &push, sizeof(push));
  ++stats_.smaa;
  return true;
}

void PostAa::DrawIntoTarget(plume::RenderCommandList* list, EdramTargets::Target& target,
                            uint32_t width, uint32_t height,
                            const plume::RenderPipeline* pipeline, const void* push,
                            uint32_t push_size) {
  edram_->Transition(list, target, plume::RenderTextureLayout::COLOR_WRITE);
  EdramTargets::Target* colors[1] = {&target};
  plume::RenderFramebuffer* framebuffer = edram_->Framebuffer(colors, 1, nullptr);
  if (!framebuffer) {
    ++stats_.failed;
    return;
  }
  list->setFramebuffer(framebuffer);
  const plume::RenderViewport viewport{0.0f, 0.0f, float(width), float(height)};
  list->setViewports(&viewport, 1);
  const plume::RenderRect scissor(0, 0, int32_t(width), int32_t(height));
  list->setScissors(&scissor, 1);
  passes_->Bind(list);
  list->setPipeline(pipeline);
  list->setGraphicsPushConstants(0, push, 0, push_size);
  list->drawInstanced(3, 1, 0, 0);
  list->setFramebuffer(nullptr);
}

bool PostAa::UploadLookups(plume::RenderCommandList* list, uint32_t slot) {
  // Rows padded to kTextureCopyPitchAlignment (upload.h): AreaTex has 320
  // bytes a row, SearchTex 64.
  constexpr uint32_t kAreaRowBytes = 512;
  constexpr uint32_t kSearchRowBytes = 256;
  static_assert(AREATEX_PITCH <= kAreaRowBytes && SEARCHTEX_PITCH <= kSearchRowBytes);
  const UploadSpan area_span = upload_->Allocate(slot, uint64_t(kAreaRowBytes) * AREATEX_HEIGHT,
                                                 kTextureCopyOffsetAlignment);
  const UploadSpan search_span = upload_->Allocate(
      slot, uint64_t(kSearchRowBytes) * SEARCHTEX_HEIGHT, kTextureCopyOffsetAlignment);
  if (!area_span || !search_span) {
    REXLOG_ERROR("[gpu] post: no upload memory for SMAA's lookup textures");
    return false;
  }
  for (uint32_t y = 0; y < AREATEX_HEIGHT; ++y) {
    std::memcpy(area_span.cpu + uint64_t(y) * kAreaRowBytes, areaTexBytes + y * AREATEX_PITCH,
                AREATEX_PITCH);
  }
  for (uint32_t y = 0; y < SEARCHTEX_HEIGHT; ++y) {
    std::memcpy(search_span.cpu + uint64_t(y) * kSearchRowBytes,
                searchTexBytes + y * SEARCHTEX_PITCH, SEARCHTEX_PITCH);
  }
  const plume::RenderTextureBarrier to_copy[] = {
      plume::RenderTextureBarrier(area_.get(), plume::RenderTextureLayout::COPY_DEST),
      plume::RenderTextureBarrier(search_.get(), plume::RenderTextureLayout::COPY_DEST)};
  list->barriers(plume::RenderBarrierStage::COPY, to_copy, 2);
  list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(area_.get()),
                          plume::RenderTextureCopyLocation::PlacedFootprint(
                              area_span.buffer, plume::RenderFormat::R8G8_UNORM, AREATEX_WIDTH,
                              AREATEX_HEIGHT, 1, kAreaRowBytes / 2, area_span.offset));
  list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(search_.get()),
                          plume::RenderTextureCopyLocation::PlacedFootprint(
                              search_span.buffer, plume::RenderFormat::R8_UNORM, SEARCHTEX_WIDTH,
                              SEARCHTEX_HEIGHT, 1, kSearchRowBytes, search_span.offset));
  const plume::RenderTextureBarrier to_read[] = {
      plume::RenderTextureBarrier(area_.get(), plume::RenderTextureLayout::SHADER_READ),
      plume::RenderTextureBarrier(search_.get(), plume::RenderTextureLayout::SHADER_READ)};
  list->barriers(plume::RenderBarrierStage::GRAPHICS, to_read, 2);
  lookups_ready_ = true;
  REXLOG_INFO("[gpu] post: SMAA lookup textures uploaded ({}x{} area, {}x{} search)",
              AREATEX_WIDTH, AREATEX_HEIGHT, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT);
  return true;
}

std::string PostAa::TakeSummary() {
  std::string text = std::format(
      "post: scene anti-aliasing {}: fxaa {}, smaa {}, target not 8:8:8:8 1x {}, failed {}, "
      "before its pipelines were made {}",
      ModeName(reported_mode_), stats_.fxaa, stats_.smaa, stats_.skipped_target, stats_.failed,
      stats_.pending);
  stats_ = Stats{};
  return text;
}

}  // namespace mcla::gpu
