#pragma once
// ===========================================================================
// MCLA GPU renderer -- a short string cvar, as the render thread reads it
// ===========================================================================
// The graphics upgrades' settings are string cvars (auto | game | high | ultra | off | low) that the pause
// menu's thread writes (rex::cvar::SetFlagByName) while the render thread reads them every frame, and in
// the side views' case at every draw. REXCVAR_GET of a string is the plain std::string that the writer
// assigns, with no lock: a read during the write can tear (for these short words it gives a wrong word
// for a frame, not a crash, but it is a data race all the same).
//
// A LiveText is the one place the render thread reads instead. It keeps the cvar's last value in one
// atomic word, set by the cvar's change callback (which has the new text, so nobody reads the cvar's
// string while it is being written), and filled the first time it is asked from the registry's own
// locked accessor (rex::cvar::GetFlagByName). The gpu_class.cpp override and shadow_filter.cpp do the same
// with an enum; these settings are read in several places as text, so the text is what is kept.
//
// The word holds the address of an interned copy of the text, so any length works (it once held 7 characters,
// and the sun shafts' `stronger` and `strongest` came out as `stronge`, which no setting knows).
//
// A change that does not go through SetFlagByName (REXCVAR_SET, rex::cvar::ResetToDefault) does not run
// the callback and would not show; the settings here are only written through SetFlagByName (the pause
// menu's CvarSet, the debug menu's reset), the way shadow_filter.cpp relies on already.
// ===========================================================================

#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

#include <rex/cvar.h>

namespace mcla::gpu {

class LiveText {
 public:
  // `cvar` names a string cvar and outlives this object (a string literal). constexpr: a namespace-scope
  // LiveText has no start-up order to get wrong.
  constexpr explicit LiveText(const char* cvar) : cvar_(cvar) {}
  LiveText(const LiveText&) = delete;
  LiveText& operator=(const LiveText&) = delete;

  // The cvar's current text. Any thread; the first call registers the callback (a few microseconds).
  std::string Get() {
    uint64_t word = word_.load(std::memory_order_acquire);
    if (word == 0) {
      Register();
      word = word_.load(std::memory_order_acquire);
    }
    return Unpack(word);
  }

  // The packing: the word is the address of the text's interned copy (a node of a set that only grows and is
  // never freed: the settings have a handful of values, so this is a few dozen bytes for the whole run). Any
  // length works ("stronger", "strongest"); an interned text never changes, so Unpack reads it without a lock.
  static uint64_t Pack(std::string_view text) {
    static std::mutex lock;
    static auto* interned = new std::set<std::string, std::less<>>();
    std::lock_guard<std::mutex> guard(lock);
    auto it = interned->find(text);
    if (it == interned->end()) {
      it = interned->emplace(text).first;
    }
    return reinterpret_cast<uint64_t>(&*it);  // never 0: 0 is "not read yet"
  }
  static std::string Unpack(uint64_t word) {
    return word == 0 ? std::string() : *reinterpret_cast<const std::string*>(word);
  }

 private:

  void Register() {
    std::call_once(once_, [this] {
      rex::cvar::RegisterChangeCallback(cvar_, [this](std::string_view, std::string_view value) {
        word_.store(Pack(value), std::memory_order_release);
      });
      // After the callback is in: a change from here on is not missed. A change that came between the two
      // lines already stored its value, which is the newer one (compare-exchange from "not read yet").
      uint64_t expected = 0;
      word_.compare_exchange_strong(expected, Pack(rex::cvar::GetFlagByName(cvar_)),
                                    std::memory_order_acq_rel);
    });
  }

  const char* cvar_;
  std::atomic<uint64_t> word_{0};
  std::once_flag once_;
};

}  // namespace mcla::gpu
