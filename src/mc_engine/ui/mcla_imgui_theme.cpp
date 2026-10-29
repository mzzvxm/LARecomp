#include "mcla_imgui_theme.h"
#include "fonts/mcla_embedded_fonts.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>
#include <rex/filesystem.h>
#include <rex/logging.h>

namespace fs = std::filesystem;

namespace mcla::ui {

static MclaFonts g_fonts;

MclaFonts& GetFonts() {
  return g_fonts;
}

// A loose file in assets/fonts wins, so a replacement face can be tried
// without a rebuild; the embedded copy is the fallback.
static ImFont* LoadFace(ImFontAtlas* atlas, const fs::path& dir, const char* file,
                        const unsigned char* data, size_t size, float px,
                        const ImFontConfig& cfg) {
  const fs::path path = dir / file;
  if (fs::exists(path)) {
    if (ImFont* font = atlas->AddFontFromFileTTF(path.string().c_str(), px, &cfg)) {
      return font;
    }
  }
  ImFontConfig mem_cfg = cfg;
  mem_cfg.FontDataOwnedByAtlas = false;
  return atlas->AddFontFromMemoryTTF(const_cast<unsigned char*>(data), static_cast<int>(size),
                                     px, &mem_cfg);
}

// The bytes LoadFace would load: the loose file when there is one, else the
// embedded copy.
static std::vector<unsigned char> FaceBytes(const fs::path& dir, const char* file,
                                            const unsigned char* data, size_t size) {
  const fs::path path = dir / file;
  std::error_code ec;
  if (fs::exists(path, ec)) {
    std::ifstream in(path, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
    if (!bytes.empty()) return bytes;
  }
  return std::vector<unsigned char>(data, data + size);
}

void LoadMclaFonts(ImFontAtlas* atlas) {
  if (!atlas) return;

  const fs::path dir = rex::filesystem::GetExecutableFolder() / "assets" / "fonts";

  ImFontConfig font_cfg;
  font_cfg.OversampleH = 2;
  font_cfg.OversampleV = 2;
  font_cfg.PixelSnapH = true;

  g_fonts.body = LoadFace(atlas, dir, "Midnight Club Logo Font.ttf", fonts::kFontMidnightClubLogo,
                          fonts::kFontMidnightClubLogoSize, kBodySize, font_cfg);

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

  g_fonts.title = LoadFace(atlas, dir, "Midnight Club Font Alt.ttf", fonts::kFontMidnightClubAlt,
                           fonts::kFontMidnightClubAltSize, kTitleSize, font_cfg);
  g_fonts.script = LoadFace(atlas, dir, "MC Los Angeles Font.ttf", fonts::kFontLosAngelesScript,
                            fonts::kFontLosAngelesScriptSize, 24.0f, font_cfg);

  // The banner faces (see MclaFonts::banner), rasterized near the size the
  // picker draws them at and limited to the banner's characters so the large
  // glyphs cost little atlas space. The ranges must outlive the atlas build.
  static ImVector<ImWchar> word_ranges;
  static ImVector<ImWchar> credit_ranges;
  if (word_ranges.empty()) {
    ImFontGlyphRangesBuilder word;
    word.AddText(kBannerWord);
    word.BuildRanges(&word_ranges);
    ImFontGlyphRangesBuilder credit;
    credit.AddText(kBannerCredit);
    credit.BuildRanges(&credit_ranges);
  }
  // Rasterized a little above the size they are drawn at on common screens,
  // so they are scaled down (sharp) rather than up. The bytes are kept: the
  // banner also reads the faces' kerning from them (RegisterKerning).
  static std::vector<unsigned char> word_ttf =
      FaceBytes(dir, "Midnight Club Font Alt.ttf", fonts::kFontMidnightClubAlt,
                fonts::kFontMidnightClubAltSize);
  static std::vector<unsigned char> credit_ttf =
      FaceBytes(dir, "MC Los Angeles Font.ttf", fonts::kFontLosAngelesScript,
                fonts::kFontLosAngelesScriptSize);
  ImFontConfig banner_cfg = font_cfg;
  banner_cfg.FontDataOwnedByAtlas = false;
  banner_cfg.OversampleH = 1;
  banner_cfg.OversampleV = 1;
  banner_cfg.GlyphRanges = word_ranges.Data;
  g_fonts.banner = atlas->AddFontFromMemoryTTF(word_ttf.data(), static_cast<int>(word_ttf.size()),
                                               160.0f, &banner_cfg);
  banner_cfg.OversampleH = 2;
  banner_cfg.GlyphRanges = credit_ranges.Data;
  g_fonts.banner_credit = atlas->AddFontFromMemoryTTF(
      credit_ttf.data(), static_cast<int>(credit_ttf.size()), 40.0f, &banner_cfg);
  RegisterKerning(g_fonts.banner, word_ttf);
  RegisterKerning(g_fonts.banner_credit, credit_ttf);

  if (g_fonts.body) {
    ImGui::GetIO().FontDefault = g_fonts.body;
  }

  REXLOG_INFO("[ui] Loaded MCLA fonts (Logo body, Font Alt titles, Los Angeles script)");
}

void ApplyMclaTheme(ImGuiStyle& style, rex::ui::Style* ui_style) {
  style.FontSizeBase = kBodySize;

  // --- Rounding & Geometry ---
  // Retail panels: a large-radius frosted bezel around a smaller-radius body.
  // Plain windows (the SDK's settings, console, achievements) get the bezel as
  // a thick translucent border.
  style.WindowRounding    = 16.0f;
  style.ChildRounding     = 8.0f;
  style.FrameRounding     = 4.0f;
  style.PopupRounding     = 10.0f;
  style.ScrollbarRounding = 2.0f;
  style.GrabRounding      = 2.0f;
  style.TabRounding       = 6.0f;
  style.WindowTitleAlign  = ImVec2(0.0f, 0.5f);

  // --- Borders ---
  style.WindowBorderSize  = 3.0f;
  style.ChildBorderSize   = 1.0f;
  style.FrameBorderSize   = 1.0f;
  style.PopupBorderSize   = 2.0f;
  style.TabBorderSize     = 0.0f;

  // --- Spacing & Padding ---
  style.WindowPadding     = ImVec2(18.0f, 14.0f);
  style.FramePadding      = ImVec2(10.0f, 5.0f);
  style.ItemSpacing       = ImVec2(10.0f, 7.0f);
  style.ItemInnerSpacing  = ImVec2(8.0f, 5.0f);
  style.CellPadding       = ImVec2(8.0f, 4.0f);
  style.TouchExtraPadding = ImVec2(0.0f, 0.0f);
  style.IndentSpacing     = 20.0f;
  // The retail scrollbar is a thin rail with a small teal thumb.
  style.ScrollbarSize     = 6.0f;
  style.GrabMinSize       = 10.0f;

  // --- Colors, sampled from the retail menus (pause/options, GPS, missions) ---
  // Panel near-black #0A0A0A, frosted bezel #D6D6CF, title yellow #F2E305,
  // selection teal #33D6D6 over a dark teal bar #0E6E6D, text #E8E8E0.
  ImVec4* colors = style.Colors;

  colors[ImGuiCol_Text]                  = kColorTextWhite;
  colors[ImGuiCol_TextDisabled]          = kColorTextDim;

  colors[ImGuiCol_WindowBg]              = kColorDarkBg;
  colors[ImGuiCol_ChildBg]               = kColorChildBg;
  colors[ImGuiCol_PopupBg]               = kColorPopupBg;

  colors[ImGuiCol_Border]                = kColorBorder;
  colors[ImGuiCol_BorderShadow]          = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

  // Frames & Inputs: the options checkbox is a white-outlined dark box.
  colors[ImGuiCol_FrameBg]               = ImVec4(0.02f, 0.02f, 0.02f, 0.85f);
  colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.05f, 0.30f, 0.30f, 0.70f);
  colors[ImGuiCol_FrameBgActive]         = ImVec4(0.05f, 0.43f, 0.43f, 0.80f);

  colors[ImGuiCol_TitleBg]               = ImVec4(0.03f, 0.03f, 0.03f, 0.96f);
  colors[ImGuiCol_TitleBgActive]         = ImVec4(0.05f, 0.05f, 0.05f, 1.00f);
  colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.03f, 0.03f, 0.03f, 0.80f);
  colors[ImGuiCol_MenuBarBg]             = ImVec4(0.06f, 0.06f, 0.06f, 0.95f);

  colors[ImGuiCol_ScrollbarBg]           = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);
  colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.20f, 0.84f, 0.84f, 0.80f);
  colors[ImGuiCol_ScrollbarGrabHovered]  = kColorCyanGlow;
  colors[ImGuiCol_ScrollbarGrabActive]   = kColorCyanGlow;

  colors[ImGuiCol_CheckMark]             = ImVec4(0.95f, 0.95f, 0.92f, 1.00f);
  colors[ImGuiCol_SliderGrab]            = kColorCyan;
  colors[ImGuiCol_SliderGrabActive]      = kColorCyanGlow;

  colors[ImGuiCol_Button]                = ImVec4(0.10f, 0.10f, 0.10f, 0.85f);
  colors[ImGuiCol_ButtonHovered]         = ImVec4(0.05f, 0.43f, 0.43f, 0.85f);
  colors[ImGuiCol_ButtonActive]          = ImVec4(0.11f, 0.61f, 0.60f, 0.95f);

  // Headers: the selection is a dark teal bar with teal text on top.
  colors[ImGuiCol_Header]                = ImVec4(0.055f, 0.43f, 0.43f, 0.85f);
  colors[ImGuiCol_HeaderHovered]         = ImVec4(0.05f, 0.35f, 0.35f, 0.60f);
  colors[ImGuiCol_HeaderActive]          = ImVec4(0.09f, 0.55f, 0.55f, 0.90f);

  // Separators: the faint hairlines between list rows.
  colors[ImGuiCol_Separator]             = ImVec4(1.00f, 1.00f, 1.00f, 0.14f);
  colors[ImGuiCol_SeparatorHovered]      = ImVec4(0.20f, 0.84f, 0.84f, 0.70f);
  colors[ImGuiCol_SeparatorActive]       = kColorCyanGlow;

  colors[ImGuiCol_ResizeGrip]            = ImVec4(0.20f, 0.84f, 0.84f, 0.20f);
  colors[ImGuiCol_ResizeGripHovered]     = ImVec4(0.20f, 0.84f, 0.84f, 0.60f);
  colors[ImGuiCol_ResizeGripActive]      = kColorCyanGlow;

  colors[ImGuiCol_Tab]                   = ImVec4(0.08f, 0.08f, 0.08f, 0.85f);
  colors[ImGuiCol_TabHovered]            = ImVec4(0.05f, 0.43f, 0.43f, 0.75f);
  colors[ImGuiCol_TabActive]             = ImVec4(0.06f, 0.06f, 0.06f, 1.00f);
  colors[ImGuiCol_TabUnfocused]          = ImVec4(0.07f, 0.07f, 0.07f, 0.75f);
  colors[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.10f, 0.10f, 0.10f, 0.85f);

  // Plots / Meters: the rep and tuner meters are teal.
  colors[ImGuiCol_PlotLines]             = kColorCyan;
  colors[ImGuiCol_PlotLinesHovered]      = kColorYellow;
  colors[ImGuiCol_PlotHistogram]         = kColorCyan;
  colors[ImGuiCol_PlotHistogramHovered]  = kColorYellow;

  // The retail selected tab: a lit teal rule over a dark tab, no blue.
  colors[ImGuiCol_TabSelectedOverline]        = kColorCyan;
  colors[ImGuiCol_TabDimmedSelectedOverline]  = ImVec4(0.20f, 0.84f, 0.84f, 0.40f);

  // Tables: the list grooves, no zebra striping.
  colors[ImGuiCol_TableHeaderBg]         = ImVec4(0.10f, 0.10f, 0.10f, 0.90f);
  colors[ImGuiCol_TableBorderStrong]     = ImVec4(1.00f, 1.00f, 1.00f, 0.16f);
  colors[ImGuiCol_TableBorderLight]      = ImVec4(1.00f, 1.00f, 1.00f, 0.08f);
  colors[ImGuiCol_TableRowBg]            = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
  colors[ImGuiCol_TableRowBgAlt]         = ImVec4(1.00f, 1.00f, 1.00f, 0.025f);

  colors[ImGuiCol_InputTextCursor]       = kColorCyanGlow;
  colors[ImGuiCol_TextLink]              = kColorCyan;
  colors[ImGuiCol_TreeLines]             = ImVec4(1.00f, 1.00f, 1.00f, 0.14f);
  colors[ImGuiCol_DragDropTarget]        = kColorYellow;
  colors[ImGuiCol_DragDropTargetBg]      = ImVec4(0.95f, 0.89f, 0.02f, 0.10f);
  colors[ImGuiCol_UnsavedMarker]         = kColorYellow;

  // Pad/keyboard navigation: the cursor ring is the selection teal.
  colors[ImGuiCol_NavCursor]             = kColorCyanGlow;
  colors[ImGuiCol_NavWindowingHighlight] = ImVec4(0.95f, 0.95f, 0.92f, 0.70f);
  colors[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.01f, 0.01f, 0.01f, 0.40f);

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
    ui_style->achievements.row_rounding      = 4.0f;
    ui_style->achievements.window_padding    = ImVec2(18.0f, 14.0f);
    ui_style->achievements.item_spacing      = ImVec2(10.0f, 7.0f);

    ui_style->toast.title                    = kColorYellow;
    ui_style->toast.text                     = kColorTextWhite;
    ui_style->toast.rounding                 = 12.0f;
    ui_style->toast.background_alpha         = 0.92f;

    ui_style->console.level[1]               = kColorCyan;
    ui_style->console.level[2]               = kColorTextWhite;
    ui_style->console.level[3]               = kColorYellow;
    ui_style->console.completion_bg          = ImVec4(0.05f, 0.05f, 0.05f, 0.96f);
    ui_style->console.completion_highlight   = ImVec4(0.05f, 0.43f, 0.43f, 0.70f);

    ui_style->debug.muted_text               = ImVec4(0.91f, 0.91f, 0.88f, 0.40f);

    // Cyan is the selection color in the retail menus, so live settings read
    // as plain text; yellow flags the ones that wait for a restart.
    ui_style->settings.warning               = kColorYellow;
    ui_style->settings.lifecycle_live        = kColorTextWhite;
    ui_style->settings.lifecycle_restart     = kColorYellow;
    ui_style->settings.lifecycle_init_only   = ImVec4(0.80f, 0.36f, 0.32f, 1.00f);
    ui_style->settings.lifecycle_unknown     = kColorTextWhite;
  }
}

// --- Panels -------------------------------------------------------------------------

namespace {

// Panel geometry, measured off the retail options menu at 1080p.
constexpr float kBezel = 7.0f;           // frosted ring around the body
constexpr float kInner = 16.0f;          // padding inside the dark body
constexpr float kFooterPad = 7.0f;       // vertical padding inside the footer bar
constexpr float kGap = 5.0f;             // bezel showing between body and footer
constexpr float kOuterRounding = 18.0f;
constexpr float kBodyRounding = 10.0f;

struct PanelState {
  bool visible = false;
  ImGuiWindowFlags flags = 0;
  std::span<const Prompt> prompts;
  float footer_px = kSmallSize;  // the footer prompts' font size
};
PanelState g_panel;

// IM_COL32 under the current style alpha, so fading toasts fade the chrome too.
ImU32 Col(int r, int g, int b, int a) {
  return IM_COL32(r, g, b, static_cast<int>(a * ImGui::GetStyle().Alpha));
}

float FooterHeight() {
  return g_panel.footer_px + 2.0f * kFooterPad;
}

void DrawIcon(ImDrawList* draw, ImVec2 c, float r, PanelIcon icon) {
  // The dashed yellow ring every retail title carries.
  constexpr float kPi = 3.14159265f;
  constexpr int kDashes = 10;
  constexpr float kStep = 2.0f * kPi / kDashes;
  for (int i = 0; i < kDashes; ++i) {
    const float a0 = i * kStep - kPi * 0.5f;
    draw->PathArcTo(c, r, a0, a0 + kStep * 0.6f, 6);
    draw->PathStroke(Col(242, 226, 5, 255), 0, std::max(2.0f, r * 0.16f));
  }

  const ImU32 white = Col(240, 240, 235, 255);
  const float s = r * 0.6f;
  switch (icon) {
    case PanelIcon::kCheck: {
      const ImVec2 pts[] = {ImVec2(c.x - s * 0.65f, c.y + s * 0.05f),
                            ImVec2(c.x - s * 0.15f, c.y + s * 0.50f),
                            ImVec2(c.x + s * 0.70f, c.y - s * 0.50f)};
      draw->AddPolyline(pts, 3, white, 0, std::max(2.5f, r * 0.2f));
      break;
    }
    case PanelIcon::kAlert:
      draw->AddRectFilled(ImVec2(c.x - s * 0.17f, c.y - s * 0.75f),
                          ImVec2(c.x + s * 0.17f, c.y + s * 0.25f), white, 1.0f);
      draw->AddCircleFilled(ImVec2(c.x, c.y + s * 0.62f), s * 0.2f, white);
      break;
    case PanelIcon::kPause:
      draw->AddRectFilled(ImVec2(c.x - s * 0.5f, c.y - s * 0.6f),
                          ImVec2(c.x - s * 0.15f, c.y + s * 0.6f), white, 1.0f);
      draw->AddRectFilled(ImVec2(c.x + s * 0.15f, c.y - s * 0.6f),
                          ImVec2(c.x + s * 0.5f, c.y + s * 0.6f), white, 1.0f);
      break;
    case PanelIcon::kInfo:
      draw->AddCircleFilled(ImVec2(c.x, c.y - s * 0.6f), s * 0.2f, white);
      draw->AddRectFilled(ImVec2(c.x - s * 0.17f, c.y - s * 0.25f),
                          ImVec2(c.x + s * 0.17f, c.y + s * 0.75f), white, 1.0f);
      break;
    case PanelIcon::kGamepad: {
      const ImU32 dark = Col(16, 16, 16, 255);
      draw->AddRectFilled(ImVec2(c.x - s * 0.9f, c.y - s * 0.45f),
                          ImVec2(c.x + s * 0.9f, c.y + s * 0.55f), white, s * 0.4f);
      const ImVec2 pad(c.x - s * 0.42f, c.y + s * 0.03f);
      draw->AddRectFilled(ImVec2(pad.x - s * 0.24f, pad.y - s * 0.07f),
                          ImVec2(pad.x + s * 0.24f, pad.y + s * 0.07f), dark);
      draw->AddRectFilled(ImVec2(pad.x - s * 0.07f, pad.y - s * 0.24f),
                          ImVec2(pad.x + s * 0.07f, pad.y + s * 0.24f), dark);
      draw->AddCircleFilled(ImVec2(c.x + s * 0.32f, c.y + s * 0.12f), s * 0.1f, dark);
      draw->AddCircleFilled(ImVec2(c.x + s * 0.55f, c.y - s * 0.08f), s * 0.1f, dark);
      break;
    }
    case PanelIcon::kNone:
      break;
  }
}

float PromptWidth(const Prompt& p) {
  const float label = ImGui::CalcTextSize(p.label).x;
  const float key = ImGui::CalcTextSize(p.key).x;
  const float badge = std::strlen(p.key) == 1 ? g_panel.footer_px + 4.0f : key + 12.0f;
  return label + 6.0f + badge;
}

// "ACCEPT (A)": a light label, then the key on a colored badge -- a circle for
// a single letter as on the pad, a pill for a keyboard key name.
void DrawPrompt(ImDrawList* draw, ImVec2 pos, const Prompt& p, bool hovered) {
  const float label_w = ImGui::CalcTextSize(p.label).x;
  draw->AddText(pos, hovered ? Col(255, 255, 255, 255) : Col(200, 200, 194, 255), p.label);

  const ImVec2 key_size = ImGui::CalcTextSize(p.key);
  const float h = g_panel.footer_px + 4.0f;
  const float x = pos.x + label_w + 6.0f;
  const float y = pos.y - 2.0f;
  const bool round = std::strlen(p.key) == 1;
  const float w = round ? h : key_size.x + 12.0f;
  const ImU32 fill = (p.key_color & ~IM_COL32_A_MASK) |
                     (static_cast<ImU32>(255 * ImGui::GetStyle().Alpha) << IM_COL32_A_SHIFT);
  draw->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), fill, h * 0.5f);
  // Dark lettering on the light badges (neutral, yellow), white on the rest.
  const int luma = (299 * static_cast<int>((p.key_color >> IM_COL32_R_SHIFT) & 0xFF) +
                    587 * static_cast<int>((p.key_color >> IM_COL32_G_SHIFT) & 0xFF) +
                    114 * static_cast<int>((p.key_color >> IM_COL32_B_SHIFT) & 0xFF)) / 1000;
  const ImU32 key_text = luma > 150 ? Col(16, 16, 16, 255) : Col(250, 250, 250, 255);
  draw->AddText(ImVec2(x + (w - key_size.x) * 0.5f, pos.y), key_text, p.key);
}

}  // namespace

bool BeginPanel(const char* id, const char* title, PanelIcon icon, ImGuiWindowFlags flags,
                std::span<const Prompt> prompts, float title_size) {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kBezel + kInner, kBezel + kFooterPad));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  g_panel.flags = flags;
  g_panel.prompts = prompts;
  // A panel drawn larger than the body size (the save picker) gets larger
  // prompts to match; smaller ones keep them readable at the small size.
  g_panel.footer_px = std::max(kSmallSize, kSmallSize * ImGui::GetFontSize() / kBodySize);
  g_panel.visible = ImGui::Begin(id, nullptr,
                                 flags | ImGuiWindowFlags_NoTitleBar |
                                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoCollapse);
  // Begin has latched the padding; children and popups inside use the theme's.
  ImGui::PopStyleVar(2);
  if (!g_panel.visible) {
    return false;
  }

  // The chrome is painted up front from this frame's window rect. The footer
  // is always the same height, so the body's bottom edge is known before any
  // content is laid out; EndPanel just has to land the prompts in that bar.
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetWindowPos();
  const ImVec2 p1(p0.x + ImGui::GetWindowWidth(), p0.y + ImGui::GetWindowHeight());
  draw->AddRectFilled(p0, p1, Col(205, 205, 200, 64), kOuterRounding);
  draw->AddRect(ImVec2(p0.x + 1.0f, p0.y + 1.0f), ImVec2(p1.x - 1.0f, p1.y - 1.0f),
                Col(236, 236, 230, 170), kOuterRounding, 0, 2.0f);
  float body_bottom = p1.y - kBezel;
  if (!prompts.empty()) {
    const float bar_top = body_bottom - FooterHeight();
    draw->AddRectFilled(ImVec2(p0.x + kBezel, bar_top), ImVec2(p1.x - kBezel, body_bottom),
                        Col(18, 18, 18, 240), kBodyRounding);
    body_bottom = bar_top - kGap;
  }
  draw->AddRectFilled(ImVec2(p0.x + kBezel, p0.y + kBezel), ImVec2(p1.x - kBezel, body_bottom),
                      Col(8, 8, 8, 232), kBodyRounding);

  // Title row: the ring and the heavy yellow caps.
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (kInner * 0.6f - kFooterPad));
  if (title && title[0] != '\0') {
    // The ring stands taller than the caps, as on the retail titles, with the
    // title centered on it; small titles (toasts) keep a ring that still reads.
    const float r = std::max(title_size * 0.5f, 13.0f);
    const float row_h = std::max(r * 2.0f, title_size);
    const float row_y = ImGui::GetCursorPosY();
    if (icon != PanelIcon::kNone) {
      const ImVec2 at = ImGui::GetCursorScreenPos();
      ImGui::Dummy(ImVec2(r * 2.0f, row_h));
      DrawIcon(draw, ImVec2(at.x + r, at.y + row_h * 0.5f), r, icon);
      ImGui::SameLine(0.0f, title_size * 0.3f);
    }
    // Font Alt's caps sit centered on its line, so centering the line centers them.
    ImGui::SetCursorPosY(row_y + (row_h - title_size) * 0.5f);
    ImGui::PushFont(g_fonts.title, title_size);
    ImGui::PushStyleColor(ImGuiCol_Text, kColorYellow);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::PopFont();
  }
  return true;
}

int EndPanel() {
  int clicked = -1;
  const std::span<const Prompt> prompts = g_panel.prompts;
  if (g_panel.visible) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetWindowPos();
    const ImVec2 p1(p0.x + ImGui::GetWindowWidth(), p0.y + ImGui::GetWindowHeight());
    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    const float content_end = ImGui::GetCursorScreenPos().y - spacing;

    if (prompts.empty()) {
      // Bottom padding of the body; the window padding only covers kFooterPad.
      ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, content_end));
      ImGui::Dummy(ImVec2(0.0f, kInner - kFooterPad));
    } else {
      // The footer bar's text line, where BeginPanel painted the bar. An
      // auto-sized window settles on exactly that spot from the content's end;
      // a fixed-size one has it pinned to the bottom edge.
      const float pinned = p1.y - kBezel - kFooterPad - g_panel.footer_px;
      float text_y = content_end + kInner + kGap + kFooterPad;
      if (!(g_panel.flags & ImGuiWindowFlags_AlwaysAutoResize)) {
        text_y = std::max(text_y, pinned);
      }

      ImGui::PushFont(g_fonts.body, g_panel.footer_px);
      float total = 0.0f;
      for (const Prompt& p : prompts) {
        total += PromptWidth(p);
      }
      constexpr float kPromptGap = 20.0f;
      total += kPromptGap * static_cast<float>(prompts.size() - 1);
      float x = p1.x - kBezel - kInner - total;
      for (size_t i = 0; i < prompts.size(); ++i) {
        const float w = PromptWidth(prompts[i]);
        ImGui::SetCursorScreenPos(ImVec2(x, text_y));
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::InvisibleButton("##prompt", ImVec2(w, g_panel.footer_px))) {
          clicked = static_cast<int>(i);
        }
        ImGui::PopID();
        DrawPrompt(draw, ImVec2(x, text_y), prompts[i], ImGui::IsItemHovered());
        x += w + kPromptGap;
      }
      ImGui::PopFont();
    }
  }
  ImGui::End();
  g_panel = {};
  return clicked;
}

float PanelBottomReserve() {
  // The content region ends kBezel + kFooterPad above the window edge. Without
  // a footer the body needs kInner there; with one, the bar and the bezel gap
  // above it sit between the body and that edge too (see EndPanel).
  if (g_panel.prompts.empty()) {
    return kInner - kFooterPad;
  }
  return kInner + kGap + kFooterPad + g_panel.footer_px;
}

void PanelSubtitle(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.75f, 0.72f, 1.00f));
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
}

ImU32 DrawListRow(ImDrawList* draw, ImVec2 low, ImVec2 high, bool selected) {
  if (selected) {
    // Saturated teal at the cursor edge fading to a nearly clear dark teal,
    // with a lit top edge and the triangle cursor drawn so it needs no glyph.
    draw->AddRectFilledMultiColor(low, high, Col(28, 155, 152, 220), Col(12, 90, 90, 40),
                                  Col(12, 90, 90, 40), Col(28, 155, 152, 220));
    draw->AddLine(low, ImVec2(high.x, low.y), Col(120, 240, 235, 90));
    const float cy = (low.y + high.y) * 0.5f;
    draw->AddTriangleFilled(ImVec2(low.x + 8.0f, cy - 6.0f), ImVec2(low.x + 8.0f, cy + 6.0f),
                            ImVec2(low.x + 18.0f, cy), ImGui::GetColorU32(kColorCyanGlow));
    return ImGui::GetColorU32(kColorCyanGlow);
  }
  // The groove under each row: a dark rule over a faint light one.
  draw->AddLine(ImVec2(low.x, high.y - 1.0f), ImVec2(high.x, high.y - 1.0f), Col(0, 0, 0, 170));
  draw->AddLine(ImVec2(low.x, high.y), ImVec2(high.x, high.y), Col(255, 255, 255, 30));
  return ImGui::GetColorU32(ImGuiCol_Text);
}

void SegmentMeter(float fraction, float width, int segments) {
  const float h = 6.0f;
  const float gap = 3.0f;
  ImGui::Dummy(ImVec2(width, h));
  const ImVec2 at = ImGui::GetItemRectMin();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const float seg = (width - gap * static_cast<float>(segments - 1)) / static_cast<float>(segments);
  fraction = std::clamp(fraction, 0.0f, 1.0f);
  for (int i = 0; i < segments; ++i) {
    const float x = at.x + static_cast<float>(i) * (seg + gap);
    const bool lit = (static_cast<float>(i) + 0.5f) / static_cast<float>(segments) <= fraction;
    draw->AddRectFilled(ImVec2(x, at.y), ImVec2(x + seg, at.y + h),
                        lit ? Col(51, 214, 214, 255) : Col(20, 70, 70, 200));
  }
}

} // namespace mcla::ui
