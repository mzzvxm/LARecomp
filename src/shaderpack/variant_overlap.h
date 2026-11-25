#pragma once
// ===========================================================================
// LARecomp shader pack builder -- a pixel shader that two variants made an entry for
// ===========================================================================
// Without the chain a pixel shader could have ONE variant at most; with it (variant_chain.h) a shader can have the edits of several
// stages in one text, but only for the pairs the chain's table lists as composable. A pixel shader that two variants that are NOT
// composable made an entry for would come out with two entries, each without the other's edit, and the renderer takes one of them per
// draw (ShaderStore::ChooseVariant: the most wanted features that the pack has a word for, the lowest feature among equals), the
// other never. Nothing in the built-in variants does that (the paint of the car is in the car self-reflection's alone: the shadow filter's
// transform leaves it out, it reads no shadow atlas; variants/variant_overlap_test.cpp checks that on the retail text), so the step tells
// it when it happens instead of building half of what was meant. Two stages the table composes are not an overlap: the step builds
// their composition (variant_pack.cpp ComposePass) and the renderer can take both.
//
// Pure: the variant step (variant_pack.cpp) collects (variant name, pixel shader identity) of every entry it made and asks here.
// ===========================================================================

#include <algorithm>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace larecomp::shaderpack {

struct VariantTarget {
  std::string variant;  // the variant's name ("shadow_filter")
  uint64_t ps_key = 0;  // the pixel shader's identity
};

struct VariantOverlap {
  uint64_t ps_key = 0;
  std::vector<std::string> variants;  // in the order given, each once; at least two
};

// The pixel shaders that more than one variant has an entry for, in increasing identity order.
inline std::vector<VariantOverlap> FindVariantOverlaps(std::span<const VariantTarget> targets) {
  std::map<uint64_t, std::vector<std::string>> by_shader;
  for (const VariantTarget& target : targets) {
    std::vector<std::string>& names = by_shader[target.ps_key];
    if (std::find(names.begin(), names.end(), target.variant) == names.end()) names.push_back(target.variant);
  }
  std::vector<VariantOverlap> overlaps;
  for (auto& [key, names] : by_shader) {
    if (names.size() > 1) overlaps.push_back({key, std::move(names)});
  }
  return overlaps;
}

// "0123456789ABCDEF in shadow_filter and car_selfrefl", for the log and the step's message.
inline std::string DescribeVariantOverlap(const VariantOverlap& overlap) {
  static constexpr char kDigits[] = "0123456789ABCDEF";
  std::string text(16, '0');
  for (int i = 0; i < 16; ++i) text[size_t(15 - i)] = kDigits[(overlap.ps_key >> (4 * i)) & 0xF];
  text += " in ";
  for (size_t i = 0; i < overlap.variants.size(); ++i) {
    if (i > 0) text += i + 1 == overlap.variants.size() ? " and " : ", ";
    text += overlap.variants[i];
  }
  return text;
}

}  // namespace larecomp::shaderpack
