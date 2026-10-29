#include "identity_ui.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")
#endif

#include "larecomp_log.h"
#include "profile/larecomp_profile_store.h"
#include "../../modloader/modsbrowser/mod_browser_ui.h"
#include "../../modloader/modsbrowser/mod_download.h"
#include "identity_client.h"
#include "identity_store.h"
#include "mc_engine/ui/mcla_imgui_theme.h"

namespace larecomp {

namespace {

namespace id = larecomp::identity;

std::atomic<bool> g_open{false};

std::string When(int64_t unix_time) {
    const std::time_t t = std::time_t(unix_time);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", std::localtime(&t));
    return buf;
}

void CopyToClipboard(const std::string& text) {
#ifdef _WIN32
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1)) {
        std::memcpy(GlobalLock(mem), text.c_str(), text.size() + 1);
        GlobalUnlock(mem);
        SetClipboardData(CF_TEXT, mem);
    }
    CloseClipboard();
#else
    ImGui::SetClipboardText(text.c_str());
#endif
}

// ImGui text is UTF-8; std::filesystem::path(std::string) on Windows reads the
// ANSI code page, so paths go through these.
std::string PathToUtf8(const std::filesystem::path& p) {
#ifdef _WIN32
    const std::wstring w = p.wstring();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), out.data(), n, nullptr, nullptr);
    return out;
#else
    return p.string();
#endif
}

std::filesystem::path Utf8ToPath(const std::string& s) {
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return std::filesystem::path(w);
#else
    return std::filesystem::path(s);
#endif
}

// The Windows file dialog, picking an existing backup or naming a new one.
// Blocking: called off the UI thread. Empty when cancelled.
std::string PickBackupFile(const std::string& start) {
#ifdef _WIN32
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const std::filesystem::path start_path = Utf8ToPath(start);
    wchar_t file[MAX_PATH] = {};
    const std::wstring name = start_path.filename().wstring();
    std::wcsncpy(file, name.c_str(), MAX_PATH - 1);
    const std::wstring dir = start_path.parent_path().wstring();
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetForegroundWindow();
    ofn.lpstrFilter = L"LARecomp identity backup (*.lrid)\0*.lrid\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    ofn.lpstrDefExt = L"lrid";
    ofn.lpstrTitle = L"Identity backup file";
    // A save dialog without the overwrite prompt: it names a new file for
    // Export and picks an existing one for Import.
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    const bool ok = GetSaveFileNameW(&ofn) != 0;
    if (SUCCEEDED(co)) CoUninitialize();
    return ok ? PathToUtf8(std::filesystem::path(file)) : std::string();
#else
    (void)start;
    return {};
#endif
}

// The MCLA palette (mc_engine/ui/mcla_imgui_theme.h) under this file's names.
constexpr ImVec4 kGood = mcla::ui::kColorGood;
constexpr ImVec4 kWarn = mcla::ui::kColorWarn;
constexpr ImVec4 kBad = mcla::ui::kColorBad;
constexpr ImVec4 kDim = mcla::ui::kColorTextDim;

class IdentityDialog final : public rex::ui::ImGuiDialog {
public:
    explicit IdentityDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

    void Toggle() {
        if (g_open.load())
            Close_();
        else
            Open_();
    }

protected:
    void OnDraw(ImGuiIO& io) override {
        status_ = id::GetStatus();
        code_ = id::PendingRecoveryCode();
        AutoOpen();
        if (!g_open.load()) return;

        bool b_pressed = false;
        const bool pad = FeedBrowserGamepad(io, &b_pressed);
        if (pad) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;

        const ImVec2 size(std::min(io.DisplaySize.x - 40.0f, 760.0f), std::min(io.DisplaySize.y - 40.0f, 600.0f));
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing,
                                ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints(ImVec2(560.0f, 360.0f), io.DisplaySize);
        const mcla::ui::Prompt prompts[] = {{"CLOSE", pad ? "B" : "ESC", mcla::ui::kPromptBack}};
        // Dense screen: rows a step smaller, like the retail mission lists.
        ImGui::PushFont(nullptr, mcla::ui::kSmallSize);
        const bool panel = mcla::ui::BeginPanel("###larecomp_identity", "ONLINE IDENTITY", mcla::ui::PanelIcon::kInfo,
                                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                                    ImGuiWindowFlags_NoScrollWithMouse,
                                                prompts);
        // The sections scroll inside the body; the footer bar stays put. EndChild
        // pairs with every BeginChild, whatever it returns.
        const bool body = panel && ImGui::BeginChild("##identity_body",
                                                     ImVec2(0.0f, ImGui::GetContentRegionAvail().y -
                                                                      mcla::ui::PanelBottomReserve()));
        if (body) {
            if (first_frame_) {
                ImGui::SetWindowFocus();
                first_frame_ = false;
            }
            DrawWho();
            ImGui::Separator();
            DrawStatus();
            if (!code_.empty()) {
                ImGui::Separator();
                DrawCode();
            }
            if (status_.incoming_until) {
                ImGui::Separator();
                DrawIncoming();
            }
            if (status_.check == id::Check::kTaken || status_.check == id::Check::kClaimPending) {
                ImGui::Separator();
                DrawChangeNick();
                ImGui::Separator();
                DrawRecover();
            }
            ImGui::Separator();
            DrawBackup();
        }
        if (panel) ImGui::EndChild();
        const bool close_clicked = mcla::ui::EndPanel() == 0;
        ImGui::PopFont();

        const bool typing = ImGui::GetIO().WantTextInput;
        if (close_clicked || ((b_pressed || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) && !typing)) Close_();
    }

private:
    // Opens once for each thing that needs the player: a code to write down,
    // someone claiming the nick, the nick being taken.
    void AutoOpen() {
        if (g_open.load()) return;
        std::string reason;
        if (!code_.empty() && code_ != shown_code_ && CodeReminderDue()) {
            shown_code_ = code_;
            reason = "code";
        } else if (status_.incoming_until && status_.incoming_until != shown_incoming_) {
            shown_incoming_ = status_.incoming_until;
            reason = "incoming";
        } else if (status_.check == id::Check::kTaken && !shown_taken_) {
            shown_taken_ = true;
            reason = "taken";
        }
        if (status_.check != id::Check::kTaken) shown_taken_ = false;
        if (!reason.empty()) {
            LARECOMP_APP_INFO("[identity] window opened ({})", reason);
            Open_();
        }
    }

    // The player can put the code's window off for 30 days or for good; F8
    // still shows it.
    static bool CodeReminderDue() {
        const int64_t after = id::RecoveryCodeRemindAfter();
        if (after == id::kNeverRemind) return false;
        return after == 0 || int64_t(std::time(nullptr)) >= after;
    }

    void DrawWho() {
        if (!id::Loaded()) {
            ImGui::TextColored(kBad, "No online identity (no profile).");
            return;
        }
        ImGui::Text("Gamertag: %s", id::Nick().c_str());
        ImGui::TextColored(kDim, "XUID %016llX   key", static_cast<unsigned long long>(id::Xuid()));
        // The key in the neon teal, so it stands out from the XUID.
        ImGui::SameLine();
        ImGui::TextColored(mcla::ui::kColorCyanGlow, "%s", id::KeyFingerprint().c_str());
        ImGui::TextWrapped(
            "Your gamertag is protected by a key kept on this PC, outside your save folder: "
            "sharing a save does not share it. Use a backup or your recovery code to keep "
            "the nick on another PC.");
        if (id::RestartNeeded()) {
            ImGui::TextColored(kWarn, "A backup was imported: restart the game to use it.");
            ImGui::SameLine();
            if (ImGui::Button("Restart now")) browser::RestartGame();
        }
    }

    void DrawStatus() {
        ImVec4 color = kDim;
        const char* title = "";
        switch (status_.check) {
            case id::Check::kOff: title = "Online is off."; break;
            case id::Check::kWorking: title = "Checking..."; break;
            case id::Check::kNoAnswer: title = "No nick check from the server"; color = kDim; break;
            case id::Check::kNotChecked: title = "Server does not check nicks"; color = kWarn; break;
            case id::Check::kVerified: title = "Nick verified"; color = kGood; break;
            case id::Check::kTaken: title = "Nick taken"; color = kBad; break;
            case id::Check::kClaimPending: title = "Nick coming to you"; color = kWarn; break;
            case id::Check::kMasterDown: title = "Nick server unreachable"; color = kBad; break;
            case id::Check::kFailed: title = "Nick check failed"; color = kBad; break;
        }
        ImGui::TextColored(color, "%s", title);
        if (!status_.relay.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(kDim, "  server %s%s%s", status_.relay.c_str(),
                               status_.master.empty() ? "" : ", nicks by ", status_.master.c_str());
        }
        if (!status_.detail.empty()) ImGui::TextWrapped("%s", status_.detail.c_str());
        if (status_.check == id::Check::kTaken)
            ImGui::TextWrapped(
                "Use another gamertag below, or, if this nick is yours from another PC, restore its "
                "backup or enter its recovery code.");
        if (status_.check == id::Check::kClaimPending && status_.claim_until)
            ImGui::TextWrapped("It moves to you on %s, unless its current owner cancels. Until then, "
                               "this server keeps refusing it.",
                               When(status_.claim_until).c_str());
        if (status_.check != id::Check::kOff && ImGui::Button("Check again")) id::CheckNow();
    }

    void DrawCode() {
        ImGui::TextColored(kWarn, "Your recovery code");
        // Large enough to copy out by hand. PushFont with a size, since
        // SetWindowFontScale is ImGui's legacy scaling.
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        ImGui::PushFont(nullptr, mcla::ui::kBodySize * 1.4f);
        ImGui::TextUnformatted(code_.c_str());
        ImGui::PopFont();
        ImGui::PopStyleColor();
        ImGui::TextWrapped(
            "Write it down somewhere safe (paper, a password manager). It is the only way to get this "
            "nick back without a backup, and it is shown only now. Anyone with it can claim the nick, "
            "but you get days to cancel from the game.");
        if (ImGui::Button("Copy")) CopyToClipboard(code_);
        ImGui::SameLine();
        if (ImGui::Button("I wrote it down")) id::AcknowledgeRecoveryCode();
        ImGui::SameLine();
        if (ImGui::Button("Remind me in 30 days")) {
            id::SetRecoveryCodeRemindAfter(int64_t(std::time(nullptr)) + 30 * 24 * 60 * 60);
            Close_();
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't remind me")) {
            id::SetRecoveryCodeRemindAfter(id::kNeverRemind);
            Close_();
        }
        const int64_t after = id::RecoveryCodeRemindAfter();
        if (after == id::kNeverRemind)
            ImGui::TextColored(kDim, "Not shown at startup any more; it stays here (F8) until you confirm.");
        else if (after > 0)
            ImGui::TextColored(kDim, "Shown at startup again on %s.", When(after).c_str());
    }

    void DrawIncoming() {
        ImGui::TextColored(kBad, "Someone is claiming your nick");
        ImGui::TextWrapped(
            "Your recovery code was used to move \"%s\" to another PC. It moves on %s. If that was "
            "not you, cancel now: you will get a new code.",
            id::Nick().c_str(), When(status_.incoming_until).c_str());
        if (ImGui::Button("Cancel the transfer")) id::RequestCancelTransfer();
    }

    // The nick belongs to someone else: file the save under another gamertag.
    // The SDK renames the save folder when user_name names a folder that does
    // not exist yet and .profile still names the current one (the same move as
    // editing user_name by hand), so the key, the XUID and the save stay with
    // the player; only the name changes. Takes effect after a restart.
    void DrawChangeNick() {
        ImGui::Text("Use another gamertag");
        if (nick_done_) {
            ImGui::TextColored(kGood, "Gamertag set to %s. Restart the game to use it.", nick_done_name_.c_str());
            if (ImGui::Button("Restart now")) browser::RestartGame();
            return;
        }
        ImGui::SetNextItemWidth(FieldWidth("new gamertag (up to 15)"));
        ImGui::InputTextWithHint("##newnick", "new gamertag (up to 15)", newnick_, sizeof(newnick_));
        ImGui::SameLine();
        if (ImGui::Button("Change gamertag")) ApplyNewNick();
        if (!nick_msg_.empty()) ImGui::TextColored(kBad, "%s", nick_msg_.c_str());
    }

    void ApplyNewNick() {
        namespace pf = larecomp::profiles;
        nick_msg_.clear();
        const std::string typed = newnick_;
        const std::string name = pf::SanitizeName(typed);
        const std::string current = id::Nick();
        const std::filesystem::path root = id::UserDataRoot();
        if (name.empty() || name != typed) {
            nick_msg_ = "Use only letters, digits and spaces, up to 15 characters.";
        } else if (pf::IsReservedName(name)) {
            nick_msg_ = "That name is reserved.";
        } else if (pf::SameName(name, current)) {
            nick_msg_ = "That is already your gamertag.";
        } else if (root.empty()) {
            nick_msg_ = "The save folder is not known; change user_name by hand.";
        } else {
            std::error_code ec;
            if (std::filesystem::exists(root / name, ec)) {
                nick_msg_ = "You already have a save with that name.";
            } else if (!std::filesystem::exists(root / current, ec)) {
                nick_msg_ = "Your save folder was not found; change user_name by hand.";
            }
        }
        if (!nick_msg_.empty()) return;

        // The content manager only renames the folder .profile names; make it
        // name the one in use now.
        if (!pf::WriteMarker(root, current)) {
            nick_msg_ = "Could not write the save marker.";
            return;
        }
        rex::cvar::SetFlagByName("user_name", name);
        std::vector<pf::ConfigEdit> edits;
        edits.push_back({"user_name", pf::QuoteTomlString(name)});
        if (!pf::EditConfig(rex::filesystem::GetExecutableFolder() / "larecomp.toml", edits)) {
            nick_msg_ = "Could not write larecomp.toml.";
            return;
        }
        LARECOMP_APP_INFO("[identity] gamertag \"{}\" -> \"{}\" (the SDK renames the save on the next boot)",
                          current, name);
        nick_done_name_ = name;
        nick_done_ = true;
        std::memset(newnick_, 0, sizeof(newnick_));
    }

    void DrawRecover() {
        ImGui::Text("Recover this nick with its code");
        ImGui::SetNextItemWidth(FieldWidth("XXXXX-XXXXX-XXXXX-XXXXX"));
        ImGui::InputTextWithHint("##code", "XXXXX-XXXXX-XXXXX-XXXXX", recover_, sizeof(recover_));
        ImGui::SameLine();
        if (ImGui::Button("Recover") && recover_[0]) {
            id::RequestRecover(recover_);
            std::memset(recover_, 0, sizeof(recover_));
        }
    }

    // A text field as wide as what it is meant to hold, in the current font.
    static float FieldWidth(const char* sample) {
        return ImGui::CalcTextSize(sample).x + ImGui::GetStyle().FramePadding.x * 2.0f + 8.0f;
    }

    void DrawBackup() {
        ImGui::Text("Backup");
        ImGui::TextWrapped("A file with your key, locked with a password. Keep it off this PC.");
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputTextWithHint("##pw1", "password", pw1_, sizeof(pw1_), ImGuiInputTextFlags_Password);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputTextWithHint("##pw2", "again (to export)", pw2_, sizeof(pw2_), ImGuiInputTextFlags_Password);
        {
            std::lock_guard<std::mutex> lk(msg_mtx_);
            if (!picked_.empty()) {
                std::snprintf(path_, sizeof(path_), "%s", picked_.c_str());
                picked_.clear();
            }
        }
        if (path_[0] == 0) {
            const std::string p = PathToUtf8(id::DefaultBackupPath());
            std::snprintf(path_, sizeof(path_), "%s", p.c_str());
        }
        const ImGuiStyle& style = ImGui::GetStyle();
        const float browse_w = ImGui::CalcTextSize("Browse...").x + style.FramePadding.x * 2.0f;
        ImGui::SetNextItemWidth(-(browse_w + style.ItemSpacing.x));
        ImGui::InputText("##path", path_, sizeof(path_));
        ImGui::SameLine();
        const bool picking = picking_.load();
        if (picking) ImGui::BeginDisabled();
        if (ImGui::Button("Browse...")) Browse();
        if (picking) ImGui::EndDisabled();
        const bool busy = busy_.load();
        if (busy) ImGui::BeginDisabled();
        if (ImGui::Button("Export")) {
            if (std::string(pw1_) != pw2_)
                backup_msg_ = "the two passwords differ";
            else
                RunBackup(true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Import")) RunBackup(false);
        if (busy) ImGui::EndDisabled();
        if (busy) {
            ImGui::SameLine();
            ImGui::TextColored(kDim, "working...");
        }
        std::string msg;
        {
            std::lock_guard<std::mutex> lk(msg_mtx_);
            msg = backup_msg_;
        }
        if (!msg.empty()) ImGui::TextWrapped("%s", msg.c_str());
    }

    // The dialog blocks its thread until it closes: not the UI's.
    void Browse() {
        picking_.store(true);
        const std::string start = path_;
        std::thread([this, start]() {
            const std::string file = PickBackupFile(start);
            std::lock_guard<std::mutex> lk(msg_mtx_);
            if (!file.empty()) picked_ = file;
            picking_.store(false);
        }).detach();
    }

    // Argon2 takes about a second: off the UI thread.
    void RunBackup(bool do_export) {
        busy_.store(true);
        std::string pw = pw1_;  // wiped by the worker
        const std::string path = path_;
        std::memset(pw1_, 0, sizeof(pw1_));
        std::memset(pw2_, 0, sizeof(pw2_));
        std::thread([this, do_export, pw, path]() mutable {
            std::string error;
            const std::filesystem::path file = Utf8ToPath(path);
            const bool ok = do_export ? id::ExportBackup(file, pw, &error) : id::ImportBackup(file, pw, &error);
            crypto_wipe(pw.data(), pw.size());
            std::lock_guard<std::mutex> lk(msg_mtx_);
            backup_msg_ = ok ? (do_export ? "Backup saved to " + path : std::string("Backup imported. Restart the game."))
                             : error;
            busy_.store(false);
        }).detach();
    }

    void Open_() {
        g_open.store(true);
        first_frame_ = true;
    }

    void Close_() {
        g_open.store(false);
        std::memset(pw1_, 0, sizeof(pw1_));
        std::memset(pw2_, 0, sizeof(pw2_));
        auto& io = GetIO();
        io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard);
        // As the mod browser does: no pad left claimed once the window is gone.
        io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    }

    id::Status status_;
    std::string code_;
    std::string shown_code_;
    int64_t shown_incoming_ = 0;
    bool shown_taken_ = false;
    bool first_frame_ = false;
    char recover_[40] = {};
    char newnick_[16] = {};
    std::string nick_msg_;
    std::string nick_done_name_;
    bool nick_done_ = false;
    char pw1_[128] = {};
    char pw2_[128] = {};
    char path_[512] = {};
    std::atomic<bool> busy_{false};
    std::atomic<bool> picking_{false};
    std::mutex msg_mtx_;
    std::string backup_msg_;  // guarded by msg_mtx_
    std::string picked_;      // guarded by msg_mtx_: the file dialog's answer
};

IdentityDialog* g_dialog = nullptr;

}  // namespace

void StartIdentity(rex::ui::ImGuiDrawer* imgui_drawer) {
    if (!imgui_drawer || g_dialog) return;
    g_dialog = new IdentityDialog(imgui_drawer);
    rex::ui::RegisterBind("bind_identity", "F8", "Open or close the online identity window", [] {
        if (g_dialog) g_dialog->Toggle();
    });
    id::StartClient();
}

bool IdentityCapturesInput() { return g_open.load(std::memory_order_relaxed); }

}  // namespace larecomp
