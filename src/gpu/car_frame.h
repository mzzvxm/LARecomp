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
//
// HOW A FRAME IS COLLECTED (car_frame.cpp). A user calls Request() in every frame it needs the car; the next frame collects (the
// Scene's per-draw call is one flag read in the frames nobody asks). In a collecting frame every main-view draw that writes a
// vehicle's stencil ID goes through OnMainDraw:
//   - the draws of the wheel proxies (car_dump_rules.h) are left out at once;
//   - the player's car is told by identity, not by shader (car_frame_rules.h IdentityTracker): the stencil ID whose gWorld is
//     nearest to the player's vehicle in the game's memory, read once a frame; the ID of the frame before stands in for a draw that
//     comes first. Draws of other cars cost the matrix's translation and a compare;
//   - for a draw of the player's car with triangles the Draw is made (the matrix of its recipe from the constants, the host
//     geometry, the host position element, the mesh key) and the listeners get OnPlayerDraw inside the draw's own call. Draws seen
//     before the ID is known are held (at most a few dozen) and join the Frame once it is.
// Guest memory is read in MeshReader::ReadTriangles (by a listener) and once per new mesh_key for its model-space box (a few
// per frame at most, then cached by key and dropped when the key has not been seen for a long while).
// ===========================================================================

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <plume_render_interface.h>

#include "car_dump_rules.h"
#include "car_frame_rules.h"
#include "car_mesh_read.h"
#include "constants.h"
#include "geometry.h"
#include "input_layout.h"
#include "shaders.h"
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
  // geometry cache's version of those ranges (a mesh streamed in again at the same address is another key). 0: not known
  // (inline vertices; and always for kSkinned, whose vertices are blended afresh in every frame).
  uint64_t mesh_key = 0;
  // kBody: the model-space box of the draw's mesh (the vertices its triangles name; read once per mesh_key, kept while it is
  // seen). has_box false: not known; box_deferred: it will be known in a frame or two (a frame reads a few boxes at most).
  bool has_box = false;
  bool box_deferred = false;
  float box_min[3] = {};
  float box_max[3] = {};

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
  // y up, z forward = the linear depth t: ao_common.hlsli AoViewPos). The game's own view looks along -z; `view` is that with z
  // negated, as ao.cpp makes the AO's view (car_frame_rules.h AoView); `view_projection` is the game's clip space.
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
    const GeometryCache* cache = nullptr;  // for the mesh key: its stamp of the guest ranges (GeometryCache::RangeStamp); may be null
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
  // How a vertex shader (by identity) places its vertices: car_dump_rules.h's recipe, from its microcode when that was read.
  struct RecipeEntry {
    car_dump::RecipeChoice choice;
    bool verified = false;          // from the shader's microcode
    bool skinned_declared = false;  // the shader's vertex elements include BLENDINDICES
    uint64_t checked = 0;           // the guest frame it was made in (an unverified one is made again after a while)
  };
  // How the host reads a vertex input layout: where POSITION, NORMAL and the blend elements are (by InputLayout::id, which is stable).
  struct LayoutInfo {
    bool position = false;  // a POSITION in a real stream the draws' triangles can be had from (float3, float4 or half4)
    uint32_t position_stream = 0;  // the guest's stream number
    uint32_t position_slot = 0;    // the slot of the host's vertex views
    uint32_t position_offset = 0;
    uint32_t position_stride = 0;
    plume::RenderFormat position_format = plume::RenderFormat::UNKNOWN;
    bool normal = false;
    uint32_t normal_stream = 0;
    uint32_t normal_offset = 0;
    plume::RenderFormat normal_format = plume::RenderFormat::UNKNOWN;
    bool blend = false;  // BLENDINDICES and BLENDWEIGHT both in real streams
    uint32_t blend_indices_slot = 0, blend_indices_offset = 0;
    plume::RenderFormat blend_indices_format = plume::RenderFormat::UNKNOWN;
    uint32_t blend_weights_slot = 0, blend_weights_offset = 0;
    plume::RenderFormat blend_weights_format = plume::RenderFormat::UNKNOWN;
  };
  // The model-space box of a mesh, by mesh_key.
  struct MeshBox {
    float lo[3] = {};
    float hi[3] = {};
    bool valid = false;  // false: the mesh could not be read (not tried again)
    uint64_t seen = 0;   // the guest frame it was last used in
  };
  // What a frame's log line says, to log only a change.
  struct Summary {
    bool valid = false;
    uint32_t id = 0;
    uint32_t draws = 0, body = 0, skinned = 0;
    bool operator==(const Summary&) const = default;
  };
  // The numbers of the frame being collected, for the log.
  struct Counts {
    uint32_t vehicle_draws = 0;   // main-view draws that wrote a vehicle ID
    uint32_t no_player = 0;       // ... with no player's vehicle to match them to
    uint32_t proxies = 0;         // ... the wheel proxies
    uint32_t other_cars = 0;      // ... another car's, skipped
    uint32_t no_triangles = 0;    // ... the player's, with no triangles (lines, a computed fetch, no POSITION)
    uint32_t not_finite = 0;      // ... the player's, with a matrix that is not a number
    uint32_t pending_full = 0;    // ... held back before the ID was known, and no room
    uint32_t boxes_read = 0;      // meshes read for their box
    uint32_t listener_reads = 0;  // MeshReader calls
  };
  // The draw being handled, for the MeshReader (valid inside the draw's own call only).
  struct Context {
    const MainDraw* draw_in = nullptr;
    const car_frame::Draw* draw = nullptr;
    bool active = false;
  };
  // The MeshReader the listeners get: the current draw's triangles.
  class Reader final : public car_frame::MeshReader {
   public:
    bool ReadTriangles(std::vector<float>* positions, std::vector<uint32_t>* indices, std::vector<float>* normals) const override;
    CarFrame* owner = nullptr;
  };

  void StartFrame();
  void FinishFrame();
  void OnVehicleDraw(const MainDraw& d, uint32_t order, uint32_t id);
  void HandleVehicleDraw(const MainDraw& d, uint32_t order, uint32_t id);
  void OnConfirmed(uint32_t id, bool switched);
  void SyncPlayer();
  bool HasTriangles(const MainDraw& d) const;
  const LayoutInfo& LayoutOf(const InputLayout& layout);
  const RecipeEntry& RecipeOf(const MainDraw& d);
  void FillDraw(const MainDraw& d, uint32_t order, uint32_t id, const RecipeEntry& recipe, const LayoutInfo& layout,
                const car_frame_rules::ModelDecode& model, car_frame::Draw* out);
  void FillPalette(const ConstantBanks::Banks& banks, std::vector<float>* out);
  uint64_t ComputeMeshKey(const MainDraw& d, const LayoutInfo& layout) const;
  void NoteBox(car_frame::Draw* out);
  void NoteCamera(const uint8_t* vs_bank, const temporal::Mat4& world);
  void AddToBounds(const car_frame::Draw& draw);
  void CallListeners();
  bool LoadMesh();
  bool ReadForListener(std::vector<float>* positions, std::vector<uint32_t>* indices, std::vector<float>* normals);
  void LogUnreadable(const char* what);
  void ReleaseDraws(std::vector<car_frame::Draw>& draws);
  std::vector<float> TakePalette();

  bool requested_ = false;
  bool collecting_ = false;
  car_frame::Frame frame_;

  GuestMemory* memory_ = nullptr;
  std::vector<car_frame::Listener*> listeners_;
  Reader reader_;
  uint64_t guest_frame_ = 0;  // guest presents seen

  // The frame being collected.
  car_frame_rules::IdentityTracker tracker_;
  uint32_t previous_id_ = 0;           // the player's ID in the frame before (0: none)
  bool player_read_ = false;           // the player's vehicle was read in this frame
  car_mesh::PlayerVehicle player_;
  uint32_t main_draws_ = 0;            // main-view draws so far in this frame
  uint32_t world_draws_ = 0;           // ... that wrote no vehicle ID
  std::vector<car_frame::Draw> pending_;  // draws held before the player's ID is known (the ones of every ID, to be sorted out)
  std::array<temporal::Mat4, car_dump::kIdSlots> best_world_{};  // gWorld of the draw nearest the vehicle, by ID
  bool camera_done_ = false;
  uint32_t camera_attempts_ = 0;
  uint32_t boxes_read_ = 0;            // meshes read for their box in this frame
  uint32_t bounds_missing_ = 0;        // listed body draws whose box is not known yet
  car_frame_rules::Box3 world_box_;
  Counts counts_;
  bool log_ = false;                   // mcla_gpu_car_frame_log, read once a frame
  Summary logged_;
  bool logged_any_ = false;

  // The draw being handled.
  Context ctx_;
  bool mesh_loaded_ = false;
  car_mesh::Triangles mesh_;
  car_mesh::Scratch scratch_;
  std::vector<float> skin_blended_, skin_normals_in_, skin_normals_out_;

  // Caches, kept across frames.
  car_mesh::ShaderReadsCache reads_;
  std::unordered_map<uint64_t, RecipeEntry> recipes_;
  std::unordered_map<uint32_t, LayoutInfo> layouts_;
  std::unordered_map<uint64_t, MeshBox> boxes_;
  std::unordered_set<uint64_t> unreadable_logged_;
  std::vector<std::vector<float>> palette_pool_;
  std::vector<float> palette_cache_;
  const ConstantBanks::Banks* palette_cache_banks_ = nullptr;
  uint64_t palette_cache_version_ = 0;
  std::vector<ShaderStore::VertexElement> elements_;
};

}  // namespace mcla::gpu
