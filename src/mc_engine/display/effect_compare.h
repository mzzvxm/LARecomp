#pragma once
// ===========================================================================
// The compare keys: flick one graphics effect off and on to see what it does (a developer tool)
// ===========================================================================
// Two keys, for the people who work on the game. They are in a build only with LARECOMP_DEV_TOOLS (the CMake option
// -DLARECOMP_DEV_TOOLS=ON, off by default): a release build has no key, no setting and no code of them, and the pause
// menu has no row for them.
//
//   F1   ("bind_mcla_compare")       flips the SELECTED effect: off if your setting has it on, on if it has it off. The next press puts your
//                                    setting back. A small label at the top of the screen says what the picture shows now ("BOUNCE LIGHT:
//                                    OFF") and fades out after about a second and a half.
//   F12  ("bind_mcla_compare_next")  selects the next effect: the bounce light, the contact shadows, the sun shafts, the ambient
//                                    occlusion, the shadow filter, the car self-reflections, and round to the bounce light again. The label says
//                                    which one is selected and whether your setting has it on ("COMPARE: SUN SHAFTS", "ON"). A flip that was on is
//                                    dropped by it. The selection is the setting `mcla_compare_effect` (F4 and the command line see it too; they
//                                    can also select the bloom size fix, which the keys do not walk to).
//
// Both keys can be rebound in F4's key bindings. They work anywhere the game takes keys; what they show is what the view draws: the pause
// menu's, the map's and the garage's world (the 4x view) does not draw the bounce light, so flipping it there changes nothing until that view
// gets it.
//
// The flip is not a setting: it writes no cvar and is never saved (the saved setting, the pause row and the launcher keep what the player
// chose), it is dropped when the setting it flips is changed or another effect is selected, and it is gone at the next start. The readers of
// the settings take it into account (gpu/effect_compare.h); what a press does, and says, is gpu/effect_compare_rules.h (checked without a game
// by gpu/effect_compare_test.cpp).
// ===========================================================================

#if defined(LARECOMP_DEV_TOOLS)

#include <rex/ui/imgui_drawer.h>

namespace mcla::compare {

// Registers the keys, the label and the changes that drop a flip. Once, for the native renderer, from
// SetupPresentation (the ImGui drawer exists then).
void Start(rex::ui::ImGuiDrawer* drawer);

}  // namespace mcla::compare

#endif  // LARECOMP_DEV_TOOLS
