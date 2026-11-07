#pragma once

// LARecomp builds its shader packs on the player's PC, from the player's own game files
// (src/shaderpack, shaderpack/PIPELINE-PLAN.md). This is the app's side of it: where it runs,
// what the player sees, and what happens when it goes wrong.
//
//   OnConfigurePaths   ConfigurePackFolder       the packs follow a cache folder the player chose
//   SetupPresentation  InstallStartupGate        before the renderer starts: it attaches the packs
//                                                that are in place only when their stamp is current
//   LaunchModule       Phase                     behind the boot progress popup, before the guest
//                                                starts: build both formats (DXIL and SPIR-V, so a
//                                                switch of graphics API needs no build), attach
//   SetupEnvironment   RunCommandLine            larecomp.exe --build_shader_packs: the same build
//                                                with no window and no runtime (the launcher's setup
//                                                runs it, so the first start has nothing to build),
//                                                then the program ends
//
// Nothing here applies when the Xenia backend runs (mcla_gpu off): it needs no pack.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace rex::ui {
class ImGuiDrawer;
class WindowedAppContext;
}  // namespace rex::ui

namespace larecomp::shaderbuild {

// UI thread, from OnConfigurePaths (the paths are final enough: logging is not up yet). When the
// player chose a cache folder (the cache_root setting) the packs go to <cache_root>/shaders; else
// the renderer's default is used (next to the program, or the local application data folder).
void ConfigurePackFolder(const std::filesystem::path& cache_root);

// UI thread, from SetupPresentation, before mcla::gpu::Startup. A pack that is in place and
// whose stamp says it was built from other inputs than today's (the game folder, the compiler,
// the builder) is not attached by Startup: it is rebuilt first, and a pack the renderer has
// mapped could not be replaced. A pack with no stamp at all (an older install's, or one put in
// shader_pack_dir by hand) is used as it is. `game_root` is the folder the game files are in.
void InstallStartupGate(const std::filesystem::path& game_root);

// UI thread, from SetupEnvironment (the paths and the config are final, logging is up, nothing else
// has started): whether the program was asked to build the shader packs and close
// (--build_shader_packs).
bool CommandLineRequested();

// Builds the packs the way the game would at its first start (same folder, formats, compiler,
// HDR step and stamp, so the game finds them valid and builds nothing), reading the game's
// executable image from `game_root`/default.xex instead of guest memory. Prints progress for the
// program that started it (the line format is described in the .cpp). Returns the exit code: 0
// when the packs are in place, 1 when not (the game then builds them itself when it starts).
// Packs that are current are left alone (build_shader_packs_force builds anyway).
int RunCommandLine(const std::filesystem::path& game_root);

// The shader phase of the boot work. Made on the UI thread at LaunchModule, run on the worker
// thread that builds the mods, finished on the UI thread before the guest starts.
class Phase {
 public:
  // `guest_memory` is the runtime's virtual_membase(): the game's executable is loaded in it at
  // 0x82000000, which is where the shaders it carries are read from. `force` builds even when the
  // stamp says the packs are current (a Retry after the renderer refused a pack that was built).
  Phase(const std::filesystem::path& game_root, const uint8_t* guest_memory, bool force);

  // True when the renderer has no pack yet: first start, a stale stamp (see InstallStartupGate),
  // or a pack that lacks a format. False with the Xenia backend and when a pack is attached.
  bool Needed() const { return needed_; }

  // Worker thread. Checks the packs against the game files and builds what is missing (about
  // fifteen seconds on a fast PC, a few minutes on a slow one), reporting to the popup, which
  // offers CANCEL meanwhile. Does nothing when not Needed.
  void Run();

  // True while nothing has gone wrong: also when nothing was needed. Valid after Run.
  bool Ok() const { return kind_ == Kind::kNone || kind_ == Kind::kBuilt; }

  // UI thread, after Run: attaches the packs to the renderer. False when the phase failed, was
  // cancelled, or the renderer refused the pack; the game must not start then (ShowFailure).
  bool Finish();

  // UI thread, after Finish returned false: tells the player and offers Retry / Open log folder /
  // Quit (a cancelled build: Retry / Quit). `retry` is called on the UI thread with whether the
  // next build has to be forced; `quit` closes the app. Nothing is shown when the app is already
  // closing. Without a drawer a plain message box says it and `quit` is called.
  void ShowFailure(rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext& app_context,
                   std::function<void(bool force)> retry, std::function<void()> quit);

 private:
  enum class Kind { kNone, kBuilt, kCancelled, kClosing, kFailed };

  std::filesystem::path game_root_;
  const uint8_t* guest_memory_ = nullptr;
  bool force_ = false;

  bool needed_ = false;
  bool first_start_ = false;  // no stamp: the packs were never built here
  uint32_t formats_ = 0;
  std::filesystem::path folder_;
  std::filesystem::path log_file_;

  Kind kind_ = Kind::kNone;
  std::string reason_;         // plain English, for the dialog
  bool retry_forces_ = false;  // the next build has to ignore the stamp
};

}  // namespace larecomp::shaderbuild
