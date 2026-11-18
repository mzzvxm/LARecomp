#pragma once
// ===========================================================================
// MCLA GPU renderer -- the car dump: the cars' triangles of one frame, from guest memory (a debug prototype)
// ===========================================================================
// perf/CAR-RT-SPIKE.md section 7. A car-only ray traced or rasterised effect (the car's own reflections, shadows, ambient
// occlusion) needs the player car's triangles every frame. This is the one-run prototype that shows whether the renderer can
// have them: for ONE frame it takes the triangles of every main-view draw that writes a vehicle's stencil ID out of guest memory,
// puts them in world space with the draw's own constants, writes them to a file and checks them against an image the renderer
// already makes of the cars.
//
//   mcla_gpu_car_dump      0 off (the default); N: dump the first frame at or after guest frame N that has a draw writing a
//                          vehicle ID (the car draws before the world, so every vehicle draw of the frame comes before the
//                          history pass; the dump frame ends at the guest's present). Hot: a new value dumps again.
//   mcla_gpu_car_dump_id   the stencil ID the OBJ and the first check are made for; 0: the ID with the most draws in the frame.
//
// WHAT A DRAW GIVES (the note's sections 1 and 3). One rigid car draw is ONE affine model-to-world matrix, in the draw's vertex
// shader constants: the bone's three registers of the palette gCarMtxBuffer (c64.., bone = trunc(c204.x)) and then gWorld c0-c3
// (the tyres have no palette: gWorld alone); gWorldViewProj c8-c11 is gWorld times the camera's view-projection. Positions are
// model-space floats (float32 or float16, never packed). So the triangles are: the draw's indices (guest memory, under the
// index endian; strips, quads and fans expanded with the renderer's own ConvertIndices / StripToList / GenerateIndices) over the
// POSITION element of the draw's declaration (its stream's fetch constant gives the address, size and the stride), taken
// through that matrix (car_dump_rules.h). Skinned vertex shaders (BLENDINDICES) and the damage branch (bool b0) are NOT done:
// those draws are counted and written with the palette matrix alone.
//
// WHEN GUEST MEMORY IS READ: in the dump frame only, in the draw's own call (OnMainDraw), as the geometry cache reads it for the
// same draw (the guest is held back then); before the dump frame and after it a main-view draw costs a flag read, and the
// frame's draws before the first vehicle draw a counter. The cvar is read once a frame.
//
// WHAT IT WRITES (the log lines are prefixed "[car dump]"; car_dump_<frame>.obj goes next to the executable):
//   - at the end of the dump frame: the census (per ID: draws, vertices, triangles, how near its gWorld is to the camera; per
//     (ID, vertex shader, pixel shader): draws, primitive, index size, the position's format, offset and stride, the bones used,
//     draws with the damage bool b0 on, skinned draws, before or after the first world draw, gWorld's translation and its
//     distance to the camera, and whether gWorld times the camera's view-projection is the draw's gWorldViewProj, to a relative
//     1e-3) and the OBJ of the chosen ID, one group per draw named id_vsHash_psHash_bone;
//   - when the car history image of the same frame arrives from the GPU (car_selfrefl.h: its alpha is the view distance of every
//     vehicle pixel; the car self-reflections must be on, mcla_gpu_car_selfrefl_test=1 does it): the check. Every triangle is
//     projected with the frame's camera (the anchor draw's constants) and rasterised on the CPU at the image's size, with a
//     nearest-distance test; it logs the share of vehicle pixels covered by at least one triangle, the mean and 95th percentile
//     of the depth error and the triangles that are wholly off the image, for the chosen ID alone and for all IDs, and for each
//     (ID, VS, PS) how the draws' own pixels sit against the image under the palette recipe and under gWorld alone (it settles
//     which recipe a family follows, see car_dump_rules.h FitCount).
// The image is a half float (its own steps are about 0.05% of the distance) and, at half size, read at the lower right pixel of
// each 2x2 block; the check samples the same point. Run it with the upscaler off, the car parked, in the game's own view.
//
// Threading: render thread only. Initialize and Shutdown on the lifecycle thread.
// ===========================================================================

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <plume_render_interface.h>

#include "car_dump_rules.h"
#include "constants.h"
#include "draw_decode.h"
#include "draw_state.h"
#include "records.h"
#include "shaders.h"
#include "temporal_camera.h"
#include "topology.h"

namespace mcla::gpu {

class Device;
class FrameRing;
class GuestMemory;
struct HostImage;

class CarDump {
 public:
  // One prepared main-view draw (Scene::NoteAmbientOcclusion): everything it is read from stays valid for the call.
  struct MainDraw {
    const DrawView* view = nullptr;
    const ConstantBanks::Banks* banks = nullptr;
    const XenosState* state = nullptr;
    const ShaderStore* shaders = nullptr;
    const ShaderStore::Entry* vs = nullptr;
    HostPrimitive primitive;
    uint32_t stencil_ids = 0;  // vehicle_motion.h StencilIdsWritten
  };

  CarDump() = default;
  ~CarDump();
  CarDump(const CarDump&) = delete;
  CarDump& operator=(const CarDump&) = delete;

  // Never fails: a dump that cannot be made is logged and skipped.
  void Initialize(Device& device, FrameRing& frames, GuestMemory& memory);
  // GPU idle and the FrameRing drained.
  void Shutdown();

  // True while a main-view draw has anything to tell (the request is on and no dump has been made for it): the frame's own answer,
  // read from the cvar at the frame boundary, so a draw costs a flag read. Any thread.
  bool wants_draws() const { return wants_; }
  // The frame being dumped has a vehicle draw: the anchor and the history image are wanted.
  bool frame_open() const { return open_; }

  // A prepared main-view draw (temporal_identities.h MatchMainView), with the vehicle IDs it writes into the stencil.
  void OnMainDraw(const MainDraw& draw);
  // The game's motion blur draw: this frame's camera (the same read as car_selfrefl.h OnAnchor makes).
  void OnAnchor(const ConstantBanks::Banks& banks);
  // The car self-reflection's history image was just drawn (car_selfrefl.h RecordHistory, `lens` the projection it was linearised
  // with, `divisor` its size against the render size): in the dump frame, copies it to a readback buffer and leaves it readable.
  void OnHistoryImage(plume::RenderCommandList* list, HostImage& image, uint32_t divisor, const temporal::Mat4& lens);
  // A guest present: ends the dump frame (the census and the OBJ), and counts the frame.
  void OnPresent();

 private:
  enum class Problem : uint8_t {
    kNone,
    kComputedFetch,  // the vertex shader's fetch index is computed (the rain's quads): no triangles of a car
    kPrimitive,      // a primitive the renderer does not draw as triangles (rectangle list, unsupported)
    kNotTriangles,   // lines
    kNoPosition,     // the declaration has no POSITION0
    kPositionFormat, // a position that does not fit its stride or has no format here
    kFetch,          // the stream's fetch constant is no vertex fetch, or has no stride
    kUnreadable,     // a range is not committed guest memory
    kNoTriangles,    // nothing left after the index expansion
  };

  // One vehicle draw of the dump frame, as a small record, and what the dump frame made of it.
  struct Draw {
    uint32_t id = 0;       // the lowest vehicle ID the draw writes (stencil reference >> 2)
    uint32_t id_mask = 0;  // every vehicle ID it writes
    uint64_t vs = 0;
    uint64_t ps = 0;
    uint32_t primitive = 0;  // xenos::PrimitiveType
    DrawKind kind = DrawKind::kIndexed;
    bool index32 = false;
    bool strip = false;      // the host topology is a strip (expanded here)
    uint32_t count = 0;      // the draw's indices or vertices
    uint32_t world_before = 0;  // main-view draws that wrote no vehicle ID, before this one, in the frame
    car_dump::PositionFormat format = car_dump::PositionFormat::kNone;
    uint32_t stream = 0;
    uint32_t offset = 0;
    uint32_t stride = 0;
    bool damage = false;   // bool b0 (enableVertDamage) is on
    bool skinned = false;  // the vertex shader reads BLENDINDICES
    Problem problem = Problem::kNone;
    car_dump::CarMatrices matrices;
    uint32_t triangles = 0;
    uint32_t degenerate = 0;
    uint32_t out_of_range = 0;
    uint32_t vertices = 0;       // distinct vertices the triangles name
    uint32_t first_vertex = 0;   // `local` starts at this vertex of the stream
    std::vector<float> local;    // model-space positions of the vertices first_vertex..first_vertex + span, 3 floats each
    std::vector<uint32_t> tri;   // 3 numbers a triangle, relative to first_vertex
  };

  // The history image's distances, once read back.
  struct History {
    bool requested = false;
    bool ready = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t divisor = 1;
    temporal::Mat4 lens;           // the projection its distances were linearised with
    std::vector<float> distance;   // the image's alpha, width x height, 0 where no vehicle
  };

  static const char* ProblemName(Problem problem);
  void Refresh();
  void Open();
  void Expand(const MainDraw& draw, Draw* out);
  void Finish();
  void Census(uint32_t chosen);
  void WriteObj(uint32_t chosen) const;
  uint32_t ChosenId() const;
  void ProcessHistory(uint64_t frame, uint32_t width, uint32_t height, uint32_t row_bytes);
  void TryCheck();
  void RunCheck();
  void Release();

  // The request.
  uint64_t target_ = 0;
  bool wants_ = false;
  bool done_ = false;  // a dump was made for this request
  uint64_t frame_ = 0;  // guest presents seen

  // The dump frame.
  bool open_ = false;
  uint64_t dump_frame_ = 0;
  uint32_t world_draws_ = 0;  // main-view draws with no vehicle ID so far this frame
  std::vector<Draw> draws_;
  bool camera_valid_ = false;
  temporal::Camera camera_;
  bool finished_ = false;     // the census is written; the check waits for the image
  History history_;
  uint32_t chosen_ = 0;

  // Scratch of Expand.
  std::vector<uint32_t> guest_indices_;
  std::vector<uint32_t> list_;
  std::vector<ShaderStore::VertexElement> elements_;

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  GuestMemory* memory_ = nullptr;
  std::unique_ptr<plume::RenderBuffer> readback_;
  uint64_t readback_bytes_ = 0;
};

}  // namespace mcla::gpu
