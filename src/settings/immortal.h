#pragma once
// A value made on first use and never destroyed, for state that a thread of the exe can still read while the process ends.
//
// The case of 2026-11-21 (run neon1-ab-vk-g2, one in about 1400): the exe crashed two milliseconds AFTER its "Process exiting
// normally" line, in larecomp::settings::Find, reading a hash bucket through a null bucket vector. Find looks a setting up by name in
// a table that was a function-local `static State s`, so the exe's static destruction destroyed it while a thread that is not the
// main one still called Find. The crash report was cut off before it named that thread (it never logged); the likely one is a guest
// thread: the SDK's TerminateTitle gives the guest threads 200 ms to leave and then
// "deliberately leaves the stragglers running, never force-killed" (system/kernel_state.cpp), so the game's hooks (rubber band, world
// tuning, the developer options: each reads settings by name every frame) can keep calling into the exe until ExitProcess ends them,
// past the destructors of every static the exe made after its first line of start-up.
//
// So a table those hooks read is not destroyed: the object lives until the process is gone, and a late reader finds it (emptied by the
// owner's shutdown, if it has one) instead of freed memory. The cost is one allocation the OS reclaims at exit. Use it as
//
//     State& S() { static larecomp::Immortal<State> s; return *s; }
//
// Immortal itself has no destructor to run (value_ is a plain pointer), so the compiler registers nothing at exit for it either.
// Do NOT use it for an object whose destructor does work the process needs (a file that must be flushed, a thread that must be
// joined): its destructor will not run.

#include <utility>

namespace larecomp {

template <typename T>
class Immortal {
 public:
  template <typename... Args>
  explicit Immortal(Args&&... args) : value_(new T(std::forward<Args>(args)...)) {}
  Immortal(const Immortal&) = delete;
  Immortal& operator=(const Immortal&) = delete;

  T& operator*() const { return *value_; }
  T* operator->() const { return value_; }

 private:
  T* value_;  // never deleted
};

}  // namespace larecomp
