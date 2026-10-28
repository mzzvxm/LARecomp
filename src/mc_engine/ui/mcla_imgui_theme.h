#pragma once

#include <imgui.h>
#include <rex/ui/style.h>

namespace mcla::ui {

struct MclaFonts {
  ImFont* body = nullptr;          // D-DIN-PRO-500-Medium (16px base)
  ImFont* body_bold = nullptr;     // D-DIN-PRO-700-Bold (17px base)
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

// Authentic MCLA Color Palette constants
inline constexpr ImVec4 kColorYellow{1.00f, 0.88f, 0.00f, 1.00f};          // #FFE000 / MCLA Racing Yellow
inline constexpr ImVec4 kColorYellowMuted{0.85f, 0.75f, 0.05f, 1.00f};
inline constexpr ImVec4 kColorCyan{0.00f, 0.88f, 1.00f, 1.00f};            // #00E0FF / Electric Cyan
inline constexpr ImVec4 kColorCyanGlow{0.20f, 0.94f, 1.00f, 1.00f};
inline constexpr ImVec4 kColorDarkBg{0.045f, 0.055f, 0.075f, 0.94f};       // Smoked Obsidian Glass
inline constexpr ImVec4 kColorChildBg{0.065f, 0.080f, 0.105f, 0.70f};      // Inner container dark glass
inline constexpr ImVec4 kColorPopupBg{0.050f, 0.062f, 0.082f, 0.96f};
inline constexpr ImVec4 kColorBorder{0.85f, 0.88f, 0.94f, 0.45f};          // Frosted Silver Bezel
inline constexpr ImVec4 kColorTextWhite{0.95f, 0.96f, 0.98f, 1.00f};
inline constexpr ImVec4 kColorTextDim{0.55f, 0.58f, 0.64f, 1.00f};

// Selection indicator arrow ("▶ ")
inline constexpr const char* kSelectionArrow = "\xE2\x96\xB6 ";

// Helpers for MCLA UI presentation
void DrawCategoryHeader(const char* title, const char* subtitle = nullptr);
void DrawBadge(const char* key, const char* label, bool primary = false);

} // namespace mcla::ui
