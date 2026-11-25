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
constexpr char kNeedsShadowsReason[] = " (SUN OFF)";
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

void CheckWidth(std::string_view page, std::initializer_list<std::string_view> labels,
                std::string_view widest_value) {
  size_t widest = 0;
  for (const auto label : labels) widest = std::max(widest, label.size());
  Check(widest + widest_value.size() <= 41, std::string(page) + " fits within 41 columns");
}

void Page(const std::string& dir) {
  std::string source;
  if (!ReadFile(dir + "pause_menu.cpp", &source)) {
    Check(false, "cannot open pause_menu.cpp beside the test: " + dir);
    return;
  }
  const std::string code = StripLineComments(source);

  const auto light = Rows(TableBody(code, "kLightItems"));
  const auto reflections = Rows(TableBody(code, "kReflectionItems"));
  const std::vector<std::string> light_expected = {
      "PM_RxNoShadows", "PM_RxGpuShadowDetail", "PM_RxGpuShadowFilter", "PM_RxFoliage",
      "PM_RxGpuTrafficShadows", "PM_RxGpuCarShadowMap", "PM_RxGpuContact", "PM_RxGpuRtShadows",
      "PM_RxGpuAo", "PM_RxGpuCarAoRt", "PM_RxGpuAoLevel", "PM_RxGpuSsgi", "PM_RxGpuRtGi",
      "PM_RxGpuSsgiLevel", "PM_RxGpuHeadlights", "PM_RxGpuLightShadows", "PM_RxGpuRtLights",
      "PM_RxGpuNeonLight", "PM_RxGpuSunShafts", "PM_RxGpuVolumetrics"};
  const std::vector<std::string> reflections_expected = {
      "PM_RxGpuRoadRefl", "PM_RxGpuSsr", "PM_RxGpuSsrRt", "PM_RxGpuCarRefl", "PM_RxGpuCarSelfRefl",
      "PM_RxGpuCarSelfReflRt", "PM_RxGpuRtCityRefl", "PM_RxGpuSideMirrors", "PM_RxGpuRtMirror"};
  const std::vector<std::string> display_expected = {
      "PM_RxDisplayMode", "PM_RxVsync", "PM_RxResolution", "PM_RxAspect", "PM_RxSubGpuHdr",
      "PM_RxDisplayGamma", "PM_RxFpsCap", "PM_RxMsaa", "PM_RxGpuAa"};
  const std::vector<std::string> image_expected = {"PM_RxGpuAniso", "PM_RxXeAniso", "PM_RxDof", "PM_RxBlur"};
  const std::vector<std::string> world_expected = {
      "PM_RxUnspawn", "PM_RxPedDensity", "PM_RxParkedCars", "PM_RxFragDist", "PM_RxLodTraffic", "PM_RxLodCity"};
  Check(Keys(light) == light_expected, "Lighting and Shadows groups shadows, occlusion, bounce and local lights");
  Check(Keys(reflections) == reflections_expected, "Reflections keeps each RT row beside its base effect");
  Check(Keys(Rows(TableBody(code, "kVideoItems"))) == display_expected, "Display owns both AA rows");
  Check(Keys(Rows(TableBody(code, "kImageQualityItems"))) == image_expected, "Image Quality holds filtering and blur");
  Check(Keys(Rows(TableBody(code, "kWorldDetailItems"))) == world_expected, "World Detail keeps all six rows");
  for (const Row& row : reflections) Check(row.native_only, row.key + " is native only");
  std::string debug;
  Check(ReadFile(dir + "debug_menu_pages.cpp", &debug), "debug menu source is available");
  const auto fixes = TableBody(StripLineComments(debug), "kDebugFixesItems");
  Check(Has(fixes, "PM_RxSharpUi") && Has(fixes, "PM_RxSharpMinimap"), "both sharpening switches are under Debug Fixes");
  CheckWidth("Lighting and Shadows", {"SUN SHADOW FILTER (SUN OFF)", "BOUNCE LIGHT RT (NO RT GPU)", "CAR AO RT (NO RT GPU)"}, "RT (NO RT GPU)");
  CheckWidth("Reflections", {"CAR SELF-REFLECTIONS RT (NO RT GPU)", "SURFACE REFLECTIONS RT (NO RT GPU)", "RT CITY (BUILDING DATA)"}, "ULTRA");

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
