#include "controller_toast.h"
#include "imgui_theme.h"
#include "overlay_input_core.h"
#include "mc_engine/input_state.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>

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

// The SDK's controller list changed (or has not been read yet): set by the input state on the thread that polls the pad,
// taken by the dialog on the UI thread. A plain static, not a member of the dialog: the listener table lives for the
// whole process and the dialog may be gone at exit.
std::atomic<bool> g_list_changed{true};

void OnControllersChanged(void*) { g_list_changed.store(true, std::memory_order_release); }

class ControllerToastDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit ControllerToastDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    const Clock::time_point now = Clock::now();
    // Nothing is read, listed or timed while nothing changed: one atomic exchange per drawn frame.
    if (g_list_changed.exchange(false, std::memory_order_acquire)) {
      Refresh(now);
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

  // The SDK's controller list changed: what came and what went since the last look. Pads already there at the first look
  // toast as connected, as the SDK's toast did at startup.
  void Refresh(Clock::time_point now) {
    const bool shown = REXCVAR_GET(mcla_controller_toast);
    for (ControllerEvent& event : roster_.Update(larecomp::input::Controllers())) {
      REXLOG_INFO("[toast] controller {}: {}{}", event.connected ? "connected" : "disconnected", event.name,
                  shown ? "" : " (toast off)");
      if (shown) Push(std::move(event.name), event.connected, now);
    }
  }

  void Push(std::string name, bool connected, Clock::time_point now) {
    toasts_.push_back({std::move(name), connected, now});
    while (toasts_.size() > kMaxQueued) {
      toasts_.pop_front();
    }
  }

  std::deque<Toast> toasts_;
  // Names are kept so a disconnect can still say which pad left.
  ControllerRoster roster_;
};

std::unique_ptr<ControllerToastDialog> g_toasts;

}  // namespace

void StartControllerToasts(rex::ui::ImGuiDrawer* drawer) {
  if (!drawer || g_toasts) return;
  // The pads come from the SDK's own device list, by the input state's events: it reads the list when the SDK's
  // device counter moves and calls us then. The flag starts set, so the first drawn frame takes the list as it is.
  if (!larecomp::input::AddControllerListener(&OnControllersChanged, nullptr)) {
    REXLOG_WARN("[toast] the input state's listener table is full: the controller toast will not see pads come and go");
  }
  g_toasts = std::make_unique<ControllerToastDialog>(drawer);
  REXLOG_INFO("[toast] controller toast ready: it follows the SDK's device list by events (no polling)");
}

} // namespace mcla::ui
