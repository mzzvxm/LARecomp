// MCLA GPU renderer -- the DENOISER setting: the word as the render thread reads it, the frame's snapshot for the pause menu's thread, and the board where the effects say they are on. See denoiser_setting.h.

#include "denoiser_setting.h"

#include <atomic>
#include <mutex>

#include "live_text.h"

namespace mcla::gpu::denoiser {

namespace {

LiveText& Text() {
  static LiveText text("mcla_gpu_denoiser");
  return text;
}

std::mutex g_mutex;
Snapshot g_snapshot;
std::atomic<uint32_t> g_consumers{0};

}  // namespace

rr::Denoiser Setting() { return Parse(Text().Get()); }

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
