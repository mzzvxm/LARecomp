#pragma once

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <array>
#include <cstdint>

namespace mcla::ui {

class MclaDebugOverlayDialog : public rex::ui::ImGuiDialog {
 public:
  explicit MclaDebugOverlayDialog(rex::ui::ImGuiDrawer* drawer);
  ~MclaDebugOverlayDialog() override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  static constexpr size_t kFrameHistorySize = 120;
  std::array<float, kFrameHistorySize> frame_time_history_{};
  size_t frame_history_idx_ = 0;
};

void ToggleDebugOverlay(rex::ui::ImGuiDrawer* drawer);
bool IsDebugOverlayOpen();

} // namespace mcla::ui
