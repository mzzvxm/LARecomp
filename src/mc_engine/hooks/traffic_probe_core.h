#pragma once
//
// The pure part of the traffic probe (traffic_probe.cpp): what a spawn test may do and what it may not, the maths of a pose
// (the scene's heading conventions, the point ahead of a car), the speed of a car from two samples, the pool counts, and a name
// read out of guest bytes. No guest memory, no cvars, no log: traffic_probe_test.cpp checks all of it on its own.
//
// The conventions are the toolkit's (cinematic/car_verbs.h): a car's matrix rows are right, up, back, position; the nose points
// along minus the back row; Racer_Teleport's native heading `a` gives the back row (sin a, 0, cos a); the scene's heading is the
// nose's atan2(x, z) in degrees. The ambient cars' matrix is the same Matrix44 (CHECK: the probe logs the player's and every
// car's heading by the same rule, so a convention that differs shows in a run).
//

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace mcla::traffic_probe {

inline constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------- pools

// One 52-byte descriptor of the game's pool template (the ambient manager's pool at [mgr+12] and the layout's at [[layout]+32]
// have the same shape; sub_826E0C40 takes an item, sub_826D6390 gives it back). The four counts are at +16 (free), +20
// (live), +40 (pending), +44 (used): a pool item moves free -> used when AddAmbientToLayout takes it. [V positions; the meaning
// of live / pending is [I].] +48 is kept raw (a capacity? CHECK).
struct PoolCounts {
    uint32_t free = 0;
    uint32_t live = 0;
    uint32_t pending = 0;
    uint32_t used = 0;
    uint32_t raw48 = 0;
    bool read = false;  // the descriptor was readable
    uint32_t Total() const { return free + live + pending + used; }
    // A count that is not a count (a garbage read: the pool is not where it was taken to be) is more than this.
    static constexpr uint32_t kSane = 100000;
    bool Sane() const { return read && free < kSane && live < kSane && pending < kSane && used < kSane; }
};

// ---------------------------------------------------------------- the spawn test's guard

// The calls the spawn test may make in one run of the game, whatever the cvar does.
inline constexpr uint32_t kMaxSpawnCalls = 3;
// A layout slot must be free, and at least this many (the last ones stay with the game).
inline constexpr uint32_t kMinFreeLayoutSlots = 2;
// The world must have been up for this many updates (the ambient manager and the player's car exist, the first spawns are done).
inline constexpr uint32_t kMinWorldUpdates = 120;

struct SpawnInputs {
    bool manager = false;          // the ambient manager exists
    bool player = false;           // the player's car exists with a readable pose
    bool free_roam = false;        // the game is in the Cruising state (not a race, a menu, a garage, a load)
    bool driving = false;          // the HUD updated lately and the game is not paused
    uint32_t world_updates = 0;    // updates since the manager and the player were both there
    bool layout_pool = false;      // the layout's pool for class 0 was read and its counts are sane
    uint32_t layout_free = 0;      // ... its free count
    bool type_known = false;       // the model is in the manager's type list (the list the layout's items are resolved against)
    bool position_ok = false;      // the spawn point is a finite point near the player
    uint32_t calls_so_far = 0;     // spawn calls made in this run of the game
};

enum class Refusal : uint8_t {
    kNone,
    kNoManager,
    kNoPlayer,
    kNotFreeRoam,
    kNotDriving,
    kTooSoon,
    kNoLayoutPool,
    kPoolFull,
    kUnknownType,
    kBadPosition,
    kCapReached,
};

inline const char* RefusalText(Refusal r) {
    switch (r) {
        case Refusal::kNone: return "none";
        case Refusal::kNoManager: return "the ambient manager does not exist";
        case Refusal::kNoPlayer: return "no player car with a readable pose";
        case Refusal::kNotFreeRoam: return "not free roam (a race, a menu, the garage or a load is up)";
        case Refusal::kNotDriving: return "the game is paused or the HUD is not updating";
        case Refusal::kTooSoon: return "the world has not been up for long enough";
        case Refusal::kNoLayoutPool: return "the layout's pool could not be read (or its counts are not sane)";
        case Refusal::kPoolFull: return "the layout's pool has no free slot to spare";
        case Refusal::kUnknownType: return "the model is not in the ambient type list";
        case Refusal::kBadPosition: return "the spawn point is not a plausible point near the player";
        case Refusal::kCapReached: return "the per-run cap on spawn calls is reached";
    }
    return "?";
}

// Every guard in one place, in the order a person would check them. kNone = the call may be made.
inline Refusal DecideSpawn(const SpawnInputs& in) {
    if (in.calls_so_far >= kMaxSpawnCalls) return Refusal::kCapReached;
    if (!in.manager) return Refusal::kNoManager;
    if (!in.player) return Refusal::kNoPlayer;
    if (!in.free_roam) return Refusal::kNotFreeRoam;
    if (!in.driving) return Refusal::kNotDriving;
    if (in.world_updates < kMinWorldUpdates) return Refusal::kTooSoon;
    if (!in.layout_pool) return Refusal::kNoLayoutPool;
    if (in.layout_free < kMinFreeLayoutSlots) return Refusal::kPoolFull;
    if (!in.type_known) return Refusal::kUnknownType;
    if (!in.position_ok) return Refusal::kBadPosition;
    return Refusal::kNone;
}

// The cvar is "one call per set": a value other than 0 that differs from the one already acted on is a new set; 0 re-arms.
// `done_for` is what the last act was for (0 = none). Returns true when this update is a new set; `done_for` is updated.
inline bool NewSpawnSet(int32_t cvar, int32_t& done_for) {
    if (cvar == 0) {
        done_for = 0;
        return false;
    }
    if (cvar == done_for) return false;
    done_for = cvar;
    return true;
}

// ---------------------------------------------------------------- pose maths

inline double NormalizeDegrees(double deg) {
    double d = std::fmod(deg, 360.0);
    if (d < 0.0) d += 360.0;
    if (d >= 360.0) d = 0.0;
    return d;
}

// The scene heading of a car whose matrix has this back row: the nose is minus the back row, atan2(x, z) of the nose, [0, 360).
inline double NoseHeadingDeg(double back_x, double back_z) { return NormalizeDegrees(std::atan2(-back_x, -back_z) * 180.0 / kPi); }

// The angle Racer_Teleport (and, CHECK, the layout's heading) takes for a car whose back row is (sin a, 0, cos a): [0, 360).
inline double NativeHeadingDeg(double back_x, double back_z) { return NormalizeDegrees(std::atan2(back_x, back_z) * 180.0 / kPi); }

// The signed difference of two headings in degrees, in (-180, 180]: how far `b` is turned from `a`.
inline double HeadingDelta(double a, double b) {
    double d = NormalizeDegrees(b - a);
    if (d > 180.0) d -= 360.0;
    return d;
}

struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;
};

inline bool Finite3(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

inline double Distance(const Vec3& a, const Vec3& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}
inline double DistanceXZ(const Vec3& a, const Vec3& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z)); }

// The point `dist` metres ahead of a car at `pos` whose back row is (back_x, ., back_z): along the nose, at the car's own height.
// False when the back row has no horizontal part (a car standing on its nose), then `out` is `pos`.
inline bool PointAhead(const Vec3& pos, double back_x, double back_z, double dist, Vec3* out) {
    const double len = std::sqrt(back_x * back_x + back_z * back_z);
    *out = pos;
    if (!(len > 1.0e-4) || !std::isfinite(len)) return false;
    out->x = pos.x - back_x / len * dist;
    out->z = pos.z - back_z / len * dist;
    return Finite3(*out);
}

// A spawn point is plausible: finite, not beyond a kilometre of the player (the probe puts it 5 to 100 m ahead).
inline bool PlausibleSpawnPoint(const Vec3& p, const Vec3& player) {
    return Finite3(p) && Finite3(player) && Distance(p, player) <= 1000.0;
}

// ---------------------------------------------------------------- the speed of a car and the census of who is there

// Cars come and go: the pool's slots are reused. One sample of the live list is `Begin`, one `Update` per car in it, then `End`
// says how many cars of the last sample are not in this one (gone) and how many are in this one and were not in the last (new).
// A speed needs two samples of the same address a moment apart (0.02 to 10 s); a slot reused by another car between two samples
// gives one wrong speed and is counted as the same car (the log samples twice a second at the least; CHECK).
class Census {
public:
    void Begin() {
        ++sample_;
        new_ = 0;
        gone_ = 0;
        seen_ = 0;
    }

    // Returns the speed in m/s, or -1 when the car is new or the samples are too far apart (or too close) in time.
    double Update(uint32_t id, const Vec3& pos, double now_s) {
        ++seen_;
        auto it = cars_.find(id);
        if (it == cars_.end()) {
            cars_[id] = Entry{pos, now_s, sample_, -1.0};
            ++new_;
            return -1.0;
        }
        Entry& e = it->second;
        const double dt = now_s - e.t;
        double v = -1.0;
        if (dt >= 0.02 && dt <= 10.0) {
            v = Distance(e.pos, pos) / dt;
            e.speed = v;
        }
        e.pos = pos;
        e.t = now_s;
        e.sample = sample_;
        return v;
    }

    void End() {
        for (auto it = cars_.begin(); it != cars_.end();) {
            if (it->second.sample != sample_) {
                ++gone_;
                it = cars_.erase(it);
            } else {
                ++it;
            }
        }
    }

    uint32_t New() const { return new_; }
    uint32_t Gone() const { return gone_; }
    uint32_t Seen() const { return seen_; }
    size_t Tracked() const { return cars_.size(); }
    void Clear() {
        cars_.clear();
        new_ = gone_ = seen_ = 0;
    }

private:
    struct Entry {
        Vec3 pos;
        double t = 0.0;
        uint64_t sample = 0;
        double speed = -1.0;
    };
    std::unordered_map<uint32_t, Entry> cars_;
    uint64_t sample_ = 0;
    uint32_t new_ = 0, gone_ = 0, seen_ = 0;
};

// ---------------------------------------------------------------- names

// A name read out of guest bytes: at most `max` characters up to a NUL, all printable ASCII with no space. "" when it is
// empty, too long, or holds anything else (so a pointer that is not a name prints as nothing, not as noise).
inline std::string PrintableName(const uint8_t* p, size_t max) {
    std::string s;
    for (size_t i = 0; i < max; ++i) {
        const uint8_t c = p[i];
        if (c == 0) return s;
        if (c <= 0x20 || c >= 0x7F) return std::string();
        s.push_back(static_cast<char>(c));
    }
    return std::string();  // no terminator within `max`
}

inline char LowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
inline bool EqualNoCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (LowerAscii(a[i]) != LowerAscii(b[i])) return false;
    }
    return true;
}

// The layout item's name buffer takes 32 characters (snprintf(item + 40, 32, "%s", name) in sub_826E1418), 31 and a NUL.
inline constexpr size_t kLayoutNameChars = 31;
inline bool FitsLayoutName(const std::string& name) { return !name.empty() && name.size() <= kLayoutNameChars; }

}  // namespace mcla::traffic_probe
