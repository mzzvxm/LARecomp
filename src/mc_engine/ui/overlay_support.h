#pragma once
// ===========================================================================
// What LARecomp's dialogs and hotkeys build on, on rexglue 0.11's UI system
// ===========================================================================
// 0.10 had rex::ui::ImGuiDialog (a class that added itself to the ImGuiDrawer, drew through OnDraw and deleted
// itself after Close()), rex::ui::RegisterBind("bind_x", "F2", help, fn) and an ImmediateDrawer that made the
// textures of icons. 0.11 has none of them. Its model (rex/ui/ui_system.h):
//
//   * rex::ui::IDialog / DialogBase: Draw(ImGuiIO&) returns false to close, on the UI thread. A dialog is drawn
//     while it is added to the UI system: IUISystem::AddDialog(unique_ptr) (the system owns it, drops it when
//     Draw returns false), AddDialog(IDialog&) (a guard removes it), or as an overlay.
//   * An overlay is an OverlayConfig{name, default KeyChord, create}: the UI system makes the dialog when the
//     window opens, and the key `ui.bind.<name>` (a setting, rebindable in F4 and the toml) shows or hides it.
//     A dialog whose OnToggle() returns true handles the key itself and stays hidden: that is how a hotkey that
//     only runs an action is written here (HotkeyDialog). `create` is a plain function pointer: no captures.
//   * ImGui 1.92 owns textures: a dialog makes an ImTextureData and registers it (IconTexture below); the
//     renderer (gpu/overlay.cpp for Plume, the SDK's drivers for Xenia) creates, updates and frees the GPU copy.
//
// PanelDialog keeps the 0.10 shape (OnDraw, Close()) so the dialogs in this folder change as little as possible.
// ===========================================================================

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>  // RegisterUserTexture / UnregisterUserTexture

#include <rex/hid/input_system.h>
#include <rex/module.h>
#include <rex/ui/ui_system.h>

#include "imgui_theme.h"

// modloader/texture.cpp: the stb decoder this program carries (the SDK's rex::ui::DecodeImageRGBA is private since 0.11).
namespace mc::modloader {
std::vector<uint8_t> DecodeImageRGBA(const uint8_t* data, size_t size, int& width, int& height);
}  // namespace mc::modloader

namespace mcla::ui {

// A dialog written the 0.10 way: override OnDraw, call Close() to end it. Draw runs EnsureMclaTheme first, so a
// panel looks the same whatever the SDK did to ImGui's style since the last frame (see imgui_theme.h).
//
// Lifetime: an overlay (OverlayConfig::create) is owned by the UI system and drawn again after a Close() when
// its key is pressed (OnShow clears the flag). A dialog made with IUISystem::AddDialog(unique_ptr) is dropped
// by the UI system when it closes.
class PanelDialog : public rex::ui::DialogBase {
 public:
  bool Draw(ImGuiIO& io) override {
    EnsureMclaTheme();
    OnDraw(io);
    return !closed_;
  }

  void OnShow() override {
    closed_ = false;
    OnOpened();
  }

 protected:
  virtual void OnDraw(ImGuiIO& io) = 0;
  // The dialog is being shown (the key, or the first time).
  virtual void OnOpened() {}
  // Ends the dialog after this frame.
  void Close() { closed_ = true; }
  bool IsClosing() const { return closed_; }

 private:
  bool closed_ = false;
};

// A key that runs an action and shows nothing (OnToggle handles the press while the dialog is hidden, and it is
// never added to the screen). Make it with MakeHotkey<&Action> in an OverlayConfig.
class HotkeyDialog final : public rex::ui::DialogBase {
 public:
  explicit HotkeyDialog(void (*action)()) : action_(action) {}
  bool Draw(ImGuiIO&) override { return false; }
  bool OnToggle() override {
    action_();
    return true;
  }

 private:
  void (*action_)();
};

template <void (*Action)()>
std::unique_ptr<rex::ui::IDialog> MakeHotkey(rex::ui::IUISystem&) {
  return std::make_unique<HotkeyDialog>(Action);
}

// The text of a bound key for a prompt ("F3"), from the setting ui.bind.<name>; `fallback` when it is not set or
// not registered (yet). Any thread.
inline std::string BindText(std::string_view name, const char* fallback) {
  auto found = REX_SETTINGS().Find(std::string("ui.bind.") + std::string(name));
  if (found && *found) {
    std::string text = (*found)->GetText();
    if (!text.empty()) {
      return text;
    }
  }
  return fallback;
}

// While held, the pads read neutral for the title (IInputSystem::AcquireUIBlock; calls nest), which is what the
// 0.10 UI system did for any open dialog: the SDK's 0.11 overlays do it for themselves only when they say
// WantsExclusiveInput. The dialogs of LARecomp read the pads themselves (ui/gamepad.h), which the block does not
// touch. Set(true) when the window opens, Set(false) when it closes; the destructor lets go.
class InputBlock {
 public:
  void Set(bool on) {
    if (on == held_) return;
    if (auto* input = REX_INTERFACE(rex::hid::IInputSystem)) {
      if (on) input->AcquireUIBlock(); else input->ReleaseUIBlock();
      held_ = on;
    }
  }
  bool held() const { return held_; }
  ~InputBlock() { Set(false); }

 private:
  bool held_ = false;
};

// A picture for ImGui::Image, made from RGBA8 pixels (what a decoded PNG is). Create it and destroy it on the UI
// thread inside a dialog's Draw/OnDraw (the ImGui context is current then); use Ref() with ImGui::Image. The
// renderer makes the GPU copy on the next pass, so a new icon shows one frame later.
class IconTexture {
 public:
  static std::unique_ptr<IconTexture> Create(int width, int height, const uint8_t* rgba) {
    if (width <= 0 || height <= 0 || !rgba || !ImGui::GetCurrentContext()) {
      return nullptr;
    }
    std::unique_ptr<IconTexture> icon(new IconTexture());
    icon->context_ = ImGui::GetCurrentContext();
    icon->data_ = std::make_unique<ImTextureData>();
    icon->data_->Create(ImTextureFormat_RGBA32, width, height);
    std::memcpy(icon->data_->GetPixels(), rgba, size_t(width) * size_t(height) * 4);
    icon->data_->UseColors = true;
    ImGui::RegisterUserTexture(icon->data_.get());
    return icon;
  }

  // A PNG (or any picture stb reads) as an icon; null when it does not decode or is larger than `max_size` a side.
  static std::unique_ptr<IconTexture> FromEncoded(const uint8_t* data, size_t size, int max_size = 2048) {
    int width = 0;
    int height = 0;
    const std::vector<uint8_t> rgba = mc::modloader::DecodeImageRGBA(data, size, width, height);
    if (rgba.empty() || width > max_size || height > max_size) {
      return nullptr;
    }
    return Create(width, height, rgba.data());
  }

  ~IconTexture() {
    // The context is current in a dialog's Draw. Elsewhere on the UI thread (a dialog dropped between frames) it is
    // not: it is made current for the call. CHECK: the UI system destroys its dialogs before the context
    // (ImGuiHost::~ImGuiHost) and the context outlives every dialog it draws; a dialog that outlived it would
    // unregister from a dead context.
    if (data_ && context_) {
      ImGuiContext* previous = ImGui::GetCurrentContext();
      ImGui::SetCurrentContext(context_);
      ImGui::UnregisterUserTexture(data_.get());
      ImGui::SetCurrentContext(previous);
    }
  }

  IconTexture(const IconTexture&) = delete;
  IconTexture& operator=(const IconTexture&) = delete;

  ImTextureRef Ref() { return data_->GetTexRef(); }
  int width() const { return data_->Width; }
  int height() const { return data_->Height; }

 private:
  IconTexture() = default;
  ImGuiContext* context_ = nullptr;
  std::unique_ptr<ImTextureData> data_;
};

}  // namespace mcla::ui
