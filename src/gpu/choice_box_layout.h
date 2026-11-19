#pragma once
// ===========================================================================
// The layout of the two-button box (choice_box.h), as arithmetic on measured sizes
// ===========================================================================
// The compatibility renderer boxes (renderer_fallback.h) have a button whose label is long ("Use the compatibility renderer"). The SDK's
// Windows box (SDL's) sized every button the same way and cut that label off ("...compatibility re"), so LARecomp draws these two boxes
// itself on Windows and sizes each button from its own label. This header is the part that needs no window: given the measured text it
// says where everything goes, in device pixels. choice_box.h measures with the real font and puts real controls at these places;
// gpu/renderer_choice_test.cpp checks the numbers without a screen.
//
// The order is the one Windows' own boxes use: the action first (left) and the way out last (right), the buttons together at the
// right edge of a strip under the text.

#include <algorithm>
#include <cstddef>
#include <vector>

namespace mcla::gpu::fallback {

struct BoxRect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

// Device pixels from device independent ones at `dpi` (96 = 100 %), rounded to the nearest.
constexpr int Dip(int dip, int dpi) { return (dip * dpi + 48) / 96; }

// The sizes (in device pixels) of the controls that were measured with the font the box uses.
struct BoxMeasure {
  int dpi = 96;
  int icon = 0;                // the icon's edge; 0 = no icon
  int body_w = 0;              // the wrapped text: the widest line,
  int body_h = 0;              //   and all its lines
  int label_h = 0;             // one line of a button label
  std::vector<int> label_w;    // the width of each button's label, in the order the buttons are given
};

struct BoxLayout {
  int client_w = 0;
  int client_h = 0;
  int text_area_h = 0;         // the upper block (icon and text); the strip with the buttons is under it
  BoxRect icon;
  BoxRect body;
  std::vector<BoxRect> buttons;  // in the order the buttons are given = left to right
};

// The widest the text may get: 560 dip, and never more than the screen's work area allows (a small screen, a large text size).
constexpr int MaxBodyWidth(int dpi, int work_width, int icon) {
  const int room = work_width * 8 / 10 - 2 * Dip(20, dpi) - (icon > 0 ? icon + Dip(14, dpi) : 0);
  return std::max(Dip(200, dpi), std::min(Dip(560, dpi), room));
}

// Every button is as wide as its label needs (label plus 14 dip each side) and at least 88 dip, all of one height, together at the
// right edge of the strip. The box is as wide as the text block or the buttons need, whichever is more.
inline BoxLayout PlanBox(const BoxMeasure& m) {
  const int dpi = m.dpi;
  const int margin = Dip(20, dpi);
  const int gap = Dip(14, dpi);
  const int strip_pad = Dip(12, dpi);
  const int button_gap = Dip(8, dpi);
  const int label_pad = Dip(14, dpi);

  BoxLayout out;
  const int body_x = margin + (m.icon > 0 ? m.icon + gap : 0);
  const int block_h = std::max(m.icon, m.body_h);
  out.text_area_h = margin + block_h + margin;
  out.icon = {margin, margin, m.icon, m.icon};
  out.body = {body_x, margin + (block_h - m.body_h) / 2, m.body_w, m.body_h};  // one short line sits beside the icon's middle

  const int button_h = std::max(Dip(24, dpi), m.label_h + Dip(12, dpi));
  std::vector<int> widths;
  int total = 0;
  for (const int label : m.label_w) {
    widths.push_back(std::max(Dip(88, dpi), label + 2 * label_pad));
    total += widths.back();
  }
  if (!widths.empty()) {
    total += static_cast<int>(widths.size() - 1) * button_gap;
  }

  out.client_w = std::max(body_x + m.body_w + margin, total + 2 * strip_pad);
  int x = out.client_w - strip_pad - total;
  const int y = out.text_area_h + strip_pad;
  for (const int width : widths) {
    out.buttons.push_back({x, y, width, button_h});
    x += width + button_gap;
  }
  out.client_h = y + button_h + strip_pad;
  return out;
}

}  // namespace mcla::gpu::fallback
