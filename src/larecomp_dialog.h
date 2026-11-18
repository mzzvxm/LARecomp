#pragma once
//
// The 0.10 rex::ui::ImGuiDialog on rexglue 0.11's dialog model (include/rex/ui/ui_system.h).
//
// 0.10: a dialog was constructed with the ImGuiDrawer, showed itself, overrode OnDraw(ImGuiIO&) and OnClose(), called
// Close() to go away and was deleted by the drawer after the frame that closed it.
// 0.11: a dialog is an rex::ui::IDialog the program hands to IUISystem::AddDialog(std::unique_ptr). The UI system calls
// Draw(io) every frame, and when Draw returns false it calls OnClose() once and destroys the dialog. DialogBase gives the
// defaults (continuous paint, no exclusive input).
//
// This adapter keeps the 0.10 shape, so each dialog changes by its base class and its constructor, and the code in
// OnDraw does not change:
//   class MyDialog final : public larecomp::ui::ImGuiDialog { void OnDraw(ImGuiIO& io) override; ... };
//   larecomp::ui::ShowDialog<MyDialog>(ui, args...);          // the UI system owns it from here
// Close() may be called from OnDraw (the usual place: a button was pressed) or from the UI thread between frames. It takes
// effect when the current Draw returns, and OnClose() then runs once, on the UI thread.
// CHECK: whether the UI system calls OnClose() for a dialog still open when it stops (IDialog says "after Draw returned
// false"; ui_system.cpp decides): the dialogs below only join a thread or reset a shared state there, and none of them
// depends on it.
//
// Any thread may call Close() (the flag is atomic), but a dialog only draws on the UI thread; IUISystem::AddDialog is
// called from the UI thread too (the wizards run in Init/LaunchTitle, which are on it).
//
// Used by: the wizards (isoinstaller, profile picker, save importer, boot progress, shader build failure). The overlays of
// mc_engine/ui, online and modloader move on to IDialog in the UI port (WP-L2B) and may use this class or DialogBase.
//
#include <atomic>
#include <memory>
#include <utility>

#include <rex/ui/ui_system.h>

namespace larecomp::ui {

class ImGuiDialog : public rex::ui::DialogBase {
 public:
  bool Draw(ImGuiIO& io) final {
    if (closed_.load(std::memory_order_acquire)) return false;
    OnDraw(io);
    return !closed_.load(std::memory_order_acquire);
  }

  // Ask the UI system to remove the dialog once the current Draw is over.
  void Close() { closed_.store(true, std::memory_order_release); }
  bool IsClosed() const { return closed_.load(std::memory_order_acquire); }

 protected:
  virtual void OnDraw(ImGuiIO& io) = 0;

 private:
  std::atomic<bool> closed_{false};
};

// Constructs a dialog and gives it to the UI system, which destroys it when it closes. Returns a pointer for the caller that
// needs to Close() it later from another place; it is valid until the dialog's last Draw has returned false.
template <typename Dialog, typename... Args>
Dialog* ShowDialog(rex::ui::IUISystem& ui, Args&&... args) {
  auto dialog = std::make_unique<Dialog>(std::forward<Args>(args)...);
  Dialog* raw = dialog.get();
  ui.AddDialog(std::unique_ptr<rex::ui::IDialog>(std::move(dialog)));
  return raw;
}

}  // namespace larecomp::ui
