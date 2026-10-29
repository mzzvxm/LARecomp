#include "controller_toast.h"
#include "gamepad.h"
#include "imgui_theme.h"

#include <rex/cvar.h>
#include <rex/ui/imgui_dialog.h>
#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>

REXCVAR_DEFINE_BOOL(mcla_controller_toast, true, "MCLA",
    "Show the MCLA-styled toast when a controller connects or disconnects. Its corner follows "
    "controller_toast_position.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::ui {

namespace {

using Clock = std::chrono::steady_clock;

// Timings match the SDK toast this replaces.
constexpr float kDisplaySeconds = 5.0f;
constexpr float kFadeIn = 0.3f;
constexpr float kFadeOut = 0.5f;
constexpr size_t kMaxQueued = 8;
constexpr auto kPollInterval = std::chrono::milliseconds(250);

class ControllerToastDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit ControllerToastDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    const Clock::time_point now = Clock::now();
    if (now >= next_poll_) {
      next_poll_ = now + kPollInterval;
      Poll(now);
    }
    while (!toasts_.empty() &&
           std::chrono::duration<float>(now - toasts_.front().arrived).count() >= kDisplaySeconds) {
      toasts_.pop_front();
    }
    if (toasts_.empty()) {
      return;
    }

    const std::string corner = rex::cvar::GetFlagByName("controller_toast_position");
    const bool right = corner.empty() || corner == "Top Right" || corner == "Bottom Right";
    const bool bottom = corner == "Bottom Left" || corner == "Bottom Right";

    constexpr float kMargin = 16.0f;
    constexpr float kMinWidth = 300.0f;
    constexpr float kGap = 8.0f;
    float offset = 0.0f;
    for (size_t i = 0; i < toasts_.size(); ++i) {
      const Toast& toast = toasts_[i];
      const float age = std::chrono::duration<float>(now - toast.arrived).count();
      const float alpha = std::clamp(
          std::min(age / kFadeIn, (kDisplaySeconds - age) / kFadeOut), 0.0f, 1.0f);

      // Stacked away from the chosen corner, each one as tall as it draws.
      const ImVec2 pos(right ? io.DisplaySize.x - kMargin : kMargin,
                       bottom ? io.DisplaySize.y - kMargin - offset : kMargin + offset);
      ImGui::SetNextWindowPos(pos, ImGuiCond_Always,
                              ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
      // As wide as the title needs: a fixed width cut "CONTROLLER CONNECTED"
      // off at the right edge, and the title's face may change.
      ImGui::SetNextWindowSizeConstraints(ImVec2(kMinWidth, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);

      char id[32];
      std::snprintf(id, sizeof(id), "##mcla_ctrl_toast_%zu", i);
      float height = 0.0f;
      if (BeginPanel(id, toast.connected ? "CONTROLLER CONNECTED" : "CONTROLLER DISCONNECTED",
                     PanelIcon::kGamepad,
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_AlwaysAutoResize,
                     {}, 25.0f)) {
        height = ImGui::GetWindowHeight();
        if (toast.connected) {
          ImGui::TextUnformatted(toast.name.c_str());
        } else {
          ImGui::TextDisabled("%s", toast.name.c_str());
        }
      }
      EndPanel();
      ImGui::PopStyleVar();
      offset += height + kGap;
    }
  }

 private:
  struct Toast {
    std::string name;
    bool connected;
    Clock::time_point arrived;
  };

  // Diffs the pad list against the last poll. Pads already there on the
  // first poll toast as connected, as the SDK's toast did at startup.
  void Poll(Clock::time_point now) {
    std::unordered_map<uint32_t, std::string> present;
    for (PadInfo& pad : ListPads()) present.emplace(pad.id, std::move(pad.name));
    if (REXCVAR_GET(mcla_controller_toast)) {
      for (const auto& [id, name] : present) {
        if (!known_.contains(id)) Push(name, true, now);
      }
      for (const auto& [id, name] : known_) {
        if (!present.contains(id)) Push(name, false, now);
      }
    }
    known_ = std::move(present);
  }

  void Push(const std::string& name, bool connected, Clock::time_point now) {
    toasts_.push_back({name, connected, now});
    while (toasts_.size() > kMaxQueued) {
      toasts_.pop_front();
    }
  }

  std::deque<Toast> toasts_;
  // Names are kept so a disconnect can still say which pad left.
  std::unordered_map<uint32_t, std::string> known_;
  Clock::time_point next_poll_{};
};

std::unique_ptr<ControllerToastDialog> g_toasts;

}  // namespace

void StartControllerToasts(rex::ui::ImGuiDrawer* drawer) {
  if (!drawer || g_toasts) return;
  // For the whole session: the pads are listed from the exe's own SDL, which
  // nothing else keeps running in game (the SDK's SDL is another instance).
  EnsurePadLayer();
  g_toasts = std::make_unique<ControllerToastDialog>(drawer);
}

} // namespace mcla::ui
