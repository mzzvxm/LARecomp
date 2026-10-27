#include "mod_browser_ui.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>

#include <rex/filesystem.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/keybinds.h>
#if __has_include(<rex/ui/image_decode.h>)
#include <rex/ui/image_decode.h>
#define LARECOMP_MODS_HAS_IMAGES 1
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include "larecomp_log.h"
#include "../../online/server_browser/browser_client.h"
#include "mod_download.h"

namespace fs = std::filesystem;

namespace larecomp {
namespace {

namespace br = browser;

std::atomic<bool> g_mod_open{false};
std::atomic<bool> g_mod_open_asked{false};

constexpr const char* kDisabledPrefix = ".disabled_";
constexpr const char* kOldPrefix = ".old_";
constexpr float kThumb = 72.0f;

const ImVec4 kGold(1.0f, 0.86f, 0.25f, 1.0f);
const ImVec4 kGreen(0.45f, 0.9f, 0.45f, 1.0f);
const ImVec4 kAmber(1.0f, 0.8f, 0.3f, 1.0f);
const ImVec4 kRed(0.95f, 0.4f, 0.4f, 1.0f);
const ImVec4 kBlue(0.45f, 0.75f, 1.0f, 1.0f);

// A path from UTF-8 text (fs::u8path is deprecated in C++20).
fs::path FromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

fs::path ModelsDir() {
    const fs::path base = rex::filesystem::GetExecutableFolder();
    std::error_code ec;
    const fs::path modloader = base / "modloader";
    if (fs::is_directory(modloader, ec)) return modloader;
    const fs::path models = base / "models";
    if (fs::is_directory(models, ec)) return models;
    return modloader;
}

std::string Utf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

// ------------------------------------------------------------- installed ---

struct LocalMod {
    std::string name;    // without the prefix
    std::string folder;  // its folder name in models/
    bool enabled = true;
    bool backup = false;  // a .old_ copy the browsers kept
    uint64_t bytes = 0;
    int files = 0;
    br::ModInfo info;
    std::vector<uint8_t> image;
};

std::vector<LocalMod> ScanLocal() {
    std::vector<LocalMod> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(ModelsDir(), ec)) {
        if (!e.is_directory()) continue;
        const std::string folder = Utf8(e.path().filename());
        if (folder == "textures_dump" || folder == "textures_replace" ||
            folder == "music" || folder.rfind('.', 0) == 0) {
            continue;
        }
        LocalMod m;
        m.folder = folder;
        if (folder.rfind(kDisabledPrefix, 0) == 0) {
            m.name = folder.substr(std::strlen(kDisabledPrefix));
            m.enabled = false;
        } else if (folder.rfind(kOldPrefix, 0) == 0) {
            m.name = folder.substr(std::strlen(kOldPrefix));
            m.backup = true;
            m.enabled = false;
        } else if (folder[0] == '.') {
            continue;  // .cache, a download in progress, ...
        } else {
            m.name = folder;
        }
        std::error_code sub;
        for (const auto& f : fs::recursive_directory_iterator(e.path(), sub)) {
            if (sub) break;
            if (!f.is_regular_file(sub)) continue;
            m.bytes += f.file_size(sub);
            ++m.files;
        }
        m.info = br::ReadLocalModInfo(folder);
        m.image = br::ReadLocalModImage(folder, m.info);
        out.push_back(std::move(m));
    }
    std::sort(out.begin(), out.end(), [](const LocalMod& a, const LocalMod& b) {
        if (a.backup != b.backup) return !a.backup;
        return a.name < b.name;
    });
    return out;
}

// ------------------------------------------------------------- available ---

struct Offer {
    std::string server;  // its name or address
    std::string host;
    uint16_t http_port = 0;
    bool required = false;
    br::ServerMod mod;
};

struct Available {
    std::string name;
    std::vector<Offer> offers;  // the highest version first
};

// Every server that answered and has mods: its manifest (with each mod's
// mod.toml and image), compared with here.
std::vector<Available> ScanServers(const br::Snapshot& snap) {
    std::vector<const br::ServerView*> servers;
    for (const auto& s : snap.servers)
        if (s.answered && s.http_port && (s.mods_required + s.mods_optional) > 0) servers.push_back(&s);
    std::vector<br::ModCheck> checks(servers.size());
    std::vector<std::thread> pool;
    std::atomic<size_t> next{0};
    for (int i = 0; i < 6 && size_t(i) < servers.size(); ++i)
        pool.emplace_back([&] {
            for (size_t k; (k = next.fetch_add(1)) < servers.size();)
                checks[k] = br::CheckServerMods(servers[k]->host, servers[k]->http_port, true, true);
        });
    for (auto& t : pool) t.join();

    std::map<std::string, Available> by_name;
    for (size_t k = 0; k < servers.size(); ++k) {
        if (!checks[k].ok) continue;
        for (auto& m : checks[k].mods) {
            Available& a = by_name[m.name];
            a.name = m.name;
            Offer o;
            o.server = servers[k]->name.empty() ? servers[k]->address : servers[k]->name;
            o.host = servers[k]->host;
            o.http_port = servers[k]->http_port;
            o.required = m.required;
            o.mod = std::move(m);
            a.offers.push_back(std::move(o));
        }
    }
    std::vector<Available> out;
    for (auto& [name, a] : by_name) {
        std::stable_sort(a.offers.begin(), a.offers.end(), [](const Offer& x, const Offer& y) {
            return br::CompareVersions(x.mod.info.version, y.mod.info.version) > 0;
        });
        out.push_back(std::move(a));
    }
    return out;
}

// What a server's copy of a mod means for the one here.
enum class Verdict { kSame, kUpdate, kDifferent, kOlderThere, kMissing };

Verdict Judge(const LocalMod* local, const br::ServerMod& remote) {
    if (!local || !local->enabled) return Verdict::kMissing;
    if (remote.state == br::ModState::kInstalled) return Verdict::kSame;
    const std::string& lv = local->info.version;
    const std::string& rv = remote.info.version;
    if (!lv.empty() && !rv.empty()) {
        const int c = br::CompareVersions(rv, lv);
        if (c > 0) return Verdict::kUpdate;
        if (c < 0) return Verdict::kOlderThere;
    }
    // No versions to go by (or the same number with other files).
    return Verdict::kDifferent;
}

// ---------------------------------------------------------------- window ---

class ModBrowserDialog final : public rex::ui::ImGuiDialog {
public:
    ModBrowserDialog(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate)
        : ImGuiDialog(drawer), immediate_(immediate) {}

    void Toggle() {
        if (g_mod_open.load())
            Close_();
        else
            Open_();
    }

protected:
    void OnDraw(ImGuiIO& io) override {
        if (g_mod_open_asked.exchange(false) && !g_mod_open.load()) Open_();
        if (!g_mod_open.load()) return;

        bool b_pressed = false;
        if (FeedBrowserGamepad(io, &b_pressed)) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
        TakeResults();

        const ImVec2 size(std::min(io.DisplaySize.x - 40.0f, 1040.0f), std::min(io.DisplaySize.y - 40.0f, 680.0f));
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing,
                                ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
        ImGui::SetNextWindowBgAlpha(0.96f);
        bool keep_open = true;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
        if (ImGui::Begin("Mod Browser###larecomp_mod_browser", &keep_open,
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
            if (first_frame_) {
                ImGui::SetWindowFocus();
                first_frame_ = false;
            }
            DrawToolbar();
            const int updates = CountUpdates();
            const std::string installed_tab =
                updates ? "Installed (" + std::to_string(updates) + " update" + (updates > 1 ? "s" : "") + ")###inst"
                        : std::string("Installed###inst");
            if (ImGui::BeginTabBar("##tabs")) {
                if (ImGui::BeginTabItem(installed_tab.c_str())) {
                    DrawInstalled();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Available on servers###avail")) {
                    DrawAvailable();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::End();
        ImGui::PopStyleVar();

        const bool typing = ImGui::GetIO().WantTextInput;
        if (!keep_open || ((b_pressed || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) && !typing)) Close_();
    }

private:
    struct Results {
        std::atomic<bool> local_ready{false};
        std::atomic<bool> remote_ready{false};
        std::vector<LocalMod> local;
        std::vector<Available> remote;
    };

    void Open_() {
        g_mod_open.store(true);
        first_frame_ = true;
        client_.SetAutoRefresh(true);
        Rescan();
        LARECOMP_APP_INFO("[mods] browser opened");
    }

    void Close_() {
        g_mod_open.store(false);
        client_.SetAutoRefresh(false);
        auto& io = GetIO();
        io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard);
        io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    }

    // Local folders now; the servers once the server list has come in.
    void Rescan(bool servers_too = true) {
        auto r = std::make_shared<Results>();
        results_ = r;
        scanning_local_ = true;
        scanning_remote_ = servers_too;
        remote_started_ = false;
        std::thread([r] {
            r->local = ScanLocal();
            r->local_ready.store(true);
        }).detach();
        if (servers_too) remote_wait_since_ = std::chrono::steady_clock::now();
    }

    void TakeResults() {
        if (!results_) return;
        if (scanning_local_ && results_->local_ready.load()) {
            local_ = std::move(results_->local);
            scanning_local_ = false;
            for (const auto& m : local_) MakeTexture("local:" + m.folder, m.image);
        }
        // The servers' manifests once their list is in (or after 4 s anyway).
        const br::Snapshot snap = client_.Take();
        if (scanning_remote_ && !remote_started_ &&
            ((!snap.refreshing && snap.generation > 1) ||
             std::chrono::steady_clock::now() - remote_wait_since_ > std::chrono::seconds(4))) {
            remote_started_ = true;
            auto r = results_;
            std::thread([r, snap] {
                r->remote = ScanServers(snap);
                r->remote_ready.store(true);
            }).detach();
        }
        if (scanning_remote_ && remote_started_ && results_->remote_ready.load()) {
            remote_ = std::move(results_->remote);
            scanning_remote_ = false;
            remote_started_ = false;
            for (const auto& a : remote_)
                for (const auto& o : a.offers) MakeTexture("remote:" + a.name + "@" + o.server, o.mod.image);
        }
    }

    void MakeTexture(const std::string& key, const std::vector<uint8_t>& bytes) {
#if defined(LARECOMP_MODS_HAS_IMAGES)
        if (bytes.empty() || !immediate_ || textures_.count(key)) return;
        int w = 0, h = 0;
        std::vector<uint8_t> rgba = rex::ui::DecodeImageRGBA(bytes.data(), bytes.size(), w, h);
        if (rgba.empty() || w <= 0 || h <= 0 || w > 2048 || h > 2048) return;
        auto tex = immediate_->CreateTexture(uint32_t(w), uint32_t(h), rex::ui::ImmediateTextureFilter::kLinear, false,
                                             rgba.data());
        if (tex) textures_[key] = std::move(tex);
#else
        (void)key;
        (void)bytes;
#endif
    }

    // The mod's picture, or a grey square with its initial.
    void Thumb(const std::string& key, const std::string& name) {
        auto it = textures_.find(key);
        if (it != textures_.end()) {
            ImGui::Image(reinterpret_cast<ImTextureID>(it->second.get()), ImVec2(kThumb, kThumb));
            return;
        }
        const ImVec2 p = ImGui::GetCursorScreenPos();
        auto* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + kThumb, p.y + kThumb), IM_COL32(60, 60, 70, 255), 6.0f);
        const char initial[2] = {name.empty() ? '?' : char(std::toupper(static_cast<unsigned char>(name[0]))), 0};
        const ImVec2 ts = ImGui::CalcTextSize(initial);
        dl->AddText(ImVec2(p.x + (kThumb - ts.x) * 0.5f, p.y + (kThumb - ts.y) * 0.5f), IM_COL32(200, 200, 210, 255),
                    initial);
        ImGui::Dummy(ImVec2(kThumb, kThumb));
    }

    // Title, version, author, description: the middle of a card.
    static void Describe(const std::string& name, const br::ModInfo& info, const char* extra = nullptr) {
        ImGui::TextColored(kGold, "%s", info.title.empty() ? name.c_str() : info.title.c_str());
        if (!info.version.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("v%s", info.version.c_str());
        }
        if (!info.author.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("by %s", info.author.c_str());
        }
        if (!info.title.empty() && info.title != name) ImGui::TextDisabled("%s", name.c_str());
        if (!info.description.empty()) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
            ImGui::TextWrapped("%s", info.description.c_str());
            ImGui::PopTextWrapPos();
        }
        if (extra) ImGui::TextDisabled("%s", extra);
    }

    const Available* RemoteFor(const std::string& name) const {
        for (const auto& a : remote_)
            if (a.name == name) return &a;
        return nullptr;
    }

    const LocalMod* LocalFor(const std::string& name) const {
        for (const auto& m : local_)
            if (!m.backup && m.name == name) return &m;
        return nullptr;
    }

    int CountUpdates() const {
        int n = 0;
        for (const auto& m : local_) {
            if (m.backup || !m.enabled) continue;
            if (const Available* a = RemoteFor(m.name))
                if (Judge(&m, a->offers.front().mod) == Verdict::kUpdate) ++n;
        }
        return n;
    }

    void StartDownload(const std::string& name, const Offer& o) {
        LARECOMP_APP_INFO("[mods] downloading {} {} ({}) from {}", name, o.mod.info.version,
                          br::FormatBytes(o.mod.need_bytes), o.server);
        download_name_ = name;
        download_ = std::make_unique<br::ModDownload>(o.host, o.http_port, std::vector<br::ServerMod>{o.mod});
        message_.clear();
    }

    void DrawToolbar() {
        ImGui::SetNextItemWidth(240.0f);
        ImGui::InputTextWithHint("##search", "Search mods...", search_, sizeof(search_));
        ImGui::SameLine();
        if (ImGui::Button(scanning_local_ || scanning_remote_ ? "Scanning..." : "Refresh")) {
            client_.Refresh();
            Rescan();
        }
        if (download_) {
            const auto p = download_->Get();
            ImGui::SameLine();
            const float frac = p.total ? float(double(p.done) / double(p.total)) : 1.0f;
            const std::string label = download_name_ + "  " + br::FormatBytes(p.done) + " / " + br::FormatBytes(p.total);
            ImGui::ProgressBar(frac, ImVec2(320, 0), label.c_str());
            if (p.finished) {
                if (p.failed) {
                    message_ = "Download of " + download_name_ + " failed: " + p.error;
                } else {
                    message_ = download_name_ + " installed.";
                    needs_restart_ = true;
                }
                download_.reset();
                textures_.erase("local:" + download_name_);
                Rescan();
            } else {
                ImGui::SameLine();
                if (ImGui::SmallButton("Cancel")) download_->Cancel();
            }
        }
        if (needs_restart_) {
            ImGui::TextColored(kAmber, "Mods changed: they load on the next start.");
            ImGui::SameLine();
            if (ImGui::Button("Restart now")) {
                if (!br::RestartGame()) message_ = "Could not restart: close and open the game yourself.";
            }
        }
        if (!message_.empty()) ImGui::TextDisabled("%s", message_.c_str());
        ImGui::Separator();
    }

    bool Matches(const std::string& name, const br::ModInfo& info) const {
        if (!search_[0]) return true;
        auto lower = [](std::string s) {
            for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        const std::string needle = lower(search_);
        for (const std::string* field : {&name, &info.title, &info.author, &info.description})
            if (lower(*field).find(needle) != std::string::npos) return true;
        return false;
    }

    bool BeginCards(const char* id) {
        return ImGui::BeginTable(id, 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
                                 ImVec2(0, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing()));
    }

    void SetupCards() {
        ImGui::TableSetupColumn("##img", ImGuiTableColumnFlags_WidthFixed, kThumb + 4);
        ImGui::TableSetupColumn("##info", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, 230.0f);
    }

    void DrawInstalled() {
        if (scanning_local_ && local_.empty()) {
            ImGui::TextDisabled("Reading models/...");
            return;
        }
        if (local_.empty()) ImGui::TextDisabled("No mods in models/.");
        if (BeginCards("##local")) {
            SetupCards();
            for (const auto& m : local_) {
                if (!Matches(m.name, m.info)) continue;
                ImGui::PushID(m.folder.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                Thumb("local:" + m.folder, m.info.title.empty() ? m.name : m.info.title);
                ImGui::TableNextColumn();
                const std::string size = br::FormatBytes(m.bytes) + ", " + std::to_string(m.files) + " files";
                Describe(m.name, m.info, size.c_str());

                ImGui::TableNextColumn();
                if (m.backup) {
                    ImGui::TextDisabled("old copy");
                    if (ImGui::SmallButton("Delete")) Delete(m);
                } else {
                    if (m.enabled)
                        ImGui::TextColored(kGreen, "on");
                    else
                        ImGui::TextColored(kRed, "off");
                    if (ImGui::SmallButton(m.enabled ? "Turn off" : "Turn on")) SwitchMod(m);
                    // Is there a newer one on a server?
                    if (const Available* a = m.enabled ? RemoteFor(m.name) : nullptr) {
                        const Offer& o = a->offers.front();
                        switch (Judge(&m, o.mod)) {
                        case Verdict::kUpdate:
                            ImGui::TextColored(kBlue, "Update: v%s -> v%s", m.info.version.c_str(),
                                               o.mod.info.version.c_str());
                            ImGui::TextDisabled("from %s", o.server.c_str());
                            ImGui::BeginDisabled(download_ != nullptr);
                            if (ImGui::SmallButton("Update")) StartDownload(m.name, o);
                            ImGui::EndDisabled();
                            break;
                        case Verdict::kDifferent:
                            ImGui::TextColored(kAmber, "differs from %s", o.server.c_str());
                            ImGui::BeginDisabled(download_ != nullptr);
                            if (ImGui::SmallButton("Get the server's copy")) StartDownload(m.name, o);
                            ImGui::EndDisabled();
                            break;
                        case Verdict::kOlderThere:
                            ImGui::TextDisabled("newer than %s's", o.server.c_str());
                            break;
                        default:
                            ImGui::TextDisabled("same as %s", o.server.c_str());
                            break;
                        }
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (ImGui::Button("Open the models folder")) OpenFolder();
    }

    void DrawAvailable() {
        if (scanning_remote_ && remote_.empty()) {
            ImGui::TextDisabled("Asking the servers for their mods...");
            return;
        }
        if (remote_.empty()) {
            ImGui::TextDisabled("No server offers mods right now (or none answered).");
            ImGui::TextDisabled("Servers come from online_master_server and server_browser_servers.");
            return;
        }
        if (BeginCards("##remote")) {
            SetupCards();
            for (const auto& a : remote_) {
                const Offer& o = a.offers.front();  // the newest
                if (!Matches(a.name, o.mod.info)) continue;
                ImGui::PushID(a.name.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                Thumb("remote:" + a.name + "@" + o.server, o.mod.info.title.empty() ? a.name : o.mod.info.title);
                ImGui::TableNextColumn();
                std::string where = br::FormatBytes(o.mod.bytes) + "  -  on ";
                for (size_t i = 0; i < a.offers.size(); ++i)
                    where += (i ? ", " : "") + a.offers[i].server + (a.offers[i].required ? " (required)" : "");
                Describe(a.name, o.mod.info, where.c_str());

                ImGui::TableNextColumn();
                const LocalMod* local = LocalFor(a.name);
                const Verdict v = Judge(local, o.mod);
                ImGui::BeginDisabled(download_ != nullptr);
                switch (v) {
                case Verdict::kSame:
                    ImGui::TextColored(kGreen, "installed");
                    break;
                case Verdict::kUpdate:
                    ImGui::TextColored(kBlue, "update: v%s -> v%s", local->info.version.c_str(),
                                       o.mod.info.version.c_str());
                    if (ImGui::Button("Update")) StartDownload(a.name, o);
                    break;
                case Verdict::kDifferent:
                    ImGui::TextColored(kAmber, "you have another copy");
                    if (ImGui::Button("Replace with this one")) StartDownload(a.name, o);
                    break;
                case Verdict::kOlderThere:
                    ImGui::TextDisabled("yours is newer (v%s)", local->info.version.c_str());
                    break;
                case Verdict::kMissing:
                    if (local && !local->enabled) ImGui::TextDisabled("yours is turned off");
                    if (ImGui::Button(("Install  " + br::FormatBytes(o.mod.need_bytes)).c_str()))
                        StartDownload(a.name, o);
                    break;
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    void SwitchMod(const LocalMod& m) {
        const fs::path from = ModelsDir() / FromUtf8(m.folder);
        const fs::path to = ModelsDir() / FromUtf8(m.enabled ? std::string(kDisabledPrefix) + m.name : m.name);
        std::error_code ec;
        if (fs::exists(to, ec)) {
            message_ = "Cannot: models/" + Utf8(to.filename()) + " already exists.";
            return;
        }
        fs::rename(from, to, ec);
        if (ec) {
            message_ = "Cannot rename " + m.folder + " (" + ec.message() + ")";
            return;
        }
        LARECOMP_APP_INFO("[mods] {} turned {}", m.name, m.enabled ? "off" : "on");
        message_ = m.name + (m.enabled ? " turned off." : " turned on.");
        needs_restart_ = true;
        textures_.erase("local:" + m.folder);
        Rescan(false);
    }

    void Delete(const LocalMod& m) {
        std::error_code ec;
        fs::remove_all(ModelsDir() / FromUtf8(m.folder), ec);
        message_ = ec ? "Cannot delete " + m.folder + " (" + ec.message() + ")" : "Deleted " + m.folder + ".";
        textures_.erase("local:" + m.folder);
        Rescan(false);
    }

    static void OpenFolder() {
#ifdef _WIN32
        const std::wstring dir = ModelsDir().wstring();
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
    }

    rex::ui::ImmediateDrawer* immediate_;
    br::Client client_;
    std::shared_ptr<Results> results_;
    bool scanning_local_ = false;
    bool scanning_remote_ = false;
    bool remote_started_ = false;
    std::chrono::steady_clock::time_point remote_wait_since_{};
    std::vector<LocalMod> local_;
    std::vector<Available> remote_;
    std::map<std::string, std::unique_ptr<rex::ui::ImmediateTexture>> textures_;
    std::unique_ptr<br::ModDownload> download_;
    std::string download_name_;
    std::string message_;
    bool needs_restart_ = false;
    bool first_frame_ = false;
    char search_[64] = {};
};

ModBrowserDialog* g_mod_dialog = nullptr;

}  // namespace

void StartModBrowser(rex::ui::ImGuiDrawer* imgui_drawer, rex::ui::ImmediateDrawer* immediate_drawer) {
    if (!imgui_drawer || g_mod_dialog) return;
    g_mod_dialog = new ModBrowserDialog(imgui_drawer, immediate_drawer);
    rex::ui::RegisterBind("bind_mod_browser", "F5", "Open or close the mod browser", [] {
        if (g_mod_dialog) g_mod_dialog->Toggle();
    });
}

void ModBrowserOpen() { g_mod_open_asked.store(true); }

bool ModBrowserCapturesInput() { return g_mod_open.load(std::memory_order_relaxed); }

}  // namespace larecomp
