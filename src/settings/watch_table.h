#pragma once
// The change-callback guards LARecomp holds for the settings it watches (larecomp::cvar::RegisterChangeCallback), one list per
// setting. A guard's release takes the watched setting's own mutex, so a guard must be released while that setting still exists.
// Some of the watched settings are the SDK's (ui.fullscreen, ui.resolution, ...) and belong to systems the SDK destroys at its
// shutdown, BEFORE the exe's static destruction: a table that is simply destroyed at exit then releases guards of settings that
// are gone (an access violation in Mtx_lock at exit, about every other run on the 0.11 SDK). So the table is released by hand
// (ReleaseAll) while everything is alive, and the one in larecomp_settings.cpp is never destroyed.
//
// No locking of its own: the caller serialises (larecomp_settings.cpp holds g_cb_mutex around every call).
// `Guard` is a move-only handle whose destructor removes the watch (rex::ScopeGuard); a default-constructed one releases nothing.

#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace larecomp::settings {

template <typename Guard>
class WatchTable {
 public:
  void Add(const std::string& key, Guard guard) { guards_[key].push_back(std::move(guard)); }

  // Releases the guards of one setting (UnregisterChangeCallbacks).
  void Erase(const std::string& key) { guards_.erase(key); }

  // Releases every guard now. Call it while the watched settings are alive (LarecompApp::OnShutdown, through UnregisterAll).
  void ReleaseAll() { guards_.clear(); }

  size_t Size() const {
    size_t n = 0;
    for (const auto& entry : guards_) n += entry.second.size();
    return n;
  }

 private:
  std::map<std::string, std::vector<Guard>> guards_;
};

}  // namespace larecomp::settings
