// Check of the scene file's line-addressed edits (scene_edit.h). A program of its own: it needs no game and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_scene_edit_test      (CMakeLists.txt)
//   <build dir>/larecomp_scene_edit_test
//
// The compile by hand (the msvc-target clang of labuild):
//   clang++ -std=c++23 -I src/mc_engine/cinematic -I src/mc_engine/cinematic/director scene_edit_test.cpp scene_edit.cpp json.cpp ../scene.cpp -o scene_edit_test.exe

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

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

std::string Show(std::string s) {
    std::string out;
    for (char c : s) {
        if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out.push_back(c);
    }
    return out;
}

EditOp Insert(const std::string& text) {
    EditOp o;
    o.kind = EditKind::kInsert;
    o.text = text;
    return o;
}
EditOp Replace(int line, const std::string& expect, const std::string& text) {
    EditOp o;
    o.kind = EditKind::kReplace;
    o.line = line;
    o.expect = expect;
    o.text = text;
    return o;
}
EditOp Delete(int line, const std::string& expect) {
    EditOp o;
    o.kind = EditKind::kDelete;
    o.line = line;
    o.expect = expect;
    return o;
}
EditOp Move(int line, const std::string& expect, double t) {
    EditOp o;
    o.kind = EditKind::kMove;
    o.line = line;
    o.expect = expect;
    o.has_t = true;
    o.t = t;
    return o;
}
EditOp Param(int line, const std::string& expect, const std::string& key, const std::string& value) {
    EditOp o;
    o.kind = EditKind::kParam;
    o.line = line;
    o.expect = expect;
    o.key = key;
    o.value = value;
    return o;
}

const char* kScene =
    "# a trailer\n"
    "scene demo fps 30\n"
    "world hour 14 hold\n"
    "\n"
    "# the car\n"
    "at 0 car p1 spawn vp_lam_murcielago_06 at 819.6,17.6,-808.4 heading 62\n"
    "cam 0 pos 800 20 -820 look car:p1 fov 40 # wide\n"
    "cam 4 pos 810 19 -815 look car:p1 fov 40 ease inout\n"
    "cut 4\n"
    "at 6 hour 14..18 over 4\n"
    "end 10\n";

// ---------------------------------------------------------------------------------------------------------------- statements

void TestStatements() {
    StatementInfo s = ReadStatement("cam 4 pos 1 2 3");
    Check(s.is_statement && s.has_time && s.t == 4.0 && s.verb == "cam" && s.track == "camera", "cam statement");
    CheckEq(std::string("cam 4 pos 1 2 3").substr(s.time_begin, s.time_end - s.time_begin), "4", "cam time word");
    s = ReadStatement("  cut 1:30.5");
    Check(s.has_time && std::fabs(s.t - 90.5) < 1e-9 && s.track == "camera" && s.verb == "cut", "cut with minutes");
    s = ReadStatement("at 3.5 car p2 spawn x at 1,2,3");
    Check(s.has_time && s.t == 3.5 && s.verb == "car" && s.track == "car:p2", "at car track is per car");
    s = ReadStatement("at 3 set mcla_gpu_ao high");
    Check(s.track == "set:mcla_gpu_ao", "at set track is per setting");
    s = ReadStatement("at 6 hour 14..18 over 4 # note");
    Check(s.has_time && s.track == "hour", "at hour");
    s = ReadStatement("scene demo fps 30");
    Check(s.is_statement && !s.has_time && s.verb == "scene", "header has no time");
    s = ReadStatement("   # just a comment");
    Check(!s.is_statement, "a comment is not a statement");
    s = ReadStatement("");
    Check(!s.is_statement, "blank is not a statement");
    s = ReadStatement("end 10");
    Check(s.has_time && s.track == "end" && s.t == 10.0, "end");
    double t = 0;
    Check(ParseSceneTime("3", &t) && t == 3 && ParseSceneTime("3.25", &t) && t == 3.25 && ParseSceneTime("1:30", &t) && t == 90 && ParseSceneTime("0:05.5", &t) && t == 5.5, "times");
    Check(!ParseSceneTime("", &t) && !ParseSceneTime("abc", &t) && !ParseSceneTime("1:", &t) && !ParseSceneTime("-1", &t) && !ParseSceneTime("1:2:3", &t) && !ParseSceneTime("1.2.3", &t), "bad times");
    CheckEq(FormatSceneTime(3.0), "3", "format whole");
    CheckEq(FormatSceneTime(3.2), "3.2", "format 3.2");
    CheckEq(FormatSceneTime(3.14159), "3.142", "format rounds to 3 decimals");
    CheckEq(FormatSceneTime(0.0), "0", "format zero");
    CheckEq(FormatSceneTime(90.5), "90.5", "format over a minute stays seconds");
}

// ---------------------------------------------------------------------------------------------------------------- the ops

void TestInsert() {
    // placed after the last camera line at or before 5: line 8 (cam 4) ... `cut 4` is the same track (camera) and later in the file
    EditResult r = SceneEdit::Apply(kScene, {Insert("cam 5 pos 1 2 3 yaw 0 pitch 0")}, 7);
    Check(r.ok && r.rev == 8, "insert ok, rev + 1");
    CheckEq(r.text,
            "# a trailer\nscene demo fps 30\nworld hour 14 hold\n\n# the car\nat 0 car p1 spawn vp_lam_murcielago_06 at 819.6,17.6,-808.4 heading 62\n"
            "cam 0 pos 800 20 -820 look car:p1 fov 40 # wide\ncam 4 pos 810 19 -815 look car:p1 fov 40 ease inout\ncut 4\n"
            "cam 5 pos 1 2 3 yaw 0 pitch 0\nat 6 hour 14..18 over 4\nend 10\n",
            "insert after the last camera line at or before its time");
    Check(r.lines.size() == 1 && r.lines[0] == 10, "the new line's number");
    Check(r.changed.size() == 1 && r.changed[0] == 10, "changed = the new line");

    r = SceneEdit::Apply(kScene, {Insert("cam 2 pos 1 2 3 yaw 0 pitch 0")}, 1);
    Check(r.ok, "insert 2");
    // after `cam 0 ...` (line 7), before `cam 4`
    Check(r.lines.size() == 1 && r.lines[0] == 8, "a camera key at 2 goes after the one at 0");

    r = SceneEdit::Apply(kScene, {Insert("at 2 hour 15")}, 1);
    // the hour track: `at 6 hour` is after it: no hour line at or before 2 -> after the header (the `world` line, 3)
    Check(r.ok && r.lines.size() == 1 && r.lines[0] == 4, "no key of that track yet: after the header's last line");

    r = SceneEdit::Apply(kScene, {Insert("at 7 hour 17")}, 1);
    Check(r.ok && r.lines[0] == 11, "after the hour key at 6 (line 10 + the new one at 11)");

    r = SceneEdit::Apply(kScene, {Insert("at 1 car p2 spawn vp_x at 1,2,3")}, 1);
    // car tracks are per car id: p2 has none -> after the header
    Check(r.ok && r.lines[0] == 4, "a new car's first key goes after the header");
    r = SceneEdit::Apply(kScene, {Insert("at 3 car p1 stop")}, 1);
    Check(r.ok && r.lines[0] == 7, "a key of an existing car goes after its last key at or before it (line 6)");

    r = SceneEdit::Apply(kScene, {Insert("cam 3 pos 1 2 3\ncut 3")}, 1);
    Check(r.ok && r.lines.size() == 2 && r.lines[1] == r.lines[0] + 1, "two lines go in together");

    r = SceneEdit::Apply(kScene, {Insert("statelog on")}, 1);
    Check(r.ok && r.lines[0] == 4, "a header line goes after the header");

    r = SceneEdit::Apply("", {Insert("scene x")}, 0);
    CheckEq(r.text, "scene x\n", "insert into an empty file");
    r = SceneEdit::Apply("# only a comment\n", {Insert("cam 1 pos 1 2 3")}, 0);
    CheckEq(r.text, "# only a comment\ncam 1 pos 1 2 3\n", "no statement: at the end");
    r = SceneEdit::Apply("# c\nscene x\n", {Insert("at 1 hour 12")}, 0);
    CheckEq(r.text, "# c\nscene x\nat 1 hour 12\n", "after scene");
    r = SceneEdit::Apply(kScene, {Insert("   ")}, 1);
    Check(!r.ok && r.code == ErrorCode::kBadArgs, "blank insert is bad_args");
}

void TestReplaceDeleteMove() {
    const std::string cam4 = "cam 4 pos 810 19 -815 look car:p1 fov 40 ease inout";
    EditResult r = SceneEdit::Apply(kScene, {Replace(8, cam4, "cam 4 pos 1 1 1 look car:p1 fov 30")}, 1);
    Check(r.ok && r.lines.size() == 1 && r.lines[0] == 8, "replace");
    Check(r.text.find("cam 4 pos 1 1 1 look car:p1 fov 30\ncut 4\n") != std::string::npos && r.text.find("ease inout") == std::string::npos, "replace text");

    r = SceneEdit::Apply(kScene, {Delete(9, "cut 4")}, 1);
    Check(r.ok && r.lines.empty() && r.text.find("cut 4") == std::string::npos, "delete");
    Check(r.changed.empty(), "a deletion adds no changed line");
    CheckEq(r.text.substr(0, 19), "# a trailer\nscene d", "comments kept");

    r = SceneEdit::Apply(kScene, {Move(8, cam4, 4.5)}, 1);
    Check(r.ok && r.text.find("cam 4.5 pos 810 19 -815 look car:p1 fov 40 ease inout\n") != std::string::npos, "move rewrites only the time word");
    r = SceneEdit::Apply(kScene, {Move(10, "at 6 hour 14..18 over 4", 7)}, 1);
    Check(r.ok && r.text.find("at 7 hour 14..18 over 4\n") != std::string::npos, "move an at line");
    r = SceneEdit::Apply(kScene, {Move(11, "end 10", 12.25)}, 1);
    Check(r.ok && r.text.find("end 12.25\n") != std::string::npos, "move end");
    r = SceneEdit::Apply(kScene, {Move(2, "scene demo fps 30", 1)}, 1);
    Check(!r.ok && r.code == ErrorCode::kBadArgs, "a line with no time word cannot move");
    r = SceneEdit::Apply(kScene, {Move(7, "cam 0 pos 800 20 -820 look car:p1 fov 40 # wide", 1)}, 1);
    Check(r.ok && r.text.find("cam 1 pos 800 20 -820 look car:p1 fov 40 # wide\n") != std::string::npos, "the comment stays on a moved line");
}

void TestParam() {
    const std::string cam0 = "cam 0 pos 800 20 -820 look car:p1 fov 40 # wide";
    EditResult r = SceneEdit::Apply(kScene, {Param(7, cam0, "fov", "35")}, 1);
    Check(r.ok, "param ok");
    Check(r.text.find("cam 0 pos 800 20 -820 look car:p1 fov 35 # wide\n") != std::string::npos, "param rewrites one word pair, comment kept");
    r = SceneEdit::Apply(kScene, {Param(7, cam0, "pos", "1 2 3")}, 1);
    Check(r.ok && r.text.find("cam 0 pos 1 2 3 look car:p1 fov 40 # wide\n") != std::string::npos, "param with three words replaces three");
    r = SceneEdit::Apply(kScene, {Param(7, cam0, "roll", "5")}, 1);
    Check(r.ok && r.text.find("cam 0 pos 800 20 -820 look car:p1 fov 40 roll 5 # wide\n") != std::string::npos, "param appends a missing key before the comment");
    r = SceneEdit::Apply(kScene, {Param(7, cam0, "look", "car:p2")}, 1);
    Check(r.ok && r.text.find("look car:p2 fov 40") != std::string::npos, "param on a word pair in the middle");
    r = SceneEdit::Apply("cam 1 pos 1 2 3 fov\n", {Param(1, "cam 1 pos 1 2 3 fov", "fov", "40")}, 1);
    CheckEq(r.text, "cam 1 pos 1 2 3 fov 40\n", "a key at the end of the line gets its value");
    r = SceneEdit::Apply(kScene, {Param(7, cam0, "fov", "")}, 1);
    Check(!r.ok && r.code == ErrorCode::kBadArgs, "empty value is bad_args");
    r = SceneEdit::Apply(kScene, {Param(7, cam0, "fo v", "1")}, 1);
    Check(!r.ok && r.code == ErrorCode::kBadArgs, "a key of two words is bad_args");
}

void TestStaleAndNumbers() {
    EditResult r = SceneEdit::Apply(kScene, {Delete(9, "cut 5")}, 1);
    Check(!r.ok && r.code == ErrorCode::kStale && r.failed_op == 0, "a wrong expect is stale");
    Check(r.details.NumberOr("line", 0) == 9 && r.details.StringOr("actual", "") == "cut 4", "stale carries the line and its actual text");
    r = SceneEdit::Apply(kScene, {Delete(99, "x")}, 1);
    Check(!r.ok && r.code == ErrorCode::kBadArgs, "a line past the end is bad_args");
    r = SceneEdit::Apply(kScene, {Delete(0, "x")}, 1);
    Check(!r.ok && r.code == ErrorCode::kBadArgs, "line 0 is bad_args");
    r = SceneEdit::Apply(kScene, {}, 1);
    Check(!r.ok && r.code == ErrorCode::kBadArgs, "no ops is bad_args");
    // trailing blanks and CR in expect do not matter
    r = SceneEdit::Apply(kScene, {Delete(9, "cut 4   "), }, 1);
    Check(r.ok, "trailing blanks of expect do not matter");
    r = SceneEdit::Apply(kScene, {Delete(9, "cut 4\r")}, 1);
    Check(r.ok, "a trailing CR of expect does not matter");

    // line numbers are those of the text the caller read, whatever the earlier ops did
    r = SceneEdit::Apply(kScene, {Delete(6, "at 0 car p1 spawn vp_lam_murcielago_06 at 819.6,17.6,-808.4 heading 62"), Move(8, "cam 4 pos 810 19 -815 look car:p1 fov 40 ease inout", 5),
                                  Insert("at 3 hour 15")},
                         1);
    Check(r.ok, "several ops apply");
    Check(r.text.find("cam 5 pos 810") != std::string::npos && r.text.find("spawn vp_lam") == std::string::npos && r.text.find("at 3 hour 15") != std::string::npos,
          "numbering stays the original's after a delete");
    // all or none
    r = SceneEdit::Apply(kScene, {Delete(9, "cut 4"), Delete(10, "wrong")}, 1);
    Check(!r.ok && r.failed_op == 1, "the second op fails: nothing is applied (the failure names it)");
    r = SceneEdit::Apply(kScene, {Delete(9, "cut 4"), Delete(9, "cut 4")}, 1);
    Check(!r.ok && r.code == ErrorCode::kBadArgs && r.failed_op == 1, "an op on a line an earlier op deleted");
}

void TestLayout() {
    // CRLF and no final newline are kept
    const std::string crlf = "scene a\r\nat 1 hour 12\r\nend 5";
    EditResult r = SceneEdit::Apply(crlf, {Insert("at 2 hour 13")}, 1);
    CheckEq(Show(r.text), Show("scene a\r\nat 1 hour 12\r\nat 2 hour 13\r\nend 5"), "CRLF kept, no final newline kept");
    r = SceneEdit::Apply(crlf, {Param(2, "at 1 hour 12", "hour", "14")}, 1);
    CheckEq(Show(r.text), Show("scene a\r\nat 1 hour 14\r\nend 5"), "param on a CRLF file");
    r = SceneEdit::Apply("scene a\nend 5\n\n\n", {Insert("at 1 hour 12")}, 1);
    CheckEq(Show(r.text), Show("scene a\nat 1 hour 12\nend 5\n\n\n"), "blank lines at the end kept");
    // a quoted word with a '#' in it is not a comment
    r = SceneEdit::Apply("at 1 label \"a # b\" side left\n", {Param(1, "at 1 label \"a # b\" side left", "side", "right")}, 1);
    CheckEq(r.text, "at 1 label \"a # b\" side right\n", "a # inside quotes is not a comment");
}

void TestEditedTextParses() {
    EditResult r = SceneEdit::Apply(kScene, {Insert("cam 5 pos 800 20 -820 yaw 10 pitch 0 fov 40"), Move(10, "at 6 hour 14..18 over 4", 7), Param(8, "cam 4 pos 810 19 -815 look car:p1 fov 40 ease inout", "fov", "30")},
                                    1);
    Check(r.ok, "edit for the parser check");
    const ParseResult before = ParseScene(kScene);
    const ParseResult after = ParseScene(r.text);
    Check(before.Ok() && after.Ok(), "the edited scene parses");
    Check(after.scene.events.size() == before.scene.events.size() + 1, "one more event");
}

void TestOpsJson() {
    Json j;
    std::string err;
    EditOp op;
    Check(Json::Parse(R"({"op":"param","line":14,"expect":"x","key":"fov","value":35})", &j, &err) && EditOpFromJson(j, &op, &err) && op.kind == EditKind::kParam && op.value == "35" && op.line == 14,
          "param op from JSON (a number value)");
    Check(Json::Parse(R"({"op":"move","line":3,"expect":"x","t":2.5})", &j, &err) && EditOpFromJson(j, &op, &err) && op.has_t && op.t == 2.5, "move op from JSON");
    Check(Json::Parse(R"({"op":"insert","text":"a"})", &j, &err) && EditOpFromJson(j, &op, &err) && op.text == "a", "insert op from JSON");
    Check(Json::Parse(R"({"op":"replace","line":3,"expect":"x","text":"y"})", &j, &err) && EditOpFromJson(j, &op, &err) && op.text == "y", "replace op from JSON");
    const char* bad[] = {R"({"op":"teleport"})",
                         R"({"op":"insert"})",
                         R"({"op":"insert","text":"a","line":1})",
                         R"({"op":"delete","line":0,"expect":"x"})",
                         R"({"op":"delete","line":1.5,"expect":"x"})",
                         R"({"op":"delete","line":1})",
                         R"({"op":"move","line":1,"expect":"x","t":-1})",
                         R"({"op":"move","line":1,"expect":"x"})",
                         R"({"op":"param","line":1,"expect":"x","key":"fov"})",
                         R"([1])",
                         R"({})"};
    for (const char* b : bad) {
        Check(Json::Parse(b, &j, &err) && !EditOpFromJson(j, &op, &err) && !err.empty(), std::string("a bad op is refused: ") + b);
    }
    EditOp m = Move(3, "x", 2.5);
    Check(EditOpToJson(m).Dump() == R"({"op":"move","line":3,"expect":"x","t":2.5})", "an op as JSON");
}

void TestDiffAndHash() {
    Check(ChangedLines("a\nb\nc\n", "a\nb\nc\n").empty(), "same text: nothing changed");
    Check(ChangedLines("a\nb\nc\n", "a\nX\nc\n") == std::vector<int>{2}, "a changed line");
    Check(ChangedLines("a\nb\nc\n", "a\nb\nNEW\nc\n") == std::vector<int>{3}, "an inserted line");
    Check(ChangedLines("a\nb\nc\n", "a\nc\n").empty(), "a deleted line adds no changed line");
    Check(ChangedLines("a\nb\nc\nd\ne\n", "a\nc\nX\nd\ne\nY\n") == std::vector<int>{3, 6}, "several");
    Check(ChangedLines("", "a\nb\n") == std::vector<int>{1, 2}, "from empty");
    Check(ChangedLines("a\r\nb\r\n", "a\nb\n").empty(), "line endings are not a change");
    CheckEq(TextHash(""), "cbf29ce484222325", "FNV-1a of nothing");
    CheckEq(TextHash("a"), "af63dc4c8601ec8c", "FNV-1a of a");
    CheckEq(TextHash("foobar"), "85944171f73967e8", "FNV-1a of foobar");
}

}  // namespace

int main() {
    TestStatements();
    TestInsert();
    TestReplaceDeleteMove();
    TestParam();
    TestStaleAndNumbers();
    TestLayout();
    TestEditedTextParses();
    TestOpsJson();
    TestDiffAndHash();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
