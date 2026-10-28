#include "mcla_imgui_theme.h"
#include "fonts/mcla_embedded_fonts.h"

#include <filesystem>
#include <rex/filesystem.h>
#include <rex/logging.h>

namespace fs = std::filesystem;

namespace mcla::ui {

static MclaFonts g_fonts;

MclaFonts& GetFonts() {
  return g_fonts;
}

ImFont* GetDefaultFont() {
  return g_fonts.body;
}

ImFont* GetBoldFont() {
  return g_fonts.body_bold;
}

ImFont* GetTitleFont() {
  return g_fonts.title;
}

ImFont* GetScriptFont() {
  return g_fonts.script;
}

void LoadMclaFonts(ImFontAtlas* atlas) {
  if (!atlas) return;

  const auto exe_dir = rex::filesystem::GetExecutableFolder();
  const auto assets_font_dir = exe_dir / "assets" / "fonts";

  ImFontConfig font_cfg;
  font_cfg.OversampleH = 2;
  font_cfg.OversampleV = 2;
  font_cfg.PixelSnapH = true;

  // 1. Body font. The retail menus set every label in the game's own condensed
  // techno face (assets/fonts/Midnight Club Font Alt.ttf); D-DIN is the
  // fallback when the asset is missing. The game marks emphasis with color,
  // not weight, so body_bold is the same face.
  constexpr float kBodySize = 19.0f;
  const fs::path mc_menu_path = assets_font_dir / "Midnight Club Font Alt.ttf";
  if (fs::exists(mc_menu_path)) {
    g_fonts.body = atlas->AddFontFromFileTTF(mc_menu_path.string().c_str(), kBodySize, &font_cfg);
  }
  if (!g_fonts.body) {
    const fs::path ddin_medium_path = assets_font_dir / "D-DIN-PRO-500-Medium.otf";
    if (fs::exists(ddin_medium_path)) {
      g_fonts.body = atlas->AddFontFromFileTTF(ddin_medium_path.string().c_str(), kBodySize, &font_cfg);
    } else {
      ImFontConfig mem_cfg = font_cfg;
      mem_cfg.FontDataOwnedByAtlas = false;
      g_fonts.body = atlas->AddFontFromMemoryTTF(
          const_cast<unsigned char*>(fonts::kFontDDinProMedium),
          static_cast<int>(fonts::kFontDDinProMediumSize), kBodySize, &mem_cfg);
    }
  }

#if defined(_WIN32)
  // Optional Japanese font glyph merging from Windows system font, merged into
  // the body face right after it so fallback lookups find the glyphs.
  const char* jp_font_path = "C:\\Windows\\Fonts\\msgothic.ttc";
  if (fs::exists(jp_font_path) && g_fonts.body) {
    ImFontConfig jp_cfg;
    jp_cfg.MergeMode = true;
    jp_cfg.OversampleH = 1;
    jp_cfg.OversampleV = 1;
    jp_cfg.PixelSnapH = true;
    jp_cfg.FontNo = 0;
    atlas->AddFontFromFileTTF(jp_font_path, kBodySize, &jp_cfg, atlas->GetGlyphRangesJapanese());
  }
#endif

  // 2. Bold font: same face as the body (see above).
  g_fonts.body_bold = g_fonts.body;

  // 3. Title font (Midnight Club Logo Font)
  const fs::path mc_title_path = assets_font_dir / "Midnight Club Logo Font.ttf";
  if (fs::exists(mc_title_path)) {
    g_fonts.title = atlas->AddFontFromFileTTF(mc_title_path.string().c_str(), 22.0f, &font_cfg);
  } else {
    ImFontConfig mem_cfg = font_cfg;
    mem_cfg.FontDataOwnedByAtlas = false;
    g_fonts.title = atlas->AddFontFromMemoryTTF(
        const_cast<unsigned char*>(fonts::kFontMidnightClubTitle),
        static_cast<int>(fonts::kFontMidnightClubTitleSize), 22.0f, &mem_cfg);
  }

  // 4. Script font (MC Los Angeles Font)
  const fs::path la_script_path = assets_font_dir / "MC Los Angeles Font.ttf";
  if (fs::exists(la_script_path)) {
    g_fonts.script = atlas->AddFontFromFileTTF(la_script_path.string().c_str(), 24.0f, &font_cfg);
  } else {
    ImFontConfig mem_cfg = font_cfg;
    mem_cfg.FontDataOwnedByAtlas = false;
    g_fonts.script = atlas->AddFontFromMemoryTTF(
        const_cast<unsigned char*>(fonts::kFontLosAngelesScript),
        static_cast<int>(fonts::kFontLosAngelesScriptSize), 24.0f, &mem_cfg);
  }

  // Set default font to the MCLA menu face
  if (g_fonts.body) {
    ImGui::GetIO().FontDefault = g_fonts.body;
  }

  REXLOG_INFO("[ui] Loaded authentic MCLA fonts (Midnight Club menu face, Title, Los Angeles Script)");
}

void ApplyMclaTheme(ImGuiStyle& style, rex::ui::Style* ui_style) {
  // --- Rounding & Geometry ---
  // The retail panels are large-radius rounded rectangles over the live world.
  style.WindowRounding    = 18.0f;
  style.ChildRounding     = 10.0f;
  style.FrameRounding     = 6.0f;
  style.PopupRounding     = 14.0f;
  style.ScrollbarRounding = 8.0f;
  style.GrabRounding      = 4.0f;
  style.TabRounding       = 8.0f;

  // --- Borders ---
  style.WindowBorderSize  = 1.5f;
  style.ChildBorderSize   = 1.0f;
  style.FrameBorderSize   = 1.0f;
  style.PopupBorderSize   = 1.5f;
  style.TabBorderSize     = 0.0f;

  // --- Spacing & Padding ---
  // Rows in the retail lists breathe: row height is roughly 1.6-2x cap height.
  style.WindowPadding     = ImVec2(24.0f, 20.0f);
  style.FramePadding      = ImVec2(12.0f, 8.0f);
  style.ItemSpacing       = ImVec2(10.0f, 10.0f);
  style.ItemInnerSpacing  = ImVec2(8.0f, 6.0f);
  style.CellPadding       = ImVec2(8.0f, 6.0f);
  style.TouchExtraPadding = ImVec2(0.0f, 0.0f);
  style.IndentSpacing     = 22.0f;
  style.ScrollbarSize     = 10.0f;
  style.GrabMinSize       = 12.0f;

  // --- Colors, sampled from the retail menus (pause/options, GPS, missions) ---
  // Panel near-black #0A0A0A, warm silver bezel #D6D2C6, title yellow #F2E205,
  // selection teal #33D6D6 over a dark teal bar #0E6E6D, text #E8E8E8.
  ImVec4* colors = style.Colors;

  // Text: warm off-white; dim rows fade toward #8B8B86.
  colors[ImGuiCol_Text]                  = kColorTextWhite;
  colors[ImGuiCol_TextDisabled]          = kColorTextDim;

  // Windows & Popups: near-black glass, slightly see-through.
  colors[ImGuiCol_WindowBg]              = kColorDarkBg;
  colors[ImGuiCol_ChildBg]               = kColorChildBg;
  colors[ImGuiCol_PopupBg]               = kColorPopupBg;

  // Outer bezel: the warm silver rim with a soft glow the menus are known for.
  colors[ImGuiCol_Border]                = kColorBorder;
  colors[ImGuiCol_BorderShadow]          = ImVec4(0.84f, 0.82f, 0.78f, 0.12f);

  // Frames & Inputs
  colors[ImGuiCol_FrameBg]               = ImVec4(0.07f, 0.07f, 0.07f, 0.85f);
  colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.12f, 0.12f, 0.11f, 0.90f);
  colors[ImGuiCol_FrameBgActive]         = ImVec4(0.05f, 0.43f, 0.43f, 0.55f);

  // Titles
  colors[ImGuiCol_TitleBg]               = ImVec4(0.04f, 0.04f, 0.04f, 0.96f);
  colors[ImGuiCol_TitleBgActive]         = ImVec4(0.07f, 0.07f, 0.07f, 1.00f);
  colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.03f, 0.03f, 0.03f, 0.80f);
  colors[ImGuiCol_MenuBarBg]             = ImVec4(0.06f, 0.06f, 0.06f, 0.95f);

  // Scrollbars: slim, teal when touched (the retail scrollbar thumb is cyan).
  colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.04f, 0.04f, 0.04f, 0.30f);
  colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.20f, 0.84f, 0.84f, 0.55f);
  colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.20f, 0.84f, 0.84f, 0.80f);
  colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.26f, 0.94f, 0.90f, 1.00f);

  // CheckMark & Sliders: white tick on a dark box, like the options checkboxes.
  colors[ImGuiCol_CheckMark]             = ImVec4(0.95f, 0.95f, 0.92f, 1.00f);
  colors[ImGuiCol_SliderGrab]            = ImVec4(0.20f, 0.84f, 0.84f, 0.85f);
  colors[ImGuiCol_SliderGrabActive]      = kColorYellow;

  // Buttons: dark glass with a teal hover and a yellow press flash.
  colors[ImGuiCol_Button]                = ImVec4(0.10f, 0.10f, 0.10f, 0.85f);
  colors[ImGuiCol_ButtonHovered]         = ImVec4(0.05f, 0.43f, 0.43f, 0.85f);
  colors[ImGuiCol_ButtonActive]          = ImVec4(0.95f, 0.89f, 0.02f, 0.90f);

  // Headers: the selection is a dark teal bar (#0E6E6D) with teal text on top,
  // not bright cyan with dark text -- the picker draws the row text itself.
  colors[ImGuiCol_Header]                = ImVec4(0.055f, 0.43f, 0.43f, 0.85f);
  colors[ImGuiCol_HeaderHovered]         = ImVec4(0.09f, 0.55f, 0.55f, 0.90f);
  colors[ImGuiCol_HeaderActive]          = ImVec4(0.14f, 0.65f, 0.63f, 0.95f);

  // Separators: faint white hairlines between list rows (~15% white).
  colors[ImGuiCol_Separator]             = ImVec4(1.00f, 1.00f, 1.00f, 0.14f);
  colors[ImGuiCol_SeparatorHovered]      = ImVec4(0.20f, 0.84f, 0.84f, 0.70f);
  colors[ImGuiCol_SeparatorActive]       = ImVec4(0.26f, 0.94f, 0.90f, 1.00f);

  // Resize grip
  colors[ImGuiCol_ResizeGrip]            = ImVec4(0.20f, 0.84f, 0.84f, 0.25f);
  colors[ImGuiCol_ResizeGripHovered]     = ImVec4(0.20f, 0.84f, 0.84f, 0.60f);
  colors[ImGuiCol_ResizeGripActive]      = ImVec4(0.95f, 0.89f, 0.02f, 0.90f);

  // Tabs
  colors[ImGuiCol_Tab]                   = ImVec4(0.10f, 0.10f, 0.10f, 0.85f);
  colors[ImGuiCol_TabHovered]            = ImVec4(0.05f, 0.43f, 0.43f, 0.75f);
  colors[ImGuiCol_TabActive]             = ImVec4(0.09f, 0.55f, 0.55f, 0.95f);
  colors[ImGuiCol_TabUnfocused]          = ImVec4(0.07f, 0.07f, 0.07f, 0.75f);
  colors[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.12f, 0.12f, 0.11f, 0.85f);

  // Plots / Meters: teal bars, yellow on hover (rep/tuner meter colors).
  colors[ImGuiCol_PlotLines]             = kColorCyan;
  colors[ImGuiCol_PlotLinesHovered]      = kColorYellow;
  colors[ImGuiCol_PlotHistogram]         = kColorCyan;
  colors[ImGuiCol_PlotHistogramHovered]  = kColorYellow;

  // Selection / Modal dim
  colors[ImGuiCol_TextSelectedBg]        = ImVec4(0.20f, 0.84f, 0.84f, 0.35f);
  colors[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.01f, 0.01f, 0.01f, 0.60f);

  // Per-overlay styling
  if (ui_style) {
    ui_style->achievements.header_text       = kColorYellow;
    ui_style->achievements.unlocked_title    = kColorCyan;
    ui_style->achievements.unlocked_desc     = ImVec4(0.85f, 0.92f, 0.90f, 1.00f);
    ui_style->achievements.locked_title      = ImVec4(0.68f, 0.68f, 0.65f, 1.00f);
    ui_style->achievements.locked_desc       = ImVec4(0.48f, 0.48f, 0.46f, 1.00f);
    ui_style->achievements.badge_gamerscore  = kColorYellow;
    ui_style->achievements.row_unlocked_bg   = ImVec4(0.05f, 0.35f, 0.35f, 0.40f);
    ui_style->achievements.progress_bar      = kColorCyan;
    ui_style->achievements.row_rounding      = 8.0f;
    ui_style->achievements.window_padding    = ImVec2(24.0f, 20.0f);
    ui_style->achievements.item_spacing      = ImVec2(10.0f, 10.0f);

    ui_style->toast.title                    = kColorYellow;
    ui_style->toast.text                     = kColorTextWhite;
    ui_style->toast.rounding                 = 14.0f;
    ui_style->toast.background_alpha         = 0.92f;

    ui_style->console.level[1]               = kColorCyan;
    ui_style->console.level[2]               = kColorTextWhite;
    ui_style->console.level[3]               = kColorYellow;
    ui_style->console.completion_bg          = ImVec4(0.05f, 0.05f, 0.05f, 0.96f);
    ui_style->console.completion_highlight   = ImVec4(0.05f, 0.43f, 0.43f, 0.70f);

    ui_style->settings.warning               = kColorYellow;
    ui_style->settings.lifecycle_live        = kColorCyan;
    ui_style->settings.lifecycle_restart     = kColorYellow;
  }
}

void DrawCategoryHeader(const char* title, const char* subtitle) {
  if (!title) return;
  // Retail titles: the menu yellow, the condensed face a step larger, tight
  // tracking comes from the face itself.
  ImGui::PushStyleColor(ImGuiCol_Text, kColorYellow);
  if (g_fonts.body) {
    ImGui::PushFont(g_fonts.body, 22.0f);
  }
  ImGui::TextUnformatted(title);
  if (g_fonts.body) {
    ImGui::PopFont();
  }
  ImGui::PopStyleColor();

  if (subtitle && subtitle[0] != '\0') {
    // Retail descriptions drop to a lighter mixed-case line under the title.
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.75f, 0.72f, 1.00f));
    ImGui::TextUnformatted(subtitle);
    ImGui::PopStyleColor();
  }
  ImGui::Spacing();
}

void DrawBadge(const char* key, const char* label, bool primary) {
  if (!key) return;

  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const ImVec2 key_size = ImGui::CalcTextSize(key);

  const float pad_x = 6.0f;
  const float pad_y = 2.0f;
  const ImVec2 badge_size(key_size.x + pad_x * 2.0f, key_size.y + pad_y * 2.0f);

  const ImU32 badge_bg = primary ? IM_COL32(51, 214, 214, 230) : IM_COL32(26, 26, 24, 220);
  const ImU32 badge_border = primary ? IM_COL32(66, 221, 204, 255) : IM_COL32(214, 210, 198, 100);
  const ImU32 key_color = primary ? IM_COL32(8, 12, 12, 255) : IM_COL32(232, 232, 224, 255);

  draw->AddRectFilled(pos, ImVec2(pos.x + badge_size.x, pos.y + badge_size.y), badge_bg, 4.0f);
  draw->AddRect(pos, ImVec2(pos.x + badge_size.x, pos.y + badge_size.y), badge_border, 4.0f, 0, 1.0f);
  draw->AddText(ImVec2(pos.x + pad_x, pos.y + pad_y), key_color, key);

  ImGui::SetCursorScreenPos(ImVec2(pos.x + badge_size.x + 8.0f, pos.y + pad_y));
  if (label && label[0] != '\0') {
    ImGui::TextUnformatted(label);
  }
}

} // namespace mcla::ui
