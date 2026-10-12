// MCLA GPU renderer -- the replay of the guest's frames. See scene.h.

#include "scene.h"

#include <bit>
#include <cstring>
#include <format>

#include <rex/logging.h>

#include "descriptors.h"
#include "draw_decode.h"
#include "draw_state.h"
#include "format.h"
#include "samplers.h"

namespace mcla::gpu {

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

}  // namespace

void Scene::Initialize(UploadRing& upload, Pipelines& pipelines, Samplers& samplers) {
  upload_ = &upload;
  pipelines_ = &pipelines;
  samplers_ = &samplers;
  base_shared_ = SharedConstants{};
  for (uint32_t i = 0; i < kFetchSlotCount; ++i) {
    base_shared_.texture_2d[i] = kNullTexture2DSlot;
    base_shared_.texture_3d[i] = kNullTexture3DSlot;
    base_shared_.texture_cube[i] = kNullTextureCubeSlot;
    base_shared_.sampler[i] = kSamplerLinearWrap;
  }
}

void Scene::BeginFrame(plume::RenderCommandList* list, uint32_t slot) {
  list_ = list;
  slot_ = slot;
  // Uploads of the previous frame live in another slot's memory.
  device_uploads_.clear();
  last_shared_span_ = UploadSpan{};
  draws_since_flush_ = 0;
}

void Scene::EndFrame() {
  list_ = nullptr;
  device_uploads_.clear();
  last_shared_span_ = UploadSpan{};
  draws_since_flush_ = 0;
}

void Scene::OnListChanged(plume::RenderCommandList* list) {
  // The upload memory belongs to the slot, not to the list: what was uploaded
  // stays valid for the rest of the frame.
  list_ = list;
  draws_since_flush_ = 0;
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
      ++draws_since_flush_;
      // Recording needs the buffers (M4.4) and the EDRAM targets (M4.5).
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

  // Input layout.
  d.input_layout = input_layouts_.Get(*d.vs, view.decl_records(), r.decl_count, r.stream_strides);
  if (d.input_layout->problems != 0) {
    *detail = std::format("VS {:016X}, {} problem(s)", r.vs_identity, d.input_layout->problems);
    return Skip::kInputLayout;
  }

  // Pipeline.
  const PipelineInputs inputs{d.vs, d.ps, d.input_layout};
  switch (pipelines_->BuildKey(s, inputs, d.primitive.topology, color_formats, depth_format, d.key,
                               d.bias)) {
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
  if (d.key.ps_variant) ++window_.alpha_test_draws;
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

void Scene::FillTextureTables(const DrawView& view, uint32_t fetch_mask, SharedConstants& sc) {
  for (uint32_t mask = fetch_mask; mask != 0; mask &= mask - 1) {
    const uint32_t slot = uint32_t(std::countr_zero(mask));
    uint32_t dwords[6];
    view.FetchGroup(slot, dwords);
    xe::xe_gpu_texture_fetch_t fetch;
    std::memcpy(&fetch, dwords, sizeof(fetch));
    if (fetch.type != xe::FetchConstantType::kTexture) {
      // The shader samples a slot the guest left without a texture: the null
      // texture is read, as an invalid fetch reads zeros.
      ++window_.fetch_not_texture;
      continue;
    }
    ++window_.fetches;
    sc.sampler[slot] = samplers_->Get(dwords);
    // Textures are M4.6: the index stays the null of the fetch's dimension,
    // with the gamma request of the fetch's signs already in place.
    const bool gamma = fetch.sign_x == xe::TextureSign::kGamma &&
                       fetch.sign_y == xe::TextureSign::kGamma &&
                       fetch.sign_z == xe::TextureSign::kGamma;
    const uint32_t gamma_bit = gamma ? kTextureGammaBit : 0u;
    switch (fetch.dimension) {
      case xe::DataDimension::k3D:
        sc.texture_3d[slot] = kNullTexture3DSlot | gamma_bit;
        break;
      case xe::DataDimension::kCube:
        sc.texture_cube[slot] = kNullTextureCubeSlot | gamma_bit;
        break;
      default:  // k1D, k2DOrStacked
        sc.texture_2d[slot] = kNullTexture2DSlot | gamma_bit;
        break;
    }
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

void Scene::OnClear(const ClearRecord&, plume::RenderCommandList*) {
  ++window_.clears;
}

void Scene::OnResolve(const ResolveRecord&, plume::RenderCommandList*) {
  ++window_.resolves;
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
  into.bank_checks += from.bank_checks;
  into.bank_mismatches += from.bank_mismatches;
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
  into.clears += from.clears;
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
              t.no_effect[0] + t.no_effect[1], pipelines_->size(), t.pipelines_failed,
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
  REXLOG_INFO(
      "[gpu] scene ({}): {} frames | draws {:.1f}/frame: {} prepared, skipped [{}], no effect "
      "{} (no target {}, culled {}) | conversions for M4.4 [{}] | alpha test {}, clip disabled "
      "{}, depth bias {}, alpha to mask {} (1x {}, biased {}) | banks {} checks, {} mismatches | "
      "pipelines {} total (+{} created, {} failed, {:.1f} "
      "ms) | input layouts {} | samplers {} ({} fetches, {} not a texture) | constants {:.2f} "
      "MiB/frame ({} bank, {} shared uploads) | clears {}, resolves {}, tiling {}/{} | prepare "
      "{:.2f} ms/frame",
      why, w.frames, double(w.draws) / frames, w.prepared, skips.empty() ? "none" : skips,
      w.no_effect[0] + w.no_effect[1], w.no_effect[0], w.no_effect[1],
      conversions.empty() ? "none" : conversions, w.alpha_test_draws, w.clip_disabled_draws,
      w.depth_bias_draws, w.alpha_to_mask_draws, w.alpha_to_mask_1x, w.alpha_to_mask_biased,
      w.bank_checks, w.bank_mismatches, pipelines_->size(), ps.created, ps.failed,
      double(ps.create_ns) / 1e6, input_layouts_.size(), samplers_->size(), w.fetches,
      w.fetch_not_texture, double(w.upload_bytes) / frames / 1048576.0, w.constant_uploads,
      w.shared_uploads, w.clears, w.resolves, w.tiling_begins, w.tiling_ends,
      double(w.prepare_ns) / frames / 1e6);
  Add(total_, window_);
  window_ = Stats{};
}

}  // namespace mcla::gpu
