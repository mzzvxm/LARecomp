#pragma once
// ===========================================================================
// MCLA GPU renderer -- the DENOISER setting and its pause row (perf/DLSS-RR.md section 5, package RR-5)
// ===========================================================================
// One row on GRAPHICS > UPSCALING, after SHARPNESS: DENOISER, AUTO / DLSS RR / OURS (mcla_gpu_denoiser, live, saved): who cleans the grain out of the noisy effects (the screen-space reflections, the ray
// traced reflections, the ray traced bounce light): NVIDIA's DLSS Ray Reconstruction, or our own denoisers. This header is every decision about it that needs no device, so that denoiser_setting_test.cpp
// (and the pause menu, which includes it) can check them: the words, the gate that greys the row and what it says, AUTO's label, and the text for the launcher and the wiki. denoiser_setting.cpp holds the
// setting's LiveText, the frame's snapshot the render thread publishes for the menu's thread, and the board where the effects say they are on.
//
// WHAT THE WORDS COME TO (the rules are dlss_rr_rules.h SelectDenoiser, which the temporal system asks every frame):
//   OURS               our denoisers, always. Streamline's plugin is not asked for at the start, so nothing of RR costs memory or start time.
//   DLSS RR            Ray Reconstruction wherever it can run: an RTX card Streamline says supports it, DLSS as the upscaler, and an effect on that has a raw mode (a CONSUMER: dlss_rr_rules.h kConsumers).
//   AUTO (the default) Ray Reconstruction where an effect is on that RR is known to clean better than ours (kConsumers' auto_uses_rr: the A/B pictures say so); our denoisers everywhere else.
// So the order of the owner's rule (2026-11-19: DLSS Ray Reconstruction when the GPU supports it, ours otherwise) holds: where RR can run AND has something to clean, AUTO is RR; where it cannot, ours.
// No effect has a raw mode yet (RR-4), so today AUTO is OURS everywhere, the row is grey with " (NOT NEEDED)", and the plugin is not loaded: a player's game is exactly what it was before the row. A
// switch of the row is live (the next frame): RR's context is made in the background beside Super Resolution's (RR-3), and the frames in between are Super Resolution's. The one thing that is not live is
// the plugin itself: Streamline loads it at its start (the start with the row on OURS leaves it out), so leaving OURS in such a game says " (RESTART)" and the usual "Restart now?" prompt follows.
//
// THE GATE (Gate, from NVIDIA's RTX UI Developer Guidelines, "DLSS Ray Reconstruction": disable where the hardware or the driver cannot run it, where Super Resolution is not on, where there is no ray traced
// content): the row is grey with its reason after the label, in the order the rules ask: " (NO RTX GPU)" (the PC cannot run DLSS: not an NVIDIA RTX card), " (N/A)" (the card or the driver cannot run
// RR, or this build has none), " (NEEDS DLSS)" (the upscaler is not DLSS), " (SETUP)" (the files are missing), " (FAILED)" (it failed in this session: Super Resolution goes on), " (NOT NEEDED)" (no
// effect that uses it is on). Two states keep the row ON and say a word: " (RESTART)" and " (APPLYING)" (RR's context is being made: several seconds the first time).
//
// THE NAMES (NVIDIA's guidelines): "DLSS Ray Reconstruction" is the feature's name, with "DLSS" always attached; its options are OFF and AUTO ("AUTO ... enabled by default where RTX hardware is found, Super
// Resolution is on and ray tracing content is on"); it "requires DLSS Super Resolution to be ON" and "a GeForce RTX graphics card"; its tooltip reads "Ray Reconstruction enhances image quality by generating
// high-quality pixels between sampled rays for intensive ray traced content. This feature requires a GeForce RTX graphics card." Our row's OURS is the OFF of that list (nothing of RR), our DLSS RR the
// explicit ON. The pause menu's lists are 41 characters wide at most (pause_menu.cpp "Page width"), so the menu says "DLSS RR" (NVIDIA's own abbreviation: "DLSS-RR", "RR"); the launcher, the wiki and the
// log write "DLSS Ray Reconstruction" in full (LauncherName, the texts below), as the page for ULTRA PERFORMANCE says ULTRA PERF.
//
// THE WIDTH (UPSCALING counts its widest label plus its widest value, both at most 41 together): the row's label is "DENOISER" (8) with the longest reason " (NO RTX GPU)" (13): 21; its values are
// "AUTO (DLSS RR)" (14), "DLSS RR" and "OURS". The page's widest value is 18 ("FSR 3.1 (APPLYING)") and its widest label 20, so the page stays at 21 + 18 = 39.
//
// Header only, includes nothing of Plume, Streamline or ReXGlue. THREADING: pure functions; the declarations at the end are in denoiser_setting.cpp.
//
// CHECK: nothing here was run in the game. The shown names ("DLSS RR") and the order AUTO, DLSS RR, OURS are the owner's to confirm against NVIDIA's rules; the launcher's wording is below for the main session.
// ===========================================================================

#include <cstdint>
#include <string>
#include <string_view>

#include "dlss_rr_rules.h"

namespace mcla::gpu::denoiser {

// ---------------------------------------------------------------------------
// the words
// ---------------------------------------------------------------------------

// The stored words (the cvar's), the menu's names, and the launcher's: in the order of the row. The stored word is rr, the rule's own (rr::ParseDenoiser).
inline constexpr const char* kWords[3] = {"auto", "rr", "ours"};
inline constexpr const char* kMenuNames[3] = {"AUTO", "DLSS RR", "OURS"};
inline constexpr const char* kLauncherNames[3] = {"Auto", "DLSS Ray Reconstruction", "Ours"};
inline constexpr const char* kCvarName = "mcla_gpu_denoiser";
inline constexpr const char* kMenuLabel = "DENOISER";

constexpr size_t IndexOf(rr::Denoiser d) { return d == rr::Denoiser::kAuto ? 0 : (d == rr::Denoiser::kRr ? 1 : 2); }
constexpr const char* WordOf(rr::Denoiser d) { return kWords[IndexOf(d)]; }

// A stored word as a setting; a word that is none of the three (an older larecomp.toml, a typo) is AUTO.
inline rr::Denoiser Parse(std::string_view text) {
  rr::Denoiser d = rr::Denoiser::kAuto;
  if (!rr::ParseDenoiser(text, &d)) d = rr::Denoiser::kAuto;
  return d;
}

// ---------------------------------------------------------------------------
// the gate
// ---------------------------------------------------------------------------

enum class Gate : uint8_t {
  kRuns,          // the row is on, nothing to say
  kStarting,      // on; Streamline has not finished starting, so what the PC can do is not known yet (nothing is said)
  kApplying,      // on; RR's context is being made (the frames are Super Resolution's meanwhile)
  kRestart,       // on; the plugin was left out of Streamline's start (the row said OURS) and loads at the next start
  kNoRtx,         // grey: the PC cannot run DLSS (not an NVIDIA RTX card)
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
    case Gate::kNoRtx: return " (NO RTX GPU)";
    case Gate::kNotAvailable: return " (N/A)";
    case Gate::kNeedsDlss: return " (NEEDS DLSS)";
    case Gate::kSetup: return " (SETUP)";
    case Gate::kFailed: return " (FAILED)";
    case Gate::kNotNeeded: return " (NOT NEEDED)";
    default: return "";
  }
}
// The launcher's line under a grey row (plain words; nothing for a row that is on, except the restart).
constexpr const char* LauncherReason(Gate g) {
  switch (g) {
    case Gate::kApplying: return "DLSS Ray Reconstruction is being prepared";
    case Gate::kRestart: return "Applies after a restart";
    case Gate::kNoRtx: return "Needs a GeForce RTX graphics card";
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
  rr::Denoiser setting = rr::Denoiser::kAuto;
  uint32_t consumers_on = 0;           // rr::Consumer bits of the effects that are on now
  bool context_making = false;         // RR's context is asked for and not ready
};

// The row's state. The PC's reasons are asked of the plan's own rules (rr::SelectDenoiser) with an effect made up, the setting forced to RR and the frame's needs met, so the row and the plan can
// never disagree about what the PC can do; then the one thing the rules cannot know here, whether an effect that uses RR is on.
inline Gate GateOf(const MenuInputs& m, const rr::ConsumerTable& table = rr::kConsumers) {
  if (!m.dlss_listed) return Gate::kNoRtx;
  if (m.debug == rr::DebugSwitch::kOff) return Gate::kNotAvailable;
  // The saved upscaler is DLSS but the game runs it from the next start (Vulkan): the restart the UPSCALING row offers brings it, and the plugin with it. Said only where this row has chosen RR.
  if (m.dlss_chosen && m.dlss_waits_for_restart) return m.setting == rr::Denoiser::kOurs ? Gate::kRuns : Gate::kRestart;
  if (!m.dlss_chosen) return Gate::kNeedsDlss;
  if (m.dlss_needs_setup) return Gate::kSetup;
  // While no effect has a raw mode (rr::kConsumers) there is nothing RR could clean and the plugin is not asked for: nothing is "not available", nothing is needed.
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

// AUTO comes to RR where the row is on (and the plugin is loaded) and an effect AUTO trusts RR with is on; "AUTO (DLSS RR)" / "AUTO (OURS)".
inline bool AutoIsRr(Gate g, uint32_t consumers_on, const rr::ConsumerTable& table = rr::kConsumers) {
  return (g == Gate::kRuns || g == Gate::kStarting || g == Gate::kApplying) && rr::AnyAutoConsumerOn(consumers_on, table);
}
inline std::string AutoLabel(Gate g, uint32_t consumers_on, const rr::ConsumerTable& table = rr::kConsumers) {
  return AutoIsRr(g, consumers_on, table) ? "AUTO (DLSS RR)" : "AUTO (OURS)";
}

// ---------------------------------------------------------------------------
// the launcher's and the wiki's text (the main session copies it; the launcher is in its own, private repository)
// ---------------------------------------------------------------------------

// NVIDIA's tooltip for the feature (RTX UI Developer Guidelines, "UI Tooltip or Setting Description"), word for word.
inline constexpr const char* kNvidiaTooltip =
    "Ray Reconstruction enhances image quality by generating high-quality pixels between sampled rays for intensive ray traced content. This feature requires a GeForce RTX graphics card.";
inline constexpr const char* kLauncherLabel = "Denoiser";
// The line under the row: what the row is, in NVIDIA's words for the feature and ours for the rest.
inline constexpr const char* kLauncherDescription =
    "Cleans the grain out of the ray traced and reflected light. DLSS Ray Reconstruction enhances image quality by generating high-quality pixels between sampled rays for intensive ray traced content; it "
    "also does the upscaling, so it needs DLSS as the upscaler and a GeForce RTX graphics card. Ours is the game's own cleanup.";
// What AUTO comes to (the same words the pause menu's label says, in a sentence).
inline constexpr const char* kLauncherAutoDescription =
    "Auto uses DLSS Ray Reconstruction where an effect that is on looks better with it, and ours everywhere else.";
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
// The setting's word as the render thread reads it (a LiveText: the pause menu's thread writes the cvar).
rr::Denoiser Setting();
// The effects that are on and have a raw mode say it here, once a frame, from the render thread (RR-4's packages): `on` is whether the effect runs this frame.
void NoteConsumer(rr::Consumer consumer, bool on);
uint32_t ConsumersOn();

}  // namespace mcla::gpu::denoiser
