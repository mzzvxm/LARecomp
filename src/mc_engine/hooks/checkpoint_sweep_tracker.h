#pragma once
//
// The bookkeeping of checkpoint_sweep.cpp, without the game: where each place in a script that asks "did
// the car cross this since my last look" had its segment end the last time, and whether the segment of this
// look has to be stretched back to there. Standard library only; checkpoint_sweep_test.cpp checks it.
//
// A "place" is a script thread and the opcode it asks from, with the checkpoint or trigger it asks about.
// One place can ask for several cars: the race script's loop asks the same checkpoint, from one opcode, for
// the player and for every opponent in turn (the race trace of 2026-11-12: three cars, one thread, one
// opcode, the same circle or gate, different positions within the same update). A table with one entry per
// place gives the second car the first car's end, which is 20 to 200 m away, and so no look is ever
// stretched in a race. So the entries of a place are told apart by where they ended. A car's previous end
// is on the road it drove, a few metres behind (or, with the game's 2 m probe, just ahead of) the start of
// its new segment, in line with the segment's direction; another car's is not in line unless it drove the
// same line, and then it is a car length or more away. A look therefore takes the entry of its place
//  - that is within kMaxGapM of the start of its segment, and within kLateralM of the straight line through
//    the segment (a car beside it is not the same car),
//  - that was not refreshed in the same millisecond (the cars of one look are asked one after the other
//    within one update; a car's own previous end comes from an earlier update),
//  - and that ended nearest to the start.
// A look that no entry of its place fits (a first look, a teleport, a car that was not asked about for more
// than the window) makes an entry of its own.
//
// Nothing here changes what a script that looks in every update is told: its previous end is then ahead of
// the new start (or on it), and Update() answers false.

#include <cstdint>

namespace mcla::sweep {

struct V3 {
    float x, y, z;
};

inline float Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Sub(const V3& a, const V3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

// A script's looks were 13 to 33 ms apart in the runs that measured it (script_input.cpp, MARKER3), so 100 ms
// is three times the longest; the fastest car covers about 100 m/s, i.e. 10 m in 100 ms, and 15 m leaves
// room for the car's own segment on top. A previous look older than that, or further away, is not the same
// drive.
constexpr int64_t kWindowMs = 100;
constexpr float kMaxGapM = 15.0f;
// How far the previous end of the same car can be from the straight line through its new segment: a car
// turns a few degrees in 33 ms, i.e. a few centimetres over the 2 to 4 m between the two ends; a car
// driving beside it is two metres or more away. A segment shorter than kStillM (a car that stands) has no
// direction, and its previous end has to be within kLateralM of its start.
constexpr float kLateralM = 1.0f;
constexpr float kStillM = 0.01f;

// A place that asks, and where its segment ended the last time.
struct Look {
    uint32_t thread = 0;
    uint32_t pc = 0;
    uint32_t subject = 0;  // the checkpoint or the trigger
    uint32_t racer = 0;    // 0 where the test does not say (the checkpoint test)
    V3 end{};
    int64_t at_ms = 0;  // 0 = unused
};

// The entries, in buckets by the place's key: a race asks one place for every checkpoint that is still open
// for every car (25 gates and 8 cars are 200 keys' worth of entries, asked in a loop at 30 Hz), so a look
// must not scan a table of all of them. All the cars of one place land in one bucket; other places that
// share the bucket by chance just take some of its ways. Not thread safe: the caller holds its lock.
class LookTable {
public:
    static constexpr int kBuckets = 512;
    static constexpr int kWays = 24;  // the cars of two places that share a bucket: 12 each (8 in an offline race, up to 12 online)

    // This look's segment is [start, end] at `now_ms`. Returns true when it begins beyond the previous end
    // of the car this place asked about (the previous end is behind `start`, within kMaxGapM, and not older
    // than kWindowMs); `previous` is that end and `gap_ms` the time since that look. Stores `end` as the
    // car's new previous end either way. `gap_ms` is -1 for a look that fits no entry.
    bool Update(uint32_t thread, uint32_t pc, uint32_t subject, uint32_t racer, const V3& start,
                const V3& end, int64_t now_ms, V3& previous, int64_t& gap_ms) {
        Look* bucket = looks_[Bucket(thread, pc, subject, racer)];
        int best = -1;
        float best_d2 = 0.0f;
        int slot = -1;  // where a new entry goes: an unused one, else the oldest expired one, else the oldest
        const V3 dir = Sub(end, start);
        const float len2 = Dot(dir, dir);
        for (int i = 0; i < kWays; ++i) {
            Look& l = bucket[i];
            const bool live = l.at_ms != 0 && now_ms - l.at_ms <= kWindowMs;
            if (!live) {
                if (slot < 0 || (bucket[slot].at_ms != 0 && (l.at_ms == 0 || l.at_ms < bucket[slot].at_ms))) slot = i;
                continue;
            }
            if (l.thread != thread || l.pc != pc || l.subject != subject || l.racer != racer) continue;
            if (l.at_ms == now_ms) continue;  // another car of this very look
            const V3 d = Sub(l.end, start);
            const float d2 = Dot(d, d);
            if (!(d2 <= kMaxGapM * kMaxGapM)) continue;
            // in line with the segment (or, for a car that stands, at its start)
            float off2 = d2;
            if (len2 > kStillM * kStillM) {
                const float along = Dot(d, dir) / len2;
                const V3 lateral{d.x - dir.x * along, d.y - dir.y * along, d.z - dir.z * along};
                off2 = Dot(lateral, lateral);
            }
            if (!(off2 <= kLateralM * kLateralM)) continue;
            if (best < 0 || d2 < best_d2) {
                best = i;
                best_d2 = d2;
            }
        }
        gap_ms = -1;
        if (best >= 0) {
            Look& l = bucket[best];
            previous = l.end;
            gap_ms = now_ms - l.at_ms;
            l.end = end;
            l.at_ms = now_ms;
            // Behind the game's own segment: the previous end is on the far side of `start`.
            return Dot(Sub(previous, start), Sub(end, start)) < 0.0f;
        }
        if (slot < 0) {  // every way is live: the one that was refreshed the longest ago goes
            slot = 0;
            for (int i = 1; i < kWays; ++i)
                if (bucket[i].at_ms < bucket[slot].at_ms) slot = i;
        }
        Look& l = bucket[slot];
        l.thread = thread;
        l.pc = pc;
        l.subject = subject;
        l.racer = racer;
        l.end = end;
        l.at_ms = now_ms;
        return false;
    }

private:
    static uint32_t Bucket(uint32_t thread, uint32_t pc, uint32_t subject, uint32_t racer) {
        uint32_t h = thread * 0x9E3779B1u;
        h = (h ^ (h >> 15)) + pc * 0x85EBCA6Bu;
        h = (h ^ (h >> 13)) + subject * 0xC2B2AE35u;
        h = (h ^ (h >> 16)) + racer * 0x27D4EB2Fu;
        h ^= h >> 15;
        h *= 0x2C1B3C6Du;
        h ^= h >> 12;
        return h % kBuckets;
    }

    Look looks_[kBuckets][kWays];
};

}  // namespace mcla::sweep
