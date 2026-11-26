#pragma once

#include <cstdint>
#include <string_view>

namespace mcla::gpu::gbuffer {

enum class PauseFxView : uint8_t { kNone, kPause, kPhoto, kGarage };
enum class MenuState : uint8_t { kNone, kPause, kPhoto, kMap };

constexpr MenuState MenuStateOf(std::string_view movie, bool paused, bool photo) {
  if (movie == "NAVSYSMOVIE") return MenuState::kMap;
  if (photo) return MenuState::kPhoto;
  if (paused || movie == "PAUSEMOVIE") return MenuState::kPause;
  return MenuState::kNone;
}

constexpr PauseFxView PauseFxViewOf(bool city_enabled, MenuState menu, bool garage_lighting, bool garage_enabled = false) {
  if (menu == MenuState::kMap) return PauseFxView::kNone;
  if (garage_lighting) {
    return garage_enabled ? PauseFxView::kGarage : PauseFxView::kNone;
  }
  if (!city_enabled) return PauseFxView::kNone;
  if (menu == MenuState::kPhoto) return PauseFxView::kPhoto;
  if (menu == MenuState::kPause) return PauseFxView::kPause;
  return PauseFxView::kNone;
}

constexpr bool IsPauseFx(PauseFxView view) { return view != PauseFxView::kNone; }

constexpr bool SupportsCityEffects(bool pause, PauseFxView view) {
  return !pause || view == PauseFxView::kPause || view == PauseFxView::kPhoto;
}

constexpr bool MatchesPauseFxView(bool pause, PauseFxView view) {
  return pause == IsPauseFx(view);
}

constexpr bool PauseFxCameraChanged(bool previous_pause, PauseFxView previous, PauseFxView current) {
  return previous_pause && previous != current;
}

}  // namespace mcla::gpu::gbuffer
