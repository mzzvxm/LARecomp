// Check of the scene file's parser, timeline, clock and reload rule (scene.h). A program of its own: it needs no game, no
// ReXGlue and no log, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_scene_test      (CMakeLists.txt)
//   <build dir>/larecomp_scene_test                              the checks
//   <build dir>/larecomp_scene_test a.scene b.scene              no checks: parses each file and prints what the game would
//                                                                say about it (exit 1 when one has an error)
//
// The compile by hand (the msvc-target clang of labuild, include path src/mc_engine/cinematic):
//   clang++ -std=c++23 src/mc_engine/cinematic/scene_test.cpp src/mc_engine/cinematic/scene.cpp -o scene_test.exe

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "scene.h"

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

void CheckEq(const std::string& got, const std::string& want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_failed;
        std::printf("FAILED: %s: got '%s', want '%s'\n", what.c_str(), got.c_str(), want.c_str());
    }
}

void CheckNum(double got, double want, const std::string& what, double tol = 1e-9) {
    ++g_checks;
    if (!(std::fabs(got - want) <= tol)) {
        ++g_failed;
        std::printf("FAILED: %s: got %.9g, want %.9g\n", what.c_str(), got, want);
    }
}

// The first diagnostic whose message holds `part`, or null.
const Diagnostic* Find(const ParseResult& r, const std::string& part) {
    for (const Diagnostic& d : r.diagnostics) {
        if (d.message.find(part) != std::string::npos) return &d;
    }
    return nullptr;
}

// One bad line (plus an end so that only it is wrong): the error must name this line and hold `part`.
void CheckError(const std::string& line, const std::string& part, int want_line = 2) {
    const ParseResult r = ParseScene("scene t\n" + line + "\nend 10\n");
    const Diagnostic* d = Find(r, part);
    Check(!r.Ok(), "'" + line + "' is an error");
    Check(d != nullptr, "'" + line + "' says '" + part + "'");
    if (d) {
        Check(d->severity == Severity::kError, "'" + line + "' is an error, not a warning");
        Check(d->line == want_line, "'" + line + "' is reported on line " + std::to_string(want_line) + ", not " + std::to_string(d->line));
    }
}

const Event* First(const Scene& s, Track track) {
    for (const Event& e : s.events) {
        if (e.track == track) return &e;
    }
    return nullptr;
}

constexpr const char* kSample = R"(# a sample scene
scene intro fps 60 step 1/60
world hour 17.4 hold weather nice traffic off peds off parked off police off
cam 0 pos 800 5 -800 yaw 80 fov 40
car p1 spawn garage:5 at 900 17 -850 heading 270
at 2 timescale 0.2 over 0.5 ease inout
at 4 hour 17.4..19 over 10
at 5.5 set mcla_gpu_ao off      # the second half of a sweep
at 6 label "AO OFF" side left fade 0.3 for 3
at 6 mark second-half
cut 8
at 8 hud off
at 12 timescale 1 over 1
at 14 weather stormy
end 1:00
)";

void TestParse() {
    const ParseResult r = ParseScene(kSample);
    Check(r.Ok(), "the sample scene parses without an error");
    CheckNum(r.WarningCount(), 0, "the sample, written in time order, has no warning");
    CheckEq(r.scene.name, "intro", "scene name");
    CheckNum(r.scene.fps, 60, "fps");
    CheckNum(r.scene.step, 1.0 / 60.0, "step 1/60", 1e-12);
    CheckNum(r.scene.end, 60, "end 1:00 is 60 s");
    CheckNum(r.scene.Duration(), 60, "Duration is the end");
    // world expands to six keys at time 0 (hour, weather, traffic, peds, parked, police), plus the other eleven lines
    Check(r.scene.events.size() == 17, "event count (got " + std::to_string(r.scene.events.size()) + ")");
    // the events are in time order
    bool sorted = true;
    for (size_t i = 1; i < r.scene.events.size(); ++i) sorted = sorted && r.scene.events[i - 1].t <= r.scene.events[i].t;
    Check(sorted, "events are sorted by time");

    const Event* hour = First(r.scene, Track::kHour);
    Check(hour && hour->t == 0 && hour->to == 17.4 && !hour->has_from, "the first hour key is a jump to 17.4 at 0");
    const Event* weather = First(r.scene, Track::kWeather);
    Check(weather && weather->index == 0, "world weather nice is index 0");
    const Event* traffic = First(r.scene, Track::kTraffic);
    Check(traffic && traffic->state == 0, "world traffic off");
    const Event* ts = First(r.scene, Track::kTimeScale);
    Check(ts && ts->t == 2 && ts->to == 0.2 && ts->over == 0.5 && ts->ease == Ease::kInOut && !ts->has_from, "timescale 0.2 over 0.5 ease inout");
    const Event* cv = First(r.scene, Track::kCvar);
    Check(cv && cv->name == "mcla_gpu_ao" && cv->text == "off" && cv->t == 5.5 && cv->line == 8, "set mcla_gpu_ao off at 5.5 on line 8 (comment cut)");
    const Event* label = First(r.scene, Track::kLabel);
    Check(label && label->text == "AO OFF" && label->side == "left" && label->fade == 0.3 && label->over == 3, "label with side, fade, for");
    const Event* mark = First(r.scene, Track::kMark);
    Check(mark && mark->name == "second-half" && mark->t == 6, "mark");
    const Event* cam = First(r.scene, Track::kCamera);
    Check(cam && cam->t == 0 && cam->words.size() == 8 && cam->words[0] == "pos" && cam->words[7] == "40", "cam keeps its words");
    const Event* cut = First(r.scene, Track::kCut);
    Check(cut && cut->t == 8 && cut->words.empty(), "cut at 8");
    const Event* hud = First(r.scene, Track::kHud);
    Check(hud && hud->t == 8 && hud->words.size() == 1 && hud->words[0] == "off", "hud off at 8");
    const Event* car = First(r.scene, Track::kCar);
    Check(car && car->t == 0 && car->words.size() >= 2 && car->words[0] == "p1" && car->words[1] == "spawn", "car p1 spawn");
    CheckEq(Summarize(r.scene).substr(0, 21), "17 events (hour 2, we", "Summarize starts with the counts");
    Check(Summarize(r.scene).find("ends at 60.00 s") != std::string::npos, "Summarize names the end");

    // the warning for a missing end, and nothing else
    const ParseResult open = ParseScene("scene x\nat 1 hour 12\n");
    Check(open.Ok() && open.WarningCount() == 1 && Find(open, "no 'end'") != nullptr, "no end is one warning, not an error");
    CheckNum(open.scene.Duration(), -1, "no end: Duration is -1");
}

void TestTimes() {
    CheckNum(ParseScene("at 1:30.5 mark a\nend 2:00")  .scene.events[0].t, 90.5, "1:30.5 is 90.5 s");
    CheckNum(ParseScene("at +3 mark a\nend 5").scene.events[0].t, 3, "a plus sign is fine");
    CheckNum(ParseScene("at .5 mark a\nend 5").scene.events[0].t, 0.5, ".5");
    CheckError("at -1 mark a", "bad time '-1'");
    CheckError("at 1:75 mark a", "bad time '1:75'");
    CheckError("at x mark a", "bad time 'x'");
    CheckError("at nan mark a", "bad time 'nan'");
    CheckError("at 3", "the statement is missing");
    CheckError("cam", "the time is missing");
    CheckError("cut abc", "bad time 'abc'");
}

void TestStatements() {
    // case does not matter for words
    const ParseResult up = ParseScene("AT 1 HOUR 12\nWeather Foggy\nTRAFFIC OFF\nEND 5\n");
    Check(up.Ok(), "verbs and names in capitals parse");
    Check(First(up.scene, Track::kWeather)->index == 3, "Foggy is 3");

    const ParseResult game = ParseScene("at 1 weather game\nat 2 hour free\nend 3\n");
    Check(game.Ok() && First(game.scene, Track::kWeather)->release && First(game.scene, Track::kHour)->release, "weather game and hour free release");

    const ParseResult sw = ParseScene("at 1 traffic 1\nat 2 peds false\nat 3 parked on\nat 4 police 0\nend 5\n");
    Check(sw.Ok(), "on/off also as 1, 0, true, false");

    const ParseResult cv = ParseScene("at 1 set mcla_gpu_scale 1.5\nat 2 set some_text hello big world\nend 3\n");
    Check(cv.Ok() && cv.scene.events[1].text == "hello big world", "a set value keeps its words");
    const ParseResult q = ParseScene("at 1 set x \"a  b\"\nend 3\n");
    Check(q.Ok() && q.scene.events[0].text == "a  b", "a quoted value keeps its blanks");

    // errors, each on its own line (line 2 of the scene the helper builds)
    CheckError("at 1 weather sunny", "unknown weather 'sunny'");
    CheckError("at 1 weather", "weather <nice|cloudy|stormy|foggy|game>");
}

void TestStatementsErrors() {
    // 25 is a legal hour (it wraps); 49 is not
    Check(ParseScene("at 1 hour 25\nend 5").Ok(), "hour 25 parses (it wraps to 1)");
    CheckError("at 1 hour 49", "bad hour '49'");
    CheckError("at 1 hour -2", "bad hour '-2'");
    CheckError("at 1 hour abc", "bad hour 'abc'");
    CheckError("at 1 hour", "hour needs a value");
    CheckError("at 1 hour 17..19", "an hour ramp needs a length");
    CheckError("at 1 hour 17 over 5", "a single hour does not move");
    CheckError("at 1 hour 17..19 over 0", "bad length '0'");
    CheckError("at 1 hour 17..19 over 4 ease wobbly", "unknown ease 'wobbly'");
    CheckError("at 1 hour 17..19 over", "'over' needs a value");
    CheckError("at 1 hour 17..19 over 4 speed 3", "unknown option 'speed'");
    CheckError("at 1 hour free now", "hour free takes nothing");
    CheckError("at 1 traffic maybe", "traffic on|off");
    CheckError("at 1 police", "police on|off");
    CheckError("at 1 timescale", "timescale <x>");
    CheckError("at 1 timescale 11", "bad time scale '11'");
    CheckError("at 1 timescale -1", "bad time scale '-1'");
    CheckError("at 1 timescale 1..0.2", "a time scale ramp needs a length");
    CheckError("at 1 set", "set <cvar> <value...>");
    CheckError("at 1 set mcla_ao", "set <cvar> <value...>");
    CheckError("at 1 set not-a-name 1", "'not-a-name' is not a setting name");
    CheckError("at 1 label", "the text is missing");
    CheckError("at 1 label \"\"", "the text is missing");
    CheckError("at 1 label \"x\" side up", "unknown label side 'up'");
    CheckError("at 1 label \"x\" fade -1", "bad fade '-1'");
    CheckError("at 1 label \"x\" for 0", "bad length '0'");
    CheckError("at 1 label \"x\" color red", "unknown option 'color'");
    CheckError("at 1 label \"x", "a double quote is not closed");
    CheckError("at 1 mark", "mark <name>");
    CheckError("at 1 mark a b", "mark <name>");
    CheckError("at 1 mark a/b", "mark <name>");
    CheckError("at 1 cam", "nothing after the time");
    CheckError("cam 1", "nothing after the time");
    CheckError("cut 1 now", "takes nothing after the time");
    CheckError("at 1 hud", "hud on, hud off");
    CheckError("at 1 car", "car <id>");
    CheckError("at 1 car p1", "car <id>");
    CheckError("at 1 car player spawn x", "'player' is the player's own car");
    CheckError("at 1 car 9x spawn x", "car <id>");
    CheckError("start", "nothing after it");
    CheckError("at 1 dance", "unknown statement 'dance'");
    CheckError("dance 1", "unknown statement 'dance'");
    CheckError("world", "world needs keys");
    CheckError("world fog thick", "world: unknown key 'fog'");
    CheckError("world hour", "world: 'hour' needs a value");
    CheckError("world weather purple", "unknown weather 'purple'");
    // the header: a scene of its own, not a line after the helper's own header
    const auto header_error = [](const std::string& line, const std::string& part) {
        const ParseResult r = ParseScene(line + "\nend 10\n");
        const Diagnostic* d = Find(r, part);
        Check(!r.Ok() && d && d->line == 1, "'" + line + "' says '" + part + "' on line 1");
    };
    header_error("scene a fps zero", "bad fps 'zero'");
    header_error("scene a fps 0", "bad fps '0'");
    header_error("scene a step 0", "bad step '0'");
    header_error("scene a step 1/0", "bad step '1/0'");
    header_error("scene a size 3", "scene: unknown option 'size'");
    header_error("scene a fps", "'fps' needs a value");
    header_error("scene", "the name is missing");
}

void TestOrderAndEnd() {
    // keys out of order: a warning, and the events come out sorted
    const ParseResult r = ParseScene("scene o\nat 5 hour 12\nat 2 weather nice\nat 3 traffic off\nend 9\n");
    Check(r.Ok(), "keys out of order are not an error");
    Check(r.WarningCount() == 2, "two out-of-order keys, two warnings (got " + std::to_string(r.WarningCount()) + ")");
    const Diagnostic* d = Find(r, "comes after line 2");
    Check(d && d->line == 3 && d->severity == Severity::kWarning, "the warning is on the late key's line, and names the line before it");
    Check(r.scene.events.size() == 3 && r.scene.events[0].track == Track::kWeather && r.scene.events[1].track == Track::kTraffic &&
              r.scene.events[2].track == Track::kHour,
          "events sorted by time");
    // same time: the file's order stays
    const ParseResult same = ParseScene("at 1 mark a\nat 1 mark b\nat 1 mark c\nend 2\n");
    Check(same.scene.events[0].name == "a" && same.scene.events[1].name == "b" && same.scene.events[2].name == "c", "keys at one time keep the file's order");
    Check(same.WarningCount() == 0, "no warning for equal times");

    // two keys of one world track at one time
    const ParseResult dup = ParseScene("at 1 hour 10\nat 1 hour 12\nend 5\n");
    const Diagnostic* dd = Find(dup, "two hour keys at t=1.00");
    Check(dup.Ok() && dd && dd->line == 2 && dd->severity == Severity::kWarning, "two hour keys at one time: a warning on the second");

    // the end
    const ParseResult after = ParseScene("scene a\nat 7 hour 12\nend 5\n");
    const Diagnostic* ad = Find(after, "is after the scene's end");
    Check(!after.Ok() && ad && ad->line == 2 && Find(after, "line 3") != nullptr, "a key after the end is an error on its line, naming the end's line");
    const ParseResult ramp = ParseScene("scene a\nat 4 hour 10..12 over 3\nend 5\n");
    const Diagnostic* rd = Find(ramp, "past the scene's end");
    Check(ramp.Ok() && rd && rd->line == 2 && rd->severity == Severity::kWarning, "a ramp that runs past the end is a warning");
    const ParseResult lab = ParseScene("scene a\nat 4 label \"x\" for 3\nend 5\n");
    Check(lab.Ok() && Find(lab, "past the scene's end") != nullptr, "a label that runs past the end is a warning");
    const ParseResult two = ParseScene("scene a\nend 5\nend 6\n");
    const Diagnostic* td = Find(two, "a second 'end'");
    Check(!two.Ok() && td && td->line == 3, "a second end is an error on its line");
    const ParseResult hdr = ParseScene("scene a\nscene b\nend 5\n");
    Check(!hdr.Ok() && Find(hdr, "a second 'scene' line") != nullptr, "a second scene line is an error");
    const ParseResult atend = ParseScene("at 8 end\nat 1 mark m\n");
    CheckNum(atend.scene.end, 8, "at 8 end");

    // empty files
    Check(!ParseScene("").Ok() && Find(ParseScene(""), "no statements")->line == 0, "an empty file is an error about the whole file");
    Check(!ParseScene("# only a comment\n\n   \n").Ok(), "a file of comments is empty");
    Check(ParseScene("\xEF\xBB\xBF" "scene bom\nend 1\n").scene.name == "bom", "a byte order mark is skipped");
    Check(ParseScene("scene crlf\r\nat 1 mark m\r\nend 2\r\n").Ok(), "CRLF line ends");
    Check(ParseScene("scene c\nat 1 mark m # trailing\nend 2 # end\n").Ok(), "trailing comments");
    Check(ParseScene("scene c\nat 1 set x a#b\nend 2\n").scene.events[0].text == "a#b", "a # inside a word is not a comment");
    Check(ParseScene("scene c\nat 1 label \"a # b\"\nend 2\n").scene.events[0].text == "a # b", "a # inside quotes is not a comment");
}

// The `statelog` option of the whole scene (state_log.h): no time, one a scene, a folder only with `on`.
void TestStateLogOption() {
    const ParseResult off = ParseScene("scene a\nmark x\nend 3\n");
    Check(off.Ok() && !off.scene.statelog && off.scene.statelog_dir.empty(), "no statelog line: the option is off");
    CheckEq(Summarize(off.scene), "1 event (mark 1); ends at 3.00 s", "the summary says nothing about the state log when it is off");

    const ParseResult on = ParseScene("scene a\nstatelog on\nend 3\n");
    Check(on.Ok() && on.scene.statelog && on.scene.statelog_dir.empty(), "statelog on");
    Check(on.scene.events.empty(), "statelog is not a timeline key");
    CheckEq(Summarize(on.scene), "0 events; ends at 3.00 s; state log on", "the summary says the state log is on");

    const ParseResult dir = ParseScene("scene a\nstatelog on dir \"C:\\takes\\one\"\nend 3\n");
    Check(dir.Ok() && dir.scene.statelog && dir.scene.statelog_dir == "C:\\takes\\one", "statelog on dir <folder> (quoted)");
    CheckEq(Summarize(dir.scene), "0 events; ends at 3.00 s; state log on (dir C:\\takes\\one)", "the summary names the folder");

    const ParseResult explicit_off = ParseScene("scene a\nstatelog OFF\nend 3\n");
    Check(explicit_off.Ok() && !explicit_off.scene.statelog, "statelog off (any case)");
    Check(ParseScene("scene a\nat 0 statelog on\nend 3\n").scene.statelog, "at 0 statelog on is the same as a bare one");
    Check(ParseScene("scene a\nworld hour 12\nstatelog on\nstatelog_note\n").ErrorCount() == 1,
          "a word that merely starts like it is still an unknown statement");

    CheckError("at 2 statelog on", "an option of the whole scene");
    CheckError("statelog", "nothing after 'statelog'");
    CheckError("statelog maybe", "'maybe' is neither");
    CheckError("statelog on dir", "'dir' needs a value");
    CheckError("statelog on file x.csv", "unknown option 'file' (dir)");
    CheckError("statelog on dir \"\"", "statelog dir needs a folder");
    CheckError("statelog off now", "statelog off takes nothing after it");
    const ParseResult twice = ParseScene("scene a\nstatelog on\nstatelog off\nend 3\n");
    const Diagnostic* d = Find(twice, "a second 'statelog'");
    Check(!twice.Ok() && d && d->line == 3 && Find(twice, "line 2") != nullptr, "a second statelog is an error on its line, naming the first");
    // a scene that is only the option and an end is a scene
    Check(ParseScene("statelog on\nend 4\n").Ok(), "a scene of only statelog and end parses");
    const ParseResult unknown = ParseScene("scene a\nstatelo on\nend 3\n");
    Check(Find(unknown, "end, statelog)") != nullptr, "the list of statements in the unknown-statement error names statelog");
}

void TestDiagnosticOrder() {
    // diagnostics come out by line, the whole-file ones first, errors and warnings together
    const ParseResult r = ParseScene("scene a\nat 5 hour 12\nat 2 weather sunny\nat 3 traffic off\nat 4 mark m\n");
    bool sorted = true;
    for (size_t i = 1; i < r.diagnostics.size(); ++i) sorted = sorted && r.diagnostics[i - 1].line <= r.diagnostics[i].line;
    Check(sorted && r.diagnostics.size() >= 3, "diagnostics are in line order (got " + std::to_string(r.diagnostics.size()) + ")");
    Check(r.diagnostics.front().line == 0, "the no-end warning is first");
}

void TestFormat() {
    Diagnostic d{12, Severity::kError, "unknown weather 'sunny'"};
    CheckEq(FormatDiagnostic(d, "intro.scene"), "intro.scene:12: error: unknown weather 'sunny'", "file and line");
    CheckEq(FormatDiagnostic(d, ""), "line 12: error: unknown weather 'sunny'", "line only");
    Diagnostic w{0, Severity::kWarning, "no 'end'"};
    CheckEq(FormatDiagnostic(w, "a.scene"), "a.scene: warning: no 'end'", "whole file warning");
    CheckEq(FormatDiagnostic(w, ""), "warning: no 'end'", "bare warning");
    const ParseResult r = ParseScene("scene a\n\n\nat 1 weather sunny\nend 2\n");
    const Diagnostic* e = Find(r, "unknown weather");
    Check(e && e->line == 4, "blank lines count: the error is on line 4");
}

void TestEase() {
    for (Ease e : {Ease::kLinear, Ease::kIn, Ease::kOut, Ease::kInOut}) {
        CheckNum(ApplyEase(e, 0), 0, "ease at 0");
        CheckNum(ApplyEase(e, 1), 1, "ease at 1");
        CheckNum(ApplyEase(e, -3), 0, "ease clamps below");
        CheckNum(ApplyEase(e, 7), 1, "ease clamps above");
        double prev = 0;
        bool monotone = true;
        for (int i = 1; i <= 100; ++i) {
            const double v = ApplyEase(e, i / 100.0);
            monotone = monotone && v >= prev;
            prev = v;
        }
        Check(monotone, "ease is monotone");
    }
    CheckNum(ApplyEase(Ease::kLinear, 0.25), 0.25, "linear");
    CheckNum(ApplyEase(Ease::kIn, 0.5), 0.25, "in is slow at the start");
    CheckNum(ApplyEase(Ease::kOut, 0.5), 0.75, "out is quick at the start");
    CheckNum(ApplyEase(Ease::kInOut, 0.5), 0.5, "inout at the middle");
    CheckNum(ApplyEase(Ease::kInOut, 0.25), 0.15625, "inout at a quarter");
}

void TestEvaluate() {
    const ParseResult r = ParseScene(
        "scene e\n"
        "world hour 10 weather nice traffic off\n"
        "at 4 hour 10..14 over 8\n"
        "at 10 weather stormy\n"
        "at 12 traffic on\n"
        "at 12 peds off\n"
        "at 14 hour free\n"
        "at 16 weather game\n"
        "at 2 timescale 0.5 over 2\n"
        "at 6 timescale 1 over 2\n"
        "at 9 timescale 0.25\n"
        "at 11 timescale 1..0.1 over 4 ease linear\n"
        "end 20\n");
    Check(r.Ok(), "the evaluation scene parses");
    const Scene& s = r.scene;

    WorldState w = EvaluateWorld(s, 0);
    Check(w.hour_set && w.hour == 10 && w.weather == 0 && w.traffic == 0 && w.peds == -1 && w.parked == -1 && w.police == -1, "at t=0 the world bundle is in force");
    Check(!w.timescale_set, "no time scale before its first key");
    CheckNum(w.timescale, 1.0, "the default time scale value is 1");

    w = EvaluateWorld(s, 3.9);
    CheckNum(w.hour, 10, "the hour holds until the ramp starts");
    w = EvaluateWorld(s, 8);
    CheckNum(w.hour, 12, "the hour ramp is at its middle at 8", 1e-9);
    w = EvaluateWorld(s, 11.9);
    CheckNum(w.hour, 10 + 4 * (7.9 / 8.0), "just before the ramp's end", 1e-9);
    w = EvaluateWorld(s, 12);
    CheckNum(w.hour, 14, "the hour ramp ends at 14, at 12");
    w = EvaluateWorld(s, 13.9);
    CheckNum(w.hour, 14, "the hour holds after the ramp");
    w = EvaluateWorld(s, 14);
    Check(!w.hour_set, "hour free releases the hour");

    w = EvaluateWorld(s, 9.99);
    Check(w.weather == 0, "weather before its key");
    w = EvaluateWorld(s, 10);
    Check(w.weather == 2, "a key's value holds from its own time");
    w = EvaluateWorld(s, 16);
    Check(w.weather == -1, "weather game releases");
    w = EvaluateWorld(s, 12);
    Check(w.traffic == 1 && w.peds == 0, "two keys at 12");

    // time scale: 0.5 over 2 from the default 1, then 1 over 2 from where it was
    w = EvaluateWorld(s, 2);
    Check(w.timescale_set, "the time scale is set from its first key");
    CheckNum(w.timescale, 1.0, "a ramp starts from 1 when there was no key before");
    w = EvaluateWorld(s, 3);
    CheckNum(w.timescale, 0.75, "the first ramp's middle (linear)");
    w = EvaluateWorld(s, 4);
    CheckNum(w.timescale, 0.5, "the first ramp's end");
    w = EvaluateWorld(s, 5);
    CheckNum(w.timescale, 0.5, "holds");
    w = EvaluateWorld(s, 7);
    CheckNum(w.timescale, 0.5 + 0.5 * 0.5, "the second ramp's middle (to 1 from 0.5, linear)");
    w = EvaluateWorld(s, 8.5);
    CheckNum(w.timescale, 1.0, "the second ramp's end");
    w = EvaluateWorld(s, 9);
    CheckNum(w.timescale, 0.25, "a jump");
    w = EvaluateWorld(s, 13);
    CheckNum(w.timescale, 0.55, "an explicit range 1..0.1 over 4 at 11: its middle, 0.55");
    w = EvaluateWorld(s, 18);
    CheckNum(w.timescale, 0.1, "the range's end holds");

    // a ramp that starts from the middle of another one
    const ParseResult mid = ParseScene("at 0 timescale 0 over 10\nat 5 timescale 1 over 5\nend 20\n");
    CheckNum(EvaluateWorld(mid.scene, 5).timescale, 0.5, "the second ramp starts from the first one's value at its key (linear half way)");
    CheckNum(EvaluateWorld(mid.scene, 10).timescale, 1.0, "and ends at 1");

    // hour past midnight wraps
    const ParseResult mn = ParseScene("at 0 hour 22..26 over 8\nat 9 hour 24\nend 20\n");
    CheckNum(EvaluateWorld(mn.scene, 0).hour, 22, "ramp start");
    CheckNum(EvaluateWorld(mn.scene, 4).hour, 0, "24 wraps to 0");
    CheckNum(EvaluateWorld(mn.scene, 6).hour, 1, "25 is 1");
    CheckNum(EvaluateWorld(mn.scene, 8).hour, 2, "26 is 2");
    CheckNum(EvaluateWorld(mn.scene, 9).hour, 0, "hour 24 is 0");

    // ease on a ramp
    const ParseResult ez = ParseScene("at 0 hour 0..10 over 10 ease in\nend 20\n");
    CheckNum(EvaluateWorld(ez.scene, 5).hour, 2.5, "ease in at the middle of 0..10", 1e-9);

    // an empty scene: nothing set
    const WorldState none = EvaluateWorld(ParseScene("scene n\nend 3\n").scene, 1);
    Check(none == WorldState{}, "a scene with no world keys sets nothing");
}

void TestCursor() {
    const ParseResult r = ParseScene("at 1 mark a\nat 2 mark b\nat 2 set x 1\nat 5 mark c\nend 6\n");
    EventCursor c;
    std::vector<const Event*> got;
    c.Take(r.scene, 0.5, &got);
    Check(got.empty(), "nothing is due before the first key");
    c.Take(r.scene, 1.0, &got);
    Check(got.size() == 1 && got[0]->name == "a", "the key at exactly its time is due");
    c.Take(r.scene, 1.0, &got);
    Check(got.size() == 1, "and only once");
    c.Take(r.scene, 3.0, &got);
    Check(got.size() == 3 && got[1]->name == "b" && got[2]->track == Track::kCvar, "two keys at 2 come in the file's order");
    got.clear();
    c.Take(r.scene, 100.0, &got);
    Check(got.size() == 1 && got[0]->name == "c", "the rest");
    Check(c.Taken() == 4, "all taken");
    c.Reset();
    got.clear();
    c.Take(r.scene, 2.0, &got);
    Check(got.size() == 3, "Reset starts over");
    // a long frame takes every key it passed, in order
    c.Reset();
    got.clear();
    c.Take(r.scene, 6.0, &got);
    Check(got.size() == 4 && got[0]->name == "a" && got[3]->name == "c", "a hitch does not skip keys");
}

void TestClock() {
    SceneClock::Config cfg;
    cfg.settle_ticks = 3;
    cfg.max_dt = 0.25;
    SceneClock c(cfg);
    SceneClock::Step s = c.Advance(true, false, 0.016, 10.0);
    Check(s.state == SceneClock::State::kIdle && !s.started, "an idle clock does nothing");
    c.Arm();
    s = c.Advance(true, false, 0.016, 10.0);
    Check(s.state == SceneClock::State::kWaiting && !s.started, "armed: waiting");
    s = c.Advance(false, false, 0.016, 10.0);
    s = c.Advance(true, false, 0.016, 10.0);
    s = c.Advance(true, false, 0.016, 10.0);
    Check(s.state == SceneClock::State::kWaiting, "a frame without the world starts the count again (2 in a row so far)");
    s = c.Advance(true, true, 0.016, 10.0);
    Check(s.state == SceneClock::State::kWaiting, "a paused frame starts the count again");
    c.Advance(true, false, 0.016, 10.0);
    c.Advance(true, false, 0.016, 10.0);
    s = c.Advance(true, false, 0.016, 10.0);
    Check(s.started && s.state == SceneClock::State::kRunning && s.t == 0.0 && !s.advanced, "three ready frames in a row start it, at time 0");
    s = c.Advance(true, false, 0.5, 10.0);
    CheckNum(s.t, 0.25, "a frame is never more than max_dt");
    Check(s.advanced, "the time moved");
    s = c.Advance(true, true, 0.1, 10.0);
    CheckNum(s.t, 0.25, "paused: the time holds");
    Check(!s.advanced, "and does not move");
    s = c.Advance(false, false, 0.1, 10.0);
    CheckNum(s.t, 0.25, "no world (a loading screen): the time holds");
    s = c.Advance(true, false, 0.0, 10.0);
    CheckNum(s.t, 0.25, "a zero delta holds");
    for (int i = 0; i < 20; ++i) s = c.Advance(true, false, 0.25, 10.0);
    CheckNum(s.t, 5.25, "20 frames of 0.25 s");
    c.SetTime(9.9, 10.0);
    CheckNum(c.time(), 9.9, "SetTime puts the clock at a time");
    s = c.Advance(true, false, 0.25, 10.0);
    Check(s.finished && s.state == SceneClock::State::kFinished && s.t == 10.0, "it finishes at the length, exactly");
    s = c.Advance(true, false, 0.25, 10.0);
    Check(!s.finished && s.state == SceneClock::State::kFinished && s.t == 10.0, "and stays finished");
    c.SetTime(2.0, 10.0);
    Check(c.state() == SceneClock::State::kRunning && c.time() == 2.0, "SetTime runs a finished scene again from there");
    c.SetTime(50.0, 10.0);
    CheckNum(c.time(), 10.0, "SetTime stops at the length");
    c.SetTime(-4.0, 10.0);
    CheckNum(c.time(), 0.0, "and at 0");

    // no end: never finishes
    SceneClock open(cfg);
    open.Arm();
    for (int i = 0; i < 3; ++i) open.Advance(true, false, 0.1, -1.0);
    for (int i = 0; i < 1000; ++i) s = open.Advance(true, false, 0.25, -1.0);
    Check(s.state == SceneClock::State::kRunning && !s.finished, "a scene with no end never finishes");

    // SetTime while idle or waiting does nothing
    SceneClock idle(cfg);
    idle.SetTime(3.0, -1.0);
    Check(idle.state() == SceneClock::State::kIdle && idle.time() == 0.0, "SetTime on an idle clock does nothing");
    idle.Arm();
    idle.SetTime(3.0, -1.0);
    Check(idle.state() == SceneClock::State::kWaiting, "nor on a waiting one");
    idle.Disarm();
    Check(idle.state() == SceneClock::State::kIdle, "Disarm");

    // Restart: running at 0 at once, also from finished
    SceneClock again(cfg);
    again.Restart();
    Check(again.state() == SceneClock::State::kRunning && again.time() == 0.0, "Restart runs at time 0 without waiting");
    again.Advance(true, false, 0.25, 0.5);
    again.Advance(true, false, 0.25, 0.5);
    Check(again.state() == SceneClock::State::kFinished, "...to its end");
    again.Restart();
    Check(again.state() == SceneClock::State::kRunning && again.time() == 0.0, "...and again from finished");

    // the default settle is 30 frames
    SceneClock def;
    def.Arm();
    int frames = 0;
    SceneClock::Step ds;
    do {
        ds = def.Advance(true, false, 0.016, -1.0);
        ++frames;
    } while (!ds.started && frames < 100);
    Check(frames == 30, "the default wait is 30 ready frames (got " + std::to_string(frames) + ")");
}

FileStamp Stamp(int64_t mtime, uint64_t size) {
    return FileStamp{true, mtime, size};
}

void TestReload() {
    ReloadWatcher w(300);
    // the file is not there
    CheckEq(w.Poll(FileStamp{}, 0) == ReloadWatcher::Action::kMissing ? "missing" : "other", "missing", "a missing file is told once");
    Check(w.Poll(FileStamp{}, 250) == ReloadWatcher::Action::kNone, "and not again");
    // it appears: the first read is at once
    Check(w.Poll(Stamp(100, 500), 500) == ReloadWatcher::Action::kLoad, "the first sight of a file with content: load");
    w.Loaded(Stamp(100, 500));
    Check(w.Poll(Stamp(100, 500), 750) == ReloadWatcher::Action::kNone, "unchanged: nothing");
    // the editor saves: the stamp changes; load only after it stayed
    Check(w.Poll(Stamp(200, 520), 1000) == ReloadWatcher::Action::kNone, "changed: wait");
    Check(w.Poll(Stamp(200, 520), 1200) == ReloadWatcher::Action::kNone, "200 ms later: still waiting");
    Check(w.Poll(Stamp(200, 520), 1300) == ReloadWatcher::Action::kLoad, "300 ms of the same stamp: load");
    w.Loaded(Stamp(200, 520));
    Check(w.Poll(Stamp(200, 520), 1600) == ReloadWatcher::Action::kNone, "loaded: nothing");
    // a write in two steps: truncated, then filled
    Check(w.Poll(Stamp(300, 0), 2000) == ReloadWatcher::Action::kNone, "truncated: wait");
    Check(w.Poll(Stamp(310, 480), 2100) == ReloadWatcher::Action::kNone, "filled 100 ms later: the timer starts over");
    Check(w.Poll(Stamp(310, 480), 2300) == ReloadWatcher::Action::kNone, "200 ms after that: still waiting");
    Check(w.Poll(Stamp(310, 480), 2400) == ReloadWatcher::Action::kLoad, "300 ms after the last change: load");
    // a bad file is told as loaded: it is not read again until it changes
    w.Loaded(Stamp(310, 480));
    for (int i = 0; i < 10; ++i) {
        if (w.Poll(Stamp(310, 480), 3000 + i * 250) != ReloadWatcher::Action::kNone) {
            Check(false, "a file that was read (bad or not) is not read again");
            break;
        }
    }
    // the same time stamp but another size is a change
    Check(w.Poll(Stamp(310, 481), 6000) == ReloadWatcher::Action::kNone, "a new size: wait");
    Check(w.Poll(Stamp(310, 481), 6400) == ReloadWatcher::Action::kLoad, "then load");
    w.Loaded(Stamp(310, 481));
    // deleted and back
    Check(w.Poll(FileStamp{}, 7000) == ReloadWatcher::Action::kMissing, "deleted: told");
    Check(w.Poll(FileStamp{}, 7250) == ReloadWatcher::Action::kNone, "once");
    Check(w.Poll(Stamp(310, 481), 7500) == ReloadWatcher::Action::kNone, "back with the same stamp as the loaded one: nothing");
    // an empty file that never fills is read after the wait (it is an error the player should see)
    ReloadWatcher e(300);
    Check(e.Poll(Stamp(5, 0), 0) == ReloadWatcher::Action::kNone, "an empty file at first sight: wait");
    Check(e.Poll(Stamp(5, 0), 299) == ReloadWatcher::Action::kNone, "299 ms");
    Check(e.Poll(Stamp(5, 0), 300) == ReloadWatcher::Action::kLoad, "300 ms: load it (it will say it is empty)");
    // Reset forgets
    w.Reset();
    Check(w.Poll(Stamp(310, 481), 9000) == ReloadWatcher::Action::kLoad, "after Reset the same file is a first sight again");
}

// Parse files given on the command line and say what the game would say. Returns 1 when one has an error.
int ParseFiles(int argc, char** argv) {
    int rc = 0;
    for (int i = 1; i < argc; ++i) {
        std::ifstream in(argv[i], std::ios::binary);
        if (!in) {
            std::printf("%s: cannot open\n", argv[i]);
            rc = 1;
            continue;
        }
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const ParseResult r = ParseScene(text);
        for (const Diagnostic& d : r.diagnostics) std::printf("%s\n", FormatDiagnostic(d, argv[i]).c_str());
        std::printf("%s: %s: %d error(s), %d warning(s); %s\n", argv[i], r.scene.name.c_str(), r.ErrorCount(), r.WarningCount(),
                    Summarize(r.scene).c_str());
        if (!r.Ok()) rc = 1;
    }
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) return ParseFiles(argc, argv);
    TestParse();
    TestTimes();
    TestStatements();
    TestStatementsErrors();
    TestOrderAndEnd();
    TestStateLogOption();
    TestDiagnosticOrder();
    TestFormat();
    TestEase();
    TestEvaluate();
    TestCursor();
    TestClock();
    TestReload();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
