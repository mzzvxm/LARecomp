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

  // 1. Body font (D-DIN-PRO-500-Medium)
  const fs::path ddin_medium_path = assets_font_dir / "D-DIN-PRO-500-Medium.otf";
  if (fs::exists(ddin_medium_path)) {
    g_fonts.body = atlas->AddFontFromFileTTF(ddin_medium_path.string().c_str(), 16.0f, &font_cfg);
  } else {
    ImFontConfig mem_cfg = font_cfg;
    mem_cfg.FontDataOwnedByAtlas = false;
    g_fonts.body = atlas->AddFontFromMemoryTTF(
        const_cast<unsigned char*>(fonts::kFontDDinProMedium),
        static_cast<int>(fonts::kFontDDinProMediumSize), 16.0f, &mem_cfg);
  }

  // 2. Bold font (D-DIN-PRO-700-Bold)
  const fs::path ddin_bold_path = assets_font_dir / "D-DIN-PRO-700-Bold.otf";
  if (fs::exists(ddin_bold_path)) {
    g_fonts.body_bold = atlas->AddFontFromFileTTF(ddin_bold_path.string().c_str(), 17.0f, &font_cfg);
  } else {
    ImFontConfig mem_cfg = font_cfg;
    mem_cfg.FontDataOwnedByAtlas = false;
    g_fonts.body_bold = atlas->AddFontFromMemoryTTF(
        const_cast<unsigned char*>(fonts::kFontDDinProBold),
        static_cast<int>(fonts::kFontDDinProBoldSize), 17.0f, &mem_cfg);
  }

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

#if defined(_WIN32)
  // Optional Japanese font glyph merging from Windows system font
  const char* jp_font_path = "C:\\Windows\\Fonts\\msgothic.ttc";
  if (fs::exists(jp_font_path) && g_fonts.body) {
    ImFontConfig jp_cfg;
    jp_cfg.MergeMode = true;
    jp_cfg.OversampleH = 1;
    jp_cfg.OversampleV = 1;
    jp_cfg.PixelSnapH = true;
    jp_cfg.FontNo = 0;
    atlas->AddFontFromFileTTF(jp_font_path, 16.0f, &jp_cfg, atlas->GetGlyphRangesJapanese());
  }
#endif

  // Set default font to D-DIN-PRO medium
  if (g_fonts.body) {
    ImGui::GetIO().FontDefault = g_fonts.body;
  }

  REXLOG_INFO("[ui] Loaded authentic MCLA fonts (D-DIN-PRO, Midnight Club Title, Los Angeles Script)");
}

void ApplyMclaTheme(ImGuiStyle& style, rex::ui::Style* ui_style) {
  // --- Rounding & Geometry ---
  style.WindowRounding    = 14.0f;
  style.ChildRounding     = 8.0f;
  style.FrameRounding     = 6.0f;
  style.PopupRounding     = 10.0f;
  style.ScrollbarRounding = 6.0f;
  style.GrabRounding      = 6.0f;
  style.TabRounding       = 8.0f;

  // --- Borders ---
  style.WindowBorderSize  = 1.5f;
  style.ChildBorderSize   = 1.0f;
  style.FrameBorderSize   = 1.0f;
  style.PopupBorderSize   = 1.5f;
  style.TabBorderSize     = 0.0f;

  // --- Spacing & Padding ---
  style.WindowPadding     = ImVec2(18.0f, 16.0f);
  style.FramePadding      = ImVec2(12.0f, 7.0f);
  style.ItemSpacing       = ImVec2(10.0f, 8.0f);
  style.ItemInnerSpacing  = ImVec2(8.0f, 6.0f);
  style.CellPadding       = ImVec2(8.0f, 6.0f);
  style.TouchExtraPadding = ImVec2(0.0f, 0.0f);
  style.IndentSpacing     = 20.0f;
  style.ScrollbarSize     = 12.0f;
  style.GrabMinSize       = 12.0f;

  // --- Colors (Authentic Midnight Club Los Angeles sampled palette) ---
  ImVec4* colors = style.Colors;

  // Text
  colors[ImGuiCol_Text]                  = ImVec4(0.95f, 0.96f, 0.98f, 1.00f);
  colors[ImGuiCol_TextDisabled]          = ImVec4(0.55f, 0.58f, 0.64f, 1.00f);

  // Windows & Popups (Smoked Obsidian glass)
  colors[ImGuiCol_WindowBg]              = ImVec4(0.045f, 0.055f, 0.075f, 0.94f);
  colors[ImGuiCol_ChildBg]               = ImVec4(0.065f, 0.080f, 0.105f, 0.70f);
  colors[ImGuiCol_PopupBg]               = ImVec4(0.050f, 0.062f, 0.082f, 0.96f);

  // Outer bezel / frosted glass outline
  colors[ImGuiCol_Border]                = ImVec4(0.85f, 0.88f, 0.94f, 0.45f);
  colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

  // Frames & Inputs
  colors[ImGuiCol_FrameBg]               = ImVec4(0.09f, 0.12f, 0.16f, 0.80f);
  colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.14f, 0.18f, 0.24f, 0.90f);
  colors[ImGuiCol_FrameBgActive]         = ImVec4(0.00f, 0.70f, 0.85f, 0.40f);

  // Titles
  colors[ImGuiCol_TitleBg]               = ImVec4(0.045f, 0.055f, 0.075f, 0.98f);
  colors[ImGuiCol_TitleBgActive]         = ImVec4(0.075f, 0.090f, 0.120f, 1.00f);
  colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.035f, 0.045f, 0.060f, 0.80f);
  colors[ImGuiCol_MenuBarBg]             = ImVec4(0.06f, 0.08f, 0.11f, 0.95f);

  // Scrollbars
  colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.04f, 0.05f, 0.07f, 0.40f);
  colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.20f, 0.26f, 0.34f, 0.80f);
  colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.00f, 0.75f, 0.90f, 0.85f);
  colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.00f, 0.92f, 1.00f, 1.00f);

  // CheckMark & Sliders
  colors[ImGuiCol_CheckMark]             = ImVec4(0.00f, 0.92f, 1.00f, 1.00f); // Electric Cyan
  colors[ImGuiCol_SliderGrab]            = ImVec4(0.00f, 0.78f, 0.92f, 0.85f);
  colors[ImGuiCol_SliderGrabActive]      = ImVec4(1.00f, 0.88f, 0.00f, 1.00f); // MCLA Yellow grab

  // Buttons
  colors[ImGuiCol_Button]                = ImVec4(0.11f, 0.15f, 0.21f, 0.85f);
  colors[ImGuiCol_ButtonHovered]         = ImVec4(0.00f, 0.75f, 0.90f, 0.85f); // Cyan hover
  colors[ImGuiCol_ButtonActive]          = ImVec4(1.00f, 0.88f, 0.00f, 0.95f); // Yellow flash

  // Headers (Selected rows / Active list items in glowing cyan)
  colors[ImGuiCol_Header]                = ImVec4(0.00f, 0.78f, 0.92f, 0.80f);
  colors[ImGuiCol_HeaderHovered]         = ImVec4(0.00f, 0.88f, 1.00f, 0.92f);
  colors[ImGuiCol_HeaderActive]          = ImVec4(0.00f, 0.96f, 1.00f, 1.00f);

  // Separators
  colors[ImGuiCol_Separator]             = ImVec4(0.20f, 0.25f, 0.32f, 0.60f);
  colors[ImGuiCol_SeparatorHovered]      = ImVec4(0.00f, 0.75f, 0.90f, 0.80f);
  colors[ImGuiCol_SeparatorActive]       = ImVec4(0.00f, 0.92f, 1.00f, 1.00f);

  // Resize grip
  colors[ImGuiCol_ResizeGrip]            = ImVec4(0.00f, 0.75f, 0.90f, 0.30f);
  colors[ImGuiCol_ResizeGripHovered]     = ImVec4(0.00f, 0.85f, 1.00f, 0.65f);
  colors[ImGuiCol_ResizeGripActive]      = ImVec4(1.00f, 0.88f, 0.00f, 0.90f);

  // Tabs
  colors[ImGuiCol_Tab]                   = ImVec4(0.10f, 0.14f, 0.19f, 0.85f);
  colors[ImGuiCol_TabHovered]            = ImVec4(0.00f, 0.75f, 0.90f, 0.75f);
  colors[ImGuiCol_TabActive]             = ImVec4(0.00f, 0.82f, 0.96f, 0.95f);
  colors[ImGuiCol_TabUnfocused]          = ImVec4(0.07f, 0.09f, 0.13f, 0.75f);
  colors[ImGuiCol_TabUnfocusedActive]     = ImVec4(0.12f, 0.17f, 0.24f, 0.85f);

  // Plots / Meters (boost gauge / performance meter)
  colors[ImGuiCol_PlotLines]             = ImVec4(0.00f, 0.88f, 1.00f, 1.00f);
  colors[ImGuiCol_PlotLinesHovered]      = ImVec4(1.00f, 0.88f, 0.00f, 1.00f);
  colors[ImGuiCol_PlotHistogram]         = ImVec4(0.00f, 0.78f, 0.92f, 1.00f);
  colors[ImGuiCol_PlotHistogramHovered]  = ImVec4(1.00f, 0.88f, 0.00f, 1.00f);

  // Selection / Modal dim
  colors[ImGuiCol_TextSelectedBg]        = ImVec4(0.00f, 0.82f, 0.96f, 0.35f);
  colors[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.02f, 0.03f, 0.04f, 0.65f);

  // Per-overlay styling
  if (ui_style) {
    ui_style->achievements.header_text       = kColorYellow;
    ui_style->achievements.unlocked_title    = kColorCyan;
    ui_style->achievements.unlocked_desc     = ImVec4(0.80f, 0.92f, 0.98f, 1.00f);
    ui_style->achievements.locked_title      = ImVec4(0.65f, 0.70f, 0.78f, 1.00f);
    ui_style->achievements.locked_desc       = ImVec4(0.45f, 0.48f, 0.54f, 1.00f);
    ui_style->achievements.badge_gamerscore  = kColorYellow;
    ui_style->achievements.row_unlocked_bg   = ImVec4(0.00f, 0.35f, 0.45f, 0.40f);
    ui_style->achievements.progress_bar      = kColorCyan;
    ui_style->achievements.row_rounding      = 8.0f;
    ui_style->achievements.window_padding    = ImVec2(18.0f, 16.0f);
    ui_style->achievements.item_spacing      = ImVec2(10.0f, 8.0f);

    ui_style->toast.title                    = kColorYellow;
    ui_style->toast.text                     = kColorTextWhite;
    ui_style->toast.rounding                 = 12.0f;
    ui_style->toast.background_alpha         = 0.92f;

    ui_style->console.level[1]               = kColorCyan;
    ui_style->console.level[2]               = kColorTextWhite;
    ui_style->console.level[3]               = kColorYellow;
    ui_style->console.completion_bg          = ImVec4(0.05f, 0.07f, 0.09f, 0.96f);
    ui_style->console.completion_highlight   = ImVec4(0.00f, 0.70f, 0.85f, 0.70f);

    ui_style->settings.warning               = kColorYellow;
    ui_style->settings.lifecycle_live        = kColorCyan;
    ui_style->settings.lifecycle_restart     = kColorYellow;
  }
}

void DrawCategoryHeader(const char* title, const char* subtitle) {
  if (!title) return;
  ImGui::PushStyleColor(ImGuiCol_Text, kColorYellow);
  if (g_fonts.body_bold) {
    ImGui::PushFont(g_fonts.body_bold, 18.0f);
  }
  ImGui::TextUnformatted(title);
  if (g_fonts.body_bold) {
    ImGui::PopFont();
  }
  ImGui::PopStyleColor();

  if (subtitle && subtitle[0] != '\0') {
    ImGui::TextDisabled("%s", subtitle);
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

  const ImU32 badge_bg = primary ? IM_COL32(0, 200, 235, 230) : IM_COL32(40, 50, 65, 220);
  const ImU32 badge_border = primary ? IM_COL32(0, 240, 255, 255) : IM_COL32(180, 200, 220, 100);
  const ImU32 key_color = primary ? IM_COL32(10, 15, 20, 255) : IM_COL32(240, 245, 250, 255);

  draw->AddRectFilled(pos, ImVec2(pos.x + badge_size.x, pos.y + badge_size.y), badge_bg, 4.0f);
  draw->AddRect(pos, ImVec2(pos.x + badge_size.x, pos.y + badge_size.y), badge_border, 4.0f, 0, 1.0f);
  draw->AddText(ImVec2(pos.x + pad_x, pos.y + pad_y), key_color, key);

  ImGui::SetCursorScreenPos(ImVec2(pos.x + badge_size.x + 8.0f, pos.y + pad_y));
  if (label && label[0] != '\0') {
    ImGui::TextUnformatted(label);
  }
}

} // namespace mcla::ui
