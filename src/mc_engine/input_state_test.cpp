// A check that needs no game: the active-device state machine (input_state_core.h) with a fake clock, the rules
// of KBM-DESIGN.md 2.2 / 2.3: what counts as activity (pad, keys, mouse), the hysteresis (jitter, drift, held
// sticks, key repeats, key-ups, trigger noise), the cross rules (a recent controller blocks mouse motion, the
// anti-flap time), hot-plug (start, removal, arrival), mixed input, the glyph setting and the generation counter;
// and (KBM P7) what the ImGui overlays use: the pad an overlay reads itself, the CLOSE word per glyph family
// (ui/overlay_input_core.h) and the controller toast's roster of connects and disconnects.
// Build: cmake --build <build dir> --target larecomp_input_state_test, then run it; exit 0 = passed. The glue
// (the window listener, the SDK's device list, the XamInputGetState wrapper) is not in here: it needs the game,
// see perf\scripts\chain-kbm-p2.ps1.

#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "input_state_core.h"
#include "ui/overlay_input_core.h"

using namespace larecomp::input;

namespace {

int g_checks = 0;
int g_failed = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        ++g_checks;                                                      \
        if (!(cond)) {                                                   \
            ++g_failed;                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                \
    } while (0)

constexpr uint16_t kA = 0x1000;
constexpr uint16_t kB = 0x2000;
constexpr uint16_t kLb = 0x0100;
constexpr uint16_t kGuide = 0x0400;

PadSample Buttons(uint16_t b) {
    PadSample s;
    s.buttons = b;
    return s;
}
PadSample Left(int x, int y = 0) {
    PadSample s;
    s.lx = static_cast<int16_t>(x);
    s.ly = static_cast<int16_t>(y);
    return s;
}
PadSample Right(int x, int y = 0) {
    PadSample s;
    s.rx = static_cast<int16_t>(x);
    s.ry = static_cast<int16_t>(y);
    return s;
}
PadSample Triggers(int lt, int rt) {
    PadSample s;
    s.lt = static_cast<uint8_t>(lt);
    s.rt = static_cast<uint8_t>(rt);
    return s;
}

const auto kUnknown = [] { return -1; };
const auto kXboxPad = [] { return 0; };
const auto kPsPad = [] { return 1; };

// A core that has seen its list of devices: one Xbox pad, the start rule applied (Pad, Xbox).
void OneXboxPad(ActiveInputCore& c) {
    const GlyphFamily pads[] = {GlyphFamily::Xbox};
    c.OnDevices(pads, 1, 0);
    c.OnFocus(true, 0, /*initial=*/true);
}

// ---- keys ------------------------------------------------------------------------------------------------

void TestKeys() {
    for (uint16_t vk : {0x41, 0x30, 0x0D, 0x1B, 0x20, 0x09, 0x08, 0x25, 0x26, 0x27, 0x28, 0x21, 0x22, 0x7C, 0x7D, 0x87, 0xBA}) {
        CHECK(IsActivityKey(vk));  // letters, digits, Return, Escape, Space, Tab, Backspace, arrows, PgUp / PgDn, F13+, OEM
    }
    for (uint16_t vk : {0x10, 0x11, 0x12, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0x5B, 0x5C}) {
        CHECK(!IsActivityKey(vk));  // Shift, Ctrl, Alt (any side), the Windows keys
    }
    for (uint16_t vk : {0x14, 0x90, 0x91, 0x13, 0x2C}) CHECK(!IsActivityKey(vk));  // Caps, Num, Scroll lock, Pause, Print Screen
    for (uint16_t vk = 0x70; vk <= 0x7B; ++vk) CHECK(!IsActivityKey(vk));          // F1 .. F12 (the overlay and window hotkeys)
    CHECK(IsActivityKey(0x7C));                                                    // F13 is the first one that counts
    CHECK(!IsActivityKey(0));
    CHECK(!IsActivityKey(0x01) && !IsActivityKey(0x06));  // mouse buttons arrive as mouse events
}

// ---- the pad filter --------------------------------------------------------------------------------------

void TestPadFilterButtons() {
    PadFilter f;
    CHECK(f.Feed(PadSample{}) == PadCause::None);
    CHECK(f.Feed(Buttons(kA)) == PadCause::Button);   // a button going down
    CHECK(f.Feed(Buttons(kA)) == PadCause::None);     // held: not again
    CHECK(f.Feed(Buttons(0)) == PadCause::None);      // released: never
    CHECK(f.Feed(Buttons(kA)) == PadCause::Button);   // pressed again
    CHECK(f.Feed(Buttons(kA | kB)) == PadCause::Button);  // a second button while the first is held
    CHECK(f.Feed(Buttons(kB)) == PadCause::None);     // one released
    CHECK(f.Feed(Buttons(0)) == PadCause::None);
    CHECK(f.Feed(Buttons(kGuide)) == PadCause::None); // the Guide bit is not a button press
    CHECK(f.Feed(Buttons(0x0800)) == PadCause::None);
    CHECK(f.Feed(Buttons(0x0001)) == PadCause::Button);  // D-pad up
    CHECK(f.Feed(Buttons(0x0001 | kLb)) == PadCause::Button);

    // The first sample after a Reset (the pad went away and came back) is compared with a pad at rest.
    CHECK(f.Feed(Buttons(0x0001 | kLb)) == PadCause::None);
    f.Reset();
    CHECK(f.Feed(Buttons(0x0001 | kLb)) == PadCause::Button);
}

void TestPadFilterTriggers() {
    PadFilter f;
    CHECK(f.Feed(Triggers(0, 0)) == PadCause::None);
    CHECK(f.Feed(Triggers(29, 0)) == PadCause::None);    // under the threshold: noise
    CHECK(f.Feed(Triggers(30, 0)) == PadCause::Trigger);  // rises to 30
    CHECK(f.Feed(Triggers(200, 0)) == PadCause::None);   // held and pressed harder: not again
    CHECK(f.Feed(Triggers(25, 0)) == PadCause::None);    // not below 20 yet: not armed
    CHECK(f.Feed(Triggers(35, 0)) == PadCause::None);    // a trigger that trembles around 30 counts once
    CHECK(f.Feed(Triggers(10, 0)) == PadCause::None);    // below 20: armed again
    CHECK(f.Feed(Triggers(40, 0)) == PadCause::Trigger);
    CHECK(f.Feed(Triggers(0, 29)) == PadCause::None);    // the right trigger is its own
    CHECK(f.Feed(Triggers(0, 31)) == PadCause::Trigger);
    CHECK(f.Feed(Triggers(0, 31)) == PadCause::None);
    // Both together: one answer, both armed off.
    PadFilter g;
    CHECK(g.Feed(Triggers(100, 100)) == PadCause::Trigger);
    CHECK(g.Feed(Triggers(100, 100)) == PadCause::None);
    CHECK(g.Feed(Triggers(0, 0)) == PadCause::None);
    CHECK(g.Feed(Triggers(0, 100)) == PadCause::Trigger);
    CHECK(g.Feed(Triggers(100, 100)) == PadCause::Trigger);  // the left one rises while the right is held
}

void TestPadFilterSticks() {
    PadFilter f;
    CHECK(f.Feed(Left(3000, -2000)) == PadCause::None);  // drift inside the dead zone, however long it lasts
    CHECK(f.Feed(Left(3000, -2000)) == PadCause::None);
    CHECK(f.Feed(Left(7000)) == PadCause::None);         // 7000 < 7849
    CHECK(f.Feed(Left(8000)) == PadCause::Stick);        // crosses the dead zone
    CHECK(f.Feed(Left(8000)) == PadCause::None);         // held still
    CHECK(f.Feed(Left(7000)) == PadCause::None);         // back in, but not below 3/4 of it (5886): not armed
    CHECK(f.Feed(Left(8000)) == PadCause::None);         // a stick that flickers around the edge counts once
    CHECK(f.Feed(Left(0)) == PadCause::None);            // released: armed again
    CHECK(f.Feed(Left(8000)) == PadCause::Stick);
    CHECK(f.Feed(Left(12000)) == PadCause::StickMove);   // past the dead zone and moved by 4000
    CHECK(f.Feed(Left(12500)) == PadCause::None);        // 500: a wobble
    CHECK(f.Feed(Left(14400)) == PadCause::None);        // 1900: under 2000
    CHECK(f.Feed(Left(16500)) == PadCause::StickMove);   // 2100: over it
    CHECK(f.Feed(Left(-16500)) == PadCause::StickMove);  // a full swing

    // The right stick has its own dead zone (8689) and arming.
    PadFilter g;
    CHECK(g.Feed(Right(8500)) == PadCause::None);
    CHECK(g.Feed(Right(9000)) == PadCause::Stick);
    CHECK(g.Feed(Left(8000)) == PadCause::Stick);        // the left one is independent of the right
    // The dead zone is a circle: a diagonal counts earlier on each axis.
    PadFilter h;
    CHECK(h.Feed(Left(6000, 6000)) == PadCause::Stick);  // sqrt(2) * 6000 = 8485 >= 7849
    CHECK(h.Feed(Left(5000, 5000)) == PadCause::None);   // 7071 < 7849 and not below 5886: not armed, no big move
    PadFilter i;
    CHECK(i.Feed(Left(5000, 5000)) == PadCause::None);   // 7071: inside

    // A button wins over everything else in the same sample.
    PadFilter j;
    PadSample s = Left(20000);
    s.buttons = kA;
    s.lt = 200;
    CHECK(j.Feed(s) == PadCause::Button);
}

// ---- the mouse filter -------------------------------------------------------------------------------------

void TestMotionFilter() {
    // The hand resting on the mouse: a pixel or two back and forth, 25 ms apart (the P0e plan's "jitter").
    {
        MotionFilter f;
        const float dx[] = {1, -1, 2, -2, 1, -1};
        bool any = false;
        for (int i = 0; i < 6; ++i) any = f.Feed(dx[i], 0, 25 * i) || any;
        CHECK(!any);
    }
    // One 4 px move.
    {
        MotionFilter f;
        CHECK(!f.Feed(4, 0, 1000));
    }
    // A short deliberate nudge: two 6 px moves 60 ms apart = 12 px in two events.
    {
        MotionFilter f;
        CHECK(!f.Feed(6, 0, 5000));
        CHECK(f.Feed(6, 0, 5060));
    }
    // A slow drift: 1 px every 100 ms for 2 s never reaches 8 px inside 200 ms.
    {
        MotionFilter f;
        bool any = false;
        for (int i = 0; i < 20; ++i) any = f.Feed(1, 0, 100 * i) || any;
        CHECK(!any);
    }
    // One big jump (a window move, a warp): a single event.
    {
        MotionFilter f;
        CHECK(!f.Feed(-200, 0, 0));
        CHECK(!f.Feed(0, 0, 10));      // no motion: ignored
        CHECK(!f.Feed(2, 0, 300));     // 300 ms later the jump has left the window: one event again
    }
    // Events with no motion are ignored and count for nothing.
    {
        MotionFilter f;
        CHECK(!f.Feed(0, 0, 0));
        CHECK(!f.Feed(20, 0, 10));
    }
    // Two moves further apart than the window are two single events.
    {
        MotionFilter f;
        CHECK(!f.Feed(5, 0, 0));
        CHECK(!f.Feed(5, 0, 250));
    }
    // The window is 200 ms and inclusive: 200 ms apart is still one window.
    {
        MotionFilter f;
        CHECK(!f.Feed(5, 0, 0));
        CHECK(f.Feed(5, 0, 200));  // 10 px in two events
    }
    // Diagonals: 3,3 twice = 8.49 px.
    {
        MotionFilter f;
        CHECK(!f.Feed(3, 3, 0));
        CHECK(f.Feed(3, 3, 10));
    }
    // Out and back: the move counts at its far end (the distance to the earlier position).
    {
        MotionFilter f;
        CHECK(!f.Feed(10, 0, 0));
        CHECK(f.Feed(-10, 0, 50));
    }
    // A steady 1 px per ms is a move at the eighth event (8 px), not before.
    {
        MotionFilter f;
        for (int i = 0; i < 7; ++i) CHECK(!f.Feed(1, 0, i));
        CHECK(f.Feed(1, 0, 7));
    }
    // More events than the ring holds do not break it: a slow drift at 1000 Hz of 0.01 px stays quiet.
    {
        MotionFilter f;
        bool any = false;
        for (int i = 0; i < 400; ++i) any = f.Feed(0.01f, 0.0f, i) || any;  // 4 px in 400 ms
        CHECK(!any);
    }
    // Reset forgets everything.
    {
        MotionFilter f;
        CHECK(!f.Feed(6, 0, 0));
        f.Reset();
        CHECK(!f.Feed(6, 0, 10));
        CHECK(f.Feed(6, 0, 20));
    }
}

// ---- start and hot-plug ------------------------------------------------------------------------------------

void TestStart() {
    {  // no controller: the keyboard and mouse
        ActiveInputCore c;
        CHECK(c.Get().device == InputDevice::Pad);  // the guess before the first list
        const Change ch = c.OnDevices(nullptr, 0, 10);
        CHECK(ch.changed);
        CHECK(ch.before.device == InputDevice::Pad && ch.after.device == InputDevice::KeyboardMouse);
        CHECK(c.Get().device == InputDevice::KeyboardMouse);
        CHECK(c.Get().Family() == GlyphFamily::Keyboard);
        CHECK(c.Get().PromptStrip() == GlyphFamily::Xbox);
        CHECK(c.Get().generation == 1);
        CHECK(c.PadCount() == 0);
    }
    {  // an Xbox pad: Pad, nothing to change
        ActiveInputCore c;
        const GlyphFamily pads[] = {GlyphFamily::Xbox};
        const Change ch = c.OnDevices(pads, 1, 10);
        CHECK(!ch.changed);
        CHECK(c.Get().device == InputDevice::Pad && c.Get().Family() == GlyphFamily::Xbox && c.Get().generation == 0);
        CHECK(c.PadCount() == 1);
    }
    {  // a PlayStation pad first, then an Xbox pad: the first connected one sets the family
        ActiveInputCore c;
        const GlyphFamily pads[] = {GlyphFamily::PlayStation, GlyphFamily::Xbox};
        const Change ch = c.OnDevices(pads, 2, 10);
        CHECK(ch.changed);
        CHECK(c.Get().device == InputDevice::Pad && c.Get().Family() == GlyphFamily::PlayStation && c.Get().PromptStrip() == GlyphFamily::PlayStation);
    }
    {  // Switch, Steam Deck and generic pads reach the core as Xbox (the glue maps them): Keyboard is never a pad family
        ActiveInputCore c;
        const GlyphFamily pads[] = {GlyphFamily::Keyboard};
        c.OnDevices(pads, 1, 10);
        CHECK(c.Get().device == InputDevice::Pad && c.Get().Family() == GlyphFamily::Xbox);
    }
    {  // the device watcher's guess: a Sony pad is plugged in, the SDK has not listed anything yet
        ActiveInputCore c;
        const Change seed = c.Seed(GlyphFamily::PlayStation);
        CHECK(seed.changed && seed.cause == Cause::Seed);
        CHECK(c.Get().device == InputDevice::Pad && c.Get().Family() == GlyphFamily::PlayStation);
        CHECK(!c.Seed(GlyphFamily::PlayStation).changed);  // same again: nothing
        CHECK(c.Seed(GlyphFamily::Xbox).changed);          // the watcher saw it go
        CHECK(c.Get().Family() == GlyphFamily::Xbox);
        // The SDK's first list takes over from the guess once it holds a controller: a guess after it is ignored.
        const GlyphFamily pads[] = {GlyphFamily::Xbox};
        c.OnDevices(pads, 1, 20);
        CHECK(!c.Seed(GlyphFamily::PlayStation).changed);
        CHECK(c.Get().Family() == GlyphFamily::Xbox);
    }
    {  // a list with no controller does not throw the guess away: SDL may list the pad a moment later
        ActiveInputCore c;
        c.Seed(GlyphFamily::PlayStation);
        const Change first = c.OnDevices(nullptr, 0, 10);
        CHECK(first.changed && c.Get().device == InputDevice::KeyboardMouse);
        CHECK(c.Get().PromptStrip() == GlyphFamily::PlayStation);  // the strip stayed PlayStation: no flicker
        const uint32_t gen = c.Get().generation;
        const GlyphFamily ps[] = {GlyphFamily::PlayStation};
        const Change later = c.OnDevices(ps, 1, 200);
        CHECK(later.changed && c.Get().device == InputDevice::Pad && c.Get().Family() == GlyphFamily::PlayStation);
        CHECK(c.Get().generation == gen + 1);  // one change: the device; the strip did not move
        // and the guess follows the watcher while the list holds no controller and nothing was used
        ActiveInputCore d;
        d.OnDevices(nullptr, 0, 10);
        CHECK(d.Get().PromptStrip() == GlyphFamily::Xbox);
        CHECK(d.Seed(GlyphFamily::PlayStation).changed && d.Get().PromptStrip() == GlyphFamily::PlayStation);
        CHECK(d.Seed(GlyphFamily::Xbox).changed && d.Get().PromptStrip() == GlyphFamily::Xbox);
        CHECK(d.Get().device == InputDevice::KeyboardMouse);
    }
    {  // the guess is ignored once something was used
        ActiveInputCore c;
        c.OnFocus(true, 0, true);
        c.OnKeyDown(0x41, false, 100);
        CHECK(!c.Seed(GlyphFamily::PlayStation).changed);
        CHECK(c.Get().device == InputDevice::KeyboardMouse && c.Get().PromptStrip() == GlyphFamily::Xbox);
    }
    {  // before any input a pad SDL announces late still counts as present at the start
        ActiveInputCore c;
        c.OnDevices(nullptr, 0, 10);  // the first poll: no pad yet
        CHECK(c.Get().device == InputDevice::KeyboardMouse);
        const GlyphFamily pads[] = {GlyphFamily::PlayStation};
        const Change ch = c.OnDevices(pads, 1, 3000);  // SDL lists it a moment later
        CHECK(ch.changed);
        CHECK(c.Get().device == InputDevice::Pad && c.Get().Family() == GlyphFamily::PlayStation);
        // and after it is gone again, before any input
        c.OnDevices(nullptr, 0, 4000);
        CHECK(c.Get().device == InputDevice::KeyboardMouse && c.Get().PromptStrip() == GlyphFamily::Xbox);
    }
    {  // after an input the same arrival changes nothing: the pad has to be used
        ActiveInputCore c;
        c.OnFocus(true, 0, true);
        c.OnDevices(nullptr, 0, 10);
        c.OnKeyDown(0x41, false, 100);
        const GlyphFamily pads[] = {GlyphFamily::PlayStation};
        const uint32_t gen = c.Get().generation;
        const Change ch = c.OnDevices(pads, 1, 3000);
        CHECK(!ch.changed && c.Get().generation == gen);
        CHECK(c.Get().device == InputDevice::KeyboardMouse);
        CHECK(c.PadCount() == 1);
        // its first input takes over, with its family
        const Change use = c.OnPadFrame(Buttons(kA), kPsPad, 3100);
        CHECK(use.changed && use.cause == Cause::PadButton);
        CHECK(c.Get().device == InputDevice::Pad && c.Get().Family() == GlyphFamily::PlayStation);
    }
}

void TestHotPlug() {
    {  // the active pad is the last one: removed, the keyboard and mouse take over at once
        ActiveInputCore c;
        OneXboxPad(c);
        c.OnPadFrame(Buttons(kA), kUnknown, 1000);
        CHECK(c.Get().device == InputDevice::Pad);
        const Change ch = c.OnDevices(nullptr, 0, 2000);
        CHECK(ch.changed && ch.cause == Cause::PadRemoved);
        CHECK(c.Get().device == InputDevice::KeyboardMouse && c.Get().Family() == GlyphFamily::Keyboard);
    }
    {  // the active PlayStation pad goes, an Xbox pad is still there: still Pad, Xbox glyphs
        ActiveInputCore c;
        c.OnFocus(true, 0, true);
        const GlyphFamily both[] = {GlyphFamily::PlayStation, GlyphFamily::Xbox};
        c.OnDevices(both, 2, 0);
        CHECK(c.Get().Family() == GlyphFamily::PlayStation);
        c.OnPadFrame(Buttons(kA), kUnknown, 1000);
        const GlyphFamily left[] = {GlyphFamily::Xbox};
        const Change ch = c.OnDevices(left, 1, 2000);
        CHECK(ch.changed && ch.cause == Cause::PadRemoved);
        CHECK(c.Get().device == InputDevice::Pad && c.Get().Family() == GlyphFamily::Xbox);
    }
    {  // a pad that is not the active one goes: nothing changes
        ActiveInputCore c;
        c.OnFocus(true, 0, true);
        const GlyphFamily both[] = {GlyphFamily::Xbox, GlyphFamily::PlayStation};
        c.OnDevices(both, 2, 0);
        c.OnPadFrame(Buttons(kA), kUnknown, 1000);
        const GlyphFamily left[] = {GlyphFamily::Xbox};
        const uint32_t gen = c.Get().generation;
        CHECK(!c.OnDevices(left, 1, 2000).changed);
        CHECK(c.Get().generation == gen && c.Get().Family() == GlyphFamily::Xbox);
    }
    {  // two pads of one family: one goes, nothing changes
        ActiveInputCore c;
        c.OnFocus(true, 0, true);
        const GlyphFamily two[] = {GlyphFamily::PlayStation, GlyphFamily::PlayStation};
        c.OnDevices(two, 2, 0);
        c.OnPadFrame(Buttons(kA), kUnknown, 1000);
        const GlyphFamily one[] = {GlyphFamily::PlayStation};
        CHECK(!c.OnDevices(one, 1, 2000).changed);
        CHECK(c.Get().Family() == GlyphFamily::PlayStation);
    }
    {  // a pad arrives while another is in use: nothing until it is used
        ActiveInputCore c;
        OneXboxPad(c);
        c.OnPadFrame(Buttons(kA), kUnknown, 1000);
        const GlyphFamily both[] = {GlyphFamily::Xbox, GlyphFamily::PlayStation};
        CHECK(!c.OnDevices(both, 2, 2000).changed);
        CHECK(c.Get().Family() == GlyphFamily::Xbox);
        // the newcomer is used: the family follows the pad pressed last
        const Change ch = c.OnPadFrame(Buttons(kB), kPsPad, 3000);
        CHECK(ch.changed && ch.before.device == InputDevice::Pad && ch.after.device == InputDevice::Pad);
        CHECK(c.Get().Family() == GlyphFamily::PlayStation);
        const Change back = c.OnPadFrame(Buttons(kA), kXboxPad, 4000);
        CHECK(back.changed && c.Get().Family() == GlyphFamily::Xbox);
    }
    {  // the keyboard is in use and the pad goes: the controller glyphs of the strip go too
        ActiveInputCore c;
        c.OnFocus(true, 0, true);
        const GlyphFamily ps[] = {GlyphFamily::PlayStation};
        c.OnDevices(ps, 1, 0);
        c.OnKeyDown(0x41, false, 1000);
        CHECK(c.Get().device == InputDevice::KeyboardMouse && c.Get().PromptStrip() == GlyphFamily::PlayStation);
        const Change ch = c.OnDevices(nullptr, 0, 2000);
        CHECK(ch.changed && ch.before.device == InputDevice::KeyboardMouse && ch.after.device == InputDevice::KeyboardMouse);
        CHECK(c.Get().PromptStrip() == GlyphFamily::Xbox);
    }
    {  // the list changes with the same pads: nothing
        ActiveInputCore c;
        OneXboxPad(c);
        const GlyphFamily one[] = {GlyphFamily::Xbox};
        const uint32_t gen = c.Get().generation;
        CHECK(!c.OnDevices(one, 1, 100).changed && c.Get().generation == gen);
    }
}

// ---- last input wins, hysteresis, cross rules ---------------------------------------------------------------

void TestLastInputWins() {
    ActiveInputCore c;
    OneXboxPad(c);
    uint32_t gen = c.Get().generation;
    CHECK(c.Get().device == InputDevice::Pad);

    Change ch = c.OnKeyDown(0x41, false, 1000);  // a key
    CHECK(ch.changed && ch.cause == Cause::Key && c.Get().device == InputDevice::KeyboardMouse);
    CHECK(c.Get().generation == ++gen);
    ch = c.OnPadFrame(Buttons(kA), kUnknown, 1100);  // a pad button, 100 ms later: no wait for any of these
    CHECK(ch.changed && ch.cause == Cause::PadButton && c.Get().device == InputDevice::Pad);
    CHECK(c.Get().generation == ++gen);
    ch = c.OnMouseButtonDown(1200);  // a click, 100 ms after a pad button: immediate
    CHECK(ch.changed && ch.cause == Cause::MouseClick && c.Get().device == InputDevice::KeyboardMouse);
    CHECK(c.Get().generation == ++gen);
    ch = c.OnPadFrame(Triggers(100, 0), kUnknown, 1300);  // a trigger
    CHECK(ch.changed && ch.cause == Cause::PadTrigger && c.Get().device == InputDevice::Pad);
    CHECK(c.Get().generation == ++gen);
    ch = c.OnMouseWheel(0, 120, 1400);  // a wheel notch
    CHECK(ch.changed && ch.cause == Cause::MouseWheel && c.Get().device == InputDevice::KeyboardMouse);
    CHECK(c.Get().generation == ++gen);
    ch = c.OnPadFrame(Left(15000), kUnknown, 1500);  // a stick crossing its dead zone
    CHECK(ch.changed && ch.cause == Cause::PadStick && c.Get().device == InputDevice::Pad);
    CHECK(c.Get().generation == ++gen);

    // The same device again: counted, nothing changes, the generation stays.
    ch = c.OnPadFrame(Buttons(kB), kUnknown, 1600);
    CHECK(!ch.changed && ch.looked == Cause::PadButton);
    CHECK(c.Get().generation == gen);
}

void TestHeldStickThenKey() {
    // "steering with a held stick, tap a key": the keyboard takes over and keeps it, because the held stick
    // does not count again; "keep driving on a key, tap a pad button": the pad takes over.
    ActiveInputCore c;
    OneXboxPad(c);
    c.OnPadFrame(Left(15000), kUnknown, 1000);
    CHECK(c.Get().device == InputDevice::Pad);
    c.OnKeyDown(0x57, false, 1100);
    CHECK(c.Get().device == InputDevice::KeyboardMouse);
    for (int i = 0; i < 20; ++i) {
        const Change ch = c.OnPadFrame(Left(15000), kUnknown, 1200 + 16 * i);  // the stick is still held
        CHECK(ch.looked == Cause::None);
    }
    CHECK(c.Get().device == InputDevice::KeyboardMouse);
    // a small wobble of the held stick does not count either
    CHECK(c.OnPadFrame(Left(15400), kUnknown, 2000).looked == Cause::None);
    c.OnKeyDown(0x41, false, 3000);
    CHECK(c.Get().device == InputDevice::KeyboardMouse);
    c.OnPadFrame(Buttons(kA), kUnknown, 3100);
    CHECK(c.Get().device == InputDevice::Pad);
}

void TestNoisyKeys() {
    ActiveInputCore c;
    OneXboxPad(c);
    c.OnPadFrame(Buttons(kA), kUnknown, 1000);
    const uint32_t gen = c.Get().generation;
    Change ch = c.OnKeyDown(0x41, true, 1100);  // an auto-repeat of a key that was already down
    CHECK(!ch.changed && ch.looked == Cause::Key);
    ch = c.OnKeyDown(0x10, false, 1110);  // Shift
    CHECK(!ch.changed && ch.looked == Cause::Key);
    ch = c.OnKeyDown(0xA2, false, 1120);  // left Ctrl
    CHECK(!ch.changed);
    ch = c.OnKeyDown(0x5B, false, 1130);  // the Windows key
    CHECK(!ch.changed);
    ch = c.OnKeyDown(0x14, false, 1140);  // Caps lock
    CHECK(!ch.changed);
    ch = c.OnKeyDown(0x73, false, 1150);  // F4: the settings panel's hotkey
    CHECK(!ch.changed);
    ch = c.OnKeyDown(0x2C, false, 1160);  // Print Screen
    CHECK(!ch.changed);
    ch = c.OnMouseWheel(0, 0, 1170);  // a wheel event with no scroll
    CHECK(!ch.changed);
    CHECK(c.Get().device == InputDevice::Pad && c.Get().generation == gen);
    ch = c.OnKeyDown(0x7C, false, 1180);  // F13 counts
    CHECK(ch.changed && c.Get().device == InputDevice::KeyboardMouse);
}

void TestMouseJitter() {
    ActiveInputCore c;
    OneXboxPad(c);
    c.OnPadFrame(Buttons(kA), kUnknown, 1000);  // a controller in use; a long time passes
    const uint32_t gen = c.Get().generation;
    // Jitter, 25 ms apart: never a switch (the P0e plan's numbers).
    const float dx[] = {1, -1, 2, -2, 1, -1};
    int64_t t = 9000;
    for (int i = 0; i < 6; ++i) {
        const Change ch = c.OnMouseMove(dx[i], 0, t);
        CHECK(!ch.changed && ch.looked == Cause::MouseMove);
        t += 25;
    }
    // One 4 px step.
    CHECK(!c.OnMouseMove(4, 0, 10000).changed);
    // A slow drift: 20 px in 2 s.
    for (int i = 0; i < 20; ++i) CHECK(!c.OnMouseMove(1, 0, 11000 + 100 * i).changed);
    // One 200 px jump in a single event.
    CHECK(!c.OnMouseMove(-200, 0, 14000).changed);
    // A move with no motion.
    CHECK(!c.OnMouseMove(0, 0, 14100).changed);
    CHECK(c.Get().device == InputDevice::Pad && c.Get().generation == gen);
    // Two 6 px moves 60 ms apart: a deliberate nudge. 8 seconds after the pad's last input.
    CHECK(!c.OnMouseMove(6, 0, 20000).changed);
    const Change ch = c.OnMouseMove(6, 0, 20060);
    CHECK(ch.changed && ch.cause == Cause::MouseMove);
    CHECK(c.Get().device == InputDevice::KeyboardMouse);
    // With the keyboard and mouse already in use motion changes nothing.
    const Change again = c.OnMouseMove(30, 0, 20100);
    CHECK(!again.changed && again.looked == Cause::MouseMove);
}

void TestPadRecentBlocksMotion() {
    ActiveInputCore c;
    OneXboxPad(c);
    c.OnPadFrame(Buttons(kA), kUnknown, 5000);
    // A deliberate move within 1 s of the pad's input does not take over...
    CHECK(!c.OnMouseMove(6, 0, 5100).changed);
    CHECK(!c.OnMouseMove(6, 0, 5150).changed);
    CHECK(c.Get().device == InputDevice::Pad);
    // ...and neither does one that keeps going until the second is nearly up (the controller's time restarts
    // with every pad input, not with the mouse).
    CHECK(!c.OnMouseMove(6, 0, 5900).changed);
    CHECK(!c.OnMouseMove(6, 0, 5950).changed);
    CHECK(c.Get().device == InputDevice::Pad);
    // The same motion going on past the second takes over (the window of 200 ms still holds the 5900 and 5950
    // events, so this one is the third and counts): 1100 ms after the pad's input.
    CHECK(c.OnMouseMove(6, 0, 6100).changed);
    CHECK(c.Get().device == InputDevice::KeyboardMouse);
    // A held button is not new pad input: the controller's time does not move on a held button.
    c.OnPadFrame(Buttons(kB), kUnknown, 7000);  // pressed: back to the pad
    CHECK(c.Get().device == InputDevice::Pad);
    c.OnPadFrame(Buttons(kB), kUnknown, 7500);  // held
    c.OnPadFrame(Buttons(kB), kUnknown, 8000);  // held
    CHECK(!c.OnMouseMove(6, 0, 8100).changed);
    CHECK(c.OnMouseMove(6, 0, 8150).changed);   // 1150 ms after the press: the held button did not count
}

void TestClicksAndWheelAreImmediate() {
    ActiveInputCore c;
    OneXboxPad(c);
    c.OnPadFrame(Buttons(kA), kUnknown, 5000);
    CHECK(c.OnMouseButtonDown(5050).changed);  // 50 ms after a controller input: a click takes over
    CHECK(c.Get().device == InputDevice::KeyboardMouse);
    c.OnPadFrame(Buttons(kB), kUnknown, 6000);
    CHECK(c.Get().device == InputDevice::Pad);
    CHECK(c.OnMouseWheel(0, -120, 6020).changed);  // so does a wheel notch
    CHECK(c.Get().device == InputDevice::KeyboardMouse);
    c.OnPadFrame(Buttons(kA), kUnknown, 7000);
    CHECK(c.OnKeyDown(0x41, false, 7010).changed);  // and a key
    CHECK(c.Get().device == InputDevice::KeyboardMouse);
}

void TestAntiFlap() {
    // A stick that is already past its dead zone and moves again is "soft": it waits 250 ms after a switch.
    ActiveInputCore c;
    OneXboxPad(c);
    c.OnPadFrame(Left(12000), kUnknown, 1000);  // crossing: the stick is held from here on
    c.OnKeyDown(0x41, false, 2000);
    CHECK(c.Get().device == InputDevice::KeyboardMouse);
    Change ch = c.OnPadFrame(Left(15000), kUnknown, 2100);  // moved by 3000: soft, 100 ms after the switch
    CHECK(!ch.changed && ch.looked == Cause::PadStickMove);
    CHECK(c.Get().device == InputDevice::KeyboardMouse);
    ch = c.OnPadFrame(Left(18000), kUnknown, 2300);  // 300 ms after the switch: allowed
    CHECK(ch.changed && ch.cause == Cause::PadStickMove);
    CHECK(c.Get().device == InputDevice::Pad);
    // A stick crossing the dead zone from rest, a button and a trigger are not subject to it.
    c.OnKeyDown(0x42, false, 2400);
    CHECK(c.Get().device == InputDevice::KeyboardMouse);
    c.OnPadFrame(Left(0), kUnknown, 2450);  // released: armed again
    ch = c.OnPadFrame(Left(12000), kUnknown, 2500);  // 100 ms after the switch to the keys
    CHECK(ch.changed && ch.cause == Cause::PadStick && c.Get().device == InputDevice::Pad);
}

void TestFocus() {
    ActiveInputCore c;
    OneXboxPad(c);  // focused at attach time, no settling
    c.OnPadFrame(Buttons(kA), kUnknown, 100);
    // lost focus: nothing counts
    c.OnFocus(false, 1000);
    CHECK(!c.OnKeyDown(0x41, false, 1100).changed);
    CHECK(!c.OnMouseButtonDown(1110).changed);
    CHECK(!c.OnMouseWheel(0, 120, 1120).changed);
    CHECK(!c.OnMouseMove(30, 0, 1130).changed);
    CHECK(c.Get().device == InputDevice::Pad);
    // focus is back at 7000: the mouse is not trusted for 300 ms (the click that gave the window focus moves it)
    c.OnFocus(true, 7000);
    CHECK(!c.OnMouseMove(6, 0, 7100).changed);
    CHECK(!c.OnMouseMove(6, 0, 7150).changed);
    CHECK(!c.OnMouseMove(6, 0, 7290).changed);
    CHECK(c.Get().device == InputDevice::Pad);
    // after it: a deliberate move counts (the earlier events were thrown away, so it takes two new ones)
    CHECK(!c.OnMouseMove(6, 0, 7400).changed);
    CHECK(c.OnMouseMove(6, 0, 7450).changed);
    CHECK(c.Get().device == InputDevice::KeyboardMouse);
    // a key is not delayed by it
    c.OnPadFrame(Buttons(kB), kUnknown, 9000);
    c.OnFocus(false, 9100);
    c.OnFocus(true, 9200);
    CHECK(c.OnKeyDown(0x41, false, 9210).changed);
}

void TestPadGoneForgetsTheSample() {
    ActiveInputCore c;
    OneXboxPad(c);
    CHECK(c.OnPadFrame(Buttons(kA), kUnknown, 100).looked == Cause::PadButton);
    CHECK(c.OnPadFrame(Buttons(kA), kUnknown, 116).looked == Cause::None);  // held
    c.OnPadGone();
    CHECK(c.OnPadFrame(Buttons(kA), kUnknown, 132).looked == Cause::PadButton);  // a new pad that already holds a button
}

void TestFamilyFollowsThePadInUse() {
    ActiveInputCore c;
    OneXboxPad(c);
    int asked = 0;
    auto counting = [&asked] {
        ++asked;
        return 1;
    };
    // Frames with no activity never ask for the family (the SDK is not queried every frame).
    for (int i = 0; i < 50; ++i) c.OnPadFrame(PadSample{}, counting, 16 * i);
    CHECK(asked == 0);
    Change ch = c.OnPadFrame(Buttons(kA), counting, 1000);
    CHECK(asked == 1);
    CHECK(ch.changed && ch.before.device == InputDevice::Pad && ch.after.device == InputDevice::Pad);  // same device, other glyphs
    CHECK(c.Get().Family() == GlyphFamily::PlayStation && c.Get().PromptStrip() == GlyphFamily::PlayStation);
    ch = c.OnPadFrame(Buttons(0), counting, 1016);
    CHECK(asked == 1);
    ch = c.OnPadFrame(Buttons(kB), counting, 1100);  // same family again: counted, nothing changes
    CHECK(asked == 2 && !ch.changed);
    // An unknown family (the test harness's pad, a wheel) leaves the strip as it was.
    ch = c.OnPadFrame(Buttons(0), kUnknown, 1200);
    ch = c.OnPadFrame(Buttons(kA), kUnknown, 1300);
    CHECK(!ch.changed && c.Get().Family() == GlyphFamily::PlayStation);
    // Used while the keyboard has it: the pad's family comes with the switch.
    c.OnKeyDown(0x41, false, 2000);
    CHECK(c.Get().Family() == GlyphFamily::Keyboard && c.Get().PromptStrip() == GlyphFamily::PlayStation);
    c.OnPadFrame(Buttons(0), kXboxPad, 2100);
    ch = c.OnPadFrame(Buttons(kA), kXboxPad, 2200);
    CHECK(ch.changed && c.Get().device == InputDevice::Pad && c.Get().Family() == GlyphFamily::Xbox);
}

// ---- the setting and the generation ------------------------------------------------------------------------

void TestPromptSetting() {
    CHECK(ParsePromptOverride("auto") == PromptOverride::Auto);
    CHECK(ParsePromptOverride("xbox") == PromptOverride::Xbox);
    CHECK(ParsePromptOverride("playstation") == PromptOverride::PlayStation);
    CHECK(ParsePromptOverride("keyboard") == PromptOverride::Keyboard);
    CHECK(ParsePromptOverride("") == PromptOverride::Auto);
    CHECK(ParsePromptOverride("Xbox 360") == PromptOverride::Auto);

    ActiveInputCore c;
    const GlyphFamily ps[] = {GlyphFamily::PlayStation};
    c.OnFocus(true, 0, true);
    c.OnDevices(ps, 1, 0);  // Pad, PlayStation
    uint32_t gen = c.Get().generation;
    CHECK(c.Get().Family() == GlyphFamily::PlayStation);

    Change ch = c.SetOverride(PromptOverride::Keyboard);
    CHECK(ch.changed && ch.cause == Cause::PromptSetting && c.Get().generation == ++gen);
    CHECK(c.Get().device == InputDevice::Pad);  // the setting is a glyph choice, not a device
    CHECK(c.Get().Family() == GlyphFamily::Keyboard);
    CHECK(c.Get().PromptStrip() == GlyphFamily::Xbox);  // the key caps go over the Xbox strip, as keyboard_native does today
    CHECK(!c.SetOverride(PromptOverride::Keyboard).changed && c.Get().generation == gen);

    ch = c.SetOverride(PromptOverride::Xbox);
    CHECK(ch.changed && c.Get().Family() == GlyphFamily::Xbox && c.Get().PromptStrip() == GlyphFamily::Xbox);
    ch = c.SetOverride(PromptOverride::PlayStation);
    CHECK(ch.changed && c.Get().Family() == GlyphFamily::PlayStation && c.Get().PromptStrip() == GlyphFamily::PlayStation);
    // A forced set stays whatever device is used, and the device still follows the input.
    c.OnKeyDown(0x41, false, 1000);
    CHECK(c.Get().device == InputDevice::KeyboardMouse && c.Get().Family() == GlyphFamily::PlayStation);
    // Back to auto: the family follows the device.
    c.SetOverride(PromptOverride::Auto);
    CHECK(c.Get().Family() == GlyphFamily::Keyboard && c.Get().PromptStrip() == GlyphFamily::PlayStation);
    c.OnPadFrame(Buttons(kA), kUnknown, 2000);
    CHECK(c.Get().Family() == GlyphFamily::PlayStation);
}

void TestGeneration() {
    ActiveInputCore c;
    OneXboxPad(c);
    CHECK(c.Get().generation == 0);
    // Alternating the setting is a change each time: the generation counts them all.
    for (uint32_t i = 1; i <= 1000; ++i) {
        c.SetOverride((i & 1) ? PromptOverride::Keyboard : PromptOverride::Auto);
        CHECK(c.Get().generation == i);
    }
    // No change, no move.
    const uint32_t before = c.Get().generation;
    c.SetOverride(PromptOverride::Auto);
    c.OnPadFrame(PadSample{}, kUnknown, 5);
    c.OnKeyDown(0x10, false, 6);
    CHECK(c.Get().generation == before);

    // The word the readers load holds all four fields, and 24 bits of generation.
    Snapshot s;
    s.device = InputDevice::KeyboardMouse;
    s.strip = GlyphFamily::PlayStation;
    s.setting = PromptOverride::Keyboard;
    s.generation = 0xFFFFFF;
    const Snapshot back = Unpack(Pack(s));
    CHECK(back.device == s.device && back.strip == s.strip && back.setting == s.setting && back.generation == 0xFFFFFFu);
    Snapshot zero;
    CHECK(Unpack(Pack(zero)).device == InputDevice::Pad && Unpack(Pack(zero)).generation == 0);
    // It wraps: one change after 0xFFFFFF reads 0 (the readers only compare for "changed").
    CHECK(((0xFFFFFFu + 1u) & 0xFFFFFFu) == 0u);
}

// ---- the pause for a lost controller (PadLossRule) --------------------------------------------------------

constexpr uint64_t kPadA = 11;
constexpr uint64_t kPadB = 12;
constexpr uint64_t kPadC = 13;

// Runs the polls of a window until the real answer is back: how many said "not connected", and whether one flagged itself the last.
int RunWindow(PadLossRule& r, int64_t t0, int64_t step_ms, bool* last_flagged = nullptr) {
    int told = 0;
    for (int i = 0; i < 40; ++i) {
        const LossAnswer a = r.OnStatePoll(t0 + int64_t(i) * step_ms);
        if (a == LossAnswer::Connected) break;
        ++told;
        if (a == LossAnswer::NotConnectedLast && last_flagged) *last_flagged = true;
    }
    return told;
}

void TestPadLossOpensAWindow() {
    PadLossRule r;
    CHECK(r.OnStatePoll(0) == LossAnswer::Connected);  // nothing open at the start: the real answer stands
    CHECK(!r.CapsHidden());
    r.NoteActivePad(kPadA);
    CHECK(r.ActivePad() == kPadA);
    // The pad was in the player's hands, the game is driven: the list no longer holds it.
    CHECK(r.OnList(nullptr, 0, InputDevice::Pad, /*driving=*/true, /*wheel=*/false, 1000) == LossVerdict::Open);
    CHECK(r.ActivePad() == 0);  // gone for good
    CHECK(r.CapsHidden());      // the capabilities call is told as well, and does not count
    CHECK(r.CapsHidden());
    bool last = false;
    CHECK(RunWindow(r, 1005, 5, &last) == kPadLossPolls);  // a few polls, then the stand-in answers again
    CHECK(last);
    CHECK(r.Answered() == kPadLossPolls);
    CHECK(!r.CapsHidden());
    CHECK(r.OnStatePoll(1100) == LossAnswer::Connected);
    // The last poll of the window is the one that says so.
    PadLossRule q;
    q.NoteActivePad(kPadA);
    q.OnList(nullptr, 0, InputDevice::Pad, true, false, 0);
    for (int i = 0; i < kPadLossPolls - 1; ++i) CHECK(q.OnStatePoll(i) == LossAnswer::NotConnected);
    CHECK(q.OnStatePoll(kPadLossPolls) == LossAnswer::NotConnectedLast);
    CHECK(q.OnStatePoll(kPadLossPolls + 1) == LossAnswer::Connected);
}

void TestPadLossOnlyForTheActivePad() {
    // Nothing remembered (boot, the title before any input, a harness pad, a wheel): a list that shrinks is nothing.
    PadLossRule r;
    CHECK(r.OnList(nullptr, 0, InputDevice::Pad, true, false, 0) == LossVerdict::None);
    CHECK(!r.CapsHidden() && r.OnStatePoll(1) == LossAnswer::Connected);
    r.NoteActivePad(0);  // "not known" changes nothing
    CHECK(r.ActivePad() == 0);
    CHECK(r.OnList(nullptr, 0, InputDevice::Pad, true, false, 2) == LossVerdict::None);

    // Two pads, A in the player's hands: B going away is not the pad in use.
    PadLossRule two;
    two.NoteActivePad(kPadA);
    const uint64_t only_a[] = {kPadA};
    CHECK(two.OnList(only_a, 1, InputDevice::Pad, true, false, 10) == LossVerdict::None);
    CHECK(!two.CapsHidden());
    CHECK(two.ActivePad() == kPadA);  // still remembered
    // The player picks up B; now A leaves: nothing; then B leaves: the window opens.
    two.NoteActivePad(kPadB);
    const uint64_t only_b[] = {kPadB};
    CHECK(two.OnList(only_b, 1, InputDevice::Pad, true, false, 20) == LossVerdict::None);
    CHECK(two.OnList(nullptr, 0, InputDevice::Pad, true, false, 30) == LossVerdict::Open);
    // A list change that finds nothing remembered (a device came or went for another reason) never reopens it.
    CHECK(RunWindow(two, 31, 5) == kPadLossPolls);
    CHECK(two.OnList(nullptr, 0, InputDevice::Pad, true, false, 500) == LossVerdict::None);
    CHECK(!two.CapsHidden());
}

void TestPadLossAnotherPadRemainsStillPauses() {
    // The pad in the player's hands left, another one is plugged in and idle: the game still pauses (the player was holding the first
    // one); the other pad works as soon as the pause is dealt with.
    PadLossRule r;
    r.NoteActivePad(kPadA);
    const uint64_t only_b[] = {kPadB};
    CHECK(r.OnList(only_b, 1, InputDevice::Pad, true, false, 0) == LossVerdict::Open);
}

void TestPadLossNotOnTheKeyboard() {
    PadLossRule r;
    r.NoteActivePad(kPadA);
    // The keyboard and mouse took over after the pad was last used: its removal is not the player's problem.
    CHECK(r.OnList(nullptr, 0, InputDevice::KeyboardMouse, true, false, 0) == LossVerdict::KeyboardActive);
    CHECK(!r.CapsHidden() && r.OnStatePoll(1) == LossAnswer::Connected);
    CHECK(r.ActivePad() == 0);
    // The pad comes back as a new one and is used again: the rule is armed again.
    r.NoteActivePad(kPadC);
    CHECK(r.OnList(nullptr, 0, InputDevice::Pad, true, false, 10) == LossVerdict::Open);
}

void TestPadLossOnlyWhileDriving() {
    // The title, a menu, the garage, a load, the pause menu itself: the game is not driven, nothing to pause.
    PadLossRule r;
    r.NoteActivePad(kPadA);
    CHECK(r.OnList(nullptr, 0, InputDevice::Pad, /*driving=*/false, false, 0) == LossVerdict::NotDriving);
    CHECK(!r.CapsHidden() && r.OnStatePoll(1) == LossAnswer::Connected);
    CHECK(r.ActivePad() == 0);  // not asked again at the next list change
    // A wheel in the player's hands: no pause for a pad on the side.
    PadLossRule w;
    w.NoteActivePad(kPadA);
    CHECK(w.OnList(nullptr, 0, InputDevice::Pad, true, /*wheel=*/true, 0) == LossVerdict::Wheel);
    CHECK(!w.CapsHidden());
    // The keyboard is named first when both apply (the order of the reasons in the log).
    PadLossRule k;
    k.NoteActivePad(kPadA);
    CHECK(k.OnList(nullptr, 0, InputDevice::KeyboardMouse, false, true, 0) == LossVerdict::KeyboardActive);
}

void TestPadLossWindowIsBounded() {
    // In game polls: all of them, and no more.
    PadLossRule r;
    r.NoteActivePad(kPadA);
    r.OnList(nullptr, 0, InputDevice::Pad, true, false, 0);
    CHECK(RunWindow(r, 0, 1) == kPadLossPolls);
    // In time: a window older than kPadLossMaxMs ends at its next poll, whatever is left of it.
    PadLossRule t;
    t.NoteActivePad(kPadA);
    t.OnList(nullptr, 0, InputDevice::Pad, true, false, 5000);
    CHECK(t.OnStatePoll(5001) == LossAnswer::NotConnected);
    CHECK(t.OnStatePoll(5000 + kPadLossMaxMs) == LossAnswer::NotConnected);   // right at the limit: still in
    CHECK(t.OnStatePoll(5000 + kPadLossMaxMs + 1) == LossAnswer::Connected);  // past it: over
    CHECK(!t.CapsHidden());
    CHECK(t.OnStatePoll(5000 + kPadLossMaxMs + 2) == LossAnswer::Connected);
    // A game that stopped polling for a long time and then polls gets the real answer at once.
    PadLossRule s;
    s.NoteActivePad(kPadA);
    s.OnList(nullptr, 0, InputDevice::Pad, true, false, 0);
    CHECK(s.OnStatePoll(60'000) == LossAnswer::Connected);
    CHECK(!s.CapsHidden());
}

void TestPadLossTestHook() {
    // OnPadLost is the decision without a list (the debug cvar input_test_pad_loss): no pad needs to be remembered.
    PadLossRule r;
    CHECK(r.OnPadLost(InputDevice::KeyboardMouse, true, false, 0) == LossVerdict::KeyboardActive);
    CHECK(r.OnPadLost(InputDevice::Pad, false, false, 0) == LossVerdict::NotDriving);
    CHECK(r.OnPadLost(InputDevice::Pad, true, true, 0) == LossVerdict::Wheel);
    CHECK(!r.CapsHidden());
    CHECK(r.OnPadLost(InputDevice::Pad, true, false, 100) == LossVerdict::Open);
    CHECK(r.CapsHidden());
    CHECK(RunWindow(r, 101, 5) == kPadLossPolls);
    // Called again while a window is open, it starts a new one.
    CHECK(r.OnPadLost(InputDevice::Pad, true, false, 200) == LossVerdict::Open);
    CHECK(r.OnStatePoll(201) == LossAnswer::NotConnected);
    CHECK(r.OnPadLost(InputDevice::Pad, true, false, 202) == LossVerdict::Open);
    CHECK(RunWindow(r, 203, 5) == kPadLossPolls && r.Answered() == kPadLossPolls);
}

// The rule next to the state machine, the way input_state.cpp drives them: the verdict is asked BEFORE the list is applied to the core
// (which moves the device off a removed pad), with the device as it was.
void TestPadLossWithTheStateMachine() {
    ActiveInputCore c;
    PadLossRule r;
    const GlyphFamily pads[] = {GlyphFamily::Xbox};
    const uint64_t ids[] = {kPadA};
    r.OnList(ids, 1, c.Get().device, true, false, 0);
    c.OnDevices(pads, 1, 0);
    c.OnFocus(true, 0, /*initial=*/true);
    CHECK(c.Get().device == InputDevice::Pad);  // before any input the device follows the pad list...
    CHECK(r.ActivePad() == 0);                  // ...but nothing was used yet: no pad is "in the player's hands"

    // The pad is used: the state machine counts it (the SDK says which pad), the rule remembers it.
    uint64_t seen = 0;
    c.OnPadFrame(Buttons(kA), [&] { seen = kPadA; return 0; }, 100);
    r.NoteActivePad(seen);
    CHECK(r.ActivePad() == kPadA);

    // The pad is removed while the player drives: the rule is asked first, then the state machine moves to the keyboard.
    const InputDevice before = c.Get().device;
    CHECK(r.OnList(nullptr, 0, before, true, false, 5000) == LossVerdict::Open);
    const Change ch = c.OnDevices(nullptr, 0, 5000);
    CHECK(ch.changed && ch.cause == Cause::PadRemoved && c.Get().device == InputDevice::KeyboardMouse);
    // The game pauses; the keyboard works; nothing asks for a second pause.
    CHECK(RunWindow(r, 5001, 5) == kPadLossPolls);
    CHECK(r.OnList(nullptr, 0, c.Get().device, true, false, 6000) == LossVerdict::None);
    // The pad returns (a new handle) and is used: the device goes back to the pad, and its removal pauses again.
    const uint64_t new_ids[] = {kPadC};
    r.OnList(new_ids, 1, c.Get().device, true, false, 7000);
    c.OnDevices(pads, 1, 7000);
    CHECK(c.Get().device == InputDevice::KeyboardMouse);  // a pad that arrives changes nothing until it is used
    c.OnPadFrame(Buttons(kB), [&] { seen = kPadC; return 0; }, 8000);
    r.NoteActivePad(seen);
    CHECK(c.Get().device == InputDevice::Pad);
    CHECK(r.OnList(nullptr, 0, c.Get().device, true, false, 9000) == LossVerdict::Open);

    // The keyboard first, then the pad dies: no pause.
    PadLossRule k;
    ActiveInputCore kc;
    OneXboxPad(kc);
    kc.OnPadFrame(Buttons(kA), kXboxPad, 100);
    k.NoteActivePad(kPadA);
    kc.OnKeyDown(0x41, false, 3000);
    CHECK(kc.Get().device == InputDevice::KeyboardMouse);
    CHECK(k.OnList(nullptr, 0, kc.Get().device, true, false, 4000) == LossVerdict::KeyboardActive);
}

// ---- the ImGui overlays (KBM P7): the pad an overlay reads, the close word, the controller toast ----------------

void TestOverlayAxis() {
    CHECK(OverlayAxis(0.0f) == 0);
    CHECK(OverlayAxis(1.0f) == 32767);
    CHECK(OverlayAxis(-1.0f) == -32767);
    CHECK(OverlayAxis(2.5f) == 32767 && OverlayAxis(-9.0f) == -32767);  // past either end is clamped, not wrapped
    CHECK(OverlayAxis(0.5f) == 16384);
    CHECK(OverlayAxis(std::numeric_limits<float>::quiet_NaN()) == 0);  // a bad reading is no stick movement
    // The stick rule of the game's own state, on an overlay's -1..1: 0.25 (the overlays' own dead zone) is past 7849, 0.2 is not.
    PadFilter f;
    CHECK(f.Feed(OverlayPadSample(0, 0.2f, 0.0f, 0.0f, 0.0f)) == PadCause::None);
    CHECK(f.Feed(OverlayPadSample(0, 0.25f, 0.0f, 0.0f, 0.0f)) == PadCause::Stick);
    PadFilter g;
    CHECK(g.Feed(OverlayPadSample(0, 0.0f, 0.0f, 0.0f, -0.3f)) == PadCause::Stick);  // the right stick, down
    CHECK(OverlayPadSample(0x2000, 0, 0, 0, 0).buttons == 0x2000);
}

void TestOverlayPadMakesThePadActive() {
    ActiveInputCore c;
    OneXboxPad(c);
    c.OnKeyDown(0x41, false, 1000);  // the keyboard has it: the player opened a dialog with it
    CHECK(c.Get().device == InputDevice::KeyboardMouse && c.Get().Family() == GlyphFamily::Keyboard);
    // The game's own polls see a pad at rest while the overlay is open (the SDK answers neutral): nothing there.
    CHECK(!c.OnPadFrame(PadSample{}, kUnknown, 1100).changed);
    // The player picks up the pad inside the overlay: A goes down.
    Change ch = c.OnOverlayPadFrame(Buttons(kA), 1200);
    CHECK(ch.changed && ch.looked == Cause::PadButton && ch.after.device == InputDevice::Pad);
    CHECK(c.Get().device == InputDevice::Pad && c.Get().Family() == GlyphFamily::Xbox);
    // Held, with the game's neutral polls in between: the overlay's filter is its own, no second "activity".
    c.OnPadFrame(PadSample{}, kUnknown, 1210);
    ch = c.OnOverlayPadFrame(Buttons(kA), 1216);
    CHECK(!ch.changed && ch.looked == Cause::None);
    // Released and another button pressed: looked at and counted, the device is the pad already.
    c.OnOverlayPadFrame(Buttons(0), 1300);
    ch = c.OnOverlayPadFrame(Buttons(kB), 1316);
    CHECK(!ch.changed && ch.looked == Cause::PadButton);
}

void TestOverlayPadKeepsTheFamily() {
    // A PlayStation pad is listed; the keyboard was used; the pad picked up inside an overlay: the pad's family is back (the strip
    // was kept while the keyboard had the device); the overlay never says which pad it was.
    ActiveInputCore c;
    const GlyphFamily pads[] = {GlyphFamily::PlayStation};
    c.OnDevices(pads, 1, 0);
    c.OnFocus(true, 0, /*initial=*/true);
    c.OnKeyDown(0x41, false, 1000);
    CHECK(c.Get().Family() == GlyphFamily::Keyboard && c.Get().PromptStrip() == GlyphFamily::PlayStation);
    c.OnOverlayPadFrame(Buttons(kA), 2000);
    CHECK(c.Get().device == InputDevice::Pad && c.Get().Family() == GlyphFamily::PlayStation);
    // The keyboard again, then a stick inside the overlay.
    c.OnKeyDown(0x42, false, 3000);
    CHECK(c.Get().Family() == GlyphFamily::Keyboard);
    c.OnOverlayPadFrame(Left(20000), 4000);
    CHECK(c.Get().device == InputDevice::Pad && c.Get().Family() == GlyphFamily::PlayStation);
}

void TestOverlayPadGoneForgetsTheSample() {
    ActiveInputCore c;
    OneXboxPad(c);
    c.OnOverlayPadFrame(Buttons(kA), 1000);
    CHECK(c.OnOverlayPadFrame(Buttons(kA), 1016).looked == Cause::None);  // held: nothing
    c.OnOverlayPadGone();                                                 // the dialog closed: its next reading starts from a pad at rest
    CHECK(c.OnOverlayPadFrame(Buttons(kA), 5000).looked == Cause::PadButton);
}

void TestOverlayPadHoldsOffTheMouse() {
    // A pad used in an overlay counts as a recent controller: a trackpad under a thumb does not take the device within a second.
    ActiveInputCore c;
    OneXboxPad(c);
    c.OnKeyDown(0x41, false, 1000);
    c.OnOverlayPadFrame(Buttons(kA), 5000);
    CHECK(c.Get().device == InputDevice::Pad);
    c.OnMouseMove(30.0f, 0.0f, 5300);  // past the 250 ms anti-flap time, still within a second of the pad
    c.OnMouseMove(30.0f, 0.0f, 5350);
    CHECK(c.Get().device == InputDevice::Pad);
    c.OnMouseMove(30.0f, 0.0f, 6100);  // a second after the pad: the mouse takes it
    c.OnMouseMove(30.0f, 0.0f, 6150);
    CHECK(c.Get().device == InputDevice::KeyboardMouse);
}

void TestCloseKeyWord() {
    using mcla::ui::CloseKeyWord;
    CHECK(std::string(CloseKeyWord(GlyphFamily::Xbox)) == "B");
    CHECK(std::string(CloseKeyWord(GlyphFamily::PlayStation)) == "CIRCLE");
    CHECK(std::string(CloseKeyWord(GlyphFamily::Keyboard)) == "ESC");
    // What a dialog shows follows the state's glyph family, and with it the last device and the button_prompts setting.
    ActiveInputCore c;
    const GlyphFamily ps[] = {GlyphFamily::PlayStation};
    c.OnDevices(ps, 1, 0);
    c.OnFocus(true, 0, /*initial=*/true);
    CHECK(std::string(CloseKeyWord(c.Get().Family())) == "CIRCLE");  // before any input: the pad that is plugged in
    c.OnKeyDown(0x1B, false, 1000);
    CHECK(std::string(CloseKeyWord(c.Get().Family())) == "ESC");     // the keyboard took over, a pad is still plugged in
    c.OnOverlayPadFrame(Buttons(kB), 2000);
    CHECK(std::string(CloseKeyWord(c.Get().Family())) == "CIRCLE");  // and the pad took it back inside the dialog
    c.SetOverride(PromptOverride::Xbox);
    CHECK(std::string(CloseKeyWord(c.Get().Family())) == "B");       // the setting forces the family
    c.SetOverride(PromptOverride::Keyboard);
    CHECK(std::string(CloseKeyWord(c.Get().Family())) == "ESC");
    // No pad at all: the keyboard's word.
    ActiveInputCore none;
    none.OnDevices(nullptr, 0, 0);
    CHECK(std::string(CloseKeyWord(none.Get().Family())) == "ESC");
}

void TestControllerRoster() {
    using mcla::ui::ControllerEvent;
    using mcla::ui::ControllerRoster;
    ControllerRoster r;
    CHECK(r.Update({}).empty() && r.Count() == 0);
    // The first list: every controller there toasts as connected, in the SDK's order.
    std::vector<ControllerEvent> ev = r.Update({{11, "Xbox Controller"}, {12, "PS5 Controller"}});
    CHECK(ev.size() == 2 && ev[0].connected && ev[0].name == "Xbox Controller" && ev[1].connected && ev[1].name == "PS5 Controller");
    CHECK(r.Count() == 2);
    // The same list again (the SDK's counter moved for another reason): nothing.
    CHECK(r.Update({{11, "Xbox Controller"}, {12, "PS5 Controller"}}).empty());
    // One leaves: a disconnect, named as it was.
    ev = r.Update({{12, "PS5 Controller"}});
    CHECK(ev.size() == 1 && !ev[0].connected && ev[0].name == "Xbox Controller" && r.Count() == 1);
    // It comes back: a new handle, a new connect.
    ev = r.Update({{12, "PS5 Controller"}, {13, "Xbox Controller"}});
    CHECK(ev.size() == 1 && ev[0].connected && ev[0].name == "Xbox Controller");
    // A swap in one look: the arrival first, then the departures in the order they had come.
    ev = r.Update({{14, "Switch Pro Controller"}});
    CHECK(ev.size() == 3 && ev[0].connected && ev[0].name == "Switch Pro Controller");
    CHECK(!ev[1].connected && ev[1].name == "PS5 Controller" && !ev[2].connected && ev[2].name == "Xbox Controller");
    // All gone.
    ev = r.Update({});
    CHECK(ev.size() == 1 && !ev[0].connected && r.Count() == 0);
    // A driver that gave no name.
    ev = r.Update({{15, ""}});
    CHECK(ev.size() == 1 && ev[0].connected && ev[0].name == "Controller");
    ev = r.Update({});
    CHECK(ev.size() == 1 && !ev[0].connected && ev[0].name == "Controller");
}

}  // namespace

int main() {
    TestKeys();
    TestPadFilterButtons();
    TestPadFilterTriggers();
    TestPadFilterSticks();
    TestMotionFilter();
    TestStart();
    TestHotPlug();
    TestLastInputWins();
    TestHeldStickThenKey();
    TestNoisyKeys();
    TestMouseJitter();
    TestPadRecentBlocksMotion();
    TestClicksAndWheelAreImmediate();
    TestAntiFlap();
    TestFocus();
    TestPadGoneForgetsTheSample();
    TestFamilyFollowsThePadInUse();
    TestPromptSetting();
    TestGeneration();
    TestPadLossOpensAWindow();
    TestPadLossOnlyForTheActivePad();
    TestPadLossAnotherPadRemainsStillPauses();
    TestPadLossNotOnTheKeyboard();
    TestPadLossOnlyWhileDriving();
    TestPadLossWindowIsBounded();
    TestPadLossTestHook();
    TestPadLossWithTheStateMachine();
    TestOverlayAxis();
    TestOverlayPadMakesThePadActive();
    TestOverlayPadKeepsTheFamily();
    TestOverlayPadGoneForgetsTheSample();
    TestOverlayPadHoldsOffTheMouse();
    TestCloseKeyWord();
    TestControllerRoster();
    std::printf("input_state_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
