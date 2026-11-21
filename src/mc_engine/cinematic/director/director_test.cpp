// Check of the Director's pure parts (director.h, command.h, line_server.h, director_math.h, png_writer.h, json.h). A program of its own: it needs no game and
// exits 0 when every check holds. The sockets are real loopback ones.
//
//   cmake --build <build dir> --target larecomp_director_test      (CMakeLists.txt)
//   <build dir>/larecomp_director_test
//
// The compile by hand (the msvc-target clang of labuild; ws2_32 on Windows):
//   clang++ -std=c++23 -I src/mc_engine/cinematic -I src/mc_engine/cinematic/director director_test.cpp json.cpp command.cpp line_server.cpp scene_edit.cpp director_math.cpp
//           png_writer.cpp ../scene.cpp ../camera.cpp ../freecam.cpp ../path_follow.cpp -o director_test.exe -lws2_32

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "command.h"
#include "director.h"
#include "director_math.h"
#include "line_server.h"
#include "path_follow.h"
#include "png_writer.h"
#include "scene.h"
#include "scene_edit.h"

using namespace mc::cinematic;
using namespace mc::cinematic::director;

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

void CheckEq(const std::string& got, const std::string& want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_failed;
        std::printf("FAILED: %s\n  got:  '%s'\n  want: '%s'\n", what.c_str(), got.c_str(), want.c_str());
    }
}

void CheckNum(double got, double want, const std::string& what, double tol = 1e-9) {
    ++g_checks;
    if (!(std::fabs(got - want) <= tol)) {
        ++g_failed;
        std::printf("FAILED: %s: got %.9g, want %.9g\n", what.c_str(), got, want);
    }
}

Json P(const std::string& text) {
    Json j;
    std::string err;
    if (!Json::Parse(text, &j, &err)) std::printf("test bug: bad JSON '%s': %s\n", text.c_str(), err.c_str());
    return j;
}

// ---------------------------------------------------------------------------------------------------------------- JSON

void TestJson() {
    Json j = P(R"({"a":1,"b":[true,false,null],"c":{"d":"x"},"e":-2.5e3,"f":"\u00e9\n\"q\""})");
    Check(j.is_object() && j.size() == 5, "parse an object");
    Check(j.Get("a").AsInt() == 1 && j.Get("b").size() == 3 && j.Get("b").at(0).AsBool() && j.Get("b").at(2).is_null(), "members");
    CheckNum(j.NumberOr("e", 0), -2500.0, "exponent");
    CheckEq(j.Get("f").AsString(), "\xC3\xA9\n\"q\"", "string escapes (UTF-8 for \\u00e9)");
    CheckEq(j.Dump(), R"({"a":1,"b":[true,false,null],"c":{"d":"x"},"e":-2500,"f":"é\n\"q\""})", "dump keeps order, integers without a point");
    // numbers
    CheckEq(FormatNumber(819.6), "819.6", "819.6");
    CheckEq(FormatNumber(0.1), "0.1", "0.1");
    CheckEq(FormatNumber(1e20), "1e+20", "1e20");
    CheckEq(FormatNumber(-0.0), "0", "negative zero");
    CheckEq(FormatNumber(3.0), "3", "3");
    CheckEq(FormatNumber(1.0 / 3.0), "0.3333333333333333", "shortest round trip");
    CheckEq(FormatNumber(std::nan("")), "null", "nan");
    Check(P("0.1").AsNumber() == 0.1 && P("819.6").AsNumber() == 819.6, "numbers read back exactly");
    // errors
    const char* bad[] = {"", "{", "{\"a\":}", "[1,]", "{\"a\":1,}", "01", "1.", "-", "\"abc", "\"\\x\"", "{\"a\" 1}", "[1 2]", "nul", "tru", "1 2", "\"\t\"", "\"\\ud800\"", "\"\\udc00\"", "{1:2}", "[", "]"};
    for (const char* b : bad) {
        Json out;
        std::string err;
        Check(!Json::Parse(b, &out, &err) && !err.empty(), std::string("bad JSON refused: ") + b);
    }
    std::string deep(100, '[');
    deep += std::string(100, ']');
    Json out;
    std::string err;
    Check(!Json::Parse(deep, &out, &err), "nested too deep is refused");
    std::string ok_deep(60, '[');
    ok_deep += std::string(60, ']');
    Check(Json::Parse(ok_deep, &out, &err), "60 levels are fine");
    Check(Json::Parse("  {\"a\" : 1 }  \n", &out, &err), "blanks around");
    Check(Json::Parse("\"\\ud83d\\ude00\"", &out, &err) && out.AsString() == "\xF0\x9F\x98\x80", "a surrogate pair");
    CheckEq(Json(std::string("a\x01z\xFF")).Dump(), "\"a\\u0001z\xEF\xBF\xBD\"", "control and invalid UTF-8 bytes");
    Check(err.empty() || true, "");
    // the builders and typed reads
    Json o = Json::Object();
    o.Set("n", 3);
    o.Set("n", 4);
    Check(o.size() == 1 && o.IntOr("n", 0) == 4, "Set replaces");
    o.Set("v", Json::Vec3(1, 2, 3));
    double v[3];
    Check(o.Get("v").ToVec3(v) && v[2] == 3, "vec3");
    Check(!P("[1,2]").ToVec3(v) && !P("[1,2,\"x\"]").ToVec3(v), "not a vec3");
    Check(o.Erase("n") && !o.Has("n") && !o.Erase("n"), "Erase");
    Check(P("[1,2,3]") == P("[1, 2, 3]") && !(P("[1,2,3]") == P("[1,2,4]")), "equality");
    Check(Json(7).is_integer() && !Json(7.5).is_integer() && Json(7.0).is_integer(), "is_integer");
    Check(Json(size_t(5)).AsInt() == 5 && Json(int64_t(-5)).AsInt() == -5 && Json(uint64_t(9)).AsInt() == 9, "integer types");
    Json a;
    a.Push(1);
    Check(a.is_array() && a.size() == 1, "Push makes an array");
}

// ---------------------------------------------------------------------------------------------------------------- the engine

struct FakeClock {
    double t = 100.0;
    double step = 0.0;
    double operator()() {
        const double v = t;
        t += step;
        return v;
    }
};

Posted Cmd(const std::string& cmd, const std::string& args = "{}", std::vector<Reply>* sink = nullptr, int64_t id = 0) {
    Posted p;
    p.cmd = cmd;
    p.args = P(args);
    p.source = Source::kAgent;
    p.client = 0;
    if (id != 0) {
        p.id = id;
        p.has_id = true;
    }
    if (sink) p.on_reply = [sink](const Reply& r) { sink->push_back(r); };
    return p;
}

void TestEngine() {
    FakeClock clock;
    Engine e([&clock] { return clock(); });
    Check(!e.Post(Cmd("ping")), "a disabled engine takes no commands");
    e.SetEnabled(true);

    Schema s;
    s.group = "test";
    s.summary = "echo";
    s.args = {{"x", ArgType::kNumber, true, ""}, {"name", ArgType::kString, false, ""}, {"v", ArgType::kVec3, false, ""}, {"t", ArgType::kTarget, false, ""}, {"n", ArgType::kInt, false, ""}};
    Check(e.Register("test.echo", [](const Request& r, const Responder& d) { Json o = Json::Object(); o.Set("x", r.args.Get("x")); d.Done(std::move(o)); }, s), "register");
    Check(!e.Register("test.echo", [](const Request&, const Responder&) {}, s), "a taken name is refused");
    Check(!e.Register("", [](const Request&, const Responder&) {}, s), "an empty name is refused");

    std::vector<Reply> got;
    e.Post(Cmd("test.echo", R"({"x":5})", &got, 7));
    e.Post(Cmd("nope.nothing", "{}", &got, 8));
    e.Post(Cmd("test.echo", "{}", &got, 9));
    e.Post(Cmd("test.echo", R"({"x":"five"})", &got, 10));
    e.Post(Cmd("test.echo", R"({"x":1,"bogus":2})", &got, 11));
    e.Post(Cmd("test.echo", R"({"x":1,"v":[1,2]})", &got, 12));
    e.Post(Cmd("test.echo", R"({"x":1,"t":[1,2,3]})", &got, 13));
    e.Post(Cmd("test.echo", R"({"x":1,"t":"car:p1"})", &got, 14));
    e.Post(Cmd("test.echo", R"({"x":1,"t":5})", &got, 15));
    e.Post(Cmd("test.echo", R"({"x":1,"n":2.5})", &got, 16));
    e.Post(Cmd("test.echo", R"({"x":1,"n":null,"name":null})", &got, 17));
    const auto st = e.RunQueued();
    Check(st.ran == 11 && st.left == 0, "all eleven ran");
    Check(got.size() == 11, "eleven replies");
    Check(got[0].ok && got[0].id == 7 && got[0].has_id && got[0].result.IntOr("x", 0) == 5, "a good command");
    Check(!got[1].ok && got[1].code == ErrorCode::kUnknownCmd, "unknown_cmd");
    Check(!got[2].ok && got[2].code == ErrorCode::kBadArgs && got[2].message.find("'x' is required") != std::string::npos, "a missing argument");
    Check(!got[3].ok && got[3].code == ErrorCode::kBadArgs && got[3].message.find("'x' must be a number") != std::string::npos, "a mistyped argument");
    Check(!got[4].ok && got[4].message.find("unknown argument 'bogus'") != std::string::npos, "an unknown argument");
    Check(!got[5].ok && got[5].message.find("'v'") != std::string::npos, "vec3 of two numbers");
    Check(got[6].ok && got[7].ok && !got[8].ok, "a target is a string or a vec3");
    Check(!got[9].ok && got[9].message.find("whole number") != std::string::npos, "int refuses 2.5");
    Check(got[10].ok, "null counts as absent for an optional argument");
    CheckEq(got[0].ToJson().Dump(), R"({"id":7,"ok":true,"result":{"x":5}})", "ok reply on the wire");
    CheckEq(got[1].ToJson().Dump(), R"J({"id":8,"ok":false,"error":{"code":"unknown_cmd","msg":"no such command 'nope.nothing' (help lists them)"}})J", "error reply on the wire");
    Reply noid;
    noid.ok = true;
    CheckEq(noid.ToJson().Dump(), R"({"ok":true,"result":{}})", "a reply without an id has none");
    Reply withdetails;
    withdetails.ok = false;
    withdetails.code = ErrorCode::kStale;
    withdetails.message = "m";
    withdetails.details = P(R"({"line":4,"actual":"x","code":"ignored"})");
    CheckEq(withdetails.ToJson().Dump(), R"({"ok":false,"error":{"code":"stale","msg":"m","line":4,"actual":"x"}})", "error details are merged, code and msg cannot be overridden");

    // handlers that misbehave
    e.Register("test.silent", [](const Request&, const Responder&) {}, Schema{});
    e.Register("test.throw", [](const Request&, const Responder&) { throw std::runtime_error("boom"); }, Schema{});
    e.Register("test.fail", [](const Request&, const Responder& r) { r.Fail(ErrorCode::kNoWorld, "in a menu"); r.Done(); }, Schema{});
    e.Register("test.nonobject", [](const Request&, const Responder& r) { r.Done(Json(5)); }, Schema{});
    got.clear();
    e.Post(Cmd("test.silent", "{}", &got));
    e.Post(Cmd("test.throw", "{}", &got));
    e.Post(Cmd("test.fail", "{}", &got));
    e.Post(Cmd("test.nonobject", "{}", &got));
    e.RunQueued();
    Check(!got[0].ok && got[0].code == ErrorCode::kFailed && got[0].message.find("no reply") != std::string::npos, "a handler that says nothing gets `failed`");
    Check(!got[1].ok && got[1].code == ErrorCode::kFailed && got[1].message.find("boom") != std::string::npos, "a handler that throws gets `failed`");
    Check(!got[2].ok && got[2].code == ErrorCode::kNoWorld, "the first answer wins");
    Check(got[3].ok && got[3].result.IntOr("value", 0) == 5, "a non-object result is wrapped");

    // deferred replies
    int polls = 0;
    e.Register("test.later", [&](const Request&, const Responder& r) {
        r.Defer([&polls](const Responder& d) {
            if (++polls >= 3) d.Done(P(R"({"done":true})"));
        });
    }, Schema{});
    got.clear();
    e.Post(Cmd("test.later", "{}", &got, 1));
    e.RunQueued();
    Check(got.empty() && e.deferred() == 1, "a deferred reply waits");
    e.PollDeferred();
    Check(got.empty(), "still waiting after one poll");
    e.PollDeferred();
    e.PollDeferred();
    Check(got.size() == 1 && got[0].ok && got[0].result.BoolOr("done", false) && e.deferred() == 0, "answered by the third poll");
    // a timeout
    e.Register("test.never", [](const Request&, const Responder& r) { r.Defer([](const Responder&) {}); }, Schema{});
    got.clear();
    Posted never = Cmd("test.never", "{}", &got, 2);
    never.timeout_s = 5.0;
    e.Post(std::move(never));
    e.RunQueued();
    e.PollDeferred();
    Check(got.empty(), "not yet timed out");
    clock.t += 6.0;
    e.PollDeferred();
    Check(got.size() == 1 && !got[0].ok && got[0].code == ErrorCode::kTimeout && got[0].has_id && got[0].id == 2, "a held reply times out");
    // a poll that throws
    e.Register("test.badpoll", [](const Request&, const Responder& r) { r.Defer([](const Responder&) { throw std::runtime_error("poll boom"); }); }, Schema{});
    got.clear();
    e.Post(Cmd("test.badpoll", "{}", &got));
    e.RunQueued();
    e.PollDeferred();
    Check(got.size() == 1 && got[0].code == ErrorCode::kFailed, "a poll that throws fails the reply");

    // the budget: a clock that moves 1 ms every look
    FakeClock slow;
    slow.step = 0.001;
    Engine b([&slow] { return slow(); });
    b.SetEnabled(true);
    int ran = 0;
    b.Register("t.n", [&](const Request&, const Responder& r) { ++ran; r.Done(); }, Schema{});
    for (int i = 0; i < 40; ++i) b.Post(Cmd("t.n"));
    const auto s1 = b.RunQueued(16, 2.0);
    Check(s1.ran >= 1 && s1.ran <= 3 && s1.left == static_cast<size_t>(40 - s1.ran), "the time budget stops the drain");
    const auto s2 = b.RunQueued(16, 1000.0);
    Check(s2.ran == 16, "the count limit stops the drain");
    // the queue limit
    Engine q;
    q.SetEnabled(true);
    q.Register("t.n", [](const Request&, const Responder& r) { r.Done(); }, Schema{});
    size_t accepted = 0;
    for (size_t i = 0; i < kMaxQueued + 10; ++i) accepted += q.Post(Cmd("t.n")) ? 1 : 0;
    Check(accepted == kMaxQueued, "the queue holds kMaxQueued");
    std::vector<Reply> dropped;
    Engine d2;
    d2.SetEnabled(true);
    d2.Post(Cmd("ping", "{}", &dropped));
    d2.SetEnabled(false);
    Check(dropped.size() == 1 && dropped[0].code == ErrorCode::kRefused, "switching off answers the queued commands");

    // help and ping
    got.clear();
    e.SetPingFields([](Json& j) { j.Set("frame", 12); });
    e.Post(Cmd("help", "{}", &got));
    e.Post(Cmd("help", R"({"cmd":"test.echo"})", &got));
    e.Post(Cmd("help", R"({"cmd":"zzz"})", &got));
    e.Post(Cmd("ping", "{}", &got));
    e.RunQueued();
    Check(got[0].ok && got[0].result.IntOr("proto", 0) == 1 && got[0].result.Get("commands").size() >= 5 && got[0].result.Get("errors").size() == 8, "help lists everything");
    Check(got[1].ok && got[1].result.Get("commands").size() == 1 && got[1].result.Get("commands").at(0).Get("args").size() == 5, "help of one command with its arguments");
    CheckEq(got[1].result.Get("commands").at(0).Get("args").at(2).StringOr("type", ""), "vec3", "help names the type");
    Check(!got[2].ok && got[2].code == ErrorCode::kUnknownCmd, "help of an unknown command");
    Check(got[3].ok && got[3].result.BoolOr("pong", false) && got[3].result.IntOr("frame", 0) == 12, "ping with the game's fields");
    Check(!got[3].result.Has("x"), "");

    // batch
    got.clear();
    e.Post(Cmd("batch", R"({"cmds":[{"cmd":"test.echo","args":{"x":1}},{"cmd":"test.echo","args":{"x":2}}]})", &got));
    e.Post(Cmd("batch", R"({"cmds":[{"cmd":"test.echo","args":{"x":1}},{"cmd":"nope"},{"cmd":"test.echo","args":{"x":3}}]})", &got));
    e.Post(Cmd("batch", R"({"cmds":[{"cmd":"test.echo","args":{"x":1}},{"cmd":"nope"},{"cmd":"test.echo","args":{"x":3}}],"stop_on_error":false})", &got));
    e.Post(Cmd("batch", R"({"cmds":[]})", &got));
    e.Post(Cmd("batch", R"({"cmds":[{"cmd":"batch","args":{}}]})", &got));
    e.Post(Cmd("batch", R"({"cmds":[5]})", &got));
    e.RunQueued();
    Check(got[0].ok && got[0].result.Get("results").size() == 2 && got[0].result.Get("results").at(1).Get("result").IntOr("x", 0) == 2, "batch runs both");
    Check(got[1].ok && got[1].result.Get("results").size() == 2 && !got[1].result.Get("results").at(1).BoolOr("ok", true), "batch stops at the first error");
    Check(got[2].ok && got[2].result.Get("results").size() == 3 && got[2].result.Get("results").at(2).BoolOr("ok", false), "batch goes on when asked");
    CheckEq(got[2].result.Get("results").at(1).Get("error").StringOr("code", ""), "unknown_cmd", "a sub-reply carries its error");
    Check(!got[3].ok && !got[4].ok && !got[5].ok, "batch refuses an empty list, a nested batch and a non-object");
    // batch with a deferred sub-command
    got.clear();
    polls = 0;
    e.Post(Cmd("batch", R"({"cmds":[{"cmd":"test.echo","args":{"x":1}},{"cmd":"test.later"},{"cmd":"test.echo","args":{"x":9}}]})", &got, 3));
    e.RunQueued();
    Check(got.empty(), "a batch waits for its held command");
    for (int i = 0; i < 4; ++i) e.PollDeferred();
    Check(got.size() == 1 && got[0].ok && got[0].result.Get("results").size() == 3 && got[0].result.Get("results").at(2).Get("result").IntOr("x", 0) == 9, "and goes on after it");

    // events
    std::vector<std::string> seen;
    const int token = e.AddListener([&](std::string_view name, const Json& ev) { seen.push_back(std::string(name) + ":" + ev.Dump()); });
    std::vector<std::pair<std::string, std::string>> lines;
    e.SetEventSink([&](const std::string& line, std::string_view name) { lines.emplace_back(line, std::string(name)); });
    e.Publish("scene.reloaded", P(R"({"rev":3,"by":"panel","event":"ignored"})"));
    Check(lines.size() == 1 && lines[0].first == R"({"event":"scene.reloaded","rev":3,"by":"panel"})" && lines[0].second == "scene.reloaded", "an event line");
    Check(seen.size() == 1 && seen[0] == R"(scene.reloaded:{"event":"scene.reloaded","rev":3,"by":"panel"})", "a listener gets the whole event");
    e.RemoveListener(token);
    e.Publish("x.y");
    Check(seen.size() == 1 && lines.size() == 2, "a removed listener hears nothing");
    // per-client filters
    got.clear();
    Check(e.ShouldSend(0, "car.ready") && e.ShouldSend(-1, "car.ready"), "events go to everyone by default");
    e.Post(Cmd("events.set", R"({"only":["car.ready"]})", &got));
    e.RunQueued();
    Check(got[0].ok && e.ShouldSend(0, "car.ready") && !e.ShouldSend(0, "scene.reloaded") && e.ShouldSend(1, "scene.reloaded"), "events.set only");
    e.Post(Cmd("events.set", R"({"on":false})", &got));
    e.RunQueued();
    Check(!e.ShouldSend(0, "car.ready"), "events.set off mutes");
    e.Post(Cmd("events.set", R"({"on":true,"only":[]})", &got));
    e.RunQueued();
    Check(e.ShouldSend(0, "scene.reloaded"), "events.set back to all");

    // the log of recent commands: quiet ones stay out, errors carry their reason
    Engine l;
    l.SetEnabled(true);
    Schema open;
    open.open_args = true;
    l.Register("t.go", [](const Request&, const Responder& r) { r.Done(); }, open);
    l.Register("t.get", [](const Request&, const Responder& r) { r.Done(); }, Schema{});
    l.Post(Cmd("t.go", R"({"pos":[1,2,3],"yaw":80})"));
    l.Post(Cmd("t.get"));
    l.Post(Cmd("t.missing"));
    l.RunQueued();
    const auto recent = l.Recent(10);
    Check(recent.size() == 2, "recent commands: two entries, got " + std::to_string(recent.size()));
    if (recent.size() == 2) {
        CheckEq(recent[0].line, "t.go pos=1,2,3 yaw=80", "recent command line");
        Check(recent[0].ok && !recent[1].ok && recent[1].error.find("unknown_cmd") == 0,
              "recent commands: ok and failed: " + std::to_string(recent[0].ok) + std::to_string(recent[1].ok) + " '" + recent[1].error + "'");
    }
}

// ---------------------------------------------------------------------------------------------------------------- the console grammar

void TestConsole() {
    Schema s;
    s.args = {{"t", ArgType::kNumber, true, ""}, {"settle_frames", ArgType::kInt, false, ""}};
    RegisterCommand("test.seek", [](const Request&, const Responder& r) { r.Done(); }, s);
    std::string cmd, err;
    Json args;
    Check(ParseConsoleLine("camera.set pos=819.6,19,-800 yaw=80 fov=40", &cmd, &args, &err) && cmd == "camera.set", "parse a console line");
    CheckEq(args.Dump(), R"({"pos":[819.6,19,-800],"yaw":80,"fov":40})", "a comma list of numbers is an array");
    Check(ParseConsoleLine("test.seek 3.2", &cmd, &args, &err) && args.Dump() == R"({"t":3.2})", "a bare value is the schema's first argument");
    Check(ParseConsoleLine("test.seek settle_frames=5 3.2", &cmd, &args, &err) && args.Dump() == R"({"settle_frames":5,"t":3.2})", "a bare value skips an argument given by name");
    Check(!ParseConsoleLine("test.seek 1 2 3", &cmd, &args, &err) && err.find("too many") != std::string::npos, "too many bare values");
    Check(!ParseConsoleLine("unknown.cmd 1", &cmd, &args, &err), "a bare value for an unknown command");
    Check(ParseConsoleLine("unknown.cmd a=1", &cmd, &args, &err) && args.Dump() == R"({"a":1})", "key=value works for any command");
    Check(ParseConsoleLine(R"(car.spawn id=p1 model=vp_lam_murcielago_06 at=[1, 2, 3] heading=62 name="a b" on=true off=false nothing=null)", &cmd, &args, &err), "a mixed line");
    CheckEq(args.Dump(), R"({"id":"p1","model":"vp_lam_murcielago_06","at":[1,2,3],"heading":62,"name":"a b","on":true,"off":false,"nothing":null})", "values of every kind");
    Check(ParseConsoleLine(R"(x.y s="12" z=spot:gate@0,2,-6 o={"a":[1,2]} e= n=-0.5e2)", &cmd, &args, &err), "quoted numbers, bare words with commas, objects, empty");
    CheckEq(args.Dump(), R"({"s":"12","z":"spot:gate@0,2,-6","o":{"a":[1,2]},"e":"","n":-50})", "...read as");
    Check(!ParseConsoleLine("", &cmd, &args, &err) && !ParseConsoleLine("Camera.set", &cmd, &args, &err) && !ParseConsoleLine("x.y a=\"open", &cmd, &args, &err) && !ParseConsoleLine("x.y a=[1,2", &cmd, &args, &err) &&
              !ParseConsoleLine("x.y a={bad}", &cmd, &args, &err),
          "bad console lines");
    // the inverse, and the round trip
    const char* samples[] = {R"({"pos":[819.6,19,-800],"yaw":80})", R"({"s":"12","b":true,"n":null,"o":{"a":[1,2]},"w":"a b","e":"","p":"routes/a.route","x":[5],"y":[],"z":["a","b"]})",
                             R"({"t":"car:p1","at":"spot:gate@0,2,-6","name":"x\"y","f":1.5e-7,"neg":-3})"};
    for (const char* sample : samples) {
        const Json in = P(sample);
        const std::string line = FormatConsoleLine("t.go", in);
        std::string c2;
        Json out;
        Check(ParseConsoleLine(line, &c2, &out, &err) && c2 == "t.go" && out == in, std::string("console round trip: ") + line + (err.empty() ? "" : " " + err));
    }
    CheckEq(FormatConsoleLine("a.b", Json::Object()), "a.b", "no arguments");
    CheckEq(FormatConsoleLine("a.b", P(R"({"n":"1.5","m":"true"})")), R"(a.b n="1.5" m="true")", "a string that reads as a number or a bool is quoted");
}

// ---------------------------------------------------------------------------------------------------------------- the wire

void TestWire() {
    Posted p;
    std::string err;
    int64_t id = 0;
    bool has_id = false;
    Check(ParseRequest(P(R"({"id":7,"cmd":"camera.set","args":{"fov":40},"wait":"settled","timeout_s":12.5})"), &p, &err, &id, &has_id), "a full request");
    Check(p.id == 7 && p.has_id && p.cmd == "camera.set" && p.args.IntOr("fov", 0) == 40 && p.wait_settled && p.timeout_s == 12.5 && p.source == Source::kAgent, "its fields");
    Check(ParseRequest(P(R"({"cmd":"ping"})"), &p, &err, &id, &has_id) && !p.has_id && p.args.is_object() && !p.wait_settled, "a bare request");
    Check(ParseRequest(P(R"({"cmd":"ping","args":null,"id":null})"), &p, &err, &id, &has_id) && !p.has_id, "null args and id");
    struct Bad {
        const char* json;
        const char* part;
    } bad[] = {{R"([1])", "JSON object"},
               {R"({"id":1.5,"cmd":"x"})", "'id'"},
               {R"({"id":"a","cmd":"x"})", "'id'"},
               {R"({"id":1})", "'cmd'"},
               {R"({"id":1,"cmd":""})", "'cmd'"},
               {R"({"cmd":5})", "'cmd'"},
               {R"({"cmd":"x","args":[1]})", "'args'"},
               {R"({"cmd":"x","wait":"now"})", "'wait'"},
               {R"({"cmd":"x","timeout_s":0})", "'timeout_s'"},
               {R"({"cmd":"x","timeout_s":99999})", "'timeout_s'"},
               {R"({"cmd":"x","timeout_s":"a"})", "'timeout_s'"}};
    for (const Bad& b : bad) {
        err.clear();
        const bool ok = ParseRequest(P(b.json), &p, &err, &id, &has_id);
        Check(!ok && err.find(b.part) != std::string::npos, std::string("a bad request: ") + b.json + " -> " + err);
    }
    ParseRequest(P(R"({"id":9,"cmd":5})"), &p, &err, &id, &has_id);
    Check(has_id && id == 9, "the id is known even when the request is bad");
    CheckEq(EventLine("car.ready", P(R"({"id":"p1"})")), R"({"event":"car.ready","id":"p1"})", "an event line");
    Reply r;
    r.ok = true;
    r.has_id = true;
    r.id = 3;
    CheckEq(ReplyLine(r), R"({"id":3,"ok":true,"result":{}})", "a reply line");
    Check(IsQuietCommand("ping") && IsQuietCommand("cars.list") && IsQuietCommand("scene.get") && !IsQuietCommand("camera.set"), "quiet commands");
    // names
    CheckEq(ErrorCodeName(ErrorCode::kBadArgs), "bad_args", "error code names");
    ErrorCode c;
    Check(ErrorCodeFromName("not_ready", &c) && c == ErrorCode::kNotReady && !ErrorCodeFromName("nope", &c), "error code lookup");
    // the schema check on its own
    Schema s;
    s.args = {{"a", ArgType::kString, true, ""}};
    Check(ValidateArgs(s, P("{}"), "x.y") == "x.y: 'a' is required" && ValidateArgs(s, P(R"({"a":"z"})"), "x.y").empty() && !ValidateArgs(s, P("[]")).empty(), "ValidateArgs");
    s.open_args = true;
    Check(ValidateArgs(s, P(R"({"a":"z","other":1})")).empty(), "open_args lets other arguments through");
}

// ---------------------------------------------------------------------------------------------------------------- maths and paths

void TestMath() {
    // projection: a camera at the origin looking along +Z (yaw 0), 90 degrees vertical, aspect 2
    SnapPose pose;
    pose.fov = 90;
    double vp[16];
    ViewProjection(pose, 2.0, 0.5, 100.0, vp);
    double nx, ny, depth, w;
    double pt[3] = {0, 0, 10};
    Check(ProjectPoint(vp, pt, &nx, &ny, &depth, &w), "a point ahead projects");
    CheckNum(nx, 0.0, "centre x");
    CheckNum(ny, 0.0, "centre y");
    CheckNum(w, 10.0, "w is the distance along the view");
    CheckNum(depth, (100.0 / 99.5) * (10.0 - 0.5) / 10.0, "depth, direct3d clip", 1e-9);
    double up_pt[3] = {0, 10, 10};  // 45 degrees up with a 90 degree vertical field of view: the top edge
    ProjectPoint(vp, up_pt, &nx, &ny, &depth, &w);
    CheckNum(ny, 1.0, "the top edge at half the field of view");
    // the right of a camera that looks along +Z is -X in this right-handed world
    double left_pt[3] = {-20, 0, 10};  // tan(half hfov) = 1 * aspect 2: x = 20 is the right edge
    ProjectPoint(vp, left_pt, &nx, &ny, &depth, &w);
    CheckNum(nx, 1.0, "the right edge (the camera's right is -X when it looks along +Z)");
    double behind[3] = {0, 0, -5};
    Check(!ProjectPoint(vp, behind, &nx, &ny, &depth, &w) && w < 0, "a point behind is not in the picture");
    // the same camera turned to look along +X (yaw 90): the old +X point is dead ahead
    pose.yaw = 90;
    ViewProjection(pose, 2.0, 0.5, 100.0, vp);
    double ahead[3] = {10, 0, 0};
    ProjectPoint(vp, ahead, &nx, &ny, &depth, &w);
    CheckNum(nx, 0.0, "yaw 90 looks along +X");
    CheckNum(w, 10.0, "..at distance 10");
    // a moved camera
    pose.pos[0] = 100, pose.pos[1] = 5, pose.pos[2] = -3;
    pose.yaw = 0;
    ViewProjection(pose, 16.0 / 9.0, 0.1, 1000.0, vp);
    double target[3] = {100, 5, 17};
    ProjectPoint(vp, target, &nx, &ny, &depth, &w);
    CheckNum(nx, 0.0, "a moved camera, x");
    CheckNum(w, 20.0, "a moved camera, w");
    // axes and angles round trip
    SnapPose p2;
    p2.pos[0] = 1, p2.pos[1] = 2, p2.pos[2] = 3;
    p2.yaw = 37, p2.pitch = -12, p2.roll = 8, p2.fov = 55;
    double r[3], u[3], f[3];
    PoseAxes(p2, r, u, f);
    SnapPose back;
    PoseFromAxes(p2.pos, r, u, f, p2.fov, 0.0, &back);
    CheckNum(back.yaw, 37, "yaw round trip", 1e-6);
    CheckNum(back.pitch, -12, "pitch round trip", 1e-6);
    CheckNum(back.roll, 8, "roll round trip", 1e-6);
    // look at
    double from[3] = {0, 0, 0}, to[3] = {10, 0, 0}, yaw, pitch;
    LookAtAngles(from, to, &yaw, &pitch);
    CheckNum(yaw, 90, "look along +X");
    CheckNum(pitch, 0, "level");
    double to2[3] = {0, 10, 10};
    LookAtAngles(from, to2, &yaw, &pitch);
    CheckNum(yaw, 0, "look along +Z");
    CheckNum(pitch, 45, "and up 45");
    LookAtAngles(from, from, &yaw, &pitch);
    CheckNum(yaw, 0, "the same point");

    // time
    const ParseResult pr = ParseScene("scene a\nat 2 timescale 0.5\nat 6 timescale 1\nend 10\n");
    CheckNum(WorldTimeAt(pr.scene, 1.0), 1.0, "before a slow-down");
    CheckNum(WorldTimeAt(pr.scene, 6.0), 2.0 + 2.0, "2 s at x1 then 4 s at x0.5", 0.01);
    CheckNum(WorldTimeAt(pr.scene, 8.0), 2.0 + 2.0 + 2.0, "and 2 s at x1 again", 0.01);
    const ParseResult plain = ParseScene("scene a\nend 10\n");
    CheckNum(WorldTimeAt(plain.scene, 7.25), 7.25, "no time scale track: the scene's own time");
    CheckNum(WorldTimeAt(plain.scene, -1), 0.0, "negative time");
    // scrub
    const ParseResult exact = ParseScene("scene a\nworld traffic off peds off\nat 0 car p1 path a.route\nend 10\n");
    Check(ScrubbableAt(exact.scene, 3).scrubbable, "traffic and peds off, kinematic car: exact");
    const ParseResult brain = ParseScene("scene a\nworld traffic off peds off\nat 0 car p1 path a.route mode brain\nend 10\n");
    ScrubInfo si = ScrubbableAt(brain.scene, 3);
    Check(!si.scrubbable && si.hint.find("mode brain") != std::string::npos, "a brain car cannot be rewound");
    si = ScrubbableAt(plain.scene, 3);
    Check(!si.scrubbable && si.hint.find("traffic") != std::string::npos && si.hint.find("peds") != std::string::npos, "traffic and peds not off");

    // routes
    std::vector<WayPoint> pts;
    for (int i = 0; i <= 100; ++i) pts.push_back({double(i), 0.0, 0.0, 14.0});  // a straight line of 101 points
    std::vector<WayPoint> red = ReduceRoute(pts, 0.25);
    Check(red.size() == 2 && red.front().x == 0 && red.back().x == 100, "a straight drive reduces to its two ends");
    pts.clear();
    for (int i = 0; i <= 50; ++i) pts.push_back({double(i), 0.0, 0.0, 14.0});
    for (int i = 1; i <= 50; ++i) pts.push_back({50.0, 0.0, double(i), 14.0});  // a corner
    red = ReduceRoute(pts, 0.25);
    Check(red.size() == 3 && red[1].x == 50 && red[1].z == 0, "a corner stays");
    pts.clear();
    for (int i = 0; i <= 20; ++i) pts.push_back({double(i), 0.0, 0.0, i < 10 ? 10.0 : 25.0});
    red = ReduceRoute(pts, 0.25, 2.0);
    Check(red.size() >= 3, "a change of speed keeps a point");
    CheckNum(RouteLength(pts), 20.0, "route length");
    pts = {{0, 17.5, 0, 10}, {10, 17.5, 5, 12.5}, {20, 18, 5, 14}};
    const std::string file = FormatRouteFile(pts, "a recorded drive\nhash abc");
    Route route;
    std::string perr;
    Check(ParseRoute(file, &route, &perr) && route.points.size() == 3 && route.has_y, "the route file reads back through path_follow's parser: " + perr);
    Check(file.find("# a recorded drive\n# hash abc\n") == 0, "header lines are comments");
    CheckNum(route.points[1].x, 10, "x");
    CheckNum(route.points[1].z, 5, "z");
    CheckNum(route.points[1].speed, 12.5, "speed");
    CheckNum(route.points[2].y, 18, "y");

    // paths
    const struct {
        const char* path;
        bool ok;
        const char* norm;
    } paths[] = {{"shots/a.png", true, "shots/a.png"},   {"shots\\a.png", true, "shots/a.png"},   {"a.png", true, "a.png"},        {"a//b///c.png", true, "a/b/c.png"},
                 {"", false, ""},                        {"/etc/passwd", false, ""},             {"\\x", false, ""},              {"../x.png", false, ""},
                 {"a/../b", false, ""},                  {"./a", false, ""},                     {"C:/x", false, ""},             {"C:x", false, ""},
                 {"a:b", false, ""},                     {"a/b?.png", false, ""},                {"nul", false, ""},              {"shots/CON.png", false, ""},
                 {"a/com1", false, ""},                  {"x./y", false, ""},                    {"x /y", false, ""},             {"a\x01", false, ""},
                 {"//", false, ""},                      {"a/b/", true, "a/b"}};
    for (const auto& c : paths) {
        const PathCheck pc = CheckRelativePath(c.path);
        Check(pc.ok == c.ok && (!c.ok || pc.normalised == c.norm), std::string("path '") + c.path + "': " + (pc.ok ? pc.normalised : pc.error));
    }
    Check(!CheckRelativePath(std::string(201, 'a')).ok && CheckRelativePath(std::string(200, 'a')).ok, "path length limit");

    // the scene as rows
    const char* text = "scene a\n# c\nat 0 car p1 spawn x at 1,2,3 # note\ncam 1 pos 1 2 3\ncut 2\nat 3 hour 12\nend 10\n";
    const ParseResult sc = ParseScene(text);
    const auto rows = TimelineEvents(sc.scene, text);
    Check(sc.Ok() && rows.size() >= 4, "scene rows");
    bool found_car = false, found_cut = false, found_cam = false, found_hour = false;
    for (const SnapEvent& e : rows) {
        if (e.kind == EventKind::kCar && e.car == "p1" && e.text == "at 0 car p1 spawn x at 1,2,3" && e.line == 3) found_car = true;
        if (e.kind == EventKind::kCut && e.line == 5) found_cut = true;
        if (e.kind == EventKind::kCamera && e.line == 4 && e.t == 1.0) found_cam = true;
        if (e.kind == EventKind::kWorld && e.line == 6) found_hour = true;
    }
    Check(found_car && found_cut && found_cam && found_hour, "rows have kind, line, car and the statement without its comment");
    const auto ids = SceneCarIds(sc.scene);
    Check(ids.size() == 1 && ids[0] == "p1", "car ids");
    CheckEq(StatementText(text, 3), "at 0 car p1 spawn x at 1,2,3", "StatementText");
    CheckEq(StatementText(text, 99), "", "a line past the end");
    CheckEq(StatementText(text, 0), "", "line 0");
    CheckEq(StripComment("  cam 1 pos 1 2 3   # hi "), "cam 1 pos 1 2 3", "StripComment");
    CheckEq(StripComment("at 1 label \"a # b\" # c"), "at 1 label \"a # b\"", "StripComment keeps a quoted #");
}

// ---------------------------------------------------------------------------------------------------------------- PNG

void TestPng() {
    // a 3x2 picture, BGRA: red, green, blue / white, black, grey
    const uint8_t px[] = {0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255, 255, 255, 255, 255, 0, 0, 0, 255, 128, 128, 128, 255};
    std::vector<uint8_t> png;
    Check(EncodePng(px, 3, 2, 0, true, &png), "encode");
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgb;
    Check(DecodeStoredPng(png, &w, &h, &rgb) && w == 3 && h == 2 && rgb.size() == 18, "decode what was encoded");
    Check(rgb[0] == 255 && rgb[1] == 0 && rgb[2] == 0 && rgb[3] == 0 && rgb[4] == 255 && rgb[6] == 0 && rgb[8] == 255 && rgb[9] == 255 && rgb[12] == 0 && rgb[15] == 128, "BGRA became RGB");
    Check(png[0] == 0x89 && png[1] == 'P' && std::memcmp(&png[png.size() - 8], "IEND", 4) == 0, "signature and IEND");
    std::vector<uint8_t> png2;
    Check(EncodePng(px, 3, 2, 0, false, &png2) && DecodeStoredPng(png2, &w, &h, &rgb) && rgb[0] == 0 && rgb[2] == 255, "RGBA order");
    // known values of the checksums
    const uint8_t abc[] = {'a', 'b', 'c'};
    Check(Crc32(abc, 3) == 0x352441C2u, "crc32(abc)");
    Check(Adler32(abc, 3) == 0x024D0127u, "adler32(abc)");
    // a bigger picture with a stride and several stored blocks
    const uint32_t W = 300, H = 100;
    std::vector<uint8_t> big(static_cast<size_t>(W) * H * 4 + H * 16);
    const size_t stride = W * 4 + 16;
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            uint8_t* p = &big[y * stride + x * 4];
            p[0] = static_cast<uint8_t>(x), p[1] = static_cast<uint8_t>(y), p[2] = static_cast<uint8_t>(x + y), p[3] = 255;
        }
    }
    Check(EncodePng(big.data(), W, H, stride, true, &png) && DecodeStoredPng(png, &w, &h, &rgb) && w == W && h == H, "a bigger picture with a stride");
    bool same = true;
    for (uint32_t y = 0; y < H && same; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            const uint8_t* s = &big[y * stride + x * 4];
            const uint8_t* d = &rgb[(y * W + x) * 3];
            if (d[0] != s[2] || d[1] != s[1] || d[2] != s[0]) same = false;
        }
    }
    Check(same, "every pixel survives (more than one stored block)");
    Check(!EncodePng(px, 0, 2, 0, true, &png) && !EncodePng(px, 3, 0, 0, true, &png) && !EncodePng(nullptr, 3, 2, 0, true, &png) && !EncodePng(px, 20000, 2, 0, true, &png) && !EncodePng(px, 3, 2, 8, true, &png),
          "nonsense sizes are refused");
    std::vector<uint8_t> broken = png2;
    broken[broken.size() - 20] ^= 0x55;
    Check(!DecodeStoredPng(broken, &w, &h, &rgb), "a corrupted file does not decode");
    // the file
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "larecomp_director_test_png";
    std::filesystem::remove_all(dir);
    std::string err;
    const std::string path = (dir / "sub" / "a.png").string();
    const bool wrote1 = WritePngFile(path, px, 3, 2, 0, true, &err);
    Check(wrote1, "write the file: " + err);
    std::vector<uint8_t> disk;
    {
        std::ifstream in(path, std::ios::binary);
        disk.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    Check(DecodeStoredPng(disk, &w, &h, &rgb), "the file on disk decodes");
    Check(!std::filesystem::exists(path + ".tmp"), "no temporary file is left");
    const bool wrote2 = WritePngFile(path, px, 3, 2, 0, false, &err);
    Check(wrote2, "overwrite it: " + err);
    const bool wrote3 = WritePngFile(path, px, 0, 2, 0, false, &err);
    Check(!wrote3 && !err.empty(), "a refused picture says why");
    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------------------------------------------- sockets

#if defined(_WIN32)
using sock_t = SOCKET;
constexpr sock_t kBad = INVALID_SOCKET;
void CloseSock(sock_t s) { closesocket(s); }
#else
using sock_t = int;
constexpr sock_t kBad = -1;
void CloseSock(sock_t s) { close(s); }
#endif

struct TestClient {
    sock_t s = kBad;
    std::string buf;
    bool Connect(uint16_t port) {
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) return false;
        // Two requests sent one after the other must reach the server as soon as they are sent: with Nagle's algorithm (the default) the second small
        // segment waits for the ACK of the first, which on Linux the server's delayed ACK holds back for about 5 ms, and the case "a held reply comes after a
        // later fast one" then saw the held reply (5 polls = 5 ms) first on every run. Windows' loopback does not do this; a real client may set it too.
        {
            int one = 1;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
        }
#if defined(_WIN32)
        DWORD to = 3000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
#else
        timeval to{3, 0};
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
#endif
        return true;
    }
    void Send(const std::string& line) {
        const std::string l = line + "\n";
        send(s, l.data(), static_cast<int>(l.size()), 0);
    }
    // One line, or "" when nothing came in the receive timeout or the peer closed (`closed` says which).
    bool closed = false;
    std::string Line() {
        while (true) {
            const size_t nl = buf.find('\n');
            if (nl != std::string::npos) {
                std::string l = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                return l;
            }
            char tmp[4096];
            const int n = static_cast<int>(recv(s, tmp, sizeof(tmp), 0));
            if (n <= 0) {
                closed = n == 0;
                return "";
            }
            buf.append(tmp, static_cast<size_t>(n));
        }
    }
    // The next line that is not an event (agent connect events and the like come in between); "" on a timeout.
    std::string Reply() {
        while (true) {
            const std::string l = Line();
            if (l.empty()) return l;
            if (l.find("\"event\"") == std::string::npos) return l;
        }
    }
    // Waits for the peer to close (true) or for the timeout (false).
    bool WaitClosed() {
        for (int i = 0; i < 20; ++i) {
            char tmp[4096];
            const int n = static_cast<int>(recv(s, tmp, sizeof(tmp), 0));
            if (n == 0) return true;
            if (n < 0) {
#if defined(_WIN32)
                const int e = WSAGetLastError();
                if (e == WSAECONNRESET || e == WSAECONNABORTED) return true;
#endif
            }
        }
        return false;
    }
    ~TestClient() {
        if (s != kBad) CloseSock(s);
    }
};

// What the "game thread" does while a test talks to the server.
struct Pump {
    Engine& e;
    std::atomic<bool> stop{false};
    std::thread t;
    explicit Pump(Engine& engine) : e(engine) {
        t = std::thread([this] {
            while (!stop.load()) {
                e.RunQueued();
                e.PollDeferred();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }
    ~Pump() {
        stop = true;
        t.join();
    }
};

void TestServer() {
    Engine engine;
    engine.SetEnabled(true);
    Schema es;
    es.args = {{"x", ArgType::kNumber, false, ""}};
    engine.Register("t.echo", [](const Request& r, const Responder& d) {
        Json o = Json::Object();
        o.Set("x", r.args.Get("x"));
        o.Set("client", r.client);
        o.Set("source", SourceName(r.source));
        d.Done(std::move(o));
    }, es);
    std::atomic<int> polls{0};
    engine.Register("t.later", [&polls](const Request&, const Responder& r) {
        polls = 0;
        r.Defer([&polls](const Responder& d) {
            if (++polls >= 5) d.Done(P(R"({"late":true})"));
        });
    }, Schema{});
    const std::string token = MakeToken();
    Check(token.size() == 32 && token != MakeToken(), "a token is 32 hex digits and different every time");
    LineServer server(engine, token);
    server.SetHello([] {
        Json h = Json::Object();
        h.Set("version", "test");
        return h;
    });
    std::string err;
    Check(server.Start(0, &err) && server.port() != 0 && server.running(), "the server starts on a free port: " + err);
    const uint16_t port = server.port();
    {
        Pump pump(engine);

        // not authenticated: a request first
        {
            TestClient c;
            Check(c.Connect(port), "connect");
            c.Send(R"({"id":1,"cmd":"ping"})");
            const Json r = P(c.Line());
            Check(!r.BoolOr("ok", true) && r.Get("error").StringOr("code", "") == "refused" && r.IntOr("id", 0) == 1, "a request before auth is refused");
            Check(c.WaitClosed(), "..and the connection is closed");
        }
        // a wrong token
        {
            TestClient c;
            c.Connect(port);
            c.Send(R"({"id":2,"cmd":"auth","token":"nope"})");
            const Json r = P(c.Line());
            Check(!r.BoolOr("ok", true) && r.Get("error").StringOr("code", "") == "refused", "a wrong token is refused");
            Check(c.WaitClosed(), "..and closed");
        }
        // bad JSON before auth closes too
        {
            TestClient c;
            c.Connect(port);
            c.Send("this is not json");
            const Json r = P(c.Line());
            Check(!r.BoolOr("ok", true) && r.Get("error").StringOr("code", "") == "bad_args", "bad JSON gets bad_args");
            Check(c.WaitClosed(), "..and an unauthenticated connection is closed");
        }
        // a good session
        TestClient a;
        Check(a.Connect(port), "connect again");
        a.Send(R"({"id":10,"cmd":"auth","token":")" + token + R"(","name":"tester"})");
        Json r = P(a.Reply());
        Check(r.BoolOr("ok", false) && r.IntOr("id", 0) == 10 && r.Get("result").IntOr("proto", 0) == 1 && r.Get("result").StringOr("version", "") == "test" && r.Get("result").IntOr("client", -1) == 0, "auth ok");
        a.Send(R"({"id":11,"cmd":"t.echo","args":{"x":42}})");
        r = P(a.Reply());
        Check(r.BoolOr("ok", false) && r.IntOr("id", 0) == 11 && r.Get("result").IntOr("x", 0) == 42 && r.Get("result").StringOr("source", "") == "agent" && r.Get("result").IntOr("client", -1) == 0, "a command and its reply");
        a.Send(R"({"cmd":"t.echo","args":{"x":1}})");
        r = P(a.Reply());
        Check(r.BoolOr("ok", false) && !r.Has("id"), "no id in, no id out");
        a.Send("{not json");
        r = P(a.Reply());
        Check(!r.BoolOr("ok", true) && r.Get("error").StringOr("code", "") == "bad_args", "bad JSON after auth: bad_args");
        a.Send(R"({"id":12,"cmd":"t.echo","args":{"x":"five"}})");
        r = P(a.Reply());
        Check(!r.BoolOr("ok", true) && r.Get("error").StringOr("code", "") == "bad_args" && r.IntOr("id", 0) == 12, "a bad argument");
        a.Send(R"({"id":13,"cmd":"nope"})");
        r = P(a.Reply());
        Check(r.Get("error").StringOr("code", "") == "unknown_cmd", "an unknown command");
        a.Send(R"({"id":14,"cmd":"auth","token":")" + token + R"("})");
        r = P(a.Reply());
        Check(r.Get("error").StringOr("code", "") == "refused", "auth twice is refused");
        a.Send(R"({"id":15,"cmd":5})");
        r = P(a.Reply());
        Check(r.Get("error").StringOr("code", "") == "bad_args" && r.IntOr("id", 0) == 15, "a bad request keeps its id");
        // the wire is not the order of finishing: a slow one and a fast one
        a.Send(R"({"id":20,"cmd":"t.later"})");
        a.Send(R"({"id":21,"cmd":"t.echo","args":{"x":3}})");
        const Json first = P(a.Reply());
        const Json second = P(a.Reply());
        Check(first.IntOr("id", 0) == 21 && second.IntOr("id", 0) == 20 && second.Get("result").BoolOr("late", false), "a held reply comes after a later fast one");
        // events: to this client, filtered by events.set
        engine.Publish("scene.reloaded", P(R"({"rev":2,"by":"panel"})"));
        r = P(a.Line());
        Check(r.StringOr("event", "") == "scene.reloaded" && r.IntOr("rev", 0) == 2 && !r.Has("id"), "an event reaches the client");
        a.Send(R"({"id":30,"cmd":"events.set","args":{"only":["car.ready"]}})");
        r = P(a.Reply());
        Check(r.BoolOr("ok", false), "events.set");
        engine.Publish("scene.reloaded", P(R"({"rev":3})"));
        engine.Publish("car.ready", P(R"({"id":"p1"})"));
        r = P(a.Line());
        Check(r.StringOr("event", "") == "car.ready", "the filter lets only car.ready through");
        // more clients: four in all, the fifth is refused
        std::vector<std::unique_ptr<TestClient>> more;
        for (int i = 0; i < 3; ++i) {
            more.push_back(std::make_unique<TestClient>());
            more.back()->Connect(port);
            more.back()->Send(R"({"id":1,"cmd":"auth","token":")" + token + R"("})");
            const Json ar = P(more.back()->Line());
            Check(ar.BoolOr("ok", false) && ar.Get("result").IntOr("client", -1) == i + 1, "client " + std::to_string(i + 1) + " is in");
        }
        TestClient fifth;
        fifth.Connect(port);
        fifth.Send(R"({"id":1,"cmd":"auth","token":")" + token + R"("})");
        r = P(fifth.Line());
        Check(!r.BoolOr("ok", true) && r.Get("error").StringOr("code", "") == "refused" && r.Get("error").StringOr("msg", "").find("too many") != std::string::npos, "the fifth client is refused");
        Check(fifth.WaitClosed(), "..and closed");
        const auto clients = server.Clients();
        Check(clients.size() == 4 && clients[0].name == "tester" && clients[0].commands >= 5, "the clients are listed");
        more.clear();  // three leave: their slots free up
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        TestClient again;
        again.Connect(port);
        again.Send(R"({"id":1,"cmd":"auth","token":")" + token + R"("})");
        r = P(again.Line());
        Check(r.BoolOr("ok", false), "a slot is free again after clients leave");
        // a line over the limit
        {
            TestClient big;
            big.Connect(port);
            big.Send(R"({"id":1,"cmd":"auth","token":")" + token + R"("})");
            big.Line();
            const std::string pad(kMaxLineBytes + 100, 'a');
            big.Send(R"({"id":2,"cmd":"t.echo","args":{"x":1},"pad":")" + pad + R"("})");
            r = P(big.Line());
            Check(!r.BoolOr("ok", true) && r.Get("error").StringOr("code", "") == "bad_args" && r.Get("error").StringOr("msg", "").find("1 MiB") != std::string::npos, "a line over 1 MiB is refused");
            Check(big.WaitClosed(), "..and the connection ends");
        }
        // a request line of exactly the limit is fine
        {
            TestClient edge;
            edge.Connect(port);
            edge.Send(R"({"id":1,"cmd":"auth","token":")" + token + R"("})");
            edge.Line();
            const std::string head = R"({"id":2,"cmd":"t.echo","args":{"x":1},"pad":")";
            const std::string tail = R"("})";
            const std::string line = head + std::string(kMaxLineBytes - head.size() - tail.size(), 'b') + tail;
            Check(line.size() == kMaxLineBytes, "test bug: the line is the limit");
            edge.Send(line);
            r = P(edge.Line());
            Check(r.BoolOr("ok", false) && r.IntOr("id", 0) == 2 && r.Get("result").IntOr("x", 0) == 1, "a line of exactly 1 MiB is read (an unknown top-level field is ignored)");
        }
        // off: the queue refuses
        engine.SetEnabled(false);
        a.Send(R"({"id":40,"cmd":"t.echo","args":{"x":1}})");
        r = P(a.Reply());
        Check(!r.BoolOr("ok", true) && r.Get("error").StringOr("code", "") == "refused", "a Director that is off refuses");
        engine.SetEnabled(true);
    }
    server.Stop();
    Check(!server.running(), "stopped");
    {
        TestClient late;
        Check(!late.Connect(port), "nothing listens after Stop");
    }
    // a second server on another port at once (two slots)
    Engine e2;
    e2.SetEnabled(true);
    LineServer s1(engine, MakeToken()), s2(e2, MakeToken());
    std::string e1, e3;
    Check(s1.Start(0, &e1) && s2.Start(0, &e3) && s1.port() != s2.port(), "two servers get two ports");
    s1.Stop();
    s2.Stop();
    // the port is taken
    LineServer s3(engine, MakeToken());
    Check(s3.Start(0, &e1), "s3");
    LineServer s4(e2, MakeToken());
    Check(!s4.Start(s3.port(), &e3) && !e3.empty(), "a taken port is an error");
    s3.Stop();
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#if defined(_WIN32)
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
#endif
    TestJson();
    TestEngine();
    TestConsole();
    TestWire();
    TestMath();
    TestPng();
    TestServer();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
