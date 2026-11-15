// A check that needs no game: the device watcher's table of present HID interfaces and its retry
// schedule (device_watch_core.h) and the DualSense effect worker's mailbox and write pacing
// (trigger_mailbox.h). Build: cmake --build <build dir> --target larecomp_device_watch_test, then run
// it; exit 0 = passed. The OS side (CM_Register_Notification, SDL's event watch, the HID writes) is
// not in here: it needs a pad, see perf\scripts\chain-dshitch.ps1.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "device_watch_core.h"
#include "trigger_mailbox.h"

using namespace larecomp::devwatch;
using namespace larecomp::triggers;
using namespace std::chrono_literals;

namespace {

int g_checks = 0;
int g_failed = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failed;                                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
        }                                                                              \
    } while (0)

// Links as Windows names them (upper case, as the OS gives them; the table lower-cases).
const std::wstring kDualSenseUsb = L"\\\\?\\HID#VID_054C&PID_0CE6&MI_03#7&1A2B3C4D&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}";
const std::wstring kDualSenseBt =
    L"\\\\?\\HID#{00001124-0000-1000-8000-00805F9B34FB}_VID&0002054C_PID&0CE6#9&2A3B4C5D&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}";
const std::wstring kDualSenseEdge = L"\\\\?\\HID#VID_054C&PID_0DF2&MI_03#7&AABBCCDD&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}";
const std::wstring kDualShock4 = L"\\\\?\\HID#VID_054C&PID_09CC#6&11223344&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}";
const std::wstring kXbox = L"\\\\?\\HID#VID_045E&PID_0B13&IG_00#7&FFEEDDCC&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}";
const std::wstring kKeyboard = L"\\\\?\\HID#VID_046D&PID_C33C&MI_00#7&12121212&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}";
// A name that merely contains the digits: not a Sony device.
const std::wstring kLookalike = L"\\\\?\\HID#VID_1234&PID_0CE6#6&054C0000&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}";

void TestClassify() {
    LinkInfo info = ClassifyLink(LowerLink(kDualSenseUsb));
    CHECK(info.sony && info.dualsense && !info.bluetooth);
    info = ClassifyLink(LowerLink(kDualSenseBt));
    CHECK(info.sony && info.dualsense && info.bluetooth);
    info = ClassifyLink(LowerLink(kDualSenseEdge));
    CHECK(info.sony && info.dualsense);
    info = ClassifyLink(LowerLink(kDualShock4));
    CHECK(info.sony && !info.dualsense);  // Sony, so the PlayStation glyphs; not a DualSense, so no trigger effects
    info = ClassifyLink(LowerLink(kXbox));
    CHECK(!info.sony && !info.dualsense);
    info = ClassifyLink(LowerLink(kKeyboard));
    CHECK(!info.sony && !info.dualsense);
    info = ClassifyLink(LowerLink(kLookalike));
    CHECK(!info.sony && !info.dualsense);
    CHECK(LowerLink(L"AbC-0CE6_\u00dc") == L"abc-0ce6_\u00dc");  // only ASCII letters are touched
}

void TestTableBasics() {
    DeviceTable table;
    CHECK(!table.ready());
    CHECK(table.summary().dualsense.empty() && !table.summary().sony);
    CHECK(table.generation() == 0);

    // The first list makes the table ready, even an empty one.
    CHECK(table.ApplySnapshot({kKeyboard, kXbox}));
    CHECK(table.ready());
    CHECK(table.count() == 2);
    CHECK(!table.summary().sony);
    const uint32_t g1 = table.generation();
    CHECK(g1 == 1);

    // A DualSense arrives: the summary changes.
    CHECK(table.Arrival(kDualSenseUsb));
    CHECK(table.summary().sony);
    CHECK(table.summary().dualsense.size() == 1);
    CHECK(table.summary().dualsense[0] == LowerLink(kDualSenseUsb));
    CHECK(table.generation() == g1 + 1);

    // The same arrival again: nothing changes.
    CHECK(!table.Arrival(kDualSenseUsb));
    CHECK(table.generation() == g1 + 1);

    // Other devices come and go: the summary does not change, so no generation moves.
    const uint32_t g2 = table.generation();
    CHECK(!table.Arrival(kKeyboard + L"2"));
    CHECK(!table.Removal(kKeyboard));
    CHECK(!table.Removal(L"\\\\?\\hid#never-seen"));
    CHECK(table.generation() == g2);

    // A second pad (Bluetooth) and a DualShock 4.
    CHECK(table.Arrival(kDualSenseBt));
    CHECK(table.summary().dualsense.size() == 2);
    CHECK(!table.Arrival(kDualShock4));  // Sony was already true and it is no DualSense: the summary is the same
    CHECK(table.summary().dualsense.size() == 2);

    // Removal: matched whatever the case of the link.
    CHECK(table.Removal(kDualSenseUsb));  // upper case, as the OS sends it
    CHECK(table.summary().dualsense.size() == 1);
    CHECK(table.summary().sony);
    CHECK(table.Removal(kDualSenseBt));
    CHECK(table.summary().dualsense.empty());
    CHECK(table.summary().sony);  // the DualShock 4 is still there
    CHECK(table.Removal(kDualShock4));
    CHECK(!table.summary().sony);
}

void TestTableOrderAroundTheFirstList() {
    // Events that arrive before the first list lands are replayed over it, in order.
    {
        DeviceTable table;
        CHECK(!table.Arrival(kDualSenseUsb));  // held
        CHECK(!table.ready());
        CHECK(!table.summary().sony);
        // The list was taken before the pad arrived: the arrival is replayed and wins.
        CHECK(table.ApplySnapshot({kKeyboard}));
        CHECK(table.summary().sony && table.summary().dualsense.size() == 1);
    }
    {
        DeviceTable table;
        CHECK(!table.Removal(kDualSenseUsb));  // held
        // The list still had the pad (taken just before it went): the removal wins.
        CHECK(table.ApplySnapshot({kDualSenseUsb, kKeyboard}));
        CHECK(!table.summary().sony && table.summary().dualsense.empty());
    }
    {
        DeviceTable table;
        table.Arrival(kDualSenseUsb);
        table.Removal(kDualSenseUsb);
        CHECK(table.ApplySnapshot({}));  // arrived and left before the list: gone
        CHECK(!table.summary().sony);
    }
    {
        DeviceTable table;
        table.Removal(kDualSenseUsb);
        table.Arrival(kDualSenseUsb);
        CHECK(table.ApplySnapshot({}));  // left and came back (a re-plug) after the list: present
        CHECK(table.summary().sony);
    }
    {
        DeviceTable table;
        table.Arrival(kDualSenseUsb);
        CHECK(table.ApplySnapshot({kDualSenseUsb}));  // already in the list: once, not twice
        CHECK(table.summary().dualsense.size() == 1);
        CHECK(table.count() == 1);
    }
    {
        // A later list replaces what the table knew (the watcher's fallback re-list).
        DeviceTable table;
        table.ApplySnapshot({kDualSenseUsb, kDualSenseBt});
        CHECK(table.summary().dualsense.size() == 2);
        CHECK(table.ApplySnapshot({kDualSenseBt}));
        CHECK(table.summary().dualsense.size() == 1);
        CHECK(!table.ApplySnapshot({kDualSenseBt}));  // unchanged: no change reported
    }
}

void TestRetry() {
    RetryBackoff retry(3);
    const std::wstring a = L"a";
    const std::wstring b = L"b";
    CHECK(retry.Allowed(a, 0));
    CHECK(retry.NextDueMs(0) == -1);

    retry.Failed(a, 1000, 2000);  // due at 3000
    CHECK(!retry.Allowed(a, 1000));
    CHECK(!retry.Allowed(a, 2999));
    CHECK(retry.Allowed(a, 3000));
    CHECK(retry.NextDueMs(1000) == 2000);
    CHECK(retry.NextDueMs(2500) == 500);
    CHECK(retry.NextDueMs(3500) == 0);  // overdue: now
    CHECK(retry.Allowed(b, 1000));      // other keys are not affected

    retry.Failed(a, 3000, 2000);
    retry.Failed(a, 5000, 2000);  // the third failure: given up
    CHECK(retry.Failures(a) == 3);
    CHECK(!retry.Allowed(a, 1000000));
    CHECK(retry.NextDueMs(1000000) == -1);

    retry.Clear(a);  // a device change
    CHECK(retry.Allowed(a, 0));

    retry.Failed(a, 0, 100);
    retry.Failed(b, 0, 50);
    CHECK(retry.NextDueMs(0) == 50);
    retry.Retain({a});  // b left
    CHECK(retry.Failures(b) == 0);
    CHECK(retry.NextDueMs(0) == 100);
    retry.ClearAll();
    CHECK(retry.NextDueMs(0) == -1);
}

void TestMailboxBasics() {
    LatestMailbox<int> box;
    int latest = -1;

    // Nothing posted: a short wait times out and leaves the value alone.
    LatestMailbox<int>::Result r = box.Wait(latest, 5ms);
    CHECK(r.timed_out && !r.new_value && !r.woken && !r.closed);
    CHECK(latest == -1);

    // Latest wins: three posts, one wait, the third value, once.
    box.Post(1);
    box.Post(2);
    box.Post(3);
    r = box.Wait(latest, 5ms);
    CHECK(r.new_value && !r.timed_out && latest == 3);
    r = box.Wait(latest, 5ms);
    CHECK(r.timed_out && !r.new_value && latest == 3);

    // A wake-up without a value.
    box.Wake();
    r = box.Wait(latest, 5ms);
    CHECK(r.woken && !r.new_value && !r.timed_out && latest == 3);
    r = box.Wait(latest, 5ms);
    CHECK(!r.woken && r.timed_out);

    // Both at once.
    box.Post(7);
    box.Wake();
    r = box.Wait(latest, 5ms);
    CHECK(r.new_value && r.woken && latest == 7);

    // Close ends every wait at once, even one with no timeout.
    box.Close();
    r = box.Wait(latest, std::chrono::milliseconds(-1));
    CHECK(r.closed && !r.timed_out);
    r = box.Wait(latest, std::chrono::milliseconds(-1));
    CHECK(r.closed);
}

void TestMailboxThreads() {
    // A waiter with no timeout is woken by a post from another thread.
    {
        LatestMailbox<int> box;
        std::atomic<int> got{-1};
        std::thread waiter([&] {
            int latest = 0;
            const auto r = box.Wait(latest, std::chrono::milliseconds(-1));
            if (r.new_value) got.store(latest);
        });
        std::this_thread::sleep_for(20ms);
        box.Post(42);
        waiter.join();
        CHECK(got.load() == 42);
    }
    // ... and by Close.
    {
        LatestMailbox<int> box;
        std::atomic<bool> closed{false};
        std::thread waiter([&] {
            int latest = 0;
            closed.store(box.Wait(latest, std::chrono::milliseconds(-1)).closed);
        });
        std::this_thread::sleep_for(20ms);
        box.Close();
        waiter.join();
        CHECK(closed.load());
    }
    // The poster is never held up by a worker that is busy (it stands for a write that waits on a
    // slow pad): while the worker sits on the first value, a thousand more go in, and only the last
    // one is there when the worker comes back.
    {
        LatestMailbox<int> box;
        std::atomic<bool> busy{false};
        std::atomic<bool> release{false};
        std::atomic<int> second{-1};
        std::thread worker([&] {
            int latest = 0;
            box.Wait(latest, std::chrono::milliseconds(-1));  // takes 0
            busy.store(true);
            while (!release.load()) std::this_thread::sleep_for(1ms);
            const auto r = box.Wait(latest, std::chrono::milliseconds(-1));
            if (r.new_value) second.store(latest);
        });
        box.Post(0);
        while (!busy.load()) std::this_thread::sleep_for(1ms);
        auto posting = std::async(std::launch::async, [&] {
            for (int i = 1; i <= 1000; ++i) box.Post(i);
        });
        const bool done_while_busy = posting.wait_for(5s) == std::future_status::ready;
        CHECK(done_while_busy);
        release.store(true);
        worker.join();
        CHECK(second.load() == 1000);
    }
}

void TestDecideWrite() {
    constexpr int64_t kStep = 100;
    // Never written: write now, whatever else.
    WriteDecision d = DecideWrite(false, false, false, 0, 1000, kStep);
    CHECK(d.action == WriteAction::kNow);
    d = DecideWrite(false, false, true, 0, 1000, kStep);
    CHECK(d.action == WriteAction::kNow);  // the first write of a vibration is not paced
    // Written and the same: nothing, vibrating or not.
    d = DecideWrite(true, true, false, 900, 1000, kStep);
    CHECK(d.action == WriteAction::kNothing);
    d = DecideWrite(true, true, true, 990, 1000, kStep);
    CHECK(d.action == WriteAction::kNothing);
    // A change that is not a vibration is never held back.
    d = DecideWrite(true, false, false, 990, 1000, kStep);
    CHECK(d.action == WriteAction::kNow);
    // A vibration that changes within the step waits for the rest of it ...
    d = DecideWrite(true, false, true, 950, 1000, kStep);
    CHECK(d.action == WriteAction::kLater && d.wait_ms == 50);
    d = DecideWrite(true, false, true, 1000, 1000, kStep);
    CHECK(d.action == WriteAction::kLater && d.wait_ms == 100);
    // ... and goes at the end of it.
    d = DecideWrite(true, false, true, 900, 1000, kStep);
    CHECK(d.action == WriteAction::kNow);
    d = DecideWrite(true, false, true, 100, 1000, kStep);
    CHECK(d.action == WriteAction::kNow);
}

}  // namespace

int main() {
    TestClassify();
    TestTableBasics();
    TestTableOrderAroundTheFirstList();
    TestRetry();
    TestMailboxBasics();
    TestMailboxThreads();
    TestDecideWrite();
    if (g_failed) {
        std::printf("device_watch_test: %d of %d checks FAILED\n", g_failed, g_checks);
        return 1;
    }
    std::printf("device_watch_test: %d checks passed\n", g_checks);
    return 0;
}
