#pragma once
// The change-callback guards LARecomp holds for the settings it watches (larecomp::cvar::RegisterChangeCallback), one list per
// setting.
//
// Two rules, both from the SDK's settings thread model (rex::Setting<T>::OnChange):
//   * A guard's release waits for a call of its function that is running on another thread. A guard is therefore never released
//     under a lock the function could take (g_cb_mutex in larecomp_settings.cpp): Take and TakeAll hand the guards back so the
//     caller lets go of them after it has unlocked.
//   * A guard released after its setting is gone is safe: the SDK detaches every watch of a setting when the setting is destroyed
//     (SettingBase::~SettingBase), and the guard then only drops its reference. Some of the watched settings are the SDK's
//     (ui.fullscreen, ui.resolution, ...) and belong to systems the SDK destroys at its shutdown, BEFORE the exe's static
//     destruction. Up to rexglue 0.11.0 the release locked the setting's own mutex, so this table had to be released by hand
//     while the settings were alive and never destroyed (the exit crash in Mtx_lock of 2026-11-21); that is not needed any more.
//     The table is released in UnregisterAll all the same (LarecompApp::OnShutdown), so that no callback runs into LARecomp code
//     that is being torn down; larecomp_watch_table_test checks both rules against the SDK itself.
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

  // Takes the guards of one setting out of the table (UnregisterChangeCallbacks). The caller releases them, outside its lock.
  std::vector<Guard> Take(const std::string& key) {
    std::vector<Guard> out;
    if (const auto it = guards_.find(key); it != guards_.end()) {
      out = std::move(it->second);
      guards_.erase(it);
    }
    return out;
  }

  // Takes every guard out of the table (UnregisterAll, through ReleaseChangeCallbacks). The caller releases them, outside its lock.
  std::vector<Guard> TakeAll() {
    std::vector<Guard> out;
    for (auto& entry : guards_) {
      for (Guard& guard : entry.second) out.push_back(std::move(guard));
    }
    guards_.clear();
    return out;
  }

  size_t Size() const {
    size_t n = 0;
    for (const auto& entry : guards_) n += entry.second.size();
    return n;
  }

 private:
  std::map<std::string, std::vector<Guard>> guards_;
};

}  // namespace larecomp::settings
