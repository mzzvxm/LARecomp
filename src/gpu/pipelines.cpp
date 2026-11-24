// MCLA GPU renderer -- graphics pipelines of the guest draws. See pipelines.h.
//
// Canonicalization follows reblue's pipeline cache (BSD-3-Clause) in spirit;
// the polygon offset conversion is ported from Xenia's draw_util
// GetD3D10IntegerPolygonOffset (rexglue 0.10's rex/graphics/util/draw.h, private
// since 0.11; Copyright Ben
// Vanik, the Xenia authors, BSD-3-Clause).

#include "pipelines.h"
#include "settings/immortal.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>

#include "settings/larecomp_settings.h"
#include <rex/platform/file.h>
#include "xxhash_inline.h"
#include <rex/logging/log_category.h>

#include "backend.h"
#include "cache_file_check.h"
#include "car_selfrefl.h"
#include "city_refl.h"
#include "constants.h"
#include "device.h"
#include "edram.h"
#include "effect_compare.h"
#include "format.h"
#include "headlight_rig.h"
#include "layout.h"
#include "mc_engine/core/threading.h"
#include "os/file_replace.h"
#include "os/thread_priority.h"
#include "pipeline_wait.h"
#include "shadow_filter.h"
#include "side_views.h"
#include "temporal_backend.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
// True when the pipeline uses a shader named by mcla_gpu_skip_shaders.
bool SkipShaders(uint64_t vs, uint64_t ps) {
  static const std::set<uint64_t>& keys = larecomp::Leak<const std::set<uint64_t>>([] {
    std::set<uint64_t> out;
    std::stringstream list(larecomp::settings::mcla_gpu_skip_shaders.Get());
    std::string item;
    while (std::getline(list, item, ',')) {
      if (!item.empty()) {
        out.insert(std::strtoull(item.c_str(), nullptr, 16));
      }
    }
    return out;
  }());
  return !keys.empty() && (keys.count(vs) != 0 || (ps != 0 && keys.count(ps) != 0));
}
}  // namespace

namespace mcla::gpu {

namespace {

// mcla_gpu_glow_visibility as its change callback last saw it (the render thread never reads the cvar's string
// while another thread may be writing it: the debug menu, the console), logged when it changes. Anything but
// `game` is soft.
std::atomic<bool> g_glow_soft{true};
bool IsGameChoice(std::string_view text) {
  std::string lower(text);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return lower == "game";
}
bool GlowSoftSelected() {
  static std::once_flag registered;
  std::call_once(registered, [] {
    g_glow_soft.store(!IsGameChoice(larecomp::settings::mcla_gpu_glow_visibility.Get()), std::memory_order_relaxed);
    REXLOG_INFO("[gpu] glow visibility: {} (mcla_gpu_glow_visibility; live)",
                g_glow_soft.load(std::memory_order_relaxed) ? "soft" : "the game's own");
    larecomp::cvar::RegisterChangeCallback("mcla_gpu_glow_visibility", [](std::string_view, std::string_view value) {
      g_glow_soft.store(!IsGameChoice(value), std::memory_order_relaxed);
      REXLOG_INFO("[gpu] glow visibility: {} (mcla_gpu_glow_visibility; live)",
                  g_glow_soft.load(std::memory_order_relaxed) ? "soft" : "the game's own");
    });
  });
  return g_glow_soft.load(std::memory_order_relaxed);
}

// The features of the variant pack that the settings want for a draw of pixel shader `ps`, from the features the pack has for
// it (`ps.features`): a mask of FeatureBit, 0 for none (the game's own shader draws). ShaderStore::ChooseVariant then takes the
// word of the pack with the most of them (a composed shader when the pack has one for the set, else a subset: shaderpack/
// variant_chain.h), so two features that the chain composes are both on a shader that has both, and two that it does not
// (every built-in feature today) are one at a time, the lowest feature first, which is the order the settings were asked in
// before the chain. The HDR scene's tone map (kFeatureHdrScene) is not chosen here: the HDR output draws it again itself
// (hdr_tonemap.cpp). One line a feature: the bloom's Gaussians follow mcla_gpu_bloom_scale, the environment map's level pass and
// reduction (kFeatureEnvMap), which are used while the car reflections class is above the game's size (mcla_gpu_env_resolution,
// side_views.h), the sun shadow collector's lookup mcla_gpu_shadow_filter (soft or pcss, one shader whose constants tell them apart;
// auto resolved for the GPU class: shadow_filter.h), the glow sprites' soft visibility mcla_gpu_glow_visibility (`game` selects
// nothing), the car paint's self-reflection mcla_gpu_car_selfrefl (kFeatureCarSelfRefl, car_selfrefl.h), the chrome's occlusion by the
// car's own parts (kFeatureCarChrome, shaderpack/variants/car_chrome.h: wherever the paint's is on, and
// mcla_gpu_car_chrome_occlusion) and, with it, while the city is in the reflections, its city stage (kFeatureCarChromeCity: CITY-RT M2 R3a) and the auto-exposure's sample pass mcla_gpu_luminance_scale_fix (the same picture at render scale 1:
// the variant makes the game's own call there); the UI sharpening variant (kFeatureUiSharp, mcla_ui_sharp) is not chosen here: it
// needs the draw (the UI phase, its target's scale and its textures' filters), so the Scene sets it on the key after this
// (Scene::UiSharpDraw); each is read at each draw so a change of the setting shows at the next frame. At `game` the game's own shaders
// draw and the picture stays the game's (the environment map's variant is the same arithmetic at the game's size, but that is not
// asked). The cars' headlights (kFeatureHeadlight, headlight_rig.h) are wanted while the rig has a light this frame. A feature that
// composes with another (variant_chain.h kFeatures composes_with) adds its line here like the others: it says whether ITS setting
// is on, and nothing about the others.
uint32_t WantedFeatures(const ShaderStore::Entry& ps) {
  namespace sp = larecomp::shaderpack;
  uint32_t wanted = 0;
  const auto has = [&](uint32_t feature) { return (ps.features & ShaderStore::FeatureBit(feature)) != 0; };
  const auto want = [&](uint32_t feature) { wanted |= ShaderStore::FeatureBit(feature); };
  // (the compare key's flip, effect_compare.h, can force the bloom fix either way for a moment; the setting is not changed)
  if (has(sp::kFeatureBloomGauss) && compare::Global().Apply(compare::Effect::kBloomFix, larecomp::settings::mcla_gpu_bloom_scale.Get())) {
    want(sp::kFeatureBloomGauss);
  }
  if (has(sp::kFeatureEnvMap) && side_views::LevelOf(SideView::kEnv) != SideLevel::kGame) {
    want(sp::kFeatureEnvMap);
  }
  if (has(sp::kFeatureShadowFilter) && shadow_filter::VariantSelected()) {
    want(sp::kFeatureShadowFilter);
  }
  if (has(sp::kFeatureGlowSoft) && GlowSoftSelected()) {
    want(sp::kFeatureGlowSoft);
  }
  // The car paint's self-reflection (car_selfrefl.h): while mcla_gpu_car_selfrefl is on (auto picks by the class), as the
  // frame's BeginFrame decided. The paint is in this feature alone: the shadow filter's transform leaves it out (it reads no
  // shadow atlas) and the others name other shaders; the pack step warns when a shader lands in two features that the chain does
  // not compose (variant_pack.cpp), because only one of them would ever draw.
  if (has(sp::kFeatureCarSelfRefl) && CarSelfRefl::Selected()) {
    want(sp::kFeatureCarSelfRefl);
  }
  // The car's chrome (the tailpipes, the rims): the same history image and constants as the paint's, a shader of its own (the pack step
  // warns when a shader lands in two features that are not composed, and the chrome is in this one alone). The debug-only switch is its A/B.
  if (has(sp::kFeatureCarChrome) && CarSelfRefl::Selected() && larecomp::settings::mcla_gpu_car_chrome_occlusion.Get()) {
    want(sp::kFeatureCarChrome);
    // CITY-RT M2 R3a: the chrome's reflection of the city, a stage on top of the chrome's (kFeatureCarChromeCity, shaderpack/variants/car_chrome_city.h), wanted only with it and only while the city is in the
    // reflections (city_refl.h CityReflChromeWanted: a level above off and mcla_gpu_city_refl_chrome). Not wanted, the chrome's own words draw: the stage never touched them.
    if (has(sp::kFeatureCarChromeCity) && CityReflChromeWanted()) {
      want(sp::kFeatureCarChromeCity);
    }
  }
  if (has(sp::kFeatureLuminanceScale) && larecomp::settings::mcla_gpu_luminance_scale_fix.Get()) {
    want(sp::kFeatureLuminanceScale);
  }
  // The cars' headlights in the city's lit shaders (headlight_rig.h): while the rig has a light this frame (at night, with the setting on). The shader
  // takes the word of the headlight feature (or the pair with the G-buffer's, when that stage is wanted too: variant_chain.h kFeatures composes_with).
  if (has(sp::kFeatureHeadlight) && headlight_rig::VariantSelected()) {
    want(sp::kFeatureHeadlight);
  }
  return wanted;
}

// The pipeline cache file: a header, the layouts, the records.
// Host byte order; any mismatch discards the whole file.
constexpr char kCacheMagic[8] = {'M', 'C', 'L', 'A', 'P', 'S', 'O', '\0'};
constexpr uint32_t kCacheVersion = 1;
constexpr uint32_t kMaxStoredElements = 64;
constexpr uint32_t kMaxSemanticName = 64;

struct CacheHeader {
  char magic[8] = {};
  uint32_t version = 0;
  uint32_t key_size = 0;
  uint32_t backend = 0;
  uint32_t layout_count = 0;
  uint32_t record_count = 0;
  uint32_t reserved = 0;
};
static_assert(sizeof(CacheHeader) == 32);

void Put32(std::vector<uint8_t>& out, uint32_t v) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
  out.insert(out.end(), p, p + 4);
}

// Reads what the file holds, never past its end.
class Reader {
 public:
  Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
  bool Get(void* out, size_t n) {
    if (n > size_ - at_) {
      return false;
    }
    std::memcpy(out, data_ + at_, n);
    at_ += n;
    return true;
  }
  bool Get32(uint32_t* v) { return Get(v, 4); }
  size_t Remaining() const { return size_ - at_; }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t at_ = 0;
};

using plume::RenderBlend;
using plume::RenderBlendOperation;
using plume::RenderComparisonFunction;
using plume::RenderStencilOp;

enum ReportKind : uint32_t {
  kReportConstantBlend = 1u << 0,
  kReportStencilBackFace = 1u << 1,
  kReportPointFill = 1u << 2,
  kReportEdramMode = 1u << 3,
  kReportColorGap = 1u << 4,
  kReportAlphaTestFunction = 1u << 6,
  kReportAlphaTestNoVariant = 1u << 7,
};

// The key packs these enums in 4-bit fields.
static_assert(uint32_t(RenderBlend::INV_BLEND_FACTOR) < 16, "blend factors used fit 4 bits");
static_assert(uint32_t(RenderBlendOperation::MAX) < 16);
static_assert(uint32_t(RenderComparisonFunction::ALWAYS) < 16);
static_assert(uint32_t(RenderStencilOp::DECREMENT_AND_WRAP) < 16);
static_assert(uint32_t(plume::RenderCullMode::BACK) < 16);
static_assert(uint32_t(plume::RenderFillMode::WIREFRAME) < 8);

RenderBlend Factor(xe::BlendFactor f, bool alpha, bool* constant) {
  switch (f) {
    case xe::BlendFactor::kZero: return RenderBlend::ZERO;
    case xe::BlendFactor::kOne: return RenderBlend::ONE;
    case xe::BlendFactor::kSrcColor: return alpha ? RenderBlend::SRC_ALPHA : RenderBlend::SRC_COLOR;
    case xe::BlendFactor::kOneMinusSrcColor:
      return alpha ? RenderBlend::INV_SRC_ALPHA : RenderBlend::INV_SRC_COLOR;
    case xe::BlendFactor::kSrcAlpha: return RenderBlend::SRC_ALPHA;
    case xe::BlendFactor::kOneMinusSrcAlpha: return RenderBlend::INV_SRC_ALPHA;
    case xe::BlendFactor::kDstColor: return alpha ? RenderBlend::DEST_ALPHA : RenderBlend::DEST_COLOR;
    case xe::BlendFactor::kOneMinusDstColor:
      return alpha ? RenderBlend::INV_DEST_ALPHA : RenderBlend::INV_DEST_COLOR;
    case xe::BlendFactor::kDstAlpha: return RenderBlend::DEST_ALPHA;
    case xe::BlendFactor::kOneMinusDstAlpha: return RenderBlend::INV_DEST_ALPHA;
    case xe::BlendFactor::kConstantColor:
    case xe::BlendFactor::kConstantAlpha:
      *constant = true;
      return RenderBlend::BLEND_FACTOR;
    case xe::BlendFactor::kOneMinusConstantColor:
    case xe::BlendFactor::kOneMinusConstantAlpha:
      *constant = true;
      return RenderBlend::INV_BLEND_FACTOR;
    case xe::BlendFactor::kSrcAlphaSaturate: return RenderBlend::SRC_ALPHA_SAT;
  }
  return RenderBlend::ONE;
}

RenderBlendOperation Op(xe::BlendOp op) {
  switch (op) {
    case xe::BlendOp::kAdd: return RenderBlendOperation::ADD;
    case xe::BlendOp::kSubtract: return RenderBlendOperation::SUBTRACT;
    case xe::BlendOp::kMin: return RenderBlendOperation::MIN;
    case xe::BlendOp::kMax: return RenderBlendOperation::MAX;
    case xe::BlendOp::kRevSubtract: return RenderBlendOperation::REV_SUBTRACT;
  }
  return RenderBlendOperation::ADD;
}

RenderComparisonFunction Compare(xe::CompareFunction f) {
  switch (f) {
    case xe::CompareFunction::kNever: return RenderComparisonFunction::NEVER;
    case xe::CompareFunction::kLess: return RenderComparisonFunction::LESS;
    case xe::CompareFunction::kEqual: return RenderComparisonFunction::EQUAL;
    case xe::CompareFunction::kLessEqual: return RenderComparisonFunction::LESS_EQUAL;
    case xe::CompareFunction::kGreater: return RenderComparisonFunction::GREATER;
    case xe::CompareFunction::kNotEqual: return RenderComparisonFunction::NOT_EQUAL;
    case xe::CompareFunction::kGreaterEqual: return RenderComparisonFunction::GREATER_EQUAL;
    case xe::CompareFunction::kAlways: return RenderComparisonFunction::ALWAYS;
  }
  return RenderComparisonFunction::ALWAYS;
}

RenderStencilOp StencilOp(xe::StencilOp op) {
  switch (op) {
    case xe::StencilOp::kKeep: return RenderStencilOp::KEEP;
    case xe::StencilOp::kZero: return RenderStencilOp::ZERO;
    case xe::StencilOp::kReplace: return RenderStencilOp::REPLACE;
    case xe::StencilOp::kIncrementClamp: return RenderStencilOp::INCREMENT_AND_CLAMP;
    case xe::StencilOp::kDecrementClamp: return RenderStencilOp::DECREMENT_AND_CLAMP;
    case xe::StencilOp::kInvert: return RenderStencilOp::INVERT;
    case xe::StencilOp::kIncrementWrap: return RenderStencilOp::INCREMENT_AND_WRAP;
    case xe::StencilOp::kDecrementWrap: return RenderStencilOp::DECREMENT_AND_WRAP;
  }
  return RenderStencilOp::KEEP;
}

// Xenia's GetD3D10IntegerPolygonOffset: the guest offset in depth-buffer units
// to the host's integer bias -- unorm24 x (2^24 - 1); float24 x 2^24 in steps of
// 8 (the float32 ULPs of one float24 ULP), with ceil so a used offset never
// rounds to none.
int32_t IntegerPolygonOffset(xe::DepthRenderTargetFormat format, float offset) {
  const bool float24 = format == xe::DepthRenderTargetFormat::kD24FS8;
  int32_t value = int32_t(std::ceil(std::abs(offset) * (float24 ? float(1u << 24) * (1.0f / 8.0f)
                                                                : float((1u << 24) - 1))));
  if (float24) {
    value <<= 3;
  }
  return offset < 0.0f ? -value : value;
}

// PackBlend and UnpackBlend (the blend words of a key) are in pipelines.h: the UI layer split reads
// and changes them (frame_layers.h).

// The most workers the precache starts: half the usable processors (steady_workers_), at most this many; boot
// mode adds workers up to all the processors but one, at most this many too.
constexpr uint32_t kMaxPrecacheWorkers = 6;
// Boot mode (pipelines.h): the processor count below which it runs and the longest it lasts.
// The count is where boot mode stops having workers to add: it starts clamp(usable - 1, steady,
// kMaxPrecacheWorkers) of them against the steady clamp(usable / 2, 1, kMaxPrecacheWorkers), which is 2 more at
// 8 and 9 usable processors, 1 more at 10 and 11, and the same number from 12 (then only the priority and the cursor
// following the render thread differ, and those are not worth the guest's threads' time). It was 8, which left
// a PC with 8 to 11 logical processors without boot mode: the STARTER-ROUND2 boot tour at 8 had the render
// thread make 41 to 45 of the entries itself (550 to 600 ms in the title's first burst) against 17 to 24 at 4
// with boot mode, and 0 at 16.
constexpr uint32_t kBootBoostBelow = 2 * kMaxPrecacheWorkers;
// The sample count of the main view's targets at the default MSAA setting (StartSampleCounts' level
// when mcla_gpu_msaa is not set); a start at any other count has its precache queue ordered by it.
constexpr uint32_t kDefaultStartSamples = 2;
constexpr std::chrono::seconds kBootSeconds{40};
// The calling thread's index in workers_ while it is a precache worker (-1 on any other thread): the build it
// begins is stamped with it, so that a render thread that waits for the build can raise this worker.
thread_local int32_t t_worker_index = -1;

}  // namespace

size_t PipelineKeyHash::operator()(const PipelineKey& k) const {
  return size_t(XXH3_64bits(&k, sizeof(k)));
}

Pipelines::~Pipelines() {
  Shutdown();
}

void Pipelines::Initialize(Device& device, const GuestLayout& layout, ShaderStore& shaders) {
  build_key_memo_.Clear();
  pipeline_lookup_memo_.Clear();
  device_ = &device;
  layout_ = &layout;
  shaders_ = &shaders;
  dynamic_depth_bias_ = device.capabilities().dynamicDepthBias;
  // Where kD24S8 is kept as D32 float: the guest's constant depth offset counts 2^-24 steps,
  // and a float depth buffer counts steps of 2^(e-23) for depth near 2^e. The shadow atlas
  // holds depth of about 1e-4 to 4e-4 (e is -14 to -12), which asks for 2^11 to 2^13.
  constexpr uint32_t kDepth24BiasShift = 12;
  depth24_bias_shift_ = format::Depth24Unsupported() ? kDepth24BiasShift : 0u;
  REXLOG_INFO("[gpu] pipelines: depth bias {}", dynamic_depth_bias_
                                                    ? "dynamic (set per draw on the command list)"
                                                    : "static (part of each pipeline)");
}

// The sample counts a start draws the images of the screen the game multisamples with, other than
// what the records were made at: the MSAA setting's level (within what the device can do, as the
// start's EdramTargets::Initialize finds it) and 1 when an upscaler is the saved choice (the start
// gives up MSAA for one that runs, EdramTargets::DecideScene; whether this PC can run it is the
// game's probe's to say, so both are made). Records of other levels need no copy: the game
// remakes them when the player changes the level (PrebuildForLevel).
std::vector<uint32_t> Pipelines::StartSampleCounts(Device& device) {
  std::vector<uint32_t> counts;
  const EdramTargets::MsaaSetting setting = EdramTargets::ReadMsaaSetting();
  const uint32_t supported = EdramTargets::SupportedScreenSamples(device.device());
  counts.push_back(EdramTargets::LevelWithin(supported, setting.level));
  if (ReadUpscalerSettings().upscaler != Upscaler::kOff && counts.front() != 1) {
    counts.push_back(1);
  }
  return counts;
}

void Pipelines::StartPersistence(ShaderStore& shaders) {
  if (!device_ || persistence_started_) {
    return;
  }
  persistence_started_ = true;
  shaders_ = &shaders;
  // persist_ is set last: the render thread's Autosave (EndFrame) reads it first, and then finds
  // the records below in place.
  const bool persist = larecomp::settings::mcla_gpu_pipeline_cache.Get();
  if (persist) {
    LoadPersistent();
    // The list holds each multisampled pipeline at the count it was recorded with; this start
    // draws the images of the screen at its own (as the command line prebuild makes them).
    // CHECK: written without a build or a run. Expected on a first start with --msaa 4 or off, or
    // an upscaler: this line, the precache line after it with the copies counted, and far fewer
    // "made at draw time" / pending pipelines than before; the copies nobody draws are held in
    // memory like the unused part of the list (about 85 KB each on D3D12, more on Vulkan).
    const std::vector<uint32_t> samples = StartSampleCounts(*device_);
    MakeLevelCopies(samples);
    std::string text;
    for (const uint32_t count : samples) {
      text += (text.empty() ? "" : ",") + std::to_string(count);
    }
    REXLOG_INFO("[gpu] pipelines: the start draws the multisampled images at {} sample(s): {} "
                "copies of the {} listed pipelines at those counts are precached too",
                text, setup_copies_.size(), records_.size());
  }
  // The workers run without a cache file too: they make what the render
  // thread hands them (background builds).
  StartPrecache();
  persist_ = persist;
}

void Pipelines::LoadPersistent() {
  // The cache is <executable>/cache/mcla_gpu_pipelines_<api>.bin and the starter list
  // <executable>/assets/mcla_pipelines.bin, whichever folder the shader pack came from.
  const std::filesystem::path exe_folder = rex::platform::GetExecutableFolder();
  std::string backend = BackendName();
  std::transform(backend.begin(), backend.end(), backend.begin(),
                 [](unsigned char ch) { return char(std::tolower(ch)); });
  cache_path_ = exe_folder / "cache" / std::format("mcla_gpu_pipelines_{}.bin", backend);
  std::string other = BackendName(ActiveBackend() == Backend::kD3D12 ? Backend::kVulkan
                                                                     : Backend::kD3D12);
  std::transform(other.begin(), other.end(), other.begin(),
                 [](unsigned char ch) { return char(std::tolower(ch)); });
  other_cache_path_ = cache_path_.parent_path() / std::format("mcla_gpu_pipelines_{}.bin", other);
  starter_path_ = exe_folder / "assets" / "mcla_pipelines.bin";
  LoadRecords();
}

uint64_t Pipelines::HashRecords() const {
  // The persistent keys in list order, the level copies after them: the same files in the same
  // state give the same hash.
  std::vector<uint8_t> bytes;
  bytes.reserve((records_.size() + setup_copies_.size()) * sizeof(PipelineKey));
  for (const std::vector<Record>* list : {&records_, &setup_copies_}) {
    for (const Record& r : *list) {
      const uint8_t* key = reinterpret_cast<const uint8_t*>(&r.key);
      bytes.insert(bytes.end(), key, key + sizeof(PipelineKey));
    }
  }
  return uint64_t(XXH3_64bits(bytes.data(), bytes.size()));
}

void Pipelines::MakeLevelCopies(const std::vector<uint32_t>& samples) {
  // A pipeline whose key has more than one sample is one of a draw into a target the guest
  // multisamples: the main view's (2 samples) and the pause menu's and the map's (4). Those
  // targets are images of the screen and take the host's MSAA level (EdramTargets::HostSamplesAt),
  // which is what the start draws with: the copy at the level's count is the pipeline the first
  // draws ask for when it is not the count they were recorded at. (A multisampled target that is
  // not an image of the screen would get a copy nobody asks for: it costs a compile, nothing else.)
  setup_copies_.clear();
  setup_copy_source_.clear();
  start_samples_ = samples;
  std::unordered_set<PipelineKey, PipelineKeyHash> seen;
  for (size_t source = 0; source < records_.size(); ++source) {
    const Record& r = records_[source];
    if (r.key.sample_count <= 1) {
      continue;
    }
    for (const uint32_t count : samples) {
      if (count == 0 || count == r.key.sample_count) {
        continue;
      }
      Record copy = r;
      copy.key.sample_count = count;
      if (recorded_.count(copy.key) != 0 || !seen.insert(copy.key).second) {
        continue;  // a record has it, or another record's copy is the same pipeline
      }
      setup_copies_.push_back(copy);
      setup_copy_source_.push_back(uint32_t(source));
    }
  }
}

Pipelines::SetupResult Pipelines::PrecacheForSetup(ShaderStore& shaders,
                                                   const SetupOptions& options) {
  using Clock = std::chrono::steady_clock;
  SetupResult result;
  if (!device_ || persistence_started_) {
    return result;
  }
  persistence_started_ = true;
  shaders_ = &shaders;
  if (!larecomp::settings::mcla_gpu_pipeline_cache.Get()) {
    result.outcome = SetupOutcome::kDisabled;
    return result;
  }
  LoadPersistent();
  MakeLevelCopies(options.level_samples);
  result.list.records = records_.size();
  result.list.copies = setup_copies_.size();
  result.list.hash = HashRecords();
  if (records_.empty()) {
    result.outcome = SetupOutcome::kEmpty;
    return result;
  }
  if (options.wanted && !options.wanted(result.list)) {
    result.outcome = SetupOutcome::kNotWanted;
    return result;
  }

  // Every usable processor but one (the machine stays usable for whoever waits for this), at
  // normal priority: nothing else of ours runs.
  const uint32_t usable = UsableProcessors();
  result.threads = options.threads != 0
                       ? std::min(options.threads, kSetupMaxThreads)
                       : std::clamp(usable > 1 ? usable - 1 : 1u, 1u, kSetupMaxThreads);
  setup_mode_ = true;
  const Clock::time_point start = Clock::now();
  StartPrecache(result.threads);
  const uint32_t total = uint32_t(precached_.size());

  uint32_t last_done = 0;
  Clock::time_point last_change = start;
  for (;;) {
    const uint32_t done = precache_finished_.load();
    if (options.progress) {
      options.progress(done, total);
    }
    if (done >= total) {
      break;
    }
    const Clock::time_point now = Clock::now();
    if (done != last_done) {
      last_done = done;
      last_change = now;
    } else if (now - last_change > std::chrono::seconds(kSetupStallSeconds)) {
      REXLOG_ERROR("[gpu] pipelines: no pipeline finished in {} s ({} of {} done): the driver "
                   "seems stuck; the prebuild gives up",
                   kSetupStallSeconds, done, total);
      result.ready = precache_ready_.load();
      result.failed = precache_failed_.load();
      result.outcome = SetupOutcome::kStalled;
      return result;  // the workers stay: joining one that is inside the driver would hang
    }
    // The workers notify per entry; the timeout keeps the progress and the stall check going.
    std::unique_lock<std::mutex> lock(precache_mutex_);
    precache_done_.wait_for(lock, std::chrono::milliseconds(100));
  }
  StopPrecache();
  setup_mode_ = false;
  result.ready = precache_ready_.load();
  result.failed = precache_failed_.load();
  result.seconds = double(std::chrono::duration_cast<std::chrono::milliseconds>(
                              Clock::now() - start)
                              .count()) /
                   1000.0;
  // As the game does at its exit: this API's file now holds the other API's records and the
  // starter list too (nothing is written when it already did).
  SaveRecords();
  result.outcome = SetupOutcome::kBuilt;
  return result;
}

void Pipelines::Shutdown() {
  StopPrecache();
  JoinAutosave();
  if (sync_log_.builds != 0 || sync_log_.waits != 0) {
    REXLOG_INFO("{}", sync_build_log::FormatSession(sync_log_));
  }
  if (handovers_ != 0) {
    REXLOG_INFO("{}", sync_build_log::FormatHandoverSession(handovers_, made_here_too_));
  }
  handovers_ = 0;
  made_here_too_ = 0;
  sync_log_ = sync_build_log::Counters{};
  if (persist_ && shaders_) {
    SaveRecords();
  }
  build_key_memo_.Clear();
  pipeline_lookup_memo_.Clear();
  pipelines_.clear();
  level_dependent_.clear();
  level_dependent_index_.clear();
  prebuild_requests_.clear();
  prebuild_precached_.clear();
  records_.clear();
  setup_copies_.clear();
  setup_copy_source_.clear();
  recorded_.clear();
  stored_layouts_.clear();
  stored_layout_index_.clear();
  session_layouts_.clear();
  names_.clear();
  loaded_records_ = 0;
  own_file_records_ = 0;
  frame_ = 0;
  persist_ = false;
  persistence_started_ = false;
  device_ = nullptr;
  layout_ = nullptr;
  shaders_ = nullptr;
}

// ---------------------------------------------------------------------------
// persistence (M5.3a)
// ---------------------------------------------------------------------------

void Pipelines::SerializeLayout(const plume::RenderInputElement* elements, size_t element_count,
                                const plume::RenderInputSlot* slots, size_t slot_count,
                                std::vector<uint8_t>& out) {
  Put32(out, uint32_t(element_count));
  Put32(out, uint32_t(slot_count));
  for (size_t i = 0; i < element_count; ++i) {
    const plume::RenderInputElement& e = elements[i];
    const std::string_view name = e.semanticName ? e.semanticName : "";
    Put32(out, uint32_t(name.size()));
    out.insert(out.end(), name.begin(), name.end());
    Put32(out, e.semanticIndex);
    Put32(out, e.location);
    Put32(out, uint32_t(e.format));
    Put32(out, e.slotIndex);
    Put32(out, e.alignedByteOffset);
  }
  for (size_t i = 0; i < slot_count; ++i) {
    Put32(out, slots[i].index);
    Put32(out, slots[i].stride);
    Put32(out, uint32_t(slots[i].classification));
  }
}

uint32_t Pipelines::StoreLayout(const InputLayout& layout) {
  const auto known = session_layouts_.find(layout.id);
  if (known != session_layouts_.end()) {
    return known->second;
  }
  std::vector<uint8_t> bytes;
  SerializeLayout(layout.elements.data(), layout.elements.size(), layout.slots.data(),
                  layout.slots.size(), bytes);
  const uint64_t hash = XXH3_64bits(bytes.data(), bytes.size());
  uint32_t index;
  const auto stored = stored_layout_index_.find(hash);
  if (stored != stored_layout_index_.end()) {
    index = stored->second;
    std::vector<uint8_t> other;
    const StoredLayout& s = *stored_layouts_[index];
    SerializeLayout(s.elements.data(), s.elements.size(), s.slots.data(), s.slots.size(), other);
    if (other != bytes) {
      // Two layouts with one hash: neither is persisted nor precached for this one.
      REXLOG_ERROR("[gpu] pipelines: two input layouts share the hash {:016X}; pipelines of the "
                   "second are neither recorded nor taken from the precache",
                   hash);
      session_layouts_.emplace(layout.id, kNoLayout);
      return kNoLayout;
    }
  } else {
    auto s = std::make_unique<StoredLayout>();
    s->hash = hash;
    s->slots = layout.slots;
    s->elements = layout.elements;
    for (plume::RenderInputElement& e : s->elements) {
      names_.emplace_back(e.semanticName ? e.semanticName : "");
      e.semanticName = names_.back().c_str();
    }
    index = uint32_t(stored_layouts_.size());
    stored_layouts_.push_back(std::move(s));
    stored_layout_index_.emplace(hash, index);
  }
  session_layouts_.emplace(layout.id, index);
  return index;
}

PipelineKey Pipelines::PersistentKey(const PipelineKey& key, uint32_t stored) const {
  const uint64_t hash = stored_layouts_[stored]->hash;
  PipelineKey persistent = key;
  persistent.input_layout = uint32_t(hash);
  persistent.reserved = uint32_t(hash >> 32);
  return persistent;
}

void Pipelines::RecordPipeline(const PipelineKey& key, const InputLayout& layout) {
  RecordStored(key, StoreLayout(layout));
}

void Pipelines::RecordStored(const PipelineKey& key, uint32_t index) {
  if (index == kNoLayout) {
    return;
  }
  const PipelineKey persistent = PersistentKey(key, index);
  if (recorded_.insert(persistent).second) {
    records_.push_back(Record{persistent, index});
  }
}

// ---------------------------------------------------------------------------
// precache (M5.3b)
// ---------------------------------------------------------------------------

// The logical processors this process may run on: its affinity mask where
// there is one (a game limited to some cores has only those), else the system's.
uint32_t Pipelines::UsableProcessors() {
#if defined(_WIN32)
  DWORD_PTR process = 0, system = 0;
  if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process) {
    return uint32_t(__builtin_popcountll(process));
  }
#else
  // Linux: the calling thread's affinity mask (a game started with taskset, or inside a cgroup with a cpuset, has only those), as the
  // decode pool and the audio and job workers count it (mc::UsableLogicalProcessors); std::thread::hardware_concurrency counts every online one.
  if (const unsigned usable = ::mc::UsableLogicalProcessors(); usable != 0) {
    return usable;
  }
#endif
  return std::max(1u, std::thread::hardware_concurrency());
}

void Pipelines::StartPrecache(uint32_t setup_threads) {
  precached_.reserve(records_.size() + setup_copies_.size());
  const auto add = [this](const Record& r) {
    auto p = std::make_unique<Precached>();
    p->key = r.key;
    p->layout = stored_layouts_[r.layout].get();  // owned by a unique_ptr: stable
    p->index = uint32_t(precached_.size());
    precached_by_key_.emplace(r.key, p.get());
    precached_.push_back(std::move(p));
  };
  // The level copies (MakeLevelCopies) each follow the record they were made from, so the list
  // stays in the order it was first needed in (the render thread's claims follow it, boot mode).
  // A draw takes a copy like a record; no record has the key of a copy (MakeLevelCopies).
  //
  // A start that draws the images of the screen at another count than the default 2 (a saved MSAA of
  // 4 or off, an upscaler: start_samples_) never asks for the multisampled entries at the other
  // counts first: its draws ask for the copies. Left between the entries the title's first frames
  // ask for, they made the queue half as dense with them as the default start's, and the workers fell
  // behind the render thread (2026-11-19 starter verify, D3D12 on 16 threads: 46 to 85 entries the
  // render thread made itself at the start with MSAA off or DLSS saved, 0 at the default). Those
  // entries now follow all the others, in their old order; the default start keeps the order above.
  // What a draw does ask for from there is made as any list entry not reached yet: a draw that may
  // be skipped makes the worker take it first, another is made by the render thread.
  // CHECK: written without a build or a run; expected: "the render thread made N entries itself"
  // (below) drops to a handful for a first start with MSAA off, 4x or DLSS saved, and the default
  // start's precache line and time do not change.
  // The order itself is precache_queue::Order (checked alone by precache_queue_test.cpp).
  for (const Record* r : precache_queue::Order(records_, setup_copies_, setup_copy_source_,
                                               precache_queue::DeferTarget(setup_threads != 0,
                                                                           start_samples_,
                                                                           kDefaultStartSamples))) {
    add(*r);
  }
  // Half the processors, below normal priority: the guest boots meanwhile, and
  // a pipeline made here is one the render thread does not stop for later.
  // The workers run with an empty list too, and stay when it is done: they
  // make the pipelines the render thread hands them (background builds).
  // With fewer than kBootBoostBelow processors the first seconds are different
  // (boot mode, pipelines.h): up to all the processors but one, at normal priority.
  // CHECK: written without a build or a run. Expected on the boot tour at 8 logical processors (affinity): the
  // line "boot mode on 8 usable processors" with 6 threads (it said 4 threads and nothing about boot mode), the
  // "boot mode over" line with the render thread's claims (41 to 45 entries before, expected near the 4
  // processors' 17 to 24), the precache's time no longer than the 5.4 to 5.7 s it was, and no longer start of
  // the title (the guest's load time at the same affinity). If the claims stay above 30 or the guest's start
  // is slower by a second or more, put it back to 8.
  // The command line prebuild (setup_threads): no guest runs, so the workers are all there is.
  const uint32_t usable = UsableProcessors();
  steady_workers_ =
      setup_threads != 0 ? setup_threads : std::clamp(usable / 2, 1u, kMaxPrecacheWorkers);
  seed_slots_ = level_seeds::Slots(steady_workers_);
  const bool boot = setup_threads == 0 && !precached_.empty() && usable < kBootBoostBelow;
  const uint32_t count =
      boot ? std::clamp(usable > 1 ? usable - 1 : 1u, steady_workers_, kMaxPrecacheWorkers)
           : steady_workers_;
  precache_start_ = std::chrono::steady_clock::now();
  stop_precache_ = false;
  list_.Reset(precached_.size());
  precache_ready_ = 0;
  precache_failed_ = 0;
  precache_finished_ = 0;
  boot_claims_ = 0;
  boot_waits_ = 0;
  precache_claims_ = 0;
  precache_first_claim_index_ = 0;
  precache_last_claim_index_ = 0;
  precache_first_claim_us_ = -1;
  precache_last_claim_us_ = -1;
  boot_deadline_ = precache_start_ + kBootSeconds;
  boot_mode_ = boot;
  // workers_ never reallocates while workers are added (the render thread reads the first workers_started_ of
  // them to raise one that a draw waits for), and a worker's flag exists before it can be raised.
  workers_started_.store(0, std::memory_order_release);
  worker_raised_ = std::make_unique<std::atomic<bool>[]>(count);
  workers_.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    workers_.emplace_back([this, i] { PrecacheWorker(i); });
    workers_started_.store(i + 1, std::memory_order_release);
  }
  if (setup_threads != 0) {
    REXLOG_INFO("[gpu] pipelines: command line prebuild: precaching {} pipelines ({} of them copies "
                "at other sample counts) on {} threads at normal priority ({} usable processors)",
                precached_.size(), setup_copies_.size(), count, usable);
  } else if (precached_.empty()) {
    REXLOG_INFO("[gpu] pipelines: nothing to precache; {} threads for background builds", count);
  } else if (boot) {
    REXLOG_INFO("[gpu] pipelines: precaching {} pipelines ({} of them copies at other sample "
                "counts) on {} threads; boot mode on {} usable processors: normal priority until "
                "the list is done or {} s have passed, then {} threads at below normal",
                precached_.size(), setup_copies_.size(), count, usable, kBootSeconds.count(),
                steady_workers_);
  } else {
    REXLOG_INFO("[gpu] pipelines: precaching {} pipelines ({} of them copies at other sample "
                "counts) on {} threads",
                precached_.size(), setup_copies_.size(), count);
  }
}

void Pipelines::StopPrecache() {
  {
    std::lock_guard<std::mutex> lock(work_mutex_);
    stop_precache_ = true;
  }
  work_cv_.notify_all();
  workers_started_.store(0, std::memory_order_release);
  for (std::thread& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
  boot_mode_ = false;
  // The workers are gone: a request that finished is taken (and recorded, so
  // the next session has it); one still queued is dropped.
  TakeFinishedRequests();
  requests_.clear();
  urgent_.clear();
  low_.clear();
  if (seed_book_.Active()) {
    DropSeeds("the precache stopped");  // the line that says how the seeds were used
  }
  seeds_by_key_.clear();  // what DropSeeds kept: the seeds that draws took
  seed_graveyard_.clear();  // the GPU is idle
  seed_.clear();
  seed_active_ = 0;
  seed_level_ = 0;
  seed_gen_ = 0;
  prebuild_requests_.clear();
  prebuild_precached_.clear();
  prebuild_seeds_.clear();
  list_.Reset(0);
  precached_by_key_.clear();
  precached_.clear();  // the GPU is idle: unclaimed pipelines die here
  aliases_.clear();
}

void Pipelines::PrecacheWorker(uint32_t index) {
  t_worker_index = int32_t(index);
  bool priority_set = false;
  bool priority_boosted = false;
  for (;;) {
    // What the render thread handed over comes first, then the next entry of
    // the list; with neither the worker sleeps. The workers above
    // steady_workers_ are the boot mode's: they leave when it is over.
    Urgent urgent;
    size_t entry = 0;
    bool from_list = false;
    bool from_seeds = false;
    {
      std::unique_lock<std::mutex> lock(work_mutex_);
      const auto leaves = [&] { return index >= steady_workers_ && !boot_mode_.load(); };
      work_cv_.wait(lock, [&] {
        return stop_precache_.load() || leaves() || !urgent_.empty() || !low_.empty() ||
               HasListWork() || HasSeedWork();
      });
      if (stop_precache_.load()) {
        return;
      }
      if (leaves()) {
        if (!urgent_.empty() || !low_.empty() || HasListWork() || HasSeedWork()) {
          work_cv_.notify_one();  // what woke this one goes to a worker that stays
        }
        return;
      }
      if (!urgent_.empty()) {
        urgent = std::move(urgent_.front());
        urgent_.pop_front();
      } else if (!low_.empty()) {
        // The copies for a coming MSAA level: nothing a draw waits for, and not the boot list.
        urgent = std::move(low_.front());
        low_.pop_front();
      } else {
        from_list = NextListEntry(entry);
        if (!from_list && HasSeedWork()) {
          // The level seeds come last: nothing urgent, no copy for a switch, the list done (and not in
          // boot mode, and not on every worker: level_seeds::MayStart).
          urgent = std::move(seed_.front());
          seed_.pop_front();
          ++seed_active_;
          from_seeds = true;
        }
      }
    }
    // Normal priority in boot mode, below normal after: the guest's threads come first once
    // the render thread no longer waits for the list. The command line prebuild has no guest.
    // Below normal, not the background level: a draw that may not be skipped waits for the
    // build under way (Get), and on Linux the lower level could not be given back.
    // The render thread may have given this worker normal priority for a draw that waited for the
    // build it was making (WaitForBuild): that build is over, the worker takes its own level again.
    if (worker_raised_ && worker_raised_[index].exchange(false, std::memory_order_acq_rel)) {
      priority_set = false;
    }
    const bool boosted = boot_mode_.load() || setup_mode_;
    if (!priority_set || boosted != priority_boosted) {
      larecomp::os::SetCurrentThreadIntent(boosted ? larecomp::os::ThreadIntent::kNormal
                                                   : larecomp::os::ThreadIntent::kBelowNormal);
      priority_set = true;
      priority_boosted = boosted;
    }
    if (urgent.request) {
      BuildRequest(*urgent.request);
    } else if (urgent.seed) {
      BuildPrecached(*urgent.seed);
    } else if (urgent.precached) {
      BuildPrecached(*urgent.precached);
    } else if (from_list) {
      BuildPrecached(*precached_[entry]);
    }
    if (from_seeds) {
      // The slot is free again; this worker goes round and takes the next seed if there is one.
      std::lock_guard<std::mutex> lock(work_mutex_);
      --seed_active_;
    }
  }
}

bool Pipelines::HasListWork() const {
  return list_.HasWork();
}

bool Pipelines::HasSeedWork() const {
  return level_seeds::MayStart(!seed_.empty(), boot_mode_.load(), seed_active_, seed_slots_);
}

bool Pipelines::NextListEntry(size_t& index) {
  // The windows of the render thread's claims, then the cursor (precache_queue.h). One the render
  // thread made or a worker has is left alone.
  return list_.Next(index, [this](size_t i) { return precached_[i]->state.load() == kQueued; });
}

void Pipelines::NoteClaim(size_t index) {
  // Every claim, in any mode, for the line that ends the precache: which entries the render thread
  // had to make and when, to tell the title's first burst from a queue the workers fall behind in.
  const int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now() - precache_start_)
                         .count();
  if (precache_claims_.fetch_add(1, std::memory_order_relaxed) == 0) {
    precache_first_claim_index_.store(uint32_t(index), std::memory_order_relaxed);
    precache_first_claim_us_.store(us, std::memory_order_relaxed);
  }
  precache_last_claim_index_.store(uint32_t(index), std::memory_order_relaxed);
  precache_last_claim_us_.store(us, std::memory_order_relaxed);
  if (!boot_mode_.load()) {
    return;
  }
  ++boot_claims_;
  FollowRenderThread(index);
}

void Pipelines::FollowRenderThread(size_t index) {
  // The render thread had to make entry `index` because no worker had reached it. It asks in about
  // the order the list was recorded in within a block (the title's first frames ask for the next
  // entries of the block they are in), so the workers make the entries after it first, a window of
  // them. The cursor stays where it is: the render thread does not ask for the blocks in the list's
  // order (a one-sample block at 1789 before the world's entries at 1347 to 1390: 44 claims when
  // the cursor jumped to the first and parked the others, sr3v-boot-4), so what the cursor has
  // still to make is the next place it asks.
  // CHECK: written without a build or a run. Expected on the boot tour at 4 logical processors
  // (chain-boot4.ps1): the "render thread made N of" line with a handful of claims (the old cursor: 17
  // and 44 in two runs) and a precache time near 7.5 s; at 8 logical processors still 0 claims. The
  // model in precache_queue_test.cpp reproduces the old 44 with a jump; it is not a measurement.
  std::lock_guard<std::mutex> lock(work_mutex_);
  list_.Follow(index);
}

void Pipelines::EndBootMode(const char* why) {
  if (!boot_mode_.load()) {
    return;
  }
  {
    // Under the workers' mutex, or one about to wait would miss the wake-up.
    std::lock_guard<std::mutex> lock(work_mutex_);
    if (!boot_mode_.exchange(false)) {
      return;
    }
  }
  work_cv_.notify_all();
  REXLOG_INFO("[gpu] pipelines: boot mode over after {:.1f} s ({}): {} of {} entries left the "
              "queue, {} made by the render thread, {} builds it waited for",
              double(std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - precache_start_)
                         .count()) /
                  1000.0,
              why, precache_finished_.load(), precached_.size(), boot_claims_.load(),
              boot_waits_.load());
}

void Pipelines::BuildPrecached(Precached& p) {
  uint8_t expected = kQueued;
  if (!p.state.compare_exchange_strong(expected, kBuilding)) {
    return;  // the render thread claimed it, or another worker has it
  }
  p.builder.store(t_worker_index, std::memory_order_release);
  plume::RenderGraphicsPipelineDesc desc;
  ShaderStore::ShaderResult vs_result = ShaderStore::ShaderResult::kOk;
  ShaderStore::ShaderResult ps_result = ShaderStore::ShaderResult::kOk;
  std::unique_ptr<plume::RenderPipeline> pipeline;
  if (BuildDesc(p.key, shaders_->Find(p.key.vs), p.key.ps ? shaders_->Find(p.key.ps) : nullptr,
                *p.layout, desc, &vs_result, &ps_result)) {
    if (!SkipShaders(p.key.vs, p.key.ps)) {
      pipeline = device_->CreateGraphicsPipeline(desc, "precached guest pipeline");
    }
  }
  const bool made = pipeline != nullptr;
  if (setup_mode_) {
    // Nobody takes it: the driver has compiled it (and keeps the result on disk). Holding the
    // whole list of pipelines would only cost memory.
    pipeline.reset();
  }
  {
    // The render thread may take it the moment the state says so.
    std::lock_guard<std::mutex> lock(precache_mutex_);
    p.pipeline = std::move(pipeline);
    p.vs_result = vs_result;
    p.ps_result = ps_result;
    p.state = made ? kReady : kFailed;
  }
  if (p.seed) {
    // Not an entry of the list: outside its counts and its "precache done" line.
    precache_done_.notify_all();
    seed_book_.Finished(p.seed_gen, made, std::chrono::steady_clock::now());
    return;
  }
  ++(made ? precache_ready_ : precache_failed_);
  precache_done_.notify_all();
  NotePrecacheEntryDone();
}

void Pipelines::BuildRequest(Request& r) {
  uint8_t expected = kQueued;
  if (!r.state.compare_exchange_strong(expected, kBuilding)) {
    return;  // the render thread made it itself
  }
  r.builder.store(t_worker_index, std::memory_order_release);
  plume::RenderGraphicsPipelineDesc desc;
  ShaderStore::ShaderResult vs_result = ShaderStore::ShaderResult::kOk;
  ShaderStore::ShaderResult ps_result = ShaderStore::ShaderResult::kOk;
  std::unique_ptr<plume::RenderPipeline> pipeline;
  if (BuildDesc(r.key, r.vs, r.ps, *r.layout, desc, &vs_result, &ps_result)) {
    if (!SkipShaders(r.key.vs, r.key.ps)) {
      pipeline = device_->CreateGraphicsPipeline(desc, "background guest pipeline");
    }
  }
  const bool made = pipeline != nullptr;
  {
    std::lock_guard<std::mutex> lock(precache_mutex_);
    r.pipeline = std::move(pipeline);
    r.vs_result = vs_result;
    r.ps_result = ps_result;
    r.state = made ? kReady : kFailed;
  }
  precache_done_.notify_all();
}

void Pipelines::NotePrecacheEntryDone() {
  // Each entry leaves the queue once: a worker builds it, or the render thread
  // claims it. The last one to leave says so.
  if (precache_finished_.fetch_add(1) + 1 == precached_.size() && !stop_precache_.load()) {
    REXLOG_INFO("[gpu] pipelines: precache done in {:.1f} ms: {} ready, {} failed",
                double(std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now() - precache_start_)
                           .count()) /
                    1000.0,
                precache_ready_.load(), precache_failed_.load());
    // A separate line: the one above is read by the run tools (run_misses.py, the starter chains).
    if (const uint32_t claims = precache_claims_.load(std::memory_order_relaxed); claims != 0) {
      REXLOG_INFO("[gpu] pipelines: the render thread made {} of the {} entries itself (they were "
                  "not reached by a worker when a draw that may not be skipped asked): entries {} "
                  "to {} of the queue, the first {:.0f} ms and the last {:.0f} ms after the start",
                  claims, precached_.size(),
                  precache_first_claim_index_.load(std::memory_order_relaxed),
                  precache_last_claim_index_.load(std::memory_order_relaxed),
                  double(precache_first_claim_us_.load(std::memory_order_relaxed)) / 1000.0,
                  double(precache_last_claim_us_.load(std::memory_order_relaxed)) / 1000.0);
    }
    EndBootMode("the list is done");
  }
}

void Pipelines::PushUrgent(Urgent urgent) {
  {
    std::lock_guard<std::mutex> lock(work_mutex_);
    urgent_.push_back(std::move(urgent));
  }
  work_cv_.notify_one();
}

bool Pipelines::MaySkip(Miss& miss) {
  const Deferral* deferral = miss.deferral;
  const bool rule = deferral && deferral->may_skip && deferral->may_skip(deferral->context);
  // While a capture runs nothing is left out for a build under way (sync_builds.h): the caller
  // takes the way of a draw that may not be skipped, and the wait is counted.
  const bool capture = rule && sync_builds::CaptureActive();
  const sync_builds::Verdict verdict = sync_builds::Decide(capture, rule);
  miss.held |= verdict.held;
  return verdict.may_skip;
}

plume::RenderPipeline* Pipelines::TakePrecached(const PipelineKey& key, Miss& miss,
                                                const PipelineInputs& inputs) {
  if ((precached_by_key_.empty() && seeds_by_key_.empty()) || !inputs.input_layout) {
    return nullptr;
  }
  const uint32_t stored = StoreLayout(*inputs.input_layout);
  if (stored == kNoLayout) {
    return nullptr;
  }
  Precached* const listed = FindListed(PersistentKey(key, stored));
  if (!listed) {
    return nullptr;
  }
  Precached& p = *listed;
  if (p.taken) {
    // Another session key with the same persistent key went first: its
    // pipeline is this one's too.
    const auto first = pipelines_.find(p.taken_as);
    if (first == pipelines_.end() || !first->second) {
      miss.take = Take::kFailed;
      return nullptr;
    }
    aliases_.emplace(key, first->second.get());
    ++stats_.shared;
    return first->second.get();
  }
  uint8_t state = p.state.load();
  if ((state == kQueued || state == kBuilding) && MaySkip(miss)) {
    // The draw may wait for it: a worker makes it, and this one before the
    // rest of the list.
    if (!p.asked) {
      p.asked = true;
      p.asked_frame = frame_;
      ++stats_.pending_keys;
      if (state == kQueued) {
        Urgent urgent;
        if (p.seed) {
          urgent.seed = p.shared_from_this();  // it is still in seed_ too: the worker that finds it begun leaves it
        } else {
          urgent.precached = &p;
        }
        PushUrgent(std::move(urgent));
      }
    }
    ++stats_.pending_draws;
    miss.take = Take::kPending;
    return nullptr;
  }
  state = kQueued;
  if (p.state.compare_exchange_strong(state, kClaimed)) {
    miss.take = Take::kMakeHere;
    if (p.seed) {
      seed_book_.Claimed(p.seed_gen);  // not an entry of the start's queue: no claim of it, no count
    } else {
      NoteClaim(p.index);
      NotePrecacheEntryDone();
    }
    ++stats_.precache_claimed;  // no worker at it yet: the caller makes it now
    p.taken = true;
    p.taken_as = key;
    return nullptr;
  }
  if (state == kBuilding) {
    const auto t0 = std::chrono::steady_clock::now();
    // A level seed's worker is not stolen from (its books count the seed made once); the rest may be.
    bool raised = false;
    const bool finished = WaitForBuild(p.state, p.builder, !p.seed, &raised);
    if (boot_mode_.load()) {
      ++boot_waits_;
    }
    ++stats_.precache_waits;
    const uint64_t waited_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::steady_clock::now() - t0)
                                            .count());
    stats_.precache_wait_ns += waited_ns;
    LogSyncWait(key, "list", waited_ns);
    NoteHandover(key, "list", raised, !finished, waited_ns);
    if (!finished) {
      // The worker is still at it (starved, or a slow driver): this thread makes the pipeline too, as if
      // no worker had begun it. The worker's result stays in the entry, unused, until the list is dropped.
      miss.take = Take::kMakeHere;
      p.taken = true;
      p.taken_as = key;
      return nullptr;
    }
  }
  std::lock_guard<std::mutex> lock(precache_mutex_);
  if (p.state.load() != kReady) {
    if (miss.deferral && miss.deferral->has_fallback) {
      // Failed on a worker, and the draw has another pipeline to draw with: final for the key. Not made again here (the render thread would stall for a pipeline that
      // fails the same), reported once like any failed build, and remembered as failed, as a build that failed on this thread would be.
      LogFailure(key, p.vs_result, p.ps_result, true);
      pipelines_.emplace(key, nullptr);
      miss.take = Take::kReported;
      return nullptr;
    }
    miss.take = Take::kFailed;
    return nullptr;  // failed: made (and reported) on the caller's path
  }
  p.state = kClaimed;
  p.taken = true;
  p.taken_as = key;
  if (p.asked) {
    NotePendingTaken(p.asked_frame);
  }
  plume::RenderPipeline* raw = p.pipeline.get();
  pipelines_.emplace(key, std::move(p.pipeline));
  ++stats_.precache_taken;
  if (p.seed) {
    seed_book_.Taken(p.seed_gen);
  }
  if (persist_) {
    // A record is known already (nothing happens); a level copy is the pipeline of a draw now.
    // CHECK: that the cache file then holds the copies the player drew with (the log's "records
    // written" count) and the next start precaches them as records.
    RecordStored(key, stored);
  }
  return raw;
}

bool Pipelines::StartRequest(const PipelineKey& key, const PipelineInputs& inputs, bool fallback) {
  if (workers_.empty() || stop_precache_.load() || !inputs.vs || !inputs.input_layout ||
      (key.ps != 0 && !inputs.ps)) {
    return false;
  }
  // The worker reads the layout and the shaders, so it gets the stored copy
  // (the session's layout may change under it) and the pack's entries.
  const uint32_t stored = StoreLayout(*inputs.input_layout);
  if (stored == kNoLayout) {
    return false;
  }
  auto request = std::make_shared<Request>();
  request->key = key;
  request->vs = inputs.vs;
  request->ps = key.ps != 0 ? inputs.ps : nullptr;
  request->layout = stored_layouts_[stored].get();
  request->stored = stored;
  request->asked_frame = frame_;
  request->fallback = fallback;
  requests_.emplace(key, request);
  Urgent urgent;
  urgent.request = std::move(request);
  PushUrgent(std::move(urgent));
  ++stats_.pending_keys;
  ++stats_.pending_draws;
  return true;
}

plume::RenderPipeline* Pipelines::TakeRequested(const PipelineKey& key, Miss& miss) {
  const auto found = requests_.find(key);
  if (found == requests_.end()) {
    return nullptr;
  }
  Request& r = *found->second;
  if (miss.deferral && miss.deferral->has_fallback) {
    r.fallback = true;  // a copy made for an MSAA level or another asker: the draw that takes it has a fallback key, so a failure is final and logged for the key
  }
  uint8_t state = r.state.load();
  if (state == kQueued || state == kBuilding) {
    if (MaySkip(miss)) {
      if (r.low && !r.promoted) {
        // A copy for the coming MSAA level that a draw already needs: a thread takes it next.
        r.promoted = true;
        ++stats_.pending_keys;
        if (state == kQueued) {
          Urgent urgent;
          urgent.request = found->second;
          PushUrgent(std::move(urgent));
        }
      }
      ++stats_.pending_draws;
      miss.take = Take::kPending;
      return nullptr;
    }
    state = kQueued;
    if (r.state.compare_exchange_strong(state, kClaimed)) {
      // No worker at it yet: made here now, and the worker that finds it in
      // the queue finds it claimed.
      ++stats_.made_here_new;
      requests_.erase(found);
      miss.take = Take::kMakeHere;
      return nullptr;
    }
    if (state == kBuilding) {
      const auto t0 = std::chrono::steady_clock::now();
      bool raised = false;
      const bool finished = WaitForBuild(r.state, r.builder, true, &raised);
      ++stats_.request_waits;
      const uint64_t waited_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now() - t0)
                                              .count());
      stats_.request_wait_ns += waited_ns;
      LogSyncWait(key, "request", waited_ns);
      NoteHandover(key, "request", raised, !finished, waited_ns);
      if (!finished) {
        // Still being made by a starved worker or a slow driver: made here too (TakePrecached's way). The
        // worker's request is let go with its own reference.
        ++stats_.made_here_new;
        requests_.erase(found);
        miss.take = Take::kMakeHere;
        return nullptr;
      }
    }
  }
  plume::RenderPipeline* raw = AdoptRequest(key, r);
  requests_.erase(found);
  if (!raw) {
    miss.take = Take::kReported;
  }
  return raw;
}

plume::RenderPipeline* Pipelines::AdoptRequest(const PipelineKey& key, Request& r) {
  std::unique_ptr<plume::RenderPipeline> pipeline;
  ShaderStore::ShaderResult vs_result;
  ShaderStore::ShaderResult ps_result;
  {
    std::lock_guard<std::mutex> lock(precache_mutex_);
    pipeline = std::move(r.pipeline);
    vs_result = r.vs_result;
    ps_result = r.ps_result;
  }
  if (pipeline) {
    ++stats_.background_taken;
    if (!r.low || r.promoted) {
      NotePendingTaken(r.asked_frame);  // a draw was waiting for it
    }
    if (persist_) {
      RecordStored(key, r.stored);
    }
  } else {
    LogFailure(key, vs_result, ps_result, r.fallback);
  }
  plume::RenderPipeline* raw = pipeline.get();
  pipelines_.emplace(key, std::move(pipeline));
  return raw;
}

void Pipelines::TakeFinishedRequests() {
  for (auto it = requests_.begin(); it != requests_.end();) {
    const uint8_t state = it->second->state.load();
    if (it->second->low && !it->second->promoted) {
      // A copy for a coming MSAA level that no draw has asked for: it is taken (and recorded for
      // the next start) by the first draw that does, so a level the player went back from leaves
      // nothing in the cache file.
      ++it;
      continue;
    }
    if (state == kReady || state == kFailed) {
      AdoptRequest(it->first, *it->second);
      it = requests_.erase(it);
    } else {
      ++it;
    }
  }
}

void Pipelines::NotePendingTaken(uint64_t asked_frame) {
  const uint64_t frames = frame_ - asked_frame;
  ++stats_.pending_taken;
  stats_.pending_frames_sum += frames;
  stats_.pending_frames_max = std::max(stats_.pending_frames_max, frames);
}

void Pipelines::EndFrame() {
  ++frame_;
  sync_builds::EndFrame();
  if (boot_mode_.load(std::memory_order_relaxed) &&
      std::chrono::steady_clock::now() >= boot_deadline_) {
    EndBootMode("time limit");
  }
  if (!requests_.empty()) {
    TakeFinishedRequests();
  }
  if (seed_level_ != 0) {
    SeedFrame();
  }
  for (uint32_t i = 0; i < level_seeds::kReleasePerFrame && !seed_graveyard_.empty(); ++i) {
    seed_graveyard_.pop_back();
  }
  Autosave();
}

void Pipelines::LoadRecords() {
  // This API's file, then the other API's: a key names guest
  // state, shaders of the pack, Plume formats and a layout by its content's
  // hash -- nothing of the API -- so what one API saw precaches the other.
  // The one exception is a static depth bias (D3D12 without dynamic bias),
  // part of the key: a device with a dynamic bias zeroes it in every file,
  // its own too -- a D3D12 file from a build with the static bias holds keys
  // no draw asks for once the bias is dynamic.
  const Backend other = ActiveBackend() == Backend::kD3D12 ? Backend::kVulkan : Backend::kD3D12;
  own_file_records_ = LoadFile(cache_path_, ActiveBackend(), dynamic_depth_bias_);
  saved_records_ = own_file_records_;
  LoadFile(other_cache_path_, other, dynamic_depth_bias_);
  // Then the starter list that ships beside the pack (tools/pipelines): what
  // play usually needs, so a first start makes it here rather than at draw time.
  // It is a D3D12 list whose keys keep their static depth bias.
  LoadFile(starter_path_, Backend::kD3D12, dynamic_depth_bias_);
  loaded_records_ = records_.size();
  last_save_ = std::chrono::steady_clock::now();
}

size_t Pipelines::LoadFile(const std::filesystem::path& path, Backend backend, bool zero_bias) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    REXLOG_INFO("[gpu] pipelines: no cache at {} yet", path.string());
    return 0;
  }
  const std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
  Reader in(data.data(), data.size());
  CacheHeader header;
  const auto discard = [&](const char* why) -> size_t {
    REXLOG_ERROR("[gpu] pipelines: the cache {} is discarded: {}", path.string(),
                 why);
    return 0;
  };
  if (!in.Get(&header, sizeof(header)) ||
      std::memcmp(header.magic, kCacheMagic, sizeof(kCacheMagic)) != 0) {
    return discard("not a pipeline cache");
  }
  if (header.version != kCacheVersion || header.key_size != sizeof(PipelineKey) ||
      header.backend != uint32_t(backend)) {
    return discard("written by another version or backend");
  }
  // Everything is read before anything is kept: a fault anywhere adds nothing.
  // (Semantic names go to names_ as they are read; a discarded file leaves a
  // few unused strings there.)
  std::vector<std::unique_ptr<StoredLayout>> layouts;
  for (uint32_t i = 0; i < header.layout_count; ++i) {
    uint64_t hash = 0;
    uint32_t element_count = 0;
    uint32_t slot_count = 0;
    if (!in.Get(&hash, 8) || !in.Get32(&element_count) || !in.Get32(&slot_count) ||
        element_count > kMaxStoredElements || slot_count > kMaxStoredElements) {
      return discard("a layout is cut short or too large");
    }
    auto s = std::make_unique<StoredLayout>();
    s->hash = hash;
    s->elements.resize(element_count);
    s->slots.resize(slot_count);
    for (plume::RenderInputElement& e : s->elements) {
      uint32_t name_size = 0;
      char name[kMaxSemanticName] = {};
      uint32_t format = 0;
      if (!in.Get32(&name_size) || name_size >= kMaxSemanticName || !in.Get(name, name_size) ||
          !in.Get32(&e.semanticIndex) || !in.Get32(&e.location) || !in.Get32(&format) ||
          !in.Get32(&e.slotIndex) || !in.Get32(&e.alignedByteOffset)) {
        return discard("an input element is cut short");
      }
      names_.emplace_back(name, name_size);
      e.semanticName = names_.back().c_str();
      e.format = plume::RenderFormat(format);
    }
    for (plume::RenderInputSlot& slot : s->slots) {
      uint32_t classification = 0;
      if (!in.Get32(&slot.index) || !in.Get32(&slot.stride) || !in.Get32(&classification)) {
        return discard("an input slot is cut short");
      }
      slot.classification = plume::RenderInputSlotClassification(classification);
    }
    // The hash is the content's: a layout whose bytes do not give it is not
    // the one the records were made with.
    std::vector<uint8_t> bytes;
    SerializeLayout(s->elements.data(), s->elements.size(), s->slots.data(), s->slots.size(),
                    bytes);
    if (XXH3_64bits(bytes.data(), bytes.size()) != hash) {
      return discard("a layout does not match its hash");
    }
    layouts.push_back(std::move(s));
  }
  // The count is checked against what is left of the file before a vector of that size is made: a damaged header must not ask for billions of records (cache_file_check.h).
  if (!cache_file::RecordsFit(header.record_count, sizeof(PipelineKey) + sizeof(uint32_t), in.Remaining())) {
    return discard("the record count does not fit the file");
  }
  std::vector<Record> records(header.record_count);
  for (Record& r : records) {
    if (!in.Get(&r.key, sizeof(r.key)) || !in.Get32(&r.layout) || r.layout >= layouts.size()) {
      return discard("a record is cut short or names no layout");
    }
  }
  // Kept: the layouts not known yet, then the records not known yet.
  std::vector<uint32_t> layout_of_file;  // file layout index -> stored_layouts_
  for (std::unique_ptr<StoredLayout>& s : layouts) {
    const auto known = stored_layout_index_.find(s->hash);
    if (known != stored_layout_index_.end()) {
      layout_of_file.push_back(known->second);
      continue;
    }
    layout_of_file.push_back(uint32_t(stored_layouts_.size()));
    stored_layout_index_.emplace(s->hash, uint32_t(stored_layouts_.size()));
    stored_layouts_.push_back(std::move(s));
  }
  size_t added = 0;
  uint32_t dropped = 0;
  uint32_t zeroed = 0;
  for (Record r : records) {
    r.layout = layout_of_file[r.layout];
    // A shader the pack no longer has: the pipeline cannot be made. Nor can one with a variant word of the
    // variant pack (the HDR scene's tone map, the bloom's Gaussians, a composed shader: ps_variant or vs_variant 2 and up)
    // while that pack is not loaded or lacks that word of that shader. A record that an older build wrote has the
    // ids of version 2 of the pack in those fields (2..13, one per change); as words they name features the
    // shader does not have (except the HDR tone map's 2, which is the same), so they are dropped here, not misread.
    const ShaderStore::Entry* record_vs = shaders_->Find(r.key.vs);
    const ShaderStore::Entry* record_ps = r.key.ps != 0 ? shaders_->Find(r.key.ps) : nullptr;
    if (!record_vs || (r.key.ps != 0 && !record_ps) ||
        (r.key.ps_variant >= ShaderStore::kFirstPackedVariant && (!record_ps || !record_ps->HasWord(r.key.ps_variant))) ||
        (r.key.vs_variant >= ShaderStore::kFirstPackedVariant && !record_vs->HasWord(r.key.vs_variant))) {
      ++dropped;
      continue;
    }
    if (zero_bias) {
      if (r.key.depth_bias != 0 || r.key.slope_scaled_depth_bias != 0.0f) {
        ++zeroed;
      }
      r.key.depth_bias = 0;
      r.key.slope_scaled_depth_bias = 0.0f;
    }
    // The bytes of a record are its identity: a -0.0 slope written by an earlier build is the +0.0 a draw asks for now.
    r.key.slope_scaled_depth_bias = key_guard::CanonicalZero(r.key.slope_scaled_depth_bias);
    // A depth format this device has no target for (the other API's file holds D24).
    r.key.depth_format =
        uint32_t(format::HostDepthFormat(plume::RenderFormat(r.key.depth_format)));
    if (recorded_.insert(r.key).second) {
      records_.push_back(r);
      ++added;
    }
  }
  REXLOG_INFO("[gpu] pipelines: {} records and {} layouts in {}: {} new here ({} dropped: a "
              "shader the pack does not have{})",
              header.record_count, header.layout_count, path.string(), added, dropped,
              zeroed ? std::format("; {} static depth biases zeroed", zeroed) : std::string());
  return added;
}

std::vector<uint8_t> Pipelines::SerializeRecords() const {
  CacheHeader header;
  std::memcpy(header.magic, kCacheMagic, sizeof(kCacheMagic));
  header.version = kCacheVersion;
  header.key_size = sizeof(PipelineKey);
  header.backend = uint32_t(ActiveBackend());
  header.layout_count = uint32_t(stored_layouts_.size());
  header.record_count = uint32_t(records_.size());
  std::vector<uint8_t> out(sizeof(header));
  std::memcpy(out.data(), &header, sizeof(header));
  for (const auto& s : stored_layouts_) {
    const uint8_t* hash = reinterpret_cast<const uint8_t*>(&s->hash);
    out.insert(out.end(), hash, hash + 8);
    SerializeLayout(s->elements.data(), s->elements.size(), s->slots.data(), s->slots.size(),
                    out);
  }
  for (const Record& r : records_) {
    const uint8_t* key = reinterpret_cast<const uint8_t*>(&r.key);
    out.insert(out.end(), key, key + sizeof(r.key));
    Put32(out, r.layout);
  }
  return out;
}

void Pipelines::WriteCacheFile(const std::vector<uint8_t>& bytes, size_t records,
                               size_t layouts) const {
  // A temporary name of its own and one step over the old file (os/file_replace.h): the games of a test batch share this folder, and the autosave thread and the
  // end-of-session save of one game must not share a temporary file either. The other game's file in place is replaced by this one's (its records were loaded
  // into this session at the start, so nothing but what the other game found since is left out; the next start has it again).
  std::string why;
  if (!larecomp::os::Succeeded(larecomp::os::WriteFileReplacing(cache_path_, bytes.data(), bytes.size(), &why))) {
    REXLOG_ERROR("[gpu] pipelines: the cache {} could not be written: {}", cache_path_.string(), why);
    return;
  }
  REXLOG_INFO("[gpu] pipelines: {} records ({} new this session, {} from the other API's file) "
              "and {} layouts written to {}",
              records, records - loaded_records_, loaded_records_ - own_file_records_, layouts,
              cache_path_.string());
}

void Pipelines::SaveRecords() {
  // Written even when an autosave holds everything already: one that failed
  // would otherwise lose the session's records.
  if (records_.size() == own_file_records_) {
    return;  // nothing beyond what this API's file already holds
  }
  WriteCacheFile(SerializeRecords(), records_.size(), stored_layouts_.size());
}

void Pipelines::JoinAutosave() {
  if (autosave_thread_.joinable()) {
    autosave_thread_.join();
  }
}

void Pipelines::Autosave() {
  if (!persist_ || records_.size() == saved_records_ ||
      autosave_running_.load(std::memory_order_acquire)) {
    return;
  }
  constexpr auto kInterval = std::chrono::seconds(30);
  const auto now = std::chrono::steady_clock::now();
  if (now - last_save_ < kInterval) {
    return;
  }
  last_save_ = now;
  saved_records_ = records_.size();
  JoinAutosave();  // finished: only the handle is left
  autosave_running_.store(true, std::memory_order_release);
  autosave_thread_ = std::thread(
      [this, bytes = SerializeRecords(), records = records_.size(),
       layouts = stored_layouts_.size()] {
        WriteCacheFile(bytes, records, layouts);
        autosave_running_.store(false, std::memory_order_release);
      });
}

void Pipelines::Report(uint32_t kind, const char* what) {
  if (reported_kinds_ & kind) {
    return;
  }
  reported_kinds_ |= kind;
  REXLOG_ERROR("[gpu] pipelines: {} (reported once)", what);
}

KeyResult Pipelines::BuildKey(const XenosState& state, const PipelineInputs& inputs,
                              plume::RenderPrimitiveTopology topology, bool polygonal,
                              const plume::RenderFormat (&color_formats)[kColorTargetCount],
                              plume::RenderFormat depth_format, PipelineKey& key,
                              DepthBias& bias) {
  if (!draw_memo_enabled_) {
    return BuildKeyUncached(state, inputs, topology, polygonal, color_formats, depth_format,
                            key, bias, std::nullopt);
  }
  const ShaderStore::Entry* ps = inputs.ps;
  const uint32_t wanted = ps && ps->features != 0 && state.mode.edram_mode == xe::EDRAMMode::kColorDepth
                              ? WantedFeatures(*ps) : 0;
  const auto signature = PipelineBuildSignature::From(state, inputs, topology, polygonal,
                                                      color_formats, depth_format, wanted);
  ++draw_memo_profile_.build_key.tries;
  const BuiltKey* found = build_key_memo_.Find(signature);
  if (!found) {
    found = build_key_memo_.FindOther(signature);
  }
  if (found) {
    ++draw_memo_profile_.build_key.hits;
    key = found->key;
    bias = found->bias;
    return KeyResult::kOk;
  }
  const KeyResult result = BuildKeyUncached(state, inputs, topology, polygonal, color_formats,
                                            depth_format, key, bias, wanted);
  if (result == KeyResult::kOk) {
    build_key_memo_.Put(signature, BuiltKey{key, bias});
  }
  return result;
}

KeyResult Pipelines::BuildKeyUncached(const XenosState& state, const PipelineInputs& inputs,
                                      plume::RenderPrimitiveTopology topology, bool polygonal,
                                      const plume::RenderFormat (&color_formats)[kColorTargetCount],
                                      plume::RenderFormat depth_format, PipelineKey& key,
                                      DepthBias& bias, std::optional<uint32_t> wanted_features) {
  key = PipelineKey{};
  bias = DepthBias{};
  const ShaderStore::Entry* vs = inputs.vs;
  const ShaderStore::Entry* ps = inputs.ps;
  const xe::EDRAMMode mode = state.mode.edram_mode;
  if (mode != xe::EDRAMMode::kColorDepth && mode != xe::EDRAMMode::kDepthOnly) {
    Report(kReportEdramMode, "a draw with an EDRAM mode other than colour+depth or depth only");
    return KeyResult::kEdramMode;
  }
  const bool depth_only = mode == xe::EDRAMMode::kDepthOnly;
  // Xenia's reading of kDepthOnly: the pixel shader is not run. In MCLA every
  // such draw has no pixel shader bound anyway (frame census 10/10).
  if (depth_only) {
    ps = nullptr;
  }

  key.vs = vs->key;
  key.ps = ps ? ps->key : 0;
  key.vs_variant = (vs->spec_mask != 0 && inputs.input_layout->packed_normal) ? 1 : 0;
  if (ps && (ps->ps_outputs & ShaderStore::kOutputColor0) != 0) {
    const AlphaTest alpha_test = ComputeAlphaTest(state);
    if (alpha_test.unsupported) {
      Report(kReportAlphaTestFunction,
             "an alpha test function that is not a lower bound (LESS, EQUAL, LESS_EQUAL, or "
             "NOT_EQUAL with a reference other than 0); nothing is discarded");
    }
    // Variant 1 (ALPHA_TEST) is the one whose coverage depends on alpha: the
    // alpha test, alpha to coverage, or both (Xenia's DoesCoverageDependOnAlpha;
    // contract v6). Variant 0 has no discard and no SV_Coverage of its own, so
    // the host keeps its early depth/stencil test.
    if (alpha_test.discard || state.color_control.alpha_to_mask_enable) {
      if (ps->spec_mask != 0) {
        key.ps_variant = 1;
      } else {
        Report(kReportAlphaTestNoVariant,
               "alpha test or alpha to coverage on a pixel shader compiled without the ALPHA_TEST "
               "variant; nothing is discarded and every sample is covered");
      }
    }
  }
  // A variant of the pixel shader from the variant pack, where the settings ask for one: the word with the most of the wanted
  // features that the pack has for it (a composed shader, or a subset of the features), and the vertex shader's word when that one
  // reads a vertex stage's export. A draw that needs the alpha test (spec variant 1) takes a variant that has the twin, else it keeps
  // the main pack's shader.
  if (ps && ps->features != 0 && shaders_) {
    if (const uint32_t wanted = wanted_features ? *wanted_features : WantedFeatures(*ps)) {
      uint32_t vs_word = key.vs_variant;
      key.ps_variant = shaders_->ChooseVariant(*ps, wanted, key.ps_variant, vs, key.vs_variant, &vs_word);
      key.vs_variant = vs_word;
    }
  }
  key.input_layout = inputs.input_layout->id;
  key.topology = uint32_t(topology);
  key.sample_count = MsaaSampleCount(state.msaa);

  // Colour targets: contiguous from 0, the framebuffer binds them in order.
  const uint32_t ps_outputs = ps ? ps->ps_outputs : 0;
  uint32_t color_count = 0;
  if (!depth_only) {
    for (uint32_t i = 0; i < kColorTargetCount; ++i) {
      if (state.color[i].bound) {
        color_count = i + 1;
      }
    }
    for (uint32_t i = 0; i < color_count; ++i) {
      if (!state.color[i].bound) {
        Report(kReportColorGap, "colour targets with a gap below a bound one");
        return KeyResult::kColorGap;
      }
    }
  }
  key.color_count = color_count;
  bool constant_blend = false;
  for (uint32_t i = 0; i < color_count; ++i) {
    const ColorTargetState& t = state.color[i];
    key.color_format[i] = uint32_t(color_formats[i]);
    plume::RenderBlendDesc b = plume::RenderBlendDesc::Copy();
    const bool written = (ps_outputs & (1u << i)) != 0;
    b.renderTargetWriteMask = written ? uint8_t(t.write_mask) : 0;
    if (b.renderTargetWriteMask != 0) {
      const xreg::RB_BLENDCONTROL& bc = t.blend;
      b.blendOp = Op(bc.color_comb_fcn);
      b.blendOpAlpha = Op(bc.alpha_comb_fcn);
      const bool minmax_color =
          b.blendOp == RenderBlendOperation::MIN || b.blendOp == RenderBlendOperation::MAX;
      const bool minmax_alpha =
          b.blendOpAlpha == RenderBlendOperation::MIN || b.blendOpAlpha == RenderBlendOperation::MAX;
      b.srcBlend = minmax_color ? RenderBlend::ONE : Factor(bc.color_srcblend, false, &constant_blend);
      b.dstBlend = minmax_color ? RenderBlend::ONE : Factor(bc.color_destblend, false, &constant_blend);
      b.srcBlendAlpha =
          minmax_alpha ? RenderBlend::ONE : Factor(bc.alpha_srcblend, true, &constant_blend);
      b.dstBlendAlpha =
          minmax_alpha ? RenderBlend::ONE : Factor(bc.alpha_destblend, true, &constant_blend);
      const bool copy = b.srcBlend == RenderBlend::ONE && b.dstBlend == RenderBlend::ZERO &&
                        b.blendOp == RenderBlendOperation::ADD &&
                        b.srcBlendAlpha == RenderBlend::ONE &&
                        b.dstBlendAlpha == RenderBlend::ZERO &&
                        b.blendOpAlpha == RenderBlendOperation::ADD;
      b.blendEnabled = !copy;
    }
    key.blend[i] = PackBlend(b);
  }
  if (constant_blend) {
    Report(kReportConstantBlend,
           "a blend with the constant colour/alpha factor (Plume has no dynamic blend factor; the "
           "host default factor is used)");
  }

  if (state.depth_bound) {
    key.depth_format = uint32_t(depth_format);
    const xreg::RB_DEPTHCONTROL& dc = state.depth_control;
    // Z_ENABLE off turns off the write too (D3D9 semantics, as Xenia reads it).
    if (dc.z_enable) {
      key.depth = 1u | (uint32_t(dc.z_write_enable) << 1) | (uint32_t(Compare(dc.zfunc)) << 2);
    }
    if (dc.stencil_enable) {
      const xe::CompareFunction func_bf = dc.backface_enable ? dc.stencilfunc_bf : dc.stencilfunc;
      const xe::StencilOp fail_bf = dc.backface_enable ? dc.stencilfail_bf : dc.stencilfail;
      const xe::StencilOp zpass_bf = dc.backface_enable ? dc.stencilzpass_bf : dc.stencilzpass;
      const xe::StencilOp zfail_bf = dc.backface_enable ? dc.stencilzfail_bf : dc.stencilzfail;
      key.stencil = 1u | (uint32_t(Compare(dc.stencilfunc)) << 1) |
                    (uint32_t(StencilOp(dc.stencilfail)) << 5) |
                    (uint32_t(StencilOp(dc.stencilzpass)) << 9) |
                    (uint32_t(StencilOp(dc.stencilzfail)) << 13) |
                    (uint32_t(Compare(func_bf)) << 17) | (uint32_t(StencilOp(fail_bf)) << 21) |
                    (uint32_t(StencilOp(zpass_bf)) << 25);
      key.stencil_masks = uint32_t(state.stencil.stencilmask) |
                          (uint32_t(state.stencil.stencilwritemask) << 8) |
                          (uint32_t(state.stencil.stencilref) << 16) |
                          (uint32_t(StencilOp(zfail_bf)) << 24);
      if (dc.backface_enable && (state.stencil_bf.stencilmask != state.stencil.stencilmask ||
                                 state.stencil_bf.stencilwritemask !=
                                     state.stencil.stencilwritemask ||
                                 state.stencil_bf.stencilref != state.stencil.stencilref)) {
        Report(kReportStencilBackFace,
               "back-face stencil masks or reference different from the front (one set in a "
               "pipeline; the front ones are used)");
      }
    }
  }

  // Raster state. Only polygonal primitives are culled, filled by mode and
  // offset by face (Xenia): a rectangle list is drawn as triangles on the host
  // but is none of these.
  const xreg::PA_SU_SC_MODE_CNTL& sc = state.sc_mode;
  plume::RenderCullMode cull = plume::RenderCullMode::NONE;
  if (polygonal) {
    if (sc.cull_front && sc.cull_back) {
      return KeyResult::kNoEffect;
    }
    cull = sc.cull_front ? plume::RenderCullMode::FRONT
                         : sc.cull_back ? plume::RenderCullMode::BACK : plume::RenderCullMode::NONE;
  }
  // `face`: 0 = front is counter-clockwise, 1 = clockwise.
  const bool front_ccw = sc.face == 0;
  plume::RenderFillMode fill = plume::RenderFillMode::SOLID;
  if (polygonal && sc.poly_mode == xe::PolygonModeEnable::kDualMode) {
    const xe::PolygonType type =
        sc.cull_front ? sc.polymode_back_ptype : sc.polymode_front_ptype;
    if (type == xe::PolygonType::kLines) {
      fill = plume::RenderFillMode::WIREFRAME;
    } else if (type == xe::PolygonType::kPoints) {
      Report(kReportPointFill, "point fill mode (the host has solid and wireframe only)");
    }
  }
  // Polygon offset for the face that is drawn (Xenia GetPreferredFacePolygonOffset).
  float poly_scale = 0.0f;
  float poly_offset = 0.0f;
  if (state.depth_bound) {
    if (polygonal) {
      if (!sc.cull_front && sc.poly_offset_front_enable) {
        poly_scale = state.poly_offset[0];
        poly_offset = state.poly_offset[1];
      } else if (!sc.cull_back && sc.poly_offset_back_enable) {
        poly_scale = state.poly_offset[2];
        poly_offset = state.poly_offset[3];
      }
    } else if (sc.poly_offset_para_enable) {
      poly_scale = state.poly_offset[0];
      poly_offset = state.poly_offset[1];
    }
  }
  if (poly_scale != 0.0f || poly_offset != 0.0f) {
    bias.constant = IntegerPolygonOffset(state.depth_format, poly_offset);
    if (depth24_bias_shift_ != 0 && state.depth_format == xe::DepthRenderTargetFormat::kD24S8) {
      bias.constant = int32_t(std::clamp<int64_t>(
          int64_t(bias.constant) * (int64_t(1) << depth24_bias_shift_), INT32_MIN, INT32_MAX));
    }
    bias.slope = poly_scale * xe::kPolygonOffsetScaleSubpixelUnit;
    if (!dynamic_depth_bias_) {
      key.depth_bias = bias.constant;
      // A slope of -0.0 (a negative zero scale times the unit) is the same pipeline as +0.0 but other bytes of the key.
      key.slope_scaled_depth_bias = key_guard::CanonicalZero(bias.slope);
    }
  }
  const bool depth_clip = !state.clip.clip_disable;
  // Alpha to coverage is the pixel shader's (contract v6: its ALPHA_TEST
  // variant, constants.h AlphaToMaskParameters): the host's would test the
  // alpha after the exponent bias, never the one the hardware tests.
  key.raster = uint32_t(cull) | (uint32_t(front_ccw) << 4) | (uint32_t(fill) << 5) |
               (uint32_t(depth_clip) << 8);
  return KeyResult::kOk;
}

bool Pipelines::BuildDesc(const PipelineKey& key, const ShaderStore::Entry* vs,
                          const ShaderStore::Entry* ps, const StoredLayout& layout,
                          plume::RenderGraphicsPipelineDesc& desc,
                          ShaderStore::ShaderResult* vs_result,
                          ShaderStore::ShaderResult* ps_result) {
  desc.pipelineLayout = layout_->layout();
  desc.vertexShader = vs ? shaders_->GetShader(*vs, key.vs_variant, vs_result) : nullptr;
  if (key.ps != 0) {
    desc.pixelShader = ps ? shaders_->GetShader(*ps, key.ps_variant, ps_result) : nullptr;
  }
  // No specialization constants: each variant is its own blob on both backends
  // (shaders.h, contract v6).
  if (!desc.vertexShader || (key.ps != 0 && !desc.pixelShader)) {
    return false;
  }
  desc.inputSlots = layout.slots.data();
  desc.inputSlotsCount = uint32_t(layout.slots.size());
  desc.inputElements = layout.elements.data();
  desc.inputElementsCount = uint32_t(layout.elements.size());
  desc.primitiveTopology = plume::RenderPrimitiveTopology(key.topology);
  desc.multisampling = plume::RenderMultisampling(plume::RenderSampleCounts(key.sample_count));
  desc.renderTargetCount = key.color_count;
  for (uint32_t i = 0; i < key.color_count; ++i) {
    desc.renderTargetFormat[i] = plume::RenderFormat(key.color_format[i]);
    desc.renderTargetBlend[i] = UnpackBlend(key.blend[i]);
  }
  desc.depthTargetFormat = plume::RenderFormat(key.depth_format);
  desc.depthEnabled = (key.depth & 1u) != 0;
  desc.depthWriteEnabled = (key.depth & 2u) != 0;
  desc.depthFunction =
      desc.depthEnabled ? RenderComparisonFunction((key.depth >> 2) & 0xF)
                        : RenderComparisonFunction::ALWAYS;
  desc.stencilEnabled = (key.stencil & 1u) != 0;
  if (desc.stencilEnabled) {
    desc.stencilFrontFace.compareFunction = RenderComparisonFunction((key.stencil >> 1) & 0xF);
    desc.stencilFrontFace.failOp = RenderStencilOp((key.stencil >> 5) & 0xF);
    desc.stencilFrontFace.passOp = RenderStencilOp((key.stencil >> 9) & 0xF);
    desc.stencilFrontFace.depthFailOp = RenderStencilOp((key.stencil >> 13) & 0xF);
    desc.stencilBackFace.compareFunction = RenderComparisonFunction((key.stencil >> 17) & 0xF);
    desc.stencilBackFace.failOp = RenderStencilOp((key.stencil >> 21) & 0xF);
    desc.stencilBackFace.passOp = RenderStencilOp((key.stencil >> 25) & 0xF);
    desc.stencilBackFace.depthFailOp = RenderStencilOp((key.stencil_masks >> 24) & 0xF);
    desc.stencilReadMask = key.stencil_masks & 0xFFu;
    desc.stencilWriteMask = (key.stencil_masks >> 8) & 0xFFu;
    desc.stencilReference = (key.stencil_masks >> 16) & 0xFFu;
  }
  desc.cullMode = plume::RenderCullMode(key.raster & 0xF);
  desc.frontFace = ((key.raster >> 4) & 1u) ? plume::RenderFrontFace::COUNTER_CLOCKWISE
                                            : plume::RenderFrontFace::CLOCKWISE;
  desc.fillMode = plume::RenderFillMode((key.raster >> 5) & 0x7);
  desc.depthClipEnabled = ((key.raster >> 8) & 1u) != 0;
  desc.alphaToCoverageEnabled = false;  // the shader's (contract v6)
  desc.dynamicDepthBiasEnabled = dynamic_depth_bias_;
  desc.depthBias = key.depth_bias;
  desc.slopeScaledDepthBias = key.slope_scaled_depth_bias;
  return true;
}

void Pipelines::LogFailure(const PipelineKey& key, ShaderStore::ShaderResult vs_result,
                           ShaderStore::ShaderResult ps_result, bool fallback) {
  ++stats_.failed;
  if (fallback) {
    // A key with a fallback: one line for each (the key is remembered as failed and never built again), not counted against the cap below. Its draws take the game's
    // own pipeline (Scene::Prepare, variant_wait::NextStep). "ok" for both shaders means the shaders were fine and the driver refused the pipeline.
    ++stats_.variant_failed;
    REXLOG_WARN("[gpu] pipeline variant for vs {:016X}/{} ps {:016X}/{} not created (vs shader {}, ps shader {}, {} colour targets, {} samples, depth format {}): "
                "its draws use the game's own pipeline from now on",
                key.vs, key.vs_variant, key.ps, key.ps_variant, ShaderStore::ResultName(vs_result), ShaderStore::ResultName(ps_result), key.color_count, key.sample_count,
                key.depth_format);
    return;
  }
  if (stats_.failed - stats_.variant_failed <= 16) {
    REXLOG_ERROR("[gpu] pipeline for vs {:016X}/{} ps {:016X}/{} not created (vs shader {}, ps "
                 "shader {}, {} colour targets, depth format {})",
                 key.vs, key.vs_variant, key.ps, key.ps_variant,
                 ShaderStore::ResultName(vs_result), ShaderStore::ResultName(ps_result),
                 key.color_count, key.depth_format);
  }
}

plume::RenderPipeline* Pipelines::Get(const PipelineKey& key, const PipelineInputs& inputs,
                                      const Deferral* deferral, bool* pending) {
  if (pending) {
    *pending = false;
  } else {
    deferral = nullptr;  // nowhere to say "pending": the draw may not wait
  }
  if (draw_memo_enabled_) {
    ++draw_memo_profile_.get.tries;
    const auto* found = pipeline_lookup_memo_.Find(key);
    if (!found) {
      found = pipeline_lookup_memo_.FindOther(key);
    }
    if (found) {
      ++draw_memo_profile_.get.hits;
      return *found;
    }
  }
  const auto it = pipelines_.find(key);
  if (it != pipelines_.end()) {
    plume::RenderPipeline* got = it->second.get();
    if (draw_memo_enabled_ && got) {
      // Only the owning map has precedence for the entire pipeline lifetime.
      pipeline_lookup_memo_.Put(key, got);
    }
    return got;
  }
  if (const auto alias = aliases_.find(key); alias != aliases_.end()) {
    return alias->second;
  }
  plume::RenderPipeline* got = Resolve(key, inputs, deferral, pending);
  if (got && inputs.msaa_guest_samples != 0) {
    NoteLevelDependent(key, inputs);  // once: the next lookups hit the maps above
  }
  return got;
}

void Pipelines::NoteLevelDependent(const PipelineKey& key, const PipelineInputs& inputs) {
  PipelineKey canonical = key;
  canonical.sample_count = inputs.msaa_guest_samples;
  if (!level_dependent_index_.insert(canonical).second || !inputs.input_layout) {
    return;
  }
  const uint32_t stored = StoreLayout(*inputs.input_layout);
  if (stored == kNoLayout) {
    return;  // neither recorded nor built in the background: made at the draw when needed
  }
  level_dependent_.push_back(LevelDependent{canonical, stored});
}

Pipelines::Precached* Pipelines::FindListed(const PipelineKey& persistent) const {
  if (const auto it = precached_by_key_.find(persistent); it != precached_by_key_.end()) {
    return it->second;
  }
  if (const auto it = seeds_by_key_.find(persistent); it != seeds_by_key_.end()) {
    return it->second.get();
  }
  return nullptr;
}

Pipelines::LevelPrebuild Pipelines::PrebuildForLevel(uint32_t level, uint32_t from_level) {
  // CHECK: that the copies are the pipelines the first draws after the switch ask for (the same key
  // but the sample count: the log's pipeline statistics for the minute after a switch should show
  // few or no pending draws and nothing made on the render thread; a missing one shows as a draw
  // left out for a few frames), that the build of 4 and 8 sample pipelines works on both backends
  // (an 8 sample pipeline against an 8 sample framebuffer under the D3D12 debug layer and Vulkan
  // validation), and how long the set takes at the first switch of a session (the 20 s limit).
  // Also that the pause menu's and the map's world draws, whose targets now have the level's samples
  // (or 1 without MSAA), mostly find the game view's pipelines (the first pause open compiles few or
  // none on the render thread), and that a level change leaves them a copy too.
  LevelPrebuild result;
  prebuild_requests_.clear();
  prebuild_precached_.clear();
  prebuild_seeds_.clear();
  result.known = uint32_t(level_dependent_.size());
  if (workers_.empty() || stop_precache_.load() || !shaders_) {
    return result;
  }
  std::vector<Urgent> items;
  // The persistent keys at `level` of the drawn pipelines: the seeds (below) leave them to the first half.
  std::unordered_set<PipelineKey, PipelineKeyHash> covered;
  covered.reserve(level_dependent_.size());
  for (const LevelDependent& d : level_dependent_) {
    PipelineKey key = d.key;
    // EdramTargets::HostSamplesAt: the level, which may be fewer than the guest's samples (the
    // pause menu's and the map's world view, which the game draws with 4).
    key.sample_count = level;
    const PipelineKey persistent = PersistentKey(key, d.stored);
    covered.insert(persistent);
    if (pipelines_.count(key) != 0 || aliases_.count(key) != 0) {
      ++result.present;
      continue;
    }
    if (const auto asked = requests_.find(key); asked != requests_.end()) {
      const uint8_t state = asked->second->state.load();
      if (state == kQueued || state == kBuilding) {
        prebuild_requests_.push_back(asked->second);
      } else {
        ++result.present;  // finished: the end of the frame, or the first draw, takes it
      }
      continue;
    }
    if (Precached* const listed = FindListed(persistent)) {
      // A record of the cache file or the starter list has it (or a seed of an earlier call for this
      // level): that entry is made, the first draw takes it (TakePrecached).
      Precached& p = *listed;
      const uint8_t state = p.state.load();
      if (p.taken || state == kReady || state == kFailed || state == kClaimed) {
        ++result.present;
        continue;
      }
      if (p.seed) {
        // Queued behind everything (seed_): moved up to the copies for this switch, which it waits for.
        std::shared_ptr<Precached> seed = p.shared_from_this();
        prebuild_seeds_.push_back(seed);
        if (state == kQueued) {
          Urgent item;
          item.seed = std::move(seed);
          items.push_back(std::move(item));
          ++result.queued;
        }
        continue;
      }
      prebuild_precached_.push_back(&p);
      if (state == kQueued) {
        Urgent item;
        item.precached = &p;
        items.push_back(std::move(item));
        ++result.queued;
      }
      continue;
    }
    const ShaderStore::Entry* vs = shaders_->Find(key.vs);
    const ShaderStore::Entry* ps = key.ps != 0 ? shaders_->Find(key.ps) : nullptr;
    if (!vs || (key.ps != 0 && !ps)) {
      continue;  // not in the pack (a pack that changed): its draws are skipped as they were
    }
    auto request = std::make_shared<Request>();
    request->key = key;
    request->vs = vs;
    request->ps = ps;
    request->layout = stored_layouts_[d.stored].get();
    request->stored = d.stored;
    request->asked_frame = frame_;
    request->low = true;
    requests_.emplace(key, request);
    prebuild_requests_.push_back(request);
    Urgent item;
    item.request = std::move(request);
    items.push_back(std::move(item));
    ++result.queued;
  }
  if (!items.empty()) {
    {
      std::lock_guard<std::mutex> lock(work_mutex_);
      for (Urgent& item : items) {
        low_.push_back(std::move(item));
      }
    }
    work_cv_.notify_all();
  }
  SeedForLevel(level, from_level, covered, result);
  result.waiting = PrebuildWaiting();
  return result;
}

uint32_t Pipelines::PrebuildWaiting() const {
  uint32_t waiting = 0;
  for (const std::shared_ptr<Request>& r : prebuild_requests_) {
    const uint8_t state = r->state.load();
    waiting += state == kQueued || state == kBuilding;
  }
  for (const Precached* p : prebuild_precached_) {
    const uint8_t state = p->state.load();
    waiting += state == kQueued || state == kBuilding;
  }
  for (const std::shared_ptr<Precached>& p : prebuild_seeds_) {
    const uint8_t state = p->state.load();
    waiting += state == kQueued || state == kBuilding;
  }
  return waiting;
}

void Pipelines::SeedForLevel(uint32_t level, uint32_t from_level,
                             const std::unordered_set<PipelineKey, PipelineKeyHash>& covered,
                             LevelPrebuild& result) {
  // CHECK: written without a build or a run. Expected on a live level change: the "msaa: live change"
  // line says how many seeds were queued; one "level seeds for Nx" line follows within a few seconds
  // (hundreds queued, nearly all ready, the time in ms); the first views at the new level that the
  // session had not drawn (the map, a race) take seeds ("taken by draws" in the line that is written
  // when they are let go: the next change, a shortage, the exit) and create nothing on the render thread;
  // the frame times of the seconds after the switch do not show the builds (they run below normal
  // priority on all the steady workers but one, and not at all in boot mode).
  if (records_.empty()) {
    return;  // no cache and no list were read (mcla_gpu_pipeline_cache is off)
  }
  const level_seeds::Retarget what = level_seeds::Decide(seed_level_, level);
  if (what == level_seeds::Retarget::kRestart) {
    DropSeeds("the level changed again");  // the untaken ones of the old level: queued ones cancelled, made ones freed
  }
  seed_level_ = level;
  const backend::VideoMemory memory = device_ ? device_->QueryVideoMemory() : backend::VideoMemory{};
  const uint32_t allowed = level_seeds::AllowedCount(memory.used, memory.budget, seed_book_.Live());
  // What needs no seed: a record, a queue entry, a seed or a drawn pipeline's copy has the key at `level`.
  const auto present = [&](const PipelineKey& copy) {
    return recorded_.count(copy) != 0 || precached_by_key_.count(copy) != 0 ||
           seeds_by_key_.count(copy) != 0 || covered.count(copy) != 0;
  };
  level_seeds::PlanCounts counts;
  const std::vector<uint32_t> picks =
      level_seeds::PlanSeeds<Record, PipelineKeyHash>(records_, level, from_level, allowed, present, counts);
  result.seed_sources = counts.sources;
  result.seed_present = counts.same_level + counts.duplicate + counts.present;
  result.seed_limited = counts.limited;
  result.seeds = counts.picked;
  if (picks.empty()) {
    return;
  }
  const uint32_t picked = uint32_t(picks.size());
  if (what == level_seeds::Retarget::kKeep && seed_gen_ != 0 && seed_book_.Active()) {
    seed_book_.Add(seed_gen_, picked);
  } else {
    seed_gen_ = seed_book_.Begin(level, picked, std::chrono::steady_clock::now());
  }
  std::vector<Urgent> queue;
  queue.reserve(picks.size());
  for (const uint32_t i : picks) {
    const Record& r = records_[i];
    auto seed = std::make_shared<Precached>();
    seed->key = level_seeds::AtLevel(r.key, level);
    seed->layout = stored_layouts_[r.layout].get();  // owned by a unique_ptr: stable
    seed->seed = true;
    seed->seed_gen = seed_gen_;
    seeds_by_key_.emplace(seed->key, seed);
    Urgent item;
    item.seed = std::move(seed);
    queue.push_back(std::move(item));
  }
  {
    std::lock_guard<std::mutex> lock(work_mutex_);
    for (Urgent& item : queue) {
      seed_.push_back(std::move(item));
    }
  }
  work_cv_.notify_all();
}

void Pipelines::DropSeeds(const char* why) {
  // Seeds nobody has taken go: the queue is emptied, a seed no worker began is marked claimed so that a worker
  // that already popped it leaves it (the same compare-exchange a draw's claim uses), a made one is freed (no
  // command list ever held it), and one being built is let finish: its worker drops the result with its
  // reference. Seeds a draw took stay in seeds_by_key_ as the alias marker a precache entry is.
  {
    std::lock_guard<std::mutex> lock(work_mutex_);
    seed_.clear();
  }
  uint32_t cancelled = 0;
  uint32_t freed = 0;
  for (auto it = seeds_by_key_.begin(); it != seeds_by_key_.end();) {
    Precached& p = *it->second;
    if (p.taken) {
      ++it;
      continue;
    }
    uint8_t state = kQueued;
    if (p.state.compare_exchange_strong(state, kClaimed)) {
      ++cancelled;
    } else if (state == kReady) {
      std::lock_guard<std::mutex> lock(precache_mutex_);
      if (p.pipeline) {
        seed_graveyard_.push_back(std::move(p.pipeline));  // destroyed a few a frame (EndFrame)
        ++freed;
      }
    }
    it = seeds_by_key_.erase(it);
  }
  if (seed_book_.Active()) {
    seed_book_.Cancelled(seed_gen_, cancelled);
    seed_book_.Freed(seed_gen_, freed);
    const level_seeds::SeedBook::Report r = seed_book_.Snapshot();
    REXLOG_INFO("[gpu] pipelines: level seeds for {}x let go ({}): {} queued, {} made, {} taken by draws "
                "({} more made by the render thread), {} failed, {} dropped before a worker began them, "
                "{} made and never asked for (freed, about {} MiB)",
                r.level, why, r.queued, r.ready, r.taken, r.claimed, r.failed, r.cancelled, r.freed,
                (uint64_t(r.freed) * level_seeds::kBytesEach) >> 20);
    seed_book_.End();
  }
  seed_level_ = 0;
  seed_gen_ = 0;
}

void Pipelines::SeedFrame() {
  level_seeds::SeedBook::Report r;
  if (seed_book_.TakeDone(r)) {
    const uint32_t held = r.ready > r.taken + r.freed ? r.ready - r.taken - r.freed : 0;
    REXLOG_INFO("[gpu] pipelines: level seeds for {}x: {} queued, {} ready, {} failed, {} taken by draws so far, "
                "{} made by the render thread, in {:.0f} ms ({} worker(s), {} at a time); about {} MiB held",
                r.level, r.queued, r.ready, r.failed, r.taken, r.claimed, r.build_ms, workers_.size(),
                seed_slots_, (uint64_t(held) * level_seeds::kBytesEach) >> 20);
  }
  if (frame_ < seed_next_check_ || seed_book_.Live() == 0) {
    return;
  }
  seed_next_check_ = frame_ + level_seeds::kCheckFrames;
  const backend::VideoMemory memory = device_ ? device_->QueryVideoMemory() : backend::VideoMemory{};
  if (level_seeds::ShortOfRoom(memory.used, memory.budget)) {
    // The seeds are a luxury: they go before the caches' own response (90%) has to release anything.
    DropSeeds("the video memory is short");
  }
}

plume::RenderPipeline* Pipelines::Resolve(const PipelineKey& key, const PipelineInputs& inputs,
                                          const Deferral* deferral, bool* pending) {
  Miss miss;
  miss.deferral = deferral;
  const auto began = std::chrono::steady_clock::now();
  plume::RenderPipeline* got = ResolveMiss(key, inputs, miss, pending);
  if (miss.held) {
    // A capture is running (sync_builds.h): the draw was not left out; what it waited for is counted.
    NoteSyncWait(key, deferral->kind, began);
  }
  return got;
}

void Pipelines::NoteSyncWait(const PipelineKey& key, sync_builds::Kind kind,
                             std::chrono::steady_clock::time_point began) {
  const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - began)
                                   .count());
  if (sync_builds::NoteWait(kind, ns)) {
    REXLOG_INFO("[gpu] capture sync: guest frame {}: {} waited {:.1f} ms for a pipeline (vs "
                "{:016X}/{} ps {:016X}/{})",
                frame_, sync_builds::KindName(kind), double(ns) / 1.0e6, key.vs, key.vs_variant,
                key.ps, key.ps_variant);
  }
}

namespace {

sync_build_log::KeyWords WordsOf(const PipelineKey& key) {
  sync_build_log::KeyWords w;
  w.vs = key.vs;
  w.ps = key.ps;
  w.vs_variant = key.vs_variant;
  w.ps_variant = key.ps_variant;
  w.color_count = key.color_count;
  w.sample_count = key.sample_count;
  w.stencil = sync_build_log::StencilWrites(key.stencil, key.stencil_masks);
  return w;
}

}  // namespace

void Pipelines::LogSyncBuild(const PipelineKey& key, const Miss& miss, uint64_t ns) {
  const uint64_t n = sync_log_.NoteBuild(ns);
  if (!sync_build_log::LogThis(n)) {
    return;
  }
  const sync_build_log::Why why = miss.take == Take::kMakeHere ? sync_build_log::Why::kClaimed
                                  : miss.take == Take::kFailed ? sync_build_log::Why::kAgain
                                                               : sync_build_log::Why::kNew;
  // Whether the draw's own rule would have let it wait for a builder thread (asked only for a line that is written).
  const bool skippable = miss.deferral && miss.deferral->may_skip && miss.deferral->may_skip(miss.deferral->context);
  REXLOG_INFO("{}", sync_build_log::FormatBuild(n, frame_, ns, why, WordsOf(key), skippable));
}

void Pipelines::LogSyncWait(const PipelineKey& key, const char* source, uint64_t ns) {
  const uint64_t n = sync_log_.NoteWait(ns);
  if (sync_build_log::LogThis(n)) {
    REXLOG_INFO("{}", sync_build_log::FormatWait(n, frame_, ns, source, WordsOf(key)));
  }
}

void Pipelines::NoteHandover(const PipelineKey& key, const char* source, bool raised, bool made_here,
                             uint64_t waited_ns) {
  if (!raised && !made_here) {
    return;
  }
  const uint64_t n = ++handovers_;
  made_here_too_ += made_here ? 1u : 0u;
  if (sync_build_log::LogThis(n)) {
    REXLOG_INFO("{}", sync_build_log::FormatHandover(n, frame_, waited_ns, source, raised, made_here,
                                                     WordsOf(key)));
  }
}

void Pipelines::RaiseWorker(int32_t index) {
  const uint32_t started = workers_started_.load(std::memory_order_acquire);
  if (index < 0 || uint32_t(index) >= started || !worker_raised_) {
    return;
  }
  larecomp::os::SetThreadIntent(workers_[size_t(index)], larecomp::os::ThreadIntent::kNormal);
  // After the raise, so that the worker's own re-lowering (at its next entry) cannot come before it.
  worker_raised_[size_t(index)].store(true, std::memory_order_release);
}

bool Pipelines::WaitForBuild(const std::atomic<uint8_t>& state, const std::atomic<int32_t>& builder,
                             bool may_make_here, bool* raised) {
  const auto began = std::chrono::steady_clock::now();
  pipeline_wait::Facts facts;
  facts.may_make_here = may_make_here;
  std::unique_lock<std::mutex> lock(precache_mutex_);
  for (;;) {
    if (state.load() != kBuilding) {
      return true;
    }
    facts.waited_ms = uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - began)
                                   .count());
    const int32_t who = builder.load(std::memory_order_acquire);
    facts.builder_known = who >= 0;
    switch (pipeline_wait::Next(facts)) {
      case pipeline_wait::Step::kMakeHere:
        return false;
      case pipeline_wait::Step::kRaise:
        facts.raised = true;
        *raised = true;
        RaiseWorker(who);
        break;
      case pipeline_wait::Step::kKeepWaiting:
        break;
    }
    precache_done_.wait_for(lock, std::chrono::milliseconds(pipeline_wait::kSliceMs),
                            [&] { return state.load() != kBuilding; });
  }
}

plume::RenderPipeline* Pipelines::ResolveMiss(const PipelineKey& key, const PipelineInputs& inputs,
                                              Miss& miss, bool* pending) {
  const bool has_fallback = miss.deferral && miss.deferral->has_fallback;
  // A background build asked for earlier, then the precache list's entry.
  if (!requests_.empty()) {
    if (plume::RenderPipeline* requested = TakeRequested(key, miss)) {
      return requested;
    }
  }
  if (miss.take == Take::kNone) {
    if (plume::RenderPipeline* precached = TakePrecached(key, miss, inputs)) {
      return precached;
    }
  }
  switch (miss.take) {
    case Take::kPending:
      *pending = true;
      return nullptr;
    case Take::kReported:
      return nullptr;
    case Take::kNone:
      // Nobody has it: a worker makes it when the draw may wait for it.
      if (MaySkip(miss) && StartRequest(key, inputs, has_fallback)) {
        *pending = true;
        return nullptr;
      }
      ++stats_.made_here_new;
      break;
    case Take::kFailed:
      ++stats_.made_here_retry;
      break;
    case Take::kMakeHere:
      break;
  }
  const auto t0 = std::chrono::steady_clock::now();
  ShaderStore::ShaderResult vs_result = ShaderStore::ShaderResult::kOk;
  ShaderStore::ShaderResult ps_result = ShaderStore::ShaderResult::kOk;
  StoredLayout layout;  // the session layout's arrays, copied
  layout.elements = inputs.input_layout->elements;
  layout.slots = inputs.input_layout->slots;
  plume::RenderGraphicsPipelineDesc desc;
  std::unique_ptr<plume::RenderPipeline> pipeline;
  if (BuildDesc(key, inputs.vs, key.ps != 0 ? inputs.ps : nullptr, layout, desc, &vs_result,
                &ps_result)) {
    if (!SkipShaders(key.vs, key.ps)) {
      pipeline = device_->CreateGraphicsPipeline(desc, "guest pipeline");
    }
  }
  const uint64_t built_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - t0)
                                         .count());
  stats_.create_ns += built_ns;
  if (pipeline) {
    ++stats_.created;
    LogSyncBuild(key, miss, built_ns);  // the same builds as the window lines' "+N created"
    if (persist_) {
      RecordPipeline(key, *inputs.input_layout);
    }
  } else {
    LogFailure(key, vs_result, ps_result, has_fallback);
  }
  plume::RenderPipeline* raw = pipeline.get();
  pipelines_.emplace(key, std::move(pipeline));
  return raw;
}

Pipelines::Stats Pipelines::TakeStats() {
  Stats s = stats_;
  stats_ = Stats{};
  return s;
}

}  // namespace mcla::gpu
