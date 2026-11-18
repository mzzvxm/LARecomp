// The glue of the active-device state machine (input_state.h, input_state_core.h): feeds it from the window's
// events (keys, mouse, focus: a layer-0 listener, the pattern of cursor_hide.cpp and map_mouse.cpp), from the
// pad state the game reads (an object on the stack of the XamInputGetState wrapper, wheel/wheel_fake.cpp) and
// from the SDK's list of devices (rexglue 0.11: rex::hid::IInputDeviceList, <rex/hid/input_device_list.h>: DeviceGeneration()
// says it changed, Devices() lists the pads with name, vendor, product, pad kind and the user each feeds; the pad of
// user 0 is the one the player holds). Nothing here polls the OS: a pad that comes or goes is a change of the SDK's
// counter (the drivers' own notifications), the Sony guess for the very first frames is the device watcher's
// (device_watch.h, one CM_Register_Notification), and its change listener only marks the list for a fresh look at the
// next poll. The list has no handle per pad, so one is made from vendor, product, name and the pad's place among
// equal ones (PadId); the keyboard and mouse pad and the idle pad are not listed, so there is no "synthetic" count.
//
// Log lines (info level), read by perf\scripts\kbm\kbm_p2_report.py:
//   [input] state machine ready: ...                      once, when the window listener is attached
//   [input] devices: N controller(s) [...]; S synthetic; ... when the SDK's list (or the watcher's Sony flag) changed. S is 0 on 0.11: the list leaves the
//                                                         idle pad (user 0 is always connected, at rest, never the active device) and the keyboard and mouse pad out
//   [input] switch device D (was E) glyphs G because C; generation N   the device changed
//   [input] glyphs device D glyphs G (was H) because C; generation N    only the glyph family / strip / setting changed
//   [input] ev ...                                        with input_log: every event the rules looked at and what they decided
//   [input] pad lost: ...                                 the controller in the player's hands was removed: the game is told "not connected" for a few polls so
//                                                         that it pauses, or why not (keyboard active, not driving, wheel); PadLossRule in input_state_core.h
//   [input] pad loss over: ...                            the window ended; the SDK's stand-in answers again
//
// Also here (KBM P7): the SDK's controllers as a list another thread may read (Controllers(), AddControllerListener(): the controller toast),
// published from CheckDevices, which already looks at the SDK's list whenever its counter moves; and the pads an overlay reads while it is
// open (NoteOverlayPad), because the SDK hands the game a pad at rest then (IInputSystem::AcquireUIBlock, which the dialogs hold: ui/overlay_support.h).

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

#include "settings/larecomp_settings.h"
#include <rex/hid/input_device_list.h>
#include <rex/hid/input_system.h>
#include <rex/hid/pad.h>
#include <rex/input_keys.h>
#include <rex/ui/ui_event.h>

#include "adaptive_triggers.h"
#include "device_watch.h"
#include "logging.h"
#include "sdk_access.h"
#include "ui_events.h"
#include "ui/overlay_support.h"
#include "ui/pad_vendor.h"
#include "wheel/wheel_input.h"


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
  return larecomp::settings::input_log.Get() && g_event_lines.fetch_add(1, std::memory_order_relaxed) < kMaxEventLines;
}

// ---- the SDK -------------------------------------------------------------------------------------------

// The handle of a listed pad, for the rules that compare lists (a controller that left, the pad in the player's hands): 0.11's list has none, so it is
// made from what the list says. Two pads that are alike in vendor, product and name get the place among them (the list is in connection order).
uint64_t PadId(const rex::hid::InputDeviceInfo& d, uint32_t nth_alike) {
  uint64_t h = 1469598103934665603ull;  // FNV-1a over the name
  for (const char c : d.name) h = (h ^ static_cast<uint8_t>(c)) * 1099511628211ull;
  return (uint64_t(d.vendor_id) << 48) ^ (uint64_t(d.product_id) << 32) ^ (h & 0xFFFFFFFFull) ^ (uint64_t(nth_alike) << 56) ^ 1ull;  // never 0
}

// The ids of a device list, in order.
std::vector<uint64_t> PadIds(const std::vector<rex::hid::InputDeviceInfo>& list) {
  std::vector<uint64_t> ids;
  ids.reserve(list.size());
  for (size_t i = 0; i < list.size(); ++i) {
    uint32_t alike = 0;
    for (size_t j = 0; j < i; ++j) {
      if (list[j].vendor_id == list[i].vendor_id && list[j].product_id == list[i].product_id && list[j].name == list[i].name) ++alike;
    }
    ids.push_back(PadId(list[i], alike));
  }
  return ids;
}

// The family of the pad of user 0, as the SDK knows it: 1 PlayStation, 0 any other pad (Xbox; Switch (kNintendo), Steam Deck and generic pads draw as Xbox,
// v1), -1 when there is none. `id` (when given) gets that pad's handle, left alone when the answer is -1. Valid on the thread that polls the pad.
// 0.10's ActiveDevice(0) was "the pad that made the last real input"; the list's user 0 is the pad the player holds (a second pad that joins user 0 under
// hid.shared_pads is listed after it), which is the same pad in every case but the shared one.
// CHECK: with hid.shared_pads on several pads feed user 0; the first listed one decides the glyphs here (0.10's ActiveDevice(0) was the one used last).
int PadFamilyFromSdk(uint64_t* id = nullptr) {
  rex::hid::IInputDeviceList* devices = larecomp::sdk::DeviceList();
  if (!devices) return -1;
  const std::vector<rex::hid::InputDeviceInfo> list = devices->Devices();
  const std::vector<uint64_t> ids = PadIds(list);
  for (size_t i = 0; i < list.size(); ++i) {
    if (list[i].user != 0) continue;
    if (id) *id = ids[i];
    return list[i].pad_kind == rex::hid::PadKind::kPlayStation ? 1 : 0;
  }
  return -1;
}

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
// The counter first and the list second, as rex::hid::IInputDeviceList asks: a change between the two shows at the next read.
void CheckDevices(rex::hid::IInputDeviceList* devices, int64_t now) {
  const uint32_t gen = devices->DeviceGeneration();
  const bool dirty = g_watch_dirty.load(std::memory_order_relaxed) && g_watch_dirty.exchange(false, std::memory_order_relaxed);
  if (g_list_known.load(std::memory_order_acquire) && gen == g_list_gen.load(std::memory_order_relaxed) && !dirty) return;
  const std::vector<rex::hid::InputDeviceInfo> devs = devices->Devices();
  const std::vector<uint64_t> device_ids = PadIds(devs);
  std::array<GlyphFamily, 16> pads{};
  std::array<uint64_t, 16> ids{};
  size_t count = 0;
  std::string names;
  std::vector<ControllerInfo> listed;  // the same controllers for Controllers() (the toast), not capped at 16
  for (size_t i = 0; i < devs.size(); ++i) {
    const rex::hid::InputDeviceInfo& d = devs[i];
    listed.push_back({device_ids[i], d.name});
    if (count >= pads.size()) continue;
    ids[count] = device_ids[i];
    pads[count++] = d.pad_kind == rex::hid::PadKind::kPlayStation ? GlyphFamily::PlayStation : GlyphFamily::Xbox;
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
          0, gen, mcla::ui::PlayStationControllerPresent() ? "yes" : "no");
  LogLoss(loss);
  Apply(ch);
  PublishControllers(std::move(listed));
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
    ApplyPromptSetting(larecomp::settings::button_prompts.Get(), /*notify=*/false);
    larecomp::cvar::RegisterChangeCallback("button_prompts",
        [](std::string_view, std::string_view value) { ApplyPromptSetting(value); });
    larecomp::cvar::RegisterChangeCallback("input_test_pad_loss", [](std::string_view, std::string_view value) {
      if (value == "true" || value == "1") g_test_loss.store(true, std::memory_order_relaxed);
    });
    larecomp::devwatch::AddChangeListener(&OnWatcherChanged, nullptr);
    g_ready.store(true, std::memory_order_release);
  });
}

// ---- the window ----------------------------------------------------------------------------------------

// The UI system's events (ui_events.h): layer 0 like before, they never mark an event handled, and what an overlay holds (typing in one, the
// mouse over one) never reaches them, so typing in a dialog does not switch the device. The UI system runs the binds AFTER its listeners, so the
// F-key overlays' presses are skipped by hand (ui/overlay_support.h IsBoundKey): 0.10 marked them handled first.
struct PointerTrack {
  float x = 0.0f;
  float y = 0.0f;
  bool known = false;
};
PointerTrack g_pointer;  // the UI thread's

void OnInput(rex::InputEvent& e) {
  using Type = rex::InputEvent::Type;
  switch (e.type) {
    case Type::kKeyDown: {
      if (mcla::ui::IsBoundKey(e.key, e.modifiers)) break;
      const uint16_t vk = static_cast<uint16_t>(e.key);
      const Change ch = Core().OnKeyDown(vk, e.repeat, NowMs());
      if (ch.looked != Cause::None && VerboseOpen()) {
        MC_INFO("[input] ev key down vk=0x{:02X} repeat={} -> {}", unsigned(vk), e.repeat ? 1 : 0, ch.note);
      }
      Apply(ch);
      break;
    }
    // Key-up never counts.
    case Type::kButtonDown: {
      if (e.button == rex::MouseButton::kNone) break;
      const Change ch = Core().OnMouseButtonDown(NowMs());
      if (ch.looked != Cause::None && VerboseOpen()) {
        MC_INFO("[input] ev mouse down button={} -> {}", static_cast<int>(e.button), ch.note);
      }
      Apply(ch);
      break;
    }
    case Type::kWheel: {
      // 0.10 gave integer wheel units (120 a detent), 0.11 detents: the rules keep the old units.
      const int32_t sx = static_cast<int32_t>(e.scroll_x * 120.0f);
      const int32_t sy = static_cast<int32_t>(e.scroll_y * 120.0f);
      const Change ch = Core().OnMouseWheel(sx, sy, NowMs());
      if (ch.looked != Cause::None && VerboseOpen()) {
        MC_INFO("[input] ev mouse wheel scroll={},{} -> {}", sx, sy, ch.note);
      }
      Apply(ch);
      break;
    }
    case Type::kMotion: {
      // Relative motion arrives in dx / dy under a pointer lock; free motion gives the position, so the step is the difference from the last one
      // (0.10's dx() / dy() were that step in both).
      float dx = e.dx;
      float dy = e.dy;
      if (dx == 0.0f && dy == 0.0f && g_pointer.known) {
        dx = e.x - g_pointer.x;
        dy = e.y - g_pointer.y;
      }
      g_pointer = {e.x, e.y, true};
      const Change ch = Core().OnMouseMove(dx, dy, NowMs());
      if (ch.looked != Cause::None && VerboseOpen()) {
        MC_INFO("[input] ev mouse move dx={:.2f} dy={:.2f} -> {}", dx, dy, ch.note);
      }
      Apply(ch);
      break;
    }
    default:
      break;
  }
}

void OnFocus(bool gained) {
  Core().OnFocus(gained, NowMs());
  if (VerboseOpen()) MC_INFO("[input] ev focus {}", gained ? "got" : "lost");
}

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
  rex::ui::IUISystem* ui = larecomp::sdk::UI();
  if (!ui || !ui->HasWindow()) return;
  // Layer 0 like the map's and the pointer's handlers: they never mark an event handled, and an overlay
  // above it (the F4 panel, a browser) gets first refusal, so typing in one does not switch the device.
  if (g_attached.exchange(true)) return;
  larecomp::uievents::AddInputHandler(&OnInput);
  larecomp::uievents::AddFocusHandler(&OnFocus);
  Core().OnFocus(ui->IsFocused(), NowMs(), /*initial=*/true);
  const Snapshot s = Core().Get();
  MC_INFO("[input] state machine ready: window listener attached (focus {}); device {} glyphs {}; generation {}",
          ui->IsFocused() ? "yes" : "no", DeviceName(s.device), FamilyName(s.Family()), s.generation);
}

// The XamInputGetState wrapper has run: the SDK's list is looked at (one counter), the game is told "not connected" while a lost
// controller's window is open, and for user 0 the state the game is about to read is tested for activity.
InputActivityScope::~InputActivityScope() {
  Init();
  rex::hid::IInputDeviceList* devices = larecomp::sdk::DeviceList();
  const int64_t now = NowMs();
  if (devices) CheckDevices(devices, now);
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
  const Change ch = Core().OnPadFrame(s, [&pad_id] { return PadFamilyFromSdk(&pad_id); }, now);
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
