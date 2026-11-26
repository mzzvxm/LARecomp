#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>

namespace mc::guest_timing {

enum class Section : uint8_t {
    kUpdate, kSubmit, kHousekeeping, kDensity, kPropDetail, kCityDetail, kTrafficLod,
    kMirrorHooks, kPlanarCarTest, kTrafficShadowHooks, kCascadeCarDraws, kLights,
    kCarWalkTap, kCapture, kHudMapCapture, kCensus, kPresentWait, kReadbackWait, kHud,
    kPlanarPhase, kEnvironmentPhases, kCascadePhases, kMirrorPhase, kOtherPhases, kNone
};

inline constexpr const char* kNames[] = {
    "update", "submit", "housekeeping", "density", "prop_detail", "city_detail", "traffic_lod",
    "mirror_hooks", "planar_car_test", "traffic_shadow_hooks", "cascade_car_draws", "lights",
    "car_walk_tap", "capture", "hud_map_capture", "census", "present_wait", "readback_wait", "hud",
    "planar_phase", "environment_phases", "cascade_phases", "mirror_phase", "other_phases"
};
inline constexpr size_t kSections = static_cast<size_t>(Section::kNone);
static_assert(std::size(kNames) == kSections);

constexpr Section PhaseSection(uint32_t phase) {
    switch (phase) {
        case 0x2: return Section::kPlanarPhase;
        case 0x4:
        case 0x8: return Section::kEnvironmentPhases;
        case 0x20:
        case 0x40:
        case 0x80:
        case 0x100: return Section::kCascadePhases;
        case 0x1000000: return Section::kMirrorPhase;
        default: return Section::kOtherPhases;
    }
}

constexpr uint64_t Elapsed(uint64_t begin, uint64_t end) {
    return end > begin ? end - begin : 0;
}

struct Frame {
    std::array<uint64_t, kSections> ticks{};
    std::array<uint32_t, kSections> calls{};

    void Add(Section section, uint64_t begin, uint64_t end) {
        const size_t i = static_cast<size_t>(section);
        ticks[i] += Elapsed(begin, end);
        ++calls[i];
    }
};

inline constexpr size_t kWindowFrames = 120;

class Window {
public:
    bool Push(const Frame& frame) {
        for (size_t i = 0; i < kSections; ++i) {
            totals_.ticks[i] += frame.ticks[i];
            totals_.calls[i] += frame.calls[i];
            samples_[i][count_] = frame.ticks[i];
        }
        return ++count_ == kWindowFrames;
    }

    struct Summary { double mean; uint64_t p95; double calls; };

    // Report only full windows. The rank is over per-frame totals, including zero on frames without the section.
    Summary Read(size_t section) {
        auto& values = samples_[section];
        const auto p95 = values.begin() + (kWindowFrames * 95 + 99) / 100 - 1;
        std::nth_element(values.begin(), p95, values.end());
        return {double(totals_.ticks[section]) / kWindowFrames, *p95,
                double(totals_.calls[section]) / kWindowFrames};
    }

    void Reset() {
        count_ = 0;
        totals_ = {};
    }

private:
    std::array<std::array<uint64_t, kWindowFrames>, kSections> samples_;
    Frame totals_;
    size_t count_ = 0;
};

// The guest loop can visit end of phase for a bit that did not draw. Advance the
// cursor either way, so time in a masked phase never rolls into the next phase.
struct Phases {
    uint64_t cursor = 0;
    uint32_t requested = 0;

    void End(Frame& frame, uint32_t phase, uint64_t now) {
        if (cursor && (phase & requested) != 0) {
            frame.Add(PhaseSection(phase), cursor, now);
        }
        if (cursor) cursor = now;
    }
};

}  // namespace mc::guest_timing
