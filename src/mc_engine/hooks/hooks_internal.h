#pragma once
//
// Shared by the hook files in this folder and nothing else; hooks.h is the
// public side. All of it was file-local while every hook lived in one
// hooks.cpp: the big-endian guest accessors, the guest clock fields, and the
// functions one category calls from another -- mostly InitHooks() and
// Patch_DeltaTimePre() in hooks.cpp calling into the rest.
//

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

// Guest memory is big-endian; these read and write it straight off the membase.
inline uint32_t ReadGuestU32(const uint8_t* base, uint32_t addr) {
    return (uint32_t(base[addr + 0]) << 24) | (uint32_t(base[addr + 1]) << 16) |
           (uint32_t(base[addr + 2]) << 8) | uint32_t(base[addr + 3]);
}

inline void WriteGuestU32(uint8_t* base, uint32_t addr, uint32_t val) {
    base[addr + 0] = (val >> 24) & 0xFF;
    base[addr + 1] = (val >> 16) & 0xFF;
    base[addr + 2] = (val >> 8) & 0xFF;
    base[addr + 3] = val & 0xFF;
}

inline float ReadGuestF32(const uint8_t* base, uint32_t addr) {
    uint32_t be = (uint32_t(base[addr + 0]) << 24) | (uint32_t(base[addr + 1]) << 16) |
                  (uint32_t(base[addr + 2]) << 8) | uint32_t(base[addr + 3]);
    float val;
    std::memcpy(&val, &be, sizeof(float));
    return val;
}

inline void WriteGuestF32(uint8_t* base, uint32_t addr, float val) {
    uint32_t be;
    std::memcpy(&be, &val, sizeof(float));
    base[addr + 0] = (be >> 24) & 0xFF;
    base[addr + 1] = (be >> 16) & 0xFF;
    base[addr + 2] = (be >> 8) & 0xFF;
    base[addr + 3] = be & 0xFF;
}

// The guest timer object is at 0x827D7500, so its struct offsets map onto
// absolute addresses. [r3+8] is the published frame delta, already used by the
// camera and chassis hooks.
constexpr uint32_t kGuestFrameDelta  = 0x827D7508;  // [r3+8]
constexpr uint32_t kGuestFrameRate   = 0x827D750C;  // [r3+12]
constexpr uint32_t kGuestAccumA      = 0x827D7514;  // [r3+20]
constexpr uint32_t kGuestAccumB      = 0x827D7518;  // [r3+24]
constexpr uint32_t kGuestTimeScale   = 0x827D7554;  // [r3+84]
// The guest's OWN delta clamp, applied by the fsel pairs at 0x821BDC78 and
// 0x821BDC88 at the end of sub_821BDA90: [r3+40] is a floor and [r3+36] a
// ceiling on both [r3+8] and [r3+88]. An engine_dt pinned to exactly the
// ceiling means real frames are at or past it - a symptom of slow frames, not
// a cause. Logged so that is visible rather than inferred.
constexpr uint32_t kGuestDtMax      = 0x827D7524;  // [r3+36]
constexpr uint32_t kGuestDtMin      = 0x827D7528;  // [r3+40]

// graphics.cpp
void ApplyAspectRatioPatch(std::string_view ratio);

// readback_window.cpp
// Who is holding the global resolve readback (d3d12_/vulkan_readback_resolve) open.
enum class ReadbackClient : uint8_t { kVinyl, kPhoto, kCount };
// Keeps the resolve readback on for at least `hold_ns` from now on behalf of
// `client`. Leaves it alone if the user turned it on themselves.
void HoldGlobalReadback(ReadbackClient client, int64_t hold_ns);
// Per frame: turns it back off once every client's hold has run out, if it was
// HoldGlobalReadback that turned it on.
void TickGlobalReadback();

// photo_album.cpp
// From TakePicture until the grab is done (snapshot vhsm phase 6 or 3).
bool PhotoCaptureInProgress(const uint8_t* base);
// A tenth of a second from photoAlbum.EnableCity: the city draws again while the
// album movie still shows the full-screen photo.
bool PhotoCityReturning();
// photo_ui_trace is on and the photo album scene is up.
bool PhotoUiTracing(const uint8_t* base);

// vinyl.cpp
void ApplyVinylLayerCaps();
void TickVinylReadbackWindow();
void ExportVinyl();
void ImportVinyl(const std::string& name_in);

// vinyl_shapes.cpp
void DumpVinylShapes();
void RequestVinylShapeCapture();
void TickVinylShapeCapture();

// dev_options.cpp
void ApplyDebugOptions();
void ApplyPerfDebugOptions();
void ApplyLoadTimeDevOptions();

// render_phases.cpp
extern std::atomic<uint64_t> g_frame_heartbeat;  // bumped by Patch_DeltaTimePre
void ApplyRenderPhaseMask();
void LogRenderPhaseMaskOnce();
void StartFreezeWatchdog();

// rear_view.cpp
void TickRearView();

// world_tuning.cpp
void ApplyAmbientDensityTuning();
void ApplyFragTuneOverrides();

// rubberband.cpp
void ApplyRubberBandLevel();
void ApplyRubberBandScales();
void DumpRubberBandTuning();

// frame_timing.cpp: the spin counters the timing log reports.
extern std::atomic<uint64_t> g_fence_hook_calls;
extern std::atomic<uint64_t> g_fence_switches;
extern std::atomic<uint64_t> g_fence_switch_us;
extern std::atomic<uint64_t> g_limiter_spin_us;
extern std::atomic<uint64_t> g_limiter_spins;

// timing_log.cpp
bool TimingLogEnabled();
void RecordFrameTime();
extern std::atomic<int32_t> g_substep_last;
extern std::atomic<uint64_t> g_fixedstep_hits;
