#pragma once
//
// The HUD track of a scene (trailer toolkit, work package M1c): the keys a scene file writes for the game's own UI (HUD, map,
// phone, prompts, gamertags, world icons), as pure code with no game in it. The runner reads the words after `hud` of each
// key (the scene parser keeps them as words), turns them into HudKeys here and asks the track for the alpha at the scene's
// time on every update; hud.h puts that alpha on the screen.
//
//   at T hud off                      the UI is hidden (alpha 0) from T on
//   at T hud on                       the game's own UI again (alpha 1)
//   at T hud <alpha>                  a share of it, 0..1
//   at T hud fade <a>..<b> to <x>     from scene time a to scene time b the alpha moves in a straight line from the value it
//                                     has at a to <x> (off, on or 0..1) and holds there; T only orders it among the keys
//   at T hud fade over <s> to <x>     the same, starting at T and taking s seconds
//
// A key's value holds until the next key. Before the first key the track asks for nothing (alpha 1, the game's own picture).
// A key that starts while a fade is still moving takes over from where the fade has got to. The fade is linear in the alpha;
// an ease is made with several keys.
//

#include <string>
#include <vector>

namespace mc::cinematic {

// One key of the HUD track: from `t` to `end` the alpha moves to `alpha` and holds there (a jump has end == t).
struct HudKey {
    double t = 0.0;
    double end = 0.0;
    float alpha = 1.0f;
};

// The key the words after `hud` mean, for a line at scene time `at`. False, with a plain message in *error (when it is not
// null), when they are not a HUD key: no words, an unknown word, an alpha outside 0..1, a fade that ends before it starts.
bool ParseHudKey(const std::vector<std::string>& words, double at, HudKey* key, std::string* error);

// The keys of a scene, in time order.
class HudTrack {
public:
    // Adds a key; keys that start at the same time keep the order they were added in.
    void Add(const HudKey& key);
    void Clear() { keys_.clear(); }
    bool empty() const { return keys_.empty(); }
    const std::vector<HudKey>& keys() const { return keys_; }

    // The alpha at scene time `t`: 1 before the first key.
    float AlphaAt(double t) const;
    // When the last key has arrived (0 for an empty track): the scene's end is not before it.
    double End() const;

private:
    std::vector<HudKey> keys_;
};

}  // namespace mc::cinematic
