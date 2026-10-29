#include "mcla_imgui_theme.h"

#include <cfloat>
#include <cmath>
#include <unordered_map>
#include <vector>

// A private copy of the stb_truetype ImGui ships (all functions static), for
// the one thing ImGui's text drawing leaves out: the faces' kerning pairs.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <imstb_truetype.h>

namespace mcla::ui {

namespace {

struct Kerning {
  std::vector<unsigned char> data;  // stbtt_fontinfo points into this
  stbtt_fontinfo info{};
};

std::unordered_map<const ImFont*, Kerning>& Registry() {
  static std::unordered_map<const ImFont*, Kerning> registry;
  return registry;
}

}  // namespace

void RegisterKerning(const ImFont* font, std::vector<unsigned char> ttf) {
  if (!font || ttf.empty()) return;
  Kerning& k = Registry()[font];
  k.data = std::move(ttf);
  if (!stbtt_InitFont(&k.info, k.data.data(), stbtt_GetFontOffsetForIndex(k.data.data(), 0))) {
    Registry().erase(font);
  }
}

float ItalicSlant(const ImFont* font) {
  const auto it = Registry().find(font);
  if (it == Registry().end()) return 0.0f;
  const stbtt_fontinfo& info = it->second.info;
  // post.italicAngle: a 16.16 fixed-point angle, negative for a right lean.
  const stbtt_uint32 post = stbtt__find_table(info.data, info.fontstart, "post");
  if (!post) return 0.0f;
  const float angle = static_cast<float>(ttSHORT(info.data + post + 4)) +
                      static_cast<float>(ttUSHORT(info.data + post + 6)) / 65536.0f;
  return std::tan(-angle * 3.14159265f / 180.0f);
}

std::vector<float> KernedOrigins(ImFont* font, float px, const char* text, float* width) {
  std::vector<float> origins;
  const auto it = Registry().find(font);
  // ImGui sizes a face by ascent - descent, which is ScaleForPixelHeight.
  const float scale = it != Registry().end() ? stbtt_ScaleForPixelHeight(&it->second.info, px) : 0.0f;
  float x = 0.0f;
  for (const char* c = text; *c; ++c) {
    origins.push_back(x);
    x += font->CalcTextSizeA(px, FLT_MAX, 0.0f, c, c + 1).x;
    if (scale != 0.0f && c[1]) {
      x += scale * static_cast<float>(stbtt_GetCodepointKernAdvance(
                       &it->second.info, static_cast<unsigned char>(c[0]),
                       static_cast<unsigned char>(c[1])));
    }
  }
  if (width) *width = x;
  return origins;
}

void DrawKerned(ImDrawList* draw, ImFont* font, float px, ImVec2 pos, const char* text,
                ImU32 (*color)(size_t index)) {
  const std::vector<float> origins = KernedOrigins(font, px, text, nullptr);
  for (size_t i = 0; text[i]; ++i) {
    draw->AddText(font, px, ImVec2(pos.x + origins[i], pos.y), color(i), text + i, text + i + 1);
  }
}

} // namespace mcla::ui
