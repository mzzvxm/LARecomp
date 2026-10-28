#include "mcla_debug_overlay.h"
#include "mcla_imgui_theme.h"

#include <rex/cvar.h>
#include <rex/perf/counter.h>
#include <rex/version.h>
#include <rex/ui/flags.h>
#include <imgui.h>

#include <algorithm>
#include <cinttypes>
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

MclaDebugOverlayDialog::MclaDebugOverlayDialog(rex::ui::ImGuiDrawer* drawer)
    : ImGuiDialog(drawer) {
  frame_time_history_.fill(16.67f);
}

MclaDebugOverlayDialog::~MclaDebugOverlayDialog() = default;

void MclaDebugOverlayDialog::OnDraw(ImGuiIO& io) {
  const float fs = ImGui::GetFontSize();

  ImGui::SetNextWindowPos(ImVec2(fs * 1.0f, fs * 1.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(
      ImVec2(fs * 18.0f, 0.0f),
      ImVec2(io.DisplaySize.x * 0.50f, io.DisplaySize.y * 0.85f));

  // Authentic MCLA Card Styling
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 14.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

  ImGui::PushStyleColor(ImGuiCol_WindowBg, kColorDarkBg);
  ImGui::PushStyleColor(ImGuiCol_Border, kColorBorder);
  ImGui::PushStyleColor(ImGuiCol_TitleBg, kColorDarkBg);
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.08f, 0.10f, 0.13f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, kColorTextWhite);
  ImGui::PushStyleColor(ImGuiCol_PlotLines, kColorCyan);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.02f, 0.08f, 0.12f, 0.85f));

  bool open = true;
  if (ImGui::Begin("SYSTEM TELEMETRY##mcla_debug", &open,
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
    // Title in MCLA Yellow
    ImGui::PushStyleColor(ImGuiCol_Text, kColorYellow);
    if (GetBoldFont()) {
      ImGui::PushFont(GetBoldFont(), 17.0f);
    }
    ImGui::TextUnformatted("PERFORMANCE MONITOR");
    if (GetBoldFont()) {
      ImGui::PopFont();
    }
    ImGui::PopStyleColor();

    ImGui::Separator();

    // Read frame statistics
    const int64_t ft_us = rex::perf::GetSnapshotCounter(rex::perf::CounterId::kFrameTimeUs);
    const float ft_ms = static_cast<float>(ft_us) / 1000.0f;
    const int64_t fps = rex::perf::GetSnapshotCounter(rex::perf::CounterId::kFps);

    frame_time_history_[frame_history_idx_] = ft_ms;
    frame_history_idx_ = (frame_history_idx_ + 1) % kFrameHistorySize;

    // Header stats
    const long long display_fps = fps > 0 ? static_cast<long long>(fps) : static_cast<long long>(1000.0f / std::max(0.1f, ft_ms));
    ImGui::TextColored(kColorYellow, "%3lld FPS", display_fps);
    ImGui::SameLine();
    ImGui::Text(" (%.2f ms)", ft_ms);

    const bool native = REXCVAR_GET(mcla_native_gfx);
    ImGui::SameLine();
    ImGui::TextDisabled("| %s", native ? "NATIVE GFX" : "REXGLUE GFX");

    // Frame plot in glowing cyan
    ImGui::PlotLines("##frametimes", frame_time_history_.data(),
                     static_cast<int>(kFrameHistorySize),
                     static_cast<int>(frame_history_idx_),
                     nullptr, 0.0f, 40.0f,
                     ImVec2(fs * 20.0f, fs * 3.5f));

    ImGui::Dummy(ImVec2(0.0f, 2.0f));

    // Target reference line / VSync
    const bool vsync_on = (rex::cvar::GetFlagByName("vsync") == "true") || (rex::cvar::GetFlagByName("mcla_native_gfx_nocp_vsync") == "true");
    const int target_hz = static_cast<int>(REXCVAR_GET(video_mode_refresh_rate));
    ImGui::TextDisabled("V-Sync: %s | Target: %d Hz", vsync_on ? "ON" : "OFF", target_hz ? target_hz : 60);

#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
      const double mem_mb = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
      ImGui::TextDisabled("Host RAM: %.1f MB", mem_mb);
    }
#endif

    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    ImGui::TextDisabled("F3: Toggle  |  F4: Settings  |  F5: Mods");
  }
  ImGui::End();

  ImGui::PopStyleColor(7);
  ImGui::PopStyleVar(4);

  if (!open) {
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
