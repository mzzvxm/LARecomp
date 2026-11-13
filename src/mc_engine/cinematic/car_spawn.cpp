// The scene's cars as a script of native calls: see car_spawn.h.

#include "car_spawn.h"

#include <algorithm>
#include <cstdio>

namespace mc::cinematic {

const char* NativeName(Native native) {
    switch (native) {
        case Native::kNone: return "none";
        case Native::kLookupCar: return "LookupCar";
        case Native::kHostCarName: return "car table name";
        case Native::kNumProfiles: return "LocalOptions_GetNumValidVehicleProfiles";
        case Native::kCurrentProfile: return "LocalOptions_GetCurrentVehicleProfileIndex";
        case Native::kGetProfile: return "LocalOptions_GetVehicleProfile";
        case Native::kCreateOpponent: return "OpponentManager_CreateOpponent";
        case Native::kLoadOpponent: return "OpponentManager_LoadOpponent";
        case Native::kLoadOpponentByConfig: return "OpponentManager_LoadOpponentByCarConfig";
        case Native::kAddOpponent: return "OpponentManager_AddOpponentToWorld";
        case Native::kGetLoadState: return "Racer_GetLoadState";
        case Native::kIsCarStreamed: return "Racer_IsCarStreamed";
        case Native::kActivate: return "Racer_Activate";
        case Native::kLockStreaming: return "Racer_SetStreamingLockedIn";
        case Native::kTeleport: return "Racer_Teleport";
        case Native::kGetPosition: return "Racer_GetPosition";
        case Native::kDeleteOpponent: return "OpponentManager_DeleteOpponent";
        case Native::kChangeCarByConfig: return "Racer_ChangeCarByConfig";
    }
    return "?";
}

namespace {

constexpr uint32_t kLoadStateStreamed = 3;  // Racer_GetLoadState: 0 until the add, then 3 (E4)

std::string Fmt(const char* format, int a) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), format, a);
    return buf;
}

}  // namespace

// ================================================================================================================ Spawner

void Spawner::Start(const CarCommand& cmd) {
    cmd_ = cmd;
    stage_ = cmd.source == CarSource::kModel ? Stage::kLookup : Stage::kNumProfiles;
    waiting_ = false;
    not_before_ = 0;
    added_ = false;
    want_delete_ = false;
    name_ptr_ = config_ = opp_ = 0;
    index_ = -1;
    failure_.clear();
}

void Spawner::RequestDelete() {
    want_delete_ = true;
    // Nothing exists in the game before the create: the spawn is simply dropped.
    if (!waiting_ && stage_ <= Stage::kGetProfile) stage_ = Stage::kGone;
}

void Spawner::MarkLost() {
    if (stage_ != Stage::kDeleting && stage_ != Stage::kGone && failure_.empty()) failure_ = "the game no longer has the car (the world was rebuilt)";
    stage_ = Stage::kGone;
    waiting_ = false;
}

void Spawner::Fail(const std::string& why) {
    if (failure_.empty()) failure_ = why;
    if (added_) {
        // In the world: give the node back through the delete the game knows.
        want_delete_ = true;
        stage_ = Stage::kDelete;
        not_before_ = now_;
    } else {
        stage_ = Stage::kGone;
    }
}

const char* Spawner::StageName() const {
    switch (stage_) {
        case Stage::kLookup: return "lookup";
        case Stage::kHostName: return "name";
        case Stage::kNumProfiles: return "garage count";
        case Stage::kGetProfile: return "garage profile";
        case Stage::kCreate: return "create";
        case Stage::kLoad: return "load";
        case Stage::kAdd: return "add";
        case Stage::kStreamState: return "streaming";
        case Stage::kStreamed: return "streamed";
        case Stage::kActivate: return "activate";
        case Stage::kLock: return "lock";
        case Stage::kTeleport: return "place";
        case Stage::kVerify: return "verify";
        case Stage::kReady: return "ready";
        case Stage::kDelete: return "delete";
        case Stage::kDeleting: return "deleting";
        case Stage::kGone: return "gone";
    }
    return "?";
}

Call Spawner::Poll(int64_t now) {
    now_ = now;
    Call c;
    if (waiting_ || stage_ == Stage::kGone || now < not_before_) return c;

    if ((stage_ == Stage::kStreamState || stage_ == Stage::kStreamed) && now - added_ms_ > timing_.stream_timeout_ms) {
        Fail(Fmt("the car was not streamed in %d s after it was added to the world", static_cast<int>(timing_.stream_timeout_ms / 1000)));
        if (stage_ == Stage::kGone) return c;
    }
    if (stage_ == Stage::kReady && want_delete_ && now - ready_ms_ >= timing_.delete_hold_ms) stage_ = Stage::kDelete;

    c.racer = opp_;
    for (int k = 0; k < 3; ++k) c.position[k] = cmd_.position[k];
    c.heading_native = NativeHeadingDeg(cmd_.heading);
    switch (stage_) {
        case Stage::kLookup:
            c.native = Native::kLookupCar;
            c.text = cmd_.model;
            break;
        case Stage::kHostName:
            c.native = Native::kHostCarName;
            c.index = index_;
            break;
        case Stage::kNumProfiles:
            c.native = Native::kNumProfiles;
            break;
        case Stage::kGetProfile:
            c.native = Native::kGetProfile;
            c.index = cmd_.slot;
            break;
        case Stage::kCreate:
            c.native = Native::kCreateOpponent;
            break;
        case Stage::kLoad:
            c.native = cmd_.source == CarSource::kModel ? Native::kLoadOpponent : Native::kLoadOpponentByConfig;
            c.ptr = cmd_.source == CarSource::kModel ? name_ptr_ : config_;
            break;
        case Stage::kAdd:
            c.native = Native::kAddOpponent;
            break;
        case Stage::kStreamState:
            c.native = Native::kGetLoadState;
            break;
        case Stage::kStreamed:
            c.native = Native::kIsCarStreamed;
            break;
        case Stage::kActivate:
            c.native = Native::kActivate;
            break;
        case Stage::kLock:
            c.native = Native::kLockStreaming;
            break;
        case Stage::kTeleport:
            c.native = Native::kTeleport;
            break;
        case Stage::kVerify:
            c.native = Native::kGetPosition;
            break;
        case Stage::kDelete:
            c.native = Native::kDeleteOpponent;
            break;
        case Stage::kDeleting:
            stage_ = Stage::kGone;  // the manager had its time to clean up
            return c;
        case Stage::kReady:
        case Stage::kGone:
            return c;
    }
    waiting_ = c.native != Native::kNone;
    return c;
}

void Spawner::Report(const Outcome& o) {
    waiting_ = false;
    if (!o.called) {
        failure_ = "a native of the spawn could not be called (no guest thread, no such function, or no block for the context)";
        stage_ = Stage::kGone;
        return;
    }
    const uint32_t ret = o.ret_written ? o.ret : 0;
    const int64_t gap = timing_.gap_ms;
    switch (stage_) {
        case Stage::kLookup:
            index_ = static_cast<int32_t>(ret);
            if (index_ < 0) Fail("the game's car table has no car called '" + cmd_.model + "'");
            else stage_ = Stage::kHostName;
            break;
        case Stage::kHostName:
            name_ptr_ = ret;
            if (!name_ptr_) Fail("the car table has no name for index " + std::to_string(index_));
            else stage_ = Stage::kCreate;
            break;
        case Stage::kNumProfiles: {
            const int n = static_cast<int32_t>(ret);
            if (cmd_.slot >= n) Fail("garage slot " + std::to_string(cmd_.slot) + " does not exist (the save has " + std::to_string(n) + ")");
            else stage_ = Stage::kGetProfile;
            break;
        }
        case Stage::kGetProfile:
            config_ = ret;
            if (!config_) Fail("the garage gave no car for slot " + std::to_string(cmd_.slot));
            else stage_ = Stage::kCreate;
            break;
        case Stage::kCreate:
            opp_ = ret;
            if (!opp_) Fail("the opponent manager has no free node (OpponentManager_CreateOpponent gave 0)");
            else {
                stage_ = Stage::kLoad;
                not_before_ = now_ + gap;
            }
            break;
        case Stage::kLoad:
            stage_ = Stage::kAdd;
            not_before_ = now_ + timing_.load_to_add_ms;
            break;
        case Stage::kAdd:
            added_ = true;
            added_ms_ = now_;
            stage_ = Stage::kStreamState;
            not_before_ = now_ + gap;
            break;
        case Stage::kStreamState:
            if (ret == kLoadStateStreamed) stage_ = Stage::kStreamed;
            else not_before_ = now_ + timing_.stream_poll_ms;
            break;
        case Stage::kStreamed:
            if ((ret & 0xFF) != 0) {
                stage_ = Stage::kActivate;
                not_before_ = now_ + gap;
            } else {
                stage_ = Stage::kStreamState;
                not_before_ = now_ + timing_.stream_poll_ms;
            }
            break;
        case Stage::kActivate:
            stage_ = Stage::kLock;
            not_before_ = now_ + gap;
            break;
        case Stage::kLock:
            stage_ = Stage::kTeleport;
            not_before_ = now_ + timing_.lock_to_teleport_ms;
            break;
        case Stage::kTeleport:
            stage_ = Stage::kVerify;
            not_before_ = now_ + gap;
            break;
        case Stage::kVerify:
            stage_ = Stage::kReady;
            ready_ms_ = now_;
            break;
        case Stage::kDelete:
            stage_ = Stage::kDeleting;
            not_before_ = now_ + timing_.delete_settle_ms;
            break;
        default:
            break;
    }
}

// ================================================================================================================ Dresser

void Dresser::Dress(int slot) {
    if (broken_) return;
    queue_.push_back({slot, false});
}

void Dresser::Restore() {
    if (broken_) return;
    queue_.clear();
    if (changed_ || stage_ != Stage::kIdle) queue_.push_back({-1, true});
}

std::vector<CarEvent> Dresser::TakeEvents() {
    std::vector<CarEvent> out;
    out.swap(events_);
    return out;
}

void Dresser::Next(int64_t now_ms) {
    stage_ = Stage::kIdle;
    while (!queue_.empty()) {
        const Request r = queue_.front();
        queue_.pop_front();
        if (r.restore) {
            if (!changed_ || worn_ == original_) continue;  // nothing to give back
            current_ = r;
            target_ = original_;
            stage_ = Stage::kGetProfile;
            break;
        }
        current_ = r;
        target_ = r.slot;
        if (!original_known_) {
            stage_ = Stage::kCurrent;
        } else if (r.slot == worn_) {
            events_.push_back({false, "the player's car already is garage slot " + std::to_string(r.slot)});
            continue;
        } else {
            stage_ = Stage::kNum;
        }
        break;
    }
    not_before_ = now_ms;
}

Call Dresser::Poll(int64_t now_ms, uint32_t player_racer) {
    now_ = now_ms;
    Call c;
    if (broken_ || waiting_ || !player_racer) return c;
    if (stage_ == Stage::kIdle) {
        if (queue_.empty()) return c;
        Next(now_ms);
    }
    if (stage_ == Stage::kSettle && now_ms >= not_before_) Next(now_ms);
    if (now_ms < not_before_ || stage_ == Stage::kIdle || stage_ == Stage::kSettle) return c;
    c.racer = player_racer;
    switch (stage_) {
        case Stage::kCurrent: c.native = Native::kCurrentProfile; break;
        case Stage::kNum: c.native = Native::kNumProfiles; break;
        case Stage::kGetProfile:
            c.native = Native::kGetProfile;
            c.index = target_;
            break;
        case Stage::kChange:
            c.native = Native::kChangeCarByConfig;
            c.ptr = config_;
            break;
        default: break;
    }
    waiting_ = c.native != Native::kNone;
    return c;
}

void Dresser::Report(const Outcome& o) {
    waiting_ = false;
    if (!o.called) {
        broken_ = true;
        queue_.clear();
        stage_ = Stage::kIdle;
        events_.push_back({true, "a native of the garage dressing could not be called: the player's car is left as it is"});
        return;
    }
    const uint32_t ret = o.ret_written ? o.ret : 0;
    switch (stage_) {
        case Stage::kCurrent:
            original_ = static_cast<int32_t>(ret);
            worn_ = original_;
            original_known_ = true;
            events_.push_back({false, "the player's car is garage slot " + std::to_string(original_) + " now (it comes back at the end)"});
            if (target_ == worn_) {
                events_.push_back({false, "the player's car already is garage slot " + std::to_string(target_)});
                Next(now_);
            } else {
                stage_ = Stage::kNum;
            }
            break;
        case Stage::kNum: {
            const int n = static_cast<int32_t>(ret);
            if (target_ < 0 || target_ >= n) {
                events_.push_back({true, "garage slot " + std::to_string(target_) + " does not exist (the save has " + std::to_string(n) + "): the player's car stays"});
                Next(now_);
            } else {
                stage_ = Stage::kGetProfile;
            }
            break;
        }
        case Stage::kGetProfile:
            config_ = ret;
            if (!config_) {
                events_.push_back({true, "the garage gave no car for slot " + std::to_string(target_) + ": the player's car stays"});
                Next(now_);
            } else {
                stage_ = Stage::kChange;
            }
            break;
        case Stage::kChange:
            worn_ = target_;
            changed_ = !current_.restore;
            events_.push_back({false, (current_.restore ? "the player's own car is back (garage slot " + std::to_string(target_) + ")"
                                                         : "the player's car is now garage slot " + std::to_string(target_)) +
                                          "; its parts finish streaming in a few seconds"});
            stage_ = Stage::kSettle;
            not_before_ = now_ + timing_.dress_settle_ms;
            break;
        default:
            break;
    }
}

// ================================================================================================================ Director

Director::SceneCar* Director::FindLive(const std::string& id) {
    for (auto& c : cars_) {
        if (!c->ended && c->id == id) return c.get();
    }
    return nullptr;
}

void Director::BeginScene() {
    scene_on_ = true;
    if (!registry_.Find("player")) registry_.Add("player", true);
}

Director::Applied Director::Apply(const CarCommand& cmd) {
    Applied a;
    switch (cmd.verb) {
        case CarVerb::kSpawn: {
            if (FindLive(cmd.id)) {
                a.ok = false;
                a.message = "car " + cmd.id + " already exists: a second spawn of it is ignored";
                return a;
            }
            auto car = std::make_unique<SceneCar>(timing_);
            car->id = cmd.id;
            car->spawner.Start(cmd);
            cars_.push_back(std::move(car));
            registry_.Add(cmd.id, false);
            a.message = "spawning " + DescribeCarCommand(cmd);
            return a;
        }
        case CarVerb::kDelete: {
            SceneCar* car = FindLive(cmd.id);
            if (!car) {
                a.ok = false;
                a.message = "car " + cmd.id + " does not exist (never spawned, or already deleted): nothing to delete";
                return a;
            }
            car->ended = true;
            car->spawner.RequestDelete();
            registry_.Remove(cmd.id);
            a.message = "deleting car " + cmd.id + (car->spawner.Ready() ? "" : " (when its spawn has run to the end)");
            return a;
        }
        case CarVerb::kProfile:
            dresser_.Dress(cmd.slot);
            a.message = "the player's car to garage slot " + std::to_string(cmd.slot);
            return a;
    }
    return a;
}

void Director::EndScene() {
    if (!scene_on_ && cars_.empty() && !dresser_.Busy() && !dresser_.Changed()) return;
    int n = 0;
    for (auto& c : cars_) {
        if (c->ended) continue;
        c->ended = true;
        c->spawner.RequestDelete();
        registry_.Remove(c->id);
        ++n;
    }
    registry_.Remove("player");
    dresser_.Restore();
    scene_on_ = false;
    const bool restore = dresser_.Changed() || dresser_.Busy();
    if (n > 0 || restore) {
        events_.push_back({false, "the scene is over: " + std::to_string(n) + " car(s) to delete" + (restore ? ", the player's car to restore" : "")});
    }
}

void Director::Collect(SceneCar& car) {
    Spawner& sp = car.spawner;
    if (sp.Ready() && !car.was_ready) {
        car.was_ready = true;
        ++spawned_;
        events_.push_back({false, "car " + car.id + " is ready"});
    }
}

Director::Next Director::Poll(int64_t now_ms, uint32_t player_racer, bool allow_calls) {
    Next out;
    if (pending_car_ || pending_dresser_) return out;  // a call is in flight: its Report comes first

    // Finished spawns leave: the log lines first, then the registry.
    for (auto it = cars_.begin(); it != cars_.end();) {
        SceneCar& car = **it;
        Collect(car);
        if (car.spawner.Finished()) {
            if (!car.spawner.ok()) {
                ++failed_;
                events_.push_back({true, "car " + car.id + " failed: " + car.spawner.failure()});
            } else if (car.was_ready) {
                ++deleted_;
                events_.push_back({false, "car " + car.id + " is deleted"});
            } else {
                events_.push_back({false, "car " + car.id + " was dropped before it was made"});
            }
            if (!car.ended) registry_.Remove(car.id);
            it = cars_.erase(it);
        } else {
            ++it;
        }
    }
    for (CarEvent& e : dresser_.TakeEvents()) events_.push_back(std::move(e));
    if (!allow_calls) return out;

    auto take = [&](SceneCar& car) {
        Call c = car.spawner.Poll(now_ms);
        if (c.native == Native::kNone) return false;
        out.car = car.id;
        out.call = std::move(c);
        pending_car_ = &car;
        return true;
    };
    // Deletes first (a finished take should not wait behind a spawn), then the player's car, then the spawns in the order of the file.
    for (auto& c : cars_) {
        if (c->spawner.delete_requested() && take(*c)) return out;
    }
    {
        Call c = dresser_.Poll(now_ms, player_racer);
        if (c.native != Native::kNone) {
            out.car = "player";
            out.call = std::move(c);
            pending_dresser_ = true;
            return out;
        }
    }
    for (auto& c : cars_) {
        if (!c->spawner.delete_requested() && take(*c)) return out;
    }
    return out;
}

void Director::Report(const Outcome& outcome) {
    if (pending_dresser_) {
        dresser_.Report(outcome);
    } else if (pending_car_) {
        pending_car_->spawner.Report(outcome);
    }
    pending_car_ = nullptr;
    pending_dresser_ = false;
}

void Director::CancelPending() {
    pending_car_ = nullptr;
    pending_dresser_ = false;
}

void Director::MarkLost(uint32_t racer) {
    for (auto& c : cars_) {
        if (c->spawner.racer() == racer && !c->spawner.Finished()) c->spawner.MarkLost();
    }
}

std::vector<Director::Reader> Director::PoseReaders() const {
    std::vector<Reader> out;
    for (const auto& c : cars_) {
        if (!c->ended && c->spawner.HasEntity()) out.push_back({c->id, c->spawner.racer()});
    }
    return out;
}

std::vector<Director::Reader> Director::InWorld() const {
    std::vector<Reader> out;
    for (const auto& c : cars_) {
        if (c->spawner.InWorld()) out.push_back({c->id, c->spawner.racer()});
    }
    return out;
}

std::vector<CarEvent> Director::TakeEvents() {
    std::vector<CarEvent> out;
    out.swap(events_);
    return out;
}

}  // namespace mc::cinematic
