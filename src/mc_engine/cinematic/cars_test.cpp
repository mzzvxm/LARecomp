// Check of the scene's car statements and bookkeeping (car_verbs.h, car_registry.h, car_spawn.h). A program of its own: it needs no
// game, no ReXGlue and no log, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_cars_test      (CMakeLists.txt)
//   <build dir>/larecomp_cars_test
//
// The compile by hand (the msvc-target clang of labuild, include path src/mc_engine/cinematic):
//   clang++ -std=c++23 -Wall -Wextra src/mc_engine/cinematic/cars_test.cpp \
//       src/mc_engine/cinematic/car_registry.cpp src/mc_engine/cinematic/car_spawn.cpp -o cars_test.exe

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "car_registry.h"
#include "car_spawn.h"
#include "car_verbs.h"

using namespace mc::cinematic;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAILED: %s\n", what.c_str());
    }
}

void CheckNum(double got, double want, const std::string& what, double tol = 1e-6) {
    ++g_checks;
    if (!(std::fabs(got - want) <= tol)) {
        ++g_failed;
        std::printf("FAILED: %s: got %.9g, want %.9g\n", what.c_str(), got, want);
    }
}

std::vector<std::string> Words(const std::string& line) {
    std::vector<std::string> w;
    std::string cur;
    for (char c : line) {
        if (c == ' ') {
            if (!cur.empty()) w.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) w.push_back(cur);
    return w;
}

struct Parsed {
    CarParse status;
    CarCommand cmd;
    std::string error;
};
Parsed P(const std::string& words) {
    Parsed p;
    p.status = ParseCarCommand(Words(words), &p.cmd, &p.error);
    return p;
}

void CheckError(const std::string& words, const std::string& part) {
    const Parsed p = P(words);
    Check(p.status == CarParse::kError && p.error.find(part) != std::string::npos,
          "car " + words + ": error holding '" + part + "' (got status " + std::to_string(static_cast<int>(p.status)) + ", '" + p.error + "')");
}

// ---------------------------------------------------------------------------------------------------------------- the verbs

void TestVerbs() {
    {
        const Parsed p = P("p1 spawn vp_lam_murcielago_06 at 900,17.5,-850 heading 270");
        Check(p.status == CarParse::kOk, "spawn by model parses");
        Check(p.cmd.verb == CarVerb::kSpawn && p.cmd.id == "p1" && p.cmd.source == CarSource::kModel, "spawn: verb, id, source");
        Check(p.cmd.model == "vp_lam_murcielago_06", "spawn: the model");
        CheckNum(p.cmd.position[0], 900.0, "x");
        CheckNum(p.cmd.position[1], 17.5, "y");
        CheckNum(p.cmd.position[2], -850.0, "z");
        CheckNum(p.cmd.heading, 270.0, "heading");
        Check(p.cmd.has_heading, "has_heading");
    }
    {
        const Parsed p = P("p2 SPAWN VP_AUD_R8_08 AT 1 2 3");  // three numbers, upper case: the game's names are lower case
        Check(p.status == CarParse::kOk && p.cmd.model == "vp_aud_r8_08", "spawn: upper case words, model lowered");
        CheckNum(p.cmd.position[2], 3.0, "three-number position");
        Check(!p.cmd.has_heading && p.cmd.heading == 0.0, "no heading: 0 and not given");
    }
    {
        const Parsed p = P("p3 spawn profile 5 heading -90 at 1,2,3");  // heading first; negative wraps
        Check(p.status == CarParse::kOk && p.cmd.source == CarSource::kGarage && p.cmd.slot == 5, "spawn profile 5, heading before at");
        CheckNum(p.cmd.heading, 270.0, "heading -90 is 270");
    }
    for (const char* spelling : {"p4 spawn garage:7 at 0,0,0", "p4 spawn slot:7 at 0 0 0"}) {
        const Parsed p = P(spelling);
        Check(p.status == CarParse::kOk && p.cmd.source == CarSource::kGarage && p.cmd.slot == 7, std::string(spelling) + ": garage slot 7");
    }
    CheckNum(P("p1 spawn x at 0,0,0 heading 725").cmd.heading, 5.0, "heading 725 is 5");
    CheckNum(P("p1 spawn x at 0,0,0 heading 360").cmd.heading, 0.0, "heading 360 is 0");
    {
        const Parsed p = P("p1 delete");
        Check(p.status == CarParse::kOk && p.cmd.verb == CarVerb::kDelete && p.cmd.id == "p1", "delete");
        Check(P("p1 despawn").status == CarParse::kOk && P("p1 despawn").cmd.verb == CarVerb::kDelete, "despawn is delete");
    }
    {
        const Parsed p = P("player profile 3");
        Check(p.status == CarParse::kOk && p.cmd.verb == CarVerb::kProfile && p.cmd.slot == 3 && p.cmd.id == "player", "player profile 3");
        Check(P("Player profile 0").status == CarParse::kOk, "Player (any case) profile 0");
    }
    // other packages' verbs pass
    Check(P("p1 path route sunset.route speed 1.0").status == CarParse::kOtherVerb, "a path is another package's");
    Check(P("p1 speed 3").status == CarParse::kOtherVerb, "speed is another package's");
    Check(P("player follow route a.route").status == CarParse::kOtherVerb, "a player verb nobody here knows passes");

    CheckError("p1", "car <id>");
    CheckError("p1 spawn", "spawn what?");
    CheckError("p1 spawn vp_x", "needs a place");
    CheckError("p1 spawn vp_x at", "at needs x,y,z");
    CheckError("p1 spawn vp_x at 1,2", "bad position");
    CheckError("p1 spawn vp_x at 1,2,3,4", "bad position");
    CheckError("p1 spawn vp_x at 1 2", "at needs x,y,z");
    CheckError("p1 spawn vp_x at 1 2 z", "bad position 'z'");
    CheckError("p1 spawn vp_x at a,b,c", "bad position");
    CheckError("p1 spawn vp_x at 1,2,3 at 4,5,6", "a second 'at'");
    CheckError("p1 spawn vp_x at 1,2,3 heading", "heading needs degrees");
    CheckError("p1 spawn vp_x at 1,2,3 heading north", "heading needs degrees");
    CheckError("p1 spawn vp_x at 1,2,3 heading 1 heading 2", "a second 'heading'");
    CheckError("p1 spawn vp_x at 1,2,3 colour red", "unknown option 'colour'");
    CheckError("p1 spawn vp_x at 999999,0,0", "outside the world");
    CheckError("p1 spawn vp_x at nan,0,0", "bad position");
    CheckError("p1 spawn profile at 1,2,3", "garage slot");
    CheckError("p1 spawn profile -1 at 1,2,3", "garage slot");
    CheckError("p1 spawn profile 1000 at 1,2,3", "garage slot");
    CheckError("p1 spawn car:3 at 1,2,3", "is not a car");
    CheckError("p1 spawn garage:x at 1,2,3", "is not a car");
    CheckError("p1 spawn Vp.Bad/Name at 1,2,3", "not a vehicle name");
    CheckError("p1 delete now", "takes nothing");
    CheckError("player spawn vp_x at 1,2,3", "'player' is the player's own car");
    CheckError("player delete", "'player' is the player's own car");
    CheckError("player profile", "car player profile <N>");
    CheckError("player profile x", "car player profile <N>");
    CheckError("player profile 1 2", "car player profile <N>");
    CheckError("p1 profile 3", "only the player's car");
}

// ---------------------------------------------------------------------------------------------------------------- headings and poses

void MatrixForNativeHeading(double native_deg, const double pos[3], float m[16]) {
    const double a = native_deg * 3.14159265358979323846 / 180.0;
    // Racer_Teleport: the back row is (sin a, 0, cos a); the right row is up x back's partner: (cos a, 0, -sin a)
    const float v[16] = {static_cast<float>(std::cos(a)), 0.f, static_cast<float>(-std::sin(a)), 0.f,
                         0.f, 1.f, 0.f, 0.f,
                         static_cast<float>(std::sin(a)), 0.f, static_cast<float>(std::cos(a)), 0.f,
                         static_cast<float>(pos[0]), static_cast<float>(pos[1]), static_cast<float>(pos[2]), 1.f};
    for (int i = 0; i < 16; ++i) m[i] = v[i];
}

void TestPoses() {
    CheckNum(NativeHeadingDeg(0.0), 180.0, "scene 0 is native 180");
    CheckNum(NativeHeadingDeg(270.0), 90.0, "scene 270 is native 90");
    CheckNum(NativeHeadingDeg(-90.0), 90.0, "scene -90 is native 90");
    CheckNum(SceneHeadingDeg(0.0, 1.0), 0.0, "forward +Z is 0");
    CheckNum(SceneHeadingDeg(1.0, 0.0), 90.0, "forward +X is 90");
    CheckNum(SceneHeadingDeg(0.0, -1.0), 180.0, "forward -Z is 180");
    CheckNum(SceneHeadingDeg(-1.0, 0.0), 270.0, "forward -X is 270");
    const double pos[3] = {900.0, 17.5, -850.0};
    for (double h : {0.0, 30.0, 61.7, 90.0, 180.0, 241.7, 270.0, 359.0}) {
        float m[16];
        MatrixForNativeHeading(NativeHeadingDeg(h), pos, m);
        CarPose pose;
        Check(PoseFromMatrix(m, &pose), "a car matrix at heading " + std::to_string(h));
        CheckNum(pose.heading, h, "scene heading -> native -> matrix -> pose heading " + std::to_string(h), 1e-4);
        CheckNum(pose.position[0], 900.0, "pose x");
        CheckNum(pose.position[2], -850.0, "pose z");
        CheckNum(pose.forward[0], std::sin(h * 3.14159265358979323846 / 180.0), "forward x is sin(heading)", 1e-5);
        CheckNum(pose.forward[2], std::cos(h * 3.14159265358979323846 / 180.0), "forward z is cos(heading)", 1e-5);
    }
    float bad[16] = {};
    CarPose pose;
    Check(!PoseFromMatrix(bad, &pose), "an all-zero matrix is no car's");
    float m[16];
    MatrixForNativeHeading(10.0, pos, m);
    m[13] = NAN;
    Check(!PoseFromMatrix(m, &pose), "a matrix with a NaN is no car's");
    MatrixForNativeHeading(10.0, pos, m);
    m[14] = INFINITY;
    Check(!PoseFromMatrix(m, &pose), "a matrix with an infinity is no car's");
}

void TestRegistry() {
    CarRegistry reg;
    Check(reg.Add("player", true) != nullptr && reg.Find("player")->is_player, "player added");
    Check(reg.Add("p1", false) != nullptr, "p1 added");
    Check(reg.Add("p1", false) == nullptr, "a second p1 is refused");
    Check(reg.Size() == 2 && reg.Ids() == std::vector<std::string>({"player", "p1"}), "ids in the order added");
    float m[16];
    Check(!reg.Matrix("p1", m), "no pose yet");
    const double pos[3] = {1.0, 2.0, 3.0};
    MatrixForNativeHeading(NativeHeadingDeg(90.0), pos, m);
    Check(reg.UpdatePose("p1", m, 7), "a pose is stored");
    CheckNum(reg.Find("p1")->pose.heading, 90.0, "stored heading");
    Check(reg.Find("p1")->pose_frame == 7 && reg.Find("p1")->pose_updates == 1, "frame and count");
    float out[16];
    Check(reg.Matrix("p1", out) && out[12] == 1.0f && out[14] == 3.0f, "the matrix comes back");
    float bad[16] = {};
    Check(!reg.UpdatePose("p1", bad, 8), "a bad matrix is refused");
    CheckNum(reg.Find("p1")->pose.heading, 90.0, "the last good pose stays");
    Check(reg.Find("p1")->pose_frame == 7, "...and its frame");
    Check(!reg.UpdatePose("nobody", m, 9), "an unknown id is refused");
    Check(reg.Remove("p1") && !reg.Remove("p1") && reg.Size() == 1, "remove");
}

// ---------------------------------------------------------------------------------------------------------------- a pretend game

struct FakeGame {
    std::vector<std::string> calls;  // native names in order
    std::map<std::string, int> count;
    std::vector<Call> detail;
    uint32_t next_node = 0xBDAE19B0;
    bool pool_empty = false;
    int profiles = 28;
    int current_profile = 27;
    int64_t stream_delay_ms = 400;
    int64_t added_at = -1;
    bool uncallable = false;
    std::vector<uint32_t> added_nodes;
    std::vector<uint32_t> deleted_nodes;
    uint32_t last_config = 0;

    Outcome Run(const Call& c, int64_t now) {
        Outcome o;
        o.called = !uncallable;
        o.ret_written = true;
        calls.push_back(NativeName(c.native));
        ++count[NativeName(c.native)];
        detail.push_back(c);
        switch (c.native) {
            case Native::kLookupCar:
                o.ret = c.text == "vp_lam_murcielago_06" ? 40u : (c.text == "vp_aud_r8_08" ? 12u : 0xFFFFFFFFu);
                break;
            case Native::kHostCarName: o.ret = 0xBD300000u + static_cast<uint32_t>(c.index); break;
            case Native::kNumProfiles: o.ret = static_cast<uint32_t>(profiles); break;
            case Native::kCurrentProfile: o.ret = static_cast<uint32_t>(current_profile); break;
            case Native::kGetProfile:
                o.ret = (c.index >= 0 && c.index < profiles) ? 0xBF000000u + static_cast<uint32_t>(c.index) * 8176u : 0u;
                break;
            case Native::kCreateOpponent:
                if (pool_empty) {
                    o.ret = 0;
                } else {
                    o.ret = next_node;
                    next_node += 0x360;
                }
                break;
            case Native::kAddOpponent:
                added_at = now;
                added_nodes.push_back(c.racer);
                o.ret_written = false;
                break;
            case Native::kGetLoadState: o.ret = (added_at >= 0 && now - added_at >= stream_delay_ms) ? 3u : 0u; break;
            case Native::kIsCarStreamed: o.ret = 1; break;
            case Native::kDeleteOpponent:
                deleted_nodes.push_back(c.racer);
                o.ret_written = false;
                break;
            case Native::kChangeCarByConfig: last_config = c.ptr; o.ret_written = false; break;
            default: o.ret_written = false; break;
        }
        return o;
    }
    int N(const char* name) const {
        const auto it = count.find(name);
        return it == count.end() ? 0 : it->second;
    }
};

CarCommand Spawn(const std::string& id, const std::string& model, double x = 900.0, double z = -850.0, double heading = 270.0) {
    CarCommand c;
    c.id = id;
    c.verb = CarVerb::kSpawn;
    c.source = CarSource::kModel;
    c.model = model;
    c.position[0] = x;
    c.position[1] = 17.5;
    c.position[2] = z;
    c.heading = heading;
    c.has_heading = true;
    return c;
}
CarCommand Garage(const std::string& id, int slot) {
    CarCommand c = Spawn(id, "");
    c.source = CarSource::kGarage;
    c.slot = slot;
    return c;
}
CarCommand Delete(const std::string& id) {
    CarCommand c;
    c.id = id;
    c.verb = CarVerb::kDelete;
    return c;
}
CarCommand Profile(int slot) {
    CarCommand c;
    c.id = "player";
    c.verb = CarVerb::kProfile;
    c.slot = slot;
    return c;
}

// Runs the director from `from` to `to` in 16 ms updates, one Poll an update, the way the game side does. Returns the last time.
int64_t Drive(Director& dir, FakeGame& game, int64_t from, int64_t to, uint32_t player = 0xBF33C450u, int* max_calls_per_poll = nullptr) {
    int64_t t = from;
    for (; t <= to; t += 16) {
        Director::Next n = dir.Poll(t, player);
        if (n.call.native != Native::kNone) {
            // a second Poll without a Report hands out nothing (one native in flight)
            Director::Next again = dir.Poll(t, player);
            if (again.call.native != Native::kNone && max_calls_per_poll) ++*max_calls_per_poll;
            dir.Report(game.Run(n.call, t));
        }
    }
    return t;
}

bool HasEvent(Director& dir, const std::string& part, bool warning_only = false) {
    for (const CarEvent& e : dir.TakeEvents()) {
        if (e.text.find(part) != std::string::npos && (!warning_only || e.warning)) return true;
    }
    return false;
}

std::vector<std::string> Seq(const FakeGame& g, size_t from = 0) {
    return std::vector<std::string>(g.calls.begin() + static_cast<std::ptrdiff_t>(from), g.calls.end());
}

void TestSpawnByModel() {
    Director dir;
    FakeGame game;
    dir.BeginScene();
    const Director::Applied a = dir.Apply(Spawn("p1", "vp_lam_murcielago_06"));
    Check(a.ok, "apply spawn");
    Check(dir.Active() && dir.registry().Find("p1") && dir.registry().Find("player"), "active; p1 and the player are in the registry");
    int double_issue = 0;
    int64_t t = Drive(dir, game, 0, 12000, 0xBF33C450u, &double_issue);
    (void)t;
    Check(double_issue == 0, "no second call is handed out while one is in flight");
    const std::vector<std::string> want = {"LookupCar", "car table name", "OpponentManager_CreateOpponent", "OpponentManager_LoadOpponent",
                                           "OpponentManager_AddOpponentToWorld"};
    Check(game.calls.size() >= 5 && std::vector<std::string>(game.calls.begin(), game.calls.begin() + 5) == want, "the first five calls: lookup, name, create, load, add");
    // the stream polls, then activate, lock, teleport, verify
    Check(game.N("Racer_GetLoadState") >= 1 && game.N("Racer_IsCarStreamed") == 1, "the load state is polled until 3, then streamed once");
    Check(game.calls.size() >= 5 && game.N("Racer_Activate") == 1 && game.N("Racer_SetStreamingLockedIn") == 1 && game.N("Racer_Teleport") == 1 &&
              game.N("Racer_GetPosition") == 1,
          "activate, lock, teleport and the position read once each");
    const size_t at_act = static_cast<size_t>(std::find(game.calls.begin(), game.calls.end(), "Racer_Activate") - game.calls.begin());
    Check(Seq(game, at_act) == std::vector<std::string>({"Racer_Activate", "Racer_SetStreamingLockedIn", "Racer_Teleport", "Racer_GetPosition"}),
          "after the stream: activate, lock, teleport, position, and nothing else");
    Check(game.N("OpponentManager_DeleteOpponent") == 0, "no delete while the scene runs");
    // the arguments
    for (const Call& c : game.detail) {
        if (c.native == Native::kLookupCar) Check(c.text == "vp_lam_murcielago_06", "lookup gets the model");
        if (c.native == Native::kLoadOpponent) {
            Check(c.ptr == 0xBD300000u + 40u, "load gets the game's own name pointer");
            Check(c.racer == 0xBDAE19B0u, "load is on the created opponent");
            CheckNum(c.position[0], 900.0, "load x");
            CheckNum(c.position[2], -850.0, "load z");
            CheckNum(c.heading_native, 90.0, "load heading is scene 270 + 180");
        }
        if (c.native == Native::kTeleport) {
            CheckNum(c.heading_native, 90.0, "teleport heading");
            Check(c.racer == 0xBDAE19B0u, "teleport is on the opponent");
        }
    }
    Check(dir.spawned() == 1, "one car is ready");
    Check(HasEvent(dir, "car p1 is ready"), "the ready line");
    // before the entity exists nobody reads a pose; after the stream the car is a reader
    const auto readers = dir.PoseReaders();
    Check(readers.size() == 1 && readers[0].id == "p1" && readers[0].racer == 0xBDAE19B0u, "p1 is a pose reader once it is ready");
    Check(dir.InWorld().size() == 1, "p1 is in the world");

    // the scene ends: the car goes
    dir.EndScene();
    Check(dir.registry().Find("p1") == nullptr && dir.registry().Find("player") == nullptr, "the registry is empty after EndScene");
    Check(dir.PoseReaders().empty(), "an ended car is no pose reader");
    Drive(dir, game, 12016, 20000);
    Check(game.N("OpponentManager_DeleteOpponent") == 1 && game.deleted_nodes == std::vector<uint32_t>({0xBDAE19B0u}), "the car is deleted once, by its node");
    Check(!dir.Active(), "nothing left to do");
    Check(dir.deleted() == 1 && dir.failed() == 0, "counts: one deleted, none failed");
    Check(HasEvent(dir, "car p1 is deleted"), "the deleted line");
}

void TestSpawnByGarage() {
    Director dir;
    FakeGame game;
    dir.BeginScene();
    dir.Apply(Garage("g1", 1));
    Drive(dir, game, 0, 9000);
    Check(game.N("LocalOptions_GetNumValidVehicleProfiles") == 1 && game.N("LocalOptions_GetVehicleProfile") == 1, "the garage is asked for the count and the profile");
    Check(game.N("OpponentManager_LoadOpponentByCarConfig") == 1 && game.N("OpponentManager_LoadOpponent") == 0 && game.N("LookupCar") == 0,
          "the config route loads by config and never looks a name up");
    for (const Call& c : game.detail) {
        if (c.native == Native::kLoadOpponentByConfig) Check(c.ptr == 0xBF000000u + 8176u, "the config is slot 1's");
        if (c.native == Native::kGetProfile) Check(c.index == 1, "profile index 1");
    }
    Check(dir.spawned() == 1, "the garage car is ready");

    // a slot that is not there: nothing is created
    Director dir2;
    FakeGame game2;
    dir2.BeginScene();
    dir2.Apply(Garage("g9", 28));
    Drive(dir2, game2, 0, 3000);
    Check(game2.N("OpponentManager_CreateOpponent") == 0, "no opponent for a missing garage slot");
    Check(dir2.failed() == 1 && dir2.Active(), "failed once (the scene is still on)");
    Check(HasEvent(dir2, "garage slot 28 does not exist", true), "a warning that says why");
}

void TestSpawnFailures() {
    {  // a car the game does not have
        Director dir;
        FakeGame game;
        dir.BeginScene();
        dir.Apply(Spawn("p1", "vp_no_such_car"));
        Drive(dir, game, 0, 3000);
        Check(game.N("OpponentManager_CreateOpponent") == 0, "an unknown car creates nothing");
        Check(dir.failed() == 1, "...and fails");
        Check(HasEvent(dir, "has no car called 'vp_no_such_car'", true), "the warning names the car");
        Check(dir.registry().Find("p1") == nullptr, "a failed car leaves the registry");
    }
    {  // the pool is empty
        Director dir;
        FakeGame game;
        game.pool_empty = true;
        dir.BeginScene();
        dir.Apply(Spawn("p1", "vp_lam_murcielago_06"));
        Drive(dir, game, 0, 3000);
        Check(game.N("OpponentManager_LoadOpponent") == 0 && game.N("OpponentManager_AddOpponentToWorld") == 0, "no load or add after an empty pool");
        Check(dir.failed() == 1 && game.N("OpponentManager_DeleteOpponent") == 0, "failed, and nothing to delete");
    }
    {  // never streams in: failed, and the node is given back
        Director dir;
        FakeGame game;
        game.stream_delay_ms = 1000000;
        dir.BeginScene();
        dir.Apply(Spawn("p1", "vp_lam_murcielago_06"));
        Drive(dir, game, 0, 40000);
        Check(game.N("Racer_Activate") == 0 && game.N("Racer_Teleport") == 0, "a car that never streams is never placed");
        Check(game.N("OpponentManager_DeleteOpponent") == 1, "...and is deleted");
        Check(dir.failed() == 1 && HasEvent(dir, "was not streamed in 20 s", true), "failed with the stream timeout");
    }
    {  // a native that cannot be called
        Director dir;
        FakeGame game;
        game.uncallable = true;
        dir.BeginScene();
        dir.Apply(Spawn("p1", "vp_lam_murcielago_06"));
        Drive(dir, game, 0, 3000);
        Check(game.calls.size() == 1, "one try, then it stops");
        Check(dir.failed() == 1, "failed");
    }
}

void TestIdsAndDeletes() {
    Director dir;
    FakeGame game;
    dir.BeginScene();
    Check(dir.Apply(Spawn("p1", "vp_lam_murcielago_06")).ok, "p1");
    const Director::Applied dup = dir.Apply(Spawn("p1", "vp_aud_r8_08"));
    Check(!dup.ok && dup.message.find("already exists") != std::string::npos, "a second p1 is ignored with a message");
    const Director::Applied none = dir.Apply(Delete("p9"));
    Check(!none.ok && none.message.find("does not exist") != std::string::npos, "deleting an unknown id says so");
    Check(dir.SceneCarCount() == 1, "one scene car");

    // delete asked for in the middle of the spawn: the spawn runs to its end, then the car goes
    Drive(dir, game, 0, 1200);
    Check(game.N("OpponentManager_LoadOpponent") == 1 && game.N("OpponentManager_AddOpponentToWorld") == 0, "the load happened, the add has not");
    Check(dir.Apply(Delete("p1")).ok, "delete p1 mid-spawn");
    Check(dir.registry().Find("p1") == nullptr, "p1 is no longer reachable by id at once");
    // ... and its id is free again for a new car while the old one finishes
    Check(dir.Apply(Spawn("p1", "vp_aud_r8_08", 905.0, -845.0)).ok, "the id p1 can be spawned again");
    Check(dir.registry().Find("p1") != nullptr, "the new p1 is in the registry");
    Drive(dir, game, 1216, 16000);
    Check(game.N("OpponentManager_AddOpponentToWorld") == 2 && game.N("Racer_Teleport") == 2, "both cars ran their whole spawn");
    Check(game.N("OpponentManager_DeleteOpponent") == 1, "only the first car was deleted");
    // the delete came after the first car's teleport, not before
    const auto first_delete = std::find(game.calls.begin(), game.calls.end(), "OpponentManager_DeleteOpponent") - game.calls.begin();
    const auto first_teleport = std::find(game.calls.begin(), game.calls.end(), "Racer_Teleport") - game.calls.begin();
    Check(first_delete > first_teleport, "the delete came after the first teleport");
    Check(game.deleted_nodes.size() == 1 && game.deleted_nodes[0] == 0xBDAE19B0u, "the first node was the one deleted");
    Check(dir.spawned() == 2 && dir.deleted() == 1, "two made, one deleted");
    Check(dir.registry().Find("p1") != nullptr && dir.registry().Find("p1")->racer == 0 /* the glue sets racers */, "the new p1 stays");

    // the scene ends: the new p1 goes too
    dir.EndScene();
    Drive(dir, game, 16016, 24000);
    Check(game.deleted_nodes.size() == 2 && game.deleted_nodes[1] == 0xBDAE19B0u + 0x360u, "the second node was deleted at the end");
    Check(!dir.Active() && dir.deleted() == 2, "all gone, all counted");
}

void TestEndMidSpawn() {
    Director dir;
    FakeGame game;
    dir.BeginScene();
    dir.Apply(Spawn("a", "vp_lam_murcielago_06"));
    dir.Apply(Spawn("b", "vp_aud_r8_08", 905.0, -845.0));
    Drive(dir, game, 0, 600);  // a is created and loaded, b is behind it
    const int created = game.N("OpponentManager_CreateOpponent");
    dir.EndScene();
    Drive(dir, game, 616, 40000);
    Check(created >= 1 && created <= 2, "a is created, b perhaps");
    Check(game.N("OpponentManager_DeleteOpponent") == game.N("OpponentManager_CreateOpponent"), "every opponent that was created is deleted");
    Check(game.N("OpponentManager_DeleteOpponent") >= 1, "at least the car that was mid-spawn is deleted");
    // a car deleted only after its own teleport
    for (const uint32_t node : game.deleted_nodes) {
        size_t del = 0;
        for (size_t i = 0; i < game.detail.size(); ++i) {
            if (game.detail[i].native == Native::kDeleteOpponent && game.detail[i].racer == node) del = i;
        }
        bool teleported = false;
        for (size_t i = 0; i < del; ++i) {
            if (game.detail[i].native == Native::kTeleport && game.detail[i].racer == node) teleported = true;
        }
        Check(teleported, "a node is deleted only after its teleport");
    }
    Check(!dir.Active(), "nothing left");

    // a spawn that is asked to go before anything exists is simply dropped
    Director dir2;
    FakeGame game2;
    dir2.BeginScene();
    dir2.Apply(Spawn("c", "vp_lam_murcielago_06"));
    dir2.EndScene();
    Drive(dir2, game2, 0, 2000);
    Check(game2.calls.empty() && !dir2.Active(), "a spawn dropped before its first call makes no call");
    Check(HasEvent(dir2, "was dropped before it was made"), "and says so");
}

void TestOneCallAnUpdate() {
    Director dir;
    FakeGame game;
    dir.BeginScene();
    for (int i = 0; i < 6; ++i) dir.Apply(Spawn("c" + std::to_string(i), "vp_lam_murcielago_06", 900.0 + i * 5.0));
    int worst = 0;
    for (int64_t t = 0; t < 20000; t += 16) {
        int issued = 0;
        Director::Next n = dir.Poll(t, 0xBF33C450u);
        while (n.call.native != Native::kNone) {
            ++issued;
            dir.Report(game.Run(n.call, t));
            break;  // the game side polls once an update
        }
        worst = std::max(worst, issued);
    }
    Check(worst == 1, "a poll gives at most one call");
    Check(dir.spawned() == 6, "six cars made in parallel");
    // the carriers are distinct opponents
    std::map<uint32_t, int> nodes;
    for (const Call& c : game.detail) {
        if (c.native == Native::kTeleport) ++nodes[c.racer];
    }
    Check(nodes.size() == 6, "six distinct opponents were placed");
    dir.EndScene();
    Drive(dir, game, 20000, 40000);
    Check(game.deleted_nodes.size() == 6 && !dir.Active(), "all six deleted at the end");
}

void TestPlayerDress() {
    {
        Director dir;
        FakeGame game;
        dir.BeginScene();
        dir.Apply(Profile(3));
        Drive(dir, game, 0, 5000);
        Check(Seq(game) == std::vector<std::string>({"LocalOptions_GetCurrentVehicleProfileIndex", "LocalOptions_GetNumValidVehicleProfiles", "LocalOptions_GetVehicleProfile",
                                                     "Racer_ChangeCarByConfig"}),
              "dress: the current slot is read, the count, the profile, then the change");
        Check(game.last_config == 0xBF000000u + 3u * 8176u, "the change gets slot 3's config");
        Check(game.detail.back().racer == 0xBF33C450u, "...on the player's racer");
        Check(dir.dresser().Changed(), "the dresser knows it changed the car");
        Check(HasEvent(dir, "the player's car is now garage slot 3; its parts finish streaming"), "the log line for the change");
        const size_t before = game.calls.size();
        dir.EndScene();
        Drive(dir, game, 5016, 12000);
        Check(Seq(game, before) == std::vector<std::string>({"LocalOptions_GetVehicleProfile", "Racer_ChangeCarByConfig"}), "the end restores: profile, change");
        Check(game.last_config == 0xBF000000u + 27u * 8176u, "the restore is the slot it wore before (27)");
        Check(!dir.dresser().Changed() && !dir.Active(), "restored and idle");
        Check(HasEvent(dir, "the player's own car is back (garage slot 27); its parts finish streaming"), "the log line for the way back");
    }
    {  // two dressings in a row: the original is read once; one restore at the end
        Director dir;
        FakeGame game;
        dir.BeginScene();
        dir.Apply(Profile(1));
        dir.Apply(Profile(2));
        Drive(dir, game, 0, 12000);
        Check(game.N("LocalOptions_GetCurrentVehicleProfileIndex") == 1, "the original slot is read once");
        Check(game.N("Racer_ChangeCarByConfig") == 2, "two changes");
        dir.EndScene();
        Drive(dir, game, 12016, 20000);
        Check(game.N("Racer_ChangeCarByConfig") == 3 && game.last_config == 0xBF000000u + 27u * 8176u, "one restore, to slot 27");
    }
    {  // the slot the player already wears: nothing is changed
        Director dir;
        FakeGame game;
        dir.BeginScene();
        dir.Apply(Profile(27));
        Drive(dir, game, 0, 3000);
        Check(game.N("Racer_ChangeCarByConfig") == 0, "no change to the slot already worn");
        Check(HasEvent(dir, "already is garage slot 27"), "and the log says so");
        dir.EndScene();
        Drive(dir, game, 3016, 6000);
        Check(game.N("Racer_ChangeCarByConfig") == 0 && !dir.Active(), "nothing to restore");
    }
    {  // a slot that does not exist
        Director dir;
        FakeGame game;
        dir.BeginScene();
        dir.Apply(Profile(40));
        Drive(dir, game, 0, 3000);
        Check(game.N("Racer_ChangeCarByConfig") == 0 && HasEvent(dir, "garage slot 40 does not exist", true), "a missing slot changes nothing and warns");
    }
    {  // no player racer: no call
        Director dir;
        FakeGame game;
        dir.BeginScene();
        dir.Apply(Profile(3));
        Drive(dir, game, 0, 3000, 0);
        Check(game.calls.empty(), "without a player racer nothing is called");
        Drive(dir, game, 3016, 9000, 0xBF33C450u);
        Check(game.N("Racer_ChangeCarByConfig") == 1, "...and it goes on when the player is there");
    }
    {  // a dressing that is still settling when the scene ends is completed, then restored
        Director dir;
        FakeGame game;
        dir.BeginScene();
        dir.Apply(Profile(3));
        Drive(dir, game, 0, 100);
        dir.EndScene();
        Drive(dir, game, 116, 15000);
        Check(game.N("Racer_ChangeCarByConfig") == 2 && game.last_config == 0xBF000000u + 27u * 8176u, "a dressing in flight completes, then the original comes back");
    }
}

void TestLost() {
    Director dir;
    FakeGame game;
    dir.BeginScene();
    dir.Apply(Spawn("p1", "vp_lam_murcielago_06"));
    Drive(dir, game, 0, 9000);
    Check(dir.spawned() == 1 && dir.InWorld().size() == 1, "ready");
    dir.MarkLost(0xBDAE19B0u);
    Check(dir.InWorld().empty(), "a lost car is no longer expected in the world");
    const size_t before = game.calls.size();
    dir.EndScene();
    Drive(dir, game, 9016, 14000);
    Check(game.calls.size() == before && game.N("OpponentManager_DeleteOpponent") == 0, "nothing is called on a lost car");
    Check(!dir.Active(), "the lost car is gone from the books");
}

// The first smoke run's scene end: the scene's own cleanup deletes the cars, which takes them out of the opponent manager's list at
// once, and a few updates later the game side's periodic look at the cars "that should be in the list" (InWorld) found them missing
// and warned "not in the opponent manager's list any more (the world was rebuilt?)" for both, as if something else had removed them.
// A car being deleted is not expected in the list: InWorld() leaves it out from the delete call on.
void TestDeleteIsNotLost() {
    Director dir;
    FakeGame game;
    dir.BeginScene();
    dir.Apply(Spawn("p1", "vp_lam_murcielago_06"));
    Drive(dir, game, 0, 9000);
    Check(dir.spawned() == 1 && dir.InWorld().size() == 1, "ready, and expected in the manager's list");
    dir.EndScene();
    int64_t t = 9016;
    for (; t < 30000 && game.N("OpponentManager_DeleteOpponent") == 0; t += 16) Drive(dir, game, t, t);
    Check(game.N("OpponentManager_DeleteOpponent") == 1, "the scene's cleanup deletes the car");
    Check(dir.Active(), "the manager is still given time to clean up");
    Check(dir.InWorld().empty(), "a car that has been deleted is no longer expected in the manager's list (no warning for its absence)");
    // a look at the list at any moment of the settle finds nothing to report; the car then ends as deleted, not as lost
    dir.TakeEvents();
    Drive(dir, game, t, t + 5000);
    Check(!dir.Active() && dir.deleted() == 1 && dir.failed() == 0, "the car ends deleted, none failed");
    bool warned = false;
    for (const CarEvent& e : dir.TakeEvents()) {
        if (e.warning || e.text.find("world was rebuilt") != std::string::npos || e.text.find("no longer has the car") != std::string::npos) warned = true;
    }
    Check(!warned, "no warning about a lost car at the scene's end");
    // the opposite case stays: a car the scene did not delete that is missing from the list is lost, and says so
    Director lost;
    FakeGame game2;
    lost.BeginScene();
    lost.Apply(Spawn("p2", "vp_lam_murcielago_06"));
    Drive(lost, game2, 0, 9000);
    Check(lost.InWorld().size() == 1, "a car that is ready is expected in the list");
    lost.MarkLost(0xBDAE19B0u);
    Drive(lost, game2, 9016, 9100);
    Check(lost.InWorld().empty() && lost.failed() == 1, "a car the game lost counts as failed");
}

}  // namespace

int main() {
    TestVerbs();
    TestPoses();
    TestRegistry();
    TestSpawnByModel();
    TestSpawnByGarage();
    TestSpawnFailures();
    TestIdsAndDeletes();
    TestEndMidSpawn();
    TestOneCallAnUpdate();
    TestPlayerDress();
    TestLost();
    TestDeleteIsNotLost();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
