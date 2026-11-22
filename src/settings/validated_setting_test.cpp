// A check of ValidatedString (settings/validated_setting.h) against the SDK's own settings: the one SettingBase LARecomp implements itself
// (frame_step_cap). It needs no game, but it links rex::base, so it needs rexbase.dll next to it or on the PATH, and the SDK's operator new /
// delete forwarders (rexglue_link_host_allocator):
//   cmake --build <build dir> --target larecomp_validated_setting_test, then run <build dir>/larecomp_validated_setting_test (exit 0 = passed).
//
// What it holds the class to is what Setting<std::string> does: a text that fails the validator stores nothing and says so, the layers and the
// restore to the default work, and OnChange / Bind give the SDK's calls (only after a change; at once and after each change; in a batch, one
// call; a guard released after the setting is gone is harmless).

#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <rex/setting.h>

#include "settings/validated_setting.h"

namespace {

int g_checks = 0;
int g_failed = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_failed;                                                            \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
    }                                                                        \
  } while (0)

bool AdaptiveOrNumber(std::string_view text) {
  if (text == "adaptive") return true;
  if (text.empty()) return false;
  for (char c : text) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

class Owner final : public rex::SettingOwner {
 public:
  Owner() : SettingOwner("validated_setting_test") {}
  larecomp::settings::ValidatedString cap{this, "cap", "adaptive", "A step cap", &AdaptiveOrNumber,
                                          "is neither adaptive nor a number of milliseconds", rex::kDebugOnly};
};

void TestStoreAndRefuse() {
  Owner owner;
  rex::SettingBase& base = owner.cap;
  CHECK(owner.cap.Get() == "adaptive" && base.GetText() == "adaptive" && base.GetDefaultText() == "adaptive");
  CHECK(base.GetConstraints().debug_only);
  CHECK(base.GetType() == rex::SettingType::kString);
  CHECK(base.GetName() == "validated_setting_test.cap");

  const rex::u64 generation = base.GetGeneration();
  CHECK(base.SetText(rex::SettingLayer::kRuntime, "50"));
  CHECK(owner.cap.Get() == "50" && base.GetText() == "50" && base.GetLayer() == rex::SettingLayer::kRuntime);
  CHECK(base.GetGeneration() > generation);

  CHECK(!base.SetText(rex::SettingLayer::kRuntime, "fast"));  // refused by the validator
  CHECK(owner.cap.Get() == "50" && base.GetText() == "50");   // the value stays
  CHECK(!base.SetText(rex::SettingLayer::kRuntime, ""));
  CHECK(owner.cap.Get() == "50");

  CHECK(base.SetText(rex::SettingLayer::kRuntime, "adaptive"));
  CHECK(owner.cap.Get() == "adaptive");
}

void TestLayersAndReset() {
  Owner owner;
  rex::SettingBase& base = owner.cap;
  CHECK(base.SetText(rex::SettingLayer::kFile, "80"));
  CHECK(base.GetLayer() == rex::SettingLayer::kFile);
  CHECK(!base.SetText(rex::SettingLayer::kDefault, "90"));  // a lower layer does not overwrite a higher one
  CHECK(owner.cap.Get() == "80");
  CHECK(base.SetText(rex::SettingLayer::kRuntime, "90"));
  CHECK(owner.cap.Get() == "90");
  CHECK(base.Reset());
  CHECK(owner.cap.Get() == "adaptive" && base.GetLayer() == rex::SettingLayer::kDefault);
  CHECK(base.SetText(rex::SettingLayer::kLock, "70"));
  CHECK(!base.SetText(rex::SettingLayer::kRuntime, "60"));  // locked
  CHECK(owner.cap.Get() == "70");
}

void TestOnChangeAndBind() {
  Owner owner;
  rex::SettingBase& base = owner.cap;
  std::vector<std::string> changes;
  std::vector<std::string> bound;
  auto on_change = owner.cap.OnChange([&](const std::string& value) { changes.push_back(value); });
  auto bind = owner.cap.Bind([&](const std::string& value) { bound.push_back(value); });
  CHECK(changes.empty());                                  // OnChange does not call when it is registered
  CHECK(bound == std::vector<std::string>{"adaptive"});    // Bind does, with the current value

  CHECK(base.SetText(rex::SettingLayer::kRuntime, "40"));
  CHECK(changes == std::vector<std::string>{"40"} && bound.size() == 2 && bound[1] == "40");
  CHECK(!base.SetText(rex::SettingLayer::kRuntime, "bad"));  // a refused text is not a change
  CHECK(changes.size() == 1 && bound.size() == 2);
  CHECK(base.SetText(rex::SettingLayer::kRuntime, "40"));    // the same text is not a change either
  CHECK(changes.size() == 1 && bound.size() == 2);
  CHECK(base.Reset());
  CHECK(changes.size() == 2 && changes[1] == "adaptive");

  {
    const rex::SettingsBatch batch;  // two writes, one call each, with the last value
    CHECK(base.SetText(rex::SettingLayer::kRuntime, "10"));
    CHECK(base.SetText(rex::SettingLayer::kRuntime, "20"));
    CHECK(changes.size() == 2);  // held back until the batch ends
    CHECK(owner.cap.Get() == "20");
  }
  CHECK(changes.size() == 3 && changes[2] == "20");

  on_change.Reset();
  CHECK(base.SetText(rex::SettingLayer::kRuntime, "30"));
  CHECK(changes.size() == 3);  // the released guard is no longer called
  CHECK(bound.back() == "30");
}

// The exit crash for this class: a guard released after the setting is gone must not touch it.
void TestGuardOutlivesTheSetting() {
  int calls = 0;
  rex::ScopeGuard guard;
  {
    auto owner = std::make_unique<Owner>();
    guard = owner->cap.OnChange([&](const std::string&) { ++calls; });
    CHECK(owner->cap.SetText(rex::SettingLayer::kRuntime, "5"));
    CHECK(calls == 1);
  }
  guard.Reset();
  CHECK(calls == 1);
}

// A write made inside a change function stores at once and its call comes after the function returns (the SDK's rule), here for the
// button pattern LARecomp uses ("export_vinyl" flips itself back).
void TestWriteInsideTheFunction() {
  Owner owner;
  rex::SettingBase& base = owner.cap;
  std::vector<std::string> seen;
  auto guard = owner.cap.OnChange([&](const std::string& value) {
    seen.push_back(value);
    if (value == "99") {
      CHECK(base.SetText(rex::SettingLayer::kRuntime, "adaptive"));
      CHECK(owner.cap.Get() == "adaptive");  // stored at once
    }
  });
  CHECK(base.SetText(rex::SettingLayer::kRuntime, "99"));
  CHECK(seen == std::vector<std::string>({"99", "adaptive"}));
  CHECK(owner.cap.Get() == "adaptive");
}

}  // namespace

int main() {
  TestStoreAndRefuse();
  TestLayersAndReset();
  TestOnChangeAndBind();
  TestGuardOutlivesTheSetting();
  TestWriteInsideTheFunction();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
