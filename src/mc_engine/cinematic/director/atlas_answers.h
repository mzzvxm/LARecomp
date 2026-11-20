#pragma once
//
// The atlas's answers as the Director's JSON (package C, DIR-ATLAS): the `query.*` results built from a World and an Index (atlas_core.h), with the same
// shapes ports\scripts\atlas.py answers offline. Pure over the standard library and director.h's Json and Snapshot types, so atlas_answers_test builds it
// without the game. handlers_world.cpp is the game-side glue that registers the commands and reads the live world.
//
// Present only once the command layer (package A, director.h) is in the tree: without it this header and its .cpp are empty (the same rule cars.cpp
// follows for state_log.h), so the branch builds and merges in any order.
//

#if __has_include("director.h")
#define LARECOMP_ATLAS_HAS_DIRECTOR 1

#include <string>

#include "atlas_core.h"
#include "director.h"

namespace mc::cinematic::atlas {

using director::Json;

// A position argument: [x, y, z], [x, z], {x, z[, y]}, "player", "car:<id>" (a scene car, from the snapshot), "camera", or "spot:<name>[@right,up,forward]"
// (the snapshot's spots; the offset is in the spot's heading frame: right = (-cos h, 0, sin h), forward = (sin h, 0, cos h), the engine's HeadingBasis).
// `snap` may be null (then only the numeric forms work). False with `error` ("unknown spot 'gate'").
bool ResolvePosition(const Json& value, const director::Snapshot* snap, Vec3* pos, bool* has_y, std::string* error);

// A route end: a position (ResolvePosition) or an object {rail, t}.
bool ResolveRouteEnd(const Json& value, const director::Snapshot* snap, RouteEnd* end, std::string* error);

// The camera of the snapshot, or the pose an argument object gives ({pos, yaw, pitch, roll?, fov?}); false with `error` when there is none.
bool ResolveCamera(const Json* override_pose, const director::Snapshot* snap, CameraPose* pose, std::string* error);

// Each fills *result (an object) and returns true, or returns false with `error` (a bad_args message).
bool AnswerLights(const Index& index, const Json& args, const director::Snapshot* snap, Json* result, std::string* error);
bool AnswerRoad(const Index& index, const Json& args, const director::Snapshot* snap, Json* result, std::string* error);
bool AnswerRoute(const Index& index, const Json& args, const director::Snapshot* snap, Json* result, std::string* error);
void AnswerGarages(const World& world, Json* result);
// The projection part of query.visible: in_front, in_frame, screen, size_frac, dist_m, depth_m. `occluded` is added by the caller (null when unknown).
void AnswerProjection(const CameraPose& pose, Vec3 subject, double radius, Json* result);

// Json forms of the pieces.
Json LightJson(const Light& light, double dist, double reach);
Json VecJson(const Vec3& v);

}  // namespace mc::cinematic::atlas

#endif  // __has_include("director.h")
