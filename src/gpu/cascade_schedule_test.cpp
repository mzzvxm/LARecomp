#include "cascade_schedule_rules.h"

#include <cstdio>
#include <cstdlib>
#include <limits>

namespace cs = mcla::gpu::cascade_schedule;

namespace {
int checks = 0;
void Check(bool ok, const char* what) {
  ++checks;
  if (ok) return;
  std::fprintf(stderr, "FAIL: %s\n", what);
  std::fflush(stderr);
  std::_Exit(1);
}
}

int main() {
  using cs::Reason;
  cs::View old;
  old.valid = true;
  old.forward = old.heading = {0, 0, 1};
  old.sun = {0, 1, 0};
  const cs::Casters empty;
  Check(cs::Cascade(0x20) == 1 && cs::Cascade(0x40) == 2, "near phase mapping");
  Check(cs::Cascade(0x80) == 3 && cs::Cascade(0x100) == 4, "far phase mapping");
  Check(cs::Cascade(0x180) == 0 && cs::Cascade(0x400) == 0, "only individual sun phases");
  Check(cs::TileBit(1) == 1 && cs::TileBit(2) == 2 && cs::TileBit(3) == 4 && cs::TileBit(4) == 8, "atlas tile mask");
  Check(cs::TileBit(0) == 0 && cs::TileBit(257) == 0, "tile range");
  Check(cs::TableEntry(0x1000, 1) == 0x1000 && cs::TableEntry(0x1000, 4) == 0x100c, "guest tables contain consecutive BE32 pointers");
  unsigned reads = 0;
  bool missing = false;
  const auto read = [&](uint32_t address) -> uint32_t {
    ++reads;
    switch (address) {
      case 0x10010: return 0x20000;
      case 0x10018: return missing ? 0 : 0x30000;
      case 0x20008: return 0x42000;
      case 0x30008: return 0x52000;
      case 0x2000c: return 0x43000;
      case 0x3000c: return 0x53000;
      default: std::_Exit(2);
    }
  };
  Check(cs::ReadTileAddresses(0x10000, 3, read) == cs::TileAddresses{0x42000, 0x52000}, "cascade 3 reads its own pointers");
  Check(cs::ReadTileAddresses(0x10000, 4, read) == cs::TileAddresses{0x43000, 0x53000}, "cascade 4 reads its own pointers");
  Check(!cs::ReadTileAddresses(0, 4, read).Valid() && reads == 8, "bad owner does not read memory");
  missing = true;
  Check(!cs::ReadTileAddresses(0x10000, 4, read).Valid() && reads == 10, "missing table does not read a tile");
  for (uint8_t c = 1; c <= 4; ++c) {
    unsigned refresh = 0;
    unsigned age = 0;
    for (unsigned frame = 0; frame < 12; ++frame) {
      if (cs::Decide(c, ++age, old, old, empty, empty) != Reason::kHold) {
        age = 0;
        ++refresh;
      }
    }
    Check(refresh == (c == 4 ? 3 : c == 3 ? 6 : 12), "cadence over a steady sequence");
  }
  const auto decide = [&](const cs::View& v) { return cs::Decide(4, 1, old, v, empty, empty); };
  Check(decide(old) == Reason::kHold, "fixed inputs hold before the refresh period");
  Check(cs::Decide(4, 4, old, old, empty, empty) == Reason::kCadence, "fixed inputs refresh at the period");
  cs::View now = old;
  now.valid = false;
  Check(decide(now) == Reason::kCold, "invalid data fail open");
  Check(cs::Decide(4, 1, now, old, empty, empty) == Reason::kCold, "prime the map before holding");
  now = old;
  now.eye[0] = 1.48f;
  Check(decide(now) == Reason::kHold, "inside far travel budget");
  Check(cs::Decide(3, 1, old, now, empty, empty) == Reason::kTravel, "cascade 3 has a smaller budget");
  now.eye[0] = 1.49f;
  Check(decide(now) == Reason::kTravel, "outside far travel budget");
  now = old;
  now.heading = {0.06f, 0, 0.9981984f};
  Check(decide(now) == Reason::kTurn, "heading rotation");
  now = old;
  now.forward = {0, 0.06f, 0.9981984f};
  Check(decide(now) == Reason::kTurn, "pitch changes coverage too");
  now = old;
  now.heading = {0.05f, 0, 0.9987492f};
  Check(decide(now) == Reason::kHold, "turn under three degrees");
  now = old;
  now.sun = {0.005f, 0.9999875f, 0};
  Check(decide(now) == Reason::kLight, "sun moved");
  now = old;
  ++now.hour_jump;
  Check(decide(now) == Reason::kLight, "hour jump");
  now = old;
  now.weather = 1;
  Check(decide(now) == Reason::kLight, "weather change");
  now = old;
  ++now.cut;
  Check(decide(now) == Reason::kCut, "same-pose cut");
  for (unsigned i = 0; i < old.fit.size(); ++i) {
    now = old;
    now.fit[i] = 1;
    Check(decide(now) == Reason::kFit, "fit or lens change");
  }
  cs::Casters a, b;
  a.Add({0x1980, {0, 0, 0}, 4});
  b.Add({0x1980, {0.14f, 0, 0}, 4});
  Check(cs::Decide(4, 1, old, old, a, b) == Reason::kHold, "sub-texel movement in 4");
  Check(cs::Decide(3, 1, old, old, a, b) == Reason::kCaster, "same movement exceeds one texel in 3");
  b.Add({0x1980, {0.15f, 0, 0}, 4});
  Check(b.count == 1 && cs::Decide(4, 1, old, old, a, b) == Reason::kCaster, "over one texel in 4");
  b.Add({0x1980, {0, 0, 0}, 3});
  Check(cs::Decide(4, 1, old, old, a, b) == Reason::kCaster, "cast coverage removed");
  Check(cs::Decide(4, 1, old, old, b, a) == Reason::kCaster, "cast coverage added");
  Check(cs::Decide(3, 1, old, old, a, b) == Reason::kHold, "a nearer cascade still has the caster");
  b = a;
  b.incomplete = true;
  Check(cs::Decide(4, 1, old, old, a, b) == Reason::kCaster, "incomplete census refreshes");
  cs::Vec bad{std::numeric_limits<float>::quiet_NaN(), 0, 1};
  Check(!cs::Normalize(bad), "bad camera direction");
  cs::Vec usable{2, 0, 0};
  Check(cs::Normalize(usable) && usable[0] == 1, "normalize direction");
  std::printf("%d checks passed\n", checks);
}
