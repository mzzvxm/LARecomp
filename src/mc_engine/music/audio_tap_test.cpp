// Check of the audio tap (audio_tap.h): the SDK's fill reaches the device unchanged outside a take; while a take holds the tap the device
// gets silence and the SDK's fill is called by the take alone, never at the same moment as by the device. A program of its own: no game, no
// SDK, exits 0 when every check holds.
//
//   tools/unit-test.ps1 -Test src/mc_engine/music/audio_tap_test.cpp -Inc src/mc_engine/music      (or the CMake target larecomp_audio_tap_test)

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "audio_tap.h"

using mcla::audio::MixTap;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAILED: %s\n", what.c_str());
    }
}

// A stand-in for the SDK's mix: writes a counter into every sample, remembers the channel count, and notices a second caller at the same time.
struct FakeSdkFill {
    std::atomic<int> calls{0};
    std::atomic<int> inside{0};
    std::atomic<int> overlaps{0};
    std::atomic<uint32_t> last_channels{0};
    std::atomic<std::thread::id> last_caller{};

    MixTap::Fill Make() {
        return [this](std::span<float> out, uint32_t channels) {
            if (inside.fetch_add(1) != 0) overlaps.fetch_add(1);
            const int n = calls.fetch_add(1) + 1;
            last_channels = channels;
            last_caller = std::this_thread::get_id();
            for (float& f : out) f = static_cast<float>(n);
            inside.fetch_sub(1);
        };
    }
};

void TestPassThrough() {
    MixTap tap;
    FakeSdkFill sdk;
    Check(!tap.HasDevice() && !tap.Hold() && !tap.Held(), "no device yet: nothing to hold");
    std::vector<float> none(4, 7.0f);
    Check(!tap.Pull(none, 2), "no device yet: nothing to pull");
    const MixTap::Fill device = tap.Attach(sdk.Make());
    Check(tap.HasDevice() && !tap.Held(), "attached: a device, not held");
    std::vector<float> buf(512, 0.0f);
    device(buf, 2);
    Check(sdk.calls == 1 && buf[0] == 1.0f && buf[511] == 1.0f && sdk.last_channels == 2, "not held: the device's fill is the SDK's (2 channels)");
    device(buf, 6);
    Check(sdk.calls == 2 && buf[0] == 2.0f && sdk.last_channels == 6, "not held: and with 6 channels");
    Check(!tap.Pull(buf, 2), "not held: the take cannot pull");
    Check(tap.SilentCalls() == 0, "not held: no silent call");
}

void TestHold() {
    MixTap tap;
    FakeSdkFill sdk;
    const MixTap::Fill device = tap.Attach(sdk.Make());
    Check(tap.Hold() && tap.Held(), "held");
    std::vector<float> buf(512, 9.0f);
    device(buf, 2);
    bool all_zero = true;
    for (float f : buf) all_zero = all_zero && f == 0.0f;
    Check(all_zero && sdk.calls == 0, "held: the device gets silence and the SDK's mix is not called");
    Check(tap.SilentCalls() == 1, "held: one silent call counted");
    std::vector<float> pulled(256 * 6, 0.0f);
    Check(tap.Pull(pulled, 6), "held: the take pulls");
    Check(sdk.calls == 1 && pulled[0] == 1.0f && pulled[256 * 6 - 1] == 1.0f && sdk.last_channels == 6, "held: it is the SDK's mix, with the channels the take asked for");
    tap.Release();
    Check(!tap.Held(), "released");
    device(buf, 2);
    Check(sdk.calls == 2 && buf[0] == 2.0f, "released: the device has the SDK's mix again");
}

void TestDeviceGoesAndComes() {
    MixTap tap;
    FakeSdkFill sdk;
    MixTap::Fill device = tap.Attach(sdk.Make());
    Check(tap.Hold(), "held");
    tap.Detach();
    std::vector<float> buf(256 * 2, 0.0f);
    Check(!tap.HasDevice() && !tap.Pull(buf, 2), "the device closed during the take: the take cannot pull");
    Check(tap.Held(), "the hold stays");
    FakeSdkFill sdk2;
    device = tap.Attach(sdk2.Make());  // the SDK reopens the device with a new fill
    Check(tap.Held(), "a device that comes back during the take is still held");
    std::fill(buf.begin(), buf.end(), 5.0f);
    device(buf, 2);
    Check(buf[0] == 0.0f && sdk2.calls == 0, "and it gets silence");
    Check(tap.Pull(buf, 2) && sdk2.calls == 1 && buf[0] == 1.0f, "and the take pulls from its fill");
    tap.Release();
    Check(tap.Hold(), "a take after that can hold again");
    tap.Release();
    tap.Detach();
    Check(!tap.Hold(), "no device: no hold");
}

// The device's thread runs all the time; a take holds, pulls and releases over and over. The SDK's fill never runs twice at once, and while
// held the device thread never reaches it.
void TestThreads() {
    MixTap tap;
    FakeSdkFill sdk;
    const MixTap::Fill device = tap.Attach(sdk.Make());
    std::atomic<bool> stop{false};
    std::atomic<int> device_calls{0};
    std::atomic<int> device_reached_sdk_while_held{0};
    std::thread dev([&] {
        std::vector<float> b(480 * 2, 0.0f);
        while (!stop.load()) {
            device(b, 2);
            ++device_calls;
        }
    });
    int pulls = 0;
    std::vector<float> pulled(256 * 2, 0.0f);
    for (int i = 0; i < 300; ++i) {
        if (!tap.Hold()) continue;
        const int calls_at_hold = sdk.calls.load();
        for (int k = 0; k < 4; ++k) {
            if (tap.Pull(pulled, 2)) ++pulls;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        // during the hold the only callers of the SDK's mix were these pulls
        if (sdk.calls.load() - calls_at_hold != 4 || sdk.last_caller.load() != std::this_thread::get_id()) ++device_reached_sdk_while_held;
        tap.Release();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    stop = true;
    dev.join();
    Check(pulls == 1200, "threads: every pull of a held tap worked");
    Check(sdk.overlaps == 0, "threads: the SDK's mix never ran twice at the same time");
    Check(device_reached_sdk_while_held == 0, "threads: while held, the device thread never reached the SDK's mix");
    Check(device_calls > 0 && tap.SilentCalls() > 0, "threads: the device thread ran, and got silence while held");
}

}  // namespace

int main() {
    TestPassThrough();
    TestHold();
    TestDeviceGoesAndComes();
    TestThreads();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
