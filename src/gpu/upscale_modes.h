#pragma once
// ===========================================================================
// MCLA GPU renderer -- the upscalers' choices and quality modes: the words, the tables, the ratios (pure rules)
// ===========================================================================
// mcla_gpu_upscaler, mcla_gpu_fsr31_mode, mcla_gpu_fsr4_mode and mcla_gpu_dlss_mode are live: the temporal system reads them at every frame
// boundary (ReadUpscalerChoice), and a change moves the path and the scene's scale between two frames (UpdatePath, SwitchPath). What this
// holds, and nothing else (no renderer, no ReXGlue, so upscale_modes_test.cpp checks it alone; temporal_backend.cpp's functions call it and
// keep the names the rest of the renderer uses):
//
//   ParseUpscaler / ParseMode   a setting's word (any case) to the upscaler or the mode; the word is also what the logs print (CvarValue)
//   kFsrModes / kDlssModes      the vendors' five modes: the word, the menu's name, the share of the output the scene is drawn at, and the
//                               ratio of output to render per axis (FSR 3.1 and FSR 4: 1, 1.5, 1.7, 2, 3; DLSS's balanced is 58%, 1.724)
//   InfoOf                      the table of an upscaler (DLSS's own, FSR's for every other)
//   Runs                        whether a mode can run with an upscaler (all five do; DLSS may refuse one at some output sizes later, by the backend)
//
// The scene's size under a mode is ChooseSceneScale (render_scale.h) of the output's scale and the mode's ratio.
// ===========================================================================

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "gpu.h"

namespace mcla::gpu::upscale_modes {

// AMD's names and ratios (FSR 3.1 and FSR 4).
inline constexpr ModeInfo kFsrModes[size_t(UpscaleMode::kCount)] = {
    {"native_aa", "NATIVE AA", 100, 1.0},
    {"quality", "QUALITY", 67, 1.5},
    {"balanced", "BALANCED", 59, 1.7},
    {"performance", "PERFORMANCE", 50, 2.0},
    {"ultra_performance", "ULTRA PERFORMANCE", 33, 3.0},
};
// NVIDIA's: DLAA, Quality 66.7%, Balanced 58%, Performance 50%, Ultra Performance 33.3% of the
// output per direction (the sizes slDLSSGetOptimalSettings gives; the backend checks the scene's
// size against the range it reports).
inline constexpr ModeInfo kDlssModes[size_t(UpscaleMode::kCount)] = {
    {"dlaa", "DLAA", 100, 1.0},
    {"quality", "QUALITY", 67, 1.5},
    {"balanced", "BALANCED", 58, 1.0 / 0.58},
    {"performance", "PERFORMANCE", 50, 2.0},
    {"ultra_performance", "ULTRA PERFORMANCE", 33, 3.0},
};

inline std::string Lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return out;
}

inline const ModeInfo& InfoOf(Upscaler upscaler, UpscaleMode mode) {
  const ModeInfo* table = upscaler == Upscaler::kDlss ? kDlssModes : kFsrModes;
  return table[size_t(mode) < size_t(UpscaleMode::kCount) ? size_t(mode) : 0];
}

// The word the setting holds for an upscaler: off, fsr31, fsr4, dlss.
inline const char* CvarValue(Upscaler upscaler) {
  switch (upscaler) {
    case Upscaler::kFsr31: return "fsr31";
    case Upscaler::kFsr4: return "fsr4";
    case Upscaler::kDlss: return "dlss";
    default: return "off";
  }
}

inline const char* Name(Upscaler upscaler) {
  switch (upscaler) {
    case Upscaler::kFsr31: return "FSR 3.1";
    case Upscaler::kFsr4: return "FSR 4";
    case Upscaler::kDlss: return "DLSS";
    default: return "OFF";
  }
}

// The setting's word as an upscaler: off (or empty), fsr31 (fsr3.1, fsr 3.1), fsr4 (fsr 4), dlss, in any case. False (and kOff in `*out`) for
// anything else.
inline bool ParseUpscaler(std::string_view text, Upscaler* out) {
  const std::string value = Lower(text);
  *out = Upscaler::kOff;
  if (value == "fsr31" || value == "fsr3.1" || value == "fsr 3.1") {
    *out = Upscaler::kFsr31;
    return true;
  }
  if (value == "fsr4" || value == "fsr 4") {
    *out = Upscaler::kFsr4;
    return true;
  }
  if (value == "dlss") {
    *out = Upscaler::kDlss;
    return true;
  }
  return value == "off" || value.empty();
}

// The setting's word as a mode (DLSS's word for the first is dlaa; FSR's is native_aa), in any case. False for anything else, and `*out` is Native AA.
inline bool ParseMode(std::string_view text, UpscaleMode* out) {
  const std::string value = Lower(text);
  *out = UpscaleMode::kNativeAa;
  if (value == "dlaa") {  // DLSS's name for the first mode
    return true;
  }
  for (size_t i = 0; i < size_t(UpscaleMode::kCount); ++i) {
    if (value == kFsrModes[i].cvar) {
      *out = UpscaleMode(i);
      return true;
    }
  }
  return false;
}

// FSR 3.1, FSR 4 and DLSS all take the five modes of their vendor (the scene's size is the start's choice: GetStartUpscale); an upscaler that is
// off takes none. DLSS can refuse a mode at some output sizes (ModeVerdicts): that is not decided here but by the backend, for the sizes the
// renderer works at.
inline bool Runs(Upscaler upscaler, UpscaleMode mode) {
  return upscaler != Upscaler::kOff && size_t(mode) < size_t(UpscaleMode::kCount);
}

}  // namespace mcla::gpu::upscale_modes
