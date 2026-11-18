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
//   mcla_gpu_car_dump_id   the stencil ID the OBJ and the first check are made for; 0: the player's car, found as car_dump_rules.h
//                          ChoosePlayerId does (the ID whose gWorld is nearest to the player's vehicle in the game's memory, else the
//                          nearest ID with a paint draw, else the one with the most body draws); the log says which rule chose it.
//
// WHAT A DRAW GIVES (the note's sections 1, 3 and 9). One rigid car draw is ONE affine model-to-world matrix, in the draw's vertex
// shader constants: the bone's three registers of the palette gCarMtxBuffer (c64.., bone = trunc(c204.x)) and then gWorld c0-c3;
// gWorldViewProj c8-c11 is gWorld times the camera's view-projection. But not every vertex shader reads the palette: the tyres and the wheel
// blur proxies read gWorld alone (placing them with the palette puts them hundreds of metres away: c204 and the palette hold what an earlier
// draw left), and the character shaders (the driver) blend up to four palette bones per vertex. The RECIPE of a draw is therefore chosen by what
// its vertex shader reads: the microcode is read from guest memory (DrawRecord::vs_ucode_address, checked against the draw's shader
// identity) and walked (car_dump_rules.h ScanConstantReads: the constants read at a fixed index, the reads through a0 of the palette's rows);
// a shader that reads the rows and c204 is palette then gWorld, one that reads the rows and no c204 blends bones per vertex (BLENDINDICES,
// BLENDWEIGHT: SkinBlend, then gWorld), one that reads no rows is gWorld alone. The shader pack has no such information (its entries hold
// the vertex elements, the texture fetch mask and the flags), so without the microcode (not readable, or not the draw's shader) the
// declaration and a short table of known world-only shaders (car_dump_rules.h kKnownWorldOnlyVs) stand in.
// Positions are model-space floats (float32 or float16, never packed). So the triangles are: the draw's indices (guest memory, under the
// index endian; strips, quads and fans expanded with the renderer's own ConvertIndices / StripToList / GenerateIndices) over the POSITION
// element of the draw's declaration (its stream's fetch constant gives the address, size and the stride), taken through that matrix
// (car_dump_rules.h). The damage branch (bool b0) is NOT done: such draws are counted.
//
// WHAT A DRAW IS. Body (the rigid parts and the tyres: what the car is made of), skinned (the driver, blended on the CPU as the vertex
// shader does, an unverified decode: the log counts the bones that are not in the palette and the weight sums) and wheel proxy (a stencil-only
// draw per wheel with a stencil ID of its own: its shader pair, or no colour write; not geometry of the car). The proxies are left out
// of the car and the checks and counted in the census; the skinned draws are left out of the OBJ of the car and its first check (the
// body: what a shadow or a reflection of the car needs) and written to a second file and a second check line.
//
// WHEN GUEST MEMORY IS READ: in the dump frame only, in the draw's own call (OnMainDraw), as the geometry cache reads it for the
// same draw (the guest is held back then); before the dump frame and after it a main-view draw costs a flag read, and the
// frame's draws before the first vehicle draw a counter. The cvar is read once a frame.
//
// WHAT IT WRITES (the log lines are prefixed "[car dump]"; car_dump_<frame>.obj goes next to the executable):
//   - at the end of the dump frame: why the chosen ID is the player's car, the census (per ID: draws by part, vertices, triangles,
//     how near its gWorld is to the camera and to the player's vehicle; per (ID, vertex shader, pixel shader): the recipe and where it
//     came from, the part, draws, primitive, index size, the position's format, offset and stride, the colour mask, the bones used,
//     draws with the damage bool b0 on, skinned draws, before or after the first world draw, gWorld's translation and its
//     distance to the camera, and whether gWorld times the camera's view-projection is the draw's gWorldViewProj, to a relative
//     1e-3), the OBJ of the chosen ID's body, one group per draw named id_vsHash_psHash_bone, and for a car with skinned draws
//     car_dump_<frame>_skinned.obj;
//   - when the car history image of the same frame arrives from the GPU (car_selfrefl.h: its alpha is the view distance of every
//     vehicle pixel; the car self-reflections must be on, mcla_gpu_car_selfrefl_test=1 does it): the check. Every triangle is
//     projected with the frame's camera (the anchor draw's constants) and rasterised on the CPU at the image's size, with a
//     nearest-distance test; it logs the share of vehicle pixels covered by at least one triangle, the mean and 95th percentile
//     of the depth error and the triangles that are wholly off the image, for the chosen ID's body alone, the body with its skinned
//     parts and all IDs (the wheel proxies are in none), and for each (ID, VS, PS) how the draws' own pixels sit against the image
//     under the palette recipe, under gWorld alone and, for a skinned group, under the blend (it settles which recipe a family
//     follows, see car_dump_rules.h FitCount).
// The image is a half float (its own steps are about 0.05% of the distance) and, at half size, read at the lower right pixel of
// each 2x2 block; the check samples the same point. Run it with the upscaler off, the car parked, in the game's own view. The
// driver's pixels are vehicle pixels of the image too: the body's coverage cannot reach 100% of them.
//
// Threading: render thread only. Initialize and Shutdown on the lifecycle thread.
// ===========================================================================

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>

#include "car_dump_rules.h"
#include "car_mesh_read.h"
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
  // Why a draw has no triangles, where the player's vehicle is in the game's own memory (read when the dump frame opens), and what
  // the microcode of a vertex shader was found to read: car_mesh_read.h, which the car frame (car_frame.h) shares.
  using Problem = car_mesh::Problem;
  using PlayerVehicle = car_mesh::PlayerVehicle;
  using ShaderReads = car_mesh::ShaderReads;

  // One vehicle draw of the dump frame, as a small record, and what the dump frame made of it. Its triangles (Triangles: the problem, the
  // primitive's shape, the position element's place, the model-space positions and the triangle list) are car_mesh::ReadTriangles'.
  struct Draw : car_mesh::Triangles {
    uint32_t id = 0;       // the lowest vehicle ID the draw writes (stencil reference >> 2)
    uint32_t id_mask = 0;  // every vehicle ID it writes
    uint64_t vs = 0;
    uint64_t ps = 0;
    uint32_t primitive = 0;  // xenos::PrimitiveType
    DrawKind kind = DrawKind::kIndexed;
    uint32_t count = 0;      // the draw's indices or vertices
    uint32_t world_before = 0;  // main-view draws that wrote no vehicle ID, before this one, in the frame
    bool damage = false;   // bool b0 (enableVertDamage) is on
    bool skinned = false;  // the vertex shader reads BLENDINDICES (the declaration says so)
    uint32_t colour_mask = 0;  // the write masks of the bound colour targets, or-ed
    car_dump::ProxyWhy proxy = car_dump::ProxyWhy::kNo;
    car_dump::Recipe recipe = car_dump::Recipe::kPalette;
    car_dump::RecipeSource recipe_source = car_dump::RecipeSource::kDefault;
    car_dump::Role role = car_dump::Role::kBody;
    bool ucode_read = false;   // the recipe comes from the microcode of this shader
    car_dump::CarMatrices matrices;
    temporal::Mat4 model;      // the recipe's model-to-world (car_dump_rules.h ModelMatrix)
    car_dump::SkinStats skin;  // the blend of a skinned draw: vertices, bones outside the palette, weight sums
    std::string skin_note;     // why a skinned draw has no blended positions
    std::vector<float> blended;  // a skinned draw: the blended local positions of the same vertices (empty when not done)
    const float* Positions() const { return recipe == car_dump::Recipe::kSkinned ? blended.data() : local.data(); }
    size_t PositionCount() const { return (recipe == car_dump::Recipe::kSkinned ? blended.size() : local.size()) / 3; }
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
  bool SkinDraw(const MainDraw& draw, uint32_t first, uint32_t span, Draw* out);
  void Finish();
  void Summarise(car_dump::IdSummary (&ids)[car_dump::kIdSlots]) const;
  car_dump::Pick ChoosePlayer(const car_dump::IdSummary (&ids)[car_dump::kIdSlots]) const;
  void Census(uint32_t chosen, const car_dump::IdSummary (&ids)[car_dump::kIdSlots]);
  void WriteObj(uint32_t chosen, car_dump::Role role) const;
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
  PlayerVehicle player_;      // read when the frame opens

  // The microcode scans, by vertex shader identity (verified ones only: a failed read is tried again).
  car_mesh::ShaderReadsCache reads_;

  // Scratch of Expand.
  car_mesh::Scratch scratch_;
  std::vector<ShaderStore::VertexElement> elements_;

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  GuestMemory* memory_ = nullptr;
  std::unique_ptr<plume::RenderBuffer> readback_;
  uint64_t readback_bytes_ = 0;
};

}  // namespace mcla::gpu
