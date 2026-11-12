// MCLA GPU renderer -- the replay of the guest's frames. See scene.h.

#include "scene.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <string_view>

#if defined(_WIN32)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/logging.h>

#include "backend.h"
#include "ab.h"
#include "ao.h"
#include "cycles.h"
#include "descriptors.h"
#include "draw_decode.h"
#include "draw_state.h"
#include "format.h"
#include "layout.h"
#include "mc_engine/ultrawide.h"
#include "native_gfx/vertex_declaration.h"
#include "occlusion.h"
#include "pass_timing.h"
#include "post.h"
#include "resolve.h"
#include "resolve_memory.h"
#include "samplers.h"
#include "temporal.h"
#include "textures.h"
#include "thread_load.h"
#include "vehicle_motion_mask.h"
#include "watch.h"

REXCVAR_DEFINE_BOOL(
    mcla_gpu_scene_profile, false, "MCLA/GPU",
    "Diagnostic (M6a): the render thread's time per stage of a draw (constant banks, "
    "state decode, shaders and layout, geometry, pipeline, constants, textures, upload, targets, "
    "draw), one line per window of 600 frames. Off: nothing is timed.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// The switch over the renderer work of 17/10/2026, asked for by
// the user, live: on, every part of it runs as its own cvar says; off, the
// renderer does what it did before that day.
REXCVAR_DEFINE_BOOL(
    mcla_gpu_work_17oct, true, "MCLA/GPU",
    "On/off for all of the renderer work of 17/10/2026: the half-pixel fill of "
    "scaled resolves (mcla_gpu_resolve_fill_half_pixel), the depth bias times the scale "
    "(mcla_gpu_scale_depth_bias), the delayed colour encodes "
    "(mcla_gpu_resolve_memory_encode_delay) and the encode batch counters. Off: as before "
    "that day. Hot reload.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    mcla_gpu_scale_depth_bias, true, "MCLA/GPU",
    "With a resolution scale above 1 (M6b.3c): the slope-scaled depth bias times the "
    "scale of the draw's targets, as Xenia does -- a host pixel is 1/scale of the guest's. Off: "
    "the guest's factor unchanged (for A/B). Hot reload.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    mcla_gpu_census_scale, false, "MCLA/GPU",
    "Diagnostic (M6e.0): logs once per case each resolve product a draw at another resolution "
    "scale reads (with the fetch's filters) and each copy of an image of the screen that lands "
    "away from its source. Off: nothing is checked.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DECLARE(uint32_t, mcla_gpu_ghost_log_interval);

namespace mcla::gpu {

namespace nfx = mcla::native_gfx;

namespace {

using Clock = std::chrono::steady_clock;

// Root CBVs need 256-byte placement (D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_
// ALIGNMENT); the SPIR-V shaders' float4 loads need 16. One rule for both.
constexpr uint64_t kConstantAlignment = 256;

const char* SkipName(uint32_t skip) {
  static const char* const kNames[] = {
      "device lost",
      "VS unreadable",
      "PS unreadable",
      "declaration unreadable or too long",
      "index buffer unreadable",
      "inline vertices unreadable",
      "inline vertices unterminated",
      "primitive type not drawable",
      "VS not in the pack",
      "PS not in the pack",
      "shader stage mismatch",
      "target format without a host format",
      "input layout cannot be supplied",
      "EDRAM mode",
      "colour target gap",
      "pipeline not created",
      "pipeline pending (being made in the background)",
      "constant upload failed",
      "geometry",
      "EDRAM target not made",
      "framebuffer not made",
  };
  return skip < std::size(kNames) ? kNames[skip] : "?";
}

// The EDRAM identities a draw names (the same in Record and in the skip rule).
EdramKey ColorEdramKey(const XenosState& s, uint32_t i) {
  EdramKey key;
  key.base_tiles = uint16_t(s.color[i].base_tiles);
  key.pitch = uint16_t(s.surface_pitch);
  key.depth = 0;
  key.format = uint8_t(FoldColorFormat(uint32_t(s.color[i].format)));
  key.samples = uint8_t(MsaaSampleCount(s.msaa));
  return key;
}

EdramKey DepthEdramKey(const XenosState& s) {
  EdramKey key;
  key.base_tiles = uint16_t(s.depth_base_tiles);
  key.pitch = uint16_t(s.surface_pitch);
  key.depth = 1;
  key.format = uint8_t(s.depth_format);
  key.samples = uint8_t(MsaaSampleCount(s.msaa));
  return key;
}

// The depth identity of a draw that has `color_count` colour targets, with its layer (the scene's
// or the UI's: EdramTargets::DepthLayer).
EdramKey DrawDepthKey(EdramTargets& edram, const XenosState& s, uint32_t color_count) {
  EdramKey key = DepthEdramKey(s);
  if (color_count != 0) {
    const EdramKey color0 = ColorEdramKey(s, 0);
    key.reserved = edram.DepthLayer(key, &color0);
  } else {
    key.reserved = edram.DepthLayer(key, nullptr);
  }
  return key;
}

const char* DrawKindName(DrawKind kind) {
  switch (kind) {
    case DrawKind::kIndexed: return "indexed";
    case DrawKind::kAuto: return "auto";
    case DrawKind::kInline: return "inline";
  }
  return "?";
}

// EDRAM ownership. A blend whose result does not depend on what
// the target held: the destination factor ZERO in both parts, no MIN/MAX
// (they ignore the factors), no source factor that reads the destination.
bool BlendIgnoresDestination(const xreg::RB_BLENDCONTROL& b) {
  const auto reads = [](xe::BlendFactor f) {
    return f == xe::BlendFactor::kDstColor || f == xe::BlendFactor::kOneMinusDstColor ||
           f == xe::BlendFactor::kDstAlpha || f == xe::BlendFactor::kOneMinusDstAlpha ||
           f == xe::BlendFactor::kSrcAlphaSaturate;
  };
  const auto min_max = [](xe::BlendOp op) {
    return op == xe::BlendOp::kMin || op == xe::BlendOp::kMax;
  };
  return b.color_destblend == xe::BlendFactor::kZero &&
         b.alpha_destblend == xe::BlendFactor::kZero && !min_max(b.color_comb_fcn) &&
         !min_max(b.alpha_comb_fcn) && !reads(b.color_srcblend) && !reads(b.alpha_srcblend);
}

// Whether a draw can be taken to write every sample inside its scissor: one
// rectangle (the census's full-target rect, 24.3) or a quad of two triangles,
// every sample of PA_SC_AA_MASK, and nothing that can reject a sample -- a
// kill in the pixel shader, alpha test, alpha to mask, a depth or stencil
// test. Whether it really spans its scissor is for its coverage check
// (EdramTargets).
bool TakeCandidate(const XenosState& s, const DrawRecord& r, uint32_t samples) {
  bool shape = false;
  switch (xe::PrimitiveType(r.primitive)) {
    case xe::PrimitiveType::kRectangleList:
      shape = r.count == 3;
      break;
    case xe::PrimitiveType::kTriangleList:
      shape = r.count == 6;
      break;
    case xe::PrimitiveType::kTriangleStrip:
    case xe::PrimitiveType::kTriangleFan:
    case xe::PrimitiveType::kQuadList:
      shape = r.count == 4;
      break;
    default:
      break;
  }
  if (!shape || (r.flags & kDrawPsKills)) {
    return false;
  }
  if ((s.color_control.alpha_test_enable &&
       s.color_control.alpha_func != xe::CompareFunction::kAlways) ||
      s.color_control.alpha_to_mask_enable) {
    return false;
  }
  const uint32_t all_samples = (1u << samples) - 1;
  if ((s.aa_mask & all_samples) != all_samples) {
    return false;
  }
  if (s.depth_bound) {
    const xreg::RB_DEPTHCONTROL& dc = s.depth_control;
    if (dc.z_enable && dc.zfunc != xe::CompareFunction::kAlways) {
      return false;
    }
    if (dc.stencil_enable &&
        (dc.stencilfunc != xe::CompareFunction::kAlways ||
         (dc.backface_enable && dc.stencilfunc_bf != xe::CompareFunction::kAlways))) {
      return false;
    }
  }
  return true;
}

// Whether a draw writes the stencil at all, and whether it writes all of it
// with a value that does not depend on what it held: ALWAYS, REPLACE when it
// passes (with no failing depth test, TakeCandidate), all 8 bits, both faces.
bool StencilWrites(const XenosState& s) {
  const xreg::RB_DEPTHCONTROL& dc = s.depth_control;
  const auto face = [](xe::StencilOp fail, xe::StencilOp zfail, xe::StencilOp zpass,
                       uint32_t write_mask) {
    return (write_mask & 0xFFu) != 0 &&
           (fail != xe::StencilOp::kKeep || zfail != xe::StencilOp::kKeep ||
            zpass != xe::StencilOp::kKeep);
  };
  return face(dc.stencilfail, dc.stencilzfail, dc.stencilzpass, s.stencil.stencilwritemask) ||
         (dc.backface_enable && face(dc.stencilfail_bf, dc.stencilzfail_bf, dc.stencilzpass_bf,
                                     s.stencil_bf.stencilwritemask));
}

bool StencilReplacesAll(const XenosState& s) {
  const xreg::RB_DEPTHCONTROL& dc = s.depth_control;
  const auto face = [](xe::CompareFunction func, xe::StencilOp zpass, uint32_t write_mask) {
    return func == xe::CompareFunction::kAlways && zpass == xe::StencilOp::kReplace &&
           (write_mask & 0xFFu) == 0xFFu;
  };
  return face(dc.stencilfunc, dc.stencilzpass, s.stencil.stencilwritemask) &&
         (!dc.backface_enable ||
          face(dc.stencilfunc_bf, dc.stencilzpass_bf, s.stencil_bf.stencilwritemask));
}

// The recording state a draw would set equals what the list holds.
bool SameViewport(const plume::RenderViewport& a, const plume::RenderViewport& b) {
  return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height &&
         a.minDepth == b.minDepth && a.maxDepth == b.maxDepth;
}

bool SameRect(const plume::RenderRect& a, const plume::RenderRect& b) {
  return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

bool SameBuffer(const plume::RenderBufferReference& a, const plume::RenderBufferReference& b) {
  return a.ref == b.ref && a.offset == b.offset;
}

bool SameIndexBuffer(const plume::RenderIndexBufferView& a, const plume::RenderIndexBufferView& b) {
  return SameBuffer(a.buffer, b.buffer) && a.size == b.size && a.format == b.format;
}

bool SameVertexBuffers(uint32_t bound_count, const plume::RenderVertexBufferView* bound_views,
                       const plume::RenderInputSlot* bound_slots, const DrawGeometry& g,
                       const plume::RenderInputSlot* slots) {
  if (bound_count != g.slot_count) {
    return false;
  }
  for (uint32_t i = 0; i < g.slot_count; ++i) {
    if (!SameBuffer(bound_views[i].buffer, g.vertex_views[i].buffer) ||
        bound_views[i].size != g.vertex_views[i].size || bound_slots[i].index != slots[i].index ||
        bound_slots[i].stride != slots[i].stride ||
        bound_slots[i].classification != slots[i].classification) {
      return false;
    }
  }
  return true;
}

const char* ConversionName(uint32_t conversion) {
  static const char* const kNames[] = {"none",       "quad list", "quad strip", "fan/polygon",
                                       "rect list",  "line loop", "unsupported"};
  return conversion < std::size(kNames) ? kNames[conversion] : "?";
}

// The resolution scale of a draw's targets: its first identity's -- RT0, or
// the depth target when it writes no colour -- or, before that target exists,
// the one the declared size of the surface binding it gives (EdramTargets::
// ScaleOf); Scene::Record makes the targets at it and checks that every other
// target agrees. `*host_samples`: that identity's samples on the host
// (EdramTargets::HostSamplesOf), which the pipeline and g_AlphaToMask take. `*scene`: that
// identity is the scene's and the scene is drawn at a scale of its own (EdramTargets::SceneApart):
// its textures are sampled with the scene's mip bias (Scene::SceneBias). `*msaa_guest`: that
// identity takes the host's MSAA level (EdramTargets::HostSamplesFollowLevel), and the guest's
// samples then; else 0.
Scale DrawScale(EdramTargets& edram, const XenosState& s, const PipelineKey& key,
                uint32_t size_bits, uint32_t* host_samples, bool* scene, uint32_t* msaa_guest) {
  *scene = false;
  *msaa_guest = 0;
  EdramKey first;
  first.pitch = uint16_t(s.surface_pitch);
  first.samples = uint8_t(MsaaSampleCount(s.msaa));
  if (key.color_count) {
    first.base_tiles = uint16_t(s.color[0].base_tiles);
    first.depth = 0;
    first.format = uint8_t(FoldColorFormat(uint32_t(s.color[0].format)));
  } else if (key.depth_format != 0) {
    first.base_tiles = uint16_t(s.depth_base_tiles);
    first.depth = 1;
    first.format = uint8_t(s.depth_format);
    first.reserved = edram.DepthLayer(first, nullptr);
  } else {
    *host_samples = first.samples;
    return Scale{};
  }
  *host_samples = edram.HostSamplesOf(first, size_bits);
  if (edram.HostSamplesFollowLevel(first, size_bits)) {
    *msaa_guest = first.samples;
  }
  *scene = EdramTargets::IsSceneKey(first) && edram.SceneApart();
  return edram.ScaleOf(first, size_bits);
}

// The pass a draw opens, for PassTiming: the targets' pitch and
// samples, then each colour target and the depth one as format@base.
std::string PassLabel(EdramTargets::Target* const* colors, uint32_t count,
                      const EdramTargets::Target* depth) {
  const EdramKey& first = count ? colors[0]->key : depth->key;
  std::string label = std::format("{} {}x", first.pitch, first.samples);
  for (uint32_t i = 0; i < count; ++i) {
    const char* name = format::ColorTargetFormat(colors[i]->key.format).guest_name;
    label += std::format(" {}@{}", name, colors[i]->key.base_tiles);
  }
  if (depth) {
    const char* name = format::DepthTargetFormat(depth->key.format).guest_name;
    label += std::format(" / {}@{}", name, depth->key.base_tiles);
  }
  return label;
}

// A colour target for the composite census: its key and scale in
// one word.
uint64_t CompositeTarget(const EdramTargets::Target* target) {
  if (!target) {
    return CompositeCensus::kNoTarget;
  }
  const EdramKey& k = target->key;
  return CompositeCensus::Pack(k.base_tiles, k.pitch, k.depth, k.format, k.samples,
                               target->scale.n);
}

// mcla_gpu_census_scale (M6e.0): a fetch of a resolve product made at another
// scale than the draw's, logged once per product, scales and filters (the
// first 64): how the reads across scales sample.
void NoteScaleRead(std::unordered_set<uint64_t>& logged, const TextureFetch& fetch,
                   const uint32_t dwords[6], const TextureCache::Binding& bound, Scale scale) {
  xe::xe_gpu_texture_fetch_t f;
  std::memcpy(&f, dwords, sizeof(f));
  const auto filter = [](xe::TextureFilter v) {
    switch (v) {
      case xe::TextureFilter::kPoint: return "point";
      case xe::TextureFilter::kLinear: return "linear";
      case xe::TextureFilter::kBaseMap: return "base map";
      default: return "from the fetch";
    }
  };
  const uint64_t id = uint64_t(fetch.key.base_page) ^ uint64_t(fetch.key.format) << 32 ^
                      uint64_t(bound.product_scale.n & 0x7Fu) << 38 ^ uint64_t(scale.n & 0x7Fu) << 45 ^
                      uint64_t(uint32_t(f.mag_filter) & 3u) << 52 ^
                      uint64_t(uint32_t(f.min_filter) & 3u) << 54 ^
                      uint64_t(uint32_t(f.mip_filter) & 3u) << 56 ^
                      uint64_t(uint32_t(f.aniso_filter) & 7u) << 58;
  if (logged.size() < 64 && logged.insert(id).second) {
    REXLOG_INFO("[gpu] census scale: {} ({}x, {}) read by a draw at {}x {}; mag {}, min {}, mip "
                "{}, aniso {}",
                fetch.key.Describe(), bound.product_scale,
                bound.depth_source ? "a depth source" : "colour", scale,
                bound.scale == bound.product_scale ? "directly"
                : bound.product_scale > scale      ? "through a reduced copy"
                                                   : "through an enlarged copy",
                filter(f.mag_filter), filter(f.min_filter), filter(f.mip_filter),
                uint32_t(f.aniso_filter));
  }
}

// The squeeze of a UI draw (Scene::UiSqueezeOf): its position's x becomes centre + (x - centre) * k
// on the screen. The draw's NDC is x_ndc = oPos.x * scale + offset * w; the same map on NDC is
// k * x_ndc + (1 - k) * u, u being where the screen's centre is in the draw's own NDC (the draw's
// host viewport need not be the whole screen).
// CHECK: that the epilogue's NDC is linear in the viewport's pixels as ComputeViewport makes it, for
// the screen-space draws with clipping disabled too (the squeeze is checked on the HUD only).
void SqueezeSharedX(SharedConstants& shared, const plume::RenderViewport& viewport,
                    uint32_t screen_width, float k) {
  if (!(viewport.width > 0.0f)) {
    return;
  }
  const float centre = 0.5f * float(screen_width);
  const float u = 2.0f * (centre - (viewport.x + 0.5f * viewport.width)) / viewport.width;
  shared.pos_scale[0] *= k;
  shared.pos_offset[0] = shared.pos_offset[0] * k + (1.0f - k) * u;
}

// The same map on the scissor, outwards to whole pixels: what the guest clips its UI to stays where
// the squeezed UI is.
plume::RenderRect SqueezeScissorX(const plume::RenderRect& rect, uint32_t screen_width, float k) {
  const double centre = 0.5 * double(screen_width);
  const int32_t width = int32_t(screen_width);
  plume::RenderRect out = rect;
  out.left = std::clamp(int32_t(std::floor(centre + (double(rect.left) - centre) * k)), 0, width);
  out.right = std::clamp(int32_t(std::ceil(centre + (double(rect.right) - centre) * k)), 0, width);
  return out;
}

// Whether an inline draw of one rectangle -- three vertices of a rectangle list, or four to six of a
// quad or two triangles -- covers the whole screen: what a fade, a tint or a dimmed backdrop is. The
// positions are the first stream's POSITION as the guest wrote them, before the vertex shader: for
// the rectangle list the game draws its full-screen passes with, in guest pixels or in the clip
// space (-1..1) of a pass-through shader; for the others in guest pixels only (a unit quad that a
// matrix scales to a panel has the other spaces' numbers too).
// CHECK: that the game's fades are inline draws with positions in pixels (or a rectangle list), the
// log line of Scene::UiSqueezeOf says which kind it found; one drawn from a vertex buffer is not
// looked at and stays squeezed.
bool InlineRectCoversScreen(const DrawView& view, const InputLayout& layout, bool rect_list,
                            uint32_t screen_width, uint32_t screen_height, const char** space) {
  const DrawRecord& r = view.record();
  if (r.kind != DrawKind::kInline || layout.streams.empty() || r.inline_stride == 0 ||
      r.count < 3 || r.count > 6 || r.inline_bytes < r.count * r.inline_stride) {
    return false;
  }
  const int32_t offset = layout.streams[0].position_offset;
  if (offset < 0 || uint32_t(offset) + 8 > r.inline_stride) {
    return false;
  }
  float x[7], y[7];
  uint32_t n = r.count;
  const uint8_t* data = view.inline_data();
  for (uint32_t v = 0; v < n; ++v) {
    const uint8_t* at = data + size_t(v) * r.inline_stride + uint32_t(offset);
    const uint32_t bx = LoadBe32(at);
    const uint32_t by = LoadBe32(at + 4);
    std::memcpy(&x[v], &bx, 4);
    std::memcpy(&y[v], &by, 4);
  }
  if (rect_list && n == 3) {
    // The fourth corner of a rectangle list is made from the first three.
    x[3] = x[0] + x[2] - x[1];
    y[3] = y[0] + y[2] - y[1];
    n = 4;
  }
  float lo_x = x[0], hi_x = x[0], lo_y = y[0], hi_y = y[0];
  for (uint32_t v = 0; v < n; ++v) {
    if (!std::isfinite(x[v]) || !std::isfinite(y[v])) {
      return false;
    }
    lo_x = std::min(lo_x, x[v]);
    hi_x = std::max(hi_x, x[v]);
    lo_y = std::min(lo_y, y[v]);
    hi_y = std::max(hi_y, y[v]);
  }
  const float w = float(screen_width), h = float(screen_height);
  if (lo_x <= 0.5f && hi_x >= w - 0.5f && lo_y <= 0.5f && hi_y >= h - 0.5f) {
    *space = "pixels";
    return true;
  }
  if (rect_list && lo_x <= -0.999f && hi_x >= 0.999f && lo_y <= -0.999f && hi_y >= 0.999f) {
    *space = "clip";
    return true;
  }
  return false;
}

}  // namespace

float Scene::UiSqueezeOf(const DrawView& view, const XenosState& state, const PipelineKey& key,
                         const InputLayout* layout, PrimitiveConversion conversion) {
  if (!ui_phase_) {
    return 1.0f;
  }
  // The world icons: the game's own 3D view put them where their points are on the widened picture,
  // and their depth test needs the position unmoved (mc_engine/ultrawide.h).
  // CHECK: the icons of a race (racers, checkpoints, places) are drawn inside the bracket the hooks
  // make (ultrawide.cpp, Patch_UiWorldIcons_*); one drawn outside it is squeezed with the HUD.
  if (ui_world_) {
    return 1.0f;
  }
  const double fraction = mc::ultrawide::UiWidthFraction();
  if (!(fraction < 0.999)) {
    return 1.0f;
  }
  // The screen's own image (colour or its stencil masks): the width of the game's screen.
  uint32_t screen_width = 0, screen_height = 0;
  if (!edram_->GuestScreen(&screen_width, &screen_height, /*quiet=*/true) ||
      state.surface_pitch != screen_width) {
    return 1.0f;
  }
  if (key.color_count != 0) {
    const ColorTargetState& color = state.color[0];
    if (!color.bound || (color.format != xe::ColorRenderTargetFormat::k_8_8_8_8 &&
                         color.format != xe::ColorRenderTargetFormat::k_8_8_8_8_GAMMA)) {
      return 1.0f;
    }
    // CHECK: that what the UI phase draws with blending off is only copies of the scene (the pause
    // view's first draw, the tone map) and full-screen backgrounds, which must keep the whole
    // screen; a UI element drawn with blending off would stay stretched.
    const plume::RenderBlendDesc blend = UnpackBlend(key.blend[0]);
    if (!blend.blendEnabled || blend.renderTargetWriteMask == 0) {
      return 1.0f;
    }
    // A fade, a tint or a dimmed backdrop (a blended rectangle over the whole screen) is stretched
    // with the picture, not squeezed with the HUD: squeezed, it would leave the sides unfaded.
    const char* space = "";
    if (layout &&
        InlineRectCoversScreen(view, *layout, conversion == PrimitiveConversion::kRectList,
                               screen_width, screen_height, &space)) {
      if (ultrawide_fills_logged_ < 12) {
        ++ultrawide_fills_logged_;
        const DrawRecord& r = view.record();
        REXLOG_INFO("[gpu] ultrawide: a full-screen UI draw is stretched, not squeezed ({}: VS {:016X} "
                    "PS {:016X}, {} vertices, positions in {}) ({} of 12 logged)",
                    conversion == PrimitiveConversion::kRectList ? "rectangle list" : "quad or triangles",
                    r.vs_identity, r.ps_identity, r.count, space, ultrawide_fills_logged_);
      }
      return 1.0f;
    }
  } else if (!state.depth_bound) {
    return 1.0f;
  }
  return float(fraction);
}

void Scene::Initialize(UploadRing& upload, Pipelines& pipelines, Samplers& samplers,
                       GeometryCache& geometry, EdramTargets& edram, TextureCache& textures,
                       ResolveCache& resolve, const GuestLayout& layout,
                       const BindlessHeap& heap, PassTiming& pass_timing, PostAa& post) {
  pass_timing_ = &pass_timing;
  post_ = &post;
  upload_ = &upload;
  pipelines_ = &pipelines;
  samplers_ = &samplers;
  geometry_ = &geometry;
  edram_ = &edram;
  msaa_.Initialize(edram, pipelines);
  textures_ = &textures;
  resolve_ = &resolve;
  layout_ = &layout;
  heap_ = &heap;
  base_shared_ = SharedConstants{};
  for (uint32_t i = 0; i < kFetchSlotCount; ++i) {
    base_shared_.texture_2d[i] = kNullTexture2DSlot;
    base_shared_.texture_3d[i] = kNullTexture3DSlot;
    base_shared_.texture_cube[i] = kNullTextureCubeSlot;
    base_shared_.sampler[i] = kSamplerLinearWrap;
  }
  // Host targets at the guest's size for now: the pixel position in guest
  // pixels is the host's (contract v7; M6b.2 sets it per target).
  base_shared_.inv_resolution_scale[0] = 1.0f;
  base_shared_.inv_resolution_scale[1] = 1.0f;
  // The vector bank swap against the scalar one, once.
  VerifySwapBankToHost();
  cycles::Measure();  // the base of the first window's TSC rate
  ab::Initialize();
  profile_ = REXCVAR_GET(mcla_gpu_scene_profile);
  census_scale_ = REXCVAR_GET(mcla_gpu_census_scale);
  geometry_->SetProfile(profile_);
  draw_census_.Configure();
  target_census_.Configure();
}

void Scene::LapStart() {
  if (profile_) {
    lap_ = __rdtsc();
  }
}

void Scene::Lap(Stage stage) {
  if (profile_) {
    const uint64_t now = __rdtsc();
    stage_cycles_[size_t(stage)] += now - lap_;
    lap_ = now;
  }
}

void Scene::ReportProfile(const char* why, double frames, const ThreadLoad::Sample& load) {
  if (!profile_) {
    return;
  }
  static const char* const kNames[] = {"banks",     "decode",   "shaders", "geometry",
                                       "pipeline",  "constants", "textures", "upload",
                                       "targets",   "bind pipeline", "bind constants",
                                       "bind state", "bind streams", "draw"};
  static_assert(std::size(kNames) == size_t(Stage::kCount));
  std::string text;
  double sum = 0.0;
  for (size_t i = 0; i < stage_cycles_.size(); ++i) {
    const double ms = cycles::ToNs(stage_cycles_[i]) / frames / 1e6;
    sum += ms;
    text += std::format("{}{} {:.3f}", text.empty() ? "" : ", ", kNames[i], ms);
  }
  static const char* const kMemoNames[] = {"targets", "pipeline", "vs", "ps", "state"};
  static_assert(std::size(kMemoNames) == size_t(Memo::kCount));
  std::string memos;
  for (size_t i = 0; i < memo_hits_.size(); ++i) {
    memos += std::format("{}{} {:.1f}%", memos.empty() ? "" : ", ", kMemoNames[i],
                         memo_tries_[i] ? 100.0 * double(memo_hits_[i]) / double(memo_tries_[i]) : 0.0);
  }
  const InputLayouts::MemoStats layouts = input_layouts_.TakeMemoStats();
  memos += std::format(", layout {:.1f}%",
                       layouts.tries ? 100.0 * double(layouts.hits) / double(layouts.tries) : 0.0);
  REXLOG_INFO("[gpu] scene ({}): profile ms/frame: {} | sum {:.3f} | memo hits: {}", why, text,
              sum, memos);
  // The render thread's CPU by record type, and its loop (the rest).
  if (load.total_cycles) {
    std::string records;
    uint64_t record_sum = 0;
    for (uint32_t i = 0; i < uint32_t(RecordType::kCount); ++i) {
      record_sum += load.record_cycles[i];
      if (load.record_cycles[i]) {
        records += std::format("{}{} {:.3f}", records.empty() ? "" : ", ",
                               RecordTypeName(RecordType(i)),
                               cycles::ToNs(load.record_cycles[i]) / frames / 1e6);
      }
    }
    const uint64_t busy =
        load.total_cycles - std::min(load.total_cycles, load.idle_cycles + load.gpu_wait_cycles);
    REXLOG_INFO("[gpu] scene ({}): thread cpu ms/frame by record: {} | loop {:.3f}", why, records,
                cycles::ToNs(busy - std::min(busy, record_sum)) / frames / 1e6);
  }
  stage_cycles_.fill(0);
  memo_hits_.fill(0);
  memo_tries_.fill(0);
}

void Scene::ResetListState() {
  bound_ = ListState{};
}

void Scene::BeginFrame(plume::RenderCommandList* list, uint32_t slot) {
  list_ = list;
  slot_ = slot;
  scale_depth_bias_ = REXCVAR_GET(mcla_gpu_scale_depth_bias) && REXCVAR_GET(mcla_gpu_work_17oct);
  geometry_->BeginFrame(slot);
  edram_->BeginFrame(list, slot);
  textures_->BeginFrame(slot);
  resolve_->BeginFrame(slot);
  if (samplers_->FrameSettingChanged()) {
    // The memoized fetches hold sampler slots of the old anisotropy.
    fetch_memo_.fill(FetchMemo{});
    last_fetch_.fill(nullptr);
  }
  ++frame_serial_;
  watch::BeginFrame(frame_serial_);
  const uint64_t targets_before = edram_->change_serial();
  if (temporal_) {
    // A frame boundary: nothing is recorded in `list` yet, so the temporal system may change the
    // scene's scale (and the path) here.
    temporal_->BeginFrame(slot, list);
    temporal_->shadow_mask().BeginFrame();  // no shadow decal is drawn into its mask yet
  }
  // The host MSAA level (msaa_level.h): the same boundary, after the path's own change, which may
  // have remade the targets this frame. It is 1 while an upscaler runs and changes once the path has
  // settled (without a temporal system, always).
  msaa_.BeginFrame(list, temporal_ && temporal_->UpscalerNext(),
                   !temporal_ || temporal_->PathSettled(),
                   edram_->change_serial() != targets_before);
  if (ao_) {
    ao_->BeginFrame();
  }
  ResetListState();
  marked_framebuffer_ = nullptr;
  if (layers_) {
    layers_->BeginFrame();
  }
  if (hdr_tone_) {
    hdr_tone_->BeginFrame();
  }
  // Uploads of the previous frame live in another slot's memory.
  device_uploads_.clear();
  last_shared_span_ = UploadSpan{};
  draws_since_flush_ = 0;
}

void Scene::EndFrame(plume::RenderCommandList* list) {
  // The render pass closes before the geometry's copies and the present pass.
  if (list && bound_.framebuffer) {
    list->setFramebuffer(nullptr);
  }
  if (layers_) {
    // A frame that ended with the UI layer started puts its target back first.
    layers_->EndFrame(list);
  }
  // The colour encodes of the frame into the resolve memory, as one batch.
  pass_timing_->Mark(list, "resolve memory encode (batch)");
  resolve_->memory().Flush(list, ResolveMemory::FlushReason::kEndOfFrame);
  pass_timing_->Mark(list, "end-of-frame copies");
  marked_framebuffer_ = nullptr;
  geometry_->EndFrame(list);
  ResetListState();
  list_ = nullptr;
  device_uploads_.clear();
  last_shared_span_ = UploadSpan{};
  draws_since_flush_ = 0;
}

void Scene::BeforeFlush(plume::RenderCommandList* list) {
  if (list && bound_.framebuffer) {
    list->setFramebuffer(nullptr);
  }
}

void Scene::OnListChanged(plume::RenderCommandList* list) {
  // The upload memory belongs to the slot, not to the list: what was uploaded
  // stays valid for the rest of the frame. Bindings belong to the list.
  list_ = list;
  draws_since_flush_ = 0;
  ResetListState();
  marked_framebuffer_ = nullptr;
}

void Scene::BindFramebuffer(plume::RenderCommandList* list,
                            plume::RenderFramebuffer* framebuffer) {
  if (bound_.framebuffer == framebuffer) {
    return;
  }
  list->setFramebuffer(framebuffer);
  bound_.framebuffer = framebuffer;
}

void Scene::UiLayersBeforeDraw(FrameLayers::DrawAction action, plume::RenderCommandList* list) {
  if (bound_.framebuffer) {
    list->setFramebuffer(nullptr);
    bound_.framebuffer = nullptr;
  }
  if (action == FrameLayers::DrawAction::kStart) {
    layers_->Start(list);
  } else {
    layers_->End(list);
  }
  // It bound its own layout, pipeline, viewport and scissor, and drew into the UI target.
  ResetListState();
  marked_framebuffer_ = nullptr;
}

void Scene::PrepareHdrDraw(PreparedDraw& d, const PipelineInputs& inputs,
                           const ConstantBanks::Banks& banks, uint32_t host_samples) {
  HdrToneMap::DrawFacts facts;
  facts.vs = d.vs;
  facts.ps = d.ps;
  facts.key = &d.key;
  facts.color0 = ColorEdramKey(d.state, 0);
  facts.host_samples = host_samples;
  facts.scale = d.scale;
  facts.state = &d.state;
  facts.viewport = &d.viewport.viewport;
  facts.ps_bank = banks.ps.data();
  // FXAA and SMAA do not run at the marker while an upscaler is on (OnComposite): not a reason to
  // keep the float scene off. CHECK: HDR on + FSR + mcla_gpu_aa=fxaa gives the float scene.
  facts.aa_held_off = temporal_ && temporal_->HoldsAaOff(false);
  const HdrToneMap::Action action = hdr_tone_->Classify(facts);
  if (action == HdrToneMap::Action::kNone) {
    return;
  }
  PipelineKey variant;
  hdr_tone_->VariantKey(action, d.key, &variant);
  // A pipeline nobody made yet is built in the background: the second draw (and with it the frame's
  // float scene) waits for it, the game's own draw does not.
  const Deferral deferral{&Scene::AlwaysMaySkip, nullptr};
  bool pending = false;
  plume::RenderPipeline* pipeline = pipelines_->Get(variant, inputs, &deferral, &pending);
  if (!pipeline) {
    hdr_tone_->Failed(action, HdrToneMap::Cause::kPipeline, pending,
                      HdrToneMap::AddsLight(d.key.blend[0]));
    return;
  }
  d.hdr_action = action;
  d.hdr_pipeline = pipeline;
}

void Scene::RecordHdrDuplicate(const PreparedDraw& d, plume::RenderCommandList* list) {
  HostImage* image = hdr_tone_->Image(d.hdr_action);
  if (!image) {
    return;  // the frame has no float scene (the tone map's image failure is logged by Image)
  }
  pass_timing_->Mark(list, "hdr scene");
  // The layout change and a new image's first clear happen outside the game's render pass.
  const bool first_use = image->layout == plume::RenderTextureLayout::UNKNOWN;
  if (image->layout != plume::RenderTextureLayout::COLOR_WRITE) {
    list->setFramebuffer(nullptr);
    list->barriers(plume::RenderBarrierStage::GRAPHICS,
                   plume::RenderTextureBarrier(image->texture.get(),
                                               plume::RenderTextureLayout::COLOR_WRITE));
    image->layout = plume::RenderTextureLayout::COLOR_WRITE;
  }
  list->setFramebuffer(image->framebuffer.get());
  if (first_use) {
    // A new colour target is initialised before it is drawn into, as the EDRAM targets and
    // HostPasses::DrawInto do (D3D12 debug layer id 1422).
    list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
  } else if (d.hdr_action == HdrToneMap::Action::kToneMap) {
    // The image is drawn again every frame and the tone map's quad leaves its first host column and
    // row out at a render scale (Scale::HalfPixelGap: the D3D pixel centre starts it half a guest
    // pixel in). A light drawn again after it adds to what is there, so without this the border
    // adds up from frame to frame and keeps an earlier view's. Cleared with each new float scene it
    // holds at most one frame's light; the consumers read it from the next column and row anyway
    // (FrameLayers::Frame::gap), so this only spares one that does not a huge value.
    // CHECK: clearColor with two rectangles on this image's framebuffer on D3D12 (debug layer) and
    // Vulkan (validation), the same call EdramTargets::ClearColor makes with one.
    const uint32_t gap = d.scale.HalfPixelGap();
    if (gap != 0) {
      const plume::RenderRect strips[2] = {
          plume::RenderRect(0, 0, int32_t(gap), int32_t(image->height)),
          plume::RenderRect(0, 0, int32_t(image->width), int32_t(gap))};
      list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f), strips, 2);
    }
  }
  // Everything the game's draw bound, set again: the list's own state is not trusted across the
  // framebuffer switch, and the list state is forgotten afterwards (ResetListState below).
  list->setGraphicsPipelineLayout(layout_->layout());
  list->setGraphicsDescriptorSet(heap_->texture_set(), 0);
  list->setGraphicsDescriptorSet(heap_->texture_set(), 1);
  list->setGraphicsDescriptorSet(heap_->texture_set(), 2);
  list->setGraphicsDescriptorSet(heap_->sampler_set(), 3);
  list->setPipeline(d.hdr_pipeline);
  const UploadSpan& shared = d.hdr_shared ? d.hdr_shared : d.shared_constants;
  if (ActiveBackend() == Backend::kD3D12) {
    list->setGraphicsRootDescriptor(d.vs_constants.reference(), GuestLayout::kVertexConstants);
    list->setGraphicsRootDescriptor(d.ps_constants.reference(), GuestLayout::kPixelConstants);
    list->setGraphicsRootDescriptor(shared.reference(), GuestLayout::kSharedConstants);
  } else {
    const uint64_t addresses[3] = {d.vs_constants.device_address, d.ps_constants.device_address,
                                   shared.device_address};
    list->setGraphicsPushConstants(0, addresses, 0, sizeof(addresses));
  }
  list->setViewports(&d.viewport.viewport, 1);
  list->setScissors(&d.scissor, 1);
  if (pipelines_->dynamic_depth_bias()) {
    list->setDepthBias(0.0f, 0.0f, 0.0f);  // no depth target: the guest's bias does not apply
  }
  const DrawGeometry& g = d.geometry;
  if (g.slot_count) {
    list->setVertexBuffers(0, g.vertex_views.data(), g.slot_count, d.input_layout->slots.data());
  }
  if (g.indexed) {
    list->setIndexBuffer(&g.index_view);
    list->drawIndexedInstanced(g.count, 1, g.first, g.base_vertex, 0);
  } else {
    list->drawInstanced(g.count, 1, g.first, 0);
  }
  // The pass ends here: the next draw of the game opens its own, and the marker needs none open.
  list->setFramebuffer(nullptr);
  ResetListState();
  marked_framebuffer_ = nullptr;
  hdr_tone_->Duplicated(d.hdr_action);
}

void Scene::PrepareShadowMask(PreparedDraw& d, const PipelineInputs& inputs) {
  // Only in a frame an upscaler works on (nothing else reads the mask), before its anchor: the decals
  // are drawn before the motion blur draw.
  const TemporalSystem::FrameState& fr = temporal_->frame();
  if (!UpscalerRunning() || fr.anchor_seen || fr.composite_seen) {
    return;
  }
  // The play view of the main view, a draw that covers it whole as the world's draws do (the gate of
  // temporal.cpp EvaluateGate): the decal's image is the render size, whatever size its draw's viewport
  // is, so a draw that does not cover the view is left out.
  const EdramKey color = ColorEdramKey(d.state, 0);
  EdramKey depth;
  const EdramKey* depth0 = nullptr;
  if (d.key.depth_format != 0) {
    depth = DrawDepthKey(*edram_, d.state, d.key.color_count);
    depth0 = &depth;
  }
  const temporal::MainMatch match = temporal::MatchMainView(&color, depth0);
  if (!match || !temporal::IsPlaySamples(match.samples) ||
      d.viewport.viewport.width + 0.5f < float(d.scale.Edge(temporal::kMainPitch))) {
    return;
  }
  PipelineKey variant;
  ShadowMask::VariantKey(d.key, &variant);
  // Not a pipeline of the host MSAA level's (a level change does not remake it): one sample, no depth.
  PipelineInputs variant_inputs = inputs;
  variant_inputs.msaa_guest_samples = 0;
  // A pipeline nobody made yet is built in the background: the second draw waits for it (the frame has
  // no mask), the game's own draw does not.
  const Deferral deferral{&Scene::AlwaysMaySkip, nullptr};
  bool pending = false;
  d.shadow_pipeline = pipelines_->Get(variant, variant_inputs, &deferral, &pending);
}

void Scene::RecordShadowMask(const PreparedDraw& d, plume::RenderCommandList* list) {
  ShadowMask& mask = temporal_->shadow_mask();
  bool clear = false;
  HostImage* image = mask.Target(uint32_t(std::lround(d.viewport.viewport.width)),
                                 uint32_t(std::lround(d.viewport.viewport.height)), &clear);
  if (!image) {
    return;
  }
  // The layout change happens outside the game's render pass (the first draw of a frame: the image was
  // left readable by the last frame's pass).
  if (image->layout != plume::RenderTextureLayout::COLOR_WRITE) {
    list->setFramebuffer(nullptr);
    list->barriers(plume::RenderBarrierStage::GRAPHICS,
                   plume::RenderTextureBarrier(image->texture.get(),
                                               plume::RenderTextureLayout::COLOR_WRITE));
    image->layout = plume::RenderTextureLayout::COLOR_WRITE;
  }
  list->setFramebuffer(image->framebuffer.get());
  if (clear) {
    // Zero opacity everywhere: the frame's first decal, or a new image (a new colour target is also
    // initialised before it is drawn into, D3D12 debug layer id 1422).
    list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
  }
  // Everything the game's draw bound, set again: the list's own state is not trusted across the
  // framebuffer switch, and the list state is forgotten afterwards (ResetListState below).
  list->setGraphicsPipelineLayout(layout_->layout());
  list->setGraphicsDescriptorSet(heap_->texture_set(), 0);
  list->setGraphicsDescriptorSet(heap_->texture_set(), 1);
  list->setGraphicsDescriptorSet(heap_->texture_set(), 2);
  list->setGraphicsDescriptorSet(heap_->sampler_set(), 3);
  list->setPipeline(d.shadow_pipeline);
  if (ActiveBackend() == Backend::kD3D12) {
    list->setGraphicsRootDescriptor(d.vs_constants.reference(), GuestLayout::kVertexConstants);
    list->setGraphicsRootDescriptor(d.ps_constants.reference(), GuestLayout::kPixelConstants);
    list->setGraphicsRootDescriptor(d.shared_constants.reference(), GuestLayout::kSharedConstants);
  } else {
    const uint64_t addresses[3] = {d.vs_constants.device_address, d.ps_constants.device_address,
                                   d.shared_constants.device_address};
    list->setGraphicsPushConstants(0, addresses, 0, sizeof(addresses));
  }
  list->setViewports(&d.viewport.viewport, 1);
  list->setScissors(&d.scissor, 1);
  if (pipelines_->dynamic_depth_bias()) {
    list->setDepthBias(0.0f, 0.0f, 0.0f);  // no depth target: the guest's bias does not apply
  }
  const DrawGeometry& g = d.geometry;
  if (g.slot_count) {
    list->setVertexBuffers(0, g.vertex_views.data(), g.slot_count, d.input_layout->slots.data());
  }
  if (g.indexed) {
    list->setIndexBuffer(&g.index_view);
    list->drawIndexedInstanced(g.count, 1, g.first, g.base_vertex, 0);
  } else {
    list->drawInstanced(g.count, 1, g.first, 0);
  }
  // The pass ends here: the next draw of the game opens its own.
  list->setFramebuffer(nullptr);
  ResetListState();
  marked_framebuffer_ = nullptr;
  mask.Drawn();
}

// ---------------------------------------------------------------------------
// draws
// ---------------------------------------------------------------------------

void Scene::PrefetchTextures(uint64_t sequence, const uint8_t* payload) {
  if (!textures_ || !shaders_.attached()) {
    return;
  }
  const DrawView view(payload);
  const DrawRecord& r = view.record();
  if (!prefetch_vs_ || prefetch_vs_identity_ != r.vs_identity) {
    prefetch_vs_ = shaders_.Find(r.vs_identity);
    prefetch_vs_identity_ = r.vs_identity;
  }
  if (!prefetch_vs_) {
    return;
  }
  uint32_t fetch_mask = prefetch_vs_->texture_fetch_mask;
  if ((r.flags & kDrawNoPixelShader) == 0) {
    if (!prefetch_ps_ || prefetch_ps_identity_ != r.ps_identity) {
      prefetch_ps_ = shaders_.Find(r.ps_identity);
      prefetch_ps_identity_ = r.ps_identity;
    }
    if (prefetch_ps_) {
      fetch_mask |= prefetch_ps_->texture_fetch_mask;
    }
  }
  for (uint32_t mask = fetch_mask; mask != 0; mask &= mask - 1) {
    uint32_t dwords[6];
    view.FetchGroup(uint32_t(std::countr_zero(mask)), dwords);
    textures_->Prefetch(dwords, sequence);
  }
}

void Scene::OnDraw(const uint8_t* payload, plume::RenderCommandList* list) {
  const uint64_t c0 = cycles::Now();
  LapStart();
  const DrawView view(payload);
  const DrawRecord& r = view.record();
  ++window_.draws;
  if (watch::Active()) {
    watch::OnDraw(r.ps_identity);
  }
  if (draw_census_.active()) {
    draw_census_.OnDraw(r.ps_identity, r.count);
  }

  ConstantBanks::Check check;
  const ConstantBanks::Banks& banks = banks_.Apply(view, &check);
  Lap(Stage::kBanks);
  if (check.done) {
    ++window_.bank_checks;
    if (!check.vs_matches || !check.ps_matches) {
      ++window_.bank_mismatches;
      if (mismatches_logged_ < 8) {
        ++mismatches_logged_;
        REXLOG_ERROR("[gpu] scene: the rebuilt {}{}{} bank of device {:#010x} differs from the "
                     "guest's own hash (draws keep using the rebuilt bank)",
                     check.vs_matches ? "" : "VS", (!check.vs_matches && !check.ps_matches) ? "+" : "",
                     check.ps_matches ? "" : "PS", r.device);
      }
    }
  }

  // The scene phase of the frame ends at the game's motion blur draw (the anchor), which comes
  // after the main view's last draw (EdramTargets::DepthLayer).
  if (r.ps_identity == temporal::kAnchorPs && r.vs_identity == temporal::kAnchorVs) {
    edram_->SetScenePhase(false);
    if (ao_ && ao_->wants_draws()) {
      ao_->OnAnchor(banks);  // this frame's camera: the next frame's occlusion uses its lens
    }
  }

  if (!list) {
    CountSkip(Skip::kDeviceLost, r, "");
  } else {
    PreparedDraw d;
    bool no_effect = false;
    NoEffect no_effect_reason = NoEffect::kNoTarget;
    std::string detail;
    const Skip skip = Prepare(view, banks, d, &no_effect, &no_effect_reason, &detail);
    if (no_effect) {
      ++window_.no_effect[size_t(no_effect_reason)];
    } else if (skip != Skip::kCount) {
      CountSkip(skip, r, detail);
    } else {
      ++window_.prepared;
      if (ao_ && ao_->wants_draws()) {
        NoteAmbientOcclusion(banks, d);
      }
      if (d.ui_layer != FrameLayers::DrawAction::kNone) {
        UiLayersBeforeDraw(d.ui_layer, list);
      }
      bool empty = false;
      const Skip recorded = Record(d, r, list, &empty, &detail);
      if (empty) {
        ++window_.no_effect[size_t(NoEffect::kEmptyScissor)];
      } else if (recorded != Skip::kCount) {
        CountSkip(recorded, r, detail);
      } else {
        ++window_.recorded;
        ++draws_since_flush_;
        if (d.hdr_action != HdrToneMap::Action::kNone) {
          RecordHdrDuplicate(d, list);
        }
        if (d.shadow_pipeline) {
          RecordShadowMask(d, list);
        }
      }
    }
  }
  window_.prepare_cycles += cycles::Now() - c0;
}

Scene::Skip Scene::Prepare(const DrawView& view, const ConstantBanks::Banks& banks,
                           PreparedDraw& d, bool* no_effect, NoEffect* no_effect_reason,
                           std::string* detail) {
  const DrawRecord& r = view.record();
  if (r.flags & kDrawVsUnreadable) return Skip::kVsUnreadable;
  if (r.flags & kDrawPsUnreadable) return Skip::kPsUnreadable;
  if (r.flags & (kDrawDeclUnreadable | kDrawDeclTooLong)) return Skip::kDeclUnreadable;
  if (r.kind == DrawKind::kIndexed && (r.flags & kDrawIndexUnreadable)) {
    return Skip::kIndexUnreadable;
  }
  if (r.kind == DrawKind::kInline) {
    if (r.flags & kDrawInlineUnreadable) return Skip::kInlineUnreadable;
    if (r.flags & kDrawInlineUnterminated) return Skip::kInlineUnterminated;
  }

  // The state: the last draw's when its window R and surfaces repeat (M6a.7).
  StateMemo& memo = state_memo_;
  ++memo_tries_[size_t(Memo::kState)];
  const bool state_same = memo.valid && r.device == memo.device &&
                          std::memcmp(r.registers, memo.registers, sizeof(r.registers)) == 0 &&
                          std::memcmp(r.surfaces, memo.surfaces, sizeof(r.surfaces)) == 0;
  if (state_same) {
    ++memo_hits_[size_t(Memo::kState)];
    d.state = memo.state;
  } else {
    d.state = DecodeXenosState(view);
    memo.valid = true;
    memo.device = r.device;
    std::memcpy(memo.registers, r.registers, sizeof(r.registers));
    std::memcpy(memo.surfaces, r.surfaces, sizeof(r.surfaces));
    memo.state = d.state;
    memo.formats_valid = false;
    memo.key_valid = false;
    memo.draw_scale_valid = false;
    memo.constants_valid = false;
  }
  const XenosState& s = d.state;

  d.primitive = HostPrimitiveFor(r.primitive);
  if (d.primitive.conversion == PrimitiveConversion::kUnsupported) {
    *detail = PrimitiveName(r.primitive);
    return Skip::kPrimitive;
  }

  // Targets. Depth only: no colour target and no pixel shader (pipelines.h).
  const bool depth_only = s.mode.edram_mode == xe::EdramMode::kDepthOnly;
  bool any_color = false;
  if (!depth_only) {
    for (const ColorTargetState& t : s.color) {
      any_color |= t.bound;
    }
  }
  if (!any_color && !s.depth_bound) {
    *no_effect = true;
    *no_effect_reason = NoEffect::kNoTarget;
    return Skip::kCount;
  }
  plume::RenderFormat color_formats[kColorTargetCount] = {};
  plume::RenderFormat depth_format = plume::RenderFormat::UNKNOWN;
  if (state_same && memo.formats_valid) {
    std::memcpy(color_formats, memo.color_formats, sizeof(color_formats));
    depth_format = memo.depth_format;
  } else {
    if (!depth_only) {
      for (uint32_t i = 0; i < kColorTargetCount; ++i) {
        if (!s.color[i].bound) {
          continue;
        }
        const format::Mapping m = format::ColorTargetFormat(uint32_t(s.color[i].format));
        if (m.status != format::Status::kOk) {
          *detail = std::format("RT{} {} ({})", i, m.guest_name, format::StatusName(m.status));
          return Skip::kTargetFormat;
        }
        color_formats[i] = m.format;
      }
    }
    if (s.depth_bound) {
      const format::Mapping m = format::DepthTargetFormat(uint32_t(s.depth_format));
      if (m.status != format::Status::kOk) {
        *detail = std::format("depth {} ({})", m.guest_name, format::StatusName(m.status));
        return Skip::kTargetFormat;
      }
      depth_format = m.format;
    }
    std::memcpy(memo.color_formats, color_formats, sizeof(color_formats));
    memo.depth_format = depth_format;
    memo.formats_valid = true;
  }
  Lap(Stage::kDecode);

  // Shaders (the last draw's when the identity repeats, M6a.3).
  ++memo_tries_[size_t(Memo::kVertexShader)];
  if (vs_memo_ && r.vs_identity == vs_memo_identity_) {
    ++memo_hits_[size_t(Memo::kVertexShader)];
    d.vs = vs_memo_;
  } else {
    d.vs = shaders_.Find(r.vs_identity);
    if (d.vs) {
      vs_memo_identity_ = r.vs_identity;
      vs_memo_ = d.vs;
    }
  }
  if (!d.vs) {
    *detail = std::format("{:016X}", r.vs_identity);
    return Skip::kVsNotInPack;
  }
  if (d.vs->stage != ShaderStore::Stage::kVertex) {
    *detail = std::format("VS {:016X}", r.vs_identity);
    return Skip::kStageMismatch;
  }
  if (!depth_only && (r.flags & kDrawNoPixelShader) == 0) {
    ++memo_tries_[size_t(Memo::kPixelShader)];
    if (ps_memo_ && r.ps_identity == ps_memo_identity_) {
      ++memo_hits_[size_t(Memo::kPixelShader)];
      d.ps = ps_memo_;
    } else {
      d.ps = shaders_.Find(r.ps_identity);
      if (d.ps) {
        ps_memo_identity_ = r.ps_identity;
        ps_memo_ = d.ps;
      }
    }
    if (!d.ps) {
      *detail = std::format("{:016X}", r.ps_identity);
      return Skip::kPsNotInPack;
    }
    if (d.ps->stage != ShaderStore::Stage::kPixel) {
      *detail = std::format("PS {:016X}", r.ps_identity);
      return Skip::kStageMismatch;
    }
  }

  // Input layout. Inline vertices carry their stride in the record (the
  // device's stride table is zero for them). A stride the
  // table's byte (dwords) cannot hold is left out here and refused by the
  // geometry (GeometryCache::Result::kInlineLayout), never drawn with 0.
  uint8_t strides[guest::kStreamCount];
  std::memcpy(strides, r.stream_strides, sizeof(strides));
  if (r.kind == DrawKind::kInline && r.inline_stride % 4 == 0 && r.inline_stride / 4 <= 0xFF) {
    for (uint32_t k = 0; k < r.decl_count; ++k) {
      const nfx::DeclarationElement e = nfx::ReadDeclarationElement(view.decl_records(), k);
      if (e.stream == 0xFF) {
        break;
      }
      if (e.stream < guest::kStreamCount) {
        strides[e.stream] = uint8_t(r.inline_stride / 4);
      }
    }
  }
  d.input_layout = input_layouts_.Get(*d.vs, view.decl_records(), r.decl_count, strides);
  if (d.input_layout->problems != 0) {
    *detail = std::format("VS {:016X}, {} problem(s)", r.vs_identity, d.input_layout->problems);
    return Skip::kInputLayout;
  }
  Lap(Stage::kShaders);

  // Geometry: vertex and index data, and the final host topology (an indexed
  // strip whose restart differs from the host's becomes a list).
  const bool computed_fetch = (d.vs->flags & ShaderStore::kComputedFetchIndex) != 0;
  const GeometryCache::Result geometry = geometry_->Prepare(
      view, s, *d.input_layout, d.primitive, computed_fetch, d.geometry, detail, list_);
  if (d.geometry.list_state_changed) {
    // A stream built on the GPU recorded its compute pass: the
    // draw binds everything again.
    ResetListState();
    marked_framebuffer_ = nullptr;
  }
  if (geometry != GeometryCache::Result::kOk) {
    ++window_.geometry_results[size_t(geometry)];
    if (geometry == GeometryCache::Result::kEmpty) {
      *no_effect = true;
      *no_effect_reason = NoEffect::kNoPrimitive;
      return Skip::kCount;
    }
    *detail = std::string(GeometryCache::ResultName(geometry)) +
              (detail->empty() ? std::string() : ": " + *detail);
    return Skip::kGeometry;
  }
  Lap(Stage::kGeometry);

  // Pipeline. The key is the last draw's for the same state, shaders, layout
  // and topology (M6a.7); BuildKey reads nothing else.
  PipelineInputs inputs{d.vs, d.ps, d.input_layout};
  const bool polygonal = IsPolygonal(r.primitive);
  KeyResult key_result;
  if (state_same && memo.key_valid && memo.key_vs == d.vs && memo.key_ps == d.ps &&
      memo.key_layout == d.input_layout && memo.key_topology == d.geometry.topology &&
      memo.key_polygonal == polygonal) {
    d.key = memo.key;
    d.bias = memo.bias;
    key_result = KeyResult::kOk;
  } else {
    key_result = pipelines_->BuildKey(s, inputs, d.geometry.topology, polygonal, color_formats,
                                      depth_format, d.key, d.bias);
    if (key_result == KeyResult::kOk) {
      memo.key_valid = true;
      memo.key_vs = d.vs;
      memo.key_ps = d.ps;
      memo.key_layout = d.input_layout;
      memo.key_topology = d.geometry.topology;
      memo.key_polygonal = polygonal;
      memo.key = d.key;
      memo.bias = d.bias;
    }
  }
  switch (key_result) {
    case KeyResult::kOk:
      break;
    case KeyResult::kNoEffect:
      *no_effect = true;
      *no_effect_reason = NoEffect::kCulled;
      return Skip::kCount;
    case KeyResult::kEdramMode:
      *detail = std::format("mode {}", uint32_t(s.mode.edram_mode));
      return Skip::kEdramMode;
    case KeyResult::kColorGap:
      return Skip::kColorGap;
  }
  // The scale of the draw's targets (the last draw's for the same state while
  // no target was made or grown since), and the slope-scaled depth
  // bias in its host pixels (M6b.3c): a host pixel is 1/scale of the guest's,
  // so is the depth slope across it -- Xenia multiplies the slope factor by
  // the scale. The constant part is in depth units and stays.
  Scale scale;
  uint32_t host_samples;
  bool scene_draw;
  uint32_t msaa_guest;
  if (state_same && memo.draw_scale_valid && memo.draw_scale_serial == edram_->change_serial() &&
      memo.draw_scale_colors == d.key.color_count &&
      memo.draw_scale_depth_format == uint32_t(d.key.depth_format)) {
    scale = memo.draw_scale;
    host_samples = memo.draw_samples;
    scene_draw = memo.draw_scene;
    msaa_guest = memo.draw_msaa_guest;
  } else {
    scale = DrawScale(*edram_, s, d.key, r.target_size_bits, &host_samples, &scene_draw,
                      &msaa_guest);
    memo.draw_scale_valid = true;
    memo.draw_scale_serial = edram_->change_serial();
    memo.draw_scale_colors = d.key.color_count;
    memo.draw_scale_depth_format = uint32_t(d.key.depth_format);
    memo.draw_scale = scale;
    memo.draw_samples = host_samples;
    memo.draw_scene = scene_draw;
    memo.draw_msaa_guest = msaa_guest;
  }
  inputs.msaa_guest_samples = msaa_guest;  // Pipelines::Get remembers the pipeline for a level change
  d.scale = scale;
  d.scene = scene_draw;
  // The pipeline rasterizes at the targets' host samples (M6d.2).
  d.key.sample_count = host_samples;
  // The UI layer split (frame_layers.h): a draw into the UI target after the composite marker
  // gets the alpha blend that collects coverage, in the key only (the memo above keeps the game's).
  if (layers_ && layers_->ui_phase() && d.key.color_count != 0) {
    d.ui_layer = layers_->ClassifyDraw(ColorEdramKey(s, 0), host_samples, r.ps_identity, d.key);
  }
  if (!scale.unit() && d.bias.slope != 0.0f && scale_depth_bias_) {
    d.bias.slope *= scale.value();
    if (!pipelines_->dynamic_depth_bias()) {
      d.key.slope_scaled_depth_bias = d.bias.slope;
    }
  }
  // The last draw's pipeline when the key repeats (M6a.3); the key names the
  // shaders, their variants and the input layout.
  ++memo_tries_[size_t(Memo::kPipeline)];
  if (pipeline_memo_ && d.key == pipeline_memo_key_) {
    ++memo_hits_[size_t(Memo::kPipeline)];
    d.pipeline = pipeline_memo_;
  } else {
    // A miss may leave the draw out while a builder thread makes the pipeline.
    const SkipAsk ask{this, &d};
    const Deferral deferral{&Scene::MaySkipForPipeline, &ask};
    bool pending = false;
    d.pipeline = pipelines_->Get(d.key, inputs, &deferral, &pending);
    if (d.pipeline) {
      pipeline_memo_key_ = d.key;
      pipeline_memo_ = d.pipeline;
    } else if (pending) {
      // Not memoized: the next draw with the key asks again, and takes the
      // pipeline once it is ready.
      NoteTargetsDrawn(d);
      return Skip::kPipelinePending;
    }
  }
  if (!d.pipeline) {
    *detail = std::format("VS {:016X} PS {:016X}", d.key.vs, d.key.ps);
    return Skip::kPipeline;
  }
  Lap(Stage::kPipeline);

  // Viewport and constants.
  const bool ps_writes_depth = d.ps && (d.ps->ps_outputs & ShaderStore::kOutputDepth) != 0;
  SharedConstants shared;
  uint32_t issues = 0;
  if (state_same && memo.constants_valid && memo.ps_writes_depth == ps_writes_depth &&
      memo.scale == scale && memo.samples == host_samples) {
    d.viewport = memo.viewport;
    shared = memo.shared;
  } else {
    // At the scale of the draw's targets (DrawScale above).
    d.viewport = ComputeViewport(s, ps_writes_depth, scale);
    shared = base_shared_;
    issues = FillSharedConstants(s, d.viewport, host_samples, shared);
    // The pixel position a pixel shader receives, in guest pixels (v7).
    shared.inv_resolution_scale[0] = 1.0f / scale.value();
    shared.inv_resolution_scale[1] = 1.0f / scale.value();
    memo.constants_valid = true;
    memo.ps_writes_depth = ps_writes_depth;
    memo.scale = scale;
    memo.samples = host_samples;
    memo.viewport = d.viewport;
    memo.shared = shared;
  }
  if (issues & ~constants_issues_reported_) {
    const uint32_t fresh = issues & ~constants_issues_reported_;
    constants_issues_reported_ |= issues;
    if (fresh & kIssueMinMaxWithExpBias) {
      REXLOG_ERROR("[gpu] scene: a MIN/MAX blend with a source factor on RT0 together with an "
                   "exponent bias; one premult mode cannot carry both (the bias is applied) "
                   "(reported once)");
    }
    if (fresh & kIssueExpBiasBeyondRt0) {
      REXLOG_ERROR("[gpu] scene: an exponent bias on RT1-3; only RT0's reaches the shader "
                   "(reported once)");
    }
  }
  if (temporal_ && temporal_->active()) {
    temporal_->BeforeTextures(r);  // the anchor draw's textures are bound without the stand-in
  }
  // Ultrawide: the UI of a widened picture is drawn squeezed, in the middle (UiSqueezeOf). After the
  // memo above, which keeps the game's own position.
  d.ui_squeeze = UiSqueezeOf(view, s, d.key, d.input_layout, d.primitive.conversion);
  if (d.ui_squeeze < 1.0f) {
    SqueezeSharedX(shared, d.viewport.viewport, scale.Edge(s.surface_pitch), d.ui_squeeze);
  }
  Lap(Stage::kConstants);
  // HDR stage 3: the tone map, and what the game draws into its target before the marker, are drawn
  // again into the float scene image (hdr_tonemap.h).
  if (hdr_tone_ && hdr_tone_->active() && d.key.color_count != 0 && hdr_tone_->Wants(d.ps)) {
    PrepareHdrDraw(d, inputs, banks, host_samples);
  }
  // A car's shadow decal is drawn again into the mask the upscaler takes as its reactive input
  // (vehicle_motion_mask.h).
  if (d.key.color_count == 1 && temporal_ && temporal_->active() &&
      ShadowMask::IsDecal(r.vs_identity, r.ps_identity)) {
    PrepareShadowMask(d, inputs);
  }
  const uint32_t fetch_mask = d.vs->texture_fetch_mask | (d.ps ? d.ps->texture_fetch_mask : 0u);
  FillTextureTables(view, fetch_mask, scale, shared, d.scene ? SceneBias() : 0);
  Lap(Stage::kTextures);
  if (temporal_ && temporal_->active()) {
    NoteTemporal(view, banks, d, shared);
  }
  if (!UploadConstants(r.device, banks, shared, d)) {
    return Skip::kUpload;
  }
  if (d.hdr_action == HdrToneMap::Action::kToneMap) {
    // The tone map's second draw reads 16 bytes after the shared constants: its own copy of them.
    const UploadSpan span = upload_->Allocate(
        slot_, kHdrSceneConstantsOffset + sizeof(HdrSceneConstants), kConstantAlignment);
    if (span) {
      std::memcpy(span.cpu, &shared, sizeof(shared));
      std::memcpy(span.cpu + kHdrSceneConstantsOffset, &hdr_tone_->constants(),
                  sizeof(HdrSceneConstants));
      d.hdr_shared = span;
    } else {
      hdr_tone_->Failed(d.hdr_action, HdrToneMap::Cause::kUpload, false, false);
      d.hdr_action = HdrToneMap::Action::kNone;
      d.hdr_pipeline = nullptr;
    }
  }
  Lap(Stage::kUpload);

  ++window_.conversions[size_t(d.primitive.conversion)];
  if (d.key.ps_variant) {
    ++window_.alpha_coverage_draws;
    if (ComputeAlphaTest(s).discard) ++window_.alpha_test_draws;
  }
  if (s.clip.clip_disable) ++window_.clip_disabled_draws;
  if (d.bias.constant != 0 || d.bias.slope != 0.0f) ++window_.depth_bias_draws;
  if (s.color_control.alpha_to_mask_enable && d.key.color_count > 0 && d.ps &&
      (d.ps->ps_outputs & ShaderStore::kOutputColor0) != 0) {
    ++window_.alpha_to_mask_draws;
    if (d.key.sample_count == 1) ++window_.alpha_to_mask_1x;
    if (s.color[0].exp_bias != 0) ++window_.alpha_to_mask_biased;
    if (AlphaToMaskOffsetsReplaced(s, d.key.sample_count)) {
      ++window_.alpha_to_mask_replaced;
      const uint32_t offsets = s.color_control.value >> 24;
      const bool solid = d.key.sample_count <= 1;
      uint64_t& logged = alpha_to_mask_replaced_logged_[(offsets >> 6) + (solid ? 4 : 0)];
      if (!(logged & (uint64_t(1) << (offsets & 63)))) {
        logged |= uint64_t(1) << (offsets & 63);
        REXLOG_INFO("[gpu] alpha to mask: the game's offsets {:#04x} ({}, {}, {}, {}) take the "
                    "{} (mcla_gpu_a2c_dither_all, {} sample(s))",
                    offsets, offsets & 3, (offsets >> 2) & 3, (offsets >> 4) & 3, offsets >> 6,
                    solid ? "solid 2, 2, 2, 2" : "dithered 3, 1, 0, 2", d.key.sample_count);
      }
    }
  }
  return Skip::kCount;
}

bool Scene::MaySkipForPipeline(const void* context) {
  const SkipAsk& ask = *static_cast<const SkipAsk*>(context);
  const Scene& scene = *ask.scene;
  const PreparedDraw& d = *ask.draw;
  const XenosState& s = d.state;
  // Multisampled targets only. A target is known by its EDRAM identity (base,
  // pitch, format, samples), and a one-off render to texture (a car's paint and
  // vinyls, made once and kept) could share that identity with a pass drawn
  // every frame; those are single-sampled, while the passes whose misses this
  // is for (the scene, 2x; the sun's shadow map, 4x; the reflections, 4x) are
  // not. A skipped draw into a target that is not redrawn would stay wrong.
  if (MsaaSampleCount(s.msaa) < 2) {
    return false;
  }
  bool any = false;
  for (uint32_t i = 0; i < d.key.color_count; ++i) {
    if (!scene.edram_->DrawnEveryFrame(ColorEdramKey(s, i), scene.frame_serial_,
                                       kPipelineSkipFrames)) {
      return false;
    }
    any = true;
  }
  if (d.key.depth_format != 0) {
    if (!scene.edram_->DrawnEveryFrame(DrawDepthKey(*scene.edram_, s, d.key.color_count),
                                       scene.frame_serial_, kPipelineSkipFrames)) {
      return false;
    }
    any = true;
  }
  return any;
}

void Scene::NoteTargetsDrawn(const PreparedDraw& d) {
  const XenosState& s = d.state;
  for (uint32_t i = 0; i < d.key.color_count; ++i) {
    edram_->NoteDrawn(ColorEdramKey(s, i), frame_serial_);
  }
  if (d.key.depth_format != 0) {
    edram_->NoteDrawn(DrawDepthKey(*edram_, s, d.key.color_count), frame_serial_);
  }
}

bool Scene::UpscalerRunning() const {
  return temporal_ && temporal_->active() && temporal_->frame().armed &&
         temporal_->frame().upscaler != Upscaler::kOff;
}

void Scene::NoteAmbientOcclusion(const ConstantBanks::Banks& banks, const PreparedDraw& d) {
  EdramKey color;
  EdramKey depth;
  const EdramKey* color0 = nullptr;
  const EdramKey* depth0 = nullptr;
  if (d.key.color_count != 0) {
    color = ColorEdramKey(d.state, 0);
    color0 = &color;
  }
  if (d.key.depth_format != 0) {
    depth = DrawDepthKey(*edram_, d.state, d.key.color_count);
    depth0 = &depth;
  }
  AmbientOcclusion::MainDraw draw;
  draw.match = temporal::MatchMainView(color0, depth0);
  if (!draw.match) {
    return;
  }
  draw.writes_depth = (d.key.depth & 2u) != 0;
  ao_->OnMainDraw(draw, banks);
}

void Scene::NoteTemporal(const DrawView& view, const ConstantBanks::Banks& banks,
                         const PreparedDraw& d, SharedConstants& shared) {
  TemporalSystem::DrawFacts f;
  f.view = &view;
  f.state = &d.state;
  f.banks = &banks;
  f.ps = d.ps;
  f.viewport = &d.viewport;
  f.scale = d.scale;
  EdramKey color;
  EdramKey depth;
  if (d.key.color_count != 0) {
    color = ColorEdramKey(d.state, 0);
    f.color0 = &color;
  }
  if (d.key.depth_format != 0) {
    depth = DrawDepthKey(*edram_, d.state, d.key.color_count);
    f.depth = &depth;
  }
  if (temporal_->OnDraw(f, shared)) {
    // The anchor was taken: its passes (the scene's depth kept, the motion vectors) read products
    // the main view's resolves made, so the open render pass ends first.
    if (list_ && bound_.framebuffer) {
      list_->setFramebuffer(nullptr);
      bound_.framebuffer = nullptr;
    }
    if (temporal_->RecordAnchorPasses(list_)) {
      // They bound their own layout, pipelines and framebuffers (and an upscaler its own heaps and
      // pipeline state): the draw binds everything again. With an upscaler running, the anchor's own
      // colour fetch reads its output.
      ResetListState();
      marked_framebuffer_ = nullptr;
      temporal_->PatchAnchor(shared);
    }
  }
}

void Scene::ApplyFetch(const FetchMemo& m, uint32_t slot, SharedConstants& sc) {
  if (!m.is_texture) {
    return;
  }
  sc.sampler[slot] = m.sampler;
  switch (xe::DataDimension(m.dimension)) {
    case xe::DataDimension::k3D:
      sc.texture_3d[slot] = m.texture;
      break;
    case xe::DataDimension::kCube:
      sc.texture_cube[slot] = m.texture;
      break;
    default:  // k1D (refused by the cache: the 2D null), k2DOrStacked
      sc.texture_2d[slot] = m.texture;
      break;
  }
}

int32_t Scene::SceneBias() {
  if (scene_bias_serial_ != frame_serial_) {
    scene_bias_serial_ = frame_serial_;
    scene_bias_ = 0;
    if (edram_->SceneApart()) {
      // The scene's textures pick their mip levels from a picture smaller than the output: the
      // levels the output's size would have picked are log2(render / output) lower. One more
      // level on top is what AMD's guide recommends for FSR. A fetch constant's own bias is added
      // to it by the sampler.
      constexpr double kExtraBias = -1.0;
      const double render = edram_->SceneScale().value();
      const double output = edram_->resolution_scale().value();
      const double bias = std::log2(render / output) + kExtraBias;
      scene_bias_ = int32_t(std::lround(std::clamp(bias, -3.9, 3.9) * 32.0));
    }
  }
  return scene_bias_;
}

void Scene::FillTextureTables(const DrawView& view, uint32_t fetch_mask, Scale scale,
                              SharedConstants& sc, int32_t bias) {
  // Write notifications first: a memoized binding must not outlive a write.
  textures_->Sync();
  for (uint32_t mask = fetch_mask; mask != 0; mask &= mask - 1) {
    const uint32_t slot = uint32_t(std::countr_zero(mask));
    uint32_t dwords[6];
    view.FetchGroup(slot, dwords);
    const auto holds = [&](const FetchMemo& m) {
      return m.epoch == textures_->epoch() && (m.frame == 0 || m.frame == frame_serial_) &&
             m.generation == textures_->ProductGeneration(m.page) && m.scale == scale &&
             m.bias == bias && std::memcmp(m.dwords, dwords, sizeof(dwords)) == 0;
    };
    // The slot's last entry first (M6a.10), then the set.
    FetchMemo* hit = nullptr;
    FetchMemo* victim = nullptr;
    if (last_fetch_[slot] && holds(*last_fetch_[slot])) {
      hit = last_fetch_[slot];
    } else {
      uint64_t h = 0;
      for (uint32_t d : dwords) {
        h = (h ^ d) * 0x9E3779B97F4A7C15ull;
      }
      FetchMemo* set = &fetch_memo_[(size_t(h >> 40) & (kFetchMemoSets - 1)) * kFetchMemoWays];
      victim = set;
      for (size_t way = 0; way < kFetchMemoWays; ++way) {
        FetchMemo& candidate = set[way];
        if (holds(candidate)) {
          hit = &candidate;
          break;
        }
        if (candidate.used < victim->used) {
          victim = &candidate;
        }
      }
    }
    if (hit) {
      if (watch::Active() && hit->is_texture) {
        const TextureFetch fetch = ReadTextureFetch(dwords);
        if (watch::TouchesKey(fetch.key)) {
          watch::Log(std::format("fetch s{} ps {:016x} {} -> memoized texture slot {}", slot,
                                 watch::CurrentPs(), fetch.key.Describe(), hit->texture));
        }
      }
      last_fetch_[slot] = hit;
      hit->used = ++fetch_memo_clock_;
      ++window_.fetch_memo_hits;
      if (hit->is_texture) {
        ++window_.fetches;
      } else {
        ++window_.fetch_not_texture;
      }
      ApplyFetch(*hit, slot, sc);
      continue;
    }
    FetchMemo& m = *victim;
    const TextureFetch fetch = ReadTextureFetch(dwords);
    FetchMemo fresh;
    std::memcpy(fresh.dwords, dwords, sizeof(dwords));
    fresh.is_texture = fetch.is_texture;
    if (!fetch.is_texture) {
      // The shader samples a slot the guest left without a texture: the null
      // texture is read, as an invalid fetch reads zeros.
      ++window_.fetch_not_texture;
    } else {
      ++window_.fetches;
      // Only 2D textures are made (100% of the measured fetches);
      // the cache logs any other dimension once and the null of its dimension
      // is bound.
      const TextureCache::Binding bound = textures_->Bind2D(fetch, scale, list_);
      if (census_scale_ && bound.from_product && !bound.product_scale.none() &&
          bound.product_scale != scale) {
        ++window_.census_scale_reads;
        NoteScaleRead(census_scale_logged_, fetch, dwords, bound, scale);
      }
      // A texture mod's replacement has its own size and mips: the fetch's
      // range moves by the size ratio and stays inside the replacement's chain.
      uint32_t mip_min = fetch.mip_min;
      uint32_t mip_max = fetch.mip_max;
      if (bound.replaced) {
        const int32_t top = int32_t(bound.levels) - 1;
        mip_min = uint32_t(std::clamp(int32_t(mip_min) + bound.lod_shift, 0, top));
        mip_max = uint32_t(std::clamp(int32_t(mip_max) + bound.lod_shift, 0, top));
      }
      fresh.sampler = samplers_->Get(dwords, mip_min, mip_max, bias);
      if (watch::Active() && watch::TouchesKey(fetch.key)) {
        watch::Log(std::format("fetch s{} ps {:016x} {} -> texture slot {}{}{}", slot,
                               watch::CurrentPs(), fetch.key.Describe(), bound.slot,
                               bound.from_product ? " (product or resolve memory)" : "",
                               bound.per_frame ? ", per frame" : ""));
      }
      if (bound.list_state_changed) {
        // A decode from the resolve memory drew into its own target with its
        // own pipeline state: the draw binds everything again.
        ResetListState();
        marked_framebuffer_ = nullptr;
      }
      const uint32_t gamma_bit = fetch.gamma ? kTextureGammaBit : 0u;
      fresh.dimension = fetch.key.dimension;
      switch (xe::DataDimension(fetch.key.dimension)) {
        case xe::DataDimension::k3D:
          fresh.texture = kNullTexture3DSlot | gamma_bit;
          break;
        case xe::DataDimension::kCube:
          fresh.texture = kNullTextureCubeSlot | gamma_bit;
          break;
        default:
          fresh.texture = bound.slot;
          break;
      }
      fresh.frame = bound.per_frame ? frame_serial_ : 0;
      fresh.page = fetch.key.base_page ? fetch.key.base_page : fetch.key.mip_page;
      fresh.generation = textures_->ProductGeneration(fresh.page);
    }
    fresh.epoch = textures_->epoch();
    fresh.scale = scale;
    fresh.bias = bias;
    fresh.used = ++fetch_memo_clock_;
    m = fresh;
    last_fetch_[slot] = &m;
    ApplyFetch(m, slot, sc);
  }
}

bool Scene::UploadConstants(uint32_t device, const ConstantBanks::Banks& banks,
                            const SharedConstants& shared, PreparedDraw& d) {
  DeviceUpload& u = device_uploads_[device];
  if (u.vs_version != banks.vs_version) {
    UploadSpan span = upload_->Allocate(slot_, guest::kConstantBankBytes, kConstantAlignment);
    if (!span) {
      return false;
    }
    SwapBankToHost(banks.vs.data(), span.cpu);
    u.vs = span;
    u.vs_version = banks.vs_version;
    ++window_.constant_uploads;
    window_.upload_bytes += guest::kConstantBankBytes;
  }
  if (u.ps_version != banks.ps_version) {
    UploadSpan span = upload_->Allocate(slot_, guest::kConstantBankBytes, kConstantAlignment);
    if (!span) {
      return false;
    }
    SwapBankToHost(banks.ps.data(), span.cpu);
    u.ps = span;
    u.ps_version = banks.ps_version;
    ++window_.constant_uploads;
    window_.upload_bytes += guest::kConstantBankBytes;
  }
  if (!last_shared_span_ || std::memcmp(&last_shared_, &shared, sizeof(shared)) != 0) {
    UploadSpan span = upload_->Allocate(slot_, sizeof(SharedConstants), kConstantAlignment);
    if (!span) {
      return false;
    }
    std::memcpy(span.cpu, &shared, sizeof(shared));
    last_shared_ = shared;
    last_shared_span_ = span;
    ++window_.shared_uploads;
    window_.upload_bytes += sizeof(SharedConstants);
  }
  d.vs_constants = u.vs;
  d.ps_constants = u.ps;
  d.shared_constants = last_shared_span_;
  return true;
}

uint32_t Scene::NoteEdramAccess(const PreparedDraw& d, const DrawRecord& r,
                                EdramTargets::Target* const* colors, EdramTargets::Target* depth,
                                const plume::RenderRect& scissor) {
  if (!edram_->aliasing()) {
    return EdramTargets::kNoCheck;
  }
  EdramNoteKey key;
  for (uint32_t i = 0; i < d.key.color_count; ++i) {
    key.targets[i] = colors[i];
  }
  key.targets[4] = depth;
  key.scissor[0] = scissor.left;
  key.scissor[1] = scissor.top;
  key.scissor[2] = scissor.right;
  key.scissor[3] = scissor.bottom;
  key.pipeline = d.pipeline;
  key.primitive = r.primitive;
  key.count = r.count;
  key.kills = r.flags & kDrawPsKills;
  key.aa_mask = d.state.aa_mask;
  key.color_control = d.state.color_control.value;
  key.serial = edram_->serial();
  if (key == last_edram_note_) {
    return EdramTargets::kNoCheck;
  }
  const uint32_t check = NoteEdramTargets(d, r, colors, depth, scissor);
  key.serial = edram_->serial();
  last_edram_note_ = key;
  return check;
}

uint32_t Scene::NoteEdramTargets(const PreparedDraw& d, const DrawRecord& r,
                                 EdramTargets::Target* const* colors,
                                 EdramTargets::Target* depth, const plume::RenderRect& scissor) {
  using Access = EdramTargets::Access;
  const XenosState& s = d.state;
  const xreg::RB_DEPTHCONTROL& dc = s.depth_control;
  // The depth target's aspects: untouched, read (a test) or written. Which
  // writes take their aspect needs the rest of the draw, worked out only when
  // the tile map has something to say.
  Access depth_main = Access::kNone;
  Access depth_stencil = Access::kNone;
  if (depth) {
    if (dc.z_enable) {
      depth_main = dc.z_write_enable ? Access::kDepend : Access::kRead;
    }
    if (dc.stencil_enable) {
      depth_stencil = StencilWrites(s) ? Access::kDepend : Access::kRead;
    }
  }
  bool settled = true;
  for (uint32_t i = 0; i < d.key.color_count && settled; ++i) {
    settled = edram_->Settled(*colors[i], scissor, EdramTargets::kMain, false) &&
              edram_->Settled(*colors[i], scissor, EdramTargets::kStencil, false);
  }
  // With colour outputs, a depth or stencil test of inherited bytes decides
  // what they get, every time (reach): only the clean case is skipped.
  const bool outputs = d.key.color_count != 0;
  const auto depth_settled = [&](EdramTargets::Aspect aspect, Access a) {
    return a == Access::kNone ||
           (outputs ? edram_->SettledClean(*depth, scissor, aspect)
                    : edram_->Settled(*depth, scissor, aspect, a == Access::kRead));
  };
  if (settled && depth) {
    settled = depth_settled(EdramTargets::kMain, depth_main) &&
              depth_settled(EdramTargets::kStencil, depth_stencil);
  }
  if (settled) {
    return EdramTargets::kNoCheck;
  }
  // A draw that may take tiles on the strength of its extent gets a coverage
  // check when the checks are on; with none left this frame it is noted as
  // depending on them. With the checks off its takes go unchecked.
  bool covers = TakeCandidate(s, r, MsaaSampleCount(s.msaa));
  uint32_t check = EdramTargets::kNoCheck;
  if (covers && edram_->coverage_queries()) {
    check = edram_->ReserveCoverageCheck();
    covers = check != EdramTargets::kNoCheck;
  }
  EdramEvent event;
  event.check = check;
  event.kind = EdramEvent::Kind::kDraw;
  event.draw_kind = DrawKindName(r.kind);
  event.primitive = PrimitiveName(r.primitive);
  event.count = r.count;
  event.blend = s.color[0].blend.value;
  event.write_mask = s.color[0].write_mask;
  event.depth_control = dc.value;
  event.color_control = s.color_control.value;
  event.stencil = s.stencil.value;
  event.stencil_bf = s.stencil_bf.value;
  event.aa_mask = s.aa_mask;
  event.scissor = scissor;
  event.vs = r.vs_identity;
  event.ps = r.ps_identity;
  edram_->DescribeCoverageCheck(check, event);
  for (uint32_t i = 0; i < d.key.color_count; ++i) {
    // A colour write writes the whole word: both aspects alike.
    const ColorTargetState& c = s.color[i];
    const uint32_t channels = (1u << xe::GetColorRenderTargetFormatComponentCount(c.format)) - 1;
    const Access a = covers && BlendIgnoresDestination(c.blend) &&
                             (c.write_mask & channels) == channels
                         ? Access::kTake
                         : Access::kDepend;
    EdramTargets::Accesses access;
    access.aspect[EdramTargets::kMain] = a;
    access.aspect[EdramTargets::kStencil] = a;
    if (!edram_->Settled(*colors[i], scissor, access)) {
      edram_->Note(*colors[i], scissor, access, event);
    }
  }
  if (depth && (depth_main != Access::kNone || depth_stencil != Access::kNone)) {
    if (depth_main == Access::kDepend && covers && dc.zfunc == xe::CompareFunction::kAlways) {
      depth_main = Access::kTake;
    }
    if (depth_stencil == Access::kDepend && covers && StencilReplacesAll(s)) {
      depth_stencil = Access::kTake;
    }
    EdramTargets::Accesses access;
    access.aspect[EdramTargets::kMain] = depth_main;
    access.aspect[EdramTargets::kStencil] = depth_stencil;
    if (outputs ? !edram_->SettledClean(*depth, scissor, access)
                : !edram_->Settled(*depth, scissor, access)) {
      edram_->Note(*depth, scissor, access, event, colors, d.key.color_count);
    }
  }
  return check;
}

bool Scene::EdramNoteKey::operator==(const EdramNoteKey& o) const {
  for (uint32_t i = 0; i < 5; ++i) {
    if (targets[i] != o.targets[i]) {
      return false;
    }
  }
  for (uint32_t i = 0; i < 4; ++i) {
    if (scissor[i] != o.scissor[i]) {
      return false;
    }
  }
  return pipeline == o.pipeline && primitive == o.primitive && count == o.count &&
         kills == o.kills && aa_mask == o.aa_mask && color_control == o.color_control &&
         serial == o.serial;
}

void Scene::CountSkip(Skip skip, const DrawRecord& r, const std::string& detail) {
  ++window_.skips[size_t(skip)];
  if (occlusion_) {
    // A draw left out inside a guest occlusion query: the count would be too low.
    occlusion_->NoteLostDraw();
  }
  uint32_t& logged = skip_logged_[size_t(skip)];
  if (skip == Skip::kPipelinePending) {
    return;  // expected and repaired by the next frame: counted, not logged
  }
  if (logged < 4) {
    ++logged;
    REXLOG_ERROR("[gpu] scene: draw not drawn -- {}{}{} ({} {}, count {}, VS {:016X}, PS "
                 "{:016X}){}",
                 SkipName(uint32_t(skip)), detail.empty() ? "" : ": ", detail,
                 DrawKindName(r.kind), PrimitiveName(r.primitive), r.count, r.vs_identity,
                 r.ps_identity, logged == 4 ? " (further ones counted, not logged)" : "");
  }
}

// ---------------------------------------------------------------------------
// clears, resolves, tiling (M4.5 and M4.6 execute them)
// ---------------------------------------------------------------------------

Scene::Skip Scene::Record(PreparedDraw& d, const DrawRecord& r, plume::RenderCommandList* list,
                          bool* no_effect, std::string* detail) {
  const XenosState& s = d.state;
  // D3D's effective window scissor bounds every pixel the draw can touch.
  const uint32_t scissor_br_x = s.scissor_br & 0x7FFFu;
  const uint32_t scissor_br_y = (s.scissor_br >> 16) & 0x7FFFu;
  const uint32_t scissor_tl_x = s.scissor_tl & 0x7FFFu;
  const uint32_t scissor_tl_y = (s.scissor_tl >> 16) & 0x7FFFu;
  if (scissor_br_x <= scissor_tl_x || scissor_br_y <= scissor_tl_y) {
    *no_effect = true;
    return Skip::kCount;
  }
  EdramTargets::Target* colors[kColorTargetCount] = {};
  EdramTargets::Target* depth = nullptr;
  const uint32_t pitch = s.surface_pitch;
  // The draw's EDRAM identities.
  EdramKey color_keys[kColorTargetCount];
  for (uint32_t i = 0; i < d.key.color_count; ++i) {
    color_keys[i] = ColorEdramKey(s, i);
  }
  const bool has_depth = d.key.depth_format != 0;
  EdramKey depth_key;
  if (has_depth) {
    depth_key = DrawDepthKey(*edram_, s, d.key.color_count);
  }
  // The last draw's targets, while nothing they rest on changed (M6a.3).
  const TargetsMemo& memo = targets_memo_;
  bool memo_hit = memo.serial != 0 && memo.serial == edram_->change_serial() &&
                  memo.color_count == d.key.color_count && memo.has_depth == has_depth &&
                  scissor_br_y <= memo.height && (!has_depth || memo.depth_key == depth_key);
  for (uint32_t i = 0; i < d.key.color_count && memo_hit; ++i) {
    memo_hit = memo.color_keys[i] == color_keys[i];
  }
  plume::RenderFramebuffer* framebuffer = nullptr;
  ++memo_tries_[size_t(Memo::kTargets)];
  if (memo_hit) {
    ++memo_hits_[size_t(Memo::kTargets)];
    for (uint32_t i = 0; i < d.key.color_count; ++i) {
      colors[i] = memo.colors[i];
    }
    depth = memo.depth;
    framebuffer = memo.framebuffer;
  } else {
    // A target that has to grow copies its contents first, outside a render pass.
    for (uint32_t i = 0; i < d.key.color_count; ++i) {
      const EdramKey& key = color_keys[i];
      const EdramTargets::Target* existing = edram_->Find(key);
      if ((!existing || existing->height < scissor_br_y) && bound_.framebuffer) {
        list->setFramebuffer(nullptr);
        bound_.framebuffer = nullptr;
      }
      bool unset = false;
      colors[i] = edram_->Get(key, scissor_br_y, d.scale, r.target_size_bits, list, &unset);
      if (unset) {
        bound_.framebuffer = nullptr;
      }
      if (!colors[i]) {
        *detail = std::format("colour {} format {} at tile {}", i, key.format, key.base_tiles);
        return Skip::kTarget;
      }
    }
    if (has_depth) {
      const EdramTargets::Target* existing = edram_->Find(depth_key);
      if ((!existing || existing->height < scissor_br_y) && bound_.framebuffer) {
        list->setFramebuffer(nullptr);
        bound_.framebuffer = nullptr;
      }
      bool unset = false;
      depth = edram_->Get(depth_key, scissor_br_y, d.scale, r.target_size_bits, list, &unset);
      if (unset) {
        bound_.framebuffer = nullptr;
      }
      if (!depth) {
        *detail = std::format("depth format {} at tile {}", depth_key.format,
                              depth_key.base_tiles);
        return Skip::kTarget;
      }
    }
  }
  const uint32_t coverage_check =
      NoteEdramAccess(d, r, colors, depth,
                      plume::RenderRect(int32_t(scissor_tl_x), int32_t(scissor_tl_y),
                                        int32_t(scissor_br_x), int32_t(scissor_br_y)));

  // The targets' scale: one for all of them -- a draw makes its targets at its
  // first one's (DrawScale, which made the viewport) -- and the declared size of
  // the surface binding that first one checked against it (EdramTargets::
  // NoteDeclaredScale: an identity used at two scales is reported).
  EdramTargets::Target* const first_target = d.key.color_count ? colors[0] : depth;
  if (first_target) {
    edram_->NoteDeclaredScale(*first_target, r.target_size_bits);
  }
  const Scale target_scale =
      d.key.color_count ? colors[0]->scale : (depth ? depth->scale : Scale{});
  bool scales_agree = !depth || depth->scale == target_scale;
  for (uint32_t i = 0; i < d.key.color_count; ++i) {
    scales_agree &= colors[i]->scale == target_scale;
  }
  if (!scales_agree) {
    const EdramTargets::Target* first = d.key.color_count ? colors[0] : depth;
    *detail = std::format("targets of different resolution scales (pitch {}): {} at {}x, {} at {}x",
                          pitch, DescribeEdramKey(first->key), first->scale,
                          depth ? DescribeEdramKey(depth->key) : std::string("no depth"),
                          depth ? depth->scale : Scale::None());
    return Skip::kTarget;
  }
  // And one sample count on the host, the one the pipeline was made for: a
  // framebuffer cannot mix them (M6d.2; the rule gives colour and depth of a
  // screen image the same).
  bool samples_agree = !depth || depth->host_samples == d.key.sample_count;
  for (uint32_t i = 0; i < d.key.color_count; ++i) {
    samples_agree &= colors[i]->host_samples == d.key.sample_count;
  }
  if (!samples_agree) {
    const EdramTargets::Target* first = d.key.color_count ? colors[0] : depth;
    *detail = std::format("targets of different host samples (pitch {}): {} with {}, {} with {}, "
                          "pipeline {}",
                          pitch, DescribeEdramKey(first->key), first->host_samples,
                          depth ? DescribeEdramKey(depth->key) : std::string("no depth"),
                          depth ? depth->host_samples : 0u, d.key.sample_count);
    return Skip::kTarget;
  }
  // The draw is recorded: its targets were drawn into in this frame.
  for (uint32_t i = 0; i < d.key.color_count; ++i) {
    EdramTargets::NoteDrawn(*colors[i], frame_serial_);
  }
  if (depth) {
    EdramTargets::NoteDrawn(*depth, frame_serial_);
  }
  // Layouts (a barrier closes an open render pass on Vulkan; Plume reopens it).
  uint32_t target_height = EdramTargets::kMaxExtent;
  for (uint32_t i = 0; i < d.key.color_count; ++i) {
    if (colors[i]->layout != plume::RenderTextureLayout::COLOR_WRITE) {
      edram_->Transition(list, *colors[i], plume::RenderTextureLayout::COLOR_WRITE);
    }
    target_height = std::min(target_height, colors[i]->height);
  }
  if (depth) {
    if (depth->layout != plume::RenderTextureLayout::DEPTH_WRITE) {
      edram_->Transition(list, *depth, plume::RenderTextureLayout::DEPTH_WRITE);
    }
    target_height = std::min(target_height, depth->height);
  }
  if (!framebuffer) {
    framebuffer = edram_->Framebuffer(colors, d.key.color_count, depth);
    if (!framebuffer) {
      return Skip::kFramebuffer;
    }
    TargetsMemo& m = targets_memo_;
    m.serial = edram_->change_serial();
    m.color_count = d.key.color_count;
    for (uint32_t i = 0; i < d.key.color_count; ++i) {
      m.color_keys[i] = color_keys[i];
      m.colors[i] = colors[i];
    }
    m.has_depth = has_depth;
    m.depth_key = depth_key;
    m.depth = depth;
    m.height = target_height;
    m.framebuffer = framebuffer;
  }
  if (pass_timing_->enabled() && marked_framebuffer_ != framebuffer) {
    pass_timing_->Mark(list, PassLabel(colors, d.key.color_count, depth));
    marked_framebuffer_ = framebuffer;
  }
  if (target_census_.active()) {
    target_census_.OnDraw(PassLabel(colors, d.key.color_count, depth), r.ps_identity);
  }
  // The frame's last colour target and the guest rectangle its draws covered
  // since it became the last: where the scene is composed.
  EdramTargets::Target* const color0 = d.key.color_count ? colors[0] : nullptr;
  if (color0 != last_color0_) {
    last_color0_ = color0;
    last_color0_width_ = 0;
    last_color0_height_ = 0;
  }
  last_color0_width_ = std::max(last_color0_width_, scissor_br_x);
  last_color0_height_ = std::max(last_color0_height_, scissor_br_y);
  if (composite_census_.after()) {
    composite_census_.OnDrawAfter(CompositeTarget(last_color0_), r.ps_identity);
  }
  BindFramebuffer(list, framebuffer);
  Lap(Stage::kTargets);

  if (!bound_.layout_bound) {
    list->setGraphicsPipelineLayout(layout_->layout());
    list->setGraphicsDescriptorSet(heap_->texture_set(), 0);
    list->setGraphicsDescriptorSet(heap_->texture_set(), 1);
    list->setGraphicsDescriptorSet(heap_->texture_set(), 2);
    list->setGraphicsDescriptorSet(heap_->sampler_set(), 3);
    bound_.layout_bound = true;
    bound_.constants_bound = false;
  }
  if (bound_.pipeline != d.pipeline) {
    list->setPipeline(d.pipeline);
    bound_.pipeline = d.pipeline;
  }
  Lap(Stage::kBindPipeline);
  const plume::RenderBufferReference constants[3] = {
      d.vs_constants.reference(), d.ps_constants.reference(), d.shared_constants.reference()};
  bool constants_changed = !bound_.constants_bound;
  for (uint32_t i = 0; i < 3 && !constants_changed; ++i) {
    constants_changed = constants[i].ref != bound_.constants[i].ref ||
                        constants[i].offset != bound_.constants[i].offset;
  }
  if (constants_changed) {
    if (ActiveBackend() == Backend::kD3D12) {
      // Each root descriptor only when it moved.
      static constexpr uint32_t kRoots[3] = {GuestLayout::kVertexConstants,
                                             GuestLayout::kPixelConstants,
                                             GuestLayout::kSharedConstants};
      for (uint32_t i = 0; i < 3; ++i) {
        if (!bound_.constants_bound || constants[i].ref != bound_.constants[i].ref ||
            constants[i].offset != bound_.constants[i].offset) {
          list->setGraphicsRootDescriptor(constants[i], kRoots[i]);
        }
      }
    } else {
      // The addresses the upload ring read once per chunk.
      const uint64_t addresses[3] = {d.vs_constants.device_address,
                                     d.ps_constants.device_address,
                                     d.shared_constants.device_address};
      list->setGraphicsPushConstants(0, addresses, 0, sizeof(addresses));
    }
    for (uint32_t i = 0; i < 3; ++i) {
      bound_.constants[i] = constants[i];
    }
    bound_.constants_bound = true;
  }
  Lap(Stage::kBindConstants);
  // Only what differs from the list's state.
  if (!bound_.viewport_bound || !SameViewport(bound_.viewport, d.viewport.viewport)) {
    list->setViewports(&d.viewport.viewport, 1);
    bound_.viewport = d.viewport.viewport;
    bound_.viewport_bound = true;
  }
  plume::RenderRect scissor = ComputeScissor(s, target_height, target_scale);
  if (d.ui_squeeze < 1.0f) {
    scissor = SqueezeScissorX(scissor, target_scale.Edge(pitch), d.ui_squeeze);
  }
  d.scissor = scissor;
  if (!bound_.scissor_bound || !SameRect(bound_.scissor, scissor)) {
    list->setScissors(&scissor, 1);
    bound_.scissor = scissor;
    bound_.scissor_bound = true;
  }
  if (pipelines_->dynamic_depth_bias() &&
      (!bound_.depth_bias_bound || !(bound_.depth_bias == d.bias))) {
    list->setDepthBias(float(d.bias.constant), 0.0f, d.bias.slope);
    bound_.depth_bias = d.bias;
    bound_.depth_bias_bound = true;
  }
  Lap(Stage::kBindState);
  const DrawGeometry& g = d.geometry;
  if (g.slot_count &&
      !SameVertexBuffers(bound_.vertex_slot_count, bound_.vertex_views.data(),
                         bound_.vertex_slots.data(), g, d.input_layout->slots.data())) {
    list->setVertexBuffers(0, g.vertex_views.data(), g.slot_count,
                           d.input_layout->slots.data());
    bound_.vertex_slot_count = g.slot_count;
    for (uint32_t i = 0; i < g.slot_count; ++i) {
      bound_.vertex_views[i] = g.vertex_views[i];
      bound_.vertex_slots[i] = d.input_layout->slots[i];
    }
  }
  if (coverage_check != EdramTargets::kNoCheck) {
    // Every host sample of the scissor the host draws in.
    const int64_t width = std::min(scissor.right, int32_t(pitch)) - scissor.left;
    const int64_t height = scissor.bottom - scissor.top;
    edram_->BeginCoverageCheck(list, coverage_check,
                               uint64_t(std::max<int64_t>(width, 0) *
                                        std::max<int64_t>(height, 0) * d.key.sample_count));
  }
  // A draw inside a guest occlusion query is counted by a host query of its own, around
  // the draw: the count the game is told is the sum of its draws', in guest samples (host
  // samples per guest sample and the resolution scale squared divided out). A host
  // query never nests with the coverage check's.
  // CHECK: that this is the console's count. The fork's runs on its own renderer read
  // about 4,000 to 6,600 samples at the title scene and with the sun in open sky (the game
  // clamps the ratio over 1,250 at 1), with the same count at 1x and 2x after dividing by
  // the scale squared; the samples-per-guest-sample factor (a host target with fewer
  // samples than the guest's) is the one part the fork did not have. Also unverified: that
  // the host depth target holds the scene's depth when the sun's draws test against it.
  uint32_t occlusion_query = GuestOcclusion::kNoQuery;
  if (occlusion_ && occlusion_->query_open()) {
    const double scale = double(target_scale.value());
    occlusion_query = occlusion_->BeginDraw(
        list, slot_,
        double(MsaaSampleCount(s.msaa)) / double(std::max(d.key.sample_count, 1u)) /
            (scale * scale),
        coverage_check == EdramTargets::kNoCheck);
  }
  if (g.indexed) {
    if (!bound_.index_bound || !SameIndexBuffer(bound_.index_view, g.index_view)) {
      list->setIndexBuffer(&g.index_view);
      bound_.index_view = g.index_view;
      bound_.index_bound = true;
    }
    Lap(Stage::kBindStreams);
    list->drawIndexedInstanced(g.count, 1, g.first, g.base_vertex, 0);
  } else {
    Lap(Stage::kBindStreams);
    list->drawInstanced(g.count, 1, g.first, 0);
  }
  if (occlusion_query != GuestOcclusion::kNoQuery) {
    occlusion_->EndDraw(list, occlusion_query);
  }
  if (coverage_check != EdramTargets::kNoCheck) {
    edram_->EndCoverageCheck(list, coverage_check);
  }
  Lap(Stage::kDraw);
  return Skip::kCount;
}

void Scene::OnClear(const ClearRecord& record, plume::RenderCommandList* list) {
  ++window_.clears;
  if (!list || (record.flags & kEventSurfaceUnreadable)) {
    ++window_.clears_failed;
    return;
  }
  // sub_824194C0 builds the rectangle, sub_82418E48 clips it and
  // clears each requested target through its surface header.
  const RegisterWindow pre(record.state.pre, guest::kEventWindowBegin, guest::kEventWindowBytes);
  const uint32_t surfaces[guest::kSurfaceCount] = {
      pre.DeviceDword(guest::kDevColorSurfaces), pre.DeviceDword(guest::kDevColorSurfaces + 4),
      pre.DeviceDword(guest::kDevColorSurfaces + 8),
      pre.DeviceDword(guest::kDevColorSurfaces + 12), pre.DeviceDword(guest::kDevDepthSurface)};
  const auto header_dword = [&](uint32_t surface, uint32_t offset) {
    return LoadBe32(record.state.surfaces[surface] + offset);
  };
  const uint8_t device_flags = pre.DeviceByte(guest::kDevFlags);
  bool tiling_detected = false;
  int32_t header_height = 0;
  int32_t x0, y0, x1, y1;
  if (record.flags & kEventHasRect) {
    x0 = int32_t(LoadBe32(record.rect + 0));
    y0 = int32_t(LoadBe32(record.rect + 4));
    x1 = int32_t(LoadBe32(record.rect + 8));
    y1 = int32_t(LoadBe32(record.rect + 12));
  } else {
    // The whole surface: RT0's (the depth one without RT0), or the tiling
    // extent while tiling is on.
    const uint32_t sized = surfaces[0] ? 0u : 4u;
    if (!surfaces[sized]) {
      ++window_.clears_failed;
      return;
    }
    const uint32_t size_bits = header_dword(sized, 0x24);
    x0 = 0;
    y0 = 0;
    x1 = int32_t((size_bits >> 18) + 1);
    y1 = int32_t(((size_bits >> 3) & 0x7FFFu) + 1);
    header_height = y1;
    const bool tiling = EventTiling(pre, surfaces);
    if (tiling) {
      x1 = int32_t(pre.DeviceDword(guest::kDevTilingExtent));
      y1 = int32_t(pre.DeviceDword(guest::kDevTilingExtent + 4));
    }
    tiling_detected = tiling;
  }
  // The D3D viewport (floats X, Y, W, H, truncated as the guest does) and the
  // user scissor when it is on.
  const auto viewport_value = [&](uint32_t i) {
    const uint32_t bits = pre.DeviceDword(guest::kDevViewport + 4 * i);
    float f;
    std::memcpy(&f, &bits, 4);
    return int32_t(f);
  };
  const int32_t vx = viewport_value(0), vy = viewport_value(1);
  const int32_t vw = viewport_value(2), vh = viewport_value(3);
  x0 = std::max(x0, vx);
  y0 = std::max(y0, vy);
  x1 = std::min(x1, vw + vx);
  y1 = std::min(y1, vh + vy);
  if (pre.DeviceDword(guest::kDevScissorEnable) != 0) {
    x0 = std::max(x0, int32_t(pre.DeviceDword(guest::kDevScissorRect + 0)));
    y0 = std::max(y0, int32_t(pre.DeviceDword(guest::kDevScissorRect + 4)));
    x1 = std::min(x1, int32_t(pre.DeviceDword(guest::kDevScissorRect + 8)));
    y1 = std::min(y1, int32_t(pre.DeviceDword(guest::kDevScissorRect + 12)));
  }
  x0 = std::max(x0, 0);
  y0 = std::max(y0, 0);
  if (x1 <= x0 || y1 <= y0) {
    ++window_.clears_empty;
    return;
  }
  uint32_t flags = record.clear_flags;
  if (!surfaces[4]) {
    flags &= ~0x30u;  // no depth surface: no depth or stencil clear
  }
  float color[4] = {};
  if (record.flags & kEventHasColor) {
    for (uint32_t i = 0; i < 4; ++i) {
      const uint32_t bits = LoadBe32(record.color + 4 * i);
      std::memcpy(&color[i], &bits, 4);
    }
  }
  // Pitch and samples of every target cleared: the device's RB_SURFACE_INFO
  // (dev+10368), which sub_824188E0 hands to each colour clear and to the depth
  // one alike. SetRenderTarget(0) writes it from RT0's header +0x18, and
  // SetDepthStencilSurface from the depth's only while no RT0 is bound
  // (sub_824165E0). A surface's own +0x18 is the pitch it was made with, which
  // RAGE does not keep: the impostor passes pair RT0 at pitch 160 with a depth
  // surface made at 280, and a clear by the header's pitch went to a target no
  // draw reads -- the impostor depth stayed uncleared and the trees failed
  // their depth test (24.31).
  xreg::RB_SURFACE_INFO surface_info;
  surface_info.value = pre.Register(guest::reg::kRbSurfaceInfo);
  const auto note_other_pitch = [&](uint32_t surface, const EdramKey& key) {
    xreg::RB_SURFACE_INFO own;
    own.value = header_dword(surface, 0x18);
    if (own.surface_pitch == surface_info.surface_pitch &&
        own.msaa_samples == surface_info.msaa_samples) {
      return;
    }
    ++window_.clears_other_pitch;
    const uint64_t id = uint64_t(surface) | uint64_t(key.base_tiles) << 4 |
                        uint64_t(key.format) << 16 | uint64_t(own.surface_pitch) << 24 |
                        uint64_t(own.msaa_samples) << 38 | uint64_t(key.pitch) << 40 |
                        uint64_t(surface_info.msaa_samples) << 54;
    if (clear_other_pitch_logged_.size() < 16 && clear_other_pitch_logged_.insert(id).second) {
      const char* name = surface == 4 ? format::DepthTargetFormat(key.format).guest_name
                                      : format::ColorTargetFormat(key.format).guest_name;
      REXLOG_INFO(
          "[gpu] clear: {} ({}@{}) was made at pitch {} {}x and is cleared at the device's "
          "pitch {} {}x",
          surface == 4 ? "depth" : std::format("colour {}", surface), name, key.base_tiles,
          uint32_t(own.surface_pitch), MsaaSampleCount(own.msaa_samples), key.pitch,
          key.samples);
    }
  };
  // The scale a target this clear makes gets: the one a draw with these
  // surfaces would make it at -- RT0's identity's, or the depth's without RT0,
  // or before it exists the class of the image the targets hold
  // (EventImageSizeBits; with no surface bound, the clear's rectangle).
  const uint32_t sized_surface = surfaces[0] ? 0u : 4u;
  const uint32_t declared_size =
      EventImageSizeBits(pre, surfaces, record.state, SizeBitsOf(uint32_t(x1), uint32_t(y1)));
  EdramKey first_key;
  first_key.pitch = uint16_t(surface_info.surface_pitch);
  first_key.samples = uint8_t(MsaaSampleCount(surface_info.msaa_samples));
  if (sized_surface == 0) {
    xreg::RB_COLOR_INFO first_info;
    first_info.value = header_dword(0, 0x1C);
    first_key.base_tiles = uint16_t(first_info.color_base | (first_info.color_base_bit_11 << 11));
    first_key.depth = 0;
    first_key.format = uint8_t(FoldColorFormat(uint32_t(first_info.color_format)));
  } else {
    xreg::RB_DEPTH_INFO first_info;
    first_info.value = header_dword(4, 0x1C);
    first_key.base_tiles = uint16_t(first_info.depth_base | (first_info.depth_base_bit_11 << 11));
    first_key.depth = 1;
    first_key.format = uint8_t(first_info.depth_format);
    // A clear of the depth alone: the scene's or the UI's (EdramTargets::DepthLayer).
    first_key.reserved = edram_->DepthLayer(first_key, nullptr);
  }
  const Scale clear_scale = edram_->ScaleOf(first_key, declared_size);
  // Growth copies sit outside a render pass.
  if (bound_.framebuffer) {
    list->setFramebuffer(nullptr);
    bound_.framebuffer = nullptr;
  }
  pass_timing_->Mark(list, "clear");
  marked_framebuffer_ = nullptr;
  for (uint32_t i = 0; i < kColorTargetCount; ++i) {
    if (!(flags & (1u << i)) || !surfaces[i]) {
      continue;
    }
    xreg::RB_COLOR_INFO color_info;
    color_info.value = header_dword(i, 0x1C);
    EdramKey key;
    key.base_tiles = uint16_t(color_info.color_base | (color_info.color_base_bit_11 << 11));
    key.pitch = uint16_t(surface_info.surface_pitch);
    key.depth = 0;
    key.format = uint8_t(FoldColorFormat(uint32_t(color_info.color_format)));
    key.samples = uint8_t(MsaaSampleCount(surface_info.msaa_samples));
    if (bound_.framebuffer) {
      list->setFramebuffer(nullptr);
      bound_.framebuffer = nullptr;
    }
    if (hdr_tone_ && hdr_tone_->active()) {
      // A clear of the float scene's target before the marker: the float scene no longer matches.
      hdr_tone_->OnColorClear(key);
    }
    if (layers_ && layers_->ui_phase() && layers_->OnColorClear(list, key)) {
      // The UI layer started: the UI target was put back before this clear (it drew into it).
      ResetListState();
      marked_framebuffer_ = nullptr;
    }
    bool unset = false;
    EdramTargets::Target* target =
        edram_->Get(key, uint32_t(y1), clear_scale, declared_size, list, &unset);
    if (!target) {
      ++window_.clears_failed;
      continue;
    }
    if (key == first_key) {
      edram_->NoteDeclaredScale(*target, declared_size);
    }
    const uint32_t ghost_interval = REXCVAR_GET(mcla_gpu_ghost_log_interval);
    if (ghost_interval != 0 && frame_serial_ % ghost_interval == 0 && key.pitch == 1280 &&
        key.base_tiles == 1024) {
      REXLOG_INFO("[gpu] ghost clear frame {} {} flags {:02x}/{:04x} explicit {} "
                  "header height {} tiling {} rect ({},{})-({},{}) target {} texture {} height {}",
                  frame_serial_, DescribeEdramKey(key), device_flags, record.clear_flags,
                  (record.flags & kEventHasRect) != 0, header_height, tiling_detected, x0, y0,
                  x1, y1, static_cast<const void*>(target),
                  static_cast<const void*>(target->texture.get()), target->height);
    }
    // What the hardware stores: the colour times 2^exp_bias, in the format.
    float value[4];
    EncodeColorClear(key.format, color_info.color_exp_bias, color, value);
    plume::RenderFramebuffer* framebuffer =
        edram_->ClearColor(list, *target, plume::RenderRect(x0, y0, x1, y1), value);
    bound_.framebuffer = framebuffer;
    if (!framebuffer) {
      ++window_.clears_failed;
      continue;
    }
    ++window_.clears_color;
    note_other_pitch(i, key);
  }
  if (flags & 0x30u) {
    xreg::RB_DEPTH_INFO depth_info;
    depth_info.value = header_dword(4, 0x1C);
    EdramKey key;
    key.base_tiles = uint16_t(depth_info.depth_base | (depth_info.depth_base_bit_11 << 11));
    key.pitch = uint16_t(surface_info.surface_pitch);
    key.depth = 1;
    key.format = uint8_t(depth_info.depth_format);
    key.samples = uint8_t(MsaaSampleCount(surface_info.msaa_samples));
    // With RT0 bound its colour says whose depth this is; without, the frame's phase does.
    key.reserved = edram_->DepthLayer(key, sized_surface == 0 ? &first_key : nullptr);
    if (ao_ && ao_->wants_draws() && (flags & 0x10u) != 0) {
      ao_->OnDepthClear(key, record.z);
    }
    if (bound_.framebuffer) {
      list->setFramebuffer(nullptr);
      bound_.framebuffer = nullptr;
    }
    bool unset = false;
    EdramTargets::Target* target =
        edram_->Get(key, uint32_t(y1), clear_scale, declared_size, list, &unset);
    if (!target) {
      ++window_.clears_failed;
      return;
    }
    if (key == first_key) {
      edram_->NoteDeclaredScale(*target, declared_size);
    }
    const uint32_t ghost_interval = REXCVAR_GET(mcla_gpu_ghost_log_interval);
    if (ghost_interval != 0 && frame_serial_ % ghost_interval == 0 && key.pitch == 1280 &&
        key.base_tiles == 0) {
      REXLOG_INFO("[gpu] ghost clear frame {} {} flags {:02x}/{:04x} explicit {} "
                  "header height {} tiling {} rect ({},{})-({},{}) target {} texture {} height {}",
                  frame_serial_, DescribeEdramKey(key), device_flags, record.clear_flags,
                  (record.flags & kEventHasRect) != 0, header_height, tiling_detected, x0, y0,
                  x1, y1, static_cast<const void*>(target),
                  static_cast<const void*>(target->texture.get()), target->height);
    }
    plume::RenderFramebuffer* framebuffer = edram_->ClearDepthStencil(
        list, *target, plume::RenderRect(x0, y0, x1, y1), (flags & 0x10u) != 0,
        (flags & 0x20u) != 0, record.z, record.stencil);
    bound_.framebuffer = framebuffer;
    if (!framebuffer) {
      ++window_.clears_failed;
      return;
    }
    ++window_.clears_depth;
    note_other_pitch(4, key);
  }
}

void Scene::OnResolve(const ResolveRecord& record, plume::RenderCommandList* list) {
  ++window_.resolves;
  if (composite_census_.after()) {
    composite_census_.OnResolveAfter();
  }
  // The copy reads targets the open render pass may be writing.
  if (list && bound_.framebuffer) {
    list->setFramebuffer(nullptr);
    bound_.framebuffer = nullptr;
  }
  if (list && layers_ && layers_->ui_phase()) {
    // The first resolve of the UI target: its UI goes to the layer and the target is put back
    // for this resolve (frame_layers.h).
    layers_->OnResolve(record, list);
  }
  const ResolveCache::Summary summary = resolve_->Execute(record, list);
  if (ao_ && ao_->wants_draws()) {
    // At the game's mid-scene depth copy this records the occlusion's passes and applies it to the
    // main view's colour: they bind their own layout, pipelines and framebuffers, which the reset
    // below forgets for the next draw.
    ao_->OnResolve(record, summary, list, UpscalerRunning());
  }
  // It bound its own layout, pipeline and push constants: the next draw binds
  // everything again.
  ResetListState();
  marked_framebuffer_ = nullptr;
}

void Scene::OnTilingBegin(const TilingBeginRecord&, plume::RenderCommandList*) {
  ++window_.tiling_begins;
  if (edram_->aliasing()) {
    edram_->BeginTiling();
  }
}

void Scene::OnTilingEnd(const TilingEndRecord& record, plume::RenderCommandList*) {
  ++window_.tiling_ends;
  if (!edram_->aliasing()) {
    return;
  }
  // The section's tiles, as BeginTiling left them in the device.
  const RegisterWindow pre(record.state.pre, guest::kEventWindowBegin, guest::kEventWindowBytes);
  const uint32_t count = std::min(pre.DeviceDword(guest::kDevTileCount), guest::kMaxTiles);
  EdramTile tiles[guest::kMaxTiles];
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t at = guest::kDevTileRects + 16 * i;
    tiles[i].rect = plume::RenderRect(
        int32_t(pre.DeviceDword(at)), int32_t(pre.DeviceDword(at + 4)),
        int32_t(pre.DeviceDword(at + 8)), int32_t(pre.DeviceDword(at + 12)));
    tiles[i].origin_x = int32_t(pre.DeviceDword(guest::kDevTileOrigins + 8 * i));
    tiles[i].origin_y = int32_t(pre.DeviceDword(guest::kDevTileOrigins + 8 * i + 4));
  }
  edram_->EndTiling(tiles, count);
  // The section's resolves found inherited at their tiles (reach).
  resolve_->DrainInherited();
}

void Scene::OnComposite(const CompositeRecord&, plume::RenderCommandList* list) {
  // Where the scene was composed -- the frame's last colour target -- counted
  // against what the frame draws after it (M6c.0).
  composite_census_.OnComposite(CompositeTarget(last_color0_));
  scene_marker_seen_ = true;
  ui_phase_ = true;
  ui_world_ = false;
  edram_->SetScenePhase(false);
  if (ao_) {
    ao_->OnComposite();
  }
  if (temporal_) {
    temporal_->OnComposite();
  }
  PostAa::Mode mode = post_->FrameMode();
  // FXAA and SMAA are held off while an upscaler is chosen and can run (MSAA stays the player's).
  if (temporal_ && temporal_->HoldsAaOff(mode != PostAa::Mode::kOff)) {
    mode = PostAa::Mode::kOff;
  }
  // The UI layer split (and the HDR scene) works at the composite even with no anti-aliasing.
  const bool layers = layers_ && layers_->active();
  if (!last_color0_ || !list || (mode == PostAa::Mode::kOff && !layers)) {
    return;
  }
  // The anti-aliasing reads and writes that target (M6c.2/M6c.3), and the UI layer split starts
  // at it: the scene's render pass ends first.
  if (bound_.framebuffer) {
    list->setFramebuffer(nullptr);
    bound_.framebuffer = nullptr;
  }
  bool recorded = false;
  if (mode != PostAa::Mode::kOff) {
    recorded = post_->Apply(list, slot_, mode, *last_color0_, last_color0_width_,
                            last_color0_height_);
  }
  if (layers) {
    // After the anti-aliasing: what it left in the target is the scene the layers keep. A second
    // marker in a frame with the layer started puts the target back here.
    layers_->OnComposite(list, last_color0_, last_color0_width_, last_color0_height_);
    recorded = true;
    if (hdr_tone_ && hdr_tone_->active()) {
      // HDR stage 3: the float scene is ready if it was made at this marker's target and size (the
      // render pass is closed above; the image goes to shader read).
      EdramKey key;
      uint32_t width = 0, height = 0;
      if (layers_->Region(&key, &width, &height)) {
        hdr_tone_->OnComposite(list, key, width, height);
      }
    }
  }
  if (recorded) {
    // It bound its own layout, pipelines, viewport and scissor: the next draw
    // binds everything again (the M6a caches skip nothing).
    ResetListState();
    marked_framebuffer_ = nullptr;
  }
}

// ---------------------------------------------------------------------------
// statistics
// ---------------------------------------------------------------------------

void Scene::Add(Stats& into, const Stats& from) const {
  into.frames += from.frames;
  into.draws += from.draws;
  into.prepared += from.prepared;
  for (size_t i = 0; i < into.skips.size(); ++i) into.skips[i] += from.skips[i];
  for (size_t i = 0; i < into.no_effect.size(); ++i) into.no_effect[i] += from.no_effect[i];
  for (size_t i = 0; i < into.conversions.size(); ++i) into.conversions[i] += from.conversions[i];
  for (size_t i = 0; i < into.geometry_results.size(); ++i) {
    into.geometry_results[i] += from.geometry_results[i];
  }
  into.bank_checks += from.bank_checks;
  into.bank_mismatches += from.bank_mismatches;
  into.alpha_coverage_draws += from.alpha_coverage_draws;
  into.alpha_test_draws += from.alpha_test_draws;
  into.clip_disabled_draws += from.clip_disabled_draws;
  into.alpha_to_mask_draws += from.alpha_to_mask_draws;
  into.alpha_to_mask_1x += from.alpha_to_mask_1x;
  into.alpha_to_mask_biased += from.alpha_to_mask_biased;
  into.alpha_to_mask_replaced += from.alpha_to_mask_replaced;
  into.depth_bias_draws += from.depth_bias_draws;
  into.constant_uploads += from.constant_uploads;
  into.shared_uploads += from.shared_uploads;
  into.upload_bytes += from.upload_bytes;
  into.fetches += from.fetches;
  into.fetch_not_texture += from.fetch_not_texture;
  into.fetch_memo_hits += from.fetch_memo_hits;
  into.census_scale_reads += from.census_scale_reads;
  into.recorded += from.recorded;
  into.clears += from.clears;
  into.clears_color += from.clears_color;
  into.clears_depth += from.clears_depth;
  into.clears_empty += from.clears_empty;
  into.clears_failed += from.clears_failed;
  into.clears_other_pitch += from.clears_other_pitch;
  into.resolves += from.resolves;
  into.tiling_begins += from.tiling_begins;
  into.tiling_ends += from.tiling_ends;
  into.prepare_cycles += from.prepare_cycles;
  into.pipelines_created += from.pipelines_created;
  into.pipelines_failed += from.pipelines_failed;
  into.pipeline_create_ns += from.pipeline_create_ns;
}

void Scene::OnPresent() {
  pipelines_->EndFrame();  // takes the background builds that finished, then autosaves
  const uint64_t now = cycles::Now();
  if (last_present_) {
    present_intervals_.push_back(now - last_present_);
  }
  last_present_ = now;
  ++window_.frames;
  draw_census_.OnPresent();
  target_census_.OnPresent();
  composite_census_.OnPresent();
  last_frame_had_scene_ = scene_marker_seen_;
  scene_marker_seen_ = false;
  ui_phase_ = false;
  ui_world_ = false;
  if (layers_) {
    layers_->OnGuestPresent();
  }
  if (hdr_tone_) {
    hdr_tone_->OnGuestPresent();
  }
  edram_->SetScenePhase(true);  // the next frame's scene
  if (ao_) {
    ao_->OnPresent();
  }
  if (temporal_) {
    temporal_->EndFrame();
  }
  last_color0_ = nullptr;
  last_color0_width_ = 0;
  last_color0_height_ = 0;
  if (window_.frames >= kReportInterval) {
    Report("interval");
  }
}

void Scene::Finish() {
  Report("final");
  const Stats& t = total_;
  const double frames = t.frames ? double(t.frames) : 1.0;
  REXLOG_INFO("[gpu] scene total: {} frames, {} draws ({:.1f}/frame), {} prepared, {} skipped, "
              "{} without effect; {} pipelines ({} failed, {:.0f} ms creating); {:.2f} MiB of "
              "constants/frame; prepare {:.2f} ms/frame",
              t.frames, t.draws, double(t.draws) / frames, t.prepared,
              [&] {
                uint64_t n = 0;
                for (uint64_t v : t.skips) n += v;
                return n;
              }(),
              t.no_effect[0] + t.no_effect[1] + t.no_effect[2] + t.no_effect[3],
              pipelines_->size(),
              t.pipelines_failed,
              double(t.pipeline_create_ns) / 1e6, double(t.upload_bytes) / frames / 1048576.0,
              cycles::ToNs(t.prepare_cycles) / frames / 1e6);
}

void Scene::Report(const char* why) {
  // The TSC rate over this window, for every per-draw time below (cycles.h),
  // and the render thread's time split (thread_load.h; Report runs on it).
  cycles::Measure();
  const ThreadLoad::Sample load = CurrentThreadLoad().Take();
  const Pipelines::Stats ps = pipelines_->TakeStats();
  window_.pipelines_created += ps.created;
  window_.pipelines_failed += ps.failed;
  window_.pipeline_create_ns += ps.create_ns;
  const Stats& w = window_;
  if (w.frames == 0 && w.draws == 0) {
    return;
  }
  const double frames = w.frames ? double(w.frames) : 1.0;
  const uint64_t busy = load.total_cycles - std::min(load.total_cycles,
                                                    load.idle_cycles + load.gpu_wait_cycles);
  const double thread_cpu_ms = cycles::ToNs(busy) / frames / 1e6;
  // The final report runs on the thread that shuts the renderer down, whose
  // load is not the render thread's.
  const std::string thread_text =
      load.total_cycles
          ? std::format("thread cpu {:.2f} ms/frame, gpu waits {:.2f}, idle {:.2f}", thread_cpu_ms,
                        cycles::ToNs(load.gpu_wait_cycles) / frames / 1e6,
                        cycles::ToNs(load.idle_cycles) / frames / 1e6)
          : std::string("thread time not on the render thread");
  std::string skips;
  for (size_t i = 0; i < w.skips.size(); ++i) {
    if (w.skips[i]) {
      skips += std::format("{}{} {}", skips.empty() ? "" : ", ", SkipName(uint32_t(i)), w.skips[i]);
    }
  }
  std::string conversions;
  for (size_t i = 1; i < w.conversions.size(); ++i) {
    if (w.conversions[i]) {
      conversions += std::format("{}{} {}", conversions.empty() ? "" : ", ",
                                 ConversionName(uint32_t(i)), w.conversions[i]);
    }
  }
  std::string geometry;
  for (size_t i = 1; i < w.geometry_results.size(); ++i) {
    if (w.geometry_results[i]) {
      geometry += std::format("{}{} {}", geometry.empty() ? "" : ", ",
                              GeometryCache::ResultName(GeometryCache::Result(i)),
                              w.geometry_results[i]);
    }
  }
  if (census_scale_) {
    REXLOG_INFO("[gpu] census scale ({}): {} fetches of a resolve product at another scale than "
                "the draw's in {} frames",
                why, w.census_scale_reads, w.frames);
  }
  REXLOG_INFO(
      "[gpu] scene ({}): {} frames | draws {:.1f}/frame: {} prepared, {} recorded, skipped [{}], "
      "geometry [{}], no effect {} (no target {}, culled {}, no primitive {}, empty scissor {}) | "
      "clears {} ({} colour, {} depth, {} empty, {} failed, {} of a surface made at another "
      "pitch) | conversions [{}] | alpha "
      "coverage variant {} (alpha test {}, alpha to mask {}: 1x {}, biased {}, offsets replaced "
      "{}), clip disabled "
      "{}, depth bias {} | banks {} checks, {} mismatches | "
      "pipelines {} total (+{} created, {} failed, {:.1f} "
      "ms; precache {} taken, {} waited {:.1f} ms, {} made here, {} shared; background {} keys "
      "pending, {} draws skipped, {} taken ({} of keys the list lacked), wait mean {:.1f}, longest "
      "{} frames; made here {} new, {} after a failure, {} waited for a running build {:.1f} ms) "
      "| input layouts {} | samplers {} ({} fetches, {} not a texture) | constants {:.2f} "
      "MiB/frame ({} bank, {} shared uploads) | fetch memo {:.0f}% | clears {}, resolves {}, "
      "tiling {}/{} | prepare "
      "{:.2f} ms/frame | {}",
      why, w.frames, double(w.draws) / frames, w.prepared, w.recorded,
      skips.empty() ? "none" : skips, geometry.empty() ? "ok" : geometry,
      w.no_effect[0] + w.no_effect[1] + w.no_effect[2] + w.no_effect[3], w.no_effect[0],
      w.no_effect[1], w.no_effect[2], w.no_effect[3], w.clears, w.clears_color, w.clears_depth,
      w.clears_empty, w.clears_failed, w.clears_other_pitch,
      conversions.empty() ? "none" : conversions, w.alpha_coverage_draws, w.alpha_test_draws,
      w.alpha_to_mask_draws, w.alpha_to_mask_1x, w.alpha_to_mask_biased,
      w.alpha_to_mask_replaced, w.clip_disabled_draws,
      w.depth_bias_draws,
      w.bank_checks, w.bank_mismatches, pipelines_->size(), ps.created, ps.failed,
      double(ps.create_ns) / 1e6, ps.precache_taken, ps.precache_waits,
      double(ps.precache_wait_ns) / 1e6, ps.precache_claimed, ps.shared, ps.pending_keys,
      ps.pending_draws, ps.pending_taken, ps.background_taken,
      ps.pending_taken ? double(ps.pending_frames_sum) / double(ps.pending_taken) : 0.0,
      ps.pending_frames_max, ps.made_here_new, ps.made_here_retry, ps.request_waits,
      double(ps.request_wait_ns) / 1e6, input_layouts_.size(), samplers_->size(), w.fetches,
      w.fetch_not_texture, double(w.upload_bytes) / frames / 1048576.0, w.constant_uploads,
      w.shared_uploads,
      (w.fetches + w.fetch_not_texture)
          ? 100.0 * double(w.fetch_memo_hits) / double(w.fetches + w.fetch_not_texture)
          : 0.0,
      w.clears, w.resolves, w.tiling_begins, w.tiling_ends,
      cycles::ToNs(w.prepare_cycles) / frames / 1e6, thread_text);
  if (ab::Enabled() && std::string_view(why) == "interval") {
    // Per draw: the scene's draw count moves a few percent between windows.
    const double ms = cycles::ToNs(w.prepare_cycles) / frames / 1e6;
    const double ns_per_draw = w.draws ? cycles::ToNs(w.prepare_cycles) / double(w.draws) : 0.0;
    const size_t v = ab::B() ? 1 : 0;
    if (ab_windows_++ > 0) {
      ab_sum_[v] += ns_per_draw;
      ab_cpu_sum_[v] += thread_cpu_ms;
      ++ab_count_[v];
    }
    const auto mean = [&](const double* sums, size_t i) {
      return ab_count_[i] ? sums[i] / ab_count_[i] : 0.0;
    };
    REXLOG_INFO("[gpu] scene ({}): ab window {} prepare {:.3f} ms/frame, {:.1f} ns/draw, thread "
                "cpu {:.3f} ms/frame | means A {:.1f} ns/draw {:.3f} ms ({}), B {:.1f} ns/draw "
                "{:.3f} ms ({})",
                why, v ? "B" : "A", ms, ns_per_draw, thread_cpu_ms, mean(ab_sum_, 0),
                mean(ab_cpu_sum_, 0), ab_count_[0], mean(ab_sum_, 1), mean(ab_cpu_sum_, 1),
                ab_count_[1]);
    ab::NextWindow();
  }
  if (present_intervals_.size() >= 2) {
    // Frame pacing: the median, the 99th percentile, the longest, and how many
    // frames took more than twice the median (V3).
    std::vector<uint64_t>& v = present_intervals_;
    std::sort(v.begin(), v.end());
    const uint64_t median = v[v.size() / 2];
    const uint64_t p99 = v[std::min(v.size() - 1, (v.size() * 99) / 100)];
    const size_t over = size_t(v.end() - std::upper_bound(v.begin(), v.end(), median * 2));
    REXLOG_INFO("[gpu] scene ({}): frame pacing: median {:.2f} ms, p99 {:.2f}, max {:.2f}, {} of "
                "{} frames over 2x the median",
                why, cycles::ToNs(median) / 1e6, cycles::ToNs(p99) / 1e6,
                cycles::ToNs(v.back()) / 1e6, over, v.size());
  }
  present_intervals_.clear();
  ReportProfile(why, frames, load);
  REXLOG_INFO("[gpu] scene ({}): {}", why, geometry_->TakeSummary(w.frames));
  REXLOG_INFO("[gpu] scene ({}): {}", why, edram_->TakeSummary());
  REXLOG_INFO("[gpu] scene ({}): {}", why, textures_->TakeSummary(w.frames));
  REXLOG_INFO("[gpu] scene ({}): {}", why, resolve_->TakeSummary(w.frames));
  REXLOG_INFO("[gpu] scene ({}): {}", why, resolve_->memory().TakeSummary(w.frames));
  if (draw_census_.active()) {
    REXLOG_INFO("[gpu] scene ({}): {}", why, draw_census_.TakeReport());
  }
  if (target_census_.active()) {
    if (const std::string text = target_census_.TakeReport(); !text.empty()) {
      REXLOG_INFO("[gpu] scene ({}): {}", why, text);
    }
  }
  REXLOG_INFO("[gpu] scene ({}): {}", why, composite_census_.TakeReport());
  Add(total_, window_);
  window_ = Stats{};
}

}  // namespace mcla::gpu
