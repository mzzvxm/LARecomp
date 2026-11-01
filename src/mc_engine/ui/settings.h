#pragma once

#include <rex/ui/imgui_drawer.h>

#include <filesystem>

namespace mcla::ui {

// Opens or closes the MCLA settings panel, LARecomp's replacement for the
// SDK's settings overlay. The SDK window places every control at a fixed 240px
// from the left whatever the label or window width, so labels run into the
// controls and nothing follows a resize; its layout cannot be reached from
// outside, so the same cvar registry gets a panel of its own here.
void ToggleSettings(rex::ui::ImGuiDrawer* drawer, const std::filesystem::path& config_path);

// Whether that panel is open. Safe from any thread.
bool SettingsOpen();

} // namespace mcla::ui
