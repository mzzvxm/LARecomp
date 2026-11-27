#ifndef REXGLUE_HAS_XEO3_TARGET
//
// The Director's transport commands (DIRECTOR-PROTOCOL.md "transport"): get, play, pause, restart, seek, step, mode. They drive the scene runner's handles
// (scene_runtime.h): a pause or a seek holds the clock and freezes the world; a seek pins the clock at a time, which is exact for what is a function of the scene
// time (camera, kinematic cars, hour, weather, hud, split). A reply that waits ("wait":"settled") comes when the frames are rendered, the cars are ready and the
// streaming is done.
//

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

#include "../capture_mode.h"
#include "../scene_runtime.h"
#include "director_internal.h"
#include "director_math.h"
#include "larecomp_log.h"
#include "session_settle.h"
#include "settings/larecomp_settings.h"

#if defined(LARECOMP_HAS_GPU)
#include "gpu/scene_noise.h"
#endif

namespace mc::cinematic::director {

namespace {

using internal::Arg;
using internal::MakeSchema;

// Answers `result` now, or, for a request that waits, when the world has settled `frames` updates after this tick. `result` is built by the caller.
void ReplyWhenSettled(const Request& req, const Responder& r, Json result, int frames) {
    if (!req.wait_settled) return r.Done(std::move(result));
    const uint64_t since = internal::TickCount();
    auto res = std::make_shared<Json>(std::move(result));
    r.Defer([since, frames, res](const Responder& d) {
        std::string why;
        if (!internal::WorldSettled(since, frames, &why)) return;
        res->Set("settled", true);
        d.Done(*res);
    });
}

void RegisterTransport() {
    RegisterCommand(
        "scene.freeze",
        [](const Request& req, const Responder& r) {
            const Scene* scene = LoadedScene();
            if (!scene || CurrentSceneState() != SceneState::kRunning) {
                return r.Fail(ErrorCode::kNotReady, "scene.freeze requires a running scene");
            }
            const double t = req.args.NumberOr("t", -1.0);
            if (!(t >= 0.0) || (scene->Duration() >= 0.0 && t >= scene->Duration())) {
                return r.Fail(ErrorCode::kBadArgs, "scene.freeze: 't' must precede the end of the scene");
            }
            const WorldState initial = EvaluateWorld(*scene, 0.0);
            const WorldState pinned = EvaluateWorld(*scene, t);
            bool override_on = false;
            SceneWorldOverride(&override_on);
            if (override_on || initial.traffic != 0 || initial.peds != 0 || pinned.peds != 0 ||
                !pinned.hour_set || pinned.weather < 0 || !ScrubbableAt(*scene, t).scrubbable) {
                return r.Fail(ErrorCode::kRefused,
                              "scene.freeze needs traffic and peds off from the start and at the pin, a fixed hour and weather at the pin, and no brain vehicles or world override");
            }
            if (larecomp::settings::mcla_scene_t.Get() >= 0.0 || larecomp::settings::mcla_scene_seed.Get() <= 0 ||
                !larecomp::settings::mcla_scene_seed_each_frame.Get()) {
                return r.Fail(ErrorCode::kRefused, "scene.freeze needs Director-owned time and per-update scene seeding");
            }
            // CHECK: The engine's existing held-world time floor can still advance
            // some particles or cars. Repeat captures establish the residual floor.
            SceneSeekTo(t);
            r.Done(internal::TransportJson());
        },
        MakeSchema("scene", "pin a scene for repeat captures with ambient spawning disabled and deterministic scene seeds",
                   {Arg("t", ArgType::kNumber, true, "scene seconds")}, "the fields of transport.get"));

    RegisterCommand(
        "scene.settle",
        [](const Request& req, const Responder& r) {
            const int64_t frames = req.args.IntOr("frames", 120);
            if (frames < 1 || frames > 600) return r.Fail(ErrorCode::kBadArgs, "scene.settle: 'frames' must be 1 to 600");
#if defined(LARECOMP_HAS_GPU)
            const SceneTransport initial = GetSceneTransport();
            if (initial.state != SceneState::kRunning || !initial.held || initial.pending_steps) {
                return r.Fail(ErrorCode::kNotReady, "scene.settle needs a held scene");
            }
            const std::string scene = SceneFilePath();
            const uint64_t since = internal::TickCount();
            r.Defer([scene, t = initial.t, since, frames, gate = SessionSettle{}](const Responder& d) mutable {
                const SceneTransport now = GetSceneTransport();
                if (now.state != SceneState::kRunning || !now.held || now.pending_steps ||
                    now.t != t || SceneFilePath() != scene) {
                    return d.Fail(ErrorCode::kStale, "scene.settle: the scene or its held time changed");
                }
                bool stream_known = false;
                const bool streamed = StreamingDone(&stream_known);
                GameStateSample state;
                const bool ready = SampleGameState(&state) && state.world_ready && !state.paused &&
                                   stream_known && streamed && internal::WorldSettled(since, 3, nullptr);
                const uint64_t present = mcla::gpu::scene_noise::Get().presents.load(std::memory_order_relaxed);
                // CHECK: Validate present counting and a warm history with the first D3D12 and Vulkan captures.
                if (!gate.Observe(ready, present, static_cast<uint64_t>(frames))) return;
                Json result = Json::Object();
                result.Set("t", t);
                result.Set("frames", frames);
                result.Set("present", present);
                d.Done(std::move(result));
            });
#else
            r.Fail(ErrorCode::kNotReady, "scene.settle requires the LARecomp renderer");
#endif
        },
        MakeSchema("scene", "wait for known streaming and car readiness, then count rendered guest frames at the held scene time",
                   {Arg("frames", ArgType::kInt, false, "1 to 600, default 120")}, "{t, frames, present}"));

    RegisterCommand(
        "transport.get", [](const Request&, const Responder& r) { r.Done(internal::TransportJson()); },
        MakeSchema("transport", "the scene's clock", {}, "{state, t, duration, fps, step, paused, frozen, frame, mode, scrubbable, scrub_hint, holding_for_ready}"));

    RegisterCommand(
        "transport.play",
        [](const Request&, const Responder& r) {
            if (!internal::NeedRunningScene(r)) return;
            if (GetSceneTransport().state == SceneState::kFinished) SceneRestartAt(0.0);  // it ended: playing is running it again
            else SceneHold(false);
            r.Done(internal::TransportJson());
        },
        MakeSchema("transport", "release a pause or a seek: the clock goes on from where it stands (a scene that ended runs again from the start)", {}, "the fields of transport.get"));

    RegisterCommand(
        "transport.pause",
        [](const Request&, const Responder& r) {
            if (!internal::NeedRunningScene(r)) return;
            SceneHold(true);
            r.Done(internal::TransportJson());
        },
        MakeSchema("transport", "hold the clock and freeze the world (time scale floor)", {}, "the fields of transport.get"));

    RegisterCommand(
        "transport.restart",
        [](const Request& req, const Responder& r) {
            const SceneState st = CurrentSceneState();
            if (st == SceneState::kOff || st == SceneState::kPending || st == SceneState::kFailed) {
                return r.Fail(ErrorCode::kNotReady, "no scene is loaded: scene.load first");
            }
            const double t = req.args.NumberOr("t", 0.0);
            const Scene* scene = LoadedScene();
            if (t < 0.0 || (scene && scene->Duration() >= 0.0 && t > scene->Duration())) return r.Fail(ErrorCode::kBadArgs, "transport.restart: 't' is outside the scene");
            SceneRestartAt(t);
            ReplyWhenSettled(req, r, internal::TransportJson(), 20);
        },
        MakeSchema("transport", "run the scene again from the start (the cars are made again); with t, seek there", {Arg("t", ArgType::kNumber, false, "default 0")},
                   "the fields of transport.get"));

    RegisterCommand(
        "transport.seek",
        [](const Request& req, const Responder& r) {
            if (!internal::NeedRunningScene(r)) return;
            const Scene* scene = LoadedScene();
            const double t = req.args.NumberOr("t", 0.0);
            const double dur = scene ? scene->Duration() : -1.0;
            if (!(t >= 0.0) || (dur >= 0.0 && t > dur + 1e-9)) {
                char msg[96];
                std::snprintf(msg, sizeof(msg), "transport.seek: 't' must be within 0 and %g", dur);
                return r.Fail(ErrorCode::kBadArgs, dur >= 0.0 ? msg : "transport.seek: 't' must be 0 or more");
            }
            const int settle = static_cast<int>(std::clamp<int64_t>(req.args.IntOr("settle_frames", 20), 0, 600));
            SceneSeekTo(t);
            const ScrubInfo si = scene ? ScrubbableAt(*scene, t) : ScrubInfo{};
            Json res = Json::Object();
            res.Set("t", t);
            res.Set("paused", true);
            res.Set("frozen", true);
            res.Set("scrubbable", si.scrubbable);
            res.Set("scrub_hint", si.hint);
            res.Set("settled", false);
            ReplyWhenSettled(req, r, std::move(res), settle);
        },
        MakeSchema("transport", "pin the clock at t with the world frozen: exact for the camera, kinematic cars, hour, weather, hud and split",
                   {Arg("t", ArgType::kNumber, true, "scene seconds"), Arg("settle_frames", ArgType::kInt, false, "default 20: updates to wait for with wait:settled")},
                   "{t, paused, frozen, scrubbable, scrub_hint, settled}"));

    RegisterCommand(
        "transport.step",
        [](const Request& req, const Responder& r) {
            if (!internal::NeedRunningScene(r)) return;
            const int frames = static_cast<int>(req.args.IntOr("frames", 1));
            if (frames < 1 || frames > 100000) return r.Fail(ErrorCode::kBadArgs, "transport.step: 'frames' must be 1 to 100000");
            if (!FixedStepActive()) {
                return r.Fail(ErrorCode::kNotReady, "transport.step needs the fixed step: call transport.mode {fixed:true} first (a step is exact only at a fixed step)");
            }
            if (!GetSceneTransport().held) SceneHold(true);
            SceneStepUpdates(frames);
            r.Defer([frames](const Responder& d) {
                if (GetSceneTransport().pending_steps > 0) return;
                Json res = Json::Object();
                res.Set("t", GetSceneTransport().t);
                res.Set("frames", frames);
                d.Done(std::move(res));
            });
        },
        MakeSchema("transport", "while paused: run exactly this many updates at the fixed step, then hold again", {Arg("frames", ArgType::kInt, false, "default 1")},
                   "{t, frames}"));

    RegisterCommand(
        "transport.mode",
        [](const Request& req, const Responder& r) {
            const Scene* scene = LoadedScene();
            double fps = req.args.NumberOr("fps", scene && scene->fps > 0.0 ? scene->fps : 60.0);
            if (!(fps >= 1.0 && fps <= 960.0)) return r.Fail(ErrorCode::kBadArgs, "transport.mode: 'fps' must be 1 to 960");
            const bool has_fixed = req.args.Has("fixed");
            const bool fixed = has_fixed ? req.args.BoolOr("fixed", true) : (req.args.Has("fps") ? true : FixedStepActive());
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%.6g", fixed ? fps : 0.0);
            larecomp::cvar::SetFlagByName("mcla_fixed_step_fps", buf);
            Json res = Json::Object();
            res.Set("mode", fixed ? "fixed" : "realtime");
            res.Set("fps", fixed ? fps : 0.0);
            res.Set("step", fixed ? 1.0 / fps : 0.0);
            r.Done(std::move(res));
        },
        MakeSchema("transport", "fixed step on or off (the preview runs at the scene's step, as a take does)",
                   {Arg("fixed", ArgType::kBool, false, "default: on when fps is given"), Arg("fps", ArgType::kNumber, false, "default the scene's header fps, else 60")}, "{mode, fps, step}"));
}

const bool kRegistered = (RegisterTransport(), true);

}  // namespace

}  // namespace mc::cinematic::director

#endif  // REXGLUE_HAS_XEO3_TARGET
