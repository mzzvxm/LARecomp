#pragma once
// ===========================================================================
// MCLA GPU renderer -- the city in the car paint's reflections: the host side (CITY-RT M2 packages R1 and R2)
// ===========================================================================
// city_refl_rules.h has the design, the levels and the numbers; hlsl/city_refl.hlsli and hlsl/car_rt_refl_cs.hlsl the shader. This class is what the pass needs from the host that is not the pass
// itself (car_rt_refl.h records that, and the opponents' variant, car_rt_opp_refl.h, the same):
//
//   THE SETTING  mcla_gpu_rt_reflections (auto, off, high, ultra; the pause menu's RT CITY REFLECTIONS row since R8, city_refl_setting.h; live): BeginFrame resolves it to a Cell (city_refl_setting.h Decide,
//                city_refl_rules.h LevelFor, CellFor: `auto` is rt_tier.h's cell for kRtReflections, off without ray tracing and on a weak card, high on a strong one) and tells the scene service
//                what the pass needs (Want(kCityReflections): the cars, the city's structures and hit data at the level, every texture) and CarRtRefl what the screen owner must make (ScreenProducts:
//                colour_post with its mip chain and lin_depth). The wants go out as soon as the word comes to something above off; the pass itself (the cell, the rows, the screen products) waits until
//                the city's data stands (city_refl_setting.h Data: the pack's and the hit file's probes, published by the two scenes; the game builds the files in the background when they are missing).
//   THE ROWS     a set of its own (set 3 of the pass's layout: t0 the constants rows, city_refl_rules.h kRows; u1 the counters), a copy for each frame slot, the rows written by Prepare at the
//                history trigger: the level, the picture's slots and sizes, the reach, the class gains, the debug view. An off frame writes zero rows (the shader sees flags 0 and the pass is as it was).
//   THE CENSUS   the counters of a stats frame (the same frames as the cars' census: one in twenty) are read back; the calibration's part moves the class gains (rt_hit_calibration.h ClassGains, one a class of
//                the G-buffer's, 0.5 to 3, in force from the frame after the window closes) and the census lines the report reads (perf/city-rt-m2/README.md):
//                  [city refl] frame N: level high, reach 150 m, texels T, rays R, city C, car K, miss M, screen S, shaded H, lamps L
//                  [city refl] gain frame N: solid 1.20, ground 0.95, glass 1.10, emissive 1.40, samples 5120
//                every 120 frames while the pass runs and at its first stats frame, each number the mean of a stats frame of the window.
//
//   CHROME AND GLASS (CITY-RT M2 R3): the player's chrome takes the chrome variant's city stage (shaderpack/variants/car_chrome_city.h) while ChromeWanted() says so: a level above off, the pass active this
//                frame and mcla_gpu_city_refl_chrome on (pipelines.cpp WantedFeatures reads it at every draw; the flag is set here, at the frame's start, like the paint's CarSelfRefl::Selected). The glass is a pass of its own
//                (car_rt_glass_refl.h) that asks GlassWanted() of the same cell and mcla_gpu_city_refl_glass.
//
//   THE LAMPS    (REFL-LAMPS) a shaded city hit is lit by the world frame's lamp grid and what glows glows by its schedule (the paint, the other cars' paint and the glass in one shader file; the chrome reads
//                the paint's image): the rows' kFlagLamps from mcla_gpu_city_refl_lamps, nothing new to bind (t10 and t11 are in the car scene's 12 view set), the lamp term's counters in the city's counter buffer and
//                one more census line `[city refl] lamp term frame N: ...` (city_refl_rules.h LampCensusText).
//
//   THE LIGHT    (CITY-M2-OPEN) a city hit is shaded with the WORLD frame's light (the sun's direction and colour, the ambient, the sky and the environment map of the main view's draws: t10's rows 0 to 31), not
//                with the constants of the cars' draws (the pass's own frame rows, whose sun is a rig of its own): the rows' kFlagWorldLight from mcla_gpu_city_refl_world_light, used where the world rows are valid and
//                hold a light (city_refl_rules.h WorldLightFor), else the cars' frame as before. Five more counters and one more census line `[city refl] light frame N: ...` (LightCensusText) say how the shaded
//                hits fared: sun reached, blocked, facing away, behind the camera, the world's light taken.
//
// DEBUG SETTINGS (all live): mcla_gpu_city_refl_world_light (false: the cars' draws' light on a hit, as before CITY-M2-OPEN), mcla_gpu_city_refl_lamps (false: no lamp light and the sun's glow schedule on a hit, as before REFL-LAMPS), mcla_gpu_city_refl_chrome and mcla_gpu_city_refl_glass (false: the chrome / the glass as before, the A/B of R3), mcla_gpu_city_refl_screen (false: the picture's colour is never the answer, every hit is shaded: the A/B of the shortcut), mcla_gpu_city_refl_coat (false: a city hit is a soft
// cone's hit as before, not a mirror's and the coat layer's), mcla_gpu_city_refl_reach (the cell's reach in metres, 0: the level's), mcla_gpu_city_refl_view (1 the hit's source, 2 its class, drawn in the
// paint through mcla_gpu_car_selfrefl_view 2, the colour the ray sees).
//
// CHECK (nothing here was run): the set's registers on both APIs (t0 and u1 in space 3, the push block in space 4); the D3D12 descriptor count of the set (2 views: the lamp term's counters are in u1's buffer,
// no view added; the car scene's set the pass binds as set 2 holds t10 and t11 since CITY-RT FRAME: 12 views); the upload buffers' reads by the
// pass; the screen images' layouts at the trigger (SHADER_READ, as the car history's own read of colour_post: a barrier to the same layout is recorded); the first stats frame's readback; the lag of the
// screen wants (one frame: the first frame after a switch resolves by the model alone).
//
// Threading: the render thread; Initialize and Shutdown on the lifecycle thread.
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include <plume_render_interface.h>

#include "city_refl_rules.h"
#include "city_refl_setting.h"
#include "frame.h"
#include "rt_hit_calibration.h"
#include "screen_frame.h"

namespace mcla::gpu {

class Device;
class RtSceneService;

// CITY-RT M2 R3a: whether the player's chrome draws of this frame take the chrome variant's city stage (pipelines.cpp WantedFeatures). Set at the frame's start by CityRefl::BeginFrame (false until a frame has
// begun, false while the city is not in the reflections or mcla_gpu_city_refl_chrome is off); read by the draw recorder, any thread.
bool CityReflChromeWanted();

class CityRefl {
 public:
  // The pass's layout puts the city's set after the scene's; the push block moved to space 4.
  static constexpr uint32_t kSetIndex = 3;
  static constexpr uint32_t kPushSpace = 4;

  CityRefl() = default;
  ~CityRefl();
  CityRefl(const CityRefl&) = delete;
  CityRefl& operator=(const CityRefl&) = delete;

  // The picture's products at the trigger, as the screen owner published them (CarSelfRefl fills it): lin_depth level 0 and colour_post's chain.
  struct ScreenIn {
    bool valid = false;
    uint32_t depth_slot = 0xFFFFFFFFu;
    uint32_t depth_width = 0, depth_height = 0;
    uint32_t colour_levels = 0;
    uint32_t colour_width = 0, colour_height = 0;
    uint32_t colour_slots[city_refl::kMaxColourLevels] = {};
    const plume::RenderTexture* depth = nullptr;                                     // to bring them to the shader-read layout before the pass reads them
    const plume::RenderTexture* colour[city_refl::kMaxColourLevels] = {};
  };

  // What a frame's pass does with the city.
  struct Frame {
    plume::RenderDescriptorSet* set = nullptr;  // set 3 (never null once the object is ready: an off frame has zero rows)
    bool on = false;
    city_refl::Cell cell;
    bool screen = false;                        // the picture's products are in the rows
    bool stats = false;
  };

  // The set's builder exists from Initialize (the pass's layout takes it).
  const plume::RenderDescriptorSetBuilder& Builder() const { return set_builder_; }
  bool Initialize(Device& device, FrameRing& frames);
  void Shutdown();
  bool ready() const { return ready_; }
  void SetService(RtSceneService* service) { service_ = service; }

  // The frame's start (CarRtRefl::BeginFrame): `pass_active` is the pass's own want (the ray traced reflection is on and can be recorded). Reads the setting, asks the scene service for
  // what it needs.
  void BeginFrame(bool pass_active, bool one_ray = false);
  // What the pass asks the screen owner for (screen_frame.h products), 0 while off.
  uint32_t ScreenProducts() const;
  bool on() const { return cell_.level != city_refl::Level::kOff && pass_active_; }
  // CITY-RT M2 R3b: the player's glass reflects the city this frame (the city is on at a level that serves the glass and mcla_gpu_city_refl_glass is on).
  bool glass_wanted() const { return glass_wanted_; }
  const city_refl::Cell& cell() const { return cell_; }
  // The level the cell came to for this frame's setting, the debug cells included (a log and a test read it).
  city_refl::Level level() const { return cell_.level; }

  // At the history trigger (once a frame): the rows of this frame slot written, the set to bind. `stats_frame`: the counters count and are read back.
  Frame Prepare(uint32_t frame_slot, const ScreenIn& screen, uint64_t frame, bool stats_frame);
  // Around the dispatches of a stats frame: the counters zeroed before, copied to the readback after (the history frame is `frame`).
  void StatsBegin(plume::RenderCommandList* list);
  void StatsEnd(plume::RenderCommandList* list, uint64_t frame);
  // Brings the picture's images to the layout the pass reads them in (a barrier to the layout they are in: harmless).
  static void BarrierScreen(plume::RenderCommandList* list, const ScreenIn& screen);

  uint64_t HeldBytes() const { return uint64_t(city_refl::kBufferBytes) * kFramesInFlight + (stats_ ? city_refl::kStatsBytes : 0u); }
  // The gains in force (rt_hit_calibration.h): the census line and the tests read them.
  const rt_hit_cal::ClassGains& gains() const { return gains_; }

 private:
  struct Slot {
    std::unique_ptr<plume::RenderBuffer> rows;
    std::unique_ptr<plume::RenderDescriptorSet> set;
  };
  bool EnsureSlot(uint32_t frame_slot);
  void NoteGate(const city_refl_setting::Decision& decision);
  void ProcessStats(uint64_t frame, uint64_t generation);

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  RtSceneService* service_ = nullptr;
  bool ready_ = false;
  plume::RenderDescriptorSetBuilder set_builder_;  // t0 the rows, u1 the counters
  std::array<Slot, kFramesInFlight> slots_;
  std::unique_ptr<plume::RenderBuffer> stats_;           // u1: city_refl::kStatsWords counters, zeroed before a stats frame
  std::unique_ptr<plume::RenderBuffer> stats_zero_;
  std::unique_ptr<plume::RenderBuffer> stats_readback_;
  bool stats_busy_ = false;
  uint64_t stats_generation_ = 0;

  // The frame.
  city_refl::Cell cell_;
  bool pass_active_ = false;
  bool glass_wanted_ = false;
  bool wanted_logged_ = false;
  int last_gate_state_ = -1;  // the last decision the log said (gate, data, level): one line a change
  city_refl::Level last_level_ = city_refl::Level::kOff;  // the census lines name the level the pass last ran at
  float last_reach_ = 0.0f;

  // The census and the calibration.
  city_refl::Totals window_;
  rt_hit_cal::ClassGains gains_;
  uint32_t stats_since_close_ = 0;
  uint64_t samples_window_ = 0;
  uint64_t last_log_frame_ = 0;
  bool logged_ever_ = false;
  bool slot_failed_logged_ = false;
  bool screen_logged_ = false;
  bool no_screen_logged_ = false;
  uint64_t first_logged_frame_ = 0;
};

}  // namespace mcla::gpu
