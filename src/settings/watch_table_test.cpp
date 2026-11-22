// A check against the SDK's own settings (rex::Setting, rex::SettingOwner from rexbase): the table of change-callback guards
// (settings/watch_table.h) and the exit crash it once worked around. It needs no game, but it links rex::base, so it needs rexbase.dll
// next to it or on the PATH, and the SDK's operator new / delete forwarders (rexglue_link_host_allocator: a guard is allocated in
// rexbase.dll and freed here, which two different heaps would corrupt):
//   cmake --build <build dir> --target larecomp_watch_table_test, then run <build dir>/larecomp_watch_table_test (exit 0 = passed).
//
// The case of 2026-11-21: the exe's table of guards was destroyed at exit, after the SDK had destroyed the UI system whose settings
// (ui.resolution, ui.fullscreen) it watched, and a guard's release locked the mutex of a setting that no longer existed (an access
// violation in Mtx_lock about every other run). On rexglue 0.11.0 TestGuardOutlivesItsSetting crashes the same way. With the settings
// thread model the SDK detaches a watch when its setting is destroyed, so the release is harmless, and the workaround (a table that is
// never destroyed, released by hand before the SDK stops) is gone.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include <rex/setting.h>

#include "settings/watch_table.h"

using larecomp::settings::WatchTable;

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

// Stands for the SDK's UISettings: an owner of settings that a system makes and destroys.
class FakeUiSettings final : public rex::SettingOwner {
 public:
  FakeUiSettings() : SettingOwner("watch_table_test") {}
  REX_SETTING(std::string, resolution, "", "A text setting, as ui.resolution");
  REX_SETTING(bool, fullscreen, false, "A flag, as ui.fullscreen");
};

// The SDK's UISettings as the SDK holds it: in memory of its own, which is gone after its shutdown. A heap block that was freed stays
// mapped, and a guard that locks the mutex of a destroyed setting in it "works" (undefined behaviour that happens to pass): the crash of
// 2026-11-21 needed the memory to be gone. So the owner lives in a page of its own and Destroy() destroys it and makes the page
// inaccessible: a late touch is an access violation here too, on rexglue 0.11.0, and nothing on the settings thread model.
class DeadOwner {
 public:
  DeadOwner() {
    static_assert(sizeof(FakeUiSettings) <= kPage, "the owner has to fit its page");
#if defined(_WIN32)
    page_ = VirtualAlloc(nullptr, kPage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    page_ = mmap(nullptr, kPage, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page_ == MAP_FAILED) page_ = nullptr;
#endif
    if (page_) ui_ = new (page_) FakeUiSettings();
  }
  DeadOwner(const DeadOwner&) = delete;
  DeadOwner& operator=(const DeadOwner&) = delete;
  ~DeadOwner() {
    if (ui_) ui_->~FakeUiSettings();
    if (!page_) return;
#if defined(_WIN32)
    VirtualFree(page_, 0, MEM_RELEASE);
#else
    munmap(page_, kPage);
#endif
  }

  FakeUiSettings& Ui() { return *ui_; }
  bool Ok() const { return ui_ != nullptr; }

  // The SDK's shutdown: the settings are destroyed and their memory can no longer be touched.
  void Destroy() {
    ui_->~FakeUiSettings();
    ui_ = nullptr;
#if defined(_WIN32)
    DWORD old = 0;
    VirtualProtect(page_, kPage, PAGE_NOACCESS, &old);
#else
    mprotect(page_, kPage, PROT_NONE);
#endif
  }

 private:
  static constexpr size_t kPage = 16384;
  void* page_ = nullptr;
  FakeUiSettings* ui_ = nullptr;
};

void TestAddTakeAndSize() {
  FakeUiSettings ui;
  int resolution_calls = 0;
  int fullscreen_calls = 0;
  WatchTable<rex::ScopeGuard> table;
  table.Add("resolution", ui.resolution.OnChange([&](const std::string&) { ++resolution_calls; }));
  table.Add("resolution", ui.resolution.OnChange([&](const std::string&) { ++resolution_calls; }));
  table.Add("fullscreen", ui.fullscreen.OnChange([&](const bool&) { ++fullscreen_calls; }));
  CHECK(table.Size() == 3);
  CHECK(resolution_calls == 0 && fullscreen_calls == 0);  // OnChange does not call at registration

  CHECK(ui.resolution.Set("1920x1080"));
  CHECK(resolution_calls == 2 && fullscreen_calls == 0);  // one call per watch, after the change

  {
    std::vector<rex::ScopeGuard> taken = table.Take("resolution");  // UnregisterChangeCallbacks: only that setting's guards go
    CHECK(taken.size() == 2);
    CHECK(table.Size() == 1);
    CHECK(ui.resolution.Set("1280x720"));
    CHECK(resolution_calls == 4);  // taken is not released yet: the functions are still called
  }
  CHECK(ui.resolution.Set("1600x900"));
  CHECK(resolution_calls == 4);  // released: no more calls

  CHECK(table.Take("nothing").empty());  // a name that was never watched is not an error
  CHECK(table.Size() == 1);

  CHECK(ui.fullscreen.Set(true));
  CHECK(fullscreen_calls == 1);
  {
    std::vector<rex::ScopeGuard> all = table.TakeAll();  // ReleaseChangeCallbacks
    CHECK(all.size() == 1 && table.Size() == 0);
    CHECK(ui.fullscreen.Set(false));
    CHECK(fullscreen_calls == 2);
  }
  CHECK(ui.fullscreen.Set(true));
  CHECK(fullscreen_calls == 2);
}

// The exit crash: the SDK destroys the system and its settings first (rex::Stop), then the exe's statics go, the table among them.
// The guard's release must not touch the dead setting.
void TestGuardOutlivesItsSetting() {
  int calls = 0;
  {
    WatchTable<rex::ScopeGuard> table;  // the exe's table: destroyed after the SDK's system
    {
      DeadOwner sdk;  // the SDK's UISettings, owned by a system the SDK destroys at its shutdown
      CHECK(sdk.Ok());
      if (!sdk.Ok()) return;
      table.Add("resolution", sdk.Ui().resolution.OnChange([&](const std::string&) { ++calls; }));
      table.Add("fullscreen", sdk.Ui().fullscreen.OnChange([&](const bool&) { ++calls; }));
      CHECK(sdk.Ui().resolution.Set("2560x1440"));
      CHECK(calls == 1);
      sdk.Destroy();  // the SDK shuts down: the settings are gone, the guards are not
      CHECK(table.Size() == 2);
    }
  }  // the table is destroyed here, as the exe's statics are at exit: this crashed in Mtx_lock before the settings thread model
  CHECK(calls == 1);
}

// The same with the release by hand (UnregisterAll after the SDK has already stopped, or a path that releases late).
void TestReleaseAfterTheSettingIsGone() {
  int calls = 0;
  WatchTable<rex::ScopeGuard> table;
  {
    DeadOwner sdk;
    CHECK(sdk.Ok());
    if (!sdk.Ok()) return;
    table.Add("resolution", sdk.Ui().resolution.OnChange([&](const std::string&) { ++calls; }));
    sdk.Destroy();
  }
  {
    std::vector<rex::ScopeGuard> all = table.TakeAll();
  }
  CHECK(table.Size() == 0 && calls == 0);
}

// A table with guards on settings that are alive and settings that are not: both kinds are released, in any order.
void TestMixedLifetimes() {
  int calls = 0;
  FakeUiSettings alive;
  WatchTable<rex::ScopeGuard> table;
  table.Add("a", alive.resolution.OnChange([&](const std::string&) { ++calls; }));
  {
    DeadOwner dying;
    CHECK(dying.Ok());
    if (!dying.Ok()) return;
    table.Add("b", dying.Ui().resolution.OnChange([&](const std::string&) { ++calls; }));
    table.Add("a", dying.Ui().fullscreen.OnChange([&](const bool&) { ++calls; }));
    dying.Destroy();
  }
  CHECK(alive.resolution.Set("1x1"));
  CHECK(calls == 1);  // only the watch of the setting that is still there
  {
    std::vector<rex::ScopeGuard> all = table.TakeAll();
  }
  CHECK(alive.resolution.Set("2x2"));
  CHECK(calls == 1);
}

// Why Take and TakeAll hand the guards back: a guard's release waits for a call of its function that runs on another thread, so it must
// not be released under a lock that function takes. The function below holds up a call until the main thread has unlocked.
void TestReleaseOutsideTheLockWaitsForACall() {
  FakeUiSettings ui;
  std::mutex lock;  // g_cb_mutex
  WatchTable<rex::ScopeGuard> table;
  std::atomic<bool> in_call{false};
  std::atomic<bool> call_done{false};
  table.Add("resolution", ui.resolution.OnChange([&](const std::string&) {
    in_call = true;
    std::lock_guard<std::mutex> take(lock);  // the function asks for the table's lock
    call_done = true;
  }));
  std::thread writer([&] { (void)ui.resolution.Set("3840x2160"); });
  while (!in_call) std::this_thread::sleep_for(std::chrono::milliseconds(1));

  std::vector<rex::ScopeGuard> taken;
  {
    std::lock_guard<std::mutex> hold(lock);
    taken = table.TakeAll();  // under the lock the table is only emptied: nothing waits here
  }
  taken.clear();  // after the unlock: the release waits for the call, which can now take the lock
  CHECK(call_done.load());
  writer.join();
  CHECK(table.Size() == 0);
}

}  // namespace

int main() {
  TestAddTakeAndSize();
  TestGuardOutlivesItsSetting();
  TestReleaseAfterTheSettingIsGone();
  TestMixedLifetimes();
  TestReleaseOutsideTheLockWaitsForACall();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
