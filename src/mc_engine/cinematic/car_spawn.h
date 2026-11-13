#pragma once
//
// The scene's cars as a script of native calls (trailer toolkit, work package M4b): the order, the waits and the clean-up of the
// calls E4 proved in free roam, as pure state machines. They know nothing of the game: each Poll hands back the one native
// call to make now (or nothing), the game side (cars.cpp) makes it and Reports what it returned. cars_test.cpp drives them
// against a pretend game.
//
// A SPAWN (E4, round 3: e4-spawn, e4-spawn-name, e4-spawn-profile; every step below ran in a free-roam game without a refusal):
//   the model route   LookupCar(name) -> index (-1: no such car) ; the game's own copy of the name from the car table ;
//   the garage route  LocalOptions_GetNumValidVehicleProfiles, LocalOptions_GetVehicleProfile(slot) -> the profile's mcCarConfig
//   OpponentManager_CreateOpponent                       a node off the manager's pool (0: the pool is empty)
//   OpponentManager_LoadOpponent(opp, name, pos, h)  or  ...LoadOpponentByCarConfig(opp, config, pos, h)
//   OpponentManager_AddOpponentToWorld                   the load state goes to 3 and the car streams within about half a second
//   Racer_GetLoadState == 3 and Racer_IsCarStreamed == 1 (polled; the spawn fails after the stream timeout)
//   Racer_Activate, Racer_SetStreamingLockedIn, Racer_Teleport(opp, pos, h, 0)
//   Racer_GetPosition (a read, to check the pose the matrix gives)
// and a DELETE: OpponentManager_DeleteOpponent, then a wait while the manager cleans up (it reads `deleting 1`, then 0.25 s later
// the count is back and the node is in the pool again).
//
// A car is deleted only when the whole spawn ran: a delete asked for earlier waits for it (a half-made opponent is not a state
// the game's own code ever deletes from). A failed spawn after AddOpponentToWorld deletes itself.
//
// THE PLAYER'S CAR (E4, e4-changecar): LocalOptions_GetVehicleProfile(slot) and Racer_ChangeCarByConfig(player, config) dress it
// live; the profile it wore before comes back through the same calls when the scene ends.
//

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "car_registry.h"
#include "car_verbs.h"

namespace mc::cinematic {

enum class Native : uint8_t {
    kNone,
    kLookupCar,            // (name*) -> index or -1
    kHostCarName,          // not a native: the game's own name string of car table record `index` (the host reads it)
    kNumProfiles,          // LocalOptions_GetNumValidVehicleProfiles() -> int
    kCurrentProfile,       // LocalOptions_GetCurrentVehicleProfileIndex() -> int
    kGetProfile,           // LocalOptions_GetVehicleProfile(index) -> mcCarConfig*
    kCreateOpponent,       // OpponentManager_CreateOpponent() -> opponent*
    kLoadOpponent,         // OpponentManager_LoadOpponent(opp, name*, pos, heading)
    kLoadOpponentByConfig, // OpponentManager_LoadOpponentByCarConfig(opp, config*, pos, heading)
    kAddOpponent,          // OpponentManager_AddOpponentToWorld(opp)
    kGetLoadState,         // Racer_GetLoadState(racer) -> int
    kIsCarStreamed,        // Racer_IsCarStreamed(racer) -> bool
    kActivate,             // Racer_Activate(racer)
    kLockStreaming,        // Racer_SetStreamingLockedIn(racer)
    kTeleport,             // Racer_Teleport(racer, pos, heading, speed 0)
    kGetPosition,          // Racer_GetPosition(racer, vec) -> vector slot
    kDeleteOpponent,       // OpponentManager_DeleteOpponent(opp)
    kChangeCarByConfig,    // Racer_ChangeCarByConfig(racer, config*)
};
const char* NativeName(Native native);

// One call to make. Which fields a native uses is in the list above and in cars.cpp's argument builder.
struct Call {
    Native native = Native::kNone;
    uint32_t racer = 0;   // the racer / opponent the call is on
    uint32_t ptr = 0;     // a name or a config the call passes
    int32_t index = 0;    // a car table or garage index
    std::string text;     // a vehicle name (kLookupCar)
    double position[3] = {};
    double heading_native = 0.0;  // the angle Racer_Teleport and the loader take (NativeHeadingDeg)
};

struct Outcome {
    bool called = false;      // false: the native could not be called (no guest thread, no such function, no block): the machine stops
    bool ret_written = false;
    uint32_t ret = 0;
};

// A line for the log that a machine wants written: the game side logs it with the car's tag.
struct CarEvent {
    bool warning = false;
    std::string text;
};

struct CarTiming {
    int64_t gap_ms = 250;               // between two steps of a spawn
    int64_t load_to_add_ms = 2000;      // E4 waited 2.5 s between the load and the add; the load state stays 0 until the add
    int64_t stream_poll_ms = 250;       // between two reads of the load state
    int64_t stream_timeout_ms = 20000;  // after the add: the car should be streamed in well inside this (E4: 0.5 s)
    int64_t lock_to_teleport_ms = 500;  // E4: 1 s
    int64_t delete_hold_ms = 1500;      // a finished spawn is not deleted before this
    int64_t delete_settle_ms = 600;     // E4: the manager is clean 0.25 s after the delete
    int64_t dress_settle_ms = 3000;     // the dressed player's car finishes streaming its parts a few seconds after the change (E4)
};

// ---------------------------------------------------------------------------------------------------------------- a spawn

class Spawner {
  public:
    enum class Stage : uint8_t {
        kLookup, kHostName, kNumProfiles, kGetProfile,  // nothing exists in the game yet
        kCreate, kLoad, kAdd,                           // the opponent exists; not in the world until the add
        kStreamState, kStreamed, kActivate, kLock, kTeleport, kVerify,
        kReady,
        kDelete, kDeleting,
        kGone,  // finished: ok() says whether the car was ever made
    };

    explicit Spawner(const CarTiming& timing = {}) : timing_(timing) {}

    void Start(const CarCommand& cmd);
    void RequestDelete();  // asked for: it happens when the spawn has run to its end (at once when nothing exists yet)
    void MarkLost();       // the game no longer has the car: finished, nothing more is called

    Call Poll(int64_t now_ms);
    void Report(const Outcome& outcome);

    Stage stage() const { return stage_; }
    const char* StageName() const;
    bool Finished() const { return stage_ == Stage::kGone; }
    bool Ready() const { return stage_ == Stage::kReady; }
    bool ok() const { return failure_.empty(); }
    const std::string& failure() const { return failure_; }
    uint32_t racer() const { return opp_; }
    bool InWorld() const { return added_ && stage_ != Stage::kGone; }
    // The car's entity (and so its matrix) exists: from the end of the stream to the delete.
    bool HasEntity() const { return stage_ >= Stage::kActivate && stage_ <= Stage::kDelete; }
    const CarCommand& command() const { return cmd_; }
    bool delete_requested() const { return want_delete_; }

  private:
    void Fail(const std::string& why);

    CarTiming timing_;
    CarCommand cmd_;
    Stage stage_ = Stage::kGone;
    bool waiting_ = false;  // a call was handed out and not reported yet
    int64_t now_ = 0;
    int64_t not_before_ = 0;
    int64_t added_ms_ = 0;
    int64_t ready_ms_ = 0;
    bool added_ = false;
    bool want_delete_ = false;
    uint32_t name_ptr_ = 0;
    uint32_t config_ = 0;
    uint32_t opp_ = 0;
    int32_t index_ = -1;
    std::string failure_;
};

// ---------------------------------------------------------------------------------------------------------------- the player's car

class Dresser {
  public:
    explicit Dresser(const CarTiming& timing = {}) : timing_(timing) {}

    void Dress(int slot);  // the player's car becomes garage slot `slot`
    void Restore();        // back to the slot it wore before the first Dress; drops the dressings not started yet

    // The player's racer is `player_racer` (0: not known, no call is made).
    Call Poll(int64_t now_ms, uint32_t player_racer);
    void Report(const Outcome& outcome);

    bool Busy() const { return stage_ != Stage::kIdle || !queue_.empty(); }
    bool Changed() const { return changed_; }
    // Lines for the log, taken once.
    std::vector<CarEvent> TakeEvents();

  private:
    enum class Stage : uint8_t { kIdle, kCurrent, kNum, kGetProfile, kChange, kSettle };
    struct Request {
        int slot = 0;
        bool restore = false;
    };
    void Next(int64_t now_ms);

    CarTiming timing_;
    std::deque<Request> queue_;
    Stage stage_ = Stage::kIdle;
    bool waiting_ = false;
    int64_t now_ = 0;
    int64_t not_before_ = 0;
    Request current_;
    bool original_known_ = false;
    int original_ = -1;
    int worn_ = -1;
    bool changed_ = false;
    uint32_t config_ = 0;
    int target_ = -1;
    bool broken_ = false;  // a call could not be made: nothing more is tried
    std::vector<CarEvent> events_;
};

// ---------------------------------------------------------------------------------------------------------------- everything

// What the scene asked for, the registry, the machines: one native call an update at most.
class Director {
  public:
    explicit Director(const CarTiming& timing = {}) : timing_(timing), dresser_(timing) {}

    struct Applied {
        bool ok = true;
        std::string message;  // for the log: what was done, or why not
    };
    // A scene's car statement due now.
    Applied Apply(const CarCommand& cmd);
    // The scene begins: the player is in the registry.
    void BeginScene();
    // The scene ends (or is replaced): every car it spawned is to go, the player's car is to come back.
    void EndScene();

    // Anything to do or to keep track of.
    bool Active() const { return scene_on_ || !cars_.empty() || dresser_.Busy(); }

    struct Next {
        std::string car;  // the id, or "player"
        Call call;
    };
    // `allow_calls` false (the world is not ready: no car on the road, paused, a loading screen): the books are kept, no call is handed out.
    Next Poll(int64_t now_ms, uint32_t player_racer, bool allow_calls = true);
    void Report(const Outcome& outcome);
    // The game side did not make the call after all (it found the car gone): nothing is reported, the next Poll goes on.
    void CancelPending();

    // The game side found the racer gone from the opponent manager (the world was rebuilt): nothing more is called on it.
    void MarkLost(uint32_t racer);

    // The racers that have an entity to read a pose from, with their ids.
    struct Reader {
        std::string id;
        uint32_t racer = 0;
    };
    std::vector<Reader> PoseReaders() const;
    // The racers that should be in the opponent manager's list (added, not gone): the game side checks them.
    std::vector<Reader> InWorld() const;

    CarRegistry& registry() { return registry_; }
    const CarRegistry& registry() const { return registry_; }
    std::vector<CarEvent> TakeEvents();

    size_t SceneCarCount() const { return cars_.size(); }
    int spawned() const { return spawned_; }
    int deleted() const { return deleted_; }
    int failed() const { return failed_; }
    const Dresser& dresser() const { return dresser_; }

  private:
    struct SceneCar {
        std::string id;
        Spawner spawner;
        bool ended = false;  // its scene is over: it is no longer reachable by id
        bool was_ready = false;
        explicit SceneCar(const CarTiming& t) : spawner(t) {}
    };
    SceneCar* FindLive(const std::string& id);
    void Collect(SceneCar& car);

    CarTiming timing_;
    CarRegistry registry_;
    std::vector<std::unique_ptr<SceneCar>> cars_;
    Dresser dresser_;
    bool scene_on_ = false;
    // Which machine the call handed out last came from.
    SceneCar* pending_car_ = nullptr;
    bool pending_dresser_ = false;
    std::vector<CarEvent> events_;
    int spawned_ = 0;
    int deleted_ = 0;
    int failed_ = 0;
};

}  // namespace mc::cinematic
