#ifndef REXGLUE_HAS_XEO3_TARGET
//
// Repeatable scenes: holds the game's random generators at a state that depends only on the scene's seed and the scene's update number, and tells the renderer the
// scene's update number (gpu/scene_noise.h). The reading of the game's code, the generators' addresses and what is and is not held are in scene_seed.h; the
// ambient car's own seed (the paint) is cars.cpp's.
//
// WHEN. The scene runner calls the applier's `apply` at every update of a running scene, from Patch_DeltaTimePre on the game thread, before the game's own update
// (physics, the AI, the ambient manager) runs. The generators are written there. The words are written as the game keeps them (big endian); the game threads that draw
// at the same moment (the ambient task) may read a half-written pair once: the draw is then one arbitrary number, nothing else happens.
//
// Written without a build or a run. CHECK items are marked where nothing has shown the behaviour in a running game.
//

#include "scene_seed_runtime.h"

#include "settings/larecomp_settings.h"
#include <rex/kernel/title_runtime.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "larecomp_log.h"
#include "scene_runtime.h"
#include "scene_seed.h"

#if defined(LARECOMP_HAS_GPU)
#include "gpu/scene_noise.h"
#endif



namespace mc::cinematic {

namespace {

uint8_t* Membase() {
    auto* rt = REX_RUNTIME();
    return rt ? rt->GetMembase().GetBase() : nullptr;
}

uint32_t Load32(const uint8_t* base, uint32_t ea) {
    return (uint32_t(base[ea + 0]) << 24) | (uint32_t(base[ea + 1]) << 16) | (uint32_t(base[ea + 2]) << 8) | uint32_t(base[ea + 3]);
}

void Store32(uint8_t* base, uint32_t ea, uint32_t v) {
    base[ea + 0] = static_cast<uint8_t>(v >> 24);
    base[ea + 1] = static_cast<uint8_t>(v >> 16);
    base[ea + 2] = static_cast<uint8_t>(v >> 8);
    base[ea + 3] = static_cast<uint8_t>(v);
}

// The config generator's address: [[0x8286D8DC] + 0x16C78]'s owner. The object is a long-lived manager of the game; the pointer is a heap address (0x4... to 0xBF...).
// CHECK: the object is stable for a scene and 0x16C80 bytes long (sub_8239CAA0 reads it with the same offsets).
uint32_t ConfigGeneratorAddress(const uint8_t* base) {
    const uint32_t owner = Load32(base, seed::kConfigOwnerPtr);
    if (owner < 0x40000000u || owner >= 0xBFFF0000u || (owner & 3u) != 0) return 0;
    return owner + seed::kConfigGeneratorAt;
}

void WriteGenerator(uint8_t* base, uint32_t at, const seed::Generator& g, bool clear_flag) {
    Store32(base, at, g.lo);
    Store32(base, at + 4, g.hi);
    if (clear_flag) base[at + 8] = 0;  // the seeder clears the cached-gaussian byte (sub_821CE210)
}

struct Runner {
    bool on = false;
    uint32_t world_seed = 1;
    seed::SceneUpdateCounter counter;  // the update numbers: one per running update, the pinned time's own number during a still
    uint64_t ticks = 0;                // updates of the scene seen
    uint64_t still_ticks = 0;          // ... of them with the clock pinned (a still: mcla_scene_t)
    bool pinned_once = false;
    uint64_t pins = 0;
    bool config_seen = false;
    bool config_missing_logged = false;
};
Runner g;

void PinAll(uint8_t* base, uint64_t update) {
    WriteGenerator(base, seed::kSimGenerator, seed::PinnedState(g.world_seed, update, seed::Lane::kSim), true);
    WriteGenerator(base, seed::kVisualGenerator, seed::PinnedState(g.world_seed, update, seed::Lane::kVisual), true);
    const uint32_t cfg = ConfigGeneratorAddress(base);
    if (cfg != 0) {
        WriteGenerator(base, cfg, seed::PinnedState(g.world_seed, update, seed::Lane::kConfig), false);
        g.config_seen = true;
    } else if (!g.config_missing_logged) {
        g.config_missing_logged = true;
        LARECOMP_APP_WARN("[seed] the car configuration's generator was not found (the pointer at 0x{:08X} is not a heap address yet): not held at update {}", seed::kConfigOwnerPtr,
                          update);
    }
    ++g.pins;
}

void ApplierBegin(const Scene& scene) {
    g = Runner{};
    LARECOMP_APP_INFO("[seed] scene '{}': mcla_scene_seed {} (0 = off; a `set` line of the scene may change it), mcla_scene_seed_each_frame {}", scene.name, larecomp::settings::mcla_scene_seed.Get(),
                      larecomp::settings::mcla_scene_seed_each_frame.Get() ? "on" : "off");
#if defined(LARECOMP_HAS_GPU)
    mcla::gpu::scene_noise::Rearm();
#endif
}

// The settings are read at every update, so that a scene's own `set mcla_scene_seed ...` line counts too. The update's number is the scene's, whether the seed is on or not.
void ApplierApply(const Scene&, double t) {
    // A still (mcla_scene_t >= 0, the harness's scene_at / scene_shot) holds the scene clock while the game goes on updating, for as long as the picture takes (a window
    // grab comes at a wall-clock moment inside the hold: det1's updates per run were 1852 and 1860 for the same 720 of the scene). Its update number is the pinned time's,
    // not a count, so that every frame of a still gets the same generators and the same renderer noise, and a still taken again after the clock ran on gets them again.
    const bool still = larecomp::settings::mcla_scene_t.Get() >= 0.0;
    const uint64_t update = g.counter.Take(still, t);
    ++g.ticks;
    if (still) ++g.still_ticks;
    const int32_t seed_value = larecomp::settings::mcla_scene_seed.Get();
    if (seed_value <= 0) {
        if (g.on) {
            g.on = false;
            LARECOMP_APP_INFO("[seed] mcla_scene_seed is 0 at update {}: the generators and the renderer's noise are left alone from here", update);
        }
#if defined(LARECOMP_HAS_GPU)
        mcla::gpu::scene_noise::SetUpdate(-1);
#endif
        return;
    }
    if (!g.on || g.world_seed != static_cast<uint32_t>(seed_value)) {
        g.on = true;
        g.world_seed = static_cast<uint32_t>(seed_value);
        g.pinned_once = false;
        LARECOMP_APP_INFO("[seed] seed {} from update {}: the simulation's generator 0x{:08X}, the cosmetic one 0x{:08X} and the car configuration's [[0x{:08X}] + 0x{:X}] are held {}; the "
                          "renderer's noise follows the scene's updates. Not held: the script generator (re-seeded from the simulation's by RAND_RESET), the ambient cars' own (the paint's "
                          "seed), thread order, streaming order, audio",
                          g.world_seed, update, seed::kSimGenerator, seed::kVisualGenerator, seed::kConfigOwnerPtr, seed::kConfigGeneratorAt,
                          larecomp::settings::mcla_scene_seed_each_frame.Get() ? "at every update" : "once");
    }
#if defined(LARECOMP_HAS_GPU)
    mcla::gpu::scene_noise::SetUpdate(static_cast<int64_t>(update));  // the frame this update draws carries this number (stamped at the hand-over: scene_noise.h)
#endif
    uint8_t* base = Membase();
    if (!base) return;
    if (larecomp::settings::mcla_scene_seed_each_frame.Get() || !g.pinned_once) {
        PinAll(base, update);
        g.pinned_once = true;
    }
}

void ApplierEnd() {
#if defined(LARECOMP_HAS_GPU)
    mcla::gpu::scene_noise::SetUpdate(-1);
#endif
    if (!g.on) return;
    LARECOMP_APP_INFO("[seed] the scene is over: {} update(s) ({} of them a still: the clock pinned), {} pin(s) of the generators, the car configuration's generator {}", g.ticks, g.still_ticks,
                      g.pins, g.config_seen ? "was found" : "was never found");
#if defined(LARECOMP_HAS_GPU)
    {
        // How the renderer's frames found the scene update they show (gpu/scene_noise.h THE TAG). The renderer is a few frames behind: about, not exact. `found` close to
        // `stamped` and `missed` near 0 = the noise followed the scene; `found` 0 = it never paired (the first version's fault).
        const mcla::gpu::scene_noise::Summary s = mcla::gpu::scene_noise::GetSummary();
        LARECOMP_APP_INFO("[seed] renderer noise: {} hand-over(s) carried a scene update, {} renderer frame(s) found theirs, {} did not", s.stamped, s.found, s.missed);
    }
#endif
    g.on = false;
}

const TrackApplier kApplier = {"seed", 0u, &ApplierBegin, &ApplierApply, &ApplierEnd};

struct Registrar {
    Registrar() { RegisterTrackApplier(&kApplier); }
} g_registrar;

}  // namespace

GeneratorHold::GeneratorHold(uint32_t seed_in) {
    uint8_t* base = Membase();
    if (!base || seed_in == 0) return;
    const uint32_t at[2] = {seed::kSimGenerator, seed::kVisualGenerator};
    for (int i = 0; i < 2; ++i) {
        saved_[2 * i] = Load32(base, at[i]);
        saved_[2 * i + 1] = Load32(base, at[i] + 4);
    }
    config_at_ = ConfigGeneratorAddress(base);
    if (config_at_ != 0) {
        saved_[4] = Load32(base, config_at_);
        saved_[5] = Load32(base, config_at_ + 4);
    }
    WriteGenerator(base, seed::kSimGenerator, seed::SeedGenerator(seed::Mix32(seed_in + 0x1u)), false);
    WriteGenerator(base, seed::kVisualGenerator, seed::SeedGenerator(seed::Mix32(seed_in + 0x2u)), false);
    if (config_at_ != 0) WriteGenerator(base, config_at_, seed::SeedGenerator(seed::Mix32(seed_in + 0x3u)), false);
    held_ = true;
}

GeneratorHold::~GeneratorHold() {
    if (!held_) return;
    uint8_t* base = Membase();
    if (!base) return;
    Store32(base, seed::kSimGenerator, saved_[0]);
    Store32(base, seed::kSimGenerator + 4, saved_[1]);
    Store32(base, seed::kVisualGenerator, saved_[2]);
    Store32(base, seed::kVisualGenerator + 4, saved_[3]);
    if (config_at_ != 0) {
        Store32(base, config_at_, saved_[4]);
        Store32(base, config_at_ + 4, saved_[5]);
    }
}

}  // namespace mc::cinematic

#else  // REXGLUE_HAS_XEO3_TARGET

#include "scene_seed_runtime.h"

namespace mc::cinematic {
GeneratorHold::GeneratorHold(uint32_t) {}
GeneratorHold::~GeneratorHold() {}
}  // namespace mc::cinematic

#endif  // REXGLUE_HAS_XEO3_TARGET
