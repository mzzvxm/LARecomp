#pragma once
//
// The car statements of a scene (trailer toolkit, work package M4b): the words after `car`, read into a command. Pure: it
// knows nothing of the game, so cars_test.cpp builds it on its own, and scene.cpp uses it to report a bad car line with its
// line number when the file is parsed.
//
//   car <id> spawn <model> at <x,y,z> [heading <deg>]      a car of that model (the game's vehicle name, vp_lam_murcielago_06)
//   car <id> spawn profile <N> at <x,y,z> [heading <deg>]  a car from garage slot N of the player's save, dressed as the owner left it
//   car <id> delete                                        the car goes (every car a scene spawned goes when it ends)
//   car player profile <N>                                 the player's own car becomes garage slot N (the old one comes back at the end)
//
// `at` takes x,y,z as one word or as three; the position is a world position (x east, z south, y up). `heading` is where the
// nose points, in degrees, as the camera's yaw: 0 along +Z, 90 along +X (the game's own Racer_Teleport angle is this plus 180:
// NativeHeadingDeg). Garage slots count from 0. `garage:N` and `slot:N` mean `profile N`; `despawn` means `delete`. The id is
// what the camera's `car:<id>` names; `player` is the player's own car and takes only `profile`.
//
// Other verbs of the car track (a path, a speed: package M4c) are not this file's: ParseCarCommand says kOtherVerb and the
// scene parser lets them through.
//

#include <cstdint>
#include <string>
#include <vector>

namespace mc::cinematic {

enum class CarVerb : uint8_t { kSpawn, kDelete, kProfile };
enum class CarSource : uint8_t { kModel, kGarage };  // spawn: a vehicle name, or a garage slot

struct CarCommand {
    std::string id;
    CarVerb verb = CarVerb::kSpawn;
    CarSource source = CarSource::kModel;
    std::string model;  // kModel: the vehicle name, lower case
    int slot = -1;      // kGarage, and the profile verb: the garage slot, from 0
    double position[3] = {0.0, 0.0, 0.0};
    double heading = 0.0;  // degrees in [0, 360): the camera's yaw convention
    bool has_heading = false;
};

enum class CarParse : uint8_t { kOk, kOtherVerb, kError };

// `words` are the words after the `car` statement (the id first). On kError `error` says what is wrong, in the scene parser's
// style; on kOtherVerb nothing is wrong, another package reads the line.
CarParse ParseCarCommand(const std::vector<std::string>& words, CarCommand* out, std::string* error);

// "p1 spawn vp_lam_murcielago_06 at (900.00, 17.50, -850.00) heading 270" for the log.
std::string DescribeCarCommand(const CarCommand& cmd);

// Degrees into [0, 360).
double NormalizeDegrees(double deg);

// The angle Racer_Teleport and the opponent loader take for a scene heading: the car's back row is (sin a, 0, cos a), so a nose
// along (sin h, cos h) needs a = h + 180.
double NativeHeadingDeg(double scene_heading);

// The scene heading of a car whose nose points along (forward_x, forward_z): atan2(x, z), in [0, 360).
double SceneHeadingDeg(double forward_x, double forward_z);

}  // namespace mc::cinematic
