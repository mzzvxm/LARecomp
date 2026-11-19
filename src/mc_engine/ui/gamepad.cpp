#include "gamepad.h"

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

#include "mc_engine/input/device_watch.h"
#include "mc_engine/input/own_sdl.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <xinput.h>
#endif

namespace mcla::ui {

namespace {

using larecomp::padfeed::PadSample;

// The ImGui gamepad keys the save picker drives, with the XInput bit each one is read from.
struct ButtonKey {
  ImGuiKey key;
  uint16_t bit;
};

constexpr ButtonKey kButtons[] = {
    {ImGuiKey_GamepadDpadUp, 0x0001},   {ImGuiKey_GamepadDpadDown, 0x0002},
    {ImGuiKey_GamepadDpadLeft, 0x0004}, {ImGuiKey_GamepadDpadRight, 0x0008},
    {ImGuiKey_GamepadStart, 0x0010},    {ImGuiKey_GamepadBack, 0x0020},
    {ImGuiKey_GamepadFaceDown, 0x1000}, {ImGuiKey_GamepadFaceRight, 0x2000},
    {ImGuiKey_GamepadFaceLeft, 0x4000}, {ImGuiKey_GamepadFaceUp, 0x8000},
};

// Every button a dialog may want, as XInput's bit and the SDL button it is read from.
struct SdlButton {
  uint16_t bit;
  SDL_GamepadButton button;
};

constexpr SdlButton kSdlButtons[] = {
    {0x0001, SDL_GAMEPAD_BUTTON_DPAD_UP},       {0x0002, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {0x0004, SDL_GAMEPAD_BUTTON_DPAD_LEFT},     {0x0008, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    {0x0010, SDL_GAMEPAD_BUTTON_START},         {0x0020, SDL_GAMEPAD_BUTTON_BACK},
    {0x0040, SDL_GAMEPAD_BUTTON_LEFT_STICK},    {0x0080, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    {0x0100, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {0x0200, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {0x1000, SDL_GAMEPAD_BUTTON_SOUTH},         {0x2000, SDL_GAMEPAD_BUTTON_EAST},
    {0x4000, SDL_GAMEPAD_BUTTON_WEST},          {0x8000, SDL_GAMEPAD_BUTTON_NORTH},
};

// Up, down, left, right.
constexpr ImGuiKey kStickKeys[] = {ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown,
                                   ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight};

bool g_layer_up = false;

uint64_t NowMs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

#if defined(_WIN32)
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

XInputGetStateFn LoadXInput() {
  static const XInputGetStateFn fn = [] {
    for (const char* dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"}) {
      if (HMODULE m = LoadLibraryA(dll)) {
        if (auto f = reinterpret_cast<XInputGetStateFn>(GetProcAddress(m, "XInputGetState"))) {
          return f;
        }
      }
    }
    return XInputGetStateFn(nullptr);
  }();
  return fn;
}
#endif

// What the dialogs read: Xbox pads through XInput (Windows; an empty slot is asked once a second,
// SlotBackoff) and every other pad through the pads this layer has open in the exe's SDL.
// One per process, UI thread only.
class OverlayPadReader {
 public:
  PadSample Read() {
    const auto started = std::chrono::steady_clock::now();
    PadSample merged;
    const uint64_t now = NowMs();
    // The device watcher says when a pad came or went: look at every slot and at SDL's list at
    // once rather than at the next retry.
    const uint32_t generation = larecomp::devwatch::Generation();
#if defined(_WIN32)
    if (generation != slots_generation_) {
      slots_generation_ = generation;
      slots_.Reset();
    }
    ReadXInput(merged, now);
#endif
    if (g_layer_up) {
      // Picks up pads that came or went, and refreshes the ones that are open.
      SDL_UpdateGamepads();
      if (enumerate_.Due(now, generation)) {
        Enumerate();
        enumerate_.Done(now, generation);
      }
      for (auto& [id, pad] : pads_) {
        larecomp::padfeed::MergeInto(merged, ReadSdl(pad));
      }
    }
    LogChange();
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
    ++reads_;
    read_us_total_ += static_cast<uint64_t>(us);
    read_us_max_ = std::max<uint64_t>(read_us_max_, static_cast<uint64_t>(us));
    return merged;
  }

  void Release() {
    for (auto& [id, pad] : pads_) SDL_CloseGamepad(pad);
    pads_.clear();
    enumerate_.Forget();
#if defined(_WIN32)
    slots_.Reset();
#endif
    if (reads_ != 0) {
      REXLOG_INFO("[padfeed] pads let go after {} reading(s): {} us each on average, {} us at most", reads_,
                  read_us_total_ / reads_, read_us_max_);
    }
    reads_ = 0;
    read_us_total_ = 0;
    read_us_max_ = 0;
    logged_sdl_ = logged_xinput_ = -1;
  }

 private:
  // The list of gamepads SDL knows, opened once and kept: a reading then touches only these.
  void Enumerate() {
    int count = 0;
    std::unordered_map<SDL_JoystickID, SDL_Gamepad*> next;
#if defined(_WIN32)
    const bool xinput_reads_xbox = LoadXInput() != nullptr;
#endif
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
      for (int i = 0; i < count; ++i) {
#if defined(_WIN32)
        // XInput reads the Xbox pads (an Elite has no DirectInput mapping, and opening one here
        // would put the exe's SDL on a pad the SDK's SDL already has): not read twice.
        if (xinput_reads_xbox) {
          const SDL_GamepadType type = SDL_GetGamepadTypeForID(ids[i]);
          if (type == SDL_GAMEPAD_TYPE_XBOX360 || type == SDL_GAMEPAD_TYPE_XBOXONE) continue;
        }
#endif
        auto it = pads_.find(ids[i]);
        if (it != pads_.end()) {
          next.emplace(ids[i], it->second);
          pads_.erase(it);
        } else if (SDL_Gamepad* pad = SDL_OpenGamepad(ids[i])) {
          next.emplace(ids[i], pad);
        }
      }
      SDL_free(ids);
    }
    for (auto& [id, pad] : pads_) SDL_CloseGamepad(pad);  // gone since the last look
    pads_ = std::move(next);
  }

  // One line when what the dialogs read changes (and the first time), not one per frame.
  void LogChange() {
    const int sdl_pads = static_cast<int>(pads_.size());
#if defined(_WIN32)
    const int xinput_pads = slots_.ConnectedCount();
#else
    const int xinput_pads = 0;
#endif
    if (sdl_pads == logged_sdl_ && xinput_pads == logged_xinput_) return;
    logged_sdl_ = sdl_pads;
    logged_xinput_ = xinput_pads;
    REXLOG_INFO("[padfeed] the dialogs read {} SDL pad(s) and {} XInput pad(s)", sdl_pads, xinput_pads);
  }

  static PadSample ReadSdl(SDL_Gamepad* pad) {
    PadSample sample;
    sample.connected = true;
    for (const SdlButton& b : kSdlButtons) {
      if (SDL_GetGamepadButton(pad, b.button)) sample.buttons = uint16_t(sample.buttons | b.bit);
    }
    sample.lx = larecomp::padfeed::AxisToUnit(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX));
    sample.ly = -larecomp::padfeed::AxisToUnit(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY));
    sample.rx = larecomp::padfeed::AxisToUnit(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX));
    sample.ry = -larecomp::padfeed::AxisToUnit(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY));
    return sample;
  }

#if defined(_WIN32)
  // A slot with a pad is read on every frame; an empty one is asked once a second (SlotBackoff).
  void ReadXInput(PadSample& merged, uint64_t now) {
    XInputGetStateFn get = LoadXInput();
    if (!get) return;
    for (int slot = 0; slot < larecomp::padfeed::SlotBackoff::kSlots; ++slot) {
      if (!slots_.Due(slot, now)) continue;
      XINPUT_STATE st{};
      const bool ok = get(static_cast<DWORD>(slot), &st) == ERROR_SUCCESS;
      slots_.Result(slot, ok, now);
      if (!ok) continue;
      PadSample sample;
      sample.connected = true;
      sample.buttons = st.Gamepad.wButtons;
      sample.lx = larecomp::padfeed::AxisToUnit(st.Gamepad.sThumbLX);
      sample.ly = larecomp::padfeed::AxisToUnit(st.Gamepad.sThumbLY);  // XInput: up is positive
      sample.rx = larecomp::padfeed::AxisToUnit(st.Gamepad.sThumbRX);
      sample.ry = larecomp::padfeed::AxisToUnit(st.Gamepad.sThumbRY);
      larecomp::padfeed::MergeInto(merged, sample);
    }
  }

  larecomp::padfeed::SlotBackoff slots_;
  uint32_t slots_generation_ = 0;
#endif

  std::unordered_map<SDL_JoystickID, SDL_Gamepad*> pads_;
  larecomp::padfeed::EnumeratePolicy enumerate_;
  // For the log line when the dialog closes: what a reading cost.
  uint64_t reads_ = 0;
  uint64_t read_us_total_ = 0;
  uint64_t read_us_max_ = 0;
  int logged_sdl_ = -1;
  int logged_xinput_ = -1;
};

OverlayPadReader& Reader() {
  static OverlayPadReader reader;
  return reader;
}

}  // namespace

void EnsurePadLayer() {
  static std::once_flag once;
  std::call_once(once, [] {
    // Is this executable's SDL the SDK's? (mc_engine/input/sdl_instance.h.) With two SDLs (the 0.10 line on
    // Windows: the SDK's is inside rexruntime.dll) this copy is nobody's but ours: it must not open pads through
    // the drivers that would take them from the SDK's copy, nothing pumps its event queue, and its gamepad layer
    // is started here, by the first dialog, and polled (SDL_UpdateGamepads). Same hints as the wheel thread's
    // (wheel_input.cpp, WheelThread; whichever starts first sets them, once).
    //
    // With one SDL (Linux, the 0.11 line) this is the SAME SDL as the SDK's driver: HIDAPI stays on for it, and
    // gamepad events stay on, because the driver is driven by exactly those events. Switching them off here left
    // a detected pad that never sent a button.
    const std::string backend = larecomp::cvar::GetFlagByName("input_backend");
    larecomp::sdlinst::ApplyOwnCopyHints(backend);
    g_layer_up = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    if (!g_layer_up) {
      REXLOG_WARN("[ui] Pad layer: SDL gamepad init failed: {}", SDL_GetError());
      return;
    }
    // Nothing pumps this SDL's event queue (with two SDLs); state is read by polling.
    larecomp::sdlinst::SilenceOwnCopyEvents(backend);
    REXLOG_INFO("[ui] Pad layer up: {}", larecomp::sdlinst::DescribeOwnCopy(backend));
    // The mappings the SDK's driver loads, so a DirectInput pad maps the same.
    const std::string mappings = larecomp::cvar::GetFlagByName("hid_mappings_file");
    std::error_code ec;
    if (!mappings.empty() && std::filesystem::exists(mappings, ec)) {
      SDL_AddGamepadMappingsFromFile(mappings.c_str());
    }
  });
}

PadSample ReadOverlayPads() {
  EnsurePadLayer();
  return Reader().Read();
}

void ReleaseOverlayPads() {
  Reader().Release();
}

MenuGamepad::MenuGamepad() {
  EnsurePadLayer();
}

MenuGamepad::~MenuGamepad() {
  Release(io_);
}

void MenuGamepad::Release(ImGuiIO* io) {
  ReleaseOverlayPads();
  // Keys left down would stay down in ImGui after the pads are gone.
  if (io && fed_) {
    for (const ButtonKey& b : kButtons) io->AddKeyEvent(b.key, false);
    for (ImGuiKey k : kStickKeys) io->AddKeyAnalogEvent(k, false, 0.0f);
    io->BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    fed_ = false;
  }
  connected_ = false;
}

void MenuGamepad::Feed(ImGuiIO& io) {
  io_ = &io;
  const PadSample pad = ReadOverlayPads();
  connected_ = pad.connected;
  if (!pad.connected) {
    if (fed_) Release(&io);
    return;
  }
  // Up, down, left, right, 0..1 before the dead zone.
  const float stick[4] = {std::max(pad.ly, 0.0f), std::max(-pad.ly, 0.0f), std::max(-pad.lx, 0.0f),
                          std::max(pad.lx, 0.0f)};
  for (const ButtonKey& b : kButtons) io.AddKeyEvent(b.key, (pad.buttons & b.bit) != 0);
  constexpr float kDead = 0.35f;
  for (size_t i = 0; i < 4; ++i) {
    const float v = std::clamp((stick[i] - kDead) / (1.0f - kDead), 0.0f, 1.0f);
    io.AddKeyAnalogEvent(kStickKeys[i], v > 0.0f, v);
  }
  // Without it ImGui's NewFrame zeroes every gamepad key, so the events above
  // would never reach IsKeyPressed.
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  fed_ = true;
}

} // namespace mcla::ui
