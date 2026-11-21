// The smoke of a closed race checkpoint dissipates over about half a second at any frame rate (fork issue 97:
// "all the smoke disappears instantly along with the checkpoint").
//
// What the game does (read in the recompiled code and the image's strings, not seen in a running game):
// a checkpoint object (mcCheckpoint, 368 bytes; created by sub_82266208, whose update is sub_822664F8) owns
// two particle effect instances: +336 the "checkpoint_indicator" (or "checkpoint_ctf_generic") effect and +340
// "light_column". Its per-slot state at +200 + 4 * slot is 3 closed, 0 open, 2 opening (0.5 s) and 1
// closing (0.25 s); CheckpointManager_ActivateSingleCheckpoint (sub_82263820) sets 2, ..._Deactivate...
// (sub_82263898) sets 1. A closing or closed checkpoint lets go of its effects by sub_8247A8D0, which hands
// the instance to its rule (the effect keeps updating until its particles are gone). Before it lets go of the
// +336 effect it writes two numbers into the instance, in three places:
//   - sub_82264E80 (the checkpoint's set-position, called by Checkpoint_SetPosition 0x82267078; it lets go of the effects and
//     makes them again at the new place), the `stfs f13,468(r6)` at 0x82264EE4
//   - sub_822664F8, a slot that is open or opening when the checkpoint's flag +329 is set: the `stfs f13,468(r11)` at 0x8226685C
//   - sub_822664F8, a slot that is closing or closed (or hidden): the `stfs f13,468(r11)` at 0x82266D6C
// (the second is the one a pickup runs: the deactivate native sets the state to 1 and the next update lets go)
// +464 = dt * 1.0 (0x827DCEF8) and +468 = dt * 25.0 (0x827DCEFC), dt the engine's step (0x827D7508). The
// instance's update (sub_8247A1B0) does `+488 += +464` (the effect's clock) and `+432 *= +468` (a strength
// that scales the colour of the live particles; every other effect has +464 = 0 and +468 = 1: no fade).
// On the console dt is 1/30 s: the smoke is multiplied by 25/30 at each of 30 updates a second and has
// dissipated in half a second. The multiplier was made from the step but is applied once per update, so a
// shorter step makes it smaller, not the fade longer: 0.4167 per update at 60 FPS (gone in 90 ms), 0.174 at
// 144 FPS (18 ms), which players saw as the smoke vanishing with the checkpoint.
//
// The fix (effect_fade.h has the rule and effect_fade_test.cpp checks it): the three stores write (25 / 30)
// ^ (dt * 30) instead of 25 * dt. At 1/30 s that is the console's 25/30 and at every other step the same
// decay per second; the effect's clock (+464) is the game's own dt and is not touched.
//
// The hook runs just before each `stfs f13,468(rX)`, where f13 = dt * 25 as the game made it. It checks that
// f13 is that product of the two globals before it changes anything, so a hook that landed in the wrong place
// (a different build of the game) leaves the game's own number. mcla_fix_checkpoint_fade turns it off (the
// FIXES page of the debug menu) for a before and after picture.
//
// CHECK: the addresses and the meaning of +432 / +464 / +468 are read from the generated code (native\tree-t11),
// not seen in a running game; that +432 is what dims the smoke (it is a factor of the colour alpha in the
// particle update: sub_824BB3E8, sub_824BBB20, sub_824BC788, sub_824B6E40 pass it with +496 to the particle's
// colour) are read; that a released instance keeps updating until its particles are gone is not read (it is
// what the console's smoke does, per the player's report in fork issue 97); the log line
// below tells a run which of the three stores fired and what it wrote; the picture of a closed checkpoint at
// 30 and 60 FPS is the check.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/ppc/context.h>

#include <atomic>
#include <cmath>
#include <cstdint>

#include "effect_fade.h"
#include "hooks_internal.h"
#include "larecomp_guest.h"
#include "larecomp_log.h"
#include "settings/larecomp_settings.h"

namespace {

constexpr uint32_t kEngineStep = 0x827D7508;     // the engine timer + 0x08: the update's step, seconds (scaled by the time scale)
constexpr uint32_t kFadeRate = 0x827DCEFC;       // 25.0: the game's constant of the fade (+468 = step * this)
constexpr uint32_t kModeTimer = 0x827D7510;      // the timer that restarts from 0 when a race loads (race.tour waits for it to pass 9.3 s)

std::atomic<int> g_logged{0};

void FadeStore(PPCRegister& f13, uint32_t site) {
    uint8_t* const base = larecomp::guest::Membase();
    if (!base) return;
    const float step = ReadGuestF32(base, kEngineStep);
    const float k = ReadGuestF32(base, kFadeRate);
    const float game = static_cast<float>(f13.f64);
    // f13 is `step * k` as the game rounded it, or this is not the place the hook was made for.
    if (std::fabs(game - mcla::fade::GameMultiplier(k, step)) > 1e-6f) return;
    const bool on = larecomp::settings::mcla_fix_checkpoint_fade.Get();
    const float ours = mcla::fade::PerUpdateMultiplier(k, step);
    if (g_logged.fetch_add(1, std::memory_order_relaxed) < 8) {
        LARECOMP_APP_INFO("[CheckpointFade] store {:08X}, mode timer {:.2f} s: a closed checkpoint's smoke effect is let go; "
                          "step {:.2f} ms, the game's multiplier per update {:.4f} (0.8333 at 30 FPS), the console's "
                          "decay per second would be {:.4f}, this update's multiplier {:.4f}: {}",
                          site, ReadGuestF32(base, kModeTimer), step * 1000.0f, game,
                          std::pow(k * mcla::fade::kConsoleStepS, 1.0f / mcla::fade::kConsoleStepS), ours,
                          on ? "held to the console's fade" : "left as the game made it (mcla_fix_checkpoint_fade is off)");
    }
    if (on) f13.f64 = static_cast<double>(ours);
}

}  // namespace

// sub_82264E80 (set-position), before the `stfs f13,468(r6)` at 0x82264EE4.
void Hook_CheckpointFadeDeactivate(PPCRegister& f13) { FadeStore(f13, 0x82264EE4); }

// sub_822664F8, before the `stfs f13,468(r11)` at 0x8226685C.
void Hook_CheckpointFadeFlagged(PPCRegister& f13) { FadeStore(f13, 0x8226685C); }

// sub_822664F8, before the `stfs f13,468(r11)` at 0x82266D6C (the block of a slot that is closing or closed).
void Hook_CheckpointFadeSlotEnd(PPCRegister& f13) { FadeStore(f13, 0x82266D6C); }
#endif  // REXGLUE_HAS_XEO3_TARGET
