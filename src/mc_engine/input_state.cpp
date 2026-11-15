// The glue of the active-device state machine (input_state.h, input_state_core.h): feeds it from the window's
// events (keys, mouse, focus: a layer-0 listener, the pattern of cursor_hide.cpp and map_mouse.cpp), from the
// pad state the game reads (an object on the stack of the XamInputGetState wrapper, wheel/wheel_fake.cpp) and
// from the SDK's list of devices (rex::input::InputSystem: device_generation() says it changed, Devices() lists
// it, ActiveDevice(0) names the pad that made the last real input). Nothing here polls the OS: a pad that comes
// or goes is a change of the SDK's counter (SDL's own notifications), the Sony guess for the very first frames
// is the device watcher's (device_watch.h, one CM_Register_Notification), and its change listener only marks the
// list for a fresh look at the next poll.
//
// SDK needed: LAR-RXSDK branch feat/input-device-info (PadKind, DeviceInfo vendor / product / pad_kind,
// InputSystem::device_generation(), Devices(), ActiveDevice()). Building against an SDK without it: define
// LARECOMP_NO_SDK_DEVICE_INFO; the list and the pad family are then not read (the device starts as Pad, the strip
// follows the Sony guess until something is used, and pads coming and going are not followed). CHECK: that
// fallback has never been compiled.
//
// Log lines (info level), read by perf\scripts\kbm\kbm_p2_report.py:
//   [input] state machine ready: ...                      once, when the window listener is attached
//   [input] devices: N controller(s) [...]; S synthetic; ... when the SDK's list (or the watcher's Sony flag) changed (S > 0: mnk_mode)
//   [input] switch device D (was E) glyphs G because C; generation N   the device changed
//   [input] glyphs device D glyphs G (was H) because C; generation N    only the glyph family / strip / setting changed
//   [input] ev ...                                        with input_log: every event the rules looked at and what they decided

#include "input_state.h"
#include "input_state_core.h"

#ifndef REXGLUE_HAS_XEO3_TARGET

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/input/device.h>
#include <rex/input/input_system.h>
#include <rex/runtime.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include "device_watch.h"
#include "logging.h"
#include "ui/pad_vendor.h"

REXCVAR_DEFINE_BOOL(input_log, false, "MCLA/Input",
    "Log every key, mouse and controller event that the active-device rules looked at and what they "
    "decided (counted, or why it was ignored), at most 4000 lines. The switches themselves are always "
    "logged.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DECLARE(std::string, button_prompts);  // hooks/ui.cpp

namespace larecomp::input {
namespace {

using Clock = std::chrono::steady_clock;

// The one state machine; never destroyed (a pool thread may still be inside a callback at exit).
ActiveInputCore& Core() {
  static ActiveInputCore* core = new ActiveInputCore();
  return *core;
}

int64_t NowMs() {
  static const Clock::time_point start = Clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

const char* DeviceName(InputDevice d) { return d == InputDevice::Pad ? "Pad" : "KeyboardMouse"; }

const char* FamilyName(GlyphFamily f) {
  switch (f) {
    case GlyphFamily::Xbox: return "Xbox";
    case GlyphFamily::PlayStation: return "PlayStation";
    case GlyphFamily::Keyboard: return "Keyboard";
  }
  return "?";
}

// ---- the listeners of the state ----------------------------------------------------------------------

struct ListenerTable {
  std::mutex mutex;
  struct Slot {
    InputListener fn = nullptr;
    void* arg = nullptr;
  };
  std::array<Slot, 8> slots{};
  std::atomic<int> count{0};
};

ListenerTable& Listeners() {
  static ListenerTable* table = new ListenerTable();
  return *table;
}

void NotifyListeners() {
  ListenerTable& table = Listeners();
  const int count = table.count.load(std::memory_order_acquire);
  for (int i = 0; i < count; ++i) table.slots[size_t(i)].fn(table.slots[size_t(i)].arg);
}

// A change of the state: one log line, then the listeners (not while the state is first set up in Init(): nothing has seen it yet,
// and a listener that reads the state from there would wait for Init() to end).
void Apply(const Change& ch, bool notify = true) {
  if (!ch.changed) return;
  const Snapshot& a = ch.after;
  const Snapshot& b = ch.before;
  if (a.device != b.device) {
    MC_INFO("[input] switch device {} (was {}) glyphs {} because {}; generation {}", DeviceName(a.device),
            DeviceName(b.device), FamilyName(a.Family()), CauseText(ch.cause), a.generation);
  } else {
    MC_INFO("[input] glyphs device {} glyphs {} (was {}) strip {} because {}; generation {}", DeviceName(a.device),
            FamilyName(a.Family()), FamilyName(b.Family()), FamilyName(a.PromptStrip()), CauseText(ch.cause), a.generation);
  }
  if (notify) NotifyListeners();
}

// ---- the verbose log -----------------------------------------------------------------------------------

constexpr int kMaxEventLines = 4000;
std::atomic<int> g_event_lines{0};

bool VerboseOpen() {
  return REXCVAR_GET(input_log) && g_event_lines.fetch_add(1, std::memory_order_relaxed) < kMaxEventLines;
}

// ---- the SDK -------------------------------------------------------------------------------------------

rex::input::InputSystem* Isys() {
  auto* rt = rex::Runtime::instance();
  return rt ? static_cast<rex::input::InputSystem*>(rt->input_system()) : nullptr;
}

#ifndef LARECOMP_NO_SDK_DEVICE_INFO
// The family of the pad that made the last real input, as the SDK knows it: 1 PlayStation, 0 any other
// pad (Xbox; Switch, Steam Deck and generic pads draw as Xbox, v1), -1 when there is none or it is the
// synthetic keyboard-and-mouse pad. Valid on the thread that polls the pad.
int PadFamilyFromSdk(rex::input::InputSystem* isys) {
  if (!isys) return -1;
  const rex::input::DeviceInfo* d = isys->ActiveDevice(0);
  if (!d || d->synthetic) return -1;
  return d->pad_kind == rex::input::PadKind::kPlayStation ? 1 : 0;
}
#else
int PadFamilyFromSdk(rex::input::InputSystem*) { return -1; }
#endif

std::atomic<bool> g_list_known{false};
std::atomic<uint32_t> g_list_gen{0};
// The device watcher heard a Sony pad come or go: look at the SDK's list again at the next poll.
std::atomic<bool> g_watch_dirty{false};

// Called on every pad poll (any user): one counter compared; the list is read only when it moved, the
// first time, or when the watcher said something. Runs on the thread that polls (the guest's).
void CheckDevices(rex::input::InputSystem* isys, int64_t now) {
#ifndef LARECOMP_NO_SDK_DEVICE_INFO
  const uint32_t gen = isys->device_generation();
  const bool dirty = g_watch_dirty.load(std::memory_order_relaxed) && g_watch_dirty.exchange(false, std::memory_order_relaxed);
  if (g_list_known.load(std::memory_order_acquire) && gen == g_list_gen.load(std::memory_order_relaxed) && !dirty) return;
  std::array<GlyphFamily, 16> pads{};
  size_t count = 0;
  size_t synthetic = 0;  // the keyboard-and-mouse pad of mnk_mode, the NOP stand-in: not controllers
  std::string names;
  for (const rex::input::DeviceInfo& d : isys->Devices()) {
    if (d.synthetic) ++synthetic;
    if (d.synthetic || count >= pads.size()) continue;
    pads[count++] = d.pad_kind == rex::input::PadKind::kPlayStation ? GlyphFamily::PlayStation : GlyphFamily::Xbox;
    names += (names.empty() ? "" : ", ");
    names += d.name.empty() ? "?" : d.name;
  }
  g_list_gen.store(gen, std::memory_order_relaxed);
  g_list_known.store(true, std::memory_order_release);
  const Change ch = Core().OnDevices(pads.data(), count, now);
  MC_INFO("[input] devices: {} controller(s){}{}; {} synthetic; SDK generation {}; the watcher says Sony pad {}", count, count ? ": " : "", names,
          synthetic, gen, mcla::ui::PlayStationControllerPresent() ? "yes" : "no");
  Apply(ch);
#else
  (void)isys;
  (void)now;
#endif
}

// ---- start-up --------------------------------------------------------------------------------------------

void OnWatcherChanged(void*) {
  // A pool thread of the OS: mark the list and refresh the guess; both are atomics and one mutex.
  g_watch_dirty.store(true, std::memory_order_relaxed);
  Apply(Core().Seed(mcla::ui::PlayStationControllerPresent() ? GlyphFamily::PlayStation : GlyphFamily::Xbox));
}

void ApplyPromptSetting(std::string_view text, bool notify = true) {
  Apply(Core().SetOverride(ParsePromptOverride(text)), notify);
}

std::once_flag g_once;
std::atomic<bool> g_ready{false};

// First use, from whichever thread asks first (the UI manager builds its first prompts on the game thread
// before any pad poll): the Sony guess, the button_prompts setting, the two change hooks.
void Init() {
  if (g_ready.load(std::memory_order_acquire)) return;
  std::call_once(g_once, [] {
    Apply(Core().Seed(mcla::ui::PlayStationControllerPresent() ? GlyphFamily::PlayStation : GlyphFamily::Xbox), /*notify=*/false);
    ApplyPromptSetting(REXCVAR_GET(button_prompts), /*notify=*/false);
    rex::cvar::RegisterChangeCallback("button_prompts",
        [](std::string_view, std::string_view value) { ApplyPromptSetting(value); });
    larecomp::devwatch::AddChangeListener(&OnWatcherChanged, nullptr);
    g_ready.store(true, std::memory_order_release);
  });
}

// ---- the window ----------------------------------------------------------------------------------------

class WindowEvents final : public rex::ui::WindowInputListener, public rex::ui::WindowListener {
 public:
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    const uint16_t vk = static_cast<uint16_t>(e.virtual_key());
    const Change ch = Core().OnKeyDown(vk, e.prev_state(), NowMs());
    if (ch.looked != Cause::None && VerboseOpen()) {
      MC_INFO("[input] ev key down vk=0x{:02X} repeat={} -> {}", unsigned(vk), e.prev_state() ? 1 : 0, ch.note);
    }
    Apply(ch);
  }
  // Key-up never counts.
  void OnKeyUp(rex::ui::KeyEvent&) override {}

  void OnMouseDown(rex::ui::MouseEvent& e) override {
    if (e.button() == rex::ui::MouseEvent::Button::kNone) return;
    const Change ch = Core().OnMouseButtonDown(NowMs());
    if (ch.looked != Cause::None && VerboseOpen()) {
      MC_INFO("[input] ev mouse down button={} -> {}", static_cast<int>(e.button()), ch.note);
    }
    Apply(ch);
  }
  void OnMouseUp(rex::ui::MouseEvent&) override {}

  void OnMouseWheel(rex::ui::MouseEvent& e) override {
    const Change ch = Core().OnMouseWheel(e.scroll_x(), e.scroll_y(), NowMs());
    if (ch.looked != Cause::None && VerboseOpen()) {
      MC_INFO("[input] ev mouse wheel scroll={},{} -> {}", e.scroll_x(), e.scroll_y(), ch.note);
    }
    Apply(ch);
  }

  void OnMouseMove(rex::ui::MouseEvent& e) override {
    const Change ch = Core().OnMouseMove(e.dx(), e.dy(), NowMs());
    if (ch.looked != Cause::None && VerboseOpen()) {
      MC_INFO("[input] ev mouse move dx={:.2f} dy={:.2f} -> {}", e.dx(), e.dy(), ch.note);
    }
    Apply(ch);
  }

  void OnGotFocus(rex::ui::UISetupEvent&) override {
    Core().OnFocus(true, NowMs());
    if (VerboseOpen()) MC_INFO("[input] ev focus got");
  }
  void OnLostFocus(rex::ui::UISetupEvent&) override {
    Core().OnFocus(false, NowMs());
    if (VerboseOpen()) MC_INFO("[input] ev focus lost");
  }
};

WindowEvents g_events;
std::atomic<bool> g_attached{false};

uint16_t BigU16(const uint8_t* p) { return static_cast<uint16_t>((uint16_t(p[0]) << 8) | p[1]); }

}  // namespace

// ---- the public readers -------------------------------------------------------------------------------

InputDevice ActiveInput() {
  Init();
  return Core().Get().device;
}

GlyphFamily ActiveGlyphFamily() {
  Init();
  return Core().Get().Family();
}

GlyphFamily PromptStrip() {
  Init();
  return Core().Get().PromptStrip();
}

uint32_t Generation() {
  Init();
  return Core().Get().generation;
}

bool AddInputListener(InputListener fn, void* arg) {
  if (!fn) return false;
  ListenerTable& table = Listeners();
  std::lock_guard<std::mutex> lock(table.mutex);
  const int count = table.count.load(std::memory_order_relaxed);
  if (count >= int(table.slots.size())) return false;
  table.slots[size_t(count)] = {fn, arg};
  table.count.store(count + 1, std::memory_order_release);
  return true;
}

void Tick() {
  Init();
  if (g_attached.load(std::memory_order_relaxed)) return;
  rex::input::InputSystem* isys = Isys();
  rex::ui::Window* window = isys ? isys->window() : nullptr;
  if (!window) return;
  // Layer 0 like the map's and the pointer's listeners: it never marks an event handled, and an overlay
  // above it (the F4 panel, a browser) gets first refusal, so typing in one does not switch the device.
  window->AddInputListener(&g_events, 0);
  window->AddListener(&g_events);
  Core().OnFocus(window->HasFocus(), NowMs(), /*initial=*/true);
  g_attached.store(true, std::memory_order_relaxed);
  const Snapshot s = Core().Get();
  MC_INFO("[input] state machine ready: window listener attached (focus {}); device {} glyphs {}; generation {}",
          window->HasFocus() ? "yes" : "no", DeviceName(s.device), FamilyName(s.Family()), s.generation);
}

// The XamInputGetState wrapper has run: the SDK's list is looked at (one counter), and for user 0 the state
// the game is about to read is tested for activity.
InputActivityScope::~InputActivityScope() {
  Init();
  rex::input::InputSystem* isys = Isys();
  const int64_t now = NowMs();
  if (isys) CheckDevices(isys, now);
  const uint32_t u = user_ & 0xFF;
  if ((u != 0 && u != 0xFF) || !base_ || state_ == 0) return;
  if ((result_ & 0xFFFFFFFFull) != 0) {  // no pad answered: the next one is compared with a pad at rest
    Core().OnPadGone();
    return;
  }
  // X_INPUT_STATE in guest memory (big-endian): be32 packet, be16 buttons, u8 LT, u8 RT, be16 LX, LY, RX, RY.
  const uint8_t* p = base_ + state_;
  PadSample s;
  s.buttons = BigU16(p + 4);
  s.lt = p[6];
  s.rt = p[7];
  s.lx = static_cast<int16_t>(BigU16(p + 8));
  s.ly = static_cast<int16_t>(BigU16(p + 10));
  s.rx = static_cast<int16_t>(BigU16(p + 12));
  s.ry = static_cast<int16_t>(BigU16(p + 14));
  const Change ch = Core().OnPadFrame(s, [isys] { return PadFamilyFromSdk(isys); }, now);
  if (ch.looked != Cause::None && VerboseOpen()) {
    MC_INFO("[input] ev pad {} buttons=0x{:04X} lt={} rt={} l=({},{}) r=({},{}) -> {}", CauseText(ch.looked), unsigned(s.buttons),
            unsigned(s.lt), unsigned(s.rt), int(s.lx), int(s.ly), int(s.rx), int(s.ry), ch.note);
  }
  Apply(ch);
}

}  // namespace larecomp::input

#else  // REXGLUE_HAS_XEO3_TARGET

namespace larecomp::input {

InputDevice ActiveInput() { return InputDevice::Pad; }
GlyphFamily ActiveGlyphFamily() { return GlyphFamily::Xbox; }
GlyphFamily PromptStrip() { return GlyphFamily::Xbox; }
uint32_t Generation() { return 0; }
bool AddInputListener(InputListener, void*) { return false; }
void Tick() {}
InputActivityScope::~InputActivityScope() {}

}  // namespace larecomp::input

#endif  // REXGLUE_HAS_XEO3_TARGET
