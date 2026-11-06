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
//
// The result is compiled for spec variant 0 only (the draw that qualifies has no alpha test) with
// the pack's usual dxc arguments, and written to mcla_shaders_hdr.v1.pack (MSHV v1; the constants
// are pack_format.h's, the reader is src/gpu/shaders.cpp LoadHdrVariants). One entry: the tone map's
// identity, the identities of the two post effect vertex shaders it may be drawn with, and its DXIL
// and SPIR-V blobs (a format the main pack was not built with has a zero blob record).
//
// The curve itself is src/gpu/hlsl/hdr_tonemap_shoulder.hlsli of the HDR feature (branch
// feat/hdr-scene); it is not part of this code. CMake embeds it into the library when the file
// exists (src/shaderpack/CMakeLists.txt, cache variable LARECOMP_HDR_HLSLI), so the pack always
// matches the curve of the build that wrote it and the stamp carries its hash. A build without the
// file has no step: CreateHdrStep() returns null, the builder writes no HDR pack and no hdr_* stamp
// lines, and the renderer stays at HDR stage 2 (which it allows).
//
// A step that fails fails the HDR pack only, never the main build (builder.h).
// ===========================================================================

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "shaderpack/builder.h"

namespace larecomp::shaderpack {

// Hand bumped: the edits, the checks or the layout of this step. It is one of the stamp's hdr_*
// lines, so changing it rebuilds the HDR pack alone.
inline constexpr uint32_t kHdrStepVersion = 1;

// The step for a build that has the curve embedded; null when it has none (HdrStepUnavailableReason()
// says why, for a log line).
std::unique_ptr<HdrStep> CreateHdrStep();
std::string HdrStepUnavailableReason();  // empty when CreateHdrStep() returns a step

// The step with the curve given as text instead of the embedded one (tests, a tool option).
std::unique_ptr<HdrStep> CreateHdrStepFromText(std::string_view hlsli_text);

// ---------------------------------------------------------------------------
// The text transform alone (pure; public for tests)
// ---------------------------------------------------------------------------

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
