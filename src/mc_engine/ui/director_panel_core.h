#pragma once
//
// The Director panel's logic, the pure half (DIRECTOR-DESIGN.md section 3). EVERY ACTION OF THE PANEL IS A COMMAND of the command layer (director.h): what a button, a drag or a
// key means is built here as a `Cmd` (a name and JSON arguments, the wire's own), the panel posts it, the game thread runs it, and the scene file (the single source of truth)
// is changed through `scene.edit` / `camera.key.add` / `car.spawn` ... and nothing else. The panel keeps no second model of the scene: it holds the last snapshot, a copy of the
// scene's text (to give an edit the `expect` of the line it changes) and a few things of its own that are not the scene (the view of the timeline, a route being edited).
//
// No ImGui, no game, no log, no sockets: director_panel_test.cpp builds this with the timeline and the gizmos and checks every builder against the wire's schema.
//

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "director_gizmos.h"
#include "director_timeline.h"
#include "mc_engine/cinematic/director/director.h"
#include "mc_engine/cinematic/director/scene_edit.h"
#include "mc_engine/cinematic/freecam.h"

namespace mcla::ui::director {

namespace fc = mc::cinematic::freecam;

// ---------------------------------------------------------------------------------------------------------------- commands

// One command of the wire: what the panel's action is, as a name and arguments. `FormatCmd` is the line the "what I did" strip prints (director.h FormatConsoleLine).
struct Cmd {
    std::string cmd;
    dir::Json args = dir::Json::Object();
    bool wait_settled = false;
    double timeout_s = 0.0;
};
std::string FormatCmd(const Cmd& cmd);

// A line of a result the panel shows: an error (red) or a note.
struct Notice {
    double time = 0.0;  // the clock of the panel (seconds)
    bool error = false;
    std::string text;
};

// Posts commands and keeps what came back. Post is injected (director.h Post in the game, a fake in the test). A reply arrives on the game thread: it is stored under a lock and its
// continuation runs later, on the thread that calls Drain (the UI thread): ImGui and the panel's state are touched there only.
class Dispatcher {
  public:
    using PostFn = std::function<bool(dir::Posted&&)>;
    using Continuation = std::function<void(const dir::Reply&)>;

    explicit Dispatcher(PostFn post);
    // Posts the command. False (and a notice) when the Director refused it (it is off, or its queue is full). `then` runs from Drain when the reply is in, ok or not.
    bool Send(const Cmd& cmd, Continuation then = {});
    // The continuations whose replies are in, in the order they came; also turns a failed reply into a notice. UI thread.
    void Drain(double now);
    // Callable from any thread (the reply callback calls it).
    void Deliver(uint64_t token, const dir::Reply& reply);

    // Recent notices, newest last (at most 8).
    const std::vector<Notice>& notices() const { return notices_; }
    void Note(double now, bool error, std::string text);
    size_t pending() const;
    uint64_t sent() const { return sent_; }

  private:
    struct Shared;
    PostFn post_;
    std::shared_ptr<Shared> shared_;  // what a reply callback holds on to: it may outlive the Dispatcher by a tick
    uint64_t sent_ = 0;
    double last_now_ = 0.0;
    std::vector<Notice> notices_;  // the UI thread's
};

// ---------------------------------------------------------------------------------------------------------------- the scene's text

// The scene file as the Director has it (`scene.get {text:true}`): an edit gives the line it changes as `expect`, and a line with a trailing comment is not what the snapshot's
// statement says. Refreshed when the snapshot's revision moves.
class SceneText {
  public:
    void Set(int64_t rev, std::string text);
    int64_t rev() const { return rev_; }
    bool Has() const { return rev_ >= 0; }
    // Line `n` (1-based) without its line ending; "" when there is no such line.
    std::string Line(int n) const;
    int LineCount() const { return static_cast<int>(lines_.size()); }

  private:
    int64_t rev_ = -1;
    std::vector<std::string> lines_;
};
std::vector<std::string> SplitLines(const std::string& text);

// What an edit needs: the revision it was made against and the text to take `expect` from. `snapshot_rev` is the snapshot's revision; the text is trusted for `expect` only while
// it is that one (otherwise the statement of the event is used, which a stale or comment-carrying line will refuse: the panel then refreshes and tries again).
struct EditContext {
    int64_t rev = 0;
    const SceneText* text = nullptr;
    std::string ExpectFor(int line, const std::string& statement) const;
};

Cmd EditCmd(int64_t base_rev, const std::vector<dir::EditOp>& ops);
dir::EditOp OpMove(int line, const std::string& expect, double t);
dir::EditOp OpParam(int line, const std::string& expect, const std::string& key, const std::string& value);
dir::EditOp OpReplace(int line, const std::string& expect, const std::string& text);
dir::EditOp OpDelete(int line, const std::string& expect);
dir::EditOp OpInsert(const std::string& text);
// The statement with its time word changed (a copy for an Alt-drag): comment and layout dropped, the time written as the scene writes it.
std::string StatementAtTime(const std::string& line, double t);

// ---------------------------------------------------------------------------------------------------------------- keys: the timeline's actions

// Drag a key: scene.edit move. Alt-drag: an insert of a copy at the new time.
Cmd MoveKey(const EditContext& ctx, const Marker& marker, double t);
Cmd DuplicateKey(const EditContext& ctx, const Marker& marker, double t);
Cmd DeleteKey(const EditContext& ctx, const Marker& marker);
// Right-click: how the segment that starts at a camera key runs. "linear", "in", "out", "inout".
Cmd SetEase(const EditContext& ctx, const Marker& marker, const std::string& ease);
inline const char* const kEases[4] = {"linear", "in", "out", "inout"};
// A camera key from the camera as it is now: camera.key.add (t default: the scene's time now).
Cmd AddKeyHere(double t, bool use_t, const std::string& ease, bool cut);
// "Update key": the key at `marker` takes the pose the free camera has: the line becomes `cam <T> pos .. yaw .. pitch .. roll .. fov ..` (its ease word is kept, a rig or attach is replaced).
Cmd UpdateKeyFromPose(const EditContext& ctx, const Marker& marker, const dir::SnapPose& pose);
Cmd SetKeyWord(const EditContext& ctx, const Marker& marker, const std::string& key, const std::string& value);
Cmd CutAt(int64_t rev, double t);           // scene.edit insert `cut <t>`
// The pose a `cam` key states by itself (pos with yaw and pitch, or pos with look x y z; fov, roll when given): false for a rig, an attach or an orbit key, whose pose depends on a car.
bool ParseKeyPose(const std::string& statement, dir::SnapPose* pose);

// The camera keys of the snapshot in time order (their times, lines and statements).
struct KeyRef {
    int line = 0;
    double t = 0.0;
    std::string text;
};
std::vector<KeyRef> CameraKeys(const std::vector<dir::SnapEvent>& events);
// Back + RB / LB, and the panel's next / previous key: select the key, go to its time, and put the free camera on its pose when it has one of its own (else the scene's camera shows it).
// `direction` +1 next, -1 previous; no commands when there is no such key.
std::vector<Cmd> GoToKeyCmds(const dir::Snapshot& snap, int direction);

// ---------------------------------------------------------------------------------------------------------------- rigs

struct RigKnob {
    const char* key;    // the scene word
    const char* label;  // on the slider
    double lo, hi, def;
    int decimals;
};
// chase, orbit, tripod, flyby, hood, bumper, roof
const std::vector<std::string>& RigKinds();
// The sliders of a kind (fov and roll are the key's own and shared by every rig: KeyKnobs).
const std::vector<RigKnob>& KnobsFor(const std::string& kind);
const std::vector<RigKnob>& KeyKnobs();

struct RigInfo {
    std::string kind;     // "chase"
    std::string subject;  // the car's id the rig is on ("p1"), "" for none
    std::map<std::string, double> values;  // the numeric words present
    bool az_range = false;
    double az_from = 0.0, az_to = 0.0;  // orbit: `az a..b`
    bool has_over = false;
};
// A `cam <T> rig <kind> ...` statement; false when it is not a rig key.
bool ParseRig(const std::string& statement, RigInfo* out);
double KnobValue(const RigInfo& rig, const RigKnob& knob);
double KeyKnobValue(const std::string& statement, const RigKnob& knob);  // fov, roll of any cam key (the default when the key has none)
// A new rig key: the kind, the car it is on, and what some kinds need besides (a tripod stands at a place, a fly-by follows a route).
struct NewRig {
    std::string kind;   // RigKinds()
    std::string car;    // "p1"
    std::string route;  // flyby: a route's name
    bool has_pos = false;
    double pos[3] = {0, 0, 0};  // tripod: where it stands (the free camera's place)
};
// The words of a new rig key: "chase car:p1 dist 6 height 1.8" (camera.key.add's `rig`); "tripod at x y z track car:p1"; "flyby path run.route track car:p1".
std::string NewRigWords(const NewRig& rig);
Cmd AddRigKey(double t, bool use_t, const NewRig& rig);
// A slider moved: one word pair of the key's line rewritten (scene.edit param).
Cmd SetKnob(const EditContext& ctx, const Marker& marker, const RigKnob& knob, double value);
// An orbit's sweep: az a..b (and `over 8` when it has none).
Cmd SetOrbitAz(const EditContext& ctx, const Marker& marker, const RigInfo& rig, double from, double to);

// ---------------------------------------------------------------------------------------------------------------- cars

// The first id of the form p1, p2 ... that no car of the scene has.
std::string NextCarId(const std::vector<dir::SnapCar>& cars, const std::vector<dir::SnapEvent>& events);
// car.spawn. t < 0: the scene's time now. A car meant to stand in the picture spawns at 0 (the ready gate holds the clock for it), so the panel's default is 0.
Cmd SpawnCar(const std::string& id, const std::string& model, const double at[3], double heading, double t, int paint);
// What `query.pick` answered: the ground point under the crosshair. False (and `why`) for a miss.
bool ParsePick(const dir::Reply& reply, double pos[3], std::string* why);
Cmd PickAt(double u, double v);  // query.pick
// The spawn line of a car in the scene, or -1; `lines` of all the car's statements.
int SpawnLineOf(const std::vector<dir::SnapEvent>& events, const std::string& car);
std::vector<int> LinesOfCar(const std::vector<dir::SnapEvent>& events, const std::string& car);
// A car dragged to a new place: scene.edit param at / heading on its spawn line (the one write on release); the live move while dragging is car.place.
Cmd MoveCarSpawn(const EditContext& ctx, const std::vector<dir::SnapEvent>& events, const std::string& car, const double at[3], double heading, bool set_heading);
// The statement with the value after each key rewritten (the key is added when the line has none), the other words kept, its trailing comment kept: one `replace` op where
// several words change (two param ops on a line would each have to carry the text the other left behind as `expect`).
std::string RewriteWords(const std::string& line, const std::vector<std::pair<std::string, std::string>>& sets);
Cmd PlaceCar(const std::string& car, const double at[3], double heading, bool set_heading);
Cmd SetCarHeading(const EditContext& ctx, const std::vector<dir::SnapEvent>& events, const std::string& car, double heading);
Cmd SetCarPaint(const EditContext& ctx, const std::vector<dir::SnapEvent>& events, const std::string& car, int paint);
Cmd DeleteCar(const EditContext& ctx, const std::vector<dir::SnapEvent>& events, const std::string& car);

// The model picker: the names `cars.models` gave, filtered by the search text (every word of it must be in the name, any case), the recent ones first.
std::vector<std::string> FilterModels(const std::vector<std::string>& all, const std::string& search, const std::vector<std::string>& recents, size_t max_count = 400);
void NoteRecent(std::vector<std::string>* recents, const std::string& model, size_t keep = 8);
// "vp_lam_murcielago_06" -> "LAM MURCIELAGO 06": a readable name for a list row.
std::string ModelTitle(const std::string& model);

// Asking the game for the model names (`cars.models`). The picker's list used to be asked for once and an answer with no names (the game's car table not set up yet, or a failed
// command) stayed for the whole session, under a line that said it "fills in a moment". Now an empty list is asked again every 2 s, six times at most, and then the picker says so.
struct ModelsAsk {
    static constexpr int kMaxAsks = 6;
    static constexpr double kRetryS = 2.0;
    int sent = 0;
    double last = 0.0;
    bool pending = false;  // a request is on its way
    // True when a request should go out now: none yet, or the list is still empty after an answer and the retry time has passed.
    bool Due(double now, size_t have) const {
        if (pending || have > 0) return false;
        if (sent == 0) return true;
        return sent < kMaxAsks && now - last >= kRetryS;
    }
    void Sent(double now) {
        ++sent;
        last = now;
        pending = true;
    }
    void Answered() { pending = false; }
    // Asked as often as it will be and still no names: the picker should say so instead of waiting.
    bool GaveUp(size_t have) const { return have == 0 && !pending && sent >= kMaxAsks; }
};

// ---------------------------------------------------------------------------------------------------------------- paths

struct WayPt {
    double x = 0, y = 0, z = 0, speed = 14.0;
};
struct RouteDoc {
    std::string name;
    std::vector<WayPt> pts;
    bool loaded = false;
};
RouteDoc RouteFromReply(const std::string& name, const dir::Json& result);  // path.get
std::vector<std::vector<double>> GizmoPoints(const RouteDoc& route);
double RouteLengthM(const RouteDoc& route);
double RouteSeconds(const RouteDoc& route);                                   // along the route at its own speeds
Cmd GetRoute(const std::string& name);                                        // path.get
Cmd SetRoute(const RouteDoc& route);                                          // path.set (handlers_panel.cpp): the whole list written again
Cmd DropWaypoint(const std::string& route, const double at[3], double speed); // path.waypoint at [x,y,z]
Cmd DropWaypointAtCamera(const std::string& route, double speed);             // path.waypoint at "camera"
Cmd StartRecording(const std::string& car, const std::string& name, double every_m);
Cmd StopRecording();
Cmd AssignRoute(const std::string& car, const std::string& route, double speed, double t, bool use_t);  // car.path
// Editing a route in the panel (a copy; SetRoute writes it): insert after an index (the middle of the two points), delete, move, set a point's speed, set every speed.
void RouteInsertAfter(RouteDoc* route, size_t index);
bool RouteDelete(RouteDoc* route, size_t index);  // false for the last two points: a route keeps two
void RouteMove(RouteDoc* route, size_t index, double x, double z);
void RouteSetSpeed(RouteDoc* route, size_t index, double speed);
void RouteSetAllSpeeds(RouteDoc* route, double speed);
// Chaikin's corner cutting on x and z, with y and the speed carried; the ends stay. One pass makes twice the points.
RouteDoc SmoothRoute(const RouteDoc& route, int passes);
// The heading a car at the first point takes to follow the route: degrees, 0 along +Z, 90 along +X.
double RouteStartHeading(const RouteDoc& route);
// "Replay with a car": the commands that put a kinematic car on a route: spawn at the first point heading along it (at 0), then car.path.
std::vector<Cmd> ReplayCmds(const std::string& car_id, const std::string& model, const RouteDoc& route, double path_t, double speed);
std::string SafeRouteName(const std::string& text);  // letters, digits, - and _ only; "" stays ""

// ---------------------------------------------------------------------------------------------------------------- take

struct TakeForm {
    int fps = 60;
    int sink = 2;  // kSinks
    std::string name;
    double from = 0.0;
    double to = -1.0;  // negative: to the scene's end
};
inline const char* const kSinks[4] = {"none", "ffmpeg", "png", "raw"};
inline const int kTakeFps[5] = {24, 25, 30, 60, 120};
Cmd TakeRenderCmd(const TakeForm& form);
Cmd TakeStopHint();  // take.status

// ---------------------------------------------------------------------------------------------------------------- flying with keys and the mouse

// What the keyboard and the mouse say this frame. The panel fills it from ImGui; MakeFlyPad turns it into the pad the free camera flies with (freecam.h): W A S D move along the view
// and sideways, Q E down and up, Shift fast, Ctrl slow, the mouse with the right button held looks, the wheel steps the speed, Z X tilt, R levels the horizon and the field of view,
// + and - zoom.
struct FlyKeys {
    bool forward = false, back = false, left = false, right = false, down = false, up = false;
    bool fast = false, slow = false;
    bool roll_left = false, roll_right = false, level = false;
    bool zoom_in = false, zoom_out = false;
    bool look = false;           // the right button is held over the picture
    float mouse_dx = 0.0f, mouse_dy = 0.0f;  // pixels this frame, only used while `look`
    float wheel = 0.0f;          // notches this frame
};
struct FlyTuning {
    double degrees_per_pixel = 0.12;
    double speed_per_notch = 1.0;  // notches become this many D-pad steps of the speed
};
fc::Pad MakeFlyPad(const FlyKeys& keys, const FlyTuning& tuning, double fov_degrees);
bool AnyFlyInput(const FlyKeys& keys);

// The pad's chords of the free camera (freecam.h Events), as commands against the snapshot: Back + B adds a key, Back + RB / LB next / previous key, Back + Start play or pause,
// Back + D-pad right one frame on, Back + D-pad up the take.
std::vector<Cmd> CommandsForFlyEvents(const fc::Events& events, const dir::Snapshot* snap, const TakeForm& take);

// ---------------------------------------------------------------------------------------------------------------- the transport

Cmd PlayCmd();
Cmd PauseCmd();
Cmd RestartCmd(double t);
Cmd SeekCmd(double t);
Cmd StepCmd(int frames);
Cmd FixedModeCmd(bool fixed);
Cmd CameraModeCmd(const std::string& mode);  // "scene" | "freecam" | "game"
Cmd CameraSetCmd(const dir::SnapPose& pose);
Cmd LookAtCmd(const std::string& target);
// Space: pause when it plays, play when it is held.
Cmd TogglePlayCmd(const dir::SnapTransport& transport);

// ---------------------------------------------------------------------------------------------------------------- small things the drawing asks

// The scene time a step of one frame is (the fixed step when there is one, else the header's fps, else 1/60).
double FrameStep(const dir::SnapTransport& transport);
// Words for the transport bar's state: "RUNNING", "PAUSED", "FROZEN ..." (what the world is doing).
std::string TransportStateText(const dir::SnapTransport& transport);
// A short description of the snapshot's camera mode for the bar: "FREE CAMERA", "SCENE CAMERA", "GAME CAMERA".
std::string CameraModeText(const std::string& mode);
// The cars' ids of the snapshot for the rig and path pickers ("player" first).
std::vector<std::string> CarIds(const dir::Snapshot& snap);

// ---------------------------------------------------------------------------------------------------------------- the pointer

// THE POINTER IS THE PANEL'S while the panel is up and showing (Tab hides it for a clean look and gives the pointer back: mouse look again). A human with a mouse works with the
// dock AND the picture (a click on the ground, a car dragged over it, the right button's look), so the pointer has to be free and visible everywhere, not only over the dock: the
// game's mouse look (relative mouse mode: the pointer hidden and locked, motion read raw, so no pointer position ever reached ImGui: the first mouse run, 2026-11-20, 14 of 15 checks)
// and the idle hide of the pointer let go of it while this is true (imgui_theme.h SetPointerHeldByPanel, read by mouse_look.cpp and cursor_hide.cpp).
inline bool HoldsPointer(bool open, bool hidden) {
    return open && !hidden;
}

// ---------------------------------------------------------------------------------------------------------------- what is on show

// A rectangle as min and max corners, in the dock's units.
struct Box {
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
};
// The part of `item` that lies inside `clip` (the clip rect of the window it was drawn in), false when none of it does (or a sliver under a unit). The geometry the panel publishes holds
// only what a click can reach: a widget under the fold of a scrolled child is clipped away and a click on its full rectangle hits nothing (the mouse run of 2026-11-20 clicked "SPAWN HERE"
// below the dock's edge and could not tell why nothing happened).
inline bool VisiblePart(const Box& item, const Box& clip, Box* out) {
    Box b;
    b.x0 = item.x0 > clip.x0 ? item.x0 : clip.x0;
    b.y0 = item.y0 > clip.y0 ? item.y0 : clip.y0;
    b.x1 = item.x1 < clip.x1 ? item.x1 : clip.x1;
    b.y1 = item.y1 < clip.y1 ? item.y1 : clip.y1;
    if (b.x1 - b.x0 < 1.0f || b.y1 - b.y0 < 1.0f) return false;
    if (out) *out = b;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------- the dock's size

// THE DOCK FOLLOWS THE WINDOW. Everything the panel draws is in ImGui's own space, io.DisplaySize: the window's LOGICAL size (the SDK's drawer sets it to the physical size times
// medium DPI over DPI: 853x480 for a 1280x720 window at 150 %), mapped by the overlay onto the whole frame. The font, the paddings and the dock's height are worked out from that
// size here, so a small window gets a small dock (a third of the picture, not half of it) and a 4K one a big one. A first run's dock was a fixed 250 to 400 units tall with a fixed 17
// unit font: half of an 853x480 window.
enum class DockSize : uint8_t { kSmall, kNormal, kTall };  // SIZE button: kSmall = no taller than it has to be, kNormal about a third, kTall about half of the window
const char* DockSizeName(DockSize size);
DockSize NextDockSize(DockSize size);  // small -> normal -> tall -> small

struct DockPlan {
    float scale = 1.0f;              // 1.0 at a display 900 units tall; 17 units of font
    float font = 17.0f;
    float pad_x = 0.0f, pad_y = 0.0f;      // the dock window's padding
    float frame_x = 0.0f, frame_y = 0.0f;  // a button's padding
    float item_x = 0.0f, item_y = 0.0f;    // the space between widgets
    float row = 0.0f;                // one row of buttons, spacing included
    float footer = 0.0f;             // the "what I did" line
    float height = 0.0f;             // the dock's height; with the bar only when collapsed
    float min_height = 0.0f;         // the least a full dock needs for its bar, a few rows of inspector and the footer
    float timeline_fraction = 0.6f;  // of the dock's width
    float fraction_of_window = 0.0f;
};
// `bar_height`: what the transport bar measured last frame (it wraps to a second row when the window is narrow); 0 or less means two rows. `size_scale`: the player's own size
// factor on top of the window's (1.0), for a 4K window that wants bigger.
DockPlan PlanDock(float display_w, float display_h, DockSize size, bool collapsed, float bar_height, float size_scale = 1.0f);
// A pixel length given for the reference font (17 units) at this plan's size.
inline float Scaled(const DockPlan& plan, float reference_px) {
    return reference_px * plan.scale;
}

// The tab strip: the inspector's tabs in a row that scrolls when they do not fit, with an arrow at each end.
struct TabStrip {
    int first = 0;           // the first tab shown
    int last = -1;           // the last tab shown (-1: none)
    bool overflow = false;   // some tab is out of view: the arrows are drawn
    bool can_left = false;   // a tab is hidden on the left (the left arrow works)
    bool can_right = false;  // a tab is hidden on the right
};
// `widths` of the tabs, `gap` between two, `avail` the strip's width, `arrow_w` one arrow button (two are kept free, with a gap each, when the tabs overflow). `first`: the strip's
// scroll, the first tab it showed. `ensure`: a tab that must be in view (the selected one; -1 none). The strip never leaves free room at the right while a tab is hidden at the left.
TabStrip LayoutTabStrip(const std::vector<float>& widths, float gap, float avail, float arrow_w, int first, int ensure);
// An arrow clicked: the strip's new `first` (direction -1 left, +1 right; unchanged when there is nothing to scroll to).
int ScrollTabStrip(const std::vector<float>& widths, float gap, float avail, float arrow_w, int first, int direction);

}  // namespace mcla::ui::director
