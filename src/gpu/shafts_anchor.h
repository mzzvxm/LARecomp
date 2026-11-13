#pragma once
// ===========================================================================
// MCLA GPU renderer -- where the sun shafts are added (the rules, shafts.h)
// ===========================================================================
// The sun shafts take the sky's brightness at the game's mid-scene depth copy (the first point) and add the
// blurred light to the main colour at one of two places:
//
//   kEndOfScene  the game's copy of the main colour that follows (the picture the post chain and the tone map
//                read, made after the fog quad and the particles): the light lies in front of the fog, as
//                in-scattering does. This is the look the design asks for.
//   kMidScene    the first point itself, before the fog: the fog covers the light at distance, but the
//                place exists in every frame the sky is taken in, because it is where the sky is taken.
//
// The end-of-scene copy is found by the identity of its source (the main colour, at the samples of the
// frame's depth) and its place in the frame. If a build or a scene gave a copy that does not look like that,
// the shafts would never be added, and the first version of this pass (which looked for a copy "inside
// EndTiling" that the game does not make) never added a single one. So the choice learns: after
// kMissesToFallBack frames in a row in which the sky was taken and no such copy came, the shafts go in at the
// first point; the copy is still watched there, and after kHitsToReturn frames in a row in which it came the
// end of the scene is used again. A debug switch (mcla_gpu_sun_shafts_early) forces the first point.
//
// Pure rules, no renderer: shafts_anchor_test.cpp builds on its own.
// ===========================================================================

#include <cstdint>

namespace mcla::gpu::shafts_anchor {

enum class Anchor : uint8_t { kEndOfScene, kMidScene };

constexpr const char* AnchorName(Anchor anchor) {
  return anchor == Anchor::kEndOfScene ? "the end-of-scene copy of the main colour"
                                       : "the mid-scene depth copy";
}

class Policy {
 public:
  // Frames with the sky taken and no end-of-scene copy, in a row, before the fall-back.
  static constexpr uint32_t kMissesToFallBack = 3;
  // Frames with the sky taken and the copy seen, in a row, before the end of the scene is used again.
  static constexpr uint32_t kHitsToReturn = 30;

  // Where this frame's shafts are added. `force_mid_scene`: the debug switch.
  Anchor Choose(bool force_mid_scene) const {
    return (force_mid_scene || fallen_back_) ? Anchor::kMidScene : Anchor::kEndOfScene;
  }

  // A frame in which the sky was taken is over: `used` is what Choose said for it and `copy_seen` whether
  // the end-of-scene copy of the main colour came before the anchor draw (watched whichever place was
  // used). Frames in which the sun gave no shafts say nothing and are not passed in.
  void EndOfFrame(Anchor used, bool copy_seen) {
    if (used == Anchor::kEndOfScene) {
      if (copy_seen) {
        misses_ = 0;
      } else if (++misses_ >= kMissesToFallBack) {
        fallen_back_ = true;
        misses_ = 0;
        hits_ = 0;
        ++fallbacks_;
      }
      return;
    }
    if (!fallen_back_) {
      return;  // the debug switch's frames: nothing to learn
    }
    if (!copy_seen) {
      hits_ = 0;
    } else if (++hits_ >= kHitsToReturn) {
      fallen_back_ = false;
      hits_ = 0;
      misses_ = 0;
      ++returns_;
    }
  }

  bool fallen_back() const { return fallen_back_; }
  uint32_t fallbacks() const { return fallbacks_; }
  uint32_t returns() const { return returns_; }

 private:
  bool fallen_back_ = false;
  uint32_t misses_ = 0;
  uint32_t hits_ = 0;
  uint32_t fallbacks_ = 0;
  uint32_t returns_ = 0;
};

// What one logging interval saw (the log line of shafts.cpp): sky == end_of_scene + mid_scene + lost
// == sun_by_sky + sun_by_light.
struct Tally {
  uint32_t frames = 0;           // guest frames of the interval (the shafts on)
  uint32_t sky = 0;              // frames in which the sun gave shafts: the sky image was taken
  uint32_t end_of_scene = 0;     // ... and the shafts were added at the end-of-scene copy
  uint32_t mid_scene = 0;        // ... and at the mid-scene depth copy
  uint32_t lost = 0;             // ... and were not added at all
  uint32_t copies_missing = 0;   // of the sky frames, those in which the end-of-scene copy never came
  uint32_t sun_by_sky = 0;       // of the sky frames, those whose sun's place came from the sky's sun constant
  uint32_t sun_by_light = 0;     // ... and from the light's direction (shafts_sun.h)

  void Frame() { ++frames; }
  void SunPlacedBy(bool sky_constant) { ++(sky_constant ? sun_by_sky : sun_by_light); }
  void Sky(Anchor used, bool composited, bool copy_seen) {
    ++sky;
    if (!copy_seen) {
      ++copies_missing;
    }
    if (!composited) {
      ++lost;
    } else if (used == Anchor::kEndOfScene) {
      ++end_of_scene;
    } else {
      ++mid_scene;
    }
  }
  // Worth a line: the sun gave shafts at least once.
  bool Worth() const { return sky != 0; }
  void Clear() { *this = Tally{}; }
};

}  // namespace mcla::gpu::shafts_anchor
