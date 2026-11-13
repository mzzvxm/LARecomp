// Check of the scene's car statements and bookkeeping (car_verbs.h, car_registry.h, car_spawn.h). A program of its own: it needs no
// game, no ReXGlue and no log, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_cars_test      (CMakeLists.txt)
//   <build dir>/larecomp_cars_test
//
// The compile by hand (the msvc-target clang of labuild, include path src/mc_engine/cinematic):
//   clang++ -std=c++23 -Wall -Wextra src/mc_engine/cinematic/cars_test.cpp src/mc_engine/cinematic/car_registry.cpp \
//       src/mc_engine/cinematic/car_spawn.cpp src/mc_engine/cinematic/path_follow.cpp -o cars_test.exe

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
    Check(P("p1 speed 3").status == CarParse::kOtherVerb, "speed is not a car verb (it is an option of path): it passes the parser");
    Check(P("p1 fly").status == CarParse::kOtherVerb, "a verb nobody here knows passes");
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

void TestPathVerbs() {
    {
        const Parsed p = P("a path street.route");
        Check(p.status == CarParse::kOk && p.cmd.verb == CarVerb::kPath && p.cmd.id == "a" && p.cmd.route == "street.route", "path: verb, id, route");
        Check(p.cmd.mode == PathMode::kKinematic, "the default mode is kinematic");
        Check(p.cmd.speed.kind == SpeedKind::kFile, "the default speed is the route's own");
    }
    {
        const Parsed p = P("b PATH routes/lane-b.route SPEED 14 MODE Brain");
        Check(p.status == CarParse::kOk && p.cmd.route == "routes/lane-b.route", "a route in a folder; the words in any case");
        Check(p.cmd.speed.kind == SpeedKind::kMetersPerSecond && p.cmd.speed.value == 14.0, "speed 14 m/s");
        Check(p.cmd.mode == PathMode::kBrain, "mode brain");
    }
    {
        const Parsed p = P("b path a.route mode kinematic speed x1.5");
        Check(p.status == CarParse::kOk && p.cmd.mode == PathMode::kKinematic && p.cmd.speed.kind == SpeedKind::kScale && p.cmd.speed.value == 1.5, "options in the other order; a factor");
        Check(P("b path a.route speed 2x").cmd.speed.value == 2.0, "2x is a factor too");
        Check(P("b path dir\\sub\\a.route").status == CarParse::kOk, "a backslash path");
    }
    {
        const Parsed p = P("a stop");
        Check(p.status == CarParse::kOk && p.cmd.verb == CarVerb::kStop && p.cmd.id == "a", "stop");
    }
    CheckError("a path", "path needs a route file");
    CheckError("a path ../secret.route", "not a route file name");
    CheckError("a path a/../../b.route", "not a route file name");
    CheckError("a path /abs/x.route", "not a route file name");
    CheckError("a path \\abs\\x.route", "not a route file name");
    CheckError("a path C:x.route", "not a route file name");
    CheckError("a path a;b.route", "not a route file name");
    CheckError("a path ./a.route", "not a route file name");
    CheckError("a path a.route speed", "speed needs metres a second");
    CheckError("a path a.route speed fast", "speed needs metres a second");
    CheckError("a path a.route speed 200", "from 0.5 to 120 metres a second");
    CheckError("a path a.route speed 0.1", "from 0.5 to 120 metres a second");
    CheckError("a path a.route speed x50", "a speed factor from 0.05 to 10");
    CheckError("a path a.route speed 5 speed 6", "a second 'speed'");
    CheckError("a path a.route mode", "mode needs brain or kinematic");
    CheckError("a path a.route mode flying", "brain (the game's own AI drives) or kinematic");
    CheckError("a path a.route mode brain mode brain", "a second 'mode'");
    CheckError("a path a.route colour red", "path: unknown option 'colour'");
    CheckError("player path a.route", "'player' is the player's own car");
    CheckError("player stop", "'player' is the player's own car");
    CheckError("a stop now", "takes nothing");

    Check(car_verbs_detail::IsRoutePath("a.route") && car_verbs_detail::IsRoutePath("routes/x y.route") && car_verbs_detail::IsRoutePath("m4c-a_1.route"), "route names that are fine");
    Check(!car_verbs_detail::IsRoutePath("") && !car_verbs_detail::IsRoutePath("..") && !car_verbs_detail::IsRoutePath("a//..//b") &&
              !car_verbs_detail::IsRoutePath(std::string(121, 'a')),
          "route names that are not");
    Check(car_verbs_detail::IsRoutePath(std::string(120, 'a')), "120 characters are fine");
    CarCommand c = P("a path r.route speed x2 mode brain").cmd;
    Check(DescribeCarCommand(c) == "a path r.route at x2 of the route's speeds (brain)", "describe a path: " + DescribeCarCommand(c));
    Check(DescribeCarCommand(P("a stop").cmd) == "a stop", "describe stop");
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
    uint32_t next_brain = 0xBF5A2D40;
    bool no_brain = false;       // CreateBrain gives 0
    int reached_after = 1 << 30;  // HasReachedGoal says yes from this many reads
    int reached_reads = 0;
    std::vector<uint32_t> brains_deleted;

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
            case Native::kCreateBrain:
                if (no_brain) {
                    o.ret = 0;
                } else {
                    o.ret = next_brain;
                    next_brain += 0x6B0;
                }
                break;
            case Native::kBrainReached: o.ret = ++reached_reads >= reached_after ? 1u : 0u; break;
            case Native::kDeleteBrain: brains_deleted.push_back(c.racer); o.ret_written = false; break;
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

// ---------------------------------------------------------------------------------------------------------------- paths

Route RouteOf(const std::string& text) {
    Route r;
    std::string e;
    if (!ParseRoute(text, &r, &e)) std::printf("  (route did not parse: %s)\n", e.c_str());
    return r;
}

CarCommand PathCmd(const std::string& id, const std::string& route, PathMode mode = PathMode::kKinematic, SpeedSpec speed = {}) {
    CarCommand c;
    c.id = id;
    c.verb = CarVerb::kPath;
    c.route = route;
    c.mode = mode;
    c.speed = speed;
    return c;
}
CarCommand StopCmd(const std::string& id) {
    CarCommand c;
    c.id = id;
    c.verb = CarVerb::kStop;
    return c;
}

// The pose the glue would read from the car's entity: a car at (x, y, z) with the scene heading h.
void FeedPose(Director& dir, const std::string& id, double x, double y, double z, double heading, uint64_t frame) {
    const double pos[3] = {x, y, z};
    float m[16];
    MatrixForNativeHeading(NativeHeadingDeg(heading), pos, m);
    dir.registry().UpdatePose(id, m, frame);
}

// A ready car "p1" (spawned the usual way), the registry given a pose for it. Returns the fake time at which it was ready.
int64_t ReadyCar(Director& dir, FakeGame& game, const std::string& id, double x, double y, double z, double heading) {
    dir.BeginScene();
    dir.Apply(Spawn(id, "vp_lam_murcielago_06", x, z, heading));
    const int64_t t = Drive(dir, game, 0, 9000);
    FeedPose(dir, id, x, y, z, heading, 1);
    return t;
}

struct KinematicRun {
    std::vector<Director::TeleportDue> puts;
    std::vector<double> clocks;
};

// Steps the path clock from `clock0` in `dt` steps for `steps` updates the way the game side does: read the pose, ask for the teleports, make them (the pretend
// game puts the car where it was told and lets a step of physics move it along its heading at its speed).
KinematicRun StepKinematic(Director& dir, const std::string& id, double clock0, double dt, int steps, double y_after_physics, bool refuse = false) {
    KinematicRun run;
    for (int i = 0; i < steps; ++i) {
        const double clock = clock0 + i * dt;
        std::vector<Director::TeleportDue> due;
        dir.PollTeleports(clock, &due);
        for (const Director::TeleportDue& d : due) {
            run.puts.push_back(d);
            run.clocks.push_back(clock);
            dir.ReportTeleport(d.car, !refuse);
            // physics: a step of the car's own velocity along its nose
            const double h = NormalizeDegrees(d.teleport.heading_native - 180.0) * 3.14159265358979323846 / 180.0;
            FeedPose(dir, d.car, d.teleport.position[0] + std::sin(h) * d.teleport.speed * dt, y_after_physics, d.teleport.position[2] + std::cos(h) * d.teleport.speed * dt,
                     NormalizeDegrees(d.teleport.heading_native - 180.0), static_cast<uint64_t>(i + 2));
        }
        dir.Poll(static_cast<int64_t>(clock * 1000.0) + 20000, 0xBF33C450u);  // the director's own poll goes on beside (no call is due)
        (void)id;
    }
    return run;
}

void TestPathKinematic() {
    const Route straight = RouteOf("100,200,10\n100,300,10\n");  // 100 m along +Z at 10 m/s: 10 s
    {
        Director dir;
        FakeGame game;
        ReadyCar(dir, game, "a", 100.0, 17.5, 200.0, 0.0);
        const Director::Applied ap = dir.Apply(PathCmd("a", "street.route"), 3.0, &straight);
        Check(ap.ok, "a path on a ready car: " + ap.message);
        Check(dir.PathOf("a") && dir.PathOf("a")->Active(), "the path is on");
        Check(HasEvent(dir, "path a starts (kinematic): route street.route, 2 points, 100.0 m, 10.0 s at the route's own speeds"), "the start line");

        const size_t calls_before = game.calls.size();
        KinematicRun run = StepKinematic(dir, "a", 3.0, 1.0 / 30.0, 400, 17.55);
        Check(game.calls.size() == calls_before, "the kinematic teleports are not natives of the director's poll (no call went through it)");
        Check(!run.puts.empty() && run.puts.front().teleport.first, "the first put is the first");
        CheckNum(run.puts.front().teleport.position[0], 100.0, "the car is put on the route's first point: x");
        CheckNum(run.puts.front().teleport.position[2], 200.0, "... z");
        CheckNum(run.puts.front().teleport.position[1], 17.5, "no heights in the route: the car's own height (from its pose)");
        CheckNum(run.puts.front().teleport.heading_native, 180.0, "heading: route along +Z is scene 0, native 180");
        CheckNum(run.puts.front().teleport.speed, 10.0, "the speed of the route's point");
        CheckNum(run.puts.front().teleport.along, 0.0, "along 0");
        // the car is on the route at the path clock
        bool on_clock = true;
        bool no_backwards = true;
        double prev_along = -1.0;
        for (size_t i = 0; i < run.puts.size(); ++i) {
            const double tau = run.clocks[i] - 3.0;
            const double want_z = 200.0 + std::min(100.0, 10.0 * tau);
            if (std::fabs(run.puts[i].teleport.position[2] - want_z) > 1e-6) on_clock = false;
            if (run.puts[i].teleport.along + 1e-9 < prev_along) no_backwards = false;
            prev_along = run.puts[i].teleport.along;
        }
        Check(on_clock, "every put is where the route is at the path clock (10 m/s from clock 3)");
        Check(no_backwards, "never backwards");
        Check(run.puts.back().teleport.last && run.puts.back().teleport.speed == 0.0, "the last put has speed 0");
        CheckNum(run.puts.back().teleport.position[2], 300.0, "the car ends on the last point");
        const size_t n_last = static_cast<size_t>(std::count_if(run.puts.begin(), run.puts.end(), [](const Director::TeleportDue& d) { return d.teleport.last; }));
        Check(n_last == 1, "one last put");
        Check(!dir.PathOf("a")->Active(), "the path is over after its last put");
        Check(run.puts.size() == 301 || run.puts.size() == 302, "a put at every update until the time is up (300 updates and the end): " + std::to_string(run.puts.size()));
        Check(dir.PathOf("a")->stats().teleports == run.puts.size() && dir.PathOf("a")->stats().refused == 0, "the counts");
        Check(dir.PathOf("a")->stats().max_off < 0.05, "the car (moved on by physics) stays on the route's place to within 5 cm: " + std::to_string(dir.PathOf("a")->stats().max_off));
        Check(HasEvent(dir, "path a finished: the car stands at the last point"), "the summary line");
        // no more puts once it is over
        KinematicRun more = StepKinematic(dir, "a", 20.0, 1.0 / 30.0, 30, 17.5);
        Check(more.puts.empty(), "nothing is put after the path is over");
    }
    {  // the progress lines: one every half second of path time, from the pose and the route
        Director dir;
        FakeGame game;
        ReadyCar(dir, game, "a", 100.0, 17.5, 200.0, 0.0);
        dir.Apply(PathCmd("a", "street.route"), 0.0, &straight);
        StepKinematic(dir, "a", 0.0, 1.0 / 30.0, 120, 17.5);
        int lines = 0;
        bool format_ok = true;
        for (const CarEvent& e : dir.TakeEvents()) {
            if (e.text.rfind("path a t=", 0) != 0) continue;
            ++lines;
            if (e.text.find("route (") == std::string::npos || e.text.find(", car (") == std::string::npos || e.text.find(" off ") == std::string::npos ||
                e.text.find("ahead") == std::string::npos) {
                format_ok = false;
            }
        }
        Check(lines >= 7 && lines <= 9, "about one line every 0.5 s for 4 s: " + std::to_string(lines));
        Check(format_ok, "a progress line says the route's place, the car's and how far off");
    }
    {  // a car that is not ready when the key comes due is put where the route is by then
        Director dir;
        FakeGame game;
        dir.BeginScene();
        dir.Apply(Spawn("a", "vp_lam_murcielago_06", 100.0, 200.0, 0.0));
        Check(dir.Apply(PathCmd("a", "street.route"), 1.0, &straight).ok, "a path key on a car that is still being made");
        std::vector<Director::TeleportDue> due;
        dir.PollTeleports(2.0, &due);
        Check(due.empty(), "no put while the car is being made");
        Drive(dir, game, 0, 9000);
        FeedPose(dir, "a", 100.0, 17.5, 200.0, 0.0, 1);
        dir.PollTeleports(6.0, &due);
        Check(due.size() == 1 && due[0].teleport.first, "ready: the first put");
        CheckNum(due[0].teleport.along, 50.0, "five seconds into the route: 50 m (the path clock started at the key)", 1e-6);
        CheckNum(due[0].teleport.position[2], 250.0, "z 250");
    }
    {  // the route's own heights are used, and a speed of the scene's own
        Director dir;
        FakeGame game;
        ReadyCar(dir, game, "a", 0.0, 10.0, 0.0, 90.0);
        const Route hills = RouteOf("0,0,10,10\n50,0,10,12\n100,0,10,15\n");
        dir.Apply(PathCmd("a", "hill.route", PathMode::kKinematic, {SpeedKind::kMetersPerSecond, 20.0}), 0.0, &hills);
        std::vector<Director::TeleportDue> due;
        dir.PollTeleports(1.0, &due);
        Check(due.size() == 1, "one put");
        CheckNum(due[0].teleport.along, 20.0, "20 m/s: 20 m after a second");
        CheckNum(due[0].teleport.position[1], 10.8, "the route's height at 20 m (10 + 2 * 0.4)", 1e-9);
        CheckNum(due[0].teleport.heading_native, 270.0, "along +X is scene 90, native 270");
        CheckNum(due[0].teleport.speed, 20.0, "speed 20");
    }
    {  // the clock stands still: no second put at the same clock
        Director dir;
        FakeGame game;
        ReadyCar(dir, game, "a", 100.0, 17.5, 200.0, 0.0);
        dir.Apply(PathCmd("a", "street.route"), 0.0, &straight);
        std::vector<Director::TeleportDue> due;
        dir.PollTeleports(0.0, &due);
        Check(due.size() == 1, "the first put at clock 0");
        dir.ReportTeleport("a", true);
        due.clear();
        dir.PollTeleports(0.0, &due);
        Check(due.empty(), "a clock that has not moved gives no put");
        dir.PollTeleports(0.016, &due);
        Check(due.size() == 1, "...and the next one does");
    }
    {  // stop: the car is put where it was, with speed 0, and the path is over
        Director dir;
        FakeGame game;
        ReadyCar(dir, game, "a", 100.0, 17.5, 200.0, 0.0);
        dir.Apply(PathCmd("a", "street.route"), 0.0, &straight);
        StepKinematic(dir, "a", 0.0, 1.0 / 30.0, 90, 17.5);  // 3 s: 30 m
        const Director::Applied st = dir.Apply(StopCmd("a"), 3.0);
        Check(st.ok, "stop a running path");
        KinematicRun run = StepKinematic(dir, "a", 3.0, 1.0 / 30.0, 30, 17.5);
        Check(run.puts.size() == 1 && run.puts[0].teleport.last && run.puts[0].teleport.speed == 0.0, "one last put with speed 0");
        CheckNum(run.puts[0].teleport.position[2], 230.0, "...at the place of the stop (30 m along)", 1e-6);
        Check(!dir.PathOf("a")->Active(), "the path is over");
        Check(HasEvent(dir, "path a stopped"), "the summary says stopped");
        const Director::Applied again = dir.Apply(StopCmd("a"), 4.0);
        Check(!again.ok && again.message.find("has no path running") != std::string::npos, "a second stop says there is nothing to stop");
        // a path again after the stop (kinematic can start again)
        Check(dir.Apply(PathCmd("a", "street.route"), 5.0, &straight).ok, "a new path after a stop");
        std::vector<Director::TeleportDue> due;
        dir.PollTeleports(5.5, &due);
        Check(due.size() == 1 && std::fabs(due[0].teleport.along - 5.0) < 1e-9, "the new path starts from the route's start at its own clock");
    }
    {  // errors of the verbs
        Director dir;
        FakeGame game;
        ReadyCar(dir, game, "a", 100.0, 17.5, 200.0, 0.0);
        Director::Applied r = dir.Apply(PathCmd("nobody", "x.route"), 0.0, &straight);
        Check(!r.ok && r.message.find("does not exist") != std::string::npos, "a path for a car that does not exist");
        r = dir.Apply(PathCmd("a", "x.route"), 0.0, nullptr);
        Check(!r.ok && r.message.find("was not read") != std::string::npos, "a path whose route was not read");
        r = dir.Apply(StopCmd("nobody"), 0.0);
        Check(!r.ok && r.message.find("does not exist") != std::string::npos, "stop on a car that does not exist");
        r = dir.Apply(StopCmd("a"), 0.0);
        Check(!r.ok && r.message.find("has no path running") != std::string::npos, "stop on a car without a path");
        const Route tiny = RouteOf("0,0\n0,0.5\n");
        r = dir.Apply(PathCmd("a", "tiny.route"), 0.0, &tiny);
        Check(!r.ok && r.message.find("at least 1 m") != std::string::npos, "a route under a metre: " + r.message);
        Check(!dir.PathOf("a")->Active(), "a refused path is no path");
    }
    {  // the game refuses a teleport: the path fails, the car stays, nothing more is asked
        Director dir;
        FakeGame game;
        ReadyCar(dir, game, "a", 100.0, 17.5, 200.0, 0.0);
        dir.Apply(PathCmd("a", "street.route"), 0.0, &straight);
        KinematicRun run = StepKinematic(dir, "a", 0.0, 1.0 / 30.0, 30, 17.5, true);
        Check(run.puts.size() == 1, "one try, then it stops");
        Check(!dir.PathOf("a")->Active() && dir.PathOf("a")->stats().refused == 1, "the path failed");
        Check(HasEvent(dir, "path a failed: Racer_Teleport could not be called", true), "a warning that says why");
    }
    {  // the scene ends: no put on a car that is going
        Director dir;
        FakeGame game;
        ReadyCar(dir, game, "a", 100.0, 17.5, 200.0, 0.0);
        dir.Apply(PathCmd("a", "street.route"), 0.0, &straight);
        StepKinematic(dir, "a", 0.0, 1.0 / 30.0, 10, 17.5);
        dir.EndScene();
        std::vector<Director::TeleportDue> due;
        dir.PollTeleports(1.0, &due);
        Check(due.empty(), "no put on an ended car");
        Drive(dir, game, 30000, 40000);
        Check(game.deleted_nodes.size() == 1 && game.N("BrainFactory_DeleteBrain") == 0, "the car is deleted, with no brain call (it never had one)");
    }
    {  // two cars at once: both put at every update (the director's one-call-an-update rule is for the spawn steps)
        Director dir;
        FakeGame game;
        dir.BeginScene();
        dir.Apply(Spawn("a", "vp_lam_murcielago_06", 100.0, 200.0, 0.0));
        dir.Apply(Spawn("b", "vp_aud_r8_08", 103.0, 300.0, 180.0));
        Drive(dir, game, 0, 15000);
        FeedPose(dir, "a", 100.0, 17.5, 200.0, 0.0, 1);
        FeedPose(dir, "b", 103.0, 17.5, 300.0, 180.0, 1);
        const Route back = RouteOf("103,300,8\n103,200,8\n");
        dir.Apply(PathCmd("a", "a.route"), 0.0, &straight);
        dir.Apply(PathCmd("b", "b.route"), 0.0, &back);
        std::vector<Director::TeleportDue> due;
        dir.PollTeleports(0.0, &due);
        Check(due.size() == 2 && due[0].car == "a" && due[1].car == "b", "both cars are due in one update");
        Check(due[0].racer != due[1].racer && due[0].racer != 0, "each on its own racer");
        CheckNum(due[1].teleport.heading_native, 0.0, "b drives along -Z: scene 180, native 0");
    }
}

void TestPathBrain() {
    const Route lane = RouteOf("100,200,8\n100,300,8\n");  // 100 m: 12 destinations at 8 m spacing
    {
        Director dir;
        FakeGame game;
        ReadyCar(dir, game, "b", 100.0, 17.5, 200.0, 0.0);
        game.reached_after = 3;
        const size_t before = game.calls.size();
        const Director::Applied ap = dir.Apply(PathCmd("b", "lane.route", PathMode::kBrain), 2.0, &lane);
        Check(ap.ok, "a brain path: " + ap.message);
        Check(HasEvent(dir, "path b starts (brain): route lane.route, 2 points, 100.0 m, 12.5 s at the route's own speeds (8.0 m/s on average); 12 destinations"),
              "the start line says how many destinations the AI gets");
        int double_issue = 0;
        int64_t t = 20000;
        std::vector<int64_t> stamps;
        for (int i = 0; i < 400; ++i, t += 16) {
            Director::Next n = dir.Poll(t, 0xBF33C450u);
            if (n.call.native != Native::kNone) {
                Director::Next again = dir.Poll(t, 0xBF33C450u);
                if (again.call.native != Native::kNone) ++double_issue;
                stamps.push_back(t);
                dir.Report(game.Run(n.call, t));
            }
        }
        Check(double_issue == 0, "one native at a time");
        std::vector<std::string> want = {"BrainFactory_CreateBrainCruising", "BrainCruising_SetSpeed", "BrainCruising_SetDestSpeed", "BrainCruising_SetLooping"};
        for (int i = 0; i < 12; ++i) want.push_back("BrainCruising_AppendDest");
        want.push_back("BrainCruising_DriveToNearestRail");
        const std::vector<std::string> got = Seq(game, before);
        Check(got.size() >= want.size() + 3 && std::vector<std::string>(got.begin(), got.begin() + static_cast<std::ptrdiff_t>(want.size())) == want,
              "the program: create, speed, destination speed, looping off, 12 destinations, the go");
        // the go is followed by polls of HasReachedGoal until the brain says yes, and then nothing
        const std::vector<std::string> tail(got.begin() + static_cast<std::ptrdiff_t>(want.size()), got.end());
        Check(tail.size() == 3 && tail[0] == "BrainCruising_HasReachedGoal" && tail[2] == "BrainCruising_HasReachedGoal", "HasReachedGoal is read until it says yes (3 reads), then nothing");
        Check(!dir.PathOf("b")->Active(), "the path is over once the goal is reached");
        Check(HasEvent(dir, "path b finished: the brain reports its goal reached"), "the summary line");
        // arguments
        uint32_t brain = 0;
        double last_z = 0.0;
        double prev_z = 0.0;
        bool increasing = true;
        for (const Call& c : game.detail) {
            switch (c.native) {
                case Native::kCreateBrain: Check(c.racer == 0xBDAE19B0u, "the brain is made for the car's racer"); break;
                case Native::kBrainSetSpeed:
                case Native::kBrainSetDestSpeed:
                    CheckNum(c.speed, 8.0, "the brain's speed is the route's mean speed");
                    brain = c.brain;
                    break;
                case Native::kBrainSetLooping: Check(c.index == 0, "looping off"); break;
                case Native::kBrainAppendDest:
                    Check(c.brain == brain && brain == 0xBF5A2D40u, "destinations go to the brain");
                    CheckNum(c.position[0], 100.0, "dest x");
                    CheckNum(c.position[1], 17.5, "dest y: the car's own height (the route has none)");
                    if (c.position[2] <= prev_z) increasing = false;
                    prev_z = c.position[2];
                    last_z = c.position[2];
                    break;
                case Native::kBrainDrive: Check(c.brain == 0xBF5A2D40u && c.racer == 0xBDAE19B0u, "the go is on the brain"); break;
                default: break;
            }
        }
        Check(increasing, "the destinations run along the route");
        CheckNum(last_z, 300.0, "the last destination is the route's end");
        // the pace: at least 100 ms between two natives of the program
        bool paced = true;
        for (size_t i = 1; i < stamps.size() && i < 17; ++i) {
            if (stamps[i] - stamps[i - 1] < 100) paced = false;
        }
        Check(paced, "100 ms between the natives of the program");
        // the car's end: the brain goes first
        const size_t b2 = game.calls.size();
        dir.EndScene();
        Drive(dir, game, t, t + 8000);
        const std::vector<std::string> end = Seq(game, b2);
        Check(end.size() == 2 && end[0] == "BrainFactory_DeleteBrain" && end[1] == "OpponentManager_DeleteOpponent", "the end: the brain is deleted, then the opponent");
        Check(game.brains_deleted.size() == 1 && game.brains_deleted[0] == 0xBDAE19B0u, "BrainFactory_DeleteBrain gets the car's racer");
        Check(!dir.Active(), "nothing left to do");
    }
    {  // a heights route gives the destinations their own heights
        Director dir;
        FakeGame game;
        ReadyCar(dir, game, "b", 0.0, 10.0, 0.0, 90.0);
        const Route hills = RouteOf("0,0,8,10\n80,0,8,18\n");
        dir.Apply(PathCmd("b", "h.route", PathMode::kBrain, {SpeedKind::kMetersPerSecond, 12.0}), 0.0, &hills);
        Drive(dir, game, 20000, 24000);
        double top = 0.0;
        for (const Call& c : game.detail) {
            if (c.native == Native::kBrainAppendDest) top = c.position[1];
            if (c.native == Native::kBrainSetSpeed) CheckNum(c.speed, 12.0, "a speed of 12 m/s is the brain's speed");
        }
        CheckNum(top, 18.0, "the last destination has the route's last height");
        Check(game.N("BrainCruising_AppendDest") == 10, "80 m at 8 m: 10 destinations");
    }
    {  // stop: the brain goes back to idle; a second path is refused
        Director dir;
        FakeGame game;
        ReadyCar(dir, game, "b", 100.0, 17.5, 200.0, 0.0);
        dir.Apply(PathCmd("b", "lane.route", PathMode::kBrain), 0.0, &lane);
        Drive(dir, game, 20000, 23000);
        Check(game.N("BrainCruising_DriveToNearestRail") == 1, "the go was made");
        Check(dir.Apply(StopCmd("b"), 1.0).ok, "stop");
        const size_t before = game.calls.size();
        Drive(dir, game, 23016, 24000);
        const std::vector<std::string> got = Seq(game, before);
        Check(!got.empty() && got.back() == "BrainCruising_RemoveFromTraffic", "the brain is taken off the lanes (idle, brake held)");
        Check(std::count(got.begin(), got.end(), "BrainCruising_RemoveFromTraffic") == 1, "once");
        Check(!dir.PathOf("b")->Active() && HasEvent(dir, "path b stopped: the brain is back to idle"), "the path is over");
        const Director::Applied again = dir.Apply(PathCmd("b", "lane.route"), 2.0, &lane);
        Check(!again.ok && again.message.find("already has a brain") != std::string::npos, "a car takes one brain path: " + again.message);
        Check(dir.Apply(PathCmd("b", "lane.route", PathMode::kBrain), 2.0, &lane).ok == false, "...in either mode");
        dir.EndScene();
        const size_t b3 = game.calls.size();
        Drive(dir, game, 24016, 32000);
        Check(Seq(game, b3) == std::vector<std::string>({"BrainFactory_DeleteBrain", "OpponentManager_DeleteOpponent"}), "the brain still goes before the opponent");
    }
    {  // stop before the brain exists: nothing is called
        Director dir;
        FakeGame game;
        ReadyCar(dir, game, "b", 100.0, 17.5, 200.0, 0.0);
        const size_t before = game.calls.size();
        dir.Apply(PathCmd("b", "lane.route", PathMode::kBrain), 0.0, &lane);
        dir.Apply(StopCmd("b"), 0.0);
        Drive(dir, game, 20000, 22000);
        Check(game.calls.size() == before, "stopped before the first native: no brain is made");
        Check(HasEvent(dir, "stopped before the brain was made"), "and the log says so");
    }
    {  // the game gives no brain
        Director dir;
        FakeGame game;
        game.no_brain = true;
        ReadyCar(dir, game, "b", 100.0, 17.5, 200.0, 0.0);
        const size_t before = game.calls.size();
        dir.Apply(PathCmd("b", "lane.route", PathMode::kBrain), 0.0, &lane);
        Drive(dir, game, 20000, 24000);
        Check(Seq(game, before) == std::vector<std::string>({"BrainFactory_CreateBrainCruising"}), "one try, nothing more");
        Check(HasEvent(dir, "path b failed: BrainFactory_CreateBrainCruising gave no brain", true), "a warning");
        dir.EndScene();
        const size_t b2 = game.calls.size();
        Drive(dir, game, 24016, 32000);
        Check(Seq(game, b2) == std::vector<std::string>({"OpponentManager_DeleteOpponent"}), "no brain to delete: the opponent only");
    }
    {  // a native that cannot be called
        Director dir;
        FakeGame game;
        ReadyCar(dir, game, "b", 100.0, 17.5, 200.0, 0.0);
        dir.Apply(PathCmd("b", "lane.route", PathMode::kBrain), 0.0, &lane);
        game.uncallable = true;
        Drive(dir, game, 20000, 22000);
        Check(!dir.PathOf("b")->Active(), "the path failed");
        Check(HasEvent(dir, "could not be called", true), "with a warning");
    }
    {  // what the brain says about itself: lines, states, how far along and how far off
        Director dir;
        FakeGame game;
        ReadyCar(dir, game, "b", 100.0, 17.5, 200.0, 0.0);
        dir.Apply(PathCmd("b", "lane.route", PathMode::kBrain), 0.0, &lane);
        Drive(dir, game, 20000, 24000);
        dir.TakeEvents();
        Check(dir.BrainReaders().size() == 1 && dir.BrainReaders()[0].id == "b" && dir.BrainReaders()[0].brain == 0xBF5A2D40u, "the running brain is read by the glue");
        FeedPose(dir, "b", 101.5, 17.5, 230.0, 0.0, 5);
        dir.NoteBrain("b", 1.0, 5, 0);
        bool saw = false;
        for (const CarEvent& e : dir.TakeEvents()) {
            if (e.text.find("brain state 5 (on the lanes) dest 0/12") != std::string::npos && e.text.find("nearest route point 30.0 of 100.0 m, 1.50 m off") != std::string::npos) saw = true;
        }
        Check(saw, "the first read is a line with the state, the destination and the car against the route");
        dir.NoteBrain("b", 1.1, 5, 0);
        Check(dir.TakeEvents().empty(), "the same state a moment later: no line (one every half second)");
        dir.NoteBrain("b", 1.2, 4, 1);
        bool changed = false;
        for (const CarEvent& e : dir.TakeEvents()) {
            if (e.text.find("brain state 4") != std::string::npos && e.text.find("state was 5") != std::string::npos && e.warning) changed = true;
        }
        Check(changed, "a change to state 4 (no lane) is a warning line");
        FeedPose(dir, "b", 100.0, 17.5, 260.0, 0.0, 6);
        dir.NoteBrain("b", 2.0, 5, 2);
        const PathDrive::Stats& st = dir.PathOf("b")->stats();
        Check((st.brain_states_seen & (1 << 5)) && (st.brain_states_seen & (1 << 4)), "the states seen are counted");
        CheckNum(st.brain_best_s, 60.0, "the farthest along");
        CheckNum(st.brain_max_off, 1.5, "the farthest off");
        Check(std::string(BrainStateName(0)).find("idle") != std::string::npos && std::string(BrainStateName(6)).find("goal") != std::string::npos, "state names");
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
    TestPathVerbs();
    TestPoses();
    TestRegistry();
    TestSpawnByModel();
    TestSpawnByGarage();
    TestSpawnFailures();
    TestIdsAndDeletes();
    TestEndMidSpawn();
    TestOneCallAnUpdate();
    TestPlayerDress();
    TestPathKinematic();
    TestPathBrain();
    TestLost();
    TestDeleteIsNotLost();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
