#ifndef REXGLUE_HAS_XEO3_TARGET
// larecomp - Save picker
#include "larecomp_profile_picker.h"

#include "larecomp_profile_store.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <span>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xam/user_profile.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include "mc_engine/ui/mcla_gamepad.h"
#include "mc_engine/ui/mcla_imgui_theme.h"
#include "larecomp_version.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

REXCVAR_DEFINE_BOOL(profile_auto_select, false, "MCLA",
    "Start with the save named by user_name and never show the save picker, even when several "
    "saves exist. The picker's 'Always use this save' box sets it; hold Shift while the game "
    "starts to see the picker once anyway.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace larecomp {

namespace {

namespace fs = std::filesystem;
namespace pf = larecomp::profiles;

constexpr char kUserName[] = "user_name";

// What a typed name becomes before it names a directory. The fork has the real
// function; the stock SDK has no profile names at all and never reaches this
// file's logic (see PrepareProfileChoice). Detected at compile time, like the
// save porter's ProfileDirName, so one source builds against both.
template <typename Profile>
std::string SanitizeWith(std::string_view raw) {
  if constexpr (requires { Profile::SanitizeName(raw); }) {
    return std::string(Profile::SanitizeName(raw));
  } else {
    return pf::SanitizeName(raw);
  }
}

std::string Sanitize(std::string_view raw) {
  return SanitizeWith<rex::system::xam::UserProfile>(raw);
}

// True when user_name came from the command line or the environment: someone
// asked for that exact profile, so there is nothing to ask. The source tracking
// is the fork's; without it every value counts as coming from the config.
bool NameGivenFromOutside() {
  auto probe = [](auto name) -> bool {
    if constexpr (requires { rex::cvar::GetFlagSource(name); }) {
      using Source = decltype(rex::cvar::GetFlagSource(name));
      const Source source = rex::cvar::GetFlagSource(name);
      return source == Source::kCommandLine || source == Source::kEnvironment;
    } else {
      return false;
    }
  };
  return probe(std::string_view(kUserName));
}

// Shift held at launch brings the picker back for a player who ticked "always".
bool ShiftHeld() {
#if defined(_WIN32)
  return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
#else
  return false;
#endif
}

bool DirExists(const fs::path& root, const std::string& name) {
  std::error_code ec;
  return !name.empty() && fs::exists(root / pf::PathFromUtf8(name), ec);
}

int IndexOf(const std::vector<pf::Profile>& choices, std::string_view name) {
  for (size_t i = 0; i < choices.size(); ++i) {
    if (pf::SameName(choices[i].name, name)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

struct Plan {
  bool ask = false;
  bool forced = false;               // Shift was held
  bool remember = false;             // profile_auto_select as found
  fs::path root;                     // the user data root
  std::vector<pf::Profile> choices;  // newest first, a not-yet-saved user_name ahead of them
  std::string preselect;
};

Plan g_plan;

std::string FormatTime(fs::file_time_type when) {
  using namespace std::chrono;
  const std::time_t t = system_clock::to_time_t(clock_cast<system_clock>(when));
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &t);
#else
  localtime_r(&t, &local);
#endif
  char buffer[40];
  std::strftime(buffer, sizeof(buffer), "%d %b %Y  %H:%M", &local);
  return buffer;
}

class ProfilePickerDialog final : public rex::ui::ImGuiDialog {
 public:
  ProfilePickerDialog(rex::ui::ImGuiDrawer* drawer, Plan plan, fs::path config_path,
                      std::function<void()> on_finished)
      : ImGuiDialog(drawer),
        plan_(std::move(plan)),
        config_path_(std::move(config_path)),
        on_finished_(std::move(on_finished)),
        remember_(plan_.remember) {
    selected_ = std::max(0, IndexOf(plan_.choices, plan_.preselect));
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!logged_first_draw_) {
      logged_first_draw_ = true;
      REXLOG_INFO("Save picker: first draw, display {}x{}", io.DisplaySize.x, io.DisplaySize.y);
    }

    // The SDK's input driver does not exist yet, so the pad is read here.
    pad_.Feed(io);

    // Landscape like the retail menus: the list runs wide and short, the
    // actions sit in the footer bar rather than in buttons. Drawn about a
    // quarter larger than the other panels: it is the first screen of the game.
    constexpr float kScale = 1.265f;
    const float width = std::min(962.0f, std::max(600.0f, io.DisplaySize.x - 64.0f));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
    // The row highlight is drawn by hand, so the Selectable's own fill is off.
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.05f, 0.35f, 0.35f, 0.40f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.08f, 0.50f, 0.48f, 0.55f));
    ImGui::PushFont(nullptr, mcla::ui::kBodySize * kScale);

    const ImU32 play_color = can_play_ ? mcla::ui::kPromptAccept : IM_COL32(70, 70, 66, 255);
    const mcla::ui::Prompt pad_prompts[] = {
        {"SELECT", "D-PAD"},
        {"ALWAYS USE", "Y", mcla::ui::kPromptY},
        {"PLAY", "A", play_color},
    };
    const mcla::ui::Prompt key_prompts[] = {
        {"SELECT", "UP/DOWN"},
        {"PLAY", "ENTER", play_color},
    };
    const bool pad = pad_.connected();
    const std::span<const mcla::ui::Prompt> prompts =
        pad ? std::span<const mcla::ui::Prompt>(pad_prompts)
            : std::span<const mcla::ui::Prompt>(key_prompts);
    const int play_prompt = static_cast<int>(prompts.size()) - 1;

    bool accept = false;
    // BeginPanel/EndPanel are paired unconditionally, like Begin/End.
    if (mcla::ui::BeginPanel("##larecomp_profile_picker", "SELECT PROFILE",
                             mcla::ui::PanelIcon::kCheck,
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_AlwaysAutoResize,
                             prompts, mcla::ui::kTitleSize * kScale)) {
      panel_pos_ = ImGui::GetWindowPos();
      panel_size_ = ImGui::GetWindowSize();
      accept = DrawBody(io);
    }
    const int clicked = mcla::ui::EndPanel();
    if (clicked == play_prompt && can_play_) {
      accept = true;
    } else if (pad && clicked == 1) {
      remember_ = !remember_;
    }

    ImGui::PopFont();
    ImGui::PopStyleColor(3);

    DrawBanner();
    DrawVersion(io);

    if (accept) {
      Accept();
    }
  }

 private:
  int NewRow() const { return static_cast<int>(plan_.choices.size()); }

  // Why the typed name cannot start a new save, or empty when it can.
  std::string ProblemWith(const std::string& name, bool typed_anything) const {
    if (name.empty()) {
      return typed_anything ? "Enter at least one usable character." : std::string();
    }
    if (pf::IsReservedName(name)) {
      return "That name is taken by one of the game's own folders.";
    }
    if (const int i = IndexOf(plan_.choices, name); i >= 0) {
      return "A save named '" + plan_.choices[i].name + "' already exists - pick it in the list.";
    }
    return {};
  }

  // Returns true when the player asked to go ahead with the current selection.
  bool DrawBody(ImGuiIO& io) {
    mcla::ui::PanelSubtitle(plan_.forced
                                ? "Choose a profile to load, or create a new career. (Shift held)"
                                : "Choose a profile to load, or create a new career.");

    bool accept = false;

    // --- the saves ------------------------------------------------------------------
    // Sized from the rows themselves: a list sized without its padding hides
    // the last row ("New profile") behind a scrollbar. The retail list has no
    // box of its own; the rows sit straight on the panel glass.
    const int rows = NewRow() + 1;
    const int shown = std::min(rows, 6);
    const float row_height = ImGui::GetTextLineHeight() + 14.0f;
    const float list_pad = 2.0f;
    const float list_height = row_height * static_cast<float>(shown) + 2.0f * list_pad;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, list_pad));
    ImGui::BeginChild("##saves", ImVec2(0.0f, list_height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();  // the padding is read as the child opens
    ImDrawList* draw = ImGui::GetWindowDrawList();
    for (int i = 0; i < rows; ++i) {
      const bool is_new = i == NewRow();
      ImGui::PushID(i);
      if (ImGui::Selectable("##row", selected_ == i, ImGuiSelectableFlags_AllowDoubleClick,
                            ImVec2(0.0f, row_height))) {
        selected_ = i;
        if (is_new) {
          focus_name_ = true;
        } else if (ImGui::IsMouseDoubleClicked(0)) {
          accept = true;
        }
      }
      if (selected_ == i && scroll_to_selected_) {
        ImGui::SetScrollHereY(0.5f);
        scroll_to_selected_ = false;
      }
      const ImVec2 low = ImGui::GetItemRectMin();
      const ImVec2 high = ImGui::GetItemRectMax();
      const float text_y = low.y + (high.y - low.y - ImGui::GetTextLineHeight()) * 0.5f;
      // MCLA selection: teal text glowing over a dark teal bar, not a solid
      // cyan fill with dark text on it.
      const bool is_selected = (selected_ == i);
      const ImU32 text_col = mcla::ui::DrawListRow(draw, low, high, is_selected);
      const ImU32 dim_col = is_selected ? text_col : ImGui::GetColorU32(ImGuiCol_TextDisabled);

      const float text_x = low.x + 26.0f;
      if (is_new) {
        draw->AddText(ImVec2(text_x, text_y), text_col, "+  NEW PROFILE");
      } else {
        const pf::Profile& save = plan_.choices[i];
        draw->AddText(ImVec2(text_x, text_y), text_col, save.name.c_str());
        const std::string right = save.has_save ? FormatTime(save.last_played) : "No save data";
        const float right_width = ImGui::CalcTextSize(right.c_str()).x;
        draw->AddText(ImVec2(high.x - right_width - 12.0f, text_y), dim_col, right.c_str());
      }
      ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();  // item spacing

    // --- a new gamertag ---------------------------------------------------------------
    bool submitted = false;
    std::string new_name;
    std::string problem;
    if (selected_ == NewRow()) {
      if (focus_name_) {
        ImGui::SetKeyboardFocusHere();
        focus_name_ = false;
      }
      ImGui::SetNextItemWidth(-1.0f);
      submitted = ImGui::InputTextWithHint("##new_name", "Gamertag for the new save", name_buffer_,
                                           sizeof(name_buffer_),
                                           ImGuiInputTextFlags_EnterReturnsTrue);
      new_name = Sanitize(name_buffer_);
      problem = ProblemWith(new_name, name_buffer_[0] != '\0');
      if (!problem.empty()) {
        // Wrapped: the message names the clashing save and outgrows the window.
        // The game's warning color is its own yellow, not a separate orange.
        ImGui::PushStyleColor(ImGuiCol_Text, mcla::ui::kColorYellow);
        ImGui::TextWrapped("%s", problem.c_str());
        ImGui::PopStyleColor();
      } else if (!new_name.empty()) {
        ImGui::TextDisabled("Saved as: %s", new_name.c_str());
      } else if (pad_.connected()) {
        // A pad has no keys to type with: A fills in a free name to start from.
        ImGui::TextDisabled("Press A for a suggested name, or type one.");
      } else {
        ImGui::TextDisabled("Starts from the beginning, in a folder of its own.");
      }
    }

    // --- remember ---------------------------------------------------------------------
    ImGui::Checkbox("Always use this save", &remember_);
    ImGui::SameLine();
    ImGui::TextDisabled("(hold Shift while the game starts to pick again)");

    // PLAY lives in the footer bar; OnDraw reads this to arm and color it.
    can_play_ = selected_ != NewRow() || (!new_name.empty() && problem.empty());

    // --- keyboard ---------------------------------------------------------------------
    if (!io.WantTextInput) {
      const int step = (ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? 1 : 0) -
                       (ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? 1 : 0);
      if (step != 0) {
        selected_ = std::clamp(selected_ + step, 0, NewRow());
        scroll_to_selected_ = true;
        focus_name_ = selected_ == NewRow();
      }
    }
    const bool enter = !io.WantTextInput && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                                             ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));

    // --- pad --------------------------------------------------------------------------
    // Read even while the name field has the keyboard: the pad is not typing.
    // Landing on "New profile" with the pad leaves the field unfocused; moving
    // off it stops drawing the field, which drops its focus.
    const int pad_step = (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown) ||
                                  ImGui::IsKeyPressed(ImGuiKey_GamepadLStickDown)
                              ? 1
                              : 0) -
                         (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp) ||
                                  ImGui::IsKeyPressed(ImGuiKey_GamepadLStickUp)
                              ? 1
                              : 0);
    if (pad_step != 0) {
      selected_ = std::clamp(selected_ + pad_step, 0, NewRow());
      scroll_to_selected_ = true;
      focus_name_ = false;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false)) {
      remember_ = !remember_;
    }
    bool pad_accept = false;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false)) {
      if (selected_ == NewRow() && name_buffer_[0] == '\0') {
        SuggestName();  // the next A plays it
      } else {
        pad_accept = true;
      }
    }

    return can_play_ && (accept || submitted || enter || pad_accept);
  }

  // The LARecomp banner above the panel, as in the artwork: "LA" in the logo
  // red and "Recomp" in the menu white, set in Font Alt with the face's own
  // kerning (which opens the gap between A and R), and the credit under it
  // in the Los Angeles face, kerned too. The credit runs from the foot of the
  // R to the foot of the m: its B's bottom-left under the R's, its n's
  // bottom-right under the m's. Its descenders bottom out with the p's. Every
  // measure comes from the faces, so replacement fonts keep the fit. As wide
  // as the panel, or smaller when the screen is short.
  void DrawBanner() {
    const mcla::ui::MclaFonts& fonts = mcla::ui::GetFonts();
    ImFont* logo = fonts.banner ? fonts.banner : fonts.title;
    ImFont* script = fonts.banner_credit ? fonts.banner_credit : fonts.script;
    if (!logo || !script || panel_size_.x <= 0.0f) return;
    const char* word = mcla::ui::kBannerWord;      // "LARecomp"
    const char* credit = mcla::ui::kBannerCredit;  // "By mzzvxm & BadassBaboon"
    const size_t word_len = std::strlen(word);
    const size_t credit_len = std::strlen(credit);
    constexpr float kRef = 100.0f;  // laid out at this size, then scaled
    constexpr float kGap = 14.0f;   // between the banner and the panel
    constexpr float kTop = 16.0f;   // kept free above the banner

    // Glyph ink box edges at a drawn size. The fixed atlas holds each face at
    // one size and GetFontBaked hands back that one, so its numbers are in
    // the loaded size's pixels and are scaled to the drawn one.
    auto ink = [](ImFont* font, float px, char c, int edge) {
      ImFontBaked* baked = font->GetFontBaked(px);
      const ImFontGlyph* g = baked->FindGlyph(static_cast<unsigned char>(c));
      const float v[] = {g->X0, g->X1, g->Y0, g->Y1};
      return v[edge] * px / baked->Size;
    };
    enum { kLeft, kRight, kTopEdge, kBottom };
    // A slanted glyph's right edge where it stands on the baseline: its ink
    // box's right edge is the top corner, one slant-run further right. (The
    // left edge needs nothing: a right lean puts the box's left at the foot.)
    auto foot_right = [&](ImFont* font, float px, char c) {
      ImFontBaked* baked = font->GetFontBaked(px);
      const float baseline = baked->Ascent * px / baked->Size;
      return ink(font, px, c, kRight) -
             mcla::ui::ItalicSlant(font) * (baseline - ink(font, px, c, kTopEdge));
    };

    // The word at a size, relative to its origin: where the R's and the m's
    // feet are, and the ink box.
    struct Word {
      std::vector<float> at;
      float r_left, m_foot, left, right, top, bottom;
    };
    auto lay_word = [&](float px) {
      Word w;
      w.at = mcla::ui::KernedOrigins(logo, px, word, nullptr);
      w.r_left = w.at[2] + ink(logo, px, word[2], kLeft);
      w.m_foot = w.at[word_len - 2] + foot_right(logo, px, word[word_len - 2]);
      w.left = w.at[0] + ink(logo, px, word[0], kLeft);
      w.right = w.at[word_len - 1] + ink(logo, px, word[word_len - 1], kRight);
      w.top = ink(logo, px, word[0], kTopEdge);
      w.bottom = ink(logo, px, word[word_len - 1], kBottom);
      return w;
    };
    // The credit's width from its first foot to its last at a size (it
    // scales linearly).
    auto credit_ink = [&](float px) {
      const std::vector<float> at = mcla::ui::KernedOrigins(script, px, credit, nullptr);
      return at[credit_len - 1] + foot_right(script, px, credit[credit_len - 1]) -
             ink(script, px, credit[0], kLeft);
    };

    // Fit: the word's ink as wide as the panel, or as tall as the room above.
    const Word ref = lay_word(kRef);
    const float fit_width = panel_size_.x * 0.97f * kRef / (ref.right - ref.left);
    const float fit_height = (panel_pos_.y - kGap - kTop) * kRef / (ref.bottom - ref.top);
    const float px = std::min(fit_width, fit_height);
    if (px < 12.0f) return;

    const Word w = lay_word(px);
    const float credit_px = (w.m_foot - w.r_left) * kRef / credit_ink(kRef);
    const float x = panel_pos_.x + (panel_size_.x - (w.right - w.left)) * 0.5f - w.left;
    const float y = panel_pos_.y - kGap - w.bottom;
    // The credit's descender ('y') bottoms out where the p's does.
    const float credit_bottom = ink(script, credit_px, 'y', kBottom);
    const ImVec2 credit_pos(x + w.r_left - ink(script, credit_px, credit[0], kLeft),
                            y + w.bottom - credit_bottom);

    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    mcla::ui::DrawKerned(draw, logo, px, ImVec2(x, y), word, [](size_t i) -> ImU32 {
      return i < 2 ? IM_COL32(0xEC, 0x04, 0x0B, 0xFF) : IM_COL32(0xE8, 0xE8, 0xE0, 0xFF);
    });
    mcla::ui::DrawKerned(draw, script, credit_px, credit_pos, credit,
                         [](size_t) -> ImU32 { return IM_COL32(0xE8, 0xE8, 0xE0, 0xFF); });
  }

  // The release, flush with the bottom-right corner whatever its length.
  static void DrawVersion(ImGuiIO& io) {
    ImFont* font = mcla::ui::GetFonts().body;
    if (!font) return;
    constexpr float kPx = 18.0f;
    constexpr float kMargin = 24.0f;
    const float w = font->CalcTextSizeA(kPx, FLT_MAX, 0.0f, larecomp::kVersionLabel).x;
    ImGui::GetBackgroundDrawList()->AddText(
        font, kPx, ImVec2(io.DisplaySize.x - kMargin - w, io.DisplaySize.y - kMargin - kPx),
        IM_COL32(0x8C, 0x8C, 0x85, 0xFF), larecomp::kVersionLabel);
  }

  // The first free "Player", "Player2", ... for a new save made from the pad.
  void SuggestName() {
    for (int n = 1; n < 100; ++n) {
      const std::string candidate = n == 1 ? "Player" : "Player" + std::to_string(n);
      if (ProblemWith(Sanitize(candidate), true).empty()) {
        std::snprintf(name_buffer_, sizeof(name_buffer_), "%s", candidate.c_str());
        return;
      }
    }
  }

  void Accept() {
    const bool fresh = selected_ == NewRow();
    const std::string name = fresh ? Sanitize(name_buffer_) : plan_.choices[selected_].name;

    rex::cvar::SetFlagByName(kUserName, name);
    rex::cvar::SetFlagByName("profile_auto_select", remember_ ? "true" : "false");

    // The content manager renames the directory the marker names to user_name
    // whenever user_name is a directory that does not exist yet. With several
    // saves that is the wrong move: a NEW save would take over the last-used one
    // under another name, and the old name would vanish from the list. Naming
    // the choice in the marker first makes the move a no-op, and keeps the
    // manager from overwriting the marker with a name it merely warned about.
    if (!pf::WriteMarker(plan_.root, name)) {
      REXLOG_WARN("Save picker: could not write {}; the SDK may rename a save",
                  (plan_.root / ".profile").string());
    }

    // Only the two lines this dialog owns. SaveConfig would persist every cvar
    // that differs from its default whatever set it, and drop the lines of cvars
    // this process has not registered. Without the tick nothing about the choice
    // is written, so the next launch asks again.
    std::vector<pf::ConfigEdit> edits;
    if (remember_) {
      edits.push_back({kUserName, pf::QuoteTomlString(name)});
      edits.push_back({"profile_auto_select", std::string("true")});
    } else if (plan_.remember) {
      edits.push_back({"profile_auto_select", std::nullopt});
    }
    if (!edits.empty() && !config_path_.empty() && !pf::EditConfig(config_path_, edits)) {
      REXLOG_WARN("Save picker: could not update {}", config_path_.string());
    }

    REXLOG_INFO("Save picker: playing '{}' ({}), always use: {}", name,
                fresh ? "new save" : "existing save", remember_ ? "yes" : "no");

    auto done = std::move(on_finished_);
    Close();
    if (done) {
      done();
    }
  }

  Plan plan_;
  fs::path config_path_;
  std::function<void()> on_finished_;
  bool remember_ = false;
  int selected_ = 0;
  char name_buffer_[64] = {};
  bool focus_name_ = false;
  bool scroll_to_selected_ = true;
  bool can_play_ = true;
  bool logged_first_draw_ = false;
  // The panel's rect this frame, for the banner placed above it.
  ImVec2 panel_pos_{};
  ImVec2 panel_size_{};
  // Its destructor ends the SDL gamepad session as the dialog is deleted on
  // close, before the deferred resume lets the SDK input driver start its own.
  mcla::ui::MenuGamepad pad_;
};

}  // namespace

void PrepareProfileChoice(const fs::path& user_data_root) {
  g_plan = {};

  // Only the SDK fork files saves under a profile name. The stock SDK uses the
  // hex XUID and has no user_name cvar, so there is nothing to choose between.
  if (!rex::cvar::GetFlagInfo(kUserName)) {
    REXLOG_INFO("Save picker: off, this SDK has no user_name profile");
    return;
  }
  if (rex::cvar::Query<bool>("headless")) {
    return;
  }
  if (NameGivenFromOutside()) {
    REXLOG_INFO("Save picker: off, user_name was given on the command line or environment");
    return;
  }

  // The content manager resolves its root to an absolute path; so must the marker.
  std::error_code ec;
  fs::path root = fs::absolute(user_data_root, ec);
  if (ec || root.empty()) {
    root = user_data_root;
  }

  const std::string configured = Sanitize(rex::cvar::GetFlagByName(kUserName));
  const std::string marker = pf::ReadMarker(root);
  std::vector<pf::Profile> choices = pf::Enumerate(root, &Sanitize);
  const size_t saves = choices.size();
  const bool forced = ShiftHeld();
  const bool remember = REXCVAR_GET(profile_auto_select);

  // user_name names a directory that is not there while the marker points at one
  // that is: the content manager will rename the second to the first. That is
  // what editing user_name (or renaming in Settings) has always meant, so a
  // picker has no business in it.
  const bool rename_pending = !configured.empty() && !DirExists(root, configured) &&
                              !marker.empty() && !pf::SameName(marker, configured) &&
                              DirExists(root, marker);

  // A name chosen earlier whose save was never written (a new save the player
  // quit before saving) is still one of the saves to pick from.
  if (!configured.empty() && !rename_pending && IndexOf(choices, configured) < 0) {
    pf::Profile pending;
    pending.name = configured;
    choices.insert(choices.begin(), std::move(pending));
  }

  bool ask = false;
  const char* reason = "";
  if (saves == 0) {
    reason = "no saves yet, the SDK asks for a name";
  } else if (forced) {
    ask = true;
    reason = "Shift held";
  } else if (configured.empty()) {
    ask = true;
    reason = "no user_name";
  } else if (rename_pending) {
    reason = "user_name was edited, the SDK renames the old save to it";
  } else if (remember) {
    reason = "profile_auto_select";
  } else if (choices.size() >= 2) {
    ask = true;
    reason = "several saves";
  } else {
    reason = "a single save";
  }

  std::string names;
  for (const pf::Profile& p : choices) {
    names += (names.empty() ? "" : ", ") + p.name + (p.has_save ? "" : " (no save yet)");
  }
  REXLOG_INFO("Save picker: {} save(s) under '{}' [{}], user_name='{}', .profile='{}': {} ({})",
              saves, root.string(), names, configured, marker, ask ? "asking" : "not asking",
              reason);
  if (!ask) {
    return;
  }

  g_plan.ask = true;
  g_plan.forced = forced;
  g_plan.remember = remember;
  g_plan.root = std::move(root);
  // The first row: the save played last, which is what pressing Enter should
  // continue. user_name is not the better guess: it is also whatever an earlier
  // session's settings save happened to write down, and a name left over from a
  // test run would send the player into a stale save. A name chosen but not yet
  // saved sits ahead of them, being the newest intent.
  g_plan.preselect = choices.front().name;
  g_plan.choices = std::move(choices);

  // The base's own prompt opens whenever user_name is empty. Seeding it keeps
  // that prompt shut, and keeps a real save in place should the picker never get
  // a window to draw on.
  if (configured.empty()) {
    rex::cvar::SetFlagByName(kUserName, g_plan.preselect);
  }
}

bool ShowProfilePicker(rex::ui::ImGuiDrawer* drawer, const rex::PathConfig& paths,
                       std::function<void()> on_finished) {
  if (!g_plan.ask) {
    return false;
  }
  Plan plan = std::move(g_plan);
  g_plan = {};
  if (!drawer) {
    REXLOG_WARN("Save picker: no UI to show it on, keeping '{}'",
                rex::cvar::GetFlagByName(kUserName));
    return false;
  }
  REXLOG_INFO("Save picker: showing {} choice(s), '{}' preselected", plan.choices.size(),
              plan.preselect);
  // Shows itself on construction and deletes itself when closed, like every
  // rex::ui::ImGuiDialog.
  (void)new ProfilePickerDialog(drawer, std::move(plan), paths.config_path, std::move(on_finished));
  return true;
}

}  // namespace larecomp

#endif  // REXGLUE_HAS_XEO3_TARGET
