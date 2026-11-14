// The registry of a scene's cars: see car_registry.h.

#include "car_registry.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "car_verbs.h"

namespace mc::cinematic {

namespace {

bool IsWWord(int i) { return (i & 3) == 3; }

}  // namespace

bool MatrixHasBadW(const float m[16]) {
    for (int i = 3; i < 16; i += 4) {
        if (!std::isfinite(m[i])) return true;
    }
    return false;
}

std::string MatrixFault(const float m[16]) {
    static const char* const kRow[4] = {"right", "up", "back", "position"};
    static const char kAxis[3] = {'x', 'y', 'z'};
    for (int i = 0; i < 16; ++i) {
        if (IsWWord(i) || std::isfinite(m[i])) continue;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "word %d (%s %c) is not finite (%g)", i, kRow[i / 4], kAxis[i % 4], static_cast<double>(m[i]));
        return buf;
    }
    const double fl = std::sqrt(static_cast<double>(m[8]) * m[8] + static_cast<double>(m[9]) * m[9] + static_cast<double>(m[10]) * m[10]);
    if (fl < 0.5) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "the back row is %.2f long (a car's is 1)", fl);
        return buf;
    }
    return std::string();
}

bool PoseFromMatrix(const float m[16], CarPose* out) {
    for (int i = 0; i < 16; ++i) {
        if (IsWWord(i)) continue;  // not kept by the game: see the header
        if (!std::isfinite(m[i])) return false;
    }
    CarPose p;
    for (int k = 0; k < 3; ++k) {
        p.right[k] = m[k];
        p.up[k] = m[4 + k];
        p.forward[k] = -static_cast<double>(m[8 + k]);
        p.position[k] = m[12 + k];
    }
    const double fl = std::sqrt(p.forward[0] * p.forward[0] + p.forward[1] * p.forward[1] + p.forward[2] * p.forward[2]);
    if (fl < 0.5) return false;
    p.heading = SceneHeadingDeg(p.forward[0], p.forward[2]);
    *out = p;
    return true;
}

CarEntry* CarRegistry::Add(const std::string& id, bool is_player) {
    if (Find(id)) return nullptr;
    CarEntry e;
    e.id = id;
    e.is_player = is_player;
    entries_.push_back(std::move(e));
    return &entries_.back();
}

CarEntry* CarRegistry::Find(std::string_view id) {
    for (CarEntry& e : entries_) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

const CarEntry* CarRegistry::Find(std::string_view id) const {
    for (const CarEntry& e : entries_) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

bool CarRegistry::Remove(std::string_view id) {
    const auto it = std::find_if(entries_.begin(), entries_.end(), [&](const CarEntry& e) { return e.id == id; });
    if (it == entries_.end()) return false;
    entries_.erase(it);
    return true;
}

std::vector<std::string> CarRegistry::Ids() const {
    std::vector<std::string> ids;
    ids.reserve(entries_.size());
    for (const CarEntry& e : entries_) ids.push_back(e.id);
    return ids;
}

bool CarRegistry::UpdatePose(std::string_view id, const float m[16], uint64_t frame) {
    CarEntry* e = Find(id);
    if (!e) return false;
    CarPose p;
    if (!PoseFromMatrix(m, &p)) return false;
    e->pose = p;
    std::memcpy(e->matrix, m, sizeof(e->matrix));
    e->matrix[3] = e->matrix[7] = e->matrix[11] = 0.0f;  // the w words are garbage at times (see PoseFromMatrix): the camera gets clean ones
    e->matrix[15] = 1.0f;
    e->have_pose = true;
    e->pose_frame = frame;
    ++e->pose_updates;
    return true;
}

bool CarRegistry::Matrix(std::string_view id, float out[16]) const {
    const CarEntry* e = Find(id);
    if (!e || !e->have_pose) return false;
    std::memcpy(out, e->matrix, sizeof(e->matrix));
    return true;
}

}  // namespace mc::cinematic
