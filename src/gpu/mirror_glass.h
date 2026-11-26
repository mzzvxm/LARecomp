#pragma once
// ===========================================================================
// MCLA GPU renderer -- the side mirrors' glass found without ray tracing (MIRROR-VIEW MV3)
// ===========================================================================
// perf/MIRROR-VIEW.md 2.6 and 4. The shared rear view's side mirror overlay (MV4) and the "is a mirror visible" need rule need to know which two draws of the player's car are the mirror glass. On a
// card uses the same finder: a listener on CarFrame (car_frame.h), which every
// GPU has (it feeds the car shadow map), reading the car's draws the way the ray traced scene does but keeping nothing of the ray traced scene.
//
//   LISTEN   OnPlayerDraw, inside CarFrame's call for a draw of the player's car: a small rigid draw of a class that may be a glass (mirror_glass_rules.h WhySkipped) whose mesh_key the cache does not
//            know is read once with the MeshReader, cleaned and measured (MeasurePatch), and cached by mesh_key; at most kReadsPerFrame a frame. A mesh already known costs one hash probe.
//   CHOOSE   Current(): from the frame's complete draw list, the draws whose cached mesh is a small flat patch, placed in the frame's view space, car_mirror::ChoosePair for the pair across the car,
//            then what the readers need (mirror_glass_rules.h Result: each glass's draw, pose in the view and in the world, box, rectangle on a reference picture, whether it is worth a view).
//            Chosen again in every frame from the cached numbers: there is no pair to lose when a draw is missing for a frame.
//   PUBLISH  to the render thread's readers through Current() / DrawOf(), and to the guest through mirror_glass_need.h (OnPresent: one relaxed atomic with a hold of kNeedHoldFrames, so the guest's
//            phase request does not flicker): MirrorView::BeginFrame (mirror_view.cpp) copies mirror_glass::SideNeed() into the
//            request it publishes to the guest once a frame (mirror_view_link.h Request::side_need), which hooks/rear_view.cpp reads.
//
// THE INTERFACE FOR MV4 (the overlay) AND THE SCENE'S READERS: see perf/MIRROR-VIEW.md section 9.
//   const mirror_glass::Result& Current()  the frame's answer; evaluated once per frame (again only if more draws arrived since), so a reader may call it at any step after the car's draws are in
//                                          (the scene's last colour step is the one MV4 uses) and OnPresent's own call shares the result.
//   const car_frame::Draw* DrawOf(k)       glass k's draw in CarFrame::Current().draws (its host geometry, position element and matrix: valid until the frame's lists are submitted); null when
//                                          there is no pair.
//   Result::glass[k] k = 0 the glass on the left of the picture, 1 on the right.
//
// WHEN IT RUNS (mirror_setting.h, MV6): while the SIDE MIRRORS row's view runs (never where RT SIDE MIRRORS replaces it, never on the Low class under AUTO), or while the debug setting mcla_gpu_mirror_glass
// (live, default off) says so, on any card. While it is not running nothing is asked of CarFrame, nothing is read and nothing is published (the guest reads "no need"). It is not an effect switch for
// players: the row is.
//
// LOG ("[mirror glass]"): the first pair and every change of it (keys, areas, normals, separation, height, the widths on the picture: the CHECK of car_mirror_patch_rules.h for other cars), and a window
// every kLogEveryFrames frames (frames with a pair, with a need, why not, reads, skips, cache size).
//
// Nothing here ran in the game (no game, no build). What only a run can say:
// CHECK: that the MeshReader's triangles of the R34's two glass draws measure as MIRROR-RT's dump says (17 triangles, 0.017 m2, normals within 6 degrees: the first "[mirror glass]" pair line prints
// them) and that the pair is found a frame or two after the car appears (a frame reads at most kReadsPerFrame meshes).
// CHECK: the cost of CarFrame collecting when nothing else asks for it (the debug switch is the only asker on a card without the car shadow map): the draw census, recipes, matrices, mesh keys and the
// boxes of new meshes for the car's ~210 draws a frame (the "[car frame]" counts); MV0's chain with the switch on and off says.
// CHECK: that Draw::geometry.count is the host index count of a strip-converted draw (the 480 index gate) and that Draw::ps is the identity the class table knows on a card with no ray traced scene.
// CHECK: that another car's glass is such a patch (car_mirror_patch_rules.h's own CHECK: three other cars).
// CHECK: the pause view's and the garage's frames list the car once per tile (car_frame.cpp's CHECK): the duplicates are dropped, the finder does not tell the views apart, the readers must (play view only).
//
// Threading: render thread only (the listener runs inside a draw's call on it); the published flag is one atomic the guest thread reads.
// ===========================================================================

#include <cstdint>

#include "car_frame.h"
#include "mirror_glass_need.h"
#include "mirror_glass_rules.h"

namespace mcla::gpu {

class MirrorGlass final : public car_frame::Listener {
 public:
  MirrorGlass() = default;
  ~MirrorGlass() override = default;
  MirrorGlass(const MirrorGlass&) = delete;
  MirrorGlass& operator=(const MirrorGlass&) = delete;

  // After CarFrame::Initialize (which clears the listeners). Never fails the renderer.
  void Initialize(CarFrame& car_frame);
  // Before CarFrame::Shutdown. The need is withdrawn (the guest reads "no need").
  void Shutdown();

  // The frame's start (Scene::BeginFrame), BEFORE CarFrame::BeginFrame: reads the setting; asks CarFrame to collect the next frame's draws while it is on.
  void BeginFrame();
  bool active() const { return active_; }

  // CarFrame's call for a draw of the player's car (the guest is held back: the read is only valid in here).
  void OnPlayerDraw(const car_frame::Draw& draw, const car_frame::MeshReader& reader) override;

  // The frame's answer (see above). With the finder off it is the empty result (not found).
  const mirror_glass::Result& Current();
  // Glass k's draw in this frame's list; null when there is no pair this frame.
  const car_frame::Draw* DrawOf(uint32_t k);

  // A guest present, BEFORE CarFrame::OnPresent clears the frame: evaluates, publishes the need, counts, logs, sweeps the cache.
  void OnPresent();

  size_t CachedMeshes() const { return finder_.cache().size(); }

 private:
  void Evaluate(const car_frame::Frame& frame);
  void LogPair(const mirror_glass::Result& r);
  void LogWindow();

  // One window of the log.
  struct Window {
    uint64_t first_frame = 0;
    uint32_t frames = 0;
    uint32_t found = 0;
    uint32_t needed = 0;
    uint64_t why[size_t(mirror_glass::Why::kCount)] = {};
    uint64_t skipped[size_t(mirror_glass::Skip::kCount)] = {};
    uint64_t draws = 0;
    uint64_t looked_at = 0;
    uint64_t measured = 0;
    uint64_t unreadable = 0;
    uint64_t deferred = 0;
    uint64_t waiting = 0;
    uint32_t patches_seen = 0;  // meshes measured in the window that are a patch
    size_t swept = 0;
  };

  CarFrame* car_frame_ = nullptr;
  bool ready_ = false;
  bool active_ = false;
  bool published_ = false;  // something other than "nothing" has been written to the atomic
  mirror_glass::Finder finder_;
  mirror_glass::NeedHold hold_;
  mirror_glass::Result result_;
  bool evaluated_ = false;
  uint64_t evaluated_frame_ = 0;
  size_t evaluated_draws_ = 0;
  bool evaluated_valid_ = false;
  uint64_t frame_ = 0;  // guest frames seen (the finder's clock)
  Window window_;
  uint64_t pair_keys_[2] = {};  // the pair last logged
  bool pair_logged_ = false;
  bool first_need_logged_ = false;
  uint64_t cabin_key_ = 0;
  bool cabin_scanned_ = false;
};

}  // namespace mcla::gpu
