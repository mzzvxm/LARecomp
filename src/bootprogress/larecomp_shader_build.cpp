#include "larecomp_shader_build.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/system.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/windowed_app_context.h>

#include "larecomp_log.h"
#include "larecomp_version.h"
#include "bootprogress/larecomp_boot_progress.h"
#include "mc_engine/boot_progress.h"
#include "mc_engine/ui/imgui_theme.h"
#include "shaderpack/builder.h"

#if defined(LARECOMP_HAS_GPU)
#include "gpu/gpu.h"
#endif

REXCVAR_DEFINE_INT32(
    shader_pack_threads, 0, "MCLA/GPU",
    "How many threads build the shader packs from your game files on the first start. 0 chooses "
    "by itself from the processor's cores and the free memory. A lower number leaves the PC more "
    "usable during the build, which then takes longer.")
    .range(0, 64)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace larecomp::shaderbuild {
namespace {

namespace sp = larecomp::shaderpack;

using Clock = std::chrono::steady_clock;

constexpr const char* kFirstStartText = "Preparing shaders from your game files (first start only)...";
constexpr const char* kUpdateText = "Updating the shaders from your game files...";

// Both formats are built, so that switching between D3D12 and Vulkan needs no build. DXIL is
// only built on Windows.
constexpr uint32_t AllFormats() {
#if defined(_WIN32)
  return sp::kDxil | sp::kSpirv;
#else
  return sp::kSpirv;
#endif
}

// The shader compiler (dxcompiler.dll and dxil.dll) is the pair the release already ships in the
// D3D12 folder next to the program (the Agility SDK's folder). Defined here once: the startup
// check, the stamp check and the build request all take it from this function.
std::filesystem::path ShaderCompilerFolder() {
  return rex::filesystem::GetExecutableFolder() / "D3D12";
}

// The HDR variant pack's step. CHECK: src/shaderpack/hdr_variant.* (agent W6) has no factory in
// this branch yet. Once it lands, return its step here: the startup check, the check in
// BuildShaderPacks and the build request all read it from this one place. Null: no HDR pack is
// built or looked for.
sp::HdrStep* HdrStepForBuild() {
  return nullptr;
}

std::string PathUtf8(const std::filesystem::path& path) {
  const std::u8string u8 = path.u8string();
  return std::string(u8.begin(), u8.end());
}

// Where the logs are: next to the file the logger writes (LARECOMP_LOG_FILE), else ./logs, as
// InitLarecompLogging does.
std::filesystem::path LogFolder() {
  std::error_code ec;
  if (const char* file = std::getenv("LARECOMP_LOG_FILE"); file && *file) {
    const std::filesystem::path parent = std::filesystem::path(file).parent_path();
    if (!parent.empty()) return std::filesystem::absolute(parent, ec);
  }
  return std::filesystem::absolute("logs", ec);
}

void OpenFolder(const std::filesystem::path& folder) {
#ifdef _WIN32
  const std::wstring dir = folder.wstring();
  ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
  (void)folder;
#endif
}

// The build's progress goes to the boot popup, its log lines to the app's log and to
// logs/shader_build.txt (all of them: the file is what a support request needs).
class AppSink final : public sp::ProgressSink {
 public:
  explicit AppSink(const std::filesystem::path& file) : started_(Clock::now()) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    file_.open(file, std::ios::out | std::ios::trunc);
  }

  void BeginPhase(const std::string& name, int total) override {
    mc::boot::BeginPhase(name, total);
    Line(total > 0 ? "phase: " + name + " (" + std::to_string(total) + " steps)" : "phase: " + name);
  }
  void Step(const std::string& item) override { mc::boot::Step(item); }
  void Log(sp::LogLevel level, const std::string& text) override {
    switch (level) {
      case sp::LogLevel::kInfo:
        LARECOMP_APP_INFO("[shaderpack] {}", text);
        break;
      case sp::LogLevel::kWarn:
        LARECOMP_APP_WARN("[shaderpack] {}", text);
        break;
      case sp::LogLevel::kError:
        LARECOMP_APP_ERROR("[shaderpack] {}", text);
        break;
    }
    Line(std::string(level == sp::LogLevel::kError  ? "error: "
                     : level == sp::LogLevel::kWarn ? "warning: "
                                                    : "") +
         text);
  }
  bool Cancelled() const override { return larecomp::BootBuildCancelRequested(); }

  void Line(const std::string& text) {
    if (!file_) return;
    const double seconds = std::chrono::duration<double>(Clock::now() - started_).count();
    char stamp[24];
    std::snprintf(stamp, sizeof(stamp), "[%7.1f s] ", seconds);
    file_ << stamp << text << '\n';
    file_.flush();
  }

  void Summary(const sp::BuildResult& r) {
    Line(std::string("result: ") + sp::StatusName(r.status) + (r.message.empty() ? "" : ": " + r.message));
    if (!r.first_shader.empty()) Line("first failing shader: " + r.first_shader);
    for (const std::string& problem : r.problems) Line("also: " + problem);
    char times[160];
    std::snprintf(times, sizeof(times), "times: read %.1f s, compile %.1f s, write %.1f s, hdr %.1f s, total %.1f s",
                  r.seconds_read, r.seconds_compile, r.seconds_write, r.seconds_hdr, r.seconds_total);
    Line(times);
    Line("shaders " + std::to_string(r.shaders) + " (" + std::to_string(r.vertex_shaders) + " vertex, " +
         std::to_string(r.pixel_shaders) + " pixel), blobs compiled " + std::to_string(r.blobs_compiled) +
         ", reused " + std::to_string(r.blobs_reused) + ", threads " + std::to_string(r.threads));
  }

 private:
  std::ofstream file_;
  Clock::time_point started_;
};

// The dialog after a build that failed or was cancelled: the short reason, and what can be done.
// Retry runs the build again from the start; the game cannot draw without the packs.
class FailureDialog final : public rex::ui::ImGuiDialog {
 public:
  FailureDialog(rex::ui::ImGuiDrawer* drawer, bool cancelled, std::string reason,
                std::filesystem::path log_folder, std::function<void()> retry,
                std::function<void()> quit)
      : ImGuiDialog(drawer),
        cancelled_(cancelled),
        reason_(std::move(reason)),
        log_folder_(std::move(log_folder)),
        retry_(std::move(retry)),
        quit_(std::move(quit)) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    const float width = std::min(640.0f, std::max(420.0f, io.DisplaySize.x - 64.0f));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);

    const mcla::ui::Prompt failed_prompts[] = {
        {"RETRY", "ENTER", mcla::ui::kPromptAccept},
        {"LOG FOLDER", "L", mcla::ui::kPromptX},
        {"QUIT", "ESC", mcla::ui::kPromptBack},
    };
    const mcla::ui::Prompt cancelled_prompts[] = {
        {"RETRY", "ENTER", mcla::ui::kPromptAccept},
        {"QUIT", "ESC", mcla::ui::kPromptBack},
    };
    const std::span<const mcla::ui::Prompt> prompts =
        cancelled_ ? std::span<const mcla::ui::Prompt>(cancelled_prompts)
                   : std::span<const mcla::ui::Prompt>(failed_prompts);

    if (mcla::ui::BeginPanel("##larecomp_shader_build_failed",
                             cancelled_ ? "SHADER BUILD STOPPED" : "SHADERS NOT READY",
                             mcla::ui::PanelIcon::kAlert,
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_AlwaysAutoResize,
                             prompts)) {
      mcla::ui::PanelSubtitle(reason_.c_str());
      ImGui::Dummy(ImVec2(0.0f, 4.0f));
      ImGui::PushStyleColor(ImGuiCol_Text, mcla::ui::kColorTextDim);
      ImGui::TextWrapped("%s",
                         "The game needs these shaders to draw, and builds them from your game "
                         "files. Retry starts the build again from the beginning.");
      if (!cancelled_) {
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        ImGui::TextWrapped("Details: %s", PathUtf8(log_folder_ / "shader_build.txt").c_str());
      }
      ImGui::PopStyleColor();
    }
    const int clicked = mcla::ui::EndPanel();

    if (done_) return;
    const int quit_index = static_cast<int>(prompts.size()) - 1;
    const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                       ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
    if (clicked == 0 || enter) {
      Finish(retry_);
    } else if (clicked == quit_index || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      Finish(quit_);
    } else if (!cancelled_ && (clicked == 1 || ImGui::IsKeyPressed(ImGuiKey_L, false))) {
      OpenFolder(log_folder_);  // stays up: the player reads the log, then picks
    }
  }

 private:
  void Finish(const std::function<void()>& action) {
    done_ = true;
    Close();
    if (action) action();
  }

  bool cancelled_;
  std::string reason_;
  std::filesystem::path log_folder_;
  std::function<void()> retry_;
  std::function<void()> quit_;
  bool done_ = false;
};

// A short reason for the dialog: the build's own plain-English message, cut when a compiler's
// error text runs long (the whole of it is in logs/shader_build.txt).
std::string ShortReason(const std::string& message) {
  constexpr size_t kMax = 600;
  if (message.size() <= kMax) return message;
  return message.substr(0, kMax - 3) + "...";
}

// Whether the stamp next to the packs vouches for them (and so whether Startup may attach them).
bool PacksCurrentAtStartup(const std::filesystem::path& game_root) {
#if defined(LARECOMP_HAS_GPU)
  const std::filesystem::path folder = mcla::gpu::ShaderPackFolder();
  const sp::GameFilesQuick files{game_root, ShaderCompilerFolder()};
  const sp::StampCheck check = sp::CheckStamp(folder, AllFormats(), files, HdrStepForBuild());
  if (check.stamp.empty()) {
    // No stamp: the packs of an older install, or ones put there by hand, are used as they are.
    // When there is no valid pack at all the shader phase builds one.
    LARECOMP_APP_INFO("[shaderpack] no stamp in {} ({}); a valid pack there is used as it is",
                      PathUtf8(folder), check.why);
    return true;
  }
  if (check.main_ok && check.hdr_ok) {
    return true;
  }
  LARECOMP_APP_INFO("[shaderpack] the packs in {} are not current: {}; they are rebuilt first",
                    PathUtf8(folder), check.why);
#else
  (void)game_root;
#endif
  return false;
}

}  // namespace

void ConfigurePackFolder(const std::filesystem::path& cache_root) {
#if defined(LARECOMP_HAS_GPU)
  // Only a cache folder the player named moves the packs; otherwise the renderer picks (next to
  // the program, or the local application data folder where that cannot be written).
  if (cache_root.empty() || rex::cvar::GetFlagByName("cache_root").empty()) return;
  mcla::gpu::SetShaderPackFolder(cache_root / "shaders");
#else
  (void)cache_root;
#endif
}

void InstallStartupGate(const std::filesystem::path& game_root) {
#if defined(LARECOMP_HAS_GPU)
  if (!mcla::gpu::Enabled()) return;
  mcla::gpu::SetShaderPackGate([game_root]() { return PacksCurrentAtStartup(game_root); });
#else
  (void)game_root;
#endif
}

Phase::Phase(const std::filesystem::path& game_root, const uint8_t* guest_memory, bool force)
    : game_root_(game_root), guest_memory_(guest_memory), force_(force) {
#if defined(LARECOMP_HAS_GPU)
  // The Xenia backend (mcla_gpu off) needs no pack. With the Plume renderer a pack that is
  // attached is in use and is left alone.
  if (mcla::gpu::Enabled() && !mcla::gpu::ShaderPacksAttached()) {
    needed_ = true;
    formats_ = AllFormats();
    folder_ = mcla::gpu::ShaderPackFolder();
    log_file_ = LogFolder() / "shader_build.txt";
    std::error_code ec;
    first_start_ = !std::filesystem::exists(folder_ / sp::kStampFileName, ec);
  }
#endif
}

void Phase::Run() {
  if (!needed_) return;

  larecomp::SetBootBuildHeading(first_start_ ? kFirstStartText : kUpdateText, true);
  AppSink sink(log_file_);
  sink.Line("building the shader packs into " + PathUtf8(folder_) + " from " + PathUtf8(game_root_));

  sp::BuildRequest request;
  request.game_root = game_root_;
  request.out_dir = folder_;
  request.dxc_dir = ShaderCompilerFolder();
  // The game's executable is in guest memory at 0x82000000, where the builder reads its shaders.
  // Nothing runs in the guest yet, and nothing writes there while the build reads.
  request.image.data = guest_memory_ ? guest_memory_ + sp::kGuestImageBase : nullptr;
  request.image.size = guest_memory_ ? sp::kGuestImageSize : 0;
  request.formats = formats_;
  request.threads = static_cast<unsigned>(std::max(0, REXCVAR_GET(shader_pack_threads)));
  request.force = force_;
  request.sink = &sink;
  request.hdr_step = HdrStepForBuild();
  request.app_version = LARECOMP_VERSION_LABEL;

  sp::BuildResult result;
  try {
    result = sp::BuildShaderPacks(request);
  } catch (const std::exception& e) {
    result.status = sp::Status::kCompileFailed;
    result.message = std::string("The shader build stopped unexpectedly: ") + e.what();
  } catch (...) {
    result.status = sp::Status::kCompileFailed;
    result.message = "The shader build stopped unexpectedly.";
  }
  sink.Summary(result);
  LARECOMP_APP_INFO("[shaderpack] {}: {}", sp::StatusName(result.status), result.message);

  mc::boot::EndPhase();
  larecomp::SetBootBuildHeading(std::string(), false);

  if (result.ok()) {
    kind_ = Kind::kBuilt;
    // The mods and the music that follow get the popup to themselves.
    mc::boot::Reset();
  } else if (result.status == sp::Status::kCancelled) {
    kind_ = larecomp::BootBuildClosing() ? Kind::kClosing : Kind::kCancelled;
    reason_ = "You stopped the shader build.";
  } else {
    kind_ = Kind::kFailed;
    reason_ = ShortReason(result.message);
  }
}

bool Phase::Finish() {
  if (!needed_) return true;
  if (kind_ == Kind::kNone) {
    kind_ = Kind::kFailed;
    reason_ = "The shader build did not run.";
  }
  if (kind_ != Kind::kBuilt) return false;
#if defined(LARECOMP_HAS_GPU)
  // Before the guest starts: nothing has drawn guest content yet. Attaching also starts the
  // pipeline cache and its precache against the packs.
  if (mcla::gpu::AttachShaderPacks(folder_)) {
    LARECOMP_APP_INFO("[shaderpack] the packs in {} are attached", PathUtf8(folder_));
    return true;
  }
#endif
  kind_ = Kind::kFailed;
  retry_forces_ = true;  // the stamp vouches for a pack the renderer does not accept
  reason_ = "The shaders were built, but the renderer could not load them. The log has the reason.";
  return false;
}

void Phase::ShowFailure(rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext& app_context,
                        std::function<void(bool force)> retry, std::function<void()> quit) {
  if (kind_ == Kind::kClosing) return;  // the app is going away: nobody to ask
  const bool cancelled = kind_ == Kind::kCancelled;
  LARECOMP_APP_WARN("[shaderpack] the game cannot start: {}", reason_);

  if (!drawer) {
    // Nothing to draw a dialog with: say it plainly and quit.
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error,
                              reason_ + "\n\nThe game needs these shaders to draw. Details: " +
                                  PathUtf8(LogFolder() / "shader_build.txt"));
    if (quit) quit();
    return;
  }

  // The buttons are pressed inside the dialog's own draw: what they start runs after it.
  const auto later = [&app_context](std::function<void()> action) -> std::function<void()> {
    if (!action) return {};
    return [&app_context, action = std::move(action)]() { app_context.CallInUIThreadDeferred(action); };
  };
  const bool force = retry_forces_;
  (void)new FailureDialog(
      drawer, cancelled, reason_, LogFolder(),
      later([retry = std::move(retry), force]() {
        if (retry) retry(force);
      }),
      later(std::move(quit)));
}

}  // namespace larecomp::shaderbuild
