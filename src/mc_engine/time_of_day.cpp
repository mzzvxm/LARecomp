#ifndef REXGLUE_HAS_XEO3_TARGET
//
// Live time of day.
//
// RE map (default.xex):
//
//   dword_8287E26C          mcLightingManager instance.
//   sub_822F1DB0(mgr)       Its per-frame time-of-day update — hooked here.
//   sub_822F1518(mgr,h,r)   Graphics_WarpToTimeOfDay(hour, rate): sets the
//                           target and raises the warp flag.
//   sub_822F10D0(mgr)       Graphics_GetTimeOfDay.
//   sub_82304F20            Graphics_GetAndSetAutoUpdateTimeOfDay: swaps the
//                           auto-advance byte.
//
//   Fields on mcLightingManager:
//     +14540 (0x38CC) float  current time of day, hours 0..24
//     +14545 (0x38D1) byte   auto-advance enabled
//     +14548 (0x38D4) float  advance/warp rate, hours per second
//     +14552 (0x38D8) byte   a warp is in progress
//     +14556 (0x38DC) float  warp target hour
//
//   The stock auto-advance rate is picked by the hour (sub_822F1DB0):
//     <= 4.5 or >= 19.0  -> 0.00556 h/s   (night, ~3 min per hour)
//     < 7.0  or >= 16.0  -> 0.00278 h/s   (dawn/dusk, ~6 min per hour)
//     otherwise          -> 0.00833 h/s   (midday, ~2 min per hour)
//
//   The menu's own presets (tune/ui/userinterface.sc.xml, TodMenu) call
//   garage.ChangeTOD with: sunrise 6.0, afternoon 16.75, sunset 17.4,
//   night 23.5 — useful reference points for the cvar.
//
// Registered as a midasm hook in larecomp_config.toml:
//   0x822F1DB0  Hook_TimeOfDay  (r3)
//

#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "larecomp_log.h"
#include "real_weather.h"
#include "time_of_day.h"

REXCVAR_DEFINE_BOOL(time_of_day_hold, false, "MCLA/World",
    "Freeze the world at time_of_day instead of letting the day cycle run. This "
    "is the on/off switch — the hour by itself changes nothing while this is "
    "off, so you can park a value and arm it later.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(time_of_day, 12.0, "MCLA/World",
    "Hour to hold the world at (0-24, fractional), applied while "
    "time_of_day_hold is on. Takes effect every frame, so dragging the bar walks "
    "the sun across the sky live. Menu presets: 6.0 sunrise, 16.75 afternoon, "
    "17.4 sunset, 23.5 night.")
    .range(0.0, 24.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(time_of_day_realtime, false, "MCLA/World",
    "Follow your computer's clock: the world sits at the real local hour and "
    "keeps up with it. Outranks time_of_day_hold.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(weather, "game", "MCLA/World",
    "Force the weather. 'game' leaves it alone, including its own random "
    "changes; a named sky pins that one and stops the automatic changes. 'real' "
    "follows the actual weather where you are, refreshed once an hour — it "
    "resolves your location from your IP through ipapi.co and reads the forecast "
    "from open-meteo.com, so those two services see your IP while it is on. "
    "Applies live.")
    .allowed({"game", "real", "nice", "cloudy", "stormy", "foggy"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(time_of_day_speed, 1.0, "MCLA/World",
    "Multiplier on the day cycle speed. Only applies while time_of_day_hold is "
    "off — a held hour does not advance at all. 1 = stock, 0 = frozen where it "
    "is, 10 = ten times faster. The stock rate varies with the hour (about 2 "
    "minutes per hour at midday, 3 at night).")
    .range(0.0, 100.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

constexpr uint32_t kLightingMgrPtr = 0x8287E26C;  // mcLightingManager global
constexpr uint32_t kTodCurrent = 14540;  // float, hours
constexpr uint32_t kTodAuto = 14545;     // byte
constexpr uint32_t kTodRate = 14548;     // float, hours per second
constexpr uint32_t kTodWarping = 14552;  // byte
constexpr uint32_t kTodTarget = 14556;   // float, hours

uint32_t ReadU32(const uint8_t* base, uint32_t addr) {
    uint32_t v;
    std::memcpy(&v, base + addr, sizeof(v));
    return __builtin_bswap32(v);
}

void WriteU32(uint8_t* base, uint32_t addr, uint32_t v) {
    v = __builtin_bswap32(v);
    std::memcpy(base + addr, &v, sizeof(v));
}

float ReadF32(const uint8_t* base, uint32_t addr) {
    uint32_t v = ReadU32(base, addr);
    float f;
    std::memcpy(&f, &v, sizeof(f));
    return f;
}

void WriteF32(uint8_t* base, uint32_t addr, float f) {
    uint32_t v;
    std::memcpy(&v, &f, sizeof(v));
    WriteU32(base, addr, v);
}

// ── Weather ───────────────────────────────────────────────────────────
//
//   dword_8287E134  current weather index
//   byte_8287E138   automatic weather change
//   0x827E1B00      name table: 0 Nice, 1 Cloudy, 2 Stormy, 3 Foggy
//   0x827E1B30..3C  blend weights, one-hot on the current index
//   sub_822E8A78    snaps those weights — what Weather_SetCurrentWeather(i, 1)
//                   calls. Replicated below rather than called, so no guest
//                   call is needed from the hook.
//   dword_8288B9F0  render buffer index; the per-buffer copy of the weights
//                   lives at 0x827E1B10 + 16*buffer.

constexpr uint32_t kWeatherIndex = 0x8287E134;
constexpr uint32_t kWeatherAuto = 0x8287E138;
constexpr uint32_t kWeatherWeights = 0x827E1B30;
constexpr uint32_t kWeatherWeightsBuf = 0x827E1B10;
constexpr uint32_t kRenderBufferIndex = 0x8288B9F0;

int64_t NowMs() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

constexpr const char* kWeatherNames[] = {"nice", "cloudy", "stormy", "foggy"};

int WeatherNameToIndex(const std::string& name) {
    for (int i = 0; i < 4; ++i) {
        if (name == kWeatherNames[i]) return i;
    }
    return -1;  // "game" / "real" / anything else
}

void SnapWeather(uint8_t* base, int idx) {
    uint32_t buf = ReadU32(base, kRenderBufferIndex);
    for (int i = 0; i < 4; ++i) {
        float w = (i == idx) ? 1.0f : 0.0f;
        WriteF32(base, kWeatherWeights + 4 * i, w);
        WriteF32(base, kWeatherWeightsBuf + 16 * buf + 4 * i, w);
    }
}

// Last manager seen by the hook, so the accessors below work off-thread.
std::atomic<uint32_t> g_mgr{0};
// Hour requested by a menu-camera shot, renewed every frame while it applies.
std::atomic<int> g_shot_hour_milli{-1};
// Same, for the sky.
std::atomic<int> g_shot_weather{-1};

}  // namespace

int Weather_GetIndex() {
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return -1;
    int idx = static_cast<int>(ReadU32(base, kWeatherIndex));
    return (idx >= 0 && idx < 4) ? idx : -1;
}

void Weather_RequestIndex(int index) {
    g_shot_weather.store(index >= 0 && index < 4 ? index : -1,
                         std::memory_order_relaxed);
}

const char* Weather_IndexToName(int index) {
    return (index >= 0 && index < 4) ? kWeatherNames[index] : nullptr;
}

int Weather_NameToIndex(const char* name) {
    return name ? WeatherNameToIndex(std::string(name)) : -1;
}

float TimeOfDay_GetHour() {
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return -1.0f;
    // Read the manager from its global rather than from whatever the hook last
    // saw: the menu-camera hook can run before the lighting update on a frame,
    // and did — that is why recorded shots came out with no hour.
    uint32_t mgr = ReadU32(base, kLightingMgrPtr);
    if (mgr < 0x10000u) mgr = g_mgr.load(std::memory_order_relaxed);
    if (mgr < 0x10000u) return -1.0f;
    return ReadF32(base, mgr + kTodCurrent);
}

void TimeOfDay_RequestHour(float hour) {
    g_shot_hour_milli.store(hour < 0.0f ? -1 : static_cast<int>(hour * 1000.0f),
                            std::memory_order_relaxed);
}

// mcLightingManager's per-frame time-of-day update (sub_822F1DB0). r3 = manager.
//
// We set the fields and let the original body run: with auto-advance off and no
// warp pending it skips both the advance and the warp maths and just publishes
// the hour to the render slot at the end, so the lighting, sky, shadows and
// headlights all follow through the game's own path.
void Hook_TimeOfDay(PPCRegister& r3) {
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    uint32_t mgr = static_cast<uint32_t>(r3.u64);
    if (mgr < 0x10000u) return;
    g_mgr.store(mgr, std::memory_order_relaxed);

    // ── Weather ───────────────────────────────────────────────────────
    {
        static bool s_forced = false;
        static uint8_t s_saved_auto = 1;
        static int s_forced_idx = -1;

        std::string want_weather = REXCVAR_GET(weather);
        bool real = (want_weather == "real");
        RealWeather_SetEnabled(real);

        // A shot's sky outranks the cvar, same contract as its hour.
        int shot_wx = g_shot_weather.load(std::memory_order_relaxed);

        // 'real' waits for the first fetch to land; until then the game keeps
        // its own sky rather than snapping to a placeholder.
        int idx = shot_wx >= 0
                      ? shot_wx
                      : (real ? RealWeather_GetIndex()
                              : WeatherNameToIndex(want_weather));
        if (idx < 0) {
            if (s_forced) {
                base[kWeatherAuto] = s_saved_auto;
                s_forced = false;
                s_forced_idx = -1;
            }
        } else {
            if (!s_forced) {
                s_saved_auto = base[kWeatherAuto];
                s_forced = true;
            }
            base[kWeatherAuto] = 0;
            if (s_forced_idx != idx ||
                static_cast<int>(ReadU32(base, kWeatherIndex)) != idx) {
                WriteU32(base, kWeatherIndex, static_cast<uint32_t>(idx));
                SnapWeather(base, idx);
                s_forced_idx = idx;
                LARECOMP_APP_INFO("[Weather] forced to index {}", idx);
            }
        }
    }

    static bool s_first = true;
    if (s_first) {
        s_first = false;
        LARECOMP_APP_INFO("[TimeOfDay] hook live, mgr=0x{:08X}, world at {:.2f}h",
                          mgr, ReadF32(base, mgr + kTodCurrent));
    }

    // The auto-advance byte is the game's, not ours. Pinning an hour clears it,
    // so it has to be put back the moment we stop pinning — otherwise the day
    // cycle stays dead for the rest of the session and every later request to
    // just rescale it silently does nothing, because there is no advance left to
    // scale. That is exactly what made time_of_day_speed look broken.
    static bool s_pinned = false;
    static uint8_t s_saved_auto = 1;

    // A shot's hour outranks everything: it only lasts while the menu camera
    // keeps renewing it.
    int shot = g_shot_hour_milli.load(std::memory_order_relaxed);
    bool want_pin = shot >= 0;
    float hour = want_pin ? static_cast<float>(shot) / 1000.0f : 0.0f;

    if (!want_pin && REXCVAR_GET(time_of_day_realtime)) {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        hour = static_cast<float>(tm.tm_hour) + static_cast<float>(tm.tm_min) / 60.0f +
               static_cast<float>(tm.tm_sec) / 3600.0f;
        want_pin = true;
    }

    bool hold_on = REXCVAR_GET(time_of_day_hold);

    // The cvar tracks the world whenever it is not the thing driving it: boot at
    // 15h and it reads 15, drive until 18h and it reads 18. So arming the hold
    // needs no separate seeding — the value is already the current hour — and
    // releasing it lets the cycle carry on from wherever you left it.
    if (!hold_on) {
        static int64_t s_next_mirror_ms = 0;
        static float s_mirrored = -1.0f;
        int64_t now_ms = NowMs();
        float current = ReadF32(base, mgr + kTodCurrent);
        if (now_ms >= s_next_mirror_ms && current >= 0.0f && current < 24.0f &&
            (s_mirrored < 0.0f || std::fabs(current - s_mirrored) > 0.005f)) {
            s_next_mirror_ms = now_ms + 200;
            s_mirrored = current;
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.4f", current);
            rex::cvar::SetFlagByName("time_of_day", buf);
        }
    }

    if (!want_pin && hold_on) {
        hour = static_cast<float>(REXCVAR_GET(time_of_day));
        want_pin = true;
    }

    if (!want_pin) {
        if (s_pinned) {
            base[mgr + kTodAuto] = s_saved_auto;  // hand the day cycle back
            s_pinned = false;
        }

        // Rescale the game's own cycle. Rather than guess at the frame time and
        // the rate it will pick, measure what it actually advanced last frame and
        // add the missing share on top. Self-calibrating, and speed 0 lands on a
        // clean freeze because the extra is exactly minus the step.
        double speed = REXCVAR_GET(time_of_day_speed);
        static float s_prev = -1.0f;
        float now = ReadF32(base, mgr + kTodCurrent);
        if (speed != 1.0 && s_prev >= 0.0f) {
            float delta = now - s_prev;
            if (delta < 0.0f) delta += 24.0f;  // wrapped past midnight
            if (delta > 0.0f && delta < 1.0f) {
                float advanced = now + static_cast<float>(delta * (speed - 1.0));
                while (advanced >= 24.0f) advanced -= 24.0f;
                while (advanced < 0.0f) advanced += 24.0f;
                WriteF32(base, mgr + kTodCurrent, advanced);
                now = advanced;
            }
        }
        s_prev = now;
        return;
    }

    while (hour >= 24.0f) hour -= 24.0f;
    if (hour < 0.0f) hour = 0.0f;

    if (!s_pinned) {
        s_saved_auto = base[mgr + kTodAuto];
        s_pinned = true;
    }

    // Pin it: kill the auto-advance and any warp the menu may have started, then
    // write the hour straight in.
    base[mgr + kTodAuto] = 0;
    base[mgr + kTodWarping] = 0;
    WriteF32(base, mgr + kTodCurrent, hour);
    WriteF32(base, mgr + kTodTarget, hour);

    static float s_last = -1.0f;
    if (static_cast<int>(s_last * 60.0f) != static_cast<int>(hour * 60.0f)) {
        s_last = hour;
        int h = static_cast<int>(hour);
        int m = static_cast<int>((hour - h) * 60.0f);
        LARECOMP_APP_INFO("[TimeOfDay] holding at {:02d}:{:02d} ({:.2f})", h, m, hour);
    }
}

#else  // REXGLUE_HAS_XEO3_TARGET

#include <rex/ppc/context.h>

void Hook_TimeOfDay(PPCRegister& r3) {}

#endif  // REXGLUE_HAS_XEO3_TARGET
