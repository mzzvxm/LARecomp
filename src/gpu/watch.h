#pragma once
// ===========================================================================
// MCLA GPU renderer -- diagnostic watch of one guest memory range
// ===========================================================================
// With mcla_gpu_watch_base set (hex),
// every event that touches [base, base + mcla_gpu_watch_size) is logged in
// order, with the guest frame and the draws recorded so far in it: resolves
// into the range, products over it activated and deactivated (and by what),
// fetches of textures over it (bound to a product, decoded from the resolve
// memory, or read from guest memory, memoized or not) and decodes from the
// resolve memory (which resolves wrote its pages, how many frames ago).
// mcla_gpu_watch_capture can ask for a RenderDoc capture of the frame after the
// first decode or the first deactivation by another resolve. An empty base
// (the default) does nothing at all.
//
// Threading: render thread only.
// ===========================================================================

#include <cstdint>
#include <functional>
#include <string>

namespace mcla::gpu {
struct TextureKey;
}

namespace mcla::gpu::watch {

// Reads the cvars (once, at the renderer's start).
void Configure();
bool Active();
// Whether [start, start + length) overlaps the watched range.
bool Touches(uint32_t start, uint32_t length);
// Whether the guest memory a texture key covers (base and mips) overlaps it.
bool TouchesKey(const TextureKey& key);

// The guest frame, and the draws recorded in it so far.
void BeginFrame(uint64_t serial);
void OnDraw(uint64_t ps_identity);

// One line, prefixed with the frame and the draw position. Kept in a ring of
// the last events until Flush, then written to the log (at most kMaxLines).
void Log(const std::string& text);
// Writes the ring out, once, and logs every later event directly: called at
// the first decode of the range, so the log holds what led to it.
void Flush(const char* why);
// The PS of the current draw, for fetch lines.
uint64_t CurrentPs();

// The capture hook (FrameCapture::Request), set by the renderer.
void SetCaptureRequest(std::function<void()> request);
enum class Moment : uint8_t { kDecode = 1, kDeactivatedByOverlap = 2 };
// Asks for the capture once, when mcla_gpu_watch_capture names this moment.
void MaybeCapture(Moment moment);

}  // namespace mcla::gpu::watch
