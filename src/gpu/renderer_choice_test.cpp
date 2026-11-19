// A check that needs no game: which renderer each value of the SDK's gpu.driver means (gpu/renderer_choice.h), and the hidden way back to
// the XENIA renderer (gpu/renderer_fallback.h): the debug row's picks, the compatibility renderer box and its larecomp.toml write, and the
// count of starts that never got going. Build: cmake --build <build dir> --target larecomp_renderer_choice_test, then run it; exit 0 = passed.
//
// The case of 2026-11-18: gpu.driver = d3d12 / vulkan ran the boot build of the shader packs, because the test of the day (the
// old mcla_gpu setting, true by default) said "Plume".
//
// The compile by hand (the msvc-target clang of labuild, from the source folder):
//   clang++ -std=c++23 -Isrc/gpu -Isrc/profile src/gpu/renderer_choice_test.cpp src/profile/larecomp_profile_store.cpp -o renderer_choice_test.exe

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "choice_box_layout.h"
#include "larecomp_profile_store.h"
#include "renderer_choice.h"
#include "renderer_fallback.h"

namespace {

int g_checks = 0;
int g_failed = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_failed;                                                            \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
    }                                                                        \
  } while (0)

std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

bool Has(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

std::string ReadAll(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteAll(const std::filesystem::path& file, const std::string& text) {
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out << text;
}

// A folder of the check's own: empty at the start, gone at the end.
struct TempFolder {
  std::filesystem::path path;
  TempFolder() {
    path = std::filesystem::temp_directory_path() / "larecomp_renderer_choice_test";
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::create_directories(path, ec);
  }
  ~TempFolder() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

bool OnlyCapitalsDigitsAndSpaces(std::string_view text) {
  return std::all_of(text.begin(), text.end(), [](char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == ' ';
  });
}

// gpu.driver -> the renderer that runs (the rule behind mcla::gpu::Enabled()).
void CheckChoice() {
  using mcla::gpu::RendererChoice;
  using mcla::gpu::RendererChoiceOf;

  // LARecomp's own renderer: the default, and the batch actions' "none".
  CHECK(RendererChoiceOf("larecomp") == RendererChoice::kLarecomp);
  CHECK(RendererChoiceOf("none") == RendererChoice::kLarecomp);

  // The SDK's Xenia GPU: every driver the SDK ships, and "any" (d3d12 on Windows, vulkan elsewhere).
  CHECK(RendererChoiceOf("any") == RendererChoice::kXenia);
  CHECK(RendererChoiceOf("d3d12") == RendererChoice::kXenia);
  CHECK(RendererChoiceOf("vulkan") == RendererChoice::kXenia);
  CHECK(RendererChoiceOf("null") == RendererChoice::kXenia);

  // No answer before the setting exists; a word nothing knows is not LARecomp's renderer (the SDK refuses to start with it).
  CHECK(RendererChoiceOf("") == RendererChoice::kNotSet);
  CHECK(RendererChoiceOf("Larecomp") == RendererChoice::kXenia);
  CHECK(RendererChoiceOf("larecomp ") == RendererChoice::kXenia);

  static_assert(RendererChoiceOf("larecomp") == RendererChoice::kLarecomp);
  static_assert(RendererChoiceOf("vulkan") == RendererChoice::kXenia);
}

// The row of the debug menu (RENDERING page): what it shows for a stored gpu.driver, and what stepping it writes.
void CheckRowPicks() {
  using namespace mcla::gpu;

  // What it shows: the native renderer, or the Xenia renderer on the API the setting names; "any" is the platform's own.
  CHECK(PickOfDriver("larecomp", true) == RendererPick::kNative);
  CHECK(PickOfDriver("larecomp", false) == RendererPick::kNative);
  CHECK(PickOfDriver("none", true) == RendererPick::kNative);
  CHECK(PickOfDriver("", true) == RendererPick::kNative);
  CHECK(PickOfDriver("d3d12", true) == RendererPick::kXeniaD3d12);
  CHECK(PickOfDriver("d3d12", false) == RendererPick::kXeniaD3d12);
  CHECK(PickOfDriver("vulkan", true) == RendererPick::kXeniaVulkan);
  CHECK(PickOfDriver("any", true) == RendererPick::kXeniaD3d12);
  CHECK(PickOfDriver("any", false) == RendererPick::kXeniaVulkan);
  CHECK(PickOfDriver("null", true) == RendererPick::kXeniaD3d12);
  CHECK(PickOfDriver("nonsense", false) == RendererPick::kXeniaVulkan);

  // What stepping writes, in the row's order; and reading it back gives the same pick (a step is never lost on the way).
  CHECK(kRendererPickCount == 3);
  CHECK(DriverOfPick(PickAt(0)) == "larecomp");
  CHECK(DriverOfPick(PickAt(1)) == "d3d12");
  CHECK(DriverOfPick(PickAt(2)) == "vulkan");
  CHECK(PickAt(-1) == RendererPick::kNative);
  CHECK(PickAt(7) == RendererPick::kXeniaVulkan);
  for (int i = 0; i < kRendererPickCount; ++i) {
    for (const bool windows : {true, false}) {
      CHECK(PickOfDriver(DriverOfPick(PickAt(i)), windows) == PickAt(i));
    }
    // The setting the row writes means the renderer its label says: only the first pick is the native (Plume) renderer.
    CHECK((RendererChoiceOf(DriverOfPick(PickAt(i))) == RendererChoice::kLarecomp) == (i == 0));
  }

  // Labels: the value's text is also the name of a Flash element, so only capitals, digits and spaces.
  CHECK(PickLabel(RendererPick::kNative) == "NATIVE");
  CHECK(PickLabel(RendererPick::kXeniaD3d12) == "XENIA D3D12");
  CHECK(PickLabel(RendererPick::kXeniaVulkan) == "XENIA VULKAN");
  for (int i = 0; i < kRendererPickCount; ++i) {
    CHECK(OnlyCapitalsDigitsAndSpaces(PickLabel(PickAt(i))));
  }
  static_assert(PickOfDriver("vulkan", true) == RendererPick::kXeniaVulkan);
  static_assert(DriverOfPick(RendererPick::kXeniaD3d12) == "d3d12");
}

// The box when the native renderer cannot start.
void CheckRefusalBox() {
  using namespace mcla::gpu;
  using namespace mcla::gpu::fallback;

  // Which API: Direct3D 12 on Windows (it also runs on a card with shader model 5.1 and binding tier 1), Vulkan elsewhere.
  CHECK(FallbackDriver(true) == "d3d12");
  CHECK(FallbackDriver(false) == "vulkan");
  // What it writes is a gpu.driver the Xenia renderer runs on, never the native renderer's.
  for (const bool windows : {true, false}) {
    CHECK(RendererChoiceOf(FallbackDriver(windows)) == RendererChoice::kXenia);
  }

  // Whether the button can do it: not when the launch sets gpu.driver itself (command line, environment: they beat the file, and a
  // restart keeps the command line), and not when the button could not write.
  CHECK(WayFor(false, true) == Way::kButton);
  CHECK(WayFor(false, false) == Way::kLine);
  CHECK(WayFor(true, true) == Way::kLaunchOption);
  CHECK(WayFor(true, false) == Way::kLaunchOption);

  // The words: always the compatibility renderer, never the branding; the refusal's own reasons stay in; each way says what to do.
  const std::string reasons = "Direct3D 12 on Test GPU: shader model 5.1 is below 6.0";
  for (const bool windows : {true, false}) {
    for (const Way way : {Way::kButton, Way::kLine, Way::kLaunchOption}) {
      const std::string text = RefusalText(reasons, way, windows);
      CHECK(Has(text, reasons));
      CHECK(Has(text, "compatibility renderer"));
      CHECK(Lower(text).find("xenia") == std::string::npos);
      CHECK(Has(text, "The details are in the log"));
    }
    CHECK(Has(RefusalText("", Way::kButton, windows), "The reason is in the log."));
    CHECK(Has(RefusalText(reasons, Way::kLine, windows), "gpu.driver = \"" + std::string(FallbackDriver(windows)) + "\""));
    CHECK(Has(RefusalText(reasons, Way::kLaunchOption, windows), "--gpu.driver=" + std::string(FallbackDriver(windows))));
    CHECK(Has(RefusalText(reasons, Way::kButton, windows), "Start the game with it now?"));
    CHECK(!Has(RefusalText(reasons, Way::kButton, windows), "gpu.driver"));  // the button needs no instruction
  }
  CHECK(Lower(std::string(kButtonText)).find("xenia") == std::string::npos);
  CHECK(Lower(std::string(kKeepTryingText)).find("xenia") == std::string::npos);
  CHECK(Lower(std::string(kShaderDialogText)).find("xenia") == std::string::npos);
  CHECK(Lower(std::string(kShaderDialogPrompt)).find("xenia") == std::string::npos);
  CHECK(Lower(UnfinishedStartsText(2)).find("xenia") == std::string::npos);
  CHECK(Has(UnfinishedStartsText(2), "last 2 times"));
  CHECK(Lower(ManualText("d3d12", false)).find("xenia") == std::string::npos);
  CHECK(Has(ManualText("d3d12", false), "gpu.driver = \"d3d12\""));
  CHECK(!Has(ManualText("d3d12", true), "gpu.driver"));
}

// The write the button makes: gpu.driver in larecomp.toml, through the profile store's edit (the function the pause menu's saves use).
void CheckConfigWrite() {
  using namespace mcla::gpu::fallback;
  namespace pf = larecomp::profiles;
  TempFolder folder;
  const auto write = [](const std::filesystem::path& file, std::string_view driver) {
    const DriverEdit edit = EditFor(driver);
    return pf::EditConfig(file, {{edit.key, edit.value, false}});
  };

  CHECK(ConfigLine("d3d12") == "gpu.driver = \"d3d12\"");
  CHECK(ConfigLine("larecomp") == "gpu.driver = \"larecomp\"");

  // No file yet: it is made, with the one line.
  const std::filesystem::path fresh = folder.path / "fresh.toml";
  CHECK(write(fresh, "d3d12"));
  const std::string fresh_text = ReadAll(fresh);
  CHECK(Has(fresh_text, "gpu.driver = \"d3d12\""));
  CHECK(fresh_text.find("gpu.driver") == fresh_text.rfind("gpu.driver"));  // once

  // A player's file: every other line stays byte for byte (a comment, other settings, a table below); the new line goes above the first table.
  const std::string original =
      "# my settings\r\n"
      "mcla_gpu_msaa = \"4x\"\r\n"
      "fps_limit = 144\r\n"
      "\r\n"
      "[gpu]\r\n"
      "vsync = false\r\n";
  const std::filesystem::path mine = folder.path / "mine.toml";
  WriteAll(mine, original);
  CHECK(write(mine, "d3d12"));
  const std::string after = ReadAll(mine);
  CHECK(after.starts_with("# my settings\r\nmcla_gpu_msaa = \"4x\"\r\nfps_limit = 144\r\ngpu.driver = \"d3d12\"\r\n"));
  CHECK(after.ends_with("\r\n[gpu]\r\nvsync = false\r\n"));
  CHECK(after.find("gpu.driver") < after.find("[gpu]"));  // never inside a table

  // Writing again changes the line it made (a second pick in the debug row, or the way back to the native renderer), and only that line.
  CHECK(write(mine, "larecomp"));
  const std::string back = ReadAll(mine);
  CHECK(Has(back, "gpu.driver = \"larecomp\"\r\n"));
  CHECK(!Has(back, "d3d12"));
  CHECK(back.find("gpu.driver") == back.rfind("gpu.driver"));
  CHECK(back.starts_with("# my settings\r\nmcla_gpu_msaa = \"4x\"\r\nfps_limit = 144\r\n"));
  CHECK(back.ends_with("\r\n[gpu]\r\nvsync = false\r\n"));

  // A line the player wrote, with a comment: the comment stays.
  const std::filesystem::path hand = folder.path / "hand.toml";
  WriteAll(hand, "gpu.driver = \"larecomp\"  # native\nfps_limit = 60\n");
  CHECK(write(hand, "vulkan"));
  CHECK(ReadAll(hand) == "gpu.driver = \"vulkan\"  # native\nfps_limit = 60\n");
}

// The starts that never got going.
void CheckStartWatch() {
  using namespace mcla::gpu::fallback;
  TempFolder folder;
  const std::filesystem::path file = folder.path / std::string(kStartFileName);

  CHECK(!OfferAfter(0));
  CHECK(!OfferAfter(1));
  CHECK(OfferAfter(2));
  CHECK(OfferAfter(5));

  // No file: nothing happened before. A start that ends by itself (the window, a failed start that told the player) leaves no file.
  CHECK(ReadUnfinished(file) == 0);
  {
    StartWatch watch;
    CHECK(watch.Arm(file) == 0);
    CHECK(ReadUnfinished(file) == 1);  // this start counts as unfinished until it finishes
    watch.Finish();
    CHECK(!std::filesystem::exists(file));
    watch.Finish();  // twice is fine
    CHECK(!watch.Armed());
  }

  // Two crashes at start in a row: each new start (a fresh watch: a new process) sees the count of the ones before it, and the third offers.
  {
    StartWatch first;
    CHECK(first.Arm(file) == 0);
  }  // ended from outside: no Finish
  {
    StartWatch second;
    const int before = second.Arm(file);
    CHECK(before == 1);
    CHECK(!OfferAfter(before));
  }
  {
    StartWatch third;
    const int before = third.Arm(file);
    CHECK(before == 2);
    CHECK(OfferAfter(before));
    // "Keep trying": this start is the first of a new count, so two more crashes are needed before the offer comes again.
    third.CountAgain();
    CHECK(ReadUnfinished(file) == 1);
  }
  {
    StartWatch fourth;
    CHECK(fourth.Arm(file) == 1);
    CHECK(!OfferAfter(1));
    // A start that presents its frames is a finished start: the next one begins clean.
    for (int i = 0; i < kFramesToFinish - 1; ++i) {
      fourth.NoteFrame();
    }
    CHECK(fourth.Armed());
    CHECK(ReadUnfinished(file) == 2);
    fourth.NoteFrame();
    CHECK(!fourth.Armed());
    CHECK(!std::filesystem::exists(file));
    fourth.NoteFrame();  // more frames change nothing
    CHECK(!std::filesystem::exists(file));
  }

  // A watch that was never armed (the XENIA renderer, a batch action) counts nothing and writes nothing.
  {
    StartWatch idle;
    for (int i = 0; i < 3 * kFramesToFinish; ++i) {
      idle.NoteFrame();
    }
    idle.Finish();
    idle.CountAgain();
    CHECK(!std::filesystem::exists(file));
  }

  // A file that holds anything but a small number counts as none; a folder that cannot be written arms nothing.
  WriteAll(file, "banana\n");
  CHECK(ReadUnfinished(file) == 0);
  WriteAll(file, "-4\n");
  CHECK(ReadUnfinished(file) == 0);
  WriteAll(file, "999999999\n");
  CHECK(ReadUnfinished(file) == 0);
  ClearUnfinished(file);
  StartWatch nowhere;
  CHECK(nowhere.Arm(folder.path / "no_such_folder" / "renderer_start.txt") == 0);
  CHECK(!nowhere.Armed());
}


// The layout of the two-button box (choice_box_layout.h, the arithmetic behind choice_box.h): the 2026-11-19 picture of the box had its main
// button cut to "...compatibility re" and the action last. Every button is as wide as its own label needs, the action is first (left), the pair
// sits at the right edge, and all of it holds at each DPI. (choice_box_test.cpp checks the real dialog.)
void CheckBoxLayout() {
  using namespace mcla::gpu::fallback;
  CHECK(Dip(32, 96) == 32);
  CHECK(Dip(32, 144) == 48);
  CHECK(Dip(20, 192) == 40);
  CHECK(Dip(7, 120) == 9);  // 8.75 rounds to 9

  for (const int dpi : {96, 120, 144, 192, 288}) {
    BoxMeasure m;
    m.dpi = dpi;
    m.icon = Dip(32, dpi);
    m.body_w = Dip(500, dpi);
    m.body_h = Dip(120, dpi);
    m.label_h = Dip(16, dpi);
    m.label_w = {Dip(210, dpi), Dip(40, dpi)};  // "Use the compatibility renderer", "Close"
    const BoxLayout l = PlanBox(m);
    CHECK(l.buttons.size() == 2);
    if (l.buttons.size() != 2) {
      continue;
    }
    // Neither label is cut: each button holds its label and 14 dip of room on both sides; the short one still has the minimum width.
    CHECK(l.buttons[0].w >= m.label_w[0] + 2 * Dip(14, dpi));
    CHECK(l.buttons[1].w >= m.label_w[1] + 2 * Dip(14, dpi));
    CHECK(l.buttons[1].w >= Dip(88, dpi));
    CHECK(l.buttons[0].w > l.buttons[1].w);  // the long label's button is the wider one (the old box gave both one width)
    // The action first (left), the way out last, side by side with a gap, the pair at the right edge of the strip.
    CHECK(l.buttons[0].x + l.buttons[0].w + Dip(8, dpi) == l.buttons[1].x);
    CHECK(l.buttons[1].x + l.buttons[1].w == l.client_w - Dip(12, dpi));
    CHECK(l.buttons[0].x >= Dip(12, dpi));
    // One row, one height, under the text block, inside the box.
    CHECK(l.buttons[0].y == l.buttons[1].y);
    CHECK(l.buttons[0].h == l.buttons[1].h);
    CHECK(l.buttons[0].h >= Dip(24, dpi));
    CHECK(l.buttons[0].y >= l.text_area_h);
    CHECK(l.client_h == l.buttons[0].y + l.buttons[0].h + Dip(12, dpi));
    // The text block: the icon at the left, the text after it, both inside the upper area and the box.
    CHECK(l.icon.x + l.icon.w <= l.body.x);
    CHECK(l.body.x + l.body.w <= l.client_w);
    CHECK(l.body.y + l.body.h <= l.text_area_h);
    CHECK(l.icon.y + l.icon.h <= l.text_area_h);
  }

  // A short text with long buttons: the box widens to the buttons, the text stays at the left, and its one line sits beside the icon's middle.
  {
    BoxMeasure m;
    m.dpi = 96;
    m.icon = 32;
    m.body_w = 100;
    m.body_h = 16;
    m.label_h = 15;
    m.label_w = {210, 70};
    const BoxLayout l = PlanBox(m);
    CHECK(l.buttons.size() == 2);
    CHECK(l.client_w == l.buttons[1].x + l.buttons[1].w + 12);
    CHECK(l.client_w > l.body.x + l.body.w + 20);
    CHECK(l.body.x == 20 + 32 + 14);
    CHECK(l.body.y == 28);  // (32 - 16) / 2 below the icon's top
  }

  // No icon: the text starts at the margin. No buttons: only the strip's padding.
  {
    BoxMeasure m;
    m.body_w = 300;
    m.body_h = 40;
    m.label_h = 15;
    m.label_w = {100};
    const BoxLayout l = PlanBox(m);
    CHECK(l.body.x == 20);
    CHECK(l.buttons.size() == 1);
    CHECK(l.client_w == 20 + 300 + 20);
    CHECK(PlanBox(BoxMeasure{}).buttons.empty());
  }

  // How wide the text may be: 560 dip on a big screen, less on a small one, never less than 200 dip.
  CHECK(MaxBodyWidth(96, 1920, 32) == 560);
  CHECK(MaxBodyWidth(96, 800, 32) == 554);
  CHECK(MaxBodyWidth(96, 300, 32) == 200);
  CHECK(MaxBodyWidth(192, 3840, 64) == 1120);
}

}  // namespace

int main() {
  CheckChoice();
  CheckRowPicks();
  CheckRefusalBox();
  CheckConfigWrite();
  CheckStartWatch();
  CheckBoxLayout();

  std::printf("renderer choice: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
