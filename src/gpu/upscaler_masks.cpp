// MCLA GPU renderer -- the masks for the temporal upscalers. See upscaler_masks.h.

#include "upscaler_masks.h"

#include <algorithm>
#include <format>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "host_shaders.h"
#include "pass_timing.h"
#include "pipelines.h"
#include "texture_bytes.h"

REXCVAR_DEFINE_INT32(
    mcla_gpu_upscaler_mask_test, 0, "MCLA/GPU",
    "Diagnostic: tests whether the upscalers read their mask inputs. 1 gives the reactive mask (what "
    "FSR calls reactive; DLSS's bias-current-colour and reactive hints) every pixel at 1, 2 gives the "
    "transparency and composition mask every pixel at 1, 3 both. An upscaler that reads a mask then "
    "stops trusting its history: the picture loses its smoothness over time (edges crawl, thin lines "
    "flicker); one that does not read it looks as before. Only while FSR or DLSS runs. 0: the real "
    "masks. Live.")
    .range(0, 3)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[gpu] upscaler masks:";
constexpr plume::RenderFormat kCoverageFormat = plume::RenderFormat::R8G8B8A8_UNORM;
constexpr plume::RenderFormat kMaskFormat = plume::RenderFormat::R8_UNORM;

using Kind = UpscalerMasks::Kind;

// One pixel shader the masks take, with the vertex shader it must be drawn with (0: any). Every value
// is a LARecomp identity (native_gfx/shader_identity.h), read off the shipped shader containers with
// the pack builder's walk; the comment names the shader as the HLSL tree's report does: group, entry
// point. The set is per pixel shader because a vertex shader only says how the vertices are fetched.
// CHECK: each of these in the real game (the census patch lists what draws, blended, in the main view).
struct Entry {
  uint64_t ps;
  uint64_t vs;
  Kind kind;
};

constexpr Entry kEntries[] = {
    // --- the cars' shadow decals (GHOSTING-DESIGN.md 9.4, the census's pairs) -------------------
    // xAmbientOcclusionShadows PS_CalcShadowsQuad / VS_DrawQuad: the contact shadow of a car.
    {0x0252424701F07592ull, 0xA829AE1437B66C8Bull, Kind::kShadowDecal},
    // xAmbientOcclusionShadows PS_CalcShadowsSingle / VS_DrawSingle: the four wheel patches.
    {0xB4C34BD40E64FB0Full, 0x72F466E81A37357Eull, Kind::kShadowDecal},

    // --- particles: tire smoke, smoke, sparks, dust, checkpoint rings, rain, lightning ----------
    // xmc4_rmptfx_column PS_Draw
    {0xD63FBF07FDD50D51ull, 0, Kind::kParticle},
    // xmc4_rmptfx_default PS_Draw
    {0xFE82C316D77DC0F4ull, 0, Kind::kParticle},
    // xmc4_rmptfx_flat PS_Draw
    {0x6434F5A3323B0E48ull, 0, Kind::kParticle},
    // xmc4_rmptfx_gta_litsprite PS_Draw, PS_DrawSoft, PS_DrawSoftUnLit, PS_DrawUnLit
    {0x5180D761DB20A4DBull, 0, Kind::kParticle},
    {0xA0069D0C8433608Cull, 0, Kind::kParticle},
    {0x0EF72CDB09E9204Bull, 0, Kind::kParticle},
    {0x1D879C46A96C4F62ull, 0, Kind::kParticle},
    // xmc4_rmptfx_litsprite PS_Draw
    {0x82090F19C9F6589Aull, 0, Kind::kParticle},
    // xmc4_rmptfx_mainlist: ps_main_SoftPart_TexBlend, _TexBlend_VertLit, _TexBlend_VertLit_Rescale,
    // _VertLit, _VertLit_BaseLight, _VertLit_Rescale; ps_main_TexBlend_VertLit, _VertLit_BaseLight
    {0x0D04DA519B34023Full, 0, Kind::kParticle},
    {0x388F5B0928DBDA4Bull, 0, Kind::kParticle},
    {0xF79AB78A76812ACFull, 0, Kind::kParticle},
    {0xA9451ACA76447AB2ull, 0, Kind::kParticle},
    {0xB6D8B71D0255189Cull, 0, Kind::kParticle},
    {0x22169B59950EFA42ull, 0, Kind::kParticle},
    {0xBF0E210415F31F04ull, 0, Kind::kParticle},
    {0x3505182426A9688Full, 0, Kind::kParticle},
    // xmc4_rmptfx_model PSRrmptfxModel, PSRrmptfxModelWithNormalMap, PSMultiLight (debris models;
    // taken only when drawn blended)
    {0x7C57A67AB1DB5163ull, 0, Kind::kParticle},
    {0x82C096AEDD04120Eull, 0, Kind::kParticle},
    {0x840949CCF4C5CE2Full, 0, Kind::kParticle},
    // xmc4_rmptfx_projected PSCheckpoint, PSCheckpointProj
    {0xA9687B2317E3B3C2ull, 0, Kind::kParticle},
    {0xA5173DD54C5A03A6ull, 0, Kind::kParticle},
    // xrmptfx_default PS_Draw; xrmptfx_litsprite PS_Draw, PS_DrawHeatHaze, PS_DrawWaterDrop
    {0xA370020CD18DB19Cull, 0, Kind::kParticle},
    {0x1F4D56E84D8C66FFull, 0, Kind::kParticle},
    {0x6F102D8EDB7C38FAull, 0, Kind::kParticle},
    {0x458E761B68FD72CDull, 0, Kind::kParticle},
    // xSoftPtx PSSoftPtx
    {0x72A4CA34BE7FB389ull, 0, Kind::kParticle},
    // xrain_system ParticleRenderPS (the rain's streaks)
    {0x313E112776627E41ull, 0, Kind::kParticle},
    // xlightning PS
    {0x96653B3EC5F735B8ull, 0, Kind::kParticle},

    // --- light volumes: headlight and spot cones -------------------------------------------------
    // xrage_spotlightfog ps_main (two containers' copies), ps_mainFast (two)
    {0x409F327897A9E03Eull, 0, Kind::kLightVolume},
    {0x90036C6E527E57CCull, 0, Kind::kLightVolume},
    {0x87EA74F91E86DF7Bull, 0, Kind::kLightVolume},
    {0x8E8931793A357EFDull, 0, Kind::kLightVolume},

    // --- glass: cars' windows, broken glass --------------------------------------------------------
    // xAmbientCarGlass PSAmbientGlassFront (traffic cars)
    {0x6C1A3BE6B0509155ull, 0, Kind::kGlass},
    // xCarGlass PS_GlassFront, PS_InteriorGlass, PS_InteriorGrime (the player's and other cars)
    {0x4594B239C3B63D42ull, 0, Kind::kGlass},
    {0xFD705246ABE541BCull, 0, Kind::kGlass},
    {0x4ECA6B7EB23F204Cull, 0, Kind::kGlass},
    // xglass_break PSCityWindowLower, PSMultiLight
    {0xE30883B4839E6136ull, 0, Kind::kGlass},
    {0x71551F378BAF22B4ull, 0, Kind::kGlass},
    // xPropBrokenGlass PSPropGlassBroken
    {0xB0038EB1241928E6ull, 0, Kind::kGlass},
    // Left out on purpose: the fog quad (xrage_postfx PSFog 5F01E6F676CFC3F8, PSFogCard
    // 999B6D2A1654643F, PSBlendedHeightFog* 9AB52FE65BE103A5 E0241FFDC4B2D16C 51A47627B1B209BF
    // 7D4E4EDD9E4B5BD5 0E5798C2CF01820B: it changes every far pixel smoothly), the night glows
    // (xstar_glow: drawn after the upscaler), xBlackOpaqueGlass PS_GlassFront (opaque), and the
    // shop and street glass of the world (xPropGlass PSPropGlass 258DA19BC9D38E9D, xbumpedglass
    // PS_Light 7540C66A0992F547: still in the world, the camera's vectors are right for them).
};

const std::unordered_map<uint64_t, Entry>& Table() {
  static const std::unordered_map<uint64_t, Entry> table = [] {
    std::unordered_map<uint64_t, Entry> t;
    for (const Entry& e : kEntries) {
      t.emplace(e.ps, e);
    }
    return t;
  }();
  return table;
}

}  // namespace

UpscalerMasks::~UpscalerMasks() {
  Shutdown();
}

UpscalerMasks::Kind UpscalerMasks::KindOf(uint64_t vs_identity, uint64_t ps_identity) {
  const auto& table = Table();
  const auto it = table.find(ps_identity);
  if (it == table.end()) {
    return Kind::kNone;
  }
  return it->second.vs == 0 || it->second.vs == vs_identity ? it->second.kind : Kind::kNone;
}

const char* UpscalerMasks::KindName(Kind kind) {
  switch (kind) {
    case Kind::kShadowDecal:
      return "shadow decal";
    case Kind::kParticle:
      return "particle";
    case Kind::kLightVolume:
      return "light volume";
    case Kind::kGlass:
      return "glass";
    default:
      return "none";
  }
}

void UpscalerMasks::VariantKey(const PipelineKey& key, PipelineKey* out) {
  const plume::RenderBlendDesc game = UnpackBlend(key.blend[0]);
  *out = key;
  out->color_count = 1;
  out->color_format[0] = uint32_t(kCoverageFormat);
  for (uint32_t i = 1; i < kColorTargetCount; ++i) {
    out->color_format[i] = 0;
    out->blend[i] = 0;
  }
  // What the game's blend says about how much of the pixel the draw is. A source alpha factor (an alpha
  // blend, an additive one that scales its light by alpha): the pixel shader's alpha, kept as the
  // union of the layers (a + (1 - a) * what is there). A premultiplied blend (one, 1 - source alpha):
  // the alpha and, because an alpha of 0 there is a plain addition, the colour too. An additive blend
  // with no alpha in it (one, one): only the colour says, added up. Anything else: the alpha.
  using Blend = plume::RenderBlend;
  const bool alpha_blend = game.srcBlend == Blend::SRC_ALPHA || game.srcBlend == Blend::SRC_ALPHA_SAT;
  const bool premultiplied =
      !alpha_blend && game.srcBlend == Blend::ONE && game.dstBlend == Blend::INV_SRC_ALPHA;
  const bool additive = !alpha_blend && game.srcBlend == Blend::ONE && game.dstBlend == Blend::ONE;
  uint8_t write = uint8_t(plume::RenderColorWriteEnable::ALPHA);
  if (premultiplied) {
    write = uint8_t(plume::RenderColorWriteEnable::ALL);
  } else if (additive) {
    write = uint8_t(plume::RenderColorWriteEnable::RED) |
            uint8_t(plume::RenderColorWriteEnable::GREEN) |
            uint8_t(plume::RenderColorWriteEnable::BLUE);
  }
  plume::RenderBlendDesc blend;
  blend.blendEnabled = true;
  blend.srcBlend = Blend::ONE;  // the colour channels add up (the target is UNORM: they saturate)
  blend.dstBlend = Blend::ONE;
  blend.blendOp = plume::RenderBlendOperation::ADD;
  blend.srcBlendAlpha = Blend::ONE;  // the alpha is the union of the layers
  blend.dstBlendAlpha = Blend::INV_SRC_ALPHA;
  blend.blendOpAlpha = plume::RenderBlendOperation::ADD;
  blend.renderTargetWriteMask = write;
  out->blend[0] = PackBlend(blend);
  // The game's depth and stencil test, and neither written: the depth bit 1 is the write enable, bits
  // 8 to 15 of the masks the stencil write mask. The depth format and the bias stay, the framebuffer
  // has the game's depth beside the coverage image.
  if ((key.depth & 3u) == 3u) {
    // A draw that writes depth: the game's own draw has just written this draw's depth, so a strict
    // comparison would fail against it; the inclusive one passes exactly where the game's passed.
    const auto func = plume::RenderComparisonFunction((key.depth >> 2) & 0xF);
    if (func == plume::RenderComparisonFunction::LESS) {
      out->depth = (out->depth & 3u) | (uint32_t(plume::RenderComparisonFunction::LESS_EQUAL) << 2);
    } else if (func == plume::RenderComparisonFunction::GREATER) {
      out->depth =
          (out->depth & 3u) | (uint32_t(plume::RenderComparisonFunction::GREATER_EQUAL) << 2);
    }
  }
  out->depth &= ~2u;
  out->stencil_masks &= ~0xFF00u;
  out->sample_count = 1;
}

void UpscalerMasks::Initialize(HostPasses& passes, PassTiming& pass_timing) {
  passes_ = &passes;
  pass_timing_ = &pass_timing;
  failed_ = false;
  initialized_ = true;
}

void UpscalerMasks::Shutdown() {
  // The GPU is idle and the heap goes later: the slots are not handed back (TemporalMotion::Shutdown).
  for (LayerState& l : layers_) {
    l.depth_framebuffer.reset();
    l.fb_depth = nullptr;
    l.fb_color = nullptr;
    l.coverage = HostImage{};
    l.mask = HostImage{};
    l.mask_valid = false;
    l.drawn = false;
    l.clear_pending = true;
  }
  pipeline_.reset();
  shader_.reset();
  passes_ = nullptr;
  pass_timing_ = nullptr;
  initialized_ = false;
}

void UpscalerMasks::DropFramebuffer(LayerState& layer) {
  if (layer.depth_framebuffer && passes_) {
    passes_->RetireFramebuffer(std::move(layer.depth_framebuffer));
  }
  layer.depth_framebuffer.reset();
  layer.fb_depth = nullptr;
  layer.fb_color = nullptr;
  layer.fb_generation = 0;
}

void UpscalerMasks::Release() {
  if (!initialized_ || !passes_) {
    return;
  }
  for (LayerState& l : layers_) {
    DropFramebuffer(l);
    passes_->ReleaseImage(l.coverage);
    passes_->ReleaseImage(l.mask);
    l.mask_valid = false;
    l.drawn = false;
    l.clear_pending = true;
  }
}

uint64_t UpscalerMasks::HeldBytes() const {
  uint64_t bytes = 0;
  for (const LayerState& l : layers_) {
    for (const HostImage* image : {&l.coverage, &l.mask}) {
      if (image->texture) {
        bytes += TextureBytes(image->width, image->height, 1, image->format);
      }
    }
  }
  return bytes;
}

void UpscalerMasks::BeginFrame() {
  stats_.max_in_frame = std::max(stats_.max_in_frame, frame_drawn_);
  stats_.frames_any += frame_drawn_ != 0 ? 1 : 0;
  frame_drawn_ = 0;
  for (LayerState& l : layers_) {
    l.clear_pending = true;
    l.drawn = false;
    l.mask_valid = false;
  }
}

HostImage* UpscalerMasks::Target(Layer layer, uint32_t width, uint32_t height, bool* clear) {
  *clear = false;
  if (!initialized_ || failed_ || width == 0 || height == 0) {
    return nullptr;
  }
  LayerState& l = layers_[size_t(layer)];
  const char* name =
      layer == Layer::kReactive ? "reactive mask coverage" : "transparency mask coverage";
  if (!passes_->EnsureImage(l.coverage, width, height, kCoverageFormat, name)) {
    ++stats_.image_failures;
    DropFramebuffer(l);
    return nullptr;
  }
  // A new image (its first use) is cleared, and the framebuffer kept for the old one is not its own.
  // The frame's first second draw clears too; the flag stays up until a draw has gone in (Drawn()).
  if (l.coverage.layout == plume::RenderTextureLayout::UNKNOWN) {
    DropFramebuffer(l);
    l.clear_pending = true;
  }
  *clear = l.clear_pending;
  return &l.coverage;
}

// CHECK: a framebuffer of an RGBA8 coverage image with the game's depth attachment (one sample, the
// depth left in DEPTH_WRITE by the game's own draw, the pipelines not writing it) on D3D12 with the debug
// layer and on Vulkan with validation: that the render pass is compatible with the pipelines of
// VariantKey, that the depth keeps what the game's draws wrote when the game's framebuffer is bound
// again, and that retiring this framebuffer after the depth texture it names was retired (the depth
// target was made again at a new scale or MSAA level) is harmless.
plume::RenderFramebuffer* UpscalerMasks::DepthFramebuffer(Layer layer,
                                                          const plume::RenderTexture* depth,
                                                          uint32_t generation) {
  LayerState& l = layers_[size_t(layer)];
  const plume::RenderTexture* color = l.coverage.texture.get();
  if (!color || !depth || !passes_) {
    return nullptr;
  }
  if (l.depth_framebuffer && l.fb_depth == depth && l.fb_color == color &&
      l.fb_generation == generation) {
    return l.depth_framebuffer.get();
  }
  DropFramebuffer(l);
  l.depth_framebuffer = passes_->CreateFramebuffer(color, depth, "upscaler mask framebuffer");
  if (!l.depth_framebuffer) {
    ++stats_.image_failures;
    return nullptr;
  }
  l.fb_depth = depth;
  l.fb_color = color;
  l.fb_generation = generation;
  return l.depth_framebuffer.get();
}

bool UpscalerMasks::Prepare() {
  if (pipeline_) {
    return true;
  }
  if (failed_ || !initialized_) {
    return false;
  }
  const HostShaderBlob blob = UpscalerMasksPixelShader();
  shader_ = passes_->CreateShader(blob.data, blob.size, blob.name);
  if (shader_) {
    pipeline_ = passes_->CreatePipeline(*shader_, kMaskFormat, "upscaler mask");
  }
  if (!pipeline_) {
    failed_ = true;
    shader_.reset();
    REXLOG_ERROR("{} the pass could not be made (shader or pipeline): no masks this session",
                 kPrefix);
    return false;
  }
  return true;
}

bool UpscalerMasks::Resolve(plume::RenderCommandList* list, uint32_t render_width,
                            uint32_t render_height) {
  const uint32_t test = uint32_t(REXCVAR_GET(mcla_gpu_upscaler_mask_test));
  const bool ready = list && initialized_ && !failed_;
  bool any = false;
  for (size_t i = 0; i < kLayerCount; ++i) {
    LayerState& l = layers_[i];
    l.mask_valid = false;
    const bool had = l.drawn;
    l.drawn = false;  // taken: a draw of a later frame starts a new image
    const bool ones = ((test >> i) & 1u) != 0;
    if (!ready || (!had && !ones)) {
      continue;
    }
    if (had) {
      ++stats_.frames[i];
    }
    if (had && !ones &&
        (l.coverage.width != render_width || l.coverage.height != render_height)) {
      // The draws' viewport was not the render size (a draw in a view of another size): no mask, and
      // never a mask that is a different picture from the colour.
      ++stats_.size_mismatch;
      if (!mismatch_logged_) {
        mismatch_logged_ = true;
        REXLOG_WARN("{} a coverage image is {}x{} and the render size is {}x{}: no mask in such "
                    "frames (the first of its kind)",
                    kPrefix, l.coverage.width, l.coverage.height, render_width, render_height);
      }
      continue;
    }
    const char* name = i == size_t(Layer::kReactive) ? "reactive mask" : "transparency mask";
    if (!Prepare() ||
        !passes_->EnsureImage(l.mask, render_width, render_height, kMaskFormat, name)) {
      ++stats_.image_failures;
      continue;
    }
    MaskPush push{};
    push.gain = i == size_t(Layer::kReactive) ? kReactiveGain : kTransparencyGain;
    push.color_gain = kAddedLightGain;
    if (ones) {
      push.mode = 1;
      ++stats_.forced[i];
    } else {
      // The second draws left the image a render target; the pass reads it.
      if (l.coverage.layout != plume::RenderTextureLayout::SHADER_READ) {
        list->barriers(plume::RenderBarrierStage::GRAPHICS,
                       plume::RenderTextureBarrier(l.coverage.texture.get(),
                                                   plume::RenderTextureLayout::SHADER_READ));
        l.coverage.layout = plume::RenderTextureLayout::SHADER_READ;
      }
      push.coverage = l.coverage.slot;
    }
    pass_timing_->Mark(list, name);
    passes_->DrawInto(list, l.mask, pipeline_.get(), &push, sizeof(push));
    l.mask_valid = true;
    ++stats_.resolved[i];
    any = true;
  }
  return any;
}

std::string UpscalerMasks::Summary() const {
  uint64_t drawn = 0;
  for (const uint64_t n : stats_.drawn) {
    drawn += n;
  }
  uint64_t skipped = 0;
  for (const uint64_t n : stats_.skipped) {
    skipped += n;
  }
  const uint64_t forced = stats_.forced[0] + stats_.forced[1];
  if (drawn == 0 && skipped == 0 && forced == 0 && stats_.image_failures == 0) {
    return {};
  }
  const auto& d = stats_.drawn;
  const auto& s = stats_.skipped;
  const uint64_t frames_any = stats_.frames_any + (frame_drawn_ != 0 ? 1 : 0);
  return std::format(
      "upscaler masks: draws recorded again {} ({} shadow decals, {} particles, {} light volumes, {} "
      "glass; {:.1f} a frame over the {} frames that had any, at most {} in a frame), masks made: "
      "reactive {} of {} frames with draws, transparency {} of {}{}; left out: {} not blended, {} no "
      "depth target, {} size or samples, {} no image, {} pipeline pending; {} frames whose image was "
      "not the render size, {} image failures",
      drawn, d[size_t(Kind::kShadowDecal)], d[size_t(Kind::kParticle)],
      d[size_t(Kind::kLightVolume)], d[size_t(Kind::kGlass)],
      frames_any ? double(drawn) / double(frames_any) : 0.0, frames_any,
      std::max(stats_.max_in_frame, frame_drawn_), stats_.resolved[0] - stats_.forced[0],
      stats_.frames[0], stats_.resolved[1] - stats_.forced[1], stats_.frames[1],
      forced != 0 ? std::format(" (all-ones diagnostic: {} and {} frames)", stats_.forced[0],
                                stats_.forced[1])
                  : std::string(),
      s[size_t(Skip::kNotBlended)], s[size_t(Skip::kNoDepth)], s[size_t(Skip::kSizeOrSamples)],
      s[size_t(Skip::kNoImage)], s[size_t(Skip::kPipelinePending)], stats_.size_mismatch,
      stats_.image_failures);
}

}  // namespace mcla::gpu
