#pragma once

#include <rex/ui/ui_system.h>

#include <filesystem>

namespace mcla::ui {

// The MCLA settings panel (F4), LARecomp's replacement for the SDK's settings overlay. The SDK window places every
// control at a fixed 240px from the left whatever the label or window width, so labels run into the controls and
// nothing follows a resize; its layout cannot be reached from outside, so the same settings registry gets a panel of its
// own here. rexglue 0.11 adds overlays through IUISystem::AddOverlays: this one is "mcla_settings" (ui.bind.mcla_settings,
// F4 by default; the 0.10 cvar was bind_mcla_settings), and LarecompApp::StartOverlays unbinds the SDK's own "settings"
// overlay, which has the F4 key by default. It asks for exclusive input: the title's pads read neutral while it is up.
rex::ui::OverlayConfig SettingsOverlayConfig();

// The file Ctrl+S merges the changed settings into (larecomp.toml). Before the panel first opens.
void SetSettingsConfigPath(const std::filesystem::path& config_path);

// Whether that panel is open (it drew in the last moments). Safe from any thread.
bool SettingsOpen();

} // namespace mcla::ui
