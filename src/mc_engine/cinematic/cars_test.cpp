// Check of the scene's car statements and bookkeeping (car_verbs.h, car_registry.h, car_spawn.h). A program of its own: it needs no
// game, no ReXGlue and no log, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_cars_test      (CMakeLists.txt)
//   <build dir>/larecomp_cars_test
//
// The compile by hand (the msvc-target clang of labuild, include path src/mc_engine/cinematic):
//   clang++ -std=c++23 -Wall -Wextra src/mc_engine/cinematic/cars_test.cpp src/mc_engine/cinematic/car_registry.cpp \
//       src/mc_engine/cinematic/car_spawn.cpp src/mc_engine/cinematic/ambient_car.cpp src/mc_engine/cinematic/path_follow.cpp -o cars_test.exe

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
#include "gpu/scene_noise.h"

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
    CheckError("p1 spawn vp_x at 1,2,3 sparkle red", "unknown option 'sparkle'");
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
    // the place a car that is not made yet will stand at (what a camera attached to it follows meanwhile): the pose reads back, and it is the matrix the game gives a car there
    for (double h : {0.0, 62.0, 242.0, 331.6}) {
        float want[16], got[16];
        MatrixForNativeHeading(NativeHeadingDeg(h), pos, want);
        MatrixAtScenePlace(pos, h, got);
        CarPose at;
        Check(PoseFromMatrix(got, &at), "the declared place's matrix is a car's, heading " + std::to_string(h));
        CheckNum(at.heading, h, "the declared place keeps the heading " + std::to_string(h), 1e-3);
        CheckNum(at.position[0], 900.0, "the declared place keeps x");
        CheckNum(at.position[1], 17.5, "the declared place keeps y");
        CheckNum(at.position[2], -850.0, "the declared place keeps z");
        bool same = true;
        for (int i = 0; i < 16; ++i) {
            if ((i & 3) == 3) continue;
            same = same && std::fabs(got[i] - want[i]) < 1e-5f;
        }
        Check(same, "the declared place's rows are the game's for the heading " + std::to_string(h));
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
    // The w words are not the game's to keep: Racer_Teleport leaves the position row's from the stack (M4c's first run: a car whose matrix "does not read")
    MatrixForNativeHeading(NativeHeadingDeg(62.0), pos, m);
    m[3] = NAN;
    m[7] = INFINITY;
    m[11] = -INFINITY;
    m[15] = NAN;
    Check(PoseFromMatrix(m, &pose), "NaN and infinite w words are no reason to refuse a car's matrix");
    CheckNum(pose.heading, 62.0, "...and the pose is right", 1e-4);
    CheckNum(pose.position[0], 900.0, "...the position too");
    Check(MatrixHasBadW(m) && MatrixFault(m).empty(), "the w words are named, the matrix has no fault");
    MatrixForNativeHeading(NativeHeadingDeg(62.0), pos, m);
    Check(!MatrixHasBadW(m), "clean w words are not named");
    m[13] = NAN;
    Check(MatrixFault(m).find("word 13 (position y)") != std::string::npos, "a NaN in the position says which word: " + MatrixFault(m));
    MatrixForNativeHeading(NativeHeadingDeg(62.0), pos, m);
    m[8] = m[9] = m[10] = 0.0f;
    Check(MatrixFault(m).find("back row") != std::string::npos, "a flat back row says so: " + MatrixFault(m));
    Check(!MatrixFault(bad).empty(), "an all-zero matrix has a fault");
    {  // the registry keeps the words clean for the camera
        CarRegistry reg;
        reg.Add("p1", false);
        MatrixForNativeHeading(NativeHeadingDeg(62.0), pos, m);
        m[3] = NAN;
        m[15] = NAN;
        Check(reg.UpdatePose("p1", m, 3), "a matrix with a NaN w word is stored");
        float out[16];
        Check(reg.Matrix("p1", out) && out[3] == 0.0f && out[7] == 0.0f && out[11] == 0.0f && out[15] == 1.0f && out[12] == 900.0f, "...with clean w words");
    }
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

// A car whose matrix stops being read (M4c's first run): the registry keeps its last pose, and the path must not take that for where the car is.
void TestPathStalePose() {
    const Route straight = RouteOf("100,200,10\n100,300,10\n");
    Director dir;
    FakeGame game;
    ReadyCar(dir, game, "a", 100.0, 17.5, 200.0, 0.0);
    dir.Apply(PathCmd("a", "street.route"), 0.0, &straight);
    std::string text;
    double last_y = 0.0;
    uint64_t frame = 2;
    for (int i = 0; i < 120; ++i, ++frame) {  // two seconds at 60 Hz; the matrix is read for the first 30 updates only
        const double clock = i / 60.0;
        if (i < 30) FeedPose(dir, "a", 100.0, 17.5 - 0.001 * i, 200.0 + 10.0 * clock, 0.0, frame);
        std::vector<Director::TeleportDue> due;
        dir.PollTeleports(clock, &due, frame);
        for (const Director::TeleportDue& d : due) {
            last_y = d.teleport.position[1];
            dir.ReportTeleport(d.car, true);
        }
        for (const CarEvent& e : dir.TakeEvents()) text += e.text + "\n";
    }
    const PathDrive::Stats st = dir.PathOf("a")->stats();  // a copy: the path goes on below
    Check(st.teleports == 120, "a put at every update all the same: " + std::to_string(st.teleports));
    Check(st.samples >= 30 && st.samples <= 34, "poses compared only while they were fresh (read 30 updates, taken for 3 more): " + std::to_string(st.samples));
    Check(st.max_off < 0.6, "the stale pose gave no distance from the route (3 updates of lag at 10 m/s is the most a fresh one has): " + std::to_string(st.max_off));
    Check(st.unread >= 80 && st.unread <= 90 && st.unread_max >= 85, "the unread updates are counted: " + std::to_string(st.unread) + " / " + std::to_string(st.unread_max));
    CheckNum(last_y, 17.5 - 0.001 * 29, "the height of the last fresh pose is kept", 1e-6);
    Check(text.find("the car's matrix has not been read for") != std::string::npos && text.find("no pose to compare") != std::string::npos, "a progress line says the matrix is not read");
    // ... and the pose is back: compared again from there on
    FeedPose(dir, "a", 100.0, 17.4, 200.0 + 10.0 * 2.0, 0.0, frame);
    std::vector<Director::TeleportDue> due;
    dir.PollTeleports(2.0 + 1.0 / 60.0, &due, frame);
    Check(dir.PathOf("a")->stats().samples == st.samples + 1, "a fresh pose is compared again");
    dir.ReportTeleport("a", true);
    dir.Apply(StopCmd("a"), 2.2);
    due.clear();
    dir.PollTeleports(2.3, &due, frame);
    for (const Director::TeleportDue& d : due) dir.ReportTeleport(d.car, true);
    Check(HasEvent(dir, "update(s) had no fresh pose to compare"), "the summary says how many updates had no pose");
    {  // the same for the brain: the state line says the pose is stale, and no distance from the route is made of it
        const Route lane = RouteOf("100,200,8\n100,300,8\n");
        Director b;
        FakeGame game2;
        ReadyCar(b, game2, "b", 100.0, 17.5, 200.0, 0.0);
        b.Apply(PathCmd("b", "lane.route", PathMode::kBrain), 0.0, &lane);
        int64_t t = 20000;
        for (int i = 0; i < 400; ++i, t += 16) {  // the program: one native at a time, then the polls of HasReachedGoal
            const Director::Next n = b.Poll(t, 0xBF33C450u);
            if (n.call.native != Native::kNone) b.Report(game2.Run(n.call, t));
        }
        b.NoteBrain("b", 6.0, 1, 0, 10);  // the pose was read at update 1 (ReadyCar): 9 updates ago at update 10
        const PathDrive::Stats& bs = b.PathOf("b")->stats();
        Check(bs.brain_samples == 1 && bs.brain_max_off == 0.0 && bs.brain_best_s == 0.0, "a stale pose is not measured against the route");
        std::string bt;
        for (const CarEvent& e : b.TakeEvents()) bt += e.text + "\n";
        Check(bt.find("brain state 1") != std::string::npos && bt.find("has not been read for 9 updates") != std::string::npos, "the state line says the matrix is unread: " + bt);
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


// ---------------------------------------------------------------------------------------------------------------- ambient cars

// `car <id> ambient <model> at <x,y,z> [heading <deg>]` (car_verbs.h), the layout's angle, the guards and the machine of one car.
void TestAmbientVerbs() {
    {
        const Parsed p = P("a1 ambient va_gmc_yukon_05 at 817.0,17.2,-797.3 heading 45");
        Check(p.status == CarParse::kOk, "ambient parses");
        Check(p.cmd.verb == CarVerb::kAmbient && p.cmd.id == "a1" && p.cmd.source == CarSource::kModel, "ambient: verb, id, source");
        Check(p.cmd.model == "va_gmc_yukon_05", "ambient: the model");
        CheckNum(p.cmd.position[0], 817.0, "ambient x");
        CheckNum(p.cmd.position[1], 17.2, "ambient y");
        CheckNum(p.cmd.position[2], -797.3, "ambient z");
        CheckNum(p.cmd.heading, 45.0, "ambient heading");
        Check(p.cmd.has_heading, "ambient has_heading");
    }
    {
        const Parsed p = P("a2 AMBIENT VA_MB_ECLIPSE_04 AT 1 2 3");  // upper case, three numbers; no heading = 0, not given
        Check(p.status == CarParse::kOk && p.cmd.model == "va_mb_eclipse_04" && !p.cmd.has_heading && p.cmd.heading == 0.0, "ambient: upper case lowered, no heading");
        CheckNum(p.cmd.position[2], 3.0, "ambient: three-number position");
    }
    CheckNum(P("a3 ambient x at 0,0,0 heading -90").cmd.heading, 270.0, "ambient heading -90 is 270");
    CheckNum(P("a3 ambient x at 0,0,0 heading 360").cmd.heading, 0.0, "ambient heading 360 is 0");
    Check(P("a3 ambient va_cad_escalade_03 at 0,0,0").status == CarParse::kOk, "a name of 24 characters is fine");
    // the same words say the same with `spawn`: the old error texts are unchanged
    CheckError("p1 spawn vp_x at 1,2,3 at 4,5,6", "spawn: a second 'at'");
    CheckError("p1 ambient vp_x at 1,2,3 at 4,5,6", "ambient: a second 'at'");
    CheckError("p1 ambient vp_x at 1,2,3 heading 1 heading 2", "ambient: a second 'heading'");
    CheckError("p1 ambient vp_x at 1,2,3 sparkle red", "ambient: unknown option 'sparkle'");
    CheckError("p1 ambient", "ambient what?");
    CheckError("p1 ambient vp_x", "ambient needs a place");
    CheckError("p1 ambient vp_x at", "at needs x,y,z");
    CheckError("p1 ambient vp_x at 1,2", "bad position");
    CheckError("p1 ambient vp_x at 999999,0,0", "outside the world");
    CheckError("p1 ambient profile 3 at 1,2,3", "not an ambient type");
    CheckError("p1 ambient garage:3 at 1,2,3", "not an ambient type");
    CheckError("p1 ambient car:3 at 1,2,3", "not an ambient type");
    CheckError("p1 ambient Vp.Bad/Name at 1,2,3", "not an ambient type name");
    CheckError("p1 ambient abcdefghijklmnopqrstuvwxyz_01234 at 1,2,3", "up to 31");  // 32 characters: the layout item's name buffer holds 31 and a NUL
    Check(P("p1 ambient abcdefghijklmnopqrstuvwxyz_0123 at 1,2,3").status == CarParse::kOk, "31 characters fit the layout item's name buffer");
    CheckError("player ambient vp_x at 1,2,3", "'player' is the player's own car");
    Check(P("p1 spawn abcdefghijklmnopqrstuvwxyz_012345 at 1,2,3").status == CarParse::kOk, "a spawn's name may be longer (47): only the layout's buffer is 32 bytes");
    // a path or a stop on an ambient car is the Director's refusal (the parser sees only the words)
    Check(P("a1 path street.route").status == CarParse::kOk, "path parses for any id");
    Check(P("a1 delete").cmd.verb == CarVerb::kDelete, "delete works for the id of an ambient car too (the Director finds which kind it is)");
    Check(DescribeCarCommand(P("a1 ambient va_gmc_yukon_05 at 1,2,3 heading 90").cmd) == "a1 ambient va_gmc_yukon_05 at (1.00, 2.00, 3.00) heading 90.00", "the log line");
    Check(DescribeCarCommand(P("p1 spawn vp_x at 1,2,3 heading 90").cmd) == "p1 spawn vp_x at (1.00, 2.00, 3.00) heading 90.00", "a spawn's log line is as it was");
}

// The layout's angle. The traffic probe (TRAFFICPROBE.txt) passed 272 (the player's Racer_Teleport angle: his nose was at the scene heading 92) to
// AmbientMgr_AddAmbientToLayout, and the car stood at the scene heading 92: the layout's angle is the scene's plus 180, like Racer_Teleport's.
void TestLayoutHeading() {
    CheckNum(LayoutHeadingDeg(92.0), 272.0, "the probe's run: the scene heading 92 is the layout angle 272");
    CheckNum(SceneHeadingFromLayoutDeg(272.0), 92.0, "...and back");
    CheckNum(LayoutHeadingDeg(0.0), 180.0, "heading 0 (along +Z) is the layout angle 180");
    CheckNum(LayoutHeadingDeg(45.0), 225.0, "heading 45 is 225");
    CheckNum(LayoutHeadingDeg(90.0), 270.0, "heading 90 is 270");
    CheckNum(LayoutHeadingDeg(135.0), 315.0, "heading 135 is 315");
    CheckNum(LayoutHeadingDeg(180.0), 0.0, "heading 180 wraps to 0");
    CheckNum(LayoutHeadingDeg(270.0), 90.0, "heading 270 is 90");
    CheckNum(LayoutHeadingDeg(-45.0), 135.0, "a negative heading wraps");
    for (double h : {0.0, 17.0, 45.0, 92.0, 135.0, 271.5, 359.0}) {
        CheckNum(SceneHeadingFromLayoutDeg(LayoutHeadingDeg(h)), h, "the two conversions undo each other at " + std::to_string(h), 1e-9);
    }
    CheckNum(LayoutHeadingDeg(1.0), NativeHeadingDeg(1.0), "the layout's angle is Racer_Teleport's");
    // the stored item: radians, as the log of the probe showed (272 degrees = 4.747 rad; the log read 4.742 for 271.7)
    CheckNum(LayoutHeadingDeg(92.0) * 3.14159265358979323846 / 180.0, 4.7473, "272 degrees are 4.7473 radians in the item", 1e-3);
}

AmbientGate OpenGate() {
    AmbientGate g;
    g.manager = true;
    g.manager_addr = 0xA92D0000u;
    g.player = true;
    g.free_roam = true;
    g.not_paused = true;
    g.world_updates = 500;
    g.layout_pool = true;
    g.layout_free = 377;  // the probe's first sample: free 377 live 135 of 512
    g.placed_so_far = 0;
    return g;
}

void TestAmbientGuards() {
    Check(DecideAmbientAdd(OpenGate()) == AmbientRefusal::kNone, "an open gate lets the call through");
    {
        AmbientGate g = OpenGate();
        g.placed_so_far = kAmbientMaxPerScene;
        g.manager = false;
        Check(DecideAmbientAdd(g) == AmbientRefusal::kCapReached, "the cap is checked first (the probe's order)");
    }
    struct Case {
        const char* what;
        void (*make)(AmbientGate&);
        AmbientRefusal want;
        bool passes;
    };
    const Case cases[] = {
        {"no manager", [](AmbientGate& g) { g.manager = false; }, AmbientRefusal::kNoManager, true},
        {"no player", [](AmbientGate& g) { g.player = false; }, AmbientRefusal::kNoPlayer, true},
        {"not free roam", [](AmbientGate& g) { g.free_roam = false; }, AmbientRefusal::kNotFreeRoam, true},
        {"paused", [](AmbientGate& g) { g.not_paused = false; }, AmbientRefusal::kNotDriving, true},
        {"too soon", [](AmbientGate& g) { g.world_updates = kAmbientMinWorldUpdates - 1; }, AmbientRefusal::kTooSoon, true},
        {"no layout pool", [](AmbientGate& g) { g.layout_pool = false; }, AmbientRefusal::kNoLayoutPool, true},
        {"pool full", [](AmbientGate& g) { g.layout_free = kAmbientMinFreeLayoutSlots - 1; }, AmbientRefusal::kPoolFull, false},
    };
    for (const Case& c : cases) {
        AmbientGate g = OpenGate();
        c.make(g);
        const AmbientRefusal r = DecideAmbientAdd(g);
        Check(r == c.want, std::string("the gate refuses: ") + c.what);
        Check(AmbientRefusalPasses(r) == c.passes, std::string("a refusal that ") + (c.passes ? "passes: " : "is final: ") + c.what);
        Check(std::string(AmbientRefusalText(r)).size() > 8, std::string("it has a sentence: ") + c.what);
    }
    {
        AmbientGate g = OpenGate();
        g.world_updates = kAmbientMinWorldUpdates;
        g.layout_free = kAmbientMinFreeLayoutSlots;
        Check(DecideAmbientAdd(g) == AmbientRefusal::kNone, "120 updates and two free slots are enough (the probe's numbers)");
    }
    Check(DecideAmbientAdd(AmbientGate{}) == AmbientRefusal::kNoManager, "a gate nobody set refuses: nothing is placed until the game side says the world is up");
    Check(AmbientRefusalPasses(AmbientRefusal::kCapReached) == false && AmbientRefusalPasses(AmbientRefusal::kNone) == false, "the cap and `none` do not pass");
}

CarCommand Ambient(const std::string& id, const std::string& model, double x = 817.0, double z = -797.3, double heading = 0.0) {
    CarCommand c;
    c.id = id;
    c.verb = CarVerb::kAmbient;
    c.source = CarSource::kModel;
    c.model = model;
    c.position[0] = x;
    c.position[1] = 17.2;
    c.position[2] = z;
    c.heading = heading;
    c.has_heading = true;
    return c;
}

// Plays the machine of one car against a pretend game: Add returns the item's id, Remove returns nothing.
struct AmbientGame {
    int32_t next_id = 135;
    int32_t add_returns = 0;      // nonzero: the id AddAmbientToLayout answers instead (-1 = the game's refusal)
    bool uncallable = false;
    std::vector<Call> adds;
    std::vector<int32_t> removed;
    Outcome Run(const Call& c) {
        Outcome o;
        o.called = !uncallable;
        if (c.native == Native::kAddAmbient) {
            adds.push_back(c);
            o.ret_written = true;
            o.ret = static_cast<uint32_t>(add_returns != 0 ? add_returns : next_id++);
        } else if (c.native == Native::kRemoveAmbient) {
            removed.push_back(c.index);
        }
        return o;
    }
};

void TestAmbientMachine() {
    {
        AmbientCar car;
        AmbientGame game;
        car.Start(Ambient("a1", "va_gmc_yukon_05", 817.0, -797.3, 45.0));
        Check(car.stage() == AmbientCar::Stage::kAdd && !car.Finished() && !car.Placed(), "a started car waits to be added");
        const Call c = car.Poll(1000, OpenGate());
        Check(c.native == Native::kAddAmbient && c.text == "va_gmc_yukon_05", "the first poll with an open gate hands out the add of the model");
        CheckNum(c.position[0], 817.0, "the add's x");
        CheckNum(c.position[2], -797.3, "the add's z");
        CheckNum(c.heading_native, 225.0, "the add's angle is the layout's: the scene's 45 plus 180");
        Check(car.Poll(1016, OpenGate()).native == Native::kNone, "one call in flight: nothing more is handed out");
        car.Report(game.Run(c));
        Check(car.Placed() && car.layout_id() == 135 && car.EverPlaced() && car.ok(), "the report with an id places it: layout item 135");
        Check(car.Poll(1032, OpenGate()).native == Native::kNone, "a placed car asks for nothing");
        car.RequestDelete();
        Check(car.stage() == AmbientCar::Stage::kRemove, "a delete after the placement goes to the remove");
        const Call r = car.Poll(1048, OpenGate());
        Check(r.native == Native::kRemoveAmbient && r.index == 135, "the remove names the layout item's id");
        car.Report(game.Run(r));
        Check(car.Finished() && car.ok() && game.removed == std::vector<int32_t>{135}, "removed, and finished without a fault");
        bool placed_line = false;
        for (const CarEvent& e : car.TakeEvents()) placed_line = placed_line || (!e.warning && e.text.find("layout item 135") != std::string::npos);
        Check(placed_line, "the log line says which layout item");
    }
    {  // a delete before anything was called drops the car
        AmbientCar car;
        car.Start(Ambient("a1", "va_gmc_yukon_05"));
        car.RequestDelete();
        Check(car.Finished() && car.ok() && !car.EverPlaced(), "a delete before the add: dropped, nothing called");
    }
    {  // a delete while the add is in flight: the remove follows the report
        AmbientCar car;
        AmbientGame game;
        car.Start(Ambient("a1", "va_gmc_yukon_05"));
        const Call c = car.Poll(0, OpenGate());
        car.RequestDelete();
        Check(!car.Finished(), "the add is in flight: the car is not finished");
        car.Report(game.Run(c));
        Check(car.stage() == AmbientCar::Stage::kRemove && car.EverPlaced(), "after the report the remove is next (a placement that exists is never left behind)");
        const Call r = car.Poll(16, OpenGate());
        Check(r.native == Native::kRemoveAmbient && r.index == 135, "...and it names the item");
        car.Report(game.Run(r));
        Check(car.Finished() && car.ok(), "gone");
    }
    {  // waiting: the world is not up yet, then it is
        AmbientCar car;
        AmbientGate g = OpenGate();
        g.free_roam = false;
        car.Start(Ambient("a1", "va_gmc_yukon_05"));
        Check(car.Poll(0, g).native == Native::kNone && !car.Finished(), "not free roam: waits");
        g.free_roam = false;
        Check(car.Poll(16, g).native == Native::kNone, "still waits");
        int lines = 0;
        for (const CarEvent& e : car.TakeEvents()) lines += e.text.find("waits") != std::string::npos ? 1 : 0;
        Check(lines == 1, "the wait is logged once, not every update");
        g.free_roam = true;
        Check(car.Poll(32, g).native == Native::kAddAmbient, "free roam: the add goes");
    }
    {  // a wait that never ends fails
        AmbientCar car;
        AmbientGate g = OpenGate();
        g.free_roam = false;
        car.Start(Ambient("a1", "va_gmc_yukon_05"));
        car.Poll(0, g);
        car.Poll(kAmbientGiveUpMs + 1, g);
        Check(car.Finished() && !car.ok() && car.failure().find("gave up waiting") != std::string::npos, "after 90 s of waiting the car fails and says why");
    }
    {  // a final refusal fails at once
        AmbientCar car;
        AmbientGate g = OpenGate();
        g.layout_free = 1;
        car.Start(Ambient("a1", "va_gmc_yukon_05"));
        car.Poll(0, g);
        Check(car.Finished() && !car.ok() && car.failure().find("no free slot") != std::string::npos, "a full layout pool fails at once");
    }
    {  // the game's refusal (-1) and a call that could not be made
        AmbientCar car;
        AmbientGame game;
        game.add_returns = -1;
        car.Start(Ambient("a1", "va_gmc_yukon_05"));
        car.Report(Outcome{});  // nothing in flight: ignored
        const Call c = car.Poll(0, OpenGate());
        car.Report(game.Run(c));
        Check(car.Finished() && !car.ok() && car.failure().find("returned -1") != std::string::npos && !car.EverPlaced(), "an id of -1 is the game's refusal");
        AmbientCar car2;
        AmbientGame game2;
        game2.uncallable = true;
        car2.Start(Ambient("a2", "va_gmc_yukon_05"));
        car2.Report(game2.Run(car2.Poll(0, OpenGate())));
        Check(car2.Finished() && !car2.ok() && car2.failure().find("was not called") != std::string::npos, "a call that could not be made fails the car");
    }
    {  // the ambient manager changed under it: nothing is called on the old item
        AmbientCar car;
        AmbientGame game;
        car.Start(Ambient("a1", "va_gmc_yukon_05"));
        car.Report(game.Run(car.Poll(0, OpenGate())));
        car.RequestDelete();
        AmbientGate other = OpenGate();
        other.manager_addr = 0xA9990000u;
        Check(car.Poll(16, other).native == Native::kNone && car.Finished() && car.ok(), "a rebuilt manager: the placement went with it, nothing is called, no fault");
        Check(game.removed.empty(), "no remove was made on the stale id");
    }
    {  // the world rebuilt while the car stands
        AmbientCar car;
        AmbientGame game;
        car.Start(Ambient("a1", "va_gmc_yukon_05"));
        car.Report(game.Run(car.Poll(0, OpenGate())));
        car.MarkLost();
        Check(car.Finished() && !car.ok(), "a placement the game lost is a fault");
    }
}

// A Director with ambient cars: one call an update, the cap, the ids, the end of the scene, the pose.
void TestAmbientDirector() {
    {
        Director dir;
        AmbientGame game;
        dir.BeginScene();
        dir.SetAmbientGate(OpenGate());
        Check(dir.Apply(Ambient("a1", "va_gmc_yukon_05", 810.0, -800.0, 0.0)).ok, "ambient a1 applies");
        Check(dir.Apply(Ambient("a2", "va_mb_eclipse_04", 817.0, -800.0, 45.0)).ok, "ambient a2 applies");
        Check(dir.Apply(Ambient("a3", "va_dge_caravan_07", 824.0, -800.0, 90.0)).ok, "ambient a3 applies");
        Check(dir.Apply(Ambient("a4", "va_golfr32_04", 831.0, -800.0, 135.0)).ok, "ambient a4 applies");
        Check(dir.registry().Find("a1") != nullptr && dir.registry().Find("a4") != nullptr, "the cars are in the registry (the camera and the state log find them by id)");
        Check(dir.Active() && dir.SceneCarCount() == 4, "active, four scene cars");
        int max_calls = 0;
        int64_t t = 0;
        for (; t < 2000; t += 16) {
            Director::Next n = dir.Poll(t, 0xBF33C450u);
            if (n.call.native != Native::kNone) {
                Director::Next again = dir.Poll(t, 0xBF33C450u);
                if (again.call.native != Native::kNone) ++max_calls;
                dir.Report(game.Run(n.call));
            }
        }
        Check(max_calls == 0, "one native an update, never two");
        Check(game.adds.size() == 4, "four adds");
        Check(game.adds.size() == 4 && game.adds[0].text == "va_gmc_yukon_05" && game.adds[3].text == "va_golfr32_04", "in the order of the file");
        double angles[4] = {};
        for (size_t i = 0; i < game.adds.size() && i < 4; ++i) angles[i] = game.adds[i].heading_native;
        CheckNum(angles[0], 180.0, "heading 0 goes in as the layout angle 180");
        CheckNum(angles[1], 225.0, "45 as 225");
        CheckNum(angles[2], 270.0, "90 as 270");
        CheckNum(angles[3], 315.0, "135 as 315");
        Check(dir.spawned() == 4 && dir.AmbientReaders().size() == 4, "four placed, four to read poses of");
        Check(dir.AmbientReaders()[0].layout_id == 135 && dir.AmbientReaders()[3].layout_id == 138, "their layout ids are the game's answers");
        // a pose: the first one is logged with what the scene asked for
        float m[16] = {0};
        m[0] = 1; m[5] = 1; m[10] = 1;      // right, up, back = identity: nose along -z... heading 180 in the scene's convention
        m[12] = 810.2f; m[13] = 17.6f; m[14] = -800.3f;
        dir.TakeEvents();
        dir.NoteAmbientPose("a1", m, 10);
        bool line = false;
        for (const CarEvent& e : dir.TakeEvents()) line = line || (e.text.find("car a1 (ambient va_gmc_yukon_05) stands at (810.20, 17.60, -800.30)") != std::string::npos && e.text.find("layout angle 180.0") != std::string::npos);
        Check(line, "the first pose is logged beside what was asked for (and the layout angle)");
        dir.NoteAmbientPose("a1", m, 11);
        Check(dir.TakeEvents().empty(), "...and only the first");
        // the end of the scene removes every one of them
        dir.EndScene();
        Check(dir.Active(), "the scene is over, the cars are still to go");
        Check(dir.registry().Find("a1") == nullptr, "out of the registry at once");
        int removes = 0;
        for (; t < 6000 && dir.Active(); t += 16) {
            Director::Next n = dir.Poll(t, 0xBF33C450u);
            if (n.call.native != Native::kNone) {
                if (n.call.native == Native::kRemoveAmbient) ++removes;
                dir.Report(game.Run(n.call));
            }
        }
        Check(removes == 4 && game.removed == (std::vector<int32_t>{135, 136, 137, 138}), "every layout item was removed by its id, once");
        Check(!dir.Active() && dir.deleted() == 4 && dir.failed() == 0, "nothing left, four removed, none failed");
    }
    {  // ids are one namespace across the kinds; a path on an ambient car is refused; delete by id
        Director dir;
        dir.BeginScene();
        dir.SetAmbientGate(OpenGate());
        Check(dir.Apply(Ambient("a1", "va_gmc_yukon_05")).ok, "a1 ambient");
        Check(!dir.Apply(Ambient("a1", "va_mb_eclipse_04")).ok, "a second ambient with the same id is refused");
        Check(!dir.Apply(Spawn("a1", "vp_lam_murcielago_06")).ok, "a spawn with an ambient's id is refused");
        dir.Apply(Spawn("p1", "vp_lam_murcielago_06"));
        Check(!dir.Apply(Ambient("p1", "va_mb_eclipse_04")).ok, "an ambient with a spawn's id is refused");
        CarCommand path;
        path.id = "a1";
        path.verb = CarVerb::kPath;
        path.route = "street.route";
        const Director::Applied pa = dir.Apply(path);
        Check(!pa.ok && pa.message.find("ambient car") != std::string::npos, "a path on an ambient car is refused with a reason");
        CarCommand stop;
        stop.id = "a1";
        stop.verb = CarVerb::kStop;
        Check(!dir.Apply(stop).ok, "a stop on an ambient car is refused");
        const Director::Applied del = dir.Apply(Delete("a1"));
        Check(del.ok && del.message.find("not placed yet: dropped") != std::string::npos, "a delete before the add is a drop");
        Check(dir.registry().Find("a1") == nullptr, "...and takes it out of the registry");
        Check(!dir.Apply(Delete("a1")).ok, "a second delete: no such car");
        Check(dir.Apply(Ambient("a1", "va_mb_eclipse_04")).ok, "the id is free again after the delete");
    }
    {  // the gate decides: nothing is called while the world is not up, the call goes when it is
        Director dir;
        AmbientGame game;
        dir.BeginScene();
        dir.Apply(Ambient("a1", "va_gmc_yukon_05"));
        Check(dir.Poll(0, 0xBF33C450u).call.native == Native::kNone, "a gate nobody set: no call");
        AmbientGate g = OpenGate();
        g.world_updates = 50;
        dir.SetAmbientGate(g);
        Check(dir.Poll(16, 0xBF33C450u).call.native == Native::kNone, "50 updates up: still too soon");
        g.world_updates = 120;
        dir.SetAmbientGate(g);
        const Director::Next n = dir.Poll(32, 0xBF33C450u);
        Check(n.call.native == Native::kAddAmbient && n.car == "a1", "120 updates: the add");
        dir.Report(game.Run(n.call));
        Check(dir.Poll(48, 0xBF33C450u).call.native == Native::kNone, "nothing more to do for a placed car");
        // not allowed to call (a menu): the books are kept, nothing is handed out, even a remove
        dir.EndScene();
        Check(dir.Poll(64, 0xBF33C450u, false).call.native == Native::kNone, "no calls while the world is not ready");
        const Director::Next r = dir.Poll(80, 0xBF33C450u);
        Check(r.call.native == Native::kRemoveAmbient, "the remove goes when the world is ready again");
        dir.Report(game.Run(r.call));
        dir.Poll(96, 0xBF33C450u);
        Check(!dir.Active(), "done");
    }
    {  // the cap: a scene makes at most eight
        Director dir;
        AmbientGame game;
        dir.BeginScene();
        dir.SetAmbientGate(OpenGate());
        for (int i = 0; i < 10; ++i) dir.Apply(Ambient("c" + std::to_string(i), "va_gmc_yukon_05", 800.0 + 7.0 * i, -800.0));
        for (int64_t t = 0; t < 4000; t += 16) {
            Director::Next n = dir.Poll(t, 0xBF33C450u);
            if (n.call.native != Native::kNone) dir.Report(game.Run(n.call));
        }
        Check(game.adds.size() == kAmbientMaxPerScene, "the ninth and tenth are not placed");
        Check(dir.failed() == 2, "...they fail, and say so");
        bool cap = false;
        for (const CarEvent& e : dir.TakeEvents()) cap = cap || (e.warning && e.text.find("as many ambient cars as one scene may") != std::string::npos);
        Check(cap, "the warning names the cap");
    }
    {  // racers and ambient cars together: the racer's delete first, one native an update
        Director dir;
        FakeGame game;
        AmbientGame agame;
        dir.BeginScene();
        dir.SetAmbientGate(OpenGate());
        dir.Apply(Spawn("p1", "vp_lam_murcielago_06"));
        dir.Apply(Ambient("a1", "va_gmc_yukon_05"));
        int64_t t = 0;
        int adds = 0;
        for (; t < 12000; t += 16) {
            Director::Next n = dir.Poll(t, 0xBF33C450u);
            if (n.call.native == Native::kNone) continue;
            if (n.call.native == Native::kAddAmbient) {
                ++adds;
                dir.Report(agame.Run(n.call));
            } else {
                dir.Report(game.Run(n.call, t));
            }
        }
        Check(adds == 1 && dir.spawned() == 2, "a racer and an ambient car both made");
        dir.EndScene();
        for (; t < 30000 && dir.Active(); t += 16) {
            Director::Next n = dir.Poll(t, 0xBF33C450u);
            if (n.call.native == Native::kNone) continue;
            if (n.call.native == Native::kRemoveAmbient) dir.Report(agame.Run(n.call));
            else dir.Report(game.Run(n.call, t));
        }
        Check(!dir.Active() && dir.deleted() == 2 && dir.failed() == 0, "both gone, none failed");
        Check(agame.removed.size() == 1 && game.N("OpponentManager_DeleteOpponent") == 1, "one remove and one delete");
    }
    {  // an ambient car the game refuses fails; a scene that ends mid-wait leaves nothing behind
        Director dir;
        AmbientGame game;
        game.add_returns = -1;
        dir.BeginScene();
        dir.SetAmbientGate(OpenGate());
        dir.Apply(Ambient("a1", "va_gmc_yukon_05"));
        for (int64_t t = 0; t < 200; t += 16) {
            Director::Next n = dir.Poll(t, 0xBF33C450u);
            if (n.call.native != Native::kNone) dir.Report(game.Run(n.call));
        }
        dir.TakeEvents();
        Check(dir.failed() == 1 && dir.spawned() == 0, "a refused placement counts as failed, not made");
        dir.EndScene();
        dir.Poll(300, 0xBF33C450u);
        Check(!dir.Active(), "nothing is left to remove");
    }
    {  // the scene ends while a car still waits for the world: dropped
        Director dir;
        dir.BeginScene();
        dir.Apply(Ambient("a1", "va_gmc_yukon_05"));
        dir.Poll(0, 0xBF33C450u);
        dir.EndScene();
        dir.Poll(16, 0xBF33C450u);
        Check(!dir.Active() && dir.failed() == 0, "dropped before it was placed, no fault");
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------- paint and the seeds (scene_seed.h)

void TestPaintVerbs() {
    {
        const Parsed p = P("a1 ambient va_gmc_yukon_05 at 1,2,3 heading 90 paint 3");
        Check(p.status == CarParse::kOk && p.cmd.has_paint && p.cmd.paint == 3, "ambient: paint 3");
        Check(DescribeCarCommand(p.cmd) == "a1 ambient va_gmc_yukon_05 at (1.00, 2.00, 3.00) heading 90.00 paint 3", "the log line says the paint");
    }
    {
        const Parsed p = P("a1 ambient va_gmc_yukon_05 paint 12 at 1,2,3");  // the order of the options is free
        Check(p.status == CarParse::kOk && p.cmd.has_paint && p.cmd.paint == 12, "paint before at");
    }
    Check(P("p1 spawn vp_x at 1,2,3 colour 5").cmd.paint == 5, "colour is paint");
    Check(P("p1 spawn vp_x at 1,2,3 COLOR 0").cmd.has_paint, "color, upper case; paint 0 is a paint");
    Check(P("p1 spawn vp_x at 1,2,3 paint 9999").status == CarParse::kOk, "paint 9999 is the largest");
    Check(!P("p1 spawn vp_x at 1,2,3").cmd.has_paint, "no paint written: none (the Director's default comes later)");
    CheckError("p1 spawn vp_x at 1,2,3 paint", "paint needs a whole number");
    CheckError("p1 spawn vp_x at 1,2,3 paint red", "paint needs a whole number");
    CheckError("p1 spawn vp_x at 1,2,3 paint 10000", "paint needs a whole number");
    CheckError("p1 spawn vp_x at 1,2,3 paint -1", "paint needs a whole number");
    CheckError("p1 spawn vp_x at 1,2,3 paint 1.5", "paint needs a whole number");
    CheckError("p1 spawn vp_x at 1,2,3 paint 1 paint 2", "spawn: a second 'paint'");
    CheckError("p1 ambient vp_x at 1,2,3 paint 1 colour 2", "ambient: a second 'paint'");
    CheckError("p1 spawn profile 2 at 1,2,3 paint 1", "keeps the paint the owner gave it");
    CheckError("p1 spawn garage:2 at 1,2,3 paint 1", "keeps the paint the owner gave it");
    CheckError("p1 spawn vp_x at 1,2,3 sparkle", "unknown option 'sparkle' (at, near, heading, paint)");
}

void TestSeeds() {
    using namespace mc::cinematic::seed;
    // the game's seeder (sub_821CE210): lo = seed (1 for 0), hi = rotl16(seed) ^ seed
    Generator g = SeedGenerator(1);
    Check(g.lo == 1u && g.hi == 0x00010001u, "seed 1: lo 1, hi 0x10001");
    Check(SeedGenerator(0).lo == 1u, "seed 0 is lo 1");
    Check(SeedGenerator(0x12345678u).hi == (0x56781234u ^ 0x12345678u), "hi is the seed turned by 16 bits, xor the seed");
    // a draw: x = lo * 0x5CDCFAA7 + hi, lo = low word, hi = high word, the draw the low 31 bits of the new lo
    const uint32_t first = Draw31(g);
    Check(first == ((1u * 0x5CDCFAA7u + 0x00010001u) & 0x7FFFFFFFu) && g.lo == 0x5CDDFAA8u && g.hi == 0u, "the first draw from seed 1 (computed by hand)");
    Generator h = SeedGenerator(0xFFFFFFFFu);
    const uint64_t x = static_cast<uint64_t>(h.lo) * 0x5CDCFAA7u + h.hi;
    Draw31(h);
    Check(h.lo == static_cast<uint32_t>(x) && h.hi == static_cast<uint32_t>(x >> 32), "the carry goes into hi");
    // the draws are a pure function of the state
    Generator a = SeedGenerator(77), b = SeedGenerator(77);
    bool same = true;
    for (int i = 0; i < 100; ++i) same = same && Draw31(a) == Draw31(b);
    Check(same, "the same seed gives the same draws");
    // a scene update's seed: the same input, the same output; another update, another generator, another seed give another one; never 0
    Check(FrameSeed(1, 40, Lane::kSim) == FrameSeed(1, 40, Lane::kSim), "FrameSeed is a function");
    Check(FrameSeed(1, 40, Lane::kSim) != FrameSeed(1, 41, Lane::kSim), "the next update has another seed");
    Check(FrameSeed(1, 40, Lane::kSim) != FrameSeed(1, 40, Lane::kVisual) && FrameSeed(1, 40, Lane::kSim) != FrameSeed(1, 40, Lane::kConfig), "each generator has its own");
    Check(FrameSeed(1, 40, Lane::kSim) != FrameSeed(2, 40, Lane::kSim), "another scene seed gives another one");
    bool nonzero = true;
    for (uint64_t u = 0; u < 5000; ++u) nonzero = nonzero && FrameSeed(0, u, Lane::kSim) != 0 && FrameSeed(1, u, Lane::kConfig) != 0;
    Check(nonzero, "a seed is never 0");
    Check(PinnedState(3, 9, Lane::kVisual).lo == SeedGenerator(FrameSeed(3, 9, Lane::kVisual)).lo, "PinnedState is the game's seeding of FrameSeed");
    // the paint's seed: 31 bits, never 0, the same every time, different for different paints
    bool paints_ok = true;
    for (int p = 0; p < 64; ++p) {
        const uint32_t s = PaintSeed(p);
        paints_ok = paints_ok && s != 0 && s <= 0x7FFFFFFFu && s == PaintSeed(p);
        for (int q = 0; q < p; ++q) paints_ok = paints_ok && PaintSeed(q) != s;
    }
    Check(paints_ok, "PaintSeed: 31 bits, not 0, stable, and the first 64 are all different");
    Check(DefaultPaint(0) == 0 && DefaultPaint(3) == 3 && DefaultPaint(10003) == 3 && DefaultPaint(-1) == 0, "the default paint is the place in the scene, wrapped");
}

void TestPaintDirector() {
    // ambient: the default paint is the place among the scene's cars; a written paint wins and still takes its place
    {
        Director dir;
        AmbientGame game;
        dir.BeginScene();
        dir.SetAmbientGate(OpenGate());
        Check(dir.Apply(P("a1 ambient va_gmc_yukon_05 at 810,17,-800").cmd).ok, "a1 applies");
        Check(dir.Apply(P("a2 ambient va_gmc_yukon_05 at 817,17,-800 paint 7").cmd).ok, "a2 applies with a paint");
        Check(dir.Apply(P("a3 ambient va_gmc_yukon_05 at 824,17,-800").cmd).ok, "a3 applies");
        Check(dir.Apply(P("a4 ambient va_gmc_yukon_05 at 831,17,-800").cmd).ok, "a4 applies");
        for (int64_t t = 0; t < 2000; t += 16) {
            Director::Next n = dir.Poll(t, 0xBF33C450u);
            if (n.call.native != Native::kNone) dir.Report(game.Run(n.call));
        }
        Check(game.adds.size() == 4, "four adds");
        if (game.adds.size() == 4) {
            Check(game.adds[0].paint == 0 && game.adds[1].paint == 7 && game.adds[2].paint == 2 && game.adds[3].paint == 3, "paints 0, 7 (written), 2, 3: the place in the file");
            Check(game.adds[0].paint_seed == seed::PaintSeed(0) && game.adds[1].paint_seed == seed::PaintSeed(7), "the call carries the paint's seed");
            Check(game.adds[0].paint_seed != game.adds[2].paint_seed, "two default paints differ");
        }
        // the same scene again gives the same numbers
        Director dir2;
        AmbientGame game2;
        dir2.BeginScene();
        dir2.SetAmbientGate(OpenGate());
        dir2.Apply(P("a1 ambient va_gmc_yukon_05 at 810,17,-800").cmd);
        dir2.Apply(P("a2 ambient va_gmc_yukon_05 at 817,17,-800 paint 7").cmd);
        for (int64_t t = 0; t < 1000; t += 16) {
            Director::Next n = dir2.Poll(t, 0xBF33C450u);
            if (n.call.native != Native::kNone) dir2.Report(game2.Run(n.call));
        }
        Check(game2.adds.size() == 2 && game.adds.size() == 4 && game2.adds[0].paint_seed == game.adds[0].paint_seed && game2.adds[1].paint_seed == game.adds[1].paint_seed,
              "a second run of the same lines asks for the same seeds");
        // the log line of the apply says the paint was a default
        Director dir3;
        dir3.BeginScene();
        const Director::Applied ap = dir3.Apply(P("a1 ambient va_gmc_yukon_05 at 810,17,-800").cmd);
        Check(ap.message.find("paint 0 (the default)") != std::string::npos, "the apply line names the default paint");
    }
    // a racer-class car: the natives that may draw its look carry the paint; the lookups and the stream polls do not
    {
        Director dir;
        FakeGame game;
        dir.BeginScene();
        dir.Apply(P("p1 spawn vp_lam_murcielago_06 at 900,17,-850 heading 270 paint 4").cmd);
        dir.Apply(P("p2 spawn vp_lam_murcielago_06 at 905,17,-850 heading 270").cmd);
        int double_issue = 0;
        Drive(dir, game, 0, 24000, 0xBF33C450u, &double_issue);
        int p1_create = 0, p1_load = 0, p2_create = 0, with_paint_elsewhere = 0;
        for (const Call& c : game.detail) {
            const bool draws = c.native == Native::kCreateOpponent || c.native == Native::kLoadOpponent || c.native == Native::kAddOpponent;
            if (!draws && c.paint >= 0) ++with_paint_elsewhere;
            if (c.native == Native::kCreateOpponent && c.paint == 4) ++p1_create;
            if (c.native == Native::kLoadOpponent && c.paint == 4 && c.paint_seed == seed::PaintSeed(4)) ++p1_load;
            if (c.native == Native::kCreateOpponent && c.paint == 1) ++p2_create;  // p2 is the second car of the scene: default paint 1
        }
        Check(p1_create >= 1 && p1_load >= 1, "p1's create and load carry paint 4 and its seed");
        Check(p2_create >= 1, "p2 has the default paint of its place (1)");
        Check(with_paint_elsewhere == 0, "no other native carries a paint");
    }
    // a garage car has none
    {
        Director dir;
        FakeGame game;
        dir.BeginScene();
        dir.Apply(P("g1 spawn profile 2 at 900,17,-850 heading 270").cmd);
        int double_issue = 0;
        Drive(dir, game, 0, 24000, 0xBF33C450u, &double_issue);
        bool any = false;
        for (const Call& c : game.detail) any = any || c.paint >= 0;
        Check(!any, "a garage car's natives carry no paint");
    }
}

// The renderer's noise frame follows the scene (gpu/scene_noise.h). The game thread: SetUpdate at the update's tick, OnHandOver when the update is handed to the guest's
// render thread, OnGuestSwap at the swap that closes the frame; the render thread: BeginFrame when a list opens, OnPresent after the present record. The tests play both
// sides in the orders the game has, including the ones that broke the first version (it paired the k-th swap with the k-th list: det1 never paired at all).
void TestSceneNoise() {
    namespace sn = mcla::gpu::scene_noise;
    Check(!sn::Active(), "no scene noise at the start");
    Check(sn::Index(77) == 77, "outside a scene the renderer's own count is the noise frame");
    // THE LEGAL SCREENS AND THE LOADING LOOPS: more swaps than the ring holds, each a present record, none a list (an idle present) and none a hand-over
    for (int i = 0; i < 700; ++i) {
        sn::OnGuestSwap();
        sn::OnPresent();
    }
    // a game frame before the scene: handed over with no scene, drawn, swapped
    sn::OnHandOver();
    Check(!sn::BeginFrame(), "a frame before the scene carries no tag");
    Check(sn::Index(500) == 500 && sn::CurrentUpdate() == -1, "...so it keeps the renderer's own count");
    sn::OnGuestSwap();
    sn::OnPresent();
    // the scene begins: update 0 is handed over, drawn, swapped
    sn::Rearm();
    sn::SetUpdate(0);
    Check(sn::Active(), "a scene holds the noise");
    sn::OnHandOver();
    Check(sn::BeginFrame(), "the first frame of the scene is the first tagged frame (the log line), after 700 idle presents");
    Check(sn::Index(501) == 0 && sn::CurrentUpdate() == 0, "...and shows update 0, whatever the renderer's count");
    sn::OnGuestSwap();
    // the game runs ahead of the renderer by one frame: update 1 is handed over and swapped before the renderer has presented frame 0
    sn::SetUpdate(1);
    sn::OnHandOver();
    sn::OnGuestSwap();
    sn::OnPresent();  // frame 0's present record is executed
    Check(!sn::BeginFrame() && sn::Index(502) == 1, "the next frame shows update 1 and is not the first");
    // a flush in the middle of a guest frame opens a second list: the same frame, the same number
    Check(!sn::BeginFrame() && sn::Index(503) == 1, "a second list of the same guest frame reads the same tag");
    sn::OnPresent();
    // an idle present inside the scene (a swap that no hand-over came before): its slot has no tag, and the frames after it are right again
    sn::OnGuestSwap();
    Check(!sn::BeginFrame() && sn::Index(600) == 600, "a frame with no hand-over in the middle of the scene keeps the renderer's count");
    sn::OnPresent();
    sn::SetUpdate(2);
    sn::OnHandOver();
    Check(!sn::BeginFrame() && sn::Index(504) == 2, "an idle present in the middle does not shift the pairing: update 2");
    sn::OnGuestSwap();
    sn::OnPresent();
    // the scene ends
    sn::SetUpdate(-1);
    sn::OnHandOver();
    Check(!sn::Active(), "the scene is over");
    Check(!sn::BeginFrame() && sn::Index(9) == 9 && sn::CurrentUpdate() == -1, "a frame after the scene keeps the renderer's count");
    sn::OnGuestSwap();
    sn::OnPresent();
    const sn::Summary s = sn::GetSummary();
    Check(s.stamped == 3 && s.found == 4 && s.missed == 1, "the scene's counters: 3 hand-overs carried an update, 4 frames found a tag, 1 (the one with no hand-over) did not");
    // the same scene again, with another renderer count and another swap count: the same numbers
    sn::Rearm();
    sn::SetUpdate(0);
    sn::OnHandOver();
    sn::OnGuestSwap();
    sn::SetUpdate(1);
    sn::OnHandOver();
    sn::OnGuestSwap();
    Check(sn::BeginFrame() && sn::Index(123456) == 0, "the second run: update 0 again");
    sn::OnPresent();
    Check(!sn::BeginFrame() && sn::Index(123457) == 1, "...and update 1");
    sn::OnPresent();
    sn::SetUpdate(-1);
    // a renderer more than the ring behind loses the tag of the frames it overran (the fallback), never a wrong number
    sn::SetUpdate(5);
    for (int i = 0; i < 300; ++i) {
        sn::OnHandOver();
        sn::OnGuestSwap();
    }
    sn::SetUpdate(-1);
    Check(!sn::BeginFrame() && sn::Index(42) == 42, "a frame the ring has overwritten keeps the renderer's count");
}

// A still (mcla_scene_t): the update number is the pinned time's, so every frame of the still, and every later still of the same time, has the same one.
void TestStillUpdates() {
    using namespace mc::cinematic::seed;
    SceneUpdateCounter c;
    bool running_ok = true;
    for (uint64_t k = 0; k < 400; ++k) running_ok = running_ok && c.Take(false, double(k) / 60.0) == k;
    Check(running_ok, "a running scene counts its updates one by one from 0");
    Check(StillUpdate(6.5) == 390 && StillUpdate(0.0) == 0 && StillUpdate(-1.0) == 0 && StillUpdate(std::nan("")) == 0, "a still's number at 60 a second: 6.5 s is 390; no time is 0");
    // the harness pins 6.5 s while the running clock stands at update 390 or just past it, and holds it for a few hundred updates
    SceneUpdateCounter p;
    for (uint64_t k = 0; k < 391; ++k) p.Take(false, double(k) / 60.0);
    bool held_ok = true;
    for (int i = 0; i < 500; ++i) held_ok = held_ok && p.Take(true, 6.5) == 390;
    Check(held_ok, "500 updates pinned at 6.5 s all carry number 390, however long the hold lasts");
    Check(p.Take(false, 6.5 + 1.0 / 60.0) == 391 && p.Take(false, 6.5 + 2.0 / 60.0) == 392, "let go, the count goes on from the pinned time's number");
    // the clock ran on 12 updates and the same time is pinned again: the same number
    for (int i = 0; i < 10; ++i) p.Take(false, 6.6 + double(i) / 60.0);
    Check(p.Take(true, 6.5) == 390, "pinned again at the same time after the clock ran on: 390 again");
    // two runs that held a still for a different number of updates number every later update alike
    SceneUpdateCounter x, y;
    for (uint64_t k = 0; k < 390; ++k) {
        x.Take(false, double(k) / 60.0);
        y.Take(false, double(k) / 60.0);
    }
    for (int i = 0; i < 363; ++i) x.Take(true, 6.5);
    for (int i = 0; i < 371; ++i) y.Take(true, 6.5);
    Check(x.Take(false, 6.5 + 1.0 / 60.0) == y.Take(false, 6.5 + 1.0 / 60.0), "a hold of 363 updates and a hold of 371 leave the same numbers behind");
    x.Take(false, 6.6);
    Check(x.Pin(0) == 0 && y.Pin(0) == 0, "frame zero of a take uses the same seed after different amounts of preview");
    Check(x.Pin(42) == 42 && x.Take(false, 0.0) == 43, "the running count resumes after the last pinned take frame");
}

// ---------------------------------------------------------------------------------------------------------------- the player's place, and cars near others (SCENE-PLAYER-POSE)

void TestPoseVerbs() {
    {
        const Parsed p = P("player at 832.19,16.9,-787.53 heading 62");
        Check(p.status == CarParse::kOk && p.cmd.verb == CarVerb::kPlace && p.cmd.id == "player", "player at: a place");
        CheckNum(p.cmd.position[0], 832.19, "place x");
        CheckNum(p.cmd.position[1], 16.9, "place y");
        CheckNum(p.cmd.position[2], -787.53, "place z");
        CheckNum(p.cmd.heading, 62.0, "place heading");
        Check(p.cmd.has_heading && !p.cmd.relative, "place: a heading, not relative");
    }
    {
        const Parsed p = P("Player AT 1 2 3");  // any case, three numbers, no heading: his own is kept
        Check(p.status == CarParse::kOk && p.cmd.verb == CarVerb::kPlace && !p.cmd.has_heading, "player at, upper case, three numbers, no heading");
        CheckNum(p.cmd.position[2], 3.0, "place z (three numbers)");
    }
    CheckNum(P("player at 1,2,3 heading -90").cmd.heading, 270.0, "place heading -90 is 270");
    CheckNum(P("player at 1,2,3 heading 10").cmd.heading, 10.0, "heading after at");
    CheckError("player at", "at needs x,y,z");
    CheckError("player at 1,2", "bad position");
    CheckError("player at 1 2", "at needs x,y,z");
    CheckError("player at 1,2,3 at 4,5,6", "a second 'at'");
    CheckError("player at 1,2,3 heading", "heading needs degrees");
    CheckError("player at 1,2,3 heading 1 heading 2", "a second 'heading'");
    CheckError("player at 999999,0,0", "outside the world");
    CheckError("player at 1,2,3 paint 3", "unknown option 'paint'");
    CheckError("player at 1,2,3 near n1 1,0,0", "unknown option 'near'");
    CheckError("player at heading 10", "at needs x,y,z");
    CheckError("p1 at 1,2,3", "only the player's car is put with `at`");
    CheckError("player spawn vp_x at 1,2,3", "car player at");  // the 'player' message names the verbs that do apply
    CheckError("player ambient vp_x at 1,2,3", "car player at");

    // near
    {
        const Parsed p = P("n1 spawn vp_lam_murcielago_06 near player -3.35,0.3,0 heading 10");
        Check(p.status == CarParse::kOk && p.cmd.verb == CarVerb::kSpawn && p.cmd.relative && p.cmd.anchor == "player", "spawn near player");
        CheckNum(p.cmd.offset[0], -3.35, "offset right");
        CheckNum(p.cmd.offset[1], 0.3, "offset up");
        CheckNum(p.cmd.offset[2], 0.0, "offset ahead");
        CheckNum(p.cmd.heading, 10.0, "the relative heading");
        Check(p.cmd.has_heading, "has_heading (relative)");
        Check(DescribeCarCommand(p.cmd) == "n1 spawn vp_lam_murcielago_06 near player (-3.35 right, 0.30 up, 0.00 ahead) heading +10.00 from its own",
              "describe a near spawn: " + DescribeCarCommand(p.cmd));
    }
    {
        const Parsed p = P("a1 AMBIENT va_gmc_yukon_05 NEAR Player 1 2 -3");  // three numbers, any case; the player is "player"
        Check(p.status == CarParse::kOk && p.cmd.verb == CarVerb::kAmbient && p.cmd.relative && p.cmd.anchor == "player" && !p.cmd.has_heading, "ambient near Player (three numbers, no heading)");
        CheckNum(p.cmd.offset[2], -3.0, "offset ahead (three numbers)");
        Check(DescribeCarCommand(p.cmd).find("heading +0.00 from its own") != std::string::npos, "describe: no heading is the same way as the car");
    }
    {
        const Parsed p = P("n2 spawn garage:3 heading 180 near n1 0,0,-5");  // heading first; a garage car; a spawned car as the anchor
        Check(p.status == CarParse::kOk && p.cmd.source == CarSource::kGarage && p.cmd.slot == 3 && p.cmd.relative && p.cmd.anchor == "n1" && p.cmd.heading == 180.0, "garage car near n1, heading first");
    }
    {
        const Parsed p = P("n3 spawn vp_x near n1 1,0,0 paint 7");
        Check(p.status == CarParse::kOk && p.cmd.has_paint && p.cmd.paint == 7 && p.cmd.relative, "near and paint go together");
    }
    CheckError("n1 spawn vp_x near", "near needs a car");
    CheckError("n1 spawn vp_x near 3x 1,0,0", "is not one");
    CheckError("n1 spawn vp_x near player", "near needs right,up,forward");
    CheckError("n1 spawn vp_x near player 1,2", "bad offset");
    CheckError("n1 spawn vp_x near player 1 2", "near needs right,up,forward (as");
    CheckError("n1 spawn vp_x near player 1 2 z", "bad offset 'z'");
    CheckError("n1 spawn vp_x near player 1,2,3 near player 1,2,3", "a second 'near'");
    CheckError("n1 spawn vp_x near player 1,2,3 at 1,2,3", "do not go together");
    CheckError("n1 spawn vp_x at 1,2,3 near player 1,2,3", "do not go together");
    CheckError("n1 spawn vp_x near n1 1,0,0", "cannot be near itself");
    CheckError("n1 spawn vp_x near player 5000,0,0", "more than 1000 m");
    CheckError("n1 spawn vp_x near player 1,2,3 heading", "heading needs degrees");
    CheckError("n1 ambient vp_x near player 1,2,3 heading 1 heading 2", "ambient: a second 'heading'");
    CheckError("n1 spawn vp_x heading 5", "needs a place");
    Check(P("n1 spawn vp_x near player nan,0,0").status == CarParse::kError, "a nan offset is refused");
    // the place line of a spawn is unchanged
    Check(P("n1 spawn vp_x at 1,2,3").status == CarParse::kOk && !P("n1 spawn vp_x at 1,2,3").cmd.relative, "an `at` spawn is not relative");
}

CarPose MakePose(double x, double y, double z, double scene_heading) {
    const double pos[3] = {x, y, z};
    float m[16];
    MatrixForNativeHeading(NativeHeadingDeg(scene_heading), pos, m);
    CarPose p;
    PoseFromMatrix(m, &p);
    return p;
}

void TestResolveRelative() {
    const double kPi = 3.14159265358979323846;
    for (double h : {0.0, 62.0, 90.0, 180.0, 270.0, 331.7}) {
        const CarPose a = MakePose(832.19, 16.5, -787.53, h);
        CarCommand c = P("n1 spawn vp_x near player 3.35,0.3,-9.5").cmd;
        double pos[3];
        double heading = -1.0;
        ResolveRelative(a, c, pos, &heading);
        const double fx = std::sin(h * kPi / 180.0), fz = std::cos(h * kPi / 180.0);  // forward: (sin h, cos h) on the ground
        const double rx = -std::cos(h * kPi / 180.0), rz = std::sin(h * kPi / 180.0);  // right: seen from above, x east, z south: facing south (h 0) his right is west
        CheckNum(pos[0], 832.19 + rx * 3.35 + fx * -9.5, "right 3.35, back 9.5: x at heading " + std::to_string(h), 2e-4);
        CheckNum(pos[1], 16.5 + 0.3, "up 0.3: y at heading " + std::to_string(h), 2e-4);
        CheckNum(pos[2], -787.53 + rz * 3.35 + fz * -9.5, "right 3.35, back 9.5: z at heading " + std::to_string(h), 2e-4);
        CheckNum(heading, h, "no heading given: the same as the anchor's, " + std::to_string(h), 1e-3);
    }
    {  // a place 1.5 m of gap beside a car is on its right when the offset is positive: the other car's matrix says the same
        const CarPose a = MakePose(100.0, 5.0, 200.0, 62.0);
        const double right_dot = a.right[0] * (-std::cos(62.0 * kPi / 180.0)) + a.right[2] * std::sin(62.0 * kPi / 180.0);
        CheckNum(right_dot, 1.0, "the pose's right is (-cos h, sin h) in x, z", 1e-5);
    }
    {  // heading is an offset: +90 is a quarter turn to the LEFT, 180 faces it, a wrap past 360
        const CarPose a = MakePose(0.0, 0.0, 0.0, 62.0);
        double pos[3];
        double heading = 0.0;
        ResolveRelative(a, P("n spawn vp_x near player 0,0,0 heading 90").cmd, pos, &heading);
        CheckNum(heading, 152.0, "62 + 90", 1e-3);
        ResolveRelative(a, P("n spawn vp_x near player 0,0,0 heading 180").cmd, pos, &heading);
        CheckNum(heading, 242.0, "62 + 180", 1e-3);
        ResolveRelative(a, P("n spawn vp_x near player 0,0,0 heading 300").cmd, pos, &heading);
        CheckNum(heading, 2.0, "62 + 300 wraps to 2", 1e-3);
        ResolveRelative(a, P("n spawn vp_x near player 0,0,0 heading -70").cmd, pos, &heading);
        CheckNum(heading, 352.0, "62 - 70 wraps to 352", 1e-3);
        // 90 degrees to the left: the new car's forward is the anchor's left (minus its right)
        ResolveRelative(a, P("n spawn vp_x near player 0,0,0 heading 90").cmd, pos, &heading);
        CheckNum(std::sin(heading * kPi / 180.0), -a.right[0], "the new car's forward x is the anchor's left", 1e-3);
        CheckNum(std::cos(heading * kPi / 180.0), -a.right[2], "...and z", 1e-3);
    }
    {  // a car on a slope has its frame tilted with it: the matrix's own axes are used (right (1,0,0), up (0,.8,.6), forward (0,-.6,.8))
        CarPose a;
        a.position[0] = 10.0; a.position[1] = 20.0; a.position[2] = 30.0;
        a.right[0] = 1.0; a.right[1] = 0.0; a.right[2] = 0.0;
        a.up[0] = 0.0; a.up[1] = 0.8; a.up[2] = 0.6;
        a.forward[0] = 0.0; a.forward[1] = -0.6; a.forward[2] = 0.8;
        a.heading = 0.0;
        double pos[3];
        double heading = 0.0;
        ResolveRelative(a, P("n spawn vp_x near player 1,2,3").cmd, pos, &heading);
        CheckNum(pos[0], 11.0, "tilted frame x");
        CheckNum(pos[1], 20.0 + 1.6 - 1.8, "tilted frame y");
        CheckNum(pos[2], 30.0 + 1.2 + 2.4, "tilted frame z");
    }
}

// A game with a player and a Director: every tick reads the player's pose (the glue's UpdatePoses), polls, makes the call that was handed out and reports it. The player's car goes where
// a `car player at` put him (and drops `drop` metres onto the road); a scene car that is ready is read at the place it was loaded at.
struct PoseWorld {
    Director dir;
    FakeGame game;
    uint32_t racer = 0xBF33C450u;
    double pos[3] = {819.61, 17.56, -808.41};  // where the starter save leaves him
    double heading = 331.7;
    double drop = 0.4;
    uint64_t frame = 0;
    int64_t t = 0;
    bool feed_player = true;
    bool gate_open = true;
    int32_t next_layout = 135;
    std::vector<Call> puts;  // the Racer_Teleports made on the player
    std::vector<Call> loads;  // the loads of racers
    std::vector<Call> adds;  // the ambient adds
    PoseWorld() { dir.BeginScene(); }

    void Tick() {
        ++frame;
        t += 16;
        if (feed_player) FeedPose(dir, "player", pos[0], pos[1], pos[2], heading, frame);
        for (const Director::Reader& r : dir.PoseReaders()) {
            for (auto it = game.detail.rbegin(); it != game.detail.rend(); ++it) {
                if (it->native == Native::kLoadOpponent && it->racer == r.racer) {
                    FeedPose(dir, r.id, it->position[0], it->position[1], it->position[2], NormalizeDegrees(it->heading_native - 180.0), frame);
                    break;
                }
            }
        }
        for (const Director::AmbientReader& r : dir.AmbientReaders()) {
            for (const Call& c : adds) {
                if (c.index == r.layout_id) {
                    const double p3[3] = {c.position[0], c.position[1], c.position[2]};
                    float m[16];
                    MatrixForNativeHeading(c.heading_native, p3, m);
                    dir.NoteAmbientPose(r.id, m, frame);
                }
            }
        }
        if (gate_open) dir.SetAmbientGate(OpenGate());
        const Director::Next n = dir.Poll(t, racer);
        if (n.call.native == Native::kNone) return;
        Outcome o;
        if (n.call.native == Native::kAddAmbient) {
            Call c = n.call;
            c.index = next_layout;  // remembered with the layout id the game answers
            adds.push_back(c);
            o.called = true;
            o.ret_written = true;
            o.ret = static_cast<uint32_t>(next_layout++);
        } else {
            o = game.Run(n.call, t);
        }
        if (n.call.native == Native::kTeleport && n.call.on_player && o.called) {
            puts.push_back(n.call);
            for (int k = 0; k < 3; ++k) pos[k] = n.call.position[k];
            pos[1] -= drop;
            heading = NormalizeDegrees(n.call.heading_native - 180.0);
        }
        if (n.call.native == Native::kLoadOpponent) loads.push_back(n.call);
        dir.Report(o);
    }
    void Run(int ticks) {
        for (int i = 0; i < ticks; ++i) Tick();
    }
    bool Event(const std::string& part, bool warning_only = false) { return HasEvent(dir, part, warning_only); }
};

CarCommand PlaceCmd(double x, double y, double z, double heading, bool has_heading = true) {
    CarCommand c;
    c.id = "player";
    c.verb = CarVerb::kPlace;
    c.position[0] = x;
    c.position[1] = y;
    c.position[2] = z;
    c.heading = heading;
    c.has_heading = has_heading;
    return c;
}

CarCommand NearCmd(const std::string& id, const std::string& model, const std::string& anchor, double r, double u, double f, double rel_heading = 0.0, bool has_heading = false,
                   bool ambient = false) {
    CarCommand c = ambient ? Ambient(id, model) : Spawn(id, model);
    c.relative = true;
    c.anchor = anchor;
    c.offset[0] = r;
    c.offset[1] = u;
    c.offset[2] = f;
    c.heading = rel_heading;
    c.has_heading = has_heading;
    return c;
}

void TestPlayerPlace() {
    {  // one Racer_Teleport on the player's racer, at the asked place, with no speed
        PoseWorld w;
        Check(w.dir.Apply(PlaceCmd(832.19, 16.9, -787.53, 62.0)).ok, "the place applies");
        Check(w.dir.Active() && w.dir.registry().Find("player") != nullptr, "active; the player is in the registry");
        w.Run(30);
        Check(w.puts.size() == 1, "one put");
        if (w.puts.size() == 1) {
            const Call& c = w.puts[0];
            Check(c.racer == 0xBF33C450u && c.on_player && c.native == Native::kTeleport, "Racer_Teleport on the player's racer, marked on_player");
            CheckNum(c.position[0], 832.19, "x");
            CheckNum(c.position[1], 16.9, "y");
            CheckNum(c.position[2], -787.53, "z");
            CheckNum(c.heading_native, NativeHeadingDeg(62.0), "the heading goes in as the native angle");
            CheckNum(c.speed, 0.0, "no speed");
        }
        Check(w.game.calls.size() == 1 && w.game.calls[0] == "Racer_Teleport", "it is the only native the place makes");
        Check(w.Event("the player's car stands at (832.19, 16.50, -787.53) heading 62.0: the call asked for (832.19, 16.90, -787.53) heading 62.0"), "the pose after the put is logged beside the ask");
        w.dir.TakeEvents();
        w.Run(30);
        Check(w.dir.TakeEvents().empty(), "...once");
    }
    {  // without a heading he keeps his own
        PoseWorld w;
        w.dir.Apply(PlaceCmd(832.19, 16.9, -787.53, 0.0, false));
        w.Run(30);
        Check(w.puts.size() == 1, "put");
        if (!w.puts.empty()) CheckNum(w.puts[0].heading_native, NativeHeadingDeg(331.7), "the heading is the one he has (331.7), as the native angle", 1e-3);
    }
    {  // no heading and no pose: nothing is called, with a warning
        PoseWorld w;
        w.feed_player = false;
        w.dir.Apply(PlaceCmd(832.19, 16.9, -787.53, 0.0, false));
        w.Run(30);
        Check(w.puts.empty() && w.game.calls.empty(), "without a pose to keep the heading nothing is called");
        Check(w.Event("gave no heading and his pose cannot be read", true), "...and the log says why");
        Check(!w.dir.PlayerPutBackPending(), "nothing to put back");
    }
    {  // no player racer (a menu, the garage): no call; it goes on when he is there
        PoseWorld w;
        w.racer = 0;
        w.dir.Apply(PlaceCmd(832.19, 16.9, -787.53, 62.0));
        w.Run(30);
        Check(w.game.calls.empty(), "no player racer: no call");
        w.racer = 0xBF33C450u;
        w.Run(10);
        Check(w.puts.size() == 1, "...and the put is made when he is there");
    }
    {  // the garage dressing finishes first
        PoseWorld w;
        w.dir.Apply(Profile(3));
        w.dir.Apply(PlaceCmd(832.19, 16.9, -787.53, 62.0));
        w.Run(100);
        Check(w.puts.empty() && w.game.N("Racer_ChangeCarByConfig") == 1, "the dressing is made, the put waits for its parts to settle (3 s)");
        w.Run(260);
        Check(w.puts.size() == 1, "...and is made after it");
    }
    {  // a refused call: a warning, nothing to put back
        PoseWorld w;
        w.game.uncallable = true;
        w.dir.Apply(PlaceCmd(832.19, 16.9, -787.53, 62.0));
        w.Run(30);
        Check(w.Event("could not be put at its place", true), "a call that could not be made is a warning");
        w.dir.EndScene();
        w.Run(30);
        Check(w.game.calls.size() == 1 && !w.dir.Active(), "no put-back for a put that never happened; nothing is left to do");
    }
    {  // the scene ends: he is put back where he stood
        PoseWorld w;
        w.dir.Apply(PlaceCmd(832.19, 16.9, -787.53, 62.0));
        w.Run(30);
        w.dir.TakeEvents();
        w.dir.EndScene();
        Check(w.dir.Active() && w.dir.PlayerPutBackPending(), "the scene is over, he is still to be put back");
        Check(w.dir.registry().Find("player") != nullptr, "his registry entry stays until then (the put-back reads where he stands)");
        w.Run(30);
        Check(w.puts.size() == 2, "a second put");
        if (w.puts.size() == 2) {
            CheckNum(w.puts[1].position[0], 819.61, "back at x", 1e-3);
            CheckNum(w.puts[1].position[1], 17.56, "back at y", 1e-3);
            CheckNum(w.puts[1].position[2], -808.41, "back at z", 1e-3);
            CheckNum(w.puts[1].heading_native, NativeHeadingDeg(331.7), "back at his heading", 1e-3);
            Check(w.puts[1].on_player && w.puts[1].racer == 0xBF33C450u, "on his racer");
        }
        Check(w.Event("the player's car is put back where he stood before the scene"), "the log says so");
        Check(!w.dir.Active() && w.dir.registry().Find("player") == nullptr, "nothing left, the registry is clear");
    }
    {  // two places: the origin is the pose before the FIRST
        PoseWorld w;
        w.dir.Apply(PlaceCmd(832.19, 16.9, -787.53, 62.0));
        w.Run(30);
        w.dir.Apply(PlaceCmd(900.0, 17.0, -800.0, 90.0));
        w.Run(30);
        Check(w.puts.size() == 2, "two puts");
        w.dir.EndScene();
        w.Run(30);
        Check(w.puts.size() == 3, "one put-back");
        if (w.puts.size() == 3) {
            CheckNum(w.puts[2].position[0], 819.61, "put back to where he stood before the first place: x", 1e-3);
            CheckNum(w.puts[2].position[2], -808.41, "z", 1e-3);
        }
    }
    {  // he was driven away: not fetched back
        PoseWorld w;
        w.dir.Apply(PlaceCmd(832.19, 16.9, -787.53, 62.0));
        w.Run(30);
        w.dir.EndScene();
        w.pos[0] += 40.0;  // somebody drove him off
        w.Run(30);
        Check(w.puts.size() == 1, "no put-back for a car driven 40 m away");
        Check(w.Event("is not put back: he is 40.0 m from where the scene left him"), "...and the log says how far");
        Check(!w.dir.Active() && w.dir.registry().Find("player") == nullptr, "nothing left to do");
    }
    {  // he moved a little (under 5 m): still put back
        PoseWorld w;
        w.dir.Apply(PlaceCmd(832.19, 16.9, -787.53, 62.0));
        w.Run(30);
        w.dir.EndScene();
        w.pos[2] += 3.0;
        w.Run(30);
        Check(w.puts.size() == 2, "3 m is within the limit: put back");
    }
    {  // another racer is his now (the world was rebuilt): not put back
        PoseWorld w;
        w.dir.Apply(PlaceCmd(832.19, 16.9, -787.53, 62.0));
        w.Run(30);
        w.dir.EndScene();
        w.racer = 0xBF33D000u;
        w.Run(30);
        Check(w.puts.size() == 1, "a different racer: no put-back");
        Check(w.Event("his car is not the one the scene placed"), "...and the log says so");
        Check(!w.dir.Active(), "nothing left to do");
    }
    {  // no pose for the put-back: it waits, and gives up
        PoseWorld w;
        w.dir.Apply(PlaceCmd(832.19, 16.9, -787.53, 62.0));
        w.Run(30);
        w.dir.EndScene();
        w.feed_player = false;
        w.dir.registry().Find("player")->have_pose = false;
        w.Run(100);
        Check(w.puts.size() == 1 && w.dir.Active(), "no pose: no put-back yet");
        w.t += kRestoreGiveUpMs + 100;
        w.Run(5);
        Check(w.puts.size() == 1 && !w.dir.Active() && w.Event("his pose could not be read"), "...it gives up after kRestoreGiveUpMs");
    }
    {  // the scene ends before the put is made: dropped, nothing is put back
        PoseWorld w;
        w.racer = 0;
        w.dir.Apply(PlaceCmd(832.19, 16.9, -787.53, 62.0));
        w.Run(10);
        w.dir.EndScene();
        w.racer = 0xBF33C450u;
        w.Run(30);
        Check(w.game.calls.empty() && !w.dir.Active(), "a place that never got to be made is dropped with the scene");
    }
    {  // the scene ends while the put is in flight: it is put back after
        PoseWorld w;
        w.dir.Apply(PlaceCmd(832.19, 16.9, -787.53, 62.0));
        FeedPose(w.dir, "player", w.pos[0], w.pos[1], w.pos[2], w.heading, 1);
        w.frame = 1;
        w.t = 16;
        const Director::Next n = w.dir.Poll(w.t, w.racer);
        Check(n.call.native == Native::kTeleport && n.call.on_player, "the put is handed out");
        w.dir.EndScene();  // ... and the scene ends before its report
        Check(w.dir.PlayerPutBackPending(), "a put in flight counts: he is to be put back");
        w.pos[0] = n.call.position[0];
        w.pos[1] = n.call.position[1];
        w.pos[2] = n.call.position[2];
        w.dir.Report(w.game.Run(n.call, w.t));
        w.Run(40);
        Check(w.game.N("Racer_Teleport") == 2, "he is put back after the report");
    }
    {  // a second scene begins while the put-back waits: it is dropped, the origin stays for the second scene's own end
        PoseWorld w;
        w.dir.Apply(PlaceCmd(832.19, 16.9, -787.53, 62.0));
        w.Run(30);
        w.dir.EndScene();
        w.racer = 0;  // not on the road: the put-back cannot be made yet
        w.Run(10);
        Check(w.dir.PlayerPutBackPending(), "the put-back waits");
        w.racer = 0xBF33C450u;
        w.dir.BeginScene();
        Check(!w.dir.PlayerPutBackPending(), "a new scene drops the waiting put-back");
        w.dir.Apply(PlaceCmd(900.0, 17.0, -800.0, 90.0));
        w.Run(30);
        w.dir.EndScene();
        w.Run(30);
        Check(w.puts.size() == 3, "the second scene's end puts him back");
        if (w.puts.size() == 3) {
            CheckNum(w.puts[2].position[0], 819.61, "to where he stood before the FIRST scene moved him: x", 1e-3);
            CheckNum(w.puts[2].position[2], -808.41, "z", 1e-3);
        }
    }
    {  // a scene that never moved him puts nothing back
        PoseWorld w;
        w.dir.Apply(Spawn("a", "vp_lam_murcielago_06"));
        w.Run(40);
        w.dir.EndScene();
        w.Run(400);
        Check(w.puts.empty(), "no place, no put-back");
    }
    {  // one native an update: the put does not share a poll with a spawn
        PoseWorld w;
        w.dir.Apply(PlaceCmd(832.19, 16.9, -787.53, 62.0));
        w.dir.Apply(Spawn("a", "vp_lam_murcielago_06"));
        w.Tick();
        Check(w.game.calls.size() == 1 && w.game.calls[0] == "Racer_Teleport", "the put goes first, alone in its update");
        w.Tick();
        Check(w.game.calls.size() == 2 && w.game.calls[1] == "LookupCar", "the spawn's first native is in the next one");
    }
}

void TestNearDirector() {
    {  // the rt-neighbours shape: the player is put, then three cars are placed in his frame
        PoseWorld w;
        w.dir.Apply(PlaceCmd(832.19, 16.9, -787.53, 62.0));
        const Director::Applied a1 = w.dir.Apply(NearCmd("n1", "vp_lam_murcielago_06", "player", -3.35, 0.3, 0.0));
        Check(a1.ok && a1.message.find("(when player's pose is known)") != std::string::npos, "a near spawn applies and says what it waits for: " + a1.message);
        w.dir.Apply(NearCmd("n2", "vp_aud_r8_08", "player", 3.35, 0.3, 0.0));
        w.dir.Apply(NearCmd("n3", "vp_aud_r8_08", "player", 0.0, 0.3, -9.5));
        Check(w.dir.registry().Find("n1") != nullptr && w.dir.WaitingCount() == 3, "the ids are in the registry at once; three wait");
        Check(w.dir.Active() && w.dir.SceneCarCount() == 0, "active; no machine has started");
        w.Tick();
        Check(w.puts.size() == 1 && w.game.calls.size() == 1, "the first update makes the put, nothing else");
        Check(w.dir.WaitingCount() == 3 && w.game.calls.size() == 1, "the cars wait for the put's pose to be read");
        w.Run(2);
        Check(w.dir.WaitingCount() == 0 && w.dir.SceneCarCount() == 3, "the pose was read after the put: the three cars are started");
        Check(w.Event("car n1 near player: player is at (832.19, 16.50, -787.53) heading 62.0; -3.35 right, 0.30 up, 0.00 ahead of it"), "the resolution is logged with the player's pose");
        w.Run(1500);
        Check(w.loads.size() == 3, "three loads");
        const double rx = -std::cos(62.0 * 3.14159265358979323846 / 180.0), rz = std::sin(62.0 * 3.14159265358979323846 / 180.0);
        const double fx = std::sin(62.0 * 3.14159265358979323846 / 180.0), fz = std::cos(62.0 * 3.14159265358979323846 / 180.0);
        if (w.loads.size() == 3) {
            CheckNum(w.loads[0].position[0], 832.19 - 3.35 * rx, "n1 (the left one): x", 2e-4);
            CheckNum(w.loads[0].position[1], 16.8, "n1: y is the player's settled 16.50 + 0.3", 2e-4);
            CheckNum(w.loads[0].position[2], -787.53 - 3.35 * rz, "n1: z", 2e-4);
            CheckNum(w.loads[1].position[0], 832.19 + 3.35 * rx, "n2 (the right one): x", 2e-4);
            CheckNum(w.loads[1].position[2], -787.53 + 3.35 * rz, "n2: z", 2e-4);
            CheckNum(w.loads[2].position[0], 832.19 - 9.5 * fx, "n3 (behind): x", 2e-4);
            CheckNum(w.loads[2].position[2], -787.53 - 9.5 * fz, "n3: z", 2e-4);
            for (const Call& c : w.loads) CheckNum(c.heading_native, NativeHeadingDeg(62.0), "the same heading as the player (native angle)", 1e-3);
        }
        Check(w.dir.spawned() == 3, "all three are ready");
        // the scene's end: the cars go and the player is put back
        w.dir.EndScene();
        w.Run(900);
        Check(w.game.deleted_nodes.size() == 3 && w.puts.size() == 2 && !w.dir.Active(), "three deleted, the player put back, nothing left");
    }
    {  // a relative heading
        PoseWorld w;
        w.dir.Apply(NearCmd("n1", "vp_lam_murcielago_06", "player", 5.0, 0.0, 0.0, 180.0, true));
        w.Run(1000);
        Check(w.loads.size() == 1, "loaded");
        if (!w.loads.empty()) {
            CheckNum(w.loads[0].heading_native, NativeHeadingDeg(NormalizeDegrees(331.7 + 180.0)), "facing back at the player: 180 from his heading", 1e-3);
        }
    }
    {  // nothing is called for a car near a player whose pose is not known; it gives up after a minute
        PoseWorld w;
        w.feed_player = false;
        w.dir.Apply(NearCmd("n1", "vp_lam_murcielago_06", "player", 5.0, 0.0, 0.0));
        w.Run(100);
        Check(w.game.calls.empty() && w.dir.WaitingCount() == 1, "no pose: no call, still waiting");
        Check(w.Event("car n1 (near player) waits for the player's car, whose pose is not known yet"), "the wait is said once");
        w.dir.TakeEvents();
        w.Run(50);
        Check(w.dir.TakeEvents().empty(), "...and only once");
        w.t += kNearGiveUpMs + 1000;
        w.Run(2);
        Check(w.dir.WaitingCount() == 0 && w.dir.failed() == 1 && w.dir.registry().Find("n1") == nullptr, "it gave up: dropped, counted as failed, out of the registry");
        Check(w.Event("waited 60 s for the player's car"), "...with a warning that says what it waited for");
    }
    {  // near a car that does not exist
        PoseWorld w;
        const Director::Applied a = w.dir.Apply(NearCmd("n1", "vp_lam_murcielago_06", "nobody", 5.0, 0.0, 0.0));
        Check(!a.ok && a.message.find("there is no car nobody") != std::string::npos, "an unknown anchor is refused at once: " + a.message);
        Check(w.dir.registry().Find("n1") == nullptr && w.dir.WaitingCount() == 0, "nothing is kept");
        Check(w.dir.Apply(Spawn("n1", "vp_lam_murcielago_06")).ok, "the id is free");
    }
    {  // near a spawned car: it waits until that car is ready
        PoseWorld w;
        w.dir.Apply(Spawn("a", "vp_lam_murcielago_06", 900.0, -850.0, 90.0));
        w.dir.Apply(NearCmd("b", "vp_aud_r8_08", "a", 0.0, 0.0, -6.0));
        w.Run(100);
        Check(w.dir.WaitingCount() == 1 && w.loads.size() == 1, "b waits, a is loading");
        Check(w.Event("car b (near a) waits for car a to be ready"), "the wait names the car and its stage");
        w.Run(800);
        Check(w.dir.WaitingCount() == 0 && w.loads.size() == 2, "a is ready: b starts");
        if (w.loads.size() == 2) {
            // a at (900, 17.5, -850) heading 90 (nose along +x): 6 m behind it is x 894
            CheckNum(w.loads[1].position[0], 894.0, "6 m behind a (heading 90): x", 2e-4);
            CheckNum(w.loads[1].position[1], 17.5, "y", 2e-4);
            CheckNum(w.loads[1].position[2], -850.0, "z", 2e-4);
            CheckNum(w.loads[1].heading_native, NativeHeadingDeg(90.0), "a's heading", 1e-3);
        }
    }
    {  // a chain: b near a near the player
        PoseWorld w;
        w.dir.Apply(NearCmd("a", "vp_lam_murcielago_06", "player", 5.0, 0.0, 0.0));
        w.dir.Apply(NearCmd("b", "vp_aud_r8_08", "a", 5.0, 0.0, 0.0));
        Check(w.dir.WaitingCount() == 2, "both wait");
        w.Run(1200);
        Check(w.dir.WaitingCount() == 0 && w.loads.size() == 2, "both are made");
        if (w.loads.size() == 2) {
            double d = std::hypot(w.loads[1].position[0] - w.loads[0].position[0], w.loads[1].position[2] - w.loads[0].position[2]);
            CheckNum(d, 5.0, "b is 5 m from a", 2e-3);
            d = std::hypot(w.loads[0].position[0] - 819.61, w.loads[0].position[2] + 808.41);
            CheckNum(d, 5.0, "a is 5 m from the player", 2e-3);
        }
    }
    {  // the anchor goes (deleted) while the car waits for it
        PoseWorld w;
        w.dir.Apply(Spawn("a", "vp_lam_murcielago_06", 900.0, -850.0, 90.0));
        w.dir.Apply(NearCmd("b", "vp_aud_r8_08", "a", 0.0, 0.0, -6.0));
        w.Run(50);
        w.dir.Apply(Delete("a"));
        w.Run(5);
        Check(w.dir.WaitingCount() == 0 && w.dir.failed() == 1 && w.Event("car a, which it is near, is gone", true), "b is dropped with a warning when a is deleted");
        Check(w.dir.registry().Find("b") == nullptr, "out of the registry");
    }
    {  // a waiting car is deleted by the scene
        PoseWorld w;
        w.feed_player = false;
        w.dir.Apply(NearCmd("n1", "vp_lam_murcielago_06", "player", 5.0, 0.0, 0.0));
        const Director::Applied d = w.dir.Apply(Delete("n1"));
        Check(d.ok && d.message.find("still waiting for the car it is near") != std::string::npos, "delete of a waiting car: " + d.message);
        Check(w.dir.WaitingCount() == 0 && w.dir.registry().Find("n1") == nullptr, "dropped, out of the registry");
        Check(w.dir.Apply(NearCmd("n1", "vp_lam_murcielago_06", "player", 5.0, 0.0, 0.0)).ok, "the id is free again");
    }
    {  // a path or a stop on a waiting car is refused with a reason
        PoseWorld w;
        w.feed_player = false;
        w.dir.Apply(NearCmd("n1", "vp_lam_murcielago_06", "player", 5.0, 0.0, 0.0));
        const Route straight = RouteOf("100,200,10\n100,300,10\n");
        const Director::Applied p = w.dir.Apply(PathCmd("n1", "r.route"), 0.0, &straight);
        Check(!p.ok && p.message.find("still waiting for the car it is near") != std::string::npos, "path on a waiting car: " + p.message);
        const Director::Applied s = w.dir.Apply(StopCmd("n1"));
        Check(!s.ok && s.message.find("still waiting") != std::string::npos, "stop on a waiting car: " + s.message);
    }
    {  // the id is taken by the waiting car
        PoseWorld w;
        w.feed_player = false;
        w.dir.Apply(NearCmd("n1", "vp_lam_murcielago_06", "player", 5.0, 0.0, 0.0));
        const Director::Applied dup = w.dir.Apply(Spawn("n1", "vp_aud_r8_08"));
        Check(!dup.ok && dup.message.find("already exists") != std::string::npos, "a second n1 is ignored while the first waits");
    }
    {  // the scene ends while a car waits: it is dropped and nothing is left
        PoseWorld w;
        w.feed_player = false;
        w.dir.Apply(NearCmd("n1", "vp_lam_murcielago_06", "player", 5.0, 0.0, 0.0));
        w.Run(10);
        w.dir.EndScene();
        Check(!w.dir.Active() && w.dir.WaitingCount() == 0 && w.dir.registry().Find("n1") == nullptr, "dropped with the scene, nothing left");
        w.Run(10);
        Check(w.game.calls.empty(), "no call was ever made");
    }
    {  // an ambient car near the player, and a racer near that ambient car
        PoseWorld w;
        w.dir.Apply(NearCmd("a1", "va_gmc_yukon_05", "player", 6.0, 0.0, 0.0, 0.0, false, true));
        w.dir.Apply(NearCmd("b", "vp_aud_r8_08", "a1", 0.0, 0.0, 8.0));
        for (int i = 0; i < 400 && w.adds.empty(); ++i) w.Tick();
        Check(w.adds.size() == 1 && w.dir.WaitingCount() == 1, "the ambient car is placed, b waits for it to stand in the world (its matrix is read an update later)");
        if (w.adds.size() == 1) {
            const double rx = -std::cos(331.7 * 3.14159265358979323846 / 180.0), rz = std::sin(331.7 * 3.14159265358979323846 / 180.0);
            CheckNum(w.adds[0].position[0], 819.61 + 6.0 * rx, "the ambient car is 6 m to the player's right: x", 2e-4);
            CheckNum(w.adds[0].position[2], -808.41 + 6.0 * rz, "z", 2e-4);
            CheckNum(w.adds[0].heading_native, LayoutHeadingDeg(331.7), "the layout angle of the player's heading", 1e-3);
        }
        w.Run(1500);
        Check(w.dir.WaitingCount() == 0 && w.loads.size() == 1, "a1 stands: b is made");
        if (w.adds.size() == 1 && w.loads.size() == 1) {
            const double d = std::hypot(w.loads[0].position[0] - w.adds[0].position[0], w.loads[0].position[2] - w.adds[0].position[2]);
            CheckNum(d, 8.0, "b is 8 m ahead of a1", 2e-3);
        }
    }
}

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
    TestPathStalePose();
    TestPathBrain();
    TestLost();
    TestDeleteIsNotLost();
    TestAmbientVerbs();
    TestLayoutHeading();
    TestAmbientGuards();
    TestAmbientMachine();
    TestAmbientDirector();
    TestPaintVerbs();
    TestSeeds();
    TestPaintDirector();
    TestSceneNoise();
    TestStillUpdates();
    TestPoseVerbs();
    TestResolveRelative();
    TestPlayerPlace();
    TestNearDirector();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
