#include "achievements.h"
#include "imgui_theme.h"
#include "overlay_support.h"

#include <rex/xam/achievement_manager.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cstdio>
#include <memory>
#include <string>

namespace mcla::ui {

namespace {

constexpr float kIcon = 44.0f;
constexpr float kScoreSlack = 10.0f;

// The running title's manager (OnTitleLaunch sets it, the unload clears it). Read on the UI thread.
std::atomic<rex::xam::IAchievementManager*> g_manager{nullptr};

// --- List (F7) ------------------------------------------------------------------------

class AchievementsDialog final : public PanelDialog {
 protected:
  void OnDraw(ImGuiIO& io) override {
    const std::string key = BindText("mcla_achievements", "F7");
    const Prompt prompts[] = {{"CLOSE", key.c_str(), kPromptBack}};

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(820.0f, io.DisplaySize.x - 48.0f),
                                    std::min(600.0f, io.DisplaySize.y - 48.0f)),
                             ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(480.0f, 320.0f), io.DisplaySize);

    if (BeginPanel("##mcla_achievements", "ACHIEVEMENTS", PanelIcon::kCheck,
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                       ImGuiWindowFlags_NoScrollWithMouse,
                   prompts)) {
      DrawList();
    }
    const bool close = EndPanel() == 0 || (!io.WantTextInput &&
                                           ImGui::IsKeyPressed(ImGuiKey_Escape, false));
    // The overlay stays in the UI system's table; the key shows it again.
    if (close) {
      Close();
    }
  }

 private:
  void DrawList() {
    rex::xam::IAchievementManager* manager = g_manager.load(std::memory_order_acquire);
    const auto list = manager ? manager->ListAchievements() : std::vector<rex::Achievement>{};
    int unlocked = 0;
    unsigned earned = 0;
    unsigned total = 0;
    for (const auto& a : list) {
      total += a.gamerscore;
      if (a.unlocked) {
        ++unlocked;
        earned += a.gamerscore;
      }
    }

    // The GPS card's headline and meter.
    char line[96];
    std::snprintf(line, sizeof(line), "%d / %d UNLOCKED - %uG / %uG", unlocked,
                  static_cast<int>(list.size()), earned, total);
    ImGui::TextUnformatted(line);
    SegmentMeter(list.empty() ? 0.0f : static_cast<float>(unlocked) / static_cast<float>(list.size()),
                 ImGui::GetContentRegionAvail().x, 20);
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    const float height = ImGui::GetContentRegionAvail().y - PanelBottomReserve();
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8.0f, 6.0f));
    if (ImGui::BeginTable("##achievements", 3,
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_SizingStretchProp,
                          ImVec2(0.0f, height))) {
      ImGui::TableSetupColumn("icon", ImGuiTableColumnFlags_WidthFixed, kIcon);
      ImGui::TableSetupColumn("text", ImGuiTableColumnFlags_WidthStretch);
      // Room past the text, so the score never sits on the panel's edge (and a
      // slanted face's last glyph, leaning out of its advance, is not cut).
      ImGui::TableSetupColumn("score", ImGuiTableColumnFlags_WidthFixed,
                              ImGui::CalcTextSize("000G").x + kScoreSlack);
      for (const auto& a : list) {
        const bool done = a.unlocked;
        ImGui::TableNextRow(ImGuiTableRowFlags_None, kIcon);
        ImGui::PushID(static_cast<int>(a.id));

        // Locked entries are dimmed, the way the game's lists show what is
        // not yet available.
        ImGui::TableNextColumn();
        // No icon: see achievements.h (the SDK gives an app no way to read the title's icons).
        ImGui::Dummy(ImVec2(kIcon, kIcon));

        ImGui::TableNextColumn();
        ImGui::TextColored(done ? kColorCyanGlow : kColorTextWhite, "%s", a.label.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, done ? ImVec4(0.80f, 0.80f, 0.77f, 1.0f) : kColorTextDim);
        ImGui::TextWrapped("%s", a.GetDescription().c_str());
        ImGui::PopStyleColor();

        ImGui::TableNextColumn();
        char score[16];
        std::snprintf(score, sizeof(score), "%uG", a.gamerscore);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x -
                             ImGui::CalcTextSize(score).x - kScoreSlack);
        ImGui::TextColored(done ? kColorYellow : kColorTextDim, "%s", score);
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    ImGui::PopStyleVar();
  }

};

}  // namespace

void SetAchievementManager(rex::xam::IAchievementManager* manager) {
  g_manager.store(manager, std::memory_order_release);
}

rex::ui::OverlayConfig AchievementsOverlayConfig() {
  return {"mcla_achievements", {rex::VirtualKey::kF7},
          [](rex::ui::IUISystem&) -> std::unique_ptr<rex::ui::IDialog> {
            return std::make_unique<AchievementsDialog>();
          }};
}

} // namespace mcla::ui
