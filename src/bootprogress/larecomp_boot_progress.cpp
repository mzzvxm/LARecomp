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
#include <rex/ui/windowed_app_context.h>

#include "mc_engine/boot_progress.h"
#include "mc_engine/ui/imgui_theme.h"

namespace larecomp {
namespace {

// A boot with nothing to build finishes in milliseconds, and a popup for that
// would be a flash of a window nobody can read.
constexpr auto kShowAfter = std::chrono::milliseconds(400);

// Shared between the worker's completion and the dialog. The dialog deletes
// itself from inside its own Draw, so nothing else holds a raw pointer to it: a
// close is asked for through `want_close`.
struct OverlayState {
    std::atomic<bool> want_close{false};
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    bool announced = false;  // UI thread only
};

class BootProgressDialog final : public rex::ui::ImGuiDialog {
 public:
    BootProgressDialog(rex::ui::ImGuiDrawer* drawer, std::shared_ptr<OverlayState> state)
        : ImGuiDialog(drawer), state_(std::move(state)) {}

 protected:
    // The popup reads the shared progress until it is gone; clearing it is the
    // last thing that happens to it.
    void OnClose() override { mc::boot::Reset(); }

    void OnDraw(ImGuiIO& io) override {
        if (state_->want_close.load()) {
            Close();
            return;
        }
        // Up from the start so the drawer keeps the window painting, but only
        // visible once the build has clearly taken a while.
        if (std::chrono::steady_clock::now() - state_->started < kShowAfter) {
            return;
        }
        if (!state_->announced) {
            state_->announced = true;
            REXLOG_INFO("Boot content build is taking a while, showing the progress overlay");
        }

        const std::vector<mc::boot::Phase> phases = mc::boot::Read();

        // Bottom-right, out of the way of anything the game puts on screen and
        // where a progress toast is expected to be.
        const float margin = 16.0f;
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - margin, io.DisplaySize.y - margin),
                                ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        // Wide enough for the 52-character lines below in the small face.
        ImGui::SetNextWindowSize(ImVec2(600.0f, 0.0f), ImGuiCond_Always);

        if (mcla::ui::BeginPanel("##larecomp_boot_progress", "PREPARING CONTENT",
                                 mcla::ui::PanelIcon::kInfo,
                                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                     ImGuiWindowFlags_NoSavedSettings |
                                     ImGuiWindowFlags_NoFocusOnAppearing,
                                 {}, 27.0f)) {
            mcla::ui::PanelSubtitle("Loading mods and music before launch.");
            ImGui::Dummy(ImVec2(0.0f, 2.0f));

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
                mcla::ui::SegmentMeter(fraction, ImGui::GetContentRegionAvail().x, 20);

                ImGui::Dummy(ImVec2(0.0f, 4.0f));
            }

            // What actually went in, newest at the bottom. A track name runs
            // longer than the popup, so it is cut instead of being allowed to
            // stretch the window from one frame to the next.
            const std::vector<std::string> recent = mc::boot::Recent();
            if (!recent.empty()) {
                ImGui::Separator();
                ImGui::Dummy(ImVec2(0.0f, 4.0f));
                ImGui::PushFont(nullptr, mcla::ui::kSmallSize);
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
                ImGui::PopFont();
            }
        }
        mcla::ui::EndPanel();
    }

 private:
    std::shared_ptr<OverlayState> state_;
};

// The build in flight, if any. Joined by the completion (on the UI thread) or by
// WaitForBootBuild when the app shuts down first.
std::thread g_boot_worker;

void JoinBootWorker() {
    if (g_boot_worker.joinable() && g_boot_worker.get_id() != std::this_thread::get_id()) {
        g_boot_worker.join();
    }
}

}  // namespace

void StartBootBuildWithOverlay(rex::ui::WindowedAppContext& app_context,
                               rex::ui::ImGuiDrawer* drawer, std::function<void()> work,
                               std::function<void()> then) {
    // No overlay to show it on: run the build the way it always ran.
    if (!drawer || !work) {
        if (work) {
            work();
            mc::boot::Reset();
        }
        if (then) {
            then();
        }
        return;
    }

    JoinBootWorker();
    auto state = std::make_shared<OverlayState>();
    (void)new BootProgressDialog(drawer, state);

    // No nested message loop: the dialog keeps the window painting and the app's
    // own loop runs it. The worker hands the continuation back to the UI thread.
    g_boot_worker = std::thread([&app_context, state, work = std::move(work),
                                 then = std::move(then)]() mutable {
        work();
        const bool queued = app_context.CallInUIThreadDeferred(
            [state, then = std::move(then)]() mutable {
                JoinBootWorker();
                // Draws itself away on its next frame.
                state->want_close = true;
                if (then) {
                    then();
                }
            });
        if (!queued) {
            // The app is already shutting down; WaitForBootBuild joins us.
            state->want_close = true;
        }
    });
}

void WaitForBootBuild() { JoinBootWorker(); }

}  // namespace larecomp
