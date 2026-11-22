#pragma once
// ===========================================================================
// MCLA GPU renderer -- the lights' string settings, as the render thread reads them
// ===========================================================================
// mcla_gpu_headlight_pass (a row of the debug menu: HEADLIGHTS VIA), mcla_gpu_light_pick (the A/B of the one picker) and mcla_gpu_light_pick_record (a file path, live) are string cvars that another
// thread writes (the menu's CvarSet, the console) while the render thread reads them every frame. REXCVAR_GET of a string is the plain std::string the writer assigns, with no lock: a read
// during the write is a data race (live_text.h says why). The render thread reads them here, through LiveText, the way the other string settings are read (LIGHT-STACK-REVIEW F3).
// One LiveText for each setting, shared by every reader (a function-local static of an inline function: one object in the whole program).

#include "live_text.h"

namespace mcla::gpu::light_text {

// mcla_gpu_headlight_pass: forward | pass | both (headlight_pass_rules.h ParseRoute). Read once a frame at the Scene's frame start (headlight_rig::SampleRoute).
inline LiveText& HeadlightPass() {
  static LiveText text("mcla_gpu_headlight_pass");
  return text;
}

// mcla_gpu_light_pick: unified | legacy (light_pick.h ParseMode).
inline LiveText& LightPick() {
  static LiveText text("mcla_gpu_light_pick");
  return text;
}

// mcla_gpu_light_pick_record: a path (or empty): where the pickers record their inputs. The first reader makes the callback; a path of any length works.
inline LiveText& LightPickRecord() {
  static LiveText text("mcla_gpu_light_pick_record");
  return text;
}

}  // namespace mcla::gpu::light_text
