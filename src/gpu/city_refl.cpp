// MCLA GPU renderer -- the city in the car paint's reflections: the host side. See city_refl.h.

#include "city_refl.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "device.h"
#include "gpu_class.h"
#include "live_text.h"
#include "rt_scene_rules.h"
#include "rt_scene_service.h"
#include "rt_tier.h"

namespace mcla::gpu {

namespace cr = city_refl;

namespace {

constexpr const char* kPrefix = "[city refl]";
// The calibration's window: this many stats frames (one in twenty frames) between two moves of the gains, so that a class has the samples it needs (rt_hit_calibration.h Params::min_samples).
constexpr uint32_t kCloseEveryStats = 4;

LiveText& Setting() {
  static LiveText text("mcla_gpu_rt_reflections");
  return text;
}

}  // namespace

CityRefl::~CityRefl() = default;

bool CityRefl::Initialize(Device& device, FrameRing& frames) {
  device_ = &device;
  frames_ = &frames;
  ready_ = false;
  // t0 the rows (a byte address buffer the host writes every frame), u1 the counters: distinct register numbers in one set (as gi_field's), space 3 of the pass's layout.
  set_builder_.begin();
  set_builder_.addByteAddressBuffer(0);
  set_builder_.addReadWriteByteAddressBuffer(1);
  set_builder_.end();
  stats_ = device.CreateBuffer(plume::RenderBufferDesc::DefaultBuffer(cr::kStatsBytes, plume::RenderBufferFlag::STORAGE | plume::RenderBufferFlag::UNORDERED_ACCESS), "city refl stats");
  stats_zero_ = device.CreateBuffer(plume::RenderBufferDesc::UploadBuffer(cr::kStatsBytes), "city refl stats zero");
  stats_readback_ = device.CreateBuffer(plume::RenderBufferDesc::ReadbackBuffer(cr::kStatsBytes), "city refl stats readback");
  if (!stats_ || !stats_zero_ || !stats_readback_) {
    REXLOG_ERROR("{} the counters' buffers could not be made: no city in the reflections", kPrefix);
    stats_.reset();
    stats_zero_.reset();
    stats_readback_.reset();
    return false;
  }
  const plume::RenderRange no_read(0, 0);
  void* mapped = stats_zero_->map(0, &no_read);
  if (!mapped) {
    REXLOG_ERROR("{} the counters' zero buffer could not be mapped: no city in the reflections", kPrefix);
    return false;
  }
  std::memset(mapped, 0, cr::kStatsBytes);
  stats_zero_->unmap(0, nullptr);
  ready_ = true;
  return true;
}

void CityRefl::Shutdown() {
  // The GPU is idle and the ring drained (CarRtRefl::Shutdown): the sets and buffers go.
  for (Slot& slot : slots_) {
    slot.set.reset();
    slot.rows.reset();
  }
  stats_.reset();
  stats_zero_.reset();
  stats_readback_.reset();
  stats_busy_ = false;
  ++stats_generation_;
  service_ = nullptr;
  device_ = nullptr;
  frames_ = nullptr;
  ready_ = false;
  pass_active_ = false;
  cell_ = cr::Cell{};
}

// ---------------------------------------------------------------------------
// the setting and the wants
// ---------------------------------------------------------------------------

void CityRefl::BeginFrame(bool pass_active) {
  pass_active_ = false;
  cell_ = cr::Cell{};
  if (!ready_ || !pass_active) {
    return;
  }
  const cr::Level level = cr::LevelFor(gpu_class::Current(), gpu_class::CurrentRtTier(), Setting().Get());
  const cr::Cell cell = cr::WithReach(cr::CellFor(level, gpu_class::CurrentRtClass()), float(larecomp::settings::mcla_gpu_city_refl_reach.Get()));
  if (cell.level == cr::Level::kOff) {
    return;
  }
  cell_ = cell;
  cell_.coat_layer = cell.coat_layer && larecomp::settings::mcla_gpu_city_refl_coat.Get();
  pass_active_ = true;
  if (service_ != nullptr) {
    // The structures, the hit data and every texture at the level, and the cars (the pass is the car's): the scene service merges it with the other consumers' wants.
    const cr::Wants want = cr::WantsFor(cell_);
    rt_scene::Needs needs;
    needs.cars = want.cars;
    needs.city = want.city;
    needs.hit = want.hit;
    needs.textures = want.textures;
    service_->Want(rt_scene::Consumer::kCityReflections, needs);
  }
  if (!wanted_logged_) {
    wanted_logged_ = true;
    REXLOG_INFO("{} the city is in the car paint's reflections: level {} (mcla_gpu_rt_reflections = {}), reach {:.0f} m with the weight full to {:.0f}%, {} cone rays at most, the opponents' paint {} texels at {} rays at most, "
                "the coat layer {}; the cars' parts of the reflection ray and the city's share one top level (the scene service)",
                kPrefix, std::string(city_rt::LevelName(cell_.level)), Setting().Get(), double(cell_.reach), 100.0 * double(cell_.fade_from), cell_.cone_rays, static_cast<unsigned long long>(cell_.opp_texels),
                cell_.opp_rays, cell_.coat_layer ? "on (a city hit is a mirror's, as another car's)" : "off (the soft cone)");
  }
}

uint32_t CityRefl::ScreenProducts() const {
  if (!pass_active_ || cell_.level == cr::Level::kOff) {
    return 0u;
  }
  return screen::ProductBit(screen::kColourPost) | screen::ProductBit(screen::kLinDepth);
}

// ---------------------------------------------------------------------------
// the frame's rows
// ---------------------------------------------------------------------------

bool CityRefl::EnsureSlot(uint32_t frame_slot) {
  if (!ready_ || frame_slot >= kFramesInFlight) {
    return false;
  }
  Slot& s = slots_[frame_slot];
  if (!s.rows) {
    s.rows = device_->CreateBuffer(plume::RenderBufferDesc::UploadBuffer(cr::kBufferBytes, plume::RenderBufferFlag::STORAGE), "city refl rows");
    if (!s.rows) {
      return false;
    }
  }
  if (!s.set) {
    s.set = device_->CreateDescriptorSet(set_builder_, 2, 0, "city refl set");  // two views: the rows and the counters (D3D12 sizes the set by this count)
    if (!s.set) {
      return false;
    }
    s.set->setBuffer(0, s.rows.get(), cr::kBufferBytes);
    s.set->setBuffer(1, stats_.get(), cr::kStatsBytes);
  }
  return true;
}

CityRefl::Frame CityRefl::Prepare(uint32_t frame_slot, const ScreenIn& screen, uint64_t frame, bool stats_frame) {
  Frame out;
  if (!EnsureSlot(frame_slot)) {
    if (!slot_failed_logged_) {
      slot_failed_logged_ = true;
      REXLOG_ERROR("{} the frame slot's rows or set could not be made: no ray traced reflection this session", kPrefix);
    }
    return out;
  }
  Slot& slot = slots_[frame_slot];
  out.set = slot.set.get();
  const bool on = this->on();
  cr::RowsIn in;
  in.level = on ? cell_.level : cr::Level::kOff;
  in.cell = cell_;
  in.stats = on && stats_frame && !stats_busy_ && stats_ && stats_readback_;
  in.serial = uint32_t(frame & 0xFFFFFFFFu);
  in.screen = on && screen.valid && larecomp::settings::mcla_gpu_city_refl_screen.Get();
  if (in.screen) {
    in.depth_slot = screen.depth_slot;
    in.depth_width = screen.depth_width;
    in.depth_height = screen.depth_height;
    in.colour_levels = std::min(screen.colour_levels, cr::kMaxColourLevels);
    in.colour_width = screen.colour_width;
    in.colour_height = screen.colour_height;
    for (uint32_t i = 0; i < cr::kMaxColourLevels; ++i) {
      in.colour_slots[i] = screen.colour_slots[i];
    }
  }
  if (in.screen && !screen_logged_) {
    screen_logged_ = true;
    REXLOG_INFO("{} the picture's products reach the pass: lin_depth {}x{} (slot {}), colour_post {}x{} with {} levels; a hit on the screen takes the picture's colour at the mip of its footprint (frame {})", kPrefix,
                in.depth_width, in.depth_height, in.depth_slot, in.colour_width, in.colour_height, in.colour_levels, static_cast<unsigned long long>(frame));
  } else if (on && !in.screen && !no_screen_logged_ && frame > 600) {
    no_screen_logged_ = true;
    REXLOG_INFO("{} no picture products at the trigger of frame {} (the screen owner has not made colour_post and lin_depth for the car pass, or mcla_gpu_city_refl_screen is off): city hits are shaded by the model alone", kPrefix,
                static_cast<unsigned long long>(frame));
  }
  in.sun_tan = cr::kSunTan;
  in.wet = 0.0f;
  in.view = std::clamp<int32_t>(int32_t(larecomp::settings::mcla_gpu_city_refl_view.Get()), 0, 2);
  const bool calibrating = larecomp::settings::mcla_gpu_car_rt_hit_calibrate.Get();
  for (uint32_t c = 0; c < rt_hit_cal::kClasses; ++c) {
    in.gains[c] = calibrating ? gains_.Gain(c) : 1.0f;
  }
  uint32_t rows[cr::kRows][cr::kRowWords];
  cr::MakeRows(in, rows);
  void* mapped = slot.rows->map();
  if (!mapped) {
    return Frame{};
  }
  std::memcpy(mapped, rows, sizeof(rows));
  slot.rows->unmap();
  out.on = on;
  out.cell = cell_;
  out.screen = in.screen;
  out.stats = in.stats;
  if (on) {
    last_level_ = cell_.level;
    last_reach_ = cell_.reach;
  }
  return out;
}

void CityRefl::BarrierScreen(plume::RenderCommandList* list, const ScreenIn& screen) {
  if (!list || !screen.valid) {
    return;
  }
  plume::RenderTextureBarrier barriers[1 + cr::kMaxColourLevels];
  uint32_t count = 0;
  if (screen.depth) {
    barriers[count++] = plume::RenderTextureBarrier(const_cast<plume::RenderTexture*>(screen.depth), plume::RenderTextureLayout::SHADER_READ);
  }
  for (uint32_t i = 0; i < std::min(screen.colour_levels, cr::kMaxColourLevels); ++i) {
    if (screen.colour[i]) {
      barriers[count++] = plume::RenderTextureBarrier(const_cast<plume::RenderTexture*>(screen.colour[i]), plume::RenderTextureLayout::SHADER_READ);
    }
  }
  if (count != 0) {
    list->barriers(plume::RenderBarrierStage::GRAPHICS_AND_COMPUTE, nullptr, 0, barriers, count);
  }
}

// ---------------------------------------------------------------------------
// the counters
// ---------------------------------------------------------------------------

void CityRefl::StatsBegin(plume::RenderCommandList* list) {
  if (!list || !stats_ || !stats_zero_ || stats_busy_) {
    return;
  }
  list->barriers(plume::RenderBarrierStage::COPY, plume::RenderBufferBarrier(stats_.get(), plume::RenderBufferAccess::WRITE));
  list->copyBufferRegion(plume::RenderBufferReference(stats_.get(), 0), plume::RenderBufferReference(stats_zero_.get(), 0), cr::kStatsBytes);
  list->barriers(plume::RenderBarrierStage::COMPUTE, plume::RenderBufferBarrier(stats_.get(), plume::RenderBufferAccess::READ | plume::RenderBufferAccess::WRITE));
}

void CityRefl::StatsEnd(plume::RenderCommandList* list, uint64_t frame) {
  if (!list || !stats_ || !stats_readback_ || stats_busy_) {
    return;
  }
  list->barriers(plume::RenderBarrierStage::COPY, plume::RenderBufferBarrier(stats_.get(), plume::RenderBufferAccess::READ));
  list->copyBufferRegion(plume::RenderBufferReference(stats_readback_.get(), 0), plume::RenderBufferReference(stats_.get(), 0), cr::kStatsBytes);
  list->barriers(plume::RenderBarrierStage::COMPUTE, plume::RenderBufferBarrier(stats_.get(), plume::RenderBufferAccess::READ | plume::RenderBufferAccess::WRITE));
  stats_busy_ = true;
  if (frames_) {
    frames_->RetireAction([this, frame, generation = stats_generation_] { ProcessStats(frame, generation); });
  }
}

// A stats frame's counters, read back once the GPU is done with them: into the window, the calibration (the gains move every kCloseEveryStats frames), and the census lines.
void CityRefl::ProcessStats(uint64_t frame, uint64_t generation) {
  stats_busy_ = false;
  if (generation != stats_generation_ || !stats_readback_) {
    return;
  }
  const void* mapped = stats_readback_->map();
  if (!mapped) {
    REXLOG_ERROR("{} the census of frame {}: the readback could not be mapped", kPrefix, frame);
    return;
  }
  uint32_t words[cr::kStatsWords];
  std::memcpy(words, mapped, sizeof(words));
  stats_readback_->unmap();
  window_.Add(words);
  gains_.Add(words);  // the first kCalWords words: three a class
  if (++stats_since_close_ >= kCloseEveryStats) {
    stats_since_close_ = 0;
    if (larecomp::settings::mcla_gpu_car_rt_hit_calibrate.Get()) {
      gains_.Close();
    } else {
      gains_ = rt_hit_cal::ClassGains{};
    }
  }
  if (!logged_ever_ || frame >= last_log_frame_ + cr::kLogEveryFrames) {
    logged_ever_ = true;
    last_log_frame_ = frame;
    REXLOG_INFO("{}", cr::CensusText(frame, last_level_, last_reach_, window_));
    REXLOG_INFO("{}", cr::GainText(frame, gains_, window_.samples));
    window_ = cr::Totals{};
  }
}

}  // namespace mcla::gpu
