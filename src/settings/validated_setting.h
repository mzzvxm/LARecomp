#pragma once
//
// A text setting with a validator: the one thing REX_SETTING has no trait for.
//
// The 0.10 cvars could carry .validator(fn); rexglue 0.11's Setting<T> is final and only knows Range and Allowed. The
// two validators LARecomp had (the debug menu latch and frame_step_cap) were dropped by the conversion script.
//   * debug_menu: needs nothing now. kStartupOnly closes the setting just before OnInit (the SDK's own freeze), which
//     is the point the validator's latch stood for. See larecomp_app_settings.h.
//   * frame_step_cap: "adaptive" or a number of milliseconds. This class carries it.
//
// HOW. SettingBase is the untyped part the registry, the console and the settings panel use. Its constructor and the
// five storage hooks (Store, Publish, Render, RenderDefault, Notify) are the interface a typed setting implements; this
// is the same as Setting<std::string> with one more test in Store. Store returns false for a refused text and
// SettingBase::SetText then reports "did not store" (the reason goes to the log from here). Check() (the registry's
// pre-test) is not virtual and does not run the validator: only a store does.
//
// CHECK: written against rexglue 0.11.0 setting.h. If the SDK adds a pure virtual to SettingBase this stops compiling,
// which is the point: nothing here may drift silently. If the SDK ever gets a validator trait, delete this file.
//
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

  [[nodiscard]] rex::ScopeGuard OnChange(rex::MoveOnlyFunction<void(const std::string&)> fn) {
    std::uint64_t id;
    {
      std::lock_guard lock(mutex_);
      id = next_watch_++;
      watches_.emplace_back(id, std::make_shared<rex::MoveOnlyFunction<void(const std::string&)>>(std::move(fn)));
    }
    return rex::ScopeGuard([this, id] {
      std::lock_guard lock(mutex_);
      std::erase_if(watches_, [id](const auto& entry) { return entry.first == id; });
    });
  }

 private:
  bool Store(std::string_view text) override {
    if (validator_ && !validator_(text)) {
      REXLOG_WARN("{}: '{}' {} (the value stays '{}')", GetName(), text, why_, Render());
      return false;
    }
    std::lock_guard lock(mutex_);
    staged_ = std::string(text);
    return true;
  }
  void Publish() override {
    std::lock_guard lock(mutex_);
    active_ = staged_;
  }
  std::string Render() const override {
    std::lock_guard lock(mutex_);
    return staged_;
  }
  std::string RenderDefault() const override { return default_; }
  void Notify() override {
    std::vector<std::shared_ptr<rex::MoveOnlyFunction<void(const std::string&)>>> calls;
    std::string value;
    {
      std::lock_guard lock(mutex_);
      value = active_;
      for (auto& entry : watches_) calls.push_back(entry.second);
    }
    for (auto& call : calls) (*call)(value);
  }

  Validator validator_;
  std::string why_;
  std::string default_;
  std::string staged_;
  std::string active_;
  mutable std::mutex mutex_;
  std::uint64_t next_watch_ = 1;
  std::vector<std::pair<std::uint64_t, std::shared_ptr<rex::MoveOnlyFunction<void(const std::string&)>>>> watches_;
};

}  // namespace larecomp::settings
