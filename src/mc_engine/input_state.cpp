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
//   [input] devices: N controller(s) [...]; S synthetic; ... when the SDK's list (or the watcher's Sony flag) changed. S is 1 on every run: the SDK's NOP
//                                                         stand-in ("None": user 0 is always connected, at rest, never the active device); 2 = mnk_mode is on too
//   [input] switch device D (was E) glyphs G because C; generation N   the device changed
//   [input] glyphs device D glyphs G (was H) because C; generation N    only the glyph family / strip / setting changed
//   [input] ev ...                                        with input_log: every event the rules looked at and what they decided
//   [input] pad lost: ...                                 the controller in the player's hands was removed: the game is told "not connected" for a few polls so
//                                                         that it pauses, or why not (keyboard active, not driving, wheel); PadLossRule in input_state_core.h
//   [input] pad loss over: ...                            the window ended; the SDK's stand-in answers again
//
// Also here (KBM P7): the SDK's controllers as a list another thread may read (Controllers(), AddControllerListener(): the controller toast),
// published from CheckDevices, which already looks at the SDK's list whenever its counter moves; and the pads an overlay reads while it is
// open (NoteOverlayPad), because the SDK hands the game a pad at rest then.

#include "input_state.h"
#include "input_state_core.h"

#ifndef REXGLUE_HAS_XEO3_TARGET

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/device.h>
#include <rex/input/input_system.h>
#include <rex/runtime.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include "adaptive_triggers.h"
#include "device_watch.h"
#include "logging.h"
#include "ui/pad_vendor.h"
#include "wheel/wheel_input.h"

REXCVAR_DEFINE_BOOL(input_log, false, "MCLA/Input",
    "Log every key, mouse and controller event that the active-device rules looked at and what they "
    "decided (counted, or why it was ignored), at most 4000 lines. The switches themselves are always "
    "logged.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(input_test_pad_loss, false, "MCLA/Input",
    "Test only: set to true to act as if the controller in the player's hands had just been unplugged: the "
    "same rule as a real removal (only while a pad is the active device and the game is driven; the pad "
    "need not be real). The next pad poll takes it; set it to false and true again for another.")
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

// Never destroyed, like the state machine: a thread may still be inside a notification at exit.
ListenerTable& Listeners() {
  static ListenerTable* table = new ListenerTable();
  return *table;
}

// The listeners of the controller list (AddControllerListener).
ListenerTable& ControllerListeners() {
  static ListenerTable* table = new ListenerTable();
  return *table;
}

void Notify(ListenerTable& table) {
  const int count = table.count.load(std::memory_order_acquire);
  for (int i = 0; i < count; ++i) table.slots[size_t(i)].fn(table.slots[size_t(i)].arg);
}

void NotifyListeners() { Notify(Listeners()); }

bool AddTo(ListenerTable& table, InputListener fn, void* arg) {
  if (!fn) return false;
  std::lock_guard<std::mutex> lock(table.mutex);
  const int count = table.count.load(std::memory_order_relaxed);
  if (count >= int(table.slots.size())) return false;
  table.slots[size_t(count)] = {fn, arg};
  table.count.store(count + 1, std::memory_order_release);
  return true;
}

// ---- the controllers, for another thread ----------------------------------------------------------------------

struct ControllerStore {
  std::mutex mutex;
  std::vector<ControllerInfo> list;
};

ControllerStore& Store() {
  static ControllerStore* store = new ControllerStore();
  return *store;
}

bool SameControllers(const std::vector<ControllerInfo>& a, const std::vector<ControllerInfo>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].id != b[i].id || a[i].name != b[i].name) return false;
  }
  return true;
}

// The SDK's list was just read (CheckDevices): when the set of controllers differs from the last one published, publish it and tell the
// listeners (after the lock is let go: a listener may read Controllers()).
void PublishControllers(std::vector<ControllerInfo> fresh) {
  ControllerStore& store = Store();
  {
    std::lock_guard<std::mutex> lock(store.mutex);
    if (SameControllers(store.list, fresh)) return;
    store.list = std::move(fresh);
  }
  Notify(ControllerListeners());
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
// pad (Xbox; Switch, Steam Deck and generic pads draw as Xbox, v1), -1 when there is none or it is a
// synthetic device (the SDK's NOP stand-in, which only an idle state is ever read from, so it is never the
// active device anyway). `id` (when given) gets that pad's SDK handle, left alone when the answer is -1.
// Valid on the thread that polls the pad.
int PadFamilyFromSdk(rex::input::InputSystem* isys, uint64_t* id = nullptr) {
  if (!isys) return -1;
  const rex::input::DeviceInfo* d = isys->ActiveDevice(0);
  if (!d || d->synthetic) return -1;
  if (id) *id = static_cast<uint64_t>(d->id);
  return d->pad_kind == rex::input::PadKind::kPlayStation ? 1 : 0;
}
#else
int PadFamilyFromSdk(rex::input::InputSystem*, uint64_t* = nullptr) { return -1; }
#endif

// The rule of the pause for a lost controller (input_state_core.h). Never destroyed, like the state machine.
PadLossRule& Loss() {
  static PadLossRule* rule = new PadLossRule();
  return *rule;
}

// The facts the rule is given when a controller leaves.
bool DrivingNow() { return larecomp::triggers::IsDriving(); }
bool WheelNow() { return larecomp::wheel::WheelPresent(); }

void LogLoss(LossVerdict v) {
  if (v == LossVerdict::None) return;
  MC_INFO("[input] pad lost: the controller in the player's hands was removed; {}{}", LossVerdictText(v),
          v == LossVerdict::Open ? std::string(" (for the next ") + std::to_string(kPadLossPolls) + " polls of user 0)" : std::string());
}

// input_test_pad_loss was set to true: the next pad poll acts as if the controller in the player's hands had been removed.
std::atomic<bool> g_test_loss{false};

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
  std::array<uint64_t, 16> ids{};
  size_t count = 0;
  size_t synthetic = 0;  // the SDK's NOP stand-in (always there) and, with mnk_mode, its keyboard-and-mouse pad: not controllers
  std::string names;
  std::vector<ControllerInfo> listed;  // the same controllers for Controllers() (the toast), not capped at 16
  for (const rex::input::DeviceInfo& d : isys->Devices()) {
    if (d.synthetic) ++synthetic;
    if (d.synthetic) continue;
    listed.push_back({static_cast<uint64_t>(d.id), d.name});
    if (count >= pads.size()) continue;
    ids[count] = static_cast<uint64_t>(d.id);
    pads[count++] = d.pad_kind == rex::input::PadKind::kPlayStation ? GlyphFamily::PlayStation : GlyphFamily::Xbox;
    names += (names.empty() ? "" : ", ");
    names += d.name.empty() ? "?" : d.name;
  }
  g_list_gen.store(gen, std::memory_order_relaxed);
  g_list_known.store(true, std::memory_order_release);
  // The pause for a lost controller is decided first: it needs the device as it was before the removal (the state machine moves it off
  // a removed pad). The list is applied to the state machine right after.
  const InputDevice device_before = Core().Get().device;
  const LossVerdict loss = Loss().OnList(ids.data(), count, device_before, DrivingNow(), WheelNow(), now);
  const Change ch = Core().OnDevices(pads.data(), count, now);
  MC_INFO("[input] devices: {} controller(s){}{}; {} synthetic; SDK generation {}; the watcher says Sony pad {}", count, count ? ": " : "", names,
          synthetic, gen, mcla::ui::PlayStationControllerPresent() ? "yes" : "no");
  LogLoss(loss);
  Apply(ch);
  PublishControllers(std::move(listed));
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
    rex::cvar::RegisterChangeCallback("input_test_pad_loss", [](std::string_view, std::string_view value) {
      if (value == "true" || value == "1") g_test_loss.store(true, std::memory_order_relaxed);
    });
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

bool AddInputListener(InputListener fn, void* arg) { return AddTo(Listeners(), fn, arg); }

std::vector<ControllerInfo> Controllers() {
  ControllerStore& store = Store();
  std::lock_guard<std::mutex> lock(store.mutex);
  return store.list;
}

bool AddControllerListener(InputListener fn, void* arg) { return AddTo(ControllerListeners(), fn, arg); }

// An overlay's own reading of the pads (the browsers' dialogs): the same rules as the game's poll, a filter of its own. UI thread.
void NoteOverlayPad(bool connected, uint16_t buttons, float lx, float ly, float rx, float ry) {
  Init();
  if (!connected) {
    Core().OnOverlayPadGone();
    return;
  }
  const Change ch = Core().OnOverlayPadFrame(OverlayPadSample(buttons, lx, ly, rx, ry), NowMs());
  if (ch.looked != Cause::None && VerboseOpen()) {
    MC_INFO("[input] ev overlay pad {} buttons=0x{:04X} l=({:.2f},{:.2f}) r=({:.2f},{:.2f}) -> {}", CauseText(ch.looked), unsigned(buttons), lx, ly, rx, ry,
            ch.note);
  }
  Apply(ch);
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

// The XamInputGetState wrapper has run: the SDK's list is looked at (one counter), the game is told "not connected" while a lost
// controller's window is open, and for user 0 the state the game is about to read is tested for activity.
InputActivityScope::~InputActivityScope() {
  Init();
  rex::input::InputSystem* isys = Isys();
  const int64_t now = NowMs();
  if (isys) CheckDevices(isys, now);
  const uint32_t u = user_ & 0xFF;
  if ((u != 0 && u != 0xFF) || !base_ || state_ == 0) return;

  // The test cvar: the same decision as a real removal, without a pad to unplug.
  if (g_test_loss.load(std::memory_order_relaxed) && g_test_loss.exchange(false, std::memory_order_relaxed)) {
    LogLoss(Loss().OnPadLost(Core().Get().device, DrivingNow(), WheelNow(), now));
  }
  // The controller in the player's hands was removed while the game is driven: "not connected" for a few polls, so the game pauses. The
  // state is cleared too (the harness's pad and anything else that filled it), like the probe of KBM P0c did.
  const LossAnswer answer = Loss().OnStatePoll(now);
  if (answer != LossAnswer::Connected) {
    std::memset(base_ + state_, 0, 16);  // X_INPUT_STATE: the packet number and a gamepad at rest
    result_ = 0x48F;                     // ERROR_DEVICE_NOT_CONNECTED
    if (answer == LossAnswer::NotConnectedLast) {
      MC_INFO("[input] pad loss over: the game was told \"not connected\" for {} polls of user 0; the stand-in answers again, the keyboard and a returning pad work as usual",
              Loss().Answered());
    }
  }
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
  uint64_t pad_id = 0;
  const Change ch = Core().OnPadFrame(s, [isys, &pad_id] { return PadFamilyFromSdk(isys, &pad_id); }, now);
  if (pad_id != 0) Loss().NoteActivePad(pad_id);  // the pad in the player's hands: what a later removal is compared with
  if (ch.looked != Cause::None && VerboseOpen()) {
    MC_INFO("[input] ev pad {} buttons=0x{:04X} lt={} rt={} l=({},{}) r=({},{}) -> {}", CauseText(ch.looked), unsigned(s.buttons),
            unsigned(s.lt), unsigned(s.rt), int(s.lx), int(s.ly), int(s.rx), int(s.ry), ch.note);
  }
  Apply(ch);
}

// The XamInputGetCapabilities wrapper has run: while the window of a lost controller is open the game is told "not connected" here too.
void PadLossCaps(uint8_t* base, uint32_t user, uint32_t caps, uint64_t& r3) {
  const uint32_t u = user & 0xFF;
  if (!base || caps == 0 || (u != 0 && u != 0xFF)) return;
  if (!Loss().CapsHidden()) return;
  std::memset(base + caps, 0, 20);  // X_INPUT_CAPABILITIES
  r3 = 0x48F;                       // ERROR_DEVICE_NOT_CONNECTED
}

}  // namespace larecomp::input

#else  // REXGLUE_HAS_XEO3_TARGET

namespace larecomp::input {

InputDevice ActiveInput() { return InputDevice::Pad; }
GlyphFamily ActiveGlyphFamily() { return GlyphFamily::Xbox; }
GlyphFamily PromptStrip() { return GlyphFamily::Xbox; }
uint32_t Generation() { return 0; }
bool AddInputListener(InputListener, void*) { return false; }
std::vector<ControllerInfo> Controllers() { return {}; }
bool AddControllerListener(InputListener, void*) { return false; }
void NoteOverlayPad(bool, uint16_t, float, float, float, float) {}
void Tick() {}
InputActivityScope::~InputActivityScope() {}
void PadLossCaps(uint8_t*, uint32_t, uint32_t, uint64_t&) {}

}  // namespace larecomp::input

#endif  // REXGLUE_HAS_XEO3_TARGET
