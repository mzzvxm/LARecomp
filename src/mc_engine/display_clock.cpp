// The display's refresh clock. See display_clock.h.
//
// A thread waits on IDXGIOutput::WaitForVBlank of the monitor the game window
// is on and timestamps every return. The grid it publishes is anchored on the
// latest return and spaced by the median of recent single-refresh intervals.
//
// Anchored on the latest return, not fitted: a least-squares line through a
// few hundred returns drifted from 4.17 to 4.40 ms within a minute (measured
// against the swap chain's own frame statistics). The display is not a rigid
// grid -- with variable refresh a refresh stretches whenever nothing was
// flipped in time -- and every miscounted refresh fed back into the period
// that counts the next ones. What frame pacing needs is the next few
// refreshes, which the last one predicts best. The presenter paints on the
// same returns (its UI tick waits on the same output), so they are also the
// reference that matters.

#ifndef REXGLUE_HAS_XEO3_TARGET

#include "display_clock.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi.h>
#include <wrl/client.h>
#endif

#include <rex/logging.h>

namespace mc::display_clock {

int64_t Grid::IndexAtOrAfter(double qpc) const {
    if (!valid || period_qpc <= 0.0) return 0;
    // A hair of tolerance, so a time computed from Time(i) maps back to i.
    return anchor_index + int64_t(std::ceil((qpc - anchor_qpc) / period_qpc - 1e-6));
}

#if defined(_WIN32)

namespace {

using Microsoft::WRL::ComPtr;

std::atomic<HWND> g_hwnd{nullptr};
std::atomic<bool> g_started{false};
std::mutex g_grid_mutex;
Grid g_grid;

double QpcFrequency() {
    static const double f = [] {
        LARGE_INTEGER v;
        QueryPerformanceFrequency(&v);
        return double(v.QuadPart);
    }();
    return f;
}

void Publish(const Grid& grid) {
    std::lock_guard<std::mutex> lock(g_grid_mutex);
    g_grid = grid;
}

ComPtr<IDXGIOutput> FindOutput(IDXGIFactory1* factory, HMONITOR monitor) {
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc;
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) {
                return output;
            }
        }
    }
    return nullptr;
}

void ClockThread() {
    SetThreadDescription(GetCurrentThread(), L"MCLA VBlank Clock");
    // The timestamps are only as good as the wake-up after each vblank.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    ComPtr<IDXGIFactory1> factory;
    HMONITOR monitor = nullptr;
    ComPtr<IDXGIOutput> output;
    // Recent single-refresh intervals; their median is the period. A median,
    // so that a stretched refresh or a late wake-up moves nothing.
    constexpr size_t kIntervals = 63;
    double intervals[kIntervals] = {};
    size_t interval_count = 0;
    size_t interval_next = 0;
    int64_t index = 0;
    double last = 0.0;
    double period = 0.0;
    uint32_t until_check = 0;
    uint32_t fast_returns = 0;
    const double freq = QpcFrequency();

    auto reset = [&] {
        interval_count = 0;
        interval_next = 0;
        index = 0;
        last = 0.0;
        period = 0.0;
        Publish(Grid{});
    };

    for (;;) {
        if (until_check-- == 0) {
            // Follow the window to another monitor, and notice new outputs.
            until_check = 480;
            HWND hwnd = g_hwnd.load(std::memory_order_relaxed);
            HMONITOR now_on = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)
                                   : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
            if (!factory || !factory->IsCurrent()) {
                factory.Reset();
                if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
                    factory.Reset();
                }
                output.Reset();
            }
            if (factory && (now_on != monitor || !output)) {
                monitor = now_on;
                output = FindOutput(factory.Get(), monitor);
                reset();
                REXLOG_INFO("[display_clock] {} the window's monitor",
                            output ? "following" : "no DXGI output for");
            }
        }
        if (!output) {
            Sleep(50);
            until_check = 0;
            continue;
        }

        const HRESULT hr = output->WaitForVBlank();
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        const double now = double(t.QuadPart);
        if (FAILED(hr)) {
            reset();
            output.Reset();
            Sleep(10);
            until_check = 0;
            continue;
        }
        if (last != 0.0 && period > 0.0 && now - last < 0.3 * period) {
            // Returned without waiting (a monitor going to sleep, a mode
            // change): not a vblank.
            if (++fast_returns > 16) {
                reset();
                Sleep(5);
            }
            continue;
        }
        fast_returns = 0;
        if (last == 0.0) {
            last = now;
            continue;
        }
        const double dt = now - last;
        if (period <= 0.0 && dt > 0.05 * freq) {
            // A first interval this long is a stall, not a refresh period.
            last = now;
            continue;
        }
        const double reference = period > 0.0 ? period : dt;
        const int64_t refreshes = std::max<int64_t>(1, std::llround(dt / reference));
        if (refreshes == 1 && (period <= 0.0 || (dt > 0.7 * period && dt < 1.3 * period))) {
            intervals[interval_next] = dt;
            interval_next = (interval_next + 1) % kIntervals;
            if (interval_count < kIntervals) ++interval_count;
        }
        index += refreshes;
        last = now;
        if (interval_count < 15) {
            period = dt;
            continue;
        }
        double sorted[kIntervals];
        std::copy(intervals, intervals + interval_count, sorted);
        std::nth_element(sorted, sorted + interval_count / 2, sorted + interval_count);
        period = sorted[interval_count / 2];
        if (!(period > freq / 500.0 && period < freq / 20.0)) {
            reset();
            continue;
        }
        Grid grid;
        grid.valid = true;
        grid.anchor_index = index;
        grid.anchor_qpc = now;
        grid.period_qpc = period;
        grid.qpc_frequency = freq;
        Publish(grid);
    }
}

}  // namespace

void SetWindow(void* hwnd) { g_hwnd.store(static_cast<HWND>(hwnd), std::memory_order_relaxed); }

void EnsureRunning() {
    if (g_started.load(std::memory_order_relaxed)) return;
    bool expected = false;
    if (g_started.compare_exchange_strong(expected, true)) {
        std::thread(ClockThread).detach();
    }
}

Grid Get() {
    std::lock_guard<std::mutex> lock(g_grid_mutex);
    return g_grid;
}

double MonitorRefreshHz() {
    // Re-read every couple of seconds: the window can move, the mode change.
    static std::atomic<double> cached{0.0};
    static std::atomic<uint64_t> read_at{0};
    const uint64_t now = QpcNow();
    const double freq = QpcFrequency();
    if (cached.load(std::memory_order_relaxed) > 0.0 &&
        double(now - read_at.load(std::memory_order_relaxed)) < 2.0 * freq) {
        return cached.load(std::memory_order_relaxed);
    }
    read_at.store(now, std::memory_order_relaxed);
    HWND hwnd = g_hwnd.load(std::memory_order_relaxed);
    HMONITOR monitor = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)
                            : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW info = {};
    info.cbSize = sizeof(info);
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    double hz = 0.0;
    if (GetMonitorInfoW(monitor, &info) &&
        EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
        mode.dmDisplayFrequency > 1) {
        hz = double(mode.dmDisplayFrequency);
    }
    cached.store(hz, std::memory_order_relaxed);
    return hz;
}

uint64_t QpcNow() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return uint64_t(t.QuadPart);
}

#else

void SetWindow(void*) {}
void EnsureRunning() {}
Grid Get() { return Grid{}; }
double MonitorRefreshHz() { return 0.0; }
uint64_t QpcNow() { return 0; }

#endif  // _WIN32

}  // namespace mc::display_clock

#endif  // REXGLUE_HAS_XEO3_TARGET
