#include "larecomp_boot_progress.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>

#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include "mc_engine/boot_progress.h"

namespace larecomp {
namespace {

// Shared between the runner and the dialog. The dialog deletes itself from
// inside its own Draw, so the runner never holds a raw pointer to it: it asks
// for a close through `want_close` and waits for `alive` to go false.
struct OverlayState {
    std::atomic<bool> want_close{false};
    std::atomic<bool> alive{true};
};

class BootProgressDialog final : public rex::ui::ImGuiDialog {
 public:
    BootProgressDialog(rex::ui::ImGuiDrawer* drawer, std::shared_ptr<OverlayState> state)
        : ImGuiDialog(drawer), state_(std::move(state)) {}

 protected:
    void OnClose() override { state_->alive = false; }

    void OnDraw(ImGuiIO& io) override {
        if (state_->want_close.load()) {
            Close();
            return;
        }

        const std::vector<mc::boot::Phase> phases = mc::boot::Read();

        // Bottom-right, out of the way of anything the game puts on screen and
        // where a progress toast is expected to be.
        const float margin = 16.0f;
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - margin, io.DisplaySize.y - margin),
                                ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.88f);

        if (ImGui::Begin("Preparing content##larecomp_boot_progress", nullptr,
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing)) {
            ImGui::TextUnformatted("Installing mods and music before the game starts.");
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0.0f, 4.0f));

            if (phases.empty()) {
                ImGui::TextDisabled("Looking for content...");
            }

            for (const mc::boot::Phase& phase : phases) {
                ImGui::TextUnformatted(phase.name.c_str());
                ImGui::SameLine();

                std::string count;
                if (phase.total > 0) {
                    count = std::to_string(phase.done) + " / " + std::to_string(phase.total);
                } else {
                    count = std::to_string(phase.done);
                }
                // Right-aligned, so the numbers do not dance as they grow.
                const float width = ImGui::CalcTextSize(count.c_str()).x;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x -
                                     width);
                ImGui::TextUnformatted(count.c_str());

                const float fraction =
                    phase.finished ? 1.0f
                                   : (phase.total > 0 ? static_cast<float>(phase.done) /
                                                            static_cast<float>(phase.total)
                                                      : 0.0f);
                ImGui::ProgressBar(fraction, ImVec2(-1.0f, 6.0f), "");

                ImGui::Dummy(ImVec2(0.0f, 4.0f));
            }

            // What actually went in, newest at the bottom. A track name runs
            // longer than the popup, so it is cut instead of being allowed to
            // stretch the window from one frame to the next.
            const std::vector<std::string> recent = mc::boot::Recent();
            if (!recent.empty()) {
                ImGui::Separator();
                ImGui::Dummy(ImVec2(0.0f, 4.0f));
                for (size_t i = 0; i < recent.size(); ++i) {
                    std::string line = recent[i];
                    if (line.size() > 52) line = line.substr(0, 49) + "...";
                    // The newest one is the one being worked on; the rest are
                    // there for context and stay quiet.
                    if (i + 1 == recent.size()) {
                        ImGui::TextUnformatted(line.c_str());
                    } else {
                        ImGui::TextDisabled("%s", line.c_str());
                    }
                }
            }
        }
        ImGui::End();
    }

 private:
    std::shared_ptr<OverlayState> state_;
};

}  // namespace

void RunBootBuildWithOverlay(rex::ui::WindowedAppContext& app_context, rex::ui::Window* window,
                             rex::ui::ImGuiDrawer* drawer, std::function<void()> work) {
    if (!work) return;

    // No overlay to show it on: run the build the way it always ran.
    if (!drawer) {
        work();
        return;
    }

    std::atomic<bool> done{false};
    std::thread worker([&work, &done]() {
        work();
        done = true;
    });

    // A boot with nothing to build finishes in milliseconds. Showing a popup for
    // that would be a flash of a window nobody can read, so the overlay only
    // comes up once the build is clearly going to take a while.
    const auto started = std::chrono::steady_clock::now();
    std::shared_ptr<OverlayState> state;

    while (!done.load()) {
        if (!state && std::chrono::steady_clock::now() - started > std::chrono::milliseconds(400)) {
            state = std::make_shared<OverlayState>();
            (void)new BootProgressDialog(drawer, state);
            REXLOG_INFO("Boot content build is taking a while, showing the progress overlay");
        }

        app_context.ExecutePendingFunctionsFromUIThread();
        app_context.PumpEvents();
        if (app_context.HasQuitFromUIThread()) {
            break;
        }
        if (state && window) {
            window->RequestPaint();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    worker.join();

    if (state) {
        // Let it draw itself away: the dialog deletes itself from inside Draw,
        // and the runner has no business deleting it from out here.
        state->want_close = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (state->alive.load() && std::chrono::steady_clock::now() < deadline &&
               !app_context.HasQuitFromUIThread()) {
            app_context.ExecutePendingFunctionsFromUIThread();
            app_context.PumpEvents();
            if (window) {
                window->RequestPaint();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
    }

    mc::boot::Reset();
}

}  // namespace larecomp
