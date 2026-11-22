// Check of the rear view mirror's draw class (mirror_draws.h): the rectangle's packing, the host edge arithmetic and the viewport rule. A program of its own:
// it needs no renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_mirror_draws_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_mirror_draws_test
//
// The compile by hand: clang++ -std=c++23 mirror_draws_test.cpp -o mirror_draws_test.exe

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "mirror_draws.h"

using namespace mcla::gpu::mirror_draws;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

// The default rectangle (rear_view.cpp): the frame's window at 894,60 plus the preset's (stock: 0,60) shift plus the viewport offsets (22, 2): 916,122 229x44.
const Rect kStock{916, 122, 229, 44};

// What ComputeViewport makes of a guest viewport (x, y, w, h) at a scale of n/20: the edges of [x, x+w) go to the host by Scale::Edge.
struct HostViewport {
  float x, y, w, h;
};
HostViewport HostOf(int x, int y, int w, int h, uint32_t n) {
  const auto edge = [&](int64_t g) { return std::floor(double(g) * double(n) / 20.0 + 0.5); };
  return {float(edge(x)), float(edge(y)), float(edge(x + w) - edge(x)), float(edge(y + h) - edge(y))};
}

bool Matches(const Rect& r, const HostViewport& v, uint32_t n) { return MatchesViewport(r, v.x, v.y, v.w, v.h, n); }

void TestPacking() {
  const Rect samples[] = {kStock, {0, 0, 1, 1}, {-852, 78, 229, 44}, {32767, 32767, 32767, 32767}, {-32768, -32768, 1280, 720}, {1, 2, 3, 4}};
  for (const Rect& r : samples) {
    Check(Unpack(Pack(r)) == r, "a rectangle survives packing: " + std::to_string(r.x) + "," + std::to_string(r.y) + " " + std::to_string(r.w) + "x" + std::to_string(r.h));
  }
  Check(Pack(Rect{}) == 0 && !Unpack(0).valid(), "nothing published is the zero word");
  Check(Pack(Rect{10, 10, 0, 44}) == 0 && Pack(Rect{10, 10, 229, 0}) == 0, "a rectangle with no area is nothing");
  Check(Pack(Rect{10, 10, -5, 44}) == 0, "a negative size is nothing");
  Check(Pack(Rect{40000, 10, 229, 44}) == 0 && Pack(Rect{10, -40000, 229, 44}) == 0, "a position beyond 16 bits is nothing, not a wrapped one");
  Check(Pack(Rect{10, 10, 70000, 44}) == 0, "a size beyond 15 bits is nothing");
}

void TestPublish() {
  Withdraw();
  Check(!Current().valid(), "nothing published at first");
  Publish(kStock);
  Check(Current() == kStock, "the published rectangle is read back");
  Publish(Rect{-852, 78, 229, 44});
  Check(Current().x == -852 && Current().y == 78, "a new rectangle replaces it, negative x kept");
  Withdraw();
  Check(!Current().valid(), "withdrawn");
  Publish(Rect{});
  Check(!Current().valid(), "publishing an empty rectangle withdraws");
}

void TestHostEdge() {
  Check(HostEdge(916, 20) == 916, "scale 1: the edge is the guest's");
  Check(HostEdge(916, 60) == 2748, "scale 3 (4K): three host pixels per guest pixel");
  Check(HostEdge(916, 30) == 1374, "scale 1.5 (1080p): 916 x 1.5");
  Check(HostEdge(1, 30) == 2, "scale 1.5 rounds half up (1.5 -> 2), as Scale::Edge does");
  Check(HostEdge(0, 45) == 0, "zero stays zero");
  Check(HostEdge(-1, 30) == -1, "a negative edge floors: -1.5 + 0.5 = -1");
  Check(HostEdge(-852, 20) == -852, "a negative edge at scale 1");
}

void TestMatches() {
  for (uint32_t n : {20u, 30u, 40u, 45u, 60u, 80u}) {
    const std::string at = " at " + std::to_string(n) + "/20";
    const HostViewport own = HostOf(kStock.x, kStock.y, kStock.w, kStock.h, n);
    Check(Matches(kStock, own, n), "the mirror's own viewport matches" + at);
    // The guest viewport's half pixel: an edge one host pixel off still matches.
    Check(Matches(kStock, {own.x + 1, own.y, own.w, own.h}, n), "one pixel right still matches" + at);
    Check(Matches(kStock, {own.x, own.y - 1, own.w + 1, own.h}, n), "one pixel up and one wider still matches" + at);
    Check(!Matches(kStock, {own.x + 3, own.y, own.w, own.h}, n), "three pixels right does not" + at);
    Check(!Matches(kStock, {own.x, own.y, own.w + 4, own.h}, n), "four pixels wider does not" + at);
    Check(!Matches(kStock, {own.x, own.y, own.w, own.h + 5}, n), "five pixels taller does not" + at);
    // The whole target (the main view) and a shadow cascade's viewport.
    const HostViewport whole = HostOf(0, 0, 1280, 720, n);
    Check(!Matches(kStock, whole, n), "the whole target is not the mirror" + at);
    const HostViewport cascade = HostOf(0, 0, 1024, 1024, n);
    Check(!Matches(kStock, cascade, n), "a cascade is not the mirror" + at);
    // Another small window of the same size elsewhere (the minimap, say) is not.
    const HostViewport elsewhere = HostOf(40, 500, kStock.w, kStock.h, n);
    Check(!Matches(kStock, elsewhere, n), "a window of the same size elsewhere is not the mirror" + at);
  }
  Check(!MatchesViewport(Rect{}, 916, 122, 229, 44, 20), "no published rectangle matches nothing");
  Check(!MatchesViewport(kStock, 916, 122, 229, 44, 0), "a scale of zero matches nothing");
  // A preset that pushes the rectangle off the left edge (negative x) at 4K.
  const Rect off{-20, 78, 229, 44};
  Check(Matches(off, HostOf(-20, 78, 229, 44, 60), 60), "a negative x is matched too");
  // The viewport is exactly the rectangle's size at scale 1 only when the numbers are integers: NaN and infinities never match.
  Check(!MatchesViewport(kStock, 0.0f / 0.0f, 122, 229, 44, 20), "NaN does not match");
}

}  // namespace

int main() {
  TestPacking();
  TestPublish();
  TestHostEdge();
  TestMatches();
  Withdraw();
  std::printf("mirror_draws_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
