// Check of the scene's `split` track (scene.h, trailer toolkit M5b): the statements the parser takes and refuses, and what EvaluateSplit
// says at a given scene time. A program of its own: it needs no game, no ReXGlue and no log, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_split_track_test      (CMakeLists.txt)
//   <build dir>/larecomp_split_track_test
//
// The compile by hand (the msvc-target clang of labuild, include path src/mc_engine/cinematic):
//   clang++ -std=c++23 -Wall -Wextra src/mc_engine/cinematic/split_track_test.cpp src/mc_engine/cinematic/scene.cpp -o split_track_test.exe

#include <cmath>
#include <cstdio>
#include <string>

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

void CheckNum(double got, double want, const std::string& what, double tol = 1e-9) {
    ++g_checks;
    if (!(std::fabs(got - want) <= tol)) {
        ++g_failed;
        std::printf("FAILED: %s: got %.9g, want %.9g\n", what.c_str(), got, want);
    }
}

const Diagnostic* Find(const ParseResult& r, const std::string& part) {
    for (const Diagnostic& d : r.diagnostics) {
        if (d.message.find(part) != std::string::npos) return &d;
    }
    return nullptr;
}

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

// The sweep scene of the toolkit: the split sits at 0.15, travels to 0.85 over 6 s (in and out), holds, then goes off.
constexpr const char* kSweep = R"(scene sweep fps 60
world hour 17.4 hold weather nice traffic off
at 0 set mcla_gpu_split_effects ao
at 1 split 0.15
at 2 split 0.15..0.85 over 6 ease inout
at 10 split 0.5
at 12 split off
end 14
)";

void TestParse() {
    const ParseResult r = ParseScene(kSweep);
    Check(r.Ok(), "the sweep scene parses without an error");
    CheckNum(r.WarningCount(), 0, "and without a warning");
    int splits = 0;
    for (const Event& e : r.scene.events) splits += e.track == Track::kSplit ? 1 : 0;
    Check(splits == 4, "four split keys (got " + std::to_string(splits) + ")");
    const Event* first = First(r.scene, Track::kSplit);
    Check(first && first->t == 1.0 && !first->has_from && first->to == 0.15 && !first->release, "a single position: the value is `to`");
    const Event* ramp = nullptr;
    const Event* off = nullptr;
    for (const Event& e : r.scene.events) {
        if (e.track != Track::kSplit) continue;
        if (e.has_from) ramp = &e;
        if (e.release) off = &e;
    }
    Check(ramp && ramp->from == 0.15 && ramp->to == 0.85 && ramp->over == 6.0 && ramp->ease == Ease::kInOut, "a ramp: from, to, over, ease");
    Check(off && off->t == 12.0, "`split off` releases");
    Check(Summarize(r.scene).find("split 4") != std::string::npos, "the summary counts the split keys");
    Check(std::string(TrackName(Track::kSplit)) == "split", "the track's name is split");
    // The statement also stands in a world-style `at` line with a minutes:seconds time, and is case-insensitive in its words.
    const ParseResult m = ParseScene("scene m\nat 0:02.5 SPLIT 0.3..0.6 OVER 1 EASE Out\nend 1:00\n");
    Check(m.Ok(), "words in any case, a time in m:ss");
    const Event* e = First(m.scene, Track::kSplit);
    Check(e && e->t == 2.5 && e->ease == Ease::kOut, "its time and ease");
    // Bare `split 0.5` is at time 0.
    const ParseResult bare = ParseScene("scene b\nsplit 0.5\nend 4\n");
    Check(bare.Ok() && First(bare.scene, Track::kSplit) && First(bare.scene, Track::kSplit)->t == 0.0, "a bare split is at time 0");
    // The ends of the range are allowed.
    Check(ParseScene("scene b\nat 1 split 0\nend 4\n").Ok(), "0 is a position (the effect nowhere)");
    Check(ParseScene("scene b\nat 1 split 1\nend 4\n").Ok(), "1 is a position (the effect everywhere)");
}

void TestErrors() {
    CheckError("at 1 split", "split needs a value");
    CheckError("at 1 split 1.5", "bad split position '1.5'");
    CheckError("at 1 split -0.1", "bad split position '-0.1'");
    CheckError("at 1 split left", "bad split position 'left'");
    CheckError("at 1 split 0.2..1.2 over 2", "bad split position '0.2..1.2'");
    CheckError("at 1 split 0.2..0.8", "a split ramp needs a length");
    CheckError("at 1 split 0.5 over 2", "a single split position does not move");
    CheckError("at 1 split 0.2..0.8 over 0", "bad length '0'");
    CheckError("at 1 split 0.2..0.8 over 2 ease wobble", "unknown ease 'wobble'");
    CheckError("at 1 split 0.2..0.8 over 2 speed 3", "split: unknown option 'speed'");
    CheckError("at 1 split 0.2..0.8 over", "'over' needs a value");
    CheckError("at 1 split off now", "split off takes nothing after it");
    // A ramp past the end is a warning like any other (the shared rule); a key past the end is an error.
    const ParseResult past = ParseScene("scene p\nat 8 split 0.1..0.9 over 4\nend 10\n");
    Check(past.Ok() && past.WarningCount() == 1 && Find(past, "past the scene's end"), "a ramp past the end warns");
    const ParseResult late = ParseScene("scene p\nat 12 split 0.5\nend 10\n");
    Check(!late.Ok() && Find(late, "after the scene's end"), "a key after the end is an error");
    // Two keys at one time: the later wins, with the warning the world tracks have.
    const ParseResult twice = ParseScene("scene p\nat 2 split 0.2\nat 2 split 0.8\nend 10\n");
    Check(twice.Ok() && Find(twice, "two split keys"), "two split keys at one time warn");
    // The verb list in the message of an unknown statement names split.
    const ParseResult dance = ParseScene("scene p\nat 1 dance\nend 10\n");
    Check(Find(dance, "split") != nullptr, "the list of statements names split");
}

void TestEvaluate() {
    const ParseResult r = ParseScene(kSweep);
    const Scene& s = r.scene;
    Check(!EvaluateSplit(s, 0.0).set, "before the first key there is no split");
    Check(!EvaluateSplit(s, 0.999).set, "still none just before it");
    Check(EvaluateSplit(s, 1.0).set, "at the key it is set");
    CheckNum(EvaluateSplit(s, 1.0).position, 0.15, "the held position");
    CheckNum(EvaluateSplit(s, 1.9).position, 0.15, "it holds until the next key");
    // The ramp: 0.15 -> 0.85 over 6 s from t = 2, in and out (smoothstep).
    CheckNum(EvaluateSplit(s, 2.0).position, 0.15, "the ramp starts at a");
    CheckNum(EvaluateSplit(s, 5.0).position, 0.5, "the middle of an in-and-out ramp is the middle");
    Check(EvaluateSplit(s, 3.0).position < 0.15 + 0.7 / 6.0, "an eased start is slower than a linear one");
    CheckNum(EvaluateSplit(s, 8.0).position, 0.85, "the ramp ends at b");
    CheckNum(EvaluateSplit(s, 9.99).position, 0.85, "and holds there");
    CheckNum(EvaluateSplit(s, 10.0).position, 0.5, "the next key is a jump");
    Check(EvaluateSplit(s, 11.9).set, "held to the off key");
    Check(!EvaluateSplit(s, 12.0).set, "`split off` ends the split");
    Check(!EvaluateSplit(s, 14.0).set, "and it stays off");
    // Monotonic along the ramp: a wipe never goes back.
    bool mono = true;
    double last = -1.0;
    for (int i = 0; i <= 600; ++i) {
        const SplitState st = EvaluateSplit(s, 2.0 + 6.0 * i / 600.0);
        mono = mono && st.set && st.position >= last - 1e-12;
        last = st.position;
    }
    Check(mono, "the ramp only moves one way");
    // A scene with no split key: nothing, at any time.
    const ParseResult none = ParseScene("scene n\nat 1 mark a\nend 3\n");
    Check(!EvaluateSplit(none.scene, 2.0).set, "a scene without split keys has no split");
    // A descending ramp.
    const ParseResult down = ParseScene("scene d\nat 0 split 0.9..0.1 over 4\nend 5\n");
    CheckNum(EvaluateSplit(down.scene, 2.0).position, 0.5, "a ramp may run right to left");
    CheckNum(EvaluateSplit(down.scene, 4.0).position, 0.1, "and ends at its b");
    // A ramp that is interrupted by the next key: the next key wins from its time.
    const ParseResult cut = ParseScene("scene c\nat 0 split 0.1..0.9 over 8\nat 4 split 0.2\nend 8\n");
    CheckNum(EvaluateSplit(cut.scene, 3.0).position, 0.4, "before the interrupting key the ramp is on its way");
    CheckNum(EvaluateSplit(cut.scene, 4.0).position, 0.2, "the interrupting key replaces the ramp");
    // The state compares by value (the runner writes the cvar only when it changed).
    Check(EvaluateSplit(s, 1.2) == EvaluateSplit(s, 1.8), "the same held position compares equal");
}

}  // namespace

int main() {
    TestParse();
    TestErrors();
    TestEvaluate();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
