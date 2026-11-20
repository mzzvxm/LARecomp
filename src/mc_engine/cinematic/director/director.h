#pragma once
//
// The Director's command layer (DIRECTOR-DESIGN.md section 2; documentation/DIRECTOR-PROTOCOL.md is the wire's reference). FROZEN: the names and the
// shapes of this header and of the wire format are what the verbs (B), the world queries (C), the tools (D) and the panel (E) build against; add to
// it, never change a name or a shape.
//
// ONE command layer. A panel button, a scene-file line and an agent's request are the same thing: a named command with JSON arguments that is
// answered with a JSON result or a coded error. The panel posts commands (Post), an agent sends them over loopback TCP (line_server.h), a
// handler runs them on the game thread at an update boundary (Tick, called from Patch_DeltaTimePre after TickScene and before TickCars), and
// the answer goes back to whoever sent it. The game never calls the panel: it publishes a Snapshot every update, and the panel reads that.
//
//   panel (UI thread) --\                              /--> Tick() on the game thread: drains <= kMaxCommandsPerTick commands within
//   agent (TCP IO thread) ---> command queue (mutex) ---<     kTickBudgetMs, runs each handler, polls the deferred replies, publishes the
//   console line in the panel -/                         \--> snapshot, and hands replies and events to their sender / listeners
//
// THREADS. Handlers and snapshot contributors run on the GAME thread only (inside Tick). Post, PublishEvent, GetSnapshot, AddEventListener
// and the reply callbacks' registration are safe from any thread. A reply callback (Posted::on_reply) and an event listener run on the thread
// that finishes the reply or publishes the event (the game thread for a reply; any thread for an event): they must be quick, take their own
// lock and never call back into Tick. ImGui is touched only in the panel's own OnDraw on the UI thread (the 0.11 ImGuiHost resets its context
// after each UI frame): nothing here calls it, and a panel must copy the snapshot, not hold game memory.
//
// INERT unless the debug setting mcla_director is on. Off, Tick costs one setting read, Post fails, GetSnapshot is null.
//
// Everything in this header is pure C++ over the standard library (no game, no SDK): the unit test larecomp_director_test builds it alone.
//

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "error_code.h"
#include "json.h"

namespace mc::cinematic::director {

inline constexpr int kProtocolVersion = 1;
inline constexpr int kMaxCommandsPerTick = 16;     // handlers run per update ...
inline constexpr double kTickBudgetMs = 2.0;       // ... and within this much time; the rest wait for the next update
inline constexpr size_t kMaxClients = 4;           // agents connected at once
inline constexpr double kDefaultTimeoutS = 30.0;   // a deferred reply that is not finished in this long becomes a `timeout` error

// ---------------------------------------------------------------------------------------------------------------- requests and replies

// Where a command came from (for the panel's audit line and the permissions: only an agent's paths are held to the write root and token).
enum class Source : uint8_t { kAgent, kPanel, kConsole, kInternal };
const char* SourceName(Source source);

struct Request {
    uint64_t seq = 0;           // the order the Director took it in (1, 2, ...)
    int64_t id = 0;             // the caller's id; meaningful when has_id
    bool has_id = false;        // a request without an id gets a reply without one
    std::string cmd;            // "camera.set"
    Json args = Json::Object(); // always an object ({} when the caller sent none)
    Source source = Source::kInternal;
    int client = -1;            // the agent's connection number (0..kMaxClients-1), -1 for the panel and internal
    bool wait_settled = false;  // "wait":"settled": a handler that can wait (seek, shot, car.spawn, scene.load, ...) answers when the world has settled
    double timeout_s = kDefaultTimeoutS;
};

// What the caller gets back. `ToJson` is the wire form: {"id":7,"ok":true,"result":{...}} or {"id":8,"ok":false,"error":{"code":"...","msg":"...",...}}.
struct Reply {
    int64_t id = 0;
    bool has_id = false;
    bool ok = true;
    Json result = Json::Object();  // ok
    ErrorCode code = ErrorCode::kFailed;  // !ok
    std::string message;
    Json details;  // !ok: optional extra fields of the error object (a null value adds none; an object's members are merged into "error")
    Json ToJson() const;
};

namespace detail {
struct ReplySlot;
}

// The handler's way to answer. A copyable handle on one reply: the first Done / Fail wins, later ones are ignored.
//   Done(result)            the reply is ok with this result (an object; default {})
//   Fail(code, msg[, ...])  the reply is an error
//   Defer(poll)             the answer is not known yet. `poll` is called on the game thread once every update, after the update's commands, until it
//                           has called Done or Fail on the handle it is given; if the request's timeout_s passes first the reply is a `timeout` error
//                           ("wait":"settled" requests set the timeout; the default is kDefaultTimeoutS). A handler that Defers returns at once.
// A handler that neither answers nor defers is answered with a `failed` error (a bug of the handler: "no reply").
class Responder {
  public:
    Responder() = default;
    void Done(Json result = Json::Object()) const;
    void Fail(ErrorCode code, std::string_view message, Json details = Json()) const;
    void Defer(std::function<void(const Responder&)> poll) const;
    bool finished() const;
    bool deferred() const;
    const Request& request() const;
    explicit operator bool() const { return slot_ != nullptr; }

  private:
    friend class Engine;
    explicit Responder(std::shared_ptr<detail::ReplySlot> slot) : slot_(std::move(slot)) {}
    std::shared_ptr<detail::ReplySlot> slot_;
};

using Handler = std::function<void(const Request&, const Responder&)>;

// ---------------------------------------------------------------------------------------------------------------- the registry

// The type an argument is checked against before the handler runs (a wrong one is a `bad_args` reply that names the argument).
//   kAny      anything            kBool, kNumber, kString, kArray, kObject
//   kInt      a number with no fractional part
//   kVec3     [x, y, z], three finite numbers
//   kTarget   a string ("car:p1", "player", "spot:gate@0,2,-6") or a kVec3: what a camera looks at
enum class ArgType : uint8_t { kAny, kBool, kNumber, kInt, kString, kArray, kObject, kVec3, kTarget };
const char* ArgTypeName(ArgType type);

struct ArgSpec {
    std::string name;
    ArgType type = ArgType::kAny;
    bool required = false;
    std::string doc;  // one line, with the default when there is one ("default 20")
};

// What `help` says about a command, and what the registry checks.
struct Schema {
    std::string group;    // "transport", "scene", "camera", "cars", "world", "query", "capture", "ui", "core"
    std::string summary;  // one line
    std::vector<ArgSpec> args;
    std::string result;   // one line: the shape of the result's fields
    bool file = false;    // [file]: the command also writes the scene file (a line is inserted / replaced)
    bool open_args = false;  // arguments beyond the list are passed through unchecked (otherwise an unknown one is `bad_args`)
};

// Adds a command. The name is lower case, words joined by dots ("camera.key.add"). False when the name is taken or empty. Safe to call during static
// initialisation (a file-scope CommandRegistrar): the registry is built on first use. The handler runs on the game thread.
bool RegisterCommand(std::string_view name, Handler handler, Schema schema);

struct CommandRegistrar {
    CommandRegistrar(std::string_view name, Handler handler, Schema schema) { RegisterCommand(name, std::move(handler), std::move(schema)); }
};

// Validates `args` (an object) against a command's schema: "" when fine, else the bad_args message ("camera.set: 'fov' must be a number"). Missing
// required arguments, wrong types, and unknown arguments unless the schema says open_args. Pure.
std::string ValidateArgs(const Schema& schema, const Json& args, std::string_view cmd = {});

// ---------------------------------------------------------------------------------------------------------------- posting

// A command for the Director, from any thread. `on_reply` (may be empty) is called with the reply when the command is finished (the game thread,
// inside Tick). The queue holds kMaxQueued commands; a full queue or an off Director makes Post return false and call nothing.
struct Posted {
    std::string cmd;
    Json args = Json::Object();
    Source source = Source::kPanel;
    int client = -1;
    int64_t id = 0;
    bool has_id = false;
    bool wait_settled = false;
    double timeout_s = 0.0;  // 0 = kDefaultTimeoutS
    std::function<void(const Reply&)> on_reply;
};
inline constexpr size_t kMaxQueued = 256;
bool Post(Posted command);

// The console grammar of the panel's command line (the same commands, no JSON braces): `camera.set pos=819.6,19,-800 yaw=80 fov=40`,
// `transport.seek 3.2` (a bare word is the next argument of the schema in order), `car.spawn id=p1 model=vp_lam_murcielago_06 at=[1,2,3]`.
// A value is: a number, true / false / null, a JSON array / object / quoted string, a comma list of numbers (an array: 1,2,3), else a bare string.
bool ParseConsoleLine(std::string_view line, std::string* cmd, Json* args, std::string* error);
// The inverse: the line a panel action prints in its "what I did" strip ("camera.set pos=819.6,19,-800 yaw=80").
std::string FormatConsoleLine(std::string_view cmd, const Json& args);

// ---------------------------------------------------------------------------------------------------------------- events

// An event goes to every authenticated agent's connection as {"event":name, ...fields} (no id) and to every listener. Any thread. The events
// of the protocol are in documentation/DIRECTOR-PROTOCOL.md: scene.reloaded, scene.state, key.reached, car.ready, shot.done, take.done, edit,
// agent (and take.progress). A handler may publish its own; name them "group.what".
void PublishEvent(std::string_view name, Json fields = Json::Object());
using EventListener = std::function<void(std::string_view name, const Json& event)>;  // `event` is the whole object, "event" member included
int AddEventListener(EventListener listener);  // a token for RemoveEventListener
void RemoveEventListener(int token);

// ---------------------------------------------------------------------------------------------------------------- the snapshot

// What the panel and the IO thread read. Built on the game thread every update and published by an atomic swap of a shared_ptr: copy the pointer,
// read it, drop it. The values are the game's as of the end of the last Tick. Frozen; fields are only ever added.
enum class EventKind : uint8_t { kCamera, kCut, kCar, kWorld, kHud, kCapture, kSplit, kLabel, kMark, kSetting, kOther };
const char* EventKindName(EventKind kind);

struct SnapEvent {  // a key of the scene, for the timeline rows
    int line = 0;       // the line in the scene file (1-based)
    double t = 0.0;     // scene time, seconds
    std::string track;  // scene.h TrackName: "camera", "cut", "car", "hour", ...
    std::string text;   // the line's statement as written, short (comment cut)
    EventKind kind = EventKind::kOther;
    std::string car;    // kCar: the car id
};
struct SnapDiag {  // what the parser said about the file
    int line = 0;
    bool error = false;
    std::string message;
};
struct SnapPose {
    double pos[3] = {0, 0, 0};
    double yaw = 0, pitch = 0, roll = 0;  // degrees, the scene file's meaning (yaw 0 looks along +Z, 90 along +X; pitch + looks up)
    double fov = 50;                      // vertical degrees
};
struct SnapCamera {
    bool valid = false;           // a camera was seen this update
    std::string mode = "game";    // "scene" | "freecam" | "game": whose camera is the final one
    SnapPose pose;
    double rows[3][3] = {};       // right, up, BACK (the game's slot rows)
    double view_proj[16] = {};    // row-major world -> clip, the camera's own aspect and planes (Direct3D clip: z in 0..w); the panel's gizmos project with it
    double aspect = 16.0 / 9.0;   // the picture's width over its height (display aspect: ultrawide aware)
    double near_plane = 0.1, far_plane = 10000.0;
};
struct SnapCar {
    std::string id;          // "player", "p1", ...
    std::string model;
    double pos[3] = {0, 0, 0};
    double heading = 0;      // degrees, the scene's convention (0 along +Z, 90 along +X)
    double speed = 0;        // metres a second, from the last two poses
    bool ready = false;      // the pose is readable and the car is made
    bool on_path = false;
    std::string route;       // the route file, when on_path
};
struct SnapRoute {
    std::string name;
    std::string car;
    bool kinematic = true;
    std::vector<std::vector<double>> points;  // [x, y, z, speed] each (metres, metres a second)
};
struct SnapSpot {
    std::string name;
    double pos[3] = {0, 0, 0};
    double heading = 0;
    std::string text;
};
struct SnapCamSample {  // the scene camera's path, sampled
    double t = 0;
    double pos[3] = {0, 0, 0};
    bool cut = false;
};
struct SnapCheck {  // "checks of the current key" (the storyboard checks: subject in frame, not occluded, ...); filled by whoever computes them
    std::string name;
    bool ok = true;
    std::string text;
};
struct SnapAgent {
    int client = 0;
    std::string name;      // what the agent called itself in `auth` (or "agent N")
    std::string last_cmd;  // the last command it sent, as the console line
    double last_cmd_age_s = 0;
    uint64_t commands = 0;
};
struct SnapTransport {
    std::string state = "off";  // the scene runner's: off, pending, waiting, running, finished, failed
    double t = 0;               // scene time, seconds
    double duration = -1;       // the scene's `end`, -1 = none
    double fps = 0;             // the scene's header fps, 0 = not given
    double step = 0;            // the fixed step in seconds when one is in force, else 0
    bool paused = false;        // the transport holds the clock (pause, or a seek)
    bool frozen = false;        // the world is held at the time scale floor
    uint64_t frame = 0;         // updates the Director has seen
    std::string mode = "realtime";  // "fixed" when a fixed step is in force
    bool scrubbable = true;     // a seek is exact: no brain car, traffic or peds in the scene
    std::string scrub_hint;     // why not
    bool holding_for_ready = false;  // before t = 0 the clock waits for the cars and the streaming
};
struct Snapshot {
    uint64_t serial = 0;  // counts publishes
    bool director = false;
    int64_t rev = 0;      // the scene file's revision (every change of the text, by anyone, counts one)
    std::string scene_path, scene_name, scene_hash;
    SnapTransport transport;
    std::vector<SnapEvent> events;
    std::vector<SnapDiag> diagnostics;
    SnapCamera camera;
    std::vector<SnapCar> cars;
    std::vector<SnapRoute> routes;
    std::vector<SnapSpot> spots;
    std::vector<SnapCamSample> cam_path;
    std::vector<SnapCheck> checks;
    std::vector<SnapAgent> agents;
    int selected_line = 0;           // ui.select: the line an agent pointed at (0 = none)
    std::string ui_request;          // ui.open: the panel an agent asked for ("director", "timeline", ...); the panel acts when ui_request_serial changes
    uint64_t ui_request_serial = 0;
    bool take_active = false;
    std::string take_name;
    uint64_t take_frames = 0;
    bool can_undo = false, can_redo = false;
    std::string last_edit_by;        // "panel", "agent", "file", ... who changed the scene last
    uint64_t proposal_serial = 0;    // scene.propose: counts proposals; the panel offers the one in `proposal_path` (empty = none pending)
    std::string proposal_path, proposal_note;
};

// The newest snapshot, or null before the first (and while the Director is off). Any thread.
std::shared_ptr<const Snapshot> GetSnapshot();
// A function that adds to the snapshot while it is built (the game thread, once an update, after the Director's own fields are filled): spots from
// spots.txt, camera-path samples, storyboard checks. Plain function pointers, added during static initialisation or at start; not removed.
using SnapshotContributor = void (*)(Snapshot&);
void AddSnapshotContributor(SnapshotContributor contributor);

// The commands the panel and the agents ran lately, newest last (the "what I did" strip and the agents' audit line). Any thread.
struct LoggedCommand {
    uint64_t seq = 0;
    Source source = Source::kInternal;
    int client = -1;
    std::string line;  // FormatConsoleLine
    bool ok = true;
    std::string error;  // code and message when !ok
};
std::vector<LoggedCommand> RecentCommands(size_t max_count = 20);

// ---------------------------------------------------------------------------------------------------------------- the listener and the game side

struct ListenerInfo {
    bool running = false;
    uint16_t port = 0;
    std::string token;      // the 128-bit token of this run (hex): what `auth` wants
    std::string info_path;  // director.json, when mcla_director_info named one
};
ListenerInfo GetListenerInfo();

// Once per game update on the game thread (Patch_DeltaTimePre, after TickScene, before TickCars): starts or stops the listener when mcla_director
// changes, runs the queued commands, polls the deferred replies, publishes the snapshot and the events. One setting read when the Director is off.
void Tick();

// Whether the world's streaming is done (the City_DoneStreaming native, polled at most every few updates); *known is false when this build cannot
// ask (no guest thread yet), and the answer then is true so a wait does not hang. Game thread only.
bool StreamingDone(bool* known = nullptr);

}  // namespace mc::cinematic::director
