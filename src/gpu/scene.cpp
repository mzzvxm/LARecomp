// MCLA GPU renderer -- the replay of the guest's frames. See scene.h.

#include "scene.h"

#include <bit>
#include <cstring>
#include <format>

#include <rex/graphics/pipeline/texture/util.h>
#include <rex/logging.h>

#include "backend.h"
#include "descriptors.h"
#include "draw_decode.h"
#include "draw_state.h"
#include "format.h"
#include "layout.h"
#include "native_gfx/vertex_declaration.h"
#include "pass_timing.h"
#include "resolve.h"
#include "resolve_memory.h"
#include "samplers.h"
#include "textures.h"

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
      "constant upload failed",
      "geometry",
      "EDRAM target not made",
      "framebuffer not made",
  };
  return skip < std::size(kNames) ? kNames[skip] : "?";
}

const char* DrawKindName(DrawKind kind) {
  switch (kind) {
    case DrawKind::kIndexed: return "indexed";
    case DrawKind::kAuto: return "auto";
    case DrawKind::kInline: return "inline";
  }
  return "?";
}

const char* ConversionName(uint32_t conversion) {
  static const char* const kNames[] = {"none",       "quad list", "quad strip", "fan/polygon",
                                       "rect list",  "line loop", "unsupported"};
  return conversion < std::size(kNames) ? kNames[conversion] : "?";
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

}  // namespace

void Scene::Initialize(UploadRing& upload, Pipelines& pipelines, Samplers& samplers,
                       GeometryCache& geometry, EdramTargets& edram, TextureCache& textures,
                       ResolveCache& resolve, const GuestLayout& layout,
                       const BindlessHeap& heap, PassTiming& pass_timing) {
  pass_timing_ = &pass_timing;
  upload_ = &upload;
  pipelines_ = &pipelines;
  samplers_ = &samplers;
  geometry_ = &geometry;
  edram_ = &edram;
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
}

void Scene::ResetListState() {
  bound_ = ListState{};
}

void Scene::BeginFrame(plume::RenderCommandList* list, uint32_t slot) {
  list_ = list;
  slot_ = slot;
  geometry_->BeginFrame(slot);
  edram_->BeginFrame();
  textures_->BeginFrame(slot);
  resolve_->BeginFrame(slot);
  ++frame_serial_;
  ResetListState();
  marked_framebuffer_ = nullptr;
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
  // The colour encodes of the frame into the resolve memory, as one batch.
  pass_timing_->Mark(list, "resolve memory encode (batch)");
  resolve_->memory().Flush(list);
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

// ---------------------------------------------------------------------------
// draws
// ---------------------------------------------------------------------------

void Scene::OnDraw(const uint8_t* payload, plume::RenderCommandList* list) {
  const auto t0 = Clock::now();
  const DrawView view(payload);
  const DrawRecord& r = view.record();
  ++window_.draws;

  ConstantBanks::Check check;
  const ConstantBanks::Banks& banks = banks_.Apply(view, &check);
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
      bool empty = false;
      const Skip recorded = Record(d, list, &empty, &detail);
      if (empty) {
        ++window_.no_effect[size_t(NoEffect::kEmptyScissor)];
      } else if (recorded != Skip::kCount) {
        CountSkip(recorded, r, detail);
      } else {
        ++window_.recorded;
        ++draws_since_flush_;
      }
    }
  }
  window_.prepare_ns +=
      uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
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

  d.state = DecodeXenosState(view);
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
  plume::RenderFormat depth_format = plume::RenderFormat::UNKNOWN;
  if (s.depth_bound) {
    const format::Mapping m = format::DepthTargetFormat(uint32_t(s.depth_format));
    if (m.status != format::Status::kOk) {
      *detail = std::format("depth {} ({})", m.guest_name, format::StatusName(m.status));
      return Skip::kTargetFormat;
    }
    depth_format = m.format;
  }

  // Shaders.
  d.vs = shaders_.Find(r.vs_identity);
  if (!d.vs) {
    *detail = std::format("{:016X}", r.vs_identity);
    return Skip::kVsNotInPack;
  }
  if (d.vs->stage != ShaderStore::Stage::kVertex) {
    *detail = std::format("VS {:016X}", r.vs_identity);
    return Skip::kStageMismatch;
  }
  if (!depth_only && (r.flags & kDrawNoPixelShader) == 0) {
    d.ps = shaders_.Find(r.ps_identity);
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

  // Geometry: vertex and index data, and the final host topology (an indexed
  // strip whose restart differs from the host's becomes a list).
  const bool computed_fetch = (d.vs->flags & ShaderStore::kComputedFetchIndex) != 0;
  const GeometryCache::Result geometry = geometry_->Prepare(
      view, s, *d.input_layout, d.primitive, computed_fetch, d.geometry, detail);
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

  // Pipeline.
  const PipelineInputs inputs{d.vs, d.ps, d.input_layout};
  switch (pipelines_->BuildKey(s, inputs, d.geometry.topology, IsPolygonal(r.primitive),
                               color_formats, depth_format, d.key, d.bias)) {
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
  d.pipeline = pipelines_->Get(d.key, inputs);
  if (!d.pipeline) {
    *detail = std::format("VS {:016X} PS {:016X}", d.key.vs, d.key.ps);
    return Skip::kPipeline;
  }

  // Viewport and constants.
  const bool ps_writes_depth = d.ps && (d.ps->ps_outputs & ShaderStore::kOutputDepth) != 0;
  d.viewport = ComputeViewport(s, ps_writes_depth);
  SharedConstants shared = base_shared_;
  const uint32_t issues = FillSharedConstants(s, d.viewport, shared);
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
  const uint32_t fetch_mask = d.vs->texture_fetch_mask | (d.ps ? d.ps->texture_fetch_mask : 0u);
  FillTextureTables(view, fetch_mask, shared);
  if (!UploadConstants(r.device, banks, shared, d)) {
    return Skip::kUpload;
  }

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
  }
  return Skip::kCount;
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

void Scene::FillTextureTables(const DrawView& view, uint32_t fetch_mask, SharedConstants& sc) {
  // Write notifications first: a memoized binding must not outlive a write.
  textures_->Sync();
  for (uint32_t mask = fetch_mask; mask != 0; mask &= mask - 1) {
    const uint32_t slot = uint32_t(std::countr_zero(mask));
    uint32_t dwords[6];
    view.FetchGroup(slot, dwords);
    uint64_t h = 0;
    for (uint32_t d : dwords) {
      h = (h ^ d) * 0x9E3779B97F4A7C15ull;
    }
    FetchMemo* set = &fetch_memo_[(size_t(h >> 40) & (kFetchMemoSets - 1)) * kFetchMemoWays];
    FetchMemo* hit = nullptr;
    FetchMemo* victim = set;
    for (size_t way = 0; way < kFetchMemoWays; ++way) {
      FetchMemo& candidate = set[way];
      if (candidate.epoch == textures_->epoch() &&
          (candidate.frame == 0 || candidate.frame == frame_serial_) &&
          candidate.generation == textures_->ProductGeneration(candidate.page) &&
          std::memcmp(candidate.dwords, dwords, sizeof(dwords)) == 0) {
        hit = &candidate;
        break;
      }
      if (candidate.used < victim->used) {
        victim = &candidate;
      }
    }
    if (hit) {
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
      fresh.sampler = samplers_->Get(dwords, fetch.mip_min, fetch.mip_max);
      // Only 2D textures are made (100% of the measured fetches);
      // the cache logs any other dimension once and the null of its dimension
      // is bound.
      const TextureCache::Binding bound = textures_->Bind2D(fetch, list_);
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
    fresh.used = ++fetch_memo_clock_;
    m = fresh;
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

void Scene::CountSkip(Skip skip, const DrawRecord& r, const std::string& detail) {
  ++window_.skips[size_t(skip)];
  uint32_t& logged = skip_logged_[size_t(skip)];
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

Scene::Skip Scene::Record(PreparedDraw& d, plume::RenderCommandList* list, bool* no_effect,
                          std::string* detail) {
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
  const uint32_t samples = MsaaSampleCount(s.msaa);
  EdramTargets::Target* colors[kColorTargetCount] = {};
  EdramTargets::Target* depth = nullptr;
  const uint32_t pitch = s.surface_pitch;
  // A target that has to grow copies its contents first, outside a render pass.
  for (uint32_t i = 0; i < d.key.color_count; ++i) {
    EdramKey key;
    key.base_tiles = uint16_t(s.color[i].base_tiles);
    key.pitch = uint16_t(pitch);
    key.depth = 0;
    key.format = uint8_t(FoldColorFormat(uint32_t(s.color[i].format)));
    key.samples = uint8_t(samples);
    const EdramTargets::Target* existing = edram_->Find(key);
    if ((!existing || existing->height < scissor_br_y) && bound_.framebuffer) {
      list->setFramebuffer(nullptr);
      bound_.framebuffer = nullptr;
    }
    bool unset = false;
    colors[i] = edram_->Get(key, scissor_br_y, list, &unset);
    if (unset) {
      bound_.framebuffer = nullptr;
    }
    if (!colors[i]) {
      *detail = std::format("colour {} format {} at tile {}", i, key.format, key.base_tiles);
      return Skip::kTarget;
    }
  }
  if (d.key.depth_format != 0) {
    EdramKey key;
    key.base_tiles = uint16_t(s.depth_base_tiles);
    key.pitch = uint16_t(pitch);
    key.depth = 1;
    key.format = uint8_t(s.depth_format);
    key.samples = uint8_t(samples);
    const EdramTargets::Target* existing = edram_->Find(key);
    if ((!existing || existing->height < scissor_br_y) && bound_.framebuffer) {
      list->setFramebuffer(nullptr);
      bound_.framebuffer = nullptr;
    }
    bool unset = false;
    depth = edram_->Get(key, scissor_br_y, list, &unset);
    if (unset) {
      bound_.framebuffer = nullptr;
    }
    if (!depth) {
      *detail = std::format("depth format {} at tile {}", key.format, key.base_tiles);
      return Skip::kTarget;
    }
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
  plume::RenderFramebuffer* framebuffer = edram_->Framebuffer(colors, d.key.color_count, depth);
  if (!framebuffer) {
    return Skip::kFramebuffer;
  }
  if (pass_timing_->enabled() && marked_framebuffer_ != framebuffer) {
    pass_timing_->Mark(list, PassLabel(colors, d.key.color_count, depth));
    marked_framebuffer_ = framebuffer;
  }
  BindFramebuffer(list, framebuffer);

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
  const plume::RenderBufferReference constants[3] = {
      d.vs_constants.reference(), d.ps_constants.reference(), d.shared_constants.reference()};
  bool constants_changed = !bound_.constants_bound;
  for (uint32_t i = 0; i < 3 && !constants_changed; ++i) {
    constants_changed = constants[i].ref != bound_.constants[i].ref ||
                        constants[i].offset != bound_.constants[i].offset;
  }
  if (constants_changed) {
    if (ActiveBackend() == Backend::kD3D12) {
      list->setGraphicsRootDescriptor(constants[0], GuestLayout::kVertexConstants);
      list->setGraphicsRootDescriptor(constants[1], GuestLayout::kPixelConstants);
      list->setGraphicsRootDescriptor(constants[2], GuestLayout::kSharedConstants);
    } else {
      const uint64_t addresses[3] = {
          d.vs_constants.buffer->getDeviceAddress() + d.vs_constants.offset,
          d.ps_constants.buffer->getDeviceAddress() + d.ps_constants.offset,
          d.shared_constants.buffer->getDeviceAddress() + d.shared_constants.offset};
      list->setGraphicsPushConstants(0, addresses, 0, sizeof(addresses));
    }
    for (uint32_t i = 0; i < 3; ++i) {
      bound_.constants[i] = constants[i];
    }
    bound_.constants_bound = true;
  }
  list->setViewports(&d.viewport.viewport, 1);
  const plume::RenderRect scissor = ComputeScissor(s, target_height);
  list->setScissors(&scissor, 1);
  if (pipelines_->dynamic_depth_bias() &&
      (!bound_.depth_bias_bound || !(bound_.depth_bias == d.bias))) {
    list->setDepthBias(float(d.bias.constant), 0.0f, d.bias.slope);
    bound_.depth_bias = d.bias;
    bound_.depth_bias_bound = true;
  }
  const DrawGeometry& g = d.geometry;
  if (g.slot_count) {
    list->setVertexBuffers(0, g.vertex_views.data(), g.slot_count,
                           d.input_layout->slots.data());
  }
  if (g.indexed) {
    list->setIndexBuffer(&g.index_view);
    list->drawIndexedInstanced(g.count, 1, g.first, g.base_vertex, 0);
  } else {
    list->drawInstanced(g.count, 1, g.first, 0);
  }
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
    const uint8_t device_flags = pre.DeviceByte(guest::kDevFlags);
    bool tiling = (device_flags & 0x10) != 0;
    if (!tiling && (device_flags & 0x20) != 0) {
      tiling = true;
      for (uint32_t i = 0; i < guest::kSurfaceCount; ++i) {
        const uint32_t tiled = pre.DeviceDword(guest::kDevSurfaceCache + 4 * i);
        if (surfaces[i] != 0 && tiled != surfaces[i]) {
          tiling = false;
        }
      }
    }
    if (tiling) {
      x1 = int32_t(pre.DeviceDword(guest::kDevTilingExtent));
      y1 = int32_t(pre.DeviceDword(guest::kDevTilingExtent + 4));
    }
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
    bool unset = false;
    EdramTargets::Target* target = edram_->Get(key, uint32_t(y1), list, &unset);
    if (!target) {
      ++window_.clears_failed;
      continue;
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
    if (bound_.framebuffer) {
      list->setFramebuffer(nullptr);
      bound_.framebuffer = nullptr;
    }
    bool unset = false;
    EdramTargets::Target* target = edram_->Get(key, uint32_t(y1), list, &unset);
    if (!target) {
      ++window_.clears_failed;
      return;
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
  // The copy reads targets the open render pass may be writing.
  if (list && bound_.framebuffer) {
    list->setFramebuffer(nullptr);
    bound_.framebuffer = nullptr;
  }
  resolve_->Execute(record, list);
  // It bound its own layout, pipeline and push constants: the next draw binds
  // everything again.
  ResetListState();
  marked_framebuffer_ = nullptr;
}

void Scene::OnTilingBegin(const TilingBeginRecord&, plume::RenderCommandList*) {
  ++window_.tiling_begins;
}

void Scene::OnTilingEnd(const TilingEndRecord&, plume::RenderCommandList*) {
  ++window_.tiling_ends;
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
  into.depth_bias_draws += from.depth_bias_draws;
  into.constant_uploads += from.constant_uploads;
  into.shared_uploads += from.shared_uploads;
  into.upload_bytes += from.upload_bytes;
  into.fetches += from.fetches;
  into.fetch_not_texture += from.fetch_not_texture;
  into.fetch_memo_hits += from.fetch_memo_hits;
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
  into.prepare_ns += from.prepare_ns;
  into.pipelines_created += from.pipelines_created;
  into.pipelines_failed += from.pipelines_failed;
  into.pipeline_create_ns += from.pipeline_create_ns;
}

void Scene::OnPresent() {
  ++window_.frames;
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
              double(t.prepare_ns) / frames / 1e6);
}

void Scene::Report(const char* why) {
  const Pipelines::Stats ps = pipelines_->TakeStats();
  window_.pipelines_created += ps.created;
  window_.pipelines_failed += ps.failed;
  window_.pipeline_create_ns += ps.create_ns;
  const Stats& w = window_;
  if (w.frames == 0 && w.draws == 0) {
    return;
  }
  const double frames = w.frames ? double(w.frames) : 1.0;
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
  REXLOG_INFO(
      "[gpu] scene ({}): {} frames | draws {:.1f}/frame: {} prepared, {} recorded, skipped [{}], "
      "geometry [{}], no effect {} (no target {}, culled {}, no primitive {}, empty scissor {}) | "
      "clears {} ({} colour, {} depth, {} empty, {} failed, {} of a surface made at another "
      "pitch) | conversions [{}] | alpha "
      "coverage variant {} (alpha test {}, alpha to mask {}: 1x {}, biased {}), clip disabled "
      "{}, depth bias {} | banks {} checks, {} mismatches | "
      "pipelines {} total (+{} created, {} failed, {:.1f} "
      "ms) | input layouts {} | samplers {} ({} fetches, {} not a texture) | constants {:.2f} "
      "MiB/frame ({} bank, {} shared uploads) | fetch memo {:.0f}% | clears {}, resolves {}, "
      "tiling {}/{} | prepare "
      "{:.2f} ms/frame",
      why, w.frames, double(w.draws) / frames, w.prepared, w.recorded,
      skips.empty() ? "none" : skips, geometry.empty() ? "ok" : geometry,
      w.no_effect[0] + w.no_effect[1] + w.no_effect[2] + w.no_effect[3], w.no_effect[0],
      w.no_effect[1], w.no_effect[2], w.no_effect[3], w.clears, w.clears_color, w.clears_depth,
      w.clears_empty, w.clears_failed, w.clears_other_pitch,
      conversions.empty() ? "none" : conversions, w.alpha_coverage_draws, w.alpha_test_draws,
      w.alpha_to_mask_draws, w.alpha_to_mask_1x, w.alpha_to_mask_biased, w.clip_disabled_draws,
      w.depth_bias_draws,
      w.bank_checks, w.bank_mismatches, pipelines_->size(), ps.created, ps.failed,
      double(ps.create_ns) / 1e6, input_layouts_.size(), samplers_->size(), w.fetches,
      w.fetch_not_texture, double(w.upload_bytes) / frames / 1048576.0, w.constant_uploads,
      w.shared_uploads,
      (w.fetches + w.fetch_not_texture)
          ? 100.0 * double(w.fetch_memo_hits) / double(w.fetches + w.fetch_not_texture)
          : 0.0,
      w.clears, w.resolves, w.tiling_begins, w.tiling_ends,
      double(w.prepare_ns) / frames / 1e6);
  REXLOG_INFO("[gpu] scene ({}): {}", why, geometry_->TakeSummary(w.frames));
  REXLOG_INFO("[gpu] scene ({}): {}", why, edram_->TakeSummary());
  REXLOG_INFO("[gpu] scene ({}): {}", why, textures_->TakeSummary(w.frames));
  REXLOG_INFO("[gpu] scene ({}): {}", why, resolve_->TakeSummary(w.frames));
  REXLOG_INFO("[gpu] scene ({}): {}", why, resolve_->memory().TakeSummary(w.frames));
  Add(total_, window_);
  window_ = Stats{};
}

}  // namespace mcla::gpu
