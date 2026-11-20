#pragma once
//
// The Director panel's timeline: the pure half (DIRECTOR-DESIGN.md section 3, "Timeline"). The scene file's keys (the snapshot's events) become LANES and MARKERS; a time
// axis maps seconds to pixels with zoom and pan; a hit test finds the marker under the pointer; a drag moves, or with Alt duplicates, a key and snaps to the frame grid, to
// the other keys, to the playhead and to the ends. What a drag or a click MEANS for the file (a `move` op, an insert, an `ease` word) is built in director_panel_core.h;
// the ImGui drawing is in director_panel.cpp. No ImGui, no game, no log: director_panel_test.cpp builds it on its own.
//
// Rows (the design's list): CAMERA (a diamond per `cam` key, a bar per rig span, a line per `cut`), one lane per CAR (spawn, a bar per path span), WORLD (hour, weather, traffic,
// time scale, settings), HUD, CAPTURE (the take's in and out), MARKS (labels, marks, the split). The CAMERA lane is always there, so the keys have somewhere to go.
//

#include <cstdint>
#include <string>
#include <vector>

#include "director_gizmos.h"  // Vec2f
#include "mc_engine/cinematic/director/director.h"

namespace mcla::ui::director {

enum class LaneKind : uint8_t { kCamera, kCar, kWorld, kHud, kCapture, kMarks, kOther };
const char* LaneKindName(LaneKind kind);

struct Lane {
    LaneKind kind = LaneKind::kOther;
    std::string title;  // "CAMERA", "CAR p1", ...
    std::string car;    // kCar: the car's id
};

struct Marker {
    int line = 0;        // the scene file's line (1-based): what every edit addresses
    double t = 0.0;
    int lane = 0;        // index into Layout::lanes
    dir::EventKind kind = dir::EventKind::kOther;
    std::string verb;    // the statement's first word ("cam", "cut", "at", "car", "hour", ...)
    std::string label;   // a few words for the marker ("rig chase p1", "pos", "spawn vp_lam_murcielago_06", "path run.route")
    std::string text;    // the statement as the snapshot has it
    std::string ease;    // a camera key's `ease` word, "" when it has none
    bool is_cut = false;
    bool has_span = false;
    double span_end = 0.0;  // a rig key runs to the next camera key; a car's path runs to its next path / stop / delete
    bool movable = true;
};

struct Layout {
    std::vector<Lane> lanes;
    std::vector<Marker> markers;  // in time order inside a lane; lanes in the order above
    double end = 10.0;            // the scene's end when it has one, else a little after the last key
};

// The lanes and markers of the snapshot's events. `duration` is the scene's `end` (negative: none).
Layout BuildLayout(const std::vector<dir::SnapEvent>& events, double duration);
// The marker with that scene line, or -1.
int FindMarkerByLine(const Layout& layout, int line);
// The statement's words that make the marker's label (exposed for the tests): "cam 5 rig chase car:p1 dist 7" -> "rig chase p1".
std::string DescribeStatement(const dir::SnapEvent& event);
// The times of the markers of a lane kind, in order.
std::vector<double> MarkerTimes(const Layout& layout, int except_index = -1);

// ---------------------------------------------------------------------------------------------------------------- the time axis

struct Axis {
    double t0 = 0.0, t1 = 10.0;  // the visible scene times
    float x0 = 0.0f, x1 = 100.0f;  // where they lie, in pixels

    double PixelsPerSecond() const;
    float TimeToX(double t) const;
    double XToTime(float x) const;
    void Fit(double end);                      // 0 .. end with a margin
    void ZoomAt(float x, double factor);       // factor above 1 zooms in; the time under x stays under x
    void Pan(float dx_pixels);                 // the content follows the pointer
    void KeepInside(double lo, double hi);     // the view never leaves lo .. hi (plus a margin the size of a tenth of the view)
};
inline constexpr double kMinViewSpan = 0.4;     // seconds: the closest zoom
inline constexpr double kMaxViewSpan = 7200.0;

struct Tick {
    double t = 0.0;
    bool major = false;
    std::string label;  // major ticks only
};
// The ruler's ticks for the visible range: a label every `min_label_px` pixels or more, on 1, 2, 5, 10, 15, 30 ... second steps, minor ticks between.
std::vector<Tick> RulerTicks(const Axis& axis, float min_label_px = 78.0f);

// ---------------------------------------------------------------------------------------------------------------- snapping

enum class SnapKind : uint8_t { kNone, kFrame, kMark, kPlayhead, kStart, kEnd };
struct Snapped {
    double t = 0.0;
    SnapKind kind = SnapKind::kNone;
};
// `t` snapped to (nearest first within `snap_px`): another key's time, the playhead, 0, the end; then to the frame grid (`frame_step` seconds, 0 = no grid). Never negative.
Snapped SnapTime(double t, const Axis& axis, const std::vector<double>& marks, double playhead, double end, double frame_step, float snap_px = 7.0f);

// ---------------------------------------------------------------------------------------------------------------- hit test

struct Metrics {
    float ruler_h = 24.0f;
    float lane_h = 26.0f;
    float lane_gap = 2.0f;
    float marker_r = 7.0f;
    // A COLLAPSED lane is a thin strip (its keys are small dots, no labels): a scene with many cars still shows every lane in a short dock. `collapsed[i]` for lane i; a lane
    // past the end of the vector is open.
    float collapsed_h = 9.0f;
    float collapsed_marker_r = 3.5f;
    std::vector<bool> collapsed;
    bool Collapsed(int lane) const { return lane >= 0 && static_cast<size_t>(lane) < collapsed.size() && collapsed[static_cast<size_t>(lane)]; }
    float LaneHeight(int lane) const { return Collapsed(lane) ? collapsed_h : lane_h; }
    float MarkerRadius(int lane) const { return Collapsed(lane) ? collapsed_marker_r : marker_r; }
    float LaneTop(int lane) const {
        float y = ruler_h;
        for (int i = 0; i < lane; ++i) y += LaneHeight(i) + lane_gap;
        return y;
    }
    float LaneCenter(int lane) const { return LaneTop(lane) + LaneHeight(lane) * 0.5f; }
    float Height(int lane_count) const { return LaneTop(lane_count); }
};
// The lane under a y inside the canvas (0 = the canvas' top edge), or -1 (the ruler, a gap, below the last lane).
int LaneAt(const Metrics& metrics, float y, int lane_count);
// Which lanes are collapsed. `pref[i]`: -1 the user collapsed lane i, +1 the user opened it, 0 untouched. Untouched lanes are open while every open lane can still be `min_lane_h`
// tall in `avail_h` (the canvas' height, ruler included); when they cannot, untouched lanes collapse from the bottom up, never the lane `keep_open` (the camera lane, or the lane of
// the selected key; -1: none), until they fit or none is left to collapse. The metrics' own `collapsed` vector is not read.
std::vector<bool> CollapsedLanes(const Metrics& metrics, const std::vector<int>& pref, float avail_h, float min_lane_h, int keep_open);
// The height of an open lane that fills `avail_h` with the lanes collapsed as `metrics.collapsed` says, clamped to min_h .. max_h.
float FitLaneHeight(const Metrics& metrics, int lane_count, float avail_h, float min_h, float max_h);
// The marker under a point of the canvas, or -1. A marker is hit within `slop` pixels of its centre; a cut (a vertical line across its lane) is hit anywhere in the lane within
// `slop` of its x. When several are hit the nearest one wins (a later one, on a tie: it is drawn on top).
int HitMarker(const Layout& layout, const Axis& axis, const Metrics& metrics, Vec2f point, float slop);
// A bar (a rig or path span) under the point: the marker that owns it, or -1. Only when no marker is hit.
int HitSpan(const Layout& layout, const Axis& axis, const Metrics& metrics, Vec2f point);

// ---------------------------------------------------------------------------------------------------------------- dragging

struct Drag {
    bool active = false;
    int marker = -1;       // index into Layout::markers
    int line = 0;
    double t_start = 0.0;  // the key's time when the drag began
    double grab_dt = 0.0;  // the pointer's time minus the key's time at the grab: the key does not jump to the pointer
    double t_now = 0.0;    // where the key would land now (snapped)
    bool duplicate = false;  // Alt held: the drop inserts a copy and the original stays
    float start_x = 0.0f;
    bool moved = false;    // the pointer went further than kDragSlopPx
    SnapKind snap = SnapKind::kNone;
};
inline constexpr float kDragSlopPx = 4.0f;

Drag BeginDrag(const Layout& layout, int marker, const Axis& axis, float x, bool duplicate);
// The pointer is at x: where the key lands. `marks` are the other keys' times (MarkerTimes without the dragged one), `snap` false lets the key go where the pointer is (Shift).
void UpdateDrag(Drag* drag, const Axis& axis, float x, const std::vector<double>& marks, double playhead, double end, double frame_step, bool snap);
enum class DragOutcome : uint8_t { kNothing, kMove, kDuplicate };
// What the drop does: nothing when the pointer never left the slop or the time is the same (to the millisecond), else a move (or a copy with Alt).
DragOutcome FinishDrag(const Drag& drag);

// The scene time of a click on the ruler or the lanes' background (the playhead): clamped to 0 .. end, snapped to the frame grid.
double ScrubTime(const Axis& axis, float x, double end, double frame_step);

}  // namespace mcla::ui::director
