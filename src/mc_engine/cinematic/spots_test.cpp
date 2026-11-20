// Check of the spots (spots.h): the names, the spot words of a statement, the file's reader, the lookup of the files a scene uses and the way
// scene.cpp turns spot words into numbers. A program of its own: it needs no game, no ReXGlue and no log, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_spots_test       (CMakeLists.txt)
//   <build dir>/larecomp_spots_test                               the checks
//   <build dir>/larecomp_spots_test a.scene b.scene               no checks: parses each scene with the spots files that belong to it and prints
//                                                                 the statements the game would read (spot words resolved); exit 1 on an error
//
// The compile by hand (the msvc-target clang of labuild, include path src/mc_engine/cinematic):
//   clang++ -std=c++23 spots_test.cpp spots.cpp scene.cpp -o spots_test.exe

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "car_verbs.h"
#include "scene.h"
#include "spots.h"

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

void CheckNum(double got, double want, const std::string& what, double tol = 1e-6) {
    ++g_checks;
    if (!(std::fabs(got - want) <= tol)) {
        ++g_failed;
        std::printf("FAILED: %s: got %.9g, want %.9g\n", what.c_str(), got, want);
    }
}

Spot Make(const char* name, double x, double y, double z, double heading, bool has_heading = true) {
    Spot s;
    s.name = name;
    s.x = x;
    s.y = y;
    s.z = z;
    s.heading = heading;
    s.has_heading = has_heading;
    return s;
}

// The street outside the Hollywood garage gate, the way the scenes name it: heading 62 (direction (0.883, 0.469) in x, z).
SpotTable SampleTable() {
    SpotTable t;
    t.Add(Make("hw-street", 832.19, 16.5, -787.53, 62.0));
    t.Add(Make("hw-lot", 819.6, 17.6, -808.4, 332.0));
    t.Add(Make("Corner", 100.0, 5.0, 200.0, 0.0, false));
    return t;
}

std::vector<std::string> Words(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : line) {
        if (c == ' ') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string Join(const std::vector<std::string>& w) {
    std::string out;
    for (size_t i = 0; i < w.size(); ++i) out += (i ? " " : "") + w[i];
    return out;
}

// ---------------------------------------------------------------------------------------------------------------- names and words

void TestNames() {
    Check(IsSpotName("hw-street") && IsSpotName("k2") && IsSpotName("a.b_c-d") && IsSpotName("9lives"), "names that are fine");
    Check(!IsSpotName("") && !IsSpotName("-x") && !IsSpotName("_x") && !IsSpotName(".x") && !IsSpotName("a b") && !IsSpotName("a@b") && !IsSpotName("a,b") &&
              !IsSpotName("a:b") && !IsSpotName(std::string(41, 'a')),
          "names that are not");
    Check(IsSpotName(std::string(40, 'a')), "40 characters are fine");
    Check(IsSpotWord("spot:x") && IsSpotWord("SPOT:x") && IsSpotWord("Spot:") && !IsSpotWord("spot") && !IsSpotWord("spots:x") && !IsSpotWord("car:x"),
          "what a spot word is");
}

void TestRef() {
    SpotRef r;
    std::string e;
    Check(ParseSpotRef("spot:hw-street", &r, &e) && r.name == "hw-street" && !r.has_offset && !r.has_turn, "a bare spot word");
    Check(ParseSpotRef("SPOT:Hw-Street@1.5,0,-3", &r, &e) && r.name == "Hw-Street" && r.has_offset && !r.has_turn, "an offset (the name as written)");
    CheckNum(r.right, 1.5, "right");
    CheckNum(r.up, 0.0, "up");
    CheckNum(r.forward, -3.0, "forward");
    Check(ParseSpotRef("spot:a@0,1.7,-8,180", &r, &e) && r.has_turn, "an offset and a turn");
    CheckNum(r.turn, 180.0, "the turn");
    Check(ParseSpotRef("spot:a@-1e1,+2,.5", &r, &e), "numbers in any spelling");
    CheckNum(r.right, -10.0, "-1e1");
    const char* const bad[] = {"spot:", "spot:a b", "spot:a@", "spot:a@1,2", "spot:a@1,2,x", "spot:a@1,2,3,4,5", "spot:a@1,,3", "spot:a@1,2,3,", "spot:a@nan,0,0",
                               "spot:a@99999,0,0", "spot:-a", "spot:a@@1,2,3", "spot:a@0x,1,2"};
    for (const char* w : bad) {
        e.clear();
        Check(!ParseSpotRef(w, &r, &e) && !e.empty(), std::string("a bad spot word: ") + w);
    }
    e.clear();
    Check(!ParseSpotRef("car:p1", &r, &e) && e.find("not a spot word") != std::string::npos, "a word that is not a spot word");
}

void TestResolve() {
    const SpotTable t = SampleTable();
    const Spot* s = t.Find("hw-street");
    SpotRef r;
    std::string e;
    double p[3];
    ParseSpotRef("spot:hw-street", &r, &e);
    Check(SpotPosition(*s, r, p, &e), "the place of a spot");
    CheckNum(p[0], 832.19, "x");
    CheckNum(p[1], 16.5, "y");
    CheckNum(p[2], -787.53, "z");
    // heading 62: forward (sin 62, cos 62) = (0.8829, 0.4695); right (-cos 62, sin 62) = (-0.4695, 0.8829)
    ParseSpotRef("spot:hw-street@0,0,10", &r, &e);
    SpotPosition(*s, r, p, &e);
    CheckNum(p[0], 832.19 + 10 * std::sin(62 * 3.14159265358979 / 180), "10 m ahead: x");
    CheckNum(p[2], -787.53 + 10 * std::cos(62 * 3.14159265358979 / 180), "10 m ahead: z");
    ParseSpotRef("spot:hw-street@2,1.5,0", &r, &e);
    SpotPosition(*s, r, p, &e);
    CheckNum(p[0], 832.19 - 2 * std::cos(62 * 3.14159265358979 / 180), "2 m to the right: x (the right of a heading is minus cos, sin)");
    CheckNum(p[1], 16.5 + 1.5, "1.5 m up");
    CheckNum(p[2], -787.53 + 2 * std::sin(62 * 3.14159265358979 / 180), "2 m to the right: z");
    // heading 0 faces +Z (south): its right is -X; heading 90 faces +X: its right is +Z
    SpotTable t0;
    t0.Add(Make("north-facing", 0, 0, 0, 0));
    t0.Add(Make("east-facing", 0, 0, 0, 90));
    ParseSpotRef("spot:north-facing@1,0,1", &r, &e);
    SpotPosition(*t0.Find("north-facing"), r, p, &e);
    CheckNum(p[0], -1.0, "heading 0: right is -X");
    CheckNum(p[2], 1.0, "heading 0: forward is +Z");
    ParseSpotRef("spot:east-facing@1,0,1", &r, &e);
    SpotPosition(*t0.Find("east-facing"), r, p, &e);
    CheckNum(p[0], 1.0, "heading 90: forward is +X");
    CheckNum(p[2], 1.0, "heading 90: right is +Z");
    // no heading: the place only
    ParseSpotRef("spot:corner@1,0,0", &r, &e);
    Check(!SpotPosition(*t.Find("corner"), r, p, &e) && e.find("has no heading") != std::string::npos, "an offset needs a heading");
    ParseSpotRef("spot:corner@0,0,0", &r, &e);
    Check(SpotPosition(*t.Find("corner"), r, p, &e), "an offset of nothing does not");
    ParseSpotRef("spot:corner", &r, &e);
    Check(SpotPosition(*t.Find("corner"), r, p, &e) && p[0] == 100.0 && p[2] == 200.0, "the bare place of a spot with no heading");
    double h = 0;
    Check(!SpotHeading(*t.Find("corner"), r, &h, &e), "a heading of a spot with none");
    ParseSpotRef("spot:hw-street", &r, &e);
    Check(SpotHeading(*s, r, &h, &e), "the heading");
    CheckNum(h, 62.0, "is the spot's");
    ParseSpotRef("spot:hw-street@5,5,5,180", &r, &e);
    SpotHeading(*s, r, &h, &e);
    CheckNum(h, 242.0, "the turn is added, the offset is not used");
    ParseSpotRef("spot:hw-street@0,0,0,-100", &r, &e);
    SpotHeading(*s, r, &h, &e);
    CheckNum(h, 322.0, "a turn that goes below 0 wraps");
}

void TestTable() {
    SpotTable t = SampleTable();
    Check(t.size() == 3 && t.Find("CORNER") != nullptr && t.Find("corner")->name == "corner", "names are not case sensitive and are kept lower case");
    Check(t.Find("nothing") == nullptr, "an unknown name");
    Check(!t.Add(Make("hw-street", 1, 2, 3, 4)) && t.size() == 3 && t.Find("hw-street")->x == 1.0, "a name added again replaces the spot and keeps the count");
    Check(t.spots()[0].name == "hw-street", "and its place in the order");
    SpotTable names;
    for (const char* n : {"hw-street", "hw-street-east", "hw-lot", "sunset-w", "sunset-e", "fairfax-n"}) names.Add(Make(n, 0, 0, 0, 0));
    const std::vector<std::string> a = names.Suggest("hw-stret");
    Check(!a.empty() && a[0] == "hw-street", "a typo finds the name");
    const std::vector<std::string> b = names.Suggest("sunset");
    Check(b.size() == 2, "a part of a name finds the names that hold it");
    Check(names.Suggest("zzzzzz").empty(), "a name that is near nothing finds nothing");
    Check(names.Suggest("hw", 1).size() == 1, "the number of suggestions is capped");
}

// ---------------------------------------------------------------------------------------------------------------- a statement's words

void TestExpand() {
    const SpotTable t = SampleTable();
    std::string e;
    std::vector<std::string> w = Words("spawn vp_x at spot:hw-lot heading spot:hw-lot paint 3");
    Check(ExpandSpotWords(&w, &t, &e), "a car line with a place and a heading");
    CheckEq(Join(w), "spawn vp_x at 819.6 17.6 -808.4 heading 332 paint 3", "is turned into x y z and degrees");
    w = Words("pos spot:hw-street@0,1.7,-8 look spot:hw-street fov 40");
    Check(ExpandSpotWords(&w, &t, &e), "a camera line");
    CheckEq(Join(w), "pos " + spots_detail::FormatNumber(832.19 + (-8) * std::sin(62 * 3.14159265358979 / 180)) + " 18.2 " +
                         spots_detail::FormatNumber(-787.53 + (-8) * std::cos(62 * 3.14159265358979 / 180)) + " look 832.19 16.5 -787.53 fov 40",
            "an offset behind the spot, and a look at its place");
    w = Words("pos 1 2 3 yaw spot:hw-street pitch -4");
    Check(ExpandSpotWords(&w, &t, &e) && Join(w) == "pos 1 2 3 yaw 62 pitch -4", "yaw takes the heading");
    w = Words("at spot:hw-street@1.4,0,0,180 heading spot:hw-street@1.4,0,0,180");
    Check(ExpandSpotWords(&w, &t, &e), "the same word places a car and turns it");
    CheckEq(w[w.size() - 1], "242", "...heading 62 + 180");
    Check(w.size() == 6 && w[0] == "at", "...and the place is three words");
    w = Words("warp spot:hw-lot");
    Check(ExpandSpotWords(&w, &t, &e) && Join(w) == "warp 819.6 17.6 -808.4", "a preamble line");
    w = Words("pos 1 2 3");
    Check(ExpandSpotWords(&w, nullptr, &e) && Join(w) == "pos 1 2 3", "no spot word: nothing to do, and no table needed");
    w = Words("rig tripod at spot:hw-lot track car:p1");
    Check(ExpandSpotWords(&w, &t, &e) && Join(w) == "rig tripod at 819.6 17.6 -808.4 track car:p1", "a rig's place");
    w = Words("SPOT:HW-LOT");
    Check(ExpandSpotWords(&w, &t, &e) && Join(w) == "819.6 17.6 -808.4", "the prefix and the name in any case");

    const std::vector<std::string> before = Words("pos spot:nope");
    w = before;
    Check(!ExpandSpotWords(&w, &t, &e) && e.find("unknown spot 'nope'") != std::string::npos && w == before, "an unknown spot: an error, the words unchanged");
    w = Words("pos spot:hw-stret");
    Check(!ExpandSpotWords(&w, &t, &e) && e.find("did you mean hw-street?") != std::string::npos, "a typo is named");
    w = Words("pos spot:hw-lot");
    Check(!ExpandSpotWords(&w, nullptr, &e) && e.find("no spot table") != std::string::npos, "no table");
    w = Words("pos spot:corner@1,0,0");
    Check(!ExpandSpotWords(&w, &t, &e) && e.find("has no heading") != std::string::npos, "an offset of a spot with no heading");
    w = Words("heading spot:corner");
    Check(!ExpandSpotWords(&w, &t, &e) && e.find("has no heading") != std::string::npos, "a heading of a spot with none");
    w = Words("pos spot:hw-lot@1,2");
    Check(!ExpandSpotWords(&w, &t, &e) && e.find("right,up,forward") != std::string::npos, "a bad offset");
    w = Words("orbit car:p1 r 8 at spot:hw-lot");
    Check(!ExpandSpotWords(&w, &t, &e) && e.find("offset in the car's frame") != std::string::npos, "an orbit's `at` is an offset, not a place");
    w = Words("orbit car:p1 r 8 look spot:hw-lot");
    Check(ExpandSpotWords(&w, &t, &e), "...but a look of an orbit is a place");
}

// ---------------------------------------------------------------------------------------------------------------- the scene parser

const Diagnostic* FindDiag(const ParseResult& r, const std::string& part) {
    for (const Diagnostic& d : r.diagnostics) {
        if (d.message.find(part) != std::string::npos) return &d;
    }
    return nullptr;
}

std::string WordsOf(const ParseResult& r, Track track, size_t index = 0) {
    size_t n = 0;
    for (const Event& e : r.scene.events) {
        if (e.track == track && n++ == index) return Join(e.words);
    }
    return "(none)";
}

void TestScene() {
    const SpotTable t = SampleTable();
    ParseOptions o;
    o.spots = &t;
    o.path = "C:\\mcv\\larecomp\\ports\\scripts\\scenes\\recipes\\x.scene";
    const ParseResult r = ParseScene(
        "scene s\n"
        "at 0.5 car p1 spawn vp_lam_murcielago_06 at spot:hw-street@-1.5,0,0 heading spot:hw-street\n"
        "at 0.5 car p2 spawn vp_aud_r8_08 at spot:hw-street@1.4,0,0 heading spot:hw-street@1.4,0,0,180\n"
        "cam 0 pos spot:hw-lot@0,1.5,-6 look spot:hw-street fov 40\n"
        "cam 3 yaw spot:hw-lot pitch -5 pos 1 2 3\n"
        "start warp spot:hw-lot\n"
        "end 9\n",
        o);
    Check(r.Ok(), "a scene with spot words parses");
    CheckEq(WordsOf(r, Track::kCar, 0), "p1 spawn vp_lam_murcielago_06 at " + spots_detail::FormatNumber(832.19 + 1.5 * std::cos(62 * 3.14159265358979 / 180)) +
                                            " 16.5 " + spots_detail::FormatNumber(-787.53 - 1.5 * std::sin(62 * 3.14159265358979 / 180)) + " heading 62",
            "a car's place and heading, kept as numbers for the cars package");
    Check(WordsOf(r, Track::kCar, 1).find("heading 242") != std::string::npos, "the oncoming lane faces the other way");
    CheckEq(WordsOf(r, Track::kCamera, 1), "yaw 332 pitch -5 pos 1 2 3", "a camera's yaw");
    CheckEq(WordsOf(r, Track::kStart), "warp 819.6 17.6 -808.4", "the preamble");
    CheckEq(r.scene.source_path, o.path, "the scene remembers the file it came from");
    CheckEq(r.scene.source_dir, "C:\\mcv\\larecomp\\ports\\scripts\\scenes\\recipes", "...and its folder");
    CheckEq(ParseScene("scene a\nend 1\n", ParseOptions{nullptr, "a/b/c.scene"}).scene.source_dir, "a/b", "a forward slash path");
    CheckEq(ParseScene("scene a\nend 1\n", ParseOptions{nullptr, "c.scene"}).scene.source_dir, "", "a bare name has no folder");
    CheckEq(ParseScene("scene a\nend 1\n").scene.source_path, "", "with no options the scene's file is not known");

    // errors: on their own line, the scene does not load
    const ParseResult unknown = ParseScene("scene s\nat 1 car p1 spawn vp_x at spot:hw-stret\nend 3\n", o);
    const Diagnostic* d = FindDiag(unknown, "unknown spot 'hw-stret' (did you mean hw-street?)");
    Check(!unknown.Ok() && d != nullptr && d->line == 2 && d->severity == Severity::kError, "an unknown spot is an error on its line, with the suggestion");
    const ParseResult none = ParseScene("scene s\ncam 1 pos spot:hw-lot look 0 0 0\nend 3\n");
    Check(!none.Ok() && FindDiag(none, "no spot table is loaded") != nullptr, "without a table a spot word is an error, not a silent zero");
    const ParseResult noheading = ParseScene("scene s\ncam 1 pos spot:corner@0,1,0 look 0 0 0\nend 3\n", o);
    Check(!noheading.Ok() && FindDiag(noheading, "has no heading") != nullptr, "an offset in the frame of a spot with no heading");
    // spot words only in the statements that take places
    const ParseResult label = ParseScene("scene s\nat 1 label \"spot:hw-lot\"\nat 1 mark spot:x\nend 3\n");
    Check(label.Ok(), "a label and a mark may hold the text spot:");
    const ParseResult bad_car = ParseScene("scene s\nat 1 car p1 spawn vp_x at spot:hw-lot heading\nend 3\n", o);
    Check(!bad_car.Ok() && FindDiag(bad_car, "heading needs degrees") != nullptr, "the car parser still judges what is left: a heading with no value");
    // a spot's place goes through the car parser's own limits
    SpotTable far;
    far.Add(Make("far", 99999.0, 0.0, 0.0, 90.0));
    ParseOptions fo;
    fo.spots = &far;
    Check(ParseScene("scene s\nat 1 car p1 spawn vp_x at spot:far\nend 3\n", fo).Ok(), "a place inside the world");
    const ParseResult past = ParseScene("scene s\nat 1 car p1 spawn vp_x at spot:far@0,0,5000\nend 3\n", fo);
    Check(!past.Ok() && FindDiag(past, "outside the world") != nullptr, "an offset that leaves the world is the car parser's error, on its line");
    // the settings a scene may not set
    CheckEq(CvarRefusal("mcla_director"), "a scene does not set mcla_director", "a scene does not drive the Director");
    CheckEq(CvarRefusal("mcla_director_info"), "a scene does not set mcla_director_info", "...nor name its info file");
    CheckEq(CvarRefusal("MCLA_DIRECTOR_ROOT"), "a scene does not set MCLA_DIRECTOR_ROOT", "...nor its root, in any case");
    CheckEq(CvarRefusal("mcla_directorx"), "a scene does not set mcla_directorx", "the prefix, as mcla_scene's");
    Check(CvarRefusal("mcla_dire").empty() && CvarRefusal("direct_mode").empty(), "names that only look like it are free");
}

// A car statement parsed on its own (a command of the Director, a test) has no table: the car parser says what a spot word is instead of "bad position".
void TestCarVerbs() {
    CarCommand cmd;
    std::string why;
    Check(ParseCarCommand(Words("p1 spawn vp_x at spot:hw-lot"), &cmd, &why) == CarParse::kError && why.find("is a spot word") != std::string::npos,
          "a spot word in `at` that nobody resolved is named as one");
    Check(ParseCarCommand(Words("p1 spawn vp_x at 1,2,3 heading spot:hw-lot"), &cmd, &why) == CarParse::kError && why.find("'spot:hw-lot' is a spot word") != std::string::npos,
          "...and in `heading`");
    Check(ParseCarCommand(Words("p1 ambient va_x at SPOT:a@1,2,3"), &cmd, &why) == CarParse::kError && why.find("is a spot word") != std::string::npos, "...in any case, for an ambient car too");
    std::vector<std::string> w = Words("p1 spawn vp_x at spot:hw-street@-1.5,0,0 heading spot:hw-street");
    const SpotTable t = SampleTable();
    Check(ExpandSpotWords(&w, &t, &why) && ParseCarCommand(w, &cmd, &why) == CarParse::kOk, "expanded first, the same statement is a car statement");
    CheckNum(cmd.heading, 62.0, "its heading is the spot's");
    CheckNum(cmd.position[1], 16.5, "its height is the spot's");
    CheckNum(std::hypot(cmd.position[0] - 832.19, cmd.position[2] + 787.53), 1.5, "and its place is 1.5 m from the spot, to its left", 1e-2);
}

// ---------------------------------------------------------------------------------------------------------------- the file

void TestParseSpots() {
    SpotTable t;
    std::vector<Diagnostic> d;
    const bool ok = ParseSpots(
        "# the spots\n"
        "\n"
        "hw-street 832.19 16.5 -787.53 62   # the street outside the gate; lamps ? src: cars-demo\n"
        "  Hw-Lot, 819.6, 17.6, -808.4, 332  # commas and blanks, any case\n"
        "corner 100 5 200\n"
        "tab\t1\t2\t3\t4\n"
        "neg -1.5e2 0 -3 -90 # the heading wraps\n"
        "has#hash 1 2 3 4\n",
        "spots.txt", &t, &d);
    Check(!ok, "a file with a bad line is not ok");
    Check(t.size() == 5, "the good lines are kept, the bad one skipped: " + std::to_string(t.size()));
    const Spot* s = t.Find("hw-street");
    Check(s && s->has_heading && s->heading == 62.0 && s->x == 832.19 && s->z == -787.53 && s->line == 3 && s->file == "spots.txt", "a spot with all its fields");
    Check(s && s->note == "the street outside the gate; lamps ? src: cars-demo", "its note");
    Check(t.Find("hw-lot") && t.Find("hw-lot")->heading == 332.0 && t.Find("hw-lot")->note == "commas and blanks, any case", "commas, blanks and case");
    Check(t.Find("corner") && !t.Find("corner")->has_heading && t.Find("corner")->note.empty(), "a spot with no heading");
    Check(t.Find("tab") && t.Find("tab")->heading == 4.0, "tabs");
    CheckNum(t.Find("neg")->heading, 270.0, "a heading below 0 wraps");
    CheckNum(t.Find("neg")->x, -150.0, "a number with an exponent");
    int errors = 0;
    for (const Diagnostic& x : d) errors += x.severity == Severity::kError ? 1 : 0;
    Check(errors == 1 && d.size() == 1 && d[0].line == 8, "one error, on the line of the bad name");

    const char* const bad[] = {"x 1 2", "x 1 2 3 4 5", "x one 2 3", "x 1 2 3 up", "x 999999 0 0", "x 1 2 nan", "-x 1 2 3", "x"};
    for (const char* line : bad) {
        SpotTable bt;
        std::vector<Diagnostic> bd;
        const bool bok = ParseSpots(std::string(line) + "\n", "s.txt", &bt, &bd);
        Check(!bok && bt.empty() && bd.size() == 1 && bd[0].line == 1, std::string("a bad spot line: ") + line);
    }
    SpotTable dup;
    std::vector<Diagnostic> dd;
    ParseSpots("a 1 2 3 4\nb 5 6 7 8\nA 9 9 9 9\n", "s.txt", &dup, &dd);
    Check(dup.size() == 2 && dup.Find("a")->x == 9.0 && dd.size() == 1 && dd[0].severity == Severity::kWarning && dd[0].line == 3 &&
              dd[0].message.find("line 1") != std::string::npos,
          "a name twice in one file: the later wins, with a warning");
    SpotTable two;
    std::vector<Diagnostic> td;
    ParseSpots("a 1 2 3 4\n", "shared.txt", &two, &td);
    ParseSpots("a 9 9 9 9\n", "local.txt", &two, &td);
    Check(two.Find("a")->x == 9.0 && td.empty(), "a name in a second file replaces the first quietly (the local file on top)");
    SpotTable bom;
    std::vector<Diagnostic> bomd;
    Check(ParseSpots("\xEF\xBB\xBF" "a 1 2 3 4\r\nb 1 2 3\r\n", "s.txt", &bom, &bomd) && bom.size() == 2, "a byte order mark and CRLF");
    SpotTable empty;
    Check(ParseSpots("", "s.txt", &empty, nullptr) && empty.empty(), "an empty file is fine, and diagnostics may be left out");
}

std::filesystem::path TempDir() {
    std::error_code ec;
    std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    std::filesystem::path dir = base / "larecomp-spots-test";
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void Write(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

void TestFiles() {
    namespace fs = std::filesystem;
    const fs::path root = TempDir();
    const fs::path scenes = root / "scenes";
    Write(scenes / "spots.txt", "shared 1 2 3 4  # the shared one\nboth 5 5 5 5\n");
    Write(scenes / "x.scene", "scene x\nend 1\n");
    Write(scenes / "recipes" / "r.scene", "scene r\nend 1\n");
    Write(root / "scenes-test" / "t.scene", "scene t\nend 1\n");
    Write(root / "elsewhere" / "e.scene", "scene e\nend 1\n");

    auto names = [](const std::vector<std::string>& v) {
        std::string out;
        for (const std::string& f : v) out += (out.empty() ? "" : "|") + fs::path(f).parent_path().filename().string() + "/" + fs::path(f).filename().string();
        return out;
    };
    CheckEq(names(SpotFilesFor((scenes / "x.scene").string())), "scenes/spots.txt", "a scene in the scenes folder gets that folder's file once");
    CheckEq(names(SpotFilesFor((scenes / "recipes" / "r.scene").string())), "scenes/spots.txt", "a scene in a subfolder (recipes) gets the shared file");
    CheckEq(names(SpotFilesFor((root / "scenes-test" / "t.scene").string())), "scenes/spots.txt", "a scene in a sibling folder (scenes-test) gets it too");
    CheckEq(names(SpotFilesFor((root / "elsewhere" / "e.scene").string())), "scenes/spots.txt", "...and so does one beside the scenes folder");
    CheckEq(names(SpotFilesFor("")), "", "no scene path: no files");
    Write(scenes / "recipes" / "spots.txt", "local 7 7 7 7\nboth 6 6 6 6\n");
    CheckEq(names(SpotFilesFor((scenes / "recipes" / "r.scene").string())), "scenes/spots.txt|recipes/spots.txt", "a file beside the scene comes after the shared one");

    LoadedSpots l = LoadSpotsFor((scenes / "recipes" / "r.scene").string());
    Check(l.Ok() && l.table.size() == 3 && l.files.size() == 2, "both files are read");
    Check(l.table.Find("shared") && l.table.Find("local"), "the spots of both");
    CheckNum(l.table.Find("both")->x, 6.0, "the local file is on top");
    Check(!SpotsChanged((scenes / "recipes" / "r.scene").string(), l), "nothing changed yet");
    Write(scenes / "recipes" / "spots.txt", "local 7 7 7 7\nboth 6 6 6 6\nmore 1 1 1 1\n");
    Check(SpotsChanged((scenes / "recipes" / "r.scene").string(), l), "a changed file is noticed (the size)");
    l = LoadSpotsFor((scenes / "recipes" / "r.scene").string());
    Check(!SpotsChanged((scenes / "recipes" / "r.scene").string(), l) && l.table.Find("more"), "...and read again");
    std::error_code ec;
    fs::remove(scenes / "recipes" / "spots.txt", ec);
    Check(SpotsChanged((scenes / "recipes" / "r.scene").string(), l), "a file that went away is noticed");
    Write(scenes / "recipes" / "spots.txt", "bad line here\nok 1 2 3 4\n");
    l = LoadSpotsFor((scenes / "recipes" / "r.scene").string());
    Check(!l.Ok() && l.problems.size() == 1 && l.problems[0].find("spots.txt:1: error:") != std::string::npos, "a bad line is a problem with its place");
    Check(l.table.Find("ok") && l.table.Find("shared"), "...and the good lines still count");

    // the whole way: a scene file next to a table, through the parser
    Write(scenes / "recipes" / "spots.txt", "here 10 20 30 90\n");
    const LoadedSpots w = LoadSpotsFor((scenes / "recipes" / "r.scene").string());
    ParseOptions o;
    o.spots = &w.table;
    o.path = (scenes / "recipes" / "r.scene").string();
    const ParseResult r = ParseScene("scene r\nat 1 car p1 spawn vp_x at spot:here@0,0,5 heading spot:here\nat 2 car p2 spawn vp_x at spot:shared\nend 4\n", o);
    Check(r.Ok(), "a scene in recipes resolves the local and the shared spot");
    CheckEq(WordsOf(r, Track::kCar, 0), "p1 spawn vp_x at 15 20 30 heading 90", "...here faces +X, so 5 m ahead is +5 in x");
    CheckEq(WordsOf(r, Track::kCar, 1), "p2 spawn vp_x at 1 2 3", "...the shared one");
    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------------------------------------------- command line

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
        const LoadedSpots spots = LoadSpotsFor(argv[i]);
        for (const std::string& p : spots.problems) std::printf("%s\n", p.c_str());
        ParseOptions o;
        o.spots = &spots.table;
        o.path = argv[i];
        const ParseResult r = ParseScene(text, o);
        for (const Diagnostic& d : r.diagnostics) std::printf("%s\n", FormatDiagnostic(d, argv[i]).c_str());
        for (const Event& e : r.scene.events) {
            if (e.track == Track::kCamera || e.track == Track::kCar || e.track == Track::kStart) {
                std::printf("  t=%7.2f line %-4d %s %s\n", e.t, e.line, TrackName(e.track), Join(e.words).c_str());
            }
        }
        std::printf("%s: %d error(s), %d warning(s); %zu spots from %zu file(s)\n", argv[i], r.ErrorCount(), r.WarningCount(), spots.table.size(), spots.files.size());
        if (!r.Ok() || !spots.Ok()) rc = 1;
    }
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) return ParseFiles(argc, argv);
    TestNames();
    TestRef();
    TestResolve();
    TestTable();
    TestExpand();
    TestScene();
    TestCarVerbs();
    TestParseSpots();
    TestFiles();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
