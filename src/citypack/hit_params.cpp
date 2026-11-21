#include "citypack/hit_params.h"

#include <cctype>

namespace larecomp::citypack {

namespace {
constexpr uint32_t kMaxParams = 64;
constexpr uint32_t kTextureNameOffset = 24;
}  // namespace

bool ReadShaderParams(RscCursor& c, uint32_t shader, ShaderParams* out) {
  out->params.clear();
  if (!c.IsPointer(shader)) return false;
  const uint32_t values = c.U32(shader + 16);
  const uint32_t count = c.U16(shader + 24);
  const uint32_t types = c.U32(shader + 28);
  const uint32_t hashes = c.U32(shader + 36);
  if (c.bad() || !c.IsPointer(values) || !c.IsPointer(types) || !c.IsPointer(hashes) || count > kMaxParams) {
    c.ClearBad();
    return false;
  }
  out->params.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    ShaderParam p;
    p.hash = c.U32(hashes + 4 * i);
    p.type = c.U8(types + i);
    const uint32_t value = c.U32(values + 4 * i);
    if (c.bad()) {
      c.ClearBad();
      p.empty = true;
      out->params.push_back(std::move(p));
      continue;
    }
    if (!c.IsPointer(value)) {
      p.empty = true;
    } else if (p.type == kParamTypeTexture) {
      const uint32_t name = c.U32(value + kTextureNameOffset);
      if (!c.bad() && c.IsPointer(name)) p.texture = c.CString(name, 128);
      if (c.bad() || p.texture.empty()) p.empty = true;
    } else {
      for (uint32_t k = 0; k < 4; ++k) p.value[k] = c.F32(value + 4 * k);
      if (c.bad()) p.empty = true;
    }
    c.ClearBad();
    out->params.push_back(std::move(p));
  }
  return true;
}

std::string TextureStem(std::string_view file_name) {
  std::string out(file_name);
  for (char& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  const size_t dot = out.rfind(".dds");
  if (dot != std::string::npos && dot + 4 == out.size()) out.erase(dot);
  return out;
}

}  // namespace larecomp::citypack
