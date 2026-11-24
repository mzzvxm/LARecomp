#pragma once
// ===========================================================================
// MCLA GPU renderer -- the denoiser setting and its pause row (perf/DLSS-RR.md section 5, package RR-5; the one on / off row: backlog DENOISER-TOGGLE, owner 2026-11-23)
// ===========================================================================
// One row on GRAPHICS > DISPLAY, after SHARPNESS: the GPU maker's ray tracing denoiser, ON / OFF (mcla_gpu_vendor_denoiser, live, saved, off by default), the way other PC games list it: who cleans the grain out of the
// noisy effects (the screen-space reflections, the ray traced reflections, the ray traced bounce light). It is shown only where this PC can run the maker's denoiser:
//   an RTX card                      DLSS RAY RECONSTRUCTION   ON = NVIDIA's DLSS Ray Reconstruction (this file and dlss_rr_rules.h), OFF = ours
//   a Radeon RX 9000, Windows 11     AMD FSR RAY REGENERATION  ON = AMD's FSR Ray Regeneration (ray_regen_rules.h and ray_regen_plan.h), OFF = ours
//   any other card, and the Xenia renderer: no row, and our own denoisers silently (RowFor).
// This header is every decision about the row that needs no device, so that denoiser_setting_test.cpp (and the pause menu, which includes it) can check them: the words, the gate that greys the row and what it
// says, which setting the plan reads, and the text for the launcher and the wiki. denoiser_setting.cpp holds the setting's live reads, the frame's snapshot the render thread publishes for the menu's thread,
// and the board where the effects say they are on.
//
// THE TWO SETTINGS. The player's row is mcla_gpu_vendor_denoiser (a bool: ON = the vendor's denoiser). The old chooser, mcla_gpu_denoiser (auto, rr, ours; "row" by default), is a DEBUG setting now: it is what
// the A/B chains and the live tours set (perf/scripts/chain-rr.ps1, rr_full.py), and when it says auto, rr or ours it overrides the row (Effective). Everything that decides who denoises reads the EFFECTIVE
// word, an rr::Denoiser, from Setting(); so the plan, the plugin request and the row never disagree.
//   OURS               our denoisers, always (the row OFF). Streamline's plugin is not asked for at the start, so nothing of RR costs memory or start time.
//   RR (the row ON)    the vendor's denoiser wherever it can run: an RTX card Streamline says supports it, DLSS as the upscaler, and an effect on that has a raw mode (a CONSUMER: dlss_rr_rules.h kConsumers).
//   AUTO (debug only)  Ray Reconstruction where an effect is on that RR is known to clean better than ours (kConsumers' auto_uses_rr: the A/B pictures say so); our denoisers everywhere else.
// The default is OFF for both vendors until the RR-DECIDE A/B has been seen (owner 2026-11-23: one default rule for both, ON or OFF): our own denoisers cost far less than RR (0.03 ms for the cars' reflections
// against 0.7 to 2.1 ms on a fast RTX card) and RR is slightly softer where it has little to clean.
// Four effects have a raw mode (RR-INT: the screen-space reflections, RR-4a, and the cars' ray traced reflections, RR-4b; CITY-RT R6: the city's ray traced reflections, the screen-space reflections' fallback;
// CITY-RT G6: the bounce light's per-pixel pass; they say they are on through NoteConsumer). The row ON takes a frame while one is on. Streamline is started with the plugin wherever the row is not OFF (and the PC
// can: an RTX card, D3D12 or Vulkan with DLSS saved). A switch of the row is live (the next frame): RR's context is made in the background beside Super Resolution's (RR-3), and the frames in between are
// Super Resolution's. The one thing that is not live is the plugin itself: Streamline loads it at its start (a start with the row OFF leaves it out), so turning the row ON in such a game says " (RESTART)" and the
// usual "Restart now?" prompt follows.
//
// THE GATE (Gate, from NVIDIA's RTX UI Developer Guidelines, "DLSS Ray Reconstruction": disable where the hardware or the driver cannot run it, where Super Resolution is not on, where there is no ray traced
// content): the row is grey with its reason after the label, in the order the rules ask: " (N/A)" (the card or the driver cannot run RR, or this build has none), " (NEEDS DLSS)" (the upscaler is not DLSS),
// " (SETUP)" (the files are missing), " (FAILED)" (it failed in this session: Super Resolution goes on), " (NOT NEEDED)" (no effect that uses it is on). Two states keep the row ON and say a word: " (RESTART)" and
// " (APPLYING)" (RR's context is being made: several seconds the first time). A PC that is not an RTX card has no row at all (kHidden: the gate never says "no RTX GPU" to anyone).
//
// THE NAMES (NVIDIA's guidelines): "DLSS Ray Reconstruction" is the feature's name, with "DLSS" always attached; "It requires DLSS Super Resolution to be ON" and "a GeForce RTX graphics card"; its tooltip reads "Ray
// Reconstruction enhances image quality by generating high-quality pixels between sampled rays for intensive ray traced content. This feature requires a GeForce RTX graphics card." The row's label is
// "DLSS RAY RECONSTRUCTION" while it says nothing, and "DLSS RR" (NVIDIA's own abbreviation) in front of a reason (RowLabel): the whole name with a reason would make the page too wide (THE WIDTH).
//
// THE WIDTH. DISPLAY counts its widest label plus its widest value, both at most 41 characters together (pause_menu.cpp "Page width"), over EVERY row of the page, so a long label on this row costs every other row
// its values. The page's widest value is 17 ("PERFORMANCE (50%)", "ULTRA PERF. (33%)", "FSR 3.1 (NO ROOM)"), so no label may be over 24: "DLSS RAY RECONSTRUCTION" is 23 and "AMD FSR RAY REGENERATION" 24, and
// with a reason they are "DLSS RR (NEEDS DLSS)" 20 and "RAY REGEN (D3D12 ONLY)" 22. (The whole names with their reasons, 36 and 37, measure 53 and 54 against that page; denoiser_setting_test.cpp counts the page.)
//
// Header only, includes nothing of Plume, Streamline or ReXGlue. THREADING: pure functions; the declarations at the end are in denoiser_setting.cpp.
//
// CHECK: nothing here was run in the game. The shown words are the owner's to confirm on screen: the whole names while the row is quiet, the short ones before a reason.
// ===========================================================================

#include <cstdint>
#include <string>
#include <string_view>

#include "dlss_rr_rules.h"

namespace mcla::gpu::denoiser {

// ---------------------------------------------------------------------------
// the words and the two settings
// ---------------------------------------------------------------------------

// The stored words of the effective setting (the rules' own, rr::ParseDenoiser; "rr" is the vendor's denoiser whichever vendor: a saved setting survives a change of graphics card).
inline constexpr const char* kWords[3] = {"auto", "rr", "ours"};
// The player's row: a bool, ON = the vendor's denoiser, OFF = ours. Off by default.
inline constexpr const char* kVendorCvarName = "mcla_gpu_vendor_denoiser";
// The debug chooser (A/B chains, tours): "row" (the default: the row above decides), or auto / rr / ours, which override the row.
inline constexpr const char* kCvarName = "mcla_gpu_denoiser";
inline constexpr const char* kChooserWords[4] = {"row", "auto", "rr", "ours"};
inline constexpr const char* kChooserRow = "row";

// The row's labels (capitals, as the menu's font): the whole name while the row says nothing, the short one in front of a reason (THE WIDTH).
inline constexpr const char* kMenuLabel = "DLSS RAY RECONSTRUCTION";
inline constexpr const char* kMenuShortLabel = "DLSS RR";

constexpr size_t IndexOf(rr::Denoiser d) { return d == rr::Denoiser::kAuto ? 0 : (d == rr::Denoiser::kRr ? 1 : 2); }
constexpr const char* WordOf(rr::Denoiser d) { return kWords[IndexOf(d)]; }

// A stored word of the debug chooser as a setting; a word that is none of auto, rr, ours ("row", an older larecomp.toml, a typo) is the row's.
inline bool IsOverride(std::string_view chooser) {
  rr::Denoiser d = rr::Denoiser::kOurs;
  return rr::ParseDenoiser(chooser, &d);
}
// The setting every reader asks: the debug chooser when it says auto, rr or ours, else the row (ON is RR, OFF is ours).
inline rr::Denoiser Effective(std::string_view chooser, bool vendor_row_on) {
  rr::Denoiser d = rr::Denoiser::kOurs;
  if (rr::ParseDenoiser(chooser, &d)) return d;
  return vendor_row_on ? rr::Denoiser::kRr : rr::Denoiser::kOurs;
}

// Which vendor's row this PC shows (the Xenia renderer's page has none: the row is tagged NativeOnly). `regen_listed` is ray_regen_rules.h RowListed (a Radeon RX 9000 series card on Windows 11), `dlss_listed` is
// the upscalers' answer that this PC can run DLSS at all (an RTX card: UpscalerAvailability dlss, needs setup or after restart). Any other PC has no row.
enum class VendorRow : uint8_t { kNone, kDlssRr, kRayRegen };
constexpr VendorRow RowFor(bool regen_listed, bool dlss_listed) {
  if (regen_listed) return VendorRow::kRayRegen;
  return dlss_listed ? VendorRow::kDlssRr : VendorRow::kNone;
}

// ---------------------------------------------------------------------------
// the gate
// ---------------------------------------------------------------------------

enum class Gate : uint8_t {
  kRuns,          // the row is on, nothing to say
  kStarting,      // on; Streamline has not finished starting, so what the PC can do is not known yet (nothing is said)
  kApplying,      // on; RR's context is being made (the frames are Super Resolution's meanwhile)
  kRestart,       // on; the plugin was left out of Streamline's start (the row said OFF) and loads at the next start
  kHidden,        // no row: the PC cannot run DLSS (not an NVIDIA RTX card). The menu does not list the row (RowFor), so no player sees this
  kNotAvailable,  // grey: this card or driver cannot run RR, this build has none, or the debug switch is off
  kNeedsDlss,     // grey: the upscaler is not DLSS (RR includes the upscale)
  kSetup,         // grey: the files are missing or fail their signature check
  kFailed,        // grey: RR failed in this session; Super Resolution goes on
  kNotNeeded,     // grey: no effect that uses RR is on
};
inline constexpr int kGateCount = 10;

constexpr bool RowEnabled(Gate g) { return g == Gate::kRuns || g == Gate::kStarting || g == Gate::kApplying || g == Gate::kRestart; }
// What the row says after its label (at most 13 characters: see THE WIDTH). " (RESTART)" is the word the pause menu's restart prompt looks for.
constexpr const char* RowSuffix(Gate g) {
  switch (g) {
    case Gate::kApplying: return " (APPLYING)";
    case Gate::kRestart: return " (RESTART)";
    case Gate::kNotAvailable: return " (N/A)";
    case Gate::kNeedsDlss: return " (NEEDS DLSS)";
    case Gate::kSetup: return " (SETUP)";
    case Gate::kFailed: return " (FAILED)";
    case Gate::kNotNeeded: return " (NOT NEEDED)";
    default: return "";
  }
}
// The row's label by state: the whole name while the row says nothing, "DLSS RR" before a reason (THE WIDTH).
constexpr const char* RowLabel(Gate g) { return RowSuffix(g)[0] == '\0' ? kMenuLabel : kMenuShortLabel; }
// The launcher's line under a grey row (plain words; nothing for a row that is on, except the restart).
constexpr const char* LauncherReason(Gate g) {
  switch (g) {
    case Gate::kApplying: return "DLSS Ray Reconstruction is being prepared";
    case Gate::kRestart: return "Applies after a restart";
    case Gate::kNotAvailable: return "Not available on this graphics card or driver";
    case Gate::kNeedsDlss: return "Needs DLSS as the upscaler";
    case Gate::kSetup: return "Setup needed: the DLSS Ray Reconstruction files are missing";
    case Gate::kFailed: return "DLSS Ray Reconstruction failed in this session; DLSS Super Resolution goes on";
    case Gate::kNotNeeded: return "No effect that uses it is on";
    default: return "";
  }
}

// What the menu knows, and what the renderer published (denoiser_setting.cpp Snapshot).
struct MenuInputs {
  bool dlss_listed = false;            // the PC can run DLSS at all (UpscalerAvailability: dlss, needs setup or after restart: an NVIDIA RTX card)
  bool dlss_chosen = false;            // DLSS is the saved upscaler
  bool dlss_waits_for_restart = false; // ... and the game can run it only from the next start (Vulkan)
  bool dlss_needs_setup = false;       // DLSS's own files are missing
  rr::Facts facts;                     // what Streamline and the files said (dlss::RayReconstructionFacts)
  rr::DebugSwitch debug = rr::DebugSwitch::kRow;
  rr::Denoiser setting = rr::Denoiser::kOurs;  // the EFFECTIVE setting (Effective)
  uint32_t consumers_on = 0;           // rr::Consumer bits of the effects that are on now
  bool context_making = false;         // RR's context is asked for and not ready
};

// The row's state. The PC's reasons are asked of the plan's own rules (rr::SelectDenoiser) with an effect made up, the setting forced to RR and the frame's needs met, so the row and the plan can
// never disagree about what the PC can do; then the one thing the rules cannot know here, whether an effect that uses RR is on.
inline Gate GateOf(const MenuInputs& m, const rr::ConsumerTable& table = rr::kConsumers) {
  if (!m.dlss_listed) return Gate::kHidden;
  if (m.debug == rr::DebugSwitch::kOff) return Gate::kNotAvailable;
  // The saved upscaler is DLSS but the game runs it from the next start (Vulkan): the restart the UPSCALING row offers brings it, and the plugin with it. Said only where this row is not OFF.
  if (m.dlss_chosen && m.dlss_waits_for_restart) return m.setting == rr::Denoiser::kOurs ? Gate::kRuns : Gate::kRestart;
  if (!m.dlss_chosen) return Gate::kNeedsDlss;
  if (m.dlss_needs_setup) return Gate::kSetup;
  // While no effect has a raw mode (a table with every raw_mode_built false: the shipped one before RR-INT) there is nothing RR could clean and the plugin is not asked for: nothing is "not available", nothing is needed.
  if (m.debug == rr::DebugSwitch::kRow && !rr::AnyRawModeBuilt(table)) return Gate::kNotNeeded;
  rr::PlanInputs in;
  in.setting = rr::Denoiser::kRr;
  in.debug = m.debug;
  in.facts = m.facts;
  in.upscaler_is_dlss = true;
  in.consumers_active = true;
  in.consumers_any = true;
  in.guides_ready = true;
  in.room_ok = true;
  switch (rr::SelectDenoiser(in).why) {
    case rr::Why::kRrOn:
    case rr::Why::kProbeOnly:
      break;
    case rr::Why::kStreamlineNotRunning: return Gate::kStarting;
    case rr::Why::kNotLoaded: return m.setting == rr::Denoiser::kOurs ? Gate::kRuns : Gate::kRestart;
    case rr::Why::kFilesMissing: return Gate::kSetup;
    case rr::Why::kRrFailed: return Gate::kFailed;
    case rr::Why::kUpscalerNotDlss: return Gate::kNeedsDlss;
    default: return Gate::kNotAvailable;  // not supported, a build without RR, the switch off
  }
  // Under the debug switch "on" (and probe) the consumer rule is lifted, as in the plan.
  if (m.debug == rr::DebugSwitch::kRow && !rr::AnyConsumerOn(m.consumers_on, table)) return Gate::kNotNeeded;
  return m.context_making ? Gate::kApplying : Gate::kRuns;
}

// ---------------------------------------------------------------------------
// the launcher's and the wiki's text (the main session copies it; the launcher is in its own, private repository)
// ---------------------------------------------------------------------------

// NVIDIA's tooltip for the feature (RTX UI Developer Guidelines, "UI Tooltip or Setting Description"), word for word.
inline constexpr const char* kNvidiaTooltip =
    "Ray Reconstruction enhances image quality by generating high-quality pixels between sampled rays for intensive ray traced content. This feature requires a GeForce RTX graphics card.";
inline constexpr const char* kLauncherLabel = "DLSS Ray Reconstruction";
// The line under the row: what the row is, in NVIDIA's words for the feature and ours for the rest.
inline constexpr const char* kLauncherDescription =
    "Cleans the grain out of the ray traced and reflected light. DLSS Ray Reconstruction enhances image quality by generating high-quality pixels between sampled rays for intensive ray traced content; it "
    "also does the upscaling, so it needs DLSS as the upscaler and a GeForce RTX graphics card. Off uses the game's own cleanup.";
// DLSS Ray Reconstruction's credit line, beside DLSS's (NVIDIA's trademark notice: "NVIDIA, the NVIDIA logo, GeForce and DLSS are trademarks or registered trademarks of NVIDIA Corporation").
inline constexpr const char* kCreditLine = "NVIDIA DLSS Ray Reconstruction, NVIDIA Corporation. NVIDIA, DLSS and GeForce are trademarks of NVIDIA Corporation.";

// ---------------------------------------------------------------------------
// what the renderer publishes (denoiser_setting.cpp)
// ---------------------------------------------------------------------------

// The frame's snapshot for the menu's thread: written once a frame by the temporal system, read by any thread.
struct Snapshot {
  bool valid = false;           // the temporal system has made a plan at least once
  rr::DebugSwitch debug = rr::DebugSwitch::kRow;  // mcla_gpu_dlss_rr as the plan read it
  rr::Facts facts;
  bool upscaler_is_dlss = false;
  bool context_making = false;
  rr::Plan plan;
};
void Publish(const Snapshot& snapshot);
Snapshot Current();
// The EFFECTIVE setting as the render thread reads it: the debug chooser mcla_gpu_denoiser (a LiveText: the pause menu's thread writes the cvar) when it says auto, rr or ours, else the row
// mcla_gpu_vendor_denoiser (ON is rr, OFF is ours).
rr::Denoiser Setting();
// The effects that are on and have a raw mode say it here, once a frame, from the render thread (RR-4's packages): `on` is whether the effect runs this frame.
void NoteConsumer(rr::Consumer consumer, bool on);
uint32_t ConsumersOn();

}  // namespace mcla::gpu::denoiser
