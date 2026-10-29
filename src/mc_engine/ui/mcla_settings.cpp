#include "mcla_settings.h"
#include "mcla_imgui_theme.h"

#include <rex/cvar.h>
#include <rex/string.h>
#include <rex/string/numeric.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>

namespace mcla::ui {

namespace {

// --- What the SDK overlay shows, and in which category (same rules) -----------------

bool CategoryIsPathOrChild(std::string_view category, std::string_view path) {
  if (category == path) {
    return true;
  }
  return category.size() > path.size() && category.compare(0, path.size(), path) == 0 &&
         category[path.size()] == '/';
}

std::string EffectiveGraphicsBackend() {
  const std::string backend = rex::cvar::GetFlagByName("graphics_backend");
  if (backend == "d3d12" || backend == "vulkan") {
    return backend;
  }
#if REX_HAS_D3D12
  return "d3d12";
#elif REX_HAS_VULKAN
  return "vulkan";
#else
  return "";
#endif
}

bool IsD3D12Setting(const rex::cvar::FlagEntry& entry) {
  const std::string_view category(entry.category);
  return CategoryIsPathOrChild(category, "GPU/D3D12") ||
         CategoryIsPathOrChild(category, "UI/D3D12") ||
         entry.name.find("d3d12") != std::string::npos;
}

bool IsVulkanSetting(const rex::cvar::FlagEntry& entry) {
  const std::string_view category(entry.category);
  return CategoryIsPathOrChild(category, "GPU/Vulkan") ||
         CategoryIsPathOrChild(category, "UI/Vulkan") ||
         entry.name.find("vulkan") != std::string::npos ||
         entry.name == "non_seamless_cube_map" || entry.name == "present_render_pass_clear";
}

// The SDK's settings_hide_inactive_backend_cvars still decides this.
bool ShouldShow(const rex::cvar::FlagEntry& entry) {
  if (rex::cvar::GetFlagByName("settings_hide_inactive_backend_cvars") == "false") {
    return true;
  }
  const std::string backend = EffectiveGraphicsBackend();
  if (backend == "d3d12") return !IsVulkanSetting(entry);
  if (backend == "vulkan") return !IsD3D12Setting(entry);
  return true;
}

bool IsKeybindCategory(const std::string& category) {
  return CategoryIsPathOrChild(category, "Input/Keybinds");
}

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

rex::ui::VirtualKey ImGuiKeyToVirtualKey(ImGuiKey key) {
  using VK = rex::ui::VirtualKey;
  if (key >= ImGuiKey_A && key <= ImGuiKey_Z) {
    return static_cast<VK>(static_cast<uint16_t>(VK::kA) + (key - ImGuiKey_A));
  }
  if (key >= ImGuiKey_0 && key <= ImGuiKey_9) {
    return static_cast<VK>(static_cast<uint16_t>(VK::k0) + (key - ImGuiKey_0));
  }
  if (key >= ImGuiKey_F1 && key <= ImGuiKey_F24) {
    return static_cast<VK>(static_cast<uint16_t>(VK::kF1) + (key - ImGuiKey_F1));
  }
  if (key >= ImGuiKey_Keypad0 && key <= ImGuiKey_Keypad9) {
    return static_cast<VK>(static_cast<uint16_t>(VK::kNumpad0) + (key - ImGuiKey_Keypad0));
  }
  switch (key) {
    case ImGuiKey_Space: return VK::kSpace;
    case ImGuiKey_Enter: return VK::kReturn;
    case ImGuiKey_Escape: return VK::kEscape;
    case ImGuiKey_Tab: return VK::kTab;
    case ImGuiKey_Backspace: return VK::kBack;
    case ImGuiKey_Delete: return VK::kDelete;
    case ImGuiKey_Insert: return VK::kInsert;
    case ImGuiKey_Home: return VK::kHome;
    case ImGuiKey_End: return VK::kEnd;
    case ImGuiKey_PageUp: return VK::kPrior;
    case ImGuiKey_PageDown: return VK::kNext;
    case ImGuiKey_LeftArrow: return VK::kLeft;
    case ImGuiKey_RightArrow: return VK::kRight;
    case ImGuiKey_UpArrow: return VK::kUp;
    case ImGuiKey_DownArrow: return VK::kDown;
    case ImGuiKey_LeftShift:
    case ImGuiKey_RightShift: return VK::kShift;
    case ImGuiKey_LeftCtrl:
    case ImGuiKey_RightCtrl: return VK::kControl;
    case ImGuiKey_LeftAlt:
    case ImGuiKey_RightAlt: return VK::kMenu;
    case ImGuiKey_CapsLock: return VK::kCapital;
    case ImGuiKey_NumLock: return VK::kNumLock;
    case ImGuiKey_ScrollLock: return VK::kScroll;
    case ImGuiKey_PrintScreen: return VK::kSnapshot;
    case ImGuiKey_Pause: return VK::kPause;
    case ImGuiKey_GraveAccent: return VK::kOem3;
    case ImGuiKey_Minus: return VK::kOemMinus;
    case ImGuiKey_Equal: return VK::kOemPlus;
    case ImGuiKey_LeftBracket: return VK::kOem4;
    case ImGuiKey_RightBracket: return VK::kOem6;
    case ImGuiKey_Backslash: return VK::kOem5;
    case ImGuiKey_Semicolon: return VK::kOem1;
    case ImGuiKey_Apostrophe: return VK::kOem7;
    case ImGuiKey_Comma: return VK::kOemComma;
    case ImGuiKey_Period: return VK::kOemPeriod;
    case ImGuiKey_Slash: return VK::kOem2;
    case ImGuiKey_KeypadDecimal: return VK::kDecimal;
    case ImGuiKey_KeypadDivide: return VK::kDivide;
    case ImGuiKey_KeypadMultiply: return VK::kMultiply;
    case ImGuiKey_KeypadSubtract: return VK::kSubtract;
    case ImGuiKey_KeypadAdd: return VK::kAdd;
    case ImGuiKey_KeypadEnter: return VK::kReturn;
    default: return VK::kNone;
  }
}

// Lets go of the open panel without deleting it (see OnDraw).
void ReleaseSettings();

struct CatNode {
  std::string full_path;
  std::string label;
  std::map<std::string, CatNode> children;
};

// --- The panel ------------------------------------------------------------------------

class SettingsDialog final : public rex::ui::ImGuiDialog {
 public:
  SettingsDialog(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path)
      : ImGuiDialog(drawer), config_path_(std::move(config_path)) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    const std::string close_key = rex::cvar::GetFlagByName("bind_mcla_settings");
    const Prompt prompts[] = {
        {"SAVE", "CTRL+S", kPromptAccept},
        {"CLOSE", close_key.empty() ? "F4" : close_key.c_str(), kPromptBack},
    };

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(900.0f, io.DisplaySize.x - 48.0f),
                                    std::min(600.0f, io.DisplaySize.y - 48.0f)),
                             ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(600.0f, 380.0f), io.DisplaySize);
    // The rows are set a step smaller than the other panels: there are
    // hundreds of them.
    ImGui::PushFont(nullptr, kSmallSize);

    int clicked = -1;
    if (BeginPanel("##mcla_settings", "SETTINGS", PanelIcon::kCheck,
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                       ImGuiWindowFlags_NoScrollWithMouse,
                   prompts, 26.0f)) {
      DrawContents();
    }
    clicked = EndPanel();
    ImGui::PopFont();

    const bool typing = io.WantTextInput;
    const bool capturing = !capturing_bind_name_.empty();
    if (clicked == 0 ||
        (!capturing && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))) {
      rex::cvar::SaveConfig(config_path_);
      saved_flash_ = 2.0f;
    }
    saved_flash_ = std::max(0.0f, saved_flash_ - io.DeltaTime);
    if (clicked == 1 ||
        (!capturing && !typing && ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
      // ImGuiDialog::Draw deletes a closed dialog itself, so the owner lets go
      // first, as with the telemetry card.
      ReleaseSettings();
      Close();
    }
  }

 private:
  void DrawContents() {
    auto& registry = rex::cvar::GetRegistry();

    std::set<std::string> categories;
    for (auto& entry : registry) {
      if (ShouldShow(entry)) categories.insert(entry.category);
    }
    if (!selected_.empty()) {
      bool visible = false;
      for (const std::string& c : categories) {
        if (CategoryIsPathOrChild(c, selected_)) {
          visible = true;
          break;
        }
      }
      if (!visible) selected_.clear();
    }

    std::map<std::string, CatNode> tree;
    for (const std::string& cat : categories) {
      std::map<std::string, CatNode>* level = &tree;
      std::string path;
      size_t start = 0;
      while (start < cat.size()) {
        const size_t slash = cat.find('/', start);
        const std::string segment = cat.substr(
            start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!path.empty()) path += "/";
        path += segment;
        CatNode& node = (*level)[segment];
        node.full_path = path;
        node.label = segment;
        level = &node.children;
        start = slash == std::string::npos ? cat.size() : slash + 1;
      }
    }

    // Yellow is the game's own "careful" color; red marks what cannot change.
    PanelSubtitle(saved_flash_ > 0.0f
                      ? "Saved."
                      : "Yellow settings apply after a restart. Hover one for what it does.");

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##search", "Search settings", search_, sizeof(search_));

    const float height = ImGui::GetContentRegionAvail().y - PanelBottomReserve();
    const float cats_width = std::clamp(ImGui::GetContentRegionAvail().x * 0.24f, 170.0f, 260.0f);

    // Categories: the retail list rows, indented for depth, all expanded.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.05f, 0.35f, 0.35f, 0.40f));
    ImGui::BeginChild("##cats", ImVec2(cats_width, height), ImGuiChildFlags_None);
    CategoryRow("ALL SETTINGS", "", 0);
    std::function<void(const std::map<std::string, CatNode>&, int)> draw_tree =
        [&](const std::map<std::string, CatNode>& nodes, int depth) {
          for (const auto& [key, node] : nodes) {
            CategoryRow(node.label.c_str(), node.full_path, depth);
            draw_tree(node.children, depth + 1);
          }
        };
    draw_tree(tree, 1);
    ImGui::EndChild();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();

    // The divider between the lists, as on the retail two-pane screens.
    ImGui::SameLine(0.0f, 10.0f);
    const ImVec2 rule = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(rule.x, rule.y), ImVec2(rule.x, rule.y + height),
                                        IM_COL32(255, 255, 255, 30), 2.0f);
    ImGui::SameLine(0.0f, 12.0f);

    DrawSettings(registry, height);
  }

  void CategoryRow(const char* label, const std::string& path, int depth) {
    const float row_h = ImGui::GetTextLineHeight() + 10.0f;
    const bool selected = selected_ == path;
    ImGui::PushID(path.empty() ? "##all" : path.c_str());
    if (ImGui::Selectable("##cat", selected, ImGuiSelectableFlags_None, ImVec2(0.0f, row_h))) {
      selected_ = path;
      search_[0] = '\0';
    }
    ImGui::PopID();
    const ImVec2 low = ImGui::GetItemRectMin();
    const ImVec2 high = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 color = DrawListRow(draw, low, high, selected);
    std::string upper(label);
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    draw->AddText(ImVec2(low.x + 22.0f + 12.0f * static_cast<float>(depth),
                         low.y + (row_h - ImGui::GetTextLineHeight()) * 0.5f),
                  depth <= 1 ? color : (selected ? color : ImGui::GetColorU32(ImGuiCol_TextDisabled)),
                  upper.c_str());
  }

  void DrawSettings(std::vector<rex::cvar::FlagEntry>& registry, float height) {
    const std::string search = Lower(search_);
    const bool searching = !search.empty();
    const auto& style = imgui_drawer()->style().settings;

    // Two columns: the name takes what is left, the control a fixed share that
    // ends at the panel's right edge, so a long name is clipped by its cell
    // instead of running under the control, and a resize moves the controls.
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, ImVec4(1.0f, 1.0f, 1.0f, 0.08f));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 4.0f));
    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX;
    if (ImGui::BeginTable("##cvars", 2, flags, ImVec2(0.0f, height))) {
      const float control_w = std::clamp(ImGui::GetContentRegionAvail().x * 0.42f, 200.0f, 320.0f);
      ImGui::TableSetupScrollFreeze(0, 0);
      ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthFixed, control_w);

      int shown = 0;
      for (auto& entry : registry) {
        if (!ShouldShow(entry)) continue;
        if (searching) {
          if (Lower(entry.name).find(search) == std::string::npos &&
              Lower(entry.description).find(search) == std::string::npos) {
            continue;
          }
        } else if (!selected_.empty() && !CategoryIsPathOrChild(entry.category, selected_)) {
          continue;
        }
        ++shown;
        ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetFrameHeight() + 8.0f);
        ImGui::PushID(entry.name.c_str());
        if (IsKeybindCategory(entry.category)) {
          KeybindRow(registry, entry, style);
        } else {
          SettingRow(entry, style);
        }
        ImGui::PopID();
      }
      if (shown == 0) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("No settings match.");
      }
      ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
  }

  static ImVec4 LifecycleColor(rex::cvar::Lifecycle lc, const rex::ui::SettingsStyle& style) {
    switch (lc) {
      case rex::cvar::Lifecycle::kHotReload: return style.lifecycle_live;
      case rex::cvar::Lifecycle::kRequiresRestart: return style.lifecycle_restart;
      case rex::cvar::Lifecycle::kInitOnly: return style.lifecycle_init_only;
    }
    return style.lifecycle_unknown;
  }

  static void NameCell(const rex::cvar::FlagEntry& entry, const std::string& label,
                       const rex::ui::SettingsStyle& style) {
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(LifecycleColor(entry.lifecycle, style), "%s", label.c_str());
    if (ImGui::IsItemHovered()) {
      const char* lifecycle = "";
      switch (entry.lifecycle) {
        case rex::cvar::Lifecycle::kHotReload: lifecycle = "Live - changes apply immediately"; break;
        case rex::cvar::Lifecycle::kRequiresRestart: lifecycle = "Requires restart to take effect"; break;
        case rex::cvar::Lifecycle::kInitOnly: lifecycle = "Read-only - set at initialization only"; break;
      }
      ImGui::SetTooltip("%s\n%s\n[%s]", entry.name.c_str(), entry.description.c_str(), lifecycle);
    }
  }

  void SettingRow(rex::cvar::FlagEntry& entry, const rex::ui::SettingsStyle& style) {
    const bool read_only = entry.lifecycle == rex::cvar::Lifecycle::kInitOnly;
    const std::string current = entry.getter();
    NameCell(entry, entry.name, style);

    ImGui::TableNextColumn();
    if (read_only) ImGui::BeginDisabled();
    switch (entry.type) {
      case rex::cvar::FlagType::Boolean: {
        // Flush right, like the retail options' check boxes.
        bool v = rex::string::from_string<bool>(current, false);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x -
                             ImGui::GetFrameHeight());
        if (ImGui::Checkbox("##v", &v)) {
          rex::cvar::SetFlagByName(entry.name, v ? "true" : "false");
        }
        break;
      }
      case rex::cvar::FlagType::Int32:
      case rex::cvar::FlagType::Int64:
      case rex::cvar::FlagType::Uint32:
      case rex::cvar::FlagType::Uint64: {
        int v = std::atoi(current.c_str());
        const int lo = entry.constraints.min ? static_cast<int>(*entry.constraints.min) : INT_MIN;
        const int hi = entry.constraints.max ? static_cast<int>(*entry.constraints.max) : INT_MAX;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputInt("##v", &v)) {
          rex::cvar::SetFlagByName(entry.name, std::to_string(std::clamp(v, lo, hi)));
        }
        break;
      }
      case rex::cvar::FlagType::Double: {
        double v = std::atof(current.c_str());
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputDouble("##v", &v, 0.0, 0.0, "%.4f")) {
          if (entry.constraints.min) v = std::max(v, *entry.constraints.min);
          if (entry.constraints.max) v = std::min(v, *entry.constraints.max);
          rex::cvar::SetFlagByName(entry.name, std::to_string(v));
        }
        break;
      }
      case rex::cvar::FlagType::Command:
        if (ImGui::Button("RUN##v", ImVec2(-FLT_MIN, 0.0f))) {
          entry.command_callback("");
        }
        break;
      case rex::cvar::FlagType::String:
        if (!entry.constraints.allowed_values.empty()) {
          const auto& options = entry.constraints.allowed_values;
          int index = 0;
          for (int i = 0; i < static_cast<int>(options.size()); ++i) {
            if (options[i] == current) {
              index = i;
              break;
            }
          }
          ImGui::SetNextItemWidth(-FLT_MIN);
          if (ImGui::BeginCombo("##v", options[index].c_str())) {
            for (int i = 0; i < static_cast<int>(options.size()); ++i) {
              if (ImGui::Selectable(options[i].c_str(), i == index)) {
                rex::cvar::SetFlagByName(entry.name, options[i]);
              }
              if (i == index) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
          }
        } else {
          char buf[256];
          rex::string::copy_truncating(buf, current, sizeof(buf));
          ImGui::SetNextItemWidth(-FLT_MIN);
          if (ImGui::InputText("##v", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
            rex::cvar::SetFlagByName(entry.name, buf);
          }
          // Right-click copy/paste for string cvars (e.g. paste a server IP).
          if (ImGui::BeginPopupContextItem("##strctx")) {
            if (ImGui::MenuItem("Copy")) ImGui::SetClipboardText(buf);
            if (ImGui::MenuItem("Paste")) {
              if (const char* clip = ImGui::GetClipboardText(); clip && *clip) {
                rex::cvar::SetFlagByName(entry.name, clip);
              }
            }
            ImGui::EndPopup();
          }
        }
        break;
    }
    if (read_only) ImGui::EndDisabled();
  }

  void KeybindRow(std::vector<rex::cvar::FlagEntry>& registry, rex::cvar::FlagEntry& entry,
                  const rex::ui::SettingsStyle& style) {
    const std::string current = entry.getter();
    // Controller binds only matter in mouse-and-keyboard mode.
    const bool inactive = entry.category == "Input/Keybinds/Controller" &&
                          rex::cvar::GetFlagByName("mnk_mode") != "true";
    if (inactive) ImGui::BeginDisabled();

    NameCell(entry, entry.description.empty() ? entry.name : entry.description, style);
    if (!current.empty()) {
      int conflicts = 0;
      for (auto& other : registry) {
        if (IsKeybindCategory(other.category) && other.name != entry.name &&
            other.getter() == current) {
          ++conflicts;
        }
      }
      if (conflicts > 0) {
        ImGui::SameLine();
        ImGui::TextColored(style.warning, "(!)");
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("Key '%s' is also bound to %d other action(s)", current.c_str(),
                            conflicts);
        }
      }
    }

    ImGui::TableNextColumn();
    const float reset_w = ImGui::CalcTextSize("RESET").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float key_w = ImGui::GetContentRegionAvail().x - reset_w - ImGui::GetStyle().ItemSpacing.x;
    if (capturing_bind_name_ == entry.name) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.05f, 0.43f, 0.43f, 0.85f));
      ImGui::Button("PRESS A KEY...##v", ImVec2(key_w, 0.0f));
      ImGui::PopStyleColor();
      Capture(entry);
    } else {
      const std::string label = (current.empty() ? std::string("-") : current) + "##v";
      if (ImGui::Button(label.c_str(), ImVec2(key_w, 0.0f))) {
        capturing_bind_name_ = entry.name;
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("RESET##v", ImVec2(reset_w, 0.0f))) {
      rex::cvar::SetFlagByName(entry.name, entry.default_value);
    }
    if (inactive) ImGui::EndDisabled();
  }

  void Capture(const rex::cvar::FlagEntry& entry) {
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      capturing_bind_name_.clear();
      return;
    }
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
      const auto key = static_cast<ImGuiKey>(k);
      if (key == ImGuiKey_Escape || !ImGui::IsKeyPressed(key)) continue;
      const std::string name = rex::ui::VirtualKeyToString(ImGuiKeyToVirtualKey(key));
      if (!name.empty()) rex::cvar::SetFlagByName(entry.name, name);
      capturing_bind_name_.clear();
      return;
    }
    // Not on the click that started the capture.
    if (!ImGui::IsItemHovered()) {
      for (int mb = 0; mb < 3; ++mb) {
        if (ImGui::IsMouseClicked(mb)) {
          const char* names[] = {"LMB", "RMB", "MMB"};
          rex::cvar::SetFlagByName(entry.name, names[mb]);
          capturing_bind_name_.clear();
          return;
        }
      }
    }
  }

  std::filesystem::path config_path_;
  std::string selected_;
  std::string capturing_bind_name_;
  char search_[128] = {};
  float saved_flash_ = 0.0f;
};

std::unique_ptr<SettingsDialog> g_settings;

void ReleaseSettings() {
  (void)g_settings.release();
}

}  // namespace

void ToggleSettings(rex::ui::ImGuiDrawer* drawer, const std::filesystem::path& config_path) {
  if (!drawer) return;
  if (g_settings) {
    g_settings.reset();
  } else {
    g_settings = std::make_unique<SettingsDialog>(drawer, config_path);
  }
}

} // namespace mcla::ui
