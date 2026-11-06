// LARecomp shader pack builder -- the HDR variant of the game's tone map (see hdr_variant.h).
// Port of native/tools/build_hdr_variants.py.

#include "shaderpack/hdr_variant.h"

#include <array>
#include <cstddef>
#include <format>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace larecomp::shaderpack {

namespace {

// ---------------------------------------------------------------------------
// The anchors. The Python uses `re`; every pattern in it is a run of literal text with `\s*` or `\s+`
// between some of the pieces, so a literal-and-whitespace matcher with the same greedy, backtracking
// rule gives the same matches (and no std::regex over 30 KB of text).
// ---------------------------------------------------------------------------

// What `\s` and str.rstrip() take as white space in this (ASCII) text: str.isspace().
bool IsSpace(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  return u == ' ' || (u >= '\t' && u <= '\r') || (u >= 0x1C && u <= 0x1F);
}

struct Piece {
  std::string_view literal;  // the text of a literal piece
  bool whitespace = false;   // a run of white space instead: at least `min` characters, as many as there are
  size_t min = 0;
};
constexpr Piece Lit(std::string_view text) { return Piece{text, false, 0}; }
constexpr Piece Ws(size_t min) { return Piece{{}, true, min}; }  // \s* is Ws(0), \s+ is Ws(1)

constexpr size_t kMaxPieces = 15;
using Bounds = std::array<size_t, kMaxPieces + 1>;  // where each piece started, and the end of the match

// Whether pieces[index..] match at `pos`. A white space run takes everything it can and gives back
// one character at a time when the rest fails (what a greedy `\s*` does: it matters when the next
// literal begins with a tab).
bool MatchFrom(std::string_view text, size_t pos, std::span<const Piece> pieces, size_t index, Bounds& bounds) {
  bounds[index] = pos;
  if (index == pieces.size()) return true;
  const Piece& piece = pieces[index];
  if (!piece.whitespace) {
    return text.substr(pos, piece.literal.size()) == piece.literal &&
           MatchFrom(text, pos + piece.literal.size(), pieces, index + 1, bounds);
  }
  size_t run = 0;
  while (pos + run < text.size() && IsSpace(text[pos + run])) ++run;
  if (run < piece.min) return false;
  for (size_t length = run;; --length) {
    if (MatchFrom(text, pos + length, pieces, index + 1, bounds)) return true;
    if (length == piece.min) return false;
  }
}

struct Match {
  size_t begin = 0;
  size_t end = 0;
  Bounds bounds{};
};

// re.findall: the matches that do not overlap, scanning left to right.
std::vector<Match> FindAll(std::string_view text, std::span<const Piece> pieces) {
  std::vector<Match> found;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t start = pos;
    if (!pieces.front().whitespace) {
      start = text.find(pieces.front().literal, pos);
      if (start == std::string_view::npos) break;
    }
    Match match;
    if (MatchFrom(text, start, pieces, 0, match.bounds)) {
      match.begin = start;
      match.end = match.bounds[pieces.size()];
      found.push_back(match);
      pos = match.end > start ? match.end : start + 1;
    } else {
      pos = start + 1;
    }
  }
  return found;
}

// regex_once(): replaces the pattern exactly once or says why not. The replacement gets the text of
// the pattern's group 1, which is pieces[group_first..group_last].
using Replacement = std::function<std::string(std::string_view group)>;

bool ReplaceOnce(std::string& text, std::span<const Piece> pieces, size_t group_first, size_t group_last,
                 std::string_view what, const Replacement& replacement, std::string* error) {
  const std::vector<Match> found = FindAll(text, pieces);
  if (found.size() != 1) {
    *error = std::format(
        "anchor '{}' found {} times (need 1): the HLSL tree is not the one this transform was written for",
        what, found.size());
    return false;
  }
  const Match& match = found.front();
  const size_t group_begin = match.bounds[group_first];
  const size_t group_end = match.bounds[group_last + 1];
  const std::string replaced = replacement(std::string_view(text).substr(group_begin, group_end - group_begin));
  text.replace(match.begin, match.end - match.begin, replaced);
  return true;
}

// str.count: the occurrences that do not overlap.
size_t CountOccurrences(std::string_view text, std::string_view needle) {
  size_t count = 0;
  for (size_t at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + needle.size())) {
    ++count;
  }
  return count;
}

// A file read with Python's open(path, "r"): "\r\n" and a lone "\r" are a "\n".
std::string NormalizeNewlines(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r') {
      out.push_back('\n');
      if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
    } else {
      out.push_back(text[i]);
    }
  }
  return out;
}

}  // namespace

HdrTransformResult TransformToneMapHlsl(std::string_view hlsl, std::string_view include_text) {
  HdrTransformResult result;
  std::string text = NormalizeNewlines(hlsl);
  std::string include = NormalizeNewlines(include_text);
  while (!include.empty() && IsSpace(include.back())) include.pop_back();  // include_text.rstrip()

  // 1. g_HdrScene in the SPIR-V shared constants (header of every generated shader).
  static constexpr Piece kSpirvDeclaration[] = {
      Lit("#define g_InvResolutionScale"), Ws(1),
      Lit("vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 592)")};
  // 2. and in the DXIL shared constants block.
  static constexpr Piece kDxilDeclaration[] = {Lit("float2 g_InvResolutionScale : packoffset(c37.x);")};
  // 3. The curve and the finish, before main.
  static constexpr Piece kMain[] = {Lit("#ifndef __spirv__"), Ws(0), Lit("[shader(\"pixel\")]"), Ws(0),
                                    Lit("#endif"),            Ws(0), Lit("void main(")};
  // 4. The game's own code: the luminance curve (Ld = Ls (1 + Ls / W) / (1 + Ls), made in r0.z from r1.x = Ls)...
  static constexpr Piece kLuminanceCurve[] = {
      Lit("\tr3.z = r1.x * r0.z;"),
      Ws(0),
      Lit("\tps = clamp(rcp(r0.x), FLT_MIN, FLT_MAX);"),
      Ws(0),
      Lit("\tr0.x = ps;"),
      Ws(0),
      Lit("\tr0.xzw = r3.xzy * r0.yxy;")};
  // 5. ... and the last assignment (the game's encode).
  static constexpr Piece kEncode[] = {Lit("\toC0.xyz = "),
                                      Lit("select(r2.xyzz == 0.0, r1.xyzz, r0.xyzz)"),
                                      Lit(";")};

  const auto fail = [&](std::string error) {
    result.ok = false;
    result.hlsl.clear();
    result.error = std::move(error);
    return result;
  };
  std::string error;
  if (!ReplaceOnce(text, kSpirvDeclaration, 0, 2, "SPIR-V g_InvResolutionScale",
                   [](std::string_view group) {
                     return std::string(group) +
                            "\n// HDR scene variant (src/gpu/constants.h HdrSceneConstants): 16 bytes after the 608.\n"
                            "#define g_HdrScene vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 608, 0x10)";
                   },
                   &error)) {
    return fail(std::move(error));
  }
  if (!ReplaceOnce(text, kDxilDeclaration, 0, 0, "DXIL g_InvResolutionScale",
                   [](std::string_view group) {
                     // the group without its closing ';', then the line continuation and the new member
                     return std::string(group.substr(0, group.size() - 1)) +
                            "; \\\n    float4 g_HdrScene : packoffset(c38);";
                   },
                   &error)) {
    return fail(std::move(error));
  }
  if (!ReplaceOnce(text, kMain, 0, 6, "pixel shader main",
                   [&](std::string_view group) { return include + "\n\n" + std::string(group); }, &error)) {
    return fail(std::move(error));
  }
  if (!ReplaceOnce(text, kLuminanceCurve, 0, 6, "Ld = Ls (1 + Ls / W) / (1 + Ls)",
                   [](std::string_view group) {
                     return std::string(group) + "\n\tr0.z = HdrToneLd(r1.x, White.x, g_HdrScene);";
                   },
                   &error)) {
    return fail(std::move(error));
  }
  if (!ReplaceOnce(text, kEncode, 1, 1, "oC0.xyz",
                   [](std::string_view group) { return "\toC0.xyz = HdrFinish(" + std::string(group) + ".xyz);"; },
                   &error)) {
    return fail(std::move(error));
  }

  for (const std::string_view needle : {std::string_view("HdrToneLd(r1.x"), std::string_view("HdrFinish(select"),
                                        std::string_view("g_HdrScene : packoffset(c38)"),
                                        std::string_view("SharedConstants + 608")}) {
    const size_t count = CountOccurrences(text, needle);
    if (count != 1) return fail(std::format("the transformed HLSL has '{}' {} times", needle, count));
  }

  result.ok = true;
  result.hlsl = std::move(text);
  return result;
}

}  // namespace larecomp::shaderpack
