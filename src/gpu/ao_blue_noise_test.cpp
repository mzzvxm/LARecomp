// Check of what the blue noise tile does to the ambient occlusion, the bounce light and the contact shadows (blue_noise.h, hlsl/ao_common.hlsli AoSpatialNoise,
// ao.cpp mcla_gpu_ao_blue_noise). A program of its own: it needs no renderer, no ReXGlue and no GPU, and exits 0 when every check holds. About ten seconds.
//
//   cmake --build <build dir> --target larecomp_ao_blue_noise_test        (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_ao_blue_noise_test [<src/gpu dir>] [--print]
//
// What the owner saw (2026-11-21): thin diagonal lines on the night road and on the kerbs, static, and with the occlusion off in the same picture they were gone from
// the road. The occlusion's sampling noise was interleaved gradient noise, a LATTICE (a few sharp spatial frequencies), so the error that the 3x3 blur and the average
// over 12 frames leave is a diagonal lattice too. This runs the C++ twin of the occlusion's main pass (ao_halo.h Visibility) over a wall seen from a road, through the
// blur and the history of the real passes (ao_stable.h HistoryWeight, 12 frames), with the first version's noise, the lattice of 13 frames with interleaved gradient
// noise (what shipped until now) and the lattice with the blue noise, and measures the error: how much of it sits in ONE spatial frequency and in one direction.
// Given the src/gpu folder it also reads the sources: the shaders' use of the tile, the flag, the setting and the debug row.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "ao_halo.h"
#include "ao_stable.h"
#include "blue_noise.h"
#include "blue_noise_spectrum.h"
#include "setting_source.h"

namespace bn = mcla::gpu::blue_noise;
namespace halo = mcla::gpu::ao_halo;
namespace stable = mcla::gpu::ao_stable;
using namespace blue_noise_spectrum;

namespace {

int g_checks = 0;
int g_failed = 0;
bool g_print = false;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}
std::string Num(const char* fmt, double a, double b = 0.0, double c = 0.0, double d = 0.0) {
  char buf[300];
  std::snprintf(buf, sizeof(buf), fmt, a, b, c, d);
  return buf;
}

// ---------------------------------------------------------------------------
// the occlusion's error on the C++ twin
// ---------------------------------------------------------------------------

struct Rig {
  int width = 1280, height = 720;
  double tan_y = 0.5773503, tan_x = 0.0;
  double cam_y = 1.5;
  double wall_x = -3.5;
  Rig() { tan_x = tan_y * width / height; }
  // Linear depth of the pixel and what it shows: 0 the road, 1 the wall, -1 the sky.
  float Depth(int px, int py, int* what) const {
    const double u = (px + 0.5) / width * 2.0 - 1.0, v = 1.0 - (py + 0.5) / height * 2.0;
    const double dx = u * tan_x, dy = v * tan_y;
    double best = 60000.0;
    int id = -1;
    if (dy < -1e-9) {
      const double t = -cam_y / dy;
      if (t > 0.0 && t < best) {
        best = t;
        id = 0;
      }
    }
    if (dx < -1e-9) {
      const double t = wall_x / dx;
      const double y = cam_y + dy * t;
      if (t > 0.0 && y >= 0.0 && y <= 6.0 && t < best) {
        best = t;
        id = 1;
      }
    }
    if (what) {
      *what = id;
    }
    return float(best);
  }
};

struct ErrorStat {
  double rms[3] = {0, 0, 0};   // the error against the truth at N = 4, 12, 24 frames
  Shape shape[3];              // its spectrum at those
  double one_frame = 0.0;      // the rms of one frame after the blur
  double at13 = 0.0;           // at 13 frames of the lattice's period
};

// One noise's error after the accumulation: each frame's visibility blurred 3x3 (the denoise pass; a pixel only counts neighbours on its own surface), then
// the history's average (ao_stable.h HistoryWeight, the default 12 frames), against the truth, the mean of 52 frames (four lattices) of the same noise.
ErrorStat Measure(const Rig& rig, const halo::Params& params, int x0, int y0, int pw) {
  halo::Scene sc;
  sc.width = rig.width;
  sc.height = rig.height;
  sc.tan_y = float(rig.tan_y);
  sc.tan_x = float(rig.tan_x);
  sc.depth = [&](int x, int y) { return rig.Depth(x, y, nullptr); };
  sc.vehicle = [](int, int) { return false; };
  const int frames = 52;
  const int m = pw + 2;
  std::vector<int> id(size_t(m) * size_t(m));
  for (int y = 0; y < m; ++y) {
    for (int x = 0; x < m; ++x) {
      rig.Depth(x0 + x - 1, y0 + y - 1, &id[size_t(y) * size_t(m) + size_t(x)]);
    }
  }
  std::vector<std::vector<double>> blur(size_t(frames), std::vector<double>(size_t(pw) * size_t(pw)));
  for (int f = 0; f < frames; ++f) {
    std::vector<double> raw(size_t(m) * size_t(m));
    for (int y = 0; y < m; ++y) {
      for (int x = 0; x < m; ++x) {
        raw[size_t(y) * size_t(m) + size_t(x)] = double(halo::Visibility(sc, params, x0 + x - 1, y0 + y - 1, f));
      }
    }
    for (int y = 0; y < pw; ++y) {
      for (int x = 0; x < pw; ++x) {
        double sum = 0.0, w = 0.0;
        for (int dy = -1; dy <= 1; ++dy) {
          for (int dx = -1; dx <= 1; ++dx) {
            const size_t q = size_t(y + 1 + dy) * size_t(m) + size_t(x + 1 + dx);
            if (id[q] != id[size_t(y + 1) * size_t(m) + size_t(x + 1)]) {
              continue;
            }
            const double k = std::exp(-0.5 * double(dx * dx + dy * dy) / (0.9 * 0.9));
            sum += k * raw[q];
            w += k;
          }
        }
        blur[size_t(f)][size_t(y) * size_t(pw) + size_t(x)] = sum / w;
      }
    }
  }
  std::vector<double> truth(size_t(pw) * size_t(pw), 0.0);
  for (int f = 0; f < frames; ++f) {
    for (size_t i = 0; i < truth.size(); ++i) {
      truth[i] += blur[size_t(f)][i] / frames;
    }
  }
  ErrorStat out;
  double var = 0.0;
  for (size_t i = 0; i < truth.size(); ++i) {
    var += (blur[0][i] - truth[i]) * (blur[0][i] - truth[i]);
  }
  out.one_frame = std::sqrt(var / double(truth.size()));
  std::vector<double> hist = blur[0];
  for (int f = 1; f < frames; ++f) {
    const double w = double(stable::HistoryWeight(uint32_t(f - 1), 12));
    for (size_t i = 0; i < hist.size(); ++i) {
      hist[i] = blur[size_t(f)][i] * (1.0 - w) + hist[i] * w;
    }
    const int n = f + 1;
    const int slot = n == 4 ? 0 : (n == 12 ? 1 : (n == 24 ? 2 : (n == 13 ? 3 : -1)));
    if (slot < 0) {
      continue;
    }
    std::vector<double> res(hist.size());
    double sum2 = 0.0;
    for (size_t i = 0; i < hist.size(); ++i) {
      res[i] = hist[i] - truth[i];
      sum2 += res[i] * res[i];
    }
    const double rms = std::sqrt(sum2 / double(res.size()));
    if (slot == 3) {
      out.at13 = rms;
    } else {
      out.rms[slot] = rms;
      out.shape[slot] = Analyse(Power(res, pw, true), pw);
    }
  }
  return out;
}

void CheckTwinAt(const bn::Tile& tile, int x0, int y0, const char* where) {
  const Rig rig;
  const int pw = 128;
  auto C = [&](bool ok, const std::string& what) { Check(ok, std::string(where) + ": " + what); };
  halo::Params first;
  halo::Params now;
  now.noise_lattice = 13;
  halo::Params blue = now;
  blue.blue = &tile;
  const ErrorStat a = Measure(rig, first, x0, y0, pw);
  const ErrorStat b = Measure(rig, now, x0, y0, pw);
  const ErrorStat c = Measure(rig, blue, x0, y0, pw);
  const char* names[3] = {"first version", "lattice + gradient noise", "lattice + blue noise"};
  const ErrorStat* all[3] = {&a, &b, &c};
  const int at[3] = {4, 12, 24};
  if (g_print) {
    for (int k = 0; k < 3; ++k) {
      std::printf("%s, %-26s one frame after the blur: rms %.4f, at 13 frames %.5f\n", where, names[k], all[k]->one_frame, all[k]->at13);
      for (int j = 0; j < 3; ++j) {
        std::printf("    N=%2d error rms %.5f  one-frequency share %.4f  band share %.3f  strongest direction %.2f times the mean\n", at[j], all[k]->rms[j], all[k]->shape[j].peak_share,
                    all[k]->shape[j].band_share, all[k]->shape[j].wedge_ratio);
      }
    }
  }
  for (int j = 0; j < 3; ++j) {
    // The error of the gradient noise is a lattice: a few per cent of it in ONE frequency, and one direction. The blue noise's is neither.
    C(b.shape[j].peak_share > 0.015, Num("lattice + gradient noise, %.0f frames: the error sits in one frequency (%.3f of it, over 0.015)", double(at[j]), b.shape[j].peak_share));
    C(c.shape[j].peak_share < 0.012, Num("lattice + blue noise, %.0f frames: no frequency stands out (%.4f of the error, under 0.012)", double(at[j]), c.shape[j].peak_share));
    C(b.shape[j].wedge_ratio > 3.0, Num("lattice + gradient noise, %.0f frames: the error has a direction (%.2f times the mean of 12, over 3)", double(at[j]), b.shape[j].wedge_ratio));
    C(c.shape[j].wedge_ratio < 2.2, Num("lattice + blue noise, %.0f frames: it has none (%.2f, under 2.2)", double(at[j]), c.shape[j].wedge_ratio));
    C(c.shape[j].peak_share * 8.0 < b.shape[j].peak_share, Num("%.0f frames: the lattice's peak is at least 8 times lower with the blue noise (%.0f times)", double(at[j]), b.shape[j].peak_share / std::max(c.shape[j].peak_share, 1e-9)));
    // And the amount of error is the same order: the blue noise takes the structure out, it does not trade it for grain (the blur takes the gradient noise's
    // 3x3 neighbourhoods nearly clean but for the lattice; the blue noise's error is a little more and has no lattice).
    C(c.rms[j] < 1.6 * b.rms[j] + 0.001, Num("%.0f frames: the blue noise's error rms %.5f is within 1.6 times the gradient noise's (%.5f)", double(at[j]), c.rms[j], b.rms[j]));
  }
  // The history's frames work: at the lattice's own period the 13 frames are the exact mean in both (nothing is lost by the change), the error of one frame is
  // the same size, and the error after 24 frames is under 0.5% of the picture's brightness.
  C(b.at13 < 0.001 && c.at13 < 0.001, Num("13 frames of the lattice leave under 0.001 (gradient %.5f, blue %.5f)", b.at13, c.at13));
  C(std::fabs(c.one_frame - b.one_frame) < 0.1 * b.one_frame, Num("one frame: the same error (%.4f against %.4f)", c.one_frame, b.one_frame));
  C(c.rms[2] < 0.005 && c.rms[1] < 0.01, Num("blue noise: the error after 12 and 24 frames is %.4f and %.4f (under 0.01 and 0.005)", c.rms[1], c.rms[2]));
}

void CheckTwin(const bn::Tile& tile) {
  // A wall seen from the road (the surface the occlusion has the most to say about), and the road at the wall's foot with the wall above it.
  CheckTwinAt(tile, 256, 330, "wall");
  CheckTwinAt(tile, 300, 440, "wall and its foot");
}

// ---------------------------------------------------------------------------
// the sources
// ---------------------------------------------------------------------------

std::string Slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void CheckSources(const std::string& dir) {
  const std::string common = Slurp(dir + "/hlsl/ao_common.hlsli");
  const std::string main_ps = Slurp(dir + "/hlsl/ao_main_ps.hlsl");
  const std::string contact = Slurp(dir + "/hlsl/ao_contact_ps.hlsl");
  const std::string cpp = Slurp(dir + "/ao.cpp");
  const std::string table = Slurp(dir + "/hlsl/blue_noise.hlsli");
  const std::string menu = Slurp(dir + "/../mc_engine/menus/debug_menu_pages.cpp");
  Check(!common.empty() && !main_ps.empty() && !contact.empty() && !cpp.empty() && !table.empty(), "the sources are readable from " + dir);
  if (common.empty() || main_ps.empty() || contact.empty() || cpp.empty() || table.empty()) {
    return;
  }
  auto has = [](const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; };
  auto count = [](const std::string& text, const std::string& what) {
    int n = 0;
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) {
      ++n;
    }
    return n;
  };
  Check(has(common, "#include \"blue_noise.hlsli\""), "ao_common.hlsli includes the tile");
  Check(has(common, "static const uint kAoNoiseBlueFlag = 1u << 29;") && stable::kBlueNoiseFlag == (1u << 29), "ao_common.hlsli: the blue noise's flag is bit 29 of slots2.y, the header's");
  Check((stable::kBlueNoiseFlag & stable::kLatticeFlag) == 0 && (stable::kBlueNoiseFlag & stable::kLatticePhaseMask) == 0 && (stable::kBlueNoiseFlag & stable::kFrameWrap) == 0,
        "the flag overlaps neither the lattice's flag, its phase nor the first version's frame number");
  Check(has(common, "float2 AoSpatialNoise(int2 pixel)") && has(common, "return BlueNoiseTile2At(pixel.x, pixel.y);"), "ao_common.hlsli: AoSpatialNoise reads both channels of the tile");
  Check(has(common, "return float2(AoNoise(p), AoNoise(p.yx + float2(31.0f, 11.0f)));"), "ao_common.hlsli: without the flag the first version's two gradient noises");
  Check(has(common, "float AoSpatialNoise1(int2 pixel)") && has(common, "return BlueNoiseTileAt(pixel.x, pixel.y, 0u);"), "ao_common.hlsli: the contact march reads channel 0");
  Check(count(main_ps, "const float2 spatial = AoSpatialNoise(pixel);") == 2 && !has(main_ps, "AoNoise("),
        "ao_main_ps.hlsl: both variants (the plain pass and the bounce light's) take the pixel's noise from AoSpatialNoise");
  Check(has(contact, "frac(AoSpatialNoise1(pixel) + frame * 0.75487766624669f)") && !has(contact, "AoNoise("), "ao_contact_ps.hlsl: the march's start offset takes the tile as well");
  Check(has(contact, "(g_push.slots2.y & ~kAoNoiseBlueFlag) != 0u"), "ao_contact_ps.hlsl: the flag in slots2.y does not count as 'a temporal upscaler runs'");
  Check(has(cpp, "larecomp::settings::mcla_gpu_ao_blue_noise.Get()"), "ao.cpp reads mcla_gpu_ao_blue_noise");
  Check(has(cpp, "ao_stable::NoiseCode(scene_noise::Index(frame_), larecomp::settings::mcla_gpu_ao_stable.Get(), use_blue_noise)"), "ao.cpp: the main pass's noise code carries the flag");
  Check(has(cpp, "push->slots2[1] = uint32_t(scene_noise::Index(frame_) & 1023) | (use_blue_noise ? ao_stable::kBlueNoiseFlag : 0u);"), "ao.cpp: the contact march's frame number carries it");
  Check(has(cpp, "push.slots2[1] = plan.upscaler ? base.slots2[1] : (base.slots2[1] & ao_stable::kBlueNoiseFlag);"), "ao.cpp: the contact march without an upscaler keeps the flag and has no frame number");
  const std::string settings_h = Slurp(dir + "/../settings/generated/settings_gpu.h"), manifest = Slurp(dir + "/../settings/generated/manifest.json"), meta = Slurp(dir + "/../settings/generated/settings_meta.inc");
  Check(has(settings_h, "inline auto& mcla_gpu_ao_blue_noise = g_gpu.mcla_gpu_ao_blue_noise;") && has(manifest, "\"mcla_gpu_ao_blue_noise\": {") && has(meta, "{\"mcla_gpu_ao_blue_noise\", \"MCLA/GPU\"},"),
        "mcla_gpu_ao_blue_noise is in the settings header's aliases (larecomp::settings::...), the manifest and the meta list");
  const setting_source::Info blue_setting = setting_source::Find(dir, cpp, "mcla_gpu_ao_blue_noise");
  Check(blue_setting.found && blue_setting.default_text == "true" && blue_setting.debug_only && blue_setting.live, "mcla_gpu_ao_blue_noise: on by default, a debug setting, live");
  Check(has(menu, "\"mcla_gpu_ao_blue_noise\""), "the debug menu's FIXES page has the A/B row");
  // The table: the file starts the way --emit writes it and holds the packed ranks (CheckTable compares the numbers).
  Check(has(table, "WRITTEN by `larecomp_blue_noise_test --emit`") && has(table, "#define BLUE_NOISE_SIZE 32"), "blue_noise.hlsli says how it is made");
}

}  // namespace

int main(int argc, char** argv) {
  std::string dir;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--print") == 0) {
      g_print = true;
    } else {
      dir = argv[i];
    }
  }
  const bn::Tile tile = bn::MakeTile();
  CheckTwin(tile);
  if (!dir.empty()) {
    CheckSources(dir);
  }
  std::printf("ao_blue_noise_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
