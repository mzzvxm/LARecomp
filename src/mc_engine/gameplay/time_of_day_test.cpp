// Standalone check of the hour sources and the two guest hooks that use them.

#include "time_of_day_rules.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

int checks = 0;
int failures = 0;

void Check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAILED: %s\n", what);
    }
}

std::string Read(const std::filesystem::path& path) {
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void CheckSources() {
    using larecomp::tod::ResolvePin;
    int clock_reads = 0;
    int hold_reads = 0;
    float clock_hour = 11.0f;
    float held_hour = 6.0f;
    const auto clock = [&] { ++clock_reads; return clock_hour; };
    const auto held = [&] { ++hold_reads; return held_hour; };

    Check(ResolvePin(9000, 7000, true, clock, held) == 9.0f, "scene overrides a menu shot");
    Check(ResolvePin(-1, 6500, true, clock, held) == 6.5f, "menu shot overrides the clock");
    Check(clock_reads == 0 && hold_reads == 0, "scene and shot leave lower-priority sources unread");
    Check(ResolvePin(-1, -1, true, clock, held) == 11.0f, "real clock overrides hold");
    Check(clock_reads == 1 && hold_reads == 0, "clock leaves the held hour unread");

    clock_hour = -1.0f;
    Check(ResolvePin(-1, -1, false, clock, held) == -1.0f, "free-running stock clock");
    Check(hold_reads == 0, "hold is read only while armed");
    Check(ResolvePin(-1, -1, true, clock, held) == 6.0f, "first held frame uses 6H");
    held_hour = 9.0f;
    Check(ResolvePin(-1, -1, true, clock, held) == 9.0f, "next held frame uses 9H");
    Check(hold_reads == 2, "one read of the held hour per frame");
    Check(ResolvePin(0, 7000, false, clock, held) == 0.0f, "midnight is a scene request");
    Check(ResolvePin(-1, 24000, false, clock, held) == 0.0f, "shot wraps at midnight");
    held_hour = -0.5f;
    Check(ResolvePin(-1, -1, true, clock, held) == 0.0f, "held hour keeps its lower clamp");
}

void CheckHooks(const std::filesystem::path& root) {
    // Both hooks run in sub_822C1FA8. The first must run before the fog call
    // and the second before the stock clock publishes the same sample.
    const std::string config = Read(root / "larecomp_config.toml");
    Check(config.contains("[[midasm_hook]]\naddress = 0x822C2858\nname = \"Hook_TimeOfDayBeforeFog\"\n\n"),
          "pin hook runs before the fog call, after the scene tick and menu camera");
    Check(config.contains("[[midasm_hook]]\naddress = 0x822F1DB0\nname = \"Hook_TimeOfDay\"\n"),
          "stock render-slot publication still happens");

    const std::string source = Read(root / "src/mc_engine/gameplay/time_of_day.cpp");
    const auto early = source.find("void Hook_TimeOfDayBeforeFog() {");
    const auto late = source.find("void Hook_TimeOfDay(PPCRegister& r3) {");
    Check(early < late && late != std::string::npos, "both hooks are implemented");
    if (early >= late || late == std::string::npos) return;

    const std::string_view prepare(source.data() + early, late - early);
    const std::string_view publish(source.data() + late, source.size() - late);
    Check(prepare.contains("larecomp::tod::ResolvePin(scene_hour, shot_hour, hold,"),
          "the frame samples the common hour channels before fog");
    Check(prepare.contains("if (hour >= 0.0f)") &&
          prepare.contains("WriteF32(base, mgr + kTodCurrent, hour);"),
          "fog reads the selected hour while the stock clock keeps running when unpinned");
    Check(publish.contains("std::exchange(g_frame_pin, FramePin{})") &&
          publish.contains("frame.manager == mgr") &&
          publish.contains("want_pin = same_manager && frame.hour >= 0.0f"),
          "publication consumes this frame's sample for the same manager");
    // The late hook decides the hour itself ONLY when the pre-fog hook did not run in this update (the fallback
    // block); otherwise it publishes the hour fog used even if the request has changed since.
    const auto fallback = publish.find("if (frame.manager != mgr) {");
    const auto fallback_end = publish.find("const bool same_manager = frame.manager == mgr;");
    const auto resolve = publish.find("larecomp::tod::ResolvePin(");
    Check(fallback != std::string_view::npos && fallback_end != std::string_view::npos && fallback < fallback_end,
          "the late hook has a fallback for an update without the pre-fog hook, before it reads the sample");
    Check(resolve == std::string_view::npos || (resolve > fallback && resolve < fallback_end &&
          publish.find("larecomp::tod::ResolvePin(", resolve + 1) == std::string_view::npos),
          "the late hook resolves the hour only inside that fallback");
    Check(publish.find("WriteF32(base, mgr + kTodCurrent, frame.hour);") <
          publish.find("mcla::gpu::world_clock::Publish("),
          "the late hook publishes the hour fog used even if the request has changed");
    Check(publish.contains("if (s_pinned_manager == mgr)") &&
          publish.contains("base[mgr + kTodAuto] = s_saved_auto;"),
          "release restores auto-advance on the manager that was pinned");
}

}  // namespace

int main() {
    const auto root = std::filesystem::path(__FILE__).parent_path() / "../../..";
    CheckSources();
    CheckHooks(root);
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
