#pragma once
// ===========================================================================
// MCLA GPU renderer -- camera cuts the game asked for, and the motion blur on a cut
// ===========================================================================
// The trailer toolkit's camera (src/mc_engine/cinematic/camera.h, work package M1b) cuts on purpose: a new shot starts where the
// camera is somewhere else. Two things in the renderer must know that the frame is the first of a shot, and the renderer's own cut
// rule (temporal_camera.h: a jump of 25 units, a turn of 30 degrees or 5% of the field of view) does not say it for a small cut:
//
//   - the temporal upscalers' history and the occlusion's history start again (temporal.cpp, ao.cpp);
//   - the game's own motion blur must not smear the frame (below).
//
// THE REQUEST. The game thread knows the frame that is a cut (the camera hook writes its camera), but the render thread draws it a
// frame or two later and has no number of the game's to compare (E5 measured: matching the request by position alone fired one
// frame EARLY for a cut that only turns, because the frame before the cut stands at the same place). A request therefore names the
// game's frame and carries the whole camera of that frame (position, forward and up), and the first frame whose anchor draw has that
// camera is the cut frame: the frame before a turn-only cut has the old direction, the frame before a jump the old position, and the
// frame after (the first of them to be moving on) is never the first to match. Each request is taken once. A frame that matches
// takes every older request with it (their frames are over); a request nothing matched in kGiveUpAfter frames is dropped and
// reported. The field of view is left out of the match: the game scales it after the hook (Patch_FOVScale), so the renderer's
// value is not the hook's; a cut of the field of view alone is left to the renderer's own rule.
//
// THE MOTION BLUR. The anchor draw (temporal_identities.h) is the game's motion blur. It reads, in its pixel constants c69 to
// c164, the previous frame's view-projection to guest pixels (c69) and, for each of the 23 vehicle ids, the same with the vehicle's
// own motion (c69 + 4 id): a pixel's streak runs from where that matrix puts the pixel to where it is. On the first frame of a
// shot the matrix is the old shot's, so the whole picture smears. E5 tried to rewrite the game's own previous-camera slots from the
// game thread and moved the smear one frame EARLIER (the render thread reads the slots after the game thread has written the next
// frame's). Here the fix is on the render thread, in the draw's own constants: for a requested cut every one of the 24 matrices
// becomes this frame's view-projection to guest pixels, so every streak is zero. It changes the constants of that one draw only, and
// only on a frame the game asked to cut.
//
// Pure: this file includes nothing of Plume or ReXGlue, cut_request_test.cpp builds on its own. Threading: Submit and Pending on any
// thread (the game thread submits); Take and the bank functions on the render thread.
// ===========================================================================

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace mcla::gpu::cut {

// The camera of a frame as the match compares it: position in metres, unit forward and up.
struct CameraPose {
  double position[3] = {};
  double forward[3] = {0.0, 0.0, 1.0};
  double up[3] = {0.0, 1.0, 0.0};
};

// Two cameras are the same frame's when they stand within kSamePosition of each other and look within about 0.8 degrees in
// both forward and up (the cosine is at least kSameCosine).
inline constexpr double kSamePosition = 0.1;
inline constexpr double kSameCosine = 0.9999;
// Anchors a request waits for its frame (the render thread runs a frame or two behind the game thread).
inline constexpr uint32_t kGiveUpAfter = 240;
bool SameCamera(const CameraPose& a, const CameraPose& b);

class Requests {
 public:
  // Any thread. Returns the request's serial.
  uint64_t Submit(uint32_t game_frame, const CameraPose& camera);
  // Any thread: a request waits.
  bool Pending() const { return pending_count_.load(std::memory_order_relaxed) != 0; }

  struct Taken {
    bool hit = false;
    uint64_t serial = 0;
    uint32_t game_frame = 0;
    uint32_t age = 0;       // anchors since it was submitted (1 = the first anchor after)
    uint32_t skipped = 0;   // older requests that went with it
    uint32_t expired = 0;   // requests dropped for waiting kGiveUpAfter anchors
    uint64_t expired_serial = 0;      // the first of those
    uint32_t expired_game_frame = 0;
  };
  // Render thread, once per accepted anchor draw with the frame's camera.
  Taken Take(const CameraPose& now);

  struct Counts {
    uint64_t submitted = 0;
    uint64_t matched = 0;
    uint64_t skipped = 0;
    uint64_t expired = 0;
  };
  Counts counts() const;

 private:
  struct Entry {
    uint64_t serial = 0;
    uint32_t game_frame = 0;
    uint32_t age = 0;
    CameraPose camera;
  };
  mutable std::mutex mutex_;
  std::vector<Entry> pending_;
  std::atomic<uint32_t> pending_count_{0};
  uint64_t next_serial_ = 1;
  Counts counts_;
};

// The process's requests (the game thread submits, the renderer takes).
Requests& Global();
inline void Submit(uint32_t game_frame, const CameraPose& camera) { Global().Submit(game_frame, camera); }

// ---------------------------------------------------------------------------------------------------------------- the anchor's constants

// The camera of the anchor's pixel constants: c12 right, c13 up, c14 backward, c15 position (temporal_camera.h). `ps_bank`: the 256
// registers of raw big-endian bytes (the banks of constants.h). False when they are not finite or have no axes.
bool CameraFromGuestBank(const uint8_t* ps_bank, CameraPose* out);

// The slots the anchor's shader addresses: the matrices c69 + 4 id for id 0 (the world) to 23.
inline constexpr uint32_t kBlurFirstRegister = 69;
inline constexpr uint32_t kBlurSlots = 24;

// Makes the bank's previous-frame matrices this frame's own, for a cut: every slot becomes the view-projection c8 to c11 times the
// pixel matrix of the frame size in c62, as the game's own c69 is on a still camera (x_pix = w/2 x + w/2 w, y_pix = h/2 w - h/2 y,
// depth and w kept). `host_bank`: 256 x 4 floats in host order, what SwapBankToHost wrote. False, and nothing written, when c8 to
// c11 or c62 are not usable.
bool NeutralizeMotionBlur(float* host_bank);

}  // namespace mcla::gpu::cut
