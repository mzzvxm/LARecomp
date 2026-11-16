// A check that needs no game: the pure part of the pad reader of LARecomp's own dialogs
// (pad_feed_core.h): merging pads, the dead zone, the back-off of empty XInput slots and the rule for
// looking at SDL's gamepad list again. Build: cmake --build <build dir> --target larecomp_pad_feed_test,
// then run it; exit 0 = passed. The SDL and XInput sides (ui/gamepad.cpp) need a pad: see
// perf\scripts\chain-inputpoll2.ps1.

#include <cmath>
#include <cstdio>

#include "pad_feed_core.h"

using namespace larecomp::padfeed;

namespace {

int g_checks = 0;
int g_failed = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(cond)) {                                                  \
            ++g_failed;                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

PadSample Pad(uint16_t buttons, float lx = 0, float ly = 0, float rx = 0, float ry = 0) {
    PadSample p;
    p.connected = true;
    p.buttons = buttons;
    p.lx = lx;
    p.ly = ly;
    p.rx = rx;
    p.ry = ry;
    return p;
}

void TestMerge() {
    PadSample merged;
    CHECK(!merged.connected);
    // A pad that is not connected adds nothing, even with garbage in it.
    PadSample absent;
    absent.buttons = 0xFFFF;
    absent.lx = 1.0f;
    MergeInto(merged, absent);
    CHECK(!merged.connected && merged.buttons == 0 && merged.lx == 0.0f);

    MergeInto(merged, Pad(0x1000, 0.2f, -0.5f));
    CHECK(merged.connected && merged.buttons == 0x1000);
    CHECK(Near(merged.lx, 0.2f) && Near(merged.ly, -0.5f));

    // A second pad: its buttons add, each axis keeps whichever pad is pushed further, either way.
    MergeInto(merged, Pad(0x0010, -0.7f, 0.4f, 0.1f, 0.0f));
    CHECK(merged.buttons == 0x1010);
    CHECK(Near(merged.lx, -0.7f));   // 0.7 beats 0.2, and keeps its sign
    CHECK(Near(merged.ly, -0.5f));   // 0.5 beats 0.4
    CHECK(Near(merged.rx, 0.1f));

    // A resting pad does not cancel the one in use.
    MergeInto(merged, Pad(0, 0, 0, 0, 0));
    CHECK(Near(merged.lx, -0.7f) && merged.buttons == 0x1010);

    // The same strength on both sides: the first one stays (no flip-flop).
    PadSample tie = Pad(0, 0.5f);
    MergeInto(tie, Pad(0, -0.5f));
    CHECK(Near(tie.lx, 0.5f));
}

void TestAxis() {
    CHECK(Near(AxisToUnit(0), 0.0f));
    CHECK(Near(AxisToUnit(32767), 1.0f));
    CHECK(Near(AxisToUnit(-32767), -1.0f));
    CHECK(Near(AxisToUnit(-32768), -1.0f));  // clamped, not -1.00003
    CHECK(Near(AxisToUnit(16384), 0.50002f, 1e-3f));
}

void TestDeadZone() {
    float neg = -1, pos = -1;
    ApplyDeadZone(0.0f, 0.25f, &neg, &pos);
    CHECK(neg == 0.0f && pos == 0.0f);
    ApplyDeadZone(0.25f, 0.25f, &neg, &pos);  // the edge itself is still dead
    CHECK(neg == 0.0f && pos == 0.0f);
    ApplyDeadZone(1.0f, 0.25f, &neg, &pos);
    CHECK(neg == 0.0f && Near(pos, 1.0f));
    ApplyDeadZone(-1.0f, 0.25f, &neg, &pos);
    CHECK(pos == 0.0f && Near(neg, 1.0f));
    ApplyDeadZone(0.625f, 0.25f, &neg, &pos);  // halfway between the edge and the end
    CHECK(Near(pos, 0.5f) && neg == 0.0f);
    ApplyDeadZone(-0.1f, 0.35f, &neg, &pos);
    CHECK(neg == 0.0f && pos == 0.0f);
}

void TestBackoff() {
    SlotBackoff b;
    CHECK(!b.AnyConnected());
    // Nothing asked yet: every slot is due.
    for (int slot = 0; slot < SlotBackoff::kSlots; ++slot) CHECK(b.Due(slot, 5000));

    // Slot 0 has a pad, slots 1-3 are empty.
    b.Result(0, true, 5000);
    for (int slot = 1; slot < SlotBackoff::kSlots; ++slot) b.Result(slot, false, 5000);
    CHECK(b.AnyConnected() && b.ConnectedCount() == 1);

    // The pad is read on every frame, the empty slots are left alone for a second.
    for (uint64_t t = 5001; t < 6000; t += 16) {
        CHECK(b.Due(0, t));
        CHECK(!b.Due(1, t));
        CHECK(!b.Due(2, t));
        CHECK(!b.Due(3, t));
    }
    CHECK(!b.Due(1, 5999));
    CHECK(b.Due(1, 6000));  // exactly kRetryMs later
    CHECK(b.Due(3, 6500));

    // An empty slot that is asked again and is still empty waits another second from then.
    b.Result(1, false, 6000);
    CHECK(!b.Due(1, 6001) && !b.Due(1, 6999) && b.Due(1, 7000));

    // A pad plugged into slot 2 is noticed at its next ask and then read every frame.
    b.Result(2, true, 6500);
    CHECK(b.Due(2, 6501) && b.Due(2, 6516));
    CHECK(b.ConnectedCount() == 2);

    // The pad in slot 0 is unplugged: the slot becomes an empty one, asked again in a second.
    b.Result(0, false, 7000);
    CHECK(!b.Due(0, 7500) && b.Due(0, 8000));
    CHECK(b.AnyConnected());  // slot 2 is still there
    b.Result(2, false, 7000);
    CHECK(!b.AnyConnected());

    // Out-of-range slots are never due and never crash.
    CHECK(!b.Due(-1, 0) && !b.Due(SlotBackoff::kSlots, 0));
    b.Result(-1, true, 0);
    b.Result(SlotBackoff::kSlots, true, 0);
    CHECK(!b.AnyConnected());

    // The empty-slot cost over a long dialog: 60 s at 60 frames a second asks each empty slot
    // once a second, not 3600 times.
    SlotBackoff c;
    int asks = 0;
    for (uint64_t t = 0; t < 60000; t += 16) {
        for (int slot = 0; slot < SlotBackoff::kSlots; ++slot) {
            if (!c.Due(slot, t)) continue;
            ++asks;
            c.Result(slot, false, t);
        }
    }
    CHECK(asks <= 4 * 61);
    CHECK(asks >= 4 * 59);

    b.Reset();
    CHECK(b.Due(1, 0) && !b.AnyConnected());
}

void TestEnumeratePolicy() {
    EnumeratePolicy p;
    CHECK(p.Due(1000, 0));  // never done
    p.Done(1000, 7);
    CHECK(!p.Due(1001, 7));
    CHECK(!p.Due(1999, 7));
    CHECK(p.Due(2000, 7));  // a second later, whatever the watcher said
    CHECK(p.Due(1001, 8));  // the watcher saw a pad come or go
    p.Done(1001, 8);
    CHECK(!p.Due(1002, 8));
    CHECK(!p.Due(2000, 8));  // the second counts from the last look
    CHECK(p.Due(2001, 8));
    // 60 s of 60 fps frames: a look each second and none per frame.
    EnumeratePolicy q;
    int looks = 0;
    for (uint64_t t = 0; t < 60000; t += 16) {
        if (!q.Due(t, 3)) continue;
        ++looks;
        q.Done(t, 3);
    }
    CHECK(looks >= 59 && looks <= 61);
    q.Forget();
    CHECK(q.Due(61000, 3));
    // A generation counter that wrapped around still counts as a change.
    EnumeratePolicy r;
    r.Done(0, 0xFFFFFFFFu);
    CHECK(r.Due(1, 0));
}

}  // namespace

int main() {
    TestMerge();
    TestAxis();
    TestDeadZone();
    TestBackoff();
    TestEnumeratePolicy();
    if (g_failed != 0) {
        std::printf("pad_feed_test: %d of %d checks FAILED\n", g_failed, g_checks);
        return 1;
    }
    std::printf("pad_feed_test: %d checks passed\n", g_checks);
    return 0;
}
