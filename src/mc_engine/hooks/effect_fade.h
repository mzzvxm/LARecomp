#pragma once
//
// The fade of a released particle effect, held to the console's decay per second at every frame rate.
//
// What the game does (hooks/checkpoint_effect_fade.cpp has the code that was read): when a race checkpoint is
// closed (CheckpointManager_DeactivateSingleCheckpoint, then its update in sub_822664F8, or the checkpoint's own
// deactivate in sub_82264E80) the game lets go of the checkpoint's smoke effect (sub_8247A8D0) after it has
// written two numbers into the effect instance:
//     +464  = dt * 1.0     the time the effect's own clock gets added at every update
//     +468  = dt * 25.0    the multiplier of the effect's strength, applied at every update (sub_8247A1B0:
//                          strength = strength * +468; the strength scales the colour of every live particle)
// dt is the engine's step of that update, 1/30 s on the console. So the console faded the smoke by 25/30 =
// 0.8333 every update, 30 updates a second: to 0.4% in one second, a trail that dissipates over about half a
// second. The multiplier is a number per UPDATE that was worked out from the step, but a step makes it
// smaller, not the decay slower: at 60 FPS it is 0.4167 per update (60 updates a second), at 144 FPS 0.174. The
// smoke has lost 99% of its strength in 90 ms at 60 FPS and in 18 ms at 144 FPS, which is "all the smoke
// disappears instantly along with the checkpoint" (fork issue 97).
//
// The rule: the multiplier of an update of `step` seconds is the console's multiplier (k / 30, the game's own
// constant over the console's step) raised to step * 30. The product over any run of updates is then the
// console's multiplier to the power of the elapsed time in console steps, whatever the update rate, and at
// exactly 1/30 s it is k / 30, the number the game wrote. Standard library only; effect_fade_test.cpp checks it.

#include <cmath>

namespace mcla::fade {

// The engine's step on the console, in seconds: the step the game's tuning numbers were made for.
constexpr float kConsoleStepS = 1.0f / 30.0f;

// What the game writes into +468 of a released effect: `k` (25.0, a constant of the game's image) times `step_s`.
inline float GameMultiplier(float k, float step_s) { return k * step_s; }

// The multiplier that fades at the console's rate: (k / 30) ^ (step_s * 30). Falls back to the game's own value
// where it is not a step or not a fade: a step of zero or less (the game is paused: its own value is 0, which
// kills the effect, as before), a step that is not a number, a `k` of zero or less or not a number.
inline float PerUpdateMultiplier(float k, float step_s) {
    if (!(step_s > 0.0f) || !std::isfinite(step_s)) return GameMultiplier(k, step_s);
    if (!(k > 0.0f) || !std::isfinite(k)) return GameMultiplier(k, step_s);
    const double console = static_cast<double>(k) * static_cast<double>(kConsoleStepS);
    const double steps = static_cast<double>(step_s) / static_cast<double>(kConsoleStepS);
    const double m = std::pow(console, steps);
    if (!std::isfinite(m)) return GameMultiplier(k, step_s);
    return static_cast<float>(m);
}

}  // namespace mcla::fade
