#pragma once
// ===========================================================================
// LARecomp shader pack builder -- anchored text edits for the variants
// ===========================================================================
// A pixel shader variant is the translator's HLSL of a game shader with a few edits. Every edit names
// its anchor, a run of literal text with white space between some of the pieces, and the anchor must
// occur exactly once (or the number of times the edit says): when it does not, the HLSL is not the one
// the edit was written for (another game version, another translator) and the variant is not built, so
// the renderer keeps drawing the game's own shader.
//
// The matcher is the one the HDR scene's transform was ported with from native/tools/build_hdr_variants.py
// (that Python used `re`, and every pattern in it is a run of literal text with `\s*` or `\s+` between
// some of the pieces, so a literal-and-whitespace matcher with the same greedy, backtracking rule gives
// the same matches, and no std::regex runs over 30 KB of text).
// ===========================================================================

#include <array>
#include <cstddef>
#include <format>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace larecomp::shaderpack::textedit {

// What `\s` and str.rstrip() take as white space in this (ASCII) text: str.isspace().
inline bool IsSpace(char c) {
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

inline constexpr size_t kMaxPieces = 15;
using Bounds = std::array<size_t, kMaxPieces + 1>;  // where each piece started, and the end of the match

// Whether pieces[index..] match at `pos`. A white space run takes everything it can and gives back
// one character at a time when the rest fails (what a greedy `\s*` does: it matters when the next
// literal begins with a tab).
inline bool MatchFrom(std::string_view text, size_t pos, std::span<const Piece> pieces, size_t index,
                      Bounds& bounds) {
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
inline std::vector<Match> FindAll(std::string_view text, std::span<const Piece> pieces) {
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

inline bool ReplaceOnce(std::string& text, std::span<const Piece> pieces, size_t group_first, size_t group_last,
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
inline size_t CountOccurrences(std::string_view text, std::string_view needle) {
  size_t count = 0;
  for (size_t at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + needle.size())) {
    ++count;
  }
  return count;
}

// Replaces every occurrence of a literal text and says why not unless there are exactly `expected` of them.
inline bool ReplaceAllExactly(std::string& text, std::string_view needle, std::string_view replacement,
                              size_t expected, std::string_view what, std::string* error) {
  const size_t count = CountOccurrences(text, needle);
  if (count != expected) {
    *error = std::format(
        "anchor '{}' found {} times (need {}): the HLSL tree is not the one this transform was written for",
        what, count, expected);
    return false;
  }
  std::string out;
  out.reserve(text.size() + expected * replacement.size());
  size_t from = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, from)) {
    out.append(text, from, at - from);
    out.append(replacement);
    from = at + needle.size();
  }
  out.append(text, from, std::string::npos);
  text = std::move(out);
  return true;
}

// A file read with Python's open(path, "r"): "\r\n" and a lone "\r" are a "\n".
inline std::string NormalizeNewlines(std::string_view text) {
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

}  // namespace larecomp::shaderpack::textedit
