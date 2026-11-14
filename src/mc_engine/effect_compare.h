#pragma once
// ===========================================================================
// The compare key: flick one graphics effect off and on to see what it does
// ===========================================================================
// One key (F1 until it is rebound in F4's key bindings, "bind_mcla_compare") flips the effect that the setting
// COMPARE KEY (GRAPHICS QUALITY, `mcla_compare_effect`) names: the bounce light, the contact shadows, the sun
// shafts, the ambient occlusion, the shadow filter, or (from F4 or the command line only) the bloom size fix.
// A small label at the top of the screen says what the picture shows now ("BOUNCE LIGHT: OFF") and fades out
// after about a second and a half. The key works anywhere the game takes keys; what it shows is what the view
// draws: the pause menu's, the map's and the garage's world (the 4x view) does not draw the bounce light, so
// flipping it there changes nothing until that view gets it.
//
// The flip is not a setting: it writes no cvar and is never saved (the saved setting, the pause row and the
// launcher keep what the player chose), it is dropped when the setting it flips is changed or the key is set
// to another effect, and it is gone at the next start. The readers of the settings take it into account
// (gpu/effect_compare.h); what a press does, and says, is gpu/effect_compare_rules.h (checked without a game
// by gpu/effect_compare_test.cpp).
// ===========================================================================

#include <string>

#include <rex/ui/imgui_drawer.h>

namespace mcla::compare {

// Registers the key, the label and the changes that drop a flip. Once, for the native renderer, from
// SetupPresentation (the ImGui drawer exists then).
void Start(rex::ui::ImGuiDrawer* drawer);

// The key's own name for the pause row ("F1" unless it was rebound; capitals, digits, space and '+').
std::string KeyLabel();

}  // namespace mcla::compare
