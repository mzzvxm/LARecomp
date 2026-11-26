#pragma once
// ===========================================================================
// MCLA GPU renderer -- sun shafts: the size of the two working images and the sky pass's taps
// ===========================================================================
// The sky image and the blurred image (shafts.h) are R16_FLOAT and used to be half the picture's size, so at 4K
// they were 1920x1080 and the radial blur (hlsl/shafts_blur_ps.hlsl: 32 or 64 bilinear taps a texel) read
// 2.07 M texels x 64 taps. The shafts are the smoothest picture of the frame (a ray's light is the sky's
// brightness averaged along 90% of the way to the sun, then stretched over the picture bilinearly), so
// they gain nothing from more texels than a 1080p picture has: the images are CAPPED.
//
// THE SIZE RULE (ImageSize, mcla_gpu_sun_shafts_cap on):
//   today's size   = half the picture, rounded up, per axis (HalfSize).
//   no cap         when it is at most 960 wide or at most 540 high: nothing above 1080p-like sizes changes
//                  (1280x720 gives 640x360, 1920x1080 gives 960x540, 1920x1200 gives 960x600).
//   cap            else the aspect is kept and the image is the SMALLEST one that still covers the 960x540 box
//                  (the larger of the two axes' ratios binds, the other axis is rounded UP): 4K and 1440p
//                  give 960x540, a 21:9 3440x1440 gives 1290x540, a 32:9 5120x1440 gives 1920x540. So the
//                  image is never smaller than 960x540 in an axis in which today's is not, never larger
//                  than today's, and always the same shape as today's.
// With the cap off (the debug cvar mcla_gpu_sun_shafts_cap = false) the images are today's.
//
// THE REST OF THE PASSES DO NOT DEPEND ON THE SIZE
//   blur   marches in UV: the step is to_sun * (length / steps) with the sun and the texel in 0..1 of the image,
//          the decay is per step and the radial weight is in image heights (so aspect only), so the rays are
//          the same length and shape on screen at any image size; only the ray's start jitter (interleaved
//          gradient noise, per texel) is as coarse as the texels.
//   add    reads the blurred image bilinearly at the picture's pixel / the picture's size (UV again).
//   sky    takes four taps a texel (hlsl/shafts_sky_ps.hlsl): at the old half size the 2x2 pixels the texel
//          covers, at the cap the same four taps SPREAD over the texel's footprint (a quarter the texels
//          at 4K, so a quarter the sky pass's reads too). SkyTap says where they are: the footprint is 2
//          pixels when the images are today's (the taps are then today's 2x2 block, bit for bit) and
//          render / image when they are capped (hlsl: g_push.tint.xy, set by shafts.cpp).
//
// Pure rules, no renderer: shafts_image_test.cpp builds on its own.
// ===========================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mcla::gpu::shafts_image {

// The box the cap covers: the size of today's images for a 1920x1080 picture.
inline constexpr uint32_t kCapWidth = 960;
inline constexpr uint32_t kCapHeight = 540;

struct Size {
  uint32_t width = 0;
  uint32_t height = 0;
  constexpr bool operator==(const Size& other) const = default;
};

// Today's images: half the picture, rounded up.
constexpr Size HalfSize(uint32_t render_width, uint32_t render_height) {
  return {(render_width + 1) / 2, (render_height + 1) / 2};
}

// The images' size for a picture of render_width x render_height host pixels (the rule above).
constexpr Size ImageSize(uint32_t render_width, uint32_t render_height, bool cap) {
  const Size half = HalfSize(render_width, render_height);
  if (!cap || half.width <= kCapWidth || half.height <= kCapHeight) {
    return half;
  }
  // Both axes are above the box. The axis that needs the smaller reduction (the larger of
  // kCapWidth / half.width and kCapHeight / half.height) is the box's edge; the other is rounded up.
  if (uint64_t(kCapWidth) * half.height >= uint64_t(kCapHeight) * half.width) {
    return {kCapWidth, uint32_t((uint64_t(half.height) * kCapWidth + half.width - 1) / half.width)};
  }
  return {uint32_t((uint64_t(half.width) * kCapHeight + half.height - 1) / half.height), kCapHeight};
}

// The iterated blur and its sky mask are half the old working resolution per axis.
constexpr Size IteratedSize(uint32_t render_width, uint32_t render_height, bool cap) {
  const Size half = ImageSize(render_width, render_height, cap);
  return HalfSize(half.width, half.height);
}

// How many picture pixels one texel of the sky image spans along an axis: exactly 2 for today's images (the
// old pass), render / image when the cap made them smaller.
constexpr float Footprint(uint32_t render, uint32_t image) {
  return image == (render + 1) / 2 ? 2.0f : float(render) / float(image);
}

// The pixel of the picture that tap k (0 or 1) of an axis reads for texel `texel` (the sky pass's formula, in
// the shader's single precision): the quarter and the three quarters of the texel's footprint, clamped to the
// picture's last pixel. With footprint 2 that is pixels 2 * texel and 2 * texel + 1, the old 2x2 block.
inline int32_t SkyTap(uint32_t texel, uint32_t k, float footprint, uint32_t render) {
  const float at = (float(texel) + (0.25f + 0.5f * float(k))) * footprint;
  return std::min(int32_t(std::floor(at)), int32_t(render) - 1);
}

// The text of that formula in hlsl/shafts_sky_ps.hlsl (the test looks for it there).
inline constexpr const char* kSkyTapHlsl = "floor((texel + float2(0.25f + 0.5f * float(i), 0.25f + 0.5f * float(j))) * footprint)";

}  // namespace mcla::gpu::shafts_image
