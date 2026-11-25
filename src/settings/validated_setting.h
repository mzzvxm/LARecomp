#pragma once
//
// A text setting with a validator: the one thing REX_SETTING has no trait for.
//
// The 0.10 cvars could carry .validator(fn); rexglue 0.11's Setting<T> is final and only knows Range and Allowed. The
// two validators LARecomp had (the debug menu latch and frame_step_cap) were dropped by the conversion script.
//   * debug_menu: needs nothing now. kInitOnly closes the setting just before OnInit (the SDK's own freeze), which
//     is the point the validator's latch stood for. See larecomp_app_settings.h.
//   * frame_step_cap: "adaptive" or a number of milliseconds. This class carries it.
//
// HOW. SettingBase is the untyped part the registry, the console and the settings panel use. The typed part is four
// virtual functions (rexglue with the settings thread model: Commit, Render, RenderDefault, WaitForCommit); this is
// the same as Setting<std::string> with one more test in Commit. A refused text stores nothing and Commit says so with
// the SDK's own "did not store" error (the reason goes to the log from here). Check() (the pre-test of SetText) is not
// virtual and does not run the validator: only a commit does.
//
// WATCHES. OnChange and Bind return the same guard as Setting<T>'s and obey the same rules (calls in order, never
// concurrent with themselves, a write made inside a call stores at once and calls later, a guard released after the
// setting is gone touches nothing). They are made from the SDK's own rex::detail::SettingWatch, so a ValidatedString is
// watched by the same machinery as every other setting, SettingsBatch and the detach when the setting is destroyed
// included.
//
// CHECK: written against the rexglue SDK of LAR-RXSDK branch upnext/settings-model (setting.h with SettingWatch, Commit,
// WaitForCommit). If the SDK adds a pure virtual to SettingBase or changes the protected part of SettingWatch this stops
// compiling, which is the point: nothing here may drift silently. If the SDK ever gets a validator trait, delete this file.
//
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <rex/logging/log_category.h>
#include <rex/setting.h>

namespace larecomp::settings {

class ValidatedString final : public rex::SettingBase {
 public:
  // The validator says whether a text may be stored; `why` is what the log says when it may not.
  using Validator = bool (*)(std::string_view text);

  template <typename... Traits>
  ValidatedString(rex::SettingOwner* owner, std::string_view name, std::string default_value, std::string_view help,
                  Validator validator, std::string_view why, Traits... traits)
      : rex::SettingBase(*owner, name, rex::SettingType::kString, help),
        validator_(validator),
        why_(why),
        default_(default_value),
        staged_(default_value),
        active_(std::move(default_value)) {
    (rex::detail::ApplyTrait(EditConstraints(), traits), ...);
  }

  std::string Get() const {
    std::lock_guard lock(mutex_);
    return active_;
  }

  // Calls `fn` with the new value after each change, never when OnChange is called (Setting<T>::OnChange).
  [[nodiscard]] rex::ScopeGuard OnChange(rex::MoveOnlyFunction<void(const std::string&)> fn) {
    auto watch = std::make_shared<Watch>(std::move(fn), *this);
    rex::ScopeGuard guard = rex::detail::SettingWatch::Guard(watch);
    watch->Start(watch);
    return guard;
  }

  // Calls `fn` with the current value before Bind returns, and again after each change (Setting<T>::Bind).
  [[nodiscard]] rex::ScopeGuard Bind(rex::MoveOnlyFunction<void(const std::string&)> fn) {
    return rex::detail::AddWatch(std::make_shared<Watch>(std::move(fn), *this));
  }

 private:
  // One function watching this setting: the SDK's SettingGroupWatch<T> for a setting that is not a Setting<T>.
  class Watch final : public rex::detail::SettingWatch {
   public:
    Watch(rex::MoveOnlyFunction<void(const std::string&)> fn, ValidatedString& setting)
        : rex::detail::SettingWatch({&setting}), setting_(&setting), fn_(std::move(fn)) {}

    // Links the watch under the setting's value lock and holds the call meanwhile, without calling the function.
    void Start(const std::shared_ptr<rex::detail::SettingWatch>& self) {
      const Hold hold(*this);
      rex::u64 generation = 0;
      (void)setting_->SnapshotLinked(generation, self);
      MarkSeen(0, generation);
    }

   private:
    void Call() override {
      rex::u64 generation = 0;
      const std::string value = setting_->Snapshot(generation);
      MarkSeen(0, generation);
      fn_(value);
    }

    ValidatedString* setting_;
    rex::MoveOnlyFunction<void(const std::string&)> fn_;
  };

  // The published value and its generation, read together.
  std::string Snapshot(rex::u64& generation) const {
    std::lock_guard lock(mutex_);
    generation = GetGeneration();
    return active_;
  }

  // The same, and the watch is linked to the setting under the same value lock, so a write that stores after the read
  // reaches the watch (Setting<T>::SnapshotLinked).
  std::string SnapshotLinked(rex::u64& generation, std::shared_ptr<rex::detail::SettingWatch> watch) {
    std::lock_guard lock(mutex_);
    generation = GetGeneration();
    LinkWatch(std::move(watch));
    return active_;
  }

  rex::Result<void> Commit(rex::SettingLayer layer, std::string_view text, CommitKind kind) override {
    if (validator_ && !validator_(text)) {
      REXLOG_WARN("{}: '{}' {} (the value stays '{}')", GetName(), text, why_, Render());
      return StoreFailed(text, kind);
    }
    bool published = false;
    {
      std::lock_guard lock(mutex_);
      if (auto admitted = Admit(layer, kind); !admitted) {
        return admitted;
      }
      const bool changed = staged_ != text;
      staged_ = std::string(text);
      published = FinishCommit(layer, kind, changed);
      if (published) {
        active_ = staged_;
        AdvanceGeneration();
      }
    }
    if (published) {
      Notify();
    }
    return rex::Ok();
  }
  std::string Render() const override {
    std::lock_guard lock(mutex_);
    return staged_;
  }
  std::string RenderDefault() const override { return default_; }
  void WaitForCommit() const override { std::lock_guard lock(mutex_); }

  Validator validator_;
  std::string why_;
  std::string default_;
  std::string staged_;
  std::string active_;
  mutable std::mutex mutex_;
};

}  // namespace larecomp::settings
