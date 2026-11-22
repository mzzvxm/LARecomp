#include "browser_ui.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>

#include "settings/larecomp_settings.h"
#include <rex/platform/file.h>
#include <rex/ui/ui_system.h>
#define LARECOMP_BROWSER_HAS_ICON 1

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "larecomp_log.h"
#include "profile/larecomp_profile_store.h"
#include "../online_platform.h"
#include "../online_toast.h"
#include "../relay_info.h"
#include "../system_link.h"
#include "browser_client.h"
#include "browser_join.h"
#include "../../modloader/modsbrowser/mod_browser_ui.h"
#include "../../modloader/modsbrowser/mod_download.h"
#include "../identity/identity_ui.h"
#include "mc_engine/ui/overlay_support.h"
#include "mc_engine/input/input_state.h"
#include "mc_engine/ui/gamepad.h"
#include "mc_engine/ui/imgui_theme.h"


namespace larecomp {
namespace {

namespace br = browser;
using Clock = std::chrono::steady_clock;

std::atomic<bool> g_open{false};
std::atomic<bool> g_open_asked{false};

// --------------------------------------------------------------- config ---

std::filesystem::path ConfigPath() { return rex::platform::GetExecutableFolder() / "larecomp.toml"; }

// Live cvar and larecomp.toml together: the game rewrites the toml from the
// live cvars when it exits, and the file is what the next boot reads.
void SaveSetting(const char* name, const std::string& toml_value, const std::string& live_value) {
    larecomp::cvar::SetFlagByName(name, live_value);
    std::vector<profiles::ConfigEdit> edits;
    edits.push_back({name, toml_value});
    if (!profiles::EditConfig(ConfigPath(), edits))
        LARECOMP_APP_WARN("[browser] could not write {} to larecomp.toml", name);
}

void SaveString(const char* name, const std::string& value) {
    SaveSetting(name, profiles::QuoteTomlString(value), value);
}

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> SplitList(const std::string& s) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t comma = s.find(',', pos);
        if (comma == std::string::npos) comma = s.size();
        std::string item = s.substr(pos, comma - pos);
        item.erase(std::remove_if(item.begin(), item.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)); }),
                   item.end());
        if (!item.empty()) out.push_back(item);
        pos = comma + 1;
    }
    return out;
}

std::string JoinList(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& i : v) s += (s.empty() ? "" : ",") + i;
    return s;
}

// The relay the game is connected to (or would connect to), as host:port.
std::string CurrentRelay() {
    std::string host;
    uint16_t port = 0;
    relay_info::ConfiguredServer(host, port);
    return host + ":" + std::to_string(port);
}

std::string VipText(uint32_t vip) {
    char b[20];
    std::snprintf(b, sizeof(b), "%u.%u.%u.%u", vip >> 24, (vip >> 16) & 0xFF, (vip >> 8) & 0xFF, vip & 0xFF);
    return b;
}

ImVec4 PingColor(int ms) {
    if (ms < 0) return mcla::ui::kColorTextDim;
    if (ms < 80) return mcla::ui::kColorGood;
    if (ms < 160) return mcla::ui::kColorWarn;
    return mcla::ui::kColorBad;
}

// ------------------------------------------------------------- gamepad ---

// The pad reaches the game through the SDK's input system, which this window
// switches off while it is open, so the window reads the pads itself
// (ui/gamepad.h: Xbox pads through XInput with a slow retry for empty slots,
// every other pad through SDL) and hands them to ImGui's own gamepad
// navigation. Returns whether a pad is connected. What it read also goes to the
// input state (a pad picked up inside the dialog makes the pad the active device,
// so the CLOSE prompt says B or CIRCLE): the SDK hands the game a pad at rest
// while the dialog is open, so nothing else sees it.
bool FeedGamepad(ImGuiIO& io, bool* b_pressed) {
    *b_pressed = false;
    static uint16_t last_buttons = 0;
    const padfeed::PadSample pad = mcla::ui::ReadOverlayPads();
    larecomp::input::NoteOverlayPad(pad.connected, pad.buttons, pad.lx, pad.ly, pad.rx, pad.ry);
    if (!pad.connected) {
        last_buttons = 0;
        return false;
    }
    const uint16_t b = pad.buttons;
    struct Map {
        uint16_t bit;
        ImGuiKey key;
    };
    static const Map kMap[] = {
        {0x0001, ImGuiKey_GamepadDpadUp},    {0x0002, ImGuiKey_GamepadDpadDown},
        {0x0004, ImGuiKey_GamepadDpadLeft},  {0x0008, ImGuiKey_GamepadDpadRight},
        {0x0010, ImGuiKey_GamepadStart},     {0x0020, ImGuiKey_GamepadBack},
        {0x0100, ImGuiKey_GamepadL1},        {0x0200, ImGuiKey_GamepadR1},
        {0x1000, ImGuiKey_GamepadFaceDown},  {0x2000, ImGuiKey_GamepadFaceRight},
        {0x4000, ImGuiKey_GamepadFaceLeft},  {0x8000, ImGuiKey_GamepadFaceUp},
    };
    for (const auto& m : kMap) io.AddKeyEvent(m.key, (b & m.bit) != 0);
    auto stick = [&](ImGuiKey neg_key, ImGuiKey pos_key, float v) {
        float n = 0.0f, p = 0.0f;
        padfeed::ApplyDeadZone(v, 0.25f, &n, &p);
        io.AddKeyAnalogEvent(neg_key, n > 0.0f, n);
        io.AddKeyAnalogEvent(pos_key, p > 0.0f, p);
    };
    stick(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, pad.lx);
    stick(ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadLStickUp, pad.ly);
    stick(ImGuiKey_GamepadRStickLeft, ImGuiKey_GamepadRStickRight, pad.rx);
    stick(ImGuiKey_GamepadRStickDown, ImGuiKey_GamepadRStickUp, pad.ry);
    *b_pressed = (b & 0x2000) && !(last_buttons & 0x2000);
    last_buttons = b;
    return true;
}

// ------------------------------------------------------------- the room ---

// What to do on a server: a room (vip), or play there in a mode (any session
// of it, or host one).
enum Kind { kRoom = 0, kCruise = 1, kRace = 2 };

struct Pending {
    std::string address;
    uint32_t vip = 0;
    std::string label;
    int kind = kRoom;
    bool valid() const { return !address.empty() && (vip || kind != kRoom); }
};

// address|vip|label|kind (kind absent = a room, as the first version wrote it).
Pending ParsePending(const std::string& s) {
    Pending p;
    const size_t a = s.find('|');
    const size_t b = a == std::string::npos ? std::string::npos : s.find('|', a + 1);
    if (b == std::string::npos) return p;
    const size_t c = s.find('|', b + 1);
    p.address = s.substr(0, a);
    p.vip = uint32_t(std::strtoul(s.substr(a + 1, b - a - 1).c_str(), nullptr, 10));
    p.label = s.substr(b + 1, c == std::string::npos ? std::string::npos : c - b - 1);
    if (c != std::string::npos) p.kind = std::atoi(s.c_str() + c + 1);
    if (p.kind < kRoom || p.kind > kRace) p.kind = kRoom;
    return p;
}

std::string PendingText(const Pending& p) {
    return p.address + "|" + std::to_string(p.vip) + "|" + p.label + "|" + std::to_string(p.kind);
}

class BrowserDialog;
BrowserDialog* g_dialog = nullptr;

class BrowserDialog final : public mcla::ui::PanelDialog {
public:
    explicit BrowserDialog(rex::ui::IUISystem& ui) : ui_(&ui) {
        g_dialog = this;
        pending_ = ParsePending(larecomp::cvar::GetFlagByName("server_browser_pending"));
        if (pending_.valid()) {
            // Offered once, right after the restart it asked for.
            SaveString("server_browser_pending", "");
            pending_until_ = Clock::now() + std::chrono::minutes(3);
        }
    }

    ~BrowserDialog() override {
        g_dialog = nullptr;
        pad_block_.Set(false);
    }

    void Toggle() {
        if (g_open.load())
            Close_();
        else
            Open_();
    }

protected:
    void OnDraw(ImGuiIO& io) override {
        if (g_open_asked.exchange(false) && !g_open.load()) Open_();
        if (pending_.valid() && !g_open.load() && Clock::now() < pending_until_ &&
            SystemLinkOnlineConnected() && br::WorldReady() && !pending_opened_) {
            pending_opened_ = true;
            Open_();
        }

        DrawJoinStrip(io);
        if (!g_open.load()) return;

        TakeIcons();
        snapshot_ = client_.Take();

        bool b_pressed = false;
        const bool pad = FeedGamepad(io, &b_pressed);
        if (pad) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;

        const ImVec2 size(std::min(io.DisplaySize.x - 40.0f, 1100.0f), std::min(io.DisplaySize.y - 40.0f, 680.0f));
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing,
                                ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints(ImVec2(720.0f, 420.0f), io.DisplaySize);
        // The word follows the device the player used last (B, CIRCLE or ESC), not whether a pad is plugged in.
        const mcla::ui::Prompt prompts[] = {{"CLOSE", mcla::ui::CloseKeyLabel("server browser"), mcla::ui::kPromptBack}};
        // Dense screen: rows a step smaller, like the game's mission lists.
        ImGui::PushFont(nullptr, mcla::ui::kSmallSize);
        if (mcla::ui::BeginPanel("###larecomp_server_browser", "SERVER BROWSER", mcla::ui::PanelIcon::kInfo,
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                     ImGuiWindowFlags_NoScrollWithMouse,
                                 prompts)) {
            if (first_frame_) {
                ImGui::SetWindowFocus();
                first_frame_ = false;
            }
            DrawToolbar();
            ImGui::Separator();
            const float left_w = std::max(320.0f, ImGui::GetContentRegionAvail().x * 0.42f);
            // The action row under the lists, then the panel's footer bar.
            const float body_h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.6f -
                                 mcla::ui::PanelBottomReserve();
            ImGui::BeginChild("##servers", ImVec2(left_w, body_h), ImGuiChildFlags_Borders);
            DrawServerList();
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("##details", ImVec2(0, body_h), ImGuiChildFlags_Borders);
            DrawDetails();
            ImGui::EndChild();
            DrawFooter();
            DrawSwitchPopup();
            DrawModsPopup();
        }
        const bool close_clicked = mcla::ui::EndPanel() == 0;
        ImGui::PopFont();

        const bool typing = ImGui::GetIO().WantTextInput;
        const bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        if (close_clicked || ((b_pressed || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) && !typing && !popup))
            Close_();
    }

private:
    void Open_() {
        // Linux (online_platform.h): there is no online client to join a server with. The window stays shut and
        // the pop-up says why.
        if (!larecomp::kOnlineAvailable) {
            LARECOMP_APP_INFO("[browser] not opened: {}", larecomp::kOnlineUnavailableText);
            larecomp::OnlineToastNotice();
            return;
        }
        g_open.store(true);
        pad_block_.Set(true);
        first_frame_ = true;
        client_.SetAutoRefresh(true);
        LARECOMP_APP_INFO("[browser] opened");
    }

    void Close_() {
        g_open.store(false);
        pad_block_.Set(false);
        client_.SetAutoRefresh(false);
        auto& io = ui_->GetImGuiIO();
        io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard);
        io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
        ReleaseBrowserGamepad();
        LARECOMP_APP_INFO("[browser] closed");
    }

    // A new icon becomes a texture here, on the UI thread, in the draw (the ImGui context is current).
    void TakeIcons() {
        for (auto& up : client_.TakeIcons()) {
#if defined(LARECOMP_BROWSER_HAS_ICON)
            // The header is read before the decoder allocates what it says (a 64 KB file can claim a huge picture).
            // CHECK: not run in a game: a real relay's icon (the recorded one is 2135 bytes) must still show in the F6 list.
            auto tex = br::PngHeaderFits(up.png.data(), up.png.size(), br::kIconMaxSide)
                           ? mcla::ui::IconTexture::FromEncoded(up.png.data(), up.png.size(), br::kIconMaxSide)
                           : nullptr;
            if (!tex) {
                LARECOMP_APP_WARN("[browser] icon of {} could not be decoded", up.address);
                continue;
            }
            icons_[up.address] = std::move(tex);
#else
            (void)up;
#endif
        }
    }

    // The icon of a server, or null (none yet).
    mcla::ui::IconTexture* Icon(const std::string& address) const {
        auto it = icons_.find(address);
        return it == icons_.end() ? nullptr : it->second.get();
    }

    bool Matches(const br::ServerView& s) const {
        if (hide_empty_ && s.rooms == 0 && s.room_list.empty()) return false;
        if (hide_dead_ && !s.answered) return false;
        if (search_[0] == 0) return true;
        const std::string needle = Lower(search_);
        if (Lower(s.name).find(needle) != std::string::npos) return true;
        if (Lower(s.tags).find(needle) != std::string::npos) return true;
        if (Lower(s.description).find(needle) != std::string::npos) return true;
        if (Lower(s.address).find(needle) != std::string::npos) return true;
        for (const auto& r : s.room_list)
            if (Lower(r.gamertag).find(needle) != std::string::npos || Lower(r.name).find(needle) != std::string::npos)
                return true;
        return false;
    }

    void DrawToolbar() {
        const char* hint = "Search name, tag, player...";
        ImGui::SetNextItemWidth(ImGui::CalcTextSize(hint).x + ImGui::GetStyle().FramePadding.x * 2.0f + 8.0f);
        ImGui::InputTextWithHint("##search", hint, search_, sizeof(search_));
        ImGui::SameLine();
        bool icons = larecomp::cvar::GetFlagByName("server_browser_show_icons") != "false";
        if (ImGui::Checkbox("Icons", &icons))
            SaveSetting("server_browser_show_icons", icons ? "true" : "false", icons ? "true" : "false");
        ImGui::SameLine();
        ImGui::Checkbox("Only with rooms", &hide_empty_);
        ImGui::SameLine();
        ImGui::Checkbox("Hide offline", &hide_dead_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        const char* sorts[] = {"Ping", "Players", "Name"};
        ImGui::Combo("##sort", &sort_, sorts, 3);
        ImGui::SameLine();
        if (ImGui::Button(snapshot_.refreshing ? "Refreshing..." : "Refresh")) client_.Refresh();
        if (!snapshot_.master_status.empty()) ImGui::TextDisabled("%s", snapshot_.master_status.c_str());
    }

    void DrawServerList() {
        std::vector<const br::ServerView*> rows;
        for (const auto& s : snapshot_.servers)
            if (Matches(s)) rows.push_back(&s);
        std::stable_sort(rows.begin(), rows.end(), [&](const br::ServerView* a, const br::ServerView* b) {
            if (a->answered != b->answered) return a->answered;
            if (sort_ == 1 && a->players != b->players) return a->players > b->players;
            if (sort_ == 2) return Lower(a->name) < Lower(b->name);
            const int pa = a->ping_ms < 0 ? 1 << 20 : a->ping_ms, pb = b->ping_ms < 0 ? 1 << 20 : b->ping_ms;
            return pa < pb;
        });
        if (rows.empty()) {
            ImGui::TextDisabled(snapshot_.refreshing ? "Looking for servers..." : "No servers.");
            ImGui::TextDisabled("Add one below, or set online_master_server.");
        }
        const bool icons = larecomp::cvar::GetFlagByName("server_browser_show_icons") != "false";
        const float row_h = icons ? 48.0f : ImGui::GetTextLineHeightWithSpacing() * 2.0f;
        const std::string current = CurrentRelay();
        // The game's list row (teal bar and cursor) instead of the flat fill.
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        for (const br::ServerView* s : rows) {
            ImGui::PushID(s->address.c_str());
            const bool selected = selected_ == s->address;
            const ImVec2 start = ImGui::GetCursorPos();
            if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, row_h)))
                selected_ = s->address;
            if (ImGui::IsItemFocused() && selected_.empty()) selected_ = s->address;
            mcla::ui::DrawListRow(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                  selected);
            // Past the cursor triangle.
            ImGui::SetCursorPos(ImVec2(start.x + 22.0f, start.y));
            if (icons) {
                mcla::ui::IconTexture* tex = Icon(s->address);
                if (tex)
                    ImGui::Image(tex->Ref(), ImVec2(row_h - 4, row_h - 4));
                else
                    ImGui::Dummy(ImVec2(row_h - 4, row_h - 4));
                ImGui::SameLine();
            }
            ImGui::BeginGroup();
            const std::string name = s->name.empty() ? s->address : s->name;
            ImGui::TextColored(s->answered ? mcla::ui::kColorYellow : mcla::ui::kColorTextDim, "%s%s",
                               name.c_str(), s->favorite ? "  [fav]" : "");
            if (s->address == current) {
                ImGui::SameLine();
                ImGui::TextColored(mcla::ui::kColorCyanGlow, "(current)");
            }
            if (s->answered) {
                ImGui::TextDisabled("%s%s%d players  %d rooms", s->region.c_str(), s->region.empty() ? "" : "  ",
                                    s->players, s->legacy ? s->rooms : int(s->room_list.size()));
                ImGui::SameLine();
                ImGui::TextColored(PingColor(s->ping_ms), "%d ms", s->ping_ms);
            } else {
                ImGui::TextDisabled(s->queried ? "offline" : "...");
            }
            ImGui::EndGroup();
            ImGui::PopID();
        }
        ImGui::PopStyleColor(2);
    }

    const br::ServerView* Selected() const {
        for (const auto& s : snapshot_.servers)
            if (s.address == selected_) return &s;
        return nullptr;
    }

    void DrawDetails() {
        const br::ServerView* s = Selected();
        if (!s) {
            ImGui::TextDisabled("Pick a server on the left.");
            return;
        }
        const bool icons = larecomp::cvar::GetFlagByName("server_browser_show_icons") != "false";
        if (icons) {
            if (mcla::ui::IconTexture* tex = Icon(s->address)) {
                ImGui::Image(tex->Ref(), ImVec2(96, 96));
                ImGui::SameLine();
            }
        }
        ImGui::BeginGroup();
        ImGui::TextColored(mcla::ui::kColorYellow, "%s", s->name.empty() ? s->address.c_str() : s->name.c_str());
        ImGui::TextDisabled("%s", s->address.c_str());
        if (!s->region.empty()) ImGui::Text("Region: %s", s->region.c_str());
        if (!s->tags.empty()) ImGui::Text("Tags: %s", s->tags.c_str());
        if (s->answered) {
            ImGui::Text("Players: %d   Rooms: %d   Ping:", s->players,
                        s->legacy ? s->rooms : int(s->room_list.size()));
            ImGui::SameLine();
            ImGui::TextColored(PingColor(s->ping_ms), "%d ms", s->ping_ms);
        } else {
            ImGui::TextColored(mcla::ui::kColorBad, "Not answering");
        }
        ImGui::EndGroup();
        if (!s->description.empty()) {
            ImGui::Spacing();
            ImGui::TextWrapped("%s", s->description.c_str());
        }
        auto favs = SplitList(larecomp::cvar::GetFlagByName("server_browser_servers"));
        const bool fav = std::find(favs.begin(), favs.end(), s->address) != favs.end();
        if (ImGui::SmallButton(fav ? "Remove from my servers" : "Add to my servers")) {
            if (fav)
                favs.erase(std::remove(favs.begin(), favs.end(), s->address), favs.end());
            else
                favs.push_back(s->address);
            SaveString("server_browser_servers", JoinList(favs));
            client_.Refresh();
        }

        // Play here even with nobody on: the game's own Quick Cruise / Quick
        // Match on this relay joins a session of that mode, or hosts one for
        // whoever comes next.
        ImGui::Spacing();
        ImGui::BeginDisabled(!s->answered);
        if (ImGui::Button("Cruise on this server")) Act(*s, {s->address, 0, "", kCruise});
        ImGui::SameLine();
        if (ImGui::Button("Race on this server")) Act(*s, {s->address, 0, "", kRace});
        ImGui::EndDisabled();
        ImGui::TextDisabled("Joins a session of that mode here, or hosts one if there is none.");

        ImGui::Separator();
        if (s->legacy) {
            ImGui::TextWrapped("This server runs an older relay: it shows its name, players and ping, "
                               "but not its rooms. Its owner can update it to larecomp_relay_server.");
            return;
        }
        if (s->room_list.empty()) {
            ImGui::TextDisabled(s->answered ? "No rooms on this server right now." : "");
            return;
        }
        if (ImGui::BeginTable("##rooms", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("Host / room", ImGuiTableColumnFlags_WidthStretch, 3.0f);
            ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthStretch, 2.4f);
            ImGui::TableSetupColumn("Players", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Ping", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableHeadersRow();
            for (const auto& r : s->room_list) {
                ImGui::PushID(int(r.host_vip));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%s", r.gamertag.empty() ? VipText(r.host_vip).c_str() : r.gamertag.c_str());
                if (!r.name.empty() && r.name != r.gamertag) ImGui::TextDisabled("%s", r.name.c_str());
                if (!r.active) ImGui::TextDisabled("idle");

                ImGui::TableNextColumn();
                const auto props = br::DescribeProps(r.props);
                std::string mode, extra;
                for (const auto& p : props) {
                    if (p.name == "GameMode") mode = p.value == "Single" ? "Race session" : p.value;
                    else if (p.name == "GameStyle" && p.value != "StyleNone") mode += (mode.empty() ? "" : " / ") + p.value;
                    else if (p.name == "EventStatus") extra = p.value;
                }
                ImGui::Text("%s", mode.empty() ? "?" : mode.c_str());
                if (!extra.empty()) ImGui::TextDisabled("%s", extra.c_str());
                if (!props.empty() && ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    for (const auto& p : props) ImGui::Text("%s: %s", p.name.c_str(), p.value.c_str());
                    ImGui::EndTooltip();
                }

                ImGui::TableNextColumn();
                ImGui::Text("%d/%d", r.cur_players, r.max_players);

                ImGui::TableNextColumn();
                const int est = (s->ping_ms >= 0 && r.host_rtt_ms >= 0) ? s->ping_ms + r.host_rtt_ms : -1;
                ImGui::TextColored(PingColor(est), est >= 0 ? "%d ms" : "?", est);

                ImGui::TableNextColumn();
                const bool full = r.max_players > 0 && r.cur_players >= r.max_players;
                ImGui::BeginDisabled(full);
                if (ImGui::Button(full ? "Full" : "Join")) Act(*s, {s->address, r.host_vip, "", kRoom});
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    // Everything that joins goes through here: a server with mods first gets
    // its mods checked (and, when some are missing, the mods pop-up).
    void Act(const br::ServerView& s, Pending action) {
        action.address = s.address;
        const bool has_mods = s.http_port && (s.mods_required + s.mods_optional) > 0;
        if (has_mods && checked_mods_ != s.address + "#" + std::to_string(s.mods_crc)) {
            mods_action_ = action;
            mods_server_ = s;
            mods_check_ = {};
            mods_checking_ = true;
            mods_download_.reset();
            mods_selected_.clear();
            open_mods_ = true;
            auto result = std::make_shared<std::pair<std::atomic<bool>, br::ModCheck>>();
            result->first = false;
            mods_result_ = result;
            const std::string host = s.host;
            const uint16_t port = s.http_port;
            std::thread([result, host, port] {
                result->second = br::CheckServerMods(host, port, true);
                result->first.store(true);
            }).detach();
            return;
        }
        Execute(s, action);
    }

    void Execute(const br::ServerView& s, const Pending& action) {
        if (action.kind != kRoom) {
            PlayOnServer(s, action.kind == kCruise);
            return;
        }
        for (const auto& r : s.room_list)
            if (r.host_vip == action.vip) Join(s, r);
    }

    void Join(const br::ServerView& s, const br::RoomView& r) {
        bool cruise = false, lan = true;
        br::ClassifyRoom(r.props, &cruise, &lan);
        const std::string label = (r.gamertag.empty() ? VipText(r.host_vip) : r.gamertag) + " @ " +
                                  (s.name.empty() ? s.address : s.name);
        if (s.address != CurrentRelay() || larecomp::cvar::GetFlagByName("mcla_online_server").empty()) {
            switch_ = {s.address, r.host_vip, label, kRoom};
            open_switch_ = true;  // opened at window level, where the modal is drawn
            return;
        }
        br::JoinRequest req;
        req.host_vip = r.host_vip;
        req.cruise = cruise;
        req.lan = lan;
        req.label = label;
        // The relay puts this host first in the list the game's Live search gets.
        const std::string host = s.host;
        const uint16_t port = s.port;
        const uint32_t vip = r.host_vip;
        std::thread([host, port, vip] { br::SendTargetHint(host, port, vip, 800); }).detach();
        br::RequestJoin(req);
        Close_();
    }

    void PlayOnServer(const br::ServerView& s, bool cruise) {
        const std::string label = s.name.empty() ? s.address : s.name;
        if (s.address != CurrentRelay() || larecomp::cvar::GetFlagByName("mcla_online_server").empty()) {
            switch_ = {s.address, 0, label, cruise ? kCruise : kRace};
            open_switch_ = true;
            return;
        }
        br::JoinRequest req;
        req.any_room = true;
        req.cruise = cruise;
        req.lan = true;  // System Link: the path the relay has carried longest
        req.label = label;
        br::RequestJoin(req);
        Close_();
    }

    void DrawModsPopup() {
        if (open_mods_) {
            ImGui::OpenPopup("Server mods");
            open_mods_ = false;
        }
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(520, 0), ImVec2(900, 640));
        if (!ImGui::BeginPopupModal("Server mods", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
        const std::string server = mods_server_.name.empty() ? mods_server_.address : mods_server_.name;

        if (mods_checking_) {
            if (mods_result_ && mods_result_->first.load()) {
                mods_check_ = mods_result_->second;
                mods_checking_ = false;
                for (const auto& m : mods_check_.mods)
                    if (!m.required) mods_selected_[m.name] = m.state != br::ModState::kInstalled;
                if (mods_check_.AllReady()) {
                    // Everything here already: straight on.
                    checked_mods_ = mods_server_.address + "#" + std::to_string(mods_server_.mods_crc);
                    ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                    Execute(mods_server_, mods_action_);
                    return;
                }
            } else {
                ImGui::Text("Checking the mods of %s...", server.c_str());
                if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                return;
            }
        }

        if (!mods_check_.ok) {
            ImGui::TextColored(mcla::ui::kColorBad, "Could not read the mods of %s:", server.c_str());
            ImGui::TextWrapped("%s", mods_check_.error.c_str());
            if (ImGui::Button("Join anyway")) {
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                Execute(mods_server_, mods_action_);
                return;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }

        if (mods_download_) {
            const auto p = mods_download_->Get();
            ImGui::Text("Downloading the mods of %s", server.c_str());
            const float frac = p.total ? float(double(p.done) / double(p.total)) : 1.0f;
            const std::string label = br::FormatBytes(p.done) + " / " + br::FormatBytes(p.total);
            ImGui::ProgressBar(frac, ImVec2(480, 0), label.c_str());
            ImGui::TextDisabled("%s", p.current.c_str());
            if (p.finished && p.failed) {
                ImGui::TextColored(mcla::ui::kColorBad, "Download failed: %s", p.error.c_str());
                if (ImGui::Button("Close")) {
                    mods_download_.reset();
                    ImGui::CloseCurrentPopup();
                }
            } else if (p.finished) {
                ImGui::TextColored(mcla::ui::kColorGood, "Done. The game restarts to load them.");
                // After the restart: this server, and what was asked for on it.
                if (mods_server_.address != CurrentRelay() || larecomp::cvar::GetFlagByName("mcla_online_server").empty())
                    SaveString("mcla_online_server", mods_server_.address);
                SaveSetting("mcla_online_enable", "true", "true");
                Pending next = mods_action_;
                next.label = server;
                SaveString("server_browser_pending", PendingText(next));
                mods_download_.reset();
                ImGui::CloseCurrentPopup();
                if (!br::RestartGame()) LARECOMP_APP_WARN("[browser] restart failed; restart the game by hand");
            } else if (ImGui::Button("Cancel download")) {
                mods_download_->Cancel();
            }
            ImGui::EndPopup();
            return;
        }

        ImGui::Text("%s plays with these mods:", server.c_str());
        ImGui::Spacing();
        uint64_t to_get = 0;
        if (ImGui::BeginTable("##mods", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                               ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Mod", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("");
            ImGui::TableSetupColumn("Here");
            ImGui::TableSetupColumn("Download");
            ImGui::TableHeadersRow();
            for (const auto& m : mods_check_.mods) {
                ImGui::PushID(m.name.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const bool installed = m.state == br::ModState::kInstalled;
                if (!m.required && !installed) {
                    bool on = mods_selected_[m.name];
                    if (ImGui::Checkbox(m.name.c_str(), &on)) mods_selected_[m.name] = on;
                } else {
                    ImGui::Text("%s", m.name.c_str());
                }
                if (!m.info.version.empty() || !m.info.author.empty()) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s%s%s", m.info.version.empty() ? "" : ("v" + m.info.version).c_str(),
                                        m.info.author.empty() ? "" : "  by ", m.info.author.c_str());
                }
                ImGui::TableNextColumn();
                if (m.required)
                    ImGui::TextColored(mcla::ui::kColorWarn, "required");
                else
                    ImGui::TextDisabled("optional");
                ImGui::TableNextColumn();
                if (installed)
                    ImGui::TextColored(mcla::ui::kColorGood, "installed");
                else if (m.state == br::ModState::kOutdated)
                    ImGui::TextColored(mcla::ui::kColorWarn, "other version");
                else
                    ImGui::TextColored(mcla::ui::kColorBad, "missing");
                ImGui::TableNextColumn();
                if (!installed) {
                    ImGui::Text("%s", br::FormatBytes(m.need_bytes).c_str());
                    if (m.required || mods_selected_[m.name]) to_get += m.need_bytes;
                } else {
                    ImGui::TextDisabled("-");
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 500);
        ImGui::TextDisabled("Mods come from the server's owner. Only download from servers you trust: "
                            "they are files the game will load. A mod being replaced is kept as models/.old_<mod>.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();

        const bool required_ok = mods_check_.RequiredReady();
        if (to_get > 0) {
            const std::string label = "Download " + br::FormatBytes(to_get) + " and restart";
            if (ImGui::Button(label.c_str())) {
                std::vector<br::ServerMod> want;
                for (const auto& m : mods_check_.mods)
                    if (m.state != br::ModState::kInstalled && (m.required || mods_selected_[m.name])) want.push_back(m);
                LARECOMP_APP_INFO("[browser] downloading {} mod(s), {}, from {}", want.size(), br::FormatBytes(to_get),
                                  mods_server_.address);
                mods_download_ = std::make_unique<br::ModDownload>(mods_server_.host, mods_server_.http_port, want);
            }
            ImGui::SameLine();
        }
        if (required_ok) {
            if (ImGui::Button(to_get > 0 ? "Join without them" : "Join")) {
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                Execute(mods_server_, mods_action_);
                return;
            }
            ImGui::SameLine();
        }
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    void DrawSwitchPopup() {
        if (open_switch_) {
            ImGui::OpenPopup("Switch server");
            open_switch_ = false;
        }
        if (!ImGui::BeginPopupModal("Switch server", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
        ImGui::Text("This is on another server:");
        ImGui::TextColored(mcla::ui::kColorYellow, "%s", switch_.address.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped("The game connects to one server per run. Switch to it and restart the game; "
                           "the browser offers it again right after it is back.");
        ImGui::Spacing();
        if (ImGui::Button("Switch and restart the game")) {
            SaveString("mcla_online_server", switch_.address);
            SaveSetting("mcla_online_enable", "true", "true");
            SaveString("server_browser_pending", PendingText(switch_));
            LARECOMP_APP_INFO("[browser] switching to {} for {}; restarting", switch_.address, switch_.label);
            ImGui::CloseCurrentPopup();
            if (!br::RestartGame()) QuitGame();
        }
        ImGui::SameLine();
        if (ImGui::Button("Switch, restart later")) {
            SaveString("mcla_online_server", switch_.address);
            SaveSetting("mcla_online_enable", "true", "true");
            SaveString("server_browser_pending", PendingText(switch_));
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    static void QuitGame() {
#ifdef _WIN32
        // A normal close of our own window: the app saves and shuts down.
        struct Ctx {
            DWORD pid;
            HWND hwnd;
        } ctx{GetCurrentProcessId(), nullptr};
        EnumWindows(
            [](HWND h, LPARAM p) -> BOOL {
                auto* c = reinterpret_cast<Ctx*>(p);
                DWORD pid = 0;
                GetWindowThreadProcessId(h, &pid);
                if (pid == c->pid && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) {
                    c->hwnd = h;
                    return FALSE;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&ctx));
        if (ctx.hwnd) PostMessageA(ctx.hwnd, WM_CLOSE, 0, 0);
#endif
    }

    void DrawFooter() {
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputTextWithHint("##add", "host:port", add_, sizeof(add_));
        ImGui::SameLine();
        if (ImGui::Button("Add server") && add_[0]) {
            auto favs = SplitList(larecomp::cvar::GetFlagByName("server_browser_servers"));
            std::string a = add_;
            a.erase(std::remove_if(a.begin(), a.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)); }),
                    a.end());
            if (a.find(':') == std::string::npos) a += ":31000";
            if (std::find(favs.begin(), favs.end(), a) == favs.end()) favs.push_back(a);
            SaveString("server_browser_servers", JoinList(favs));
            add_[0] = 0;
            selected_ = a;
            client_.Refresh();
        }
        ImGui::SameLine();
        if (pending_.valid() && Clock::now() < pending_until_) {
            const char* what = pending_.kind == kCruise ? "Cruise on " : pending_.kind == kRace ? "Race on " : "Join ";
            if (ImGui::Button((what + pending_.label).c_str())) {
                for (const auto& s : snapshot_.servers) {
                    if (s.address != pending_.address) continue;
                    if (pending_.kind != kRoom) Act(s, pending_);
                    for (const auto& r : s.room_list)
                        if (pending_.kind == kRoom && r.host_vip == pending_.vip) Act(s, pending_);
                }
                pending_ = Pending{};
            }
            ImGui::SameLine();
        }
    }

    // While a join is under way the window is closed; this strip says how it goes.
    void DrawJoinStrip(ImGuiIO& io) {
        const br::JoinStatus st = br::GetJoinStatus();
        if (st.text.empty()) return;
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 16.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
        const bool failed = st.phase == br::JoinPhase::kFailed;
        if (mcla::ui::BeginPanel("##larecomp_browser_join", failed ? "JOIN FAILED" : "JOINING",
                                 failed ? mcla::ui::PanelIcon::kAlert : mcla::ui::PanelIcon::kInfo, flags, {},
                                 25.0f)) {
            ImGui::TextColored(failed ? mcla::ui::kColorBad : mcla::ui::kColorTextWhite, "%s", st.text.c_str());
        }
        mcla::ui::EndPanel();
        if (st.phase == br::JoinPhase::kFailed) {
            if (failed_since_ == Clock::time_point{}) failed_since_ = Clock::now();
            if (Clock::now() - failed_since_ > std::chrono::seconds(6)) {
                br::CancelJoin();
                failed_since_ = {};
            }
        } else {
            failed_since_ = {};
        }
    }

    rex::ui::IUISystem* ui_;
    mcla::ui::InputBlock pad_block_;
    br::Client client_;
    br::Snapshot snapshot_;
    std::map<std::string, std::unique_ptr<mcla::ui::IconTexture>> icons_;
    std::string selected_;
    char search_[64] = {};
    char add_[96] = {};
    bool hide_empty_ = false;
    bool hide_dead_ = false;
    int sort_ = 0;
    bool first_frame_ = false;
    bool open_switch_ = false;
    Pending switch_;
    // The mods pop-up.
    bool open_mods_ = false;
    bool mods_checking_ = false;
    Pending mods_action_;
    br::ServerView mods_server_;
    br::ModCheck mods_check_;
    std::shared_ptr<std::pair<std::atomic<bool>, br::ModCheck>> mods_result_;
    std::map<std::string, bool> mods_selected_;  // optional mods ticked
    std::unique_ptr<br::ModDownload> mods_download_;
    std::string checked_mods_;  // address#crc whose mods are all fine here
    Pending pending_;
    Clock::time_point pending_until_{};
    bool pending_opened_ = false;
    Clock::time_point failed_since_{};
};

}  // namespace

void StartServerBrowser(rex::ui::IUISystem& ui, rex::hid::IInputSystem*) {
    if (g_dialog) return;
    // The UI system owns the dialogs; they are always in its list and draw nothing while closed (as on 0.10).
    ui.AddDialog(std::make_unique<BrowserDialog>(ui));
    StartModBrowser(ui);
    // 0.10 replaced the input system's "active" callback here (the game reads input unless an overlay has the mouse,
    // and nothing while a browser is open). 0.11's UI system keeps the game's keyboard and mouse from an overlay that
    // wants them (SetInputCaptured); the rest is ours: the pads through InputBlock above, the keyboard and mouse
    // listeners of LARecomp through ServerBrowserCapturesInput (keyboard_native.cpp, mouse_look.cpp).
}

void ServerBrowserKeyPressed() {
    if (g_dialog) g_dialog->Toggle();
}

void ServerBrowserOpen() { g_open_asked.store(true); }

bool ServerBrowserCapturesInput() {
    return g_open.load(std::memory_order_relaxed) || ModBrowserCapturesInput() || IdentityCapturesInput();
}

bool FeedBrowserGamepad(ImGuiIO& io, bool* b_pressed) { return FeedGamepad(io, b_pressed); }

void ReleaseBrowserGamepad() {
    mcla::ui::ReleaseOverlayPads();
    // The input state compares the next reading with a pad at rest, not with the last one of this dialog.
    larecomp::input::NoteOverlayPad(false, 0, 0.0f, 0.0f, 0.0f, 0.0f);
}

}  // namespace larecomp
