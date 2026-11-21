// MCLA GPU renderer -- the city in the car paint's reflections: the flags the scenes set for the pause menu's thread. See city_refl_setting.h.

#include "city_refl_setting.h"

#include <atomic>

namespace mcla::gpu::city_refl_setting {

namespace {

// The last answer of the city pack's probe (CityRtScene::OpenPack) and of the hit data's (CityHitScene::OpenFile), as Data; unknown until a scene has asked.
std::atomic<uint8_t> g_base{static_cast<uint8_t>(Data::kUnknown)};
std::atomic<uint8_t> g_hit{static_cast<uint8_t>(Data::kUnknown)};

Data Load(const std::atomic<uint8_t>& flag) { return static_cast<Data>(flag.load(std::memory_order_relaxed)); }

}  // namespace

void NoteBase(Data data) { g_base.store(static_cast<uint8_t>(data), std::memory_order_relaxed); }

void NoteHit(Data data) { g_hit.store(static_cast<uint8_t>(data), std::memory_order_relaxed); }

Data DataNow() { return Combine(Load(g_base), Load(g_hit)); }

}  // namespace mcla::gpu::city_refl_setting
