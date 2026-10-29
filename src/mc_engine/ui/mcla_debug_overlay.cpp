#include "mcla_debug_overlay.h"
#include "mcla_imgui_theme.h"

#include <rex/cvar.h>
#include <rex/perf/counter.h>
#include <rex/version.h>
#include <rex/ui/flags.h>
#include <imgui.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <memory>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#endif

REXCVAR_DECLARE(bool, mcla_native_gfx);
REXCVAR_DECLARE(double, video_mode_refresh_rate);

namespace mcla::ui {

static std::unique_ptr<MclaDebugOverlayDialog> g_debug_dialog;

namespace {

// Width of the card's content: the meter and graph run this wide, the window
// fits around them like the retail GPS card.
constexpr float kContentWidth = 320.0f;

// "LABEL ........ VALUE": dim label, value flush right.
void StatRow(const char* label, const char* value) {
  ImGui::TextDisabled("%s", label);
  ImGui::SameLine();
  const float w = ImGui::CalcTextSize(value).x;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - w);
  ImGui::TextUnformatted(value);
}

}  // namespace

MclaDebugOverlayDialog::MclaDebugOverlayDialog(rex::ui::ImGuiDrawer* drawer)
    : ImGuiDialog(drawer) {
  frame_time_history_.fill(16.67f);
}

MclaDebugOverlayDialog::~MclaDebugOverlayDialog() = default;

void MclaDebugOverlayDialog::OnDraw(ImGuiIO& io) {
  ImGui::SetNextWindowPos(ImVec2(24.0f, 24.0f), ImGuiCond_FirstUseEver);

  const std::string key = rex::cvar::GetFlagByName("bind_mcla_telemetry");
  const Prompt prompts[] = {{"CLOSE", key.empty() ? "F3" : key.c_str(), kPromptBack}};

  bool close = false;
  if (BeginPanel("##mcla_telemetry", "TELEMETRY", PanelIcon::kInfo,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing,
                 prompts)) {
    const int64_t ft_us = rex::perf::GetSnapshotCounter(rex::perf::CounterId::kFrameTimeUs);
    const float ft_ms = static_cast<float>(ft_us) / 1000.0f;
    const int64_t fps = rex::perf::GetSnapshotCounter(rex::perf::CounterId::kFps);

    frame_time_history_[frame_history_idx_] = ft_ms;
    frame_history_idx_ = (frame_history_idx_ + 1) % kFrameHistorySize;

    const long long display_fps =
        fps > 0 ? static_cast<long long>(fps)
                : static_cast<long long>(1000.0f / std::max(0.1f, ft_ms));
    const int refresh = static_cast<int>(REXCVAR_GET(video_mode_refresh_rate));
    const int target_hz = refresh > 0 ? refresh : 60;

    // The headline reads like the GPS card's "ROOKIE - 3640 REP" line, with
    // the meter under it filling toward the target rate.
    char line[64];
    std::snprintf(line, sizeof(line), "%lld FPS - %.2f MS", display_fps, ft_ms);
    ImGui::TextUnformatted(line);
    SegmentMeter(static_cast<float>(display_fps) / static_cast<float>(target_hz), kContentWidth);
    ImGui::Dummy(ImVec2(0.0f, 2.0f));

    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.55f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PlotLines("##frametimes", frame_time_history_.data(),
                     static_cast<int>(kFrameHistorySize),
                     static_cast<int>(frame_history_idx_), nullptr, 0.0f, 40.0f,
                     ImVec2(kContentWidth, 56.0f));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0.0f, 2.0f));

    StatRow("RENDERER", REXCVAR_GET(mcla_native_gfx) ? "NATIVE GFX" : "REXGLUE GFX");

    const bool vsync_on = (rex::cvar::GetFlagByName("vsync") == "true") ||
                          (rex::cvar::GetFlagByName("mcla_native_gfx_nocp_vsync") == "true");
    std::snprintf(line, sizeof(line), "%s - %d HZ", vsync_on ? "ON" : "OFF", target_hz);
    StatRow("VSYNC", line);

#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
      const double mem_mb = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
      std::snprintf(line, sizeof(line), "%.0f MB", mem_mb);
      StatRow("HOST RAM", line);
    }
#endif
  }
  close = EndPanel() == 0;

  // The build stamp the SDK's overlay carried, centered near the bottom.
  ImGui::PushFont(nullptr, kSmallSize);
  const ImVec2 text_size = ImGui::CalcTextSize(REXGLUE_BUILD_STAMP);
  ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - text_size.x) * 0.5f,
                                 io.DisplaySize.y - text_size.y - io.DisplaySize.y * 0.03f));
  ImGui::SetNextWindowSize(ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, imgui_drawer()->style().debug.muted_text);
  if (ImGui::Begin("##mcla_watermark", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                       ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted(REXGLUE_BUILD_STAMP);
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
  ImGui::PopFont();

  if (close) {
    // ImGuiDialog::Draw deletes a closed dialog itself, so the owner lets go
    // first; otherwise the next toggle would delete it a second time.
    (void)g_debug_dialog.release();
    Close();
  }
}

void ToggleDebugOverlay(rex::ui::ImGuiDrawer* drawer) {
  if (!drawer) return;
  if (g_debug_dialog) {
    g_debug_dialog.reset();
  } else {
    g_debug_dialog = std::make_unique<MclaDebugOverlayDialog>(drawer);
  }
}

bool IsDebugOverlayOpen() {
  return g_debug_dialog != nullptr;
}

} // namespace mcla::ui
