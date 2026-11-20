#pragma once
//
// What the Director's game-side files share: director_runtime.cpp (the tick, the scene document, the snapshot, the listener) and the handlers
// (handlers_transport, _scene, _camera, _cars, _capture). Game thread only. Not a public interface: director.h and DIRECTOR-PROTOCOL.md are.
//

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "command.h"
#include "director.h"
#include "../scene.h"
#include "scene_edit.h"

namespace mc::cinematic::director::internal {

// ---------------------------------------------------------------------------------------------------------------- writing command schemas

// {"name", type, required, "doc"}
inline ArgSpec Arg(const char* name, ArgType type, bool required, const char* doc = "") {
    ArgSpec a;
    a.name = name;
    a.type = type;
    a.required = required;
    a.doc = doc;
    return a;
}
inline Schema MakeSchema(const char* group, const char* summary, std::vector<ArgSpec> args, const char* result, bool file = false, bool open_args = false) {
    Schema s;
    s.group = group;
    s.summary = summary;
    s.args = std::move(args);
    s.result = result;
    s.file = file;
    s.open_args = open_args;
    return s;
}

// ---------------------------------------------------------------------------------------------------------------- the scene as the Director keeps it

// The scene file's text and its revision. Every change of the text, by anyone, is one revision: an edit of the Director (agent or panel), an undo, a proposal
// accepted, a file saved by a person (noticed when the runner reads it).
struct SceneDoc {
    std::string path;  // the file as the runner has it
    std::string text, hash;
    int64_t rev = 0;
    std::vector<std::string> undo, redo;  // texts, newest last
    std::string last_by;
    // the parse of `text`, kept for scene.get and the snapshot
    std::string name;
    double fps = 0.0, duration = -1.0;
    std::vector<SnapEvent> events;
    std::vector<SnapDiag> diagnostics;
    std::vector<std::string> car_ids;
    std::vector<int> pending_changed;  // the lines the edit that is being loaded changed (for scene.reloaded)
    // a proposal waiting for the human (scene.propose)
    std::string proposal_path, proposal_note;
    uint64_t proposal_serial = 0;
};
SceneDoc& Doc();
// Makes the document match the runner's scene when it does not (the first command after the Director was switched on, or after another scene was armed).
void EnsureDoc();
void RefreshDocCaches(SceneDoc& doc);
// The text of a file, or false.
bool ReadTextFile(const std::filesystem::path& path, std::string* out);
// The whole text written to a temporary name and renamed over `path`.
bool WriteTextFileAtomic(const std::filesystem::path& path, const std::string& text, std::string* error);

struct EditOutcome {
    bool ok = false;
    ErrorCode code = ErrorCode::kFailed;
    std::string message;
    Json details;
    Json result;  // ok: {rev, hash, lines, changed, errors}
    std::vector<int> lines;
};
// Applies ops to the scene file (base_rev checked: stale when the file moved on), writes it, keeps the revision, loads it in the runner and tells the clients.
// `by`: "agent", "panel", ... A result that does not parse is refused unless allow_errors (or the scene did not parse before).
EditOutcome ApplySceneEdit(const std::vector<EditOp>& ops, int64_t base_rev, const char* by, bool allow_errors);
// One line inserted at the current revision (the [file] commands).
EditOutcome InsertSceneLine(const std::string& text, const char* by);
// The scene text becomes `text` as a new revision (undo, redo, accept).
EditOutcome ReplaceSceneText(const std::string& text, const char* by, bool push_undo);

// ---------------------------------------------------------------------------------------------------------------- paths

// The folder the commands may write to: mcla_director_root, else the working folder. Absolute.
std::filesystem::path WriteRoot();
// The scene file's folder. Absolute; empty with no scene.
std::filesystem::path SceneFolder();
// The lexical rule (director_math.h CheckRelativePath) and a check that the result stays under its base even through links. `under_scene`: relative to the scene's
// folder, else to the write root. False with `error` (a `refused` message).
bool ResolveWritePath(std::string_view user, bool under_scene, std::filesystem::path* out, std::string* error);
std::string Utf8(const std::filesystem::path& p);
std::filesystem::path FromUtf8(const std::string& s);

// ---------------------------------------------------------------------------------------------------------------- the world

// The number of Director ticks so far (updates the Director saw).
uint64_t TickCount();
// What a settled world is: `frames` updates have passed since `since_tick`, every scene car is ready and the streaming is done. `waiting_for` says what is not.
bool WorldSettled(uint64_t since_tick, int frames, std::string* waiting_for);
// transport.get's result.
Json TransportJson();
// The camera as the Director last saw it (the hook): for camera.get and key.add.
struct CameraView {
    bool valid = false;
    std::string mode = "game";
    SnapPose pose;
    double rows[3][3] = {};
    double near_plane = 0.1, far_plane = 10000.0;
    double aspect = 16.0 / 9.0;
};
CameraView CurrentCamera();

// ---------------------------------------------------------------------------------------------------------------- misc

void RequestUi(const std::string& panel);
void SelectLine(int line);
// "refused": a Reply for a bad path.
inline void FailRefused(const Responder& r, const std::string& why) {
    r.Fail(ErrorCode::kRefused, why);
}
// A request's scene must be loaded and running: replies `not_ready` / `no_world` and returns false when it is not.
bool NeedRunningScene(const Responder& r);

}  // namespace mc::cinematic::director::internal
