// Check of the rule that sizes the occlusion's and the bounce light's images (ao_size.h): above 1080 lines of render height the traced
// images are half the picture's size, 1080 lines and below are as they were, the contact shadows keep the size they had, what each frame
// holds in video memory, the cost model, and that the shaders and ao.cpp repeat the header's numbers.
// A program of its own: it needs no renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_ao_size_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_ao_size_test <src/gpu dir> [--print]
//
// With the src/gpu folder given it also reads ao.cpp, ao.h and the shaders and checks the flags and the cvars. --print writes the table of
// memory (before and after, at 3840x2160, 2560x1440 and 1920x1080) and of the modelled cost.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

#include "ao_size.h"
#include "ao_stable.h"
#include "setting_source.h"
#include "ssgi_stable.h"

namespace as = mcla::gpu::ao_size;
namespace stable = mcla::gpu::ao_stable;
namespace sstable = mcla::gpu::ssgi_stable;

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

bool Near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

bool Slurp(const std::string& path, std::string* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return false;
  }
  out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

// A frame's inputs for a named configuration at a render size.
struct Config {
  const char* name;
  bool occlusion, occlusion_ultra;
  bool contact, contact_ultra;
  bool bounce, bounce_ultra, bounce_half;
};

const Config kConfigs[] = {
    {"AO ultra", true, true, false, false, false, false, false},
    {"AO ultra + contact ultra (the High class today)", true, true, true, true, false, false, false},
    {"AO ultra + contact high (the Mid class today)", true, true, true, false, false, false, false},
    {"AO high", true, false, false, false, false, false, false},
    {"AO high + contact high (every image half size already)", true, false, true, false, false, false, false},
    {"AO high + contact ultra", true, false, true, true, false, false, false},
    {"AO ultra + bounce high", true, true, false, false, true, false, false},
    {"AO ultra + bounce ultra", true, true, false, false, true, true, false},
    {"AO ultra + contact ultra + bounce high", true, true, true, true, true, false, false},
    {"AO ultra + bounce high, mcla_gpu_ssgi_low 1 (half size by the old setting)", true, true, false, false, true, false, true},
    {"contact ultra alone", false, false, true, true, false, false, false},
    {"contact high alone", false, false, true, false, false, false, false},
};

as::Inputs InputsOf(const Config& c, uint32_t height, bool half_ao = true, bool half_bounce = true) {
  as::Inputs in;
  in.render_height = height;
  in.occlusion = c.occlusion;
  in.occlusion_ultra = c.occlusion_ultra;
  in.contact = c.contact;
  in.contact_ultra = c.contact_ultra;
  in.bounce = c.bounce;
  in.bounce_ultra = c.bounce_ultra;
  in.bounce_half = c.bounce_half;
  in.half_above_ao = half_ao;
  in.half_above_bounce = half_bounce;
  return in;
}

as::Frame FrameOf(const Config& c, uint32_t width, uint32_t height, bool contact_fast = true, bool half_ao = true, bool half_bounce = true) {
  as::Frame f;
  f.render[0] = width;
  f.render[1] = height;
  f.choice = as::Decide(InputsOf(c, height, half_ao, half_bounce));
  f.occlusion = c.occlusion;
  f.apply = true;
  f.probe = false;
  f.contact = c.contact;
  f.contact_fast = contact_fast;
  f.bounce = c.bounce;
  return f;
}

// ---------------------------------------------------------------------------

void CheckRule() {
  // The old rule, written out again.
  const auto legacy = [](const Config& c) {
    const bool contact_ultra = c.contact && c.contact_ultra;
    bool low = !(c.occlusion && c.occlusion_ultra) && !contact_ultra && !(c.bounce && c.bounce_ultra);
    if (c.bounce && c.bounce_half && !contact_ultra) {
      low = true;
    }
    return low;
  };
  for (const Config& c : kConfigs) {
    for (const uint32_t height : {360u, 540u, 720u, 900u, 1024u, 1079u, 1080u}) {
      const as::Choice choice = as::Decide(InputsOf(c, height));
      const std::string what = std::string(c.name) + " at " + std::to_string(height) + " lines";
      Check(choice.low == legacy(c), what + ": 1080 lines and below the size is the old rule's");
      Check(choice.legacy_low == legacy(c), what + ": the old rule is reported");
      Check(!choice.by_height && !choice.contact_full, what + ": the height rule does nothing");
    }
    // With both cvars off every size is the old rule's, at any height.
    for (const uint32_t height : {1081u, 1440u, 2160u, 4320u}) {
      const as::Choice choice = as::Decide(InputsOf(c, height, false, false));
      Check(choice.low == legacy(c) && !choice.by_height && !choice.contact_full,
            std::string(c.name) + " at " + std::to_string(height) + " lines with both cvars false: today");
    }
  }
  // The threshold: 1080 is not above, 1081 is (the render height: an upscaler's render size, a window with a render scale).
  {
    const Config& ao = kConfigs[0];
    Check(!as::Decide(InputsOf(ao, 1080)).low, "AO ultra at 1080 lines is traced at the picture's size");
    Check(as::Decide(InputsOf(ao, 1081)).low, "AO ultra at 1081 lines is traced at half the picture's size");
    Check(as::Decide(InputsOf(ao, 1200)).low, "AO ultra at 1200 lines (1920x1200) is half size");
    Check(as::Decide(InputsOf(ao, 1440)).by_height && as::Decide(InputsOf(ao, 2160)).by_height, "1440p and 4K are half by the height rule");
  }
  // The occlusion has to be on for the rule to act (the contact shadows alone keep their own rule).
  {
    const Config& contact_only = kConfigs[10];
    const as::Choice c = as::Decide(InputsOf(contact_only, 2160));
    Check(!c.low && !c.by_height && !c.contact_full, "contact ultra alone at 4K: full size, no split");
    const Config& contact_high_only = kConfigs[11];
    const as::Choice l = as::Decide(InputsOf(contact_high_only, 2160));
    Check(l.low && l.legacy_low && !l.by_height && !l.contact_full, "contact high alone at 4K: the old half size, no split");
  }
  // The split: above 1080 lines with the contact shadows on, they keep the size they had (the picture's size where the occlusion's
  // being ultra or the contact on ultra pinned it, half where the old rule made everything half).
  {
    const as::Choice c = as::Decide(InputsOf(kConfigs[1], 2160));
    Check(c.low && c.by_height && c.contact_full, "AO ultra + contact ultra at 4K: the occlusion half, the contact shadows full");
    const as::Choice m = as::Decide(InputsOf(kConfigs[2], 2160));
    Check(m.low && m.by_height && m.contact_full, "AO ultra + contact high at 4K: the occlusion half, the contact shadows at the size they had (full)");
    const as::Choice both_high = as::Decide(InputsOf(kConfigs[4], 2160));
    Check(both_high.low && both_high.legacy_low && !both_high.by_height && !both_high.contact_full,
          "AO high + contact high at 4K: everything half already, nothing changes");
    const as::Choice high_ultra = as::Decide(InputsOf(kConfigs[5], 2160));
    Check(high_ultra.low && high_ultra.by_height && high_ultra.contact_full, "AO high + contact ultra at 4K: the contact pinned it full, now only the contact is");
    const as::Choice no_contact = as::Decide(InputsOf(kConfigs[0], 2160));
    Check(no_contact.low && !no_contact.contact_full, "no contact shadows: nothing to keep at full size");
  }
  // The bounce light: its own cvar decides when it is on (its images give the occlusion's visibility too).
  {
    const as::Choice ultra = as::Decide(InputsOf(kConfigs[7], 2160));
    Check(ultra.low && ultra.by_height && !ultra.contact_full, "AO ultra + bounce ultra at 4K: half size by the height rule (it was pinned full)");
    Check(!as::Decide(InputsOf(kConfigs[7], 2160, true, false)).low, "bounce ultra at 4K with mcla_gpu_ssgi_half false: full size, whatever mcla_gpu_ao_half_above_1080 says");
    Check(as::Decide(InputsOf(kConfigs[7], 2160, false, true)).low, "bounce ultra at 4K with only mcla_gpu_ssgi_half true: half size (the bounce's cvar decides)");
    const as::Choice high = as::Decide(InputsOf(kConfigs[6], 2160));
    Check(high.low && high.by_height, "AO ultra + bounce high at 4K (the default mcla_gpu_ssgi_low 0: it was full size): half size now");
    const as::Choice old_half = as::Decide(InputsOf(kConfigs[9], 2160));
    Check(old_half.low && old_half.legacy_low && !old_half.by_height, "bounce high with mcla_gpu_ssgi_low 1: half size by the old setting, not by the height (the colour chain keeps all samples)");
    const as::Choice with_contact = as::Decide(InputsOf(kConfigs[8], 2160));
    Check(with_contact.low && with_contact.contact_full, "bounce high + contact ultra at 4K: the bounce half, the contact shadows full");
  }
  // The AO's cvar alone does not move a bounce frame, and the bounce's does not move a frame without the bounce.
  Check(as::Decide(InputsOf(kConfigs[0], 2160, true, false)).low, "AO ultra at 4K: mcla_gpu_ssgi_half does not matter without the bounce light");
  Check(!as::Decide(InputsOf(kConfigs[0], 2160, false, true)).low, "AO ultra at 4K: mcla_gpu_ao_half_above_1080 false is today's");
}

void CheckSizes() {
  const uint32_t r4k[2] = {3840, 2160};
  const uint32_t r1440[2] = {2560, 1440};
  const uint32_t r1080[2] = {1920, 1080};
  as::Choice half;
  half.low = true;
  half.by_height = true;
  as::Choice full;
  const as::Sizes s4k = as::SizesOf(r4k, half);
  Check(s4k.ao[0] == 1920 && s4k.ao[1] == 1080 && s4k.level0[0] == 960 && s4k.level0[1] == 540 && s4k.level1[0] == 480 && s4k.level1[1] == 270,
        "4K traced at 1920x1080 with levels 960x540 and 480x270");
  const as::Sizes s1440 = as::SizesOf(r1440, half);
  Check(s1440.ao[0] == 1280 && s1440.ao[1] == 720, "1440p traced at 1280x720");
  const as::Sizes s1080 = as::SizesOf(r1080, full);
  Check(s1080.ao[0] == 1920 && s1080.ao[1] == 1080 && s1080.contact[0] == 1920, "1080p at its own size");
  // The contact shadows' images: the picture's size beside a half size trace, half where the old rule made all half.
  as::Choice split = half;
  split.contact_full = true;
  const as::Sizes sp = as::SizesOf(r4k, split);
  Check(sp.ao[1] == 1080 && sp.contact[0] == 3840 && sp.contact[1] == 2160, "the split: the contact images stay 3840x2160 beside the 1920x1080 trace");
  as::Choice old_low;
  old_low.low = old_low.legacy_low = true;
  const as::Sizes ol = as::SizesOf(r4k, old_low);
  Check(ol.ao[0] == 1920 && ol.contact[0] == 1920, "the old rule's half size: the contact images are half with the rest");
  // Odd sizes (an upscaler's render size): the half image rounds up, as ao.cpp always did, and covers the picture.
  const uint32_t odd[2] = {1921, 1081};
  const as::Sizes so = as::SizesOf(odd, half);
  Check(so.ao[0] == 961 && so.ao[1] == 541 && so.level0[0] == 481 && so.level1[0] == 241, "odd sizes round up");
  Check(so.ao[0] * 2 >= odd[0] && so.ao[1] * 2 >= odd[1], "the half size image covers the picture");
  // The blur's radius is the traced image's height (ao_stable.h): 4K traced at 1080 lines has 5x5, 1440p traced at 720 has 3x3.
  Check(stable::DenoiseRadius(s4k.ao[1]) == 2 && stable::DenoiseRadius(s1440.ao[1]) == 1, "the blur's radius follows the traced image's height (4K 5x5, 1440p 3x3)");
  // Its reach in the picture's own terms stays in ao_stable.h's band at every render height above 1080 lines, traced at half size.
  bool reach_ok = true;
  bool same_as_full = true;
  for (uint32_t h = 1081; h <= 4320; ++h) {
    // the reach as a share of the picture's height in units of the 720 line picture's pixels: radius * 720 / traced lines, which is the
    // same number whichever image it is measured on
    const double r_half = stable::ReachIn720Pixels(as::Half(h));
    const double r_full = stable::ReachIn720Pixels(h);
    reach_ok = reach_ok && r_half >= 0.5 && r_half <= 1.4;
    // up to 4K between 0.66 and 1.34 times what the full size image's blur reaches (the radius is a whole number of pixels; above 4K the
    // full size radius is clamped at 3 and reaches less than the band, the half size one is nearer to it)
    if (h <= 2160) {
      same_as_full = same_as_full && r_half / r_full >= 0.66 && r_half / r_full <= 1.34;
    }
  }
  Check(reach_ok, "the blur's reach at half size is between 0.5 and 1.4 pixels of the 720 line picture for every render height 1081-4320");
  Check(same_as_full, "... and within a third of what the full size image's blur reached, for every height up to 4K (4K: 1.33 against 1.0, 1440p: the same)");
  // The bounce's a-trous filter (ssgi_stable.h) at the traced height: the share of the picture its reach covers stays within a quarter of the full
  // size image's.
  for (const uint32_t h : {1200u, 1440u, 1620u, 2160u}) {
    const double share_full = double(sstable::AtrousReachPixels(sstable::AtrousIterations(2, h))) / double(h);
    const double share_half = double(sstable::AtrousReachPixels(sstable::AtrousIterations(2, as::Half(h)))) / double(as::Half(h));
    Check(share_half / share_full >= 0.7 && share_half / share_full <= 1.3,
          "the bounce's a-trous reach as a share of the picture at " + std::to_string(h) + " lines is within 30% of the full size image's");
    if (g_print) {
      std::printf("   a-trous at %u lines: %d iterations (reach %.4f of the height) full, %d (%.4f) at half size\n", h,
                  sstable::AtrousIterations(2, h), share_full, sstable::AtrousIterations(2, as::Half(h)), share_half);
    }
  }
}

void CheckHalfDepth() {
  // A road at a grazing angle: the linear depth of the plane under a camera 1.4 m high changes by t * t * 2 tan(fov / 2) / (h * H) from one row to
  // the next (h the camera's height, H the picture's rows). The block takes two rows: {t, t, t + step, t + step}.
  const double tan_y = 0.5773503, camera_h = 1.4;
  for (const double rows : {1440.0, 2160.0}) {
    for (const double t : {3.0, 10.0, 20.0, 30.0}) {
      const double step = t * t * 2.0 * tan_y / (camera_h * rows);
      const float d[4] = {float(t), float(t), float(t + step), float(t + step)};
      const as::HalfDepth h = as::HalfSizeDepth(d);
      const double centre = t + 0.5 * step;
      const bool one_surface = step <= as::kHalfSurfaceShare * (t + 0.5 * step) + as::kHalfSurfaceFloor;
      Check(h.averaged == one_surface, "a road at " + std::to_string(int(t)) + " m in " + std::to_string(int(rows)) + " rows: one surface exactly while the step is under the tolerance");
      if (one_surface) {
        Check(std::fabs(double(h.depth) - centre) < 1e-4, "... and the half size depth is the block's centre (the old rule's was " + std::to_string(0.5 * step) + " m off)");
      }
    }
  }
  // The road stays one surface to 105 m at 4K and to 70 m at 1440p (the step is 4% of the depth at t = 0.04 * rows * h / (2 tan(fov / 2))); the
  // occlusion has faded out from 80 m (mcla_gpu_ao_fade_end 160, fading from half of it).
  {
    const auto averaged_at = [&](double t, double rows) {
      const double step = t * t * 2.0 * tan_y / (camera_h * rows);
      const float d[4] = {float(t), float(t), float(t + step), float(t + step)};
      return as::HalfSizeDepth(d).averaged;
    };
    Check(averaged_at(50.0, 2160.0) && averaged_at(30.0, 1440.0) && averaged_at(60.0, 1440.0) && averaged_at(100.0, 2160.0),
          "the grazing road is one surface to 100 m at 4K and 60 m at 1440p");
    Check(!averaged_at(150.0, 2160.0) && !averaged_at(150.0, 1440.0) && !averaged_at(90.0, 1440.0), "... and not at 150 m, nor at 90 m in 1440 rows");
  }
  // An edge keeps the old rule: the sample nearest to the mean.
  {
    const float car[4] = {10.0f, 10.0f, 15.0f, 15.0f};
    const as::HalfDepth h = as::HalfSizeDepth(car);
    Check(!h.averaged && h.depth == 10.0f && h.source == 0, "a 5 m step is an edge: the pixel takes one of the block's own depths (the first of two equally near)");
    const float pole[4] = {30.0f, 30.0f, 30.0f, 14.0f};
    const as::HalfDepth p = as::HalfSizeDepth(pole);
    Check(!p.averaged && p.depth == 30.0f, "a thin pole (1 sample of 4) against the background: the background (the half size image loses a pole under 2 pixels)");
    const float sky_edge[4] = {60000.0f, 60000.0f, 12.0f, 12.0f};
    Check(!as::HalfSizeDepth(sky_edge).averaged, "the skyline is an edge");
    const float sky[4] = {60000.0f, 60000.0f, 60000.0f, 60000.0f};
    const as::HalfDepth s = as::HalfSizeDepth(sky);
    Check(s.averaged && s.depth == 60000.0f, "four sky samples stay the sky");
    const float flat[4] = {7.25f, 7.25f, 7.25f, 7.25f};
    Check(as::HalfSizeDepth(flat).depth == 7.25f, "a flat surface keeps its depth");
    // a kerb: a 15 cm step at 5 m is under the tolerance (4% of 5 m = 20 cm)
    const float kerb[4] = {5.0f, 5.0f, 5.15f, 5.15f};
    Check(as::HalfSizeDepth(kerb).averaged, "a 15 cm kerb step at 5 m is smoothed in the half size image (the apply's upsample treats it as one surface too)");
  }
}

void CheckMemory() {
  const double mib = 1024.0 * 1024.0;
  // The measured state (WEAKGPU.txt, COMPAT-AUDIT.md): AO ultra alone, full size, 3840x2160 = 131.5 MiB; at half size 32.9 MiB.
  {
    const as::Frame f = FrameOf(kConfigs[0], 3840, 2160);
    as::Frame legacy = f;
    legacy.choice.low = false;
    legacy.choice.by_height = false;
    const double before = as::BytesBefore(f).Total() / mib;
    const double after = as::Bytes(f).Total() / mib;
    Check(Near(before, 131.5, 0.1), "AO ultra at 4K before: 131.5 MiB (the measured 131.5)");
    Check(Near(after, 32.9, 0.1), "AO ultra at 4K now: 32.9 MiB (the measured half size's 32.9)");
    Check(Near(as::Bytes(legacy).Total() / mib, 131.5, 0.1), "the model with the old size gives the measured 131.5 MiB too");
  }
  // 1440p
  {
    const as::Frame f = FrameOf(kConfigs[0], 2560, 1440);
    const double before = as::BytesBefore(f).Total() / mib;
    const double after = as::Bytes(f).Total() / mib;
    Check(after < 0.3 * before && after > 0.2 * before, "AO ultra at 1440p: a quarter of the memory (a little over: the levels' rounding)");
  }
  // 1080p: nothing changes without the bounce light.
  for (const Config& c : kConfigs) {
    const as::Frame f = FrameOf(c, 1920, 1080);
    const as::Held before = as::BytesBefore(f);
    const as::Held after = as::Bytes(f);
    if (c.occlusion && !c.bounce) {
      Check(before.Total() == after.Total(), std::string(c.name) + " at 1080p: exactly the same images");
    }
    Check(after.Total() <= before.Total(), std::string(c.name) + " at 1080p: never more than before");
    Check(after.depth == before.depth && after.levels == before.levels && after.history == before.history && after.bounce == before.bounce &&
              after.contact == 0,
          std::string(c.name) + " at 1080p: the traced images are the same sizes (only the unused scratch images may go)");
  }
  // The split's own images: 4K, AO ultra + contact ultra with the filter in the apply: depth R32 + visibility R16 at the picture's size.
  {
    const as::Frame f = FrameOf(kConfigs[1], 3840, 2160);
    const as::Held after = as::Bytes(f);
    const double contact = after.contact / mib;
    Check(Near(contact, 31.64 + 15.82, 0.05), "4K split with the filter in the apply: 31.6 MiB depth + 15.8 MiB visibility of the contact shadows");
    const as::Frame slow = FrameOf(kConfigs[1], 3840, 2160, /*contact_fast=*/false);
    Check(Near(as::Bytes(slow).contact / mib, 31.64 + 15.82 + 15.82, 0.05), "... and a filter pass image more without mcla_gpu_contact_shadows_fast");
    Check(as::Bytes(f).Total() < as::BytesBefore(f).Total() && as::Bytes(slow).Total() < as::BytesBefore(slow).Total(),
          "AO ultra + contact ultra at 4K holds less than before, with and without the cheaper contact path");
  }
  // Never more memory than before, at any size above 1080 lines, for any configuration and either contact path.
  for (const Config& c : kConfigs) {
    for (const uint32_t height : {1081u, 1200u, 1440u, 1620u, 2160u, 4320u}) {
      for (const bool fast : {true, false}) {
        const uint32_t width = height * 16 / 9;
        const as::Frame f = FrameOf(c, width, height, fast);
        Check(as::Bytes(f).Total() <= as::BytesBefore(f).Total(),
              std::string(c.name) + " at " + std::to_string(height) + " lines" + (fast ? "" : " (plain contact)") + ": never more memory than before");
      }
    }
  }
  // The bounce light at 4K: the audit's 436 MiB (AO's depth chain and scratch 68.2 with the bounce's 367.9) before R3 dropped the scratch images.
  {
    const as::Frame f = FrameOf(kConfigs[6], 3840, 2160);
    const as::Held before = as::BytesBefore(f);
    const as::Held after = as::Bytes(f);
    Check(Near(before.bounce / mib, 367.9, 0.2), "bounce light at 4K before: the images are the audit's 367.9 MiB");
    Check(Near(before.Total() / mib, 436.1, 0.3), "bounce light at 4K before: 436 MiB with the AO's depth chain and scratch");
    Check(Near(after.bounce / mib, 367.9 / 4.0, 1.0), "bounce light at 4K now: a quarter of the images (92 MiB)");
    Check(after.scratch == 0, "the visibility passes' two images are not held while the bounce light runs without the contact shadows");
    Check(after.Total() < 0.25 * before.Total() + 2.0 * mib, "bounce light at 4K now: about a quarter of 436 MiB");
  }
  // The scratch images are held for the contact shadows' march while the bounce runs (the unsplit case) and not in the split.
  {
    as::Frame f = FrameOf(kConfigs[8], 3840, 2160);
    const as::Held h = as::Bytes(f);
    Check(h.scratch == 0 && h.contact > 0, "bounce + contact ultra at 4K: the contact shadows have their own images, the scratch pair is not held");
    // contact alone: the pair's second image is the filter pass's, which the apply does instead.
    const as::Frame c = FrameOf(kConfigs[10], 3840, 2160);
    Check(as::Bytes(c).Total() == as::BytesBefore(c).Total() - as::ImageBytes(3840, 2160, as::kBytesR16),
          "contact ultra alone at 4K: the filter pass's image is not held any more (16 MiB), nothing else changes");
    Check(as::Bytes(c).scratch == as::ImageBytes(3840, 2160, as::kBytesR16), "contact ultra alone at 4K (the filter in the apply): one scratch image, the march's, not two");
  }
}

void CheckCost() {
  const uint32_t r4k[2] = {3840, 2160};
  as::Choice full;
  as::Choice half;
  half.low = half.by_height = true;
  const as::Sizes sf = as::SizesOf(r4k, full);
  const as::Sizes sh = as::SizesOf(r4k, half);
  const as::AoCost cf = as::Cost(sf, r4k, 9, 3, false);
  const as::AoCost ch = as::Cost(sh, r4k, 9, 2, false);
  Check(Near(cf.Total(), 0.81 + 0.17 + 0.147 + 0.14 + 0.1, 1e-9), "the model at 4K full size is the measured passes' sum (1.367 ms)");
  Check(Near(cf.main_ms, 0.81, 1e-9) && Near(cf.denoise_ms, 0.147, 1e-9) && Near(cf.temporal_ms, 0.17, 1e-9) && Near(cf.apply_ms, 0.14, 1e-9),
        "... pass by pass");
  Check(ch.main_ms < 0.26 * cf.main_ms && ch.main_ms > 0.24 * cf.main_ms, "half size: the visibility pass costs a quarter");
  Check(ch.denoise_ms < 0.14 * cf.denoise_ms, "half size: the blur costs under a seventh (a quarter of the pixels, 25 taps against 49)");
  Check(Near(ch.apply_ms, cf.apply_ms, 1e-12), "half size: the apply costs the picture's pixels as before");
  const double saving = cf.Total() - ch.Total();
  Check(saving > 0.85 && saving < 1.0, "4K: the model saves 0.85 to 1.0 ms (the audit: about 0.85)");
  const as::AoCost chs = as::Cost(sh, r4k, 9, 2, true);
  Check(chs.Total() > ch.Total() && cf.Total() - chs.Total() > 0.8, "with the contact shadows' own depth pass it still saves over 0.8 ms");
  if (g_print) {
    std::printf("   cost model 4K: full %.3f ms (main %.3f, blur %.3f, history %.3f, depth %.3f, apply %.3f); half %.3f ms (%.3f, %.3f, %.3f, %.3f, %.3f); "
                "half + the contact shadows' depth %.3f ms; saving %.3f ms\n",
                cf.Total(), cf.main_ms, cf.denoise_ms, cf.temporal_ms, cf.depth_ms, cf.apply_ms, ch.Total(), ch.main_ms, ch.denoise_ms,
                ch.temporal_ms, ch.depth_ms, ch.apply_ms, chs.Total(), saving);
  }
}

void PrintTable() {
  const double mib = 1024.0 * 1024.0;
  const uint32_t sizes[3][2] = {{3840, 2160}, {2560, 1440}, {1920, 1080}};
  std::printf("Video memory of the occlusion's and the bounce light's images (MiB; the model of ao_size.h, the images the version before held / now)\n");
  std::printf("%-76s %18s %18s %18s\n", "configuration", "3840x2160", "2560x1440", "1920x1080");
  for (const Config& c : kConfigs) {
    std::printf("%-76s", c.name);
    for (const auto& s : sizes) {
      const as::Frame f = FrameOf(c, s[0], s[1]);
      std::printf(" %7.1f / %7.1f ", as::BytesBefore(f).Total() / mib, as::Bytes(f).Total() / mib);
    }
    std::printf("\n");
  }
}

// ---------------------------------------------------------------------------

void CheckSources(const std::string& gpu) {
  std::string cpp, header, apply, color, temporal, depth, cmake, common;
  if (!Slurp(gpu + "/ao.cpp", &cpp) || !Slurp(gpu + "/ao.h", &header) || !Slurp(gpu + "/hlsl/ao_apply_ps.hlsl", &apply) ||
      !Slurp(gpu + "/hlsl/ao_color_ps.hlsl", &color) || !Slurp(gpu + "/hlsl/ao_temporal_ps.hlsl", &temporal) ||
      !Slurp(gpu + "/hlsl/ao_depth_ps.hlsl", &depth) || !Slurp(gpu + "/CMakeLists.txt", &cmake) || !Slurp(gpu + "/hlsl/ao_common.hlsli", &common)) {
    Check(false, "the files of src/gpu could not be read (" + gpu + ")");
    return;
  }
  // The two settings (REX_SETTING rows of src/settings/generated; the REXCVAR_DEFINEs of ao.cpp before the conversion)
  const setting_source::Info half_above = setting_source::Find(gpu, cpp, "mcla_gpu_ao_half_above_1080");
  const setting_source::Info ssgi_half = setting_source::Find(gpu, cpp, "mcla_gpu_ssgi_half");
  Check(setting_source::DefaultIsTrue(half_above) && setting_source::DefaultIsTrue(ssgi_half), "the settings mcla_gpu_ao_half_above_1080 and mcla_gpu_ssgi_half exist and default to true");
  // both are debug-only and live
  Check(half_above.debug_only && half_above.live, "mcla_gpu_ao_half_above_1080 is debug-only and hot-reloadable");
  Check(ssgi_half.debug_only && ssgi_half.live, "mcla_gpu_ssgi_half is debug-only and hot-reloadable");
  Check(Has(cpp, "larecomp::settings::mcla_gpu_ao_half_above_1080.Get()") && Has(cpp, "larecomp::settings::mcla_gpu_ssgi_half.Get()"), "ao.cpp: both cvars are read by the plan");
  Check(Has(cpp, "ao_size::Decide(in)") && Has(cpp, "ao_size::SizesOf(plan.render, choice)"), "ao.cpp: the plan takes the sizes from ao_size.h");
  Check(Has(cpp, "kApplyContactFullFlag = 1u << 21") && Has(apply, "g_push.slots2.z & (1u << 21)"), "the apply's contact-at-picture-size flag is bit 21 in ao.cpp and in the shader");
  Check(Has(cpp, "kColourOneSampleFlag = 1u << 21") && Has(color, "g_push.slots2.z & (1u << 21)"), "the colour chain's one-sample flag is bit 21 in ao.cpp and in the shader");
  Check(Has(cpp, "kApplyHighSlotShift = 16") && Has(apply, "g_push.slots2.y >> 16") && Has(apply, "g_push.slots2.y & 0xFFFFu"),
        "slots2.y of the apply: the colour slot in the low 16 bits, the contact depth's in the high 16, in ao.cpp and in the shader");
  Check(Has(cpp, "static_assert(kBindlessTextureCount <= (1u << kApplyHighSlotShift)"), "ao.cpp: two bindless slots are asserted to fit one word");
  Check(Has(cpp, "push.slots2[1] |= Colour(0).slot;"), "ao.cpp: the colour slot (the colour chain's level 0 from the screen owner, ScreenShared::ColourPre) is OR-ed beside the contact depth's");
  Check(Has(cpp, "DepthFull().slot << kApplyHighSlotShift"), "ao.cpp: the contact depth slot (the screen owner's full size depth, SS-2) is packed into the high bits");
  // The bits the flags use must not overlap in the apply (limit 8-15 and cap 26-31 of the bounce light; 22-25 the apply's own).
  {
    const uint32_t limit_cap = 0xFFu << 8 | 0x3Fu << 26;
    const uint32_t own = (1u << 22) | (1u << 23) | (1u << 24) | (1u << 25) | (1u << 16);
    Check(((1u << 21) & (limit_cap | own)) == 0u, "bit 21 is free in the apply's flags (view 0-7, limit 8-15, low 16, radius 17-19, apply 22-25, cap 26-31)");
    const uint32_t radius_bits = 0x7u << 17;
    Check(((1u << 21) & radius_bits) == 0u, "bit 21 is clear in the pass flags FillPush builds (the radius is bits 17-19)");
  }
  Check(Has(cpp, "plan.SplitContact()") && Has(cpp, "DepthFull()") && Has(cpp, "contact_raw_"), "ao.cpp: the split has its own depth (the screen owner's DepthFull) and contact images");
  Check(Has(cpp, "push.slots2[2] &= ~(1u << 16);"), "ao.cpp: the contact depth pass is drawn without the half size flag");
  Check(Has(header, "bool SplitContact() const") && Has(header, "bool OneSampleColour() const"), "ao.h: the plan has the split and the one sample rules");
  Check(Has(header, "ContactFilterInApply() const { return !contact_history && ao_size::ContactFilterInApply(contact.fast, SizeChoice()); }"),
        "ao.h: the contact filter in the apply follows ao_size.h (the picture's size, split or not) and is not used while the contact image has a history of its own (noise-all)");
  Check(Has(cpp, "FillPush") && Has(cpp, "ao_stable::DenoiseRadius(plan.ao[1])"), "ao.cpp: the blur's radius is taken from the traced image's height (plan.ao)");
  Check(Has(cpp, "AtrousIterations(int(plan.gi.atrous), plan.ao[1])"), "ao.cpp: the a-trous passes follow the traced image's height");
  Check(Has(cpp, "history_size_[0] != plan.ao[0] || history_size_[1] != plan.ao[1]"), "ao.cpp: a change of the traced size throws the history away (kSize)");
  // The apply uses the depth-aware upsample for the occlusion's and the bounce's images, and not for the contact image beside them.
  Check(Has(apply, "VisibilityAt(g_push.slots.z, pixel, tc, ao_vehicle)") && Has(apply, "GiAt(g_push.slots.z, ColourSlot()"), "the apply upsamples the occlusion's and the bounce's images with the depth-aware weights");
  Check(Has(apply, "AoContactFull() ? g_textures[ContactSlot()].Load(int3(pixel, 0)).x : VisibilityAt(ContactSlot(), pixel, tc)"),
        "the apply reads a full size contact image as it is");
  Check(Has(apply, "AoBilateralEdgeFolded(ContactSlot(), ContactDepthSlot(), pixel, ContactImageSize()"), "the folded contact filter takes the contact image's own depth and size");
  Check(Has(apply, "const float dz = abs(td - tc) / max(0.02f * tc, 0.05f);"), "the apply's depth weights are the old ones (2% of the depth, 5 cm)");
  // The colour chain: one sample, the host sample in slots2.x.
  Check(Has(color, "bool AoOneSample()") && Has(color, "TakeColour(p, first + s)") && Has(color, "g_push.slots2.x"), "ao_color_ps.hlsl: one sample of each pixel, the host sample in slots2.x");
  Check(Has(cpp, "plan.OneSampleColour()") && Has(cpp, "push.slots2[0] = base.slots[1];"), "ao.cpp: the chain is told the depth's host sample");
  // The temporal pass follows the vehicle that the half size depth pass marked.
  Check(Has(temporal, "uint AoVehicleId(int2 pixel, float depth)") && Has(temporal, "if (!AoIsVehicle(depth))"), "ao_temporal_ps.hlsl: at half size a pixel is a vehicle where the depth pass marked it");
  Check(Has(depth, "const uint id = DepthVehicleId(source);\n  float marked = AoSetVehicleBit(t, id != 0u);"), "ao_depth_ps.hlsl: the mark is the chosen sample's");
  Check(Has(depth, "kHalfSurfaceShare = 0.04f") && Has(depth, "kHalfSurfaceFloor = 0.02f") && as::kHalfSurfaceShare == 0.04f && as::kHalfSurfaceFloor == 0.02f,
        "ao_depth_ps.hlsl: the half size depth's tolerance is the header's");
  Check(Has(depth, "if (spread <= kHalfSurfaceShare * mean + kHalfSurfaceFloor) {") && Has(depth, "t = mean;"), "ao_depth_ps.hlsl: one surface takes the mean");
  Check(Has(cmake, "larecomp_ao_size_test") && Has(cmake, "ao_size_test.cpp"), "CMakeLists.txt: the test is a target");
  Check(Has(common, "bool AoLow()"), "ao_common.hlsli: AoLow is still the flag of the half size images");
}

}  // namespace

int main(int argc, char** argv) {
  std::string gpu;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--print") == 0) {
      g_print = true;
    } else {
      gpu = argv[i];
    }
  }
  CheckRule();
  CheckSizes();
  CheckHalfDepth();
  CheckMemory();
  CheckCost();
  if (!gpu.empty()) {
    CheckSources(gpu);
  } else {
    std::printf("(no src/gpu folder given: the shaders' and ao.cpp's flags were not compared)\n");
  }
  if (g_print) {
    PrintTable();
  }
  if (g_failed != 0) {
    std::printf("ao_size_test: %d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  std::printf("ao_size_test: %d checks passed\n", g_checks);
  return 0;
}
