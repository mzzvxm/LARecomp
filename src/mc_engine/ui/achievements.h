#pragma once

#include <rex/ui/ui_system.h>
#include <rex/ui/overlay/achievement_notification.h>
#include <rex/kernel/title_runtime.h>

namespace rex::xam {
class IAchievementManager;
}  // namespace rex::xam

namespace mcla::ui {

// The achievement list (F7) as an MCLA panel: the overlay "mcla_achievements", key ui.bind.mcla_achievements (F7).
// The SDK has its own overlay on that key ("achievements"); LarecompApp::StartOverlays unbinds it.
//
// The "ACHIEVEMENT UNLOCKED" toast is the SDK's now (0.10 let an app replace it with its own MCLA panel; 0.11 adds
// the toast itself when the title starts, and sizes and colours it from rex::ui::Style: see ApplyMclaTheme).
//
// The list reads the achievements of the running title: set the manager from LarecompApp::OnTitleLaunch (null
// before the title and after it). CHECK / SDK needs: the SDK's icons come from IAchievementRegistry
// (GetAchievementIcon), which an app cannot reach (it is the kernel's TitleContext, private); until the SDK
// gives the manager or the runtime a public icon call, this list has no icons.
void SetAchievementManager(rex::xam::IAchievementManager* manager);

rex::ui::OverlayConfig AchievementsOverlayConfig();

} // namespace mcla::ui
