#pragma once
//
// DualSense adaptive triggers: R2 is a light spring that firms up towards the end of its
// travel, L2 is firmer, and both go slack whenever the car is not being driven. See
// adaptive_triggers.cpp.
//

namespace larecomp::triggers {

// Every frame, on the game's thread (Patch_DeltaTimePre). Does its work at most every 50 ms
// and nothing while adaptive_triggers is off and no pad is held.
void Tick();

// Lets both triggers go and closes the pads. The process ends without further notice (the
// window's close, QUIT), and an effect stays in the pad until it is told otherwise. Safe to
// call more than once and from any thread.
void Shutdown();

}  // namespace larecomp::triggers
