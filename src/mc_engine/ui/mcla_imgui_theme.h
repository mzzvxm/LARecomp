#pragma once

#include <imgui.h>
#include <rex/ui/style.h>

#include <span>

namespace mcla::ui {

// The retail menus use two faces: a wide heavy one for panel titles ("MODES",
// "DISPLAY") and a narrower one for everything else. Until the community menu
// fonts land, the Logo font stands in for the body -- its caps run ~7 cap
// heights per "ARCADE" like the retail rows, where Font Alt runs ~9.3 -- and
// Font Alt takes the titles.
struct MclaFonts {
  ImFont* body = nullptr;    // Midnight Club Logo Font
  ImFont* title = nullptr;   // Midnight Club Font Alt
  ImFont* script = nullptr;  // MC Los Angeles Font
};

inline constexpr float kBodySize = 18.0f;
inline constexpr float kSmallSize = 15.0f;
inline constexpr float kTitleSize = 28.0f;

// Returns loaded font instances
MclaFonts& GetFonts();

// Loads MCLA fonts into the ImGui font atlas.
// Looks in assets/fonts/ first, falling back to embedded binary fonts.
void LoadMclaFonts(ImFontAtlas* atlas);

// Applies the MCLA UI theme (palette, rounding, borders, spacing).
void ApplyMclaTheme(ImGuiStyle& style, rex::ui::Style* ui_style = nullptr);

// MCLA palette, sampled from the retail menus: near-black glass panels inside a
// frosted silver bezel, chartreuse-yellow titles and the teal selection color.
inline constexpr ImVec4 kColorYellow{0.95f, 0.89f, 0.02f, 1.00f};          // #F2E305 / MCLA menu yellow
inline constexpr ImVec4 kColorCyan{0.20f, 0.84f, 0.84f, 1.00f};            // #33D6D6 / selection teal
inline constexpr ImVec4 kColorCyanGlow{0.26f, 0.94f, 0.90f, 1.00f};
inline constexpr ImVec4 kColorDarkBg{0.04f, 0.04f, 0.04f, 0.90f};          // near-black glass
inline constexpr ImVec4 kColorChildBg{0.00f, 0.00f, 0.00f, 0.00f};         // lists sit on the panel glass
inline constexpr ImVec4 kColorPopupBg{0.04f, 0.04f, 0.04f, 0.96f};
inline constexpr ImVec4 kColorBorder{0.84f, 0.84f, 0.81f, 0.50f};          // frosted bezel #D6D6CF
inline constexpr ImVec4 kColorTextWhite{0.91f, 0.91f, 0.88f, 1.00f};       // warm off-white #E8E8E0
inline constexpr ImVec4 kColorTextDim{0.55f, 0.55f, 0.52f, 1.00f};

// Retail button colors for the footer prompts (B red, A green).
inline constexpr ImU32 kPromptBack = IM_COL32(222, 38, 38, 255);
inline constexpr ImU32 kPromptAccept = IM_COL32(110, 190, 40, 255);
inline constexpr ImU32 kPromptNeutral = IM_COL32(190, 190, 184, 255);

// The pictogram inside the dashed yellow ring left of a panel title.
enum class PanelIcon { kNone, kCheck, kAlert, kPause, kInfo, kGamepad };

// A button hint in a panel's footer bar: "ACCEPT (ENTER)".
struct Prompt {
  const char* label;
  const char* key;
  ImU32 key_color = kPromptNeutral;
};

// A retail-style panel: frosted bezel, dark glass body with the title at its
// top, and an optional footer bar of prompts below the body. Set the window
// position and size first, as for ImGui::Begin. The prompts must outlive the
// panel. EndPanel must be called whatever BeginPanel returns, and returns the
// index of a clicked prompt, or -1.
bool BeginPanel(const char* id, const char* title, PanelIcon icon,
                ImGuiWindowFlags flags = 0, std::span<const Prompt> prompts = {},
                float title_size = kTitleSize);
int EndPanel();

// The lighter mixed-case line the retail menus put under a title.
void PanelSubtitle(const char* text);

// One list row: the teal gradient bar and cursor triangle when selected, the
// faint hairline under it otherwise. Returns the color for the row's text.
ImU32 DrawListRow(ImDrawList* draw, ImVec2 low, ImVec2 high, bool selected);

// The segmented meter of the GPS screen's rep and tuner bars.
void SegmentMeter(float fraction, float width, int segments = 10);

} // namespace mcla::ui
