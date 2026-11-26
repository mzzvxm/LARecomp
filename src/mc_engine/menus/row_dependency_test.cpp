// Pause-menu page order, width and input gates. The declarative greying
// and captions themselves are checked by ui_parity_test.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "input/menu_mouse_core.h"
#include "menus/menu_items.h"

namespace {

using mcla_menu::ItemDef;

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

// Model the native menu input gates with the old SHADOWS dependency.
bool g_perf_no_shadows = false;
constexpr char kNeedsShadowsReason[] = " (NO SHADOW)";
constexpr ItemDef kPage[] = {
    mcla_menu::Bool("PM_RxNoShadows", "perf_no_shadows", "SHADOWS: ", true),
    mcla_menu::Bool("PM_RxFoliage", "disable_imposter_shadows", "FOLIAGE SHADOWS: ", true),
    mcla_menu::Str("PM_RxGpuShadowDetail", "mcla_gpu_shadow_resolution", "SHADOW DETAIL: ", nullptr, nullptr, 0),
    mcla_menu::Str("PM_RxGpuContact", "mcla_gpu_contact_shadows", "CONTACT SHADOWS: ", nullptr, nullptr, 0),
};
constexpr int kPageRows = int(std::size(kPage));

bool RowEnabled(const ItemDef& row) {
  const std::string_view key(row.key);
  return !g_perf_no_shadows || (key != "PM_RxFoliage" && key != "PM_RxGpuShadowDetail");
}

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
  bool native_only = false;
};

// The rows of a table in order, identified by their "PM_Rx..." keys.
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

bool Has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }

// Width of a page (pause_menu.cpp "Page width"): the widest label (suffix included) and the widest value.
void PageWidthOfLighting(const std::vector<Row>& rows) {
  // Every label in kLightItems (pause_menu.cpp's own constructors) with the reason on the four rows; the widest value is SUN SHAFTS' "AUTO (DEFAULT)".
  struct L { const char* key; const char* label; bool needs_shadows; };
  const L labels[] = {
      {"PM_RxNoShadows", "SHADOWS", false},          {"PM_RxFoliage", "FOLIAGE SHADOWS", true},
      {"PM_RxGpuShadowDetail", "SHADOW DETAIL", true}, {"PM_RxGpuShadowFilter", "SHADOW FILTER", true},
      {"PM_RxGpuContact", "CONTACT SHADOWS", false},   {"PM_RxGpuRtShadows", "RT SHADOWS (GPU TOO SLOW)", false},
      {"PM_RxGpuTrafficShadows", "TRAFFIC SHADOWS", true}, {"PM_RxGpuCarShadowMap", "CAR SHADOWS", false},
      {"PM_RxGpuAo", "AMBIENT OCCLUSION", false},
      {"PM_RxGpuAoLevel", "AO INTENSITY", false},      {"PM_RxGpuSsgi", "BOUNCE LIGHT (REPLACED)", false},
      {"PM_RxGpuRtGi", "RT BOUNCE LIGHT (NO RT GPU)", false},
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

// Width of REFLECTIONS (pause_menu.cpp "Page width"): every label of kReflectionItems in every state its suffix can be in, and the widest value, which is CAR SELF-REFLECTIONS' "AUTO (RAY TRACED)". The
// RT CITY REFLECTIONS row names itself by state (gpu/city_refl_setting.h RowLabel and RowSuffix: the whole name while it is on, "RT CITY" with the reason otherwise: the whole name with a reason would
// make the page 48 wide); gpu/city_refl_setting_test.cpp holds the states against the header itself, this is the page's table.
void PageWidthOfReflections(const std::vector<Row>& rows) {
  const char* const labels[] = {"ROAD REFLECTIONS",        "CAR REFLECTIONS",        "CAR SELF-REFLECTIONS",       "RT CITY REFLECTIONS", "RT CITY (NO RT GPU)", "RT CITY (TOO SLOW)",
                                "RT CITY (NEEDS CAR RT)",  "RT CITY (NOT READY)",    "RT CITY (BUILDING DATA)",    "REFLECTIONS ON SURFACES",
                                "RT SIDE MIRRORS",         "RT MIRRORS (NO RT GPU)", "RT MIRRORS (TOO SLOW)",  "SIDE MIRRORS",         "SIDE MIRRORS (REPLACED)"};
  size_t widest_label = 0;
  for (const char* label : labels) widest_label = std::max(widest_label, std::strlen(label));
  const size_t widest_value = std::strlen("AUTO (RAY TRACED)");
  Check(widest_label + widest_value <= 41, "REFLECTIONS stays within 41 characters in every state of RT CITY REFLECTIONS: " + std::to_string(widest_label + widest_value));
  Check(std::strlen("RT CITY REFLECTIONS (BUILDING DATA)") + widest_value > 41, "the whole name with the longest reason would not fit the page");
  Check(std::strlen("RT SIDE MIRRORS (NO RT GPU)") + widest_value > 41, "the whole name of the mirrors' row with a reason would not fit the page");
  const std::vector<std::string> expected = {"PM_RxGpuRoadRefl", "PM_RxGpuCarRefl", "PM_RxGpuCarSelfRefl", "PM_RxGpuRtCityRefl", "PM_RxGpuSideMirrors", "PM_RxGpuRtMirror", "PM_RxGpuSsr", "PM_RxGpuSsrRt"};
  Check(Keys(rows) == expected, "REFLECTIONS has its seven rows in order, the city's right after the car's own ray traced reflection it rides on, then the side mirrors and the traced ones that replace them");
  for (const Row& r : rows) {
    Check(r.native_only, r.key + " is a native renderer row");
  }
  Check(rows.size() <= 24, "REFLECTIONS has at most 24 rows");
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
      "PM_RxGpuRtShadows",    "PM_RxGpuTrafficShadows", "PM_RxGpuCarShadowMap", "PM_RxGpuAo",         "PM_RxGpuAoLevel",      "PM_RxGpuSsgi",
      "PM_RxGpuRtGi",         "PM_RxGpuSsgiLevel",    "PM_RxGpuSunShafts",  "PM_RxGpuHeadlights",   "PM_RxGpuLightShadows", "PM_RxGpuNeonLight",
      "PM_RxGpuVolumetrics"};
  const std::vector<std::string> world_expected = {"PM_RxUnspawn", "PM_RxPedDensity", "PM_RxParkedCars",
                                                   "PM_RxFragDist", "PM_RxLodTraffic", "PM_RxLodCity"};
  Check(Keys(light) == light_expected, "LIGHTING AND SHADOWS has its rows in the approved order, FOLIAGE SHADOWS right after SHADOWS");
  Check(Keys(world) == world_expected, "WORLD DETAIL has its six rows and no FOLIAGE SHADOWS");
  Check(light.size() >= 2 && light[0].key == "PM_RxNoShadows" && light[1].key == "PM_RxFoliage", "SHADOWS is first and FOLIAGE SHADOWS second");
  PageWidthOfLighting(light);
  PageWidthOfReflections(Rows(TableBody(code, "kReflectionItems")));

  // FOLIAGE SHADOWS is a plain checkbox of the setting it always was (key, cvar, inverted) on both renderers, with the shadows rule.
  Check(Has(code, "Bool(\"PM_RxFoliage\", \"disable_imposter_shadows\", \"FOLIAGE SHADOWS: \", true)"),
        "FOLIAGE SHADOWS keeps its key, its cvar and its inverted checkbox");
  for (const Row& row : light) {
    if (row.key == "PM_RxFoliage" || row.key == "PM_RxNoShadows")
      Check(!row.native_only, row.key + " is on both renderers");
  }
  Check(Has(code, "if (const auto* state = ItemState(it)) return (*state)[\"enabled\"].value_or(true);"),
        "the menu's enabled test uses the rule state");

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
  Page(Here());
  Navigation();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
