#pragma once

#include <imgui.h>
#include <rex/ui/style.h>

#include <span>
#include <vector>

namespace mcla::ui {

// The retail menus set everything in one face, MCLA Ingame Font: the rows and
// labels at body size, the panel titles ("RACE RESULTS", "REWARDS") larger,
// heavier and spaced wider (see BeginPanel). The SDK renderer keeps ImGui's
// fixed atlas, so glyphs exist only at the sizes loaded here and other sizes
// are scaled from them; each role is loaded near the size it is drawn at.
struct MclaFonts {
  ImFont* body = nullptr;     // MCLA Ingame Font
  ImFont* title = nullptr;    // MCLA Ingame Font, at title size
  ImFont* version = nullptr;  // Midnight Club Logo Font: the save picker's version label
  // The save picker banner (Font Alt, Los Angeles), holding only its characters.
  ImFont* banner = nullptr;
  ImFont* banner_credit = nullptr;
};

inline constexpr char kBannerWord[] = "LARecomp";
inline constexpr char kBannerCredit[] = "By mzzvxm & BadassBaboon";

// Kerned text (mcla_kerning.cpp). ImGui lays text out by advance widths alone,
// but the Midnight Club faces carry real kerning (A-R opens, B-y and m-m close
// up a lot), which is what makes the banner read like the artwork. A font
// gets kerning once its TTF bytes are registered; any other lays out as ImGui
// would. Single-byte (ASCII) text only.
void RegisterKerning(const ImFont* font, std::vector<unsigned char> ttf);
// The face's declared slant (post.italicAngle) as run over rise, 0 when upright
// or unregistered.
float ItalicSlant(const ImFont* font);
// Each character's x origin at `px`, and the total advance in `width`;
// `tracking` adds that many pixels between characters.
std::vector<float> KernedOrigins(ImFont* font, float px, const char* text, float* width,
                                 float tracking = 0.0f);
void DrawKerned(ImDrawList* draw, ImFont* font, float px, ImVec2 pos, const char* text,
                ImU32 (*color)(size_t index), float tracking = 0.0f);

// MCLA Ingame Font's caps are 0.53 of its size (the Logo font's were 0.58):
// these keep the text the height it was.
inline constexpr float kBodySize = 20.0f;
inline constexpr float kSmallSize = 17.0f;
inline constexpr float kTitleSize = 30.0f;

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

// Status colors for the app's own windows, kept to the retail palette: the
// game says "good" in its teal and "careful" in its yellow, and keeps red for
// failures (the B button red, lifted for text on dark glass).
inline constexpr ImVec4 kColorGood = kColorCyan;
inline constexpr ImVec4 kColorWarn = kColorYellow;
inline constexpr ImVec4 kColorBad{0.93f, 0.33f, 0.29f, 1.00f};

// Retail button colors for the footer prompts (B red, A green, X blue, Y yellow).
inline constexpr ImU32 kPromptBack = IM_COL32(222, 38, 38, 255);
inline constexpr ImU32 kPromptAccept = IM_COL32(110, 190, 40, 255);
inline constexpr ImU32 kPromptX = IM_COL32(40, 110, 220, 255);
inline constexpr ImU32 kPromptY = IM_COL32(240, 196, 20, 255);
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

// Height a fixed-size panel's content must leave free at the bottom of
// GetContentRegionAvail() for the body padding and footer bar, so a child that
// fills the panel ends where the body does.
float PanelBottomReserve();

// The lighter mixed-case line the retail menus put under a title.
void PanelSubtitle(const char* text);

// One list row: the teal gradient bar and cursor triangle when selected, the
// faint hairline under it otherwise. Returns the color for the row's text.
ImU32 DrawListRow(ImDrawList* draw, ImVec2 low, ImVec2 high, bool selected);

// The segmented meter of the GPS screen's rep and tuner bars.
void SegmentMeter(float fraction, float width, int segments = 10);

} // namespace mcla::ui
