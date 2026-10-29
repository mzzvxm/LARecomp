#pragma once

#include <rex/ui/imgui_drawer.h>
#include <rex/ui/overlay/achievement_notification.h>

#include <memory>

namespace rex {
class Runtime;
namespace system {
class AchievementManager;
}  // namespace system
namespace ui {
class ImmediateDrawer;
}  // namespace ui
}  // namespace rex

namespace mcla::ui {

// The "ACHIEVEMENT UNLOCKED" toast as an MCLA panel. The SDK's toast is a
// fixed 320x70 window whose second line no longer fits once the panels' font
// and padding apply. Null when any argument is (as the SDK's factory).
std::unique_ptr<rex::ui::AchievementNotificationDialog> MakeAchievementToast(
    rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, rex::Runtime* runtime);

// Opens or closes the achievement list (F7). LARecomp owns this one rather
// than overriding CreateAchievementsOverlay: the SDK keeps that dialog in a
// unique_ptr only its own bind releases, so a list that closes itself (Esc,
// the footer prompt) could not be closed safely through it.
void ToggleAchievements(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate,
                        rex::Runtime* runtime, rex::system::AchievementManager* achievements);

} // namespace mcla::ui
