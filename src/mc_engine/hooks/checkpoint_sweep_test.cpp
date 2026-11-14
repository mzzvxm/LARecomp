// Check of the checkpoint sweep's bookkeeping (checkpoint_sweep_tracker.h). A program of its own: it needs
// no ReXGlue, no guest and no game, and exits 0 when every check holds.
//
//   clang++ -std=c++20 -O1 -Wall -Wextra -I src/mc_engine/hooks src/mc_engine/hooks/checkpoint_sweep_test.cpp -o checkpoint_sweep_test
//   ./checkpoint_sweep_test
//
// What it pins down:
//  1. The rules of one look: the first look and a look after a pause or a jump start afresh (a teleport
//     never sweeps through a gate), a script that looks in every update is never told anything new, a look
//     whose segment starts beyond the previous end gets that end, places that differ in thread, opcode,
//     checkpoint or racer do not share an entry, a full table gives up its oldest entry.
//  2. Several cars at one place (the race script asks the same checkpoint, from one opcode, for the player and
//     for every opponent): each car finds its own previous end, the one that ended nearest to its new start.
//  3. The point of it all, simulated with the game's own test (a segment against a gate or a circle, the
//     checkpoint test of sub_822653A8) at 30 to 360 FPS and at the script cadences the traces measured (every
//     update, 13, 20, 33 and 66 ms between looks): a car that crosses a gate is reported by the game's test
//     alone only when a look falls on the update that crosses; with the sweep every crossing is reported at the
//     first look after it and never before the car got there, a car that misses the gate or never reaches it is
//     never reported, and with one entry per place (the first version of the sweep) the cars of a race are
//     missed above the speed the game's 2 m probe covers.
// The table printed at the end is what the numbers in checkpoint_sweep.cpp's comment come from.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "checkpoint_sweep_tracker.h"

using namespace mcla::sweep;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

constexpr int64_t kT0 = 100000;  // the table treats 0 as "unused": start the clock well after it

// The production table is a global; here every check makes its own, so they live on the heap (164 KB each).
class Table {
public:
    Table() : t_(std::make_unique<LookTable>()) {}
    bool Update(uint32_t thread, uint32_t pc, uint32_t subject, uint32_t racer, const V3& start, const V3& end,
                int64_t now, V3& previous, int64_t& gap_ms) {
        return t_->Update(thread, pc, subject, racer, start, end, now, previous, gap_ms);
    }

private:
    std::unique_ptr<LookTable> t_;
};

// The table as it was before the cars of a place were told apart: one entry per (thread, opcode, subject,
// racer), replaced by every look. Kept here to show what the per-car entries change.
class LegacyTable {
public:
    bool Update(uint32_t thread, uint32_t pc, uint32_t subject, uint32_t racer, const V3& start, const V3& end,
                int64_t now, V3& previous, int64_t& gap_ms) {
        Look* look = nullptr;
        for (Look& l : looks_)
            if (l.at_ms != 0 && l.thread == thread && l.pc == pc && l.subject == subject && l.racer == racer) {
                look = &l;
                break;
            }
        if (!look) {
            look = &looks_[next_];
            next_ = (next_ + 1) % kLooks;
            look->thread = thread;
            look->pc = pc;
            look->subject = subject;
            look->racer = racer;
            look->end = end;
            look->at_ms = now;
            return false;
        }
        previous = look->end;
        gap_ms = now - look->at_ms;
        look->end = end;
        look->at_ms = now;
        if (gap_ms > kWindowMs) return false;
        const V3 jump = Sub(previous, start);
        if (!(Dot(jump, jump) <= kMaxGapM * kMaxGapM)) return false;
        return Dot(jump, Sub(end, start)) < 0.0f;
    }

private:
    static constexpr int kLooks = 256;
    Look looks_[kLooks];
    int next_ = 0;
};

// ---- the game's test: a segment (x, z) against a gate (a line checkpoint) or a circle ----

struct P2 {
    double x, z;
};

double Cross(P2 a, P2 b, P2 c) { return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x); }

// Does the segment p-q cross the segment a-b?
bool SegmentsCross(P2 p, P2 q, P2 a, P2 b) {
    const double d1 = Cross(a, b, p), d2 = Cross(a, b, q), d3 = Cross(p, q, a), d4 = Cross(p, q, b);
    return ((d1 > 0) != (d2 > 0) || d1 == 0 || d2 == 0) && ((d3 > 0) != (d4 > 0) || d3 == 0 || d4 == 0);
}

double DistToSegment(P2 c, P2 p, P2 q) {
    const double dx = q.x - p.x, dz = q.z - p.z;
    const double l2 = dx * dx + dz * dz;
    double t = l2 > 0 ? ((c.x - p.x) * dx + (c.z - p.z) * dz) / l2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    return std::hypot(c.x - (p.x + t * dx), c.z - (p.z + t * dz));
}

struct Gate {
    bool circle = false;
    P2 a{}, b{};      // a line: its two ends
    P2 centre{};      // a circle
    double radius = 0;
    bool Hit(P2 p, P2 q) const {
        return circle ? DistToSegment(centre, p, q) <= radius : SegmentsCross(p, q, a, b);
    }
};

// ---- the simulated cars and script ----

struct Car {
    P2 p0;       // where it is at t = 0
    double dx, dz;  // unit heading
    double v;    // m/s
    P2 At(double t) const { return {p0.x + dx * v * t, p0.z + dz * v * t}; }
};

struct Setup {
    double fps = 240;
    double look_ms = 33.3;   // between two looks of the script; 0 = every update
    double jitter = 0.0;     // relative jitter of an update's length (uncapped play)
    bool probe = true;       // HasHit: the segment ends 2 m ahead of the car; false: GetWhichHit, it ends at the car
    double phase_ms = 0;     // when the first look falls
    double seconds = 6.0;
    int seed = 1;
};

struct Outcome {
    int detected_game = 0;     // reported by the game's own segment, at any look
    int detected_legacy = 0;   // game or one entry per place
    int detected_new = 0;      // game or the per-car entries
    int too_early = 0;         // reported by the sweep before the car's path reached the gate
    int late_updates = 0;      // worst delay of the report after the update in which the path crossed
};

uint32_t Lcg(uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    return s;
}

// Runs the cars past the gate. Every look asks every car in turn, from one place (one thread, one opcode, one
// checkpoint), as the race script does. `crosses[i]` says whether car i's path really crosses the gate.
Outcome Run(const Gate& gate, const std::vector<Car>& cars, const std::vector<bool>& crosses, const Setup& s,
            std::vector<bool>* got_game = nullptr, std::vector<bool>* got_new = nullptr,
            std::vector<bool>* got_legacy = nullptr) {
    const size_t n = cars.size();
    std::vector<bool> g(n, false), l(n, false), w(n, false);
    std::vector<int> cross_update(n, -1);
    std::vector<int> report_update(n, -1);
    Table table;
    LegacyTable legacy;
    uint32_t rng = uint32_t(s.seed) * 2654435761u + 12345u;
    const double base_dt = 1.0 / s.fps;
    double t = 0.0, prev_t = 0.0;
    double next_look = s.phase_ms / 1000.0;
    Outcome out;
    for (int u = 1; t < s.seconds; ++u) {
        prev_t = t;
        const double j = s.jitter > 0 ? (double(Lcg(rng) >> 8) / double(1 << 24) * 2.0 - 1.0) * s.jitter : 0.0;
        t += base_dt * (1.0 + j);
        // where each car's path really crosses the gate: some update's own segment
        for (size_t i = 0; i < n; ++i) {
            const P2 p = cars[i].At(prev_t), q = cars[i].At(t);
            if (cross_update[i] < 0 && gate.Hit(p, q)) cross_update[i] = u;
        }
        const bool look = s.look_ms <= 0 || t >= next_look;
        if (!look) continue;
        if (s.look_ms > 0) next_look += s.look_ms / 1000.0;
        const int64_t now = kT0 + int64_t(std::floor(t * 1000.0));
        for (size_t i = 0; i < n; ++i) {
            const Car& c = cars[i];
            const P2 ps = c.At(prev_t), pe = c.At(t);
            const double ex = s.probe ? c.dx * 2.0 : 0.0, ez = s.probe ? c.dz * 2.0 : 0.0;
            const V3 start{float(ps.x), 18.0f, float(ps.z)};
            const V3 end{float(pe.x + ex), 18.0f, float(pe.z + ez)};
            const bool game = gate.Hit({start.x, start.z}, {end.x, end.z});
            V3 previous{};
            int64_t gap = -1;
            const bool behind_new = table.Update(1, 0x28DC, 7, 0, start, end, now, previous, gap);
            const bool sweep_new = !game && behind_new && gate.Hit({previous.x, previous.z}, {end.x, end.z});
            V3 previous2{};
            int64_t gap2 = -1;
            const bool behind_old = legacy.Update(1, 0x28DC, 7, 0, start, end, now, previous2, gap2);
            const bool sweep_old = !game && behind_old && gate.Hit({previous2.x, previous2.z}, {end.x, end.z});
            if (game) g[i] = true;
            if (game || sweep_old) l[i] = true;
            if (game || sweep_new) {
                if (!w[i]) report_update[i] = u;
                w[i] = true;
            }
        }
    }
    for (size_t i = 0; i < n; ++i) {
        out.detected_game += g[i];
        out.detected_legacy += l[i];
        out.detected_new += w[i];
        if (w[i] && !crosses[i]) ++out.too_early;
        // a report before the car's path reached the gate is the game's own probe (2 m ahead): not early
        if (w[i] && crosses[i] && cross_update[i] > 0) {
            out.late_updates = std::max(out.late_updates, report_update[i] - cross_update[i]);
        }
    }
    if (got_game) *got_game = g;
    if (got_new) *got_new = w;
    if (got_legacy) *got_legacy = l;
    return out;
}

Gate WideGate() {  // as the traced race's: a line 100 m wide across the road (the x axis is the road)
    Gate g;
    g.a = {500.0, -50.0};
    g.b = {500.0, 50.0};
    return g;
}

Car CarAlong(double x0, double z0, double v) { return Car{{x0, z0}, 1.0, 0.0, v}; }

// ---- 1. the rules of one look ----

void TestRules() {
    Table t;
    V3 prev{};
    int64_t gap = 0;
    const V3 s0{0, 0, 0}, e0{1, 0, 0};
    Check(!t.Update(1, 2, 3, 0, s0, e0, kT0, prev, gap) && gap == -1, "the first look starts afresh");

    // a script that looks in every update: the new start is the old end (or ahead of it), nothing is told
    V3 start = e0;
    V3 end{2, 0, 0};
    Check(!t.Update(1, 2, 3, 0, start, end, kT0 + 5, prev, gap) && gap == 5,
          "a look in the next update: the previous end is the new start, no sweep");
    // the probe case: the previous end is 2 m ahead of the new start
    Table p;
    p.Update(1, 2, 3, 0, {0, 0, 0}, {3, 0, 0}, kT0, prev, gap);   // car at 1, probe to 3
    Check(!p.Update(1, 2, 3, 0, {1, 0, 0}, {4, 0, 0}, kT0 + 5, prev, gap), "probe: the previous end is ahead of the new start");

    // updates skipped: the previous end is behind the new start
    Table q;
    q.Update(1, 2, 3, 0, {0, 0, 0}, {1, 0, 0}, kT0, prev, gap);
    const bool told = q.Update(1, 2, 3, 0, {6, 0, 0}, {7, 0, 0}, kT0 + 33, prev, gap);
    Check(told && prev.x == 1.0f && gap == 33, "a look that skipped 5 m of road is given the previous end");
    Check(!q.Update(1, 2, 3, 0, {7, 0, 0}, {8, 0, 0}, kT0 + 40, prev, gap), "and the entry now holds the new end");

    // a pause: older than the window
    Table a;
    a.Update(1, 2, 3, 0, {0, 0, 0}, {1, 0, 0}, kT0, prev, gap);
    Check(!a.Update(1, 2, 3, 0, {6, 0, 0}, {7, 0, 0}, kT0 + kWindowMs + 1, prev, gap), "older than the window: afresh");
    // a jump: further than kMaxGapM (a reset, a race start)
    Table b;
    b.Update(1, 2, 3, 0, {0, 0, 0}, {1, 0, 0}, kT0, prev, gap);
    Check(!b.Update(1, 2, 3, 0, {400, 0, 0}, {401, 0, 0}, kT0 + 33, prev, gap), "a jump of 400 m is a new drive");
    Check(b.Update(1, 2, 3, 0, {406, 0, 0}, {407, 0, 0}, kT0 + 66, prev, gap) && prev.x == 401.0f,
          "after the jump the car has its own entry");
    // just inside and just outside the distance limit
    Table c;
    c.Update(1, 2, 3, 0, {0, 0, 0}, {0, 0, 0}, kT0, prev, gap);
    Check(c.Update(1, 2, 3, 0, {14.9f, 0, 0}, {16, 0, 0}, kT0 + 33, prev, gap), "14.9 m behind: swept");
    Table d;
    d.Update(1, 2, 3, 0, {0, 0, 0}, {0, 0, 0}, kT0, prev, gap);
    Check(!d.Update(1, 2, 3, 0, {15.1f, 0, 0}, {16, 0, 0}, kT0 + 33, prev, gap), "15.1 m behind: not swept");
    // the previous end ahead of the new segment's direction: not behind
    Table e;
    e.Update(1, 2, 3, 0, {0, 0, 0}, {10, 0, 0}, kT0, prev, gap);
    Check(!e.Update(1, 2, 3, 0, {6, 0, 0}, {7, 0, 0}, kT0 + 10, prev, gap), "a previous end ahead of the start is not behind it");

    // places that differ do not share an entry
    Table f;
    f.Update(1, 2, 3, 0, {0, 0, 0}, {1, 0, 0}, kT0, prev, gap);
    Check(!f.Update(9, 2, 3, 0, {6, 0, 0}, {7, 0, 0}, kT0 + 33, prev, gap), "another thread");
    Check(!f.Update(1, 9, 3, 0, {6, 0, 0}, {7, 0, 0}, kT0 + 33, prev, gap), "another opcode");
    Check(!f.Update(1, 2, 9, 0, {6, 0, 0}, {7, 0, 0}, kT0 + 33, prev, gap), "another checkpoint");
    Check(!f.Update(1, 2, 3, 9, {6, 0, 0}, {7, 0, 0}, kT0 + 33, prev, gap), "another racer");
    Check(f.Update(1, 2, 3, 0, {6, 0, 0}, {7, 0, 0}, kT0 + 33, prev, gap), "the original place still has its entry");

    // NaN positions: nothing is swept and nothing breaks
    Table g;
    const float nan = std::nanf("");
    Check(!g.Update(1, 2, 3, 0, {nan, 0, 0}, {nan, 0, 0}, kT0, prev, gap), "NaN first look");
    Check(!g.Update(1, 2, 3, 0, {nan, 0, 0}, {nan, 0, 0}, kT0 + 5, prev, gap), "NaN look");
    Check(!g.Update(1, 2, 3, 0, {0, 0, 0}, {1, 0, 0}, kT0 + 10, prev, gap), "a look after NaN looks");

    // two cars of one look (the same millisecond) do not take each other's entry; a car beside another does not either
    Table two;
    two.Update(1, 2, 3, 0, {0, 0, 0}, {1, 0, 0}, kT0, prev, gap);
    Check(!two.Update(1, 2, 3, 0, {0, 0, 5}, {1, 0, 5}, kT0, prev, gap) && gap == -1,
          "a second car in the same millisecond has no previous end of its own yet");
    Check(two.Update(1, 2, 3, 0, {6, 0, 0}, {7, 0, 0}, kT0 + 33, prev, gap) && prev.x == 1.0f && prev.z == 0.0f,
          "the first car finds its own end, not the second's");
    Check(two.Update(1, 2, 3, 0, {6, 0, 5}, {7, 0, 5}, kT0 + 33, prev, gap) && prev.z == 5.0f,
          "the second car finds its own end");
    Table side;
    side.Update(1, 2, 3, 0, {0, 0, 0}, {1, 0, 0}, kT0, prev, gap);
    Check(!side.Update(1, 2, 3, 0, {6, 0, 2.2f}, {7, 0, 2.2f}, kT0 + 33, prev, gap) && gap == -1,
          "a car 2.2 m beside the previous end does not take it");
    // a car that stands (no direction): its own end, not a car 5 m away
    Table still;
    still.Update(1, 2, 3, 0, {10, 0, 0}, {10, 0, 0}, kT0, prev, gap);
    still.Update(1, 2, 3, 0, {10, 0, 5}, {10, 0, 5}, kT0, prev, gap);
    Check(!still.Update(1, 2, 3, 0, {10, 0, 0}, {10, 0, 0}, kT0 + 33, prev, gap) && gap == 33 && prev.z == 0.0f,
          "a standing car finds its own entry, and is never swept (no direction)");

    // the cars of one place fill their bucket: the next car (cars 50 m apart) takes the oldest car's way
    Table full;
    for (int i = 0; i < LookTable::kWays; ++i)
        full.Update(1, 2, 3, 0, {50.0f * float(i), 0, 0}, {50.0f * float(i) + 1, 0, 0}, kT0 + i / 4, prev, gap);  // within 4 ms
    const float nx = 50.0f * float(LookTable::kWays);
    full.Update(1, 2, 3, 0, {nx, 0, 0}, {nx + 1, 0, 0}, kT0 + 10, prev, gap);
    Check(!full.Update(1, 2, 3, 0, {6, 0, 0}, {7, 0, 0}, kT0 + 50, prev, gap), "the oldest car's entry was given up");
    Check(full.Update(1, 2, 3, 0, {nx - 50.0f + 6, 0, 0}, {nx - 50.0f + 7, 0, 0}, kT0 + 50, prev, gap),
          "the newest cars' entries are still there");
    Check(full.Update(1, 2, 3, 0, {nx + 6, 0, 0}, {nx + 7, 0, 0}, kT0 + 50, prev, gap), "and the newcomer's");

    // a race: 25 gates, 8 cars, all asked in turn at 30 Hz: no entry is lost to another (the table is not thrashed)
    Table race;
    int found = 0, asked = 0;
    for (int look = 0; look < 6; ++look) {
        for (int gate = 0; gate < 25; ++gate)
            for (int car = 0; car < 8; ++car) {
                const float x = 300.0f * float(car) + 2.0f * float(look);  // cars 300 m apart, each moving 2 m a look
                const bool s2 = race.Update(1, 0x28DC, 0xBF5F3FD0u + 0x170u * uint32_t(gate), 0, {x, 0, 0}, {x + 0.3f, 0, 0},
                                            kT0 + 33 * look + car / 8, prev, gap);
                if (look > 0) {
                    ++asked;
                    found += s2 ? 1 : 0;
                }
            }
    }
    Check(found == asked, "25 gates x 8 cars: every look of every car finds its own previous end (" + std::to_string(found) + "/" +
                              std::to_string(asked) + ")");
    // a bigger one: 40 gates x 12 cars (a bucket holds two gates' cars when they share one)
    Table big;
    int bfound = 0, basked = 0;
    for (int look = 0; look < 6; ++look)
        for (int gate = 0; gate < 40; ++gate)
            for (int car = 0; car < 12; ++car) {
                const float x = 300.0f * float(car) + 2.0f * float(look);
                const bool s2 = big.Update(1, 0x28DC, 0xBF5F3FD0u + 0x170u * uint32_t(gate), 0, {x, 0, 0}, {x + 0.3f, 0, 0},
                                           kT0 + 33 * look + car / 8, prev, gap);
                if (look > 0) {
                    ++basked;
                    bfound += s2 ? 1 : 0;
                }
            }
    Check(bfound == basked, "40 gates x 12 cars: every look of every car finds its own previous end (" + std::to_string(bfound) + "/" + std::to_string(basked) + ")");
}

// ---- 2. several cars at one place ----

void TestCarsAtOnePlace() {
    // three cars 40 m, 90 m and 200 m apart on one road at 80 m/s, one place: every look asks them in turn
    Table t;
    LegacyTable lt;
    const double v = 80.0, dt = 1.0 / 240.0;
    const double x0[3] = {0.0, -40.0, -130.0};
    int own_new = 0, own_legacy = 0, looks = 0;
    for (int u = 1; u <= 240; ++u) {
        const double tm = u * dt;
        if (u % 8 != 0) continue;  // a look every 8th update = 33 ms
        for (int i = 0; i < 3; ++i) {
            const V3 start{float(x0[i] + v * (tm - dt)), 0, 0}, end{float(x0[i] + v * tm), 0, 0};
            V3 prev{}, prev2{};
            int64_t gap = -1, gap2 = -1;
            const int64_t now = kT0 + int64_t(tm * 1000);
            const bool s = t.Update(1, 2, 3, 0, start, end, now, prev, gap);
            const bool s2 = lt.Update(1, 2, 3, 0, start, end, now, prev2, gap2);
            if (u > 8) {
                ++looks;
                own_new += s ? 1 : 0;
                own_legacy += s2 ? 1 : 0;
                // the previous end of this car is where this car was 7 updates ago
                if (s) Check(std::fabs(prev.x - float(x0[i] + v * (tm - 8 * dt))) < 0.01f, "each car gets its own previous end");
            }
        }
    }
    Check(own_new == looks, "three cars at one place: every look of every car is stretched back to the car's own previous end");
    Check(own_legacy == 0, "one entry per place: no look of the three cars is stretched (they are further apart than 15 m)");

    // cars 6 m apart (a pack) and the same speed: each finds its own end, never the neighbour's
    Table pk;
    int wrong = 0, total = 0;
    for (int u = 1; u <= 480; ++u) {
        if (u % 8 != 0) continue;
        const double tm = u * dt;
        for (int i = 0; i < 4; ++i) {
            const double x = -6.0 * i + 70.0 * tm;
            const V3 start{float(x - 70.0 * dt), 0, 0}, end{float(x), 0, 0};
            V3 prev{};
            int64_t gap = -1;
            if (pk.Update(1, 2, 3, 0, start, end, kT0 + int64_t(tm * 1000), prev, gap) && u > 8) {
                ++total;
                if (std::fabs(prev.x - float(x - 70.0 * 8 * dt)) > 0.01f) ++wrong;
            }
        }
    }
    Check(total > 100 && wrong == 0, "a pack 6 m apart: no car takes a neighbour's end");

    // a pack side by side and nose to tail: 4 cars at 100 m/s, 2.5 m apart across and 5 m along, looks every 33 ms
    Table pk2;
    int wrong2 = 0, total2 = 0;
    const double v2 = 100.0;
    for (int u = 1; u <= 480; ++u) {
        if (u % 8 != 0) continue;
        const double tm = u * dt;
        for (int i = 0; i < 4; ++i) {
            const double x = -5.0 * (i / 2) + v2 * tm, z = 2.5 * (i % 2);
            const V3 start{float(x - v2 * dt), 0, float(z)}, end{float(x), 0, float(z)};
            V3 prev{};
            int64_t gap = -1;
            if (pk2.Update(1, 2, 3, 0, start, end, kT0 + int64_t(tm * 1000), prev, gap) && u > 8) {
                ++total2;
                if (std::fabs(prev.x - float(x - v2 * 8 * dt)) > 0.01f || std::fabs(prev.z - float(z)) > 0.01f) ++wrong2;
            }
        }
    }
    Check(total2 > 100 && wrong2 == 0, "a pack across and along (2.5 m, 5 m) at 100 m/s: no car takes a neighbour's end");
}

// ---- 3. the simulation ----

struct Row {
    double fps;
    double look_ms;
    double v;
    bool probe;
    int cars, game, legacy, sweep;
};
std::vector<Row> g_rows;

void TestSingleCar() {
    const Gate gate = WideGate();
    const double fpss[] = {30, 60, 120, 144, 240, 360};
    const double looks[] = {0, 13, 20, 33.3, 66};
    const double speeds[] = {15, 40, 60, 80, 100, 120};
    for (double fps : fpss)
        for (double look : looks)
            for (double v : speeds)
                for (bool probe : {false, true}) {
                    // the look gap must be one the sweep was built for: at most the window, at most 15 m of road
                    const double gap_m = v * std::max(look / 1000.0, 1.0 / fps);
                    int cars = 0, game = 0, legacy = 0, sweep = 0, early = 0, late = 0;
                    for (int phase = 0; phase < 12; ++phase) {
                        Setup s;
                        s.fps = fps;
                        s.look_ms = look;
                        s.probe = probe;
                        s.phase_ms = phase * 2.7;
                        s.seed = phase + 1;
                        s.jitter = (phase % 3 == 0) ? 0.15 : 0.0;
                        // one car, a different lateral place on the gate each time, crossing near x = 500
                        const Car car = CarAlong(500.0 - v * 2.0 - 3.0 * phase, -40.0 + 7.0 * phase, v);
                        const Outcome o = Run(gate, {car}, {true}, s);
                        ++cars;
                        game += o.detected_game;
                        legacy += o.detected_legacy;
                        sweep += o.detected_new;
                        early += o.too_early;
                        late = std::max(late, o.late_updates);
                    }
                    const std::string tag = "single car " + std::to_string(int(fps)) + " FPS, looks " + std::to_string(look) +
                                            " ms, " + std::to_string(int(v)) + " m/s" + (probe ? ", probe" : ", no probe");
                    Check(early == 0, tag + ": reported although the path never crossed");
                    if (gap_m <= 14.0) {
                        Check(sweep == cars, tag + ": every crossing reported (" + std::to_string(sweep) + "/" + std::to_string(cars) + ")");
                    }
                    Check(sweep >= game, tag + ": the sweep never reports less than the game");
                    Check(sweep >= legacy, tag + ": nor less than one entry per place");
                    // the report comes at the first look after the crossing: within the look gap (in updates) + 1
                    const double gap_updates = (look <= 0 ? 1.0 : look / 1000.0 * fps * 1.2) + 2;
                    Check(late <= gap_updates, tag + ": reported at the first look after the crossing");
                    if (look == 33.3 && (fps == 60 || fps == 144 || fps == 240) && v == 80 && !probe)
                        g_rows.push_back({fps, look, v, probe, cars, game, legacy, sweep});
                }
}

void TestRaceField() {
    // a race: 8 cars, speeds 40 to 110 m/s, each starting a different distance before the gate, asked in turn
    // from one place; half of them also pass beside the gate's end or never reach it
    const Gate gate = WideGate();
    const double fpss[] = {60, 120, 144, 240};
    for (double fps : fpss)
        for (bool probe : {false, true}) {
            int cars = 0, game = 0, legacy = 0, sweep = 0, early = 0;
            for (int phase = 0; phase < 40; ++phase) {
                Setup s;
                s.fps = fps;
                s.look_ms = 33.3;
                s.probe = probe;
                s.phase_ms = phase * 0.8;
                s.seed = 100 + phase;
                s.jitter = (phase % 2) ? 0.1 : 0.0;
                std::vector<Car> field;
                std::vector<bool> crosses;
                for (int i = 0; i < 8; ++i) {
                    const double v = 40.0 + 10.0 * i;
                    const double lateral = -42.0 + 12.0 * i + (phase % 5);
                    field.push_back(CarAlong(500.0 - v * (1.0 + 0.25 * i) - 4.0 * (phase % 7), lateral, v));
                    crosses.push_back(true);
                }
                // cars that miss: 8 m beside either end of the gate, one that stands on the grid, one that drives parallel to it
                field.push_back(CarAlong(380.0, 58.0, 90.0));
                crosses.push_back(false);
                field.push_back(CarAlong(380.0, -58.0, 100.0));
                crosses.push_back(false);
                field.push_back(CarAlong(380.0, 0.0, 0.0));  // the grid: stands still 120 m before the gate
                crosses.push_back(false);
                field.push_back(Car{{400.0, -30.0}, 0.0, 1.0, 80.0});  // drives parallel to the gate, 100 m before it
                crosses.push_back(false);
                std::vector<bool> g, w, l;
                const Outcome o = Run(gate, field, crosses, s, &g, &w, &l);
                // count only the cars that cross
                for (size_t i = 0; i < field.size(); ++i) {
                    if (!crosses[i]) continue;
                    ++cars;
                    game += g[i];
                    legacy += l[i];
                    sweep += w[i];
                }
                early += o.too_early;
            }
            const std::string tag = "race field 8+4 cars at " + std::to_string(int(fps)) + " FPS" + (probe ? " (probe)" : " (no probe)");
            Check(early == 0, tag + ": a car that misses the gate was reported");
            Check(sweep == cars, tag + ": every car that crossed was reported (" + std::to_string(sweep) + "/" + std::to_string(cars) + ")");
            Check(sweep >= legacy && legacy >= game, tag + ": game <= one entry per place <= per-car entries");
            if (fps >= 120) Check(sweep > game, tag + ": at this frame rate the game's own test missed some");
            if (!probe && fps >= 120) Check(sweep > legacy, tag + ": and one entry per place did not rescue them all");
            std::printf("  %-40s crossings %4d: game %4d (%5.1f%%)  one entry per place %4d (%5.1f%%)  per-car entries %4d (%5.1f%%)\n",
                        tag.c_str(), cars, game, 100.0 * game / cars, legacy, 100.0 * legacy / cars, sweep, 100.0 * sweep / cars);
        }
}

void TestCircle() {
    // a race circle (radius 30): the game's own test is enough at every rate, the sweep adds nothing and takes nothing
    Gate c;
    c.circle = true;
    c.centre = {500.0, 0.0};
    c.radius = 30.0;
    int cars = 0, game = 0, sweep = 0, early = 0;
    for (double fps : {60.0, 144.0, 240.0, 360.0})
        for (double v : {20.0, 60.0, 110.0})
            for (int phase = 0; phase < 20; ++phase) {
                Setup s;
                s.fps = fps;
                s.look_ms = 33.3;
                s.phase_ms = phase * 1.5;
                s.seed = 7 + phase;
                s.seconds = 12.0;
                const Car car = CarAlong(500.0 - 150.0, -25.0 + 2.5 * phase, v);
                const Outcome o = Run(c, {car}, {true}, s);
                // a car that passes 30 to 56 m beside the centre misses the circle
                const Car miss = CarAlong(500.0 - 150.0, 31.0 + 1.0 * phase, v);
                const Outcome m = Run(c, {miss}, {false}, s);
                ++cars;
                game += o.detected_game;
                sweep += o.detected_new;
                early += m.too_early;
            }
    Check(game == cars && sweep == cars, "a circle of radius 30 is found by the game's own test at every rate, with or without the sweep");
    Check(early == 0, "a car that passes the circle is not reported");
}

}  // namespace

int main() {
    TestRules();
    TestCarsAtOnePlace();
    std::printf("single car, one place, looks every 33.3 ms, GetWhichHit-style segment (no probe), 80 m/s: gate crossings reported\n");
    TestSingleCar();
    for (const Row& r : g_rows)
        std::printf("  %3.0f FPS: game %3d / %3d  one entry per place %3d  per-car entries %3d\n", r.fps, r.game, r.cars, r.legacy, r.sweep);
    std::printf("race field (8 cars crossing a 100 m gate, 4 that do not; one place for all; looks every 33.3 ms):\n");
    TestRaceField();
    TestCircle();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
