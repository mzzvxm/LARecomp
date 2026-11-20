#pragma once
//
// The scene's cars in the game (trailer toolkit, work packages M4b and M4d): `car <id> spawn / delete / profile` of a scene file made
// real through the script natives (car_spawn.h has the order of the calls, car_verbs.h the statements), and the registry of the
// poses of the player and of those cars for the camera (car_registry.h).
//
// Everything is `.debug_only()` through the scene's own cvar (mcla_scene) and inert unless a scene with car keys runs: TickCars costs
// one flag test an update otherwise.
//
// FOR THE CAMERA (M1b's look-at, attach and orbit): include this header and call GetCarPose("p1", &pose), or hand the camera's
// CarResolver CarMatrixById (camera_runtime.h: `SetCarResolver(&mc::cinematic::CarMatrixById)`; cars.cpp does that itself when
// camera_runtime.h is in the build). Both read the car's entity matrix as it is NOW (the camera hook runs after the physics of the
// update), and fall back to the last pose of the last update when the entity cannot be read this instant. An id that is not a
// car of the scene is false. "player" is always known.
//

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "car_registry.h"

namespace mc::cinematic {

// Once per game update on the game thread (Patch_DeltaTimePre), after TickScene: makes the one native call the scene's cars are due
// (a spawn step, a delete, the player's garage dressing), at a point where a car is on the road, the game is not paused and the
// opponent manager exists; reads the pose of every car that exists. One flag test when no scene uses cars.
void TickCars();

// The pose of the car a scene calls `id` ("player" for the player's own car). False when there is no such car, or no pose yet
// (a car is there from "car <id> is ready" in the log, about 4 s after its spawn key).
bool GetCarPose(std::string_view id, CarPose* out);

// The same as the world matrix as the game stores it, row by row: right, up, BACK, position (16 floats): the shape of M1b's
// CarResolver (`bool (*)(const char* id, float matrix[16])`).
bool CarMatrixById(const char* id, float matrix[16]);

// Cars of the scene known now (ready or not), without the player.
size_t SceneCarCount();

// ---------------------------------------------------------------------------------------------------------------- the Director's handles (src/mc_engine/cinematic/director)
// Game thread only.

// One car as the Director shows it: the scene's cars and "player".
struct SceneCarInfo {
    std::string id;
    std::string model;  // the vehicle name, "profile N" for a garage car, "player"
    double position[3] = {0.0, 0.0, 0.0};
    double heading = 0.0;  // the scene's convention
    bool ready = false;    // its pose has been read
    bool on_path = false;
    bool brain = false;    // its path is driven by the game's own AI (cannot be scrubbed)
    std::string route;     // the route file of its path
};
std::vector<SceneCarInfo> ListSceneCars();

// Puts a car there at the next update that may call natives (Racer_Teleport; speed 0). 0: asked for; 1: no such car; 2: the car is not made yet.
int PlaceCar(std::string_view id, const double position[3], double heading, bool has_heading);

// The game's vehicle names (its car table), in table order.
std::vector<std::string> ListCarModels();

// Every car key due by now was handed to the cars and every scene car has a readable pose: what the ready gate waits for. `waiting_for` says what is missing.
bool SceneCarsSettled(std::string* waiting_for = nullptr);

// The routes the scene's `path` keys name, as read (x, y, z, speed per point; y is 0 for a route with no heights).
struct SceneRouteInfo {
    std::string name;
    std::string car;
    bool kinematic = true;
    int line = 0;
    std::vector<std::array<double, 4>> points;
};
std::vector<SceneRouteInfo> ListSceneRoutes();

}  // namespace mc::cinematic
