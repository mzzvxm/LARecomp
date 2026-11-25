#pragma once
//
// Repeatable scenes: the game side of scene_seed.h. A scene that runs holds the game's random generators at the start of every update (the scene seed's applier,
// scene_seed.cpp) and tells the renderer which scene update each frame shows (gpu/scene_noise.h). Nothing runs unless a scene is armed.
//
// Two debug settings, both read at every update of a scene (so the scene's own `set` line can change them):
//   mcla_scene_seed             the scene's seed, 1 by default; 0 = the generators and the renderer's noise are left alone
//   mcla_scene_seed_each_frame  true by default: the generators get a state from (seed, update number) at every update; false: once, at the scene's start
//

#include <cstdint>

namespace mc::cinematic {

// Holds the game's generators (the simulation's, the cosmetic one and the car configuration's) at the state `seed` makes, for the life of the object, and then puts the
// words back that were there. For the racer-class natives that may draw a car's look (OpponentManager_CreateOpponent / Load / Add): the call runs on the game thread, so the
// look it draws depends on `seed` and on nothing else. Inert (held() false) when the runtime is not up or `seed` is 0.
class GeneratorHold {
  public:
    explicit GeneratorHold(uint32_t seed);
    ~GeneratorHold();
    GeneratorHold(const GeneratorHold&) = delete;
    GeneratorHold& operator=(const GeneratorHold&) = delete;
    bool held() const { return held_; }

  private:
    bool held_ = false;
    uint32_t saved_[6] = {};     // sim lo, hi; visual lo, hi; config lo, hi (big endian words as the game stores them, already swapped to host order)
    uint32_t config_at_ = 0;     // the config generator's address, 0 = not found
};

}  // namespace mc::cinematic
