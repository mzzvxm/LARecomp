#ifndef REXGLUE_HAS_XEO3_TARGET
//
// The Director's capture commands (DIRECTOR-PROTOCOL.md "capture"): `shot` (one clean engine frame as a PNG, with the scene pinned at a time when asked),
// `shot.schedule` (stills at exact scene times during one play-through), `take.render` (a take over a stretch of the scene) and `take.status`.
//
// A still is a take of one frame (capture_mode.h): no overlay, no ImGui, the engine's 1:1 size, the upscaler's history not restarted for it. A take.render
// restarts the scene (the cars are made again), waits until the world has settled, seeks to `from`, plays, asks capture mode for the take, and ends it at `to`.
//

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "../capture_mode.h"
#include "settings/immortal.h"
#include "../scene_runtime.h"
#include "director_internal.h"
#include "director_math.h"
#include "larecomp_log.h"

namespace mc::cinematic::director {

namespace {

using internal::Arg;
using internal::MakeSchema;

uint64_t g_still_ids = 0;

bool EndsWithPng(const std::string& p) {
    if (p.size() < 4) return false;
    std::string e = p.substr(p.size() - 4);
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e == ".png";
}

double FixedStepSeconds() {
    return FixedStepActive() ? static_cast<double>(FixedStepTicks()) / 49875000.0 : 0.0;
}

// ---- stills at scene times

struct Schedule {
    bool active = false;
    std::vector<double> times;
    size_t next = 0;
    std::filesystem::path dir;
    std::string dir_text;
    uint64_t pending_id = 0;
    bool pending = false;
    std::string pending_path;
};
Schedule& g_sched = larecomp::Leak<Schedule>();

// ---- a take over a stretch of the scene

enum class TakePhase : uint8_t { kIdle, kWaitReady, kSeeking, kRecording, kEnding };
struct TakeJob {
    TakePhase phase = TakePhase::kIdle;
    double fps = 0.0, from = 0.0;
    CaptureSpec spec;
    uint64_t since = 0;
    uint64_t finished_seen = 0;
    uint64_t last_progress = 0;
    std::string name;
};
TakeJob& g_take = larecomp::Leak<TakeJob>();

void PublishShotDone(const std::string& path, double t, bool ok, const std::string& error, int index, int total) {
    Json f = Json::Object();
    f.Set("path", path);
    f.Set("t", t);
    f.Set("ok", ok);
    if (!ok) f.Set("error", error);
    if (total > 0) {
        f.Set("index", index);
        f.Set("total", total);
    }
    PublishEvent("shot.done", std::move(f));
}

void RegisterCapture() {
    RegisterCommand(
        "shot",
        [](const Request& req, const Responder& r) {
            const std::string user = req.args.StringOr("path", "");
            std::filesystem::path file;
            std::string err;
            if (!internal::ResolveWritePath(user, false, &file, &err)) return internal::FailRefused(r, err);
            if (!EndsWithPng(user)) return r.Fail(ErrorCode::kBadArgs, "shot: 'path' must end in .png");
            const bool has_t = req.args.Get("t").is_number();
            double t = 0.0;
            if (has_t) {
                if (!internal::NeedRunningScene(r)) return;
                const Scene* scene = LoadedScene();
                t = req.args.NumberOr("t", 0.0);
                if (!(t >= 0.0) || (scene && scene->Duration() >= 0.0 && t > scene->Duration() + 1e-9)) return r.Fail(ErrorCode::kBadArgs, "shot: 't' is outside the scene");
                SceneSeekTo(t);
            }
            struct Job {
                int stage = 0;  // 0 waiting to settle, 1 asking, 2 waiting for the picture
                uint64_t id = 0;
                uint64_t since = 0;
                std::string path;
                bool settle = false;
            };
            auto job = std::make_shared<Job>();
            job->since = internal::TickCount();
            job->path = internal::Utf8(file);
            job->settle = has_t || req.wait_settled;
            r.Defer([job](const Responder& d) {
                if (job->stage == 0) {
                    std::string why;
                    if (job->settle && !internal::WorldSettled(job->since, 20, &why)) return;
                    job->stage = 1;
                }
                if (job->stage == 1) {
                    StillRequest sr;
                    sr.id = ++g_still_ids;
                    sr.path = job->path;
                    std::string why;
                    if (!CaptureStillRequest(sr, &why)) {
                        if (why == "another still is waiting") return;  // try again at the next update
                        return d.Fail(ErrorCode::kNotReady, "shot: " + why);
                    }
                    job->id = sr.id;
                    job->stage = 2;
                    return;
                }
                StillResult res;
                if (!CaptureStillPoll(job->id, &res)) return;
                if (!res.ok) {
                    PublishShotDone(job->path, res.scene_t, false, res.error, 0, 0);
                    return d.Fail(ErrorCode::kFailed, "shot: " + res.error);
                }
                Json out = Json::Object();
                out.Set("path", res.path);
                out.Set("width", res.width);
                out.Set("height", res.height);
                out.Set("t", res.scene_t >= 0.0 ? res.scene_t : GetSceneTransport().t);
                out.Set("bytes", res.bytes);
                out.Set("frame", res.update);
                PublishShotDone(res.path, res.scene_t, true, "", 0, 0);
                d.Done(std::move(out));
            });
        },
        MakeSchema("capture", "a clean engine frame (no overlay, no ImGui) as a PNG at the engine's 1:1 size; with t, the scene is pinned there first and the world settles",
                   {Arg("path", ArgType::kString, true, "relative to mcla_director_root, ends in .png"), Arg("size", ArgType::kAny, false, "a hint: the result says the real size"),
                    Arg("t", ArgType::kNumber, false, "seek here first")},
                   "{path, width, height, t, bytes, frame}"));

    RegisterCommand(
        "shot.schedule",
        [](const Request& req, const Responder& r) {
            const Json& list = req.args.Get("times");
            if (list.size() == 0) {
                const bool was = g_sched.active;
                g_sched = Schedule{};
                Json res = Json::Object();
                res.Set("count", 0);
                res.Set("cancelled", was);
                return r.Done(std::move(res));
            }
            if (list.size() > 2000) return r.Fail(ErrorCode::kBadArgs, "shot.schedule: at most 2000 times");
            if (g_sched.active) return r.Fail(ErrorCode::kNotReady, "a schedule is running: shot.schedule {times:[]} cancels it");
            if (!internal::NeedRunningScene(r)) return;
            const Scene* scene = LoadedScene();
            std::vector<double> times;
            for (size_t i = 0; i < list.size(); ++i) {
                if (!list.at(i).is_number() || !(list.at(i).AsNumber() >= 0.0)) return r.Fail(ErrorCode::kBadArgs, "shot.schedule: times[" + std::to_string(i) + "] must be a time of 0 or more");
                times.push_back(list.at(i).AsNumber());
            }
            std::sort(times.begin(), times.end());
            if (scene && scene->Duration() >= 0.0 && times.back() > scene->Duration() + 1e-9) return r.Fail(ErrorCode::kBadArgs, "shot.schedule: a time is past the scene's end");
            const double step = FixedStepSeconds();
            if (step <= 0.0) return r.Fail(ErrorCode::kNotReady, "shot.schedule needs the fixed step: call transport.mode {fixed:true} first (the stills must fall on the take's frames)");
            const SceneTransport tr = GetSceneTransport();
            if (times.front() < tr.t - step) return r.Fail(ErrorCode::kBadArgs, "shot.schedule: the first time is behind the scene's clock: transport.restart first");
            std::filesystem::path dir;
            std::string err;
            if (!internal::ResolveWritePath(req.args.StringOr("dir", ""), false, &dir, &err)) return internal::FailRefused(r, err);
            g_sched = Schedule{};
            g_sched.active = true;
            g_sched.times = std::move(times);
            g_sched.dir = dir;
            g_sched.dir_text = internal::Utf8(dir);
            SceneHold(false);  // the stills come during a play-through
            Json res = Json::Object();
            res.Set("count", static_cast<int64_t>(g_sched.times.size()));
            res.Set("dir", g_sched.dir_text);
            r.Done(std::move(res));
        },
        MakeSchema("capture", "stills at exact scene times during one play-through (each is a one-frame take at the fixed step); shot.done for each; an empty list cancels",
                   {Arg("times", ArgType::kArray, true, "scene seconds"), Arg("dir", ArgType::kString, true, "relative to mcla_director_root")}, "{count, dir}"));

    RegisterCommand(
        "take.render",
        [](const Request& req, const Responder& r) {
            if (g_take.phase != TakePhase::kIdle || CaptureGetStatus().active) return r.Fail(ErrorCode::kNotReady, "a take is running");
            if (!internal::NeedRunningScene(r)) return;
            const Scene* scene = LoadedScene();
            const double fps = req.args.NumberOr("fps", 0.0);
            if (!(fps >= 1.0 && fps <= 960.0)) return r.Fail(ErrorCode::kBadArgs, "take.render: 'fps' must be 1 to 960");
            const std::string sink = req.args.StringOr("sink", "");
            if (!sink.empty() && sink != "none" && sink != "ffmpeg" && sink != "png" && sink != "raw") return r.Fail(ErrorCode::kBadArgs, "take.render: 'sink' must be none, ffmpeg, png or raw");
            const double from = req.args.NumberOr("from", 0.0);
            double to = req.args.NumberOr("to", scene ? scene->Duration() : -1.0);
            if (!std::isfinite(from) || from < 0.0) return r.Fail(ErrorCode::kBadArgs, "take.render: 'from' must be a finite time of 0 or more");
            if (!std::isfinite(to) || to <= from) return r.Fail(ErrorCode::kBadArgs, to < 0.0 ? "take.render: the scene has no `end`: give 'to'" : "take.render: 'to' must be a finite time after 'from'");
            if (scene && scene->Duration() >= 0.0 && to > scene->Duration() + 1e-9) return r.Fail(ErrorCode::kBadArgs, "take.render: 'to' is past the scene's end");
            g_take = TakeJob{};
            g_take.phase = TakePhase::kWaitReady;
            g_take.fps = fps;
            g_take.from = from;
            g_take.spec.name = req.args.StringOr("name", "");
            g_take.spec.sink = sink;
            g_take.spec.t0 = from;
            g_take.spec.end_scene_t = to;
            g_take.name = g_take.spec.name;
            g_take.since = internal::TickCount();
            g_take.finished_seen = CaptureGetStatus().finished;
            SceneRestartAt(0.0);
            // The clock stands at 0 until the take begins: left running, the scene was already 0.15 to 0.27 s in when the take started (run 1, 11-20: a 2 s take had 54 frames, a
            // 10 s take 297). Not when the restart went to "waiting" (no world just now): a held clock would never start it.
            if (CurrentSceneState() == SceneState::kRunning) SceneHold(true);
            Json res = Json::Object();
            res.Set("name", g_take.name);
            res.Set("fps", fps);
            res.Set("sink", sink);
            res.Set("from", from);
            res.Set("to", to);
            r.Done(std::move(res));
        },
        MakeSchema("capture", "render a take over a stretch of the scene: the scene restarts, settles, runs from `from` to `to`; events take.progress and take.done",
                   {Arg("fps", ArgType::kNumber, true), Arg("sink", ArgType::kString, false, "none, ffmpeg, png or raw (default the capture setting's)"), Arg("name", ArgType::kString, false),
                    Arg("from", ArgType::kNumber, false, "first frame's exact scene time, default 0"), Arg("to", ArgType::kNumber, false, "exclusive end, default the scene's end")},
                   "{name, fps, sink, from, to}"));

    RegisterCommand(
        "take.status",
        [](const Request&, const Responder& r) {
            const CaptureStatus st = CaptureGetStatus();
            Json res = Json::Object();
            res.Set("active", st.active || g_take.phase != TakePhase::kIdle);
            res.Set("name", st.active ? st.name : g_take.name);
            res.Set("folder", st.folder);
            res.Set("fps", st.active ? st.fps : g_take.fps);
            res.Set("frames", st.frames);
            const char* phase = g_take.phase == TakePhase::kIdle ? (st.active ? "recording" : "idle")
                                : g_take.phase == TakePhase::kWaitReady ? "waiting for the world"
                                : g_take.phase == TakePhase::kSeeking ? "seeking"
                                : g_take.phase == TakePhase::kRecording ? "recording"
                                                                          : "ending";
            res.Set("state", phase);
            if (st.failed) res.Set("error", st.error);
            Json last = Json::Object();
            last.Set("name", st.last_name);
            last.Set("folder", st.last_folder);
            last.Set("state", st.last_state);
            last.Set("frames", st.last_frames);
            if (!st.last_error.empty()) last.Set("error", st.last_error);
            res.Set("last", std::move(last));
            r.Done(std::move(res));
        },
        MakeSchema("capture", "the running take and the one that ended last", {}, "{active, name, folder, fps, frames, state, error?, last:{name, folder, state, frames, error?}}"));
}

const bool kRegistered = (RegisterCapture(), true);

}  // namespace

namespace internal {

// Once an update: the schedule of stills and the take that take.render drives.
void TickCaptureHandlers() {
    // ---- shot.schedule
    if (g_sched.active) {
        const SceneTransport tr = GetSceneTransport();
        const double step = FixedStepSeconds();
        const int total = static_cast<int>(g_sched.times.size());
        if (g_sched.pending) {
            StillResult res;
            if (CaptureStillPoll(g_sched.pending_id, &res)) {
                g_sched.pending = false;
                const double t = g_sched.times[g_sched.next];
                PublishShotDone(res.path, t, res.ok, res.error, static_cast<int>(g_sched.next) + 1, total);
                ++g_sched.next;
                if (g_sched.next >= g_sched.times.size()) g_sched = Schedule{};
            }
        } else if (tr.state == SceneState::kFinished || tr.state == SceneState::kFailed || tr.state == SceneState::kOff) {
            for (size_t i = g_sched.next; i < g_sched.times.size(); ++i) {
                PublishShotDone("", g_sched.times[i], false, "the scene ended before this time", static_cast<int>(i) + 1, total);
            }
            g_sched = Schedule{};
        } else if (step > 0.0 && tr.state == SceneState::kRunning && !tr.holding_for_ready) {
            const double want = g_sched.times[g_sched.next];
            // The still's frame shows the scene time after this update's step: ask when the next update is the one nearest to `want`.
            if (tr.t + step >= want - 0.5 * step) {
                char name[48];
                std::snprintf(name, sizeof(name), "t%06lld.png", static_cast<long long>(std::llround(want * 1000.0)));
                StillRequest sr;
                sr.id = ++g_still_ids;
                sr.path = Utf8(g_sched.dir / name);
                std::string why;
                if (CaptureStillRequest(sr, &why)) {
                    g_sched.pending = true;
                    g_sched.pending_id = sr.id;
                    g_sched.pending_path = sr.path;
                } else if (why != "a take is running" && why != "another still is waiting") {
                    PublishShotDone(sr.path, want, false, why, static_cast<int>(g_sched.next) + 1, total);
                    ++g_sched.next;
                    if (g_sched.next >= g_sched.times.size()) g_sched = Schedule{};
                }
            }
        }
    }

    // ---- take.render
    TakeJob& j = g_take;
    if (j.phase == TakePhase::kIdle) return;
    const CaptureStatus st = CaptureGetStatus();
    const SceneTransport tr = GetSceneTransport();
    switch (j.phase) {
        case TakePhase::kWaitReady: {
            if (tr.state == SceneState::kFailed || tr.state == SceneState::kOff) {
                Json f = Json::Object();
                f.Set("name", j.name);
                f.Set("state", "failed");
                f.Set("error", "the scene stopped before the take could start");
                PublishEvent("take.done", std::move(f));
                j = TakeJob{};
                return;
            }
            std::string why;
            if (!WorldSettled(j.since, 10, &why) || tr.holding_for_ready) return;
            if (j.from > 0.0) {
                SceneSeekTo(j.from);
                j.since = TickCount();
                j.phase = TakePhase::kSeeking;
            } else {
                SceneHold(false);
                CaptureDirectorRequest(j.fps, j.spec);
                j.phase = TakePhase::kRecording;
            }
            return;
        }
        case TakePhase::kSeeking: {
            std::string why;
            if (!WorldSettled(j.since, 20, &why)) return;
            SceneHold(false);
            CaptureDirectorRequest(j.fps, j.spec);
            j.phase = TakePhase::kRecording;
            return;
        }
        case TakePhase::kRecording: {
            if (st.active && st.frames >= j.last_progress + 60) {
                j.last_progress = st.frames;
                Json f = Json::Object();
                f.Set("name", st.name);
                f.Set("frames", st.frames);
                PublishEvent("take.progress", std::move(f));
            }
            if (st.finished != j.finished_seen) {
                CaptureDirectorRequest(0.0, CaptureSpec{});
                j.phase = TakePhase::kEnding;
            }
            return;
        }
        case TakePhase::kEnding: {
            if (st.finished == j.finished_seen) return;
            Json f = Json::Object();
            f.Set("name", st.last_name);
            f.Set("folder", st.last_folder);
            f.Set("state", st.last_state);
            f.Set("frames", st.last_frames);
            if (!st.last_error.empty()) f.Set("error", st.last_error);
            PublishEvent("take.done", std::move(f));
            j = TakeJob{};
            return;
        }
        case TakePhase::kIdle:
            return;
    }
}

}  // namespace internal

}  // namespace mc::cinematic::director

#endif  // REXGLUE_HAS_XEO3_TARGET
