#pragma once

#include <cstdint>
#include <string_view>

namespace mcla::gpu::gbuffer {

enum class PauseFxView : uint8_t { kNone, kPause, kPhoto, kGarage };

constexpr PauseFxView PauseFxViewOf(bool city_enabled, std::string_view movie, bool garage_lighting, bool garage_enabled = false) {
  if (garage_lighting) {
    return garage_enabled && movie != "NAVSYSMOVIE" ? PauseFxView::kGarage : PauseFxView::kNone;
  }
  if (!city_enabled) return PauseFxView::kNone;
  if (movie == "PHOTOALBUMMOVIE") return PauseFxView::kPhoto;
  if (movie == "PAUSEMOVIE") return PauseFxView::kPause;
  return PauseFxView::kNone;
}

constexpr bool IsPauseFx(PauseFxView view) { return view != PauseFxView::kNone; }

constexpr bool MatchesPauseFxView(bool pause, PauseFxView view) {
  return pause == IsPauseFx(view);
}

constexpr bool PauseFxCameraChanged(bool previous_pause, PauseFxView previous, PauseFxView current) {
  return previous_pause && previous != current;
}

}  // namespace mcla::gpu::gbuffer
