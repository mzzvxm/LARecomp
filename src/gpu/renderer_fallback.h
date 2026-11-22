#pragma once
// ===========================================================================
// When the native renderer cannot start: one plain way out, the compatibility renderer
// ===========================================================================
// The XENIA renderer stays in the program as a hidden fallback (owner, 2026-11-19): "if some user is having an issue with the native
// renderer for whatever reason, they can switch to the Xenia renderer through the debug menu or some other means and still be able to
// play the game". Nothing in the launcher or the normal menus offers it. A player meets it in three places, and in each it is called
// the compatibility renderer (no branding):
//   * the box that says the native renderer could not start (renderer.cpp Startup: no usable graphics device, a driver that refuses
//     the renderer), with a button that writes the setting and restarts;
//   * the dialog that says the shader packs could not be built (bootprogress/larecomp_shader_build.cpp): the compatibility renderer
//     needs no packs;
//   * the box at the start after two starts in a row that never got going (StartWatch below).
// The debug menu's RENDERER row (menus/pause_menu.cpp, RENDERING page) is the tester's way, in both directions.
//
// All of them only write the SDK's gpu.driver into larecomp.toml (gpu.driver is startup-only, so it applies at the next start) and
// start the game again; renderer_choice.h says what each value means. This header is the part that needs no game, the SDK or the
// graphics libraries (standard library only): the words of the boxes, the line that is written, and the memory of unfinished starts.
// gpu/renderer_choice_test.cpp compiles it with the profile store (profile/larecomp_profile_store.h, standard library only too), which is what
// writes larecomp.toml.
//
// What the compatibility renderer needs on a PC the native one refuses (perf/XENIA-RETIRE-PLAN.md 1.1): Direct3D 12 feature level
// 11_0 with shader model 5.1 or 6.0 (its shaders are DXBC 5.1) and binding tier 1 or 2 (bindless resources switch themselves off on
// tier 1), or Vulkan with independent blend, fragment stores and atomics, geometry shaders, fill mode non-solid and dynamic rendering.
// It does not need the shader packs. It has no ambient occlusion, HDR, upscalers or frame generation, and a lower frame rate.
// CHECK: not run on a tier 1 or shader model 5.1 card (the test PC has none).

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "renderer_choice.h"
#include "settings/immortal.h"

namespace mcla::gpu::fallback {

// The key of larecomp.toml the fallback writes: the SDK's renderer choice (renderer_choice.h).
inline constexpr std::string_view kConfigKey = "gpu.driver";

// One line of larecomp.toml, as the profile store's edit takes it (profiles::ConfigEdit: key, and the value as TOML text).
struct DriverEdit {
  std::string key;
  std::string value;  // quoted: "d3d12"
};

// The edit that sets gpu.driver. The drivers renderer_choice.h names are lower-case words and digits, so the quotes need no escapes.
inline DriverEdit EditFor(std::string_view driver) {
  return {std::string(kConfigKey), "\"" + std::string(driver) + "\""};
}

// The same as the line a player types: gpu.driver = "d3d12".
inline std::string ConfigLine(std::string_view driver) {
  const DriverEdit edit = EditFor(driver);
  return edit.key + " = " + edit.value;
}

// How a box can offer the compatibility renderer.
enum class Way {
  kButton,        // a button writes the setting and restarts
  kLine,          // the text names the line to put in larecomp.toml (the button could not do it)
  kLaunchOption,  // a command line option or an environment variable sets gpu.driver and wins over the file: the text names the option
};

// A saved choice only wins when the launch does not set gpu.driver itself: the command line and the environment beat larecomp.toml,
// and a restart keeps the command line.
constexpr Way WayFor(bool launch_sets_driver, bool button_works) {
  if (launch_sets_driver) {
    return Way::kLaunchOption;
  }
  return button_works ? Way::kButton : Way::kLine;
}

inline constexpr std::string_view kButtonText = "Use the compatibility renderer";

// The box shown when the native renderer cannot start. `refusals` is what each graphics API said when it was refused (empty: the log has it).
inline std::string RefusalText(const std::string& refusals, Way way, bool windows) {
  std::string text =
      "LARecomp could not start on this PC's graphics card.\n\n" +
      (refusals.empty() ? std::string("The reason is in the log.\n\n") : refusals + "\n\n") +
      "The game needs a graphics card and driver with Direct3D 12 (shader model 6.0 and resource binding tier 2) or Vulkan "
      "(descriptor indexing and buffer device address). Updating the graphics driver may help. The details are in the log.";
  switch (way) {
    case Way::kButton:
      text += "\n\nThe compatibility renderer runs on more graphics cards, with fewer graphics options. Start the game with it now?";
      break;
    case Way::kLine:
      text += "\n\nTo try the compatibility renderer, add this line to larecomp.toml and start the game again:\n" +
              ConfigLine(FallbackDriver(windows));
      break;
    case Way::kLaunchOption:
      text += "\n\nTo try the compatibility renderer, start the game with the option --" + std::string(kConfigKey) + "=" +
              std::string(FallbackDriver(windows)) + " instead.";
      break;
  }
  return text;
}

// The box after starts that never got going (StartWatch): always the button.
inline std::string UnfinishedStartsText(int starts) {
  return "LARecomp did not finish starting the last " + std::to_string(starts) +
         " times.\n\nThe graphics card or its driver may not agree with the game's renderer. The compatibility renderer runs on more "
         "graphics cards, with fewer graphics options.\n\nStart the game with it now?";
}
inline constexpr std::string_view kKeepTryingText = "Keep trying";

// The line of the shader build's failure dialog (it has RETRY, LOG FOLDER and QUIT already).
inline constexpr const char* kShaderDialogText =
    "The compatibility renderer needs none of these shaders, and has fewer graphics options.";
inline constexpr const char* kShaderDialogPrompt = "COMPATIBILITY RENDERER";

// What to tell a player when the button could not write larecomp.toml or start the game again.
inline std::string ManualText(std::string_view driver, bool written) {
  return written ? std::string("The compatibility renderer is set, but the game could not be started again. Start it again.")
                 : "The game could not write larecomp.toml. To use the compatibility renderer, add this line to it and start the game again:\n" +
                       ConfigLine(driver);
}

// ---------------------------------------------------------------------------
// Starts that never got going
// ---------------------------------------------------------------------------
// A native renderer start is "unfinished" until the game has presented kFramesToFinish frames, or the game closed on its own (the window,
// the pause menu's QUIT, a failed start that told the player). A start that is still unfinished when the next one begins was ended from
// outside: a crash or a kill. The count of such starts is one number in a file next to the log (renderer_start.txt); no file means 0. At
// kOfferAfterUnfinished in a row the next start offers the compatibility renderer, once: choosing "keep trying" starts the count again.
// Only the native renderer arms it, so the XENIA renderer, the batch actions and a PC with no file access never count anything.
inline constexpr int kOfferAfterUnfinished = 2;
inline constexpr int kFramesToFinish = 120;
inline constexpr std::string_view kStartFileName = "renderer_start.txt";

constexpr bool OfferAfter(int unfinished_starts) { return unfinished_starts >= kOfferAfterUnfinished; }

// The count in a file: 0 when there is none, it cannot be read, or it holds anything but a small number.
inline int ReadUnfinished(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  int count = 0;
  if (!in || !(in >> count) || count < 0 || count > 1000) {
    return 0;
  }
  return count;
}

inline bool WriteUnfinished(const std::filesystem::path& file, int count) {
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out << count << '\n';
  return static_cast<bool>(out);
}

inline void ClearUnfinished(const std::filesystem::path& file) {
  std::error_code ec;
  std::filesystem::remove(file, ec);
}

class StartWatch {
 public:
  // Before the renderer starts (UI thread). Returns how many starts before this one never finished, and counts this one as unfinished.
  // Nothing is armed, and 0 comes back, when the file cannot be written.
  int Arm(std::filesystem::path file) {
    const int before = ReadUnfinished(file);
    if (!WriteUnfinished(file, before + 1)) {
      return 0;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      file_ = std::move(file);
    }
    frames_.store(0, std::memory_order_relaxed);
    armed_.store(true, std::memory_order_release);
    return before;
  }

  // One guest frame was presented (the guest thread, at the rate of the frames: one load while nothing is armed).
  void NoteFrame() {
    if (!armed_.load(std::memory_order_acquire)) {
      return;
    }
    if (frames_.fetch_add(1, std::memory_order_relaxed) + 1 == kFramesToFinish) {
      Finish();
    }
  }

  // This start is not a crash: it finished, or it ended by itself. The file goes away. Safe to call any number of times.
  void Finish() {
    if (!armed_.exchange(false, std::memory_order_acq_rel)) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    ClearUnfinished(file_);
  }

  // The player chose to keep trying after the offer: this start is the first of a new count.
  void CountAgain() {
    if (!armed_.load(std::memory_order_acquire)) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    WriteUnfinished(file_, 1);
  }

  bool Armed() const { return armed_.load(std::memory_order_acquire); }

 private:
  std::mutex mutex_;
  std::filesystem::path file_;
  std::atomic<bool> armed_{false};
  std::atomic<int> frames_{0};
};

// The one in the program: the app arms it and finishes it, the renderer counts the frames.
inline StartWatch& Watch() {
  static StartWatch& watch = larecomp::Leak<StartWatch>();
  return watch;
}

}  // namespace mcla::gpu::fallback
