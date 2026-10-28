#include "online_toast.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>
#if __has_include(<rex/ui/image_decode.h>)
#include <rex/ui/image_decode.h>
#define LARECOMP_TOAST_HAS_ICON 1
#endif

#include "larecomp_log.h"
#include "relay_info.h"
#include "system_link.h"

namespace larecomp {
namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kShowOnChange = std::chrono::seconds(6);
constexpr auto kShowOnPoke = std::chrono::milliseconds(1500);
constexpr auto kQueryEvery = std::chrono::seconds(3);

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now().time_since_epoch())
        .count();
}

std::atomic<int64_t> g_poke_until_ms{0};

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

class OnlineToastDialog final : public rex::ui::ImGuiDialog {
 public:
    OnlineToastDialog(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate,
                      std::shared_ptr<Shared> shared)
        : ImGuiDialog(drawer), immediate_(immediate), shared_(std::move(shared)) {}

 protected:
    void OnDraw(ImGuiIO& io) override {
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
        ImGui::SetNextWindowSize(ImVec2(360.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.88f);
        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoFocusOnAppearing;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f);
        if (ImGui::Begin("##larecomp_online_toast", nullptr, flags)) {
            constexpr float kIcon = 56.0f;
            if (icon_) {
                ImGui::Image(reinterpret_cast<ImTextureID>(icon_.get()), ImVec2(kIcon, kIcon));
                ImGui::SameLine();
            }
            ImGui::BeginGroup();
            const std::string name =
                info.ok && !info.name.empty() ? info.name : std::string("System Link server");
            ImGui::TextColored(ImVec4(1.0f, 0.86f, 0.25f, 1.0f), "%s", name.c_str());

            switch (link) {
            case Link::kConnected:
                ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.0f), "Connected");
                break;
            case Link::kConnecting:
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Connecting...");
                break;
            case Link::kDisconnected:
                ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.4f, 1.0f), "Disconnected");
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
        ImGui::End();
        ImGui::PopStyleVar();
    }

 private:
    // A new icon from the worker becomes a texture here, on the UI thread,
    // where the drawer may be used. Called with the mutex held.
    void TakeIcon() {
#if defined(LARECOMP_TOAST_HAS_ICON)
        if (shared_->icon_png.empty() || !immediate_) return;
        std::vector<uint8_t> png = std::move(shared_->icon_png);
        shared_->icon_png.clear();
        int width = 0;
        int height = 0;
        std::vector<uint8_t> rgba = rex::ui::DecodeImageRGBA(png.data(), png.size(), width, height);
        if (rgba.empty() || width <= 0 || height <= 0 || width > 512 || height > 512) {
            LARECOMP_APP_WARN("[online] server icon could not be decoded");
            return;
        }
        icon_ = immediate_->CreateTexture(uint32_t(width), uint32_t(height),
                                          rex::ui::ImmediateTextureFilter::kLinear, false,
                                          rgba.data());
        if (!icon_) LARECOMP_APP_WARN("[online] this renderer gave the server icon no texture");
#endif
    }

    rex::ui::ImmediateDrawer* immediate_;
    std::shared_ptr<Shared> shared_;
    std::unique_ptr<rex::ui::ImmediateTexture> icon_;
    Link last_link_ = Link::kDisconnected;
    bool seen_ = false;
    Clock::time_point show_until_{};
};

}  // namespace

void StartOnlineToast(rex::ui::ImGuiDrawer* imgui_drawer,
                      rex::ui::ImmediateDrawer* immediate_drawer) {
    if (!imgui_drawer || rex::cvar::GetFlagByName("mcla_online_enable").empty()) return;
    auto shared = std::make_shared<Shared>();
    std::thread(Worker, shared).detach();
    (void)new OnlineToastDialog(imgui_drawer, immediate_drawer, shared);
}

void OnlineToastPoke() { g_poke_until_ms.store(NowMs() + kShowOnPoke.count()); }

}  // namespace larecomp
