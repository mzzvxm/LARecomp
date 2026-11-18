#pragma once
//
// What the DualSense effect worker (adaptive_triggers.cpp) is built from, with no OS calls so
// device_watch_test.cpp runs it without a game:
//   LatestMailbox<T>  the game thread keeps the newest wish in it and never waits; the worker
//                     takes the newest one when it gets to it (latest wins, older ones are gone).
//   DecideWrite       whether a pad is written now, later (a vibration's pacing) or not at all.
//

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace larecomp::triggers {

// One slot with a wake-up. Post() holds the mutex for the length of a copy of T, so the poster
// (the game's thread) is never held up by the worker's I/O: the worker does not hold the mutex
// while it works. T must be copyable.
template <class T>
class LatestMailbox {
 public:
    // Replaces the pending value (a value the worker has not taken yet is dropped).
    void Post(const T& value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            value_ = value;
            has_value_ = true;
            ++posted_;
        }
        cv_.notify_one();
    }

    // Wakes the worker without a new value (a device came or went).
    void Wake() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            woken_ = true;
        }
        cv_.notify_one();
    }

    // Ends the waiting for good: every Wait() from now on returns at once with `closed`.
    void Close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    struct Result {
        bool new_value = false;  // Post() was called since the last Wait()
        bool woken = false;      // Wake() was called since the last Wait()
        bool closed = false;
        bool timed_out = false;  // nothing happened within the timeout
    };

    // The worker's wait: until a new value, a Wake(), Close() or the timeout (negative = no
    // timeout). `latest` is set to the newest value ever posted (unchanged while none was).
    Result Wait(T& latest, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto ready = [&] { return closed_ || woken_ || posted_ != taken_; };
        bool signalled = true;
        if (timeout.count() < 0) {
            cv_.wait(lock, ready);
        } else {
            signalled = cv_.wait_for(lock, timeout, ready);
        }
        Result result;
        result.closed = closed_;
        result.timed_out = !signalled;
        result.woken = woken_;
        woken_ = false;
        if (posted_ != taken_) {
            result.new_value = true;
            taken_ = posted_;
        }
        if (has_value_) latest = value_;
        return result;
    }

 private:
    std::mutex mutex_;
    std::condition_variable cv_;
    T value_{};
    bool has_value_ = false;
    bool woken_ = false;
    bool closed_ = false;
    uint64_t posted_ = 0;
    uint64_t taken_ = 0;
};

enum class WriteAction { kNothing, kNow, kLater };

struct WriteDecision {
    WriteAction action = WriteAction::kNothing;
    int64_t wait_ms = 0;  // kLater: how long until the write is allowed
};

// A pad is written when something changed, except that a vibration's strength follows the slip at
// most every `step_ms`: it waits for the rest of the step (a later change that is not a vibration,
// such as the vibration ending, comes with vibrating = false and is never held back).
inline WriteDecision DecideWrite(bool sent, bool same_as_pad, bool vibrating, int64_t written_ms, int64_t now_ms,
                                 int64_t step_ms) {
    if (sent && same_as_pad) return {WriteAction::kNothing, 0};
    if (vibrating && sent && now_ms - written_ms < step_ms) {
        return {WriteAction::kLater, step_ms - (now_ms - written_ms)};
    }
    return {WriteAction::kNow, 0};
}

}  // namespace larecomp::triggers
