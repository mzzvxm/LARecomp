// MCLA GPU renderer -- AMD FSR Ray Regeneration: the plan of a frame and what the DENOISER row reads. See ray_regen_plan.h.

#include "ray_regen_plan.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>

#include <rex/logging/log_category.h>

#include "settings/larecomp_settings.h"
#include "backend.h"
#include "denoiser_setting.h"
#include "device.h"
#include "ray_regen.h"
#include "vram_room.h"

namespace mcla::gpu::rayregen {

namespace {

constexpr const char* kPrefix = "[gpu] ray regen:";

std::atomic<uint32_t> g_vendor{0};
std::atomic<bool> g_d3d12{false};
std::atomic<bool> g_row_listed{false};
constinit std::array<ContextRequest, kSiteCount> g_traced{};

double Mib(uint64_t bytes) { return double(bytes) / double(rr::kMiB); }

uint64_t NowMs() { return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }

}  // namespace

rr::DebugSwitch ReadDebugSwitch() {
  rr::DebugSwitch debug = rr::DebugSwitch::kRow;
  if (!rr::ParseDebugSwitch(larecomp::settings::mcla_gpu_ray_regen.Get(), &debug)) {
    debug = rr::DebugSwitch::kRow;
  }
  return debug;
}

void NoteDevice(uint32_t vendor_id, bool d3d12, std::string_view adapter_name, uint32_t windows_build) {
  g_vendor.store(vendor_id, std::memory_order_relaxed);
  g_d3d12.store(d3d12, std::memory_order_relaxed);
  ListInputs list;
  list.vendor_id = vendor_id;
  list.adapter_name = adapter_name;
  list.windows_build = windows_build;
  list.windows = kWindowsHost;
  g_row_listed.store(mcla::gpu::rayregen::RowListed(list), std::memory_order_relaxed);
}

uint32_t DeviceVendor() { return g_vendor.load(std::memory_order_relaxed); }

bool RowListed() { return g_row_listed.load(std::memory_order_relaxed); }

Facts MenuFacts() {
  if (!RowListed()) return {};
  const Published pub = Current();
  if (pub.vendor_id != 0) {
    Facts facts = pub.facts;   // the runtime was asked (or the device alone said no): its own answer
    facts.build_has = BuildHas();
    return facts;
  }
  // Not asked yet: what the device alone says. An AMD card on Direct3D 12 may be able (the runtime says whether, when the row asks it), so the row stays usable and says nothing.
  Facts facts;
  facts.build_has = BuildHas();
  const uint32_t vendor = DeviceVendor();
  if (vendor == kVendorAmd) {
    facts.device = g_d3d12.load(std::memory_order_relaxed) ? DeviceVerdict::kOk : DeviceVerdict::kNotD3D12;
    facts.files_ok = true;
  } else {
    facts.device = DeviceVerdict::kNotAmd;
  }
  return facts;
}

bool ContextMaking() { return Current().context_making; }

void NoteTraced(Site site, uint32_t width, uint32_t height) {
  g_traced[size_t(site)] = {0, width, height};
}

bool Planner::RoomOk(Device* device, uint64_t need) {
  if (!device) {
    return true;
  }
  const uint64_t now_ms = NowMs();
  if (need == room_last_need_ && now_ms < room_next_ms_) {
    return room_ok_;
  }
  room_last_need_ = need;
  const backend::VideoMemory memory = device->QueryVideoMemory();
  const bool ok = vram_room::HasRoom(memory.used, memory.budget, need);
  if (ok) {
    room_ok_ = true;
    room_wait_ms_ = 0;
    room_next_ms_ = now_ms + 1000;
    if (room_refused_logged_) {
      room_refused_logged_ = false;
      REXLOG_INFO("{} there is room in video memory for the context again (about {:.0f} MiB asked for, {} of {} MiB in use): it is made now", kPrefix, Mib(need), memory.used >> 20, memory.budget >> 20);
    }
  } else {
    room_ok_ = false;
    room_wait_ms_ = vram_room::NextWaitMs(room_wait_ms_);
    room_next_ms_ = now_ms + room_wait_ms_;
    if (!room_refused_logged_) {
      room_refused_logged_ = true;
      REXLOG_INFO("{} no room in video memory for new contexts and guides (about {:.0f} MiB; {} of {} MiB in use, the line is {}%); retry in {} s",
                  kPrefix, Mib(need), memory.used >> 20, memory.budget >> 20, vram_room::kHighPercent, room_wait_ms_ / 1000);
    }
  }
  return room_ok_;
}

// On Vulkan the row should show (D3D12 ONLY). On older Radeons a saved ON should never start the worker.
void Planner::Update(const Inputs& in) {
  debug_ = ReadDebugSwitch();
  setting_ = in.setting;
  PlanInputs pi;
  pi.setting = in.setting;
  pi.debug = debug_;
  pi.consumers_on = in.consumers_on;
  pi.consumers_active = AnyAutoConsumerOn(in.consumers_on);
  pi.consumers_any = AnyConsumerOn(in.consumers_on);
  pi.guides_ready = true;   // the guide pass is made inside the effect's own frame (ray_regen_guides.h), not before it
  pi.room_ok = true;
  // The runtime is opened live where the plan would want it and the card could run it (a start with the row on OURS opened nothing; Ray Regeneration needs no restart).
  const bool debug_asks = debug_ == rr::DebugSwitch::kOn || debug_ == rr::DebugSwitch::kProbe;
  const bool row_asks = (in.setting == rr::Denoiser::kRr && pi.consumers_any) || (in.setting == rr::Denoiser::kAuto && pi.consumers_active);
  if (debug_ != rr::DebugSwitch::kOff && (debug_asks || row_asks) && RowListed() && in.device && in.frames &&
      g_d3d12.load(std::memory_order_relaxed)) {
    ProbeNow(*in.device, *in.frames);
  }
  pi.facts = MenuFacts();
  plan_ = SelectRayRegen(pi);
  std::array<ContextRequest, kSiteCount> wanted = g_traced;
  uint64_t need = 0;
  bool needs_context = false;
  bool has_context = false;
  for (size_t i = 0; i < kSiteCount; ++i) {
    SitePlan& site = sites_[i];
    ContextRequest& request = wanted[i];
    request.signals = plan_.use && !in.dlss_plan.use_rr ? SiteSignals(Site(i), in.setting, debug_, in.consumers_on) : 0;
    site.active = request.signals != 0;
    if (!site.active || request.width == 0 || request.height == 0) continue;
    if (site.asked.Covers(request)) {
      has_context = true;
      if (Instance().Ready(request.signals, request.width, request.height, Site(i))) continue;
    } else {
      needs_context = true;
      request.width = std::max(request.width, site.asked.width);
      request.height = std::max(request.height, site.asked.height);
    }
    need += EstimateBytes(request.signals, request.width, request.height) + guides::BytesOf(request.width, request.height);
  }
  const bool room = !needs_context || RoomOk(in.device, need);
  if (!room && !has_context) {
    pi.room_ok = false;
    plan_ = SelectRayRegen(pi);
  }
  chosen_ = ChooseDenoiser(plan_, in.dlss_plan);
  if (plan_.use) {
    ++counters_.frames_planned_on;
  }

  // The context: made while the plan is on and kept for kContextHoldFrames after, then freed (only while the option is selected and an effect that uses it runs).
  for (size_t i = 0; i < kSiteCount; ++i) {
    SitePlan& site = sites_[i];
    const ContextRequest& request = wanted[i];
    site.active = site.active && plan_.use;
    if (!site.keeper.Update(site.active)) {
      if (site.asked.signals != 0) {
        Instance().Request(0, 0, 0, Site(i));
        REXLOG_INFO("{} the context for signals {:#x} is released after {} idle frames", kPrefix, site.asked.signals, rr::kContextHoldFrames);
        site.asked = {};
      }
    } else if (site.active && room && request.width != 0 && request.height != 0 && !site.asked.Covers(request)) {
      REXLOG_INFO("{} making a context in the background (signals {:#x}, up to {}x{})", kPrefix, request.signals, request.width, request.height);
      Instance().Request(request.signals, request.width, request.height, Site(i));
      site.asked = request;
    }
  }

  const bool default_on = in.uses_default && DefaultOn(pi.facts, debug_);
  if (!plan_logged_ || plan_.use != plan_last_.use || plan_.why != plan_last_.why || chosen_ != chosen_last_ || default_on != default_on_last_) {
    plan_logged_ = true;
    default_on_last_ = default_on;
    const auto row = in.uses_default && !default_on ? rr::Denoiser::kOurs : in.setting;
    REXLOG_INFO("{} denoiser: {} ({}{}; DENOISER row = {}, mcla_gpu_ray_regen = {})", kPrefix, DenoiserKindName(chosen_), WhyText(plan_.why),
                in.uses_default ? ", vendor default" : "", denoiser::WordOf(row), rr::DebugSwitchName(debug_));
    plan_last_ = plan_;
    chosen_last_ = chosen_;
  }
}

SignalModes Planner::Modes(bool play_view, bool switch_ssr) {
  if (!plan_.use && !modes_logged_) return {};
  rr::RawFrame f;
  f.plan_rr = chosen_ == DenoiserKind::kRayRegen;
  f.failed = f.plan_rr && Current().facts.failed;
  f.play_view = play_view;
  f.row_auto = debug_ == rr::DebugSwitch::kRow && setting_ == rr::Denoiser::kAuto;
  const auto ready = [&](Site index) {
    const SitePlan& site = sites_[size_t(index)];
    const auto& size = g_traced[size_t(index)];
    return f.plan_rr && site.active && site.asked.signals != 0 && Instance().Ready(site.asked.signals, size.width, size.height, index);
  };
  f.context_ready = ready(Site::kReflections);
  SignalModes modes;
  modes.ssr = SignalModeOf(f, rr::Consumer::kSsr, switch_ssr);
  modes.city = SignalModeOf(f, rr::Consumer::kCityRtReflections, switch_ssr);
  f.context_ready = ready(Site::kBounce);
  modes.bounce = SignalModeOf(f, rr::Consumer::kRtGi, true);
  ++counters_.modes_asked;
  if (f.plan_rr || modes_logged_) {
    if (!modes_logged_ || !(modes == modes_last_)) {
      modes_logged_ = true;
      modes_last_ = modes;
      REXLOG_INFO("{} modes for the effects: SSR: {}; city RT reflections: {}; per-pixel bounce light: {} (logged when it changes)",
                  kPrefix, rr::RawWhyText(modes.ssr), rr::RawWhyText(modes.city), rr::RawWhyText(modes.bounce));
    }
  }
  return modes;
}

}  // namespace mcla::gpu::rayregen
