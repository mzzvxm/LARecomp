// Check of the CAR SELF-REFLECTIONS row's values and how it reads the two settings back
// (pause_selfrefl_row.h). A program of its own: it needs no game, no ReXGlue and no GPU, and exits 0 when
// every check holds.
//
//   cmake --build <build dir> --target larecomp_pause_selfrefl_test     (CMakeLists.txt)
//   <build dir>/larecomp_pause_selfrefl_test
//
// or, with no build directory:
//   clang++ -std=c++20 -I src/mc_engine/menus src/mc_engine/menus/pause_selfrefl_row_test.cpp -o pause_selfrefl_row_test

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "pause_selfrefl_row.h"

using namespace mcla_menu::selfrefl;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

bool Same(const char* a, const char* b) {
  if (!a || !b) return a == b;
  return std::strcmp(a, b) == 0;
}

void Values() {
  Check(Count(false) == 3 && Count(true) == 4, "AUTO, OFF, ON, and RAY TRACED only where it can run");
  const Choice order[] = {Choice::kAuto, Choice::kOff, Choice::kOn, Choice::kRayTraced};
  for (int i = 0; i < 4; ++i) {
    Check(At(i) == order[i], "value " + std::to_string(i) + " is in the order AUTO, OFF, ON, RAY TRACED");
    Check(IndexOf(order[i]) == i, "IndexOf is At's inverse at " + std::to_string(i));
  }
  Check(At(-1) == Choice::kAuto, "a step below the list is AUTO");
}

void Writes_() {
  Check(Same(WritesOf(Choice::kAuto).selfrefl, "auto") && Same(WritesOf(Choice::kAuto).rt, "auto"), "AUTO: auto, auto");
  Check(Same(WritesOf(Choice::kOff).selfrefl, "off") && WritesOf(Choice::kOff).rt == nullptr,
        "OFF: off, the ray tracing setting is left as it was");
  Check(Same(WritesOf(Choice::kOn).selfrefl, "on") && Same(WritesOf(Choice::kOn).rt, "off"), "ON: on, rt off");
  Check(Same(WritesOf(Choice::kRayTraced).selfrefl, "on") && Same(WritesOf(Choice::kRayTraced).rt, "auto"),
        "RAY TRACED: on, rt auto");
}

// Every value reads back as itself from what it writes (the ray tracing setting left over from before
// included), whatever the graphics card says of auto, where the value is listed.
void RoundTrip() {
  const char* const rt_before[] = {"auto", "off"};
  for (const bool auto_on : {false, true}) {
    for (const bool rt_available : {false, true}) {
      for (int i = 0; i < Count(rt_available); ++i) {
        const Choice choice = At(i);
        const Writes w = WritesOf(choice);
        for (const char* before : rt_before) {
          const char* rt = w.rt ? w.rt : before;
          const Choice back = ChoiceOf(w.selfrefl, rt, auto_on, rt_available);
          // AUTO with auto_on false still reads as AUTO: the pair is auto and auto.
          Check(back == choice, std::string("round trip of value ") + std::to_string(i) + " (auto_on " +
                                    (auto_on ? "1" : "0") + ", rt " + (rt_available ? "1" : "0") + ", rt was " + before + ")");
        }
      }
    }
  }
}

// The pairs a larecomp.toml can hold that the row does not write.
void ReadBack() {
  // auto with rt off: what runs is the screen-space reflection where auto is on, nothing where it is off.
  Check(ChoiceOf("auto", "off", true, true) == Choice::kOn, "auto, rt off, auto on: ON");
  Check(ChoiceOf("auto", "off", true, false) == Choice::kOn, "auto, rt off, auto on, no ray tracing: ON");
  Check(ChoiceOf("auto", "off", false, true) == Choice::kOff, "auto, rt off, auto off: OFF");
  Check(ChoiceOf("auto", "off", false, false) == Choice::kOff, "auto, rt off, auto off, no ray tracing: OFF");
  // off wins over any ray tracing setting.
  Check(ChoiceOf("off", "auto", true, true) == Choice::kOff, "off, rt auto: OFF");
  Check(ChoiceOf("off", "off", true, true) == Choice::kOff, "off, rt off: OFF");
  // on with rt auto: ray traced where it can run, else the screen-space reflection.
  Check(ChoiceOf("on", "auto", true, true) == Choice::kRayTraced, "on, rt auto, can run: RAY TRACED");
  Check(ChoiceOf("on", "auto", true, false) == Choice::kOn, "on, rt auto, cannot run: ON");
  Check(ChoiceOf("on", "off", true, true) == Choice::kOn, "on, rt off: ON");
  // a word that is no value is auto, as the settings read it.
  Check(ChoiceOf("low", "auto", true, true) == Choice::kAuto, "an old word for selfrefl is auto");
  Check(ChoiceOf("auto", "maybe", true, true) == Choice::kAuto, "a word that is no value for rt is auto");
  Check(ChoiceOf("", "", false, false) == Choice::kAuto, "empty texts are auto");
  // The index of every state the row can show is in the list that is listed.
  for (const bool rt_available : {false, true})
    for (const char* s : {"auto", "off", "on", "low", ""})
      for (const char* r : {"auto", "off", ""})
        for (const bool auto_on : {false, true})
          Check(IndexOf(ChoiceOf(s, r, auto_on, rt_available)) < Count(rt_available),
                std::string("the value of ") + s + "/" + r + " is listed");
}

void Labels() {
  Check(AutoLabel(true, true) == "AUTO (RAY TRACED)", "AUTO where it traces");
  Check(AutoLabel(true, false) == "AUTO (ON)", "AUTO on a card that cannot trace");
  Check(AutoLabel(false, true) == "AUTO (OFF)", "AUTO off on an integrated GPU, even where it could trace");
  Check(AutoLabel(false, false) == "AUTO (OFF)", "AUTO off");
  Check(LabelOf(Choice::kOff, true, true) == "OFF" && LabelOf(Choice::kOn, true, true) == "ON" &&
            LabelOf(Choice::kRayTraced, true, true) == "RAY TRACED" &&
            LabelOf(Choice::kAuto, true, true) == "AUTO (RAY TRACED)",
        "the four labels");
  // Width: "CAR SELF-REFLECTIONS" 20 and the longest value must stay under the 41 a page can take.
  size_t widest = 0;
  for (const bool a : {false, true})
    for (const bool r : {false, true})
      for (int i = 0; i < 4; ++i) widest = std::max(widest, LabelOf(At(i), a, r).size());
  Check(widest == 17 && 20 + widest <= 41, "the widest value is 17 and the row fits the page");
}

}  // namespace

int main() {
  Values();
  Writes_();
  RoundTrip();
  ReadBack();
  Labels();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
