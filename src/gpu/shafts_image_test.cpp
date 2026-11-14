// Check of the sun shafts' image size rule (shafts_image.h): the cap at a 960x540 box, the footprint of a texel of the sky
// image and the sky pass's taps. A program of its own: it needs no renderer, no ReXGlue and no GPU, and exits 0 when every
// check holds.
//
//   cmake --build <build dir> --target larecomp_shafts_image_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_shafts_image_test [<src/gpu/hlsl dir>]
//
// With the hlsl folder it also checks that the shaders say what the rule's header says: the sky pass's tap formula and its
// footprint input, and that the blur's step is in UV (so the rays do not depend on the image's size).
//
// The expected sizes are written out here again on purpose (the audit's R6: 4K and 1440p give 960x540, 1080p and below are
// untouched, other shapes keep their aspect and stay at least as large as the box).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "shafts_image.h"

using namespace mcla::gpu::shafts_image;

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

std::string Name(uint32_t w, uint32_t h) { return std::to_string(w) + "x" + std::to_string(h); }

void CheckSize(uint32_t render_w, uint32_t render_h, bool cap, uint32_t want_w, uint32_t want_h) {
  const Size got = ImageSize(render_w, render_h, cap);
  ++g_checks;
  if (got.width != want_w || got.height != want_h) {
    ++g_failed;
    std::printf("FAILED: ImageSize(%s, cap %d): got %s, want %s\n", Name(render_w, render_h).c_str(), int(cap),
                Name(got.width, got.height).c_str(), Name(want_w, want_h).c_str());
  }
}

// The old sky pass's taps: the 2x2 block of the texel, clamped.
int32_t OldTap(uint32_t texel, uint32_t k, uint32_t render) {
  return std::min(int32_t(texel * 2 + k), int32_t(render) - 1);
}

bool ReadFile(const std::string& path, std::string* text) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  std::stringstream buffer;
  buffer << in.rdbuf();
  *text = buffer.str();
  return true;
}

bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

}  // namespace

int main(int argc, char** argv) {
  // --- today's size, and the cap off -----------------------------------------------------------------------------
  Check(HalfSize(1920, 1080) == Size{960, 540}, "half of 1080p");
  Check(HalfSize(1281, 721) == Size{641, 361}, "half rounds up");
  const uint32_t kPictures[][2] = {{1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}, {3440, 1440}, {1920, 1200},
                                   {5120, 1440}, {7680, 4320}, {1366, 768}, {1281, 721}};
  for (const auto& p : kPictures) {
    const Size half = HalfSize(p[0], p[1]);
    CheckSize(p[0], p[1], false, half.width, half.height);  // cap off: today's, always
  }

  // --- the cap on: the sizes -------------------------------------------------------------------------------------
  // At or under 1080p-like sizes nothing changes (at 1x and at the window sizes a 1080p screen gives).
  CheckSize(1280, 720, true, 640, 360);
  CheckSize(1366, 768, true, 683, 384);
  CheckSize(1600, 900, true, 800, 450);
  CheckSize(1920, 1080, true, 960, 540);
  CheckSize(1281, 721, true, 641, 361);
  CheckSize(1920, 1200, true, 960, 600);  // 16:10: 960 wide already, so untouched (not shrunk to 540 high)
  CheckSize(1440, 1080, true, 720, 540);  // 4:3
  CheckSize(2560, 1080, true, 1280, 540); // 21:9 at 1080p is 540 high already: untouched
  // Above: the 960x540 box, the aspect kept.
  CheckSize(2560, 1440, true, 960, 540);  // 1440p
  CheckSize(3840, 2160, true, 960, 540);  // 4K
  CheckSize(7680, 4320, true, 960, 540);  // 8K
  CheckSize(2560, 1600, true, 960, 600);  // 16:10 1600p: 960 wide binds, 600 high
  CheckSize(3440, 1440, true, 1290, 540); // 21:9 1440p: the height binds, the width follows the aspect (1720 * 540 / 720)
  CheckSize(5120, 1440, true, 1920, 540); // 32:9
  CheckSize(3841, 2161, true, 960, 541);  // odd sizes: the other axis is rounded up
  // The sun gives shafts at 4K: the image is a quarter (the audit's saving), 4x fewer texels for the blur's taps.
  {
    const Size big = ImageSize(3840, 2160, false);
    const Size small = ImageSize(3840, 2160, true);
    Check(big == Size{1920, 1080}, "4K without the cap is 1920x1080 (the old size)");
    Check(uint64_t(big.width) * big.height == 4ull * small.width * small.height, "4K: a quarter of the texels");
  }

  // --- the cap on: properties over many pictures -----------------------------------------------------------------
  {
    uint32_t checked = 0;
    bool never_larger = true;
    bool covers_box = true;
    bool keeps_untouched = true;
    bool aspect = true;
    bool half_is_cap_off = true;
    for (uint32_t h = 64; h <= 4400; h += 13) {
      for (uint32_t w = 64; w <= 8200; w += 17) {
        ++checked;
        const Size half = HalfSize(w, h);
        const Size on = ImageSize(w, h, true);
        const Size off = ImageSize(w, h, false);
        half_is_cap_off = half_is_cap_off && off == half;
        never_larger = never_larger && on.width <= half.width && on.height <= half.height;
        // Each axis is at least what the box needs, unless today's is smaller than the box there.
        covers_box = covers_box && on.width >= std::min(half.width, kCapWidth) &&
                     on.height >= std::min(half.height, kCapHeight);
        keeps_untouched = keeps_untouched && ((half.width <= kCapWidth || half.height <= kCapHeight) ? on == half : true);
        // The aspect: within one texel of today's in the axis that was rounded up.
        const double a_half = double(half.width) / double(half.height);
        const double a_on = double(on.width) / double(on.height);
        aspect = aspect && std::fabs(a_on - a_half) <= a_half / double(std::min(on.width, on.height)) + 1e-9;
      }
    }
    Check(checked > 100000, "the sweep covers many pictures (" + std::to_string(checked) + ")");
    Check(half_is_cap_off, "cap off is half the picture for every size");
    Check(never_larger, "the capped image is never larger than today's");
    Check(covers_box, "the capped image covers the 960x540 box in every axis in which today's does");
    Check(keeps_untouched, "a picture whose half is within the box's width or height is untouched");
    Check(aspect, "the capped image keeps today's aspect (within the rounding)");
  }

  // --- the footprint and the sky pass's taps ---------------------------------------------------------------------
  Check(Footprint(1920, 960) == 2.0f && Footprint(1281, 641) == 2.0f, "the half size has footprint exactly 2 (odd sizes too)");
  Check(Footprint(3840, 960) == 4.0f && Footprint(2160, 540) == 4.0f, "4K at the cap spans 4 pixels a texel");
  Check(std::fabs(Footprint(2560, 960) - 2560.0f / 960.0f) < 1e-6f, "1440p at the cap spans 2.667 pixels a texel");
  {
    // With the old size the taps are the old ones, bit for bit, at every texel of pictures of many sizes (odd ones too).
    bool same = true;
    for (uint32_t render : {1280u, 1281u, 1366u, 1919u, 1920u, 1921u, 640u, 65u}) {
      const uint32_t image = (render + 1) / 2;
      const float footprint = Footprint(render, image);
      for (uint32_t texel = 0; texel < image; ++texel) {
        for (uint32_t k = 0; k < 2; ++k) {
          same = same && SkyTap(texel, k, footprint, render) == OldTap(texel, k, render);
        }
      }
    }
    Check(same, "at the old size the four taps are the old 2x2 block of every texel, clamped as before");
  }
  {
    // At the cap the taps are in the picture and inside their texel's footprint, one quarter and three quarters across.
    bool inside = true;
    bool spread = true;
    for (uint32_t render : {2560u, 3840u, 3841u, 5120u, 7680u}) {
      const uint32_t image = ImageSize(render, 2160, true).width;
      const float footprint = Footprint(render, image);
      for (uint32_t texel = 0; texel < image; ++texel) {
        const int32_t a = SkyTap(texel, 0, footprint, render);
        const int32_t b = SkyTap(texel, 1, footprint, render);
        const double lo = double(texel) * footprint - 1.0;
        const double hi = double(texel + 1) * footprint + 1.0;
        inside = inside && a >= 0 && b >= a && b <= int32_t(render) - 1 && a >= lo && b <= hi;
        // The two taps are about half a footprint apart (the sparse pattern), up to the clamp at the last texel.
        if (texel + 1 < image) {
          spread = spread && std::fabs(double(b - a) - 0.5 * footprint) <= 1.0;
        }
      }
    }
    Check(inside, "capped taps lie in the picture and in their texel's footprint");
    Check(spread, "capped taps are half a footprint apart");
    // 4K: texel x reads pixels 4x + 1 and 4x + 3 of each axis.
    Check(SkyTap(10, 0, 4.0f, 3840) == 41 && SkyTap(10, 1, 4.0f, 3840) == 43, "4K: texel 10 reads pixels 41 and 43");
    Check(SkyTap(959, 1, 4.0f, 3840) == 3839, "4K: the last texel's second tap is the last pixel");
  }

  // --- the shaders say it ----------------------------------------------------------------------------------------
  if (argc > 1) {
    const std::string dir = argv[1];
    std::string sky;
    std::string blur;
    std::string add;
    Check(ReadFile(dir + "/shafts_sky_ps.hlsl", &sky), "reads shafts_sky_ps.hlsl");
    Check(ReadFile(dir + "/shafts_blur_ps.hlsl", &blur), "reads shafts_blur_ps.hlsl");
    Check(ReadFile(dir + "/shafts_add_ps.hlsl", &add), "reads shafts_add_ps.hlsl");
    Check(Has(sky, kSkyTapHlsl), "the sky shader has the tap formula of shafts_image.h SkyTap");
    Check(Has(sky, "g_push.tint.xy"), "the sky shader reads the footprint from the push (tint.xy)");
    Check(Has(sky, "floor(position.xy)"), "the sky shader's texel is the pixel's integer position");
    // The blur works in UV: the texel, the step and the march are fractions of the image, and the radial weight is
    // in image heights, so only the aspect of the image enters.
    Check(Has(blur, "const float2 uv = position.xy / size;"), "the blur's texel is in UV");
    Check(Has(blur, "const float2 step_uv = to_sun * (length_of_way / float(steps));"), "the blur's step is in UV");
    Check(Has(blur, "const float aspect = size.x / size.y;"), "the blur's radial weight takes the aspect only");
    Check(Has(add, "const float2 uv = position.xy / g_push.sizes.xy;"), "the add pass reads the image in UV");
  }

  if (g_failed) {
    std::printf("shafts_image_test: %d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  std::printf("shafts_image_test: %d checks passed\n", g_checks);
  return 0;
}
