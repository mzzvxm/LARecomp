#pragma once

#include <cstdint>
#include <string_view>

namespace mcla::gpu::gbuffer {

enum class PauseFxView : uint8_t { kNone, kPause, kPhoto };

constexpr PauseFxView PauseFxViewOf(bool enabled, std::string_view movie, bool garage_lighting) {
  if (!enabled || garage_lighting) return PauseFxView::kNone;
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
