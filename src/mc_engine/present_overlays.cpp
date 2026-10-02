// The ImGui overlays and direct presents. See present_overlays.h.
//
// Everything that touches the presenter or the ImGui drawer runs on the UI
// thread (Reconcile); the main thread, the key handler and the achievement
// callback only post requests to it.

#ifndef REXGLUE_HAS_XEO3_TARGET

#include "present_overlays.h"

#include <atomic>
#include <cstdint>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/achievement_manager.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/presenter.h>
#include <rex/ui/ui_drawer.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include "../native_gfx/nocp/nocp_app.h"
#include "display_clock.h"

namespace mc::present_overlays {

namespace {

// Right above the SDK's ImGui drawer (64), so it runs after it in each paint.
constexpr size_t kWatcherZOrder = 65;
// Nothing of the overlays drawn for this long: they come off.
constexpr double kIdleSeconds = 1.0;
// How long an achievement toast stays up (AchievementNotificationDialog).
constexpr double kToastSeconds = 7.0;

// The binds that open an overlay (rex_app.cpp, SetupOverlays).
constexpr const char* kOverlayBinds[] = {"bind_debug_overlay", "bind_console", "bind_settings",
                                         "bind_achievements"};

std::atomic<bool> g_want_direct{false};
std::atomic<bool> g_attach_request{false};
std::atomic<uint64_t> g_attach_until{0};
std::atomic<bool> g_detached{false};
std::atomic<bool> g_reconcile_posted{false};
std::atomic<bool> g_achievements_hooked{false};

// UI thread only.
bool g_watcher_registered = false;
uint64_t g_last_visible = 0;

double Frequency() {
    static const double f = [] {
        LARGE_INTEGER v;
        QueryPerformanceFrequency(&v);
        return double(v.QuadPart);
    }();
    return f;
}

uint64_t Seconds(double s) { return uint64_t(s * Frequency()); }

void PostReconcile();

// Runs right after the ImGui drawer in every paint while the overlays are on
// the presenter, and notices when they stop drawing anything.
class Watcher final : public rex::ui::UIDrawer {
public:
    void Draw(rex::ui::UIDrawContext&) override {
        const uint64_t now = mc::display_clock::QpcNow();
        // The ImGui drawer leaves its context current and has just rendered
        // it: this is the overlays' output for this paint.
        const ImDrawData* data = ImGui::GetCurrentContext() ? ImGui::GetDrawData() : nullptr;
        if (data && data->TotalVtxCount > 0) {
            g_last_visible = now;
        }
        if (g_want_direct.load(std::memory_order_relaxed) &&
            now > g_attach_until.load(std::memory_order_relaxed) &&
            now - g_last_visible > Seconds(kIdleSeconds)) {
            // Not from inside a UI drawer: the presenter forbids changing its
            // drawers while it runs them.
            PostReconcile();
        }
    }
};
Watcher g_watcher;

void Reconcile() {
    g_reconcile_posted.store(false, std::memory_order_relaxed);
    auto* runtime = rex::Runtime::instance();
    rex::ui::ImGuiDrawer* drawer = runtime ? runtime->imgui_drawer() : nullptr;
    rex::ui::Presenter* presenter = mcla::native_gfx::nocp::PresenterPtr();
    if (!drawer || !presenter) return;

    const uint64_t now = mc::display_clock::QpcNow();
    const bool want_direct = g_want_direct.load(std::memory_order_relaxed);
    if (g_attach_request.exchange(false, std::memory_order_relaxed)) {
        // Give what was asked for time to draw before judging it idle.
        g_last_visible = now;
    }
    const bool keep = !want_direct || now <= g_attach_until.load(std::memory_order_relaxed) ||
                      now - g_last_visible <= Seconds(kIdleSeconds);
    if (keep) {
        if (g_detached.load(std::memory_order_relaxed)) {
            drawer->SetPresenter(presenter);
            g_detached.store(false, std::memory_order_relaxed);
        }
        if (want_direct && !g_watcher_registered) {
            g_last_visible = now;
            presenter->AddUIDrawerFromUIThread(&g_watcher, kWatcherZOrder);
            g_watcher_registered = true;
        } else if (!want_direct && g_watcher_registered) {
            presenter->RemoveUIDrawerFromUIThread(&g_watcher);
            g_watcher_registered = false;
        }
        return;
    }
    if (g_watcher_registered) {
        presenter->RemoveUIDrawerFromUIThread(&g_watcher);
        g_watcher_registered = false;
    }
    if (!g_detached.load(std::memory_order_relaxed)) {
        drawer->SetPresenter(nullptr);
        g_detached.store(true, std::memory_order_relaxed);
    }
}

void PostReconcile() {
    if (g_reconcile_posted.exchange(true, std::memory_order_relaxed)) return;
    auto* runtime = rex::Runtime::instance();
    auto* app_context = runtime ? runtime->app_context() : nullptr;
    if (!app_context) {
        g_reconcile_posted.store(false, std::memory_order_relaxed);
        return;
    }
    app_context->CallInUIThreadDeferred([] { Reconcile(); });
}

void RequestAttach(double hold_seconds) {
    const uint64_t until = mc::display_clock::QpcNow() + Seconds(hold_seconds);
    uint64_t prev = g_attach_until.load(std::memory_order_relaxed);
    while (prev < until &&
           !g_attach_until.compare_exchange_weak(prev, until, std::memory_order_relaxed)) {
    }
    g_attach_request.store(true, std::memory_order_relaxed);
    PostReconcile();
}

// An unlock shows a toast: the overlays have to be on for it.
void HookAchievements() {
    if (g_achievements_hooked.load(std::memory_order_relaxed)) return;
    auto* runtime = rex::Runtime::instance();
    auto* kernel = runtime ? runtime->kernel_state() : nullptr;
    if (!kernel) return;
    g_achievements_hooked.store(true, std::memory_order_relaxed);
    kernel->achievements().RegisterNotificationCallback(
        [](const rex::system::AchievementEvent&) { RequestAttach(kToastSeconds); });
}


// Sees every key ahead of the app (ReXApp listens at Z 0, the overlays at 64)
// and never consumes one.
class KeyWatch final : public rex::ui::WindowInputListener {
public:
    void OnKeyDown(rex::ui::KeyEvent& e) override {
        if (!g_want_direct.load(std::memory_order_relaxed)) return;
        for (const char* bind : kOverlayBinds) {
            const std::string name = rex::cvar::GetFlagByName(bind);
            if (!name.empty() && rex::ui::ParseVirtualKey(name) == e.virtual_key()) {
                // Whether this opens or closes it, the overlays go on: the
                // idle check takes them off again once nothing draws.
                RequestAttach(0.5);
                return;
            }
        }
    }
};
KeyWatch g_key_watch;

}  // namespace

void Install(rex::ui::Window* window) {
    if (window) window->AddInputListener(&g_key_watch, 1000);
}

void Update(bool direct_wanted) {
    if (direct_wanted) HookAchievements();
    const bool before = g_want_direct.exchange(direct_wanted, std::memory_order_relaxed);
    if (before != direct_wanted) PostReconcile();
}

bool Detached() { return g_detached.load(std::memory_order_relaxed); }

}  // namespace mc::present_overlays

#endif  // REXGLUE_HAS_XEO3_TARGET
