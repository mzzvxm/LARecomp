#pragma once
// ===========================================================================
// MCLA GPU renderer -- AMD FSR Ray Regeneration: the pure rules (perf/RAY-REGEN.md)
// ===========================================================================
// AMD's ML denoiser of the FSR Redstone family (FSR SDK 2.4.0: Ray Regeneration 1.2.1, amd_fidelityfx_denoiser_dx12.dll, RDNA 4 / Radeon RX 9000, Direct3D 12 with shader model 6.6,
// Windows 11; no Vulkan). What needs no AMD header, no Plume, no log and no state is here, so that a program of its own checks it (ray_regen_rules_test.cpp, target
// larecomp_ray_regen_rules_test). ray_regen.cpp turns these answers into FidelityFX API calls; the AMD constants below are pinned by the test against the values of ffx_denoiser.h / ffx_api.h
// (SDK 2.4.0) and by static_asserts in ray_regen.cpp against the real ones.
//
// WHAT IS DIFFERENT FROM DLSS RAY RECONSTRUCTION (dlss_rr_rules.h), and why this file is not a copy of it:
//   - Ray Regeneration is a DECOUPLED denoiser. It takes SEPARATE noisy signals at the render size (an indirect specular radiance, an indirect diffuse radiance, an occlusion ..., each with the first
//     bounce's albedo left OUT), and gives back denoised signals; the application multiplies them by the albedo and adds them to the picture, and an upscaler (FSR 4, DLSS Super Resolution, ours) runs
//     afterwards. DLSS RR takes the whole composed noisy colour and upscales it. So an effect hands Ray Regeneration its SIGNAL image (not "raw noise in the colour"), and the compose step is ours.
//   - It does not need DLSS or any upscaler, and it never replaces one: the upscaler choice is untouched, the denoiser runs at the effect's place in the frame, before the anchor.
//   - Its inputs: linear depth (signed view space z), 2.5D motion (UV delta + linear depth delta), normals (octahedral) + linear roughness + material type, the two albedos (sqrt-encoded, or raw with a
//     flag), the camera's world-to-view and unjittered projection, the jitter, the camera's position delta and a frame index. The hit distance rides in the ALPHA of an indirect signal.
//
//   Signals            the seven signals and which of our consumers (rr::Consumer) hands which one over.
//   Support            the device's part of "can it run" (AMD, Direct3D 12, shader model 6.6), the runtime's part is the version query and a trial context (ray_regen.cpp).
//   SelectRayRegen     the DENOISER row (AUTO / the vendor's value / OURS) against what the PC and the frame have; ChooseDenoiser puts it beside DLSS RR's plan: AMD Ray Regeneration where it can
//                      run, else DLSS RR, else ours (the owner's order, 2026-11-19, extended to AMD 2026-11-22).
//   SignalModes        which effects write their signal instead of adding their noise to the colour (the twin of rr::RawModesOf; no upscaled frame is needed).
//   InputMap           each Ray Regeneration input against the Ray Reconstruction guide we already make (shared, converted, new) and the guides Ray Regeneration has no use for.
//   PlanDispatch       the images of a dispatch against the context's signals: presence, sizes, formats (the twin of rr::PlanTags).
//   Conventions        octahedral normals, material type, albedo encoding, signed linear depth of the game's own view space, motion in UV space, the camera matrices, the frame index and reset.
//   EstimateBytes      the context's video memory, from AMD's table (RX 9070 XT), before the context exists.
//   Gate / labels      the row's words (AMD's naming guidelines: "AMD FSR Ray Regeneration", no version number).
//
// CHECK: nothing here was run against AMD's runtime (this PC has an RTX 5080 and a 2-CU RDNA 2 iGPU: no RDNA 4). The sign of the signed linear depth and the camera pair (section CONVENTIONS) are
// the first things a Radeon RX 9000 run has to confirm (perf/RAY-REGEN.md section 8).
// ===========================================================================

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "dlss_rr_rules.h"

namespace mcla::gpu::rayregen {

// --- AMD's ids (ffx_api.h, ffx_denoiser.h, FSR SDK 2.4.0) ------------------------------------------------------------------------------------------------

inline constexpr uint32_t kEffectIdDenoiser = 0x00050000u;  // FFX_API_EFFECT_ID_DENOISER
inline constexpr uint32_t kHeaderVersion = (1u << 22) | (2u << 12) | 1u;  // FFX_DENOISER_VERSION: Ray Regeneration 1.2.1
inline constexpr uint32_t kDispatchReset = 1u << 0;            // FFX_DENOISER_DISPATCH_RESET
inline constexpr uint32_t kDispatchNonEncodedAlbedo = 1u << 1; // FFX_DENOISER_DISPATCH_NON_ENCODED_ALBEDO (kept under the name that SDK 2.4.0 added; FFX_DENOISER_DISPATCH_NON_GAMMA_ALBEDO is its deprecated alias)
inline constexpr uint32_t kMaterialTypes = 4;                  // material types 0..3 (0 is the default)
inline constexpr uint32_t kVendorAmd = 0x1002;                 // PCI vendor id of AMD
inline constexpr uint32_t kShaderModelMinimum = 0x66;          // shader model 6.6 (D3D_SHADER_MODEL_6_6)

// --- the signals -----------------------------------------------------------------------------------------------------------------------------------------

// The seven signals in the order of FfxApiDenoiserSignalFlags' bits (1 << index).
enum class Signal : uint8_t {
  kAmbientOcclusion,     // R [0,1], 0 occluded: works best with an indirect diffuse beside it
  kDirectDiffuse,        // RGB radiance, no first-bounce albedo
  kDirectSpecular,       // RGB radiance, no first-bounce albedo
  kDominantLight,        // R in: the ray's hit distance (FP16_MAX = unshadowed), out: visibility [0,1]; needs the light's direction, emission and angular radius
  kIndirectDiffuse,      // RGB radiance, A in: the hit distance
  kIndirectSpecular,     // RGB radiance, A in: the hit distance
  kSpecularOcclusion,    // R [0,1]: works best with an indirect specular beside it
  kCount,
};
inline constexpr size_t kSignalCount = size_t(Signal::kCount);
constexpr uint32_t FlagOf(Signal s) { return 1u << uint32_t(s); }
constexpr bool HasFlag(uint32_t flags, Signal s) { return (flags & FlagOf(s)) != 0; }
inline constexpr uint32_t kAllSignalFlags = (1u << kSignalCount) - 1u;
constexpr const char* SignalName(Signal s) {
  switch (s) {
    case Signal::kAmbientOcclusion: return "ambient occlusion";
    case Signal::kDirectDiffuse: return "direct diffuse";
    case Signal::kDirectSpecular: return "direct specular";
    case Signal::kDominantLight: return "dominant light visibility";
    case Signal::kIndirectDiffuse: return "indirect diffuse";
    case Signal::kIndirectSpecular: return "indirect specular";
    case Signal::kSpecularOcclusion: return "specular occlusion";
    case Signal::kCount: break;
  }
  return "?";
}
// What a signal's images are: RGB(+A) radiance signals are RGBA16F (the alpha of an indirect one is the hit distance), the single channel ones R16F (or R8).
constexpr bool IsRadiance(Signal s) {
  return s == Signal::kDirectDiffuse || s == Signal::kDirectSpecular || s == Signal::kIndirectDiffuse || s == Signal::kIndirectSpecular;
}
constexpr bool CarriesHitDistanceInAlpha(Signal s) { return s == Signal::kIndirectDiffuse || s == Signal::kIndirectSpecular; }
// The albedo the signal's first bounce is multiplied by after the denoise.
enum class AlbedoOf : uint8_t { kNone, kDiffuse, kSpecular };
constexpr AlbedoOf AlbedoOfSignal(Signal s) {
  switch (s) {
    case Signal::kDirectDiffuse:
    case Signal::kIndirectDiffuse: return AlbedoOf::kDiffuse;
    case Signal::kDirectSpecular:
    case Signal::kIndirectSpecular: return AlbedoOf::kSpecular;
    default: return AlbedoOf::kNone;
  }
}

// The signals ray_regen.cpp drives. The dominant light visibility is left out of the first cut: it needs the light's direction, emission and angular radius in the dispatch (the city's sun shadow and the RT
// lights are its consumers, a later package); a context for it is refused with a plain reason.
inline constexpr uint32_t kDrivenSignals = kAllSignalFlags & ~FlagOf(Signal::kDominantLight);
constexpr uint32_t UndrivenSignals(uint32_t flags) { return flags & ~kDrivenSignals; }

// The consumers of the DENOISER row (dlss_rr_rules.h rr::Consumer) and the signal each hands Ray Regeneration:
//   SSR (reflections on surfaces)        indirect specular: the trace's hit radiance, the resolve's gain and Fresnel are the specular albedo's job after the denoise
//   the cars' ray traced reflections     indirect specular (the paint reads the denoised image; it is read at the car's draw, so its dispatch is earlier than the anchor)
//   the city's ray traced reflections    indirect specular
//   ray traced bounce light              indirect diffuse (an ambient occlusion beside it is the AO signal, a later package)
//   ray traced light visibility          the DOMINANT LIGHT signal (RT-LIGHTS-D1 is a DLSS RR consumer since the integration; Ray Regeneration denoises a visibility, which DLSS RR cannot, but ray_regen.cpp does not drive
//                                        that signal yet: UndrivenSignals, and the cell below has no writer)
// A light visibility multiplier (the RT lights, the city's sun shadow) is the DOMINANT LIGHT signal.
constexpr Signal SignalOf(rr::Consumer c) {
  switch (c) {
    case rr::Consumer::kSsr:
    case rr::Consumer::kCarRtReflections:
    case rr::Consumer::kCityRtReflections: return Signal::kIndirectSpecular;
    case rr::Consumer::kRtGi: return Signal::kIndirectDiffuse;
    case rr::Consumer::kRtLights: return Signal::kDominantLight;
    case rr::Consumer::kCount: break;
  }
  return Signal::kIndirectSpecular;
}

// Two flags each, decided per effect by measurement, never by the row (the twin of rr::ConsumerInfo):
//   signal_built   the effect can write its signal image and take the denoised one back (RG-3 .. RG-5). Until it can it is not a consumer: it keeps its own denoiser and its "on" does not count.
//   auto_uses      AUTO trusts Ray Regeneration with it: the tester's A/B pictures and the cost said it beats ours there. The row's own value takes every consumer with a signal mode.
struct ConsumerCell {
  bool signal_built;
  bool auto_uses;
};
using ConsumerTable = std::array<ConsumerCell, rr::kConsumerCount>;
// TODAY (RG-3): the screen-space reflections have their signal writer (ray_regen_guides.h, hlsl/ray_regen_ssr_cs.hlsl: the trace's radiance and hit distance and the guides in AMD's formats, composed by the
// reflections' own resolve), so signal_built is true for them and the DENOISER row's AMD value takes them; auto_uses stays false for every effect until a Radeon RX 9000 A/B (RAY-REGEN.md section 8: the
// DLL costs 0.7 ms at 960x540 (the reflections' trace of a 1080p picture: the dispatch is at the trace's size) and 2.6 ms at 1080p (a 4K picture's) for one specular signal on an RX 9070 XT, against our own denoisers' 0.03 ms), so AUTO is ours on every PC.
//   the cars' ray traced reflections: NOT a fit, so the cell stays false (RAY-REGEN.md section 4.2): their image is read by the NEXT frame's paint at the texels of the car's history image, with a weight
//     code in the alpha that a hit distance has no room for, and a denoiser that costs the same for the whole picture (2.6 ms) would run for a car that fills a tenth of it, to replace a filter that costs 0.03 ms.
//   the city's ray traced reflections, the ray traced bounce light, the lights' visibility: their effects are in the integrated tree (CITY-RT R6 / G6, RT-LIGHTS-D1) as DLSS RR consumers, but none has a
//     writer of its Ray Regeneration signal (indirect specular / indirect diffuse / dominant light visibility) or a compose step yet; each flips its cell when it gets them.
inline constexpr ConsumerTable kConsumers = {{
    {true, false},   // screen-space reflections
    {false, false},  // ray traced car reflections
    {false, false},  // ray traced city reflections
    {false, false},  // ray traced bounce light
    {false, false},  // ray traced light visibility (rr::Consumer::kRtLights, added by RT-LIGHTS-D1: the table follows rr::kConsumerCount)
}};
constexpr bool AnySignalBuilt(const ConsumerTable& table = kConsumers) {
  for (const ConsumerCell& c : table) {
    if (c.signal_built) return true;
  }
  return false;
}
constexpr bool AnyConsumerOn(uint32_t on_mask, const ConsumerTable& table = kConsumers) {
  for (size_t i = 0; i < rr::kConsumerCount; ++i) {
    if (((on_mask >> i) & 1u) != 0 && table[i].signal_built) return true;
  }
  return false;
}
constexpr bool AnyAutoConsumerOn(uint32_t on_mask, const ConsumerTable& table = kConsumers) {
  for (size_t i = 0; i < rr::kConsumerCount; ++i) {
    if (((on_mask >> i) & 1u) != 0 && table[i].signal_built && table[i].auto_uses) return true;
  }
  return false;
}
// Under AUTO only the effects with auto_uses are taken (an effect AUTO does not trust keeps its own filter and writes no signal), so a dispatch carries exactly their signals: the context is made for
// those. The row's own value (and the debug switch's "on") takes every effect with a signal mode.
constexpr uint32_t ContextSignals(rr::Denoiser setting, rr::DebugSwitch debug, uint32_t on_mask, const ConsumerTable& table = kConsumers) {
  const bool auto_row = debug == rr::DebugSwitch::kRow && setting == rr::Denoiser::kAuto;
  uint32_t flags = 0;
  for (size_t i = 0; i < rr::kConsumerCount; ++i) {
    if (((on_mask >> i) & 1u) == 0 || !table[i].signal_built) continue;
    if (auto_row && !table[i].auto_uses) continue;
    flags |= FlagOf(SignalOf(rr::Consumer(i)));
  }
  return flags;
}

// Whether the runtime is opened at the start: the debug switch (mcla_gpu_ray_regen): off never, probe and on always (the device check inside Probe keeps every PC but a Radeon RX 9000 out), auto only where an
// effect has a signal mode to hand it (ConsumerTable has a built cell) and the DENOISER row is not OURS (OURS opens nothing: no memory, no start time). A row that leaves OURS later opens it live (the plan
// asks ray_regen.h ProbeNow): Ray Regeneration needs no restart.
constexpr bool ProbeWanted(rr::DebugSwitch debug, bool any_signal_built, bool row_is_ours = false) {
  switch (debug) {
    case rr::DebugSwitch::kOff: return false;
    case rr::DebugSwitch::kProbe:
    case rr::DebugSwitch::kOn: return true;
    case rr::DebugSwitch::kRow: return any_signal_built && !row_is_ours;
  }
  return false;
}

// --- the device ------------------------------------------------------------------------------------------------------------------------------------------

struct DeviceInputs {
  uint32_t vendor_id = 0;
  bool d3d12 = false;           // the renderer runs on Direct3D 12 (Ray Regeneration has no Vulkan provider)
  uint32_t shader_model = 0;    // highest the runtime and the driver offer (0x66 = 6.6)
};
enum class DeviceVerdict : uint8_t { kOk, kNotAmd, kNotD3D12, kShaderModelTooLow };
constexpr DeviceVerdict DeviceOk(const DeviceInputs& d) {
  if (d.vendor_id != kVendorAmd) return DeviceVerdict::kNotAmd;
  if (!d.d3d12) return DeviceVerdict::kNotD3D12;
  if (d.shader_model < kShaderModelMinimum) return DeviceVerdict::kShaderModelTooLow;
  return DeviceVerdict::kOk;
}
constexpr const char* DeviceVerdictText(DeviceVerdict v) {
  switch (v) {
    case DeviceVerdict::kOk: return "an AMD GPU on Direct3D 12 with shader model 6.6";
    case DeviceVerdict::kNotAmd: return "not an AMD graphics card";
    case DeviceVerdict::kNotD3D12: return "AMD FSR Ray Regeneration has no Vulkan provider: Direct3D 12 only";
    case DeviceVerdict::kShaderModelTooLow: return "the graphics card or driver offers a shader model below 6.6";
  }
  return "?";
}

// The runtime's list of providers (ffxQueryDescGetVersions): the context is pinned to one (an override must be used consistently, AMD's FSR API guide), the newest one the application ships; a provider the
// driver offers ("trailing *" in its name or bit 31 of its id) is taken only when the list has no shipped one. Ties and unparsed versions keep the first of the list. -1: the list is empty.
struct ProviderVersion {
  uint64_t id = 0;
  int major = 0, minor = 0, patch = 0;
  bool driver_provided = false;
};
constexpr bool IsDriverProvided(uint64_t id, std::string_view name) { return (id & (1ull << 31)) != 0 || (!name.empty() && name.back() == '*'); }
constexpr bool NewerVersion(const ProviderVersion& a, const ProviderVersion& b) {
  if (a.major != b.major) return a.major > b.major;
  if (a.minor != b.minor) return a.minor > b.minor;
  return a.patch > b.patch;
}
inline int PickProvider(const ProviderVersion* list, size_t count) {
  int best = -1;
  for (size_t i = 0; i < count; ++i) {
    if (best < 0) {
      best = int(i);
      continue;
    }
    const ProviderVersion& cur = list[best];
    const ProviderVersion& cand = list[i];
    if (cur.driver_provided != cand.driver_provided) {
      if (cur.driver_provided) best = int(i);  // a shipped one beats a driver's
      continue;
    }
    if (NewerVersion(cand, cur)) best = int(i);
  }
  return best;
}

// --- who does the denoising ------------------------------------------------------------------------------------------------------------------------------

// What the PC and the build can do (published by ray_regen.cpp).
struct Facts {
  bool build_has = false;       // LARECOMP_HAS_RAY_REGEN: the headers and the DLL were fetched
  bool files_ok = false;        // the loader and the denoiser DLL are next to the exe (a check that ran and said no is false)
  DeviceVerdict device = DeviceVerdict::kNotAmd;
  bool probed = false;          // the runtime was opened and asked (the version query and the trial context are done)
  bool supported = false;       // the runtime lists a denoiser provider for this device and a trial context was made
  bool failed = false;          // dropped for this session: a context could not be made, or three dispatches in a row failed
};

enum class Why : uint8_t {
  kOn,
  kSwitchOff,         // the debug switch is off
  kSettingOurs,       // the row says OURS
  kNoConsumer,        // AUTO and nothing AUTO trusts is on, or the row's value and nothing with a signal mode is on
  kBuildHasNone,
  kNotThisGpu,        // facts.device is not kOk (not AMD, not Direct3D 12, shader model below 6.6)
  kFilesMissing,
  kNotProbed,         // the runtime is still being opened
  kNotSupported,      // the runtime lists no provider for this device (RDNA 3 and older, an old driver) or the trial context failed
  kFailed,
  kProbeOnly,
  kNoRoom,
  kGuidesNotReady,
};
// The context exists exactly while the plan is on and for kContextHoldFrames after (rr::ContextKeeper): created only while the option is selected and an effect that uses it runs, freed when it is not.
constexpr bool ContextWanted(Why w) { return w == Why::kOn; }
constexpr const char* WhyText(Why w) {
  switch (w) {
    case Why::kOn: return "AMD FSR Ray Regeneration is on";
    case Why::kSwitchOff: return "mcla_gpu_ray_regen is off";
    case Why::kSettingOurs: return "the setting is OURS";
    case Why::kNoConsumer: return "no effect that Ray Regeneration denoises is on (AUTO needs one it cleans better than ours, the row's value any with a signal mode)";
    case Why::kBuildHasNone: return "this build has no AMD FSR Ray Regeneration";
    case Why::kNotThisGpu: return "this graphics card cannot run AMD FSR Ray Regeneration";
    case Why::kFilesMissing: return "amd_fidelityfx_loader_dx12.dll or amd_fidelityfx_denoiser_dx12.dll is not next to the exe";
    case Why::kNotProbed: return "the FidelityFX runtime has not finished opening";
    case Why::kNotSupported: return "the FidelityFX runtime offers no Ray Regeneration for this graphics card or driver (it needs a Radeon RX 9000 series)";
    case Why::kFailed: return "Ray Regeneration failed in this session (the log says why) and stays off until the game is started again";
    case Why::kProbeOnly: return "mcla_gpu_ray_regen is probe: the runtime is opened and asked, no frame uses it";
    case Why::kNoRoom: return "there is no room in video memory for the Ray Regeneration context";
    case Why::kGuidesNotReady: return "the guide images are not made yet";
  }
  return "?";
}

struct PlanInputs {
  rr::Denoiser setting = rr::Denoiser::kAuto;  // the DENOISER row: AUTO, the vendor's value ("rr"), OURS
  rr::DebugSwitch debug = rr::DebugSwitch::kRow;  // the debug cvar's: off, probe, on (the row's value with the consumer rule lifted), auto (the row decides)
  Facts facts;
  uint32_t consumers_on = 0;       // rr::Consumer bits of the effects that run now
  bool consumers_active = false;   // AUTO's: an effect with auto_uses is on
  bool consumers_any = false;      // the row's value's: an effect with a signal mode is on
  bool guides_ready = false;       // this frame's guide images exist (RG-2)
  bool room_ok = true;
};

struct Plan {
  bool use = false;
  Why why = Why::kSwitchOff;
};

// The first reason that applies is the answer: the same order as rr::SelectDenoiser, so that the row asks one function and the two plans never disagree about what the PC can do.
inline Plan SelectRayRegen(const PlanInputs& in) {
  const auto no = [](Why why) { return Plan{false, why}; };
  if (in.debug == rr::DebugSwitch::kOff) return no(Why::kSwitchOff);
  const rr::Denoiser setting = in.debug == rr::DebugSwitch::kOn ? rr::Denoiser::kRr : in.setting;
  if (setting == rr::Denoiser::kOurs) return no(Why::kSettingOurs);
  if (in.debug == rr::DebugSwitch::kRow) {
    if (setting == rr::Denoiser::kAuto && !in.consumers_active) return no(Why::kNoConsumer);
    if (setting == rr::Denoiser::kRr && !in.consumers_active && !in.consumers_any) return no(Why::kNoConsumer);
  }
  if (!in.facts.build_has) return no(Why::kBuildHasNone);
  if (in.facts.device != DeviceVerdict::kOk) return no(Why::kNotThisGpu);
  if (!in.facts.files_ok) return no(Why::kFilesMissing);
  if (!in.facts.probed) return no(Why::kNotProbed);
  if (!in.facts.supported) return no(Why::kNotSupported);
  if (in.facts.failed) return no(Why::kFailed);
  if (in.debug == rr::DebugSwitch::kProbe) return no(Why::kProbeOnly);
  if (!in.room_ok) return no(Why::kNoRoom);
  if (!in.guides_ready) return no(Why::kGuidesNotReady);
  return Plan{true, Why::kOn};
}

// Which denoiser takes the frame: AMD FSR Ray Regeneration where it can run, else NVIDIA DLSS Ray Reconstruction, else ours. One adapter is one vendor, so at most one of the two plans is ever on;
// the order only says what a PC would do if both were (an NVIDIA card and an AMD one in the same renderer cannot happen today).
enum class DenoiserKind : uint8_t { kOurs, kRayRegen, kDlssRr };
constexpr const char* DenoiserKindName(DenoiserKind b) {
  switch (b) {
    case DenoiserKind::kOurs: return "ours";
    case DenoiserKind::kRayRegen: return "AMD FSR Ray Regeneration";
    case DenoiserKind::kDlssRr: return "NVIDIA DLSS Ray Reconstruction";
  }
  return "?";
}
constexpr DenoiserKind ChooseDenoiser(const Plan& amd, const rr::Plan& nvidia) {
  if (amd.use) return DenoiserKind::kRayRegen;
  if (nvidia.use_rr) return DenoiserKind::kDlssRr;
  return DenoiserKind::kOurs;
}

// The vendor decides the NAME of the row's middle value (the stored word stays "rr", so a saved setting survives a change of graphics card).
enum class Vendor : uint8_t { kAmd, kNvidia, kOther };
constexpr Vendor VendorOf(uint32_t pci_vendor_id) { return pci_vendor_id == kVendorAmd ? Vendor::kAmd : (pci_vendor_id == 0x10DEu ? Vendor::kNvidia : Vendor::kOther); }

// --- a frame's effects: signal mode ------------------------------------------------------------------------------------------------------------------------

// The twin of rr::RawModesOf. An effect writes its SIGNAL image (radiance without the albedo, the hit distance in the alpha) and takes the denoised one back for its compose step when the plan says
// Ray Regeneration, its context is made, nothing failed and the frame is the play view. Unlike DLSS RR no upscaled frame is needed: Ray Regeneration denoises at the render size whatever the upscaler
// does, so `armed` is not asked. The row's AUTO takes only the effects with auto_uses (an effect it does not trust keeps its own filter in a frame Ray Regeneration takes for another one).
// An effect with no signal writer (signal_built false) is never taken, whatever the plan says: it keeps its own filter (reported as kPlanOurs: "not Ray Regeneration's").
constexpr rr::RawWhy SignalWhyOf(rr::RawFrame f, bool switch_on, bool auto_uses, bool signal_built = true) {
  if (!signal_built) return rr::RawWhy::kPlanOurs;
  f.armed = true;
  return rr::RawWhyOf(f, switch_on, auto_uses);
}
constexpr rr::RawModes SignalModesOf(const rr::RawFrame& f, bool switch_ssr, bool switch_car_refl, const ConsumerTable& table = kConsumers) {
  const ConsumerCell& ssr = table[size_t(rr::Consumer::kSsr)];
  const ConsumerCell& car = table[size_t(rr::Consumer::kCarRtReflections)];
  return {SignalWhyOf(f, switch_ssr, ssr.auto_uses, ssr.signal_built), SignalWhyOf(f, switch_car_refl, car.auto_uses, car.signal_built)};
}

// --- the inputs: what we already make, what is new -----------------------------------------------------------------------------------------------------------

// The five inputs of ffxDispatchDescDenoiser every dispatch carries (the signals' images come with the effects).
enum class Input : uint8_t { kLinearDepth, kMotionVectors, kNormals, kSpecularAlbedo, kDiffuseAlbedo, kCount };
inline constexpr size_t kInputCount = size_t(Input::kCount);
constexpr const char* InputName(Input i) {
  switch (i) {
    case Input::kLinearDepth: return "linear depth";
    case Input::kMotionVectors: return "motion vectors";
    case Input::kNormals: return "normals + roughness + material";
    case Input::kSpecularAlbedo: return "specular albedo";
    case Input::kDiffuseAlbedo: return "diffuse albedo";
    case Input::kCount: break;
  }
  return "?";
}
// How an input comes from the Ray Reconstruction guides (rr_guides.h) and the frame's kept images.
enum class Share : uint8_t {
  kShared,     // the very image the RR guide pass makes (no copy; the formats are the same)
  kConverted,  // a pass converts a guide / kept image into the layout Ray Regeneration reads
};
struct InputMapEntry {
  Input input;
  rr::Buffer source;   // the guide or kept image it is made from (dlss_rr_rules.h Buffer)
  Share share;
  const char* note;
};
inline constexpr std::array<InputMapEntry, kInputCount> kInputMap = {{
    {Input::kLinearDepth, rr::Buffer::kDepth, Share::kConverted, "kept depth R32_FLOAT (1 near, 0 far) -> signed view-space z of the game's own camera (negative ahead), the sky outside linearDepthBounds"},
    {Input::kMotionVectors, rr::Buffer::kMotion, Share::kConverted, "kept RG16F render pixels (previous minus current) kept as they are with motionVectorScale 1/size; B = previous minus current linear depth (the previous frame's linear depth image)"},
    {Input::kNormals, rr::Buffer::kNormalRoughness, Share::kConverted, "world normal xyz + linear roughness -> octahedral RG, linear roughness B, material type 0 A (the G-buffer's own RT1 code is the same octahedral map)"},
    {Input::kSpecularAlbedo, rr::Buffer::kSpecularAlbedo, Share::kShared, "the RR guide's RGBA16F (Ray Tracing Gems chapter 32 approximation: the one AMD's guide lists) with kDispatchNonEncodedAlbedo"},
    {Input::kDiffuseAlbedo, rr::Buffer::kDiffuseAlbedo, Share::kShared, "the RR guide's RGBA16F linear albedo with kDispatchNonEncodedAlbedo (metals' diffuse should be zero: BaseColor * (1 - Metalness))"},
}};
constexpr const InputMapEntry& MapOf(Input i) { return kInputMap[size_t(i)]; }
// The RR guides Ray Regeneration has no input for, with what stands in for each.
struct UnusedGuide {
  rr::Buffer guide;
  const char* instead;
};
inline constexpr std::array<UnusedGuide, 3> kUnusedGuides = {{
    {rr::Buffer::kSpecularHitDistance, "rides in the alpha of the indirect signals (the effect's own hit image is merged into its signal image)"},
    {rr::Buffer::kResponsivity, "none: Ray Regeneration has no responsivity input (a decal or particle pixel keeps the temporal weight of its surface)"},
    {rr::Buffer::kColorBeforeTransparency, "none: the denoiser sees signals, not the colour, so a transparency layer cannot ghost inside it"},
}};
// Whether a Ray Reconstruction guide is shared with Ray Regeneration unchanged.
constexpr bool GuideIsShared(rr::Buffer b) {
  for (const InputMapEntry& e : kInputMap) {
    if (e.source == b && e.share == Share::kShared) return true;
  }
  return false;
}
// The dispatch flags: reset (the frame is not the one after the last dispatch, a camera cut, the first one) and the albedo encoding (the shared RR guides are linear: the identity encoding).
constexpr uint32_t DispatchFlags(bool reset, bool shared_linear_albedo = true) {
  return (reset ? kDispatchReset : 0u) | (shared_linear_albedo ? kDispatchNonEncodedAlbedo : 0u);
}

// --- the images of a dispatch --------------------------------------------------------------------------------------------------------------------------------

struct Inputs {
  rr::Image linear_depth, motion, normals, specular_albedo, diffuse_albedo;
  std::array<rr::Image, kSignalCount> signal_in{};   // each signal's noisy input
  std::array<rr::Image, kSignalCount> signal_out{};  // and the denoised image it is written to
  uint32_t render_size[2] = {};
  uint32_t context_max_size[2] = {};                 // the size the context was made for (the render size may not exceed it)
  uint32_t context_signals = 0;                      // the context's signalFlags
  uint32_t dispatch_signals = 0;                     // the signals this dispatch carries a description for
};
enum class Verdict : uint8_t { kOk, kNoSignals, kSignalMismatch, kMissingInput, kMissingSignal, kBadSize, kBadFormat, kTooBig };
constexpr const char* VerdictText(Verdict v) {
  switch (v) {
    case Verdict::kOk: return "ok";
    case Verdict::kNoSignals: return "the dispatch carries no signal";
    case Verdict::kSignalMismatch: return "the dispatch's signals are not the context's (every signal of the context is dispatched, none else)";
    case Verdict::kMissingInput: return "a common input image is missing";
    case Verdict::kMissingSignal: return "a signal has no input or no output image";
    case Verdict::kBadSize: return "an image is not the render size";
    case Verdict::kBadFormat: return "an image has a format Ray Regeneration does not take";
    case Verdict::kTooBig: return "the render size is larger than the size the context was made for";
  }
  return "?";
}
struct DispatchPlan {
  Verdict verdict = Verdict::kNoSignals;
  const char* culprit = "";
};

// AMD: the specular albedo may be left out only when no specular signal and no dominant light visibility is set; the diffuse albedo only when no diffuse signal and no dominant light is.
constexpr bool NeedsSpecularAlbedo(uint32_t signals) { return HasFlag(signals, Signal::kDirectSpecular) || HasFlag(signals, Signal::kIndirectSpecular) || HasFlag(signals, Signal::kDominantLight); }
constexpr bool NeedsDiffuseAlbedo(uint32_t signals) { return HasFlag(signals, Signal::kDirectDiffuse) || HasFlag(signals, Signal::kIndirectDiffuse) || HasFlag(signals, Signal::kDominantLight); }

constexpr bool FormatOkInput(Input i, rr::Format f) {
  switch (i) {
    case Input::kLinearDepth: return f == rr::Format::kR32Float || f == rr::Format::kR16Float;
    case Input::kMotionVectors: return f == rr::Format::kRgba16Float || f == rr::Format::kRgba32Float;
    case Input::kNormals: return f == rr::Format::kRgba16Float || f == rr::Format::kRgba32Float || f == rr::Format::kRgba8Unorm || f == rr::Format::kRgb10A2Unorm;
    case Input::kSpecularAlbedo:
    case Input::kDiffuseAlbedo: return f == rr::Format::kRgba16Float || f == rr::Format::kRgba32Float || f == rr::Format::kRgba8Unorm || f == rr::Format::kRgb10A2Unorm;
    case Input::kCount: break;
  }
  return false;
}
constexpr bool FormatOkSignal(Signal s, rr::Format f) {
  if (IsRadiance(s)) return f == rr::Format::kRgba16Float || f == rr::Format::kRgba32Float;
  return f == rr::Format::kR16Float || f == rr::Format::kR32Float || f == rr::Format::kR8Unorm;
}

// The dispatch's images against the context. Every signal of the context is dispatched with its input and output image at the render size, none that the context lacks; the common inputs are all
// there at the render size (the albedos only where a signal reads them); the formats are the ones above. The first problem found is the verdict (the log names it).
inline DispatchPlan PlanDispatch(const Inputs& in) {
  DispatchPlan plan;
  const auto refuse = [&plan](Verdict v, const char* culprit) {
    plan.verdict = v;
    plan.culprit = culprit;
    return plan;
  };
  if (in.dispatch_signals == 0) return refuse(Verdict::kNoSignals, "");
  if (in.dispatch_signals != in.context_signals) return refuse(Verdict::kSignalMismatch, "signal flags");
  if (in.render_size[0] == 0 || in.render_size[1] == 0) return refuse(Verdict::kBadSize, "render size");
  if (in.render_size[0] > in.context_max_size[0] || in.render_size[1] > in.context_max_size[1]) return refuse(Verdict::kTooBig, "render size");
  struct Common {
    Input input;
    const rr::Image* image;
    bool required;
  };
  const Common common[kInputCount] = {
      {Input::kLinearDepth, &in.linear_depth, true},
      {Input::kMotionVectors, &in.motion, true},
      {Input::kNormals, &in.normals, true},
      {Input::kSpecularAlbedo, &in.specular_albedo, NeedsSpecularAlbedo(in.dispatch_signals)},
      {Input::kDiffuseAlbedo, &in.diffuse_albedo, NeedsDiffuseAlbedo(in.dispatch_signals)},
  };
  for (const Common& c : common) {
    if (c.required && !c.image->present) return refuse(Verdict::kMissingInput, InputName(c.input));
  }
  for (size_t s = 0; s < kSignalCount; ++s) {
    if (!HasFlag(in.dispatch_signals, Signal(s))) continue;
    if (!in.signal_in[s].present || !in.signal_out[s].present) return refuse(Verdict::kMissingSignal, SignalName(Signal(s)));
  }
  const auto size_ok = [&in](const rr::Image& image) { return image.width == in.render_size[0] && image.height == in.render_size[1]; };
  for (const Common& c : common) {
    if (!c.required) continue;
    if (!size_ok(*c.image)) return refuse(Verdict::kBadSize, InputName(c.input));
    if (!FormatOkInput(c.input, c.image->format)) return refuse(Verdict::kBadFormat, InputName(c.input));
  }
  for (size_t s = 0; s < kSignalCount; ++s) {
    if (!HasFlag(in.dispatch_signals, Signal(s))) continue;
    // The signals are at the render size (checkerboard reconstruction, half width, is a later option: checkerboardSignalFlags stays 0).
    if (!size_ok(in.signal_in[s]) || !size_ok(in.signal_out[s])) return refuse(Verdict::kBadSize, SignalName(Signal(s)));
    if (!FormatOkSignal(Signal(s), in.signal_in[s].format) || !FormatOkSignal(Signal(s), in.signal_out[s].format)) return refuse(Verdict::kBadFormat, SignalName(Signal(s)));
  }
  plan.verdict = Verdict::kOk;
  plan.culprit = "";
  return plan;
}

// --- CONVENTIONS -------------------------------------------------------------------------------------------------------------------------------------------

struct Vec3 {
  float x = 0, y = 0, z = 0;
};
struct Vec2 {
  float x = 0, y = 0;
};
constexpr float SignNotZeroF(float v) { return v >= 0.0f ? 1.0f : -1.0f; }

// AMD's octahedral map (denoising.md 5.3.1 / 5.3.2, word for word). The game's G-buffer (gbuffer_rules.h OctEncode) is the same map, so its RT1 code can be handed over as it is.
inline Vec2 OctahedronUv(Vec3 n) {
  const float inv = 1.0f / (std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z));
  float x = n.x * inv;
  float y = n.y * inv;
  if (n.z < 0.0f) {
    const float kx = SignNotZeroF(x);
    const float ky = SignNotZeroF(y);
    const float nx = (1.0f - std::fabs(y)) * kx;
    const float ny = (1.0f - std::fabs(x)) * ky;
    x = nx;
    y = ny;
  }
  return {x * 0.5f + 0.5f, y * 0.5f + 0.5f};
}
inline Vec3 OctahedronUvToNormal(Vec2 uv) {
  float x = uv.x * 2.0f - 1.0f;
  float y = uv.y * 2.0f - 1.0f;
  const float z = 1.0f - std::fabs(x) - std::fabs(y);
  const float t = std::min(std::max(-z, 0.0f), 1.0f);
  x -= SignNotZeroF(x) * t;
  y -= SignNotZeroF(y) * t;
  const float il = 1.0f / std::sqrt(x * x + y * y + z * z);
  return {x * il, y * il, z * il};
}

// The material type's stored value: a fraction of the three non-default types (0 for the default: every pixel starts at 0, denoising.md 5.4).
constexpr float MaterialFraction(uint32_t type) { return float(type < kMaterialTypes ? type : kMaterialTypes - 1) / float(kMaterialTypes - 1); }

// One texel of the `normals` input: octahedral RG, the LINEAR roughness (1 - smoothness: dlss_rr_rules.h LinearRoughnessOfSmoothness) in B, the material fraction in A.
struct NormalsTexel {
  float u = 0, v = 0, roughness = 0, material = 0;
};
inline NormalsTexel MakeNormalsTexel(Vec3 world_normal, float linear_roughness, uint32_t material_type) {
  const Vec2 uv = OctahedronUv(world_normal);
  const float r = linear_roughness < 0.0f ? 0.0f : (linear_roughness > 1.0f ? 1.0f : linear_roughness);
  return {uv.x, uv.y, r, MaterialFraction(material_type)};
}

// The albedos: sqrt-encoded by default, raw with kDispatchNonEncodedAlbedo (what the shared RR guides are).
inline float EncodeAlbedo(float albedo, bool non_encoded) { return non_encoded ? albedo : std::sqrt(albedo < 0.0f ? 0.0f : albedo); }
inline float DecodeAlbedo(float stored, bool non_encoded) { return non_encoded ? stored : stored * stored; }

// The camera. The game's view space is right-handed and looks along -z (temporal_camera.h: the view inverse's rows are right, up, BACKWARD and the position; the projection puts the near plane at view
// z = -5, clip w = -z; depth 1 near / 0 far). AMD's sample feeds the denoiser the view-space z of its own camera, a right-handed Cauldron one (view z < 0 ahead: its debug view takes abs()), next to
// that camera's view and projection; AMD's text only says "signed linear depth ... depends on whether the camera is facing forward or backward in view space", and its motion-vector example assumes a +z
// camera (z = clip w). So the rule here is ONE view space for everything: the game's own `view` and unjittered `projection` (row-major, row vectors: a direct copy, the layout FFX names), the signed
// linear depth is the z of that same view space (negative ahead), and the depth delta is previous minus current in it. The other consistent choice is the mirror (z flipped in all three: positive ahead,
// the +z camera of AMD's example): kLinearDepthPositiveAhead below says which one the dispatch uses; it is the first thing a Radeon RX 9000 run confirms (RAY-REGEN.md section 8).
inline constexpr bool kLinearDepthPositiveAhead = false;
// The signed linear depth of a stored depth value (1 near, 0 far: reverse z) under a projection P in row vectors: clip z = z P22 + P32, clip w = z P23 + P33, depth = clip z / clip w, solved for the
// view-space z. `p` is the 4x4 projection row-major (p[row][column]).
inline double ViewZOfDepth(double depth, const double (&p)[4][4]) {
  const double den = depth * p[2][3] - p[2][2];
  return den == 0.0 ? 0.0 : (p[3][2] - depth * p[3][3]) / den;
}
inline double SignedLinearDepth(double depth, const double (&p)[4][4]) {
  const double z = ViewZOfDepth(depth, p);
  return kLinearDepthPositiveAhead ? -z : z;
}
// The linear depth delta of the motion vectors' B channel: previous minus current (FfxApi: PreviousLinearDepth - CurrentLinearDepth), with motionVectorScale.z 1.
constexpr double LinearDepthDelta(double previous, double current) { return previous - current; }
// The 2D vectors are the kept ones (render pixels, previous minus current, +y down: the same sign as AMD's PreviousUV - CurrentUV once divided by the size), so the dispatch scale is 1 / size.
struct MotionScale {
  float x = 1.0f, y = 1.0f, z = 1.0f;
};
constexpr MotionScale MotionScaleFor(uint32_t render_w, uint32_t render_h) { return {render_w ? 1.0f / float(render_w) : 1.0f, render_h ? 1.0f / float(render_h) : 1.0f, 1.0f}; }
// The camera's world position delta: previous minus current (FfxApi cameraPositionDelta).
constexpr Vec3 CameraPositionDelta(Vec3 previous, Vec3 current) { return {previous.x - current.x, previous.y - current.y, previous.z - current.z}; }
// The depth bounds a dispatch passes: |z| between the near plane and just inside the far plane; a texel outside passes through (the sky, stored depth 0, is the far plane itself).
struct Bounds {
  float min = 0, max = 0;
};
constexpr Bounds LinearDepthBounds(double near_plane, double far_plane) { return {float(near_plane), float(far_plane * 0.999)}; }
constexpr bool InBounds(double abs_linear_depth, const Bounds& b) { return abs_linear_depth >= double(b.min) && abs_linear_depth <= double(b.max); }

// The frame index and the reset. FfxApi wants frameIndex == previous + 1 for a normal frame and treats anything else as a scene discontinuity (an implicit reset); our dispatches skip frames (a pause,
// a frame another denoiser took), so the clock hands out a counter that always goes up by one and decides the reset itself: the first dispatch, a game frame that is not the one after the last
// dispatched, or a reset the caller asks for (a camera cut, a size change, a context made anew).
class FrameClock {
 public:
  struct Step {
    uint32_t index = 0;
    bool reset = false;
  };
  Step Next(uint64_t game_frame, bool force_reset) {
    const bool consecutive = have_ && game_frame == last_game_frame_ + 1;
    Step s;
    s.index = ++counter_;
    s.reset = force_reset || !consecutive;
    have_ = true;
    last_game_frame_ = game_frame;
    return s;
  }
  void Forget() { have_ = false; }  // the context was made anew or dropped: the next step resets
  uint32_t counter() const { return counter_; }

 private:
  bool have_ = false;
  uint64_t last_game_frame_ = 0;
  uint32_t counter_ = 0;
};

// --- video memory ------------------------------------------------------------------------------------------------------------------------------------------

// AMD's table (denoising.md 3.2, RX 9070 XT, "subject to change"): the context's total MiB (persistent + aliasable) at 960x540, 1920x1080 and 2560x1440 for the signal sets it lists. A set it does not
// list takes the smallest listed set that contains it. The runtime's own figure (ffxQueryDescDenoiserGetGPUMemoryUsage, with no context) replaces this before the context is made.
struct MemoryRow {
  uint32_t signals;
  uint32_t mib[3];
};
inline constexpr uint32_t kSingleSignals = FlagOf(Signal::kAmbientOcclusion) | FlagOf(Signal::kDominantLight) | FlagOf(Signal::kSpecularOcclusion);
inline constexpr std::array<MemoryRow, 4> kMemoryRows = {{
    {FlagOf(Signal::kAmbientOcclusion), {32, 72, 118}},   // AO, dominant light visibility or specular occlusion alone (the three share a row in AMD's table)
    {FlagOf(Signal::kIndirectSpecular), {90, 282, 462}},
    {FlagOf(Signal::kIndirectDiffuse) | FlagOf(Signal::kIndirectSpecular), {99, 313, 519}},
    {FlagOf(Signal::kDirectDiffuse) | FlagOf(Signal::kDirectSpecular) | FlagOf(Signal::kIndirectDiffuse) | FlagOf(Signal::kIndirectSpecular), {117, 377, 634}},
}};
inline uint64_t EstimateBytes(uint32_t signals, uint32_t w, uint32_t h) {
  if (signals == 0 || w == 0 || h == 0) return 0;
  const MemoryRow* row = nullptr;
  if ((signals & ~kSingleSignals) == 0) {
    row = &kMemoryRows[0];
    if (std::popcount(signals) > 1) row = &kMemoryRows[1];  // several single signals: a context for them is at least the specular one's size
  } else {
    for (const MemoryRow& r : kMemoryRows) {
      if ((signals & ~r.signals) == 0) {
        row = &r;
        break;
      }
    }
    if (!row) row = &kMemoryRows[3];
  }
  const uint64_t px = uint64_t(w) * h;
  constexpr uint64_t p[3] = {960ull * 540ull, 1920ull * 1080ull, 2560ull * 1440ull};
  const uint64_t m[3] = {row->mib[0] * rr::kMiB, row->mib[1] * rr::kMiB, row->mib[2] * rr::kMiB};
  if (px <= p[0]) return m[0] * px / p[0];
  if (px <= p[1]) return m[0] + (m[1] - m[0]) * (px - p[0]) / (p[1] - p[0]);
  if (px <= p[2]) return m[1] + (m[2] - m[1]) * (px - p[1]) / (p[2] - p[1]);
  return m[2] * px / p[2];  // beyond 1440p: in proportion to the pixels
}

// --- the row's words ---------------------------------------------------------------------------------------------------------------------------------------

// AMD's naming guidelines (gpuopen.com/amd-fsr-naming-guidelines, read 2026-11-22): the UI name is "AMD FSR Ray Regeneration"; "AMD FSR" is named where the effect is named in a game's options; the version
// number is not shown beside the name; "FSR" is used only as part of "AMD FSR". The pause menu's lists are 41 characters wide (denoiser_setting.h THE WIDTH): the menu says "FSR RAY REGEN" (13) and
// "AUTO (FSR RAY REGEN)" (20), the launcher, the wiki and the log the full name.
inline constexpr const char* kMenuName = "FSR RAY REGEN";
inline constexpr const char* kLauncherName = "AMD FSR Ray Regeneration";
inline constexpr const char* kLauncherLabel = "Denoiser";
// AMD's recommended description starts: "AMD FSR Ray Regeneration infers and restores full-quality ray-traced detail from sparse samples."; the rest of ours says what the row does here.
inline constexpr const char* kLauncherDescription =
    "AMD FSR Ray Regeneration infers and restores full-quality ray-traced detail from sparse samples. It cleans the grain out of the ray traced and reflected light before the upscaler runs, on a Radeon RX 9000 "
    "series graphics card with Direct3D 12. Ours is the game's own cleanup.";
// What AUTO comes to, in a sentence (the launcher's line under the row on a Radeon).
inline constexpr const char* kLauncherAutoDescription = "Auto uses AMD FSR Ray Regeneration where an effect that is on looks better with it, and ours everywhere else.";
inline constexpr const char* kCreditLine = "AMD FSR Ray Regeneration, Advanced Micro Devices, Inc. (MIT licence). AMD, FSR and Radeon are trademarks of Advanced Micro Devices, Inc.";
constexpr const char* AutoLabel(bool auto_is_regen) { return auto_is_regen ? "AUTO (FSR RAY REGEN)" : "AUTO (OURS)"; }
constexpr const char* MenuNameOfRr(Vendor v) { return v == Vendor::kAmd ? kMenuName : "DLSS RR"; }

enum class Gate : uint8_t {
  kRuns,
  kStarting,      // the runtime is being opened (nothing is said)
  kApplying,      // on; the context is being made
  kNotThisGpu,    // grey: not an AMD card (the row of an RTX card is DLSS RR's: denoiser_setting.h)
  kNeedsD3D12,    // grey: an AMD card, but the renderer runs on Vulkan (AMD ships no Vulkan provider for Ray Regeneration)
  kNotAvailable,  // grey: the runtime offers it for no card of this PC (RDNA 3 and older, an old driver, a shader model below 6.6), or this build has none
  kSetup,         // grey: the DLLs are missing
  kFailed,
  kNotNeeded,     // grey: no effect that uses it is on
};
inline constexpr int kGateCount = 9;
constexpr bool RowEnabled(Gate g) { return g == Gate::kRuns || g == Gate::kStarting || g == Gate::kApplying; }
// At most 13 characters after the label (the page's width rule).
constexpr const char* RowSuffix(Gate g) {
  switch (g) {
    case Gate::kApplying: return " (APPLYING)";
    case Gate::kNotThisGpu: return " (NO RX 9000)";
    case Gate::kNeedsD3D12: return " (D3D12 ONLY)";
    case Gate::kNotAvailable: return " (N/A)";
    case Gate::kSetup: return " (SETUP)";
    case Gate::kFailed: return " (FAILED)";
    case Gate::kNotNeeded: return " (NOT NEEDED)";
    default: return "";
  }
}
constexpr const char* LauncherReason(Gate g) {
  switch (g) {
    case Gate::kApplying: return "AMD FSR Ray Regeneration is being prepared";
    case Gate::kNotThisGpu: return "Needs a Radeon RX 9000 series graphics card";
    case Gate::kNeedsD3D12: return "Needs Direct3D 12 (AMD FSR Ray Regeneration has no Vulkan version)";
    case Gate::kNotAvailable: return "Not available on this graphics card or driver";
    case Gate::kSetup: return "Setup needed: the AMD FSR Ray Regeneration files are missing";
    case Gate::kFailed: return "AMD FSR Ray Regeneration failed in this session; ours goes on";
    case Gate::kNotNeeded: return "No effect that uses it is on";
    default: return "";
  }
}

// The row's state, asked of the plan's own rules with the setting forced to the vendor's value and the frame's needs met (so the row and the plan can never disagree about what the PC can do), then the
// one thing the rules cannot know here: whether an effect that uses it is on.
inline Gate GateOf(const Facts& facts, rr::DebugSwitch debug, uint32_t consumers_on, bool context_making, const ConsumerTable& table = kConsumers) {
  if (facts.build_has) {
    if (facts.device == DeviceVerdict::kNotD3D12) return Gate::kNeedsD3D12;
    if (facts.device == DeviceVerdict::kShaderModelTooLow) return Gate::kNotAvailable;
    if (facts.device != DeviceVerdict::kOk) return Gate::kNotThisGpu;
  }
  if (debug == rr::DebugSwitch::kOff) return Gate::kNotAvailable;
  // Nothing has a signal mode (the shipped table before RG-3): nothing could use it, so nothing is "not available".
  if (debug == rr::DebugSwitch::kRow && !AnySignalBuilt(table)) return Gate::kNotNeeded;
  PlanInputs in;
  in.setting = rr::Denoiser::kRr;
  in.debug = debug;
  in.facts = facts;
  in.consumers_active = true;
  in.consumers_any = true;
  in.guides_ready = true;
  in.room_ok = true;
  switch (SelectRayRegen(in).why) {
    case Why::kOn:
    case Why::kProbeOnly: break;
    case Why::kNotProbed: return Gate::kStarting;
    case Why::kFilesMissing: return Gate::kSetup;
    case Why::kFailed: return Gate::kFailed;
    case Why::kNotThisGpu: return Gate::kNotThisGpu;
    default: return Gate::kNotAvailable;
  }
  if (debug == rr::DebugSwitch::kRow && !AnyConsumerOn(consumers_on, table)) return Gate::kNotNeeded;
  return context_making ? Gate::kApplying : Gate::kRuns;
}

}  // namespace mcla::gpu::rayregen
