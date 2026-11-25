#pragma once

#include <cctype>
#include <string>

namespace mcla::gpu::dlss {

// GeForce RTX 40 and 50 series and their workstation equivalents. Streamline's
// runtime query is the final word (a Quadro RTX 4000, for example, is Turing).
inline bool NameLooksLikeFrameGeneration(const std::string& name) {
  std::string upper;
  for (char c : name) {
    upper += char(std::toupper(static_cast<unsigned char>(c)));
  }
  if (upper.find("BLACKWELL") != std::string::npos || upper.find(" ADA") != std::string::npos) {
    return true;
  }
  const size_t rtx = upper.find("RTX");
  if (rtx == std::string::npos) {
    return false;
  }
  size_t at = rtx + 3;
  while (at < upper.size() && upper[at] == ' ') {
    ++at;
  }
  return at < upper.size() && (upper[at] == '4' || upper[at] == '5');
}

}  // namespace mcla::gpu::dlss
