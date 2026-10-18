// MCLA GPU renderer -- diagnostic watch of one guest memory range. See watch.h.

#include "watch.h"

#include <charconv>
#include <deque>
#include <format>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "textures.h"

REXCVAR_DEFINE_STRING(
    mcla_gpu_watch_base, "", "MCLA/GPU",
    "Diagnostic: a guest physical address in hex. Every resolve into, product "
    "activated or deactivated over, texture fetched from and resolve-memory decode of "
    "[base, base + mcla_gpu_watch_size) is logged in order with the guest frame and draw: the "
    "last 1500 events are kept until the first decode of the range, then written out with "
    "every later one (at most 20000 lines). Empty: off.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(mcla_gpu_watch_size, "400000", "MCLA/GPU",
                      "Diagnostic: the length, in hex, of mcla_gpu_watch_base's range.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_UINT32(
    mcla_gpu_watch_capture, 1, "MCLA/GPU",
    "Diagnostic, with mcla_gpu_watch_base: a RenderDoc capture of the frame after 1 = the first "
    "decode of the range from the resolve memory, 2 = the first product over it deactivated by "
    "another resolve. 0: none. Needs the game started under RenderDoc.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace mcla::gpu::watch {

namespace {

// Lines are kept in a ring until the first decode of the range (what led to
// it), then written out with everything after, up to kMaxLines.
constexpr uint32_t kRingLines = 1500;
constexpr uint32_t kMaxLines = 20000;
std::deque<std::string> g_ring;
bool g_flushed = false;

bool g_active = false;
uint32_t g_base = 0;
uint32_t g_size = 0;
uint64_t g_frame = 0;
uint32_t g_draws = 0;  // draws recorded in the current frame so far
uint64_t g_ps = 0;     // the current draw's pixel shader identity
uint32_t g_lines = 0;
uint32_t g_capture_moment = 0;
bool g_captured = false;
std::function<void()> g_capture;

bool ParseHex(std::string text, uint32_t* out) {
  if (text.starts_with("0x") || text.starts_with("0X")) {
    text.erase(0, 2);
  }
  if (text.empty()) {
    return false;
  }
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), *out, 16);
  return ec == std::errc() && end == text.data() + text.size();
}

}  // namespace

void Configure() {
  g_active = false;
  const std::string base = REXCVAR_GET(mcla_gpu_watch_base);
  if (base.empty()) {
    return;
  }
  const std::string size = REXCVAR_GET(mcla_gpu_watch_size);
  uint32_t b = 0;
  uint32_t s = 0;
  if (!ParseHex(base, &b) || !ParseHex(size, &s) || s == 0) {
    REXLOG_ERROR("[gpu] watch: mcla_gpu_watch_base '{}' / mcla_gpu_watch_size '{}' is not a hex "
                 "address and length; the watch stays off",
                 base, size);
    return;
  }
  g_base = b;
  g_size = s;
  g_capture_moment = REXCVAR_GET(mcla_gpu_watch_capture);
  g_active = true;
  REXLOG_INFO("[gpu] watch: guest {:08x}..{:08x}, every event in order{}", g_base,
              uint64_t(g_base) + g_size,
              g_capture_moment == 1   ? "; RenderDoc capture after its first decode"
              : g_capture_moment == 2 ? "; RenderDoc capture after its first deactivation"
                                      : "");
}

bool Active() { return g_active; }

bool Touches(uint32_t start, uint32_t length) {
  return g_active && length != 0 && uint64_t(start) < uint64_t(g_base) + g_size &&
         uint64_t(g_base) < uint64_t(start) + length;
}

bool TouchesKey(const TextureKey& key) {
  if (!g_active) {
    return false;
  }
  uint32_t ranges[2][2];
  TextureGuestRanges(key, ranges);
  return Touches(ranges[0][0], ranges[0][1]) || Touches(ranges[1][0], ranges[1][1]);
}

void BeginFrame(uint64_t serial) {
  g_frame = serial;
  g_draws = 0;
  g_ps = 0;
}

void OnDraw(uint64_t ps_identity) {
  ++g_draws;
  g_ps = ps_identity;
}

uint64_t CurrentPs() { return g_ps; }

void Log(const std::string& text) {
  if (!g_active || g_lines >= kMaxLines) {
    return;
  }
  std::string line = std::format("f{} d{}: {}", g_frame, g_draws, text);
  if (!g_flushed) {
    g_ring.push_back(std::move(line));
    if (g_ring.size() > kRingLines) {
      g_ring.pop_front();
    }
    return;
  }
  ++g_lines;
  REXLOG_INFO("[gpu] watch {}{}", line,
              g_lines == kMaxLines ? " (line limit reached: the watch logs nothing more)" : "");
}

void Flush(const char* why) {
  if (!g_active || g_flushed) {
    return;
  }
  g_flushed = true;
  REXLOG_INFO("[gpu] watch: {} -- the last {} events before it, then every one after", why,
              g_ring.size());
  for (const std::string& line : g_ring) {
    REXLOG_INFO("[gpu] watch {}", line);
  }
  g_ring.clear();
}

void SetCaptureRequest(std::function<void()> request) { g_capture = std::move(request); }

void MaybeCapture(Moment moment) {
  if (!g_active || g_captured || g_capture_moment != uint32_t(moment) || !g_capture) {
    return;
  }
  g_captured = true;
  Log(std::format("RenderDoc capture requested for the next frame ({})",
                  moment == Moment::kDecode ? "first decode" : "first deactivation by overlap"));
  g_capture();
}

}  // namespace mcla::gpu::watch
