// MCLA GPU renderer -- synchronous builds while capturing. See sync_builds.h.

#include "sync_builds.h"

#include <atomic>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    mcla_gpu_sync_builds, false, "MCLA/GPU",
    "Trailer toolkit (M2d): while on, nothing is left out for a pipeline that is still being made. "
    "A draw whose pipeline a builder thread has not finished (\"skipped [pipeline pending ...]\" in "
    "the scene lines), and the second draws of the HDR scene and the upscaler masks, wait for it "
    "on the render thread instead, so no frame of a recording misses an object. The frames that "
    "make a new pipeline are slow while it is on. The log counts the waits and their time "
    "(\"[gpu] capture sync\"). Capture mode turns the same switch on by itself while it records. "
    "Off: as before. Live.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gpu::sync_builds {

namespace {

std::atomic<bool> g_call_flag{false};
std::mutex g_mutex;
Books g_books;                         // g_mutex
std::atomic<bool> g_logged_on{false};  // the state the last EndFrame saw (the render thread's)
bool g_ever_on = false;                // g_mutex: the flag was on at some frame

}  // namespace

void SetCaptureActive(bool active) {
  g_call_flag.store(active, std::memory_order_release);
}

bool CaptureActive() {
  return g_call_flag.load(std::memory_order_acquire) || REXCVAR_GET(mcla_gpu_sync_builds);
}

bool NoteWait(Kind kind, uint64_t ns) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_books.Note(kind, ns);
}

void EndFrame() {
  const bool on = CaptureActive();
  if (on == g_logged_on.load(std::memory_order_relaxed)) {
    return;  // the usual frame: no lock
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  g_logged_on.store(on, std::memory_order_relaxed);
  if (on) {
    g_ever_on = true;
    REXLOG_INFO("[gpu] capture sync: on ({}): a draw whose pipeline is still being made waits for it "
                "on the render thread instead of being left out",
                g_call_flag.load(std::memory_order_acquire) ? "capture mode" : "mcla_gpu_sync_builds");
  } else {
    const Totals& s = g_books.session();
    REXLOG_INFO("[gpu] capture sync: off; {} wait(s), {:.1f} ms in this session so far", s.Waits(),
                double(s.WaitNs()) / 1.0e6);
  }
}

void LogInterval(const char* why) {
  const bool on = CaptureActive();
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_ever_on && !on && g_books.session().Waits() == 0) {
    return;
  }
  const Totals window = g_books.Take();
  REXLOG_INFO("{}", FormatLine(why, on, window, g_books.session()));
}

}  // namespace mcla::gpu::sync_builds
