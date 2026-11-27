// MCLA GPU renderer -- the side mirrors' glass overlay. See mirror_overlay.h and mirror_overlay_rules.h.

#include "mirror_overlay.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "backend.h"
#include "descriptors.h"
#include "device.h"
#include "frame.h"
#include "mirror_glass.h"
#include "mirror_setting.h"
#include "mirror_view.h"
#include "pass_timing.h"
#include "upscaler_math.h"
#include "upload.h"

// The generated blobs of this pass's two shaders (src/gpu/CMakeLists.txt larecomp_gpu_host_shader), included here beside host_shaders.h's macros so that file is not touched: the arrays have internal linkage.
#if defined(LARECOMP_GPU_HAS_D3D12)
#include "mirror_overlay_vs.hlsl.dxil.h"
#include "mirror_overlay_ps.hlsl.dxil.h"
#endif
#if defined(LARECOMP_GPU_HAS_VULKAN)
#include "mirror_overlay_vs.hlsl.spirv.h"
#include "mirror_overlay_ps.hlsl.spirv.h"
#endif
#include "host_shaders.h"

namespace mcla::gpu {

namespace {

namespace rules = mirror_overlay;
namespace sm = car_shadow_map;
namespace cab = cabin_mirror;

struct OverlayDraw {
  rules::Push push;
  float mask_u[4] = {};
  float mask_v[4] = {};
  float mask_meta[4] = {};
};
static_assert(sizeof(OverlayDraw) == 176);

constexpr const char* kPrefix = "[mirror overlay]";
constexpr plume::RenderFormat kColourFormat = plume::RenderFormat::R16G16B16A16_FLOAT;  // the main colour as a host texture
constexpr plume::RenderFormat kDepthFormat = plume::RenderFormat::D32_FLOAT_S8_UINT;     // the format the renderer's own depth targets use (edram.cpp)
constexpr uint8_t kWriteRgb = uint8_t(plume::RenderColorWriteEnable::RED) | uint8_t(plume::RenderColorWriteEnable::GREEN) | uint8_t(plume::RenderColorWriteEnable::BLUE);
constexpr uint64_t kCensusFrames = 1800;

static_assert(uint32_t(plume::RenderComparisonFunction::LESS) == rules::kCmpLess && uint32_t(plume::RenderComparisonFunction::LESS_EQUAL) == rules::kCmpLessEqual &&
                  uint32_t(plume::RenderComparisonFunction::GREATER) == rules::kCmpGreater && uint32_t(plume::RenderComparisonFunction::GREATER_EQUAL) == rules::kCmpGreaterEqual,
              "mirror_overlay_rules.h repeats plume's comparison values (the pipeline key's depth word holds them)");

HostShaderBlob VertexBlob() { return MCLA_GPU_HOST_SHADER_BLOB(mirror_overlay_vs); }
HostShaderBlob PixelBlob() { return MCLA_GPU_HOST_SHADER_BLOB(mirror_overlay_ps); }

sm::PositionKind KindOf(plume::RenderFormat format) {
  switch (format) {
    case plume::RenderFormat::R32G32B32_FLOAT:
    case plume::RenderFormat::R32G32B32A32_FLOAT:
      return sm::PositionKind::kFloat3;  // a float4's fourth float is not read
    case plume::RenderFormat::R16G16B16A16_FLOAT:
      return sm::PositionKind::kHalf4;
    default:
      return sm::PositionKind::kNone;
  }
}

bool IsTriangles(plume::RenderPrimitiveTopology topology) {
  return topology == plume::RenderPrimitiveTopology::TRIANGLE_LIST || topology == plume::RenderPrimitiveTopology::TRIANGLE_STRIP;
}

constexpr uint32_t TriangleList() { return uint32_t(plume::RenderPrimitiveTopology::TRIANGLE_LIST); }

}  // namespace

MirrorOverlay::MirrorOverlay() = default;
MirrorOverlay::~MirrorOverlay() = default;

// ---------------------------------------------------------------------------
// start and end
// ---------------------------------------------------------------------------

void MirrorOverlay::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, EdramTargets& edram, PassTiming& pass_timing) {
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  passes_ = &passes;
  edram_ = &edram;
  pass_timing_ = &pass_timing;
  base_listed_ = false;
  shaders_failed_ = false;
  // The shaders are small blobs and made at once; what costs is the pipelines (a driver compile each), listed only where the overlay is wanted at the start: the SIDE MIRRORS row's view runs, or the debug switch
  // mcla_gpu_mirror_overlay says so (mirror_setting.h Current().overlay).
  const bool on = mirror_setting::Current().overlay;
  if (!MakeShaders()) {
    passes.precache().Note("mirror overlay: the shaders or the layout could not be made, no overlay", 0);
    return;
  }
  if (on) {
    ListBase(true, "mirror overlay: on at the start (the SIDE MIRRORS row or mcla_gpu_mirror_overlay)");
  } else {
    passes.precache().Note("mirror overlay: off at the start (the SIDE MIRRORS row and mcla_gpu_mirror_overlay): its pipelines are made in the background when it is first wanted", 0);
  }
}

bool MirrorOverlay::MakeShaders() {
  if (ready_) {
    return true;
  }
  if (shaders_failed_ || !device_ || !passes_ || !heap_) {
    return false;
  }
  // The host pass texture and sampler sets, the glass vertex input, and the
  // per-draw constants (a root CBV on D3D12, an uploaded address on Vulkan).
  plume::RenderPipelineLayoutBuilder builder;
  builder.begin(/*isLocal=*/false, /*allowInputLayout=*/true);
  for (uint32_t set = 0; set < 4; ++set) {
    builder.addDescriptorSet(heap_->texture_set_builder());
  }
  builder.addDescriptorSet(heap_->sampler_set_builder());
  if (ActiveBackend() == Backend::kD3D12) {
    builder.addRootDescriptor(0, 4, plume::RenderRootDescriptorType::CONSTANT_BUFFER);
  } else {
    builder.addPushConstant(0, 4, sizeof(uint64_t), plume::RenderShaderStageFlag::VERTEX | plume::RenderShaderStageFlag::PIXEL);
  }
  builder.end();
  layout_ = device_->CreatePipelineLayout(builder, "mirror overlay layout");
  const HostShaderBlob vs = VertexBlob();
  const HostShaderBlob ps = PixelBlob();
  vertex_shader_ = device_->CreateShader(vs.data, vs.size, "main", vs.name);
  pixel_shader_ = device_->CreateShader(ps.data, ps.size, "main", ps.name);
  if (!layout_ || !vertex_shader_ || !pixel_shader_) {
    REXLOG_ERROR("{} a shader or the pipeline layout could not be made: no glass overlay this session", kPrefix);
    layout_.reset();
    vertex_shader_.reset();
    pixel_shader_.reset();
    shaders_failed_ = true;
    return false;
  }
  ready_ = true;
  return true;
}

void MirrorOverlay::Shutdown() {
  // The host builder's functions name this object's shaders and layout: stop it first (CarShadowMap::Shutdown).
  if (passes_) {
    passes_->precache().Stop();
  }
  cabin_mask_.Shutdown();
  mirror_view::PublishCabinDrawn(false);
  cabin_drawn_ = cabin_pose_ = false;
  cabin_log_key_ = 0;
  cabin_status_ = {};
  pipelines_.clear();
  listed_.clear();
  failed_.clear();
  to_list_.clear();
  vertex_shader_.reset();
  pixel_shader_.reset();
  layout_.reset();
  ready_ = false;
  pose_ = rules::ViewPose{};
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
  passes_ = nullptr;
  edram_ = nullptr;
  pass_timing_ = nullptr;
}

// ---------------------------------------------------------------------------
// the pipelines
// ---------------------------------------------------------------------------

uint64_t MirrorOverlay::PrecacheKey(uint32_t variant) { return HostPrecache::Key(HostPrecache::kMirrorOverlay, variant); }

std::string MirrorOverlay::VariantName(uint32_t variant) const {
  const uint32_t vertex = rules::VertexOf(variant);
  return std::format("mirror overlay ({} stride {}, {}, {} samples, {})", sm::VariantKind(vertex) == sm::PositionKind::kHalf4 ? "half4" : "float3", sm::VariantStride(vertex),
                     sm::VariantTopology(vertex) == uint32_t(plume::RenderPrimitiveTopology::TRIANGLE_STRIP) ? "strip" : "list", rules::SamplesOf(rules::SampleIndexOf(variant)),
                     rules::TestOf(variant) == rules::DepthTest::kGreaterEqual ? "greater or equal" : "less or equal");
}

std::unique_ptr<plume::RenderPipeline> MirrorOverlay::MakePipeline(uint32_t variant) {
  if (!device_ || !ready_) {
    return nullptr;
  }
  const uint32_t vertex = rules::VertexOf(variant);
  const sm::PositionKind kind = sm::VariantKind(vertex);
  const plume::RenderInputSlot slot(0, sm::VariantStride(vertex));
  // The element is at offset 0: the vertex view is shifted by the position's offset when the draw is recorded.
  const plume::RenderInputElement element("POSITION", 0, 0, kind == sm::PositionKind::kHalf4 ? plume::RenderFormat::R16G16B16A16_FLOAT : plume::RenderFormat::R32G32B32_FLOAT, 0, 0);
  plume::RenderBlendDesc blend = plume::RenderBlendDesc::Copy();
  blend.blendEnabled = true;
  blend.srcBlend = plume::RenderBlend::SRC_ALPHA;
  blend.dstBlend = plume::RenderBlend::INV_SRC_ALPHA;
  blend.blendOp = plume::RenderBlendOperation::ADD;
  blend.srcBlendAlpha = plume::RenderBlend::ZERO;
  blend.dstBlendAlpha = plume::RenderBlend::ONE;
  blend.blendOpAlpha = plume::RenderBlendOperation::ADD;
  blend.renderTargetWriteMask = kWriteRgb;
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = layout_.get();
  desc.vertexShader = vertex_shader_.get();
  desc.pixelShader = pixel_shader_.get();
  desc.inputSlots = &slot;
  desc.inputSlotsCount = 1;
  desc.inputElements = &element;
  desc.inputElementsCount = 1;
  desc.primitiveTopology = plume::RenderPrimitiveTopology(sm::VariantTopology(vertex));
  desc.multisampling = plume::RenderMultisampling(plume::RenderSampleCounts(rules::SamplesOf(rules::SampleIndexOf(variant))));
  desc.depthTargetFormat = kDepthFormat;
  desc.depthEnabled = true;
  desc.depthWriteEnabled = false;  // the glass has its depth already (the game drew it); the overlay never writes any
  desc.depthFunction = rules::TestOf(variant) == rules::DepthTest::kGreaterEqual ? plume::RenderComparisonFunction::GREATER_EQUAL : plume::RenderComparisonFunction::LESS_EQUAL;
  desc.depthClipEnabled = true;
  // Both faces: the glass patch's winding says nothing here (the depth test decides what shows).
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.fillMode = plume::RenderFillMode::SOLID;
  desc.renderTargetFormat[0] = kColourFormat;
  desc.renderTargetBlend[0] = blend;
  desc.renderTargetCount = 1;
  const std::string name = VariantName(variant);
  return device_->CreateGraphicsPipeline(desc, name.c_str());
}

// The known strides of the player's car meshes at 1, 2 and 4 host samples with the game's reverse-Z test (stored depth 1 near): what the first frames of a session with the setting on need. A variant outside
// it (8 samples, another stride, a forward-Z scene) is made in the background when first asked for.
void MirrorOverlay::ListBase(bool at_start, const std::string& note) {
  std::vector<uint32_t> variants;
  for (uint32_t sample_index = 0; sample_index < 3; ++sample_index) {
    for (uint32_t stride : sm::kKnownStrides) {
      variants.push_back(rules::Variant(sm::VariantKey(sm::PositionKind::kFloat3, TriangleList(), stride), sample_index, rules::DepthTest::kGreaterEqual));
    }
  }
  HostPrecache& precache = passes_->precache();
  if (at_start) {
    uint32_t added = 0;
    for (uint32_t variant : variants) {
      if (listed_.insert(variant).second) {
        added += precache.Add(PrecacheKey(variant), VariantName(variant), [this, variant] { return MakePipeline(variant); }) ? 1u : 0u;
      }
    }
    precache.Note(note, added);
  } else {
    std::vector<HostPrecache::Item> fresh;
    for (uint32_t variant : variants) {
      if (listed_.insert(variant).second) {
        fresh.push_back({PrecacheKey(variant), VariantName(variant), [this, variant] { return MakePipeline(variant); }});
      }
    }
    const uint32_t count = precache.AddLive("mirror overlay", std::move(fresh));
    REXLOG_INFO("{} the overlay was off at the start: {} pipelines are made in the background, and the glass is drawn from the frame those are done", kPrefix, count);
  }
  base_listed_ = true;
}

void MirrorOverlay::FlushListing() {
  if (to_list_.empty()) {
    return;
  }
  std::vector<HostPrecache::Item> fresh;
  for (uint32_t variant : to_list_) {
    fresh.push_back({PrecacheKey(variant), VariantName(variant), [this, variant] { return MakePipeline(variant); }});
  }
  to_list_.clear();
  passes_->precache().AddLive("mirror overlay", std::move(fresh));
}

plume::RenderPipeline* MirrorOverlay::Pipeline(uint32_t variant, bool* pending) {
  const auto found = pipelines_.find(variant);
  if (found != pipelines_.end()) {
    return found->second.get();
  }
  if (failed_.count(variant) != 0) {
    return nullptr;
  }
  if (listed_.insert(variant).second) {
    // Nobody listed it: the background builder makes it; no overlay until it is made.
    to_list_.push_back(variant);
    *pending = true;
    return nullptr;
  }
  HostPrecache& precache = passes_->precache();
  if (!precache.Ready(PrecacheKey(variant))) {
    *pending = true;
    return nullptr;
  }
  std::unique_ptr<plume::RenderPipeline> made = precache.Acquire(PrecacheKey(variant), VariantName(variant), [this, variant] { return MakePipeline(variant); });
  if (!made) {
    failed_.insert(variant);
    REXLOG_ERROR("{} the pipeline '{}' could not be made: no overlay for the glass that needs it", kPrefix, VariantName(variant));
    return nullptr;
  }
  return (pipelines_[variant] = std::move(made)).get();
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

void MirrorOverlay::BeginFrame() {
  cabin_drawn_ = false;
  ++frame_;
  on_ = mirror_setting::Current().overlay;  // the row's view or the debug switch (mirror_setting.h); the rules still decide per frame whether a picture, a pair and a pose are there
  distance_ = rules::ClampDistance(larecomp::settings::mcla_gpu_mirror_view_t0.Get());
  if (on_ && ready_ && !base_listed_ && passes_) {
    // Switched on after the start: the base pipelines are made in the background.
    ListBase(false, std::string());
  }
  Census(false);
}

void MirrorOverlay::Count(rules::Why why) {
  ++why_[size_t(why)];
  ++census_total_;
}

void MirrorOverlay::Census(bool force) {
  if (!force && frame_ < census_frame_ + kCensusFrames) {
    return;
  }
  census_frame_ = frame_;
  if (census_total_ == 0 && !force) {
    return;  // nothing armed since the last line (the switch off, or no mirror phase)
  }
  std::string reasons;
  for (size_t i = size_t(rules::Why::kSettingOff); i < size_t(rules::Why::kCount); ++i) {
    if (why_[i] != 0) {
      reasons += std::format("{}{} {}", reasons.empty() ? "" : ", ", why_[i], rules::WhyName(rules::Why(i)));
    }
  }
  REXLOG_INFO("{} frame {}: armed in {} frames, the glass drawn in {} ({} glass draws); not drawn in: {}", kPrefix, frame_, census_total_, why_[size_t(rules::Why::kDraw)], drawn_glasses_,
              reasons.empty() ? "none" : reasons);
  for (uint64_t& w : why_) {
    w = 0;
  }
  census_total_ = 0;
  drawn_glasses_ = 0;
}

// The end marker of a phase that drew a fresh picture: the camera it was drawn from and the field it was built with. THE ONE GLUE POINT WITH THE GUEST HALF (perf/MIRROR-VIEW.md 13.3, 15.1 and 16.3): the
// field is the one the camera of THIS phase used, which the begin marker carried (mirror_view_link.h PhaseField, MirrorView::PictureField), not the guest's latest camera build: the guest runs one to three
// frames ahead of the render thread, and the latest build can be a later phase's, with another field (a mirror coming into view or the HUD stamp coming and going changes it), which would put the lookup
// at the wrong place in this picture for a frame or two. Not known = no camera had been built when the phase began: the overlay then draws nothing (Why::kNoField) rather than a mirror in the wrong place.
void MirrorOverlay::NoteViewDrawn(const car_frame::Frame& frame, uint64_t guest_frame, bool field_known, float field_h, float field_v, uint32_t cabin_offset) {
  if (!on_) {
    return;
  }
  const float h = field_known ? field_h : 0.0f;
  const float v = field_known ? field_v : 0.0f;
  field_known_ = field_known && h > 0.0f && v > 0.0f;
  field_h_ = h;
  field_v_ = v;
  pose_ = rules::ViewPose{};
  cabin_pose_ = false;
  picture_cabin_offset_ = cabin_offset;
  picture_frame_ = frame_;
  picture_guest_frame_ = guest_frame;
  FinishViewPose(frame);
}

void MirrorOverlay::FinishViewPose(const car_frame::Frame& frame) {
  // The rear phase can finish before the main vehicle's draws are available.
  // CHECK: the main-colour copy recovers that phase's pose on both APIs.
  if (!rules::CanFinishViewPose(field_known_, pose_.known, frame.valid, picture_frame_, frame_)) return;
  if (rules::PoseOfView(frame.view, double(field_h_), double(field_v_), picture_guest_frame_, &pose_) && picture_cabin_offset_ != 0) {
    const auto axis = [](const double v[3]) { return cab::V3{v[0], v[1], v[2]}; };
    const auto delta = cab::OffsetInRearAxes(cab::UnpackOffset(picture_cabin_offset_), axis(pose_.right), axis(pose_.up), axis(pose_.forward) * -1.0);
    pose_.eye[0] += delta.x;
    pose_.eye[1] += delta.y;
    pose_.eye[2] += delta.z;
    cabin_pose_ = true;
  }
}

void MirrorOverlay::LogCabin(uint64_t key, std::string_view status) {
  if (cabin_log_key_ == key && cabin_status_ == status) return;
  cabin_log_key_ = key;
  cabin_status_ = status;
  if (key) REXLOG_INFO("[cabin mirror] frame {}: overlay key {:016x}: {}", frame_, key, status);
}

void MirrorOverlay::OnPresent() {
  mirror_view::PublishCabinDrawn(cabin_drawn_);
}

// ---------------------------------------------------------------------------
// the overlay
// ---------------------------------------------------------------------------

bool MirrorOverlay::Record(plume::RenderCommandList* list, const Inputs& in, EdramTargets::Target* colour, EdramTargets::Target* depth) {
  if (!list || !ready_ || !in.frame || !in.glass || !in.view || !in.upload || !device_ || !heap_ || !passes_ || !edram_ || !pass_timing_) {
    return false;
  }
  const MirrorView::Picture picture = in.view->CurrentPicture();
  const mirror_glass::Result& glass = in.glass->Current();
  FinishViewPose(*in.frame);
  const uint64_t cabin_key = glass.glass[cab::kGlassIndex].mesh_key;
  const bool cabin_needed = (glass.need.mask & cab::kReader) != 0;
  if (!cabin_needed) LogCabin(0, {});
  rules::Facts facts;
  facts.setting_on = on_;
  facts.picture_whole = picture.whole && picture.slot != kInvalidBindlessSlot;
  facts.picture_age = picture.age;
  facts.play_view = in.play_view;
  facts.glass_found = glass.found || glass.cabin_found;
  facts.needed_mask = glass.need.mask;
  if (!cabin_pose_) facts.needed_mask &= ~cab::kReader;  // wait for the first image from the virtual eye
  facts.field_known = field_known_;
  facts.pose_known = pose_.known;
  const rules::Why why = rules::Decide(facts);
  if (cabin_needed && !cabin_pose_) {
    LogCabin(cabin_key, picture_cabin_offset_ == 0 ? "rear phase used the stock eye" :
        !field_known_ ? "rear phase field unavailable" : "camera pose unavailable for rear phase");
  }
  if (why != rules::Why::kDraw) {
    if (cabin_needed && cabin_pose_) LogCabin(cabin_key, rules::WhyName(why));
    Count(why);
    return false;
  }
  if (!colour || !depth || !colour->texture || !depth->texture || colour->host_samples != depth->host_samples || colour->scale != depth->scale) {
    if (cabin_needed && cabin_pose_) LogCabin(cabin_key, "main colour/depth pair unavailable");
    Count(rules::Why::kTargets);
    return false;
  }
  const uint32_t sample_index = rules::SampleIndex(colour->host_samples);
  if (sample_index > 3) {
    if (cabin_needed && cabin_pose_) LogCabin(cabin_key, "unsupported main view sample count");
    Count(rules::Why::kTargets);
    return false;
  }

  // The draws: each needed glass with its pipeline and its push block.
  struct Item {
    const car_frame::Draw* draw = nullptr;
    plume::RenderPipeline* pipeline = nullptr;
    UploadSpan constants;
    plume::RenderViewport viewport;
    bool cabin = false;
  };
  Item items[3];
  size_t count = 0;
  bool pending = false;
  rules::Why last = rules::Why::kGeometry;
  const char* cabin_failure = nullptr;
  for (uint32_t k = 0; k < 3; ++k) {
    if ((facts.needed_mask & (1u << k)) == 0) {
      continue;
    }
    const bool cabin = k == cab::kGlassIndex;
    if (cabin) cabin_failure = "glass draw or vertex input unavailable";
    const mirror_glass::Glass& g = glass.glass[k];
    const car_frame::Draw* d = in.glass->DrawOf(k);
    if (!d || d->part != car_frame::Part::kBody || !d->viewport_known) {
      last = rules::Why::kGeometry;
      continue;
    }
    const rules::DepthTest test = rules::DepthTestOf(d->depth_word);
    if (test == rules::DepthTest::kNone) {
      if (cabin) cabin_failure = "glass depth test cannot be replayed";
      last = rules::Why::kDepthFunc;
      continue;
    }
    const DrawGeometry& geo = d->geometry;
    const sm::PositionKind kind = KindOf(d->position_format);
    if (!IsTriangles(geo.topology) || geo.count == 0 || d->position_slot >= geo.slot_count || d->position_slot >= DrawGeometry::kMaxSlots || (geo.indexed && !geo.index_view.buffer.ref) ||
        d->position_stride >= rules::kMaxStride) {
      last = rules::Why::kGeometry;
      continue;
    }
    const plume::RenderVertexBufferView& vb = geo.vertex_views[d->position_slot];
    if (!vb.buffer.ref || !sm::PositionUsable(kind, d->position_offset, d->position_stride, vb.size)) {
      last = rules::Why::kGeometry;
      continue;
    }
    bool item_pending = false;
    plume::RenderPipeline* pipeline = Pipeline(rules::Variant(sm::VariantKey(kind, uint32_t(geo.topology), d->position_stride), sample_index, test), &item_pending);
    pending |= item_pending;
    if (!pipeline) {
      if (cabin) cabin_failure = item_pending ? "overlay pipeline pending" : "overlay pipeline failed";
      last = rules::Why::kNotReady;
      continue;
    }
    rules::PushInputs push;
    push.model_to_world = d->model_to_world;
    push.view = in.frame->view;
    push.view_projection = in.frame->view_projection;
    for (int c = 0; c < 3; ++c) {
      push.ndc_scale[c] = d->viewport.ndc_scale[c];
      push.ndc_offset[c] = d->viewport.ndc_offset[c];
      push.normal_view[c] = double(g.normal_view[c]);
    }
    upscaler_math::JitterNdcOffset(in.jitter, d->viewport.viewport.width, d->viewport.viewport.height, push.jitter_ndc);
    push.rig = pose_;
    push.distance = distance_;
    push.picture_slot = picture.slot;
    OverlayDraw constants;
    const rules::PushResult made = rules::MakePush(push, &constants.push);
    if (made != rules::PushResult::kOk) {
      last = made == rules::PushResult::kProjection ? rules::Why::kProjection : made == rules::PushResult::kBadCamera ? rules::Why::kNoCamera : rules::Why::kNoPose;
      if (cabin) cabin_failure = rules::WhyName(last);
      if (made == rules::PushResult::kProjection && !projection_logged_) {
        projection_logged_ = true;
        REXLOG_WARN("{} the frame's camera has a projection that is not a perspective of the main view space (the game's clip space is not sparse there): no overlay (reported once; CHECK 'projection' of "
                    "perf/MIRROR-VIEW.md section 11)",
                    kPrefix);
      }
      continue;
    }
    const UploadSpan span = in.upload->Allocate(in.slot, sizeof(constants), 256);
    if (cabin) cabin_failure = "overlay constants unavailable";
    if (!span || !span.cpu) continue;
    if (cabin) {
      const auto mask = cabin_mask_.Record(*list, *device_, *passes_, *pass_timing_, *in.frame, *d, glass);
      cabin_failure = mask.reason;
      if (mask.slot == kInvalidBindlessSlot) continue;
      const auto& a = glass.aperture;
      const double width = a.right - a.left, height = a.top - a.bottom;
      const auto centre = glass.cabin_centre_model;
      constants.mask_u[0] = float(a.across.x / width);
      constants.mask_u[1] = float(a.across.y / width);
      constants.mask_u[2] = float(a.across.z / width);
      constants.mask_u[3] = float((-cab::Dot(a.across, centre) - a.left) / width);
      constants.mask_v[0] = float(-a.up.x / height);
      constants.mask_v[1] = float(-a.up.y / height);
      constants.mask_v[2] = float(-a.up.z / height);
      constants.mask_v[3] = float((cab::Dot(a.up, centre) + a.top) / height);
      const float night = rt_hit::NightFromSunHeight(float(in.frame->to_sun[1]));
      constants.mask_meta[0] = rules::Bits(mask.slot);
      constants.mask_meta[1] = car_rt_material::ClassAmbient(car_rt_material::Class::kInterior) * (0.065f - 0.055f * night) + 0.002f * night;
      constants.mask_meta[2] = 1.0f;
    }
    std::memcpy(span.cpu, &constants, sizeof(constants));
    items[count].constants = span;
    items[count].cabin = cabin;
    items[count].draw = d;
    items[count].pipeline = pipeline;
    items[count].viewport = d->viewport.viewport;
    ++count;
  }
  if (cabin_failure) LogCabin(cabin_key, cabin_failure);
  FlushListing();
  if (pending && !pending_logged_) {
    pending_logged_ = true;
    REXLOG_INFO("{} a pipeline for the glass's vertex format is still being made in the background: no overlay until it is (reported once)", kPrefix);
  }
  if (count == 0) {
    Count(last);
    return false;
  }

  // The pass: the main colour and the main depth bound together, the colour as it is blended over, the depth read (and never written: the pipeline).
  edram_->Transition(list, *colour, plume::RenderTextureLayout::COLOR_WRITE);
  edram_->Transition(list, *depth, plume::RenderTextureLayout::DEPTH_WRITE);
  EdramTargets::Target* only[1] = {colour};
  plume::RenderFramebuffer* framebuffer = edram_->Framebuffer(only, 1, depth);
  if (!framebuffer) {
    if (cabin_needed && cabin_pose_ && !cabin_failure) LogCabin(cabin_key, "main framebuffer unavailable");
    Count(rules::Why::kTargets);
    return true;  // the layouts were recorded: the caller's list state is gone
  }
  pass_timing_->Mark(list, "mirror overlay");
  list->setFramebuffer(framebuffer);
  const plume::RenderRect scissor(0, 0, int32_t(colour->scale.Edge(colour->width)), int32_t(colour->scale.Edge(colour->height)));
  list->setScissors(&scissor, 1);
  list->setGraphicsPipelineLayout(layout_.get());
  for (uint32_t set = 0; set < 4; ++set) {
    list->setGraphicsDescriptorSet(heap_->texture_set(), set);
  }
  list->setGraphicsDescriptorSet(heap_->sampler_set(), 4);
  for (size_t i = 0; i < count; ++i) {
    const Item& item = items[i];
    const car_frame::Draw& d = *item.draw;
    const DrawGeometry& geo = d.geometry;
    list->setViewports(&item.viewport, 1);
    list->setPipeline(item.pipeline);
    if (ActiveBackend() == Backend::kD3D12) {
      list->setGraphicsRootDescriptor(item.constants.reference(), 0);
    } else {
      list->setGraphicsPushConstants(0, &item.constants.device_address, 0, sizeof(item.constants.device_address));
    }
    // The draw's own vertex view, shifted to its POSITION element (checked usable above), at the stride its main-view draw read.
    plume::RenderVertexBufferView view = geo.vertex_views[d.position_slot];
    view.buffer.offset += d.position_offset;
    view.size -= d.position_offset;
    const plume::RenderInputSlot slot(0, d.position_stride);
    list->setVertexBuffers(0, &view, 1, &slot);
    if (geo.indexed) {
      list->setIndexBuffer(&geo.index_view);
      list->drawIndexedInstanced(geo.count, 1, geo.first, geo.base_vertex, 0);
    } else {
      list->drawInstanced(geo.count, 1, geo.first, 0);
    }
    // CHECK: confirm the depth-tested cabin patch and HUD suppression line up in cockpit cuts on both APIs.
    cabin_drawn_ |= item.cabin;
  }
  if (cabin_drawn_) LogCabin(cabin_key, "draw recorded");
  list->setFramebuffer(nullptr);
  // The closing mark (PassTiming: a mark's time runs to the next mark): what is recorded next is not the overlay's.
  pass_timing_->Mark(list, "after mirror overlay");
  Count(rules::Why::kDraw);
  drawn_glasses_ += count;
  if (!first_logged_) {
    first_logged_ = true;
    LogFirst(in, uint32_t(count));
  }
  return true;
}

void MirrorOverlay::LogFirst(const Inputs& in, uint32_t drawn) {
  const MirrorView::Picture picture = in.view->CurrentPicture();
  const mirror_glass::Result& r = in.glass->Current();
  REXLOG_INFO("{} the first glass drawn, frame {}: {} glasses (needed mask {}), the picture {}x{} drawn {} frames ago from the eye ({:.2f}, {:.2f}, {:.2f}) with a field of {:.1f} x {:.1f} degrees "
              "(across, up), the lookup's distance {:.1f} m, the overlay {:.1f} mm towards the eye; glass widths on a 1080 line picture {} and {} px (timed as 'mirror overlay')",
              kPrefix, frame_, drawn, r.need.mask, picture.width, picture.height, picture.age, pose_.eye[0], pose_.eye[1], pose_.eye[2], double(field_h_), double(field_v_), distance_,
              rules::kBiasMetres * 1000.0, r.glass[0].px_wide, r.glass[1].px_wide);
}

}  // namespace mcla::gpu
