#ifndef REXGLUE_HAS_XEO3_TARGET
//
// The Director on the game thread (DIRECTOR-DESIGN.md section 2): the tick that runs the queued commands, the scene document (the text, its revision, undo), the
// observer the scene runner tells about loads and keys, the snapshot the panel reads, the listener for agents, the camera as the hook sees it, and the few commands
// that belong to no group (log.tail, ui.open, ui.select, quit).
//
// Everything is inert unless the debug setting mcla_director is on: Tick reads the setting and returns. Switching it on starts the listener (127.0.0.1, a random
// port, a token written to mcla_director_info), installs the runner's observer, the ready gate and the in-place reload; switching it off puts everything back.
//
// Written without a build or a run in the game; CHECK marks what rests on reading the code only (documentation/DIRECTOR-PROTOCOL.md, the CHECK list of the commit).
//

#include <rex/app/app.h>
#include <rex/logging/logger.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "../camera.h"
#include "../capture_core.h"
#include "../capture_mode.h"
#include "../cars.h"
#include "../freecam.h"
#include "../freecam_runtime.h"
#include "../native_call.h"
#include "../scene_runtime.h"
#include "command.h"
#include "director_game.h"
#include "director_internal.h"
#include "director_math.h"
#include "larecomp_log.h"
#include "larecomp_version.h"
#include "line_server.h"
#include "mc_engine/display/ultrawide.h"
#include "settings/larecomp_settings.h"

namespace mc::cinematic::director {

namespace cam = mc::cinematic::camera;
namespace fc = mc::cinematic::freecam;

namespace {

// sub_822C0320's camera slots (camera_runtime.cpp has the same list) and the pause flag.
constexpr uint32_t kSlotFov = 0x50;
constexpr uint32_t kSlotFar = 0x54;
constexpr uint32_t kSlotNear = 0x58;
constexpr uint32_t kSlotRow0 = 0x60;
constexpr uint32_t kSlotRow1 = 0x70;
constexpr uint32_t kSlotRow2 = 0x80;
constexpr uint32_t kSlotPos = 0x90;
constexpr uint32_t kPauseStatePtr = 0x828CCEA0;
constexpr uint32_t kModeMenu = 6;
constexpr uint32_t kCityDoneStreaming = 0x822C89D8;  // City_DoneStreaming (feature-parity/natives/natives.tsv)

uint32_t R32(const uint8_t* base, uint32_t ea) {
    return (uint32_t(base[ea + 0]) << 24) | (uint32_t(base[ea + 1]) << 16) | (uint32_t(base[ea + 2]) << 8) | uint32_t(base[ea + 3]);
}
float RF(const uint8_t* base, uint32_t ea) {
    const uint32_t v = R32(base, ea);
    float f;
    std::memcpy(&f, &v, sizeof(f));
    return f;
}
bool GamePaused(const uint8_t* base) {
    const uint32_t p = R32(base, kPauseStatePtr);
    return p != 0 && base[p + 4] != 0 && base[p + 10] == 0;
}

// The scene camera's own rig, evaluated the way camera_runtime.cpp's hook does, to know what the scene's camera shows (see NoteCameraHook).
class DirectorCars final : public cam::CarSource {
  public:
    bool Get(const std::string& id, cam::CarPose* out) override {
        CarPose p;
        if (!GetCarPose(id, &p)) return false;
        out->position = {p.position[0], p.position[1], p.position[2]};
        out->frame.right = {p.right[0], p.right[1], p.right[2]};
        out->frame.up = {p.up[0], p.up[1], p.up[2]};
        out->frame.forward = {p.forward[0], p.forward[1], p.forward[2]};
        return true;
    }
};

struct CamState {
    internal::CameraView view;
    cam::CameraTrack track;
    bool track_ok = false;
    cam::Rig rig;
    uint64_t track_epoch = ~0ull;
    double last_t = -1.0;
    uint64_t last_updates = ~0ull;
    cam::View last_view;
    double keep_yaw = 0.0;
};

struct TakeInfoSeen {
    uint64_t finished = 0;
};

struct State {
    bool on = false;
    uint64_t tick = 0;
    uint64_t serial = 0;
    uint64_t scene_epoch = 0;
    std::unique_ptr<LineServer> server;
    std::string info_path;
    std::string token;
    uint16_t port = 0;

    std::mutex hello_mutex;
    std::string hello_scene;

    SceneState prev_state = SceneState::kOff;
    std::map<std::string, bool> car_ready;
    struct PoseAt {
        double x = 0, y = 0, z = 0, t = 0;
        double speed = 0;
    };
    std::map<std::string, PoseAt> poses;
    CamState cam;
    std::vector<SnapRoute> routes;
    int64_t routes_rev = -1;
    size_t routes_count = ~size_t(0);
    int selected_line = 0;
    std::string ui_request;
    uint64_t ui_serial = 0;
    TakeInfoSeen take_seen;

    bool streaming_known = false;
    bool streaming_done = true;
    uint64_t streaming_tick = 0;

    rex::log::LogCaptureSink log_sink;
    rex::ScopeGuard log_guard;
    int64_t started_unix = 0;
};
State& S() {
    static State* s = new State();  // never destroyed: the listener thread and the engine's handlers outlive static destruction
    return *s;
}

int64_t UnixNow() {
    return static_cast<int64_t>(std::time(nullptr));
}

std::string IsoUtc(int64_t unix_s) {
    const std::time_t t = static_cast<std::time_t>(unix_s);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[40];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------- the public face

bool Enabled() {
    return GlobalEngine().enabled();
}

ListenerInfo GetListenerInfo() {
    ListenerInfo i;
    State& s = S();
    i.running = s.server && s.server->running();
    i.port = s.port;
    i.token = s.token;
    i.info_path = s.info_path;
    return i;
}

bool StreamingDone(bool* known) {
    State& s = S();
    if (s.streaming_tick == 0 || s.tick - s.streaming_tick >= 3) {
        s.streaming_tick = s.tick ? s.tick : 1;
        GameStateSample gs;
        if (SampleGameState(&gs) && gs.world_ready) {
            const NativeResult r = CallNative(kCityDoneStreaming, {}, false);
            s.streaming_known = r.called && r.ret_written;
            s.streaming_done = s.streaming_known ? (r.ret & 0xFF) != 0 : true;
        } else {
            s.streaming_known = false;
            s.streaming_done = true;
        }
    }
    if (known) *known = s.streaming_known;
    return s.streaming_done;
}

// ---------------------------------------------------------------------------------------------------------------- the camera as the hook sees it

void NoteCameraHook(uint8_t* base, uint32_t sp, uint32_t mode, CameraSource who) {
    State& s = S();
    CamState& cs = s.cam;
    internal::CameraView v;
    v.valid = true;
    v.aspect = mc::ultrawide::DisplayAspect();
    const auto vec = [&](uint32_t off) { return cam::Vec3{RF(base, sp + off + 0), RF(base, sp + off + 4), RF(base, sp + off + 8)}; };
    const cam::SlotRows rows = {vec(kSlotRow0), vec(kSlotRow1), vec(kSlotRow2)};
    const cam::Vec3 slot_pos = vec(kSlotPos);
    double fov = RF(base, sp + kSlotFov);
    double near_plane = RF(base, sp + kSlotNear);
    double far_plane = RF(base, sp + kSlotFar);
    cam::Vec3 pos = slot_pos;
    cam::Basis basis = cam::FromSlotRows(rows);
    v.mode = who == CameraSource::kFreecam ? "freecam" : "game";
    if (who == CameraSource::kNone) {
        // The slots hold the game's own camera and the scene's camera is written after this hook (camera_runtime.cpp): the Director evaluates the scene's camera
        // track the way that hook will. CHECK: the two agree when the scene's camera has no follow lag; with a lag the shadow rig's springs have seen the same
        // times, so they agree too, but an exact reading needs one call in camera_runtime.cpp after WriteCamera.
        const Scene* scene = LoadedScene();
        if (cs.track_epoch != s.scene_epoch) {
            cs.track_epoch = s.scene_epoch;
            cs.track_ok = false;
            cs.rig.Reset();
            cs.last_t = -1.0;
            cs.last_updates = ~0ull;
            if (scene) {
                cam::BuildResult r = cam::BuildTrack(*scene);
                if (r.Ok() && !r.track.Empty()) {
                    cs.track = std::move(r.track);
                    cs.track_ok = true;
                }
            }
        }
        if (scene && cs.track_ok && CurrentSceneState() == SceneState::kRunning && !GamePaused(base) && mode != kModeMenu) {
            const SceneTransport tr = GetSceneTransport();
            if (tr.t != cs.last_t || tr.updates != cs.last_updates) {
                DirectorCars cars;
                cs.last_view = cs.rig.Evaluate(cs.track, tr.t, cars);
                cs.last_t = tr.t;
                cs.last_updates = tr.updates;
            }
            if (cs.last_view.active) {
                v.mode = "scene";
                pos = cs.last_view.position;
                basis = cs.last_view.basis;
                if (cs.last_view.fov > 0.0) fov = cs.last_view.fov;
                if (cs.last_view.near_plane > 0.0) near_plane = cs.last_view.near_plane;
                if (cs.last_view.far_plane > 0.0) far_plane = cs.last_view.far_plane;
            }
        }
    }
    if (!std::isfinite(fov) || fov < 1.0 || fov > 170.0) fov = 50.0;
    if (!std::isfinite(near_plane) || near_plane <= 0.0) near_plane = 0.1;
    if (!std::isfinite(far_plane) || far_plane <= near_plane) far_plane = 10000.0;
    double yaw = 0, pitch = 0, roll = 0;
    fc::AnglesFromBasis(basis, cs.keep_yaw, &yaw, &pitch, &roll);
    cs.keep_yaw = yaw;
    v.pose.pos[0] = pos.x, v.pose.pos[1] = pos.y, v.pose.pos[2] = pos.z;
    v.pose.yaw = yaw;
    v.pose.pitch = pitch;
    v.pose.roll = roll;
    v.pose.fov = fov;
    const cam::SlotRows out_rows = cam::ToSlotRows(basis);
    const cam::Vec3 r[3] = {out_rows.right, out_rows.up, out_rows.back};
    for (int i = 0; i < 3; ++i) v.rows[i][0] = r[i].x, v.rows[i][1] = r[i].y, v.rows[i][2] = r[i].z;
    v.near_plane = near_plane;
    v.far_plane = far_plane;
    cs.view = v;
}

namespace internal {

CameraView CurrentCamera() {
    return S().cam.view;
}

// ---------------------------------------------------------------------------------------------------------------- files and paths

std::string Utf8(const std::filesystem::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::filesystem::path FromUtf8(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

bool ReadTextFile(const std::filesystem::path& path, std::string* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out->assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return true;
}

bool WriteTextFileAtomic(const std::filesystem::path& path, const std::string& text, std::string* error) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            if (error) *error = "cannot write " + Utf8(path);
            return false;
        }
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.flush();
        if (!f) {
            std::filesystem::remove(tmp, ec);
            if (error) *error = "the disk did not take " + Utf8(path);
            return false;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        std::error_code ec2;
        std::filesystem::rename(tmp, path, ec2);
        if (ec2) {
            std::filesystem::remove(tmp, ec);
            if (error) *error = "cannot replace " + Utf8(path) + ": " + ec2.message();
            return false;
        }
    }
    return true;
}

std::filesystem::path WriteRoot() {
    std::error_code ec;
    const std::string root = larecomp::settings::mcla_director_root.Get();
    std::filesystem::path p = root.empty() ? std::filesystem::current_path(ec) : std::filesystem::absolute(FromUtf8(root), ec);
    return p;
}

std::filesystem::path SceneFolder() {
    SceneDoc& d = Doc();
    if (d.path.empty()) return {};
    std::error_code ec;
    return std::filesystem::absolute(FromUtf8(d.path), ec).parent_path();
}

bool ResolveWritePath(std::string_view user, bool under_scene, std::filesystem::path* out, std::string* error) {
    const PathCheck c = CheckRelativePath(user);
    if (!c.ok) {
        *error = "path '" + std::string(user) + "': " + c.error;
        return false;
    }
    const std::filesystem::path base = under_scene ? SceneFolder() : WriteRoot();
    if (base.empty()) {
        *error = "there is no folder to write under (no scene is loaded)";
        return false;
    }
    std::error_code ec;
    const std::filesystem::path full = base / FromUtf8(c.normalised);
    // A link inside the folder must not lead out of it.
    const std::filesystem::path canon_base = std::filesystem::weakly_canonical(base, ec);
    const std::filesystem::path canon_full = std::filesystem::weakly_canonical(full, ec);
    const auto rel = canon_full.lexically_relative(canon_base);
    if (ec || rel.empty() || *rel.begin() == "..") {
        *error = "path '" + std::string(user) + "' leaves " + Utf8(base);
        return false;
    }
    *out = full;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------- the scene document

SceneDoc& Doc() {
    static SceneDoc* d = new SceneDoc();
    return *d;
}

void RefreshDocCaches(SceneDoc& doc) {
    const ParseResult r = ParseScene(doc.text);
    doc.name = r.scene.name;
    doc.fps = r.scene.fps;
    doc.duration = r.scene.Duration();
    doc.events = TimelineEvents(r.scene, doc.text);
    doc.car_ids = SceneCarIds(r.scene);
    doc.diagnostics.clear();
    for (const Diagnostic& d : r.diagnostics) {
        SnapDiag s;
        s.line = d.line;
        s.error = d.severity == Severity::kError;
        s.message = d.message;
        doc.diagnostics.push_back(std::move(s));
    }
}

static void SetDocText(SceneDoc& d, const std::string& text) {
    d.text = text;
    d.hash = TextHash(text);
    RefreshDocCaches(d);
}

static void NoteHello() {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.hello_mutex);
    s.hello_scene = Doc().path;
}

void EnsureDoc() {
    SceneDoc& d = Doc();
    const std::string path = SceneFilePath();
    if (path.empty() || path == d.path) return;
    d = SceneDoc{};
    d.path = path;
    std::string text;
    if (ReadTextFile(FromUtf8(path), &text)) SetDocText(d, text);
    d.rev = 1;
    d.last_by = "load";
    ++S().scene_epoch;
    NoteHello();
}

// A scene text was loaded by the runner (from the file, or the Director's own text).
static void OnLoaded(const std::string& path, const std::string& text, bool ok, int errors, bool soft, const char* by) {
    State& s = S();
    SceneDoc& d = Doc();
    ++s.scene_epoch;
    Json f = Json::Object();
    if (path != d.path) {  // another scene: a fresh document
        d = SceneDoc{};
        d.path = path;
        SetDocText(d, text);
        d.rev = 1;
        d.last_by = by;
        NoteHello();
        f.Set("rev", d.rev);
        f.Set("by", by);
        f.Set("changed", Json::Array());
        f.Set("errors", errors);
        f.Set("soft", false);
        f.Set("path", path);
        PublishEvent("scene.reloaded", std::move(f));
        return;
    }
    const std::string h = TextHash(text);
    Json changed = Json::Array();
    if (h == d.hash) {  // the Director's own write: the revision is already counted
        for (int line : d.pending_changed) changed.Push(line);
        d.pending_changed.clear();
    } else {  // a person saved the file (or another program wrote it)
        d.undo.push_back(d.text);
        d.redo.clear();
        for (int line : ChangedLines(d.text, text)) changed.Push(line);
        SetDocText(d, text);
        ++d.rev;
        d.last_by = by;
    }
    f.Set("rev", d.rev);
    f.Set("by", by);
    f.Set("changed", std::move(changed));
    f.Set("errors", ok ? 0 : errors);
    f.Set("soft", soft);
    PublishEvent("scene.reloaded", std::move(f));
}

static void OnKey(const Event& e, double t) {
    Json f = Json::Object();
    f.Set("t", t);
    f.Set("line", e.line);
    f.Set("track", TrackName(e.track));
    f.Set("text", StatementText(Doc().text, e.line));
    PublishEvent("key.reached", std::move(f));
}

static const SceneObserver kObserver = {&OnLoaded, &OnKey};

static void KeepRevisionFile(const SceneDoc& d, int64_t rev, const std::string& text) {
    // scenes\.director\<name>\rev-N.scene next to the scene (local; git-ignored): the text as it was at that revision. Best effort.
    std::error_code ec;
    const std::filesystem::path scene = FromUtf8(d.path);
    const std::filesystem::path dir = std::filesystem::absolute(scene, ec).parent_path() / ".director" / scene.stem();
    std::filesystem::create_directories(dir, ec);
    std::string err;
    WriteTextFileAtomic(dir / ("rev-" + std::to_string(rev) + ".scene"), text, &err);
}

EditOutcome ReplaceSceneText(const std::string& text, const char* by, bool push_undo) {
    EditOutcome out;
    SceneDoc& d = Doc();
    if (d.path.empty()) {
        out.code = ErrorCode::kNotReady;
        out.message = "no scene is loaded (scene.load first)";
        return out;
    }
    std::string err;
    if (!WriteTextFileAtomic(FromUtf8(d.path), text, &err)) {
        out.code = ErrorCode::kFailed;
        out.message = err;
        return out;
    }
    KeepRevisionFile(d, d.rev, d.text);
    const std::vector<int> changed = ChangedLines(d.text, text);
    if (push_undo) {
        d.undo.push_back(d.text);
        d.redo.clear();
    }
    SetDocText(d, text);
    ++d.rev;
    d.last_by = by;
    d.pending_changed = changed;
    const SceneTextResult lr = SceneLoadText(text, by);
    if (!lr.ok && d.pending_changed == changed) {
        // the runner did not take it (no scene armed, or an error): say so ourselves
        Json f = Json::Object();
        f.Set("rev", d.rev);
        f.Set("by", by);
        Json ch = Json::Array();
        for (int line : changed) ch.Push(line);
        f.Set("changed", std::move(ch));
        f.Set("errors", lr.errors);
        f.Set("soft", false);
        d.pending_changed.clear();
        PublishEvent("scene.reloaded", std::move(f));
    }
    out.ok = true;
    out.result = Json::Object();
    out.result.Set("rev", d.rev);
    out.result.Set("hash", d.hash);
    Json ch = Json::Array();
    for (int line : changed) ch.Push(line);
    out.result.Set("changed", std::move(ch));
    out.result.Set("errors", lr.errors);
    return out;
}

EditOutcome ApplySceneEdit(const std::vector<EditOp>& ops, int64_t base_rev, const char* by, bool allow_errors) {
    EditOutcome out;
    EnsureDoc();
    SceneDoc& d = Doc();
    if (d.path.empty()) {
        out.code = ErrorCode::kNotReady;
        out.message = "no scene is loaded (scene.load first)";
        return out;
    }
    // A person may have saved the file since the runner last looked (it waits 300 ms for a file to settle): take that in as a revision first, so a stale base
    // revision is noticed.
    std::string on_disk;
    if (ReadTextFile(FromUtf8(d.path), &on_disk) && TextHash(on_disk) != d.hash) {
        d.undo.push_back(d.text);
        d.redo.clear();
        d.pending_changed = ChangedLines(d.text, on_disk);
        SetDocText(d, on_disk);
        ++d.rev;
        d.last_by = "file";
        Json f = Json::Object();
        f.Set("rev", d.rev);
        f.Set("by", "file");
        Json ch = Json::Array();
        for (int line : d.pending_changed) ch.Push(line);
        f.Set("changed", std::move(ch));
        f.Set("errors", 0);
        PublishEvent("scene.reloaded", std::move(f));
        d.pending_changed.clear();
    }
    if (base_rev != d.rev) {
        out.code = ErrorCode::kStale;
        out.message = "scene.edit: base_rev " + std::to_string(base_rev) + " is not the current revision " + std::to_string(d.rev) + ": read the scene again";
        out.details = Json::Object();
        out.details.Set("rev", d.rev);
        return out;
    }
    EditResult er = SceneEdit::Apply(d.text, ops, base_rev);
    if (!er.ok) {
        out.code = er.code;
        out.message = er.message;
        out.details = er.details.is_object() ? er.details : Json::Object();
        out.details.Set("op", er.failed_op);
        return out;
    }
    const ParseResult before = ParseScene(d.text);
    const ParseResult after = ParseScene(er.text);
    if (!after.Ok() && !allow_errors && before.Ok()) {
        out.code = ErrorCode::kFailed;
        out.message = "the edit makes the scene not parse (" + std::to_string(after.ErrorCount()) + " error(s)); nothing was written. Pass allow_errors to write it anyway";
        Json diags = Json::Array();
        for (const Diagnostic& dg : after.diagnostics) {
            Json j = Json::Object();
            j.Set("line", dg.line);
            j.Set("severity", dg.severity == Severity::kError ? "error" : "warning");
            j.Set("message", dg.message);
            diags.Push(std::move(j));
        }
        out.details = Json::Object();
        out.details.Set("diagnostics", std::move(diags));
        return out;
    }
    EditOutcome w = ReplaceSceneText(er.text, by, true);
    if (!w.ok) return w;
    w.lines = er.lines;
    Json lines = Json::Array();
    for (int line : er.lines) lines.Push(line);
    w.result.Set("lines", std::move(lines));
    w.result.Set("errors", after.ErrorCount());
    Json opsj = Json::Array();
    for (const EditOp& op : ops) opsj.Push(EditOpToJson(op));
    Json ev = Json::Object();
    ev.Set("rev", d.rev);
    ev.Set("by", by);
    ev.Set("ops", std::move(opsj));
    ev.Set("lines", w.result.Get("lines"));
    PublishEvent("edit", std::move(ev));
    return w;
}

EditOutcome InsertSceneLine(const std::string& text, const char* by) {
    EnsureDoc();
    EditOp op;
    op.kind = EditKind::kInsert;
    op.text = text;
    return ApplySceneEdit({op}, Doc().rev, by, false);
}

// ---------------------------------------------------------------------------------------------------------------- the world

uint64_t TickCount() {
    return S().tick;
}

bool WorldSettled(uint64_t since_tick, int frames, std::string* waiting_for) {
    const auto wait = [&](const std::string& why) {
        if (waiting_for) *waiting_for = why;
        return false;
    };
    if (S().tick < since_tick + static_cast<uint64_t>(std::max(0, frames))) return wait("frames are still being rendered");
    if (CurrentSceneState() != SceneState::kRunning) return wait(std::string("the scene is ") + SceneStateName(CurrentSceneState()));
    std::string cars;
    if (!SceneCarsSettled(&cars)) return wait(cars);
    if (!StreamingDone(nullptr)) return wait("the world's streaming is not done");
    return true;
}

Json TransportJson() {
    const SceneTransport t = GetSceneTransport();
    const Scene* scene = LoadedScene();
    const ScrubInfo si = scene ? ScrubbableAt(*scene, t.t) : ScrubInfo{};
    Json j = Json::Object();
    j.Set("state", SceneStateName(t.state));
    j.Set("t", t.t);
    j.Set("duration", t.duration);
    j.Set("fps", scene ? scene->fps : 0.0);
    const bool fixed = FixedStepActive();
    j.Set("step", fixed ? static_cast<double>(FixedStepTicks()) / 49875000.0 : 0.0);
    j.Set("paused", t.held);
    j.Set("frozen", t.frozen);
    j.Set("frame", S().tick);
    j.Set("mode", fixed ? "fixed" : "realtime");
    j.Set("scrubbable", si.scrubbable);
    j.Set("scrub_hint", si.hint);
    j.Set("holding_for_ready", t.holding_for_ready);
    return j;
}

void RequestUi(const std::string& panel) {
    State& s = S();
    s.ui_request = panel;
    ++s.ui_serial;
}

void SelectLine(int line) {
    S().selected_line = line;
}

bool NeedRunningScene(const Responder& r) {
    const SceneState st = CurrentSceneState();
    if (st == SceneState::kOff || st == SceneState::kPending || st == SceneState::kFailed) {
        r.Fail(ErrorCode::kNotReady, std::string("no scene is running (it is ") + SceneStateName(st) + "): scene.load first");
        return false;
    }
    if (st == SceneState::kWaiting) {
        r.Fail(ErrorCode::kNoWorld, "the scene is waiting for a world (a car on the road): load a save and leave the menus");
        return false;
    }
    return true;
}

}  // namespace internal

// ---------------------------------------------------------------------------------------------------------------- the tick

namespace {

bool ReadyGate() {
    return SceneCarsSettled(nullptr) && StreamingDone(nullptr);
}

void WriteInfoFile(State& s) {
    const std::string path = larecomp::settings::mcla_director_info.Get();
    s.info_path = path;
    if (path.empty()) return;
    Json j = Json::Object();
    j.Set("proto", kProtocolVersion);
#if defined(_WIN32)
    j.Set("pid", static_cast<int>(_getpid()));
#else
    j.Set("pid", static_cast<int>(getpid()));
#endif
    j.Set("port", static_cast<int>(s.port));
    j.Set("token", s.token);
    const std::filesystem::path p = internal::FromUtf8(path);
    std::error_code abs_ec;
    j.Set("slot", internal::Utf8(std::filesystem::absolute(p, abs_ec).parent_path().filename()));
    j.Set("exe", "larecomp");
    j.Set("version", LARECOMP_VERSION_LABEL);
    j.Set("scene", SceneFilePath());
    j.Set("started", IsoUtc(s.started_unix));
    std::string err;
    if (!internal::WriteTextFileAtomic(p, j.Dump() + "\n", &err)) LARECOMP_APP_ERROR("[dir] {}", err);
}

void Start(State& s) {
    Engine& e = GlobalEngine();
    e.SetEnabled(true);
    e.SetPingFields([](Json& j) {
        const SceneTransport t = GetSceneTransport();
        j.Set("frame", S().tick);
        j.Set("t", t.t);
        j.Set("state", SceneStateName(t.state));
    });
    s.token = MakeToken();
    s.server = std::make_unique<LineServer>(e, s.token);
    s.server->SetHello([] {
        State& st = S();
        Json h = Json::Object();
        h.Set("version", LARECOMP_VERSION_LABEL);
#if defined(_WIN32)
        h.Set("pid", static_cast<int>(_getpid()));
#else
        h.Set("pid", static_cast<int>(getpid()));
#endif
        std::string scene;
        {
            std::lock_guard<std::mutex> lock(st.hello_mutex);
            scene = st.hello_scene;
        }
        h.Set("scene", scene);
        h.Set("commands", static_cast<int64_t>(GlobalEngine().Names().size()));
        return h;
    });
    std::string err;
    if (!s.server->Start(0, &err)) {
        LARECOMP_APP_ERROR("[dir] the listener did not start: {}", err);
        s.server.reset();
        e.SetEnabled(false);
        return;
    }
    s.port = s.server->port();
    s.started_unix = UnixNow();
    s.on = true;
    s.log_guard = rex::GetLogger().AddSink(s.log_sink);
    SetSceneObserver(&internal::kObserver);
    SetSceneReadyGate(&ReadyGate);
    SetSceneSoftReload(true);
    internal::EnsureDoc();
    WriteInfoFile(s);
    LARECOMP_APP_INFO("[dir] the Director listens on 127.0.0.1:{} (token in {}); {} commands", s.port, s.info_path.empty() ? std::string("the log only: set mcla_director_info") : s.info_path,
                      e.Names().size());
}

void Stop(State& s) {
    LARECOMP_APP_INFO("[dir] the Director is off");
    s.log_guard.Reset();
    if (s.server) s.server->Stop();
    s.server.reset();
    GlobalEngine().SetEnabled(false);
    SetSceneObserver(nullptr);
    SetSceneReadyGate(nullptr);
    SetSceneSoftReload(false);
    SceneClearWorldOverride();
    SceneHold(false);
    FreecamSetOverride(CameraOverride::kNone);
    CaptureDirectorRequest(0.0, CaptureSpec{});
    if (!s.info_path.empty()) {
        std::error_code ec;
        std::filesystem::remove(internal::FromUtf8(s.info_path), ec);
    }
    s.info_path.clear();
    s.on = false;
    s.port = 0;
    s.token.clear();
}

std::shared_ptr<const Snapshot> BuildSnapshot(State& s, const std::vector<SceneCarInfo>& car_list) {
    auto sn = std::make_shared<Snapshot>();
    internal::SceneDoc& d = internal::Doc();
    sn->serial = ++s.serial;
    sn->director = true;
    sn->rev = d.rev;
    sn->scene_path = d.path;
    sn->scene_name = d.name;
    sn->scene_hash = d.hash;
    const SceneTransport t = GetSceneTransport();
    const Scene* scene = LoadedScene();
    const ScrubInfo si = scene ? ScrubbableAt(*scene, t.t) : ScrubInfo{};
    sn->transport.state = SceneStateName(t.state);
    sn->transport.t = t.t;
    sn->transport.duration = t.duration;
    sn->transport.fps = scene ? scene->fps : 0.0;
    const bool fixed = FixedStepActive();
    sn->transport.step = fixed ? static_cast<double>(FixedStepTicks()) / 49875000.0 : 0.0;
    sn->transport.paused = t.held;
    sn->transport.frozen = t.frozen;
    sn->transport.frame = s.tick;
    sn->transport.mode = fixed ? "fixed" : "realtime";
    sn->transport.scrubbable = si.scrubbable;
    sn->transport.scrub_hint = si.hint;
    sn->transport.holding_for_ready = t.holding_for_ready;
    sn->events = d.events;
    sn->diagnostics = d.diagnostics;

    const internal::CameraView cv = s.cam.view;
    sn->camera.valid = cv.valid;
    sn->camera.mode = cv.mode;
    sn->camera.pose = cv.pose;
    for (int i = 0; i < 3; ++i) {
        for (int k = 0; k < 3; ++k) sn->camera.rows[i][k] = cv.rows[i][k];
    }
    sn->camera.aspect = cv.aspect;
    sn->camera.near_plane = cv.near_plane;
    sn->camera.far_plane = cv.far_plane;
    if (cv.valid) ViewProjection(cv.pose, cv.aspect, cv.near_plane, cv.far_plane, sn->camera.view_proj);

    // cars, with a speed from the last two poses
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    for (const SceneCarInfo& c : car_list) {
        SnapCar sc;
        sc.id = c.id;
        sc.model = c.model;
        sc.pos[0] = c.position[0], sc.pos[1] = c.position[1], sc.pos[2] = c.position[2];
        sc.heading = c.heading;
        sc.ready = c.ready;
        sc.on_path = c.on_path;
        sc.route = c.route;
        State::PoseAt& p = s.poses[c.id];
        if (c.ready) {
            const double dt = now - p.t;
            if (p.t > 0.0 && dt > 1e-4 && dt < 0.5) {
                const double v = std::sqrt((c.position[0] - p.x) * (c.position[0] - p.x) + (c.position[1] - p.y) * (c.position[1] - p.y) +
                                           (c.position[2] - p.z) * (c.position[2] - p.z)) / dt;
                p.speed = p.speed * 0.7 + v * 0.3;
            } else if (dt >= 0.5) {
                p.speed = 0.0;
            }
            p.x = c.position[0], p.y = c.position[1], p.z = c.position[2];
            p.t = now;
        }
        sc.speed = p.speed;
        sn->cars.push_back(std::move(sc));
    }

    // routes: rebuilt when the scene's revision or the number of paths changes (a route is thousands of points; the snapshot keeps a few hundred)
    const std::vector<SceneRouteInfo> routes = ListSceneRoutes();
    if (s.routes_rev != d.rev || s.routes_count != routes.size()) {
        s.routes_rev = d.rev;
        s.routes_count = routes.size();
        s.routes.clear();
        for (const SceneRouteInfo& r : routes) {
            SnapRoute sr;
            sr.name = r.name;
            sr.car = r.car;
            sr.kinematic = r.kinematic;
            std::vector<WayPoint> wp;
            for (const auto& p : r.points) wp.push_back({p[0], p[1], p[2], p[3]});
            wp = ReduceRoute(wp, 0.5, 4.0);
            const size_t stride = wp.size() > 400 ? (wp.size() + 399) / 400 : 1;
            for (size_t i = 0; i < wp.size(); i += stride) sr.points.push_back({wp[i].x, wp[i].y, wp[i].z, wp[i].speed});
            if (!wp.empty() && stride > 1) sr.points.push_back({wp.back().x, wp.back().y, wp.back().z, wp.back().speed});
            s.routes.push_back(std::move(sr));
        }
    }
    sn->routes = s.routes;

    if (s.server) {
        for (const LineServer::ClientInfo& c : s.server->Clients()) {
            SnapAgent a;
            a.client = c.index;
            a.name = c.name;
            a.last_cmd = c.last_cmd;
            a.last_cmd_age_s = c.last_cmd_age_s;
            a.commands = c.commands;
            sn->agents.push_back(std::move(a));
        }
    }
    sn->selected_line = s.selected_line;
    sn->ui_request = s.ui_request;
    sn->ui_request_serial = s.ui_serial;
    const CaptureStatus cs = CaptureGetStatus();
    sn->take_active = cs.active;
    sn->take_name = cs.name;
    sn->take_frames = cs.frames;
    sn->can_undo = !d.undo.empty();
    sn->can_redo = !d.redo.empty();
    sn->last_edit_by = d.last_by;
    sn->proposal_serial = d.proposal_serial;
    sn->proposal_path = d.proposal_path;
    sn->proposal_note = d.proposal_note;
    for (const SnapshotContributor c : SnapshotContributors()) c(*sn);
    return sn;
}

// The events that follow from what changed since the last update: the runner's state, a car that became ready, a take that ended.
void DetectChanges(State& s, const std::vector<SceneCarInfo>& cars) {
    const SceneState st = CurrentSceneState();
    if (st != s.prev_state) {
        s.prev_state = st;
        Json f = Json::Object();
        f.Set("state", SceneStateName(st));
        f.Set("t", SceneTime());
        f.Set("path", internal::Doc().path);
        PublishEvent("scene.state", std::move(f));
    }
    for (const SceneCarInfo& c : cars) {
        bool& was = s.car_ready[c.id];
        if (c.ready && !was) {
            Json f = Json::Object();
            f.Set("id", c.id);
            PublishEvent("car.ready", std::move(f));
        }
        was = c.ready;
    }
    for (auto it = s.car_ready.begin(); it != s.car_ready.end();) {
        const bool exists = std::any_of(cars.begin(), cars.end(), [&](const SceneCarInfo& c) { return c.id == it->first; });
        it = exists ? std::next(it) : s.car_ready.erase(it);
    }
}

}  // namespace

namespace internal {
void TickCaptureHandlers();  // handlers_capture.cpp
void TickCarHandlers();      // handlers_cars.cpp
}  // namespace internal

void Tick() {
    State& s = S();
    const bool want = larecomp::settings::mcla_director.Get();
    if (want != s.on) {
        if (want) Start(s);
        else Stop(s);
    }
    if (!s.on) return;
    ++s.tick;
    internal::EnsureDoc();
    Engine& e = GlobalEngine();
    e.RunQueued();
    e.PollDeferred();
    internal::TickCaptureHandlers();
    internal::TickCarHandlers();
    const std::vector<SceneCarInfo> car_list = ListSceneCars();
    DetectChanges(s, car_list);
    e.SetSnapshot(BuildSnapshot(s, car_list));
}

// ---------------------------------------------------------------------------------------------------------------- the commands that belong to no group

namespace {

using internal::Arg;
using internal::MakeSchema;

void RegisterCore() {
    RegisterCommand(
        "log.tail",
        [](const Request& req, const Responder& r) {
            const int n = static_cast<int>(std::clamp<int64_t>(req.args.IntOr("n", 50), 1, 500));
            const std::string filter = req.args.StringOr("filter", "");
            const std::string level = req.args.StringOr("level", "");
            int min_level = 0;
            if (!level.empty()) {
                const auto& names = rex::log::kLevelNames;
                min_level = -1;
                for (size_t i = 0; i < names.size(); ++i) {
                    if (names[i] == level) min_level = static_cast<int>(i);
                }
                if (min_level < 0) return r.Fail(ErrorCode::kBadArgs, "log.tail: 'level' must be trace, dev, debug, info, warn, error or critical");
            }
            std::vector<rex::log::CapturedMessage> all;
            const uint64_t latest = S().log_sink.CopySince(0, all);
            Json lines = Json::Array();
            std::vector<const rex::log::CapturedMessage*> keep;
            for (const auto& m : all) {
                if (static_cast<int>(m.level) < min_level) continue;
                if (!filter.empty() && m.text.find(filter) == std::string::npos) continue;
                keep.push_back(&m);
            }
            const size_t from = keep.size() > static_cast<size_t>(n) ? keep.size() - static_cast<size_t>(n) : 0;
            for (size_t i = from; i < keep.size(); ++i) {
                Json j = Json::Object();
                j.Set("seq", keep[i]->sequence);
                j.Set("level", std::string(rex::log::kLevelNames[static_cast<size_t>(keep[i]->level)]));
                j.Set("cat", keep[i]->category);
                j.Set("text", keep[i]->text);
                lines.Push(std::move(j));
            }
            Json res = Json::Object();
            res.Set("lines", std::move(lines));
            res.Set("latest", latest);
            r.Done(std::move(res));
        },
        MakeSchema("core", "the game's recent log lines ([scene], [cars], [cap], [dir]): read errors without the file",
                   {Arg("n", ArgType::kInt, false, "default 50, at most 500"), Arg("filter", ArgType::kString, false, "keep lines holding this text"),
                    Arg("level", ArgType::kString, false, "lowest level: trace dev debug info warn error critical")},
                   "{lines:[{seq, level, cat, text}], latest}"));
    RegisterCommand(
        "ui.open",
        [](const Request& req, const Responder& r) {
            internal::RequestUi(req.args.StringOr("panel", ""));
            r.Done();
        },
        MakeSchema("ui", "ask the Director panel to open", {Arg("panel", ArgType::kString, true, "director, timeline, ...")}, "{}"));
    RegisterCommand(
        "ui.select",
        [](const Request& req, const Responder& r) {
            internal::SelectLine(static_cast<int>(req.args.IntOr("line", 0)));
            r.Done();
        },
        MakeSchema("ui", "point at a key of the scene: the panel highlights it", {Arg("line", ArgType::kInt, true, "the scene file's line")}, "{}"));
    RegisterCommand(
        "quit",
        [](const Request&, const Responder& r) {
            Json res = Json::Object();
            res.Set("quitting", true);
            r.Done(std::move(res));
            LARECOMP_APP_INFO("[dir] quit asked for by a client");
            rex::app::RequestShutdown(0);
        },
        MakeSchema("core", "end the game", {}, "{quitting}"));
}

const bool kCoreRegistered = (RegisterCore(), true);

}  // namespace

}  // namespace mc::cinematic::director

#else  // REXGLUE_HAS_XEO3_TARGET

#include "director.h"
#include "director_game.h"

namespace mc::cinematic::director {
bool Enabled() { return false; }
void NoteCameraHook(uint8_t*, uint32_t, uint32_t, CameraSource) {}
void Tick() {}
bool StreamingDone(bool* known) {
    if (known) *known = false;
    return true;
}
ListenerInfo GetListenerInfo() { return ListenerInfo{}; }
}  // namespace mc::cinematic::director

#endif  // REXGLUE_HAS_XEO3_TARGET
