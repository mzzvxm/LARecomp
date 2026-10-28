#pragma once

#include <imgui.h>
#include <rex/ui/style.h>

namespace mcla::ui {

struct MclaFonts {
  ImFont* body = nullptr;          // Midnight Club menu font (18px base), D-DIN fallback
  ImFont* body_bold = nullptr;     // Same menu face (18px); MCLA marks emphasis by color
  ImFont* title = nullptr;         // Midnight Club Logo Font (22px base)
  ImFont* script = nullptr;        // MC Los Angeles Script Font (24px base)
};

// Returns loaded font instances
MclaFonts& GetFonts();

// Convenience font getters
ImFont* GetDefaultFont();
ImFont* GetBoldFont();
ImFont* GetTitleFont();
ImFont* GetScriptFont();

// Loads MCLA fonts into the ImGui font atlas.
// Looks in assets/fonts/ first, falling back to embedded binary fonts.
void LoadMclaFonts(ImFontAtlas* atlas);

// Applies the authentic MCLA UI theme (palette, rounding, borders, spacing).
void ApplyMclaTheme(ImGuiStyle& style, rex::ui::Style* ui_style = nullptr);

// Authentic MCLA Color Palette constants, sampled from the retail menus:
// near-black glass panels, a warm silver bezel, chartreuse-yellow titles and
// the teal selection color (yellow #F2E305, teal #33D6D6, bezel #D6D2C6).
inline constexpr ImVec4 kColorYellow{0.95f, 0.89f, 0.02f, 1.00f};          // #F2E305 / MCLA menu yellow
inline constexpr ImVec4 kColorYellowMuted{0.83f, 0.75f, 0.16f, 1.00f};     // dimmer gold (tab rings)
inline constexpr ImVec4 kColorCyan{0.20f, 0.84f, 0.84f, 1.00f};            // #33D6D6 / selection teal
inline constexpr ImVec4 kColorCyanGlow{0.26f, 0.94f, 0.90f, 1.00f};
inline constexpr ImVec4 kColorDarkBg{0.04f, 0.04f, 0.04f, 0.88f};          // near-black glass
inline constexpr ImVec4 kColorChildBg{0.05f, 0.05f, 0.05f, 0.70f};         // inner container dark glass
inline constexpr ImVec4 kColorPopupBg{0.04f, 0.04f, 0.04f, 0.94f};
inline constexpr ImVec4 kColorBorder{0.84f, 0.82f, 0.78f, 0.55f};          // warm silver bezel #D6D2C6
inline constexpr ImVec4 kColorTextWhite{0.91f, 0.91f, 0.88f, 1.00f};       // warm off-white #E8E8E0
inline constexpr ImVec4 kColorTextDim{0.55f, 0.55f, 0.52f, 1.00f};

// Selection indicator arrow ("▶ ")
inline constexpr const char* kSelectionArrow = "\xE2\x96\xB6 ";

// Helpers for MCLA UI presentation
void DrawCategoryHeader(const char* title, const char* subtitle = nullptr);
void DrawBadge(const char* key, const char* label, bool primary = false);

} // namespace mcla::ui
