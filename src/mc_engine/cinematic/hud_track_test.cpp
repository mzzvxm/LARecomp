// A check of the HUD track (hud_track.h) that needs no game.
//   cmake --build <dir> --target larecomp_hud_track_test, then run it (exit 0 = passed).
// Or by hand: clang++ -std=c++20 -I src/mc_engine/cinematic src/mc_engine/cinematic/hud_track_test.cpp
//             src/mc_engine/cinematic/hud_track.cpp -o hud_track_test

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "hud_track.h"

namespace {

using mc::cinematic::HudKey;
using mc::cinematic::HudTrack;
using mc::cinematic::ParseHudKey;

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const char* what, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAILED line %d: %s\n", line, what);
    }
}
#define CHECK(cond) Check((cond), #cond, __LINE__)

bool Near(float a, float b, float eps = 1e-5f) {
    return std::fabs(a - b) <= eps;
}

std::vector<std::string> Words(const char* text) {
    std::vector<std::string> out;
    std::string word;
    for (const char* p = text;; ++p) {
        if (*p == ' ' || *p == '\0') {
            if (!word.empty()) {
                out.push_back(word);
                word.clear();
            }
            if (*p == '\0') {
                break;
            }
        } else {
            word += *p;
        }
    }
    return out;
}

// Parses `text` for a line at scene time `at`; the key, or a note of the message when it is refused.
bool Parse(const char* text, double at, HudKey* key, std::string* error = nullptr) {
    return ParseHudKey(Words(text), at, key, error);
}

bool Refused(const char* text) {
    HudKey key;
    std::string error;
    const bool ok = Parse(text, 0.0, &key, &error);
    return !ok && !error.empty();
}

}  // namespace

int main() {
    // ---- parsing: the simple keys
    {
        HudKey k;
        CHECK(Parse("off", 3.0, &k));
        CHECK(k.t == 3.0 && k.end == 3.0 && k.alpha == 0.0f);
        CHECK(Parse("on", 4.5, &k));
        CHECK(k.t == 4.5 && k.end == 4.5 && k.alpha == 1.0f);
        CHECK(Parse("OFF", 1.0, &k) && k.alpha == 0.0f);
        CHECK(Parse("0.5", 2.0, &k));
        CHECK(k.t == 2.0 && k.alpha == 0.5f);
        CHECK(Parse("0", 0.0, &k) && k.alpha == 0.0f);
        CHECK(Parse("1", 0.0, &k) && k.alpha == 1.0f);
    }
    // ---- parsing: fades
    {
        HudKey k;
        CHECK(Parse("fade 1.5..2.0 to 1", 0.0, &k));
        CHECK(k.t == 1.5 && k.end == 2.0 && k.alpha == 1.0f);
        CHECK(Parse("fade 1.5..2.0 to 1", 9.0, &k));  // the line's own time only orders it
        CHECK(k.t == 1.5 && k.end == 2.0);
        CHECK(Parse("fade 0..3 to off", 0.0, &k));
        CHECK(k.t == 0.0 && k.end == 3.0 && k.alpha == 0.0f);
        CHECK(Parse("fade 2..2 to 0.25", 0.0, &k));  // a fade of no length is a jump
        CHECK(k.t == 2.0 && k.end == 2.0 && k.alpha == 0.25f);
        CHECK(Parse("fade over 2 to off", 5.0, &k));
        CHECK(k.t == 5.0 && k.end == 7.0 && k.alpha == 0.0f);
        CHECK(Parse("Fade Over 0.5 To On", 1.0, &k));
        CHECK(k.t == 1.0 && k.end == 1.5 && k.alpha == 1.0f);
        CHECK(Parse("fade over 0 to 0.5", 1.0, &k));
        CHECK(k.t == 1.0 && k.end == 1.0);
    }
    // ---- parsing: what is refused, with a message
    {
        CHECK(Refused(""));
        CHECK(Refused("fade"));
        CHECK(Refused("banana"));
        CHECK(Refused("off now"));
        CHECK(Refused("2"));
        CHECK(Refused("-0.1"));
        CHECK(Refused("0.5x"));
        CHECK(Refused("nan"));
        CHECK(Refused("fade 2..1 to 0"));
        CHECK(Refused("fade 1..2 0"));
        CHECK(Refused("fade 1..2 to 3"));
        CHECK(Refused("fade 1..2 to"));
        CHECK(Refused("fade 1.5 to 0"));
        CHECK(Refused("fade a..b to 0"));
        CHECK(Refused("fade 1.. to 0"));
        CHECK(Refused("fade ..2 to 0"));
        CHECK(Refused("fade -1..2 to 0"));
        CHECK(Refused("fade over -1 to 0"));
        CHECK(Refused("fade over x to 0"));
        CHECK(Refused("fade over 2 0"));
        CHECK(Refused("fade over 2 to 0 now"));
        // The message names the problem, and no key is made.
        HudKey k;
        k.alpha = 0.123f;
        std::string error;
        CHECK(!Parse("fade 2..1 to 0", 0.0, &k, &error));
        CHECK(error.find("ends before it starts") != std::string::npos);
        CHECK(k.alpha == 0.123f);
        CHECK(!Parse("2", 0.0, &k, &error));
        CHECK(error.find("outside 0 to 1") != std::string::npos);
        // A null error pointer is fine.
        CHECK(!ParseHudKey(Words("nope"), 0.0, &k, nullptr));
    }
    // ---- the track: empty, and a single jump
    {
        HudTrack t;
        CHECK(t.empty());
        CHECK(t.AlphaAt(0.0) == 1.0f);
        CHECK(t.AlphaAt(100.0) == 1.0f);
        CHECK(t.End() == 0.0);
        HudKey k;
        CHECK(Parse("off", 2.0, &k));
        t.Add(k);
        CHECK(!t.empty());
        CHECK(t.AlphaAt(0.0) == 1.0f);
        CHECK(t.AlphaAt(1.999) == 1.0f);
        CHECK(t.AlphaAt(2.0) == 0.0f);
        CHECK(t.AlphaAt(50.0) == 0.0f);
        CHECK(t.End() == 2.0);
    }
    // ---- the track: `hud off` at 0 and a fade back in, as the design sketch writes it
    {
        HudTrack t;
        HudKey k;
        CHECK(Parse("off", 0.0, &k));
        t.Add(k);
        CHECK(Parse("fade 1.5..2.0 to 1", 0.0, &k));
        t.Add(k);
        CHECK(t.AlphaAt(0.0) == 0.0f);
        CHECK(t.AlphaAt(1.0) == 0.0f);
        CHECK(t.AlphaAt(1.5) == 0.0f);
        CHECK(Near(t.AlphaAt(1.75), 0.5f));
        CHECK(Near(t.AlphaAt(2.0), 1.0f));
        CHECK(t.AlphaAt(30.0) == 1.0f);
        CHECK(t.End() == 2.0);
    }
    // ---- the track: a fade starts from the value in force, a later key takes over mid-fade
    {
        HudTrack t;
        HudKey k;
        CHECK(Parse("fade 0..4 to 0", 0.0, &k));  // 1 -> 0 over 4 s
        t.Add(k);
        CHECK(Near(t.AlphaAt(1.0), 0.75f));
        CHECK(Near(t.AlphaAt(2.0), 0.5f));
        CHECK(Parse("fade 2..3 to 1", 0.0, &k));  // from 0.5 at 2 s back to 1 at 3 s
        t.Add(k);
        CHECK(Near(t.AlphaAt(1.0), 0.75f));  // the first fade as before
        CHECK(Near(t.AlphaAt(2.0), 0.5f));
        CHECK(Near(t.AlphaAt(2.5), 0.75f));
        CHECK(Near(t.AlphaAt(3.0), 1.0f));
        CHECK(Near(t.AlphaAt(10.0), 1.0f));
        CHECK(Parse("0.2", 6.0, &k));  // a jump to 0.2 after both
        t.Add(k);
        CHECK(Near(t.AlphaAt(5.9), 1.0f));
        CHECK(Near(t.AlphaAt(6.0), 0.2f));
        CHECK(t.End() == 6.0);
    }
    // ---- the track: keys added out of order are sorted, equal times keep the order given (the last wins)
    {
        HudTrack t;
        HudKey k;
        CHECK(Parse("off", 4.0, &k));
        t.Add(k);
        CHECK(Parse("on", 1.0, &k));
        t.Add(k);
        CHECK(Parse("0.5", 4.0, &k));
        t.Add(k);
        CHECK(t.keys().size() == 3);
        CHECK(t.keys()[0].t == 1.0 && t.keys()[1].t == 4.0 && t.keys()[2].t == 4.0);
        CHECK(t.keys()[1].alpha == 0.0f && t.keys()[2].alpha == 0.5f);
        CHECK(t.AlphaAt(0.5) == 1.0f);
        CHECK(t.AlphaAt(3.9) == 1.0f);
        CHECK(t.AlphaAt(4.0) == 0.5f);
        t.Clear();
        CHECK(t.empty() && t.AlphaAt(4.0) == 1.0f);
    }
    // ---- a runner that steps the track every frame at 60 fps sees every value inside 0..1 and no jump bigger than a step
    {
        HudTrack t;
        HudKey k;
        CHECK(Parse("fade 1..3 to 0", 0.0, &k));
        t.Add(k);
        CHECK(Parse("fade over 2 to 1", 5.0, &k));
        t.Add(k);
        bool inside = true;
        float biggest = 0.0f;
        float last = t.AlphaAt(0.0);
        for (int i = 1; i <= 600; ++i) {
            const float v = t.AlphaAt(i / 60.0);
            inside = inside && v >= 0.0f && v <= 1.0f;
            biggest = std::fmax(biggest, std::fabs(v - last));
            last = v;
        }
        CHECK(inside);
        CHECK(biggest < 0.0085f);  // 1 / (2 s * 60) = 0.00833
        CHECK(Near(last, 1.0f));
    }

    std::printf("hud_track_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
