// MCLA GPU renderer -- the replay of the guest's frames. See scene.h.

#include "scene.h"

#include <algorithm>
#include <bit>
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
#include "cycles.h"
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
#include "thread_load.h"
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
    "(mcla_gpu_resolve_memory_encode_delay), the scale classes (mcla_gpu_scale_shadow, "
    "_reflection, _post; these at the next start) and the encode batch counters. Off: as before "
    "that day. Hot reload.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(
    mcla_gpu_scale_depth_bias, true, "MCLA/GPU",
    "With a resolution scale above 1 (M6b.3c): the slope-scaled depth bias times the "
    "scale of the draw's targets, as Xenia does -- a host pixel is 1/scale of the guest's. Off: "
    "the guest's factor unchanged (for A/B). Hot reload.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

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

// The resolution scale of a draw's targets: the class of its
// first identity -- RT0, or the depth target when it writes no colour -- as
// Scene::Record finds them (and checks that every other target agrees).
uint32_t DrawScale(const EdramTargets& edram, const XenosState& s, const PipelineKey& key) {
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
  } else {
    return 1;
  }
  return edram.ScaleFor(first);
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
  // Host targets at the guest's size for now: the pixel position in guest
  // pixels is the host's (contract v7; M6b.2 sets it per target).
  base_shared_.inv_resolution_scale[0] = 1.0f;
  base_shared_.inv_resolution_scale[1] = 1.0f;
  // The vector bank swap against the scalar one, once.
  VerifySwapBankToHost();
  cycles::Measure();  // the base of the first window's TSC rate
  ab::Initialize();
  profile_ = REXCVAR_GET(mcla_gpu_scene_profile);
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
  ++frame_serial_;
  watch::BeginFrame(frame_serial_);
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

// ---------------------------------------------------------------------------
// draws
// ---------------------------------------------------------------------------

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
      const Skip recorded = Record(d, r, list, &empty, &detail);
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
  const PipelineInputs inputs{d.vs, d.ps, d.input_layout};
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
  // The scale of the draw's targets, and the slope-scaled depth
  // bias in its host pixels (M6b.3c): a host pixel is 1/scale of the guest's,
  // so is the depth slope across it -- Xenia multiplies the slope factor by
  // the scale. The constant part is in depth units and stays.
  const uint32_t scale = DrawScale(*edram_, s, d.key);
  if (scale != 1 && d.bias.slope != 0.0f && scale_depth_bias_) {
    d.bias.slope *= float(scale);
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
    d.pipeline = pipelines_->Get(d.key, inputs);
    if (d.pipeline) {
      pipeline_memo_key_ = d.key;
      pipeline_memo_ = d.pipeline;
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
      memo.scale == scale) {
    d.viewport = memo.viewport;
    shared = memo.shared;
  } else {
    // At the scale of the draw's targets (DrawScale above).
    d.viewport = ComputeViewport(s, ps_writes_depth, scale);
    shared = base_shared_;
    issues = FillSharedConstants(s, d.viewport, shared);
    // The pixel position a pixel shader receives, in guest pixels (v7).
    shared.inv_resolution_scale[0] = 1.0f / float(scale);
    shared.inv_resolution_scale[1] = 1.0f / float(scale);
    memo.constants_valid = true;
    memo.ps_writes_depth = ps_writes_depth;
    memo.scale = scale;
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
  Lap(Stage::kConstants);
  const uint32_t fetch_mask = d.vs->texture_fetch_mask | (d.ps ? d.ps->texture_fetch_mask : 0u);
  FillTextureTables(view, fetch_mask, shared);
  Lap(Stage::kTextures);
  if (!UploadConstants(r.device, banks, shared, d)) {
    return Skip::kUpload;
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
    const auto holds = [&](const FetchMemo& m) {
      return m.epoch == textures_->epoch() && (m.frame == 0 || m.frame == frame_serial_) &&
             m.generation == textures_->ProductGeneration(m.page) &&
             std::memcmp(m.dwords, dwords, sizeof(dwords)) == 0;
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
      fresh.sampler = samplers_->Get(dwords, fetch.mip_min, fetch.mip_max);
      // Only 2D textures are made (100% of the measured fetches);
      // the cache logs any other dimension once and the null of its dimension
      // is bound.
      const TextureCache::Binding bound = textures_->Bind2D(fetch, list_);
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
  const uint32_t samples = MsaaSampleCount(s.msaa);
  EdramTargets::Target* colors[kColorTargetCount] = {};
  EdramTargets::Target* depth = nullptr;
  const uint32_t pitch = s.surface_pitch;
  // The draw's EDRAM identities.
  EdramKey color_keys[kColorTargetCount];
  for (uint32_t i = 0; i < d.key.color_count; ++i) {
    EdramKey& key = color_keys[i];
    key.base_tiles = uint16_t(s.color[i].base_tiles);
    key.pitch = uint16_t(pitch);
    key.depth = 0;
    key.format = uint8_t(FoldColorFormat(uint32_t(s.color[i].format)));
    key.samples = uint8_t(samples);
  }
  const bool has_depth = d.key.depth_format != 0;
  EdramKey depth_key;
  if (has_depth) {
    depth_key.base_tiles = uint16_t(s.depth_base_tiles);
    depth_key.pitch = uint16_t(pitch);
    depth_key.depth = 1;
    depth_key.format = uint8_t(s.depth_format);
    depth_key.samples = uint8_t(samples);
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
      colors[i] = edram_->Get(key, scissor_br_y, list, &unset);
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
      depth = edram_->Get(depth_key, scissor_br_y, list, &unset);
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

  // The targets' scale: one for all of them, as the
  // classes give a draw's colour and depth targets the same (DrawScale made the
  // viewport for the first one's).
  const uint32_t target_scale =
      d.key.color_count ? colors[0]->scale : (depth ? depth->scale : 1u);
  bool scales_agree = !depth || depth->scale == target_scale;
  for (uint32_t i = 0; i < d.key.color_count; ++i) {
    scales_agree &= colors[i]->scale == target_scale;
  }
  if (!scales_agree) {
    const EdramTargets::Target* first = d.key.color_count ? colors[0] : depth;
    *detail = std::format("targets of different resolution scales (pitch {}): {} at {}x, {} at {}x",
                          pitch, DescribeEdramKey(first->key), first->scale,
                          depth ? DescribeEdramKey(depth->key) : std::string("no depth"),
                          depth ? depth->scale : 0u);
    return Skip::kTarget;
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
  const plume::RenderRect scissor = ComputeScissor(s, target_height, target_scale);
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
    // Every sample of the scissor the host draws in.
    const int64_t width = std::min(scissor.right, int32_t(pitch)) - scissor.left;
    const int64_t height = scissor.bottom - scissor.top;
    edram_->BeginCoverageCheck(list, coverage_check,
                               uint64_t(std::max<int64_t>(width, 0) *
                                        std::max<int64_t>(height, 0) * samples));
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
  into.prepare_cycles += from.prepare_cycles;
  into.pipelines_created += from.pipelines_created;
  into.pipelines_failed += from.pipelines_failed;
  into.pipeline_create_ns += from.pipeline_create_ns;
}

void Scene::OnPresent() {
  const uint64_t now = cycles::Now();
  if (last_present_) {
    present_intervals_.push_back(now - last_present_);
  }
  last_present_ = now;
  ++window_.frames;
  draw_census_.OnPresent();
  target_census_.OnPresent();
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
  REXLOG_INFO(
      "[gpu] scene ({}): {} frames | draws {:.1f}/frame: {} prepared, {} recorded, skipped [{}], "
      "geometry [{}], no effect {} (no target {}, culled {}, no primitive {}, empty scissor {}) | "
      "clears {} ({} colour, {} depth, {} empty, {} failed, {} of a surface made at another "
      "pitch) | conversions [{}] | alpha "
      "coverage variant {} (alpha test {}, alpha to mask {}: 1x {}, biased {}), clip disabled "
      "{}, depth bias {} | banks {} checks, {} mismatches | "
      "pipelines {} total (+{} created, {} failed, {:.1f} "
      "ms; precache {} taken, {} waited {:.1f} ms, {} made here, {} shared) | input layouts {} | samplers {} ({} fetches, {} not a texture) | constants {:.2f} "
      "MiB/frame ({} bank, {} shared uploads) | fetch memo {:.0f}% | clears {}, resolves {}, "
      "tiling {}/{} | prepare "
      "{:.2f} ms/frame | {}",
      why, w.frames, double(w.draws) / frames, w.prepared, w.recorded,
      skips.empty() ? "none" : skips, geometry.empty() ? "ok" : geometry,
      w.no_effect[0] + w.no_effect[1] + w.no_effect[2] + w.no_effect[3], w.no_effect[0],
      w.no_effect[1], w.no_effect[2], w.no_effect[3], w.clears, w.clears_color, w.clears_depth,
      w.clears_empty, w.clears_failed, w.clears_other_pitch,
      conversions.empty() ? "none" : conversions, w.alpha_coverage_draws, w.alpha_test_draws,
      w.alpha_to_mask_draws, w.alpha_to_mask_1x, w.alpha_to_mask_biased, w.clip_disabled_draws,
      w.depth_bias_draws,
      w.bank_checks, w.bank_mismatches, pipelines_->size(), ps.created, ps.failed,
      double(ps.create_ns) / 1e6, ps.precache_taken, ps.precache_waits,
      double(ps.precache_wait_ns) / 1e6, ps.precache_claimed, ps.shared, input_layouts_.size(), samplers_->size(), w.fetches,
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
  Add(total_, window_);
  window_ = Stats{};
}

}  // namespace mcla::gpu
