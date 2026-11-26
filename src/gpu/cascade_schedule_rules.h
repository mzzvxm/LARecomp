#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace mcla::gpu::cascade_schedule {

constexpr uint32_t kCascadeMask = 0x1e0;
constexpr uint8_t Cascade(uint32_t phase) {
  switch (phase) {
    case 0x20: return 1;
    case 0x40: return 2;
    case 0x80: return 3;
    case 0x100: return 4;
    default: return 0;
  }
}
constexpr uint32_t Period(uint8_t cascade) {
  return cascade == 4 ? 4 : cascade == 3 ? 2 : 1;
}
constexpr unsigned TileBit(uint32_t cascade) {
  return cascade >= 1 && cascade <= 4 ? 1u << (cascade - 1) : 0u;
}
constexpr uint32_t TableEntry(uint32_t table, uint8_t cascade) {
  return table + 4u * (cascade - 1);
}
constexpr bool GuestPointer(uint32_t address) {
  return address >= 0x10000 && address < 0xdffff000;
}
struct TileAddresses {
  uint32_t viewport = 0;
  uint32_t tile = 0;
  bool Valid() const { return GuestPointer(viewport) && GuestPointer(tile); }
  bool operator==(const TileAddresses&) const = default;
};
template <typename ReadU32>
TileAddresses ReadTileAddresses(uint32_t manager, uint8_t cascade, ReadU32 read) {
  if (!GuestPointer(manager) || !TileBit(cascade)) return {};
  const uint32_t viewports = read(manager + 16);
  const uint32_t tiles = read(manager + 24);
  if (!GuestPointer(viewports) || !GuestPointer(tiles)) return {};
  return {read(TableEntry(viewports, cascade)), read(TableEntry(tiles, cascade))};
}

using Vec = std::array<float, 3>;
inline float Dot(const Vec& a, const Vec& b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline float DeltaSquared(const Vec& a, const Vec& b) {
  const Vec delta = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
  return Dot(delta, delta);
}
inline bool Normalize(Vec& v) {
  const float length = std::sqrt(Dot(v, v));
  if (!std::isfinite(length) || length < 0.0001f) return false;
  for (float& x : v) x /= length;
  return true;
}

struct View {
  Vec eye{};
  Vec forward{};
  Vec heading{};  // horizontal projection, Y is up
  Vec sun{};
  std::array<float, 5> fit{};  // far, FOV and bias scales, and the viewport FOV
  uint32_t cut = 0;
  uint32_t hour_jump = 0;
  int weather = -1;
  bool valid = false;
};

struct Caster {
  uint32_t id = 0;
  Vec position{};
  uint8_t last_cascade = 0;
};
struct Casters {
  std::array<Caster, 32> items{};  // the traffic selector caps its combined count at 24
  unsigned count = 0;
  bool incomplete = false;

  void Add(const Caster& caster) {
    for (unsigned i = 0; i < count; ++i) {
      if (items[i].id == caster.id) {
        items[i] = caster;
        return;
      }
    }
    if (count < items.size()) items[count++] = caster;
    else incomplete = true;
  }
};

// Width of the narrow side of the measured day boxes (SHADOW-ARCH 4.1).
// Base the motion budget on guest texels so the same content gets the same
// schedule at every host scale, including when a device changes scale.
constexpr float Width(uint8_t cascade) { return cascade == 3 ? 47.6f : 95.2f; }

inline bool CasterChanged(uint8_t cascade, const Casters& old, const Casters& now) {
  if (old.incomplete || now.incomplete) return true;
  const float texel = Width(cascade) / 640.0f;
  unsigned old_count = 0;
  unsigned new_count = 0;
  for (unsigned i = 0; i < old.count; ++i) {
    old_count += old.items[i].last_cascade >= cascade;
  }
  for (unsigned i = 0; i < now.count; ++i) {
    const Caster& a = now.items[i];
    if (a.last_cascade < cascade) continue;
    ++new_count;
    bool match = false;
    for (unsigned j = 0; j < old.count; ++j) {
      const Caster& b = old.items[j];
      if (b.id == a.id && b.last_cascade >= cascade) {
        match = DeltaSquared(a.position, b.position) <= texel * texel;
        break;
      }
    }
    if (!match) return true;
  }
  return new_count != old_count;
}

enum class Reason { kHold, kCold, kCadence, kTravel, kTurn, kLight, kCut, kFit, kCaster, kCount };

inline Reason Decide(uint8_t cascade, uint64_t age, const View& old, const View& now,
                     const Casters& old_casters, const Casters& casters) {
  if (!old.valid || !now.valid) return Reason::kCold;
  if (old.cut != now.cut) return Reason::kCut;
  // 3 degrees for the heading frame and the full forward cone. A sun rotation
  // of 0.25 degrees spans about three guest texels across the box.
  if (old.hour_jump != now.hour_jump || old.weather != now.weather ||
      DeltaSquared(old.sun, now.sun) > 0.00001903856f) return Reason::kLight;
  if (old.fit != now.fit) return Reason::kFit;
  const float travel = Width(cascade) / 64.0f;
  if (DeltaSquared(old.eye, now.eye) > travel * travel) return Reason::kTravel;
  if (Dot(old.heading, now.heading) < 0.998629535f ||
      Dot(old.forward, now.forward) < 0.998629535f) return Reason::kTurn;
  if (CasterChanged(cascade, old_casters, casters)) return Reason::kCaster;
  if (age >= Period(cascade)) return Reason::kCadence;
  return Reason::kHold;
}

inline constexpr std::array kDrawLabels = {
    "sun cascade 1 draws", "sun cascade 2 draws", "sun cascade 3 draws", "sun cascade 4 draws"};
inline constexpr std::array kResolveLabels = {
    "sun cascade 1 resolve", "sun cascade 2 resolve", "sun cascade 3 resolve", "sun cascade 4 resolve"};

}  // namespace mcla::gpu::cascade_schedule
