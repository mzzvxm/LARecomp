// Check of the fog volume's per-voxel sample offset (hlsl/volumetrics_noise.hlsli, volumetrics_inject_cs.hlsl, volumetrics.cpp's flag; NOISE-AUDIT, perf\NOISE-AUDIT.md). A program of its own: no renderer,
// no ReXGlue, no GPU.
//
//   cmake --build <dir> --target larecomp_volumetrics_noise_test, then run src/gpu/larecomp_volumetrics_noise_test [<src/gpu dir>] [--print] (a few seconds).
//
// A voxel's sample is moved inside it by the frame's Halton offsets (the host's, 16 frames, the same for every voxel) plus an offset of the voxel's own. The first version's own offset was interleaved
// gradient noise of the cell moved 5.588 cells along the diagonal every frame: it slid across the volume. It compiles the .hlsli as C++ (hlsl_shim.h) and checks, with the shared measures of
// noise_resolve_check.h, that the offset the pass adds up (frac(Halton + the voxel's noise)):
//   is uniform, changes at every voxel every frame, leaves no gap over 0.25 in 16 frames and takes the same step at every voxel
//   leaves, after the history's average of a shadow edge's visibility, no lattice and no pattern that moves; the first version's error is measured beside it
//   is the tile at the voxel moved by (37, 17) a slice: slices differ, and x and y offsets of a voxel are the tile's two channels (uncorrelated)
//   the sources: the shader includes the noise file, carries no gradient noise of its own, its flag is the header's bit 2 and the host sets it from mcla_gpu_noise_blue

#include <cstdint>
#include <cstdio>
#include <string>

#include "hlsl_shim.h"
#include "noise_resolve_check.h"
#include "volumetrics_rules.h"
#include "volumetrics_stable.h"

namespace hlsl_shim {
#include "hlsl/blue_noise.hlsli"
#include "hlsl/volumetrics_noise.hlsli"
}  // namespace hlsl_shim

namespace nc = noise_check;
namespace hs = hlsl_shim;
namespace vol = mcla::gpu::volumetrics;

namespace {

nc::Checks g_check;

// The sample offset of voxel (x, y) of slice z at `frame`, in 0..1 (the shader's frac(jitter.xyz + noise) before 0.5 is taken out): x and y offsets.
std::pair<float, float> Offset(int x, int y, int z, uint32_t frame, bool blue) {
  const hs::float3 n = hs::VolVoxelNoise(x, y, z, float(frame & 0xFFFFu), blue);
  const float jx = vol::Halton(frame % 16u + 1u, 2), jy = vol::Halton(frame % 16u + 1u, 3);
  return {hs::frac(jx + n.x), hs::frac(jy + n.y)};
}

// A shadow edge across the voxel (a lamp's or the sun's): lit where the x offset is past a threshold, and the y offset moves the edge a little.
double Edge(float u1, float u2) { return (u1 + 0.25f * (u2 - 0.5f)) > 0.5f ? 1.0 : 0.0; }

void TestOffset() {
  const auto blue = [](int x, int y, uint32_t f) { return Offset(x, y, 3, f, true); };
  const auto legacy = [](int x, int y, uint32_t f) { return Offset(x, y, 3, f, false); };
  for (int channel = 0; channel < 2; ++channel) {
    const nc::PerFrame p = nc::PerFrameOf(blue, channel);
    if (g_check.print) {
      nc::Print(channel == 0 ? "offset x, blue" : "offset y, blue", p);
      nc::Print(channel == 0 ? "offset x, first version" : "offset y, first version", nc::PerFrameOf(legacy, channel));
    }
    g_check(p.changed > 0.99 && p.max_gap < 0.25 && p.step_spread < 1e-4 && std::fabs(p.mean - 0.5) < 0.02 && std::fabs(p.var - 1.0 / 12.0) < 0.006,
            nc::Num("offset %c is uniform (mean %.3f, variance %.4f), changes at every voxel every frame (%.3f), leaves no gap over %.3f in 16 frames and takes one step at every voxel (%.6f): nothing slides",
                    channel == 0 ? 'x' : 'y', p.mean, p.var, p.changed, p.max_gap, p.step_spread));
  }
}

void TestResolve() {
  const nc::Resolved rl = nc::Resolve([](int x, int y, uint32_t f) { return Offset(x, y, 3, f, false); }, Edge, 100, 128, 40, 0.9, false);
  const nc::Resolved rb = nc::Resolve([](int x, int y, uint32_t f) { return Offset(x, y, 3, f, true); }, Edge, 100, 128, 40, 0.9, false);
  if (g_check.print) {
    nc::Print("shadow edge, first version", rl);
    nc::Print("shadow edge, blue", rb);
  }
  g_check(nc::NoLattice(rb, 3.0) && !nc::Crawls(rb),
          nc::Num("the history's error over a shadow edge has no frequency that stands out (%.4f) and no direction (%.2f, under 3) and does not move (best match with the next frame at (%d, %d): %.2f)",
                  rb.shape.peak_share, rb.shape.wedge_ratio, rb.best_dx, rb.best_dy, rb.best_corr));
  g_check(nc::IsLattice(rl), nc::Num("(the first version's error is a lattice: %.3f of it in one frequency, a direction %.1f times the mean)", rl.shape.peak_share, rl.shape.wedge_ratio));
  g_check(rb.rms < 1.3 * rl.rms + 0.01, nc::Num("... at an rms of %.4f against the first version's %.4f", rb.rms, rl.rms));
}

void TestVoxel() {
  // slices differ (the tile is read at the cell moved by (37, 17) a slice), and the x and y offsets are the tile's two channels
  int same = 0, n = 0;
  double sxy = 0.0, sx = 0.0, sy = 0.0, sxx = 0.0, syy = 0.0;
  for (int y = 0; y < 32; ++y) {
    for (int x = 0; x < 32; ++x) {
      const hs::float3 a = hs::VolVoxelNoise(x, y, 0, 0.0f, true), b = hs::VolVoxelNoise(x, y, 1, 0.0f, true);
      same += (a.x == b.x) ? 1 : 0;
      ++n;
      sx += a.x;
      sy += a.y;
      sxx += double(a.x) * a.x;
      syy += double(a.y) * a.y;
      sxy += double(a.x) * a.y;
    }
  }
  const double c = (sxy / n - sx / n * sy / n) / std::sqrt((sxx / n - sx / n * sx / n) * (syy / n - sy / n * sy / n));
  g_check(same < n / 8 && std::fabs(c) < 0.15, nc::Num("two slices have different noise (%d of %d voxels agree) and a voxel's x and y offsets are uncorrelated (%.3f)", same, n, c));
  bool tile = true;
  for (int y = 0; y < 20 && tile; ++y) {
    for (int x = 0; x < 20 && tile; ++x) {
      const hs::float3 a = hs::VolVoxelNoise(x, y, 2, 5.0f, true), b = hs::VolVoxelNoise(x, y, 2, 9.0f, true);
      tile = a.x == hs::BlueNoiseTileAt(x + 74, y + 34, 0u) && a.y == hs::BlueNoiseTileAt(x + 74, y + 34, 1u) && a.x == b.x && a.y == b.y && a.z == b.z;
    }
  }
  g_check(tile, "the voxel's own offset is the tile at the cell moved by (37, 17) a slice, the same in every frame (the frame's change is the Halton term alone)");
}

void TestSources(const std::string& dir) {
  const std::string inject = nc::ReadFile(dir + "/hlsl/volumetrics_inject_cs.hlsl");
  const std::string host = nc::ReadFile(dir + "/volumetrics.cpp");
  if (inject.empty() || host.empty()) {
    g_check(false, "the sources could not be read (run the test with the src/gpu folder as its argument)");
    return;
  }
  g_check(nc::Has(inject, "#include \"volumetrics_noise.hlsli\"") && nc::Has(inject, "VolVoxelNoise(int(id.x), int(id.y), int(id.z), jitter.w, (g_push.slots.w & kInjectFlagBlueNoise) != 0u)") &&
              !nc::Has(inject, "InterleavedGradient(") && !nc::Has(inject, "52.9829189"),
          "volumetrics_inject_cs.hlsl takes the voxel's offset from volumetrics_noise.hlsli and carries no gradient noise of its own");
  g_check(nc::Has(inject, "static const uint kInjectFlagBlueNoise = 4u;") && vol::kInjectFlagBlueNoise == 4u && (vol::kInjectFlagBlueNoise & (vol::kInjectFlagSliceLamps | vol::kInjectFlagSteadyHeads)) == 0u,
          "the flag is slots.w bit 2, the header's, and overlaps neither the slice lamps' nor the steady headlights' bit");
  g_check(nc::Has(host, "(larecomp::settings::mcla_gpu_noise_blue.Get() ? vol::kInjectFlagBlueNoise : 0u)"), "volumetrics.cpp sets the flag from mcla_gpu_noise_blue (live: read at each frame's inject)");
}

}  // namespace

int main(int argc, char** argv) {
  std::string dir = nc::DirOf(__FILE__);
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--print") {
      g_check.print = true;
    } else {
      dir = a;
    }
  }
  TestOffset();
  TestResolve();
  TestVoxel();
  TestSources(dir);
  std::printf("\n%d checks, %d failed\n", g_check.total, g_check.fails);
  return g_check.fails == 0 ? 0 : 1;
}
