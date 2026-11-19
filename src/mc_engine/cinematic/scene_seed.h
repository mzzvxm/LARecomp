#pragma once
//
// Repeatable scenes (trailer toolkit): the game's random number generators, the seeds a scene holds, the paint of a scene car. Pure: no game, no log;
// cars_test.cpp checks it alone. The game side is in scene_runtime.cpp (the pins) and cars.cpp (the car seed hook).
//
// WHY. Two runs of the same scene differ by a mean of 0.1 to 5 codes of 255 (perf\out\TEST-SCENES.txt): the four parked SUVs of `traffic-headings` came out
// white, black, black, black in one run and black, black, white, silver in the other (ports\run\ts-fixon-... and ts-fixoff-traffic-headings-r1, tour_01_top.png).
//
// WHAT THE GAME DOES (read in the generated code, native\tree-t2\generated, 2026-11-19; [V] = read there, CHECK = not shown by a run).
//
//   The generator. Every generator of the game is the same two-word state (lo, hi): x = lo * 0x5CDCFAA7 + hi (64 bits); lo = low 32 bits of x; hi = high 32 bits;
//   the draw is the low 31 bits of x (a float draw takes the low 23 bits). Seeding (sub_821CE210) is lo = seed (1 for 0), hi = rotl32(seed, 16) ^ seed. [V]
//   The generators there are:
//     SIM     0x82860890   the one the simulation, the AI and the ambient manager draw from (RAND_RESET and the ambient spawners among them): 51 functions. [V]
//     VISUAL  0x82860880   the cosmetic one: particles, the effects' phases, 51 functions in the 0x8231 .. 0x827A ranges, the ambient car's three "personality" floats. [V]
//     CONFIG  [[0x8286D8DC] + 0x16C78]   the car configuration's: mcCarConfigData_GetRandomConfigData and the opponents' random car picks (sub_8239CAA0 / sub_8239CB10). [V reading;
//             that the object behind 0x8286D8DC is stable in a scene is CHECK]
//     SCRIPT  [[[0x82BDA3BC] + 48] + 20]   the script's RAND_INT_RANGE and the like. RAND_RESET and RAND_SET_SEED re-seed it from SIM (sub_822C8618), so a pinned SIM
//             holds it too after a script's RAND_RESET. Not pinned here by itself.
//     CAR     car + 384    one for every ambient car, for its own driving personality. Seeded (sub_826EAD00) from the car's spawn request, word +288.
//   All of them are seeded from the clock once at start (sub_822D4878: SIM, VISUAL). The dev option `randomseed` (0x828608A4) makes the seeding functions take its
//   value instead: a start-up pin only, which a scene cannot use. A scene writes the state words instead.
//
//   The paint of an ambient car. A car of the traffic class is made on the ambient task from a spawn request that the manager's queue (mgr + 8800) holds. When the
//   queue gives the request out (sub_826DF260, at 0x826DF2DC `stw r3,832(r31)`) it draws a 31-bit number from SIM and stores it in the queue entry's word +832, which is
//   the request's word +288 (the request sits at entry + 544): the seed of the car's own generator. [V] Cars of the same model made one after the other therefore have
//   different generators, and a car's look follows from what it draws from its own (the first draws of sub_826EAD00 / sub_826E5788 / sub_826E86C8) and from SIM and VISUAL
//   at the time. CHECK: which of these picks the colour of the body is not read yet (the type has `iPaintType` and `vColors` in the game's data: mcAmbientCarTypeColor).
//   What the hook (MCLAAmbientCarSeed, cars.cpp) does: for the queue entry of one of the scene's own layout items (entry + 560 holds the item, request + 16 as sub_826DF7F0 writes it; CHECK that the entry carries the request at +544 when it is given out: the log's `hook fired` counts say) it stores
//   PaintSeed(n) instead of the draw. That makes the car's own generator the same in every run, which is the whole of the paint if the colour is chosen from it, and a
//   necessary half of it otherwise. The other half is the pins of SIM and VISUAL below. A repeat run with the same `paint` and a different colour says it is not that
//   generator: the log's `[cars] ... seed` lines say whether the hook fired.
//
//   The layout item's own word +128 is 0 for an item made by AmbientMgr_AddAmbientToLayout and byte +140 is 1 (sub_826E1418), so the car is made through sub_826DF7F0 (called from
//   sub_82687A60 with the variant 0 .. 19 drawn from SIM at 0x82687BCC: a number the car's code never reads as a 16-bit word, so it is not the paint [V search]). [V]
//
// WHAT A SCENE HOLDS (mcla_scene_seed, default 1 = on; 0 = off): at the scene's start and, with mcla_scene_seed_each_frame (default on), at every update, the three
// generators SIM, VISUAL and CONFIG are given a state that depends only on the seed and on the scene's update number (FrameSeed). Every update of the scene then
// starts from the same state in every run; what is left is the order of the draws inside one update between threads (the ambient task, the AI). Held once only
// (each_frame off), the stream would stay lined up only while no thread draws a different number of times, which they do.
//
// NOT HELD (named, for the report): the generators of other threads' private objects (the ambient cars' own, except through the paint seed); the SCRIPT generator
// (see above); anything the operating system decides: the order the ambient task and the game thread reach a shared structure in, the streaming order, the physics
// pool's order (hooks/job_workers.cpp); the audio. The renderer's per-frame noise follows the scene's update number (gpu/scene_noise.h).
//

#include <cstdint>

namespace mc::cinematic::seed {

inline constexpr uint32_t kMultiplier = 0x5CDCFAA7u;

// Guest addresses (see above).
inline constexpr uint32_t kSimGenerator = 0x82860890u;     // two words: lo, hi (big endian in the guest memory); +8 a byte, +12 a float (the seeder clears the byte)
inline constexpr uint32_t kVisualGenerator = 0x82860880u;  // the same shape
inline constexpr uint32_t kConfigOwnerPtr = 0x8286D8DCu;   // a pointer to the object that holds the config generator ...
inline constexpr uint32_t kConfigGeneratorAt = 0x16C78u;   // ... at this offset (lis 0x8287 -10020; + 65536 + 27768 in sub_8239CAA0)
inline constexpr uint32_t kQueueSeedStoreAddr = 0x826DF2DCu;  // `stw r3,832(r31)` in sub_826DF260: the hook is before it (larecomp_config.toml)
inline constexpr uint32_t kQueueEntryItemAt = 560;            // the layout item of the request in the queue entry (entry + 544 + 16)
inline constexpr uint32_t kQueueEntrySeedAt = 832;            // the request's word +288 (entry + 544 + 288)

struct Generator {
    uint32_t lo = 0;
    uint32_t hi = 0;
};

// The state the game's seeder (sub_821CE210) makes from `seed`.
inline Generator SeedGenerator(uint32_t seed) {
    Generator g;
    g.lo = seed == 0 ? 1u : seed;
    const uint32_t rot = (seed << 16) | (seed >> 16);
    g.hi = rot ^ seed;
    return g;
}

// One draw, exactly the game's: the state moves on and the low 31 bits of the new `lo` are returned.
inline uint32_t Draw31(Generator& g) {
    const uint64_t x = static_cast<uint64_t>(g.lo) * kMultiplier + g.hi;
    g.lo = static_cast<uint32_t>(x);
    g.hi = static_cast<uint32_t>(x >> 32);
    return g.lo & 0x7FFFFFFFu;
}

// A well mixed 32-bit number from a 32-bit one (the murmur3 finalizer).
inline uint32_t Mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x85EBCA6Bu;
    x ^= x >> 13;
    x *= 0xC2B2AE35u;
    x ^= x >> 16;
    return x;
}

enum class Lane : uint32_t { kSim = 1, kVisual = 2, kConfig = 3 };

// The seed of one generator for one update of a scene: from the scene's seed, the update's number (counted from the scene's start) and the generator. Never 0.
inline uint32_t FrameSeed(uint32_t world_seed, uint64_t update, Lane lane) {
    uint32_t h = Mix32(world_seed * 0x9E3779B1u + 0x7F4A7C15u);
    h = Mix32(h ^ (static_cast<uint32_t>(update) * 0x85EBCA6Bu + static_cast<uint32_t>(update >> 32) * 0xC2B2AE35u));
    h = Mix32(h + static_cast<uint32_t>(lane) * 0x27D4EB2Fu);
    return h == 0 ? 1u : h;
}

// The state a generator gets at an update of a scene.
inline Generator PinnedState(uint32_t world_seed, uint64_t update, Lane lane) {
    return SeedGenerator(FrameSeed(world_seed, update, lane));
}

// The 31-bit seed of an ambient car's own generator for paint `paint` (0 .. 9999): a number that is the same in every run and differs from one paint to the next. Never 0.
inline uint32_t PaintSeed(int paint) {
    const uint32_t h = Mix32(0xA11CE000u ^ (static_cast<uint32_t>(paint) * 0x9E3779B9u)) & 0x7FFFFFFFu;
    return h == 0 ? 1u : h;
}

// The paint a scene car has when the scene does not say: its place among the scene's cars that take one (0 for the first), wrapped into 0 .. 9999.
inline int DefaultPaint(int ordinal) {
    return ordinal < 0 ? 0 : ordinal % 10000;
}

}  // namespace mc::cinematic::seed
