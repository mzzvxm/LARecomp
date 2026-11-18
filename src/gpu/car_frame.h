#pragma once
// ===========================================================================
// MCLA GPU renderer -- the player car's draws of a frame
// ===========================================================================
// The car-only effects need the same thing every frame: which main-view draws are the player's car, how each one is placed
// (its model-to-world matrix, chosen per draw by what its vertex shader reads: car_dump_rules.h), and the geometry it drew.
//   - car_shadow_map.h: a sun shadow map of the player's car alone, drawn again from the draws' own host geometry (every GPU);
//   - car_rt_scene.h: the player's car as ray tracing acceleration structures, built once per mesh from guest memory (RT GPUs).
// CarFrame collects that during the frame, in the per-draw call the car dump has (Scene::NoteAmbientOcclusion), and hands it
// out at the history trigger (car_selfrefl.h kTriggerDestination), when every car draw of the frame is in.
//
// CONTRACT (2026-11-18): feat/car-frame implements this header in car_frame.cpp; feat/car-shadow-map and feat/car-rt-scene
// use it. The names and fields below are fixed; the implementer may add members. A field marked "may be empty" may stay
// empty in a first version, and its users must cope.
//
// Threading: render thread only. Initialize and Shutdown on the lifecycle thread.
// ===========================================================================

#include <cstdint>
#include <vector>

#include <plume_render_interface.h>

#include "car_dump_rules.h"
#include "geometry.h"
#include "temporal_math.h"

namespace mcla::gpu {

class GuestMemory;

namespace car_frame {

enum class Part : uint8_t {
  kBody,     // a rigid part: ONE matrix (a palette bone then gWorld, or gWorld alone: the tyres)
  kSkinned,  // the driver: up to four palette bones per vertex, then gWorld
};

// One main-view draw of the player's car. The wheel proxies (car_dump_rules.h WheelProxyOf) and draws without triangles
// (line lists, a computed fetch, no POSITION) are never listed.
struct Draw {
  Part part = Part::kBody;
  uint32_t stencil_id = 0;  // the vehicle ID it writes (stencil reference >> 2)
  uint64_t vs = 0;          // shader identities
  uint64_t ps = 0;
  uint32_t order = 0;       // its place among the frame's main-view draws
  bool after_world = false; // drawn after the frame's first world draw (the translucent parts and glass, often)

  // kBody: the draw's model-to-world, row vectors as the game's (world = (p, 1) * M; car_dump_rules.h ModelMatrix).
  // kSkinned: gWorld alone (the bones are per vertex).
  temporal::Mat4 model_to_world;

  // Same key = the same triangles in model space (kBody), so a cache by key never reads guest memory twice for a mesh. Made
  // from the position stream's and the index buffer's guest addresses, sizes and formats, the primitive, the counts and the
  // geometry cache's version of those ranges (a mesh streamed in again at the same address is another key). 0: not known.
  uint64_t mesh_key = 0;

  // For a host replay (car_shadow_map.h): the geometry the draw itself was issued with (Scene::PreparedDraw::geometry): host
  // vertex and index views (byte-swapped host copies), the host topology (strips and fans already converted where the host
  // needs it), counts and base vertex. Valid until the frame's command lists are submitted.
  DrawGeometry geometry;
  uint32_t position_slot = 0;    // the vertex view in `geometry` that holds POSITION
  uint32_t position_offset = 0;  // bytes into a vertex of that view
  uint32_t position_stride = 0;  // bytes
  plume::RenderFormat position_format = plume::RenderFormat::UNKNOWN;  // R32G32B32(A32)_FLOAT, R32G32_FLOAT, R16G16B16A16_FLOAT, ...
  // kSkinned only, for a host replay of the blend: the elements and the palette. May be empty (then a replay leaves the
  // driver out, or uses MeshReader's blended triangles).
  uint32_t blend_indices_slot = 0, blend_indices_offset = 0;
  plume::RenderFormat blend_indices_format = plume::RenderFormat::UNKNOWN;
  uint32_t blend_weights_slot = 0, blend_weights_offset = 0;
  plume::RenderFormat blend_weights_format = plume::RenderFormat::UNKNOWN;
  std::vector<float> palette;  // 64 bones x 3 rows x float4, as the vertex shader reads c64..c255
};

// The frame so far. Complete at the history trigger (every car draw of a frame comes before it); cleared at the guest present.
struct Frame {
  bool valid = false;             // the player's car was found in this frame
  uint64_t guest_frame = 0;
  uint32_t player_id = 0;         // its stencil ID
  temporal::Mat4 player_world;    // its body's gWorld (the vehicle's root; row 3 = its position in the world)
  // The camera of this frame as the player's draws carry it (no jitter: that is added on the host): world -> view, the inverse
  // of the draws' c12-c15, and world -> clip, gWorld^-1 * gWorldViewProj of a body draw. View space is the AO's (x right,
  // y up, z forward = the linear depth t: ao_common.hlsli AoViewPos). // CHECK (implementer): the axes against AoViewPos.
  temporal::Mat4 view;
  temporal::Mat4 view_projection;
  double to_sun[3] = {};          // world-space unit vector toward the sun (the draws' c16, as ao.cpp reads it); may be empty (0,0,0)
  // A world-space box around the car's body (the meshes' model-space boxes through their matrices). May be empty (then
  // bounds_valid is false and a user takes a sphere of a few metres around player_world's row 3).
  bool bounds_valid = false;
  double bounds_min[3] = {};
  double bounds_max[3] = {};
  std::vector<Draw> draws;        // the player's car only, in draw order
};

// Reads one draw's triangles out of guest memory. Usable ONLY inside Listener::OnPlayerDraw (the guest is held back then).
class MeshReader {
 public:
  virtual ~MeshReader() = default;
  // The draw's triangles as a triangle list: `positions` x,y,z per vertex (model space for kBody; world space, blended as the
  // vertex shader does, for kSkinned), `indices` three a triangle into them. `normals` (null: not wanted) x,y,z per vertex in
  // the same space, from the declaration's NORMAL element; may be empty (an empty vector: the user makes smooth normals from
  // the triangles). False: unreadable (logged once per mesh), the vectors are then empty.
  virtual bool ReadTriangles(std::vector<float>* positions, std::vector<uint32_t>* indices,
                             std::vector<float>* normals) const = 0;
};

class Listener {
 public:
  virtual ~Listener() = default;
  // A draw known to be the player's car, inside the draw's own call. The ID is the one this frame confirmed, else the previous
  // frame's: a draw before the frame's first confirmed draw may be missed here (it is in the trigger's Frame all the same).
  virtual void OnPlayerDraw(const Draw& draw, const MeshReader& reader) = 0;
};

}  // namespace car_frame

class CarFrame {
 public:
  // One prepared main-view draw (Scene::NoteAmbientOcclusion): everything it is read from stays valid for the call.
  struct MainDraw {
    const DrawView* view = nullptr;
    const ConstantBanks::Banks* banks = nullptr;
    const XenosState* state = nullptr;
    const ShaderStore* shaders = nullptr;
    const ShaderStore::Entry* vs = nullptr;
    const ShaderStore::Entry* ps = nullptr;
    const InputLayout* input_layout = nullptr;
    const DrawGeometry* geometry = nullptr;
    HostPrimitive primitive;
    uint32_t stencil_ids = 0;  // vehicle_motion.h StencilIdsWritten
  };

  CarFrame() = default;
  ~CarFrame();
  CarFrame(const CarFrame&) = delete;
  CarFrame& operator=(const CarFrame&) = delete;

  void Initialize(GuestMemory& memory);
  void Shutdown();

  // A user wants the frames: call it in every frame it needs one (any time in the frame); a request in frame N makes frame N+1
  // collect. Nobody asks: the per-draw call is one flag read.
  void Request() { requested_ = true; }
  // Listeners get OnPlayerDraw while the frames are collected (at most a few; null entries are ignored).
  void AddListener(car_frame::Listener* listener);

  bool wants_draws() const { return collecting_; }
  // The Scene opened a frame's first list.
  void BeginFrame();
  // A prepared main-view draw (temporal_identities.h MatchMainView).
  void OnMainDraw(const MainDraw& draw);
  // The frame so far (complete at the history trigger).
  const car_frame::Frame& Current() const { return frame_; }
  // A guest present: the frame boundary.
  void OnPresent();

 private:
  bool requested_ = false;
  bool collecting_ = false;
  car_frame::Frame frame_;
  // The implementer adds the rest.
};

}  // namespace mcla::gpu
