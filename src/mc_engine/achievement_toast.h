// Achievement pop-ups, registered with the ImGui drawer only while one shows.
//
// The SDK registers its achievement toast as an ImGui dialog for the whole
// session, and while any dialog is registered the ImGui drawer asks for another
// paint after every one, so the presenter showed the same game frame over and
// over at the monitor's refresh rate (240 times a second on a 240 Hz display),
// whatever the frame cap. This wrapper keeps the SDK's toast and registers it
// while an achievement is on screen only; the rest of the time the window is
// repainted when the game presents a frame.
//
// While it is on screen, it is drawn with the game's own frames instead of
// asking for repaints of its own (ImGuiDialog::RepaintsContinuously, from
// patches/rexglue-imgui-dialog-repaint-on-demand.patch; without that patch it
// still builds, and repaints while shown). Those extra presents, from the UI
// thread on the queue the GPU command thread submits to, halved the game's
// frame rate for as long as a toast was shown.
//
// From RecompiledHaloReach (reach-recomp/src/platform/achievement_toast.cpp),
// by way of ForzaHorizon1-ReImagined (forza-recomp/src/platform/
// achievement_toast.cpp), both by Dyotson, under their license:
//
//   MIT License
//
//   Copyright (c) 2026 Dyotson and RecompiledHaloReach contributors
//   Copyright (c) 2026 Dyotson and Forza Horizon 1: ReImagined contributors
//
//   Permission is hereby granted, free of charge, to any person obtaining a copy
//   of this software and associated documentation files (the "Software"), to deal
//   in the Software without restriction, including without limitation the rights
//   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//   copies of the Software, and to permit persons to whom the Software is
//   furnished to do so, subject to the following conditions:
//
//   The above copyright notice and this permission notice shall be included in all
//   copies or substantial portions of the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
//   SOFTWARE.

#pragma once

#include <rex/ui/imgui_drawer.h>
#include <rex/ui/overlay/achievement_notification.h>
#include <rex/ui/windowed_app_context.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>

namespace mcla {
namespace detail {

class OnDemandToast final : public rex::ui::AchievementNotificationDialog {
 public:
  using Clock = std::chrono::steady_clock;

  // The SDK's toast shows each achievement for 4.5 s, one after another
  // (AchievementToastDialog::kDisplaySeconds); the margin covers the last frames.
  static constexpr auto kToastTime = std::chrono::milliseconds(4500);
  static constexpr auto kMargin = std::chrono::milliseconds(1000);

  OnDemandToast(std::unique_ptr<rex::ui::AchievementNotificationDialog> toast,
                rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext& app_context)
      : AchievementNotificationDialog(drawer),
        drawer_(drawer),
        app_context_(app_context),
        toast_(std::move(toast)),
        self_(std::make_shared<std::atomic<OnDemandToast*>>(this)) {
    // Both constructors registered a dialog. The SDK's toast is drawn through
    // this one, and this one is registered while a toast shows.
    drawer_->RemoveDialog(toast_.get());
    drawer_->RemoveDialog(this);
  }

  ~OnDemandToast() override { self_->store(nullptr); }

#ifdef REX_UI_IMGUI_DIALOG_REPAINT_ON_DEMAND
  // Drawn with the game's frames; see the top of this file.
  bool RepaintsContinuously() const override { return false; }
#endif

  // Any thread.
  void Push(const rex::system::AchievementEvent& event) override {
    toast_->Push(event);
    {
      std::lock_guard lock(mutex_);
      shown_until_ = std::max(shown_until_, Clock::now()) + kToastTime;
    }
    app_context_.CallInUIThread([self = self_] {
      if (OnDemandToast* toast = self->load()) toast->drawer_->AddDialog(toast);
    });
  }

 protected:
  void OnDraw(ImGuiIO&) override {
    toast_->Draw();
    bool done;
    {
      std::lock_guard lock(mutex_);
      done = Clock::now() >= shown_until_ + kMargin;
    }
    // The drawer detaches from the presenter once its last dialog is gone.
    if (done) drawer_->RemoveDialog(this);
  }

 private:
  rex::ui::ImGuiDrawer* drawer_;
  rex::ui::WindowedAppContext& app_context_;
  std::unique_ptr<rex::ui::AchievementNotificationDialog> toast_;
  // Lets a queued UI-thread call see that this dialog is gone.
  std::shared_ptr<std::atomic<OnDemandToast*>> self_;
  std::mutex mutex_;
  Clock::time_point shown_until_{};
};

}  // namespace detail

// Wraps the SDK's toast dialog (null stays null).
inline std::unique_ptr<rex::ui::AchievementNotificationDialog> MakeOnDemandToast(
    std::unique_ptr<rex::ui::AchievementNotificationDialog> toast, rex::ui::ImGuiDrawer* drawer,
    rex::ui::WindowedAppContext& app_context) {
  if (!toast || !drawer) return toast;
  return std::make_unique<detail::OnDemandToast>(std::move(toast), drawer, app_context);
}

}  // namespace mcla
