#include "citypack/material_class.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace larecomp::citypack {

uint32_t RageHashText(std::string_view text) {
  uint32_t h = 0;
  for (const char raw : text) {
    uint32_t ch = static_cast<uint8_t>(raw);
    if (ch >= 'A' && ch <= 'Z') ch += 32;
    if (ch == '\\') ch = '/';
    h += ch;
    h += h << 10;
    h ^= h >> 6;
  }
  h += h << 3;
  h ^= h >> 11;
  h += h << 15;
  return h;
}

uint32_t ShaderHash(std::string_view group, std::string_view preset) {
  std::string text(group);
  text += '/';
  text += preset;
  return RageHashText(text);
}

namespace {

std::string Lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

bool Has(const std::string& text, std::string_view word) { return text.find(word) != std::string::npos; }

}  // namespace

MaterialClass ClassifyShader(std::string_view group_in, std::string_view preset_in) {
  const std::string group = Lower(group_in);
  const std::string preset = Lower(preset_in);
  const auto any = [&](std::string_view word) { return Has(group, word) || Has(preset, word); };

  // A layer over a surface (road markings, stains, grime on a wall: CityGrimeDecal, CityGroundGrimeDecal, CityDecal, TintBumpDecal2): it lies a
  // few millimetres off its surface, so as a caster it would shadow the surface it belongs to.
  if (any("decal")) return MaterialClass::kDecal;
  // Dirt over a window (CityWindowDirt) is a layer over the glass, like a decal.
  if (any("windowdirt")) return MaterialClass::kDecal;
  // Store interiors (CityStoreInterior) and the interior variants of the materials (presets ending in _int: CityGrime_UnpaintedWall_int, ...): the rooms behind
  // the windows and the garage, inside the walls, where no sun ray comes from outside.
  if (any("interior") || (preset.size() > 4 && preset.compare(preset.size() - 4, 4, "_int") == 0)) return MaterialClass::kInterior;
  if (any("water") || any("pond") || any("pool")) return MaterialClass::kWater;
  // A shader that says it is cut out or blended (the preset CityNeonAlpha_Metal) is not an opaque caster; foliage and fences, if a sector has any,
  // come under the same words.
  // (No short words like "tree": "street" has it.)
  if (Has(preset, "alpha") || any("foliage") || any("fence") || any("chainlink")) return MaterialClass::kAlphaTested;
  if (any("window") || any("glass")) return MaterialClass::kGlass;
  if (any("neon")) return MaterialClass::kEmissive;
  if (any("billboard")) return MaterialClass::kBillboard;
  if (any("road") || any("terrain") || any("ground") || any("sidewalk") || any("asphalt")) return MaterialClass::kGround;
  return MaterialClass::kSolid;
}

}  // namespace larecomp::citypack
