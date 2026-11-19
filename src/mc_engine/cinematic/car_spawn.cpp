// The scene's cars as a script of native calls: see car_spawn.h.

#include "car_spawn.h"

#include <algorithm>
#include <cmath>
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
        case Native::kCreateBrain: return "BrainFactory_CreateBrainCruising";
        case Native::kBrainSetSpeed: return "BrainCruising_SetSpeed";
        case Native::kBrainSetDestSpeed: return "BrainCruising_SetDestSpeed";
        case Native::kBrainSetLooping: return "BrainCruising_SetLooping";
        case Native::kBrainAppendDest: return "BrainCruising_AppendDest";
        case Native::kBrainDrive: return "BrainCruising_DriveToNearestRail";
        case Native::kBrainReached: return "BrainCruising_HasReachedGoal";
        case Native::kBrainStop: return "BrainCruising_RemoveFromTraffic";
        case Native::kDeleteBrain: return "BrainFactory_DeleteBrain";
        case Native::kAddAmbient: return "AmbientMgr_AddAmbientToLayout";
        case Native::kRemoveAmbient: return "AmbientMgr_RemoveAmbientFromLayout";
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
            SetPaint(&c);
            break;
        case Stage::kLoad:
            c.native = cmd_.source == CarSource::kModel ? Native::kLoadOpponent : Native::kLoadOpponentByConfig;
            c.ptr = cmd_.source == CarSource::kModel ? name_ptr_ : config_;
            SetPaint(&c);
            break;
        case Stage::kAdd:
            c.native = Native::kAddOpponent;
            SetPaint(&c);
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

// ================================================================================================================ PathDrive

namespace {

constexpr size_t kBrainMaxDests = 48;       // the brain's array holds 64 (AppendDest does not check: the 65th would be written over its own count)
constexpr double kBrainMinSpacing = 8.0;    // metres between two destinations

std::string Fmt1(const char* format, double a) {
    char buf[200];
    std::snprintf(buf, sizeof(buf), format, a);
    return buf;
}

double HeadingGap(double a, double b) {
    double d = std::fabs(a - b);
    if (d > 180.0) d = 360.0 - d;
    return d;
}

}  // namespace

const char* BrainStateName(int state) {
    switch (state) {
        case 0: return "idle: no throttle, brake held";
        case 1:
        case 2:
        case 3:
        case 5: return "on the lanes";
        case 4: return "no lane to join: it stands, or hops to the lane of its next destination";
        case 6: return "goal reached";
        default: return "unknown";
    }
}

void PathDrive::Event(bool warning, std::string text) {
    events_.push_back({warning, std::move(text)});
}

std::vector<CarEvent> PathDrive::TakeEvents() {
    std::vector<CarEvent> out;
    out.swap(events_);
    return out;
}

bool PathDrive::Start(const CarCommand& cmd, const Route& route, double clock, std::string* why) {
    if (brain_ever_) {
        *why = "car " + cmd.id + " already has a brain from an earlier path: a car takes one brain path (give the next path to a new car)";
        return false;
    }
    PathSampler built;
    if (!built.Build(route, cmd.speed, why)) {
        *why = "route '" + cmd.route + "': " + *why;
        return false;
    }
    const bool replaced = active_;
    id_ = cmd.id;
    route_name_ = cmd.route;
    mode_ = cmd.mode;
    speed_ = cmd.speed;
    sampler_ = std::move(built);
    active_ = true;
    start_clock_ = clock;
    stopping_ = false;
    stop_wanted_ = false;
    stop_tau_ = 0.0;
    teleported_ = false;
    last_clock_ = -1e300;
    next_log_tau_ = 0.0;
    have_hold_y_ = false;
    stage_ = BStage::kIdle;
    dests_.clear();
    next_dest_ = 0;
    reached_ = false;
    waiting_ = false;
    last_state_ = -1;
    next_brain_log_ = 0.0;
    stats_ = Stats{};
    std::string text = "path " + id_ + (replaced ? " starts again" : " starts") + " (" + (mode_ == PathMode::kBrain ? "brain" : "kinematic") + "): route " + route_name_ +
                       ", " + std::to_string(sampler_.PointCount()) + " points, " + Fmt1("%.1f m", sampler_.Length()) + ", " + Fmt1("%.1f s", sampler_.Duration()) +
                       " at " + DescribeSpeed(speed_) + (speed_.kind == SpeedKind::kMetersPerSecond ? "" : Fmt1(" (%.1f m/s on average)", sampler_.MeanSpeed()));
    if (mode_ == PathMode::kBrain) {
        dests_ = sampler_.Resample(kBrainMaxDests, kBrainMinSpacing);
        stage_ = BStage::kCreate;
        text += "; " + std::to_string(dests_.size()) + " destinations for the game's AI";
    } else {
        text += "; the car is put on the route at every update";
    }
    Event(false, text);
    return true;
}

void PathDrive::Stop(double clock) {
    if (!active_) {
        Event(false, "path " + id_ + ": no path is running, nothing to stop");
        return;
    }
    if (mode_ == PathMode::kKinematic) {
        stop_tau_ = std::max(0.0, clock - start_clock_);
        stopping_ = true;
    } else {
        stop_wanted_ = true;
    }
    Event(false, "path " + id_ + ": stop asked at path time " + Fmt1("%.2f s", std::max(0.0, clock - start_clock_)));
}

void PathDrive::Fail(const std::string& why) {
    Event(true, "path " + id_ + " failed: " + why);
    active_ = false;
    if (mode_ == PathMode::kBrain) stage_ = BStage::kDone;
}

void PathDrive::Summary(const char* why) {
    if (mode_ == PathMode::kKinematic) {
        const double mean = stats_.samples ? stats_.sum_off / static_cast<double>(stats_.samples) : 0.0;
        std::string text = "path " + id_ + " " + why + ": " + std::to_string(stats_.teleports) + " put(s) of the car on the route (" + std::to_string(stats_.refused) +
                           " not made), " + std::to_string(stats_.samples) + " poses compared with the route's place: " + Fmt1("mean off %.2f m", mean) +
                           Fmt1(", max %.2f m", stats_.max_off) + Fmt1(", heading off at most %.1f degrees", stats_.max_heading_off);
        // A car whose matrix could not be read says so here: its poses were not compared (the means above are of the updates that had a fresh one).
        if (stats_.unread > 0) {
            text += "; " + std::to_string(stats_.unread) + " update(s) had no fresh pose to compare (the matrix was unread for up to " + std::to_string(stats_.unread_max) +
                    " updates)";
        }
        Event(false, std::move(text));
    } else {
        std::string seen;
        for (int s = 0; s < 8; ++s) {
            if (stats_.brain_states_seen & (1 << s)) seen += (seen.empty() ? "" : ",") + std::to_string(s);
        }
        Event(false, "path " + id_ + " " + why + ": brain states seen {" + seen + "}, " + std::to_string(stats_.brain_samples) + " reads; farthest along the route " +
                         Fmt1("%.1f", stats_.brain_best_s) + Fmt1(" of %.1f m", sampler_.Length()) + Fmt1(", farthest off the route %.2f m", stats_.brain_max_off));
    }
}

// ---- kinematic

bool PathDrive::PollTeleport(double clock, const CarPose* pose, Teleport* out, uint64_t unread) {
    if (!active_ || mode_ != PathMode::kKinematic) return false;
    if (teleported_ && !stopping_ && clock == last_clock_) return false;  // the clock stood still (a frozen time scale): the car is where it was put
    const double tau = clock - start_clock_;
    const PathSample s = stopping_ ? sampler_.AtTime(stop_tau_) : sampler_.AtTime(tau);

    double y = 0.0;
    if (s.has_y) {
        y = s.y;
    } else if (pose) {
        y = pose->position[1];  // the route has no heights: the car keeps the height it has (the ground under it, after a step of physics)
        hold_y_ = y;
        have_hold_y_ = true;
    } else if (have_hold_y_) {
        y = hold_y_;
    } else {
        return false;  // no pose yet: nothing to take the height from
    }

    const bool last = stopping_ || s.finished;
    if (!pose && unread > 0 && teleported_ && !stopping_) {
        // The matrix cannot be read: there is nothing to compare the route's place with, and the last pose is not where the car is (the car is put on the route
        // all the same: the picture shows it). The progress line says so, so that a run is not read as "the car lags" (M4c's first run).
        ++stats_.unread;
        stats_.unread_max = std::max(stats_.unread_max, unread);
        if (tau >= next_log_tau_) {
            next_log_tau_ = tau + timing_.path_log_s;
            char buf[240];
            std::snprintf(buf, sizeof(buf), "path %s t=%.2f s=%.1f/%.1f m v=%.1f m/s: route (%.2f, %.2f, %.2f) h %.1f, the car's matrix has not been read for %llu updates: no pose to compare",
                          id_.c_str(), tau, s.s, sampler_.Length(), s.speed, s.x, y, s.z, s.heading, static_cast<unsigned long long>(unread));
            Event(false, buf);
        }
    }
    if (pose && teleported_ && !stopping_) {
        const double dx = pose->position[0] - s.x;
        const double dz = pose->position[2] - s.z;
        const double off = std::hypot(dx, dz);
        const double hoff = HeadingGap(pose->heading, s.heading);
        stats_.sum_off += off;
        ++stats_.samples;
        stats_.max_off = std::max(stats_.max_off, off);
        stats_.max_heading_off = std::max(stats_.max_heading_off, hoff);
        if (tau >= next_log_tau_) {
            next_log_tau_ = tau + timing_.path_log_s;
            const double fx = std::sin(s.heading * 3.14159265358979323846 / 180.0), fz = std::cos(s.heading * 3.14159265358979323846 / 180.0);
            char buf[420];
            std::snprintf(buf, sizeof(buf),
                          "path %s t=%.2f s=%.1f/%.1f m v=%.1f m/s: route (%.2f, %.2f, %.2f) h %.1f, car (%.2f, %.2f, %.2f) h %.1f, off %.2f m (ahead %+.2f, side %+.2f), %.1f deg",
                          id_.c_str(), tau, s.s, sampler_.Length(), s.speed, s.x, y, s.z, s.heading, pose->position[0], pose->position[1], pose->position[2],
                          pose->heading, off, dx * fx + dz * fz, dx * fz - dz * fx, hoff);
            Event(false, buf);
        }
    }

    out->position[0] = s.x;
    out->position[1] = y;
    out->position[2] = s.z;
    out->heading_native = NativeHeadingDeg(s.heading);
    out->speed = last ? 0.0 : s.speed;
    out->along = s.s;
    out->first = !teleported_;
    out->last = last;
    teleport_last_ = last;
    last_clock_ = clock;
    return true;
}

void PathDrive::ReportTeleport(bool called) {
    if (!called) {
        ++stats_.refused;
        Fail("Racer_Teleport could not be called (no guest thread, no such function, or no block for the context): the car stays where it is");
        return;
    }
    ++stats_.teleports;
    teleported_ = true;
    if (teleport_last_) {
        active_ = false;
        Summary(stopping_ ? "stopped" : "finished: the car stands at the last point");
    }
}

// ---- brain

Call PathDrive::PollBrain(int64_t now_ms, uint32_t racer, double y_ref) {
    now_ = now_ms;
    Call c;
    if (!active_ || mode_ != PathMode::kBrain || waiting_ || racer == 0) return c;
    if (stop_wanted_ && stage_ != BStage::kStopping && stage_ != BStage::kDone) {
        if (brain_ != 0 && stage_ >= BStage::kSetSpeed) {
            stage_ = BStage::kStopping;
        } else {
            active_ = false;
            Summary("stopped before the brain was made");
            return c;
        }
    }
    if (now_ms < not_before_) return c;
    c.racer = racer;
    c.brain = brain_;
    switch (stage_) {
        case BStage::kCreate:
            c.native = Native::kCreateBrain;
            y_ref_ = y_ref;
            break;
        case BStage::kSetSpeed:
            c.native = Native::kBrainSetSpeed;
            c.speed = sampler_.MeanSpeed();
            break;
        case BStage::kSetDestSpeed:
            c.native = Native::kBrainSetDestSpeed;
            c.speed = sampler_.MeanSpeed();
            break;
        case BStage::kSetLooping:
            c.native = Native::kBrainSetLooping;
            c.index = 0;  // false: the brain stops at the last destination (HasReachedGoal)
            break;
        case BStage::kAppend: {
            const PathSample& d = dests_[next_dest_];
            c.native = Native::kBrainAppendDest;
            c.position[0] = d.x;
            c.position[1] = d.has_y ? d.y : y_ref_;
            c.position[2] = d.z;
            c.index = static_cast<int32_t>(next_dest_);
            break;
        }
        case BStage::kDrive:
            c.native = Native::kBrainDrive;
            break;
        case BStage::kRunning:
            c.native = Native::kBrainReached;
            break;
        case BStage::kStopping:
            c.native = Native::kBrainStop;
            break;
        case BStage::kIdle:
        case BStage::kDone:
            return c;
    }
    waiting_ = c.native != Native::kNone;
    return c;
}

void PathDrive::ReportBrain(const Outcome& o) {
    waiting_ = false;
    if (!o.called) {
        Fail("a native of the brain program could not be called (no guest thread, no such function, or no block for the context)");
        return;
    }
    const uint32_t ret = o.ret_written ? o.ret : 0;
    const int64_t gap = timing_.brain_gap_ms;
    switch (stage_) {
        case BStage::kCreate:
            brain_ = ret;
            if (!brain_) {
                Fail("BrainFactory_CreateBrainCruising gave no brain");
            } else {
                brain_ever_ = true;
                stage_ = BStage::kSetSpeed;
                not_before_ = now_ + gap;
            }
            break;
        case BStage::kSetSpeed:
            stage_ = BStage::kSetDestSpeed;
            not_before_ = now_ + gap;
            break;
        case BStage::kSetDestSpeed:
            stage_ = BStage::kSetLooping;
            not_before_ = now_ + gap;
            break;
        case BStage::kSetLooping:
            stage_ = dests_.empty() ? BStage::kDrive : BStage::kAppend;
            not_before_ = now_ + gap;
            break;
        case BStage::kAppend:
            if (++next_dest_ >= dests_.size()) stage_ = BStage::kDrive;
            not_before_ = now_ + gap;
            break;
        case BStage::kDrive:
            stage_ = BStage::kRunning;
            not_before_ = now_ + timing_.reached_poll_ms;
            Event(false, "path " + id_ + ": the brain is told to drive (BrainCruising_DriveToNearestRail) with " + std::to_string(dests_.size()) + " destinations");
            break;
        case BStage::kRunning:
            if ((ret & 0xFF) != 0) {
                reached_ = true;
                active_ = false;
                Summary("finished: the brain reports its goal reached");
            } else {
                not_before_ = now_ + timing_.reached_poll_ms;
            }
            break;
        case BStage::kStopping:
            stage_ = BStage::kDone;
            active_ = false;
            Summary("stopped: the brain is back to idle (brake held)");
            break;
        case BStage::kIdle:
        case BStage::kDone:
            break;
    }
}

Call PathDrive::PollBrainDelete(int64_t now_ms, uint32_t racer) {
    now_ = now_ms;
    Call c;
    if (!NeedsBrainDelete() || waiting_ || racer == 0 || now_ms < not_before_) return c;
    c.native = Native::kDeleteBrain;
    c.racer = racer;
    c.brain = brain_;
    waiting_ = true;
    return c;
}

void PathDrive::ReportBrainDelete(const Outcome& o) {
    waiting_ = false;
    if (!o.called) Event(true, "path " + id_ + ": BrainFactory_DeleteBrain could not be called: the brain is left to the car's own delete");
    brain_ = 0;
    brain_gone_ = true;
    not_before_ = now_ + timing_.brain_gap_ms;
}

void PathDrive::NoteBrain(double clock, int state, int dest_index, const CarPose* pose, uint64_t unread) {
    if (!active_ || mode_ != PathMode::kBrain || brain_ == 0 || stage_ < BStage::kRunning) return;
    stats_.brain_states_seen |= 1 << std::clamp(state, 0, 7);
    ++stats_.brain_samples;
    PathSampler::Nearest nearest;
    if (pose) {
        nearest = sampler_.NearestTo(pose->position[0], pose->position[2]);
        stats_.brain_best_s = std::max(stats_.brain_best_s, nearest.s);
        stats_.brain_max_off = std::max(stats_.brain_max_off, nearest.distance);
    }
    const bool changed = state != last_state_;
    if (!changed && clock < next_brain_log_) return;
    next_brain_log_ = clock + timing_.path_log_s;
    std::string text = "path " + id_ + Fmt1(" t=%.2f", clock - start_clock_) + " brain state " + std::to_string(state) + " (" + BrainStateName(state) + ") dest " +
                       std::to_string(dest_index) + "/" + std::to_string(dests_.size());
    if (pose) {
        char buf[200];
        std::snprintf(buf, sizeof(buf), "; car (%.2f, %.2f, %.2f) h %.1f; nearest route point %.1f of %.1f m, %.2f m off", pose->position[0], pose->position[1], pose->position[2],
                      pose->heading, nearest.s, sampler_.Length(), nearest.distance);
        text += buf;
    } else if (unread > 0) {
        text += "; the car's matrix has not been read for " + std::to_string(unread) + " updates: no pose";
    }
    if (changed && last_state_ >= 0) text += "; state was " + std::to_string(last_state_);
    last_state_ = state;
    Event(changed && state == 4, text);
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
    ambient_calls_ = 0;
    paint_ordinal_ = 0;
    if (!registry_.Find("player")) registry_.Add("player", true);
}

// A car that takes a paint and was not given one gets the default of its place in the scene (scene_seed.h): never the game's random pick.
static CarCommand WithPaint(const CarCommand& in, int* ordinal) {
    CarCommand cmd = in;
    const bool takes_paint = cmd.verb == CarVerb::kAmbient || (cmd.verb == CarVerb::kSpawn && cmd.source == CarSource::kModel);
    if (!takes_paint) return cmd;
    if (!cmd.has_paint) {
        cmd.has_paint = true;
        cmd.paint = seed::DefaultPaint(*ordinal);
        cmd.paint_default = true;
    }
    ++*ordinal;
    return cmd;
}

Director::Applied Director::Apply(const CarCommand& in_cmd, double clock, const Route* route) {
    Applied a;
    const CarCommand cmd = WithPaint(in_cmd, &paint_ordinal_);
    switch (cmd.verb) {
        case CarVerb::kAmbient: {
            if (FindLive(cmd.id) || FindLiveAmbient(cmd.id)) {
                a.ok = false;
                a.message = "car " + cmd.id + " already exists: a second ambient of it is ignored";
                return a;
            }
            auto entry = std::make_unique<AmbientEntry>(timing_);
            entry->id = cmd.id;
            entry->car.Start(cmd);
            ambient_.push_back(std::move(entry));
            registry_.Add(cmd.id, false);
            a.message = "placing " + DescribeCarCommand(cmd) + " (an ambient car: the game's parked spawner makes it from the layout item)";
            return a;
        }
        case CarVerb::kSpawn: {
            if (FindLive(cmd.id) || FindLiveAmbient(cmd.id)) {
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
            if (AmbientEntry* amb = FindLiveAmbient(cmd.id)) {
                const bool placed = amb->car.Placed() || amb->car.stage() == AmbientCar::Stage::kRemove;
                amb->ended = true;
                amb->car.RequestDelete();
                registry_.Remove(cmd.id);
                a.message = "removing ambient car " + cmd.id + (placed ? " (AmbientMgr_RemoveAmbientFromLayout)" : " (it was not placed yet: dropped)");
                return a;
            }
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
        case CarVerb::kPath: {
            if (FindLiveAmbient(cmd.id)) {
                a.ok = false;
                a.message = "car " + cmd.id + " is an ambient car: it stands where it was placed and takes no route (spawn a car for a path)";
                return a;
            }
            SceneCar* car = FindLive(cmd.id);
            if (!car) {
                a.ok = false;
                a.message = "car " + cmd.id + " does not exist (never spawned, or already deleted): no path for it";
                return a;
            }
            if (!route) {
                a.ok = false;
                a.message = "car " + cmd.id + " path " + cmd.route + ": the route file was not read";
                return a;
            }
            std::string why;
            if (!car->path.Start(cmd, *route, clock, &why)) {
                a.ok = false;
                a.message = "car " + cmd.id + " path " + cmd.route + ": " + why;
                return a;
            }
            a.message = DescribeCarCommand(cmd) + (car->spawner.Ready() ? "" : " (when the car is ready)");
            return a;
        }
        case CarVerb::kStop: {
            if (FindLiveAmbient(cmd.id)) {
                a.ok = false;
                a.message = "car " + cmd.id + " is an ambient car: it has no route to stop (it stands where it was placed)";
                return a;
            }
            SceneCar* car = FindLive(cmd.id);
            if (!car) {
                a.ok = false;
                a.message = "car " + cmd.id + " does not exist (never spawned, or already deleted): nothing to stop";
                return a;
            }
            if (!car->path.Active()) {
                a.ok = false;
                a.message = "car " + cmd.id + " has no path running (it never had one, or it is over): nothing to stop";
                return a;
            }
            car->path.Stop(clock);
            a.message = "stopping car " + cmd.id;
            return a;
        }
    }
    return a;
}

void Director::EndScene() {
    if (!scene_on_ && cars_.empty() && ambient_.empty() && !dresser_.Busy() && !dresser_.Changed()) return;
    int n = 0;
    for (auto& c : cars_) {
        if (c->ended) continue;
        c->ended = true;
        c->spawner.RequestDelete();
        registry_.Remove(c->id);
        ++n;
    }
    for (auto& a : ambient_) {
        if (a->ended) continue;
        a->ended = true;
        a->car.RequestDelete();
        registry_.Remove(a->id);
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
    if (pending_car_ || pending_dresser_ || pending_path_car_ || pending_ambient_) return out;  // a call is in flight: its Report comes first

    // Finished spawns leave: the log lines first, then the registry.
    for (auto it = cars_.begin(); it != cars_.end();) {
        SceneCar& car = **it;
        Collect(car);
        for (CarEvent& e : car.path.TakeEvents()) events_.push_back(std::move(e));
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
    // Finished ambient cars leave the same way. (A failure was logged by the machine as it happened.)
    for (auto it = ambient_.begin(); it != ambient_.end();) {
        AmbientEntry& a = **it;
        for (CarEvent& e : a.car.TakeEvents()) events_.push_back(std::move(e));
        if (a.car.EverPlaced() && !a.was_placed) {
            a.was_placed = true;
            ++spawned_;
        }
        if (a.car.Finished()) {
            if (!a.car.ok()) {
                ++failed_;
            } else if (a.was_placed) {
                ++deleted_;
                events_.push_back({false, "car " + a.id + " (ambient) is removed: layout item " + std::to_string(a.car.layout_id()) + " given back"});
            } else {
                events_.push_back({false, "car " + a.id + " (ambient) was dropped before it was placed"});
            }
            if (!a.ended) registry_.Remove(a.id);
            it = ambient_.erase(it);
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
        if (!c->spawner.delete_requested()) continue;
        if (c->spawner.Ready() && c->path.NeedsBrainDelete()) {
            // the brain goes before the opponent does (E4's order); the delete waits for it
            Call bc = c->path.PollBrainDelete(now_ms, c->spawner.racer());
            if (bc.native != Native::kNone) {
                out.car = c->id;
                out.call = std::move(bc);
                pending_path_car_ = c.get();
                pending_path_delete_ = true;
                return out;
            }
            continue;
        }
        if (take(*c)) return out;
    }
    auto take_ambient = [&](AmbientEntry& a) {
        Call c = a.car.Poll(now_ms, gate_);
        if (c.native == Native::kNone) return false;
        out.car = a.id;
        out.call = std::move(c);
        pending_ambient_ = &a;
        if (out.call.native == Native::kAddAmbient) ++ambient_calls_;
        return true;
    };
    gate_.placed_so_far = ambient_calls_;
    for (auto& a : ambient_) {
        if (a->car.delete_requested() && take_ambient(*a)) return out;
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
        if (c->spawner.delete_requested()) continue;
        if (take(*c)) return out;
        if (c->ended || !c->spawner.Ready() || !c->path.Active() || c->path.mode() != PathMode::kBrain) continue;
        const CarEntry* e = registry_.Find(c->id);
        Call bc = c->path.PollBrain(now_ms, c->spawner.racer(), e && e->have_pose ? e->pose.position[1] : 0.0);
        if (bc.native != Native::kNone) {
            out.car = c->id;
            out.call = std::move(bc);
            pending_path_car_ = c.get();
            pending_path_delete_ = false;
            return out;
        }
    }
    for (auto& a : ambient_) {
        if (!a->car.delete_requested() && take_ambient(*a)) return out;
    }
    return out;
}

void Director::Report(const Outcome& outcome) {
    if (pending_dresser_) {
        dresser_.Report(outcome);
    } else if (pending_path_car_) {
        if (pending_path_delete_) pending_path_car_->path.ReportBrainDelete(outcome);
        else pending_path_car_->path.ReportBrain(outcome);
    } else if (pending_car_) {
        pending_car_->spawner.Report(outcome);
    } else if (pending_ambient_) {
        pending_ambient_->car.Report(outcome);
    }
    pending_car_ = nullptr;
    pending_ambient_ = nullptr;
    pending_path_car_ = nullptr;
    pending_path_delete_ = false;
    pending_dresser_ = false;
}

void Director::CancelPending() {
    pending_car_ = nullptr;
    pending_ambient_ = nullptr;
    pending_path_car_ = nullptr;
    pending_path_delete_ = false;
    pending_dresser_ = false;
}

void Director::MarkLost(uint32_t racer) {
    for (auto& c : cars_) {
        if (c->spawner.racer() == racer && !c->spawner.Finished()) c->spawner.MarkLost();
    }
}

namespace {

// How many updates the pose of `e` has gone unread at update `frame` (0: fresh, or the caller does not count updates, or it never had one).
uint64_t UnreadUpdates(const CarEntry* e, uint64_t frame) {
    if (!e || !e->have_pose || frame == kNoFrame || frame <= e->pose_frame) return 0;
    const uint64_t age = frame - e->pose_frame;
    return age > kPoseFreshUpdates ? age : 0;
}

}  // namespace

void Director::PollTeleports(double clock, std::vector<TeleportDue>* out, uint64_t frame) {
    for (auto& c : cars_) {
        if (c->ended || !c->spawner.Ready() || !c->path.Active() || c->path.mode() != PathMode::kKinematic) continue;
        const CarEntry* e = registry_.Find(c->id);
        const uint64_t unread = UnreadUpdates(e, frame);
        const CarPose* pose = e && e->have_pose && unread == 0 ? &e->pose : nullptr;  // a stale pose is not where the car is
        TeleportDue due;
        if (!c->path.PollTeleport(clock, pose, &due.teleport, unread)) continue;
        due.car = c->id;
        due.racer = c->spawner.racer();
        out->push_back(std::move(due));
    }
}

void Director::ReportTeleport(const std::string& car, bool called) {
    for (auto& c : cars_) {
        if (c->id == car && !c->ended) {
            c->path.ReportTeleport(called);
            return;
        }
    }
}

std::vector<Director::BrainReader> Director::BrainReaders() const {
    std::vector<BrainReader> out;
    for (const auto& c : cars_) {
        if (!c->ended && c->spawner.Ready() && c->path.Active() && c->path.BrainRunning()) out.push_back({c->id, c->path.brain()});
    }
    return out;
}

void Director::NoteBrain(const std::string& id, double clock, int state, int dest_index, uint64_t frame) {
    for (auto& c : cars_) {
        if (c->id != id || c->ended) continue;
        const CarEntry* e = registry_.Find(id);
        const uint64_t unread = UnreadUpdates(e, frame);
        c->path.NoteBrain(clock, state, dest_index, e && e->have_pose && unread == 0 ? &e->pose : nullptr, unread);
        return;
    }
}

const PathDrive* Director::PathOf(const std::string& id) const {
    for (const auto& c : cars_) {
        if (!c->ended && c->id == id) return &c->path;
    }
    return nullptr;
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
    for (auto& c : cars_) {
        for (CarEvent& e : c->path.TakeEvents()) events_.push_back(std::move(e));
    }
    for (auto& a : ambient_) {
        for (CarEvent& e : a->car.TakeEvents()) events_.push_back(std::move(e));
    }
    std::vector<CarEvent> out;
    out.swap(events_);
    return out;
}

}  // namespace mc::cinematic
