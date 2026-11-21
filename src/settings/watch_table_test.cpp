// A check that needs no game: the table of change-callback guards (settings/watch_table.h). Build:
// cmake --build <build dir> --target larecomp_watch_table_test, then run it; exit 0 = passed.
//
// The case of 2026-11-21: the exe's table of guards was destroyed at exit, after the SDK had destroyed the UI system whose settings
// (ui.resolution, ui.fullscreen) it watched, and a guard's release locked the mutex of a setting that no longer existed (an access
// violation in Mtx_lock about every other run). The fake guard below does what rex::ScopeGuard does for a watch (it runs its release
// once, when it is destroyed or reset), and the fake setting notes a release that arrives after the setting was destroyed.

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <utility>

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

// rex::ScopeGuard in miniature: move-only, the release runs once.
class FakeGuard {
 public:
  FakeGuard() = default;
  explicit FakeGuard(std::function<void()> release) : release_(std::move(release)) {}
  FakeGuard(FakeGuard&& other) noexcept : release_(std::move(other.release_)) { other.release_ = nullptr; }
  FakeGuard& operator=(FakeGuard&& other) noexcept {
    if (this != &other) {
      Run();
      release_ = std::move(other.release_);
      other.release_ = nullptr;
    }
    return *this;
  }
  FakeGuard(const FakeGuard&) = delete;
  FakeGuard& operator=(const FakeGuard&) = delete;
  ~FakeGuard() { Run(); }

 private:
  void Run() {
    if (!release_) return;
    auto release = std::move(release_);
    release_ = nullptr;
    release();
  }
  std::function<void()> release_;
};

// A setting that is watched: the guard's release removes the watch, which needs the setting (its mutex, in the real one).
struct FakeSetting {
  int watches = 0;
  bool destroyed = false;
};

// The status of the settings the releases reach: a release that finds its setting destroyed is the crash.
struct World {
  int released_alive = 0;
  int released_dead = 0;
};

FakeGuard Watch(World& world, FakeSetting& setting) {
  ++setting.watches;
  return FakeGuard([&world, &setting] {
    if (setting.destroyed) {
      ++world.released_dead;
    } else {
      --setting.watches;
      ++world.released_alive;
    }
  });
}

void TestAddEraseAndSize() {
  World world;
  FakeSetting a, b;
  WatchTable<FakeGuard> table;
  table.Add("a", Watch(world, a));
  table.Add("a", Watch(world, a));
  table.Add("b", Watch(world, b));
  CHECK(table.Size() == 3);
  CHECK(a.watches == 2 && b.watches == 1);

  table.Erase("a");  // UnregisterChangeCallbacks: only that setting's guards go
  CHECK(table.Size() == 1);
  CHECK(a.watches == 0 && b.watches == 1);
  CHECK(world.released_alive == 2 && world.released_dead == 0);

  table.Erase("nothing");  // a name that was never watched is not an error
  CHECK(table.Size() == 1);

  table.ReleaseAll();
  CHECK(table.Size() == 0);
  CHECK(b.watches == 0);
  CHECK(world.released_alive == 3 && world.released_dead == 0);
}

// The fix: the guards are released while the settings are alive, so the table's own destruction (static destruction at exit) finds
// nothing to release, whatever was destroyed in between.
void TestReleasedBeforeTheSettingsGo() {
  World world;
  auto owner = std::make_unique<FakeSetting>();  // the SDK's UISettings, owned by a system the SDK destroys at its shutdown
  {
    WatchTable<FakeGuard> table;
    table.Add("resolution", Watch(world, *owner));
    table.Add("fullscreen", Watch(world, *owner));
    table.ReleaseAll();  // UnregisterAll, in LarecompApp::OnShutdown: the systems are still there
    CHECK(owner->watches == 0);
    owner->destroyed = true;  // the SDK shuts down
    owner.reset();
  }  // the table is destroyed here, as the exe's statics are at exit
  CHECK(world.released_alive == 2);
  CHECK(world.released_dead == 0);
}

// What it was before: nothing releases the table by hand, so its destruction releases guards of a setting that is gone.
void TestTheOldOrderTouchesDeadSettings() {
  World world;
  FakeSetting setting;
  {
    WatchTable<FakeGuard> table;
    table.Add("resolution", Watch(world, setting));
    setting.destroyed = true;  // the SDK shuts down first
  }  // then the static destruction
  CHECK(world.released_dead == 1);
}

// A table that is never destroyed (the one in larecomp_settings.cpp) leaves its guards alone at exit, so a path that never reaches
// ReleaseAll cannot reach a dead setting either.
void TestNeverDestroyedTableTouchesNothing() {
  World world;
  FakeSetting setting;
  auto* table = new WatchTable<FakeGuard>();  // leaked on purpose, like Guards()
  table->Add("resolution", Watch(world, setting));
  setting.destroyed = true;
  CHECK(world.released_dead == 0 && world.released_alive == 0);
  CHECK(table->Size() == 1);
}

// Guards added again after a release (a watch asked for later) are held and released like the first ones.
void TestUsableAfterReleaseAll() {
  World world;
  FakeSetting setting;
  WatchTable<FakeGuard> table;
  table.Add("x", Watch(world, setting));
  table.ReleaseAll();
  table.Add("x", Watch(world, setting));
  CHECK(table.Size() == 1 && setting.watches == 1);
  table.ReleaseAll();
  CHECK(setting.watches == 0 && world.released_alive == 2);
}

}  // namespace

int main() {
  TestAddEraseAndSize();
  TestReleasedBeforeTheSettingsGo();
  TestTheOldOrderTouchesDeadSettings();
  TestNeverDestroyedTableTouchesNothing();
  TestUsableAfterReleaseAll();
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
