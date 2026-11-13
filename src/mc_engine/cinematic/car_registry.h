#pragma once
//
// The registry of the cars a scene knows (trailer toolkit, work package M4d): the player's own car and every car a scene spawned,
// by the id the scene gave them, with the pose each had at the last update. Pure: the game side (cars.cpp) reads each car's
// matrix and puts it in; the camera (M1b's look-at, attach and orbit) reads it out through cars.h.
//
// THE POSE. Not racer+0x470: that field is the race cars' own copy of their position and holds the 0xCDCDCDCD fill for a car
// that is not in a race (E4, round 3). The pose is the car entity's world matrix, the one the draw reads: racer+48 -> entity,
// entity+28 -> matrix, 16 floats from matrix+16, row by row: right, up, BACK, position (E4's matrix route; Racer_GetPosition
// and Racer_GetMatrixC read the same words). The car's forward is the negative of the third row.
//

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mc::cinematic {

struct CarPose {
    double position[3] = {0.0, 0.0, 0.0};
    double right[3] = {1.0, 0.0, 0.0};
    double up[3] = {0.0, 1.0, 0.0};
    double forward[3] = {0.0, 0.0, 1.0};
    double heading = 0.0;  // degrees in [0, 360), the scene's heading convention: 0 along +Z, 90 along +X
};

// `m` = right, up, back, position as 4 floats each (the w words are ignored). False when a word is not finite or the rows are
// no rotation (a forward shorter than 0.5): such a matrix is not a car's.
bool PoseFromMatrix(const float m[16], CarPose* out);

struct CarEntry {
    std::string id;
    bool is_player = false;
    uint32_t racer = 0;  // the guest address of the racer; 0 until the car exists
    bool have_pose = false;
    CarPose pose;          // the last good pose
    float matrix[16] = {};  // ... and the matrix it came from
    uint64_t pose_frame = 0;    // the update that read it
    uint64_t pose_updates = 0;  // how many good reads
};

class CarRegistry {
  public:
    // The new entry, or null when the id is taken.
    CarEntry* Add(const std::string& id, bool is_player);
    CarEntry* Find(std::string_view id);
    const CarEntry* Find(std::string_view id) const;
    bool Remove(std::string_view id);
    void Clear() { entries_.clear(); }
    size_t Size() const { return entries_.size(); }
    std::vector<std::string> Ids() const;
    // Entries in the order they were added (the pose update walks them).
    std::vector<CarEntry>& entries() { return entries_; }
    const std::vector<CarEntry>& entries() const { return entries_; }

    // Stores a read matrix as the pose of `id` at update `frame`; false when the id is unknown or the matrix is not a car's (the
    // last good pose stays).
    bool UpdatePose(std::string_view id, const float m[16], uint64_t frame);
    // The last good matrix of `id`, or false when it never had one.
    bool Matrix(std::string_view id, float out[16]) const;

  private:
    std::vector<CarEntry> entries_;
};

}  // namespace mc::cinematic
