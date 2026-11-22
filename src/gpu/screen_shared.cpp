// MCLA GPU renderer -- the shared screen-space products: the owner. See screen_shared.h.

#include "screen_shared.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <string>
#include <utility>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "backend.h"
#include "host_shaders.h"
#include "menu_world.h"
#include "pass_timing.h"
#include "records.h"
#include "temporal_camera.h"
#include "temporal_identities.h"
#include "texture_bytes.h"

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[screen]";
// The frames the Hi-Z's images are kept after the last frame that wanted it (a pause spell, a menu: not made again at once).
constexpr uint32_t kHizReleaseAfterFrames = 120;
constexpr plume::RenderFormat kHizFormat = plume::RenderFormat::R32_FLOAT;
// The colour captures' format (the frozen header: RGBA16F) and the frames the capture's images are kept after the last frame that planned it (a pause spell, a menu).
constexpr plume::RenderFormat kColourFormat = plume::RenderFormat::R16G16B16A16_FLOAT;
constexpr uint32_t kPostReleaseAfterFrames = 120;
// The smallest picture a capture is made for (the car history's rule: a size below it is no main view).
constexpr uint32_t kMinRenderSize = 64;
// The destination of the game's mid-scene depth copy in the retail build (car_selfrefl.h kTriggerDestination, shafts.h, ao.h): another build gives another address and then there is no trigger.
constexpr uint32_t kTriggerDestination = 0x078B0000;

constexpr const char* kHizNames[screen::kHizMaxLevels] = {"screen hiz 0", "screen hiz 1", "screen hiz 2", "screen hiz 3",
                                                          "screen hiz 4", "screen hiz 5", "screen hiz 6", "screen hiz 7"};
constexpr const char* kPostNames[screen::kColourPostMaxLevels] = {"screen colour post 0", "screen colour post 1", "screen colour post 2", "screen colour post 3",
                                                                  "screen colour post 4", "screen colour post 5", "screen colour post 6", "screen colour post 7"};
// The AO's own names for the bounce light's colour chain (they were its images): the log and the memory tools read the same.
constexpr const char* kPreNames[screen::kColourPreLevels] = {"ssgi colour", "ssgi colour 1/2", "ssgi colour 1/4"};

float BankFloat(const uint8_t* bank, uint32_t reg, uint32_t comp) {
  return std::bit_cast<float>(LoadBe32(bank + reg * 16 + comp * 4));
}

double Mib(uint64_t bytes) { return double(bytes) / (1024.0 * 1024.0); }

uint32_t SlotOf(const HostImage* image) {
  return image && image->texture ? image->slot : screen::kNoSlot;
}

// "lin_depth, hiz_min" of a mask.
std::string ProductNames(uint32_t products) {
  std::string text;
  for (uint32_t p = 0; p < screen::kProductCount; ++p) {
    if ((products & (1u << p)) == 0) continue;
    if (!text.empty()) text += ", ";
    text += screen::ProductName(screen::Product(p));
  }
  return text.empty() ? std::string("none") : text;
}

}  // namespace

ScreenShared::~ScreenShared() = default;

void ScreenShared::Initialize(HostPasses& passes, EdramTargets& edram, PassTiming& pass_timing) {
  passes_ = &passes;
  edram_ = &edram;
  pass_timing_ = &pass_timing;
  // A new start: nothing is listed with the host precache, nothing has failed yet.
  hiz_listed_ = false;
  hiz_failed_ = false;
  view_failed_ = false;
  colour_listed_ = false;
  colour_failed_ = false;
  const HostShaderBlob hiz = ScreenHizPixelShader();
  hiz_shader_ = passes.CreateShader(hiz.data, hiz.size, hiz.name);
  if (!hiz_shader_) {
    hiz_failed_ = true;
    REXLOG_ERROR("{} the Hi-Z's shader could not be made: no hiz_min this session", kPrefix);
  }
  const HostShaderBlob view = ScreenViewPixelShader();
  view_shader_ = passes.CreateShader(view.data, view.size, view.name);
  if (!view_shader_) {
    view_failed_ = true;
    REXLOG_ERROR("{} the debug view's shader could not be made: no debug view this session", kPrefix);
  }
  for (uint32_t pass = 0; pass < kColourPassCount; ++pass) {
    const HostShaderBlob colour = ScreenColourPixelShader(pass);
    colour_shaders_[pass] = passes.CreateShader(colour.data, colour.size, colour.name);
    if (!colour_shaders_[pass]) {
      colour_failed_ = true;
    }
  }
  if (colour_failed_) {
    REXLOG_ERROR("{} the colour capture's shaders could not be made: no colour_post this session", kPrefix);
  }
}

void ScreenShared::Shutdown() {
  // The host builder's function names the Hi-Z's shader: stop it first (AmbientOcclusion::Shutdown does the same; stopping twice is safe).
  if (passes_) {
    passes_->precache().Stop();
  }
  // The GPU is idle and the heap goes later: the slots are not handed back (AmbientOcclusion::Shutdown).
  view_pipelines_.clear();
  hiz_pipeline_.reset();
  hiz_shader_.reset();
  view_shader_.reset();
  for (auto& pipeline : colour_pipelines_) {
    pipeline.reset();
  }
  for (auto& shader : colour_shaders_) {
    shader.reset();
  }
  for (HostImage& image : hiz_) {
    image = HostImage{};
  }
  hiz_images_ = 0;
  for (HostImage& image : colour_pre_) {
    image = HostImage{};
  }
  for (HostImage& image : post_) {
    image = HostImage{};
  }
  history_ = HostImage{};
  post_images_ = 0;
  last_capture_ = screen::CaptureKey{};
  cameras_.Reset();
  frame_ = screen::ScreenFrame{};
  inputs_ = screen::FrameInputs{};
  adopted_ = Adopted{};
  published_ = false;
  passes_ = nullptr;
}

float ScreenShared::ExposureFactor() const {
  return screen::ExposureOf(exp_bias_, exposure_known_);
}

void ScreenShared::BeginFrame(uint64_t frame_serial) {
  frame_serial_ = frame_serial;
  // The frame is empty until the AO's depth chain publishes it.
  frame_ = screen::ScreenFrame{};
  frame_.frame_serial = uint32_t(frame_serial);
  adopted_ = Adopted{};
  inputs_ = screen::FrameInputs{};
  inputs_.serial = uint32_t(frame_serial);
  published_ = false;
  sealed_ = false;
  pre_made_ = false;
  post_done_ = false;
  plan_ = screen::PlanFrame(wants_, screen::ViewFromSetting(larecomp::settings::mcla_gpu_screen_view.Get()),
                            screen::LevelFromSetting(larecomp::settings::mcla_gpu_screen_view_level.Get()));
  if (plan_.wanted != logged_wanted_) {
    LogWants();
  }
  if ((plan_.make & screen::ProductBit(screen::kHizMin)) != 0) {
    hiz_idle_frames_ = 0;
  } else if (hiz_images_ != 0 && ++hiz_idle_frames_ >= kHizReleaseAfterFrames) {
    ReleaseHiz();
  }
  // The colour capture's images and the history's go back after a couple of seconds without a plan for them.
  if ((plan_.make & screen::ProductBit(screen::kColourPost)) != 0) {
    post_idle_frames_ = 0;
  } else if (post_images_ != 0 && ++post_idle_frames_ >= kPostReleaseAfterFrames) {
    ReleaseColourPost();
  }
  if ((plan_.make & screen::ProductBit(screen::kHistory)) != 0) {
    hist_idle_frames_ = 0;
  } else if (history_.texture && ++hist_idle_frames_ >= kPostReleaseAfterFrames) {
    passes_->ReleaseImage(history_);
    last_capture_.valid = false;
    ++colour_generation_;
  }
}

void ScreenShared::LogWants() {
  logged_wanted_ = plan_.wanted;
  std::string who;
  for (uint32_t c = 0; c < uint32_t(screen::Consumer::kCount); ++c) {
    const screen::Consumer consumer = screen::Consumer(c);
    const uint32_t products = consumer == screen::Consumer::kDebugView ? screen::ViewNeeds(plan_.view) : wants_.Of(consumer);
    if (products == 0) continue;
    who += std::format("{}{} ({})", who.empty() ? "" : "; ", screen::ConsumerName(consumer), ProductNames(products));
  }
  REXLOG_INFO("{} products wanted: {}{}{}", kPrefix, ProductNames(plan_.wanted), who.empty() ? "" : " by ", who);
}

// ---------------------------------------------------------------------------
// the Hi-Z
// ---------------------------------------------------------------------------

std::unique_ptr<plume::RenderPipeline> ScreenShared::MakeHizPipeline() {
  if (!hiz_shader_) {
    return nullptr;
  }
  // The host pass layout (a push block of 32 bytes), one R32_FLOAT target, written whole: the full-screen triangle's pipeline.
  return passes_->CreatePipeline(*hiz_shader_, kHizFormat, "screen hiz");
}

plume::RenderPipeline* ScreenShared::HizPipeline() {
  if (hiz_pipeline_) {
    return hiz_pipeline_.get();
  }
  if (hiz_failed_ || !hiz_shader_) {
    return nullptr;
  }
  HostPrecache& precache = passes_->precache();
  const uint64_t key = HostPrecache::Key(HostPrecache::kScreenShared, 0);
  if (!hiz_listed_) {
    hiz_listed_ = true;
    std::vector<HostPrecache::Item> items;
    items.push_back({key, "screen hiz", [this] { return MakeHizPipeline(); }});
    const uint32_t count = precache.AddLive("screen products", std::move(items));
    REXLOG_INFO("{} hiz_min is wanted: its pipeline ({}) is made in the background first, and the chain is made from the frame it is done", kPrefix, count);
  }
  if (!precache.Ready(key)) {
    return nullptr;  // a frame never waits for a compile: no hiz_min this frame
  }
  hiz_pipeline_ = precache.Acquire(key, "screen hiz", [this] { return MakeHizPipeline(); });
  if (!hiz_pipeline_) {
    hiz_failed_ = true;
    REXLOG_ERROR("{} the Hi-Z's pipeline could not be made: no hiz_min this session", kPrefix);
  }
  return hiz_pipeline_.get();
}

uint32_t ScreenShared::RecordHiz(plume::RenderCommandList* list, const HostImage& depth0, uint32_t slots[screen::kHizMaxLevels]) {
  plume::RenderPipeline* pipeline = HizPipeline();
  if (!pipeline || !list || !depth0.texture || depth0.width == 0 || depth0.height == 0) {
    return 0;
  }
  const uint32_t levels = screen::HizLevelCount(depth0.width, depth0.height);
  // The images: one a level, kept while the size is; a different size remakes them (and the generation says so).
  bool ok = true;
  for (uint32_t level = 0; level < levels && ok; ++level) {
    const plume::RenderTexture* before = hiz_[level].texture.get();
    ok = passes_->EnsureImage(hiz_[level], screen::HizLevelSize(depth0.width, level), screen::HizLevelSize(depth0.height, level), kHizFormat, kHizNames[level]);
    if (ok && hiz_[level].texture.get() != before) {
      ++hiz_generation_;
    }
  }
  for (uint32_t level = levels; level < screen::kHizMaxLevels; ++level) {
    if (hiz_[level].texture) {
      passes_->ReleaseImage(hiz_[level]);
      ++hiz_generation_;
    }
  }
  uint32_t held = 0;
  for (uint32_t level = 0; level < screen::kHizMaxLevels; ++level) {
    held += hiz_[level].texture ? 1u : 0u;
  }
  hiz_images_ = held;
  if (!ok) {
    if (!hiz_images_failed_logged_) {
      hiz_images_failed_logged_ = true;
      REXLOG_ERROR("{} the Hi-Z's images could not be made: no hiz_min while that lasts (asked again every frame)", kPrefix);
    }
    return 0;
  }
  // CHECK (not run): the pass reads the AO's level 0 depth in the shader-read layout it was left in (DrawInto leaves its own images there too; the ray traced occlusion's compute read keeps
  // that layout, car_rt_ao.cpp), on both APIs, with the Vulkan synchronisation validation on; and the seven passes' cost (expected below 0.05 ms at 1080p traced: 7 tiny R32_FLOAT passes).
  pass_timing_->Mark(list, "screen hiz");
  for (uint32_t level = 0; level < levels; ++level) {
    const HostImage& source = level == 0 ? depth0 : hiz_[level - 1];
    HizPush push;
    push.slots[0] = source.slot;
    push.slots[1] = level == 0 ? 0u : 1u;
    push.sizes[0] = source.width;
    push.sizes[1] = source.height;
    push.sizes[2] = hiz_[level].width;
    push.sizes[3] = hiz_[level].height;
    passes_->DrawInto(list, hiz_[level], pipeline, &push, sizeof(push));
    slots[level] = hiz_[level].slot;
  }
  if (hiz_logged_size_[0] != depth0.width || hiz_logged_size_[1] != depth0.height) {
    hiz_logged_size_[0] = depth0.width;
    hiz_logged_size_[1] = depth0.height;
    REXLOG_INFO("{} hiz_min: {} levels from {}x{} down to {}x{} (R32_FLOAT, the min of 2x2 each, level 0 the linear depth with its marks off), {:.1f} MiB", kPrefix, levels,
                depth0.width, depth0.height, screen::HizLevelSize(depth0.width, levels - 1), screen::HizLevelSize(depth0.height, levels - 1),
                Mib(screen::HizBytes(depth0.width, depth0.height)));
  }
  return levels;
}

void ScreenShared::ReleaseHiz() {
  for (HostImage& image : hiz_) {
    if (image.texture) {
      passes_->ReleaseImage(image);
    }
  }
  hiz_images_ = 0;
  hiz_idle_frames_ = 0;
  ++hiz_generation_;
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

void ScreenShared::Publish(plume::RenderCommandList* list, const Adopted& adopted) {
  adopted_ = adopted;
  published_ = false;
  frame_ = screen::ScreenFrame{};
  frame_.frame_serial = uint32_t(frame_serial_);
  const HostImage* depth0 = adopted.depth[0];
  if (!depth0 || !depth0->texture) {
    return;  // no depth chain this frame: nothing to publish
  }
  screen::FrameInputs& in = inputs_;
  in = screen::FrameInputs{};
  in.serial = uint32_t(frame_serial_);
  in.traced[0] = depth0->width;
  in.traced[1] = depth0->height;
  in.render[0] = adopted.render[0];
  in.render[1] = adopted.render[1];
  in.host_samples = adopted.host_samples;
  in.sample0 = adopted.sample0;
  in.flags = (adopted.pause_view ? screen::kFramePause : 0u);
  if (in.traced[0] < in.render[0] || in.traced[1] < in.render[1]) {
    in.flags |= screen::kFrameHalfTraced;
  }
  for (uint32_t i = 0; i < screen::kDepthLevels; ++i) {
    in.depth[i] = SlotOf(adopted.depth[i]);
  }
  in.depth_full = SlotOf(adopted.depth_full);
  in.normal = SlotOf(adopted.normal);
  for (uint32_t i = 0; i < screen::kColourPreLevels; ++i) {
    in.colour_pre[i] = pre_made_ ? SlotOf(&colour_pre_[i]) : screen::kNoSlot;
  }
  in.colour_generation = colour_generation_;
  in.owner_generation = adopted.owner_generation;
  in.camera = adopted.camera;
  // The camera's exposure bias (the last main draw's) and its previous view-projection (the camera history's pair, when it is the pair of the camera the AO worked with).
  in.camera.exposure_bias = screen::ExposureOf(exp_bias_, exposure_known_);
  screen::FillPreviousViewProjection(cameras_.Previous(frame_count_, adopted.pause_view), cameras_.Before(frame_count_, adopted.pause_view), &in.camera);
  if ((plan_.make & screen::ProductBit(screen::kHizMin)) != 0) {
    in.hiz_levels = RecordHiz(list, *depth0, in.hiz);
  }
  in.hiz_generation = hiz_generation_;
  BuildFrame();
  if ((plan_.make & screen::ProductBit(screen::kColourPost)) == 0) {
    Seal();  // no capture is planned behind this: the frame is whole (else CapturePost seals it, or the frame's end)
  }
  published_ = true;
  ++published_frames_;
  if (!first_publish_logged_) {
    first_publish_logged_ = true;
    REXLOG_INFO("{} the first published frame: {} at {}x{} traced, {}x{} picture, {} host samples", kPrefix, ProductNames(frame_.valid), frame_.traced[0], frame_.traced[1],
                frame_.render[0], frame_.render[1], frame_.host_samples);
  }
}

const HostImage* ScreenShared::Image(screen::Product product, uint32_t level) const {
  if (!frame_.Has(product)) {
    return nullptr;
  }
  switch (product) {
    case screen::kLinDepth: return level < screen::kDepthLevels ? adopted_.depth[level] : nullptr;
    case screen::kLinDepthFull: return adopted_.depth_full;
    case screen::kHizMin: return level < frame_.hiz_levels && hiz_[level].texture ? &hiz_[level] : nullptr;
    case screen::kNormal: return adopted_.normal;
    case screen::kColourPre: return pre_made_ && level < screen::kColourPreLevels ? &colour_pre_[level] : nullptr;
    case screen::kColourPost: return level < frame_.colour_post_levels && post_[level].texture ? &post_[level] : nullptr;
    case screen::kHistory: return history_.texture ? &history_ : nullptr;
    case screen::kProductCount: break;
  }
  return nullptr;
}

uint64_t ScreenShared::HeldBytes() const {
  uint64_t bytes = 0;
  const auto add = [&bytes](const HostImage& image) {
    if (image.texture) {
      bytes += TextureBytes(image.width, image.height, 1, image.format);
    }
  };
  for (const HostImage& image : hiz_) {
    add(image);
  }
  for (const HostImage& image : colour_pre_) {
    add(image);
  }
  for (const HostImage& image : post_) {
    add(image);
  }
  add(history_);
  return bytes;
}

// ---------------------------------------------------------------------------
// the camera history and the frame's own draws
// ---------------------------------------------------------------------------

void ScreenShared::WantCamera(screen::Consumer consumer, bool on) {
  if (consumer >= screen::Consumer::kCount) {
    return;
  }
  const uint32_t bit = 1u << uint32_t(consumer);
  camera_users_ = on ? (camera_users_ | bit) : (camera_users_ & ~bit);
}

void ScreenShared::OnMainDraw(const temporal::MainMatch& match, int32_t exp_bias) {
  if (!match) {
    return;
  }
  if (match.samples == temporal::kPauseSamples) {
    pause_view_ = true;  // the pause menu's, the race pause's, the garage's and the map's world (menu_world.h)
  } else {
    play_view_ = true;
  }
  if (match.color) {
    // Only a draw that writes the main colour has its target's bias (a depth-only draw's colour state is whatever the last draw left).
    exp_bias_ = exp_bias;
    exposure_known_ = true;
  }
}

void ScreenShared::OnAnchor(const ConstantBanks::Banks& banks) {
  anchor_seen_ = true;
  // The play view's camera and the pause/map world's (4x samples) are kept apart: the lens differs (a field of view of 25 to 54 degrees). A frame with draws of both kinds or of neither gives no
  // camera, and the history starts again (the rule the car self-reflection's own bookkeeping had, and the AO's).
  if (play_view_ == pause_view_) {
    cameras_.OnAnchor(frame_count_, false, nullptr);
    return;
  }
  const bool pause = pause_view_;
  const uint8_t* ps = banks.ps.data();
  // The fetch size the camera test compares c62 against is c62 itself here: only the temporal system knows the anchor's fetches, and a frame size that is not the main view's is caught by the
  // rectangle of the trigger.
  const float width = BankFloat(ps, 62, 0);
  const float height = BankFloat(ps, 62, 1);
  if (!(width >= 16.0f && width <= 16384.0f && height >= 16.0f && height <= 16384.0f)) {
    cameras_.OnAnchor(frame_count_, pause, nullptr);
    return;
  }
  temporal::Camera camera;
  const temporal::Reject reject = temporal::ExtractCamera(ps, uint32_t(width + 0.5f), uint32_t(height + 0.5f), frame_count_, &camera);
  if (reject != temporal::Reject::kNone) {
    cameras_.OnAnchor(frame_count_, pause, nullptr);
    if (anchor_rejects_logged_ < 8) {
      ++anchor_rejects_logged_;
      REXLOG_INFO("{} the anchor's constants gave no camera: {} (frame {}, logged {} of 8)", kPrefix, temporal::RejectName(reject), frame_count_, anchor_rejects_logged_);
    }
    return;
  }
  cameras_.OnAnchor(frame_count_, pause, &camera);
}

void ScreenShared::OnPresent() {
  if (!sealed_ && frame_.valid != 0) {
    Seal();  // a frame whose capture was planned and did not come (no trigger, a pipeline still being made): whole as it is
  }
  // The history: this frame's capture becomes the last frame's, while the history is wanted (a trade of the two images: no copy). A frame without a capture leaves no history for the next.
  const bool keep = (plan_.make & screen::ProductBit(screen::kHistory)) != 0;
  if (post_done_ && keep && post_[0].texture) {
    std::swap(post_[0], history_);
    last_capture_ = this_capture_;
  } else {
    last_capture_.valid = false;
  }
  ++frame_count_;
  play_view_ = false;
  pause_view_ = false;
  anchor_seen_ = false;
  composite_seen_ = false;
  trigger_seen_ = false;
}

bool ScreenShared::IsTrigger(const ResolveRecord& record, const ResolveCache::Summary& summary, bool anchor_seen, bool composite_seen, bool trigger_seen) {
  // The ambient occlusion's trigger (ao.cpp Judge, shafts.cpp IsFirstPoint, car_selfrefl.cpp's own before SS-1 moved it here): the first copy of the main depth that is no end-of-scene copy, into
  // 078B0000, before the anchor. The play view's rectangle starts at the origin and stays inside the picture (the game copies the picture in two tiles; the target is whole at the first); the
  // pause view's three tiles come in an order nobody has looked at, so any rectangle inside the picture will do (the host draws the world once, so the target is whole at whichever tile comes
  // first) and the destination must be the whole picture (menu_world.h).
  if (!summary.depth_copy || !summary.source || trigger_seen) {
    return false;
  }
  const uint32_t samples = temporal::MainDepthSamples(summary.source_key);
  const bool pause = samples == temporal::kPauseSamples;
  if (!temporal::IsPlaySamples(samples) && !pause) {
    return false;
  }
  if ((record.flags & kEventInsideEndTiling) || summary.clears_depth || summary.dest_address != kTriggerDestination || anchor_seen || composite_seen) {
    return false;
  }
  const uint32_t width = std::min(summary.dest_width, summary.source->width);
  const uint32_t height = std::min(summary.dest_height, summary.source->height);
  if (pause) {
    return menu_world::PauseFirstPointGeometry(summary.x0, summary.y0, summary.x1, summary.y1, width, height, summary.source->width, summary.source->height, EdramTargets::kHeightAlignment);
  }
  return summary.x0 == 0 && summary.y0 == 0 && summary.x1 > summary.x0 && summary.y1 > summary.y0 && uint32_t(summary.x1) <= width && uint32_t(summary.y1) <= height;
}

// ---------------------------------------------------------------------------
// the frame's making and sealing
// ---------------------------------------------------------------------------

void ScreenShared::BuildFrame() {
  frame_ = screen::MakeFrame(inputs_, last_generation_);
}

void ScreenShared::Seal() {
  frame_.generation = generation_.Of(screen::SignatureOf(inputs_, frame_));
  last_generation_ = frame_.generation;
  sealed_ = true;
}

// ---------------------------------------------------------------------------
// colour_pre: the bounce light's colour chain (the images and the call are the owner's, the pass is the AO's until SS-2)
// ---------------------------------------------------------------------------

bool ScreenShared::EnsureColourPre(const uint32_t sizes[screen::kColourPreLevels][2], plume::RenderFormat format) {
  if (!passes_) {
    return false;
  }
  bool ok = true;
  for (uint32_t level = 0; level < screen::kColourPreLevels && ok; ++level) {
    const plume::RenderTexture* before = colour_pre_[level].texture.get();
    ok = passes_->EnsureImage(colour_pre_[level], sizes[level][0], sizes[level][1], format, kPreNames[level]);
    if (ok && colour_pre_[level].texture.get() != before) {
      ++colour_generation_;
    }
  }
  return ok;
}

void ScreenShared::ReleaseColourPre() {
  if (!passes_) {
    return;
  }
  for (HostImage& image : colour_pre_) {
    if (image.texture) {
      passes_->ReleaseImage(image);
      ++colour_generation_;
    }
  }
  pre_made_ = false;
}

void ScreenShared::CapturePre(const std::function<void()>& record) {
  // CHECK (not run): colour_pre is the bounce light's chain exactly as before (the AO's own pass body, called from here at the place it was recorded from): the picture, the pass timings ("ssgi
  // colour chain") and the image names are unchanged; the memory moves from the AO's line of the frame stats to the screen products'.
  record();
  pre_made_ = true;
}

// ---------------------------------------------------------------------------
// colour_post and its history
// ---------------------------------------------------------------------------

std::unique_ptr<plume::RenderPipeline> ScreenShared::MakeColourPipeline(uint32_t pass) {
  if (!colour_shaders_[pass]) {
    return nullptr;
  }
  // The host pass layout (a push block of 32 bytes), one RGBA16_FLOAT target, written whole: the full-screen triangle's pipeline.
  return passes_->CreatePipeline(*colour_shaders_[pass], kColourFormat, pass == kColourMip ? "screen colour post mip" : "screen colour post");
}

plume::RenderPipeline* ScreenShared::ColourPipeline(uint32_t pass) {
  if (colour_pipelines_[pass]) {
    return colour_pipelines_[pass].get();
  }
  if (colour_failed_ || !colour_shaders_[pass]) {
    return nullptr;
  }
  HostPrecache& precache = passes_->precache();
  const uint64_t key = HostPrecache::Key(HostPrecache::kScreenShared, 1 + pass);
  if (!colour_listed_) {
    colour_listed_ = true;
    std::vector<HostPrecache::Item> items;
    for (uint32_t i = 0; i < kColourPassCount; ++i) {
      items.push_back({HostPrecache::Key(HostPrecache::kScreenShared, 1 + i), i == kColourMip ? "screen colour post mip" : "screen colour post", [this, i] { return MakeColourPipeline(i); }});
    }
    const uint32_t count = precache.AddLive("screen products", std::move(items));
    REXLOG_INFO("{} colour_post is wanted: its pipelines ({}) are made in the background first, and the capture is made from the frame they are done", kPrefix, count);
  }
  if (!precache.Ready(key)) {
    return nullptr;  // a frame never waits for a compile: no capture this frame
  }
  colour_pipelines_[pass] = precache.Acquire(key, pass == kColourMip ? "screen colour post mip" : "screen colour post", [this, pass] { return MakeColourPipeline(pass); });
  if (!colour_pipelines_[pass]) {
    colour_failed_ = true;
    REXLOG_ERROR("{} the colour capture's pipeline could not be made: no colour_post this session", kPrefix);
  }
  return colour_pipelines_[pass].get();
}

bool ScreenShared::ColourPipelinesReady() {
  bool ready = true;
  for (uint32_t pass = 0; pass < kColourPassCount; ++pass) {
    ready = ColourPipeline(pass) != nullptr && ready;
  }
  return ready;
}

void ScreenShared::ReleaseColourPost() {
  for (HostImage& image : post_) {
    if (image.texture) {
      passes_->ReleaseImage(image);
    }
  }
  if (history_.texture) {
    passes_->ReleaseImage(history_);
  }
  post_images_ = 0;
  post_idle_frames_ = 0;
  last_capture_.valid = false;
  ++colour_generation_;
}

bool ScreenShared::CapturePost(const ResolveRecord& record, const ResolveCache::Summary& summary, plume::RenderCommandList* list) {
  // CHECK (not run): (1) colour_post is made at the HEIGHT rule's size (screen_rules.h ColourPostSize: the picture's up to 1080 lines, half above), not at the depth's traced size
  // (ScreenFrame::traced): the AO's trace is half size at its high quality at any height (ao_size.h legacy_low), and the car history, which reads colour_post pixel for pixel, needs the picture's
  // size at 1080 lines and below. The size is Image(kColourPost)->width / height. GB-I1 decides whether reflections want another (a field would be appended to the frozen frame then).
  // (2) The capture's bytes equal the car history pass's own colour at the picture's size (the run plan's A/B: mcla_gpu_car_selfrefl_source 1 against 2, the history image and the pictures of the
  // paint identical); it relies on the two shaders' float arithmetic being the same sequence on both drivers (additions in sample order, a division by the sample count, a power of two at 2, 4 and 8
  // samples; the unit test compares the C++ twin and the shaders' text, not the drivers). (3) The pause menu's 4x view and the host MSAA at 1, 2, 4 and 8.
  if (!list || !capture_wanted() || !IsTrigger(record, summary, anchor_seen_, composite_seen_, trigger_seen_)) {
    return false;
  }
  trigger_seen_ = true;
  EdramTargets::Target& depth = *summary.source;
  const uint32_t width = std::min(summary.dest_width, depth.width);
  const uint32_t height = std::min(summary.dest_height, depth.height);
  const uint32_t render_w = depth.scale.Edge(width);
  const uint32_t render_h = depth.scale.Edge(height);
  if (render_w < kMinRenderSize || render_h < kMinRenderSize) {
    return false;
  }
  // The main colour's identity beside the depth's, at the same samples (shafts.cpp RecordSky, car_selfrefl.cpp RecordHistory).
  EdramKey colour_key;
  colour_key.base_tiles = temporal::kMainColorBase;
  colour_key.pitch = temporal::kMainPitch;
  colour_key.depth = 0;
  colour_key.format = uint8_t(xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT);
  colour_key.samples = summary.source_key.samples;
  const EdramTargets::Target* found = edram_->Find(colour_key);
  bool unset = false;
  EdramTargets::Target* colour = found && found->texture ? edram_->Get(colour_key, found->height, found->scale, 0, list, &unset) : nullptr;
  if (!colour) {
    // A depth with no colour at its samples: the game's first frames (a loading scene's 1-sample depth before the game view's targets exist), not a failure while it is short (screen_rules.h JudgeColourPartner).
    ++colour_missing_run_;
    if (screen::JudgeColourPartner(false, colour_missing_run_) == screen::ColourPartner::kEarly) {
      if (!colour_missing_noted_) {
        colour_missing_noted_ = true;
        REXLOG_INFO("{} a trigger frame whose depth ({} host samples at {}x) has no main colour target {} yet: no colour_post in it (the first frames, before the game view's targets; an error if it lasts over {} trigger frames)",
                    kPrefix, depth.host_samples, depth.scale, DescribeEdramKey(colour_key), screen::kColourMissingGraceFrames);
      }
      return false;
    }
  } else {
    colour_missing_run_ = 0;
  }
  if (!colour || colour->host_samples != depth.host_samples || colour->scale != depth.scale) {
    if (!post_images_failed_logged_) {
      post_images_failed_logged_ = true;
      REXLOG_ERROR("{} the main view's colour target {} {} (depth: {} host samples at {}x): no colour_post (reported once)", kPrefix, DescribeEdramKey(colour_key),
                   !colour ? "does not exist" : "differs from the depth's samples or scale", depth.host_samples, depth.scale);
    }
    return false;
  }
  if (!ColourPipelinesReady()) {
    return false;
  }
  const bool ms = depth.host_samples > 1;
  plume::RenderPipeline* pipeline = ColourPipeline(ms ? kColourMs : kColourPlain);
  plume::RenderPipeline* mip_pipeline = ColourPipeline(kColourMip);
  if (!pipeline || !mip_pipeline) {
    return false;
  }
  // The size by the height rule (screen_rules.h ColourPostSize) and the chain's levels (the mips only while the reflections want them).
  const bool half = screen::ColourPostIsHalf(render_h);
  const uint32_t post_w = screen::ColourPostEdge(render_w, half);
  const uint32_t post_h = screen::ColourPostEdge(render_h, half);
  const uint32_t levels = screen::ColourPostLevelCount(post_w, post_h, screen::ColourPostWithMips(wants_, plan_.view, plan_.view_level));
  // The images: one a level, kept while the size is. A trade of level 0 with the history is not a new image (the pointer before this call is the one the trade left), a different size is.
  bool ok = true;
  for (uint32_t level = 0; level < levels && ok; ++level) {
    const plume::RenderTexture* before = post_[level].texture.get();
    ok = passes_->EnsureImage(post_[level], screen::HizLevelSize(post_w, level), screen::HizLevelSize(post_h, level), kColourFormat, kPostNames[level]);
    if (ok && post_[level].texture.get() != before) {
      ++colour_generation_;
    }
  }
  for (uint32_t level = levels; level < screen::kColourPostMaxLevels; ++level) {
    if (post_[level].texture) {
      passes_->ReleaseImage(post_[level]);
      ++colour_generation_;
    }
  }
  uint32_t held = 0;
  for (const HostImage& image : post_) {
    held += image.texture ? 1u : 0u;
  }
  post_images_ = held;
  if (!ok) {
    if (!post_images_failed_logged_) {
      post_images_failed_logged_ = true;
      REXLOG_ERROR("{} the colour_post images could not be made: no capture while that lasts (asked again every frame)", kPrefix);
    }
    return false;
  }
  uint32_t colour_slot = 0;
  uint32_t colour_stencil = 0;
  if (!edram_->SampleSlots(*colour, &colour_slot, &colour_stencil)) {
    return false;
  }
  // The colour plane is read as a texture (after the resolve it is already there: the call records a barrier only when it is elsewhere); the game's next draws move it back themselves.
  edram_->Transition(list, *colour, plume::RenderTextureLayout::SHADER_READ);
  // CHECK (not run): the capture's passes on both APIs with the Vulkan synchronisation validation on (the colour plane in the shader-read layout it was left in by the AO's apply, the new images'
  // first use: DrawInto initialises them), with the host MSAA at 1, 2, 4 and 8 and in the pause menu's 4x view; and the cost (expected: one full-screen pass reading every sample of the colour
  // plane, about the car history pass's own, plus the mips while the reflections want them).
  pass_timing_->Mark(list, "screen colour post");
  ColourPush push;
  push.slots[0] = colour_slot;
  push.slots[1] = depth.host_samples;
  push.slots[2] = half ? 2u : 1u;
  push.sizes[0] = post_w;
  push.sizes[1] = post_h;
  push.sizes[2] = render_w;
  push.sizes[3] = render_h;
  passes_->DrawInto(list, post_[0], pipeline, &push, sizeof(push));
  for (uint32_t level = 1; level < levels; ++level) {
    ColourPush mip;
    mip.slots[0] = post_[level - 1].slot;
    mip.slots[2] = 2u;
    mip.sizes[0] = post_[level].width;
    mip.sizes[1] = post_[level].height;
    mip.sizes[2] = post_[level - 1].width;
    mip.sizes[3] = post_[level - 1].height;
    passes_->DrawInto(list, post_[level], mip_pipeline, &mip, sizeof(mip));
  }
  post_done_ = true;
  ++post_captures_;
  const bool pause = temporal::MainDepthSamples(summary.source_key) == temporal::kPauseSamples;
  this_capture_.valid = true;
  this_capture_.frame = frame_count_;
  this_capture_.pause = pause;
  this_capture_.size[0] = post_w;
  this_capture_.size[1] = post_h;
  // The frame: what the AO's publication left, plus the capture (a frame the AO did not publish has the picture's numbers from here).
  screen::FrameInputs& in = inputs_;
  if (!published_) {
    in.render[0] = render_w;
    in.render[1] = render_h;
    in.host_samples = depth.host_samples;
    in.sample0 = summary.host_sample;
    in.flags = pause ? screen::kFramePause : 0u;
    in.camera.exposure_bias = screen::ExposureOf(exp_bias_, exposure_known_);
  }
  in.colour_post_levels = levels;
  for (uint32_t level = 0; level < screen::kColourPostMaxLevels; ++level) {
    in.colour_post[level] = level < levels ? post_[level].slot : screen::kNoSlot;
  }
  in.colour_post_size[0] = post_w;
  in.colour_post_size[1] = post_h;
  const bool keep = (plan_.make & screen::ProductBit(screen::kHistory)) != 0;
  const bool usable = keep && history_.texture && screen::HistoryUsable(last_capture_, this_capture_) && history_.width == post_w && history_.height == post_h;
  in.history_colour = usable ? history_.slot : screen::kNoSlot;
  in.colour_generation = colour_generation_;
  BuildFrame();
  Seal();
  if (!post_first_logged_) {
    post_first_logged_ = true;
    REXLOG_INFO("{} the first colour_post: {}x{} RGBA16F, {} level{} ({:.1f} MiB), from {} host sample{} of a {}x{} picture{}", kPrefix, post_w, post_h, levels, levels == 1 ? "" : "s",
                Mib(screen::ColourPostBytes(post_w, post_h, levels)), depth.host_samples, depth.host_samples == 1 ? "" : "s", render_w, render_h, half ? " (half size: above 1080 lines)" : "");
  }
  if (post_logged_size_[0] != post_w || post_logged_size_[1] != post_h) {
    if (post_logged_size_[0] != 0) {
      REXLOG_INFO("{} colour_post is {}x{} now ({} level{})", kPrefix, post_w, post_h, levels, levels == 1 ? "" : "s");
    }
    post_logged_size_[0] = post_w;
    post_logged_size_[1] = post_h;
  }
  return true;
}

// ---------------------------------------------------------------------------
// the debug view
// ---------------------------------------------------------------------------

plume::RenderPipeline* ScreenShared::ViewPipelineFor(plume::RenderFormat format, uint32_t samples) {
  for (ViewPipeline& v : view_pipelines_) {
    if (v.format == format && v.samples == samples) {
      return v.pipeline.get();
    }
  }
  if (view_failed_ || !view_shader_) {
    return nullptr;
  }
  ViewPipeline v;
  v.format = format;
  v.samples = samples;
  // Into the main view's colour with the target's sample count, the colour replaced (the pass writes one colour for every sample of a pixel).
  v.pipeline = passes_->CreatePipeline(passes_->layout(), *view_shader_, format, plume::RenderBlendDesc::Copy(), samples, "screen view");
  if (!v.pipeline) {
    view_failed_ = true;
    REXLOG_ERROR("{} the debug view's pipeline could not be made: no debug view this session", kPrefix);
    return nullptr;
  }
  plume::RenderPipeline* raw = v.pipeline.get();
  view_pipelines_.push_back(std::move(v));
  return raw;
}

bool ScreenShared::DrawView(const ResolveCache::Summary& summary, plume::RenderCommandList* list) {
  if (plan_.view == screen::View::kOff || !list || !summary.source) {
    return false;
  }
  const uint32_t needs = screen::ViewNeeds(plan_.view);
  if ((frame_.valid & needs) != needs) {
    return false;  // the product is not in this frame (no normal image, the Hi-Z's pipeline still being made, no capture): the picture stays the game's
  }
  // The main view's colour: the identity of the draws that wrote the depth, at the same samples and scale (ao.cpp RecordApply, gbuffer.cpp DrawView).
  EdramKey key;
  key.base_tiles = temporal::kMainColorBase;
  key.pitch = temporal::kMainPitch;
  key.depth = 0;
  key.format = uint8_t(xe::ColorRenderTargetFormat::k_2_10_10_10_FLOAT);
  key.samples = summary.source_key.samples;
  const EdramTargets::Target* found = edram_->Find(key);
  if (!found || !found->texture || found->host_samples != summary.source->host_samples || found->scale != summary.source->scale) {
    return false;
  }
  // CHECK (not run): the view's pipeline for each colour format and sample count the main view has (made on the render thread at the first use: a debug view), D3D12 and Vulkan.
  plume::RenderPipeline* pipeline = ViewPipelineFor(found->format, found->host_samples);
  if (!pipeline) {
    return false;
  }
  bool unset = false;
  EdramTargets::Target* colour = edram_->Get(key, found->height, found->scale, 0, list, &unset);
  if (!colour) {
    return false;
  }
  edram_->Transition(list, *colour, plume::RenderTextureLayout::COLOR_WRITE);
  EdramTargets::Target* only[1] = {colour};
  plume::RenderFramebuffer* framebuffer = edram_->Framebuffer(only, 1, nullptr);
  if (!framebuffer) {
    return false;
  }
  ViewPush push;
  push.slots[0] = frame_.lin_depth_slot[0];
  push.slots[1] = screen::kNoSlot;
  push.slots[2] = frame_.normal_slot;
  push.slots[3] = uint32_t(plan_.view);
  push.sizes[0] = frame_.render[0];
  push.sizes[1] = frame_.render[1];
  push.sizes[2] = frame_.traced[0];
  push.sizes[3] = frame_.traced[1];
  if (frame_.Has(screen::kHizMin)) {
    const uint32_t level = std::min(plan_.view_level, frame_.hiz_levels - 1);
    push.slots[1] = frame_.hiz_min_slot[level];
    push.level[0] = level;
    push.level[1] = screen::HizLevelSize(frame_.traced[0], level);
    push.level[2] = screen::HizLevelSize(frame_.traced[1], level);
  }
  if (plan_.view == screen::View::kColourPost || plan_.view == screen::View::kHistory) {
    // The colour views: the slot of the level (or of the history) in the Hi-Z's place, with its own size.
    const bool history = plan_.view == screen::View::kHistory;
    const uint32_t level = history ? 0u : std::min(plan_.view_level, frame_.colour_post_levels - 1);
    const HostImage* image = history ? Image(screen::kHistory) : Image(screen::kColourPost, level);
    if (!image) {
      return false;
    }
    push.slots[1] = image->slot;
    push.level[0] = level;
    push.level[1] = image->width;
    push.level[2] = image->height;
  }
  pass_timing_->Mark(list, "screen view");
  list->setFramebuffer(framebuffer);
  const plume::RenderViewport viewport{0.0f, 0.0f, float(frame_.render[0]), float(frame_.render[1])};
  list->setViewports(&viewport, 1);
  const plume::RenderRect scissor(0, 0, int32_t(frame_.render[0]), int32_t(frame_.render[1]));
  list->setScissors(&scissor, 1);
  passes_->Bind(list);
  list->setPipeline(pipeline);
  list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  list->drawInstanced(3, 1, 0, 0);
  list->setFramebuffer(nullptr);
  return true;
}

}  // namespace mcla::gpu
