// MCLA GPU renderer -- the ray tracing world frame. See rt_world_frame.h (the specification is rt_world_frame_rules.h).

#include "rt_world_frame.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <utility>

#include <rex/logging/log_category.h>

#include "ao.h"
#include "car_frame.h"
#include "car_rt_instances_rules.h"
#include "city_rt_scene.h"
#include "device.h"
#include "mc_engine/cinematic/capture_mode.h"
#include "rt_scene_service.h"
#include "screen_shared.h"
#include "settings/larecomp_settings.h"
#include "vol_lamp_list.h"
#include "world_clock.h"

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[rt world]";
namespace ch = larecomp::citypack;
constexpr std::chrono::milliseconds kMaxListAge{500};  // as the light manager's: the hook copies the list every night frame
constexpr uint64_t kNoFrame = ~0ull;
constexpr uint64_t kLogEveryFrames = 1800;

uint64_t AlignUp(uint64_t value, uint64_t alignment) { return (value + alignment - 1) / alignment * alignment; }

}  // namespace

RtWorldFrame::Bindings RtWorldFrame::AddBindings(plume::RenderDescriptorSetBuilder& builder, uint32_t world_register, uint32_t lamps_register) {
  Bindings out;
  out.world = builder.addStructuredBuffer(world_register);
  out.lamps = builder.addStructuredBuffer(lamps_register);
  return out;
}

RtWorldFrame::~RtWorldFrame() = default;

bool RtWorldFrame::Initialize(Device& device, FrameRing& frames, const Sources& sources) {
  device_ = &device;
  frames_ = &frames;
  src_ = sources;
  gains_ = rt_hit_cal::ClassGains{};
  frame_ = 0;
  filled_frame_ = kNoFrame;
  view_frame_ = kNoFrame;
  last_view_frame_ = kNoFrame;
  for (Slot& s : slots_) {
    s.filled_frame = kNoFrame;
  }
  return true;
}

void RtWorldFrame::Shutdown() {
  Wanted(false);
  for (Slot& s : slots_) {
    if (s.world && s.world_cpu) s.world->unmap(0, nullptr);
    if (s.lamps && s.lamps_cpu) s.lamps->unmap(0, nullptr);
    s = Slot{};
  }
  table_.reset();
  table_given_.reset();
  live_.clear();
  device_ = nullptr;
  frames_ = nullptr;
  src_ = Sources{};
}

void RtWorldFrame::Wanted(bool on) {
  if (on == hooks_wanted_) {
    return;
  }
  hooks_wanted_ = on;
  if (src_.ao != nullptr) {
    src_.ao->SetWorldLightWanted(on);
  }
  vol_lamps::SetWantedBy(vol_lamps::kWantedByWorldFrame, on);
  if (!on) {
    live_.clear();
    live_seq_ = 0;
    live_fresh_ = false;
  }
}

std::unique_ptr<plume::RenderBuffer> RtWorldFrame::MakeUpload(uint64_t bytes, uint8_t** cpu, const char* name) {
  std::unique_ptr<plume::RenderBuffer> buffer = device_->CreateBuffer(plume::RenderBufferDesc::UploadBuffer(bytes, plume::RenderBufferFlag::STORAGE), name);
  if (!buffer) {
    return nullptr;
  }
  const plume::RenderRange no_read(0, 0);
  uint8_t* mapped = static_cast<uint8_t*>(buffer->map(0, &no_read));
  if (!mapped) {
    return nullptr;
  }
  std::memset(mapped, 0, size_t(bytes));
  *cpu = mapped;
  return buffer;
}

bool RtWorldFrame::EnsureSlot(Slot& slot, uint32_t index) {
  if (!device_) {
    return false;
  }
  if (!slot.world) {
    uint8_t* cpu = nullptr;
    std::unique_ptr<plume::RenderBuffer> buffer = MakeUpload(AlignUp(rt_world::kWorldBytes, 256), &cpu, std::format("rt world rows {}", index).c_str());
    if (!buffer) {
      return false;
    }
    slot.world = std::move(buffer);
    slot.world_cpu = cpu;
  }
  if (!slot.lamps) {
    uint8_t* cpu = nullptr;
    std::unique_ptr<plume::RenderBuffer> buffer = MakeUpload(AlignUp(rt_world::kLampBufferBytes, 256), &cpu, std::format("rt world lamps {}", index).c_str());
    if (!buffer) {
      return false;
    }
    slot.lamps = std::move(buffer);
    slot.lamps_cpu = cpu;
  }
  return true;
}

uint64_t RtWorldFrame::HeldBytes() const {
  uint64_t bytes = 0;
  for (const Slot& s : slots_) {
    bytes += (s.world ? AlignUp(rt_world::kWorldBytes, 256) : 0) + (s.lamps ? AlignUp(rt_world::kLampBufferBytes, 256) : 0);
  }
  return bytes;
}

void RtWorldFrame::BeginFrame(uint64_t frame) {
  // The frame that ended: nobody built a top level in it although something wanted the scene (the car scene could not, or the camera was lost): the census counts it.
  if (device_ != nullptr && frame_ != 0 && view_frame_ != frame_ && src_.service != nullptr && src_.service->Needs().any()) {
    ++census_.frames_no_view;
  }
  frame_ = frame;
  cascades_ = rt_hit::SunCascades{};
  car_ = false;
  // Nothing has had a top level for a while: the draw hooks and the lamp list are not asked for any more (they were for the light and the wetness, which cost the AO's draw hook a little).
  if (hooks_wanted_ && last_view_frame_ != kNoFrame && frame_ > last_view_frame_ + kIdleReleaseFrames) {
    Wanted(false);
  }
}

void RtWorldFrame::NoteView(const temporal::Mat4& view, bool flip_z, bool car) {
  view_ = view;
  flip_z_ = flip_z;
  car_ = car;
  view_frame_ = frame_;
  last_view_frame_ = frame_;
  Wanted(true);
}

void RtWorldFrame::Bind(plume::RenderDescriptorSet* set, const Bindings& b, uint32_t slot_index) {
  if (!set || !device_ || slot_index >= kFramesInFlight) {
    return;
  }
  Slot& s = slots_[slot_index];
  if (!EnsureSlot(s, slot_index)) {
    return;
  }
  if (s.filled_frame != frame_) {
    // Not filled in this frame (yet: the fill follows the top level's build in the same frame, and writes over this): a consumer that reads before it, or in a frame where it never comes, finds
    // rows that say they are not valid (flags 0) and a window with no lamp, not the frame the slot held two frames ago.
    std::memset(s.world_cpu, 0, rt_world::kWorldBytes);
    std::memset(s.lamps_cpu, 0, 16);
  }
  const plume::RenderBufferStructuredView rows(16, 0);
  set->setBuffer(b.world, s.world.get(), rt_world::kWorldBytes, &rows);
  set->setBuffer(b.lamps, s.lamps.get(), rt_world::kLampBufferBytes, &rows);
}

void RtWorldFrame::SetStaticLamps(std::shared_ptr<const ch::LampTable> table) {
  table_given_ = std::move(table);
}

void RtWorldFrame::LoadStaticLamps() {
  const ch::PackReader* pack = src_.city != nullptr ? src_.city->pack_if_open() : nullptr;
  if (pack == nullptr) {
    pack_was_open_ = false;
    return;
  }
  if (!pack_was_open_) {
    pack_was_open_ = true;
    table_tried_ = false;  // a pack that was given up and opened again may be another build's
  }
  if (table_tried_) {
    return;
  }
  table_tried_ = true;
  table_.reset();
  if (!pack->has_lamps()) {
    REXLOG_WARN("{} frame {}: the open city pack has no lamps section (a pack of builder version 1, or lights.xlm was not readable when it was built): the lamp term has the game's live list only", kPrefix,
                frame_);
    return;
  }
  ch::LampTable table;
  std::string error;
  if (!pack->ReadLamps(&table, &error)) {
    REXLOG_WARN("{} frame {}: the city pack's lamps section could not be read ({}): the lamp term has the game's live list only", kPrefix, frame_, error);
    return;
  }
  const ch::LampStats stats = table.Stats();
  REXLOG_INFO("{} frame {}: the static lamp table is read from the pack: {} lights ({} spots, {} omni), the grid {} x {} cells of {} m, the lamp hours {} and {}", kPrefix, frame_, stats.count, stats.spots,
              stats.omni, table.grid().nx, table.grid().nz, table.grid().cell_m, table.hours().on_hour, table.hours().off_hour);
  table_ = std::make_shared<const ch::LampTable>(std::move(table));
}

void RtWorldFrame::RefreshLiveList() {
  std::vector<vol_lamps::GameLamp> list;
  bool stale = false;
  const bool changed = vol_lamps::Global().Latest(&list, &live_seq_, &stale, kMaxListAge);
  if (stale) {
    live_.clear();
    live_seq_ = 0;
    live_fresh_ = false;
    return;
  }
  live_fresh_ = true;
  if (changed) {
    live_.clear();
    live_.reserve(list.size());
    for (const vol_lamps::GameLamp& g : list) {
      if (vol_lamps::Plausible(g)) {
        live_.push_back(rt_world::LiveLampOf(g));
      }
    }
  }
}

bool RtWorldFrame::OnTrigger(uint32_t slot_index) {
  if (!device_ || slot_index >= kFramesInFlight) {
    return false;
  }
  if (view_frame_ != frame_) {
    return false;  // no top level has been built in this frame (yet: the scene calls this at every resolve, the builder's trigger is one of them): nothing can trace, nothing is made
  }
  if (filled_frame_ == frame_) {
    return true;  // a resolve after the trigger
  }
  Slot& s = slots_[slot_index];
  if (!EnsureSlot(s, slot_index)) {
    return false;
  }
  LoadStaticLamps();
  RefreshLiveList();

  namespace ci = car_rt_instances;
  rt_world::WorldInput in;
  in.frame = frame_;
  rt_hit::FrameInput& hit = in.hit;
  hit.tuning.enabled = true;
  hit.tuning.shadow = larecomp::settings::mcla_gpu_car_rt_hit_shadow.Get();
  hit.tuning.cascade = larecomp::settings::mcla_gpu_car_rt_hit_cascade.Get();
  hit.tuning.ambient = float(larecomp::settings::mcla_gpu_car_rt_hit_ambient.Get());
  hit.tuning.spec = float(larecomp::settings::mcla_gpu_car_rt_hit_spec.Get());
  hit.tuning.gain = 1.0f;  // the city's gains are per class (the rows' gains), not this one
  hit.shadow_reach = rt_hit::kSunReach;
  hit.cascades = cascades_;

  // the camera and the world's axes in the top level's view space
  double camera[3] = {};
  if (ci::CameraWorld(view_, camera)) {
    in.camera_known = true;
    hit.camera_known = true;
    for (int c = 0; c < 3; ++c) {
      in.camera[c] = float(camera[c]);
      hit.camera_world[c] = float(camera[c]);
    }
  }
  for (int axis = 0; axis < 3; ++axis) {
    const double unit[3] = {axis == 0 ? 1.0 : 0.0, axis == 1 ? 1.0 : 0.0, axis == 2 ? 1.0 : 0.0};
    ci::ViewVector(view_, flip_z_, unit, hit.world_axes_view[axis]);  // false: the identity stays
  }

  // the light: the world's (the commonest light of the main view's draws), else the car frame's
  float ambient[3] = {std::nanf(""), 0.0f, 0.0f}, sun[3] = {std::nanf(""), 0.0f, 0.0f};
  double to_sun[3] = {};
  const bool sun_direction = src_.ao != nullptr && src_.ao->FrameLight(ambient, sun, to_sun);  // false: no light, or no direction; the colours are written only when there was a light
  const bool world_light = src_.ao != nullptr && !std::isnan(sun[0]);
  const car_rt_material::FrameLight* car_light = nullptr;
  if (src_.cars != nullptr && src_.cars->Current().light.valid) {
    car_light = &src_.cars->Current().light;
  }
  double direction[3] = {};
  bool direction_known = false;
  if (world_light) {
    hit.light.valid = true;
    for (int c = 0; c < 3; ++c) {
      hit.light.sun[c] = std::max(sun[c], 0.0f);
      hit.light.ambient[c] = std::max(ambient[c], 0.0f);
    }
    if (sun_direction) {
      std::memcpy(direction, to_sun, sizeof(direction));
      direction_known = true;
    }
    in.world_light = true;
  } else if (car_light != nullptr) {
    hit.light.valid = true;
    for (int c = 0; c < 3; ++c) {
      hit.light.sun[c] = car_light->sun[c];
      hit.light.ambient[c] = car_light->ambient[c];
    }
    if (src_.cars != nullptr) {
      std::memcpy(direction, src_.cars->Current().to_sun, sizeof(direction));
      direction_known = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]) > 0.5;
    }
    ++census_.frames_light_car;
  }
  if (direction_known) {
    const double length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
    in.sun_world_known = std::isfinite(length) && length > 0.5;
    for (int c = 0; c < 3; ++c) {
      in.to_sun_world[c] = in.sun_world_known ? float(direction[c] / length) : 0.0f;
    }
    hit.sun_known = in.sun_world_known && ci::ViewVector(view_, flip_z_, direction, hit.to_sun_view);
  }
  // the sky and the environment map: from the cars' draws when a car frame had them, kept a while
  if (car_light != nullptr) {
    if (car_light->sky[0] > 0.0f || car_light->sky[1] > 0.0f || car_light->sky[2] > 0.0f) {
      for (int c = 0; c < 3; ++c) sky_[c] = car_light->sky[c];
      sky_known_ = true;
    }
    if (car_light->env_sharp != 0) {
      env_sharp_ = car_light->env_sharp;
      env_blur_ = car_light->env_blur;
      env_frame_ = frame_;
    }
  }
  for (int c = 0; c < 3; ++c) {
    hit.light.sky[c] = sky_known_ ? sky_[c] : hit.light.ambient[c];
  }
  if (env_sharp_ != 0 && frame_ >= env_frame_ && frame_ - env_frame_ <= kEnvStaleFrames) {
    hit.light.env_sharp = env_sharp_;
    hit.light.env_blur = env_blur_;
  } else {
    hit.light.env_sharp = 0;
    hit.light.env_blur = 0;
  }

  // the clock, the weather, the exposure, the wetness, the kind of world
  const float hour = world_clock::Hour();
  in.hour = hour;
  in.weather = world_clock::Weather() >= 0 ? uint32_t(world_clock::Weather()) : rt_world::kUnknownWeather;
  in.units = rt_world::UnitsOf(src_.screen != nullptr ? src_.screen->ExposureFactor() : 1.0f);
  float wet_raw = 0.0f;
  in.wet = src_.ao != nullptr && src_.ao->FrameWetness(&wet_raw) ? rt_world::WetOf(wet_raw) : 0.0f;
  in.time_s = float(std::fmod(mc::cinematic::SceneClockNowMs(frame_) * 1.0e-3, 4096.0));
  in.world_kind = src_.ao != nullptr ? uint32_t(src_.ao->FrameWorld()) : 0u;
  in.car = car_;
  in.live_fresh = live_fresh_;

  // the calibration: a window closes every kWindowFrames frames, one frame in kStatsPeriod counts
  if (frame_ >= window_start_ + rt_world::kWindowFrames) {
    window_start_ = frame_;
    census_.classes_moved += gains_.Close();
    ++census_.windows_closed;
  }
  in.stats_frame = rt_world::StatsFrameDue(frame_);
  in.calibration_windows = gains_.windows;
  gains_.Table(in.gains);

  // the lamps
  const std::shared_ptr<const ch::LampTable>& table = table_given_ ? table_given_ : table_;
  const ch::LampHours default_hours;
  const ch::LampHours& hours = table ? table->hours() : default_hours;
  const float night_hint = hit.light.valid ? rt_hit::NightFactor(hit.light.sun) : -1.0f;
  schedule_ = rt_world::ScheduleAt(hours, hour, night_hint);
  // R7: the game's live lamp list is the cross-check of the hour (and of the weather the hour does not know: the list is made with the live on hours): lamps that shine in a fresh list are on, an
  // empty fresh list next to a table that has street lamps within its reach says they are off (rt_world_frame_rules.h ApplyLiveEvidence)
  rt_world::LiveEvidence evidence = rt_world::EvidenceOf(live_fresh_, live_, 0);
  if (evidence.fresh && (evidence.shining > 0 || schedule_.hour_street > 0.0f) && table) {
    const float eye[3] = {float(camera[0]), float(camera[1]), float(camera[2])};
    std::vector<uint32_t> near_table;
    table->NearSphere(eye, rt_world::kLiveListReach, &near_table);
    for (const uint32_t index : near_table) {
      evidence.table_streets += (*table)[index].source == ch::LampSource::kStreetLamp ? 1u : 0u;
    }
  }
  schedule_ = rt_world::ApplyLiveEvidence(schedule_, evidence);
  in.schedule = schedule_;
  in.live_shining = evidence.shining;
  rt_world::LampInput li;
  li.table = table.get();
  li.live = &live_;
  li.live_fresh = live_fresh_;
  for (int c = 0; c < 3; ++c) {
    li.camera[c] = camera[c];
    for (int k = 0; k < 3; ++k) li.axes[k][c] = hit.world_axes_view[k][c];
  }
  li.hour = hour;
  li.schedule = schedule_;
  const rt_scene::Level level = src_.service != nullptr ? src_.service->Needs().city : rt_scene::Level::kOff;
  li.budget = rt_world::BudgetFor(level == rt_scene::Level::kUltra ? rt_world::Tier::kUltra : (level == rt_scene::Level::kHigh ? rt_world::Tier::kHigh : rt_world::Tier::kMid));
  rt_world::BuildLampFrame(li, &lamp_frame_);
  in.lamps = &lamp_frame_;

  // the words, into the slot's buffers
  uint32_t(*rows)[4] = reinterpret_cast<uint32_t(*)[4]>(rows_.data());
  rt_world::MakeWorldRows(in, rows);
  std::memcpy(s.world_cpu, rows_.data(), rt_world::kWorldBytes);
  const size_t lamp_bytes = std::min<size_t>(lamp_frame_.words.size() * sizeof(uint32_t), rt_world::kLampBufferBytes);
  std::memcpy(s.lamps_cpu, lamp_frame_.words.data(), lamp_bytes);
  s.filled_frame = frame_;
  filled_frame_ = frame_;
  decoded_ = rt_world::DecodeWorldRows(rows);

  // the census
  ++census_.frames;
  census_.frames_no_light += hit.light.valid ? 0u : 1u;
  census_.frames_hour += schedule_.from_hour ? 1u : 0u;
  census_.frames_live += live_fresh_ ? 1u : 0u;
  census_.frames_night += schedule_.factor > 0.0f ? 1u : 0u;
  census_.frames_evidence_on += (schedule_.evidence & rt_world::kEvidenceOn) != 0 ? 1u : 0u;
  census_.frames_evidence_off += (schedule_.evidence & rt_world::kEvidenceOff) != 0 ? 1u : 0u;
  census_.frames_cascade += (rows_[3] & rt_hit::kFrameCascade) != 0u ? 1u : 0u;
  census_.frames_env += hit.light.env_sharp != 0u ? 1u : 0u;
  census_.lamps_sum += lamp_frame_.lamp_count;
  census_.lamps_max = std::max(census_.lamps_max, lamp_frame_.lamp_count);
  census_.table_sum += lamp_frame_.stats.from_table;
  census_.live_sum += lamp_frame_.stats.from_live;
  census_.over_budget_sum += lamp_frame_.stats.over_budget;
  census_.outside_sum += lamp_frame_.stats.outside_window;
  census_.inactive_sum += lamp_frame_.stats.inactive;
  census_.bytes_sum += rt_world::kWorldBytes + lamp_bytes;
  Log(in, lamp_frame_, li.budget.radius_m);
  return true;
}

RtWorldFrame::Current RtWorldFrame::current() const {
  Current c;
  if (filled_frame_ == frame_ && frame_ != kNoFrame) {
    c.rows = rows_.data();
    c.lamp_words = lamp_frame_.words.data();
    c.lamp_rows = lamp_frame_.rows();
    c.decoded = decoded_;
    c.lamps = &lamp_frame_;
  }
  return c;
}

void RtWorldFrame::Log(const rt_world::WorldInput& in, const rt_world::LampFrame& lamps, float radius_m) {
  const bool first = !logged_first_;
  if (!first && (last_log_frame_ == 0 || frame_ < last_log_frame_ + kLogEveryFrames)) {
    return;
  }
  logged_first_ = true;
  last_log_frame_ = frame_;
  const rt_hit::FrameLight& l = in.hit.light;
  REXLOG_INFO("{} frame {}: the world frame{}: light {} (sun {:.3f} {:.3f} {:.3f}, ambient {:.3f} {:.3f} {:.3f}), the sun's direction {}, the environment map {}, the cascades {}; hour {}, weather {}, units {:.0f}, "
              "wetness {:.2f}, the world {}; lamps {} kept of {} within {:.0f} m (the table {}, the live list {} of {}{}, off {}, over the budget {}, outside the window {}), the window {} x {} cells, the "
              "schedule {:.2f} (street lamps {:.2f}, fill lights {:.2f}{}){}; the player's car {}",
              kPrefix, frame_, first ? " (first)" : "", !l.valid ? "none" : (in.world_light ? "the world's" : "the car's"), l.sun[0], l.sun[1], l.sun[2], l.ambient[0], l.ambient[1], l.ambient[2],
              in.sun_world_known ? "known" : "unknown", l.env_sharp != 0 ? "known" : "unknown", (in.hit.cascades.valid ? "present" : "none"),
              in.schedule.from_hour ? std::format("{:.2f}", in.hour) : std::string("unknown"), in.weather == rt_world::kUnknownWeather ? std::string("unknown") : std::to_string(in.weather), in.units, in.wet,
              in.world_kind == 1 ? "play" : (in.world_kind == 2 ? "pause" : (in.world_kind == 3 ? "garage" : (in.world_kind == 4 ? "map" : "none"))), lamps.lamp_count,
              lamps.stats.kept + lamps.stats.over_budget, radius_m, lamps.stats.from_table, lamps.stats.from_live, lamps.stats.live_given,
              live_fresh_ ? ", fresh" : ", not fresh", lamps.stats.inactive, lamps.stats.over_budget, lamps.stats.outside_window, lamps.nx, lamps.nz, in.schedule.factor, in.schedule.street, in.schedule.fill,
              (in.schedule.evidence & rt_world::kEvidenceOn) != 0 ? ", the game's lamp list says on" : ((in.schedule.evidence & rt_world::kEvidenceOff) != 0 ? ", the game's lamp list says off" : ""),
              (table_given_ || table_) ? "; the static table is loaded" : "; no static table", in.car ? "is in the top level" : "is not");
}

std::string RtWorldFrame::CensusText() const {
  return rt_world::CensusText(census_, decoded_.hour, schedule_.factor, lamp_frame_.lamp_count, bool(table_given_ || table_)) + rt_world::LampCensusText(lamp_census_);
}

}  // namespace mcla::gpu
