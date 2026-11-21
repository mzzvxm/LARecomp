// Check of the closed checkpoint smoke's fade (effect_fade.h). A program of its own: it needs no ReXGlue, no guest
// and no game, and exits 0 when every check holds.
//
//   clang++ -std=c++20 -O1 -Wall -Wextra -I src/mc_engine/hooks src/mc_engine/hooks/effect_fade_test.cpp -o effect_fade_test
//   ./effect_fade_test
//
// What it pins down: the rule at the console's step (the number the game wrote, 25 / 30), its fallbacks (a step of
// zero or less, a number that is not one), and the point of it all: a smoke strength that the instance's update
// multiplies by the number once an update falls to the same fraction after the same time at 20 to 1000 FPS and at
// an irregular uncapped rate, where the game's own number (25 x the step) made it 0.4167 per update at 60 FPS and
// the smoke was gone in 90 ms (fork issue 97). The table printed is where the numbers in
// checkpoint_effect_fade.cpp's comment come from.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "effect_fade.h"

using namespace mcla::fade;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const char* what, double a = 0.0, double b = 0.0) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAIL: %s (%.9g, %.9g)\n", what, a, b);
    }
}

constexpr float kK = 25.0f;  // the game's constant at 0x827DCEFC

enum class Rule { kGame, kFixed };

// The multiplier the hook leaves in +468 at the release, for the step of the update that releases the effect.
float Released(Rule rule, float step) {
    return rule == Rule::kGame ? GameMultiplier(kK, step) : PerUpdateMultiplier(kK, step);
}

// The strength of the effect `seconds` after its release, updated at `fps` (steady) or at an irregular rate
// (`jitter`: 4 to 12 ms, the same series every time). The multiplier is made once, at the release, from the step of
// that update (`release_step`), as the game's code and the hook do; the effect multiplies its strength by it at
// every update.
double StrengthAfter(Rule rule, double fps, bool jitter, double release_step, double seconds) {
    const float m = Released(rule, static_cast<float>(release_step));
    double strength = 1.0;
    double t = 0.0;
    uint32_t lcg = 777u;
    while (true) {
        double dt = 1.0 / fps;
        if (jitter) {
            lcg = lcg * 1664525u + 1013904223u;
            dt = 0.004 + 0.008 * (static_cast<double>(lcg >> 8) / 16777216.0);
        }
        if (t + dt > seconds + 1e-9) break;
        t += dt;
        strength *= static_cast<double>(m);
    }
    return strength;
}

// The seconds until the strength is below `level` (the update that crosses it), for a steady rate.
double SecondsBelow(Rule rule, double fps, double level) {
    const float m = Released(rule, static_cast<float>(1.0 / fps));
    double strength = 1.0;
    int n = 0;
    while (strength >= level && n < 100000) {
        strength *= static_cast<double>(m);
        ++n;
    }
    return n / fps;
}

}  // namespace

int main() {
    // the rule at the console's step is the number the game wrote
    {
        const float game = GameMultiplier(kK, kConsoleStepS);
        const float ours = PerUpdateMultiplier(kK, kConsoleStepS);
        std::printf("info: at 1/30 s the game writes %.6f, the rule gives %.6f\n", static_cast<double>(game), static_cast<double>(ours));
        Check(std::fabs(game - 25.0f / 30.0f) < 1e-6f, "the game's number at 1/30 s is 25/30", game, 25.0 / 30.0);
        Check(std::fabs(ours - game) < 1e-6f, "at the console's step the rule is the game's own number", ours, game);
    }

    // fallbacks: the game's own number where this is not a step or not a fade
    {
        Check(PerUpdateMultiplier(kK, 0.0f) == 0.0f, "a step of zero keeps the game's 0 (a paused game kills the effect, as before)");
        Check(PerUpdateMultiplier(kK, -0.01f) == GameMultiplier(kK, -0.01f), "a negative step keeps the game's number");
        Check(PerUpdateMultiplier(kK, NAN) != PerUpdateMultiplier(kK, NAN), "a step that is not a number gives not a number, as the game's does");
        Check(PerUpdateMultiplier(0.0f, 0.02f) == 0.0f, "a constant of 0 keeps the game's 0");
        Check(PerUpdateMultiplier(-5.0f, 0.02f) == GameMultiplier(-5.0f, 0.02f), "a negative constant keeps the game's number");
        Check(PerUpdateMultiplier(INFINITY, 0.02f) == GameMultiplier(INFINITY, 0.02f), "an infinite constant keeps the game's number");
        Check(PerUpdateMultiplier(kK, INFINITY) == GameMultiplier(kK, INFINITY), "an infinite step keeps the game's number");
        // a longer step (a hitch: the engine's cap is 125 ms) fades more in that one update, never to a negative or a number above 1
        const float hitch = PerUpdateMultiplier(kK, 0.125f);
        Check(hitch > 0.0f && hitch < 1.0f, "a 125 ms step still gives a multiplier between 0 and 1", hitch, 0.5);
        Check(PerUpdateMultiplier(kK, 0.0166667f) > PerUpdateMultiplier(kK, 0.0333333f), "the longer the step, the smaller the multiplier of that update");
        Check(PerUpdateMultiplier(kK, 0.0083333f) > PerUpdateMultiplier(kK, 0.0166667f), "and the shorter, the closer to 1");
    }

    // the point: the same fade after the same time at every frame rate
    {
        const double fps_list[] = {20, 24, 30, 45, 60, 90, 120, 144, 165, 240, 360, 1000};
        const double times[] = {0.1, 0.25, 0.5, 1.0};
        const double ref[] = {std::pow(25.0 / 30.0, 30 * times[0]), std::pow(25.0 / 30.0, 30 * times[1]),
                              std::pow(25.0 / 30.0, 30 * times[2]), std::pow(25.0 / 30.0, 30 * times[3])};
        std::printf("info: strength of the smoke after 0.1 / 0.25 / 0.5 / 1 s; console %.4f %.4f %.4f %.4f\n", ref[0], ref[1], ref[2], ref[3]);
        for (double fps : fps_list) {
            std::printf("info: %5.0f FPS  game's number %.4f %.2e %.2e %.2e   held %.4f %.4f %.4f %.4f   per update: game %.4f held %.4f\n", fps,
                        StrengthAfter(Rule::kGame, fps, false, 1.0 / fps, times[0]), StrengthAfter(Rule::kGame, fps, false, 1.0 / fps, times[1]),
                        StrengthAfter(Rule::kGame, fps, false, 1.0 / fps, times[2]), StrengthAfter(Rule::kGame, fps, false, 1.0 / fps, times[3]),
                        StrengthAfter(Rule::kFixed, fps, false, 1.0 / fps, times[0]), StrengthAfter(Rule::kFixed, fps, false, 1.0 / fps, times[1]),
                        StrengthAfter(Rule::kFixed, fps, false, 1.0 / fps, times[2]), StrengthAfter(Rule::kFixed, fps, false, 1.0 / fps, times[3]),
                        static_cast<double>(Released(Rule::kGame, static_cast<float>(1.0 / fps))),
                        static_cast<double>(Released(Rule::kFixed, static_cast<float>(1.0 / fps))));
            for (int i = 0; i < 4; ++i) {
                // `seconds` is not a whole number of updates at every rate (24, 45, 165 FPS): one update's worth of slack on the console's curve
                const double s = StrengthAfter(Rule::kFixed, fps, false, 1.0 / fps, times[i]);
                const double lo = std::pow(25.0 / 30.0, 30 * (times[i] + 1.0 / fps));
                const double hi = std::pow(25.0 / 30.0, 30 * (times[i] - 1.0 / fps));
                Check(s >= lo * 0.995 && s <= hi * 1.005, "the held fade is the console's curve at every frame rate", s, ref[i]);
            }
        }
        // the bug: the game's number at 60 FPS and above has no smoke left after the first tenth of a second
        for (double fps : {60.0, 120.0, 144.0, 240.0}) {
            const double s = StrengthAfter(Rule::kGame, fps, false, 1.0 / fps, 0.1);
            Check(s < 0.01, "the game's number leaves under 1% of the smoke after 0.1 s at 60 FPS and above", s, 0.01);
        }
        Check(StrengthAfter(Rule::kGame, 30, false, 1.0 / 30, 0.1) > 0.5, "the game's number at 30 FPS leaves more than half after 0.1 s", 0, 0);
    }

    // how long until 1% is left
    {
        // the console's curve, continuous (its own 30 updates a second cross 1% on the 26th update, 0.867 s)
        const double console = std::log(0.01) / (30.0 * std::log(25.0 / 30.0));
        std::printf("info: seconds until the smoke is under 1%%: console %.3f s (its 30 updates a second: %.3f s)\n", console,
                    SecondsBelow(Rule::kGame, 30.0, 0.01));
        for (double fps : {60.0, 90.0, 120.0, 144.0, 240.0}) {
            const double game = SecondsBelow(Rule::kGame, fps, 0.01);
            const double held = SecondsBelow(Rule::kFixed, fps, 0.01);
            std::printf("info:   %5.0f FPS  game's number %.3f s   held %.3f s\n", fps, game, held);
            Check(std::fabs(held - console) <= 1.5 / fps, "held: under 1% at the console's time, to one update", held, console);
            Check(game <= console * 0.12, "game's number: under 1% in about a tenth of the console's time at 60 FPS and above", game, console);
        }
        Check(console > 0.83 && console < 0.85, "the console's smoke has dissipated in about 0.84 s", console, 0.84);
        Check(SecondsBelow(Rule::kGame, 60.0, 0.01) <= 0.1 + 1e-9, "the game's number at 60 FPS: under 1% within 0.1 s", SecondsBelow(Rule::kGame, 60.0, 0.01), 0.1);
        Check(SecondsBelow(Rule::kGame, 144.0, 0.01) < 0.03, "and at 144 FPS in less than 30 ms", SecondsBelow(Rule::kGame, 144.0, 0.01), 0.03);
    }

    // an irregular uncapped rate (4 to 12 ms an update, mean 8 ms): the multiplier is made from the step of the update that releases the
    // effect, so a release on a short or a long update is off in proportion; a release at any step within 30% of the mean stays close
    {
        const double ref = std::pow(25.0 / 30.0, 30 * 0.5);
        std::printf("info: irregular 4-12 ms, after 0.5 s: console %.4f; held, released on a 6 / 8 / 10 ms update: %.4f %.4f %.4f; game's number %.2e\n", ref,
                    StrengthAfter(Rule::kFixed, 0, true, 0.006, 0.5), StrengthAfter(Rule::kFixed, 0, true, 0.008, 0.5),
                    StrengthAfter(Rule::kFixed, 0, true, 0.010, 0.5), StrengthAfter(Rule::kGame, 0, true, 0.008, 0.5));
        for (double step : {0.006, 0.008, 0.010}) {
            const double s = StrengthAfter(Rule::kFixed, 0, true, step, 0.5);
            Check(s > ref * 0.3 && s < ref * 3.0, "irregular rate: after 0.5 s the held smoke is within a factor 3 of the console's", s, ref);
        }
        Check(StrengthAfter(Rule::kFixed, 0, true, 0.008, 0.5) > ref * 0.5 && StrengthAfter(Rule::kFixed, 0, true, 0.008, 0.5) < ref * 2.0,
              "released on an update of the mean step the held smoke is within a factor 2", StrengthAfter(Rule::kFixed, 0, true, 0.008, 0.5), ref);
        Check(StrengthAfter(Rule::kGame, 0, true, 0.008, 0.5) < 1e-9, "the game's number at an uncapped rate leaves nothing after 0.5 s",
              StrengthAfter(Rule::kGame, 0, true, 0.008, 0.5), 1e-9);
    }

    // the time scale (slow motion) shortens the step the game gives its update: the fade follows the game's own time, so 30 updates
    // at half speed are half a second of the console's curve
    {
        // 30 updates of a 1/60 s step = 0.5 s of game time
        const double ref = std::pow(25.0 / 30.0, 30 * 0.5);
        const float m = PerUpdateMultiplier(kK, 1.0f / 60.0f);
        double strength = 1.0;
        for (int i = 0; i < 30; ++i) strength *= static_cast<double>(m);
        std::printf("info: 30 updates of a 1/60 s step (slow motion at 30 FPS): %.4f; console after 0.5 s of game time: %.4f\n", strength, ref);
        Check(std::fabs(strength - ref) < ref * 0.002, "30 updates of a half step are half a second of the console's curve", strength, ref);
    }

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
