#include "online_toast.h"

#include <cfloat>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>

#include "settings/larecomp_settings.h"
#include <rex/ui/ui_system.h>
#define LARECOMP_TOAST_HAS_ICON 1

#include "larecomp_log.h"
#include "online_platform.h"
#include "relay_info.h"
#include "server_browser/browser_query.h"
#include "system_link.h"
#include "mc_engine/ui/imgui_theme.h"
#include "mc_engine/ui/overlay_support.h"

namespace larecomp {
namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kShowOnChange = std::chrono::seconds(6);
constexpr auto kShowOnPoke = std::chrono::milliseconds(1500);
constexpr auto kQueryEvery = std::chrono::seconds(3);
// How long OnlineToastNotice keeps the pop-up up.
constexpr auto kShowNotice = std::chrono::seconds(5);

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now().time_since_epoch())
        .count();
}

std::atomic<int64_t> g_poke_until_ms{0};

// Only ever later: a short poke never shortens a notice that is still showing.
void PokeUntil(int64_t until_ms) {
    int64_t current = g_poke_until_ms.load();
    while (current < until_ms && !g_poke_until_ms.compare_exchange_weak(current, until_ms)) {
    }
}

enum class Link { kDisconnected, kConnecting, kConnected };

Link CurrentLink() {
    const int pending = SystemLinkOnlinePending();
    if (pending > 0) return Link::kConnecting;
    if (pending == 0 && SystemLinkOnlineConnected()) return Link::kConnected;
    return Link::kDisconnected;
}

// Filled by the worker, read by the dialog.
struct Shared {
    std::mutex mutex;
    relay_info::Info info;
    std::string host;
    uint16_t port = 0;
    std::vector<uint8_t> icon_png;  // new icon bytes waiting for a texture
    uint32_t icon_crc = 0;          // what icon_png / the texture is
    std::atomic<bool> want_info{false};
};

// Asks the relay every few seconds while the pop-up is up, and fetches its
// icon when the icon changes. Never on the UI or guest thread: a query waits
// up to a second for an answer that an older relay never sends.
void Worker(std::shared_ptr<Shared> shared) {
    uint32_t fetched_crc = 0;
    Clock::time_point next{};
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (!shared->want_info.load() || Clock::now() < next) continue;
        next = Clock::now() + kQueryEvery;

        std::string host;
        uint16_t port = 0;
        relay_info::ConfiguredServer(host, port);
        relay_info::Info info = relay_info::Query(host, port, 1000);

        std::vector<uint8_t> icon;
        if (info.ok && info.icon_size && info.icon_crc != fetched_crc) {
            icon = relay_info::FetchIcon(host, port, info.icon_crc, info.icon_size, 1000);
            if (!icon.empty()) fetched_crc = info.icon_crc;
        }

        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->info = info;
        shared->host = host;
        shared->port = port;
        if (!icon.empty()) {
            shared->icon_png = std::move(icon);
            shared->icon_crc = info.icon_crc;
        }
    }
}

class OnlineToastDialog final : public mcla::ui::PanelDialog {
 public:
    explicit OnlineToastDialog(std::shared_ptr<Shared> shared) : shared_(std::move(shared)) {}

 protected:
    void OnDraw(ImGuiIO& io) override {
        if (!kOnlineAvailable) {
            DrawUnavailable(io);
            return;
        }
        const Link link = CurrentLink();
        const auto now = Clock::now();
        if (link != last_link_) {
            // The first look is not a change: a game that boots offline does
            // not need to be told so.
            if (seen_) show_until_ = now + kShowOnChange;
            if (!seen_ && link == Link::kConnected) show_until_ = now + kShowOnChange;
            seen_ = true;
            last_link_ = link;
        }
        const bool poked = NowMs() < g_poke_until_ms.load();
        const bool visible = poked || now < show_until_;
        shared_->want_info.store(visible);
        if (!visible) return;

        relay_info::Info info;
        std::string host;
        uint16_t port = 0;
        {
            std::lock_guard<std::mutex> lock(shared_->mutex);
            info = shared_->info;
            host = shared_->host;
            port = shared_->port;
            TakeIcon();
        }
        if (host.empty()) relay_info::ConfiguredServer(host, port);

        const float pad = 16.0f;
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - pad, pad), ImGuiCond_Always,
                                ImVec2(1.0f, 0.0f));
        // As wide as its lines need, at least 380.
        ImGui::SetNextWindowSizeConstraints(ImVec2(380.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize;

        // The game's race-result card: the title says what this is, the server's
        // own name heads the body in the menu yellow.
        if (mcla::ui::BeginPanel("##larecomp_online_toast", "SYSTEM LINK", mcla::ui::PanelIcon::kInfo, flags,
                                 {}, 25.0f)) {
            constexpr float kIcon = 56.0f;
            if (icon_) {
                ImGui::Image(icon_->Ref(), ImVec2(kIcon, kIcon));
                ImGui::SameLine();
            }
            ImGui::BeginGroup();
            const std::string name =
                info.ok && !info.name.empty() ? info.name : std::string("System Link server");
            ImGui::TextColored(mcla::ui::kColorYellow, "%s", name.c_str());

            switch (link) {
            case Link::kConnected:
                ImGui::TextColored(mcla::ui::kColorGood, "Connected");
                break;
            case Link::kConnecting:
                ImGui::TextColored(mcla::ui::kColorWarn, "Connecting...");
                break;
            case Link::kDisconnected:
                ImGui::TextColored(mcla::ui::kColorBad, "Disconnected");
                break;
            }
            ImGui::TextDisabled("%s:%u", host.c_str(), unsigned(port));
            ImGui::EndGroup();

            ImGui::Separator();
            if (info.ok) {
                ImGui::Text("Players online: %d", info.players);
                ImGui::SameLine();
                ImGui::TextDisabled("  sessions: %d", info.sessions);
                ImGui::Text("Ping: %d ms", info.ping_ms);
            } else {
                ImGui::TextDisabled("Players online: ?");
                ImGui::TextDisabled("No answer from the server's info service");
            }
        }
        mcla::ui::EndPanel();
    }

 private:
    // Linux: there is no connection to show. While the SYSTEM LINK menu is up, or after a press on its ONLINE row or on F6, one
    // sentence says why, in the same card.
    void DrawUnavailable(ImGuiIO& io) {
        if (NowMs() >= g_poke_until_ms.load()) return;
        const float pad = 16.0f;
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - pad, pad), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(380.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize;
        if (mcla::ui::BeginPanel("##larecomp_online_toast", "SYSTEM LINK", mcla::ui::PanelIcon::kInfo, flags,
                                 {}, 25.0f)) {
            ImGui::TextColored(mcla::ui::kColorWarn, "%s.", kOnlineUnavailableText);
        }
        mcla::ui::EndPanel();
    }

    // A new icon from the worker becomes a texture here, in the draw, on the UI thread, where the ImGui context is
    // current (an ImGui texture is made with it). Called with the mutex held.
    void TakeIcon() {
#if defined(LARECOMP_TOAST_HAS_ICON)
        if (shared_->icon_png.empty()) return;
        std::vector<uint8_t> png = std::move(shared_->icon_png);
        shared_->icon_png.clear();
        // The header is read before the decoder allocates what it says (a 64 KB file can claim a huge picture).
        if (browser::PngHeaderFits(png.data(), png.size(), browser::kIconMaxSide))
            icon_ = mcla::ui::IconTexture::FromEncoded(png.data(), png.size(), browser::kIconMaxSide);
        if (!icon_) LARECOMP_APP_WARN("[online] server icon could not be decoded");
#endif
    }

    std::shared_ptr<Shared> shared_;
    std::unique_ptr<mcla::ui::IconTexture> icon_;
    Link last_link_ = Link::kDisconnected;
    bool seen_ = false;
    Clock::time_point show_until_{};
};

}  // namespace

void StartOnlineToast(rex::ui::IUISystem& ui) {
    // Linux: the card says "not available" and asks no server, so there is no worker and no cvar to wait for.
    if (kOnlineAvailable && larecomp::cvar::GetFlagByName("mcla_online_enable").empty()) return;
    auto shared = std::make_shared<Shared>();
    if (kOnlineAvailable) std::thread(Worker, shared).detach();
    ui.AddDialog(std::make_unique<OnlineToastDialog>(shared));
}

void OnlineToastPoke() { PokeUntil(NowMs() + kShowOnPoke.count()); }

void OnlineToastNotice() { PokeUntil(NowMs() + std::chrono::milliseconds(kShowNotice).count()); }

}  // namespace larecomp
