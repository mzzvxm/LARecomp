#pragma once
// ===========================================================================
// MCLA GPU renderer -- the motion of vehicles, for the temporal upscalers' vectors
// ===========================================================================
// feature-parity/GHOSTING-DESIGN.md sections 3 and 9. The motion vectors of temporal_mv.h carry the
// camera's motion for every pixel, which is wrong for what moves by itself: the player's car and the
// traffic ghost under FSR and DLSS. The game already computes the right thing for its own motion blur:
// the anchor draw (temporal_identities.h) reads, for each pixel, a stencil ID (the byte >> 2 that the
// main view's draws write: 0 the world, 1 to 23 a vehicle) and one matrix a vehicle, the pixel
// constants c(69 + 4 ID) to c(72 + 4 ID), which takes the world position of a point to its guest pixel
// one frame ago when the point belongs to that vehicle (c69 is the world's: the same for a point that
// does not move). The census of the real game (GHOSTING-DESIGN.md 9, six runs of 60 s on D3D12 with the
// blur on and off, at 30, 60 and 144 FPS) found:
//   - the slots are alive with the motion blur off, every frame, as with it on;
//   - c(69 + 4 ID) times the inverse of c69 is the vehicle's own motion, a rigid transform in world
//     coordinates, in 99.97% of the slot-frames (the others: a car placed in that frame);
//   - it is exactly one rendered frame of motion at every frame rate, with no scale;
//   - c69 is NOT this renderer's own previous view-projection in pixels (0.2 to 3 px off while driving,
//     tens of pixels in a turn), so the camera term stays from this renderer's own history and only the
//     vehicle's relative motion T = c(69 + 4 ID) * inverse(c69) is taken from the game;
//   - the IDs go from 1 to 15 and are handed out per frame in draw order: a car has no stable ID.
//
// What this file makes, per frame, on the CPU: the IDs the main view wrote before the anchor (the draws
// that REPLACE the stencil with a reference of 4 ID + {0, 1}: StencilIdsWritten) and the table of the 4x4
// every pixel of such a vehicle uses (BuildVehicleTable): this frame's clip space to the vehicle's own
// previous guest pixels,
//
//   C_ID = inverse(VP_now) * T_ID * VP_previous * ClipToPixels            (row vectors, double)
//
// with VP_now and VP_previous this renderer's own cameras (temporal_camera.h) and ClipToPixels the
// frame's. Only columns x, y and w are used (the depth column does not enter the vectors): three float4
// an ID. A slot that is not finite, not a rigid transform (a car placed or teleported this frame holds
// garbage) or without a usable c69 is left out, and its pixels keep the camera's motion.
// TemporalMotion (temporal_mv.h) draws the table into an image and the vector pass reads it.
//
// Pure functions on the bank and the camera: nothing here touches the GPU. Threading: render thread.
// ===========================================================================

#include <cstdint>

#include "draw_state.h"
#include "temporal_camera.h"

namespace mcla::gpu::temporal {

// The slots the anchor's shader can address: IDs 0 (the world) to 23. The shader's next constant,
// the frame size at c165, is the register after them.
inline constexpr uint32_t kVehicleIds = 24;
// The first register of the slots and the registers each takes.
inline constexpr uint32_t kVehicleFirstRegister = 69;

// What the vector pass takes of one frame: bit k of `valid` (1 to 23) says that row k holds a usable
// matrix; `columns[k]` is C_k's column x (4 floats), then y, then w.
struct VehicleTable {
  uint32_t valid = 0;
  float columns[kVehicleIds][12] = {};
};

// What one BuildVehicleTable found, for the summary line.
struct VehicleStats {
  uint32_t asked = 0;       // IDs 1 to 23 the main view wrote
  uint32_t taken = 0;       // ... with a usable matrix
  uint32_t not_sane = 0;    // the slot is not finite, or one of the denormals and huge numbers an unused slot holds
  uint32_t not_rigid = 0;   // c(69 + 4 ID) times inverse(c69) is not a rigid transform
  uint32_t not_finite = 0;  // the result is not finite
  bool no_world = false;    // c69 is not usable: nothing could be made
};

// The IDs (bit k, 0 to 23) the draw writes into the stencil: a REPLACE on pass with a reference of
// 4 ID + {0, 1}, either face (census R2.3: the vehicles' draws and the wheels' stencil-only proxies; the
// world's own draws write ID 0). 0 when it writes none, or one of 24 or more (the UI's).
// CHECK: a draw that writes the stencil another way (REPLACE on a failed depth test, INCR, INVERT) is
// not an ID writer here; the census saw REPLACE on pass with mask 0xff only.
uint32_t StencilIdsWritten(const XenosState& state);

// The table of a frame whose anchor was accepted and whose previous frame follows it directly
// (`before`: this renderer's own camera of that frame). `ps_bank`: the anchor's 256 pixel registers,
// raw big-endian bytes. `ids`: the IDs the frame's main view draws wrote before the anchor (bit k).
VehicleStats BuildVehicleTable(const uint8_t* ps_bank, uint32_t ids, const Camera& now,
                               const Camera& before, VehicleTable* out);

}  // namespace mcla::gpu::temporal
