#ifndef REXGLUE_HAS_XEO3_TARGET
//
// The Director's scene commands (DIRECTOR-PROTOCOL.md "scene" and "world"): load, get, edit (line-addressed text patches: the file stays the single source
// of truth), propose / accept / reject, undo / redo, and the world's live preview or committed keys.
//

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <memory>
#include <string>

#include "../scene_runtime.h"
#include "../capture_mode.h"
#include "../spots.h"
#include "director_internal.h"
#include "director_math.h"
#include "larecomp_log.h"
#include "mc_engine/gameplay/time_of_day.h"
#include "settings/larecomp_settings.h"
#include "preset_command.h"
#if defined(LARECOMP_HAS_GPU)
#include "gpu/gfx_preset_apply.h"
#endif

namespace mc::cinematic::director {

namespace {

using internal::Arg;
using internal::EditOutcome;
using internal::MakeSchema;

const char* ByOf(const Request& req) {
    return req.source == Source::kPanel || req.source == Source::kConsole ? "panel" : "agent";
}

void FailFrom(const Responder& r, const EditOutcome& o) {
    r.Fail(o.code, o.message, o.details);
}

void RegisterPresets();

std::string Stamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    return buf;
}

Json DiagnosticsJson(const std::vector<SnapDiag>& diags) {
    Json a = Json::Array();
    for (const SnapDiag& d : diags) {
        Json j = Json::Object();
        j.Set("line", d.line);
        j.Set("severity", d.error ? "error" : "warning");
        j.Set("message", d.message);
        a.Push(std::move(j));
    }
    return a;
}

Json OverrideJson(const WorldState& o) {
    Json j = Json::Object();
    if (o.hour_set) j.Set("hour", o.hour);
    if (o.weather >= 0) j.Set("weather", Weather_IndexToName(o.weather) ? Weather_IndexToName(o.weather) : "?");
    if (o.traffic >= 0) j.Set("traffic", o.traffic == 1);
    if (o.peds >= 0) j.Set("peds", o.peds == 1);
    if (o.parked >= 0) j.Set("parked", o.parked == 1);
    if (o.peds_scale >= 0.0) j.Set("peds", o.peds_scale);
    if (o.parked_scale >= 0.0) j.Set("parked", o.parked_scale);
    if (o.police >= 0) j.Set("police", o.police == 1);
    if (o.timescale_set) j.Set("timescale", o.timescale);
    return j;
}

void RegisterScene() {
    RegisterPresets();
    RegisterCommand(
        "scene.unload",
        [](const Request&, const Responder& r) {
            if (!larecomp::cvar::SetFlagByName("mcla_scene", "")) return r.Fail(ErrorCode::kFailed, "could not clear mcla_scene");
            r.Defer([](const Responder& d) {
                if (CurrentSceneState() != SceneState::kOff) return;
                SceneSetPresetPreview(std::nullopt);
                SceneSetPresetLabel(std::nullopt);
                d.Done(Json::Object());
            });
        },
        MakeSchema("scene", "unload the scene and restore its settings", {}, "{}"));

    RegisterCommand(
        "scene.setting",
        [](const Request& req, const Responder& r) {
            if (!internal::NeedRunningScene(r)) return;
            const std::string name = req.args.StringOr("name", "");
            if (name.empty() || name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos) {
                return r.Fail(ErrorCode::kBadArgs, "scene.setting: expected a lowercase setting name");
            }
            if (const auto why = CvarRefusal(name); !why.empty()) return r.Fail(ErrorCode::kRefused, why);
            const auto* info = larecomp::cvar::GetFlagInfo(name);
            if (!info || info->type == larecomp::cvar::FlagType::Command || info->lifecycle != larecomp::cvar::Lifecycle::kHotReload) {
                return r.Fail(ErrorCode::kBadArgs, "scene.setting: no live setting called '" + name + "'");
            }
            if (req.args.Has("value") && !SceneSetLiveSetting(name, req.args.StringOr("value", ""))) {
                return r.Fail(ErrorCode::kBadArgs, "scene.setting: the setting rejected its value");
            }
            Json out = Json::Object();
            out.Set("value", larecomp::cvar::GetFlagByName(name));
            r.Done(std::move(out));
        },
        MakeSchema("scene", "read or temporarily set a live setting for the current scene",
                   {Arg("name", ArgType::kString, true), Arg("value", ArgType::kString, false)}, "{value}"));

    RegisterCommand(
        "scene.load",
        [](const Request& req, const Responder& r) {
            const std::string path = req.args.StringOr("path", "");
            if (path.empty()) return r.Fail(ErrorCode::kBadArgs, "scene.load: 'path' is empty");
            const std::string current = larecomp::cvar::GetFlagByName("mcla_scene");
            if (current == path && CurrentSceneState() != SceneState::kOff) {
                larecomp::cvar::SetFlagByName("mcla_scene_restart", std::to_string(larecomp::settings::mcla_scene_restart.Get() + 1));  // the same scene: run it again
            } else {
                larecomp::cvar::SetFlagByName("mcla_scene", path);
            }
            Json res = Json::Object();
            res.Set("path", path);
            res.Set("state", "pending");
            if (!req.wait_settled) return r.Done(std::move(res));
            const uint64_t since = internal::TickCount();
            auto result = std::make_shared<Json>(std::move(res));
            r.Defer([since, result](const Responder& d) {
                const SceneState st = CurrentSceneState();
                if (st == SceneState::kFailed) return d.Fail(ErrorCode::kFailed, "the scene did not load: scene.get shows its diagnostics, log.tail the [scene] lines");
                if (internal::TickCount() < since + 5) return;
                std::string why;
                if (!internal::WorldSettled(since, 20, &why)) return;
                result->Set("state", SceneStateName(st));
                result->Set("path", internal::Doc().path);
                d.Done(*result);
            });
        },
        MakeSchema("scene", "arm a scene file (a path, or a name looked up next to the game and in scenes); with wait:settled, answers when it runs",
                   {Arg("path", ArgType::kString, true, "the scene file")}, "{path, state}"));

    RegisterCommand(
        "scene.get",
        [](const Request& req, const Responder& r) {
            internal::EnsureDoc();
            internal::SceneDoc& d = internal::Doc();
            if (d.path.empty()) return r.Fail(ErrorCode::kNotReady, "no scene is loaded: scene.load first");
            Json res = Json::Object();
            res.Set("path", d.path);
            res.Set("name", d.name);
            res.Set("rev", d.rev);
            res.Set("hash", d.hash);
            Json events = Json::Array();
            for (const SnapEvent& e : d.events) {
                Json j = Json::Object();
                j.Set("line", e.line);
                j.Set("t", e.t);
                j.Set("track", e.track);
                j.Set("text", e.text);
                j.Set("kind", EventKindName(e.kind));
                if (!e.car.empty()) j.Set("car", e.car);
                events.Push(std::move(j));
            }
            res.Set("events", std::move(events));
            res.Set("preset", internal::PresetJson());
            res.Set("diagnostics", DiagnosticsJson(d.diagnostics));
            Json ids = Json::Array();
            for (const std::string& id : d.car_ids) ids.Push(id);
            res.Set("car_ids", std::move(ids));
            res.Set("duration", d.duration);
            res.Set("fps", d.fps);
            if (req.args.BoolOr("text", false)) res.Set("text", d.text);
            r.Done(std::move(res));
        },
        MakeSchema("scene", "the scene as parsed: its keys with their lines, the diagnostics, the revision", {Arg("text", ArgType::kBool, false, "also the file's text")},
                   "{path, name, rev, hash, events:[{line, t, track, text, kind, car?}], diagnostics, car_ids, duration, fps, text?}"));

    RegisterCommand(
        "scene.edit",
        [](const Request& req, const Responder& r) {
            std::vector<EditOp> ops;
            const Json& list = req.args.Get("ops");
            if (list.size() == 0) return r.Fail(ErrorCode::kBadArgs, "scene.edit: 'ops' is empty");
            for (size_t i = 0; i < list.size(); ++i) {
                EditOp op;
                std::string err;
                if (!EditOpFromJson(list.at(i), &op, &err)) return r.Fail(ErrorCode::kBadArgs, "scene.edit: ops[" + std::to_string(i) + "]: " + err);
                ops.push_back(std::move(op));
            }
            const EditOutcome o = internal::ApplySceneEdit(ops, req.args.IntOr("base_rev", -1), ByOf(req), req.args.BoolOr("allow_errors", false));
            if (!o.ok) return FailFrom(r, o);
            r.Done(o.result);
        },
        MakeSchema("scene", "line-addressed text patches (insert, replace, delete, move, param); all or none; comments and layout are kept",
                   {Arg("base_rev", ArgType::kInt, true, "the revision you read (scene.get)"), Arg("ops", ArgType::kArray, true, "[{op:insert|replace|delete|move|param, ...}]"),
                    Arg("allow_errors", ArgType::kBool, false, "write a result that does not parse")},
                   "{rev, hash, lines, changed, errors}"));

    RegisterCommand(
        "scene.propose",
        [](const Request& req, const Responder& r) {
            internal::EnsureDoc();
            internal::SceneDoc& d = internal::Doc();
            if (d.path.empty()) return r.Fail(ErrorCode::kNotReady, "no scene is loaded: scene.load first");
            std::filesystem::path file;
            std::string err;
            std::string text;
            if (req.args.Has("text")) {
                const std::string stem = internal::FromUtf8(d.path).stem().string();
                const std::string rel = "proposals/" + stem + "-" + Stamp() + ".scene";
                if (!internal::ResolveWritePath(rel, true, &file, &err)) return internal::FailRefused(r, err);
                text = req.args.StringOr("text", "");
                if (!internal::WriteTextFileAtomic(file, text, &err)) return r.Fail(ErrorCode::kFailed, err);
            } else if (req.args.Has("path")) {
                if (!internal::ResolveWritePath(req.args.StringOr("path", ""), true, &file, &err)) return internal::FailRefused(r, err);
                if (!internal::ReadTextFile(file, &text)) return r.Fail(ErrorCode::kFailed, "scene.propose: cannot read " + internal::Utf8(file));
            } else {
                return r.Fail(ErrorCode::kBadArgs, "scene.propose: give 'text' or 'path'");
            }
            const ParseResult parsed = ParseSceneWithSpots(text, d.path);  // against the scene's own spots, wherever the proposal lies
            d.proposal_path = internal::Utf8(file);
            d.proposal_note = req.args.StringOr("note", "");
            ++d.proposal_serial;
            std::vector<SnapDiag> diags;
            for (const Diagnostic& dg : parsed.diagnostics) diags.push_back({dg.line, dg.severity == Severity::kError, dg.message});
            Json res = Json::Object();
            res.Set("path", d.proposal_path);
            res.Set("errors", parsed.ErrorCount());
            res.Set("diagnostics", DiagnosticsJson(diags));
            r.Done(std::move(res));
        },
        MakeSchema("scene", "a proposal beside the scene: the panel offers Preview / Accept / Reject", {Arg("note", ArgType::kString, true, "what it changes, in a line"),
                                                                                                     Arg("path", ArgType::kString, false, "a file in the scene's folder"),
                                                                                                     Arg("text", ArgType::kString, false, "or the scene's new text")},
                   "{path, errors, diagnostics}"));

    RegisterCommand(
        "scene.accept",
        [](const Request& req, const Responder& r) {
            internal::SceneDoc& d = internal::Doc();
            if (d.proposal_path.empty()) return r.Fail(ErrorCode::kNotReady, "no proposal is pending");
            std::string text;
            if (!internal::ReadTextFile(internal::FromUtf8(d.proposal_path), &text)) return r.Fail(ErrorCode::kFailed, "cannot read the proposal " + d.proposal_path);
            const ParseResult parsed = ParseSceneWithSpots(text, d.path);
            if (!parsed.Ok()) return r.Fail(ErrorCode::kFailed, "the proposal does not parse (" + std::to_string(parsed.ErrorCount()) + " error(s)): it is not accepted");
            std::string err;
            internal::WriteTextFileAtomic(internal::FromUtf8(d.path + ".bak-" + Stamp()), d.text, &err);  // the old scene, beside it
            const EditOutcome o = internal::ReplaceSceneText(text, "accept", true);
            if (!o.ok) return FailFrom(r, o);
            d.proposal_path.clear();
            d.proposal_note.clear();
            ++d.proposal_serial;
            (void)req;
            r.Done(o.result);
        },
        MakeSchema("scene", "replace the scene with the pending proposal (the old one is kept as .bak-<time>)", {}, "{rev, hash}"));

    RegisterCommand(
        "scene.reject",
        [](const Request&, const Responder& r) {
            internal::SceneDoc& d = internal::Doc();
            d.proposal_path.clear();
            d.proposal_note.clear();
            ++d.proposal_serial;
            r.Done();
        },
        MakeSchema("scene", "drop the pending proposal", {}, "{}"));

    RegisterCommand(
        "scene.undo",
        [](const Request&, const Responder& r) {
            internal::EnsureDoc();
            internal::SceneDoc& d = internal::Doc();
            if (d.undo.empty()) return r.Fail(ErrorCode::kNotReady, "nothing to undo");
            const std::string prev = d.undo.back();
            d.undo.pop_back();
            d.redo.push_back(d.text);
            const EditOutcome o = internal::ReplaceSceneText(prev, "undo", false);
            if (!o.ok) return FailFrom(r, o);
            r.Done(o.result);
        },
        MakeSchema("scene", "the file as it was before the last change (by anyone)", {}, "{rev, hash}"));

    RegisterCommand(
        "scene.redo",
        [](const Request&, const Responder& r) {
            internal::EnsureDoc();
            internal::SceneDoc& d = internal::Doc();
            if (d.redo.empty()) return r.Fail(ErrorCode::kNotReady, "nothing to redo");
            const std::string next = d.redo.back();
            d.redo.pop_back();
            d.undo.push_back(d.text);
            const EditOutcome o = internal::ReplaceSceneText(next, "undo", false);
            if (!o.ok) return FailFrom(r, o);
            r.Done(o.result);
        },
        MakeSchema("scene", "undo the undo", {}, "{rev, hash}"));

    // ---- the world
    RegisterCommand(
        "world.get",
        [](const Request&, const Responder& r) {
            const Scene* scene = LoadedScene();
            const WorldState w = scene ? EvaluateWorld(*scene, SceneTime()) : WorldState{};
            Json res = Json::Object();
            res.Set("hour", static_cast<double>(TimeOfDay_GetHour()));
            const int weather = Weather_GetIndex();
            res.Set("weather", weather);
            res.Set("weather_name", Weather_IndexToName(weather) ? Weather_IndexToName(weather) : "?");
            res.Set("timescale", w.timescale_set ? w.timescale : 1.0);
            Json sw = Json::Object();
            if (w.hour_set) sw.Set("hour", w.hour);
            if (w.weather >= 0) sw.Set("weather", Weather_IndexToName(w.weather) ? Weather_IndexToName(w.weather) : "?");
            if (w.traffic >= 0) sw.Set("traffic", w.traffic == 1);
            if (w.peds >= 0) sw.Set("peds", w.peds == 1);
            if (w.parked >= 0) sw.Set("parked", w.parked == 1);
            if (w.peds_scale >= 0.0) sw.Set("peds", w.peds_scale);
            if (w.parked_scale >= 0.0) sw.Set("parked", w.parked_scale);
            if (w.police >= 0) sw.Set("police", w.police == 1);
            if (w.timescale_set) sw.Set("timescale", w.timescale);
            res.Set("scene", std::move(sw));
            bool on = false;
            const WorldState o = SceneWorldOverride(&on);
            res.Set("override", on ? OverrideJson(o) : Json::Object());
            r.Done(std::move(res));
        },
        MakeSchema("world", "the hour, the weather and the ambient switches: the game's now, what the scene's timeline says, and the live override", {},
                   "{hour, weather, weather_name, timescale, scene:{...}, override:{...}}"));

    RegisterCommand(
        "world.set",
        [](const Request& req, const Responder& r) {
            if (req.args.BoolOr("clear", false)) {
                SceneClearWorldOverride();
                Json res = Json::Object();
                res.Set("committed", false);
                res.Set("override", Json::Object());
                return r.Done(std::move(res));
            }
            WorldState o;
            std::vector<std::pair<std::string, std::string>> keys;  // the scene-file statements, without their time
            if (const Json* h = req.args.Find("hour"); h && h->is_number()) {
                const double hour = h->AsNumber();
                if (!(hour >= 0.0 && hour < 48.0)) return r.Fail(ErrorCode::kBadArgs, "world.set: 'hour' must be 0 to 24");
                o.hour_set = true;
                o.hour = hour;
                keys.push_back({"hour", FormatNumber(hour)});
            }
            if (const Json* w = req.args.Find("weather"); w && w->is_string()) {
                const std::string name = w->AsString();
                if (name == "game") {
                    return r.Fail(ErrorCode::kBadArgs, "world.set: 'weather' game only means something committed as a key (it gives the game's weather back); use commit:true");
                }
                const int idx = WeatherIndexFromName(name);
                if (idx < 0) return r.Fail(ErrorCode::kBadArgs, "world.set: 'weather' must be nice, cloudy, stormy or foggy");
                o.weather = idx;
                keys.push_back({"weather", name});
            }
            const auto onoff = [&](const char* key, int WorldState::*field) {
                if (const Json* v = req.args.Find(key); v && v->is_bool()) {
                    o.*field = v->AsBool() ? 1 : 0;
                    keys.push_back({key, v->AsBool() ? "on" : "off"});
                }
            };
            onoff("traffic", &WorldState::traffic);
            onoff("police", &WorldState::police);
            const auto density = [&](const char* key, int WorldState::*sw, double WorldState::*scale) {
                const Json* v = req.args.Find(key);
                if (!v) return true;
                if (v->is_bool()) {
                    o.*sw = v->AsBool() ? 1 : 0;
                    keys.push_back({key, v->AsBool() ? "on" : "off"});
                } else if (v->is_number()) {
                    if (!(v->AsNumber() >= 0.0 && v->AsNumber() <= 2.0)) return false;
                    o.*scale = v->AsNumber();
                    keys.push_back({key, FormatNumber(v->AsNumber())});
                } else if (!v->is_null()) {
                    return false;
                }
                return true;
            };
            if (!density("peds", &WorldState::peds, &WorldState::peds_scale)) return r.Fail(ErrorCode::kBadArgs, "world.set: 'peds' must be true, false or a density from 0 to 2");
            if (!density("parked", &WorldState::parked, &WorldState::parked_scale)) return r.Fail(ErrorCode::kBadArgs, "world.set: 'parked' must be true, false or a density from 0 to 2");
            if (const Json* ts = req.args.Find("timescale"); ts && ts->is_number()) {
                if (!(ts->AsNumber() >= 0.0 && ts->AsNumber() <= 10.0)) return r.Fail(ErrorCode::kBadArgs, "world.set: 'timescale' must be 0 to 10");
                o.timescale_set = true;
                o.timescale = ts->AsNumber();
                keys.push_back({"timescale", FormatNumber(ts->AsNumber())});
            }
            if (keys.empty()) return r.Fail(ErrorCode::kBadArgs, "world.set: give hour, weather, traffic, peds, parked, police or timescale (or clear:true)");
            if (!req.args.BoolOr("commit", false)) {
                bool on = false;
                WorldState merged = SceneWorldOverride(&on);
                if (!on) merged = WorldState{};
                if (o.hour_set) merged.hour_set = true, merged.hour = o.hour;
                if (o.weather >= 0) merged.weather = o.weather;
                if (o.traffic >= 0) merged.traffic = o.traffic;
                if (o.police >= 0) merged.police = o.police;
                if (o.peds >= 0) merged.peds = o.peds, merged.peds_scale = -1.0;
                if (o.parked >= 0) merged.parked = o.parked, merged.parked_scale = -1.0;
                if (o.peds_scale >= 0.0) merged.peds_scale = o.peds_scale, merged.peds = -1;
                if (o.parked_scale >= 0.0) merged.parked_scale = o.parked_scale, merged.parked = -1;
                if (o.timescale_set) merged.timescale_set = true, merged.timescale = o.timescale;
                SceneSetWorldOverride(merged);
                Json res = Json::Object();
                res.Set("committed", false);
                res.Set("override", OverrideJson(merged));
                return r.Done(std::move(res));
            }
            // committed: one `at <t> <key> <value>` line each, all in one edit
            internal::EnsureDoc();
            if (internal::Doc().path.empty()) return r.Fail(ErrorCode::kNotReady, "no scene is loaded: scene.load first");
            const double t = req.args.NumberOr("t", SceneTime());
            std::vector<EditOp> ops;
            for (const auto& kv : keys) {
                EditOp op;
                op.kind = EditKind::kInsert;
                op.text = "at " + FormatSceneTime(t) + " " + kv.first + " " + kv.second;
                ops.push_back(std::move(op));
            }
            const EditOutcome out = internal::ApplySceneEdit(ops, internal::Doc().rev, ByOf(req), false);
            if (!out.ok) return FailFrom(r, out);
            SceneClearWorldOverride();
            Json res = out.result;
            res.Set("committed", true);
            res.Set("override", Json::Object());
            r.Done(std::move(res));
        },
        MakeSchema("world", "hour, weather, traffic, peds, parked, police, time scale: a live preview, or committed as scene keys",
                   {Arg("hour", ArgType::kNumber, false, "0 to 24"), Arg("weather", ArgType::kString, false, "nice, cloudy, stormy, foggy"), Arg("traffic", ArgType::kBool, false),
                    Arg("peds", ArgType::kAny, false, "true, false or a density 0 to 2"), Arg("parked", ArgType::kAny, false, "true, false or a density 0 to 2"),
                    Arg("police", ArgType::kBool, false), Arg("timescale", ArgType::kNumber, false, "0 to 10"),
                    Arg("commit", ArgType::kBool, false, "write `at <t> ...` keys instead of a live preview"), Arg("t", ArgType::kNumber, false, "the keys' time, default now"),
                    Arg("clear", ArgType::kBool, false, "drop the live preview")},
                   "{committed, override, rev?, lines?}", true));
}

void RegisterPresets() {
    RegisterCommand("preset.get", [](const Request&, const Responder& r) {
        r.Done(internal::PresetJson());
    }, MakeSchema("preset", "the active graphics preset, timeline key and label", {}, "{name, scene, label, warming, rt_available}"));

    RegisterCommand("preset.set", [](const Request& req, const Responder& r) {
        const auto edit = ParsePresetEdit(req.args, SceneTime(), false);
        if (!edit.error.empty()) return r.Fail(ErrorCode::kBadArgs, edit.error);
#if defined(LARECOMP_HAS_GPU)
        namespace gfx = mcla::gpu::gfx_preset;
        if (edit.preset && !gfx::Available(*edit.preset)) return r.Fail(ErrorCode::kRefused, std::string(gfx::kNeedsRtReason));
        if (internal::DirectorTakeBusy() || CaptureTakeActive()) return r.Fail(ErrorCode::kNotReady, "a take is running");
        if (edit.commit) {
            if (const Scene* scene = LoadedScene(); scene && scene->Duration() >= 0.0 && edit.t >= scene->Duration()) {
                return r.Fail(ErrorCode::kBadArgs, "the preset key must precede the scene's end");
            }
            const EditOutcome out = internal::InsertSceneLine(edit.Line(false), ByOf(req));
            if (!out.ok) return FailFrom(r, out);
            SceneSetPresetPreview(std::nullopt);
            Json res = out.result;
            res.Set("committed", true);
            return r.Done(std::move(res));
        }
        if (ScenePresetWarming()) return r.Fail(ErrorCode::kNotReady, "graphics presets are warming");
        if (edit.persist && !gfx::Persist(*edit.preset)) return r.Fail(ErrorCode::kFailed, "could not save the preset");
        if (!SceneSetPresetPreview(edit.preset)) return r.Fail(ErrorCode::kNotReady, "the renderer or preset warm-up is busy");
        Json res = internal::PresetJson();
        res.Set("committed", false);
        res.Set("persisted", edit.persist);
        r.Done(std::move(res));
#else
        r.Fail(ErrorCode::kNotReady, "preset.set requires the LARecomp renderer");
#endif
    }, MakeSchema("preset", "switch live or write a preset key at the playhead",
                  {Arg("name", ArgType::kString, false, "vanilla, enhanced, enhanced_rt or custom"), Arg("clear", ArgType::kBool, false, "clear the live preview"),
                   Arg("t", ArgType::kNumber, false, "scene seconds"), Arg("commit", ArgType::kBool, false, "write a key"),
                   Arg("persist", ArgType::kBool, false, "save this live selection to larecomp.toml")},
                  "{name, committed, persisted?, rev?, lines?}", true));

    RegisterCommand("preset.label", [](const Request& req, const Responder& r) {
        const auto edit = ParsePresetEdit(req.args, SceneTime(), true);
        if (!edit.error.empty()) return r.Fail(ErrorCode::kBadArgs, edit.error);
        if (edit.commit) {
            if (internal::DirectorTakeBusy() || CaptureTakeActive()) return r.Fail(ErrorCode::kNotReady, "a take is running");
            if (const Scene* scene = LoadedScene(); scene && scene->Duration() >= 0.0 && edit.t >= scene->Duration()) {
                return r.Fail(ErrorCode::kBadArgs, "the label key must precede the scene's end");
            }
            const EditOutcome out = internal::InsertSceneLine(edit.Line(true), ByOf(req));
            if (!out.ok) return FailFrom(r, out);
            SceneSetPresetLabel(std::nullopt);
            Json res = out.result;
            res.Set("committed", true);
            return r.Done(std::move(res));
        }
        SceneSetPresetLabel(edit.label);
        r.Done(internal::PresetJson());
    }, MakeSchema("preset", "show or hide the preset label live or as a scene key",
                  {Arg("on", ArgType::kBool, false), Arg("clear", ArgType::kBool, false), Arg("t", ArgType::kNumber, false),
                   Arg("commit", ArgType::kBool, false)}, "{label, committed?, rev?, lines?}", true));
}

const bool kRegistered = (RegisterScene(), true);

}  // namespace

}  // namespace mc::cinematic::director

#endif  // REXGLUE_HAS_XEO3_TARGET
