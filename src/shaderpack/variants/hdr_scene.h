#pragma once
// ===========================================================================
// LARecomp shader pack builder -- variant "hdr_scene": the HDR variant of the game's tone map
// ===========================================================================
// HDR stage 3 draws the game's tone map pixel shader a second time, into a float image, with a
// variant whose luminance curve continues above the game's white and whose encode is written
// unclamped. The variant is the HLSL the translator makes of the shader (xrage_postfx__PSToneMap) with
// a few anchored text edits, so its sampling, constants, grade and encode are the game's own.
//
// This is a port of native/tools/build_hdr_variants.py (the pack builder of the HDR work, branch
// feat/hdr-scene), moved into the variant pack list with no change to the edits. The edits, in this
// order (each anchor must occur exactly once or the variant is not built: the HLSL is not the one the
// transform was written for):
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
//
// The entry carries the tone map's identity and the identities of the two post effect vertex shaders it
// may be drawn with (the runtime requires one of them: src/gpu/hdr_tonemap.cpp). The curve itself is
// src/gpu/hlsl/hdr_tonemap_shoulder.hlsli of the HDR feature; it is not part of this code. CMake
// embeds it into the library when the file exists (src/shaderpack/CMakeLists.txt, cache variable
// LARECOMP_HDR_HLSLI), so the pack always matches the curve of the build that wrote it and the stamp
// carries its hash (variants_hdr_scene_input). A build without the file has no HDR variant and the
// renderer stays at HDR stage 2 (which it allows).
// ===========================================================================

#include <string>
#include <string_view>
#include <vector>

#include "shaderpack/variant_pack.h"

namespace larecomp::shaderpack {

// Hand bumped: the edits or the checks of this variant (its stamp line variants_hdr_scene_version).
inline constexpr uint32_t kHdrSceneVariantVersion = 1;

// The variant for a build that has the curve embedded; false with *why when it has none.
bool MakeHdrSceneVariant(VariantDef* out, std::string* why);

// The variant with the curve given as text instead of the embedded one (tests, a tool option).
VariantDef MakeHdrSceneVariantFromText(std::string_view hlsli_text);

// The text transform alone (pure; public for tests). `hlsl` is the tone map's translated HLSL,
// `include_text` the curve's file. Both are read the way the Python reads them (open(..., "r"): CRLF and
// lone CR become LF), so the variant is the same text whatever line endings the translator's include
// carries.
VariantTransformResult TransformToneMapHlsl(std::string_view hlsl, std::string_view include_text);

}  // namespace larecomp::shaderpack
