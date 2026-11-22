// Check of the AMD FSR Ray Regeneration rules (ray_regen_rules.h): AMD's ids, the signals and the consumers, the device and the plan (the whole table against a restatement of the order), the choice
// beside DLSS Ray Reconstruction, the signal modes, the input map against the Ray Reconstruction guides, the dispatch plan, the conventions (octahedral normals, material, albedo, the signed linear depth of
// the game's own view space, motion, the camera pair, the frame clock), the memory estimate and the row's words. A program of its own: no renderer, no Windows, no GPU, no AMD runtime; exits 0 when every
// check holds.
//
//   cmake --build <build dir> --target larecomp_ray_regen_rules_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_ray_regen_rules_test

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "gbuffer_rules.h"
#include "ray_regen_rules.h"
#include "temporal_math.h"

using namespace mcla::gpu::rayregen;
namespace rr = mcla::gpu::rr;
namespace gb = mcla::gpu::gbuffer;
namespace tmath = mcla::gpu::temporal;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

bool Near(double a, double b, double eps = 1e-5) { return std::fabs(a - b) <= eps; }

// --- AMD's ids --------------------------------------------------------------------------------------------------------------------------------------

void CheckIds() {
  // ffx_api.h / ffx_denoiser.h, FSR SDK 2.4.0 (ray_regen.cpp static_asserts the same against the real headers).
  Check(kEffectIdDenoiser == 0x00050000u, "FFX_API_EFFECT_ID_DENOISER is 0x00050000");
  Check(kHeaderVersion == 4202497u, "FFX_DENOISER_VERSION is Ray Regeneration 1.2.1: (1 << 22) | (2 << 12) | 1");
  Check(kDispatchReset == 1u && kDispatchNonEncodedAlbedo == 2u, "dispatch flags: reset 1, non-encoded albedo 2");
  Check(kMaterialTypes == 4, "four material types (0..3)");
  Check(kVendorAmd == 0x1002u && kShaderModelMinimum == 0x66u, "AMD's PCI vendor id and shader model 6.6");
  Check(FlagOf(Signal::kAmbientOcclusion) == 1u && FlagOf(Signal::kDirectDiffuse) == 2u && FlagOf(Signal::kDirectSpecular) == 4u && FlagOf(Signal::kDominantLight) == 8u &&
            FlagOf(Signal::kIndirectDiffuse) == 16u && FlagOf(Signal::kIndirectSpecular) == 32u && FlagOf(Signal::kSpecularOcclusion) == 64u,
        "the signal flags are FfxApiDenoiserSignalFlags' bits (AO 1, direct diffuse 2, direct specular 4, dominant light 8, indirect diffuse 16, indirect specular 32, specular occlusion 64)");
  Check(kSignalCount == 7 && kAllSignalFlags == 127u, "seven signals");
}

// --- signals and consumers ----------------------------------------------------------------------------------------------------------------------------

void CheckSignals() {
  Check(IsRadiance(Signal::kDirectDiffuse) && IsRadiance(Signal::kDirectSpecular) && IsRadiance(Signal::kIndirectDiffuse) && IsRadiance(Signal::kIndirectSpecular), "four radiance signals");
  Check(!IsRadiance(Signal::kAmbientOcclusion) && !IsRadiance(Signal::kDominantLight) && !IsRadiance(Signal::kSpecularOcclusion), "the three single channel signals are not radiance");
  Check(CarriesHitDistanceInAlpha(Signal::kIndirectDiffuse) && CarriesHitDistanceInAlpha(Signal::kIndirectSpecular) && !CarriesHitDistanceInAlpha(Signal::kDirectSpecular) &&
            !CarriesHitDistanceInAlpha(Signal::kDominantLight),
        "the hit distance rides in the alpha of the indirect signals only (the dominant light's R is the distance itself)");
  Check(AlbedoOfSignal(Signal::kIndirectSpecular) == AlbedoOf::kSpecular && AlbedoOfSignal(Signal::kDirectSpecular) == AlbedoOf::kSpecular &&
            AlbedoOfSignal(Signal::kIndirectDiffuse) == AlbedoOf::kDiffuse && AlbedoOfSignal(Signal::kDirectDiffuse) == AlbedoOf::kDiffuse &&
            AlbedoOfSignal(Signal::kAmbientOcclusion) == AlbedoOf::kNone && AlbedoOfSignal(Signal::kDominantLight) == AlbedoOf::kNone,
        "which albedo each signal is multiplied by after the denoise");
  Check(SignalOf(rr::Consumer::kSsr) == Signal::kIndirectSpecular && SignalOf(rr::Consumer::kCarRtReflections) == Signal::kIndirectSpecular &&
            SignalOf(rr::Consumer::kCityRtReflections) == Signal::kIndirectSpecular && SignalOf(rr::Consumer::kRtGi) == Signal::kIndirectDiffuse,
        "the consumers' signals: three reflections are indirect specular, the bounce light indirect diffuse");
  // AMD: the specular albedo may be left empty only when no specular signal and no dominant light visibility is set; the diffuse one likewise.
  Check(!NeedsSpecularAlbedo(FlagOf(Signal::kIndirectDiffuse)) && NeedsSpecularAlbedo(FlagOf(Signal::kIndirectSpecular)) && NeedsSpecularAlbedo(FlagOf(Signal::kDirectSpecular)) &&
            NeedsSpecularAlbedo(FlagOf(Signal::kDominantLight)) && !NeedsSpecularAlbedo(FlagOf(Signal::kAmbientOcclusion)),
        "the specular albedo is needed by direct / indirect specular and the dominant light");
  Check(!NeedsDiffuseAlbedo(FlagOf(Signal::kIndirectSpecular)) && NeedsDiffuseAlbedo(FlagOf(Signal::kIndirectDiffuse)) && NeedsDiffuseAlbedo(FlagOf(Signal::kDirectDiffuse)) &&
            NeedsDiffuseAlbedo(FlagOf(Signal::kDominantLight)) && !NeedsDiffuseAlbedo(FlagOf(Signal::kSpecularOcclusion)),
        "the diffuse albedo is needed by direct / indirect diffuse and the dominant light");
}

ConsumerTable Cells(bool built0, bool auto0, bool built1, bool auto1, bool built2, bool auto2, bool built3, bool auto3) {
  return {{{built0, auto0}, {built1, auto1}, {built2, auto2}, {built3, auto3}}};
}

void CheckConsumers() {
  Check(rr::kConsumerCount == 4, "the table has the four consumers it is written for");
  // RG-3: the screen-space reflections have their signal writer; no cell is trusted by AUTO until a Radeon A/B; the cars' reflections are not a fit (RAY-REGEN.md 4.2); the city's and the bounce light's
  // effects are on other branches.
  Check(AnySignalBuilt(), "shipped: an effect writes a signal (the screen-space reflections)");
  for (size_t i = 0; i < rr::kConsumerCount; ++i) {
    Check(!kConsumers[i].auto_uses, "shipped cell " + std::to_string(i) + " is not trusted by AUTO (until a Radeon A/B)");
    Check(kConsumers[i].signal_built == (i == size_t(rr::Consumer::kSsr)), "shipped cell " + std::to_string(i) + (i == 0 ? " has a signal writer" : " has none (the car's is not a fit, the others are not in this tree)"));
  }
  Check(AnyConsumerOn(rr::kAllConsumers) && AnyConsumerOn(rr::ConsumerBit(rr::Consumer::kSsr)) && !AnyConsumerOn(rr::ConsumerBit(rr::Consumer::kCarRtReflections)) && !AnyConsumerOn(0) &&
            !AnyAutoConsumerOn(rr::kAllConsumers),
        "with the shipped table the reflections count, the cars' do not, and AUTO counts nothing");
  const ConsumerTable none = Cells(false, false, false, false, false, false, false, false);
  Check(!AnySignalBuilt(none) && !AnyConsumerOn(rr::kAllConsumers, none) && !AnyAutoConsumerOn(rr::kAllConsumers, none), "with every cell false no effect counts");
  const ConsumerTable ssr_built = Cells(true, false, false, false, false, false, false, false);
  Check(AnySignalBuilt(ssr_built), "a table with SSR built has a signal mode");
  Check(AnyConsumerOn(rr::ConsumerBit(rr::Consumer::kSsr), ssr_built) && !AnyConsumerOn(rr::ConsumerBit(rr::Consumer::kRtGi), ssr_built) && !AnyConsumerOn(0, ssr_built),
        "an effect counts when it is on and built");
  Check(!AnyAutoConsumerOn(rr::kAllConsumers, ssr_built), "built but not trusted: AUTO does not count it");
  const ConsumerTable ssr_auto = Cells(true, true, true, false, false, true, false, false);
  Check(AnyAutoConsumerOn(rr::ConsumerBit(rr::Consumer::kSsr), ssr_auto) && !AnyAutoConsumerOn(rr::ConsumerBit(rr::Consumer::kCarRtReflections), ssr_auto),
        "AUTO counts a built, trusted effect only");
  Check(!AnyAutoConsumerOn(rr::ConsumerBit(rr::Consumer::kCityRtReflections), ssr_auto), "an effect that is trusted but not built never counts");

  // The context's signals: the row AUTO takes the trusted effects only, the row's value and the debug switch's "on" every built one.
  const ConsumerTable mixed = Cells(true, true, true, false, true, false, true, false);  // SSR trusted; cars, city reflections, GI built only
  const uint32_t all = rr::kAllConsumers;
  Check(ContextSignals(rr::Denoiser::kAuto, rr::DebugSwitch::kRow, all, mixed) == FlagOf(Signal::kIndirectSpecular), "AUTO: the trusted SSR's indirect specular only");
  Check(ContextSignals(rr::Denoiser::kRr, rr::DebugSwitch::kRow, all, mixed) == (FlagOf(Signal::kIndirectSpecular) | FlagOf(Signal::kIndirectDiffuse)), "the row's value: every built effect (specular and diffuse)");
  Check(ContextSignals(rr::Denoiser::kAuto, rr::DebugSwitch::kOn, all, mixed) == (FlagOf(Signal::kIndirectSpecular) | FlagOf(Signal::kIndirectDiffuse)), "the debug switch on stands for the row's value even over AUTO");
  Check(ContextSignals(rr::Denoiser::kRr, rr::DebugSwitch::kRow, rr::ConsumerBit(rr::Consumer::kRtGi), mixed) == FlagOf(Signal::kIndirectDiffuse), "only the effects that are on");
  Check(ContextSignals(rr::Denoiser::kRr, rr::DebugSwitch::kRow, all) == FlagOf(Signal::kIndirectSpecular), "shipped table: the row's value makes a context for the reflections' indirect specular signal");
  Check(ContextSignals(rr::Denoiser::kAuto, rr::DebugSwitch::kRow, all) == 0, "shipped table: AUTO makes none (no effect is trusted yet)");
  Check(ContextSignals(rr::Denoiser::kRr, rr::DebugSwitch::kRow, rr::ConsumerBit(rr::Consumer::kCarRtReflections)) == 0, "shipped table: the cars' reflections alone make none");
  Check(ContextSignals(rr::Denoiser::kRr, rr::DebugSwitch::kRow, 0) == 0, "no effect on: no context (it is freed after the hold)");
  Check(ContextSignals(rr::Denoiser::kAuto, rr::DebugSwitch::kRow, rr::ConsumerBit(rr::Consumer::kCarRtReflections), mixed) == 0, "AUTO: an untrusted effect alone makes no context");
}

void CheckProbeWanted() {
  Check(!ProbeWanted(rr::DebugSwitch::kOff, true) && !ProbeWanted(rr::DebugSwitch::kOff, false), "off never opens the runtime");
  Check(ProbeWanted(rr::DebugSwitch::kProbe, false) && ProbeWanted(rr::DebugSwitch::kOn, false), "probe and on open it even with no effect to use it");
  Check(!ProbeWanted(rr::DebugSwitch::kRow, false) && ProbeWanted(rr::DebugSwitch::kRow, true), "auto opens it only where an effect has a signal mode");
  Check(ProbeWanted(rr::DebugSwitch::kRow, AnySignalBuilt()), "the shipped table has a signal writer: a start with the row not on OURS may open the runtime (on a Radeon only: ray_regen.cpp checks the vendor)");
  Check(!ProbeWanted(rr::DebugSwitch::kRow, true, true), "the row on OURS opens nothing at the start (no memory, no start time): a row that leaves OURS opens it live");
  Check(ProbeWanted(rr::DebugSwitch::kProbe, false, true) && ProbeWanted(rr::DebugSwitch::kOn, false, true) && !ProbeWanted(rr::DebugSwitch::kOff, true, false), "the debug switch decides over the row: probe and on open it, off never");
  rr::DebugSwitch parsed = rr::DebugSwitch::kRow;
  Check(rr::ParseDebugSwitch("probe", &parsed) && parsed == rr::DebugSwitch::kProbe && rr::ParseDebugSwitch("auto", &parsed) && parsed == rr::DebugSwitch::kRow, "the cvar's words are the DLSS switch's");
}

void CheckProviders() {
  Check(UndrivenSignals(FlagOf(Signal::kIndirectSpecular) | FlagOf(Signal::kDominantLight)) == FlagOf(Signal::kDominantLight) && UndrivenSignals(kDrivenSignals) == 0 &&
            kDrivenSignals == (kAllSignalFlags & ~8u),
        "every signal but the dominant light visibility is driven in the first cut");
  Check(IsDriverProvided(1ull << 31, "1.2.1") && IsDriverProvided(5, "1.3.0*") && !IsDriverProvided(0x00010203, "1.2.1") && !IsDriverProvided(7, ""), "a driver's provider: bit 31 of the id or a trailing * in the name");
  const ProviderVersion shipped_121{0x100, 1, 2, 1, false};
  const ProviderVersion shipped_120{0x101, 1, 2, 0, false};
  const ProviderVersion driver_130{0x80000102ull, 1, 3, 0, true};
  const ProviderVersion list1[] = {shipped_120, shipped_121};
  Check(PickProvider(list1, 2) == 1, "the newest shipped version");
  const ProviderVersion list2[] = {shipped_121, shipped_120};
  Check(PickProvider(list2, 2) == 0, "in any order");
  const ProviderVersion list3[] = {shipped_121, driver_130};
  Check(PickProvider(list3, 2) == 0, "a driver's newer provider is not taken while a shipped one exists (the context is pinned)");
  const ProviderVersion list4[] = {driver_130, shipped_120};
  Check(PickProvider(list4, 2) == 1, "even when it comes first");
  const ProviderVersion list5[] = {driver_130};
  Check(PickProvider(list5, 1) == 0, "a driver's provider alone is taken");
  Check(PickProvider(nullptr, 0) == -1, "an empty list");
  const ProviderVersion tie[] = {shipped_121, {0x200, 1, 2, 1, false}};
  Check(PickProvider(tie, 2) == 0, "a tie keeps the first");
}

// --- the device -------------------------------------------------------------------------------------------------------------------------------------

void CheckDevice() {
  DeviceInputs d;
  d.vendor_id = 0x1002;
  d.d3d12 = true;
  d.shader_model = 0x66;
  Check(DeviceOk(d) == DeviceVerdict::kOk, "an AMD card on Direct3D 12 with shader model 6.6");
  d.shader_model = 0x69;
  Check(DeviceOk(d) == DeviceVerdict::kOk, "shader model 6.9 is enough");
  d.shader_model = 0x65;
  Check(DeviceOk(d) == DeviceVerdict::kShaderModelTooLow, "shader model 6.5 is not");
  d.shader_model = 0x66;
  d.d3d12 = false;
  Check(DeviceOk(d) == DeviceVerdict::kNotD3D12, "Vulkan has no provider");
  d.d3d12 = true;
  d.vendor_id = 0x10DE;
  Check(DeviceOk(d) == DeviceVerdict::kNotAmd, "an NVIDIA card");
  d.vendor_id = 0x8086;
  Check(DeviceOk(d) == DeviceVerdict::kNotAmd, "an Intel card");
  d.vendor_id = 0x1002;
  d.d3d12 = false;
  d.shader_model = 0x60;
  Check(DeviceOk(d) == DeviceVerdict::kNotD3D12, "the API is asked before the shader model");
  d.vendor_id = 0x10DE;
  Check(DeviceOk(d) == DeviceVerdict::kNotAmd, "the vendor is asked first");
  Check(VendorOf(0x1002) == Vendor::kAmd && VendorOf(0x10DE) == Vendor::kNvidia && VendorOf(0x8086) == Vendor::kOther, "the vendors");
}

// --- the plan ---------------------------------------------------------------------------------------------------------------------------------------

// A restatement of the order, written out of the rules' header on purpose: the first reason that applies is the answer.
Why Reference(const PlanInputs& in) {
  if (in.debug == rr::DebugSwitch::kOff) return Why::kSwitchOff;
  const rr::Denoiser setting = in.debug == rr::DebugSwitch::kOn ? rr::Denoiser::kRr : in.setting;
  if (setting == rr::Denoiser::kOurs) return Why::kSettingOurs;
  if (in.debug == rr::DebugSwitch::kRow) {
    if (setting == rr::Denoiser::kAuto && !in.consumers_active) return Why::kNoConsumer;
    if (setting == rr::Denoiser::kRr && !(in.consumers_active || in.consumers_any)) return Why::kNoConsumer;
  }
  if (!in.facts.build_has) return Why::kBuildHasNone;
  if (in.facts.device != DeviceVerdict::kOk) return Why::kNotThisGpu;
  if (!in.facts.files_ok) return Why::kFilesMissing;
  if (!in.facts.probed) return Why::kNotProbed;
  if (!in.facts.supported) return Why::kNotSupported;
  if (in.facts.failed) return Why::kFailed;
  if (in.debug == rr::DebugSwitch::kProbe) return Why::kProbeOnly;
  if (!in.room_ok) return Why::kNoRoom;
  if (!in.guides_ready) return Why::kGuidesNotReady;
  return Why::kOn;
}

PlanInputs Ready() {
  PlanInputs in;
  in.setting = rr::Denoiser::kRr;
  in.facts.build_has = true;
  in.facts.files_ok = true;
  in.facts.device = DeviceVerdict::kOk;
  in.facts.probed = true;
  in.facts.supported = true;
  in.consumers_any = true;
  in.guides_ready = true;
  in.room_ok = true;
  return in;
}

void CheckPlan() {
  const rr::DebugSwitch debugs[4] = {rr::DebugSwitch::kOff, rr::DebugSwitch::kProbe, rr::DebugSwitch::kOn, rr::DebugSwitch::kRow};
  const rr::Denoiser settings[3] = {rr::Denoiser::kAuto, rr::Denoiser::kRr, rr::Denoiser::kOurs};
  const DeviceVerdict devices[4] = {DeviceVerdict::kOk, DeviceVerdict::kNotAmd, DeviceVerdict::kNotD3D12, DeviceVerdict::kShaderModelTooLow};
  bool table = true;
  int reached_on = 0;
  uint64_t seen[16] = {};
  for (rr::DebugSwitch debug : debugs) {
    for (rr::Denoiser setting : settings) {
      for (DeviceVerdict device : devices) {
        for (int bits = 0; bits < 64; ++bits) {
          for (int c = 0; c < 3; ++c) {
            for (int extra = 0; extra < 4; ++extra) {
              PlanInputs in;
              in.debug = debug;
              in.setting = setting;
              in.facts.device = device;
              in.facts.build_has = (bits & 1) != 0;
              in.facts.files_ok = (bits & 2) != 0;
              in.facts.probed = (bits & 4) != 0;
              in.facts.supported = (bits & 8) != 0;
              in.facts.failed = (bits & 16) != 0;
              in.room_ok = (bits & 32) != 0;
              in.consumers_active = c == 2;
              in.consumers_any = c >= 1;
              in.guides_ready = (extra & 1) != 0;
              const Plan got = SelectRayRegen(in);
              const Why want = Reference(in);
              table = table && got.why == want && got.use == (want == Why::kOn);
              reached_on += want == Why::kOn;
              ++seen[size_t(want)];
              (void)extra;
            }
          }
        }
      }
    }
  }
  Check(table, "the plan: the whole table (4 switches x 3 settings x 4 devices x 64 facts x 3 consumer states x guides) against the restated order");
  Check(reached_on > 0, "the table reaches 'on'");
  bool every_reason = true;
  for (size_t w = 0; w <= size_t(Why::kGuidesNotReady); ++w) every_reason = every_reason && seen[w] > 0;
  Check(every_reason, "the table reaches every reason (the restatement is not vacuous)");

  // Scenes.
  PlanInputs in = Ready();
  Check(SelectRayRegen(in).use && SelectRayRegen(in).why == Why::kOn, "an RX 9070 XT, the row's value, an effect with a signal mode on, guides ready: Ray Regeneration");
  in.setting = rr::Denoiser::kAuto;
  Check(SelectRayRegen(in).why == Why::kNoConsumer, "AUTO with only effects ours cleans as well: ours (every cell is false today)");
  in.consumers_active = true;
  Check(SelectRayRegen(in).use, "AUTO with an effect it trusts: on");
  in = Ready();
  in.setting = rr::Denoiser::kOurs;
  Check(SelectRayRegen(in).why == Why::kSettingOurs, "the row OURS: never");
  in = Ready();
  in.consumers_any = false;
  Check(SelectRayRegen(in).why == Why::kNoConsumer, "the row's value with nothing to denoise: ours, said as 'no consumer'");
  in.debug = rr::DebugSwitch::kOn;
  Check(SelectRayRegen(in).use, "the debug switch on lifts the consumer rule (the A/B and cost runs)");
  in.debug = rr::DebugSwitch::kProbe;
  Check(!SelectRayRegen(in).use && SelectRayRegen(in).why == Why::kProbeOnly, "probe: asked and logged, no frame");
  in = Ready();
  in.facts.device = DeviceVerdict::kNotAmd;
  Check(SelectRayRegen(in).why == Why::kNotThisGpu, "an NVIDIA or Intel PC: not this GPU (the runtime is never opened there)");
  in = Ready();
  in.facts.supported = false;
  Check(SelectRayRegen(in).why == Why::kNotSupported, "an RDNA 3 card: the runtime lists no provider");
  in = Ready();
  in.facts.probed = false;
  Check(SelectRayRegen(in).why == Why::kNotProbed, "still opening the runtime");
  in = Ready();
  in.facts.failed = true;
  Check(SelectRayRegen(in).why == Why::kFailed, "failed for the session");
  in = Ready();
  in.guides_ready = false;
  Check(!SelectRayRegen(in).use && SelectRayRegen(in).why == Why::kGuidesNotReady, "no guides this frame: ours, no black frame (the effects keep their own filter)");
}

void CheckChoice() {
  const Plan amd_on{true, Why::kOn};
  const Plan amd_off{false, Why::kNotThisGpu};
  const rr::Plan nv_on{true, rr::Why::kRrOn};
  const rr::Plan nv_off{false, rr::Why::kNotSupported};
  Check(ChooseDenoiser(amd_on, nv_off) == DenoiserKind::kRayRegen, "AMD Ray Regeneration where it runs");
  Check(ChooseDenoiser(amd_off, nv_on) == DenoiserKind::kDlssRr, "else DLSS Ray Reconstruction");
  Check(ChooseDenoiser(amd_off, nv_off) == DenoiserKind::kOurs, "else ours");
  Check(ChooseDenoiser(amd_on, nv_on) == DenoiserKind::kRayRegen, "the owner's order if both could run: AMD first (one adapter is one vendor, so it cannot happen today)");
  // On one PC the two plans cannot both be on: the AMD facts of an NVIDIA device say 'not this GPU', DLSS RR's facts of an AMD device say 'not supported' / 'no plugin'.
  PlanInputs nvidia_pc = Ready();
  nvidia_pc.facts.device = DeviceOk({0x10DE, true, 0x66});
  Check(!SelectRayRegen(nvidia_pc).use, "an RTX PC never takes the AMD plan");
}

// --- the signal modes -------------------------------------------------------------------------------------------------------------------------------

void CheckSignalModes() {
  rr::RawFrame f;
  f.plan_rr = true;
  f.context_ready = true;
  f.armed = false;  // no upscaler this frame (native, or the frame is not upscaled): Ray Regeneration does not care
  f.play_view = true;
  const ConsumerTable both_built = Cells(true, false, true, false, false, false, false, false);
  Check(SignalModesOf(f, true, true, both_built).Ssr() && SignalModesOf(f, true, true, both_built).CarRefl(), "an unarmed frame still takes the signals (Ray Regeneration needs no upscaler)");
  Check(SignalModesOf(f, true, true).Ssr() && !SignalModesOf(f, true, true).CarRefl() && SignalModesOf(f, true, true).car_refl == rr::RawWhy::kPlanOurs,
        "the shipped table: the reflections take the frame, the cars' reflections have no signal writer and keep their own filter whatever the plan says");
  Check(rr::RawModesOf(f, true, true).ssr == rr::RawWhy::kNoRrFrame, "while DLSS RR's raw modes refuse the same frame (the twin that differs on purpose)");
  f.armed = true;
  f.play_view = false;
  Check(SignalModesOf(f, true, true).ssr == rr::RawWhy::kNoRrFrame, "the pause, garage and map worlds carry no guides: no signal there");
  f.play_view = true;
  f.plan_rr = false;
  Check(SignalModesOf(f, true, true).ssr == rr::RawWhy::kPlanOurs, "plan not Ray Regeneration: the effects run as they always did");
  f.plan_rr = true;
  f.context_ready = false;
  Check(SignalModesOf(f, true, true).ssr == rr::RawWhy::kNoContext, "the context is being made: ours meanwhile");
  f.context_ready = true;
  f.failed = true;
  Check(SignalModesOf(f, true, true).ssr == rr::RawWhy::kFailed, "failed: ours");
  f.failed = false;
  Check(SignalModesOf(f, false, true, both_built).ssr == rr::RawWhy::kSwitchOff && SignalModesOf(f, false, true, both_built).CarRefl(), "the effect's own switch is the last word");
  // AUTO: an effect it does not trust keeps its own filter in a frame taken for another.
  f.row_auto = true;
  const ConsumerTable ssr_only_trusted = Cells(true, true, true, false, false, false, false, false);
  const rr::RawModes modes = SignalModesOf(f, true, true, ssr_only_trusted);
  Check(modes.Ssr() && modes.car_refl == rr::RawWhy::kAutoKeepsOurs, "AUTO: SSR trusted goes, the cars' reflections keep their own filter");
  f.row_auto = false;
  Check(SignalModesOf(f, true, true, ssr_only_trusted).CarRefl(), "the row's own value takes every effect with a signal mode");
}

// --- the input map ----------------------------------------------------------------------------------------------------------------------------------

void CheckInputMap() {
  Check(kInputMap.size() == 5, "five common inputs");
  for (size_t i = 0; i < kInputCount; ++i) Check(size_t(kInputMap[i].input) == i, std::string("the map is in input order: ") + InputName(Input(i)));
  Check(MapOf(Input::kDiffuseAlbedo).source == rr::Buffer::kDiffuseAlbedo && MapOf(Input::kDiffuseAlbedo).share == Share::kShared, "the diffuse albedo is the RR guide as it is");
  Check(MapOf(Input::kSpecularAlbedo).source == rr::Buffer::kSpecularAlbedo && MapOf(Input::kSpecularAlbedo).share == Share::kShared, "the specular albedo is the RR guide as it is");
  Check(MapOf(Input::kNormals).source == rr::Buffer::kNormalRoughness && MapOf(Input::kNormals).share == Share::kConverted, "the normals are made from the RR normals + roughness");
  Check(MapOf(Input::kMotionVectors).source == rr::Buffer::kMotion && MapOf(Input::kMotionVectors).share == Share::kConverted, "the motion is made from the kept vectors");
  Check(MapOf(Input::kLinearDepth).source == rr::Buffer::kDepth && MapOf(Input::kLinearDepth).share == Share::kConverted, "the linear depth is made from the kept depth");
  Check(GuideIsShared(rr::Buffer::kDiffuseAlbedo) && GuideIsShared(rr::Buffer::kSpecularAlbedo) && !GuideIsShared(rr::Buffer::kNormalRoughness) && !GuideIsShared(rr::Buffer::kDepth) &&
            !GuideIsShared(rr::Buffer::kMotion),
        "exactly the two albedos are shared");
  // Every guide the RR plan tags for the frame is either an input here or listed as unused, never both and never forgotten.
  const rr::Buffer rr_guides[] = {rr::Buffer::kDepth,          rr::Buffer::kMotion,         rr::Buffer::kDiffuseAlbedo,  rr::Buffer::kSpecularAlbedo, rr::Buffer::kNormalRoughness,
                                  rr::Buffer::kSpecularHitDistance, rr::Buffer::kResponsivity, rr::Buffer::kColorBeforeTransparency};
  bool covered = true;
  for (rr::Buffer b : rr_guides) {
    bool as_input = false;
    bool as_unused = false;
    for (const InputMapEntry& e : kInputMap) as_input = as_input || e.source == b;
    for (const UnusedGuide& u : kUnusedGuides) as_unused = as_unused || u.guide == b;
    covered = covered && (as_input != as_unused);
  }
  Check(covered, "every RR guide is mapped or listed as unused, exactly one of the two");
  // The shared guides are RGBA16F in the RR guide pass: valid for both denoisers.
  Check(rr::FormatOk(rr::Buffer::kDiffuseAlbedo, rr::Format::kRgba16Float) && FormatOkInput(Input::kDiffuseAlbedo, rr::Format::kRgba16Float), "RGBA16F diffuse albedo is valid for both");
  Check(rr::FormatOk(rr::Buffer::kSpecularAlbedo, rr::Format::kRgba16Float) && FormatOkInput(Input::kSpecularAlbedo, rr::Format::kRgba16Float), "RGBA16F specular albedo is valid for both");
  Check(DispatchFlags(false) == kDispatchNonEncodedAlbedo && DispatchFlags(true) == (kDispatchNonEncodedAlbedo | kDispatchReset) && DispatchFlags(true, false) == kDispatchReset && DispatchFlags(false, false) == 0,
        "the dispatch flags: reset, and the identity albedo encoding for the shared linear guides");
}

// --- the dispatch plan ------------------------------------------------------------------------------------------------------------------------------

rr::Image At(uint32_t w, uint32_t h, rr::Format f) {
  rr::Image i;
  i.present = true;
  i.width = w;
  i.height = h;
  i.format = f;
  return i;
}

// A dispatch for the indirect specular signal at 1920x1080 in a context made for 2560x1440.
Inputs Dispatch() {
  Inputs in;
  in.render_size[0] = 1920;
  in.render_size[1] = 1080;
  in.context_max_size[0] = 2560;
  in.context_max_size[1] = 1440;
  in.context_signals = FlagOf(Signal::kIndirectSpecular);
  in.dispatch_signals = in.context_signals;
  in.linear_depth = At(1920, 1080, rr::Format::kR32Float);
  in.motion = At(1920, 1080, rr::Format::kRgba16Float);
  in.normals = At(1920, 1080, rr::Format::kRgba16Float);
  in.specular_albedo = At(1920, 1080, rr::Format::kRgba16Float);
  in.signal_in[size_t(Signal::kIndirectSpecular)] = At(1920, 1080, rr::Format::kRgba16Float);
  in.signal_out[size_t(Signal::kIndirectSpecular)] = At(1920, 1080, rr::Format::kRgba16Float);
  return in;
}

void CheckDispatchPlan() {
  Inputs in = Dispatch();
  DispatchPlan p = PlanDispatch(in);
  Check(p.verdict == Verdict::kOk, "an indirect specular dispatch with its inputs is ok (no diffuse albedo: no diffuse signal)");
  in.render_size[0] = 1280;
  in.render_size[1] = 720;
  Check(PlanDispatch(in).verdict == Verdict::kBadSize, "the images are not the render size");
  in = Dispatch();
  in.render_size[0] = 3840;
  in.render_size[1] = 2160;
  Check(PlanDispatch(in).verdict == Verdict::kTooBig, "a render size above the context's maximum");
  in = Dispatch();
  in.dispatch_signals = 0;
  Check(PlanDispatch(in).verdict == Verdict::kNoSignals, "no signal: nothing to dispatch");
  in = Dispatch();
  in.dispatch_signals |= FlagOf(Signal::kIndirectDiffuse);
  Check(PlanDispatch(in).verdict == Verdict::kSignalMismatch, "a signal the context lacks");
  in = Dispatch();
  in.context_signals |= FlagOf(Signal::kAmbientOcclusion);
  Check(PlanDispatch(in).verdict == Verdict::kSignalMismatch, "a context signal the dispatch leaves out (AMD: every signal of the context is dispatched)");
  in = Dispatch();
  in.linear_depth.present = false;
  Check(PlanDispatch(in).verdict == Verdict::kMissingInput && std::string(PlanDispatch(in).culprit) == "linear depth", "no linear depth");
  in = Dispatch();
  in.normals.present = false;
  Check(PlanDispatch(in).verdict == Verdict::kMissingInput, "no normals");
  in = Dispatch();
  in.specular_albedo.present = false;
  Check(PlanDispatch(in).verdict == Verdict::kMissingInput, "an indirect specular signal needs the specular albedo");
  in = Dispatch();
  in.signal_out[size_t(Signal::kIndirectSpecular)].present = false;
  Check(PlanDispatch(in).verdict == Verdict::kMissingSignal, "no output image for the signal");
  in = Dispatch();
  in.signal_in[size_t(Signal::kIndirectSpecular)].width = 960;
  Check(PlanDispatch(in).verdict == Verdict::kBadSize, "the signal's input is half width (checkerboard is not on: the context has no checkerboard flags)");
  in = Dispatch();
  in.signal_in[size_t(Signal::kIndirectSpecular)].format = rr::Format::kR16Float;
  Check(PlanDispatch(in).verdict == Verdict::kBadFormat, "a radiance signal is RGBA");
  in = Dispatch();
  in.linear_depth.format = rr::Format::kRgba8Unorm;
  Check(PlanDispatch(in).verdict == Verdict::kBadFormat, "an 8 bit linear depth");
  // Diffuse signals need the diffuse albedo; AMD allows an empty one only without them.
  in = Dispatch();
  in.context_signals = in.dispatch_signals = FlagOf(Signal::kIndirectSpecular) | FlagOf(Signal::kIndirectDiffuse);
  in.signal_in[size_t(Signal::kIndirectDiffuse)] = At(1920, 1080, rr::Format::kRgba16Float);
  in.signal_out[size_t(Signal::kIndirectDiffuse)] = At(1920, 1080, rr::Format::kRgba16Float);
  Check(PlanDispatch(in).verdict == Verdict::kMissingInput, "an indirect diffuse signal without the diffuse albedo");
  in.diffuse_albedo = At(1920, 1080, rr::Format::kRgba16Float);
  Check(PlanDispatch(in).verdict == Verdict::kOk, "with the diffuse albedo: ok");
  // A single channel signal.
  in = Dispatch();
  in.context_signals = in.dispatch_signals = FlagOf(Signal::kSpecularOcclusion) | FlagOf(Signal::kIndirectSpecular);
  in.signal_in[size_t(Signal::kSpecularOcclusion)] = At(1920, 1080, rr::Format::kR16Float);
  in.signal_out[size_t(Signal::kSpecularOcclusion)] = At(1920, 1080, rr::Format::kR16Float);
  Check(PlanDispatch(in).verdict == Verdict::kOk, "a specular occlusion signal beside the indirect specular one: R16F");
}

// --- conventions ------------------------------------------------------------------------------------------------------------------------------------

// A tiny deterministic generator of unit vectors.
struct Lcg {
  uint32_t s = 12345u;
  float Next() {
    s = s * 1664525u + 1013904223u;
    return float(s >> 8) / float(1u << 24);
  }
};
gb::Vec3 RandomUnit(Lcg& g) {
  float x = g.Next() * 2 - 1, y = g.Next() * 2 - 1, z = g.Next() * 2 - 1;
  const float l = std::sqrt(x * x + y * y + z * z);
  if (l < 1e-3f) return {0, 0, 1};
  return {x / l, y / l, z / l};
}

void CheckNormals() {
  Lcg g;
  bool round_trip = true;
  bool same_map = true;
  double worst = 0;
  for (int i = 0; i < 20000; ++i) {
    const gb::Vec3 n = RandomUnit(g);
    const Vec2 uv = OctahedronUv({n.x, n.y, n.z});
    const Vec3 back = OctahedronUvToNormal(uv);
    const double err = std::sqrt(double(back.x - n.x) * (back.x - n.x) + double(back.y - n.y) * (back.y - n.y) + double(back.z - n.z) * (back.z - n.z));
    worst = std::max(worst, err);
    round_trip = round_trip && err < 1e-4 && uv.x >= 0 && uv.x <= 1 && uv.y >= 0 && uv.y <= 1;
    const gb::Vec2 game = gb::OctEncode(n);
    same_map = same_map && Near(game.x, uv.x, 1e-6) && Near(game.y, uv.y, 1e-6);
  }
  Check(round_trip, "the octahedral map round-trips 20,000 unit vectors (worst error " + std::to_string(worst) + ") and stays in [0, 1]");
  Check(same_map, "AMD's map is the G-buffer's (gbuffer_rules.h OctEncode): RT1's code can be handed over as it is");
  // The axis-aligned normals, where HLSL's sign(0) = 0 would fold a hemisphere wrong: the encoder here takes +1 at exactly 0.
  const gb::Vec3 axes[] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0.70710678f, 0, -0.70710678f}, {0, -0.70710678f, -0.70710678f}, {-0.70710678f, 0, -0.70710678f}};
  bool axis_ok = true;
  for (const gb::Vec3& n : axes) {
    const Vec3 back = OctahedronUvToNormal(OctahedronUv({n.x, n.y, n.z}));
    axis_ok = axis_ok && Near(back.x, n.x, 1e-5) && Near(back.y, n.y, 1e-5) && Near(back.z, n.z, 1e-5);
  }
  Check(axis_ok, "axis-aligned and exactly-zero-component normals (down and sideways) come back");
  const Vec2 up = OctahedronUv({0, 0, 1});
  Check(Near(up.x, 0.5) && Near(up.y, 0.5), "+z is the middle of the square");

  Check(Near(MaterialFraction(0), 0.0) && Near(MaterialFraction(1), 1.0 / 3.0) && Near(MaterialFraction(2), 2.0 / 3.0) && Near(MaterialFraction(3), 1.0) && Near(MaterialFraction(9), 1.0),
        "the material type is type / 3, clamped: 0 is the default");
  const NormalsTexel t = MakeNormalsTexel({0, 0, 1}, 0.4f, 0);
  Check(Near(t.u, 0.5) && Near(t.v, 0.5) && Near(t.roughness, 0.4) && Near(t.material, 0.0), "a texel: octahedral RG, linear roughness B, material 0 A");
  Check(MakeNormalsTexel({0, 0, 1}, 1.7f, 0).roughness == 1.0f && MakeNormalsTexel({0, 0, 1}, -0.2f, 0).roughness == 0.0f, "the roughness is clamped to [0, 1]");
  // Our G-buffer's smoothness -> the linear roughness (the same rule the RR guide uses).
  Check(Near(rr::LinearRoughnessOfSmoothness(0.75f), 0.25), "linear roughness is one minus smoothness");

  Check(Near(EncodeAlbedo(0.25f, false), 0.5) && Near(DecodeAlbedo(0.5f, false), 0.25) && Near(EncodeAlbedo(0.25f, true), 0.25) && Near(DecodeAlbedo(0.25f, true), 0.25),
        "albedo: sqrt by default, the identity with the non-encoded flag");
  bool albedo_round = true;
  for (int i = 0; i <= 100; ++i) {
    const float a = float(i) / 100.0f;
    albedo_round = albedo_round && Near(DecodeAlbedo(EncodeAlbedo(a, false), false), a, 1e-5) && Near(DecodeAlbedo(EncodeAlbedo(a, true), true), a, 1e-7);
  }
  Check(albedo_round, "albedo encode and decode are inverses in both encodings");
}

// The game's projection (temporal_camera.cpp): row vectors, clip z = z P22 + P32, clip w = z P23 + P33, the near plane at view z = -n (depth 1), the far plane at -f (depth 0), the camera looks along -z.
tmath::Mat4 GameProjection(double n, double f, double fov_y, double aspect) {
  tmath::Mat4 p;
  const double t = 1.0 / std::tan(fov_y * 0.5);
  p.m[0][0] = t / aspect;
  p.m[1][1] = t;
  p.m[2][2] = n / (f - n);
  p.m[2][3] = -1.0;
  p.m[3][2] = n * f / (f - n);
  p.m[3][3] = 0.0;
  return p;
}

// The game's view inverse for a camera at `pos` with right/up/forward axes: rows right, up, BACKWARD, position (c12..c15).
tmath::Mat4 GameViewInverse(const double right[3], const double up[3], const double fwd[3], const double pos[3]) {
  tmath::Mat4 m;
  for (int k = 0; k < 3; ++k) {
    m.m[0][k] = right[k];
    m.m[1][k] = up[k];
    m.m[2][k] = -fwd[k];
    m.m[3][k] = pos[k];
  }
  m.m[3][3] = 1.0;
  return m;
}

// A world point through the view matrix and the projection (row vectors): the view-space point, then the clip coordinates.
struct Seen {
  double view_z = 0, clip_w = 0, depth = 0;
};
Seen See(const tmath::Mat4& view, const tmath::Mat4& projection, const double world[3]) {
  double v[4] = {world[0], world[1], world[2], 1.0};
  double vs[4] = {};
  for (int j = 0; j < 4; ++j)
    for (int k = 0; k < 4; ++k) vs[j] += v[k] * view.m[k][j];
  double cs[4] = {};
  for (int j = 0; j < 4; ++j)
    for (int k = 0; k < 4; ++k) cs[j] += vs[k] * projection.m[k][j];
  Seen s;
  s.view_z = vs[2];
  s.clip_w = cs[3];
  s.depth = cs[2] / cs[3];
  return s;
}

void CheckGameViewSpace() {
  const double n = 5.0, f = 10000.0;
  const tmath::Mat4 p = GameProjection(n, f, 0.8, 16.0 / 9.0);
  Check(Near(ViewZOfDepth(1.0, p.m), -n, 1e-9) && Near(ViewZOfDepth(0.0, p.m), -f, 1e-6), "the stored depth 1 is the near plane (view z -5), 0 the far plane (-10000)");
  bool inverse = true;
  for (int i = 1; i < 200; ++i) {
    const double z = -n - (f - n) * i / 200.0 * (i / 200.0);
    const double depth = (z * p.m[2][2] + p.m[3][2]) / (z * p.m[2][3] + p.m[3][3]);
    inverse = inverse && Near(ViewZOfDepth(depth, p.m), z, std::fabs(z) * 1e-9 + 1e-9);
  }
  Check(inverse, "view z of the stored depth inverts the game's projection over the whole range");
  Check(!kLinearDepthPositiveAhead, "the dispatch uses the game's own view space: the signed linear depth is negative ahead (the sign a Radeon run confirms, RAY-REGEN.md section 8)");
  Check(SignedLinearDepth(1.0, p.m) < 0.0 && Near(SignedLinearDepth(0.5, p.m), ViewZOfDepth(0.5, p.m)), "signed linear depth is the view-space z");

  // A rotated camera that is not at the origin, as the RR-INT-REVIEW blocker had it: one view space for the matrices, the depth and the projection.
  const double a = 0.6;
  const double right[3] = {std::cos(a), 0, -std::sin(a)};
  const double up[3] = {0, 1, 0};
  const double fwd[3] = {std::sin(a), 0, std::cos(a)};
  const double pos[3] = {2903.0, 12.0, -418.0};
  const tmath::Mat4 inv = GameViewInverse(right, up, fwd, pos);
  tmath::Mat4 view;
  Check(tmath::Inverse(inv, &view), "the view inverse has an inverse");
  const double ahead[3] = {pos[0] + 10 * fwd[0], pos[1] + 10 * fwd[1], pos[2] + 10 * fwd[2]};
  const Seen s = See(view, p, ahead);
  Check(Near(s.view_z, -10.0, 1e-9), "a point 10 units ahead is at view z -10 in the game's own view matrix");
  Check(Near(s.clip_w, 10.0, 1e-9) && s.clip_w > 0, "and its clip w is +10: in front of the projection");
  Check(Near(SignedLinearDepth(s.depth, p.m), s.view_z, 1e-6), "the linear depth made from its stored depth is that view z: the two agree (one view space)");
  const double mirrored[3] = {pos[0] - 10 * fwd[0], pos[1] - 10 * fwd[1], pos[2] - 10 * fwd[2]};
  const Seen behind = See(view, p, mirrored);
  Check(behind.view_z > 0 && behind.clip_w < 0, "a point behind the camera is at positive view z and negative clip w");
  // The mirrored (+z) pair of AMD's example: the depth is clip w, positive ahead. Both consistent pairs give the same distance.
  Check(Near(-SignedLinearDepth(s.depth, p.m), s.clip_w, 1e-6), "the mirror's positive linear depth would be the clip w, the same distance");

  // The projection matrix is handed over as a copy of the row-major rows (no transpose): the layout FFX names for row vectors.
  float copy[16];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) copy[r * 4 + c] = float(p.m[r][c]);
  Check(Near(copy[11], -1.0) && Near(copy[10], p.m[2][2]) && Near(copy[14], p.m[3][2]), "the copy keeps clip w = -z in the fourth column and the depth terms in the third and fourth rows");
}

void CheckMotion() {
  const MotionScale s = MotionScaleFor(1920, 1080);
  Check(Near(s.x, 1.0 / 1920.0, 1e-9) && Near(s.y, 1.0 / 1080.0, 1e-9) && s.z == 1.0f, "pixel vectors become UV with a scale of 1 / size, the depth delta as it is");
  // A surface that moved 3 pixels right and 2 down since the last frame: the kept vector is previous minus current (-3, -2) pixels; AMD's PreviousUV - CurrentUV is the same in UV.
  const double kept[2] = {-3.0, -2.0};
  const double prev_uv[2] = {(100.0 - 3.0) / 1920.0, (200.0 - 2.0) / 1080.0};
  const double cur_uv[2] = {100.0 / 1920.0, 200.0 / 1080.0};
  Check(Near(kept[0] * s.x, prev_uv[0] - cur_uv[0], 1e-9) && Near(kept[1] * s.y, prev_uv[1] - cur_uv[1], 1e-9), "kept vector * scale = PreviousUV - CurrentUV");
  Check(Near(LinearDepthDelta(-12.0, -10.0), -2.0), "a surface that came 2 units closer (view z -12 then, -10 now): -2 (previous minus current, signed z)");
  Check(LinearDepthDelta(-10.0, -12.0) > 0, "and one that moved away: positive");
  const Vec3 d = CameraPositionDelta({10, 0, 5}, {12, 1, 5});
  Check(Near(d.x, -2) && Near(d.y, -1) && Near(d.z, 0), "the camera delta is previous minus current");
  const Bounds b = LinearDepthBounds(5.0, 10000.0);
  Check(InBounds(5.0, b) && InBounds(120.0, b) && !InBounds(10000.0, b) && !InBounds(4.9, b), "the bounds take the near plane and exclude the sky (stored depth 0 is the far plane)");
}

void CheckClock() {
  FrameClock c;
  FrameClock::Step s = c.Next(100, false);
  Check(s.reset && s.index == 1, "the first dispatch resets");
  s = c.Next(101, false);
  Check(!s.reset && s.index == 2, "the next game frame: no reset, the index goes up by one");
  s = c.Next(102, false);
  Check(!s.reset && s.index == 3, "and again");
  s = c.Next(110, false);
  Check(s.reset && s.index == 4, "a gap (frames another denoiser took, a pause): reset, and the index still goes up by exactly one");
  s = c.Next(111, true);
  Check(s.reset && s.index == 5, "a reset asked for (a camera cut) resets");
  s = c.Next(112, false);
  Check(!s.reset && s.index == 6, "and the frame after it does not");
  c.Forget();
  s = c.Next(113, false);
  Check(s.reset && s.index == 7, "after Forget (a context made anew) the next step resets");
  s = c.Next(113, false);
  Check(s.reset, "the same game frame twice is not the next one");
  Check(c.counter() == 8, "the counter counts every step");
}

// --- memory ----------------------------------------------------------------------------------------------------------------------------------------

void CheckMemory() {
  const uint32_t is = FlagOf(Signal::kIndirectSpecular);
  Check(EstimateBytes(is, 1920, 1080) == 282 * rr::kMiB, "indirect specular at 1080p: 282 MiB (AMD's table, RX 9070 XT)");
  Check(EstimateBytes(is, 960, 540) == 90 * rr::kMiB && EstimateBytes(is, 2560, 1440) == 462 * rr::kMiB, "and at 540p and 1440p: 90 and 462 MiB");
  Check(EstimateBytes(FlagOf(Signal::kIndirectDiffuse) | is, 1920, 1080) == 313 * rr::kMiB, "indirect diffuse + specular at 1080p: 313 MiB");
  Check(EstimateBytes(FlagOf(Signal::kDirectDiffuse) | FlagOf(Signal::kDirectSpecular) | FlagOf(Signal::kIndirectDiffuse) | is, 1920, 1080) == 377 * rr::kMiB, "all four radiance signals at 1080p: 377 MiB");
  Check(EstimateBytes(FlagOf(Signal::kDominantLight), 1920, 1080) == 72 * rr::kMiB && EstimateBytes(FlagOf(Signal::kAmbientOcclusion), 1920, 1080) == 72 * rr::kMiB, "a single occlusion or visibility signal: 72 MiB at 1080p");
  Check(EstimateBytes(FlagOf(Signal::kAmbientOcclusion) | is, 1920, 1080) == 377 * rr::kMiB, "a set the table does not list takes the largest row that holds it");
  Check(EstimateBytes(FlagOf(Signal::kDirectSpecular), 1920, 1080) == 377 * rr::kMiB, "direct specular alone is in no smaller row than the four-signal one");
  Check(EstimateBytes(0, 1920, 1080) == 0 && EstimateBytes(is, 0, 1080) == 0, "no signals or no size: nothing");
  bool monotonic = true;
  uint64_t last = 0;
  for (uint32_t w = 320; w <= 7680; w += 160) {
    const uint64_t b = EstimateBytes(is, w, w * 9 / 16);
    monotonic = monotonic && b >= last;
    last = b;
  }
  Check(monotonic, "the estimate never falls as the size grows");
  Check(EstimateBytes(is, 3840, 2160) > 462 * rr::kMiB && EstimateBytes(is, 3840, 2160) < 462 * rr::kMiB * 4, "4K is above 1440p's and in proportion to the pixels");
}

// --- the words ----------------------------------------------------------------------------------------------------------------------------------------

void CheckWords() {
  Check(std::string(kLauncherName) == "AMD FSR Ray Regeneration", "AMD's UI name");
  Check(std::strlen(kMenuName) == 13 && std::strlen(AutoLabel(true)) == 20, "the menu's value is 13 characters, AUTO's label 20: the page stays at 21 + 20 = 41");
  const char* names[] = {kLauncherName, kMenuName, kLauncherDescription, kCreditLine};
  bool no_version = true;
  for (const char* n : names) no_version = no_version && std::string(n).find("1.2") == std::string::npos && std::string(n).find("1.1") == std::string::npos && std::string(n).find("v1") == std::string::npos;
  Check(no_version, "no version number beside the name (AMD's naming guidelines)");
  Check(std::string(kLauncherDescription).rfind("AMD FSR Ray Regeneration infers and restores full-quality ray-traced detail from sparse samples.", 0) == 0, "the description starts with AMD's recommended sentence");
  bool widths = true;
  for (int g = 0; g < kGateCount; ++g) widths = widths && std::strlen(RowSuffix(Gate(g))) <= 13;
  Check(widths, "no row suffix is wider than 13 characters");
  Check(std::string(RowSuffix(Gate::kNotThisGpu)) == " (NO RX 9000)" && std::string(RowSuffix(Gate::kNotNeeded)) == " (NOT NEEDED)", "the suffixes the pause menu shows");
  Check(RowEnabled(Gate::kRuns) && RowEnabled(Gate::kStarting) && RowEnabled(Gate::kApplying) && !RowEnabled(Gate::kNotThisGpu) && !RowEnabled(Gate::kNotNeeded) && !RowEnabled(Gate::kSetup) &&
            !RowEnabled(Gate::kFailed) && !RowEnabled(Gate::kNotAvailable),
        "which gates keep the row enabled");
  bool reasons = true;
  for (int g = 0; g < kGateCount; ++g) {
    const bool on = RowEnabled(Gate(g));
    reasons = reasons && (std::strlen(LauncherReason(Gate(g))) == 0) == (g == int(Gate::kRuns) || g == int(Gate::kStarting));
    (void)on;
  }
  Check(reasons, "every gate but the two silent ones has a launcher line");
  Check(std::string(MenuNameOfRr(Vendor::kAmd)) == "FSR RAY REGEN" && std::string(MenuNameOfRr(Vendor::kNvidia)) == "DLSS RR", "the row's middle value is named by the vendor, stored as the same word");
  Check(std::string(AutoLabel(false)) == "AUTO (OURS)", "AUTO's label when it comes to ours");
}

void CheckGate() {
  Facts f;
  f.build_has = true;
  f.files_ok = true;
  f.device = DeviceVerdict::kOk;
  f.probed = true;
  f.supported = true;
  const ConsumerTable built = Cells(true, false, true, false, false, false, false, false);
  const uint32_t ssr = rr::ConsumerBit(rr::Consumer::kSsr);
  Check(GateOf(f, rr::DebugSwitch::kRow, ssr, false, built) == Gate::kRuns, "supported, an effect on: the row runs");
  Check(GateOf(f, rr::DebugSwitch::kRow, ssr, true, built) == Gate::kApplying, "the context is being made");
  Check(GateOf(f, rr::DebugSwitch::kRow, 0, false, built) == Gate::kNotNeeded, "nothing to denoise is on: not needed");
  Check(GateOf(f, rr::DebugSwitch::kOn, 0, false, built) == Gate::kRuns, "the debug switch on lifts the consumer rule");
  Check(GateOf(f, rr::DebugSwitch::kOff, ssr, false, built) == Gate::kNotAvailable, "the debug switch off: not available");
  Check(GateOf(f, rr::DebugSwitch::kRow, ssr, false) == Gate::kRuns && GateOf(f, rr::DebugSwitch::kRow, 0, false) == Gate::kNotNeeded &&
            GateOf(f, rr::DebugSwitch::kRow, rr::ConsumerBit(rr::Consumer::kCarRtReflections), false) == Gate::kNotNeeded,
        "the shipped table: the row runs while the reflections are on, and is grey 'not needed' with nothing on or only the cars' reflections (no signal writer)");
  const ConsumerTable none = Cells(false, false, false, false, false, false, false, false);
  Check(GateOf(f, rr::DebugSwitch::kRow, ssr, false, none) == Gate::kNotNeeded, "a table with no signal writer: not needed, nothing 'not available'");
  Facts nv = f;
  nv.device = DeviceVerdict::kNotAmd;
  Check(GateOf(nv, rr::DebugSwitch::kRow, ssr, false, built) == Gate::kNotThisGpu, "an NVIDIA PC's row is DLSS RR's, this one says: not this GPU");
  Check(GateOf(nv, rr::DebugSwitch::kRow, ssr, false) == Gate::kNotThisGpu, "the PC's reason comes before 'not needed': an NVIDIA PC with the shipped table is still 'not this GPU'");
  Check(GateOf(nv, rr::DebugSwitch::kOff, ssr, false, built) == Gate::kNotThisGpu, "and before the debug switch's 'off'");
  Facts vk = f;
  vk.device = DeviceVerdict::kNotD3D12;
  Check(GateOf(vk, rr::DebugSwitch::kRow, ssr, false, built) == Gate::kNeedsD3D12 && std::string(RowSuffix(Gate::kNeedsD3D12)) == " (D3D12 ONLY)" && RowEnabled(Gate::kNotThisGpu) == false && !RowEnabled(Gate::kNeedsD3D12),
        "a Radeon on Vulkan: AMD ships no Vulkan provider, the row says so (not 'NO RX 9000')");
  Facts sm = f;
  sm.device = DeviceVerdict::kShaderModelTooLow;
  Check(GateOf(sm, rr::DebugSwitch::kRow, ssr, false, built) == Gate::kNotAvailable, "a card or driver below shader model 6.6: not available");
  Check(GateOf(nv, rr::DebugSwitch::kRow, 0, false, built) == Gate::kNotThisGpu && GateOf(vk, rr::DebugSwitch::kOff, ssr, false, built) == Gate::kNeedsD3D12, "the PC's reason comes first, for Vulkan too");
  Facts loading = f;
  loading.probed = false;
  Check(GateOf(loading, rr::DebugSwitch::kRow, ssr, false, built) == Gate::kStarting, "the runtime is still opening: silent");
  Facts old = f;
  old.supported = false;
  Check(GateOf(old, rr::DebugSwitch::kRow, ssr, false, built) == Gate::kNotAvailable, "an RX 7000 card or an old driver: not available");
  Facts nofiles = f;
  nofiles.files_ok = false;
  Check(GateOf(nofiles, rr::DebugSwitch::kRow, ssr, false, built) == Gate::kSetup, "the DLL is missing: setup");
  Facts failed = f;
  failed.failed = true;
  Check(GateOf(failed, rr::DebugSwitch::kRow, ssr, false, built) == Gate::kFailed, "failed this session");
  Facts nobuild = f;
  nobuild.build_has = false;
  Check(GateOf(nobuild, rr::DebugSwitch::kRow, ssr, false, built) == Gate::kNotAvailable, "a build without it: not available");
}

// --- the DENOISER row's AMD value, end to end (RG-5) --------------------------------------------------------------------------------------------------

// The row on a Radeon RX 9000 with the shipped table: what each setting and each set of effects comes to, with the plan's own function (the row's gate asks the same one), and the live switch that needs
// no restart.
void CheckRowEndToEnd() {
  const uint32_t ssr = rr::ConsumerBit(rr::Consumer::kSsr);
  const uint32_t cars = rr::ConsumerBit(rr::Consumer::kCarRtReflections);
  const auto plan = [&](rr::Denoiser setting, uint32_t on_mask, DeviceVerdict device = DeviceVerdict::kOk) {
    PlanInputs in = Ready();
    in.setting = setting;
    in.facts.device = device;
    in.consumers_on = on_mask;
    in.consumers_active = AnyAutoConsumerOn(on_mask);
    in.consumers_any = AnyConsumerOn(on_mask);
    return SelectRayRegen(in);
  };
  Check(plan(rr::Denoiser::kRr, ssr).use && plan(rr::Denoiser::kRr, ssr).why == Why::kOn, "the row's AMD value with the reflections on: Ray Regeneration takes the frame");
  Check(!plan(rr::Denoiser::kAuto, ssr).use && plan(rr::Denoiser::kAuto, ssr).why == Why::kNoConsumer, "AUTO keeps today's choice (ours): nothing is trusted to AUTO until a Radeon A/B");
  Check(!plan(rr::Denoiser::kOurs, ssr).use && plan(rr::Denoiser::kOurs, ssr).why == Why::kSettingOurs, "OURS never");
  Check(plan(rr::Denoiser::kRr, 0).why == Why::kNoConsumer && plan(rr::Denoiser::kRr, cars).why == Why::kNoConsumer, "no effect it can clean is on (the cars' reflections have no signal writer): nothing to do");
  Check(plan(rr::Denoiser::kRr, ssr, DeviceVerdict::kNotAmd).why == Why::kNotThisGpu && plan(rr::Denoiser::kRr, ssr, DeviceVerdict::kNotD3D12).why == Why::kNotThisGpu &&
            plan(rr::Denoiser::kRr, ssr, DeviceVerdict::kShaderModelTooLow).why == Why::kNotThisGpu,
        "an RTX card, Vulkan, shader model below 6.6: the plan refuses, ours");
  // the live switch: the plan changes with the setting alone, and no reason of the plan is a restart
  bool no_restart = true;
  for (int w = 0; w <= int(Why::kGuidesNotReady); ++w) {
    std::string text = WhyText(Why(w));
    for (char& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
    no_restart = no_restart && text.find("restart") == std::string::npos;
  }
  Check(no_restart, "no reason of the plan asks for a restart: the runtime is opened when the row asks");
  Check(std::string(WhyText(Why::kFailed)).find("started again") != std::string::npos, "a session's failure waits for the next start, and says so");
  PlanInputs live = Ready();
  live.consumers_on = ssr;
  live.consumers_any = true;
  live.setting = rr::Denoiser::kOurs;
  const bool before = SelectRayRegen(live).use;
  live.setting = rr::Denoiser::kRr;
  const bool after = SelectRayRegen(live).use;
  live.setting = rr::Denoiser::kOurs;
  Check(!before && after && !SelectRayRegen(live).use, "switching the row there and back changes the plan at the next frame, with no restart");
  // the choice beside DLSS RR's plan: never both on one PC, ours when neither
  Check(ChooseDenoiser(plan(rr::Denoiser::kRr, ssr), rr::Plan{false, rr::Why::kBuildHasNoRr}) == DenoiserKind::kRayRegen && ChooseDenoiser(plan(rr::Denoiser::kAuto, ssr), rr::Plan{false, rr::Why::kBuildHasNoRr}) == DenoiserKind::kOurs,
        "a Radeon with the row on its value is Ray Regeneration's, with AUTO ours");
  // the context lives while the plan is on and for kContextHoldFrames after, then it is freed (only while the option is selected)
  rr::ContextKeeper keeper;
  bool held = true;
  for (int frame = 0; frame < 5; ++frame) held = held && keeper.Update(ContextWanted(Why::kOn));
  Check(held && keeper.keep(), "the context is wanted while the plan is on");
  int frames_kept = 0;
  while (keeper.Update(ContextWanted(Why::kSettingOurs)) && frames_kept < 5000) ++frames_kept;
  Check(frames_kept == int(rr::kContextHoldFrames) && !keeper.keep(), "after the row leaves its value the context is kept " + std::to_string(rr::kContextHoldFrames) + " frames, then freed");
  Check(ContextWanted(Why::kOn) && !ContextWanted(Why::kNoRoom) && !ContextWanted(Why::kFailed) && !ContextWanted(Why::kNotSupported) && !ContextWanted(Why::kNoConsumer) && !ContextWanted(Why::kNotThisGpu),
        "only a plan that is on wants the context: no room, a failure, a card that cannot, nothing to clean: none is made");
  // a refused dispatch is never a count against Ray Regeneration; three errors in a row drop it for the session (ours from then on)
  rr::FailureBook book;
  bool dropped = false;
  for (int i = 0; i < 10; ++i) dropped = dropped || book.Note(rr::FailureBook::Event::kRefused);
  Check(!dropped, "refused dispatches (the effect keeps its own filter for the frame) never drop the runtime");
  Check(!book.Note(rr::FailureBook::Event::kError) && !book.Note(rr::FailureBook::Event::kError) && book.Note(rr::FailureBook::Event::kError), "three errors in a row drop it for the session");
  PlanInputs failed = Ready();
  failed.consumers_on = ssr;
  failed.facts.failed = true;
  Check(SelectRayRegen(failed).why == Why::kFailed, "dropped: the plan says failed, ours");
  // the video memory: the row's context for the reflections at 1080p (the trace is half of it), and the room check says no where it does not fit
  Check(EstimateBytes(FlagOf(Signal::kIndirectSpecular), 960, 540) / rr::kMiB == 90, "the context for the 1080p trace's indirect specular is AMD's 90 MiB");
  PlanInputs no_room = Ready();
  no_room.consumers_on = ssr;
  no_room.room_ok = false;
  Check(SelectRayRegen(no_room).why == Why::kNoRoom, "no room in video memory: the effects keep their own denoisers");
}

}  // namespace

int main() {
  CheckIds();
  CheckSignals();
  CheckConsumers();
  CheckProviders();
  CheckProbeWanted();
  CheckDevice();
  CheckPlan();
  CheckChoice();
  CheckSignalModes();
  CheckInputMap();
  CheckDispatchPlan();
  CheckNormals();
  CheckGameViewSpace();
  CheckMotion();
  CheckClock();
  CheckMemory();
  CheckWords();
  CheckGate();
  CheckRowEndToEnd();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
