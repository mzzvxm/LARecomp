// A scene's ambient cars as a script of native calls: the guards (ambient_car.h), the machine of one car (car_spawn.h, AmbientCar) and the parts of the
// Director that only ambient cars need.

#include <cmath>
#include <cstdio>

#include "car_spawn.h"

namespace mc::cinematic {

const char* AmbientRefusalText(AmbientRefusal r) {
    switch (r) {
        case AmbientRefusal::kNone: return "none";
        case AmbientRefusal::kNoManager: return "the ambient manager does not exist";
        case AmbientRefusal::kNoPlayer: return "no player car on the road";
        case AmbientRefusal::kNotFreeRoam: return "not free roam (a race, a menu, the garage or a load is up)";
        case AmbientRefusal::kNotDriving: return "the game is paused";
        case AmbientRefusal::kTooSoon: return "the world has not been up for long enough";
        case AmbientRefusal::kNoLayoutPool: return "the layout's pool could not be read (or its counts are not sane)";
        case AmbientRefusal::kPoolFull: return "the layout's pool has no free slot to spare";
        case AmbientRefusal::kCapReached: return "the scene has made as many ambient cars as one scene may";
    }
    return "?";
}

bool AmbientRefusalPasses(AmbientRefusal r) {
    switch (r) {
        case AmbientRefusal::kNoManager:
        case AmbientRefusal::kNoPlayer:
        case AmbientRefusal::kNotFreeRoam:
        case AmbientRefusal::kNotDriving:
        case AmbientRefusal::kTooSoon:
        case AmbientRefusal::kNoLayoutPool:
            return true;
        case AmbientRefusal::kNone:
        case AmbientRefusal::kPoolFull:
        case AmbientRefusal::kCapReached:
            return false;
    }
    return false;
}

AmbientRefusal DecideAmbientAdd(const AmbientGate& g) {
    if (g.placed_so_far >= kAmbientMaxPerScene) return AmbientRefusal::kCapReached;
    if (!g.manager) return AmbientRefusal::kNoManager;
    if (!g.player) return AmbientRefusal::kNoPlayer;
    if (!g.free_roam) return AmbientRefusal::kNotFreeRoam;
    if (!g.not_paused) return AmbientRefusal::kNotDriving;
    if (g.world_updates < kAmbientMinWorldUpdates) return AmbientRefusal::kTooSoon;
    if (!g.layout_pool) return AmbientRefusal::kNoLayoutPool;
    if (g.layout_free < kAmbientMinFreeLayoutSlots) return AmbientRefusal::kPoolFull;
    return AmbientRefusal::kNone;
}

// ================================================================================================================ AmbientCar

void AmbientCar::Event(bool warning, std::string text) {
    events_.push_back({warning, std::move(text)});
}

void AmbientCar::Fail(const std::string& why) {
    if (failure_.empty()) failure_ = why;
    stage_ = Stage::kGone;
    waiting_ = false;
    Event(true, "car " + cmd_.id + " (ambient " + cmd_.model + "): " + why);
}

void AmbientCar::Start(const CarCommand& cmd) {
    cmd_ = cmd;
    stage_ = Stage::kAdd;
    waiting_ = false;
    want_delete_ = false;
    placed_ever_ = false;
    first_poll_ms_ = -1;
    last_wait_ = AmbientRefusal::kNone;
    layout_id_ = -1;
    manager_ = manager_at_add_ = 0;
    failure_.clear();
    events_.clear();
}

void AmbientCar::RequestDelete() {
    want_delete_ = true;
    if (stage_ == Stage::kAdd && !waiting_) stage_ = Stage::kGone;  // nothing exists in the game yet: dropped
    else if (stage_ == Stage::kPlaced) stage_ = Stage::kRemove;
    // kAdd with its call in flight: Report goes to kRemove
}

void AmbientCar::MarkLost() {
    if (stage_ != Stage::kGone && failure_.empty() && !want_delete_) failure_ = "the game no longer has the placement (the ambient manager was rebuilt)";
    stage_ = Stage::kGone;
    waiting_ = false;
}

Call AmbientCar::Poll(int64_t now_ms, const AmbientGate& gate) {
    Call none;
    if (waiting_) return none;
    switch (stage_) {
        case Stage::kAdd: {
            if (first_poll_ms_ < 0) first_poll_ms_ = now_ms;
            const AmbientRefusal r = DecideAmbientAdd(gate);
            if (r != AmbientRefusal::kNone) {
                if (!AmbientRefusalPasses(r)) {
                    Fail(std::string("not placed: ") + AmbientRefusalText(r));
                    return none;
                }
                if (now_ms - first_poll_ms_ > kAmbientGiveUpMs) {
                    Fail(std::string("gave up waiting: ") + AmbientRefusalText(r));
                    return none;
                }
                if (r != last_wait_) {
                    last_wait_ = r;
                    Event(false, "car " + cmd_.id + " (ambient " + cmd_.model + ") waits: " + AmbientRefusalText(r));
                }
                return none;
            }
            Call c;
            c.native = Native::kAddAmbient;
            c.text = cmd_.model;
            for (int i = 0; i < 3; ++i) c.position[i] = cmd_.position[i];
            c.heading_native = LayoutHeadingDeg(cmd_.heading);
            manager_at_add_ = gate.manager_addr;
            waiting_ = true;
            return c;
        }
        case Stage::kRemove: {
            if (manager_ != 0 && gate.manager_addr != manager_) {
                Event(false, "car " + cmd_.id + " (ambient " + cmd_.model + "): the ambient manager is not the one that made the placement (the world was rebuilt): it went with it, nothing is called");
                stage_ = Stage::kGone;
                return none;
            }
            Call c;
            c.native = Native::kRemoveAmbient;
            c.index = layout_id_;
            waiting_ = true;
            return c;
        }
        case Stage::kPlaced:
        case Stage::kGone:
            return none;
    }
    return none;
}

void AmbientCar::Report(const Outcome& o) {
    if (!waiting_) return;
    waiting_ = false;
    if (stage_ == Stage::kAdd) {
        if (!o.called) {
            Fail("AmbientMgr_AddAmbientToLayout was not called (the game side refused it or could not make the call: the log says why)");
            return;
        }
        const int32_t id = static_cast<int32_t>(o.ret);
        if (!o.ret_written || id < 0) {
            Fail(std::string("the game refused the placement: AmbientMgr_AddAmbientToLayout returned ") + (o.ret_written ? std::to_string(id) : std::string("nothing")) +
                 " (-1: no ambient manager, an empty name, or its pool is empty)");
            return;
        }
        layout_id_ = id;
        manager_ = manager_at_add_;
        placed_ever_ = true;
        stage_ = want_delete_ ? Stage::kRemove : Stage::kPlaced;
        Event(false, "car " + cmd_.id + " (ambient " + cmd_.model + ") is placed: layout item " + std::to_string(id) + "; the game's parked spawner makes the car");
    } else if (stage_ == Stage::kRemove) {
        stage_ = Stage::kGone;
        if (!o.called) {
            failure_ = "AmbientMgr_RemoveAmbientFromLayout was not called: layout item " + std::to_string(layout_id_) + " stays";
            Event(true, "car " + cmd_.id + " (ambient " + cmd_.model + "): " + failure_);
        }
    }
}

std::vector<CarEvent> AmbientCar::TakeEvents() {
    std::vector<CarEvent> out;
    out.swap(events_);
    return out;
}

// ================================================================================================================ Director, ambient parts

Director::AmbientEntry* Director::FindLiveAmbient(const std::string& id) {
    for (auto& a : ambient_) {
        if (!a->ended && a->id == id) return a.get();
    }
    return nullptr;
}

std::vector<Director::AmbientReader> Director::AmbientReaders() const {
    std::vector<AmbientReader> out;
    for (const auto& a : ambient_) {
        if (!a->ended && a->car.Placed()) out.push_back({a->id, a->car.layout_id()});
    }
    return out;
}

void Director::MarkAmbientLost() {
    for (auto& a : ambient_) a->car.MarkLost();
}

void Director::NoteAmbientPose(const std::string& id, const float matrix[16], uint64_t frame) {
    CarEntry* e = registry_.Find(id);
    if (!e) return;
    const bool first = !e->have_pose;
    if (!registry_.UpdatePose(id, matrix, frame)) return;
    if (!first) return;
    for (const auto& a : ambient_) {
        if (a->id != id) continue;
        const CarCommand& cmd = a->car.command();
        const CarPose& p = e->pose;
        const double dx = p.position[0] - cmd.position[0];
        const double dz = p.position[2] - cmd.position[2];
        double dh = std::fabs(p.heading - cmd.heading);
        if (dh > 180.0) dh = 360.0 - dh;
        char buf[400];
        std::snprintf(buf, sizeof(buf),
                      "car %s (ambient %s) stands at (%.2f, %.2f, %.2f) heading %.1f: the scene asked for (%.2f, %.2f, %.2f) heading %.1f (the layout angle %.1f): %.2f m off on the "
                      "ground, %.2f m in height, %.1f degrees",
                      id.c_str(), cmd.model.c_str(), p.position[0], p.position[1], p.position[2], p.heading, cmd.position[0], cmd.position[1], cmd.position[2], cmd.heading,
                      LayoutHeadingDeg(cmd.heading), std::sqrt(dx * dx + dz * dz), p.position[1] - cmd.position[1], dh);
        events_.push_back({false, buf});
        return;
    }
}

}  // namespace mc::cinematic
