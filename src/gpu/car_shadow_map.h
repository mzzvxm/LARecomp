#pragma once
// ===========================================================================
// MCLA GPU renderer -- a sun shadow map of the player's car alone
// ===========================================================================
// The game's sun shadow map holds the player's car at a forced low level of detail in cascades whose texels are 1 to 25 cm:
// the wing, the mirrors, the sills and the wheels cast no shadow of their own shape on the car or on the road near it, and
// the screen-space contact shadows (ao_contact_ps.hlsl) cannot see a part's thickness. This map draws the player's car again,
// depth only, from the sun, at full detail, with the geometry its own main-view draws used (car_frame.h), into a small square
// map fitted to the car. The contact shadows pass reads it.
//
// car_shadow_map.cpp makes the map (the replay, the fit, the debug view, the setting); the AO's contact pass reads View()
// (ao.cpp, hlsl/ao_contact_ps.hlsl).
//
// WHAT IT DOES, and when (the details are in car_shadow_map.cpp and car_shadow_map_rules.h):
//   mcla_gpu_car_shadow_map        auto | off | high | ultra: the map's size, 2048 (ultra) or 1024 (high); `auto` is ultra on a
//                                  discrete graphics card and high on an integrated one (the Low class). Live: a change applies
//                                  at the next frame. It only runs while the contact shadows do (mcla_gpu_contact_shadows is not
//                                  off), in the game view, with the player's car found (CarFrame) and the sun at least 3 degrees up.
//   mcla_gpu_car_shadow_map_view   debug only: 1 draws the map in a square in the lower left corner of the picture.
// AT THE HISTORY TRIGGER (the game's mid-scene depth copy into 078B0000, which is also the ambient occlusion's trigger and
// comes BEFORE its passes: Scene::OnResolve calls OnResolve here first), with the frame's CarFrame in hand, one depth-only
// pass redraws the car's rigid draws (car_frame.h Part::kBody) from the sun: the draw's own host vertex view (the byte-swapped
// copy its main-view draw read), POSITION only, one push block of 64 bytes a draw (the model -> map clip matrix), the draw's
// own topology, index view, count and base vertex, no pixel shader, slope scaled depth bias in the pipeline. THE SKINNED DRIVER IS
// LEFT OUT (car_frame.h Part::kSkinned: its blend is per vertex, and the shadow of a head is the game's cascade's business).
// The pipelines (one per position format and stride: the census saw float3 at strides 12, 24, 28, 32 and 36) are made ahead on
// the host builder thread where the setting is on at the start, and in the background otherwise or for a stride nobody listed
// (HostPrecache::AddLive); a frame whose draws need a pipeline that is not made yet records no map (View().valid is false), it
// never waits for a compile.
//
// THE FIT (car_shadow_map_rules.h): an orthographic view along the sun, square, centred on the car and turned with it, the
// centre and radius held from frame to frame, so a straight drive rasterises the car onto the same texels in every frame.
//
// Threading: render thread only. Initialize and Shutdown on the lifecycle thread. The pipelines are made on the host builder thread.
// ===========================================================================

#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <plume_render_interface.h>

#include "car_shadow_map_rules.h"
#include "edram.h"
#include "host_passes.h"
#include "records.h"
#include "resolve.h"

namespace mcla::gpu {

class AmbientOcclusion;
class BindlessHeap;
class CarFrame;
class Device;
class FrameRing;
class PassTiming;

namespace car_frame {
struct Draw;
}

struct CarShadowMapView {
  bool valid = false;           // a map of THIS frame's player car was recorded on the list before the AO's passes
  uint32_t slot = 0xFFFFFFFFu;  // bindless texture slot of the map: one float channel, depth from the sun (0 nearest the sun,
                                // 1 farthest), 1 where no part of the car is. A reader treats a stored 1 as "no car here" BEFORE it
                                // compares: a point behind the car's far plane has a depth above 1, and (1 < depth - bias) would
                                // shadow it in an empty texel. A point beyond the far plane under a texel that holds a car is
                                // behind the car, and shadowed.
  uint32_t size = 0;            // texels per side (square)
  // VIEW space (the AO's: x right, y up, z forward = the linear depth t; ao_common.hlsli AoViewPos) -> the map:
  //   u = dot(float4(p, 1), to_map[0]), v = dot(float4(p, 1), to_map[1]), depth = dot(float4(p, 1), to_map[2]),
  // u and v in [0, 1] across the map (v down), depth in [0, 1] as stored. A point is in the car's shadow when the stored depth
  // at (u, v) is smaller than its own depth by more than a bias.
  float to_map[3][4] = {};
  // The same three rows in a bindless 4x1 R32G32B32A32_FLOAT texture (texel k = to_map[k], texel 3 = (texel_m, depth_m, size,
  // the world's shadow filter: contact_trace.h CarMapFilterCode)), for a pass whose push block has no room for a matrix. 0xFFFFFFFF: none.
  uint32_t matrix_slot = 0xFFFFFFFFu;
  float texel_m = 0.0f;  // a texel's side in metres
  float depth_m = 0.0f;  // the map's depth range in metres (a stored depth difference times this = metres)
};

class CarShadowMap {
 public:
  // The destination of the game's mid-scene depth copy in the retail build (car_selfrefl.h, ao.h, shafts.h): another build gives
  // another address and then there is no map, never a wrong one.
  static constexpr uint32_t kTriggerDestination = 0x078B0000;

  CarShadowMap();
  ~CarShadowMap();
  CarShadowMap(const CarShadowMap&) = delete;
  CarShadowMap& operator=(const CarShadowMap&) = delete;

  // Makes the shaders and the pipeline layout, and lists the pipelines with the host precache (before the Renderer starts it) where
  // the setting is on at the start (the contact shadows too); where it is off nothing is compiled until it is switched on. Never
  // fails the renderer: without the shaders there is no map.
  void Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, EdramTargets& edram,
                  PassTiming& pass_timing);
  // GPU idle and the FrameRing drained, before HostPasses shuts down.
  void Shutdown();

  // Where the player car's draws come from (car_frame.h). The Renderer owns the CarFrame; null: no map.
  void SetCarFrame(CarFrame* car_frame) { car_frame_ = car_frame; }
  // The ambient occlusion, for the world's sun direction (AmbientOcclusion::SunDirection): the car's own draws carry a light of their own
  // (at 17.7 h it is 2 degrees BELOW the horizon while the world's sun is well above it), so car_frame::Frame::to_sun is only the fallback.
  void SetAmbientOcclusion(const AmbientOcclusion* ao) { ao_ = ao; }

  // The Scene opened a frame's first list: reads the settings, asks the CarFrame for the next frame's draws while a map is wanted.
  void BeginFrame();
  // The game's motion blur draw (the anchor) and the composite marker: a depth copy after either is not the trigger.
  void OnAnchor() { anchor_seen_ = true; }
  void OnComposite() { composite_seen_ = true; }
  // A resolve, after ResolveCache::Execute and BEFORE the ambient occlusion's OnResolve (which records its passes at the same
  // resolve): the history trigger is where the map is recorded. True when passes were recorded (the caller's list state is gone).
  bool OnResolve(const ResolveRecord& record, const ResolveCache::Summary& summary, plume::RenderCommandList* list);

  // The map of this frame; valid == false when none was recorded (cleared at BeginFrame).
  const CarShadowMapView& View() const { return view_; }

  // The video memory the map and the matrix texture hold now.
  uint64_t HeldBytes() const;

 private:
  // The depth map: a D32 float + S8 target (the format the renderer's own depth targets use, whose depth plane Plume views as one float
  // on both backends), a view in a bindless slot, a framebuffer of the depth alone.
  struct Map {
    // Release order matters as in HostImage (host_passes.h): the view reads its texture when it is destroyed (Vulkan), and the defaulted
    // move assignment (`map_ = Map{}`) freed the texture first. Assignment destroys this map (the destructor's order: framebuffer, view,
    // texture), then move-constructs in place.
    Map() = default;
    Map(Map&&) noexcept = default;
    Map& operator=(Map&& other) noexcept {
      if (this != &other) {
        this->~Map();
        new (this) Map(std::move(other));
      }
      return *this;
    }
    ~Map() = default;

    std::unique_ptr<plume::RenderTexture> texture;
    std::unique_ptr<plume::RenderTextureView> view;
    std::unique_ptr<plume::RenderFramebuffer> framebuffer;
    uint32_t slot = 0xFFFFFFFFu;
    uint32_t size = 0;
    plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
  };
  // One draw of the replay: the car's draw and the pipeline of its position variant.
  struct Item {
    const car_frame::Draw* draw = nullptr;
    plume::RenderPipeline* pipeline = nullptr;
  };

  // The keys below the depth variants' (VariantKey is at least 256: a stride of 1 byte and over): 0 the matrix texture's pass, 16 + n
  // the debug view's for n host samples.
  static constexpr uint32_t kMatrixVariant = 0;
  static constexpr uint32_t kViewVariantBase = 16;

  bool IsTrigger(const ResolveRecord& record, const ResolveCache::Summary& summary) const;
  bool MakeShaders();
  // The key of the pipeline for a variant (host precache), and what makes it.
  static uint64_t PrecacheKey(uint32_t variant);
  std::unique_ptr<plume::RenderPipeline> MakePipeline(uint32_t variant);
  std::unique_ptr<plume::RenderPipeline> MakeDepthPipeline(uint32_t variant);
  std::string VariantName(uint32_t variant) const;
  // Lists what the start's setting needs with the precache (Add), or, on a later switch on, in the background (AddLive).
  void ListBase(bool at_start, const std::string& note);
  // The pipeline of `variant` if it is made; else null with *pending set (it is listed, or being made), or null (it failed).
  plume::RenderPipeline* Pipeline(uint32_t variant, bool* pending);
  // Hands the variants asked for this frame that nobody had listed to the precache's background builder.
  void FlushListing();

  bool EnsureMap(uint32_t size);
  bool EnsureMatrix();
  void ReleaseImages();
  void RecordMap(const car_shadow_map::Fit& fit, plume::RenderCommandList* list);
  bool RecordView(const ResolveCache::Summary& summary, plume::RenderCommandList* list);
  plume::RenderPipeline* ViewPipeline(uint32_t host_samples);
  void NoteSkipped(car_shadow_map::Why why);
  void Census();

  // The frame's decisions (BeginFrame).
  car_shadow_map::Choice choice_;
  bool contact_on_ = false;
  bool wanted_ = false;  // the setting and the contact shadows are on: the CarFrame is asked for draws
  int view_mode_ = 0;
  uint32_t idle_frames_ = 0;
  uint64_t frame_ = 0;

  // The frame.
  bool anchor_seen_ = false;
  bool composite_seen_ = false;
  bool trigger_seen_ = false;
  CarShadowMapView view_;

  // The car's fit, held from frame to frame.
  car_shadow_map::Held held_;
  // Whether the map alone casts the player's car's shadow on the world (car_shadow_map_rules.h Owner).
  car_shadow_map::Owner owner_;

  // The GPU side.
  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  HostPasses* passes_ = nullptr;
  EdramTargets* edram_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  CarFrame* car_frame_ = nullptr;
  const AmbientOcclusion* ao_ = nullptr;
  std::unique_ptr<plume::RenderPipelineLayout> layout_;  // no sets: a push block of 64 bytes (vertex), an input layout
  std::unique_ptr<plume::RenderShader> vertex_shader_;
  std::unique_ptr<plume::RenderShader> matrix_shader_;
  std::unique_ptr<plume::RenderShader> view_shader_;
  bool ready_ = false;
  bool shaders_failed_ = false;
  bool base_listed_ = false;  // the matrix pass and the known strides were listed (at the start, or in the background)
  std::unordered_map<uint32_t, std::unique_ptr<plume::RenderPipeline>> pipelines_;
  std::unordered_set<uint32_t> listed_;
  std::unordered_set<uint32_t> failed_;
  std::vector<uint32_t> to_list_;  // variants asked for this frame that nobody had listed
  Map map_;
  HostImage matrix_;  // R32G32B32A32_FLOAT, 4x1
  std::vector<Item> items_;

  // What the log has said once, and the totals.
  uint32_t last_level_logged_ = ~0u;
  uint32_t last_filter_logged_ = ~0u;
  uint32_t last_why_logged_ = ~0u;
  uint32_t size_logged_ = 0;
  bool pending_logged_ = false;
  bool unusable_logged_ = false;
  bool view_failed_logged_ = false;
  uint64_t triggers_ = 0;
  uint64_t maps_ = 0;
  uint64_t pending_frames_ = 0;
  uint64_t skipped_[8] = {};
  uint64_t draws_ = 0;
  uint64_t draws_unusable_ = 0;
  uint64_t draws_skinned_ = 0;
  uint32_t last_draw_count_ = 0;
  uint32_t last_skinned_ = 0;
};

}  // namespace mcla::gpu
