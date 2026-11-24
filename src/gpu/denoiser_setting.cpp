// MCLA GPU renderer -- the denoiser setting: the effective word as the render thread reads it, the frame's snapshot for the pause menu's thread, and the board where the effects say they are on. See denoiser_setting.h.

#include "denoiser_setting.h"

#include <atomic>
#include <mutex>

#include "live_text.h"
#include "settings/larecomp_settings.h"

namespace mcla::gpu::denoiser {

namespace {

// The debug chooser (mcla_gpu_denoiser: row, auto, rr, ours), read as text by the render thread.
LiveText& Chooser() {
  static LiveText text("mcla_gpu_denoiser");
  return text;
}

std::mutex g_mutex;
Snapshot g_snapshot;
std::atomic<uint32_t> g_consumers{0};

}  // namespace

// The player's row is a bool setting (mcla_gpu_vendor_denoiser); the chooser, when it says auto, rr or ours, overrides it (Effective).
rr::Denoiser Setting() { return Effective(Chooser().Get(), larecomp::settings::mcla_gpu_vendor_denoiser.Get()); }

void Publish(const Snapshot& snapshot) {
  std::lock_guard lock(g_mutex);
  g_snapshot = snapshot;
  g_snapshot.valid = true;
}

Snapshot Current() {
  std::lock_guard lock(g_mutex);
  return g_snapshot;
}

void NoteConsumer(rr::Consumer consumer, bool on) {
  const uint32_t bit = rr::ConsumerBit(consumer);
  if (on) {
    g_consumers.fetch_or(bit, std::memory_order_relaxed);
  } else {
    g_consumers.fetch_and(~bit, std::memory_order_relaxed);
  }
}

uint32_t ConsumersOn() { return g_consumers.load(std::memory_order_relaxed); }

}  // namespace mcla::gpu::denoiser
