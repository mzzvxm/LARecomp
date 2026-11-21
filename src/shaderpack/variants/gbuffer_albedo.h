#pragma once
// ===========================================================================
// LARecomp shader pack builder -- the G-buffer stage's RT2 writer: the albedo and the lit weight (package GB-W1 of perf/LIGHT-UNIFY.md 11.2)
// ===========================================================================
// src/gpu/gbuffer_types.h: RT2 is R8G8B8A8_UNORM, rgb = sqrt(albedo) (gamma 2: dark albedo keeps its steps), a = the lit weight (255: lit by the forward lights, 0: emissive or unlit,
// the deferred lights add nothing there). Both stages of the G-buffer variant (the night's gbuffer_night.cpp and the day's gbuffer_day.cpp) write it with the code of this file, so
// the two cannot drift apart. Header only (the day test builds without gbuffer.cpp).
//
// THE ALBEDO is the shader's own diffuse texel, before any lighting: the `DiffuseSampler` fetch the translator writes as
//     r6.xyzw = tfetch2D(DiffuseSampler_Texture2DDescriptorIndex, DiffuseSampler_SamplerDescriptorIndex, r2.xy, float2(0, 0)).xwyz;
// (`dest.lanes = tex.swizzle`: lane k of the destination takes the texel's channel swizzle[k]; here r6.x = tex.x, r6.y = tex.w, r6.z = tex.y, r6.w = tex.z). The albedo is the three lanes of
// the destination that hold the texel's x, y and z (`r6.xzw`). The vertex colour is not a diffuse term (its alpha is the baked ambient occlusion, its rgb joins the vertex lit term), so
// it is not in it. THE ANCHOR is the fetch that has the offset (0, 0) (the garage floor and the decals fetch the same texture four more times at half a texel's offset for their height
// slope, one channel each: not these) and yields all of x, y and z (a two lane fetch is an opacity mask, not a colour), at the top level of main (a fetch inside a branch, as the store
// interiors' two paths have, has no value on the other path). Where it is not found (no DiffuseSampler: the shaders of the paint, the traffic, the lamp lenses...) the plane gets the
// "no data" code (all zero): a consumer that wants an albedo there falls back (the SSGI's estimate, a deferred light that adds nothing). RT2 = (0, 0, 0, 0) is therefore "no albedo",
// and a covered pixel with an albedo has a > 0 unless it is emissive (then rgb > 0 and a = 0).
//
// WHERE IT IS READ. The register the fetch wrote can be overwritten before the end of main (the translator reuses registers), so the stage follows the rule the night's normal does: when none of
// the three lanes is written by any later statement (at any nesting depth) the write at the end of main reads the register itself (`r6.xzw`: no live range is added), else one line right
// after the fetch captures them (`float3 gbAlbedo = r6.xzw;`: the value the texture gave, which is what the game multiplies the light with) and the write reads that.
//
// THE LIT WEIGHT is the draw's, not the pixel's: bits 16 to 23 of the material word (GBufferConstants::material, which the host fills per draw: g_GBufferMode.y; gbuffer_types.h
// MaterialWord, hlsl/gbuffer_common.hlsli GbMaterialLit). The word's default is lit = 1; the class table by group name of GB-F1 (gbuffer_material.*) sets it 0 for the emissive groups (neon,
// signs, lamp glass, the traffic signal), the per texture table for lit windows. No table is kept here a second time: the shader writes what the host says.
//
// All of it only ADDS lines: the output, the captured copy, the write after `oC0 = applyBlendPremult(oC0);`. The colour is the game's (the stages' tests check that the text, minus the
// added lines, is the game's, line by line).
// ===========================================================================

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "shaderpack/variants/text_edit.h"

namespace larecomp::shaderpack {

// The third colour output, after the second (the normal): SV_Target2 is RT2 (gbuffer_rules.h kStagePlanes: the stage declares as many outputs as it has planes).
inline constexpr std::string_view kGBufferAlbedoOutput = "\n\t, out float4 oC2 : SV_Target2";
// What every shader of the stage writes to RT2 whatever else: the "no data" code. A draw that does not write the albedo (the mode bit clear) and a shader without an anchor end here.
inline constexpr std::string_view kGBufferAlbedoNoData = "\n\toC2 = float4(0.0f, 0.0f, 0.0f, 0.0f);";
// The name of the captured copy.
inline constexpr std::string_view kGBufferAlbedoCapture = "gbAlbedo";

// The reasons a shader has no albedo (also what the tests look for).
inline constexpr std::string_view kGBufferAlbedoSkipNoFetch = "no DiffuseSampler fetch of the texel's x, y and z at its centre";
inline constexpr std::string_view kGBufferAlbedoSkipNested = "its DiffuseSampler fetch is inside a branch";

struct GBufferAlbedo {
  bool found = false;
  std::string why_not;                    // when !found: one of the kGBufferAlbedoSkip* reasons
  std::string expression;                 // what GbAlbedoEncode takes: `r6.xzw` (read at the end of main) or `gbAlbedo` (captured)
  std::string lanes;                      // the fetch's destination lanes that hold the texel's x, y, z: `r6.xzw`
  bool captured = false;                  // a later statement writes one of the lanes: the copy is made right after the fetch
  size_t capture_at = std::string::npos;  // the offset (in the text with LF line ends) of the line the capture goes before: the line after the fetch; npos: none
  std::string capture;                    // the line to insert there, indentation and line end included
  int line = -1;                          // the fetch's line, counted from `void main(` (0): for the census
  std::string detail;                     // the census' words
};

namespace albedo_detail {

struct Line {
  size_t begin = 0;       // offset of the line's first character in the text
  size_t indent = 0;      // tabs and spaces before the code
  std::string_view code;  // the line without its indentation and trailing white space
};

inline std::vector<Line> LinesFrom(const std::string& text, size_t from) {
  std::vector<Line> lines;
  size_t at = from;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    Line line;
    line.begin = at;
    size_t first = at;
    while (first < end && (text[first] == '\t' || text[first] == ' ')) ++first;
    size_t last = end;
    while (last > first && textedit::IsSpace(text[last - 1])) --last;
    line.indent = first - at;
    line.code = std::string_view(text).substr(first, last - first);
    lines.push_back(line);
    at = end + 1;
  }
  return lines;
}

inline bool IsWordChar(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

inline bool AllLanes(std::string_view text) {
  if (text.empty() || text.size() > 4) return false;
  for (const char c : text) {
    if (std::string_view("xyzw").find(c) == std::string_view::npos) return false;
  }
  return true;
}

inline bool IsRegisterName(std::string_view name) {
  if (name.size() < 2 || name[0] != 'r') return false;
  for (size_t i = 1; i < name.size(); ++i) {
    if (name[i] < '0' || name[i] > '9') return false;
  }
  return true;
}

// Whether a statement of `code` writes one of `lanes` of the register `reg`: the register at a word boundary, its lanes (a bare register is all four), then an assignment
// operator (`=` but not `==`, or `+=`, `-=`, `*=`, `/=`). Conservative on purpose: any occurrence counts, at any nesting depth, and a line that has several statements is looked at all through.
inline bool WritesLanes(std::string_view code, std::string_view reg, std::string_view lanes) {
  for (size_t at = code.find(reg); at != std::string_view::npos; at = code.find(reg, at + 1)) {
    if (at > 0 && (IsWordChar(code[at - 1]) || code[at - 1] == '.')) continue;
    size_t end = at + reg.size();
    if (end < code.size() && IsWordChar(code[end])) continue;  // r6 inside r60
    std::string_view used = "xyzw";
    if (end < code.size() && code[end] == '.') {
      size_t lane_end = end + 1;
      while (lane_end < code.size() && std::string_view("xyzw").find(code[lane_end]) != std::string_view::npos) ++lane_end;
      used = code.substr(end + 1, lane_end - end - 1);
      end = lane_end;
    }
    while (end < code.size() && (code[end] == ' ' || code[end] == '\t')) ++end;
    if (end >= code.size()) continue;
    bool assigns = false;
    if (code[end] == '=') {
      assigns = !(end + 1 < code.size() && code[end + 1] == '=');
    } else if (std::string_view("+-*/").find(code[end]) != std::string_view::npos && end + 1 < code.size() && code[end + 1] == '=') {
      assigns = !(end + 2 < code.size() && code[end + 2] == '=');
    }
    if (!assigns) continue;
    for (const char lane : lanes) {
      if (used.find(lane) != std::string_view::npos) return true;
    }
  }
  return false;
}

}  // namespace albedo_detail

// The albedo of one shader's text (pure; public for the stages, the tests and the census). `hlsl` may have any line ends; offsets are into the text with LF line ends.
inline GBufferAlbedo AnalyseGBufferAlbedo(std::string_view hlsl) {
  using namespace albedo_detail;
  GBufferAlbedo out;
  const std::string text = textedit::NormalizeNewlines(hlsl);
  const size_t main_at = text.rfind("\nvoid main(");
  out.why_not = std::string(kGBufferAlbedoSkipNoFetch);
  if (main_at == std::string::npos) return out;
  const std::vector<Line> lines = LinesFrom(text, main_at + 1);

  static constexpr std::string_view kFetch = " = tfetch2D(DiffuseSampler_Texture2DDescriptorIndex, DiffuseSampler_SamplerDescriptorIndex, ";
  static constexpr std::string_view kCentre = ", float2(0, 0)).";
  bool nested = false;
  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string_view code = lines[i].code;
    const size_t at = code.find(kFetch);
    if (at == std::string_view::npos || !code.ends_with(";")) continue;
    // `r6.xyzw` (or a bare `r6`) is the destination: a register of the game's and nothing else
    const std::string_view dest = code.substr(0, at);
    const size_t dot = dest.find('.');
    const std::string_view reg = dot == std::string_view::npos ? dest : dest.substr(0, dot);
    const std::string_view lanes = dot == std::string_view::npos ? std::string_view("xyzw") : dest.substr(dot + 1);
    if (!IsRegisterName(reg) || !AllLanes(lanes)) continue;
    // `... float2(0, 0)).xwyz;`: the texel's centre, and the channels the lanes take
    const std::string_view tail = code.substr(at + kFetch.size());
    const size_t centre = tail.find(kCentre);
    if (centre == std::string_view::npos) continue;
    std::string_view swizzle = tail.substr(centre + kCentre.size());
    swizzle.remove_suffix(1);  // the `;`
    if (!AllLanes(swizzle) || swizzle.size() != lanes.size()) continue;
    // the lanes that hold the texel's x, y and z
    char rgb[3] = {};
    bool all = true;
    for (size_t c = 0; c < 3; ++c) {
      const size_t k = swizzle.find("xyz"[c]);
      if (k == std::string_view::npos) {
        all = false;
        break;
      }
      rgb[c] = lanes[k];
    }
    if (!all) continue;
    if (lines[i].indent != 1) {
      nested = true;
      continue;
    }
    if (i + 1 >= lines.size()) continue;
    out.found = true;
    out.line = int(i);
    out.lanes = std::string(reg) + "." + rgb[0] + rgb[1] + rgb[2];
    const std::string_view written(rgb, 3);
    for (size_t j = i + 1; j < lines.size() && !out.captured; ++j) out.captured = WritesLanes(lines[j].code, reg, written);
    if (out.captured) {
      out.expression = std::string(kGBufferAlbedoCapture);
      out.capture_at = lines[i + 1].begin;
      out.capture = text.substr(lines[i].begin, lines[i].indent) + "float3 " + std::string(kGBufferAlbedoCapture) + " = " + out.lanes + ";\n";
    } else {
      out.expression = out.lanes;
    }
    out.why_not.clear();
    out.detail = std::string(dest) + " = tex." + std::string(swizzle) + " -> " + out.lanes + (out.captured ? ", captured (a later statement writes it)" : ", read at the end of main");
    return out;
  }
  if (nested) out.why_not = std::string(kGBufferAlbedoSkipNested);
  return out;
}

// What the stage writes to RT2 after the game's colour statement (`oC0 = applyBlendPremult(oC0);`): the "no data" code, and, when the shader has an albedo and the host's mode bit is set, the
// albedo and the lit weight of the draw's material word.
inline std::string GBufferAlbedoWriteText(const GBufferAlbedo& albedo) {
  std::string text(kGBufferAlbedoNoData);
  if (albedo.found) {
    text += "\n\tif ((g_GBufferMode.x & uint(GB_MODE_WRITE_ALBEDO)) != 0u) {\n\t\toC2 = float4(GbAlbedoEncode(" + albedo.expression + "), GbMaterialLit(g_GBufferMode.y));\n\t}";
  }
  return text;
}

}  // namespace larecomp::shaderpack
