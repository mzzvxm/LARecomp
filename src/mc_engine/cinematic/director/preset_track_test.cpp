#include "preset_command.h"
#include "session_settle.h"
#include "scene_edit.h"
#include "mc_engine/cinematic/scene.h"
#include "mc_engine/core/config_save.h"

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

namespace {
namespace gfx = mcla::gpu::gfx_preset;
using namespace mc::cinematic;
using namespace mc::cinematic::director;
int checks = 0;
int errors = 0;

void Check(bool ok, const char* description) {
    ++checks;
    if (!ok) {
        ++errors;
        std::fprintf(stderr, "FAIL %s\n", description);
    }
}

Json Read(const char* text) {
    Json result;
    if (!Json::Parse(text, &result, nullptr)) std::abort();
    return result;
}

void TestCommands() {
    Check(ParsePresetEdit(Read(R"({"name":"enhanced"})"), 1.0, false).preset == gfx::Preset::kEnhanced, "parse live preset");
    Check(ParsePresetEdit(Read(R"({"name":"enhanced_rt","persist":true})"), 0, false).persist, "explicit persistence");
    Check(!ParsePresetEdit(Read(R"({"name":"ultra"})"), 0, false).error.empty(), "refuse unknown name");
    Check(!ParsePresetEdit(Read(R"({"name":"vanilla","t":3})"), 0, false).error.empty(), "live switch cannot use t");
    Check(!ParsePresetEdit(Read(R"({"name":"vanilla","t":-1,"commit":true})"), 0, false).error.empty(), "refuse negative time");
    Check(!ParsePresetEdit(Read(R"({"name":"custom","commit":true,"persist":true})"), 0, false).error.empty(), "scene edit cannot save config");
    Check(!ParsePresetEdit(Read(R"({"clear":true,"name":"custom"})"), 0, false).error.empty(), "clear cannot also set a value");
    Check(!ParsePresetEdit(Read(R"({"clear":true})"), 0, false).preset, "clear returns to the scene");
    Check(ParsePresetEdit(Read(R"({"on":false})"), 0, true).label == false, "an explicit label off is kept");
    Check(!ParsePresetEdit(Read(R"({"on":"off"})"), 0, true).error.empty(), "label requires a bool");
    const auto passes = ParseTakePresets(Read(R"(["enhanced_rt","vanilla","custom"])"));
    Check(passes.error.empty() && passes.values.size() == 3 && passes.values.front() == gfx::Preset::kEnhancedRt && passes.mask == 13,
          "per-preset take preserves list order and warms requested slots");
    Check(!ParseTakePresets(Read(R"(["vanilla","vanilla"])")).error.empty(), "refuse duplicate pass");
    Check(!ParseTakePresets(Read("[]")).error.empty(), "refuse an empty pass list");
}

void TestTimeline() {
    const std::string start = "scene s fps 60 step 1/60\npreset vanilla\nat 3 preset enhanced\nat 6 preset enhanced_rt\nend 9\n";
    const auto parsed = ParseScene(start);
    Check(parsed.Ok(), "scene has three preset keys");
    Check(!EvaluatePreset(parsed.scene, -1).preset, "the scene has no preset before the start");
    Check(EvaluatePreset(parsed.scene, 0).preset == gfx::Preset::kVanilla, "frame 0");
    Check(EvaluatePreset(parsed.scene, 179.0 / 60).preset == gfx::Preset::kVanilla, "frame before first switch");
    Check(EvaluatePreset(parsed.scene, 180.0 / 60).preset == gfx::Preset::kEnhanced, "frame at first switch");
    Check(EvaluatePreset(parsed.scene, 359.0 / 60).preset == gfx::Preset::kEnhanced, "frame before second switch");
    Check(EvaluatePreset(parsed.scene, 360.0 / 60).preset == gfx::Preset::kEnhancedRt, "frame at second switch");
    Check(EvaluatePreset(parsed.scene, 2).preset == gfx::Preset::kVanilla, "backward scrub");
    Check(!ParseScene("preset nope\n").Ok() && !ParseScene("preset_label yes\n").Ok(), "refuse unknown preset and label keys");
    Check(!ParseScene("preset vanilla extra\n").Ok(), "refuse trailing key data");

    EditOp preset, label;
    preset.kind = label.kind = EditKind::kInsert;
    preset.text = ParsePresetEdit(Read(R"({"name":"custom","commit":true})"), 181.0 / 60, false).Line(false);
    label.text = ParsePresetEdit(Read(R"({"on":true,"commit":true,"t":3})"), 0, true).Line(true);
    const auto edited = SceneEdit::Apply(start, {preset, label}, 0);
    Check(edited.ok, "the Director inserts both keys");
    const auto round_trip = ParseScene(edited.text);
    Check(round_trip.Ok(), "edited scene round trips");
    Check(!EvaluatePreset(round_trip.scene, 179.0 / 60).label && EvaluatePreset(round_trip.scene, 3).label, "label edge is independent");
    Check(EvaluatePreset(round_trip.scene, 180.0 / 60).preset == gfx::Preset::kEnhanced &&
          EvaluatePreset(round_trip.scene, 181.0 / 60).preset == gfx::Preset::kCustom, "fractional playhead survives serialization");
    Check(EvaluatePreset(ParseScene(start).scene, 181.0 / 60).preset == gfx::Preset::kEnhanced, "the undo text restores the former key");
}

void TestSession() {
    using larecomp::CvarHold;
    constexpr auto file = static_cast<larecomp::cvar::Source>(1);
    constexpr auto runtime = static_cast<larecomp::cvar::Source>(4);
    CvarHold row{"player", file, "vanilla", "scene"};
    Check(!row.Visible(&CvarHold::preset) && row.Visible(&CvarHold::scene), "scene rows override the preset");
    row.preset = "enhanced_rt";
    row.Remove(&CvarHold::scene);
    Check(row.Visible(&CvarHold::preset) && row.preset == "enhanced_rt", "unload exposes the new lower preset");
    row.Remove(&CvarHold::preset);
    Check(!row.preset && row.value == "player" && row.source == file, "session switch restores config value and provenance");
    row.preset = "enhanced";
    row.Remove(&CvarHold::preset, runtime);
    Check(!row.preset && row.value == "enhanced" && row.source == runtime, "explicit promotion survives unload");

    Check(gfx::NextNamed(gfx::Preset::kVanilla, true) == gfx::Preset::kEnhanced, "vanilla to enhanced");
    Check(gfx::NextNamed(gfx::Preset::kEnhanced, true) == gfx::Preset::kEnhancedRt, "enhanced to RT");
    Check(gfx::NextNamed(gfx::Preset::kEnhancedRt, true) == gfx::Preset::kVanilla, "RT wraps");
    Check(gfx::NextNamed(gfx::Preset::kEnhanced, false) == gfx::Preset::kVanilla, "skip unavailable RT");
    Check(gfx::NextNamed(gfx::Preset::kCustom, true) == gfx::Preset::kVanilla, "custom enters the cycle at vanilla");

    SessionSettle gate;
    Check(!gate.Observe(true, 50, 10, 2) && gate.Observe(true, 62, 10, 2), "a preset settles after rendered frames");
    Check(!gate.Observe(true, 63, 10, 3) && !gate.Observe(true, 74, 10, 3) &&
          gate.Observe(true, 75, 10, 3), "switch invalidates an already settled window");
}
}  // namespace

int main() {
    TestCommands();
    TestTimeline();
    TestSession();
    std::printf("%d checks, %d failed\n", checks, errors);
    return errors != 0;
}
