#pragma once
//
// The pure half of the device watcher (device_watch.h): what a HID interface's symbolic link says
// about the device, the table of present links that arrival and removal notifications keep
// current, and the retry schedule for something that failed. No OS calls and no threads in here, so
// device_watch_test.cpp compiles it alone and runs it without a game.
//

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace larecomp::devwatch {

// A symbolic link, lower-cased (the links are ASCII: a device path, hex ids and a class guid).
inline std::wstring LowerLink(std::wstring link) {
    for (wchar_t& c : link) {
        if (c >= L'A' && c <= L'Z') c = wchar_t(c - L'A' + L'a');
    }
    return link;
}

struct LinkInfo {
    bool sony = false;       // a Sony HID interface (vendor 054C), over USB or Bluetooth
    bool dualsense = false;  // a DualSense or DualSense Edge
    bool bluetooth = false;  // named for the HID service ({0000...}): the 78-byte output report
};

// What a lower-case HID interface link says, from the name alone (no I/O).
//   USB:       \\?\hid#vid_054c&pid_0ce6&mi_03#...
//   Bluetooth: \\?\hid#{00001124-0000-1000-8000-00805f9b34fb}_vid&0002054c_pid&0ce6#...
inline LinkInfo ClassifyLink(const std::wstring& link) {
    LinkInfo info;
    info.sony = link.find(L"vid_054c") != std::wstring::npos || link.find(L"vid&0002054c") != std::wstring::npos;
    // 0CE6 = DualSense, 0DF2 = DualSense Edge
    info.dualsense = info.sony && (link.find(L"0ce6") != std::wstring::npos || link.find(L"0df2") != std::wstring::npos);
    info.bluetooth = link.find(L"#{0000") != std::wstring::npos;
    return info;
}

// The HID interfaces that are present, kept current by notifications. A notification can arrive
// before the first full list has been taken (the watcher registers first, then lists, so that
// nothing falls between the two); such events are held and replayed over the list in the order they
// came, which gives the right answer whichever side of the list each one happened on: the last
// event for a link is the truth after it, and a link with no event is the list's.
// Not thread-safe: the caller holds its own lock.
class DeviceTable {
 public:
    struct Summary {
        bool sony = false;                    // any Sony HID interface
        std::vector<std::wstring> dualsense;  // the DualSense interfaces, sorted
        bool operator==(const Summary& o) const { return sony == o.sony && dualsense == o.dualsense; }
        bool operator!=(const Summary& o) const { return !(*this == o); }
    };

    // True if the summary changed (a list that makes the table ready counts as a change).
    bool Arrival(const std::wstring& link) { return Event(true, link); }
    bool Removal(const std::wstring& link) { return Event(false, link); }

    bool ApplySnapshot(const std::vector<std::wstring>& links) {
        present_.clear();
        for (const std::wstring& link : links) present_.insert(LowerLink(link));
        const bool first = !ready_;
        ready_ = true;
        for (const auto& event : pending_) {
            if (event.first) {
                present_.insert(event.second);
            } else {
                present_.erase(event.second);
            }
        }
        pending_.clear();
        const bool changed = Recompute();
        if (first || changed) ++generation_;
        return first || changed;
    }

    bool ready() const { return ready_; }
    const Summary& summary() const { return summary_; }
    // Moves on every change of the summary and when the first list lands.
    uint32_t generation() const { return generation_; }
    // How many HID interfaces are present (what the list and the events add up to).
    size_t count() const { return present_.size(); }

 private:
    bool Event(bool arrival, const std::wstring& link) {
        std::wstring lowered = LowerLink(link);
        if (!ready_) {
            pending_.emplace_back(arrival, std::move(lowered));
            return false;
        }
        if (arrival) {
            present_.insert(std::move(lowered));
        } else {
            present_.erase(lowered);
        }
        const bool changed = Recompute();
        if (changed) ++generation_;
        return changed;
    }

    bool Recompute() {
        Summary next;
        for (const std::wstring& link : present_) {  // a std::set: already sorted
            const LinkInfo info = ClassifyLink(link);
            if (info.sony) next.sony = true;
            if (info.dualsense) next.dualsense.push_back(link);
        }
        if (next == summary_) return false;
        summary_ = std::move(next);
        return true;
    }

    std::set<std::wstring> present_;
    std::vector<std::pair<bool, std::wstring>> pending_;  // (arrival, link) before the first list
    Summary summary_;
    bool ready_ = false;
    uint32_t generation_ = 0;
};

// When to try again after something failed (opening a pad). A key may fail `max_tries` times, each
// retry due `delay_ms` after the failure; after that it is left alone until Clear(key) (a device
// change). All times are milliseconds on one clock chosen by the caller.
class RetryBackoff {
 public:
    explicit RetryBackoff(int max_tries = 3) : max_tries_(max_tries) {}

    bool Allowed(const std::wstring& key, int64_t now_ms) const {
        const auto it = entries_.find(key);
        if (it == entries_.end()) return true;
        return it->second.failures < max_tries_ && now_ms >= it->second.due_ms;
    }
    void Failed(const std::wstring& key, int64_t now_ms, int64_t delay_ms) {
        Entry& entry = entries_[key];
        ++entry.failures;
        entry.due_ms = now_ms + delay_ms;
    }
    void Clear(const std::wstring& key) { entries_.erase(key); }
    void ClearAll() { entries_.clear(); }
    // Keeps only the entries of the keys in `present` (a pad that left has nothing left to retry).
    void Retain(const std::vector<std::wstring>& present) {
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (std::find(present.begin(), present.end(), it->first) == present.end()) {
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Milliseconds until the soonest retry that is still allowed (0 = due now); -1 = nothing waits.
    int64_t NextDueMs(int64_t now_ms) const {
        int64_t soonest = -1;
        for (const auto& entry : entries_) {
            if (entry.second.failures >= max_tries_) continue;
            const int64_t wait = std::max<int64_t>(0, entry.second.due_ms - now_ms);
            if (soonest < 0 || wait < soonest) soonest = wait;
        }
        return soonest;
    }
    int Failures(const std::wstring& key) const {
        const auto it = entries_.find(key);
        return it == entries_.end() ? 0 : it->second.failures;
    }

 private:
    struct Entry {
        int failures = 0;
        int64_t due_ms = 0;
    };
    std::map<std::wstring, Entry> entries_;
    int max_tries_;
};

}  // namespace larecomp::devwatch
