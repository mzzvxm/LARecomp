// Check of the pause menu's "a row that depends on another row" rule (row_dependency.h) and of its first use: FOLIAGE SHADOWS, SHADOW DETAIL, SHADOW
// FILTER and TRAFFIC SHADOWS are grey while SHADOWS is off, and FOLIAGE SHADOWS sits right after SHADOWS on LIGHTING AND SHADOWS. A program of its own:
// it needs no game, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_row_dependency_test     (CMakeLists.txt)
//   <build dir>/larecomp_row_dependency_test
//
// or, with no build directory (from the repo root):
//   clang++ -std=c++20 -I src -I src/mc_engine src/mc_engine/menus/row_dependency_test.cpp -o row_dependency_test
//
// What it covers:
//   1. the helper: grey and the reason follow the row it needs live (no copy, both ways, any number of rows, two masters at once), the row's own
//      labels survive, and a row with no dependency is never grey;
//   2. the page: the tables in pause_menu.cpp (read as text, as menu_mouse_test.cpp reads menu_mouse.cpp) have FOLIAGE SHADOWS second on LIGHTING AND
//      SHADOWS and not on WORLD DETAIL, the four rows use NeedsShadows with ShadowsOn = !perf_no_shadows, FOLIAGE SHADOWS is on both renderers, the
//      page stays within 41 characters with the reason, and the setting keeps its default (true) and has the new help text;
//   3. the navigation rules, as pause_menu.cpp and menu_mouse_core.h state them: the cursor lands on a grey row (a move never skips a disabled one),
//      the mouse hovers and clicks onto it, and A / left / right / a click then do nothing, because the menu asks RowEnabled first (the gate is
//      in MenuFormAccept and PollNativeRows, found in the source text).

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "input/menu_mouse_core.h"
#include "menus/row_dependency.h"

namespace {

using mcla_menu::DependentRow;
using mcla_menu::ItemDef;
using mcla_menu::RowDynamics;
using mcla_menu::RowEnabled;
using mcla_menu::kNeedsShadowsReason;

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

// ---- Fake settings the rows depend on (what CvarGetBool reads in the game) -----------------------------------------------------------------------

bool g_perf_no_shadows = false;  // perf_no_shadows: true = SHADOWS is OFF
bool g_other_on = true;          // a second master, to see two rows with two masters do not share a state

bool ShadowsOn() { return !g_perf_no_shadows; }
bool OtherOn() { return g_other_on; }

constexpr char kOtherReason[] = " (NO OTHER)";

constexpr RowDynamics NeedsShadows(RowDynamics own = {}) { return DependentRow<ShadowsOn, kNeedsShadowsReason>(own); }
std::string DetailLabel(int i) { return i == 0 ? "AUTO (HIGH)" : "GAME"; }

constexpr RowDynamics kFoliageDyn = NeedsShadows();
constexpr RowDynamics kDetailDyn = NeedsShadows({.str_label = DetailLabel});
constexpr RowDynamics kOtherDyn = DependentRow<OtherOn, kOtherReason>();

// The page the real one is made of (pause_menu.cpp, kLightItems), with the same constructors: SHADOWS, FOLIAGE SHADOWS, SHADOW DETAIL, CONTACT SHADOWS.
const char* const kVals[] = {"auto", "game"};
const char* const kNames[] = {"AUTO", "GAME"};
const ItemDef kPage[] = {
    mcla_menu::Bool("PM_RxNoShadows", "perf_no_shadows", "SHADOWS: ", true),
    mcla_menu::WithDynamics(mcla_menu::Bool("PM_RxFoliage", "disable_imposter_shadows", "FOLIAGE SHADOWS: ", true), &kFoliageDyn),
    mcla_menu::WithDynamics(mcla_menu::Str("PM_RxGpuShadowDetail", "mcla_gpu_shadow_resolution", "SHADOW DETAIL: ", kVals, kNames, 2), &kDetailDyn),
    mcla_menu::Str("PM_RxGpuContact", "mcla_gpu_contact_shadows", "CONTACT SHADOWS: ", kVals, kNames, 2),
    mcla_menu::WithDynamics(mcla_menu::Bool("PM_RxOther", "other", "OTHER: "), &kOtherDyn),
};
constexpr int kPageRows = int(sizeof(kPage) / sizeof(kPage[0]));

// ItemRowLabel (pause_menu.cpp): the prefix without its trailing ": ", then the dynamic suffix.
std::string RowLabel(const ItemDef& it) {
  std::string label = it.prefix;
  while (!label.empty() && (label.back() == ' ' || label.back() == ':')) label.pop_back();
  if (it.dynamics && it.dynamics->suffix) return label + it.dynamics->suffix();
  return label + it.suffix;
}

// ---- 1. The helper -------------------------------------------------------------------------------------------------------------------------------

void Helper() {
  g_perf_no_shadows = false;
  g_other_on = true;
  for (int i : {1, 2}) {
    Check(RowEnabled(kPage[i]), std::string(kPage[i].key) + " is enabled while SHADOWS is on");
    Check(RowLabel(kPage[i]).find('(') == std::string::npos, std::string(kPage[i].key) + " has no reason while SHADOWS is on");
  }

  g_perf_no_shadows = true;  // SHADOWS: OFF
  Check(!RowEnabled(kPage[1]), "FOLIAGE SHADOWS is grey while SHADOWS is off");
  Check(!RowEnabled(kPage[2]), "SHADOW DETAIL is grey while SHADOWS is off");
  Check(RowLabel(kPage[1]) == "FOLIAGE SHADOWS (NO SHADOW)", "FOLIAGE SHADOWS says why: " + RowLabel(kPage[1]));
  Check(RowLabel(kPage[2]) == "SHADOW DETAIL (NO SHADOW)", "SHADOW DETAIL says why: " + RowLabel(kPage[2]));
  Check(RowEnabled(kPage[0]), "SHADOWS itself is never grey");
  Check(RowEnabled(kPage[3]), "CONTACT SHADOWS has no dependency on SHADOWS and stays enabled");
  Check(RowLabel(kPage[3]) == "CONTACT SHADOWS", "CONTACT SHADOWS has no reason");

  // Live, both ways, as often as the setting changes: nothing is cached.
  for (int n = 0; n < 4; ++n) {
    g_perf_no_shadows = (n % 2 == 0);
    Check(RowEnabled(kPage[1]) == !g_perf_no_shadows, "FOLIAGE SHADOWS follows SHADOWS at flip " + std::to_string(n));
    Check(RowEnabled(kPage[2]) == !g_perf_no_shadows, "SHADOW DETAIL follows SHADOWS at flip " + std::to_string(n));
    Check(std::strlen(kPage[1].dynamics->suffix()) == (g_perf_no_shadows ? std::strlen(kNeedsShadowsReason) : 0u),
          "the reason is there exactly while SHADOWS is off at flip " + std::to_string(n));
  }

  // Two masters do not share a state, and one row can have the other one's reason.
  g_perf_no_shadows = false;
  g_other_on = false;
  Check(RowEnabled(kPage[1]) && !RowEnabled(kPage[4]), "the second master greys only its own row");
  Check(RowLabel(kPage[4]) == "OTHER (NO OTHER)", "the second master has its own reason: " + RowLabel(kPage[4]));
  g_perf_no_shadows = true;
  g_other_on = true;
  Check(!RowEnabled(kPage[1]) && RowEnabled(kPage[4]), "and the first greys only its rows");

  // The row's own labels and list survive; only `enabled` and `suffix` are set.
  Check(kDetailDyn.str_label == DetailLabel && kDetailDyn.str_label(0) == "AUTO (HIGH)", "the row's own value labels are kept");
  Check(kFoliageDyn.str_label == nullptr && kFoliageDyn.value_label == nullptr && kFoliageDyn.value_count == nullptr,
        "a row with nothing of its own gets nothing else");
  Check(RowEnabled(mcla_menu::Bool("PM_x", "x", "X: ")), "a row with no dynamics at all is enabled");
  const RowDynamics plain = {.str_label = DetailLabel};
  const ItemDef no_enabled = mcla_menu::WithDynamics(mcla_menu::Bool("PM_y", "y", "Y: "), &plain);
  Check(RowEnabled(no_enabled), "a row whose dynamics have no `enabled` is enabled");

  // The reason is a short one: the same function is the page's width rule (below).
  Check(std::strlen(kNeedsShadowsReason) <= 12, "the shadows reason is at most 12 characters");
  Check(kNeedsShadowsReason[0] == ' ' && kNeedsShadowsReason[1] == '(' && kNeedsShadowsReason[std::strlen(kNeedsShadowsReason) - 1] == ')',
        "the reason is ' (...)' like every other suffix");
  g_perf_no_shadows = false;
}

// ---- 2. The page, read from the source -----------------------------------------------------------------------------------------------------------

std::string Here() {
  std::string path = __FILE__;
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

bool ReadFile(const std::string& path, std::string* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::stringstream text;
  text << in.rdbuf();
  *out = text.str();
  return true;
}

// The text with comments (`// ...` and `/* ... */`) removed, strings and character literals kept as they are (pause_menu.cpp has a '"' in one).
std::string StripLineComments(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  size_t i = 0;
  while (i < text.size()) {
    const char c = text[i];
    if (c == '"' || c == '\'') {
      // A string or a character literal, to its closing quote (a backslash takes the next character with it).
      out.push_back(c);
      ++i;
      while (i < text.size() && text[i] != c && text[i] != '\n') {
        if (text[i] == '\\' && i + 1 < text.size()) out.push_back(text[i++]);
        out.push_back(text[i++]);
      }
      if (i < text.size() && text[i] == c) out.push_back(text[i++]);
    } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
      while (i < text.size() && text[i] != '\n') ++i;
    } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
      const size_t close = text.find("*/", i + 2);
      i = close == std::string::npos ? text.size() : close + 2;
    } else {
      out.push_back(c);
      ++i;
    }
  }
  return out;
}

// The body of `const ItemDef <name>[] = { ... };` (comments stripped), "" when it is not there.
std::string TableBody(const std::string& text, const std::string& name) {
  const std::string head = "const ItemDef " + name + "[] = {";
  const size_t at = text.find(head);
  if (at == std::string::npos) return "";
  size_t i = at + head.size();
  int depth = 1;
  bool in_string = false;
  const size_t start = i;
  for (; i < text.size(); ++i) {
    const char c = text[i];
    if (in_string) {
      if (c == '\\') ++i;
      else if (c == '"') in_string = false;
    } else if (c == '"') {
      in_string = true;
    } else if (c == '{') {
      ++depth;
    } else if (c == '}') {
      if (--depth == 0) return text.substr(start, i - start);
    }
  }
  return "";
}

struct Row {
  std::string key;
  std::string dynamics;   // the `&kXxxDynamics` the row names, "" when none
  bool native_only = false;
};

// The rows of a table in order: each starts at its "PM_Rx..." key; the dynamics are the `&k...Dynamics` before the next key.
std::vector<Row> Rows(const std::string& body) {
  std::vector<Row> rows;
  std::vector<size_t> at;
  size_t pos = 0;
  while ((pos = body.find("\"PM_Rx", pos)) != std::string::npos) {
    at.push_back(pos);
    pos += 6;
  }
  for (size_t i = 0; i < at.size(); ++i) {
    Row r;
    const size_t end_key = body.find('"', at[i] + 1);
    r.key = body.substr(at[i] + 1, end_key - at[i] - 1);
    const size_t next = i + 1 < at.size() ? at[i + 1] : body.size();
    const std::string seg = body.substr(at[i], next - at[i]);
    const size_t amp = seg.find("&k");
    if (amp != std::string::npos) {
      size_t e = amp + 1;
      while (e < seg.size() && (std::isalnum(static_cast<unsigned char>(seg[e])) || seg[e] == '_')) ++e;
      r.dynamics = seg.substr(amp + 1, e - amp - 1);
    }
    // NativeOnly( is written before the row's constructor on the row's own first line, so it is in the line's text before the key.
    const size_t line_start = body.rfind('\n', at[i]);
    const std::string prefix = body.substr(line_start == std::string::npos ? 0 : line_start + 1,
                                           at[i] - (line_start == std::string::npos ? 0 : line_start + 1));
    r.native_only = prefix.find("NativeOnly(") != std::string::npos;
    rows.push_back(r);
  }
  return rows;
}

std::vector<std::string> Keys(const std::vector<Row>& rows) {
  std::vector<std::string> keys;
  for (const Row& r : rows) keys.push_back(r.key);
  return keys;
}

// `constexpr RowDynamics <name> = <init>;` -> <init>.
std::string DynamicsInit(const std::string& text, const std::string& name) {
  const std::string head = "constexpr RowDynamics " + name + " =";
  const size_t at = text.find(head);
  if (at == std::string::npos) return "";
  const size_t semi = text.find(';', at);
  return semi == std::string::npos ? "" : text.substr(at + head.size(), semi - at - head.size());
}

bool Has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }

// Width of a page (pause_menu.cpp "Page width"): the widest label (suffix included) and the widest value.
void PageWidthOfLighting(const std::vector<Row>& rows) {
  // Every label in kLightItems (pause_menu.cpp's own constructors) with the reason on the four rows; the widest value is SUN SHAFTS' "AUTO (DEFAULT)".
  struct L { const char* key; const char* label; bool needs_shadows; };
  const L labels[] = {
      {"PM_RxNoShadows", "SHADOWS", false},          {"PM_RxFoliage", "FOLIAGE SHADOWS", true},
      {"PM_RxGpuShadowDetail", "SHADOW DETAIL", true}, {"PM_RxGpuShadowFilter", "SHADOW FILTER", true},
      {"PM_RxGpuContact", "CONTACT SHADOWS", false},   {"PM_RxGpuTrafficShadows", "TRAFFIC SHADOWS", true},
      {"PM_RxGpuCarShadowMap", "CAR SHADOWS", false},  {"PM_RxGpuAo", "AMBIENT OCCLUSION", false},
      {"PM_RxGpuAoLevel", "AO INTENSITY", false},      {"PM_RxGpuSsgi", "BOUNCE LIGHT", false},
      {"PM_RxGpuSsgiLevel", "BOUNCE LIGHT STRENGTH", false}, {"PM_RxGpuSunShafts", "SUN SHAFTS", false},
      {"PM_RxGpuHeadlights", "HEADLIGHTS", false},     {"PM_RxGpuLightShadows", "LIGHT SHADOWS", false},
      {"PM_RxGpuNeonLight", "NEON LIGHT", false},      {"PM_RxGpuVolumetrics", "VOLUMETRIC LIGHT", false},
  };
  size_t widest_label = 0;
  for (const L& l : labels) {
    size_t n = std::strlen(l.label) + (l.needs_shadows ? std::strlen(kNeedsShadowsReason) : 0);
    widest_label = std::max(widest_label, n);
  }
  const size_t widest_value = std::strlen("AUTO (DEFAULT)");
  Check(widest_label + widest_value <= 41,
        "LIGHTING AND SHADOWS stays within 41 characters with the reason: " + std::to_string(widest_label + widest_value));
  // The wording the owner would pick is two characters too wide for this page (why the reason is " (NO SHADOW)").
  Check(std::strlen("TRAFFIC SHADOWS (SHADOWS OFF)") + widest_value > 41, "' (SHADOWS OFF)' would not fit the page");
  Check(rows.size() == sizeof(labels) / sizeof(labels[0]), "the width table lists every row of LIGHTING AND SHADOWS");
  for (size_t i = 0; i < rows.size() && i < sizeof(labels) / sizeof(labels[0]); ++i)
    Check(rows[i].key == labels[i].key, std::string("the width table is in the page's order at ") + labels[i].key);
  Check(rows.size() <= 24, "LIGHTING AND SHADOWS has at most 24 rows");
}

void Page(const std::string& dir) {
  std::string source;
  if (!ReadFile(dir + "pause_menu.cpp", &source)) {
    Check(false, "cannot open pause_menu.cpp beside the test: " + dir);
    return;
  }
  const std::string code = StripLineComments(source);

  const std::vector<Row> light = Rows(TableBody(code, "kLightItems"));
  const std::vector<Row> world = Rows(TableBody(code, "kWorldDetailItems"));
  const std::vector<std::string> light_expected = {
      "PM_RxNoShadows",       "PM_RxFoliage",       "PM_RxGpuShadowDetail", "PM_RxGpuShadowFilter", "PM_RxGpuContact",
      "PM_RxGpuTrafficShadows", "PM_RxGpuCarShadowMap", "PM_RxGpuAo",         "PM_RxGpuAoLevel",      "PM_RxGpuSsgi",
      "PM_RxGpuSsgiLevel",    "PM_RxGpuSunShafts",  "PM_RxGpuHeadlights",   "PM_RxGpuLightShadows", "PM_RxGpuNeonLight",
      "PM_RxGpuVolumetrics"};
  const std::vector<std::string> world_expected = {"PM_RxUnspawn", "PM_RxPedDensity", "PM_RxParkedCars",
                                                   "PM_RxFragDist", "PM_RxLodTraffic", "PM_RxLodCity"};
  Check(Keys(light) == light_expected, "LIGHTING AND SHADOWS has its rows in the approved order, FOLIAGE SHADOWS right after SHADOWS");
  Check(Keys(world) == world_expected, "WORLD DETAIL has its six rows and no FOLIAGE SHADOWS");
  Check(light.size() >= 2 && light[0].key == "PM_RxNoShadows" && light[1].key == "PM_RxFoliage", "SHADOWS is first and FOLIAGE SHADOWS second");
  PageWidthOfLighting(light);

  // FOLIAGE SHADOWS is a plain checkbox of the setting it always was (key, cvar, inverted) on both renderers, with the shadows rule.
  Check(Has(code, "Bool(\"PM_RxFoliage\", \"disable_imposter_shadows\", \"FOLIAGE SHADOWS: \", true)"),
        "FOLIAGE SHADOWS keeps its key, its cvar and its inverted checkbox");
  for (const Row& r : light)
    if (r.key == "PM_RxFoliage")
      Check(!r.native_only && r.dynamics == "kFoliageDynamics", "FOLIAGE SHADOWS is on both renderers and uses kFoliageDynamics");
  for (const Row& r : light)
    if (r.key == "PM_RxNoShadows") Check(!r.native_only && r.dynamics.empty(), "SHADOWS is on both renderers and depends on nothing");

  // The four rows get their grey from NeedsShadows; the rows that must not depend on SHADOWS do not.
  const std::string needs[] = {"kFoliageDynamics", "kShadowDetailDynamics", "kShadowFilterDynamics", "kTrafficShadowsDynamics"};
  for (const std::string& name : needs)
    Check(Has(DynamicsInit(code, name), "NeedsShadows("), name + " is made by NeedsShadows");
  const std::string independent[] = {"kContactDynamics", "kCarShadowDynamics", "kAoDynamics", "kShaftsDynamics", "kHeadlightsDynamics", "kNeonLightDynamics",
                                     "kVolumetricsDynamics", "kRoadReflectionDynamics", "kCarReflectionDynamics"};
  for (const std::string& name : independent) {
    const std::string init = DynamicsInit(code, name);
    Check(!init.empty() && !Has(init, "NeedsShadows("), name + " does not depend on SHADOWS (the audit's rows 2 and 3 are only the four)");
  }
  for (const Row& r : light) {
    const bool four = r.key == "PM_RxFoliage" || r.key == "PM_RxGpuShadowDetail" || r.key == "PM_RxGpuShadowFilter" ||
                      r.key == "PM_RxGpuTrafficShadows";
    if (four) Check(Has(DynamicsInit(code, r.dynamics), "NeedsShadows("), r.key + " names a NeedsShadows dynamics");
  }
  Check(Has(code, "bool ShadowsOn() { return !CvarGetBool(\"perf_no_shadows\"); }"), "ShadowsOn is the inverse of perf_no_shadows");
  Check(Has(code, "DependentRow<ShadowsOn, mcla_menu::kNeedsShadowsReason>(own)"), "NeedsShadows is DependentRow of ShadowsOn and the shared reason");
  Check(Has(code, "bool ItemEnabled(const ItemDef& it) { return mcla_menu::RowEnabled(it); }"),
        "the menu's enabled test is the helper's RowEnabled, the one this test runs");

  // The menu's gates on a grey row: A, a click and a step all ask first.
  const size_t accept = code.find("bool MenuFormAccept(uint32_t form, int row) {");
  const size_t click = code.find("ClickItem(it);", accept);
  const size_t gate = code.find("if (!ItemEnabled(it)) return false;", accept);
  Check(accept != std::string::npos && gate != std::string::npos && click != std::string::npos && gate < click,
        "MenuFormAccept refuses a grey row before it clicks it");
  const size_t poll = code.find("void PollNativeRows(int m) {");
  const size_t step_gate = code.find("if (!ItemEnabled(it)) {", poll);
  const size_t step_write = code.find("ItemSetValueIndex(it, idx);", poll);
  Check(poll != std::string::npos && step_gate != std::string::npos && step_write != std::string::npos && step_gate < step_write,
        "PollNativeRows puts a step on a grey row back before it writes it");
  Check(Has(code, "ApplyRowEnabled(nr, i, it);"), "the rows' enabled state is applied on every poll and every sync");

  // The setting: the default is untouched (true: FOLIAGE SHADOWS is OFF out of the box) and the help says what the setting does.
  std::string header;
  if (!ReadFile(dir + "../../settings/generated/settings_patches.h", &header)) {
    Check(false, "cannot open settings_patches.h");
    return;
  }
  // (Written in two pieces: tools/check_settings_exposure.py takes a literal "REX_SETTING(" anywhere in src for a setting definition.)
  const std::string declaration = std::string("REX_SET") + "TING(bool, disable_imposter_shadows, ";
  const size_t decl = header.find(declaration);
  Check(decl != std::string::npos && header.compare(decl + declaration.size(), 4, "true") == 0,
        "disable_imposter_shadows still defaults to true");
  const size_t end = header.find(");", decl);
  const std::string help_h = decl == std::string::npos ? "" : header.substr(decl, end - decl);
  std::string manifest;
  if (!ReadFile(dir + "../../settings/generated/manifest.json", &manifest)) {
    Check(false, "cannot open manifest.json");
    return;
  }
  const size_t mat = manifest.find("\"disable_imposter_shadows\": {");
  const size_t mend = manifest.find("\"traits\"", mat);
  const std::string help_m = mat == std::string::npos ? "" : manifest.substr(mat, mend - mat);
  Check(Has(help_m, "\"default\": \"true\""), "the manifest default is still true");
  for (const std::string* help : {&help_h, &help_m}) {
    const char* where = help == &help_h ? "settings_patches.h" : "manifest.json";
    Check(!Has(*help, "Performance Mode"), std::string(where) + ": the inverted old help is gone");
    for (const char* phrase : {"flat cards", "baked shadow silhouette", "sun's shadows", "refresh", "Daylight only", "perf_no_shadows"})
      Check(Has(*help, phrase), std::string(where) + ": the help says '" + phrase + "'");
  }
}

// ---- 3. Navigation: the cursor lands on a grey row, nothing acts on it -----------------------------------------------------------------------------

// The game's list moves the highlight one row for an up or a down and does not skip a disabled row (pause_menu.cpp, kRowValueEnabled: "Cursor moves do not
// skip such a row"); a press at the end of the list is the list's own (it wraps or stops: not asserted here).
int Move(int selected, larecomp::menumouse::Act press) {
  if (press == larecomp::menumouse::Act::Down && selected + 1 < kPageRows) return selected + 1;
  if (press == larecomp::menumouse::Act::Up && selected > 0) return selected - 1;
  return selected;
}

// What a press does to a row's setting: left, right and accept change it only on a row the menu lets through (RowEnabled), as MenuFormAccept and
// PollNativeRows do. Returns whether the setting would be written.
bool WouldWrite(const ItemDef& it, larecomp::menumouse::Act press) {
  using larecomp::menumouse::Act;
  if (press != Act::Accept && press != Act::Left && press != Act::Right) return false;
  return RowEnabled(it);
}

void Navigation() {
  using namespace larecomp::menumouse;
  g_perf_no_shadows = true;  // SHADOWS: OFF: rows 1 and 2 are grey
  g_other_on = true;

  // Keyboard and pad (the same list keys): from SHADOWS, Down lands on the grey FOLIAGE SHADOWS, Down again on the grey SHADOW DETAIL, then on
  // CONTACT SHADOWS, which is not grey. Up walks back through them.
  int at = 0;
  at = Move(at, Act::Down);
  Check(at == 1 && !RowEnabled(kPage[at]), "Down from SHADOWS lands on the grey FOLIAGE SHADOWS");
  at = Move(at, Act::Down);
  Check(at == 2 && !RowEnabled(kPage[at]), "Down again lands on the grey SHADOW DETAIL (no skipping)");
  at = Move(at, Act::Down);
  Check(at == 3 && RowEnabled(kPage[at]), "and on CONTACT SHADOWS, which is not grey");
  at = Move(at, Act::Up);
  at = Move(at, Act::Up);
  Check(at == 1, "Up walks back onto the grey rows");

  // On a grey row A, left and right write nothing; on an enabled one they do.
  for (Act press : {Act::Accept, Act::Left, Act::Right}) {
    Check(!WouldWrite(kPage[1], press), "a press on the grey FOLIAGE SHADOWS writes nothing");
    Check(!WouldWrite(kPage[2], press), "a press on the grey SHADOW DETAIL writes nothing");
    Check(WouldWrite(kPage[0], press), "a press on SHADOWS itself still writes");
    Check(WouldWrite(kPage[3], press), "a press on CONTACT SHADOWS still writes");
  }
  Check(!WouldWrite(kPage[1], Act::Up) && !WouldWrite(kPage[1], Act::Down), "moving the cursor never writes a setting");

  // The mouse: hover takes the highlight to a grey row like any other (the plan does not ask the row), and the highlight walk gets there by the arrows.
  Check(HoverTarget(true, true, true, true, 1, 0) == 1, "hovering the grey FOLIAGE SHADOWS highlights it");
  Check(HoverTarget(true, true, true, true, 2, 0) == 2, "hovering the grey SHADOW DETAIL highlights it");
  Check(WalkStep(0, 2) == Act::Down && WalkStep(3, 1) == Act::Up, "the walk reaches a grey row with the arrows");

  // A click: the highlight goes to the row first, then ACCEPT (label) or the arrow (a value zone): the menu refuses all three on a grey row.
  const struct { Zone zone; Act act; } clicks[] = {{Zone::Label, Act::Accept}, {Zone::ValueLeft, Act::Left}, {Zone::ValueRight, Act::Right}};
  for (const auto& c : clicks) {
    const ClickPlan plan = PlanLeftClick(true, 2, c.zone, true);
    Check(plan.row == 2 && plan.act == c.act, "a click on the grey SHADOW DETAIL targets it");
    Check(!WouldWrite(kPage[plan.row], plan.act), "and the menu refuses it, so the click writes nothing");
  }
  const ClickPlan on_checkbox = PlanLeftClick(true, 1, Zone::Label, false);
  Check(on_checkbox.row == 1 && on_checkbox.act == Act::Accept && !WouldWrite(kPage[1], on_checkbox.act),
        "a click on the grey FOLIAGE SHADOWS checkbox is an ACCEPT the menu refuses");

  // The rows come back when SHADOWS is turned on again, and the same presses work.
  g_perf_no_shadows = false;
  Check(RowEnabled(kPage[1]) && RowEnabled(kPage[2]), "turning SHADOWS back on lights both rows at once");
  Check(WouldWrite(kPage[1], Act::Accept) && WouldWrite(kPage[2], Act::Right), "and a press on them writes again");
}

}  // namespace

int main() {
  Helper();
  Page(Here());
  Navigation();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
