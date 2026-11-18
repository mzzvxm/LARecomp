#pragma once
//
// What frame_step_cap accepts: "adaptive" or a number of milliseconds. Shared by the setting's validator
// (settings/validated_setting.h, larecomp_app_settings.h) and by its reader (mc_engine/hooks/frame_timing.cpp), so the
// two can never disagree about what a valid text is. A number is not clamped here: the reader keeps it between 16 ms
// and 1 s (kStepCapMinFixedMs / kStepCapMaxFixedMs).
//
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>

namespace larecomp::settings {

// A number of milliseconds as written in frame_step_cap or MCLA_MAX_FRAME_MS: plain digits, a positive finite number,
// blanks around it allowed.
inline bool ParseStepCapMs(std::string_view text, double& ms) {
  const std::string copy(text);
  char* end = nullptr;
  const double v = std::strtod(copy.c_str(), &end);
  if (end == copy.c_str()) return false;
  while (*end == ' ' || *end == '\t') ++end;
  if (*end != '\0') return false;
  if (!std::isfinite(v) || !(v > 0.0)) return false;
  ms = v;
  return true;
}

inline bool IsAdaptiveStepCap(std::string_view text) {
  constexpr std::string_view kWord = "adaptive";
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  if (text.size() != kWord.size()) return false;
  for (size_t i = 0; i < kWord.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(text[i])) != kWord[i]) return false;
  }
  return true;
}

// The old .validator() of frame_step_cap.
inline bool IsValidStepCap(std::string_view text) {
  double ms = 0.0;
  return IsAdaptiveStepCap(text) || ParseStepCapMs(text, ms);
}

}  // namespace larecomp::settings
