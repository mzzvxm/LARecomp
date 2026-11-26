#include "guest_timing_rules.h"
#include "guest_work_rules.h"
#include "ambient_density.h"
#include "../../gpu/traffic_shadow.h"

#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
namespace gt = mc::guest_timing;
namespace gw = mc::guest_work;
namespace ts = mcla::gpu::traffic_shadow;
namespace ad = mcla::ambient;

unsigned checks = 0;

void Check(bool ok, const char* message) {
    ++checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

void CheckWindow() {
    gt::Window window;
    constexpr size_t density = size_t(gt::Section::kDensity);
    constexpr size_t mirror = size_t(gt::Section::kMirrorPhase);
    for (uint64_t i = 1; i <= gt::kWindowFrames; ++i) {
        gt::Frame frame;
        frame.Add(gt::Section::kDensity, 100, 100 + i);
        if (i >= 115) frame.Add(gt::Section::kMirrorPhase, 100, 200);
        Check(window.Push(frame) == (i == gt::kWindowFrames), "window closes at exactly 120 samples");
    }
    auto result = window.Read(density);
    Check(result.mean == 60.5 && result.p95 == 114 && result.calls == 1, "mean and nearest-rank p95 of the full window");
    result = window.Read(mirror);
    Check(result.mean == 5 && result.p95 == 0 && result.calls == 0.05, "missing section is zero in percentiles and means");
    window.Reset();
    for (size_t i = 0; i < gt::kWindowFrames; ++i) {
        gt::Frame frame;
        frame.Add(gt::Section::kDensity, 200, 100);
        frame.Add(gt::Section::kDensity, 300, 312);
        window.Push(frame);
    }
    result = window.Read(density);
    Check(result.mean == 12 && result.p95 == 12 && result.calls == 2, "reset clears previous data and clamps a reversed clock");
}

void CheckPhases() {
    gt::Frame frame;
    gt::Phases phases{100, 0x12C};
    phases.End(frame, 0x2, 110);
    phases.End(frame, 0x4, 130);
    phases.End(frame, 0x8, 170);
    phases.End(frame, 0x20, 190);
    phases.End(frame, 0x100, 210);
    phases.End(frame, 0x1000000, 250);
    Check(frame.ticks[size_t(gt::Section::kPlanarPhase)] == 0, "masked planar phase is zero");
    Check(frame.ticks[size_t(gt::Section::kEnvironmentPhases)] == 60, "both environment phases are timed");
    Check(frame.ticks[size_t(gt::Section::kCascadePhases)] == 40, "cascade phases accumulate");
    Check(frame.ticks[size_t(gt::Section::kMirrorPhase)] == 0, "held mirror phase is zero");
    Check(gt::PhaseSection(0x2) == gt::Section::kPlanarPhase &&
          gt::PhaseSection(0x1000000) == gt::Section::kMirrorPhase &&
          gt::PhaseSection(0x400) == gt::Section::kOtherPhases, "phase classification");
}

void CheckDensityLookup() {
    gw::ZoneLookup<ad::ZoneState, 31> zones;
    auto& constructor = zones.At(0x1000);
    const ad::ZoneDensity original{300, 400, 700, 10, 0.25f, 1.5f, 1};
    const ad::DensityWant want{400, 0.5f, 1, 1};
    const auto first = ad::StepZone(constructor, original, true, want, true);
    auto& tick = zones.At(0x1000, 0, true);
    Check(&constructor == &tick && !ad::StepZone(tick, first.out, true, want, false).write,
          "constructor and indexed path use the same original");
    Check(&tick == &zones.At(0x1000, 0, false), "switch off keeps the same state");
    auto reparsed = original;
    reparsed.ped = 20;
    const auto next = ad::StepZone(zones.At(0x1000, 0, true), reparsed, true, want, false);
    Check(next.captured && next.out.ped == 10, "live zone reparse still uses new originals");
    Check(!zones.At(0x2000, 1, true).has_written, "distinct indexed zones have independent state");
    zones.Reset();
    Check(!zones.At(0x2000, 0, true).has_written, "new manager clears indexed state");
}

void CheckMirrorCarLookup() {
    struct Car { uint32_t item; };
    const std::array<Car, 5> ranked{{{22}, {9}, {42}, {16}, {7}}};
    gw::MirrorCarLookup<ranked.size()> lookup;
    for (const bool indexed : {true, false, true}) {
        lookup.Build(ranked, 4, indexed);
        for (uint32_t item = 0; item <= 50; ++item) {
            const bool selected = item == 9 || item == 16 || item == 22 || item == 42;
            Check(lookup.Contains(ranked, 4, item) == selected, "both paths use the active prefix of the ranked snapshot");
        }
        const std::array<Car, 5> next{{{80}, {6}, {22}, {9}, {42}}};
        lookup.Build(next, 2, indexed);
        Check(lookup.Contains(next, 2, 80) && lookup.Contains(next, 2, 6), "rebuilding indexes the new view");
        Check(!lookup.Contains(next, 2, 42), "the previous view leaves no stale entries");
        Check(!lookup.Contains(next, 0, 80), "zero selected cars excludes all previous entries");
    }
}

void CheckCutoff() {
    Check(!ts::NeedsCurrentEye(true, false, true), "no viewport read when the main eye serves the caster");
    Check(ts::NeedsCurrentEye(false, false, true), "an unavailable main eye still falls back");
    Check(ts::NeedsCurrentEye(true, true, true), "the census still reads both eyes");
    Check(ts::NeedsCurrentEye(true, false, false), "legacy path reads both eyes");
    uint32_t seed = 1001;
    for (size_t trial = 0; trial < 200; ++trial) {
        float distances[40];
        for (float& distance : distances) {
            seed = 1664525u * seed + 1013904223u;
            distance = seed % 7u ? float((seed >> 16) % 161u) : std::numeric_limits<float>::infinity();
        }
        for (uint32_t cap = 0; cap <= 24; ++cap) {
            const ts::Limits limits{trial % 2 ? 40.0f : 150.0f, cap, 4};
            Check(ts::CutoffFor(distances, 40, limits, true) == ts::CutoffFor(distances, 40, limits, false),
                  "selection and legacy sort give the same cutoff with ties and invalid samples");
        }
    }
    ts::Selector selector;
    unsigned policy_reads = 0;
    const auto use_selection = [&] { ++policy_reads; return true; };
    const ts::Limits limits{40.0f, 1, 4};
    selector.Decide(1, 0x20, 10.0f, limits, use_selection);
    selector.Decide(2, 0x20, 15.0f, limits, use_selection);
    selector.Decide(2, 0x40, 15.0f, limits, use_selection);
    Check(policy_reads == 0, "the A/B setting is not read for every car");
    Check(selector.Decide(1, 0x20, 10.0f, limits, use_selection) && policy_reads == 1 &&
          selector.last().cutoff == 12.5f, "next frame reads the A/B setting and publishes the same cutoff");
}
}  // namespace

int main() {
    CheckWindow();
    CheckPhases();
    CheckDensityLookup();
    CheckMirrorCarLookup();
    CheckCutoff();
    std::printf("guest timing and work: %u checks passed\n", checks);
}
