#pragma once
// The scene file the runner was asked for (the value of mcla_scene), kept so the game thread reads it with no lock and no allocation.
// Header only and free of the game: scene_want_test.cpp checks it with a fake settings registry.
//
// The setting's string is written by whoever edits it (the F4 panel, the console, a tour's `set`: SetFlagByName, under the settings
// registry's lock) and read by the game thread (TickScene) and by the thread that runs the camera hook (CurrentSceneState). A
// std::string read while it is assigned is a data race, so nothing here reads the setting's own storage:
//   - the setting's change callback (it runs on the writer's thread, under the registry's lock) hands every new value to Store, which
//     copies it under a mutex and bumps a generation;
//   - a value that arrives with no write through SetFlagByName (the command line, larecomp.toml) is read once, by the settings' own
//     locked getter, the first time anyone asks (on a guest thread, long after the command line and the file were read);
//   - the game thread keeps a copy of its own and takes the lock only when the generation moved. With no scene and no edit a tick is
//     two atomic loads and an emptiness test: no lock, no allocation. IsSet reads one atomic.
//
// THE CALLBACK IS HOOKED UP ON FIRST USE, NEVER FROM A STATIC INITIALIZER. On 0.10 the setting existed from static initialization (the
// REXCVAR_DEFINE of the file) and the callback was added by a namespace-scope initializer. On 0.11 the settings are registered by
// RegisterAll in OnConfigure, long after static initialization: a callback asked for earlier finds no setting and is dropped, and a
// scene armed at run time (a tour's `set mcla_scene`, the F4 panel) never started, while one named in larecomp.toml or on the command
// line did (the first read finds the value). If the watch is refused anyway, the value is read again on every ask (slower, right).

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace mc::cinematic {

class SceneWant {
public:
    using Notify = std::function<void(std::string_view value)>;
    struct Source {
        // Hooks `notify` up to the setting's changes; false when it could not be (no such setting yet). Called once, on first use.
        std::function<bool(Notify notify)> watch;
        // The setting's text now (the settings' own locked getter). Any thread; none of this class's locks is held.
        std::function<std::string()> read;
    };

    explicit SceneWant(Source source) : source_(std::move(source)) {}  // touches nothing: safe as a static

    // The first ask: hooks the watch up, THEN reads, so a change that lands between the two is not lost. Any thread.
    void EnsureKnown() {
        if (seeded_.load(std::memory_order_acquire)) return;
        std::call_once(attached_, [this] { watched_ = source_.watch ? source_.watch([this](std::string_view v) { Store(v); }) : false; });
        const std::string value = source_.read ? source_.read() : std::string();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!known_) StoreLocked(value);  // a callback that ran meanwhile holds a newer value
        }
        seeded_.store(true, std::memory_order_release);
    }

    // The scene asked for, as of this ask. The game thread only; the reference holds until the next call.
    const std::string& Wanted() {
        EnsureKnown();
        Refresh();
        if (gen_.load(std::memory_order_acquire) != mine_gen_) {
            std::lock_guard<std::mutex> lock(mutex_);
            mine_ = text_;
            mine_gen_ = gen_.load(std::memory_order_relaxed);
        }
        return mine_;
    }

    // Is a scene asked for? Any thread.
    bool IsSet() {
        EnsureKnown();
        Refresh();
        return set_.load(std::memory_order_acquire);
    }

    // True when the change notification is hooked up (false: every ask reads the setting). After the first ask.
    bool Watched() const { return watched_; }

    // The change callback's side, on the writer's thread.
    void Store(std::string_view value) {
        std::lock_guard<std::mutex> lock(mutex_);
        StoreLocked(value);
    }

private:
    void StoreLocked(std::string_view value) {
        text_.assign(value);
        known_ = true;
        set_.store(!value.empty(), std::memory_order_release);
        gen_.fetch_add(1, std::memory_order_release);
    }

    // With no watch there is nothing to tell a change, so look every time (after the first ask, which has read once).
    void Refresh() {
        if (watched_ || !source_.read) return;
        const std::string value = source_.read();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!known_ || value != text_) StoreLocked(value);
    }

    Source source_;
    std::once_flag attached_;
    bool watched_ = false;                 // written once inside attached_, read after it
    std::mutex mutex_;
    std::string text_;                     // guarded by mutex_
    bool known_ = false;                   // guarded by mutex_: text_ holds a value (from the callback or the first read)
    std::atomic<uint32_t> gen_{0};         // bumped under mutex_ with every new value
    std::atomic<bool> set_{false};         // the value is not empty
    std::atomic<bool> seeded_{false};      // the first read is done
    std::string mine_;                     // the game thread's own copy (Wanted only)
    uint32_t mine_gen_ = 0;
};

}  // namespace mc::cinematic
