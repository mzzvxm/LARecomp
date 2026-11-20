#ifndef REXGLUE_HAS_XEO3_TARGET
//
// The Director's camera commands (DIRECTOR-PROTOCOL.md "camera"): get, set, look_at, mode, and the camera keys of the scene (key.add writes a `cam` line from the
// camera as it is, key.list reads them). The free camera is the command target for placing a camera (camera.set); it is the same flight the pad flies
// (freecam_runtime.h), so an agent and a human can take turns.
//

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "../cars.h"
#include "../freecam.h"
#include "../freecam_runtime.h"
#include "../scene_runtime.h"
#include "director_internal.h"
#include "director_math.h"
#include "larecomp_log.h"
#include "settings/larecomp_settings.h"

namespace mc::cinematic::director {

namespace {

using internal::Arg;
using internal::EditOutcome;
using internal::MakeSchema;
namespace fc = mc::cinematic::freecam;

Json Vec(const double v[3]) {
    return Json::Vec3(v[0], v[1], v[2]);
}

Json PoseJson(const SnapPose& p) {
    Json j = Json::Object();
    j.Set("pos", Vec(p.pos));
    j.Set("yaw", p.yaw);
    j.Set("pitch", p.pitch);
    j.Set("roll", p.roll);
    j.Set("fov", p.fov);
    return j;
}

// The pose the next change starts from: the flight's when it has one, else the camera as the hook last saw it.
SnapPose StartPose() {
    fc::Pose f;
    if (FreecamGetPose(&f) && FreecamOverride() != CameraOverride::kGame) {
        SnapPose p;
        p.pos[0] = f.position.x, p.pos[1] = f.position.y, p.pos[2] = f.position.z;
        p.yaw = f.yaw, p.pitch = f.pitch, p.roll = f.roll, p.fov = f.fov;
        return p;
    }
    return internal::CurrentCamera().pose;
}

void Place(const SnapPose& p) {
    fc::Pose f;
    f.position = {p.pos[0], p.pos[1], p.pos[2]};
    f.yaw = p.yaw;
    f.pitch = std::clamp(p.pitch, -fc::kMaxPitch, fc::kMaxPitch);
    f.roll = p.roll;
    f.fov = std::clamp(p.fov, 5.0, 150.0);
    FreecamSetPose(f);
}

// A number as the scene file writes it: three decimals at most, trailing zeros dropped.
std::string Num(double v) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return (s.empty() || s == "-0") ? "0" : s;
}

// A target (a car by id, the player, or a point) as a world point. Cars are aimed at 0.6 m above their origin.
bool ResolveTarget(const Json& t, double out[3], std::string* why) {
    if (t.ToVec3(out)) return true;
    if (!t.is_string()) {
        *why = "a target is \"car:<id>\", \"player\" or [x, y, z]";
        return false;
    }
    const std::string text = t.AsString();
    std::string id;
    if (text == "player") id = text;
    else if (text.rfind("car:", 0) == 0 && text.size() > 4) id = text.substr(4);
    else {
        *why = "a target is \"car:<id>\", \"player\" or [x, y, z]";
        return false;
    }
    CarPose p;
    if (!GetCarPose(id, &p)) {
        *why = "car '" + id + "' has no pose (it is not in the scene, or not made yet)";
        return false;
    }
    out[0] = p.position[0];
    out[1] = p.position[1] + 0.6;
    out[2] = p.position[2];
    return true;
}

void RegisterCamera() {
    RegisterCommand(
        "camera.get",
        [](const Request&, const Responder& r) {
            const internal::CameraView v = internal::CurrentCamera();
            Json res = Json::Object();
            res.Set("mode", v.mode);
            res.Set("pose", PoseJson(v.pose));
            Json rows = Json::Object();
            rows.Set("right", Vec(v.rows[0]));
            rows.Set("up", Vec(v.rows[1]));
            rows.Set("back", Vec(v.rows[2]));
            res.Set("rows", std::move(rows));
            res.Set("near", v.near_plane);
            res.Set("far", v.far_plane);
            res.Set("aspect", v.aspect);
            res.Set("valid", v.valid);
            r.Done(std::move(res));
        },
        MakeSchema("camera", "the final camera: whose it is, its pose, its axes, the planes, the picture's aspect", {}, "{mode, pose, rows:{right, up, back}, near, far, aspect, valid}"));

    RegisterCommand(
        "camera.set",
        [](const Request& req, const Responder& r) {
            SnapPose p = StartPose();
            const Json* pose = req.args.Find("pose");
            if (pose && !pose->is_object()) return r.Fail(ErrorCode::kBadArgs, "camera.set: 'pose' must be an object");
            const auto number = [&](const char* key, double* dst) {
                const Json* v = req.args.Find(key);
                if (!v && pose) v = pose->Find(key);
                if (!v || v->is_null()) return true;
                if (!v->is_number() || !std::isfinite(v->AsNumber())) return false;
                *dst = v->AsNumber();
                return true;
            };
            const Json* pos = req.args.Find("pos");
            if (!pos && pose) pos = pose->Find("pos");
            if (pos && !pos->is_null()) {
                if (!pos->ToVec3(p.pos)) return r.Fail(ErrorCode::kBadArgs, "camera.set: 'pos' must be [x, y, z]");
            }
            if (!number("yaw", &p.yaw) || !number("pitch", &p.pitch) || !number("roll", &p.roll) || !number("fov", &p.fov)) {
                return r.Fail(ErrorCode::kBadArgs, "camera.set: yaw, pitch, roll and fov must be numbers");
            }
            if (p.fov < 5.0 || p.fov > 150.0) return r.Fail(ErrorCode::kBadArgs, "camera.set: 'fov' must be 5 to 150 degrees");
            Place(p);
            Json res = Json::Object();
            res.Set("mode", "freecam");
            res.Set("pose", PoseJson(p));
            r.Done(std::move(res));
        },
        MakeSchema("camera", "place the free camera (no key is written); fields left out stay as they are",
                   {Arg("pos", ArgType::kVec3, false, "[x, y, z]"), Arg("yaw", ArgType::kNumber, false, "degrees, 0 looks along +Z"), Arg("pitch", ArgType::kNumber, false),
                    Arg("roll", ArgType::kNumber, false), Arg("fov", ArgType::kNumber, false, "vertical degrees"), Arg("pose", ArgType::kObject, false, "the same fields as one object")},
                   "{mode:\"freecam\", pose}"));

    RegisterCommand(
        "camera.look_at",
        [](const Request& req, const Responder& r) {
            SnapPose p = StartPose();
            std::string why;
            if (const Json* from = req.args.Find("from"); from && !from->is_null()) {
                double f[3];
                if (!ResolveTarget(*from, f, &why)) return r.Fail(ErrorCode::kBadArgs, "camera.look_at: 'from': " + why);
                p.pos[0] = f[0], p.pos[1] = f[1], p.pos[2] = f[2];
            }
            double to[3];
            if (!ResolveTarget(req.args.Get("target"), to, &why)) return r.Fail(why.find("no pose") != std::string::npos ? ErrorCode::kNotReady : ErrorCode::kBadArgs, "camera.look_at: " + why);
            LookAtAngles(p.pos, to, &p.yaw, &p.pitch);
            p.roll = 0.0;
            if (const Json* fov = req.args.Find("fov"); fov && fov->is_number()) {
                if (fov->AsNumber() < 5.0 || fov->AsNumber() > 150.0) return r.Fail(ErrorCode::kBadArgs, "camera.look_at: 'fov' must be 5 to 150 degrees");
                p.fov = fov->AsNumber();
            }
            Place(p);
            Json res = Json::Object();
            res.Set("mode", "freecam");
            res.Set("pose", PoseJson(p));
            r.Done(std::move(res));
        },
        MakeSchema("camera", "aim the free camera at a target (a car is aimed at 0.6 m above its origin)",
                   {Arg("target", ArgType::kTarget, true, "\"car:p1\", \"player\" or [x, y, z]"), Arg("from", ArgType::kTarget, false, "where to stand, default where the camera is"),
                    Arg("fov", ArgType::kNumber, false)},
                   "{mode:\"freecam\", pose}"));

    RegisterCommand(
        "camera.mode",
        [](const Request& req, const Responder& r) {
            const std::string mode = req.args.StringOr("mode", "");
            if (mode == "scene") {
                FreecamSetOverride(CameraOverride::kNone);
                larecomp::cvar::SetFlagByName("mcla_freecam", "false");
            } else if (mode == "freecam") {
                FreecamSetOverride(CameraOverride::kNone);
                larecomp::cvar::SetFlagByName("mcla_freecam", "true");
            } else if (mode == "game") {
                FreecamSetOverride(CameraOverride::kGame);
                larecomp::cvar::SetFlagByName("mcla_freecam", "false");
            } else {
                return r.Fail(ErrorCode::kBadArgs, "camera.mode: 'mode' must be scene, freecam or game");
            }
            Json res = Json::Object();
            res.Set("mode", mode);
            r.Done(std::move(res));
        },
        MakeSchema("camera", "whose camera is final: the scene's, the free camera, or the game's own", {Arg("mode", ArgType::kString, true, "scene, freecam or game")}, "{mode}"));

    RegisterCommand(
        "camera.key.add",
        [](const Request& req, const Responder& r) {
            internal::EnsureDoc();
            const double t = req.args.NumberOr("t", SceneTime());
            if (!(t >= 0.0)) return r.Fail(ErrorCode::kBadArgs, "camera.key.add: 't' must be 0 or more");
            const std::string ease = req.args.StringOr("ease", "");
            if (!ease.empty() && ease != "linear" && ease != "in" && ease != "out" && ease != "inout") return r.Fail(ErrorCode::kBadArgs, "camera.key.add: 'ease' must be linear, in, out or inout");
            std::string text;
            const std::string rig = req.args.StringOr("rig", "");
            if (!rig.empty()) {
                text = "cam " + FormatSceneTime(t) + " rig " + rig;
            } else {
                const SnapPose p = StartPose();
                text = "cam " + FormatSceneTime(t) + " pos " + Num(p.pos[0]) + " " + Num(p.pos[1]) + " " + Num(p.pos[2]) + " yaw " + Num(p.yaw) + " pitch " + Num(p.pitch) + " roll " + Num(p.roll) +
                       " fov " + Num(p.fov);
            }
            if (!ease.empty()) text += " ease " + ease;
            if (req.args.BoolOr("cut", false)) text += "\ncut " + FormatSceneTime(t);
            const EditOutcome o = internal::InsertSceneLine(text, req.source == Source::kPanel ? "panel" : "agent");
            if (!o.ok) return r.Fail(o.code, o.message, o.details);
            Json res = o.result;
            Json lines = Json::Array();
            for (int line : o.lines) lines.Push(line);
            res.Set("lines", lines);
            res.Set("line", o.lines.empty() ? 0 : o.lines[0]);
            res.Set("text", text);
            r.Done(std::move(res));
        },
        MakeSchema("camera", "insert a `cam` key from the camera as it is now (plus a `cut` line), in time order", {Arg("t", ArgType::kNumber, false, "default the scene's time now"),
                                                                                                                    Arg("ease", ArgType::kString, false, "linear, in, out, inout"),
                                                                                                                    Arg("cut", ArgType::kBool, false, "also write a cut at t"),
                                                                                                                    Arg("rig", ArgType::kString, false, "a rig's words instead of a pose")},
                   "{line, lines, text, rev, hash}", true));

    RegisterCommand(
        "camera.key.list",
        [](const Request&, const Responder& r) {
            internal::EnsureDoc();
            Json keys = Json::Array();
            for (const SnapEvent& e : internal::Doc().events) {
                if (e.kind != EventKind::kCamera && e.kind != EventKind::kCut) continue;
                Json k = Json::Object();
                k.Set("line", e.line);
                k.Set("t", e.t);
                k.Set("kind", e.kind == EventKind::kCut ? "cut" : "cam");
                k.Set("text", e.text);
                keys.Push(std::move(k));
            }
            Json res = Json::Object();
            res.Set("keys", std::move(keys));
            r.Done(std::move(res));
        },
        MakeSchema("camera", "the scene's camera and cut keys", {}, "{keys:[{line, t, kind, text}]}"));
}

const bool kRegistered = (RegisterCamera(), true);

}  // namespace

}  // namespace mc::cinematic::director

#endif  // REXGLUE_HAS_XEO3_TARGET
