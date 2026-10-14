// MCLA GPU renderer -- validation of the captured stream. See validation.h.

#include "validation.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <rex/filesystem.h>
#include <rex/logging.h>

#include "backend.h"
#include "draw_decode.h"
#include "guest_device.h"
#include "native_gfx/guest/guest_resources.h"
#include "native_gfx/guest/texture_format.h"
#include "native_gfx/vertex_declaration.h"

namespace mcla::gpu {

namespace nfx = mcla::native_gfx;

namespace {

using Clock = std::chrono::steady_clock;

const char* const kCounterNames[] = {
    "draws", "draws_indexed", "draws_auto", "draws_inline",
    "vs_unreadable", "ps_unreadable", "no_pixel_shader", "decl_unreadable", "decl_too_long",
    "index_unreadable", "inline_unreadable", "inline_unterminated", "index_32bit",
    "vs_in_pack", "vs_not_in_pack", "ps_in_pack", "ps_not_in_pack", "stage_mismatch",
    "vs_variant1", "ps_variant1", "variant_missing",
    "shader_ok", "shader_bad_blob", "shader_create_failed",
    "input_ok", "input_no_table", "input_unsupported_usage", "input_unknown_decltype",
    "input_missing_in_plume", "input_unsupplied_attributes", "input_no_vertex_fetch",
    "input_no_decl",
    "computed_fetch_draws", "computed_fetch_mismatch",
    "color_targets", "color_format_missing", "color_format_unknown", "depth_targets",
    "depth_format_unknown",
    "textures_bound", "texture_fetch_invalid", "texture_format_missing",
    "texture_format_unknown",
    "bank_checks", "bank_mismatch_vs", "bank_mismatch_ps",
    "poly_offset_draws", "other_device_draws", "viz_query_draws",
    "predicated_draws", "zpass_draws",
    "clears", "clear_color", "clear_depth", "clear_stencil", "clear_whole_surface",
    "clear_rect", "clear_nothing_bound", "clear_unreadable",
    "resolves", "resolves_nested", "resolve_source_unbound", "resolve_dest_unreadable",
    "resolve_dest_format_unknown", "resolve_dest_format_missing", "resolve_with_clear",
    "resolve_stack_unreadable",
    "tiling_begins", "tiling_ends", "tiles", "tiling_nested_resolves",
    "presents", "front_buffer_unreadable", "front_buffer_format_unknown",
    "guest_unreadable", "identity_new", "identity_renormalized", "constant_blocks_masked",
    "constant_blocks_unmasked", "frames_draw_count_mismatch", "frames_record_count_mismatch",
};
static_assert(std::size(kCounterNames) == FrameValidator::kCounterCount);

// Return addresses of the calls into ClearF and Resolve (call site + 4), from
// the xrefs in Anything else is game code, shown as an address.
const char* CallerName(uint32_t lr) {
  switch (lr) {
    case 0x8241965Cu:
    case 0x8241968Cu:
      return "D3DDevice_Clear";
    case 0x8241C160u:
      return "BeginTiling";
    case 0x8241C9C4u:
    case 0x8241CA10u:
      return "Z pass (sub_8241C7C0)";
    case 0x8241C46Cu:
    case 0x8241C49Cu:
      return "EndTiling";
    case 0x8217BA80u:
    case 0x8217BB10u:
      return "grcDevice::EndFrame";
    case 0x8217B574u:
    case 0x8217B624u:
      return "grcDevice::EndTiledRendering";
    case 0x8242816Cu:
      return "sub_82427A80";
    default:
      return nullptr;
  }
}

std::string Callers(const std::map<uint32_t, uint64_t>& callers) {
  std::string out;
  for (const auto& [lr, n] : callers) {
    if (!out.empty()) {
      out += ", ";
    }
    const char* name = CallerName(lr);
    char text[80];
    if (name) {
      std::snprintf(text, sizeof(text), "%s %llu", name, static_cast<unsigned long long>(n));
    } else {
      std::snprintf(text, sizeof(text), "0x%08X %llu", lr, static_cast<unsigned long long>(n));
    }
    out += text;
  }
  return out.empty() ? "none" : out;
}

double Percent(uint64_t part, uint64_t whole) {
  return whole == 0 ? 100.0 : 100.0 * double(part) / double(whole);
}

bool Dxil() {
  return HostShaderFormat() == plume::RenderShaderFormat::DXIL;
}

}  // namespace

// ---------------------------------------------------------------------------
// draws
// ---------------------------------------------------------------------------

const ShaderStore::Entry* FrameValidator::NoteShader(uint64_t identity, bool pixel,
                                                     uint32_t ucode_bytes, bool readable) {
  if (!readable || identity == 0) {
    return nullptr;
  }
  ShaderInfo& info = shader_table_[identity];
  if (info.draws == 0) {
    info.pixel = pixel;
    info.ucode_bytes = ucode_bytes;
    info.entry = shaders_.Find(identity);
    if (info.entry) {
      info.stage_mismatch = (info.entry->stage == ShaderStore::Stage::kPixel) != pixel;
      info.blob_bytes = Dxil() ? info.entry->dxil[0].size : info.entry->spirv[0].size;
    }
  }
  ++info.draws;
  Count(pixel ? (info.entry ? kPsInPack : kPsNotInPack) : (info.entry ? kVsInPack : kVsNotInPack));
  if (info.entry && info.stage_mismatch) {
    Count(kStageMismatch);
    return nullptr;
  }
  return info.entry;
}

void FrameValidator::CheckShaderObject(const ShaderStore::Entry& entry, uint32_t variant) {
  ShaderStore::ShaderResult result = ShaderStore::ShaderResult::kOk;
  shaders_.GetShader(entry, variant, &result);
  switch (result) {
    case ShaderStore::ShaderResult::kOk: Count(kShaderOk); break;
    case ShaderStore::ShaderResult::kNoVariant: Count(kVariantMissing); break;
    case ShaderStore::ShaderResult::kBadBlob: Count(kShaderBadBlob); break;
    case ShaderStore::ShaderResult::kCreateFailed: Count(kShaderCreateFailed); break;
  }
}

void FrameValidator::NoteFormat(const char* kind, uint32_t value, const format::Mapping& mapping) {
  FormatUse& use = formats_[{kind, value}];
  use.mapping = mapping;
  ++use.uses;
}

void FrameValidator::CheckInputLayout(const uint8_t* payload, const ShaderStore::Entry& vs,
                                      bool* packed_normal) {
  const DrawView view(payload);
  const DrawRecord& r = view.record();
  auto table = velems_.find(vs.key);
  if (table == velems_.end()) {
    shaders_.GetVertexElements(vs, scratch_velems_);
    table = velems_.emplace(vs.key, scratch_velems_).first;
  }
  const std::vector<ShaderStore::VertexElement>& velems = table->second;
  if (velems.empty()) {
    Count(kInputNoTable);
    return;
  }
  if (r.decl_count == 0) {
    Count(kInputNoDecl);
    return;
  }
  bool ok = true;
  for (const ShaderStore::VertexElement& ve : velems) {
    if (!format::UsageSemantic(ve.usage)) {
      Count(kInputUnsupportedUsage);
      ok = false;
      continue;
    }
    // The first declaration element with the same usage and index: the rule of
    // the D3D vfetch patcher (sub_82423A38), as the legacy geometry follows it.
    bool found = false;
    nfx::DeclarationElement match;
    for (uint32_t d = 0; d < r.decl_count; ++d) {
      const nfx::DeclarationElement e = nfx::ReadDeclarationElement(view.decl_records(), d);
      if (e.stream == 0xFF) {
        break;  // D3DDECL_END
      }
      if (e.usage == ve.usage && e.usage_index == ve.usage_index) {
        match = e;
        found = true;
        break;
      }
    }
    if (!found) {
      // The vfetch stays unpatched and the hardware reads zeros: a stream of
      // zeros, not a failure (legacy measurement, geometry.cpp).
      Count(kInputUnsupplied);
      continue;
    }
    const format::Mapping mapping = format::VertexFormat(match.type);
    NoteFormat("vertex", match.type, mapping);
    if (mapping.status == format::Status::kUnknown) {
      Count(kInputUnknownDecltype);
      ok = false;
      continue;
    }
    if (mapping.status == format::Status::kMissingInPlume) {
      Count(kInputMissingInPlume);
      ok = false;
    }
    if ((ve.usage == 3 || ve.usage == 6 || ve.usage == 7) &&
        format::IsPackedNormalDeclType(match.type)) {
      *packed_normal = true;
    }
    // An inline draw's vertex fetch went straight into the command buffer.
    if (r.kind != DrawKind::kInline) {
      const uint32_t slot = nfx::FetchSlotForStream(match.stream);
      bool valid = false;
      if (slot < guest::kFetchGroupCount * 3) {
        uint32_t d0 = 0, d1 = 0;
        view.VertexFetch(slot, &d0, &d1);
        valid = nfx::DecodeVertexFetch(d0, d1).valid();
      }
      if (!valid) {
        Count(kInputNoVertexFetch);
        ok = false;
      }
    }
  }
  if (ok) {
    Count(kInputOk);
  }
}

void FrameValidator::ApplyConstants(const uint8_t* payload) {
  ConstantBanks::Check check;
  banks_.Apply(DrawView(payload), &check);
  if (check.done) {
    Count(kBankChecks);
    if (!check.vs_matches) {
      Count(kBankMismatchVs);
    }
    if (!check.ps_matches) {
      Count(kBankMismatchPs);
    }
  }
}

void FrameValidator::OnDraw(const uint8_t* payload) {
  const auto t0 = Clock::now();
  const DrawView view(payload);
  const DrawRecord& r = view.record();
  const RegisterWindow regs = view.registers();
  ++frame_draws_;
  ++frame_records_;
  Count(kDraws);
  Count(r.kind == DrawKind::kIndexed ? kDrawsIndexed
                                     : r.kind == DrawKind::kAuto ? kDrawsAuto : kDrawsInline);
  const uint32_t flags = r.flags;
  if (flags & kDrawVsUnreadable) Count(kVsUnreadable);
  if (flags & kDrawPsUnreadable) Count(kPsUnreadable);
  if (flags & kDrawNoPixelShader) Count(kNoPixelShader);
  if (flags & kDrawDeclUnreadable) Count(kDeclUnreadable);
  if (flags & kDrawDeclTooLong) Count(kDeclTooLong);
  if (flags & kDrawIndexUnreadable) Count(kIndexUnreadable);
  if (flags & kDrawInlineUnreadable) Count(kInlineUnreadable);
  if (flags & kDrawInlineUnterminated) Count(kInlineUnterminated);
  if (present_device_ != 0 && r.device != present_device_) {
    Count(kOtherDeviceDraws);
  }
  // Byte dev+10940 is the most significant byte of the captured dword.
  const uint32_t device_flags = r.device_flags >> 24;
  if (device_flags & 0x01) Count(kPredicatedDraws);
  if (device_flags & 0x40) Count(kZPassDraws);
  if (r.viz_query_index != 0) Count(kVizQueryDraws);
  {
    const uint32_t value = regs.Register(guest::reg::kRbSampleCountCtl);
    const auto it = sample_count_ctl_.find(value);
    if (it != sample_count_ctl_.end()) {
      ++it->second;
    } else if (sample_count_ctl_.size() < 16) {
      sample_count_ctl_.emplace(value, 1);
    }
  }
  if (r.kind == DrawKind::kIndexed && !(flags & kDrawIndexUnreadable) &&
      nfx::DecodeIndexBuffer(r.index_dword0, r.index_address).indices_32bit) {
    Count(kIndex32Bit);
  }

  // Shaders, their variants and their objects.
  const ShaderStore::Entry* vs =
      NoteShader(r.vs_identity, false, r.vs_ucode_size, !(flags & kDrawVsUnreadable));
  const ShaderStore::Entry* ps =
      (flags & kDrawNoPixelShader)
          ? nullptr
          : NoteShader(r.ps_identity, true, r.ps_ucode_size, !(flags & kDrawPsUnreadable));
  const bool computed = (flags & kDrawVsComputedFetchIndex) != 0;
  if (computed) Count(kComputedFetchDraws);
  if (vs && ((vs->flags & ShaderStore::kComputedFetchIndex) != 0) != computed) {
    Count(kComputedFetchMismatch);
  }
  if (vs) {
    bool packed_normal = false;
    CheckInputLayout(payload, *vs, &packed_normal);
    if (packed_normal) Count(kVsVariant1);
    CheckShaderObject(*vs, packed_normal ? 1 : 0);
  }
  if (ps) {
    // The ALPHA_TEST variant: coverage depends on alpha (contract v6, Xenia's
    // DoesCoverageDependOnAlpha) -- RB_COLORCONTROL alpha test (bit 3) with a
    // function (bits 0-2) other than ALWAYS (7), or alpha to mask (bit 4).
    const uint32_t color_control = regs.Register(guest::reg::kRbColorControl);
    const bool alpha_coverage = ((color_control & 0x8u) != 0 && (color_control & 0x7u) != 7u) ||
                                (color_control & 0x10u) != 0;
    if (alpha_coverage) Count(kPsVariant1);
    CheckShaderObject(*ps, alpha_coverage ? 1 : 0);
  }

  // Render targets: a colour target is live when its RB_COLOR_MASK nibble is
  // non-zero and a surface is bound; depth when stencil, Z test or Z write is.
  static constexpr uint32_t kColorInfo[4] = {0x2001, 0x2003, 0x2004, 0x2005};
  const uint32_t color_mask = regs.Register(guest::reg::kRbColorMask);
  for (uint32_t i = 0; i < 4; ++i) {
    if (((color_mask >> (4 * i)) & 0xFu) == 0 || r.surfaces[i] == 0) {
      continue;
    }
    Count(kColorTargets);
    const uint32_t value = (regs.Register(kColorInfo[i]) >> 16) & 0xFu;
    const format::Mapping mapping = format::ColorTargetFormat(value);
    NoteFormat("color", value, mapping);
    if (mapping.status == format::Status::kMissingInPlume) Count(kColorFormatMissing);
    if (mapping.status == format::Status::kUnknown) Count(kColorFormatUnknown);
  }
  if (r.surfaces[4] != 0 && (regs.Register(guest::reg::kRbDepthControl) & 0x7u) != 0) {
    Count(kDepthTargets);
    const uint32_t value = (regs.Register(guest::reg::kRbDepthInfo) >> 16) & 0x1u;
    const format::Mapping mapping = format::DepthTargetFormat(value);
    NoteFormat("depth", value, mapping);
    if (mapping.status != format::Status::kOk) Count(kDepthFormatUnknown);
  }

  // Textures: every sampler with a texture bound; its fetch group is the
  // sampler's (SetTexture writes dev+1152+24*sampler).
  for (uint32_t s = 0; s < guest::kSamplerCount; ++s) {
    if (r.sampler_textures[s] == 0) {
      continue;
    }
    Count(kTexturesBound);
    uint32_t d[6];
    view.FetchGroup(s, d);
    const nfx::TextureFetch fetch = nfx::DecodeTextureFetch(d);
    if (!fetch.type_valid) {
      Count(kTextureFetchInvalid);
      continue;
    }
    const format::Mapping mapping = format::TextureFormat(fetch.format);
    NoteFormat("texture", fetch.format, mapping);
    if (mapping.status == format::Status::kMissingInPlume) Count(kTextureFormatMissing);
    if (mapping.status == format::Status::kUnknown) Count(kTextureFormatUnknown);
  }

  ApplyConstants(payload);

  // Polygon offset, from the packed 0x2380 block (dev+10832; the legacy reads
  // dev+11192).
  if (regs.Register(guest::reg::kPaSuScModeCntl) & (3u << 11)) {
    Count(kPolyOffsetDraws);
    const std::array<float, 4> values = {
        regs.RegisterFloat(0x2380), regs.RegisterFloat(0x2381), regs.RegisterFloat(0x2382),
        regs.RegisterFloat(0x2383)};
    if (poly_offsets_.size() < 16 &&
        std::find(poly_offsets_.begin(), poly_offsets_.end(), values) == poly_offsets_.end()) {
      poly_offsets_.push_back(values);
    }
  }
  window_decode_ns_ += uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
}

// ---------------------------------------------------------------------------
// clear, resolve, tiling, present
// ---------------------------------------------------------------------------

void FrameValidator::OnClear(const ClearRecord& r) {
  ++frame_records_;
  Count(kClears);
  if (r.clear_flags & 0xFu) Count(kClearColor);
  if (r.clear_flags & 0x10u) Count(kClearDepth);
  if (r.clear_flags & 0x20u) Count(kClearStencil);
  Count(r.rect_ptr == 0 ? kClearWholeSurface : kClearRect);
  if (r.flags & (kEventRectUnreadable | kEventColorUnreadable | kEventSurfaceUnreadable)) {
    Count(kClearUnreadable);
  }
  const RegisterWindow pre(r.state.pre, guest::kEventWindowBegin, guest::kEventWindowBytes);
  bool any = false;
  for (uint32_t i = 0; i < guest::kSurfaceCount; ++i) {
    any |= pre.DeviceDword(guest::kDevColorSurfaces + 4 * i) != 0;
  }
  if (!any) Count(kClearNothingBound);
  ++clear_callers_[r.caller];
}

void FrameValidator::OnResolve(const ResolveRecord& r) {
  ++frame_records_;
  Count(kResolves);
  if (r.flags & kEventInsideEndTiling) Count(kResolvesNested);
  if (r.flags & kEventStackUnreadable) Count(kResolveStackUnreadable);
  if (r.resolve_flags & 0x300u) Count(kResolveWithClear);
  const RegisterWindow pre(r.state.pre, guest::kEventWindowBegin, guest::kEventWindowBytes);
  const uint32_t source = r.resolve_flags & 7u;
  const uint32_t surface =
      source <= 4 ? pre.DeviceDword(guest::kDevColorSurfaces + 4 * source) : 0;
  if (surface == 0) Count(kResolveSourceUnbound);
  if (r.flags & kEventHasDest) {
    uint32_t d[6];
    TextureHeaderFetch(r.dest_header, d);
    const nfx::TextureFetch fetch = nfx::DecodeTextureFetch(d);
    if (!fetch.type_valid) {
      Count(kResolveDestFormatUnknown);
    } else {
      const format::Mapping mapping = format::TextureFormat(fetch.format);
      NoteFormat("resolve_dest", fetch.format, mapping);
      if (mapping.status == format::Status::kUnknown) Count(kResolveDestFormatUnknown);
      if (mapping.status == format::Status::kMissingInPlume) Count(kResolveDestFormatMissing);
    }
  } else if (r.dest_ptr != 0) {
    Count(kResolveDestUnreadable);
  }
  ++resolve_callers_[r.caller];
}

void FrameValidator::OnTilingBegin(const TilingBeginRecord& r) {
  ++frame_records_;
  Count(kTilingBegins);
  Count(kTiles, std::min(r.tile_count, guest::kMaxTiles));
}

void FrameValidator::OnTilingEnd(const TilingEndRecord& r) {
  ++frame_records_;
  Count(kTilingEnds);
  Count(kTilingNestedResolves, r.nested_resolves);
}

void FrameValidator::OnPresent(const PresentRecord& r) {
  Count(kPresents);
  present_device_ = r.device;
  if (r.flags & kPresentFrontBufferUnreadable) {
    Count(kFrontBufferUnreadable);
  } else if (r.front_buffer != 0) {
    uint32_t d[6];
    TextureHeaderFetch(r.front_buffer_header, d);
    const nfx::TextureFetch fetch = nfx::DecodeTextureFetch(d);
    const format::Mapping mapping =
        fetch.type_valid ? format::TextureFormat(fetch.format) : format::Mapping{};
    NoteFormat("front_buffer", fetch.format, mapping);
    if (mapping.status != format::Status::kOk) Count(kFrontBufferFormatUnknown);
  }
  const GuestFrameStats& s = r.stats;
  // The guest counted every record it wrote for this frame, the present too.
  ++frame_records_;
  if (frame_draws_ != s.draws) Count(kFramesDrawCountMismatch);
  if (frame_records_ != s.records) Count(kFramesRecordCountMismatch);
  frame_draws_ = 0;
  frame_records_ = 0;
  Count(kGuestUnreadable, s.unreadable);
  Count(kIdentityNew, s.identity_new);
  Count(kIdentityRenormalized, s.identity_renormalized);
  Count(kConstantBlocksMasked, s.constant_blocks_masked);
  Count(kConstantBlocksUnmasked, s.constant_blocks_unmasked);
  ++window_frames_;
  window_capture_ns_ += s.capture_ns;
  window_bytes_ += s.bytes;
  if (total_[kPresents] % kReportInterval == 0) {
    Report("periodic");
  }
}

// ---------------------------------------------------------------------------
// reports
// ---------------------------------------------------------------------------

void FrameValidator::Report(const char* why) {
  const auto& w = window_;
  const uint64_t frames = std::max<uint64_t>(window_frames_, 1);
  const uint64_t draws = std::max<uint64_t>(w[kDraws], 1);
  const uint64_t vs_seen = w[kVsInPack] + w[kVsNotInPack];
  const uint64_t ps_seen = w[kPsInPack] + w[kPsNotInPack];
  std::string missing;
  for (const auto& [key, use] : formats_) {
    if (use.mapping.status == format::Status::kOk) {
      continue;
    }
    if (!missing.empty()) {
      missing += ", ";
    }
    missing += key.first + " " + use.mapping.guest_name + " (" +
               format::StatusName(use.mapping.status) + ") " + std::to_string(use.uses);
  }
  REXLOG_INFO(
      "[gpu] validation ({}, {} frames): {:.0f} draws/frame ({} indexed, {} auto, {} inline) | "
      "VS {:.3f}% / PS {:.3f}% in pack, shaders ok {} bad {} failed {}, variant missing {}, "
      "{} created | input ok {:.2f}% (no table {}, unsupplied {}, unknown decltype {}, no "
      "vfetch {}) | banks {} checks, {}/{} mismatches, {} blocks changed outside the masks | "
      "computed fetch mismatch {} | frames with lost/extra draws {} records {} | other-device draws {}, viz {}, predicated "
      "{}, Z pass {}, poly offset {} | {:.1f} clears, {:.1f} resolves ({} nested), {:.1f} tiling "
      "begins/frame | guest capture {:.3f} ms/frame ({:.2f} us/draw), render decode {:.3f} "
      "ms/frame, stream {:.2f} MB/frame | formats not ok: {}",
      why, window_frames_, double(w[kDraws]) / frames, w[kDrawsIndexed], w[kDrawsAuto],
      w[kDrawsInline], Percent(w[kVsInPack], vs_seen), Percent(w[kPsInPack], ps_seen),
      w[kShaderOk], w[kShaderBadBlob], w[kShaderCreateFailed], w[kVariantMissing],
      shaders_.stats().shaders_created, Percent(w[kInputOk], w[kVsInPack]), w[kInputNoTable],
      w[kInputUnsupplied], w[kInputUnknownDecltype], w[kInputNoVertexFetch], w[kBankChecks],
      w[kBankMismatchVs], w[kBankMismatchPs], w[kConstantBlocksUnmasked],
      w[kComputedFetchMismatch], w[kFramesDrawCountMismatch], w[kFramesRecordCountMismatch],
      w[kOtherDeviceDraws], w[kVizQueryDraws],
      w[kPredicatedDraws], w[kZPassDraws], w[kPolyOffsetDraws], double(w[kClears]) / frames,
      double(w[kResolves]) / frames, w[kResolvesNested], double(w[kTilingBegins]) / frames,
      window_capture_ns_ / 1e6 / frames, window_capture_ns_ / 1e3 / draws,
      window_decode_ns_ / 1e6 / frames, window_bytes_ / 1e6 / frames,
      missing.empty() ? "none" : missing);
  std::string sample_counts;
  for (const auto& [value, n] : sample_count_ctl_) {
    char text[48];
    std::snprintf(text, sizeof(text), "%s0x%08X %llu", sample_counts.empty() ? "" : ", ", value,
                  static_cast<unsigned long long>(n));
    sample_counts += text;
  }
  REXLOG_INFO("[gpu] validation callers: clears [{}] | resolves [{}] | RB_SAMPLE_COUNT_CTL [{}]",
              Callers(clear_callers_), Callers(resolve_callers_),
              sample_counts.empty() ? "none" : sample_counts);
  if (!poly_offsets_.empty()) {
    std::string values;
    for (const auto& v : poly_offsets_) {
      char text[96];
      std::snprintf(text, sizeof(text), "%s(%g, %g / %g, %g)", values.empty() ? "" : " ", v[0],
                    v[1], v[2], v[3]);
      values += text;
    }
    REXLOG_INFO("[gpu] validation: polygon offsets (front scale, offset / back scale, offset) "
                "from 0x2380: {}",
                values);
  }
  total_frames_ += window_frames_;
  total_decode_ns_ += window_decode_ns_;
  total_capture_ns_ += window_capture_ns_;
  total_bytes_ += window_bytes_;
  window_.fill(0);
  window_frames_ = 0;
  window_decode_ns_ = 0;
  window_capture_ns_ = 0;
  window_bytes_ = 0;
  WriteCsv();
}

void FrameValidator::Finish() {
  if (window_frames_ != 0 || total_frames_ == 0) {
    Report("shutdown");
  } else {
    WriteCsv();
  }
}

void FrameValidator::WriteCsv() const {
  const std::filesystem::path dir = rex::filesystem::GetExecutableFolder();
  {
    std::ofstream f(dir / "mcla_gpu_validation_shaders.csv", std::ios::binary | std::ios::trunc);
    f << "identity,stage,ucode_bytes,draws,in_pack,blob_bytes\n";
    char line[96];
    for (const auto& [identity, info] : shader_table_) {
      std::snprintf(line, sizeof(line), "%016llX,%s,%u,%llu,%d,%u\n",
                    static_cast<unsigned long long>(identity), info.pixel ? "ps" : "vs",
                    info.ucode_bytes, static_cast<unsigned long long>(info.draws),
                    info.entry ? 1 : 0, info.blob_bytes);
      f << line;
    }
  }
  {
    std::ofstream f(dir / "mcla_gpu_validation_summary.csv", std::ios::binary | std::ios::trunc);
    f << "counter,total\n";
    for (uint32_t i = 0; i < kCounterCount; ++i) {
      f << kCounterNames[i] << ',' << total_[i] << '\n';
    }
    f << "frames," << total_frames_ << '\n';
    f << "guest_capture_ns," << total_capture_ns_ << '\n';
    f << "render_decode_ns," << total_decode_ns_ << '\n';
    f << "stream_bytes," << total_bytes_ << '\n';
    f << "shaders_created," << shaders_.stats().shaders_created << '\n';
    f << "blobs_verified," << shaders_.stats().blobs_verified << '\n';
  }
  {
    std::ofstream f(dir / "mcla_gpu_validation_formats.csv", std::ios::binary | std::ios::trunc);
    f << "kind,guest_value,guest_name,plume,status,uses\n";
    char line[192];
    for (const auto& [key, use] : formats_) {
      std::snprintf(line, sizeof(line), "%s,0x%08X,%s,%s,%s,%llu\n", key.first.c_str(), key.second,
                    use.mapping.guest_name, format::PlumeFormatName(use.mapping.format),
                    format::StatusName(use.mapping.status),
                    static_cast<unsigned long long>(use.uses));
      f << line;
    }
  }
}

}  // namespace mcla::gpu
