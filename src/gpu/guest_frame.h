#pragma once
// ===========================================================================
// MCLA GPU renderer -- the guest frame's once-only state of an owner that is started by every list (pure logic, no GPU)
// ===========================================================================
// Scene::BeginFrame runs once for every command list the renderer opens, and the renderer also opens one for a present that had no scene records (an idle present, a legal screen:
// Renderer::ExecutePresent), after Scene::OnPresent has run. The owners whose per-frame state is made at a trigger in the middle of the frame and used again at the present (the screen
// space reflections' once-a-frame trigger and its `ran` mark, the shared screen products' published frame, colour capture and history trade) must not forget that state when a SECOND list is
// opened before the present (LIGHT-STACK-REVIEW F2: the trigger recorded in the first list, `ran_` false at the present, so the reflections' history is dropped and the colour_post history
// trade skipped). The light manager does it with `begun_` (light_manager.cpp BeginLights); the begun flag alone would also take the empty list of an idle present for the frame, so this
// gate asks whether the frame has DONE its work (a trigger ran, a frame was published): a list that opened before any work starts the frame afresh, whatever lists came before it.
//
//   Begin()    at the owner's BeginFrame. True: start the frame (reset the per-frame state, as before). False: the frame already did its work in an earlier list; keep the state and count it.
//   Worked()   when the frame's work is done (the trigger's `trigger_done_ = true`, the products' `published_ = true`).
//   Present()  at the owner's OnPresent (the guest frame ends).
//
// Repeats() is the number of lists that opened after the work was done; the owners log it at 1, 10, 100, ... (LogDue) so that a run settles whether such a frame occurs in play at all.

#include <cstdint>

namespace mcla::gpu::guest_frame {

class Gate {
 public:
  bool Begin() {
    if (worked_) {
      ++repeats_;
      return false;
    }
    return true;
  }
  void Worked() { worked_ = true; }
  void Present() { worked_ = false; }
  bool worked() const { return worked_; }
  uint64_t repeats() const { return repeats_; }

 private:
  bool worked_ = false;
  uint64_t repeats_ = 0;
};

// Whether the `count`-th repeat is logged: the first, the tenth, the hundredth, ...
constexpr bool LogDue(uint64_t count) {
  if (count == 0) {
    return false;
  }
  while (count % 10 == 0) {
    count /= 10;
  }
  return count == 1;
}

}  // namespace mcla::gpu::guest_frame
