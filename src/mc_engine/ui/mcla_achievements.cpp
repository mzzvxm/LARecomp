#include "mcla_achievements.h"
#include "mcla_imgui_theme.h"

#include <rex/cvar.h>
#include <rex/system/achievement_manager.h>
#include <rex/ui/overlay/achievement_icon_cache.h>
#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

namespace mcla::ui {

namespace {

using Clock = std::chrono::steady_clock;

constexpr float kIcon = 44.0f;
constexpr float kScoreSlack = 10.0f;

// --- Toast ----------------------------------------------------------------------------

class AchievementToast final : public rex::ui::AchievementNotificationDialog {
 public:
  // Matches the SDK toast's time per achievement, which OnDemandToast
  // (achievement_toast.h) budgets for.
  static constexpr float kDisplaySeconds = 4.5f;

  AchievementToast(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate,
                   rex::Runtime* runtime)
      : AchievementNotificationDialog(drawer), icons_(immediate, runtime) {}

  // Any thread: guest threads unlock achievements.
  void Push(const rex::system::AchievementEvent& event) override {
    std::lock_guard lock(mutex_);
    queue_.push_back({event, std::nullopt});
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    Entry toast;
    float age = 0.0f;
    {
      std::lock_guard lock(mutex_);
      const Clock::time_point now = Clock::now();
      // One at a time: each starts its clock when it first shows.
      while (!queue_.empty()) {
        if (!queue_.front().shown_since) queue_.front().shown_since = now;
        age = std::chrono::duration<float>(now - *queue_.front().shown_since).count();
        if (age < kDisplaySeconds) break;
        queue_.pop_front();
      }
      if (queue_.empty()) return;
      toast = queue_.front();
    }
    const float alpha =
        std::clamp(std::min(age / 0.3f, (kDisplaySeconds - age) / 0.5f), 0.0f, 1.0f);

    constexpr float kMargin = 16.0f;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - kMargin, io.DisplaySize.y - kMargin),
                            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    // As wide as the title needs (a fixed width cut it off), at least 360.
    ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    if (BeginPanel("##mcla_achievement_toast", "ACHIEVEMENT UNLOCKED", PanelIcon::kCheck,
                   ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                       ImGuiWindowFlags_AlwaysAutoResize,
                   {}, 25.0f)) {
      const auto& a = toast.event.achievement;
      if (rex::ui::ImmediateTexture* icon = icons_.GetIcon(a)) {
        ImGui::Image(reinterpret_cast<ImTextureID>(icon), ImVec2(kIcon, kIcon));
        ImGui::SameLine();
      }
      ImGui::BeginGroup();
      // An absolute wrap point: the window sizes itself to its content, so
      // wrapping at its own edge would never wrap.
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 300.0f);
      ImGui::TextUnformatted(a.label.c_str());
      ImGui::PopTextWrapPos();
      ImGui::TextColored(kColorYellow, "%uG", a.gamerscore);
      ImGui::EndGroup();
    }
    EndPanel();
    ImGui::PopStyleVar();
  }

 private:
  struct Entry {
    rex::system::AchievementEvent event;
    std::optional<Clock::time_point> shown_since;
  };

  rex::ui::AchievementIconCache icons_;
  std::mutex mutex_;
  std::deque<Entry> queue_;
};

// --- List (F7) ------------------------------------------------------------------------

void ReleaseList();

class AchievementsDialog final : public rex::ui::ImGuiDialog {
 public:
  AchievementsDialog(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate,
                     rex::Runtime* runtime, rex::system::AchievementManager* achievements)
      : ImGuiDialog(drawer), achievements_(achievements), icons_(immediate, runtime) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    const std::string key = rex::cvar::GetFlagByName("bind_mcla_achievements");
    const Prompt prompts[] = {{"CLOSE", key.empty() ? "F7" : key.c_str(), kPromptBack}};

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
    if (close) {
      // ImGuiDialog::Draw deletes a closed dialog itself: the owner lets go.
      ReleaseList();
      Close();
    }
  }

 private:
  void DrawList() {
    const auto list = achievements_->ListAchievements();
    int unlocked = 0;
    unsigned earned = 0;
    unsigned total = 0;
    for (const auto& a : list) {
      total += a.gamerscore;
      if (achievements_->IsUnlocked(a.id)) {
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
        const bool done = achievements_->IsUnlocked(a.id);
        ImGui::TableNextRow(ImGuiTableRowFlags_None, kIcon);
        ImGui::PushID(static_cast<int>(a.id));

        // Locked entries are dimmed, the way the retail lists show what is
        // not yet available.
        ImGui::TableNextColumn();
        if (rex::ui::ImmediateTexture* icon = icons_.GetIcon(a)) {
          ImGui::ImageWithBg(reinterpret_cast<ImTextureID>(icon), ImVec2(kIcon, kIcon),
                             ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0),
                             done ? ImVec4(1, 1, 1, 1) : ImVec4(0.45f, 0.45f, 0.45f, 0.8f));
        } else {
          ImGui::Dummy(ImVec2(kIcon, kIcon));
        }

        ImGui::TableNextColumn();
        ImGui::TextColored(done ? kColorCyanGlow : kColorTextWhite, "%s", a.label.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, done ? ImVec4(0.80f, 0.80f, 0.77f, 1.0f) : kColorTextDim);
        ImGui::TextWrapped("%s", (done ? a.description : a.unachieved_description).c_str());
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

  rex::system::AchievementManager* achievements_;
  rex::ui::AchievementIconCache icons_;
};

std::unique_ptr<AchievementsDialog> g_list;

void ReleaseList() {
  (void)g_list.release();
}

}  // namespace

std::unique_ptr<rex::ui::AchievementNotificationDialog> MakeAchievementToast(
    rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, rex::Runtime* runtime) {
  if (!drawer || !immediate || !runtime) return nullptr;
  return std::make_unique<AchievementToast>(drawer, immediate, runtime);
}

void ToggleAchievements(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate,
                        rex::Runtime* runtime, rex::system::AchievementManager* achievements) {
  if (g_list) {
    g_list.reset();
  } else if (drawer && immediate && runtime && achievements) {
    g_list = std::make_unique<AchievementsDialog>(drawer, immediate, runtime, achievements);
  }
}

} // namespace mcla::ui
