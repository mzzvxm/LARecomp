#pragma once

#include <rex/ui/ui_system.h>
#include <array>
#include <cstdint>

#include "overlay_support.h"

namespace mcla::ui {

class MclaDebugOverlayDialog : public PanelDialog {
 public:
  explicit MclaDebugOverlayDialog(rex::ui::IUISystem& ui);
  ~MclaDebugOverlayDialog() override;

 protected:
  void OnDraw(ImGuiIO& io) override;
  void OnOpened() override { frame_time_history_.fill(16.67f); }

 private:
  static constexpr size_t kFrameHistorySize = 120;
  std::array<float, kFrameHistorySize> frame_time_history_{};
  size_t frame_history_idx_ = 0;
  rex::ui::IUISystem* ui_;
};

// The F3 telemetry card as an overlay: ui.bind.mcla_telemetry (F3 by default). The SDK's own "debug" overlay
// keeps its name and is unbound by LarecompApp::StartOverlays, so the key reaches this one.
rex::ui::OverlayConfig DebugOverlayConfig();

} // namespace mcla::ui
