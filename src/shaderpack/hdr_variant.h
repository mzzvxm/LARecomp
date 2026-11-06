#pragma once
// ===========================================================================
// LARecomp shader pack builder -- the HDR variant of the game's tone map
// ===========================================================================
// HDR stage 3 draws the game's tone map pixel shader a second time, into a float image, with a
// variant whose luminance curve continues above the game's white and whose encode is written
// unclamped. The variant is not written by hand: it is the HLSL the translator makes of the
// shader (xrage_postfx__PSToneMap) with a few anchored text edits, so its sampling, constants,
// grade and encode are the game's own.
//
// This is a port of native/tools/build_hdr_variants.py (the pack builder of the HDR work, branch
// feat/hdr-scene). The edits, in this order (each anchor must occur exactly once or the step
// fails: the HLSL is not the one the transform was written for):
//
//   1. SPIR-V:  #define g_HdrScene vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 608, 0x10)
//               after the g_InvResolutionScale define (16 bytes after the 608 of contract v7/v8,
//               src/gpu/constants.h HdrSceneConstants)
//   2. DXIL:    float4 g_HdrScene : packoffset(c38);   in DEFINE_SHARED_CONSTANTS()
//   3. src/gpu/hlsl/hdr_tonemap_shoulder.hlsli (HdrToneLd, HdrFinish) is pasted before `void main(`
//   4. after the line that makes Ld (Ls (1 + Ls / W) / (1 + Ls), r0.z):
//                r0.z = HdrToneLd(r1.x, White.x, g_HdrScene);
//   5. the last assignment:
//                oC0.xyz = HdrFinish(<the game's encode>.xyz);
// ===========================================================================

#include <string>
#include <string_view>

namespace larecomp::shaderpack {

struct HdrTransformResult {
  bool ok = false;
  std::string hlsl;   // the variant's HLSL
  std::string error;  // plain English when !ok: names the anchor and how often it was found
};

// `hlsl` is the tone map's translated HLSL, `include_text` the curve's file. Both are read the way
// the Python reads them (open(..., "r"): CRLF and lone CR become LF), so the variant is the same
// text whatever line endings the translator's include carries.
HdrTransformResult TransformToneMapHlsl(std::string_view hlsl, std::string_view include_text);

}  // namespace larecomp::shaderpack
