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
// car_frame.cpp implements it; car_shadow_map.cpp uses it. A field marked "may be empty" may stay empty, and its users must cope.
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
// OTHER CARS (car_rt_instances_rules.h): the ray traced scene also holds the cars driving alongside, so a consumer that wants them calls
// RequestOtherCars(limits) (the ray traced scene, in every frame it is asked for structures: nobody else asks, and where no device can trace rays
// nobody does, so nothing below runs there). Then the draws of every ID that is not the player's go through the CarSelector: the cars within the
// radius of the player's vehicle, the nearest few; a kept car's draws get the same treatment as the player's (the recipe, the matrix, the mesh
// key) into Frame::other_draws, without the host geometry and without boxes, and the listeners get OnOtherCarDraw inside the draw's call, so
// that the scene reads the triangles of a model it has not seen. The skinned parts (the drivers) and the parts under Limits::min_indices
// are left out. A car that is new costs little in any one frame: the fingerprints of its meshes are made within Limits::content_bytes a frame
// (a draw whose mesh has no fingerprint yet waits for the next frame) and the scene reads the triangles within Limits::stage_triangles.
// Guest memory is read in MeshReader::ReadTriangles (by a listener) and once per new mesh_key for its model-space box (a few
// per frame at most, then cached by key and dropped when the key has not been seen for a long while). The mesh_key itself is made
// from a fingerprint of the mesh's own indices and vertices (car_frame_rules.h): those bytes are read again only when the geometry
// cache's stamp of their range has moved, so a mesh the guest did not change keeps its key whatever happens around it in memory.
// With mcla_gpu_car_frame_log on, a 'keys' line says when new keys appear (a parked car should make none).
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
#include "car_model_walk.h"
#include "car_rt_instances_rules.h"
#include "constants.h"
#include "geometry.h"
#include "input_layout.h"
#include "shaders.h"
#include "temporal_math.h"
#include "viewport.h"

namespace mcla::gpu {

class GuestMemory;

namespace car_frame {

// Another car's draw and the car (car_rt_instances_rules.h).
using OtherDraw = car_rt_instances::OtherDraw;
using OtherCar = car_rt_instances::OtherCar;

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
  // from the position stream's and the index buffer's guest addresses, sizes and formats, the primitive, the counts and a
  // fingerprint of the mesh's OWN guest bytes: the indices of the draw (in any order, for a triangle list) and the vertices they
  // name (car_frame_rules.h). A mesh streamed in again over the same address with other bytes is another key; a write the guest
  // makes next to it, or over it with the same bytes, is not. 0: not known (inline vertices; and always for kSkinned, whose
  // vertices are blended afresh in every frame).
  uint64_t mesh_key = 0;
  // What the surface is made of (car_rt_material_rules.h): the shader's class, its colours, its diffuse texture's fetch. Made only while the ray traced scene asks for the
  // other cars (RT-HIT-SHADING); class kNone otherwise and for a shader the table does not know.
  car_rt_material::Material material;
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
  // For a host replay that must land where the draw did (mirror_overlay.h, MIRROR-VIEW MV4): the viewport and the NDC transform the draw was issued with (ViewportInfo: the host viewport with its depth range, and
  // the scale and offset the vertex shader's epilogue applies) and its pipeline key's depth word (enable | write << 1 | the comparison function << 2, plume's values). viewport_known false: not recorded.
  ViewportInfo viewport;
  uint32_t depth_word = 0;
  bool viewport_known = false;
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
  // The other cars the selection kept (RequestOtherCars), with their draws in draw order: the nearest few within the radius of the player's
  // vehicle, each draw with a mesh key. Empty where nobody asks for them. Valid together with the player's: the distances are from the vehicle.
  std::vector<OtherCar> other_cars;
  std::vector<OtherDraw> other_draws;
  // The frame's lighting as the cars' own draws saw it, and the environment map's slots (car_rt_material_rules.h FrameLight): read from the first vehicle draw's pixel constants
  // (c26 ambient, c39 sun colour, c57 sky) and from a draw of a shader that reads the map (fetch slots 0 and 1). Only while the other cars are asked for.
  car_rt_material::FrameLight light;
  // For the cars the camera does not see (car_rt_memory_rules.h): read in the frame when the other cars are asked for, from the first vehicle draw on.
  //   - `vehicles`: the game's own vehicles (the opponent manager's racers, the ambient manager's live cars) within the scan radius of the player's vehicle,
  //     the player's own left out, each with its world matrix; `vehicles_valid`: the lists were read (an empty list then says nobody is near).
  //   - `player_vehicle`: the player's vehicle position (the distance the other cars are ranked by).
  //   - `seen_cars`: every stencil ID of another car the frame drew (kept or not) with the sum of its draws' gWorld translations, so that a car the selection
  //     did not keep still covers its vehicle.
  bool vehicles_valid = false;
  bool player_vehicle_known = false;
  double player_vehicle[3] = {};
  std::vector<car_rt_memory::VehicleRead> vehicles;
  std::vector<car_rt_memory::SeenCar> seen_cars;
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
  // After a successful ReadTriangles of the same draw: the TEXCOORD0 of the draw's declaration for the vertices ReadTriangles returned (u v a vertex, in the same order). False
  // (and `uvs` empty): the draw has none, or in a format that is not read (a half2 or a float2 is).
  virtual bool ReadTexcoords(std::vector<float>* uvs) const {
    if (uvs) {
      uvs->clear();
    }
    return false;
  }
};

class Listener {
 public:
  virtual ~Listener() = default;
  // A draw known to be the player's car, inside the draw's own call. The ID is the one this frame confirmed, else the previous
  // frame's: a draw before the frame's first confirmed draw may be missed here (it is in the trigger's Frame all the same).
  virtual void OnPlayerDraw(const Draw& draw, const MeshReader& reader) = 0;
  // A draw of another car that the selection kept (a listener that wants them asked with RequestOtherCars), inside the draw's own call; the
  // reader reads its triangles (model space). The car may be given up later in the frame by a nearer one (its draws leave the Frame; a mesh
  // read for it stays in the listener's cache by key).
  virtual void OnOtherCarDraw(const OtherDraw& /*draw*/, const MeshReader& /*reader*/) {}
  // The cars of the model walk (car_model_walk.h Production, asked for with RequestOtherCars' model mode): in the first vehicle draw's call of a frame that
  // collects them (the guest is held back), the cars near the player walked from their own model data. The listener stages the meshes of the cars marked
  // `staging` through `source`.
  virtual void OnWalkedCars(const std::vector<car_model_walk::WalkedCar>& /*cars*/, const car_model_walk::MeshSource& /*source*/) {}
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
    // For the mesh key: its stamp of the guest ranges (GeometryCache::RangeStamp), which says when a range MAY have changed and so
    // when its bytes are fingerprinted again; may be null (the key then has no fingerprints).
    const GeometryCache* cache = nullptr;
    // The draw's SharedConstants as the scene uploaded them (the bindless slot of every fetch constant's texture: texture_2d[i]): the environment map's slots are read from a draw
    // that binds it. Null: none (a caller that has none).
    const SharedConstants* shared = nullptr;
    // The draw's viewport info and its pipeline key's depth word (Draw::viewport, Draw::depth_word). Null: not known.
    const ViewportInfo* viewport = nullptr;
    uint32_t depth_word = 0;
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
  // As Request, and the draws of the other cars too, within `limits` (Limits::On() false: no other cars). Call it in every frame it is wanted.
  // `model`: the cars the camera does not draw are also walked from their own model data (car_model_walk.h Production; Mode::kOff: not).
  // `traffic`: how many traffic and parked cars the walk takes besides the racers (car_model_walk.h Production::traffic_cars; 0: none), `wheels`: the racers' wheels are walked.
  void RequestOtherCars(const car_rt_instances::Limits& limits, car_model_rt::Mode model = car_model_rt::Mode::kOff, uint32_t traffic = 0, bool wheels = true) {
    requested_ = true;
    others_requested_ = limits.On();
    other_limits_next_ = limits;
    model_mode_next_ = model;
    traffic_cars_next_ = traffic;
    wheels_next_ = wheels;
  }
  // The walker of the cars' model data: the production walk's results (walked(), Resolve at the history trigger) and the debug log.
  car_model_walk::Walker& walker() { return walk_; }
  // Listeners get OnPlayerDraw while the frames are collected (at most a few; null entries are ignored).
  void AddListener(car_frame::Listener* listener);

  bool wants_draws() const { return collecting_; }
  // The other cars are collected in this frame (and under which limits).
  bool others_on() const { return others_on_; }
  const car_rt_instances::Limits& other_limits() const { return other_limits_; }
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
    bool pending = false;  // the key was met and the box is not read yet (a frame reads a few at most)
    uint64_t seen = 0;   // the guest frame it was last used in
  };
  // What a frame's log line says, to log only a change.
  struct Summary {
    bool valid = false;
    uint32_t id = 0;
    uint32_t draws = 0, body = 0, skinned = 0;
    uint32_t other_cars = 0, other_draws = 0;
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
    // The mesh keys (car_frame_rules.h ContentCache): how many keys this frame had never been made before, and what the fingerprints
    // behind the keys did.
    uint32_t new_keys = 0;         // keys not in the box cache: a mesh seen for the first time, or one whose own bytes changed
    uint32_t content_new = 0;      // fingerprints made for a range not fingerprinted before (a new mesh)
    uint32_t content_changed = 0;  // ... made again because the stamp moved, and the bytes had changed: another key
    uint32_t content_same = 0;     // ... made again because the stamp moved, and the bytes were the same: the key stayed
    uint64_t content_bytes = 0;    // guest bytes hashed or scanned for them
    // The other cars (RequestOtherCars): draws of IDs that are not the player's, while they are collected, and what became of them.
    uint32_t others_seen = 0;
    uint32_t others_far = 0;          // ... of a car that is not kept: beyond the radius, or more cars nearer than the limit
    uint32_t others_listed = 0;
    uint32_t others_small = 0;        // ... under Limits::min_indices
    uint32_t others_skinned = 0;      // ... a driver (not in the scene)
    uint32_t others_no_triangles = 0; // ... lines, a computed fetch, no POSITION
    uint32_t others_not_finite = 0;
    uint32_t others_deferred = 0;     // ... whose mesh has no fingerprint yet and the frame's budget for them was spent: it joins in a later frame
    uint32_t others_no_key = 0;       // ... with no mesh key at all (inline vertices, a stream that cannot be resolved)
    uint32_t others_evicted = 0;      // cars given up for a nearer one
  };
  // What the keys did since the last [car frame] keys line.
  struct KeyWindow {
    uint64_t since = 0;            // the guest frame the window began after (0: nothing logged yet)
    uint32_t frames = 0;           // frames collected in it
    uint32_t frames_with_new = 0;  // ... that had a new key
    uint32_t max_new = 0;          // the most new keys in one of them
    uint32_t new_keys = 0, content_new = 0, content_changed = 0, content_same = 0;
    uint64_t content_bytes = 0;
    bool Active() const { return new_keys != 0 || content_changed != 0 || content_same != 0; }
  };
  // The fingerprint of the draw's own indices (what ComputeMeshKey takes from the guest): the number, and the smallest and largest index.
  struct IndexInfo {
    uint64_t content = 0;  // 0: not known
    bool have_range = false;
    uint32_t lo = 0, hi = 0;
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
    bool ReadTexcoords(std::vector<float>* uvs) const override;
    CarFrame* owner = nullptr;
  };

  void StartFrame();
  void FinishFrame();
  void OnVehicleDraw(const MainDraw& d, uint32_t order, uint32_t id);
  void HandleVehicleDraw(const MainDraw& d, uint32_t order, uint32_t id);
  // A draw of another car (not the player's ID): the selection, then the recipe, the matrix and the key as the player's draws get them.
  void HandleOtherDraw(const MainDraw& d, uint32_t order, uint32_t id, double distance, const uint8_t* bank);
  void DropOtherCar(uint32_t id);
  void OnConfirmed(uint32_t id, bool switched);
  void SyncPlayer();
  bool HasTriangles(const MainDraw& d) const;
  const LayoutInfo& LayoutOf(const InputLayout& layout);
  const RecipeEntry& RecipeOf(const MainDraw& d);
  void FillDraw(const MainDraw& d, uint32_t order, uint32_t id, const RecipeEntry& recipe, const LayoutInfo& layout,
                const car_frame_rules::ModelDecode& model, car_frame::Draw* out);
  void FillPalette(const ConstantBanks::Banks& banks, std::vector<float>* out);
  uint64_t ComputeMeshKey(const MainDraw& d, const LayoutInfo& layout);
  IndexInfo IndexContent(const MainDraw& d, uint32_t address, uint32_t bytes, uint32_t count, bool index32, uint32_t endian, bool order_free,
                         bool skip_reset, uint32_t reset_index);
  uint64_t StreamContent(const MainDraw& d, const car_mesh::StreamRef& stream, const car_frame_rules::VertexSpan& span);
  void NoteOutcome(car_frame_rules::ContentCache::Outcome outcome);
  void LogKeys();
  std::string OthersText() const;
  void NoteBox(car_frame::Draw* out);
  void NoteCamera(const uint8_t* vs_bank, const temporal::Mat4& world);
  void AddToBounds(const car_frame::Draw& draw);
  void CallListeners();
  bool LoadMesh();
  bool ReadForListener(std::vector<float>* positions, std::vector<uint32_t>* indices, std::vector<float>* normals);
  bool ReadTexcoordsForListener(std::vector<float>* uvs);
  // RT-HIT-SHADING: the material of a vehicle draw (its shader's class, constants and diffuse fetch) and the frame's light and environment map slots from it.
  car_rt_material::Material MaterialOf(const MainDraw& d) const;
  void NoteLight(const MainDraw& d, const car_rt_material::Material& material);
  void LogUnreadable(const char* what);
  void ReleaseDraws(std::vector<car_frame::Draw>& draws);
  std::vector<float> TakePalette();

  bool requested_ = false;
  bool collecting_ = false;
  car_frame::Frame frame_;
  // The debug walk of the cars' own model data against this frame's draws (car_model_walk.h, mcla_gpu_car_model_walk): reads only, idle while the setting is 0.
  car_model_walk::Walker walk_;
  car_model_rt::Mode model_mode_next_ = car_model_rt::Mode::kOff;  // asked for in the frame before (RequestOtherCars)
  uint32_t traffic_cars_next_ = 0;   // RT-MODEL-PARTS: asked for with RequestOtherCars
  bool wheels_next_ = true;

  GuestMemory* memory_ = nullptr;
  std::vector<car_frame::Listener*> listeners_;
  Reader reader_;
  uint64_t guest_frame_ = 0;  // guest presents seen

  // The frame being collected.
  car_frame_rules::IdentityTracker tracker_;
  uint32_t previous_id_ = 0;           // the player's ID in the frame before (0: none)
  bool player_read_ = false;           // the player's vehicle was read in this frame
  car_mesh::PlayerVehicle player_;
  bool vehicles_read_ = false;         // the game's other vehicles were read in this frame (others_on_ only)
  car_mesh::VehicleScan vehicle_scan_;
  // The other cars (RequestOtherCars): asked for in a frame, collected in the next, under the limits of the request.
  bool others_requested_ = false;
  car_rt_instances::Limits other_limits_next_;
  car_rt_instances::Limits other_limits_;
  bool others_on_ = false;
  car_rt_instances::CarSelector selector_;
  uint64_t content_limit_ = 0;   // the bytes of fingerprints the draw being handled may make for a new mesh (0: no limit: the player's draws)
  uint64_t content_spent_ = 0;   // ... what the frame's other cars' draws have made so far
  bool content_blocked_ = false; // a fingerprint was wanted that the budget did not allow: the draw's key is not made (it waits)
  car_frame::Draw scratch_draw_; // the draw the MeshReader works for while another car's draw is handled (its part, its keys, its shaders)
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
  car_frame_rules::ContentCache contents_;  // the fingerprints of the meshes' own bytes, by range, each with the stamp it was made at
  KeyWindow key_window_;
  std::unordered_set<uint64_t> unreadable_logged_;
  std::vector<std::vector<float>> palette_pool_;
  std::vector<float> palette_cache_;
  const ConstantBanks::Banks* palette_cache_banks_ = nullptr;
  uint64_t palette_cache_version_ = 0;
  std::vector<ShaderStore::VertexElement> elements_;
};

}  // namespace mcla::gpu
