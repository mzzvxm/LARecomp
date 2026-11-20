// The Director panel: see director_panel.h. The logic that does not draw is in director_panel_core.h, director_timeline.h and director_gizmos.h (tested without a game); this file is
// the ImGui around it.

#include "director_panel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

#include "director_gizmos.h"
#include "director_panel_core.h"
#include "director_text.h"
#include "director_timeline.h"
#include "imgui_theme.h"
#include "larecomp_log.h"
#include "mc_engine/cinematic/director/director.h"
#include "mc_engine/cinematic/freecam_runtime.h"
#include "overlay_support.h"
#include "settings/larecomp_settings.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace mcla::ui {

namespace {

namespace dp = mcla::ui::director;
namespace dir = mc::cinematic::director;
namespace fc = mc::cinematic::freecam;

constexpr float kFontSize = 17.0f;
constexpr int kWaypointBase = 1000;  // a gizmo dot with an id of this or more is waypoint (id - this); a smaller one is a car's index

enum Tab { kTabCamera, kTabRigs, kTabCars, kTabPaths, kTabTake, kTabAgent, kTabCount };
const char* const kTabNames[kTabCount] = {"CAMERA", "RIGS", "CARS", "PATHS", "TAKE", "AGENT"};

constexpr ImU32 kYellow = IM_COL32(242, 227, 5, 255);
constexpr ImU32 kCyan = IM_COL32(51, 214, 214, 255);
constexpr ImU32 kRed = IM_COL32(238, 85, 68, 255);
constexpr ImU32 kGreen = IM_COL32(120, 210, 80, 255);
constexpr ImU32 kWhite = IM_COL32(235, 235, 228, 255);
constexpr ImU32 kDim = IM_COL32(140, 140, 132, 255);

ImU32 MarkerColor(const dp::Marker& m) {
    switch (m.kind) {
        case dir::EventKind::kCamera: return kCyan;
        case dir::EventKind::kCut: return kRed;
        case dir::EventKind::kCar: return kGreen;
        case dir::EventKind::kWorld:
        case dir::EventKind::kSetting: return IM_COL32(150, 140, 255, 255);
        case dir::EventKind::kCapture: return IM_COL32(255, 140, 90, 255);
        case dir::EventKind::kHud: return IM_COL32(200, 200, 190, 255);
        default: return IM_COL32(220, 200, 150, 255);
    }
}

ImVec4 ToVec4(ImU32 c) {
    return ImGui::ColorConvertU32ToFloat4(c);
}

// The pad's chords (freecam.h Events) arrive on the GAME thread, from the flight's own step: they become commands from there (Post is safe from any thread). The take's form is the
// panel's, copied here every frame it is open, so Back + D-pad up renders what the TAKE tab says.
std::mutex g_hook_mutex;
dp::TakeForm g_hook_take;

void OnFlyEvents(const fc::Events& events) {
    if (!events.Any()) return;
    const std::shared_ptr<const dir::Snapshot> snap = dir::GetSnapshot();
    if (!snap || !snap->director) return;
    dp::TakeForm take;
    {
        std::lock_guard<std::mutex> lock(g_hook_mutex);
        take = g_hook_take;
    }
    for (dp::Cmd& c : dp::CommandsForFlyEvents(events, snap.get(), take)) {
        dir::Posted p;
        p.cmd = c.cmd;
        p.args = std::move(c.args);
        p.source = dir::Source::kPanel;
        p.wait_settled = c.wait_settled;
        p.timeout_s = c.timeout_s;
        dir::Post(std::move(p));
    }
}

// An edit of the scene file that waits for the panel to hold the file's current text (an edit needs the `expect` of its line).
struct QueuedEdit {
    std::string key;  // an edit with the same key replaces the one that waits (a slider's drag is one edit, the last value)
    std::function<dp::Cmd(const dp::EditContext&, const dir::Snapshot&)> build;
};

// What the event listener hands the UI thread (a take that ended).
struct SharedEvents {
    std::mutex mutex;
    std::string take_folder;
    std::string take_state;
    uint64_t take_done_serial = 0;
};

class DirectorPanel final : public rex::ui::DialogBase {
  public:
    explicit DirectorPanel(rex::ui::IUISystem& ui)
        : ui_(ui), disp_([](dir::Posted&& p) { return dir::Post(std::move(p)); }), events_(std::make_shared<SharedEvents>()) {
        std::shared_ptr<SharedEvents> ev = events_;
        listener_ = dir::AddEventListener([ev](std::string_view name, const dir::Json& e) {
            if (name != "take.done") return;
            std::lock_guard<std::mutex> lock(ev->mutex);
            ev->take_folder = e.StringOr("folder", "");
            ev->take_state = e.StringOr("state", "");
            ++ev->take_done_serial;
        });
        std::memset(search_, 0, sizeof(search_));
        std::strncpy(route_name_, "path1", sizeof(route_name_) - 1);
        std::strncpy(rec_name_, "drive1", sizeof(rec_name_) - 1);
        std::memset(take_name_, 0, sizeof(take_name_));
        std::memset(console_, 0, sizeof(console_));
    }
    ~DirectorPanel() override { dir::RemoveEventListener(listener_); }

    bool NeedsContinuousPaint() const override { return open_; }

    bool Draw(ImGuiIO& io) override {
        EnsureMclaTheme();
        now_ = ImGui::GetTime();
        snap_ = dir::GetSnapshot();
        const bool want = larecomp::settings::mcla_director_panel.Get();
        if (snap_ && snap_->director && snap_->ui_request_serial != seen_ui_serial_) {
            seen_ui_serial_ = snap_->ui_request_serial;
            geom_dirty_ = true;
            if (OpenFor(snap_->ui_request) && !want) larecomp::cvar::SetFlagByName("mcla_director_panel", "true");
        }
        if (!want) {
            if (open_) Closed();
            return true;
        }
        if (!open_) Opened();
        take_.name = take_name_;
        {
            std::lock_guard<std::mutex> lock(g_hook_mutex);
            g_hook_take = take_;
        }
        disp_.Drain(now_);
        if (!snap_ || !snap_->director) {
            DrawOffNotice();
            return true;
        }
        Frame(io, *snap_);
        return true;
    }

  private:
    // ------------------------------------------------------------------------------------------------------------ lifecycle

    bool OpenFor(const std::string& panel) {
        if (panel.empty()) return false;
        for (int i = 0; i < kTabCount; ++i) {
            if (dp::Lower(panel) == dp::Lower(kTabNames[i])) want_tab_ = i;
        }
        hidden_ = false;
        return true;
    }

    void Opened() {
        open_ = true;
        axis_fit_ = false;
        layout_rev_ = -1;
        scene_text_ = dp::SceneText();
        text_inflight_ = false;
        LARECOMP_APP_INFO("[dirpanel] the Director panel is open (Ctrl+F12 or the setting mcla_director_panel closes it)");
    }

    void Closed() {
        open_ = false;
        queue_.clear();
        drag_ = dp::Drag();
        LARECOMP_APP_INFO("[dirpanel] the Director panel is closed");
    }

    void DrawOffNotice() {
        ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.85f);
        if (ImGui::Begin("##mcla_dir_off", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoMove)) {
            ImGui::PushFont(nullptr, kFontSize);
            ImGui::TextColored(ToVec4(kYellow), "DIRECTOR PANEL");
            ImGui::TextUnformatted("The Director (the command layer) is off, so the panel has nothing to command.");
            if (ImGui::Button("SWITCH THE DIRECTOR ON")) larecomp::cvar::SetFlagByName("mcla_director", "true");
            ImGui::SameLine();
            if (ImGui::Button("CLOSE")) larecomp::cvar::SetFlagByName("mcla_director_panel", "false");
            ImGui::PopFont();
        }
        ImGui::End();
    }

    // ------------------------------------------------------------------------------------------------------------ one frame

    void Frame(ImGuiIO& io, const dir::Snapshot& s) {
        Sync(s);
        Shortcuts(io, s);
        FeedFly(io, s);
        FlushEdits(s);
        view_ = dp::Projector::Make(s.camera, io.DisplaySize.x, io.DisplaySize.y);
        BuildGizmos(s);
        geom_.clear();
        if (!hidden_) {
            DrawGizmos(io);
            DrawDock(io, s);
            WorldMouse(io, s);
        }
        PublishGeometry(io);
    }

    // Where the panel's things are, as an event (`ui.geometry`, in ImGui's own units: io.DisplaySize), when they move: a test that clicks and drags with the real mouse needs to know
    // where the buttons, the lanes and the keys are, and the panel is the one that knows. At most four a second; ui.open asks for one at once.
    void Geo(const std::string& name) {
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        geom_[name] = {std::round(a.x * 2.0f) / 2.0f, std::round(a.y * 2.0f) / 2.0f, std::round((b.x - a.x) * 2.0f) / 2.0f, std::round((b.y - a.y) * 2.0f) / 2.0f};
    }

    void PublishGeometry(ImGuiIO& io) {
        if (now_ - last_geom_ < 0.25 && !geom_dirty_) return;
        dir::Json e = dir::Json::Object();
        e.Set("hidden", hidden_);
        e.Set("collapsed", collapsed_);
        e.Set("tab", kTabNames[std::clamp(tab_, 0, static_cast<int>(kTabCount) - 1)]);
        e.Set("display", dir::Json::Vec3(std::round(io.DisplaySize.x), std::round(io.DisplaySize.y), 0.0));
        e.Set("picture", dir::Json::Vec3(view_.picture.x, view_.picture.y, view_.picture.w));
        e.Set("selected_line", selected_line_);
        dir::Json axis = dir::Json::Object();
        axis.Set("t0", std::round(axis_.t0 * 1000.0) / 1000.0);
        axis.Set("t1", std::round(axis_.t1 * 1000.0) / 1000.0);
        axis.Set("x0", std::round(axis_.x0 * 2.0f) / 2.0f);
        axis.Set("x1", std::round(axis_.x1 * 2.0f) / 2.0f);
        e.Set("axis", std::move(axis));
        dir::Json lanes = dir::Json::Array();
        for (size_t i = 0; i < layout_.lanes.size(); ++i) {
            dir::Json l = dir::Json::Object();
            l.Set("title", layout_.lanes[i].title);
            l.Set("y", std::round((canvas_y_ + metrics_.LaneTop(static_cast<int>(i))) * 2.0f) / 2.0f);
            l.Set("h", metrics_.lane_h);
            lanes.Push(std::move(l));
        }
        e.Set("lanes", std::move(lanes));
        dir::Json markers = dir::Json::Array();
        if (!hidden_ && !collapsed_) {
            for (const dp::Marker& m : layout_.markers) {
                const float x = axis_.TimeToX(m.t);
                if (x < axis_.x0 - 2.0f || x > axis_.x1 + 2.0f) continue;
                dir::Json j = dir::Json::Object();
                j.Set("line", m.line);
                j.Set("t", m.t);
                j.Set("x", std::round(x * 2.0f) / 2.0f);
                j.Set("y", std::round((canvas_y_ + metrics_.LaneCenter(m.lane)) * 2.0f) / 2.0f);
                j.Set("label", m.label);
                markers.Push(std::move(j));
            }
        }
        e.Set("markers", std::move(markers));
        dir::Json widgets = dir::Json::Object();
        for (const auto& kv : geom_) {
            dir::Json r = dir::Json::Array();
            for (float v : kv.second) r.Push(v);
            widgets.Set(kv.first, std::move(r));
        }
        e.Set("widgets", std::move(widgets));
        const std::string dump = e.Dump();
        if (dump == last_geom_dump_ && !geom_dirty_) return;
        last_geom_dump_ = dump;
        last_geom_ = now_;
        geom_dirty_ = false;
        dir::PublishEvent("ui.geometry", std::move(e));
    }

    bool CanEdit(const dir::Snapshot& s) const { return scene_text_.Has() && scene_text_.rev() == s.rev && edits_inflight_ == 0; }
    dp::EditContext Ctx(const dir::Snapshot& s) const { return dp::EditContext{s.rev, &scene_text_}; }

    bool Send(const dp::Cmd& c, dp::Dispatcher::Continuation then = {}) {
        last_cmd_ = dp::FormatCmd(c);
        last_cmd_time_ = now_;
        return disp_.Send(c, std::move(then));
    }

    void QueueEdit(std::string key, std::function<dp::Cmd(const dp::EditContext&, const dir::Snapshot&)> build) {
        for (QueuedEdit& q : queue_) {
            if (!key.empty() && q.key == key) {
                q.build = std::move(build);
                return;
            }
        }
        queue_.push_back({std::move(key), std::move(build)});
        if (queue_.size() > 32) queue_.erase(queue_.begin());
    }

    void FlushEdits(const dir::Snapshot& s) {
        if (queue_.empty() || !CanEdit(s)) return;
        QueuedEdit q = std::move(queue_.front());
        queue_.erase(queue_.begin());
        const dp::Cmd c = q.build(Ctx(s), s);
        if (c.cmd.empty()) return;
        ++edits_inflight_;
        const bool taken = Send(c, [this](const dir::Reply& r) {
            --edits_inflight_;
            if (!r.ok && (r.code == dir::ErrorCode::kStale || r.code == dir::ErrorCode::kBadArgs)) scene_text_ = dp::SceneText();  // read the file again
        });
        if (!taken) --edits_inflight_;  // the Director refused it (off, queue full): nothing will answer
    }

    // The file's text (for `expect`), the timeline's lanes, the selection an agent pointed at.
    void Sync(const dir::Snapshot& s) {
        if (layout_rev_ != s.rev || layout_events_ != s.events.size() || layout_end_ != s.transport.duration) {
            layout_ = dp::BuildLayout(s.events, s.transport.duration);
            layout_rev_ = s.rev;
            layout_events_ = s.events.size();
            layout_end_ = s.transport.duration;
            drag_ = dp::Drag();  // its marker index belonged to the old layout
            if (!axis_fit_) {
                axis_.Fit(layout_.end);
                axis_fit_ = true;
            }
        }
        if (scene_text_.rev() != s.rev && !text_inflight_ && now_ >= text_retry_at_) {
            text_inflight_ = true;
            dp::Cmd get;
            get.cmd = "scene.get";
            get.args.Set("text", true);
            if (!disp_.Send(get, [this](const dir::Reply& r) {
                    text_inflight_ = false;
                    if (r.ok) scene_text_.Set(static_cast<int64_t>(r.result.NumberOr("rev", -1)), r.result.StringOr("text", ""));
                    else text_retry_at_ = now_ + 1.0;  // no scene yet, or the Director is busy: not again every frame
                })) {
                text_inflight_ = false;
                text_retry_at_ = now_ + 1.0;
            }
        }
        if (s.selected_line != seen_snapshot_selected_) {
            seen_snapshot_selected_ = s.selected_line;
            if (s.selected_line > 0) selected_line_ = s.selected_line;
        }
        {
            std::lock_guard<std::mutex> lock(events_->mutex);
            if (events_->take_done_serial != seen_take_done_) {
                seen_take_done_ = events_->take_done_serial;
                take_folder_ = events_->take_folder;
                disp_.Note(now_, events_->take_state != "complete", "the take is " + events_->take_state + (take_folder_.empty() ? "" : ": " + take_folder_));
            }
        }
    }

    const dp::Marker* SelectedMarker() const {
        const int i = dp::FindMarkerByLine(layout_, selected_line_);
        return i >= 0 ? &layout_.markers[static_cast<size_t>(i)] : nullptr;
    }

    void Select(int line) {
        selected_line_ = line;
        dp::Cmd c;
        c.cmd = "ui.select";
        c.args.Set("line", line);
        Send(c);
    }

    // ------------------------------------------------------------------------------------------------------------ keys

    bool Pressed(ImGuiKey key) const { return ImGui::IsKeyPressed(key, false); }

    void Shortcuts(ImGuiIO& io, const dir::Snapshot& s) {
        // The keyboard stays the game's too, on purpose: the UI system runs the F-key menus (and Ctrl+F12) only for a key no overlay holds, so capturing every key would take F4, F9
        // and the rest away while the panel is open. The pointer on the dock holds them (a key typed there is the panel's: ImGui's own capture, and here); with the free camera flying
        // the game gets no pad input from W A S D anyway (freecam.h). CHECK: Space, G, R and X also reach the game while the camera is not flying (handbrake, horn, look back, nitro).
        if (io.WantCaptureMouse && !game_keys_) ImGui::SetNextFrameWantCaptureKeyboard(true);
        if (io.WantTextInput || game_keys_) return;
        const bool ctrl = io.KeyCtrl;
        if (Pressed(ImGuiKey_Tab)) hidden_ = !hidden_;
        if (Pressed(ImGuiKey_Escape) && drop_mode_) drop_mode_ = false;
        if (Pressed(ImGuiKey_Space)) Send(dp::TogglePlayCmd(s.transport));
        if (Pressed(ImGuiKey_Home)) Send(dp::RestartCmd(0.0));
        if (Pressed(ImGuiKey_Comma)) Send(dp::SeekCmd(s.transport.t - dp::FrameStep(s.transport)));
        if (Pressed(ImGuiKey_Period)) StepOn(s);
        if (Pressed(ImGuiKey_K)) Send(dp::AddKeyHere(0.0, false, "", false));
        if (Pressed(ImGuiKey_LeftBracket)) GoToKey(s, -1);
        if (Pressed(ImGuiKey_RightBracket)) GoToKey(s, +1);
        if (Pressed(ImGuiKey_F)) Send(dp::CameraModeCmd(s.camera.mode == "freecam" ? "scene" : "freecam"));
        if (Pressed(ImGuiKey_G)) gizmos_on_ = !gizmos_on_;
        if (Pressed(ImGuiKey_Delete)) DeleteSelected();
        if (ctrl && Pressed(ImGuiKey_Z)) Send(UndoCmd("scene.undo"));
        if (ctrl && Pressed(ImGuiKey_Y)) Send(UndoCmd("scene.redo"));
    }

    static dp::Cmd UndoCmd(const char* name) {
        dp::Cmd c;
        c.cmd = name;
        return c;
    }

    void StepOn(const dir::Snapshot& s) {
        if (s.transport.mode != "fixed") Send(dp::FixedModeCmd(true));  // a step needs the fixed step
        Send(dp::StepCmd(1));
    }

    void GoToKey(const dir::Snapshot& s, int direction) {
        for (const dp::Cmd& c : dp::GoToKeyCmds(s, direction)) {
            if (c.cmd == "ui.select") selected_line_ = static_cast<int>(c.args.NumberOr("line", 0));
            Send(c);
        }
    }

    void DeleteSelected() {
        const dp::Marker* m = SelectedMarker();
        if (!m) return;
        const int line = m->line;
        QueueEdit("del" + std::to_string(line), [this, line](const dp::EditContext& ctx, const dir::Snapshot&) {
            const int i = dp::FindMarkerByLine(layout_, line);
            return i < 0 ? dp::Cmd{} : dp::DeleteKey(ctx, layout_.markers[static_cast<size_t>(i)]);
        });
        selected_line_ = 0;
    }

    // The keyboard and the mouse fly the free camera through a pad (freecam.h): W A S D, Q E, Shift, Ctrl, the right button's look and the wheel.
    void FeedFly(ImGuiIO& io, const dir::Snapshot& s) {
        const bool flying = s.camera.mode == "freecam";
        if (!flying || game_keys_ || io.WantTextInput) {
            fly_prev_ = false;
            look_held_ = false;
            return;
        }
        const bool over_ui = io.WantCaptureMouse;
        if (io.MouseClicked[1] && !over_ui) look_held_ = true;
        if (!io.MouseDown[1]) look_held_ = false;
        dp::FlyKeys k;
        k.forward = ImGui::IsKeyDown(ImGuiKey_W);
        k.back = ImGui::IsKeyDown(ImGuiKey_S);
        k.left = ImGui::IsKeyDown(ImGuiKey_A);
        k.right = ImGui::IsKeyDown(ImGuiKey_D);
        k.up = ImGui::IsKeyDown(ImGuiKey_E);
        k.down = ImGui::IsKeyDown(ImGuiKey_Q);
        k.fast = io.KeyShift;
        k.slow = io.KeyCtrl;
        k.roll_left = ImGui::IsKeyDown(ImGuiKey_Z);
        k.roll_right = ImGui::IsKeyDown(ImGuiKey_X);
        k.level = ImGui::IsKeyDown(ImGuiKey_R);
        k.zoom_in = ImGui::IsKeyDown(ImGuiKey_Equal);
        k.zoom_out = ImGui::IsKeyDown(ImGuiKey_Minus);
        k.look = look_held_;
        k.mouse_dx = io.MouseDelta.x;
        k.mouse_dy = io.MouseDelta.y;
        k.wheel = over_ui ? 0.0f : io.MouseWheel;
        const bool any = dp::AnyFlyInput(k);
        if (any || fly_prev_) mc::cinematic::FreecamFeedPad(dp::MakeFlyPad(k, tune_, s.camera.pose.fov));
        fly_prev_ = any;
    }

    // ------------------------------------------------------------------------------------------------------------ the world

    void BuildGizmos(const dir::Snapshot& s) {
        gizmos_.Clear();
        if (!gizmos_on_ || !view_.valid) return;
        for (const dir::SnapRoute& r : s.routes) {
            dp::AddRoute(view_, r.points, -1, r.name + (r.car.empty() ? "" : "  (" + r.car + ")"), false, &gizmos_);
        }
        if (route_.loaded && !route_.pts.empty()) dp::AddRoute(view_, dp::GizmoPoints(route_), kWaypointBase, route_.name, true, &gizmos_);
        // the camera's path: the snapshot's samples, else the spline through the explicit `pos` keys
        std::vector<dir::SnapCamSample> keys_path;
        const std::vector<dp::KeyRef> keys = dp::CameraKeys(s.events);
        const std::vector<dir::SnapCamSample>* path = &s.cam_path;
        if (s.cam_path.empty()) {
            for (const dp::KeyRef& k : keys) {
                dir::SnapPose p;
                if (!dp::ParseKeyPose(k.text, &p)) continue;
                dir::SnapCamSample c;
                c.t = k.t;
                c.pos[0] = p.pos[0], c.pos[1] = p.pos[1], c.pos[2] = p.pos[2];
                keys_path.push_back(c);
            }
            keys_path = dp::SplineThrough(keys_path, 10);
            path = &keys_path;
        }
        dp::AddCameraPath(view_, *path, IM_COL32(51, 214, 214, 200), &gizmos_);
        int shown = 0;
        for (const dp::KeyRef& k : keys) {
            dir::SnapPose p;
            if (!dp::ParseKeyPose(k.text, &p) || ++shown > 48) continue;
            const bool sel = k.line == selected_line_;
            dp::AddFrustum(view_, p, view_.aspect, sel ? 3.0 : 2.0, sel ? kYellow : IM_COL32(51, 214, 214, 220), sel, &gizmos_);
            dp::Vec2f px;
            if (view_.Project(p.pos, &px)) gizmos_.labels.push_back({px, dp::Num(k.t) + " s", sel ? kYellow : IM_COL32(180, 230, 230, 255)});
        }
        for (size_t i = 0; i < s.cars.size(); ++i) dp::AddCar(view_, s.cars[i], s.cars[i].id == selected_car_, static_cast<int>(i), &gizmos_);
    }

    void DrawGizmos(ImGuiIO&) {
        if (!gizmos_on_ || !view_.valid) return;
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        for (const dp::Line2& l : gizmos_.lines) dl->AddLine(ImVec2(l.a.x, l.a.y), ImVec2(l.b.x, l.b.y), l.color, l.width);
        for (const dp::Dot2& d : gizmos_.dots) {
            if (d.filled) dl->AddCircleFilled(ImVec2(d.p.x, d.p.y), d.radius, d.color);
            else dl->AddCircle(ImVec2(d.p.x, d.p.y), d.radius, d.color, 0, 2.0f);
        }
        ImGui::PushFont(nullptr, 15.0f);
        for (const dp::Label2& t : gizmos_.labels) {
            const ImVec2 size = ImGui::CalcTextSize(t.text.c_str());
            const ImVec2 at(t.p.x + 8.0f, t.p.y - size.y * 0.5f);
            dl->AddRectFilled(ImVec2(at.x - 2, at.y - 1), ImVec2(at.x + size.x + 2, at.y + size.y + 1), IM_COL32(0, 0, 0, 150), 2.0f);
            dl->AddText(at, t.color, t.text.c_str());
        }
        ImGui::PopFont();
    }

    // The mouse in the picture, where no panel window is: pick a car, drag a car or a waypoint over the ground, drop a waypoint.
    void WorldMouse(ImGuiIO& io, const dir::Snapshot& s) {
        if (!view_.valid) return;
        const dp::Vec2f mp{io.MousePos.x, io.MousePos.y};
        const bool over_ui = io.WantCaptureMouse;
        const bool in_picture = !over_ui && view_.picture.Contains(mp);
        if (io.MouseClicked[0] && in_picture) {
            const int dot = dp::NearestDot(gizmos_.dots, mp, 12.0f);
            if (dot >= 0) {
                const int id = gizmos_.dots[static_cast<size_t>(dot)].id;
                if (id >= kWaypointBase) {
                    wp_drag_ = id - kWaypointBase;
                } else if (id >= 0 && static_cast<size_t>(id) < s.cars.size()) {
                    selected_car_ = s.cars[static_cast<size_t>(id)].id;
                    if (selected_car_ != "player" && s.cars[static_cast<size_t>(id)].ready) {
                        car_drag_ = selected_car_;
                        car_drag_y_ = s.cars[static_cast<size_t>(id)].pos[1];
                        car_drag_moved_ = false;
                        want_tab_ = kTabCars;
                    }
                }
            } else if (drop_mode_) {
                DropAtCursor(mp);
            }
        }
        if (io.MouseDown[0]) {
            const bool moved = std::fabs(io.MouseDelta.x) + std::fabs(io.MouseDelta.y) > 0.0f;
            double hit[3];
            if (wp_drag_ >= 0 && static_cast<size_t>(wp_drag_) < route_.pts.size() && moved && dp::RayPlaneY(view_.PixelRay(mp), route_.pts[static_cast<size_t>(wp_drag_)].y, hit)) {
                dp::RouteMove(&route_, static_cast<size_t>(wp_drag_), hit[0], hit[2]);
            }
            if (!car_drag_.empty() && moved && dp::RayPlaneY(view_.PixelRay(mp), car_drag_y_, hit)) {
                car_drag_moved_ = true;
                std::memcpy(car_drop_, hit, sizeof(hit));
                if (now_ - last_place_ > 0.06) {  // the car follows the pointer a few times a second: car.place is live and transient
                    last_place_ = now_;
                    Send(dp::PlaceCar(car_drag_, hit, 0.0, false));
                }
            }
        }
        if (io.MouseReleased[0]) {
            if (wp_drag_ >= 0) {
                wp_drag_ = -1;
                Send(dp::SetRoute(route_));
            }
            if (!car_drag_.empty()) {
                const std::string car = car_drag_;
                car_drag_.clear();
                if (car_drag_moved_) CommitCarDrop(car, mp);
            }
        }
    }

    // Where a car was dropped: the ground under the pointer (the physics probe knows the height; the drag's plane did not), then one write of its spawn line.
    void CommitCarDrop(const std::string& car, const dp::Vec2f& mp) {
        double u = 0.0, v = 0.0;
        view_.ToUV(mp, &u, &v);
        std::array<double, 3> fallback = {car_drop_[0], car_drop_[1], car_drop_[2]};
        Send(dp::PickAt(std::clamp(u, 0.0, 1.0), std::clamp(v, 0.0, 1.0)), [this, car, fallback](const dir::Reply& r) {
            double pos[3] = {fallback[0], fallback[1], fallback[2]};
            std::string why;
            double hit[3];
            if (dp::ParsePick(r, hit, &why)) std::memcpy(pos, hit, sizeof(pos));
            const std::array<double, 3> at = {pos[0], pos[1], pos[2]};
            Send(dp::PlaceCar(car, pos, 0.0, false));
            QueueEdit("drop" + car, [this, car, at](const dp::EditContext& ctx, const dir::Snapshot& s) {
                const double p[3] = {at[0], at[1], at[2]};
                return dp::MoveCarSpawn(ctx, s.events, car, p, 0.0, false);
            });
        });
    }

    void DropAtCursor(const dp::Vec2f& mp) {
        double u = 0.0, v = 0.0;
        if (!view_.ToUV(mp, &u, &v)) return;
        const std::string name = dp::SafeRouteName(route_name_);
        if (name.empty()) {
            disp_.Note(now_, true, "name the route first");
            return;
        }
        const float speed = wp_speed_;
        Send(dp::PickAt(u, v), [this, name, speed](const dir::Reply& r) {
            double pos[3];
            std::string why;
            if (!dp::ParsePick(r, pos, &why)) {
                disp_.Note(now_, true, why);
                return;
            }
            Send(dp::DropWaypoint(name, pos, speed));
            LoadRoute(name);
        });
    }

    void LoadRoute(const std::string& name) {
        Send(dp::GetRoute(name), [this, name](const dir::Reply& r) {
            if (!r.ok) return;
            route_ = dp::RouteFromReply(r.result.StringOr("route", name), r.result);
            if (route_.name.empty()) route_.name = name;
        });
    }

    void RefreshRouteList() {
        dp::Cmd c;
        c.cmd = "path.list";
        routes_asked_ = true;
        disp_.Send(c, [this](const dir::Reply& r) {
            route_names_.clear();
            for (const dir::Json& j : r.result.Get("routes").items()) route_names_.push_back(j.StringOr("name", ""));
        });
    }

    void RequestModels() {
        dp::Cmd c;
        c.cmd = "cars.models";
        models_asked_ = true;
        disp_.Send(c, [this](const dir::Reply& r) {
            models_.clear();
            for (const dir::Json& j : r.result.Get("models").items()) {
                const std::string n = j.StringOr("name", "");
                if (!n.empty()) models_.push_back(n);
            }
        });
    }

    // ------------------------------------------------------------------------------------------------------------ the dock

    void DrawDock(ImGuiIO& io, const dir::Snapshot& s) {
        const float full = std::clamp(io.DisplaySize.y * 0.36f, 250.0f, 400.0f);
        const float bar = io.DisplaySize.x < 1500.0f ? 66.0f : 36.0f;
        const float h = collapsed_ ? bar + 28.0f : full;
        ImGui::SetNextWindowPos(ImVec2(0.0f, io.DisplaySize.y - h));
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, h));
        ImGui::SetNextWindowBgAlpha(0.74f);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
        if (ImGui::Begin("##mcla_dir_dock", nullptr, flags)) {
            ImGui::PushFont(nullptr, kFontSize);
            DrawTransportBar(s, bar);
            if (!collapsed_) {
                const float footer = ImGui::GetFontSize() + 8.0f;
                const float body = std::max(60.0f, ImGui::GetContentRegionAvail().y - footer);
                const float width = ImGui::GetContentRegionAvail().x;
                const float left = std::floor(width * 0.60f);
                ImGui::BeginChild("##dir_timeline", ImVec2(left, body), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                DrawTimeline(io, s);
                ImGui::EndChild();
                ImGui::SameLine();
                ImGui::BeginChild("##dir_inspector", ImVec2(0.0f, body), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
                DrawInspector(io, s);
                ImGui::EndChild();
                DrawFooter(s);
            }
            ImGui::PopFont();
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    // A row of buttons that wraps at the window's edge.
    bool Item(float width) {
        if (ImGui::GetContentRegionAvail().x < width && ImGui::GetCursorPosX() > 20.0f) return false;
        return true;
    }
    void Gap() { ImGui::SameLine(0.0f, 8.0f); }

    bool Btn(const char* label, bool enabled = true) {
        if (!enabled) ImGui::BeginDisabled();
        const bool hit = ImGui::Button(label);
        Geo(label);
        if (!enabled) ImGui::EndDisabled();
        return hit && enabled;
    }

    void DrawTransportBar(const dir::Snapshot& s, float bar_height) {
        ImGui::BeginChild("##dir_bar", ImVec2(0.0f, bar_height), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ToVec4(kYellow), "DIRECTOR");
        Gap();
        if (Btn("RESTART")) Send(dp::RestartCmd(0.0));
        Gap();
        if (Btn(s.transport.paused ? "PLAY" : "PAUSE")) Send(dp::TogglePlayCmd(s.transport));
        Gap();
        if (Btn("< FRAME")) Send(dp::SeekCmd(s.transport.t - dp::FrameStep(s.transport)));
        Gap();
        if (Btn("FRAME >")) StepOn(s);
        Gap();
        ImGui::TextColored(ToVec4(kWhite), "%s / %s", dp::FormatClock(s.transport.t).c_str(), s.transport.duration > 0.0 ? dp::FormatClock(s.transport.duration).c_str() : "--");
        Gap();
        ImGui::TextColored(ToVec4(s.transport.paused ? kYellow : kGreen), "%s", dp::TransportStateText(s.transport).c_str());
        if (!s.transport.scrubbable) {
            Gap();
            ImGui::TextColored(ToVec4(kYellow), "SCRUB NOT EXACT: %s", s.transport.scrub_hint.c_str());
        }
        Gap();
        bool fixed = s.transport.mode == "fixed";
        if (ImGui::Checkbox("FIXED STEP", &fixed)) Send(dp::FixedModeCmd(fixed));
        Geo("FIXED STEP");
        // the second group
        if (bar_height > 40.0f) ImGui::NewLine();
        else Gap();
        const bool flying = s.camera.mode == "freecam";
        if (Btn(flying ? "FREE CAMERA: ON" : "FREE CAMERA: OFF")) Send(dp::CameraModeCmd(flying ? "scene" : "freecam"));
        Gap();
        if (Btn("ADD KEY")) Send(dp::AddKeyHere(0.0, false, "", false));
        Gap();
        if (Btn("CUT")) QueueEdit("cut", [](const dp::EditContext& ctx, const dir::Snapshot& snap) { return dp::CutAt(ctx.rev, snap.transport.t); });
        Gap();
        if (Btn("UNDO", s.can_undo)) Send(UndoCmd("scene.undo"));
        Gap();
        if (Btn("REDO", s.can_redo)) Send(UndoCmd("scene.redo"));
        Gap();
        ImGui::Checkbox("GIZMOS", &gizmos_on_);
        Geo("GIZMOS");
        Gap();
        if (ImGui::Checkbox("GAME KEYS", &game_keys_)) {}
        Geo("GAME KEYS");
        Gap();
        if (s.take_active) {
            ImGui::TextColored(ToVec4(kRed), "RENDERING %s: %llu FRAMES", s.take_name.c_str(), static_cast<unsigned long long>(s.take_frames));
        } else if (Btn("RENDER TAKE")) {
            take_.name = take_name_;
            Send(dp::TakeRenderCmd(take_));
        }
        Gap();
        if (Btn(collapsed_ ? "SHOW" : "COLLAPSE")) collapsed_ = !collapsed_;
        Gap();
        if (Btn("CLOSE")) larecomp::cvar::SetFlagByName("mcla_director_panel", "false");
        ImGui::EndChild();
    }

    void DrawFooter(const dir::Snapshot&) {
        ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(kDim));
        std::string line = last_cmd_.empty() ? "WHAT I DID: (nothing yet: every action prints its command here, the same one an agent sends and the scene file shows)" : "WHAT I DID: " + last_cmd_;
        if (line.size() > 150) line = line.substr(0, 147) + "...";
        ImGui::TextUnformatted(line.c_str());
        ImGui::PopStyleColor();
        const std::vector<dp::Notice>& notices = disp_.notices();
        if (!notices.empty() && now_ - notices.back().time < 10.0) {
            ImGui::SameLine();
            ImGui::TextColored(ToVec4(notices.back().error ? kRed : kCyan), "   %s", notices.back().text.c_str());
        }
    }

    // ------------------------------------------------------------------------------------------------------------ the timeline

    void DrawTimeline(ImGuiIO& io, const dir::Snapshot& s) {
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        if (avail.x < 80.0f || avail.y < 40.0f) return;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float gutter = 86.0f;
        const int lanes = static_cast<int>(layout_.lanes.size());
        dp::Metrics m;
        m.lane_h = std::clamp((avail.y - m.ruler_h) / static_cast<float>(std::max(1, lanes)) - m.lane_gap, 15.0f, 28.0f);
        metrics_ = m;
        canvas_y_ = origin.y;
        axis_.x0 = origin.x + gutter;
        axis_.x1 = origin.x + avail.x - 10.0f;
        const double end = layout_.end;
        const double frame = dp::FrameStep(s.transport);

        ImGui::InvisibleButton("##canvas", avail, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
        Geo("TIMELINE");
        const bool hovered = ImGui::IsItemHovered();
        const dp::Vec2f mouse{io.MousePos.x, io.MousePos.y - origin.y};  // y in the canvas; x as the axis has it

        if (hovered && io.MouseWheel != 0.0f) axis_.ZoomAt(io.MousePos.x, std::pow(1.2, static_cast<double>(io.MouseWheel)));
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) axis_.Pan(io.MouseDelta.x);
        axis_.KeepInside(0.0, end);

        int hot = hovered ? dp::HitMarker(layout_, axis_, m, mouse, 8.0f) : -1;
        if (hovered && io.MouseClicked[0]) {
            if (hot >= 0) {
                const dp::Marker& mk = layout_.markers[static_cast<size_t>(hot)];
                Select(mk.line);
                drag_ = dp::BeginDrag(layout_, hot, axis_, io.MousePos.x, io.KeyAlt);
            } else if (const int bar = dp::HitSpan(layout_, axis_, m, mouse); bar >= 0) {
                Select(layout_.markers[static_cast<size_t>(bar)].line);
            } else if (mouse.y >= 0.0f) {
                scrubbing_ = true;
            }
        }
        if (hovered && io.MouseClicked[1] && hot >= 0) {
            Select(layout_.markers[static_cast<size_t>(hot)].line);
            ImGui::OpenPopup("##dir_keymenu");
        }
        if (io.MouseDown[0]) {
            if (drag_.active) {
                std::vector<double> marks = dp::MarkerTimes(layout_, drag_.marker);
                dp::UpdateDrag(&drag_, axis_, io.MousePos.x, marks, s.transport.t, end, frame, !io.KeyShift);
            }
            if (scrubbing_ && now_ - last_scrub_ > 0.05) {
                last_scrub_ = now_;
                Send(dp::SeekCmd(dp::ScrubTime(axis_, io.MousePos.x, end, frame)));
            }
        }
        if (io.MouseReleased[0]) {
            if (drag_.active) FinishDrag();
            if (scrubbing_) Send(dp::SeekCmd(dp::ScrubTime(axis_, io.MousePos.x, end, frame)));
            scrubbing_ = false;
            drag_ = dp::Drag();
        }

        // ---- drawing
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 top_left = origin;
        const ImVec2 bottom_right(origin.x + avail.x, origin.y + avail.y);
        dl->PushClipRect(top_left, bottom_right, true);
        dl->AddRectFilled(top_left, bottom_right, IM_COL32(0, 0, 0, 90));
        for (int i = 0; i < lanes; ++i) {
            const float y0 = origin.y + m.LaneTop(i);
            dl->AddRectFilled(ImVec2(origin.x, y0), ImVec2(bottom_right.x, y0 + m.lane_h), i % 2 ? IM_COL32(255, 255, 255, 10) : IM_COL32(255, 255, 255, 22));
            dl->AddText(ImVec2(origin.x + 6.0f, y0 + (m.lane_h - ImGui::GetFontSize()) * 0.5f), kDim, layout_.lanes[static_cast<size_t>(i)].title.c_str());
        }
        // the take's range
        {
            const double from = std::max(0.0, take_.from), to = take_.to >= 0.0 ? take_.to : end;
            dl->AddRectFilled(ImVec2(axis_.TimeToX(from), origin.y + 2.0f), ImVec2(axis_.TimeToX(to), origin.y + m.ruler_h - 2.0f), IM_COL32(238, 85, 68, 60));
        }
        // the ruler
        for (const dp::Tick& t : dp::RulerTicks(axis_)) {
            const float x = axis_.TimeToX(t.t);
            if (x < axis_.x0 - 1.0f || x > axis_.x1 + 1.0f) continue;
            dl->AddLine(ImVec2(x, origin.y + (t.major ? 6.0f : 14.0f)), ImVec2(x, origin.y + m.ruler_h), t.major ? kDim : IM_COL32(110, 110, 104, 255));
            if (t.major) {
                dl->AddText(ImVec2(x + 3.0f, origin.y + 1.0f), kDim, t.label.c_str());
                dl->AddLine(ImVec2(x, origin.y + m.ruler_h), ImVec2(x, bottom_right.y), IM_COL32(255, 255, 255, 14));
            }
        }
        dl->AddLine(ImVec2(axis_.TimeToX(0.0), origin.y), ImVec2(axis_.TimeToX(0.0), bottom_right.y), IM_COL32(255, 255, 255, 60));
        if (end > 0.0) dl->AddLine(ImVec2(axis_.TimeToX(end), origin.y), ImVec2(axis_.TimeToX(end), bottom_right.y), IM_COL32(238, 85, 68, 120), 2.0f);
        // spans, then markers
        for (size_t i = 0; i < layout_.markers.size(); ++i) {
            const dp::Marker& mk = layout_.markers[i];
            if (!mk.has_span) continue;
            const float cy = origin.y + m.LaneCenter(mk.lane);
            const float xa = axis_.TimeToX(mk.t), xb = axis_.TimeToX(mk.span_end);
            ImU32 col = MarkerColor(mk);
            col = (col & 0x00FFFFFFu) | (static_cast<ImU32>(mk.line == selected_line_ ? 110 : 55) << 24);
            dl->AddRectFilled(ImVec2(xa, cy - 5.0f), ImVec2(xb, cy + 5.0f), col, 2.0f);
        }
        for (size_t i = 0; i < layout_.markers.size(); ++i) {
            const dp::Marker& mk = layout_.markers[i];
            const float x = axis_.TimeToX(mk.t);
            if (x < origin.x || x > bottom_right.x) continue;
            const float cy = origin.y + m.LaneCenter(mk.lane);
            const bool selected = mk.line == selected_line_;
            const bool is_hot = static_cast<int>(i) == hot;
            const ImU32 col = selected ? kYellow : MarkerColor(mk);
            if (mk.is_cut) {
                dl->AddLine(ImVec2(x, origin.y + m.LaneTop(mk.lane)), ImVec2(x, origin.y + m.LaneTop(mk.lane) + m.lane_h), col, selected ? 3.0f : 2.0f);
                continue;
            }
            const float r = m.marker_r * (is_hot || selected ? 1.15f : 1.0f);
            const ImVec2 pts[4] = {ImVec2(x, cy - r), ImVec2(x + r, cy), ImVec2(x, cy + r), ImVec2(x - r, cy)};
            dl->AddConvexPolyFilled(pts, 4, col);
            if (is_hot || selected) dl->AddPolyline(pts, 4, kWhite, ImDrawFlags_Closed, 1.5f);
            // the label, when there is room before the next marker of the lane
            float room = 220.0f;
            for (size_t k = i + 1; k < layout_.markers.size() && layout_.markers[k].lane == mk.lane; ++k) {
                if (layout_.markers[k].t > mk.t + 1e-6) {
                    room = axis_.TimeToX(layout_.markers[k].t) - x - r - 6.0f;
                    break;
                }
            }
            if (room > 36.0f) {
                dl->PushClipRect(ImVec2(x + r + 3.0f, cy - m.lane_h * 0.5f), ImVec2(x + r + 3.0f + room, cy + m.lane_h * 0.5f), true);
                dl->AddText(ImVec2(x + r + 4.0f, cy - ImGui::GetFontSize() * 0.5f), IM_COL32(230, 230, 222, 230), mk.label.c_str());
                dl->PopClipRect();
            }
        }
        // the drag's ghost
        if (drag_.active && drag_.moved && drag_.marker >= 0 && static_cast<size_t>(drag_.marker) < layout_.markers.size()) {
            const float x = axis_.TimeToX(drag_.t_now);
            const dp::Marker& mk = layout_.markers[static_cast<size_t>(drag_.marker)];
            const float cy = origin.y + m.LaneCenter(mk.lane);
            dl->AddLine(ImVec2(x, origin.y), ImVec2(x, bottom_right.y), IM_COL32(255, 255, 255, 110));
            const ImVec2 pts[4] = {ImVec2(x, cy - 8.0f), ImVec2(x + 8.0f, cy), ImVec2(x, cy + 8.0f), ImVec2(x - 8.0f, cy)};
            dl->AddConvexPolyFilled(pts, 4, drag_.duplicate ? kGreen : kWhite);
            char tag[48];
            std::snprintf(tag, sizeof(tag), "%s%s", drag_.duplicate ? "COPY AT " : "", dp::FormatClock(drag_.t_now, 3).c_str());
            dl->AddText(ImVec2(x + 10.0f, origin.y + 2.0f), kWhite, tag);
        }
        // the playhead
        {
            const float x = axis_.TimeToX(s.transport.t);
            dl->AddLine(ImVec2(x, origin.y), ImVec2(x, bottom_right.y), kCyan, 2.0f);
            const ImVec2 tri[3] = {ImVec2(x - 6.0f, origin.y), ImVec2(x + 6.0f, origin.y), ImVec2(x, origin.y + 10.0f)};
            dl->AddConvexPolyFilled(tri, 3, kCyan);
        }
        dl->PopClipRect();

        if (hot >= 0 && !drag_.active) {
            const dp::Marker& mk = layout_.markers[static_cast<size_t>(hot)];
            ImGui::SetTooltip("%s\n%s s%s", mk.text.c_str(), dp::FormatClock(mk.t, 3).c_str(), mk.ease.empty() ? "" : ("  ease " + mk.ease).c_str());
        }
        DrawKeyMenu(s);
    }

    void FinishDrag() {
        const dp::DragOutcome out = dp::FinishDrag(drag_);
        if (out == dp::DragOutcome::kNothing) return;
        const int line = drag_.line;
        const double t = drag_.t_now;
        if (out == dp::DragOutcome::kMove) {
            QueueEdit("move" + std::to_string(line), [this, line, t](const dp::EditContext& ctx, const dir::Snapshot&) {
                const int i = dp::FindMarkerByLine(layout_, line);
                return i < 0 ? dp::Cmd{} : dp::MoveKey(ctx, layout_.markers[static_cast<size_t>(i)], t);
            });
        } else {
            QueueEdit("dup" + std::to_string(line) + "@" + dp::Num(t), [this, line, t](const dp::EditContext& ctx, const dir::Snapshot&) {
                const int i = dp::FindMarkerByLine(layout_, line);
                return i < 0 ? dp::Cmd{} : dp::DuplicateKey(ctx, layout_.markers[static_cast<size_t>(i)], t);
            });
        }
    }

    // Right click on a key: how its segment runs, a copy here, go there, delete.
    void DrawKeyMenu(const dir::Snapshot& s) {
        if (!ImGui::BeginPopup("##dir_keymenu")) return;
        const dp::Marker* m = SelectedMarker();
        if (!m) {
            ImGui::TextUnformatted("(no key)");
            ImGui::EndPopup();
            return;
        }
        ImGui::TextColored(ToVec4(kYellow), "%s  %s s", m->label.c_str(), dp::FormatClock(m->t, 3).c_str());
        ImGui::Separator();
        if (m->kind == dir::EventKind::kCamera) {
            ImGui::TextColored(ToVec4(kDim), "EASE (how the segment that starts here runs)");
            for (const char* ease : dp::kEases) {
                const bool current = (m->ease.empty() ? std::string("linear") : m->ease) == ease;
                if (ImGui::MenuItem(ease, nullptr, current)) {
                    const int line = m->line;
                    const std::string e = ease;
                    QueueEdit("ease" + std::to_string(line), [this, line, e](const dp::EditContext& ctx, const dir::Snapshot&) {
                        const int i = dp::FindMarkerByLine(layout_, line);
                        return i < 0 ? dp::Cmd{} : dp::SetEase(ctx, layout_.markers[static_cast<size_t>(i)], e);
                    });
                }
            }
            ImGui::Separator();
        }
        if (ImGui::MenuItem("GO TO THIS KEY")) Send(dp::SeekCmd(m->t));
        if (ImGui::MenuItem("COPY HERE (AT THE PLAYHEAD)")) {
            const int line = m->line;
            const double t = s.transport.t;
            QueueEdit("dup" + std::to_string(line) + "@" + dp::Num(t), [this, line, t](const dp::EditContext& ctx, const dir::Snapshot&) {
                const int i = dp::FindMarkerByLine(layout_, line);
                return i < 0 ? dp::Cmd{} : dp::DuplicateKey(ctx, layout_.markers[static_cast<size_t>(i)], t);
            });
        }
        if (ImGui::MenuItem("DELETE KEY", "Del")) DeleteSelected();
        ImGui::EndPopup();
    }

    // ------------------------------------------------------------------------------------------------------------ the inspector

    void DrawInspector(ImGuiIO& io, const dir::Snapshot& s) {
        if (!ImGui::BeginTabBar("##dir_tabs")) return;
        for (int i = 0; i < kTabCount; ++i) {
            ImGuiTabItemFlags f = 0;
            if (want_tab_ == i) {
                f |= ImGuiTabItemFlags_SetSelected;
                want_tab_ = -1;
            }
            const bool shown = ImGui::BeginTabItem(kTabNames[i], nullptr, f);
            Geo(std::string("TAB ") + kTabNames[i]);
            if (!shown) continue;
            tab_ = i;
            ImGui::BeginChild("##dir_tab_body", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_None);
            switch (i) {
                case kTabCamera: TabCamera(io, s); break;
                case kTabRigs: TabRigs(s); break;
                case kTabCars: TabCars(s); break;
                case kTabPaths: TabPaths(s); break;
                case kTabTake: TabTake(s); break;
                case kTabAgent: TabAgent(s); break;
                default: break;
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    // A slider that sends its value as an edit while it is dragged (at most four times a second) and once more at the end.
    bool KnobSlider(const dir::Snapshot&, const dp::Marker& m, const dp::RigKnob& knob, double value) {
        float v = static_cast<float>(value);
        char fmt[16];
        std::snprintf(fmt, sizeof(fmt), "%%.%df", knob.decimals);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.62f);
        ImGui::PushID(knob.key);
        const bool changed = ImGui::SliderFloat(knob.label, &v, static_cast<float>(knob.lo), static_cast<float>(knob.hi), fmt);
        const bool done = ImGui::IsItemDeactivatedAfterEdit();
        ImGui::PopID();
        if (changed && now_ - last_knob_ > 0.25) {
            last_knob_ = now_;
            QueueKnob(m.line, knob, v);
        }
        if (done) QueueKnob(m.line, knob, v);
        return changed;
    }

    void QueueKnob(int line, const dp::RigKnob& knob, double v) {
        const dp::RigKnob k = knob;
        QueueEdit("knob" + std::to_string(line) + k.key, [this, line, k, v](const dp::EditContext& ctx, const dir::Snapshot&) {
            const int i = dp::FindMarkerByLine(layout_, line);
            return i < 0 ? dp::Cmd{} : dp::SetKnob(ctx, layout_.markers[static_cast<size_t>(i)], k, v);
        });
    }

    void TabCamera(ImGuiIO&, const dir::Snapshot& s) {
        ImGui::TextColored(ToVec4(kCyan), "%s", dp::CameraModeText(s.camera.mode).c_str());
        ImGui::SameLine();
        ImGui::TextColored(ToVec4(kDim), "   %.1f %.1f %.1f   YAW %.1f  PITCH %.1f  FOV %.0f", s.camera.pose.pos[0], s.camera.pose.pos[1], s.camera.pose.pos[2], s.camera.pose.yaw, s.camera.pose.pitch, s.camera.pose.fov);
        if (Btn("FREE")) Send(dp::CameraModeCmd("freecam"));
        Gap();
        if (Btn("SCENE")) Send(dp::CameraModeCmd("scene"));
        Gap();
        if (Btn("GAME")) Send(dp::CameraModeCmd("game"));
        Gap();
        if (Btn("LOOK AT PLAYER")) Send(dp::LookAtCmd("player"));
        if (Btn("ADD KEY HERE")) Send(dp::AddKeyHere(0.0, false, "", false));
        Gap();
        if (Btn("ADD KEY + CUT")) Send(dp::AddKeyHere(0.0, false, "", true));
        const dp::Marker* m = SelectedMarker();
        const bool cam_key = m && m->kind == dir::EventKind::kCamera;
        ImGui::Separator();
        if (!cam_key) {
            ImGui::TextColored(ToVec4(kDim), "SELECT A CAMERA KEY ON THE TIMELINE (or press ] to go to the next one)");
        } else {
            ImGui::TextColored(ToVec4(kYellow), "KEY AT %s s: %s", dp::FormatClock(m->t, 3).c_str(), m->label.c_str());
            const dir::SnapPose pose = s.camera.pose;
            const int line = m->line;
            if (Btn("UPDATE KEY FROM THE VIEW")) {
                QueueEdit("upd" + std::to_string(line), [this, line, pose](const dp::EditContext& ctx, const dir::Snapshot&) {
                    const int i = dp::FindMarkerByLine(layout_, line);
                    return i < 0 ? dp::Cmd{} : dp::UpdateKeyFromPose(ctx, layout_.markers[static_cast<size_t>(i)], pose);
                });
            }
            Gap();
            if (Btn("GO TO KEY")) Send(dp::SeekCmd(m->t));
            Gap();
            if (Btn("DELETE")) DeleteSelected();
            ImGui::TextColored(ToVec4(kDim), "EASE");
            ImGui::SameLine();
            for (const char* ease : dp::kEases) {
                const bool current = (m->ease.empty() ? std::string("linear") : m->ease) == ease;
                if (current) ImGui::PushStyleColor(ImGuiCol_Button, ToVec4(IM_COL32(51, 214, 214, 120)));
                if (ImGui::SmallButton(ease)) {
                    const std::string e = ease;
                    QueueEdit("ease" + std::to_string(line), [this, line, e](const dp::EditContext& ctx, const dir::Snapshot&) {
                        const int i = dp::FindMarkerByLine(layout_, line);
                        return i < 0 ? dp::Cmd{} : dp::SetEase(ctx, layout_.markers[static_cast<size_t>(i)], e);
                    });
                }
                if (current) ImGui::PopStyleColor();
                ImGui::SameLine();
            }
            ImGui::NewLine();
            for (const dp::RigKnob& knob : dp::KeyKnobs()) KnobSlider(s, *m, knob, dp::KeyKnobValue(m->text, knob));
        }
        ImGui::Separator();
        float sens = static_cast<float>(tune_.degrees_per_pixel);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
        if (ImGui::SliderFloat("MOUSE LOOK", &sens, 0.03f, 0.4f, "%.2f deg/px")) tune_.degrees_per_pixel = sens;
        ImGui::TextColored(ToVec4(kDim), "WASD MOVE  Q E DOWN UP  SHIFT FAST  CTRL SLOW  RIGHT MOUSE LOOK  WHEEL SPEED  Z X TILT  R LEVEL  + - ZOOM");
    }

    void TabRigs(const dir::Snapshot& s) {
        const dp::Marker* m = SelectedMarker();
        dp::RigInfo rig;
        const bool is_rig = m && m->kind == dir::EventKind::kCamera && dp::ParseRig(m->text, &rig);
        if (is_rig) {
            ImGui::TextColored(ToVec4(kYellow), "%s RIG ON %s  AT %s s", dp::Lower(rig.kind).c_str(), rig.subject.empty() ? "A PLACE" : rig.subject.c_str(), dp::FormatClock(m->t, 2).c_str());
            for (const dp::RigKnob& knob : dp::KnobsFor(rig.kind)) KnobSlider(s, *m, knob, dp::KnobValue(rig, knob));
            if (rig.kind == "orbit") {
                float a = static_cast<float>(rig.az_from), b = static_cast<float>(rig.az_to);
                const dp::RigKnob az{"az", "", -360.0, 360.0, 0.0, 0};
                const int line = m->line;
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.62f);
                bool changed = ImGui::SliderFloat("AZIMUTH FROM", &a, -360.0f, 360.0f, "%.0f");
                const bool done_a = ImGui::IsItemDeactivatedAfterEdit();
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.62f);
                changed = ImGui::SliderFloat("AZIMUTH TO", &b, -360.0f, 360.0f, "%.0f") || changed;
                const bool done_b = ImGui::IsItemDeactivatedAfterEdit();
                if ((changed && now_ - last_knob_ > 0.25) || done_a || done_b) {
                    last_knob_ = now_;
                    QueueEdit("az" + std::to_string(line), [this, line, a, b, rig](const dp::EditContext& ctx, const dir::Snapshot&) {
                        const int i = dp::FindMarkerByLine(layout_, line);
                        return i < 0 ? dp::Cmd{} : dp::SetOrbitAz(ctx, layout_.markers[static_cast<size_t>(i)], rig, a, b);
                    });
                }
                (void)az;
            }
            for (const dp::RigKnob& knob : dp::KeyKnobs()) KnobSlider(s, *m, knob, dp::KeyKnobValue(m->text, knob));
        } else {
            ImGui::TextColored(ToVec4(kDim), "SELECT A RIG KEY ON THE TIMELINE FOR ITS SLIDERS, OR MAKE ONE:");
        }
        ImGui::Separator();
        ImGui::TextColored(ToVec4(kCyan), "NEW RIG KEY AT THE PLAYHEAD");
        const std::vector<std::string>& kinds = dp::RigKinds();
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::BeginCombo("##rigkind", dp::Lower(kinds[static_cast<size_t>(new_rig_kind_)]).c_str())) {
            for (size_t i = 0; i < kinds.size(); ++i) {
                if (ImGui::Selectable(kinds[i].c_str(), static_cast<int>(i) == new_rig_kind_)) new_rig_kind_ = static_cast<int>(i);
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        const std::vector<std::string> cars = dp::CarIds(s);
        if (new_rig_car_.empty() || std::find(cars.begin(), cars.end(), new_rig_car_) == cars.end()) new_rig_car_ = cars.size() > 1 ? cars[1] : cars[0];
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::BeginCombo("##rigcar", new_rig_car_.c_str())) {
            for (const std::string& c : cars) {
                if (ImGui::Selectable(c.c_str(), c == new_rig_car_)) new_rig_car_ = c;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (Btn("ADD RIG KEY")) {
            dp::NewRig nr;
            nr.kind = kinds[static_cast<size_t>(new_rig_kind_)];
            nr.car = new_rig_car_;
            nr.route = route_.name.empty() ? "" : route_.name;
            nr.has_pos = true;
            for (int i = 0; i < 3; ++i) nr.pos[i] = s.camera.pose.pos[i];
            Send(dp::AddRigKey(0.0, false, nr));
        }
        ImGui::TextColored(ToVec4(kDim), "A tripod stands where the camera is; a fly-by follows the route in the PATHS tab.");
    }

    void TabCars(const dir::Snapshot& s) {
        if (!models_asked_) RequestModels();
        ImGui::TextColored(ToVec4(kCyan), "SPAWN A CAR AT THE CROSSHAIR");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
        ImGui::InputTextWithHint("##modelsearch", "search models", search_, sizeof(search_));
        ImGui::SameLine();
        ImGui::TextColored(ToVec4(kYellow), "%s", model_.empty() ? "(pick a model)" : dp::ModelTitle(model_).c_str());
        const std::vector<std::string> list = dp::FilterModels(models_, search_, recents_, 300);
        ImGui::BeginChild("##modellist", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, std::max(40.0f, ImGui::GetContentRegionAvail().y - 88.0f)), ImGuiChildFlags_Borders);
        int shown_rows = 0;
        for (const std::string& name : list) {
            const bool recent = std::find(recents_.begin(), recents_.end(), name) != recents_.end();
            std::string row = dp::ModelTitle(name) + (recent ? "  (RECENT)" : "");
            if (ImGui::Selectable(row.c_str(), name == model_)) model_ = name;
            if (shown_rows < 6) Geo("MODEL " + std::to_string(shown_rows));  // where a test clicks the first rows of the picker
            ++shown_rows;
        }
        if (models_.empty()) ImGui::TextColored(ToVec4(kDim), "(the model list comes from the game: it fills in a moment)");
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##carlist", ImVec2(0.0f, std::max(40.0f, ImGui::GetContentRegionAvail().y - 88.0f)), ImGuiChildFlags_Borders);
        ImGui::TextColored(ToVec4(kDim), "CARS (click one in the world to drag it)");
        for (const dir::SnapCar& c : s.cars) {
            std::string row = c.id + (c.ready ? "" : "  (loading)") + (c.on_path ? "  on a path" : "");
            if (ImGui::Selectable(row.c_str(), c.id == selected_car_)) selected_car_ = c.id;
        }
        ImGui::EndChild();
        ImGui::Checkbox("SPAWN AT 0", &spawn_zero_);
        ImGui::SameLine();
        if (Btn("SPAWN HERE", !model_.empty())) SpawnHere(s);
        ImGui::SameLine();
        const dir::SnapCar* sel = nullptr;
        for (const dir::SnapCar& c : s.cars) {
            if (c.id == selected_car_) sel = &c;
        }
        if (sel && sel->id != "player") {
            if (Btn("PLACE AT CROSSHAIR")) PlaceAtCrosshair(*sel);
            ImGui::SameLine();
            if (Btn("LOOK AT")) Send(dp::LookAtCmd("car:" + sel->id));
            ImGui::SameLine();
            if (Btn("DELETE CAR")) {
                const std::string id = sel->id;
                QueueEdit("delcar" + id, [id](const dp::EditContext& ctx, const dir::Snapshot& snap) { return dp::DeleteCar(ctx, snap.events, id); });
                selected_car_.clear();
            }
            float heading = static_cast<float>(sel->heading);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.35f);
            // (written on release only: a changed spawn line makes the car again, about 4 s, so a drag must not write it on the way)
            ImGui::SliderFloat("HEADING", &heading, 0.0f, 360.0f, "%.0f");
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                const std::string id = sel->id;
                const double h = heading;
                QueueEdit("head" + id, [id, h](const dp::EditContext& ctx, const dir::Snapshot& snap) { return dp::SetCarHeading(ctx, snap.events, id, h); });
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70.0f);
            ImGui::InputInt("PAINT", &paint_, 0, 0);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                const std::string id = sel->id;
                const int p = paint_;
                QueueEdit("paint" + id, [id, p](const dp::EditContext& ctx, const dir::Snapshot& snap) { return dp::SetCarPaint(ctx, snap.events, id, p); });
            }
        }
    }

    // The ground under the crosshair (the middle of the picture), the car's heading = the camera's yaw; two commands: query.pick, then car.spawn.
    void SpawnHere(const dir::Snapshot& s) {
        const std::string id = dp::NextCarId(s.cars, s.events);
        const std::string model = model_;
        const double yaw = s.camera.pose.yaw;
        const bool at_zero = spawn_zero_;
        Send(dp::PickAt(0.5, 0.5), [this, id, model, yaw, at_zero](const dir::Reply& r) {
            double pos[3];
            std::string why;
            if (!dp::ParsePick(r, pos, &why)) {
                disp_.Note(now_, true, why);
                return;
            }
            pos[1] += 0.3;  // a hand above the road: the game settles the car onto it
            Send(dp::SpawnCar(id, model, pos, yaw, at_zero ? 0.0 : -1.0, -1));
            dp::NoteRecent(&recents_, model);
            selected_car_ = id;
        });
    }

    void PlaceAtCrosshair(const dir::SnapCar& car) {
        const std::string id = car.id;
        const double yaw = car.heading;
        Send(dp::PickAt(0.5, 0.5), [this, id, yaw](const dir::Reply& r) {
            double pos[3];
            std::string why;
            if (!dp::ParsePick(r, pos, &why)) {
                disp_.Note(now_, true, why);
                return;
            }
            pos[1] += 0.3;
            Send(dp::PlaceCar(id, pos, yaw, false));
            const std::array<double, 3> at = {pos[0], pos[1], pos[2]};
            QueueEdit("drop" + id, [id, at](const dp::EditContext& ctx, const dir::Snapshot& snap) {
                const double p[3] = {at[0], at[1], at[2]};
                return dp::MoveCarSpawn(ctx, snap.events, id, p, 0.0, false);
            });
        });
    }

    void TabPaths(const dir::Snapshot& s) {
        if (!routes_asked_) RefreshRouteList();
        // the working route
        ImGui::SetNextItemWidth(140.0f);
        ImGui::InputText("ROUTE", route_name_, sizeof(route_name_));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::SliderFloat("SPEED", &wp_speed_, 2.0f, 60.0f, "%.0f m/s");
        ImGui::SameLine();
        if (ImGui::Checkbox("DROP WAYPOINTS", &drop_mode_)) {}
        Geo("DROP WAYPOINTS");
        ImGui::SameLine();
        if (Btn("DROP AT CAMERA")) {
            const std::string name = dp::SafeRouteName(route_name_);
            if (name.empty()) {
                disp_.Note(now_, true, "name the route first");
            } else {
                Send(dp::DropWaypointAtCamera(name, wp_speed_));
                LoadRoute(name);
            }
        }
        if (drop_mode_) ImGui::TextColored(ToVec4(kYellow), "CLICK THE GROUND IN THE PICTURE TO DROP A POINT (ESC STOPS)");
        // the route list
        const float below = ImGui::GetFontSize() * 2.0f + 14.0f;  // the recording row under the lists
        ImGui::BeginChild("##routelist", ImVec2(ImGui::GetContentRegionAvail().x * 0.30f, std::max(40.0f, ImGui::GetContentRegionAvail().y - below)), ImGuiChildFlags_Borders);
        if (ImGui::SmallButton("REFRESH")) RefreshRouteList();
        for (const std::string& n : route_names_) {
            if (ImGui::Selectable(n.c_str(), n == route_.name)) {
                LoadRoute(n);
                std::memset(route_name_, 0, sizeof(route_name_));
                std::strncpy(route_name_, dp::SafeRouteName(n.substr(0, n.find(".route"))).c_str(), sizeof(route_name_) - 1);
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##routebody", ImVec2(0.0f, std::max(40.0f, ImGui::GetContentRegionAvail().y - below)), ImGuiChildFlags_Borders);
        if (!route_.loaded) {
            ImGui::TextColored(ToVec4(kDim), "PICK A ROUTE, OR DROP POINTS (a new route is made by the first one)");
        } else {
            ImGui::TextColored(ToVec4(kYellow), "%s: %zu POINTS, %.0f M, %.1f S", route_.name.c_str(), route_.pts.size(), dp::RouteLengthM(route_), dp::RouteSeconds(route_));
            if (Btn("SMOOTH")) {
                route_ = dp::SmoothRoute(route_, 1);
                Send(dp::SetRoute(route_));
            }
            Gap();
            ImGui::SetNextItemWidth(70.0f);
            ImGui::SliderFloat("##allspeed", &all_speed_, 2.0f, 60.0f, "%.0f");
            ImGui::SameLine();
            if (Btn("ALL SPEEDS")) {
                dp::RouteSetAllSpeeds(&route_, all_speed_);
                Send(dp::SetRoute(route_));
            }
            Gap();
            const std::vector<std::string> cars = dp::CarIds(s);
            if (Btn("REPLAY WITH A CAR", !model_.empty())) {
                for (const dp::Cmd& c : dp::ReplayCmds(dp::NextCarId(s.cars, s.events), model_, route_, s.transport.t, 0.0)) Send(c);
            }
            if (selected_car_ != "" && selected_car_ != "player") {
                Gap();
                if (Btn("USE ON SELECTED CAR")) Send(dp::AssignRoute(selected_car_, route_.name, 0.0, s.transport.t, true));
            }
            for (size_t i = 0; i < route_.pts.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                dp::WayPt& p = route_.pts[i];
                ImGui::Text("%zu", i + 1);
                ImGui::SameLine(34.0f);
                ImGui::TextColored(ToVec4(kDim), "%.0f %.0f", p.x, p.z);
                ImGui::SameLine(130.0f);
                float sp = static_cast<float>(p.speed);
                ImGui::SetNextItemWidth(110.0f);
                if (ImGui::SliderFloat("##sp", &sp, 0.5f, 60.0f, "%.1f m/s")) dp::RouteSetSpeed(&route_, i, sp);
                if (ImGui::IsItemDeactivatedAfterEdit()) Send(dp::SetRoute(route_));
                ImGui::SameLine();
                if (ImGui::SmallButton("+")) {
                    dp::RouteInsertAfter(&route_, i);
                    Send(dp::SetRoute(route_));
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("x") && dp::RouteDelete(&route_, i)) {
                    Send(dp::SetRoute(route_));
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        // drive and record
        RecordRow(s);
    }

    void RecordRow(const dir::Snapshot& s) {
        // (drawn under the route body by the caller's layout: a row of its own at the bottom of the tab)
        ImGui::Separator();
        ImGui::SetNextItemWidth(110.0f);
        ImGui::InputText("##recname", rec_name_, sizeof(rec_name_));
        ImGui::SameLine();
        if (!recording_) {
            if (Btn("RECORD A DRIVE (PLAYER)")) {
                const std::string name = dp::SafeRouteName(rec_name_);
                if (name.empty()) {
                    disp_.Note(now_, true, "name the recording first");
                } else {
                    Send(dp::StartRecording("player", name, 2.0), [this](const dir::Reply& r) {
                        if (!r.ok) return;
                        recording_ = true;
                        keys_before_record_ = game_keys_;
                        game_keys_ = true;  // the keyboard drives the car now
                    });
                }
            }
        } else {
            ImGui::TextColored(ToVec4(kRed), "RECORDING: DRIVE");
            ImGui::SameLine();
            if (Btn("STOP")) {
                Send(dp::StopRecording(), [this](const dir::Reply& r) {
                    recording_ = false;
                    game_keys_ = keys_before_record_;
                    if (!r.ok) return;
                    const std::string name = r.result.StringOr("route", "");
                    disp_.Note(now_, false, "recorded " + name + ": " + std::to_string(r.result.IntOr("points", 0)) + " points");
                    RefreshRouteList();
                    LoadRoute(name);
                });
            }
        }
        (void)s;
    }

    void TabTake(const dir::Snapshot& s) {
        ImGui::SetNextItemWidth(70.0f);
        if (ImGui::BeginCombo("FPS", std::to_string(take_.fps).c_str())) {
            for (int f : dp::kTakeFps) {
                if (ImGui::Selectable(std::to_string(f).c_str(), f == take_.fps)) take_.fps = f;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::BeginCombo("SINK", dp::kSinks[take_.sink])) {
            for (int i = 0; i < 4; ++i) {
                if (ImGui::Selectable(dp::kSinks[i], i == take_.sink)) take_.sink = i;
            }
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputTextWithHint("NAME", "from the scene", take_name_, sizeof(take_name_));
        float from = static_cast<float>(take_.from), to = static_cast<float>(take_.to < 0.0 ? s.transport.duration : take_.to);
        ImGui::SetNextItemWidth(80.0f);
        if (ImGui::InputFloat("IN", &from, 0.0f, 0.0f, "%.2f")) take_.from = std::max(0.0f, from);
        ImGui::SameLine();
        if (Btn("IN = PLAYHEAD")) take_.from = s.transport.t;
        ImGui::SetNextItemWidth(80.0f);
        if (ImGui::InputFloat("OUT", &to, 0.0f, 0.0f, "%.2f")) take_.to = std::max(0.0f, to);
        ImGui::SameLine();
        if (Btn("OUT = PLAYHEAD")) take_.to = s.transport.t;
        ImGui::SameLine();
        if (Btn("WHOLE SCENE")) {
            take_.from = 0.0;
            take_.to = -1.0;
        }
        if (s.take_active) {
            ImGui::TextColored(ToVec4(kRed), "RENDERING %s: %llu FRAMES", s.take_name.c_str(), static_cast<unsigned long long>(s.take_frames));
        } else if (Btn("RENDER TAKE")) {
            take_.name = take_name_;
            Send(dp::TakeRenderCmd(take_));
        }
        if (!take_folder_.empty()) {
            ImGui::TextColored(ToVec4(kDim), "LAST TAKE: %s", take_folder_.c_str());
            if (Btn("OPEN FOLDER")) OpenFolder(take_folder_);
        }
        ImGui::TextColored(ToVec4(kDim), "The take is made by the game at the scene's fixed step: preview = take. Back + D-pad up renders it from the pad.");
    }

    static void OpenFolder(const std::string& folder) {
#if defined(_WIN32)
        const std::wstring w(folder.begin(), folder.end());
        ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
        (void)folder;
#endif
    }

    void TabAgent(const dir::Snapshot& s) {
        if (s.agents.empty()) ImGui::TextColored(ToVec4(kDim), "NO AGENT IS CONNECTED");
        for (const dir::SnapAgent& a : s.agents) {
            ImGui::TextColored(ToVec4(kGreen), "AGENT %s", a.name.c_str());
            ImGui::SameLine();
            ImGui::TextColored(ToVec4(kDim), "  %llu COMMANDS, LAST %.0f S AGO: %s", static_cast<unsigned long long>(a.commands), a.last_cmd_age_s, a.last_cmd.c_str());
        }
        if (!s.proposal_path.empty()) {
            ImGui::Separator();
            ImGui::TextColored(ToVec4(kYellow), "AGENT PROPOSAL: %s", s.proposal_note.c_str());
            if (Btn("ACCEPT")) Send(UndoCmd("scene.accept"));
            Gap();
            if (Btn("REJECT")) Send(UndoCmd("scene.reject"));
        }
        ImGui::Separator();
        ImGui::TextColored(ToVec4(kDim), "LAST COMMANDS (the panel's and the agents')");
        const std::vector<dir::LoggedCommand> recent = dir::RecentCommands(8);
        for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
            ImGui::TextColored(ToVec4(it->ok ? kWhite : kRed), "[%s] %s%s", dir::SourceName(it->source), it->line.c_str(), it->ok ? "" : ("   -> " + it->error).c_str());
        }
        ImGui::Separator();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.8f);
        const bool enter = ImGui::InputTextWithHint("##console", "a command: camera.set pos=819.6,19,-800 yaw=80   (help lists them)", console_, sizeof(console_), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (enter || Btn("RUN")) {
            std::string cmd, err;
            dir::Json args;
            if (console_[0] && dir::ParseConsoleLine(console_, &cmd, &args, &err)) {
                dp::Cmd c;
                c.cmd = cmd;
                c.args = args;
                Send(c);
                console_[0] = 0;
            } else if (console_[0]) {
                disp_.Note(now_, true, err);
            }
        }
    }

    // ------------------------------------------------------------------------------------------------------------ state

    rex::ui::IUISystem& ui_;
    dp::Dispatcher disp_;
    std::shared_ptr<SharedEvents> events_;
    int listener_ = -1;
    std::shared_ptr<const dir::Snapshot> snap_;
    double now_ = 0.0;
    bool open_ = false, hidden_ = false, collapsed_ = false, gizmos_on_ = true, game_keys_ = false, keys_before_record_ = false;
    int tab_ = 0, want_tab_ = -1;
    uint64_t seen_ui_serial_ = 0;
    int seen_snapshot_selected_ = 0, selected_line_ = 0;
    std::string last_cmd_;
    double last_cmd_time_ = 0.0;

    // the scene as the panel holds it
    dp::SceneText scene_text_;
    bool text_inflight_ = false;
    double text_retry_at_ = 0.0;
    int edits_inflight_ = 0;
    std::vector<QueuedEdit> queue_;
    dp::Layout layout_;
    int64_t layout_rev_ = -1;
    size_t layout_events_ = 0;
    double layout_end_ = -2.0;
    dp::Axis axis_;
    bool axis_fit_ = false;
    dp::Drag drag_;
    bool scrubbing_ = false;
    double last_scrub_ = 0.0, last_knob_ = 0.0, last_place_ = 0.0;

    // where things are (the ui.geometry event)
    std::map<std::string, std::array<float, 4>> geom_;
    dp::Metrics metrics_;
    float canvas_y_ = 0.0f;
    double last_geom_ = 0.0;
    bool geom_dirty_ = false;
    std::string last_geom_dump_;

    // the world
    dp::Projector view_;
    dp::Gizmos gizmos_;
    dp::FlyTuning tune_;
    bool fly_prev_ = false, look_held_ = false;

    // cars
    std::vector<std::string> models_, recents_;
    bool models_asked_ = false, spawn_zero_ = true;
    char search_[64];
    std::string model_, selected_car_;
    std::string car_drag_;
    double car_drag_y_ = 0.0, car_drop_[3] = {0, 0, 0};
    bool car_drag_moved_ = false;
    int paint_ = 0;
    int new_rig_kind_ = 0;
    std::string new_rig_car_;

    // paths
    std::vector<std::string> route_names_;
    bool routes_asked_ = false, drop_mode_ = false, recording_ = false;
    dp::RouteDoc route_;
    char route_name_[48], rec_name_[48];
    float wp_speed_ = 14.0f, all_speed_ = 14.0f;
    int wp_drag_ = -1;

    // take and console
    dp::TakeForm take_;
    char take_name_[48];
    std::string take_folder_;
    uint64_t seen_take_done_ = 0;
    char console_[160];
};

}  // namespace

void StartDirectorPanel(rex::ui::IUISystem& ui) {
    mc::cinematic::SetFreecamEventHook(&OnFlyEvents);  // the pad's chords: Back + B, RB, LB, Start, D-pad right, D-pad up (a flag test an update while the camera does not fly)
    ui.AddDialog(std::make_unique<DirectorPanel>(ui));
}

namespace {

void DirectorKeyPressed() {
    const bool on = larecomp::settings::mcla_director_panel.Get();
    larecomp::cvar::SetFlagByName("mcla_director_panel", on ? "false" : "true");
}

}  // namespace

rex::ui::OverlayConfig DirectorOverlayConfig() {
    return {"mcla_director", rex::KeyChord{rex::VirtualKey::kF12, rex::KeyModifiers::kCtrl}, &MakeHotkey<&DirectorKeyPressed>};
}

}  // namespace mcla::ui
