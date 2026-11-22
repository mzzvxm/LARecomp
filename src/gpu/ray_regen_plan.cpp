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
std::atomic<uint32_t> g_traced_w{0};
std::atomic<uint32_t> g_traced_h{0};

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

void NoteDevice(uint32_t vendor_id, bool d3d12) {
  g_vendor.store(vendor_id, std::memory_order_relaxed);
  g_d3d12.store(d3d12, std::memory_order_relaxed);
}

uint32_t DeviceVendor() { return g_vendor.load(std::memory_order_relaxed); }

Vendor RowVendor() { return VendorOf(DeviceVendor()); }

Facts MenuFacts() {
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

void NoteSsrTraced(uint32_t width, uint32_t height) {
  g_traced_w.store(width, std::memory_order_relaxed);
  g_traced_h.store(height, std::memory_order_relaxed);
}

bool Planner::RoomOk(Device* device, uint32_t signals, uint32_t w, uint32_t h) {
  if (!device) {
    return true;
  }
  const uint64_t now_ms = NowMs();
  if (asked_ && Current().context_bytes != 0) {
    room_ok_ = true;   // the context exists: what it holds is in the adapter's use already
    room_wait_ms_ = 0;
    return true;
  }
  if (now_ms < room_next_ms_) {
    return room_ok_;
  }
  const backend::VideoMemory memory = device->QueryVideoMemory();
  const uint64_t need = EstimateBytes(signals, w, h) + guides::BytesOf(w, h);
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
      REXLOG_INFO("{} no room in video memory for the context (about {:.0f} MiB: the runtime's table {:.0f} and the guide images {:.0f}; {} of {} MiB in use, the line is {}%): the effects keep their own denoisers, "
                  "and the context is asked for again in {} s, then less often, up to a minute",
                  kPrefix, Mib(need), Mib(EstimateBytes(signals, w, h)), Mib(guides::BytesOf(w, h)), memory.used >> 20, memory.budget >> 20, vram_room::kHighPercent, room_wait_ms_ / 1000);
    }
  }
  return room_ok_;
}

// CHECK (not run): C26 the live probe (ProbeNow) from the render thread when the row leaves OURS: the worker opens AMD's DLLs while frames run (the effect keeps its own filter until the context is ready), and a
// game started with the row on OURS shows no AMD line in the log until then.
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
  if (debug_ != rr::DebugSwitch::kOff && (debug_asks || row_asks) && DeviceVendor() == kVendorAmd && in.device && in.frames) {
    ProbeNow(*in.device, *in.frames);
  }
  pi.facts = MenuFacts();
  Plan plan = SelectRayRegen(pi);
  const uint32_t w = g_traced_w.load(std::memory_order_relaxed);
  const uint32_t h = g_traced_h.load(std::memory_order_relaxed);
  const uint32_t signals = ContextSignals(in.setting, debug_, in.consumers_on);
  if (plan.use && signals != 0 && w != 0 && h != 0) {
    pi.room_ok = RoomOk(in.device, signals, w, h);   // asked only where Ray Regeneration would be the choice and the size is known
    plan = SelectRayRegen(pi);
  }
  plan_ = plan;
  chosen_ = ChooseDenoiser(plan_, in.dlss_plan);
  if (plan_.use) {
    ++counters_.frames_planned_on;
  }

  // The context: made while the plan is on and kept for kContextHoldFrames after, then freed (only while the option is selected and an effect that uses it runs).
  const bool keep = keeper_.Update(ContextWanted(plan_.why));
  if (!keep) {
    if (asked_) {
      Instance().Request(0, 0, 0);
      asked_ = false;
      signals_ = 0;
      REXLOG_INFO("{} the context is released ({} frames without Ray Regeneration wanted): its video memory is given back", kPrefix, rr::kContextHoldFrames);
    }
  } else if (plan_.use && signals != 0 && w != 0 && h != 0) {
    if (!asked_ || signals != signals_ || w > asked_size_[0] || h > asked_size_[1]) {
      REXLOG_INFO("{} {} its context (signals {:#x}, up to {}x{}: about {:.0f} MiB by the runtime's table) in the background; the effects keep their own denoisers until it is ready", kPrefix,
                  asked_ ? "making again" : "making", signals, w, h, Mib(EstimateBytes(signals, w, h)));
    }
    Instance().Request(signals, w, h);
    asked_ = true;
    signals_ = signals;
    asked_size_[0] = std::max(w, asked_ ? asked_size_[0] : 0u);
    asked_size_[1] = std::max(h, asked_ ? asked_size_[1] : 0u);
  }

  if (!plan_logged_ || plan_.use != plan_last_.use || plan_.why != plan_last_.why || chosen_ != chosen_last_) {
    plan_logged_ = true;
    REXLOG_INFO("{} denoiser: {} ({}; DENOISER row = {}, mcla_gpu_ray_regen = {})", kPrefix, DenoiserKindName(chosen_), WhyText(plan_.why), denoiser::WordOf(in.setting), rr::DebugSwitchName(debug_));
    plan_last_ = plan_;
    chosen_last_ = chosen_;
  }
}

rr::RawModes Planner::Modes(bool play_view, bool switch_ssr, bool switch_car_refl) {
  rr::RawFrame f;
  f.plan_rr = plan_.use;
  f.failed = Current().facts.failed;
  f.context_ready = plan_.use && asked_ && Instance().Ready(signals_, asked_size_[0], asked_size_[1]);
  f.armed = true;   // Ray Regeneration needs no upscaled frame (SignalWhyOf)
  f.play_view = play_view;
  f.row_auto = debug_ == rr::DebugSwitch::kRow && setting_ == rr::Denoiser::kAuto;
  const rr::RawModes modes = SignalModesOf(f, switch_ssr, switch_car_refl);
  ++counters_.modes_asked;
  if (f.plan_rr || modes_logged_) {
    if (!modes_logged_ || !(modes == modes_last_)) {
      modes_logged_ = true;
      modes_last_ = modes;
      REXLOG_INFO("{} modes for the effects: {}: {}; {}: {} (logged when it changes)", kPrefix, rr::RawEffectName(rr::RawEffect::kSsr), rr::RawWhyText(modes.ssr), rr::RawEffectName(rr::RawEffect::kCarRefl),
                  rr::RawWhyText(modes.car_refl));
    }
  }
  return modes;
}

}  // namespace mcla::gpu::rayregen
