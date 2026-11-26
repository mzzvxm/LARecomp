// MCLA GPU renderer -- the renderer and the module entry points. See
// renderer.h and gpu.h.

#include "renderer.h"

#include "mc_engine/cinematic/capture_mode.h"
#include "settings/immortal.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <format>
#include <memory>
#include <mutex>
#include <string>

#include "settings/larecomp_settings.h"
#include "render_wait_rules.h"
#include <rex/logging/log_category.h>
#include <rex/kernel/title_runtime.h>
#include <rex/platform/native_window.h>
#include <rex/platform/process.h>
#include <rex/ui/ui_system.h>

#include "backend.h"
#include "device_fault_format.h"
#include "display_refresh.h"
#include "gpu.h"
#include "gi_setting.h"
#include "citypack/pack_format.h"
#include "guest_capture.h"
#include "hdr.h"
#include "native_gfx/guest/guest_fence.h"
#include "native_gfx/guest/occlusion.h"
#include "native_gfx/guest/vblank_probe.h"
#include "native_gfx/nocp/guest_gpu.h"
#include "native_gfx/nocp/nocp_driver.h"
#include "renderer_choice.h"
#include "choice_box.h"
#include "renderer_fallback.h"
#include "rt_bench.h"
#include "rt_tier.h"
#include "shutdown_rules.h"
#include "native_window.h"
#include "ray_regen.h"
#include "temporal_dlss.h"
#include "thread_load.h"
#include "latency.h"  // after the others: keeps this hunk apart from the neighbouring includes
#include "mc_engine/display/display_mode.h"
#include "mc_engine/display/ultrawide.h"
#include "watch.h"


namespace {

// D3DDevice_BlockOnFence calls that found their fence pending, and the time
// they waited (gpu.h NoteGuestFenceWait); taken by the stats line.
std::atomic<uint64_t> g_fence_waits{0};
std::atomic<uint64_t> g_fence_wait_ns{0};
// D3DResource_Lock calls, and those on a resolve destination (gpu.h
// OnGuestLock); taken by the stats line.
std::atomic<uint64_t> g_guest_locks{0};
std::atomic<uint64_t> g_guest_locks_on_destinations{0};
// The swap chain's size (width << 32 | height), set at Initialize and after
// each rebuild, for gpu.h PreviewRenderScale; 0 before.
std::atomic<uint64_t> g_window_size{0};

// gpu.h SetPresentObserver.
std::atomic<mcla::gpu::PresentObserver> g_present_observer{nullptr};
// gpu.h SetNextPresentTarget: the due time OnGuestSwap hands the next present.
std::atomic<int64_t> g_next_present_target{0};
// gpu.h SetNextPresentTag (capture mode): the tag OnGuestSwap hands the next present.
std::atomic<uint64_t> g_next_present_tag{0};

void PublishWindowSize(uint32_t width, uint32_t height) {
  g_window_size.store(uint64_t(width) << 32 | height, std::memory_order_relaxed);
}

// The folder the app wants the shader packs in (gpu.h SetShaderPackFolder); empty: the default.
std::filesystem::path& g_shader_pack_folder = larecomp::Leak<std::filesystem::path>();
// The app's say on attaching the packs that are in place at Startup (gpu.h SetShaderPackGate).
std::function<bool()>& g_shader_pack_gate = larecomp::Leak<std::function<bool()>>();
// The app's say on the city pack (gpu.h SetCityPackHooks): set before Startup, read on the render thread. Never destroyed (EXIT-HARD), like the gate above.
mcla::gpu::CityPackHooks& g_city_pack_hooks = larecomp::Leak<mcla::gpu::CityPackHooks>();

// Names the renderer teardown's slow stages. About one test run in 90 took 2.7 to 4.7 s to get from "pipelines: ... written" to
// "renderer shut down" (the 2026-11-21 stack runs: all of them with another game on the same GPU, none of 103 alone), and the 5 s close
// watchdog (larecomp_app.cpp) ended two of them; no line said which stage waited. Stage(what) is called where a stage BEGINS: the stage
// before it ends there and is logged when it took kSlowMs or more. Done() ends the last one. One thread uses it (Shutdown's).
//
// It also publishes where the shutdown is (gpu.h ReadShutdownProgress), for the close watchdog: a close whose renderer is moving, or is inside
// the driver's own teardown call, is not hung (close_watchdog.h, VK-SLOW-CLOSE). The published values are plain atomics read by one other
// thread; a read that catches a stage change half done names a neighbouring stage, which the watchdog's line can afford.
constexpr int kShutdownNotStarted = 0;
constexpr int kShutdownRunning = 1;
constexpr int kShutdownFinished = 2;
std::atomic<int> g_shutdown_state{kShutdownNotStarted};
std::atomic<const char*> g_shutdown_stage{nullptr};  // static text
std::atomic<bool> g_shutdown_driver_call{false};
std::atomic<int64_t> g_shutdown_moved{0};  // steady_clock ticks of the last change of stage

int64_t SteadyTicks() {
  return std::chrono::steady_clock::now().time_since_epoch().count();
}

void WaitForPresentDeadline(int64_t due) {
  if (mcla::gpu::render_wait::ShouldSpin(larecomp::settings::render_wait_mode.Get())) {
    mcla::gpu::WaitUntilQpc(due);
  } else {
    mcla::gpu::BlockUntilQpc(due);
  }
}

class ShutdownClock {
 public:
  ShutdownClock() { Publish(kShutdownRunning, nullptr, false); }

  // `driver_call`: the stage is one call into the graphics driver, which returns by itself (the device's own release), so the close
  // watchdog gives it more time than a stage of ours that may be waiting for something that never comes.
  void Stage(const char* what, bool driver_call = false) {
    End();
    current_ = what;
    Publish(kShutdownRunning, what, driver_call);
  }

  void Done() {
    End();
    current_ = nullptr;
    Publish(kShutdownFinished, nullptr, false);
  }

 private:
  void End() {
    const auto now = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_).count();
    if (current_ && ms >= kSlowMs) {
      REXLOG_INFO("[gpu] shutdown: {} took {} ms", current_, ms);
    }
    last_ = now;
  }

  static void Publish(int state, const char* stage, bool driver_call) {
    g_shutdown_stage.store(stage, std::memory_order_relaxed);
    g_shutdown_driver_call.store(driver_call, std::memory_order_relaxed);
    g_shutdown_moved.store(SteadyTicks(), std::memory_order_relaxed);
    g_shutdown_state.store(state, std::memory_order_release);
  }

  static constexpr int64_t kSlowMs = 250;
  const char* current_ = nullptr;
  std::chrono::steady_clock::time_point last_ = std::chrono::steady_clock::now();
};

}  // namespace

namespace mcla::gpu {

// ---------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------

Renderer::~Renderer() {
  Shutdown();
}

bool Renderer::Initialize(const rex::NativeWindow& window, const Options& options,
                          std::shared_ptr<OverlayChannel> overlay_channel,
                          std::shared_ptr<OverlayHost> overlay_host) {
  if (initialized_) {
    return true;
  }
  if (!overlay_channel || !overlay_host) {
    REXLOG_ERROR("[gpu] renderer: no overlay channel/host; the SDK overlays could not be drawn");
    return false;
  }
  plume::RenderWindow render_window{};
  if (!GetRenderWindow(window, render_window)) {
    return false;
  }
  overlay_host_ = std::move(overlay_host);
  // mcla_gpu_hdr: reads the setting and the display the window is on; the swap chain asks whether
  // to try HDR10 (hdr.h).
  hdr::Start(render_window);
  SwapChain::Options swap_options = options.swap_chain;
  swap_options.hdr10 = hdr::Wanted();
  // Each step needs the ones before it; on a failure everything already made
  // is torn down in reverse (each Shutdown is a no-op on a component that was
  // never initialized).
  bool ok = device_.Initialize(options.device) && frames_.Initialize(device_) &&
            heap_.Initialize(device_, frames_) && guest_layout_.Initialize(device_, heap_) &&
            shaders_.Initialize(device_);
  if (ok) {
    // A pack that is in place is attached now, as before the store could start without one;
    // with none the store stays pending until AttachShaderPacks (the packs are built from the
    // player's game files after the renderer, which draws the ISO wizard, is up). Until then a
    // guest draw is skipped (Scene: kVsNotInPack, counted by the store too).
    // Scene::CountSkip logs the first four draws skipped for want of a pack as errors, then counts
    // them; the app attaches the packs before the guest starts, so none are skipped in its flow.
    // The app may hold the attach back: packs whose stamp is stale are rebuilt first, and a pack
    // that is mapped here could not be replaced.
    if (!g_shader_pack_gate || g_shader_pack_gate()) {
      shaders_.AttachFirst(ShaderStore::SearchFolders(g_shader_pack_folder));
    } else {
      REXLOG_INFO("[gpu] shader pack: not attached at startup; the app checks and, where needed, "
                  "rebuilds it first");
    }
    samplers_.Initialize(device_, heap_);
    pipelines_.Initialize(device_, guest_layout_, shaders_);
    if (shaders_.attached()) {
      OnShaderPackAttached();
    }
    ok = upload_.Initialize(device_) &&
         geometry_.Initialize(device_, frames_, upload_, guest_memory_) &&
         // Before the EDRAM: the window's size can pick the render scale.
         swap_chain_.Initialize(device_, render_window, swap_options);
  }
  if (ok) {
    hdr::SetActive(swap_chain_.hdr());
    // The window's size: the swap chain's; a window with no area yet has none,
    // then what the window reports; with neither it measures as the game's
    // screen (1x).
    uint32_t width = swap_chain_.width();
    uint32_t height = swap_chain_.height();
    if (width == 0 || height == 0) {
      GetWindowPixelSize(window, width, height);
      if (width == 0 || height == 0) {
        REXLOG_WARN("[gpu] the window has no size yet: a render scale taken from it measures it "
                    "as the game's {}x{}",
                    kAssumedScreenWidth, kAssumedScreenHeight);
      }
    }
    // The picture's frame in the window (ASPECT RATIO): "the screen" the scale measures is its 16:9 area.
    const mc::frame::Rect frame = mc::ultrawide::FrameFor(width, height);
    render_scale_ = ChooseStartRenderScale(frame.w, frame.h);
    PublishWindowSize(width, height);
    edram_.Initialize(device_, frames_, heap_, render_scale_);
    // mcla_gpu_edram_capture: the detector asks for a RenderDoc capture.
    edram_.SetCaptureRequest([this] { capture_.Request(); });
    // mcla_gpu_watch_base: the diagnostic watch, and its capture.
    watch::Configure();
    watch::SetCaptureRequest([this] { capture_.Request(); });
    ok = passes_.Initialize(device_, frames_, heap_) &&
         resolve_memory_.Initialize(device_, frames_, heap_, upload_, guest_memory_, passes_,
                                    textures_, pass_timing_) &&
         resolve_.Initialize(device_, frames_, heap_, guest_memory_, textures_, edram_, passes_,
                             pass_timing_, resolve_memory_);
  }
  if (ok) {
    // The ambient occlusion lists its pipelines too (ao.h); it never fails the renderer.
    ao_.Initialize(device_, frames_, heap_, passes_, edram_, pass_timing_, resolve_);
    shafts_.Initialize(passes_, edram_, pass_timing_);  // the sun shafts too (shafts.h)
    volumetrics_.Initialize(device_, frames_, heap_, passes_, edram_, pass_timing_);  // and the volumetric light (volumetrics.h)
    volumetrics_.SetInputSource(&vol_inputs_);  // its street lamps and headlights are the producer's (volumetrics_inputs.h)
#if __has_include("local_shadow.h")
    vol_inputs_.SetLocalShadow(&local_shadow_);  // and the headlights are shadowed by the replay's maps (local_shadow.h)
#endif
    gbuffer_.Initialize(passes_, edram_, pass_timing_);  // and the G-buffer's sidecar (gbuffer.h): made on the first draw that carries it
    screen_.Initialize(device_, heap_, passes_, edram_, pass_timing_);  // and the shared screen products (screen_shared.h): the AO's depth chain published, the Hi-Z, made only while wanted
    screen_.ListChainAtStart(ao_.ChainKindsAtStart());  // the depth chain's and colour chain's pipelines the AO's start choices use: in the boot precache, as the AO's own table had them before SS-2 (SS-PRECACHE)
    ssr_.Initialize(device_, heap_, passes_, edram_, pass_timing_, &frames_);  // and the screen-space reflections (ssr.h): the layout and the shaders, the pipelines only when the setting is on
    rr_guides_.Initialize(device_, frames_, heap_, passes_, pass_timing_);  // and DLSS Ray Reconstruction's guide pass (rr_guides.h): the layouts and shaders; the pipelines only when it is first wanted
    rr_guides_.SetGBuffer(&gbuffer_);
    car_refl_.Initialize(passes_, edram_, pass_timing_);  // and the car self-reflection's history pass (car_selfrefl.h)
    car_dump_.Initialize(device_, frames_, guest_memory_);  // and the debug car dump (car_dump.h)
    car_refl_.SetCarDump(&car_dump_);
    car_frame_.Initialize(guest_memory_);  // and the player car's draws (car_frame.h), which the car effects ask for
    mirror_glass_.Initialize(car_frame_);  // and the side mirrors' glass finder, a listener of the car's draws (mirror_glass.h; debug only, idle while mcla_gpu_mirror_glass is off)
    headlights_.Initialize(guest_memory_);  // and the cars' headlights (headlight_rig.h), which the lit shaders' variant reads
    car_shadow_.Initialize(device_, frames_, heap_, passes_, edram_, pass_timing_);  // and the player car's sun shadow map (car_shadow_map.h)
    car_shadow_.SetCarFrame(&car_frame_);
    car_shadow_.SetAmbientOcclusion(&ao_);  // the world's sun direction
    mirror_view_.Initialize(device_, frames_, heap_, passes_, edram_, pass_timing_);  // and the shared rear view (mirror_view.h): nothing is made until mcla_gpu_mirror_view is on
    mirror_overlay_.Initialize(device_, frames_, heap_, passes_, edram_, pass_timing_);  // and the side mirrors' glass overlay (mirror_overlay.h): its pipelines are listed with the precache where mcla_gpu_mirror_overlay is on
    // The headlight shadow maps (local_shadow.h): a second pass over the main view's opaque draws from the cars' lamps; makes nothing until a draw is seen.
    local_shadow_.Initialize(device_, frames_, heap_, passes_, edram_, pass_timing_, upload_, shaders_, guest_layout_);
    // The one shadow atlas (shadow_atlas.h): the replay is its first provider and the light manager reads its published form; both use it only with mcla_gpu_light_frame bit 8 (LU-5).
    shadow_atlas_.Initialize(device_, frames_, heap_);
    local_shadow_.SetShadowAtlas(&shadow_atlas_);
    // The street lamps' shadows (lamp_shadows.h, perf/LAMP-SHADOWS-2.md): a second provider of the atlas, joined only while mcla_gpu_lamp_shadows is on and the game's lamp list has lamps (by night); the
    // replay draws their passes and the deferred light pass reads their views. Nothing runs with the setting off (the default).
    lamp_shadows_.SetShadowAtlas(&shadow_atlas_);
    local_shadow_.SetLampShadows(&lamp_shadows_);
    light_manager_.SetShadowAtlas(&shadow_atlas_);
    // The passive light manager (light_manager.h): off unless mcla_gpu_lights_census is set; it reads the rig's lights, the replay's maps and the volume's inputs and changes none of them.
    light_manager_.Initialize(upload_, headlights_, local_shadow_);
    // The one picker of the lights (light_pick.h, LIGHT-UNIFY U3): the rig's four lights and the cars that get shadow views are picked by the manager's picker (mcla_gpu_light_pick).
    headlights_.SetLightPicker(&light_manager_.Pickers());
    local_shadow_.SetLightPicker(&light_manager_.Pickers());
    local_shadow_.SetLightManager(&light_manager_);  // the replay picks from the manager's frame of lights when mcla_gpu_light_frame bit 4 says so (LIGHT-UNIFY U4c)
    vol_inputs_.SetLightManager(&light_manager_);
    // The deferred local light pass (local_lights.h): makes nothing until mcla_gpu_local_lights is on; reads the G-buffer's planes, the shared products' camera, the AO's pose and the manager's frame.
    local_lights_.Initialize(device_, frames_, heap_, passes_, edram_, pass_timing_);
    local_lights_.SetGBuffer(&gbuffer_);
    local_lights_.SetScreenShared(&screen_);
    local_lights_.SetAmbientOcclusion(&ao_);
    local_lights_.SetLightManager(&light_manager_);
    local_lights_.SetLampShadows(&lamp_shadows_);
    // The probe field's apply (gi_apply.h, CITY-RT M3 G3): makes nothing until a probe field runs (SetGiField: the integration, G4, gives it); reads the G-buffer's RT1 and RT2, the shared products' depth
    // and camera, and the AO's visibility, light and pose. The AO asks it for the screen-space bounce light's share (SetGiApply).
    gi_apply_.Initialize(device_, frames_, heap_, passes_, edram_, pass_timing_);
    gi_apply_.SetGBuffer(&gbuffer_);
    gi_apply_.SetScreenShared(&screen_);
    gi_apply_.SetAmbientOcclusion(&ao_);
    gi_apply_.SetCarFrame(&car_frame_);  // the player's box, for the cabin term (gi_cabin_rules.h)
    ao_.SetGiApply(&gi_apply_);
    // The probe field (gi_field.h, CITY-RT M3 G1): its layouts and shaders now, its pipelines listed with the precache below when the setting is not off at the start, its textures made when a frame
    // first wants them. The integration (gi_integration.h, G4) gives it the setting and the frame (its city scene is set below, with the city block); the apply reads it.
    gi_field_.Initialize(device_, frames_, heap_, passes_, pass_timing_, gi_setting::Current().level != gi::Level::kOff);
    gi_apply_.SetGiField(&gi_field_);
    // The player's car as ray tracing structures (car_rt_scene.h), where the device can trace rays inline; it builds nothing until an effect
    // asks for it. Initialized before the effects that use it (the ray traced car reflections).
    {
      const bool vulkan = ActiveBackend() == Backend::kVulkan;
      const std::string why = CarRtUnsupportedReason(device_.capabilities(), vulkan, device_.wave_ops());
      if (!why.empty()) {
        REXLOG_INFO("[gpu] car ray tracing: not available: {}", why);
      } else if (car_rt_.Initialize(device_, frames_, upload_, heap_, passes_, edram_, pass_timing_, car_frame_)) {
        if (gpu_class::CurrentRtRung() == gpu_class::RtTier::kNone) {
          // The device can trace, but the ray tracing tier (rt_tier.h) is none: a card with no ray tracing units (the GTX 10 and 16 series run DXR in the
          // shaders, far too slowly) or the Low class (shared memory). The effects stay off and the pause row does not offer RAY TRACED (CarRtReflAvailable);
          // the structures are still set up, so mcla_gpu_rt_tier can force a tier for a test. The launcher reads this line like the others.
          REXLOG_INFO("[gpu] car ray tracing: not available: the ray tracing tier is none on this graphics card or class (see the '[gpu] ray tracing tier' line), "
                      "so the ray traced car effects stay off");
        } else if (vulkan) {
          REXLOG_INFO("[gpu] car ray tracing: available (Vulkan ray queries, VK_KHR_ray_query): the player's car is built into ray tracing "
                      "structures while an effect asks for them");
        } else {
          REXLOG_INFO("[gpu] car ray tracing: available (DirectX Raytracing 1.1, shader model {}.{}): the player's car is built into ray tracing "
                      "structures while an effect asks for them",
                      device_.capabilities().shaderModel >> 4, device_.capabilities().shaderModel & 0xF);
        }
      } else {
        REXLOG_WARN("[gpu] car ray tracing: the device can trace rays but the structures could not be set up: not available");
      }
    }
    // The ray traced half of the self-reflection (car_rt_refl.h): after the car's structures exist (their descriptor set is in its layout)
    // and before the precache starts.
    car_rt_refl_.Initialize(device_, frames_, heap_, passes_, pass_timing_, &car_rt_, car_refl_.ListedAtStart());
    car_refl_.SetCarRtRefl(&car_rt_refl_);
    // The side mirrors, ray traced (car_mirror_rt.h): the same structures, listed with the start's precache where the pass is wanted at the start.
    car_mirror_.Initialize(device_, frames_, heap_, passes_, pass_timing_, edram_, car_rt_);
    // And the ray traced occlusion and sun term around the car (ao.h, car_rt_ao.h): the occlusion's own pass, with the same structures.
    ao_.InitializeCarRt(&car_rt_, &car_frame_);
    // === CITY-RT BLOCK (city_rt_scene.h, city_rt_shadow.h; CITY-RT M1): the city's geometry (the city pack the launcher builds, in the shader pack folder's `city` folder) as ray tracing
    // structures that join the car scene's top level, and the sun's shadows traced against them, read by the contact march. Both make nothing until the setting asks (mcla_gpu_rt_shadows;
    // auto is the High class), and nothing at all without ray queries.
    {
      city_rt_.Initialize(device_, frames_, upload_, pass_timing_, CityPackFolder() / larecomp::citypack::kPackFileName, gpu_class::Current());
      if (g_city_pack_hooks.state) {
        // The app decides whether the pack in place is the player's (stat only) and builds it in the background when it is not; without hooks (a build with no app) the pack is opened as it is.
        city_rt_.SetPackProbe(g_city_pack_hooks.state, g_city_pack_hooks.request_build);
      }
      // CITY-RT M2 H3: the city's hit data (the file the hit data's builder writes beside the pack; nothing is made until a consumer wants it) and the scene service that every consumer asks for
      // the top level, the structures and the hit data (rt_scene_service.h).
      city_hit_.Initialize(device_, frames_, upload_, pass_timing_, heap_, CityPackFolder() / larecomp::citypack::kHitFileName, CityPackFolder() / larecomp::citypack::kTexFileName, gpu_class::Current());
      if (g_city_pack_hooks.hit_state) {
        // The app decides whether the hit data in place is the player's (stat only) and builds it in the background when it is not, once a consumer wants it and the pack is open.
        city_hit_.SetFileProbe(g_city_pack_hooks.hit_state, g_city_pack_hooks.request_hit_build);
      }
      city_rt_.SetHitScene(&city_hit_);
      rt_scene_.Initialize(&car_rt_, &city_rt_, &city_hit_);
      ao_.InitializeRtAo(car_rt_, &city_rt_, rt_scene_);
      car_rt_refl_.SetSceneService(&rt_scene_);  // CITY-RT M2 R1/R2: the city in the car paint's reflections asks the service for the structures, the hit data and the textures (city_refl.h)
      // CITY-RT FRAME (rt_world_frame.h): the world's rows and the lamps of every frame that has a top level, car or no car, in t10 and t11 of the car scene's set (the scene's builder tells it the view and
      // binds the buffers; the scene writes them at the trigger). The AO's draw hook gives it the light, the car frame the sky and the environment map, the shared products the exposure, the pack the lamps.
      {
        RtWorldFrame::Sources world_sources;
        world_sources.ao = &ao_;
        world_sources.cars = &car_frame_;
        world_sources.screen = &screen_;
        world_sources.city = &city_rt_;
        world_sources.service = &rt_scene_;
        world_sources.lights = &light_manager_;  // GI-BOUNCE-RT: the headlights of the frame and the cars' lamps, for the emitters
        rt_world_.Initialize(device_, frames_, world_sources);
        car_rt_.SetWorldFrame(&rt_world_);
      }
      car_rt_.SetCityScene(&city_rt_);
      car_rt_.SetService(&rt_scene_, &city_hit_);
      car_mirror_.SetService(&rt_scene_);  // MIRROR-CITY: the side mirrors' wants of the city (car_mirror_rt.h, Consumer::kCityMirror)
      city_shadow_.Initialize(device_, frames_, heap_, passes_, pass_timing_, &car_rt_, &city_rt_, &rt_scene_, /*listed_at_start=*/true);
      ao_.SetCityRtShadow(&city_shadow_);
      // CITY-RT R4: the screen-space reflections' ray traced fallback (ssr_rt.h): the SSR host pass layout with the output set and this scene's ray tracing set; nothing is made where the device cannot trace.
      ssr_.InitializeRt(device_, frames_, heap_, passes_, pass_timing_, &car_rt_, &rt_scene_, &rt_world_);
      // The probe field's integration (gi_integration.h): the field needs M1's structures for its safe range and (G2) its rays.
      gi_integration_.Initialize(&gi_field_, &gi_apply_, &ao_, &city_rt_);
      // The probe field's update (gi_update.h, CITY-RT M3 G2): the rays of the probes and their blend, in the top level the car scene builds. Its pipelines are listed with the precache when the setting is not off at
      // the start as it will be once the update is built in; given to the integration only where the device can trace and its layouts and shaders exist (else the row says NOT READY and no field is made).
      {
        const bool update_listed = gi_setting::Decide(gpu_class::Known() ? gpu_class::Current() : gpu_class::GpuClass::kLow, gpu_class::CurrentRtTier(), larecomp::settings::mcla_gpu_rt_gi.Get(), true).level !=
                                   gi::Level::kOff;
        gi_update_.Initialize(device_, frames_, heap_, passes_, pass_timing_, gi_field_, car_rt_, city_rt_, city_hit_, rt_scene_, rt_world_, update_listed);
        if (gi_update_.available()) {
          gi_integration_.SetUpdate([this](plume::RenderCommandList* list, uint32_t frame_slot, const gi::GiFrame& update_frame) { gi_update_.Record(list, frame_slot, update_frame); },
                                    [this](gi::Level level) { gi_update_.OnWant(level); });
        }
        // The per-pixel detail pass (gi_pixel.h, CITY-RT M3 G5): the apply runs it, at ULTRA on a strong card only. Its pipelines are made in the background the first time it is wanted; nothing is made now.
        gi_pixel_.Initialize(device_, frames_, heap_, passes_, pass_timing_, gi_field_, car_rt_, city_rt_, city_hit_, rt_world_);
        if (gi_pixel_.available()) {
          gi_apply_.SetGiPixel(&gi_pixel_);
        }
        // RT-LIGHTS-D1 (rt_lights.h, debug only): the ray traced visibility of the top K lights, run by the deferred light pass; makes nothing until mcla_gpu_rt_lights asks (its pipelines are listed with the
        // precache the first time it is wanted).
        rt_lights_.Initialize(device_, frames_, heap_, passes_, pass_timing_, car_rt_, rt_world_, rt_scene_);
        if (rt_lights_.available()) {
          local_lights_.SetRtLights(&rt_lights_);
        }
      }
    }
    // The ray tracing benchmark (rt_bench.h, rt_tier.h): once with a card that can trace (kept next to larecomp.toml), while the GPU is idle and before the
    // precache below starts its builder; it can move the card between the weak and the strong tier, which the ray traced upgrades' `auto` reads.
    rt_bench::RunIfWanted(device_);
    // The resolve conversions and the decode listed the pipelines a session
    // uses (host_precache.h): one builder thread makes them while the rest of
    // the renderer starts and the guest boots.
    passes_.precache().Start();
    geometry_.SetResolveMemory(&resolve_memory_);  //
    textures_.Initialize(device_, frames_, heap_, upload_, guest_memory_, resolve_,
                         resolve_memory_);
    vram_.Initialize(device_, textures_, resolve_, resolve_memory_, edram_);
    scene_.Initialize(upload_, pipelines_, samplers_, geometry_, edram_, textures_, resolve_,
                      guest_layout_, heap_, pass_timing_, post_);
    // The guest's occlusion queries (the sun's glare): never fails the renderer.
    occlusion_.Initialize(device_);
    scene_.SetOcclusion(&occlusion_);
    scene_.SetTemporal(&temporal_);
    scene_.SetAmbientOcclusion(&ao_);
    ao_.SetCarFrame(&car_frame_);
    ao_.SetCarShadowMap(&car_shadow_);  // the contact shadows read the car shadow map (car_shadow_map.h)
    ao_.SetGBuffer(&gbuffer_);  // the occlusion, the bounce light and the contact shadows ask for the G-buffer's normal plane and read it (ao.h, GB-E)
    ao_.SetScreenShared(&screen_);  // ... and hand their depth chain to the shared screen products (ao.h, screen_shared.h)
    car_refl_.SetScreenShared(&screen_);  // ... and the car self-reflection reads the frames' cameras and the colour capture from them (car_selfrefl.h, SS-1)
    scene_.SetSunShafts(&shafts_);
    scene_.SetVolumetrics(&volumetrics_);
    scene_.SetGBuffer(&gbuffer_);
    scene_.SetScreenShared(&screen_);
    ssr_.SetScreenShared(&screen_);  // the reflections read the Hi-Z, colour_post and the camera of the shared products, the G-buffer's planes and the AO's camera (ssr.h)
    ssr_.SetGBuffer(&gbuffer_);
    ssr_.SetResolveCache(&resolve_);
    ssr_.SetAmbientOcclusion(&ao_);
    scene_.SetScreenReflections(&ssr_);
    scene_.SetCarSelfRefl(&car_refl_);
    scene_.SetCarDump(&car_dump_);
    scene_.SetCarFrame(&car_frame_);
    scene_.SetMirrorGlass(&mirror_glass_);
    scene_.SetHeadlightRig(&headlights_);
    scene_.SetCarShadowMap(&car_shadow_);
    scene_.SetMirrorView(&mirror_view_);
    scene_.SetMirrorOverlay(&mirror_overlay_);
    scene_.SetLocalShadow(&local_shadow_);
    scene_.SetLightManager(&light_manager_);
    scene_.SetLocalLights(&local_lights_);
    scene_.SetGiApply(&gi_apply_);
    scene_.SetGiIntegration(&gi_integration_);
    scene_.SetRtWorldFrame(&rt_world_);
    scene_.SetCarRtScene(&car_rt_);
    scene_.SetCarMirrorRt(&car_mirror_);
    car_rt_.SetTextureCache(&textures_);  // the car ray tracing's materials ask it for the textures of the cars beside the player (TextureCache::Peek2D)
    ok = post_.Initialize(device_, heap_, upload_, passes_, edram_, pass_timing_) &&
         present_.Initialize(device_, frames_, heap_, upload_, passes_, textures_, pass_timing_,
                             swap_chain_.format(), swap_chain_.hdr()) &&
         overlay_.Initialize(device_, frames_, heap_, upload_, std::move(overlay_channel),
                             swap_chain_.format(), swap_chain_.hdr());
  }
  if (ok) {
    // The frame's scene and UI as two layers: HDR10 output keeps the UI at paper white with them
    // (frame_layers.h). Not wanted, they make nothing and touch no draw; a failure to make them
    // leaves the HDR output on the front buffer (stage 1), which is not a reason to stop.
    layers_.Request(FrameLayers::kRequesterHdr, swap_chain_.hdr());
    const bool layers_ok = layers_.Initialize(passes_, edram_, pass_timing_);
    scene_.SetFrameLayers(&layers_);  // a draw is touched only while the layers are active
    if (swap_chain_.hdr()) {
      SetUpHdrScene(layers_ok);
    }
  }
  if (ok) {
    flicker_probe_.Initialize(device_, frames_, swap_chain_.format());
    capture_readback_.Initialize(device_, frames_, pass_timing_, swap_chain_.format());  // capture mode: made on a take's first frame
    // The temporal groundwork's passes: made on first use, never fail the renderer.
    // The upscaler backend (FidelityFX, when the build has it) is probed on a worker thread.
    temporal_.Initialize(device_, frames_, heap_, passes_, pass_timing_, textures_, resolve_,
                         edram_);
    temporal_.SetRrGuidePass(&rr_guides_);  // the upscale runs the guide pass where Ray Reconstruction (or a guide view) wants it
    // Frame generation: probed on a worker thread; the setting is taken at a frame boundary.
    fg_.Initialize(device_, frames_, passes_, pass_timing_, layers_, temporal_, swap_chain_);
    // Its images and AMD's context are not something the budget's response can release; its lines
    // name them (vram_budget.h), and frame generation asks the budget's rule before it takes any.
    vram_.AddHolder("frame generation", [this] { return fg_.HeldBytes(); });
    // The city's hit data pool (64 to 256 MiB) is not something the response can release (the structures built over it would be rebuilt): its line names it.
    vram_.AddHolder("city hit data", [this] { return city_hit_.HeldBytes(); });
    // The headlight shadow maps' atlas (the replay's own or the shared one: local_shadow.h HeldBytes counts both) is memory the response cannot release either: the lines name it.
    vram_.AddHolder("headlight shadow atlas", [this] { return local_shadow_.HeldBytes(); });
    // The probe field's textures (gi_field.h): the OS counts them in the use it reports and the caches' trim cannot give them back; the field asks the room test before it makes any.
    vram_.AddHolder("probe field", [this] { return gi_field_.HeldBytes(); });
    vram_.AddHolder("probe update", [this] { return gi_update_.HeldBytes(); });
    vram_.AddHolder("per-pixel bounce light", [this] { return gi_pixel_.HeldBytes(); });
    vram_.AddHolder("rtao", [this] { return ao_.RtAoHeldBytes(); });
    vram_.AddHolder("ray traced light visibility", [this] { return rt_lights_.HeldBytes(); });
    // DLSS frame generation: asked of Streamline when it has started; the setting is taken at a
    // frame boundary.
    dlssg_.Initialize(device_, frames_, passes_, pass_timing_, layers_, temporal_, present_,
                      swap_chain_);
    timing_.Initialize(device_);
    pass_timing_.Initialize(device_, larecomp::settings::mcla_gpu_pass_timing.Get());
    markers_.Initialize(device_);
    capture_.Initialize();
    census_.SetEnabled(larecomp::settings::mcla_gpu_memory_census.Get());
    validate_ = larecomp::settings::mcla_gpu_validate.Get();
#ifndef NDEBUG
    teardown_diagnostics_ = true;  // Debug Plume builds request the graphics API's validation layer.
#else
    teardown_diagnostics_ = validate_ || options.device.debug_layer ||
        (ActiveBackend() == Backend::kVulkan &&
         (std::getenv("VK_INSTANCE_LAYERS") || std::getenv("VK_LOADER_LAYERS_ENABLE")));
#endif
    batch_draws_ = larecomp::settings::mcla_gpu_batch_draws.Get();
    profile_records_ = larecomp::settings::mcla_gpu_scene_profile.Get();
    if (larecomp::settings::debug_menu.Get() && larecomp::settings::mcla_test_present_nowait.Get()) {
      present_nowait_ = ActiveBackend() == Backend::kD3D12;
      REXLOG_INFO("[gpu] present nowait test: {}", present_nowait_ ? "enabled on D3D12" : "D3D12 only");
    }
    release_early_ = larecomp::settings::mcla_gpu_present_release.Get();
    // The swap chain exists and the render thread, which marks the frames, has not started.
    latency::Initialize(device_, swap_chain_);
    check_release_ = release_early_ && larecomp::settings::mcla_gpu_present_release_check.Get();
#if defined(_WIN32)
    const bool vulkan = ActiveBackend() == Backend::kVulkan;
    const auto adapter = vulkan && device_.device() ? backend::QueryAdapterIds(device_.device())
                                                    : backend::AdapterIds{};
    may_use_composed_copy_ = render_wait::MayUseComposedCopy(true, vulkan, adapter.vendor_id);
    UpdateComposedPresentCeiling();
#endif
    ok = thread_.Start(*this, kIdlePeriod);
  }
  if (!ok) {
    latency::Shutdown();
    capture_.Shutdown();
    occlusion_.Shutdown();
    pass_timing_.Shutdown();
    timing_.Shutdown();
    fg_.Shutdown();
    dlssg_.Shutdown(swap_chain_);
    temporal_.Shutdown();
    rt_lights_.Shutdown();  // RT-LIGHTS-D1: holds pointers to the car scene, the world frame and the scene service: before them
    gi_pixel_.Shutdown();  // CITY-RT M3 G5: holds pointers to the field, the scenes, the world frame and the lights, and the apply holds it: before them
    rr_guides_.Shutdown();  // it holds a pointer to the G-buffer: before it
    ssr_.Shutdown();  // it holds pointers to the AO, the shared products and the G-buffer: before them
    rt_world_.Shutdown();  // it holds pointers to the AO, the car frame, the shared products and the city scene (and asked the AO's draw hook for the light): before them (CITY-RT FRAME)
    gi_integration_.Shutdown();  // it holds pointers to the field, the apply, the AO and the city scene: first
    gi_update_.Shutdown();  // it holds pointers to the field, the car scene, the city scene, the scene service and the world frame: before the field (and before HostPasses)
    gi_apply_.Shutdown();  // the same: before the AO, the shared products and the G-buffer it reads (the AO holds a pointer to it: after the AO's last frame, which is over)
    gi_field_.Shutdown();  // after the apply that reads it, GPU idle and the FrameRing drained, before HostPasses shuts down
    local_lights_.Shutdown();  // before the owners it reads (the AO, the shared products, the G-buffer, the manager)
    ao_.Shutdown();
    screen_.Shutdown();  // the AO holds a pointer to it: after the AO
    shafts_.Shutdown();
    volumetrics_.Shutdown();  // before the headlight shadow maps it reads (volumetrics_inputs.h)
    light_manager_.Shutdown();  // after the volume, whose inputs it looks at
    lamp_shadows_.Reset();  // leaves the shadow atlas and the game's lamp list's hook before the replay (its owner) and the atlas go
    local_shadow_.Shutdown();
    shadow_atlas_.Shutdown();  // after the replay, its provider
    gbuffer_.Shutdown();
    mirror_overlay_.Shutdown();
    mirror_view_.Shutdown();
    car_shadow_.Shutdown();
    car_refl_.Shutdown();
    car_dump_.Shutdown();
    city_shadow_.Shutdown();  // before the car scene it traces against and the pipelines' owner
    rt_scene_.Shutdown();
    city_rt_.Shutdown();
    city_hit_.Shutdown();  // after the structures that evicted their blocks from it
    car_rt_refl_.Shutdown();
    car_mirror_.Shutdown();  // after the host builder was stopped (CarRtRefl::Shutdown): its functions name this object's layout and shaders
    car_rt_.Shutdown();
    mirror_glass_.Shutdown();
    car_frame_.Shutdown();
    headlights_.Shutdown();
    flicker_probe_.Shutdown();
    capture_readback_.Shutdown();
    hdr_tone_.Shutdown();
    hdr_scene_.Shutdown();
    layers_.Shutdown();
    overlay_.Shutdown();
    present_.Shutdown();
    post_.Shutdown();
    vram_.Shutdown();
    textures_.Shutdown();
    resolve_.Shutdown();
    resolve_memory_.Shutdown();
    passes_.Shutdown();
    edram_.Shutdown();
    swap_chain_.Shutdown();
    fg_.ReleaseQueue();
    hdr::Stop();
    geometry_.Shutdown();
    upload_.Shutdown();
    pipelines_.Shutdown();
    samplers_.Shutdown();
    shaders_.Shutdown();
    guest_layout_.Shutdown();
    heap_.Shutdown();
    frames_.Shutdown();
    device_.Shutdown();
    overlay_host_.reset();
    return false;
  }
  initialized_ = true;
  REXLOG_INFO("[gpu] renderer up: {} frames in flight, render thread running (idle tick {} ms), "
              "validator {}, {}, present release {}",
              kFramesInFlight, kIdlePeriod.count(), validate_ ? "on" : "off",
              batch_draws_ ? "a batch every " + std::to_string(batch_draws_) + " draws"
                           : std::string("one submission per guest frame"),
              release_early_ ? (check_release_ ? "early, checked" : "early") : "after the present");
  return true;
}

bool Renderer::SetUpHdrScene(bool layers_ok) {
  if (layers_ok &&
      (hdr_scene_.ready() || hdr_scene_.Initialize(passes_, layers_, pass_timing_))) {
    present_.SetHdrScene(&hdr_scene_);
    // Stage 3, the scene above paper white. Without the variants the scene stays at paper white.
    if (hdr_tone_.ready() ||
        hdr_tone_.Initialize(passes_, shaders_, layers_, post_, pass_timing_)) {
      layers_.SetSceneSource(&hdr_tone_);
      hdr_scene_.SetSceneSource(&hdr_tone_);
      scene_.SetHdrToneMap(&hdr_tone_);
    }
    return true;
  }
  REXLOG_ERROR("[gpu] hdr: the UI layer split could not be set up; the HDR output shows "
               "the front buffer as it is (the scene and the UI together)");
  layers_.Request(FrameLayers::kRequesterHdr, false);
  return false;
}

void Renderer::LoadShaderVariants() {
  if (!shaders_.attached() || shaders_.has_variants()) {
    return;
  }
  std::string why;
  if (!shaders_.LoadVariants(&why)) {
    REXLOG_INFO("[gpu] shader variants: the variant pack beside the main shader pack is not available "
                "({}); the HDR output keeps the scene at paper white (stage 2) and the bloom keeps the "
                "game's taps above a render scale of 1x",
                why);
  }
}

void Renderer::OnShaderPackAttached() {
  // The shader variants (shaders.h: the HDR scene's tone map for stage 3, hdr_tonemap.h, and the bloom's
  // Gaussians at a render scale), from the folder the main pack came from. Always, whether HDR output is
  // on or not: the bloom's are used by every start. Before the pipeline records are read: a record that
  // names a variant is dropped while the variants are not loaded. They change the entries (variants,
  // hdr_variant), so this is the one place they are loaded, for a pack attached at startup and for one
  // attached later; HdrToneMap::active() waits for them.
  LoadShaderVariants();
  pipelines_.StartPersistence(shaders_);
}

bool Renderer::AttachShaderPacks(const std::filesystem::path& folder) {
  if (shaders_.attached()) {
    return true;
  }
  if (!initialized_) {
    REXLOG_ERROR("[gpu] shader pack: cannot attach, the renderer is not running");
    return false;
  }
  if (!shaders_.Attach(ShaderStore::BuildFolder(folder))) {
    return false;
  }
  OnShaderPackAttached();
  return true;
}

void Renderer::BeginShutdown() {
  shutting_down_.store(true, std::memory_order_release);
}

void Renderer::Shutdown(bool fast_exit) {
  BeginShutdown();
  if (!initialized_) {
    return;
  }
  initialized_ = false;
  ShutdownClock clock;
  // 1. No more recording: the render thread finishes the command it is on and
  //    exits; producers and waiters are released. A thread that does not exit
  //    in time may still be inside any component, so nothing is destroyed.
  clock.Stage("the render thread's stop");
  if (!thread_.Stop(kStopDeadline)) {
    REXLOG_ERROR("[gpu] renderer left alive after {} presents: its render thread is stuck",
                 presented_);
    clock.Done();  // nothing more happens here: the close watchdog is not to wait for this shutdown
    return;
  }
  // The pacer drains what it holds and ends before anything waits the GPU idle or goes.
  clock.Stage("the frame generation pacer's stop");
  fg_.StopPacer();
  clock.Stage("the final reports and the first idle wait");
  ReportDroppedPresents(present_drops_.TakePending());
  if (release_early_) {
    REXLOG_INFO("[gpu] present release: {} of {} guest presents released the guest early{}",
                early_releases_, guest_presents_,
                check_release_ ? std::format("; {} reads or writes of guest memory after the "
                                             "release (must be 0)",
                                             released_accesses_)
                               : std::string());
  }
  // The last reports (and the validator's CSVs), now that no record arrives.
  if (validate_) {
    validator_.Finish();
  }
  scene_.Finish();
  temporal_.Finish();
  census_.Finish();
  trace_.Finish();
  // Nothing reads or watches guest memory any more.
  guest_memory_.Shutdown();
  // A frame held to its time is not presented any more: presenting from here
  // could wait on the window this thread is closing.
  deferred_ = {};
  // A guest frame the render thread was recording when it stopped: its list is
  // closed without executing, now -- closing it after the components below
  // destroyed what it names is a use after free (a D3D12 Close reads every
  // object the list recorded; seen as debug layer id 921 and a driver crash).
  if (frames_.is_open()) {
    scene_.EndFrame(nullptr);
    frames_.Abandon();
    markers_.Drop();
  }
  // 2. Drain. Every retirement is released here, release actions included,
  //    while all the components they name are still alive. Skipped by
  //    FrameRing when the device is lost.
  frames_.WaitIdle();
  // 3. Reverse creation order; the GPU is idle, so components destroy their
  //    objects directly. Ordered teardown rather than reblue's deliberate leak:
  //    every Plume object here has a single owner destroyed before the device.
  clock.Stage("the vendor calls, the captures, the timers and the frame generation");
  latency::Shutdown();  // the vendor's calls name the swap chain
  capture_.Shutdown();
  occlusion_.Shutdown();
  pass_timing_.Shutdown();
  timing_.Shutdown();
  fg_.Shutdown();
  // DLSS frame generation: the proxy swap chain is destroyed and the plugin unloaded before
  // Streamline itself is shut down (temporal_.Shutdown below).
  clock.Stage("DLSS frame generation and the upscaler (Streamline)");
  dlssg_.Shutdown(swap_chain_);
  temporal_.Shutdown();
  clock.Stage("the effects, the targets and the memory");
  rt_lights_.Shutdown();  // the same, RT-LIGHTS-D1 (rt_lights.h)
  gi_pixel_.Shutdown();  // the same, CITY-RT M3 G5 (gi_pixel.h)
  rr_guides_.Shutdown();  // it holds a pointer to the G-buffer: before it
  ssr_.Shutdown();  // it holds pointers to the AO, the shared products and the G-buffer: before them
  rt_world_.Shutdown();  // the same, CITY-RT FRAME (rt_world_frame.h)
  gi_integration_.Shutdown();  // it holds pointers to the field, the apply, the AO and the city scene: first
  gi_update_.Shutdown();  // it holds pointers to the field, the car scene, the city scene, the scene service and the world frame: before the field (and before HostPasses)
  gi_apply_.Shutdown();  // the same: before the AO, the shared products and the G-buffer it reads (the AO holds a pointer to it: after the AO's last frame, which is over)
  gi_field_.Shutdown();  // after the apply that reads it, GPU idle and the FrameRing drained, before HostPasses shuts down
  local_lights_.Shutdown();  // before the owners it reads (the AO, the shared products, the G-buffer, the manager)
  ao_.Shutdown();
  screen_.Shutdown();  // the AO holds a pointer to it: after the AO
  shafts_.Shutdown();
  volumetrics_.Shutdown();  // before the headlight shadow maps it reads (volumetrics_inputs.h)
  light_manager_.Shutdown();  // after the volume, whose inputs it looks at
  lamp_shadows_.Reset();  // leaves the shadow atlas and the game's lamp list's hook before the replay (its owner) and the atlas go
  local_shadow_.Shutdown();
  shadow_atlas_.Shutdown();  // after the replay, its provider
  gbuffer_.Shutdown();
  mirror_overlay_.Shutdown();
  mirror_view_.Shutdown();
  car_shadow_.Shutdown();
  car_refl_.Shutdown();
  car_dump_.Shutdown();
  city_shadow_.Shutdown();
  rt_scene_.Shutdown();
  city_rt_.Shutdown();
  city_hit_.Shutdown();
  car_rt_refl_.Shutdown();
  car_mirror_.Shutdown();  // after the host builder was stopped (CarRtRefl::Shutdown): its functions name this object's layout and shaders
  car_rt_.Shutdown();
  mirror_glass_.Shutdown();
  car_frame_.Shutdown();
  headlights_.Shutdown();
  flicker_probe_.Shutdown();
  capture_readback_.Shutdown();
  hdr_tone_.Shutdown();
  hdr_scene_.Shutdown();
  layers_.Shutdown();
  overlay_.Shutdown();
  present_.Shutdown();
  post_.Shutdown();
  vram_.Shutdown();
  textures_.Shutdown();
  resolve_.Shutdown();
  resolve_memory_.Shutdown();
  clock.Stage("the host passes (the builders' join)");
  passes_.Shutdown();  // the host pipeline builders are joined here: one in the middle of a driver compile is waited for
  clock.Stage("the swap chain");
  edram_.Shutdown();
  swap_chain_.Shutdown();
  clock.Stage("the pipelines (the precache's join, the cache file, the pipelines' release)");
  fg_.ReleaseQueue();  // the swap chain made on it is gone
  hdr::Stop();
  geometry_.Shutdown();
  upload_.Shutdown();
  pipelines_.Shutdown();
  clock.Stage("the samplers, the shaders and the guest layout");
  samplers_.Shutdown();
  shaders_.Shutdown();
  guest_layout_.Shutdown();
  if (ShouldSkipDeviceTeardown(larecomp::settings::mcla_skip_gpu_teardown.Get(), fast_exit,
                               teardown_diagnostics_,
                               [this] { return device_.CheckLost("final teardown"); })) {
    // The process-lifetime renderer still owns the frames, pending GPU retirements, heap and
    // device. Leave them together for OS cleanup after the SDK shutdown and exit flushes.
    // CHECK: exit status, saved data and close time with three games sharing the GPU on both APIs.
    REXLOG_INFO("[gpu] shutdown: leaving the final GPU objects for process exit (mcla_skip_gpu_teardown)");
  } else {
    // 4. A component shut down above can still retire a release, such as a bindless slot its
    //    images held (DLSS frame generation's, while it was generating). The release writes the
    //    heap, so it runs here, before the heap goes; the GPU is idle, so nothing is waited for.
    clock.Stage("the second idle wait");
    frames_.WaitIdle();
    clock.Stage("the bindless heap and the frames");
    heap_.Shutdown();
    frames_.Shutdown();
    // The last stage is the driver's own: Device::Shutdown names its parts (the queue, the device with its memory allocator, the
    // instance), each one call into the driver that returns by itself. It is where 1.4 to 4.7 s went in the slow closes of 11-20 to 11-22,
    // with another game running (close_watchdog.h), so the close watchdog gives these calls more time than a stage of ours.
    clock.Stage("the device (the driver's own teardown)", true);
    device_.Shutdown([&clock](const char* part) { clock.Stage(part, true); });
  }
  clock.Done();
  overlay_host_.reset();
  REXLOG_INFO("[gpu] renderer shut down after {} presents ({} guest, {} idle, {} skipped)",
              presented_, guest_presents_, idle_presents_, skipped_);
}

void Renderer::Execute(const RecordHeader& header, const uint8_t* payload) {
  if (!profile_records_) {
    ExecuteRecord(header, payload);
    return;
  }
  // The record's CPU time: its time less the GPU waits inside it (the totals only grow, so a Take inside the record is harmless).
  ThreadLoad& load = CurrentThreadLoad();
  const uint64_t start = cycles::Now();
  const uint64_t gpu_wait_start = load.gpu_wait_total();
  ExecuteRecord(header, payload);
  const uint64_t elapsed = cycles::Now() - start;
  const uint64_t waited = load.gpu_wait_total() - gpu_wait_start;
  load.AddRecord(uint32_t(header.type), elapsed - std::min(elapsed, waited));
}

void Renderer::ExecuteRecord(const RecordHeader& header, const uint8_t* payload) {
  const auto type = static_cast<size_t>(header.type);
  if (type < std::size(records_)) {
    ++records_[type];
  }
  // The render thread is the only writer: a plain store, not a locked add
  // (the hang check reads it from another thread).
  records_executed_.store(records_executed_.load(std::memory_order_relaxed) + 1,
                          std::memory_order_relaxed);
  trace_.OnRecord(guest_frames_, header, payload);
  // Scene records open the guest frame's list; the present closes it.
  switch (header.type) {
    case RecordType::kPresent:
      ++guest_frames_;
      if (validate_) {
        validator_.OnPresent(*reinterpret_cast<const PresentRecord*>(payload));
      }
      present_tag_ = reinterpret_cast<const PresentRecord*>(payload)->tag;
      if ((present_tag_ & mc::cinematic::capture::kTagCapture) && frames_.is_open() &&
          mc::cinematic::CaptureVisualConfig().Samples(present_tag_ & mc::cinematic::capture::kTagUpdateMask)) {
        scene_.CollectVisualSources(capture_readback_.visual_sources());
      }
      scene_.OnPresent();
      census_.OnPresent();
      present_.OnGuestPresent(*reinterpret_cast<const PresentRecord*>(payload));
      // The guest waits for this record (kRecordSync); ReleaseGuest may let it go sooner.
      release_sequence_ = (header.flags & kRecordSync) != 0 ? header.sequence : 0;
      present_target_ = reinterpret_cast<const PresentRecord*>(payload)->target_qpc;
      ExecutePresent(PresentSource::kGuest);
      present_tag_ = 0;
      present_target_ = 0;
      release_sequence_ = 0;
      if (check_release_) {
        GuestMemory::MarkReleased(false);
        released_accesses_ += GuestMemory::TakeReleasedAccesses();
      }
      return;
    case RecordType::kDraw:
      if (validate_) {
        validator_.OnDraw(payload);
      }
      census_.OnDraw(payload);
      scene_.OnDraw(payload, OpenFrameList());
      MaybeFlushBatch();
      textures_.DrawExecuted();
      if (textures_.prefetching()) {
        PrefetchTextures(header);
      } else if (prefetch_live_) {
        // The burst is over: the next one starts from the record it is on then.
        prefetch_live_ = false;
        prefetch_position_ = 0;
        prefetch_ahead_ = 0;
      }
      return;
    case RecordType::kClear: {
      const auto& record = *reinterpret_cast<const ClearRecord*>(payload);
      if (validate_) {
        validator_.OnClear(record);
      }
      plume::RenderCommandList* list = OpenFrameList();
      PerfScope label(markers_, list, "Clear", PerfMarkers::kEvent);
      scene_.OnClear(record, list);
      return;
    }
    case RecordType::kResolve: {
      const auto& record = *reinterpret_cast<const ResolveRecord*>(payload);
      if (validate_) {
        validator_.OnResolve(record);
      }
      plume::RenderCommandList* list = OpenFrameList();
      PerfScope label(markers_, list, "Resolve", PerfMarkers::kEvent);
      scene_.OnResolve(record, list);
      return;
    }
    case RecordType::kTilingBegin: {
      const auto& record = *reinterpret_cast<const TilingBeginRecord*>(payload);
      if (validate_) {
        validator_.OnTilingBegin(record);
      }
      scene_.OnTilingBegin(record, OpenFrameList());
      return;
    }
    case RecordType::kTilingEnd: {
      const auto& record = *reinterpret_cast<const TilingEndRecord*>(payload);
      if (validate_) {
        validator_.OnTilingEnd(record);
      }
      scene_.OnTilingEnd(record, OpenFrameList());
      return;
    }
    case RecordType::kFence:
      // The guest may reuse what the records before this fence named once it is published: no
      // decode of theirs may still be reading it.
      textures_.QuiesceDecodes(header.sequence);
      ExecuteFence(*reinterpret_cast<const FenceRecord*>(payload));
      return;
    case RecordType::kLock: {
      // The write-back reads the products on the GPU: what this frame recorded
      // so far (the resolves among it) goes to the queue first, as a batch.
      const auto& lock = *reinterpret_cast<const LockRecord*>(payload);
      resolve_.OnLock(lock, [this, &lock] {
        if (frames_.is_open()) {
          markers_.Push(frames_.list(), "LockWriteBack", PerfMarkers::kEvent);
          scene_.BeforeFlush(frames_.list());
          // The encodes still waiting write bytes the write-back copies.
          resolve_memory_.FlushForLock(frames_.list(), lock.address, lock.size);
          pass_timing_.Mark(frames_.list(), "lock write-back");
          markers_.Pop(frames_.list(), PerfMarkers::kEvent);
          // The labels open here (Frame, Scene) stay open across the flush and end in the
          // frame's next list, on the same queue: the frame keeps one Frame.
          plume::RenderCommandList* next = frames_.FlushBatch();
          if (!next) {
            markers_.Drop();
          }
          scene_.OnListChanged(next);
        }
      });
      return;
    }
    case RecordType::kComposite: {
      // Ultrawide: the guest's hooks around the draw of the world icons, and around each piece of
      // the HUD, use the composite record as a one-way message to the render thread
      // (mc_engine/display/ultrawide.h, IsUiMessage): not a composition, no list to open.
      const uint32_t bucket = reinterpret_cast<const CompositeRecord*>(payload)->bucket;
      if (mirror_view::IsMarker(bucket)) {
        // The rear view mirror's phase (mirror_view.h): the markers need the open list (the view's pass, its resolve), unlike the UI messages below, which only set flags. They are on the
        // same side channel (top byte 0x7F), so they are told first.
        plume::RenderCommandList* list = OpenFrameList();
        PerfScope label(markers_, list, mirror_view::IsBeginMarker(bucket) ? "MirrorViewBegin" : "MirrorViewEnd", PerfMarkers::kEvent);
        scene_.OnMirrorMarker(bucket, reinterpret_cast<const CompositeRecord*>(payload)->tag, list);  // the begin's tag: the field this phase's camera was built with (MV-J2)
        return;
      }
      if (mc::ultrawide::IsUiMessage(bucket)) {
        scene_.OnUiMessage(bucket);
        return;
      }
      plume::RenderCommandList* list = OpenFrameList();
      PerfScope label(markers_, list, "Composite", PerfMarkers::kEvent);
      scene_.OnComposite(*reinterpret_cast<const CompositeRecord*>(payload), list);
      return;
    }
    case RecordType::kOcclusion:
      occlusion_.OnRecord(*reinterpret_cast<const OcclusionRecord*>(payload));
      return;
    case RecordType::kPad:
    case RecordType::kCount:
      break;
  }
  REXLOG_ERROR("[gpu] render thread: unexpected record type {}", static_cast<uint32_t>(type));
}

// The render thread is the guest's GPU: everything captured before
// the fence was inserted has been executed by now -- the M4 render thread reads
// what a record names from guest memory while executing it -- so the guest may
// reuse that memory. Publishing the value is what lets it.
void Renderer::ExecuteFence(const FenceRecord& record) {
  if (mcla::native_gfx::nocp::PublishOwnedRetiredFence(record.device, record.value)) {
    fences_published_.fetch_add(1, std::memory_order_relaxed);
    last_fence_.store(record.value, std::memory_order_relaxed);
    return;
  }
  // Only after Shutdown handed the write back, while this thread stops: the
  // vblank thread already retires everything.
  if (fences_not_held_++ == 0) {
    REXLOG_WARN("[gpu] render thread: fence {} of device {:#010x} arrived while the retired "
                "fence is not held by this renderer; not published",
                record.value, record.device);
  }
}

void Renderer::PrefetchTextures(const RecordHeader& executing) {
  if (!frames_.is_open()) {
    return;
  }
  prefetch_live_ = true;
  CommandStream& stream = thread_.stream();
  // Never behind the record that just ran (it is still the stream's current one: Release comes
  // after Execute). A draw that ran was one that had been offered, if the position was ahead of it.
  const uint64_t here = stream.consumer_position() + executing.size;
  if (prefetch_position_ < here) {
    prefetch_position_ = here;
    prefetch_ahead_ = 0;
  } else if (prefetch_ahead_ > 0) {
    --prefetch_ahead_;
  }
  for (uint32_t budget = kPrefetchScanRecords;
       budget != 0 && prefetch_ahead_ < kPrefetchAheadDraws; --budget) {
    const uint64_t before = prefetch_position_;
    const RecordHeader* next = stream.Peek(&prefetch_position_);
    if (!next) {
      break;
    }
    if (next->type == RecordType::kPresent) {
      // The frame ends there: the next one's draws are looked at once it has started.
      prefetch_position_ = before;
      break;
    }
    if (next->type == RecordType::kDraw) {
      scene_.PrefetchTextures(next->sequence, reinterpret_cast<const uint8_t*>(next + 1));
      ++prefetch_ahead_;
    }
  }
}

plume::RenderCommandList* Renderer::OpenFrameList() {
  if (frames_.is_open()) {
    return frames_.list();
  }
  if (!presenting_) {
    // A guest frame starts: nothing of it is recorded, so the HDR setting can change the output now
    // (from a present the back buffer is already acquired: ExecutePresent does it before that).
    // The order of the live changes at a frame boundary: this one first (GPU idle, the swap chain
    // made again, the window's size published again), then Scene::BeginFrame: the render scale and
    // the upscaler path (TemporalSystem::BeginFrame, which reads the size published here), then the
    // MSAA level (MsaaLevel::BeginFrame, after the path's own change). Frame generation's swap chain
    // switch comes right after the HDR one (both make the swap chain again).
    ApplyHdrSetting();
    ApplyFrameGenerationSetting();
    // The HUD fade, after the output is settled: it asks for the layers and the compose.
    ApplyUiFade();
  }
  plume::RenderCommandList* list = frames_.Begin();
  if (!list) {
    return nullptr;
  }
  const uint32_t slot = frames_.slot();
  device_.BeginFrame();
  // A guest frame starts here for the latency reducer (not an idle present's list).
  latency::OnRenderFrameOpen();
  // Outside any render pass: timestamp pool reset, one-time heap transitions.
  timing_.BeginFrame(list, slot);
  pass_timing_.BeginFrame(list, slot);
  heap_.RecordPendingBarriers(list);
  occlusion_.BeginFrame(list, slot);
  scene_.BeginFrame(list, slot);
  // After the caches' own frame start, before any record of the frame: what
  // the budget releases is retired behind this frame's submission.
  vram_.BeginFrame();
  markers_.BeginFrame();
  markers_.Push(list, "Frame", PerfMarkers::kFrame);
  markers_.Push(list, "Scene", PerfMarkers::kPhase);
  return list;
}

void Renderer::MaybeFlushBatch() {
  if (batch_draws_ == 0 || !frames_.is_open() || scene_.draws_since_flush() < batch_draws_) {
    return;
  }
  scene_.BeforeFlush(frames_.list());
  plume::RenderCommandList* next = frames_.FlushBatch();
  if (!next) {
    markers_.Drop();
  }
  scene_.OnListChanged(next);
}

void Renderer::SubmitFrameWithoutPresent() {
  if (!frames_.is_open()) {
    return;
  }
  plume::RenderCommandList* list = frames_.list();
  const uint32_t slot = frames_.slot();
  markers_.Pop(list, PerfMarkers::kPhase);  // Scene
  markers_.Push(list, "EndOfFrame", PerfMarkers::kPhase);
  scene_.EndFrame(list);
  markers_.PopAll(list);
  timing_.CompleteFrame(list, slot);
  pass_timing_.CompleteFrame(list, slot);
  if (frames_.Submit(nullptr, 0, nullptr, 0)) {
    timing_.OnSubmitted(slot);
    pass_timing_.OnSubmitted(slot);
    occlusion_.OnSubmitted(slot);
    gi_update_.OnSubmitted(slot);
    gi_pixel_.OnSubmitted(slot);
    latency::OnRenderSubmitted();
  }
  frames_.Advance();
  // The slot about to be recorded had its fence waited by Advance.
  upload_.ResetSlot(frames_.slot());
  occlusion_.OnSlotReady(frames_.slot());
}

void Renderer::OnIdle() {
  if (shutting_down_.load(std::memory_order_acquire) || device_.IsLost()) {
    ReportDeviceLost();
    return;
  }
  if (mc::cinematic::CaptureVisualDrainRequested()) frames_.WaitPreviousFrame();
  if (guest_has_presented_ &&
      std::chrono::steady_clock::now() - last_guest_present_ < kGuestActiveWindow) {
    // The guest is swapping; its presents carry the overlay.
    return;
  }
  if (frames_.is_open()) {
    // A guest frame is being recorded (the guest stopped between its first
    // scene record and its present, e.g. while loading): presenting now would
    // cut it in half. The overlay waits for the guest's present.
    return;
  }
  // The guest is quiet (not started yet, or stalled): keep the UI passes coming
  // and present when there is something new to show. No frame is open, so this is a boundary where
  // the HDR setting is applied (and what it changed is shown).
  // (A change of the HUD fade shows at an idle present too.)
  const bool hdr_changed = ApplyHdrSetting() | ApplyFrameGenerationSetting() | ApplyUiFade();
  overlay_host_->RequestPass();
  const bool first_frame = presented_ == 0;
  const bool resize = resize_requests_.load(std::memory_order_acquire) != 0 ||
                      swap_chain_.recovery_requested();
  if (first_frame || resize || hdr_changed || overlay_.HasNewContent()) {
    ExecutePresent(PresentSource::kIdle);
  }
}

bool Renderer::WantsSwapChainVsync() const {
  if (fg_.active()) {
    return fg_.WantsVsync();
  }
  return dlssg_.active() ? dlssg_.WantsVsync() : larecomp::settings::mcla_gpu_vsync.Get();
}

bool Renderer::RebuildSwapChain() {
  if (dlssg_.active()) {
    // DLSS frame generation must be off before any change of the window, and the proxy chain is not
    // resized: it is switched off, which makes the chain again, plain, at the window's size; frame
    // generation comes back by itself once the window has been still (DlssFrameGeneration::kSettle).
    if (SwitchDlssFrameGeneration(DlssFrameGeneration::Change::kOff, /*resize=*/true)) {
      return !swap_chain_.empty();
    }
  }
  // With frame generation the pacer presents: it stops first, and the present queue's work (every
  // image it copied or presented) is waited for.
  const FrameGeneration::PauseScope pause(fg_);
  // The back buffers are about to be released (and on D3D12 Release()d by
  // Plume's resize): nothing in flight may still write them.
  frames_.WaitIdle();
  // Vulkan's swap chain handle changes inside: the latency reducer's calls wait for it.
  latency::SwapChainChange latency_change;
  if (!swap_chain_.Rebuild()) {
    return false;
  }
  // The new size is what the render scale is measured against from the next frame boundary on
  // (temporal.cpp ReadRenderScale: with the screen's scale or an FSR1 preset the images follow).
  PublishWindowSize(swap_chain_.width(), swap_chain_.height());
  return true;
}

#if defined(_WIN32)
void Renderer::UpdateComposedPresentCeiling() {
  if (!may_use_composed_copy_) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now < composed_monitor_poll_) {
    return;
  }
  const bool first = composed_monitor_poll_ == std::chrono::steady_clock::time_point{};
  composed_monitor_poll_ = now + std::chrono::seconds(1);
  // Win32 Vulkan present modes do not report DWM's composition path. Assume copy on this
  // adapter/window combination, based on 78 of 78 RTX traces in VK-PRESENT-GDI.md.
  const bool exclusive = mcla::display::WantedMode() == mcla::display::Mode::kExclusive;
  const bool enabled = larecomp::settings::mcla_gpu_vk_present_pacing.Get();
  const auto window = swap_chain_.window();
  const uint32_t refresh_hz = enabled && !exclusive && window ? ReadDisplayRefreshHz(window) : 0;
  const uint32_t ceiling = render_wait::ComposedPresentCeiling(exclusive, enabled, refresh_hz);
  const uint32_t previous = composed_present_ceiling_.exchange(ceiling, std::memory_order_relaxed);
  if (first || previous != ceiling) {
    REXLOG_INFO("[gpu] Vulkan composed present ceiling: {} FPS (refresh {} Hz, exclusive {}, switch {})",
                ceiling, refresh_hz, exclusive, enabled);
  }
}
#endif

void Renderer::PresentNow(uint32_t image, PresentSource source, int64_t target, int64_t due,
                          int64_t ready, uint64_t guest_present, uint64_t latency_frame) {
  const bool nowait = present_nowait_ && !dlssg_.active();
#if defined(_WIN32)
  const uint32_t ceiling = composed_present_ceiling();
  const bool pace_present = ceiling != 0 && !dlssg_.active();
  if (pace_present) {
    // The game clock normally sets the cadence. Guard late, unscheduled and idle presents at the
    // actual call too, before acquiring the swap chain lock, so catch-up cannot bunch presents.
    const int64_t earliest = render_wait::ComposedPresentDue(last_composed_present_, QpcFrequency(), ceiling);
    if (QpcNow() < earliest) {
      WaitForPresentDeadline(earliest);
      due = earliest;
    }
  }
#endif
  latency::OnPresentBegin(latency_frame);
#if defined(_WIN32)
  if (pace_present) {
    last_composed_present_ = QpcNow();
  }
#endif
  const PresentResult result = swap_chain_.Present(image, nowait);
  latency::OnPresentEnd(latency_frame);
  if (nowait) {
    ReportDroppedPresents(present_drops_.OnPresent(result, PresentDrops::Clock::now()));
  }
  if (result == PresentResult::kDropped) {
    // The render submission and its fence still live in the ring. No image was
    // queued for display, so there are no present statistics for this frame.
    return;
  }
  if (dlssg_.active()) {
    dlssg_.AfterPresent();  // Streamline's status now and then
  }
  if (result == PresentResult::kFailed) {
    // SwapChain recorded the failure for recovery; check whether the device itself was lost.
    device_.CheckLost("swap chain present");
  }
  if (source != PresentSource::kGuest) {
    return;
  }
  const PresentObserver observer = g_present_observer.load(std::memory_order_acquire);
  if (target == 0 && !observer) {
    return;
  }
  backend::PresentStatistics stats;
  const bool have_stats = swap_chain_.QueryPresentStatistics(stats);
  if (target != 0) {
    schedule_.NotePresented(have_stats ? stats.present_count : 0, target, due, ready);
  }
  if (have_stats && stats.statistics) {
    schedule_.NoteOnScreen(stats.on_screen_present, stats.sync_qpc);
  }
  if (observer) {
    PresentObservation observation;
    observation.guest_present = guest_present;
    if (have_stats) {
      observation.present_count = stats.present_count;
      observation.statistics = stats.statistics;
      observation.on_screen_present = stats.on_screen_present;
      observation.present_refresh = stats.present_refresh;
      observation.sync_refresh = stats.sync_refresh;
      observation.sync_qpc = stats.sync_qpc;
    }
    observation.target_qpc = target;
    observation.due_qpc = due;
    observation.ready_qpc = ready;
    observer(observation);
  }
}

void Renderer::ReportDroppedPresents(uint64_t count) const {
  if (count != 0) {
    REXLOG_INFO("[gpu] present nowait: dropped_present_count={} dropped_present_total={}",
                count, present_drops_.total());
  }
}

void Renderer::FlushDeferredPresent(bool wait) {
  if (!deferred_.pending) {
    return;
  }
  const DeferredPresent d = deferred_;
  deferred_ = {};
  if (device_.IsLost()) {
    return;
  }
  if (wait) {
    // CHECK: verify presentation order when HDR or frame generation changes during a hold.
    WaitForPresentDeadline(d.due);
  }
  PresentNow(d.image, PresentSource::kGuest, d.target, d.due, d.ready, d.guest_present,
             d.latency_frame);
}


bool Renderer::ApplyHdrSetting() {
  if (swap_chain_.lost()) {
    // A switch left the window without a chain (nothing could be made, not even the old format):
    // try again each second, in the format the passes use.
    const auto now = std::chrono::steady_clock::now();
    if (now - swap_chain_retry_ < std::chrono::seconds(1)) {
      return false;
    }
    swap_chain_retry_ = now;
    const FrameGeneration::PauseScope pause(fg_);
    frames_.WaitIdle();
    SwapChain::Recreated result;
    {
      latency::SwapChainChange latency_change;
      result = swap_chain_.Recreate(present_.hdr_output(), WantsSwapChainVsync());
      latency_change.Replaced(swap_chain_);
    }
    dlss::SwapChainRecreated();
    if (result != SwapChain::Recreated::kLost) {
      fg_.OnSwapChainChanged();
      dlssg_.OnSwapChainChanged();
    }
    if (result == SwapChain::Recreated::kLost) {
      if (!swap_chain_lost_logged_) {
        swap_chain_lost_logged_ = true;
        REXLOG_ERROR("[gpu] the window has no swap chain and a new one could not be made; trying "
                     "again each second (nothing is presented meanwhile)");
      }
      return false;
    }
    swap_chain_lost_logged_ = false;
    AdoptSwapChain();
    hdr::SwitchDone(swap_chain_.hdr(), swap_chain_.hdr());
    REXLOG_INFO("[gpu] the swap chain was made again after it was lost: {}x{}, {}", swap_chain_.width(),
                swap_chain_.height(), swap_chain_.hdr() ? "HDR10" : "SDR");
    return true;
  }
  bool to_hdr = false;
  std::string why;
  if (!hdr::SwitchWanted(swap_chain_.hdr(), &to_hdr, &why)) {
    return false;
  }
  return SwitchHdr(to_hdr, why);
}

void Renderer::AdoptSwapChain() {
  // What draws into the back buffer takes the set made for its format. The set of the format the
  // chain has was built (a switch builds the one it asks for first and the chain keeps the other
  // one's only after it was in use); a chain that came back in a format neither was built for is
  // not expected, and is logged.
  const bool hdr = swap_chain_.hdr();
  if (!present_.has_output(hdr)) {
    present_.BuildOutput(SwapChain::FormatFor(hdr), hdr);
  }
  if (!overlay_.has_target(hdr)) {
    overlay_.BuildTarget(SwapChain::FormatFor(hdr), hdr);
  }
  if (!present_.has_output(hdr) || !overlay_.has_target(hdr)) {
    REXLOG_ERROR("[gpu] hdr: the passes for the {} back buffer could not be made; the present and "
                 "the overlay keep the {} ones, which do not match the swap chain",
                 hdr ? "HDR10" : "SDR", present_.hdr_output() ? "HDR10" : "SDR");
  } else {
    present_.UseOutput(hdr);
    overlay_.UseTarget(hdr);
  }
  // The scene half of HDR output follows the chain: layers, float scene and tone map up, or down
  // (their images released at once; the idle wait frees them). Their failures leave an HDR10 output
  // at stage 1 (the front buffer at paper white), as at a start.
  if (hdr) {
    // Also when frame generation already holds the layers up (frame_generation.h): the HDR scene
    // and the tone map are set up (and are no-ops when they are) whenever the output goes HDR10.
    layers_.Request(FrameLayers::kRequesterHdr, true);
    SetUpHdrScene(layers_.active());
  } else if (!hdr && (layers_.active() || hdr_scene_.ready())) {
    layers_.Request(FrameLayers::kRequesterHdr, false);
    hdr_scene_.Release();
    hdr_tone_.Release();
  }
  // A new chain has no metadata; UpdateHdr sends it before the next present. The sizes are the
  // window's, published again as after any rebuild (RebuildSwapChain): the render scale is measured
  // against them at the next frame boundary (temporal.cpp ReadRenderScale), as after a resize.
  hdr_metadata_sent_ = false;
  PublishWindowSize(swap_chain_.width(), swap_chain_.height());
}

// CHECK: not run (written without a build). Look for, in the log of a switch: "hdr: switching A -> B
// (why)" then "hdr: switched A -> B in N ms (passes, swap chain, layers)"; a refusal says "could not
// switch" with the reason above it; the first frames after a switch to HDR may show the scene at
// paper white (stage 2) until the HDR variants' pipelines are made in the background (they are not
// in the pipeline cache when the session started without HDR: the records naming them are dropped
// at load, pipelines.cpp); the picture should flicker at most once.
bool Renderer::SwitchHdr(bool to_hdr, const std::string& why) {
  // A held present (deferred_) names a back buffer of the chain about to be made again: it goes
  // out first, at its time.
  FlushDeferredPresent(/*wait=*/true);
  using Clock = std::chrono::steady_clock;
  const auto millis = [](Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
  };
  const char* const from_name = swap_chain_.hdr() ? "HDR10" : "SDR";
  const char* const to_name = to_hdr ? "HDR10" : "SDR";
  const bool from_hdr = swap_chain_.hdr();
  const auto begin = Clock::now();
  REXLOG_INFO("[gpu] hdr: switching {} -> {} ({})", from_name, to_name, why);

  // 1. What can fail without leaving a trace comes first, while the old chain still runs: the
  //    passes that draw into the new back buffer's format. A set that could not be made leaves
  //    everything as it was.
  if (!present_.BuildOutput(SwapChain::FormatFor(to_hdr), to_hdr) ||
      !overlay_.BuildTarget(SwapChain::FormatFor(to_hdr), to_hdr)) {
    REXLOG_ERROR("[gpu] hdr: could not switch {} -> {}: the present or overlay passes for the {} "
                 "back buffer could not be made (reason above); the output stays {}",
                 from_name, to_name, to_name, from_name);
    hdr::SwitchDone(to_hdr, from_hdr);
    return false;
  }
  if (to_hdr) {
    // The scene variants of the game's tone map; without them the HDR output keeps the scene at
    // paper white (logged). Loaded by OnShaderPackAttached already unless the pack was missing then.
    LoadShaderVariants();
  }
  const auto built = Clock::now();

  // 2. The swap chain, in the other format: nothing in flight may name the back buffers, and the
  //    latency reducer's calls that name the chain wait until the new one is in. With frame
  //    generation the pacer is paused for it (the new chain stays on the present queue).
  const FrameGeneration::PauseScope fg_pause(fg_);
  frames_.WaitIdle();
  SwapChain::Recreated result;
  {
    latency::SwapChainChange latency_change;
    result = swap_chain_.Recreate(to_hdr, WantsSwapChainVsync());
    latency_change.Replaced(swap_chain_);
  }
  dlss::SwapChainRecreated();  // D3D12 with Streamline: the plain new chain is offered again
  if (result != SwapChain::Recreated::kLost) {
    fg_.OnSwapChainChanged();  // the pacer's copy pass is for the chain's format
    dlssg_.OnSwapChainChanged();  // and DLSS frame generation's images and UI pass
  }
  const auto remade = Clock::now();

  if (result == SwapChain::Recreated::kLost) {
    // Not even the old format could be made. The passes stay as they were; ApplyHdrSetting makes
    // the chain again each second.
    REXLOG_ERROR("[gpu] hdr: switching {} -> {} left the window without a swap chain: neither the "
                 "new format nor the old one could be made; trying again each second",
                 from_name, to_name);
    swap_chain_lost_logged_ = true;
    swap_chain_retry_ = Clock::now();
    hdr::SwitchDone(to_hdr, from_hdr);
    return false;
  }
  // 3. What draws into the back buffer takes the new format's set, and the scene half of HDR output
  //    (layers, float scene, tone map) comes up or goes down with it.
  AdoptSwapChain();
  const bool now_hdr = swap_chain_.hdr();
  hdr::SwitchDone(to_hdr, now_hdr);
  const auto done = Clock::now();
  if (now_hdr == to_hdr) {
    REXLOG_INFO("[gpu] hdr: switched {} -> {} in {:.0f} ms (passes {:.0f} ms, swap chain {:.0f} ms, "
                "layers {:.0f} ms): swap chain {}x{}, {}",
                from_name, to_name, millis(begin, done), millis(begin, built),
                millis(built, remade), millis(remade, done), swap_chain_.width(),
                swap_chain_.height(), to_name);
    return true;
  }
  REXLOG_WARN("[gpu] hdr: could not switch {} -> {} (the output does not present the HDR10 colour "
              "space; the reason is above): the {} swap chain was made again and stays, {:.0f} ms; "
              "not tried again until mcla_gpu_hdr or Windows' HDR state changes",
              from_name, to_name, from_name, millis(begin, done));
  return true;
}

void Renderer::UpdateHdr() {
  if (!swap_chain_.hdr()) {
    return;
  }
  const HdrLevels levels = hdr::Levels();
  present_.SetHdrLevels(levels);
  overlay_.SetHdrLevels(levels);
  // The scene's curve is bent towards the display's peak (stage 3): the levels, and the game's display
  // gamma table the peak is inverted through.
  float gamma_table[256];
  hdr_tone_.SetDisplay(levels, present_.GreenGammaTable(gamma_table) ? gamma_table : nullptr);
  // The display tone maps towards what it is told the content reaches: the peak setting.
  const plume::RenderHdrMetadata metadata = hdr::Metadata();
  if (!hdr_metadata_sent_ || metadata.maxMasteringLuminance != hdr_metadata_.maxMasteringLuminance ||
      metadata.minMasteringLuminance != hdr_metadata_.minMasteringLuminance ||
      metadata.maxContentLightLevel != hdr_metadata_.maxContentLightLevel ||
      metadata.maxFrameAverageLightLevel != hdr_metadata_.maxFrameAverageLightLevel) {
    hdr_metadata_ = metadata;
    hdr_metadata_sent_ = true;
    if (!swap_chain_.SetHdrMetadata(metadata)) {
      // Once per change: Vulkan without VK_EXT_hdr_metadata lands here, the display then uses its
      // own defaults.
      REXLOG_WARN("[gpu] hdr: the display could not be told the content's levels (peak {:.0f} "
                  "nits); it uses its own",
                  metadata.maxContentLightLevel);
    }
  }
}

void Renderer::UpdateDisplayAspect() {
  // The guest frame's own 16:9 picture (a movie, a loading screen, a flat menu: no scene composed in
  // it) is not stretched; one with a scene is, to the shape its cameras were built for.
  // CHECK: that every frame that shows a 3D picture has the marker (title, garage, showroom, map,
  // photo mode, the rear-view mirror inside play): one that has none is shown 16:9 with its picture
  // still squeezed.
  // At 16:9 nothing is asked: the fit is the front buffer's own, to the pixel. A taller picture (16:10, 4:3,
  // a portrait window: Vert+, mc_engine/display/ultrawide.h) is asked for the same way as a wider one: the guest drew
  // it stretched sideways into the 16:9 image and the present squeezes it into the frame.
  present_.SetDisplayAspect(scene_.last_frame_had_scene() && mc::ultrawide::Active()
                                ? mc::ultrawide::DisplayAspect()
                                : 0.0);
  // The frame the picture lies in (the ASPECT RATIO setting, mc_engine/display/frame_fit.h): the window itself
  // while the setting follows it, a smaller one in the middle with black around it for a shape the
  // window is not. A 3D picture fills the frame, any other keeps its own shape inside it.
  present_.SetFrameAspect(mc::ultrawide::DisplayAspect());
}

void Renderer::ExecutePresent(PresentSource source) {
  // Capture mode: a guest present tagged as a frame of the capture take must be read back (RecordPresentPass) or reported lost,
  // whichever way this function returns. An untagged present (every present, when no take runs) costs a flag test.
  capture_readback_.BeginPresent(source == PresentSource::kGuest ? present_tag_ : 0);
  struct CapturePresentScope {
    CaptureReadback& readback;
    ~CapturePresentScope() { readback.EndPresent(); }
  } capture_scope{capture_readback_};
  // A held present goes first: its back buffer is the one ahead of this
  // frame's in the chain, and nothing else may touch the chain before it.
#if defined(_WIN32)
  UpdateComposedPresentCeiling();
#endif
  FlushDeferredPresent(/*wait=*/true);
  tail_open_ = false;
  const latency::IdlePresentScope idle_scope(source == PresentSource::kIdle);
  if (device_.IsLost()) {
    ReportDeviceLost();
    // Nothing recorded will run: drop the guest frame being recorded.
    if (frames_.is_open()) {
      scene_.EndFrame(nullptr);
      frames_.Abandon();
      markers_.Drop();
    }
    return;
  }
  if (source == PresentSource::kGuest) {
    guest_has_presented_ = true;
    last_guest_present_ = std::chrono::steady_clock::now();
  }
  if (!frames_.is_open()) {
    // A present with no frame recorded before it: a boundary too (a frame that has scene records
    // applied the setting when it opened).
    ApplyHdrSetting();
    ApplyFrameGenerationSetting();
  }
  struct PresentingScope {
    explicit PresentingScope(bool& flag) : flag_(flag) { flag_ = true; }
    ~PresentingScope() { flag_ = false; }
    bool& flag_;
  } presenting(presenting_);
  // Frame generation presents FIFO (or immediate by its own setting), whatever mcla_gpu_vsync says.
  swap_chain_.SetVsync(WantsSwapChainVsync());
  UpdateHdr();
  UpdateDisplayAspect();

  // The SDK can forward a window event both directly and through ResizeSurface. Consume
  // the notifications together and use the latest size and mode once for this present.
  // and toggle HDR and vsync together, including with frame generation active.
  const uint8_t resize_requests = resize_requests_.exchange(0, std::memory_order_acq_rel);
  const bool rebuild_attempted = swap_chain_.NeedsRebuild();
  if (rebuild_attempted) {
    RebuildSwapChain();
  } else if (resize_requests && !swap_chain_.empty()) {
    const char* reason = "SDL pixel-size event and SDK surface resize";
    if (resize_requests == static_cast<uint8_t>(ResizeSource::kWindowEvent)) {
      reason = "SDL pixel-size event";
    } else if (resize_requests == static_cast<uint8_t>(ResizeSource::kSdkSurface)) {
      reason = "SDK surface resize";
    }
    REXLOG_INFO("[gpu] swap chain rebuild skipped: nothing changed ({})", reason);
  }
  if (swap_chain_.empty()) {
    // Minimised, or a rebuild the backend refused: nothing to present into.
    // The guest frame still runs (its targets and resolves feed later frames);
    // overlay uploads and deaths wait in the channel for the next frame.
    SubmitFrameWithoutPresent();
    ++skipped_;
    return;
  }
  if (fg_.active()) {
    ExecutePresentGenerated(source);
    return;
  }

  if (!dlssg_.active()) {
    // Streamline's pacer owns the chain's frame latency while DLSS frame generation runs (the
    // game's Reflex sleep paces the frames instead); the waitable object is not waited on.
    swap_chain_.WaitForPresentSlot();
  }
  uint32_t image = 0;
  if (!swap_chain_.Acquire(frames_.acquire_semaphore(), &image)) {
    // A failed acquire did not signal the semaphore. If this present already rebuilt the
    // buffers, leave the failure pending so the next present can recover them.
    if (rebuild_attempted || !RebuildSwapChain() ||
        !swap_chain_.Acquire(frames_.acquire_semaphore(), &image)) {
      device_.CheckLost("swap chain acquire");
      SubmitFrameWithoutPresent();
      ++skipped_;
      return;
    }
  }
  if (dlssg_.active()) {
    // Streamline's fake swap chain image, which its frame generation may have left in GENERAL: the
    // present pass overwrites all of it, so the first barrier on it starts from UNDEFINED (Vulkan).
    backend::ForgetImageContents(swap_chain_.texture(image));
  }

  // Already open when the guest frame had scene records.
  plume::RenderCommandList* list = OpenFrameList();
  if (!list) {
    ++skipped_;
    return;
  }
  const uint32_t slot = frames_.slot();
  // Outside any render pass: the scene's last work (geometry promotions), then
  // the overlay's texture uploads.
  markers_.Pop(list, PerfMarkers::kPhase);  // Scene
  markers_.Push(list, "EndOfFrame", PerfMarkers::kPhase);
  scene_.EndFrame(list);
  markers_.Pop(list, PerfMarkers::kPhase);
  overlay_.Prepare(list, slot);
  RecordPresentPass(list, slot, image, source);

  plume::RenderCommandSemaphore* waits[] = {frames_.acquire_semaphore()};
  plume::RenderCommandSemaphore* signals[] = {swap_chain_.render_semaphore(image)};
  if (!frames_.Submit(waits, 1, signals, 1)) {
    // Device lost: the render semaphore will never be signalled, so presenting
    // would wait forever on Vulkan.
    frames_.Advance();
    ++skipped_;
    return;
  }
  timing_.OnSubmitted(slot);
  pass_timing_.OnSubmitted(slot);
  occlusion_.OnSubmitted(slot);
  gi_update_.OnSubmitted(slot);
  gi_pixel_.OnSubmitted(slot);
  latency::OnRenderSubmitted();

  // A scheduled guest frame (gpu.h SetNextPresentTarget) goes to the screen at
  // its time: submitted now, presented by the render thread loop when the time
  // comes (RunDeferred), so this thread executes the next frame's records
  // meanwhile and neither the guest nor the GPU waits on the hold.
  const int64_t ready_qpc = QpcNow();
  // The reducer's present markers name this frame whenever its present runs.
  const uint64_t latency_frame = latency::DetachRenderFrame();
  // When to present it (present_schedule.h): its time plus the lateness carried
  // from late frames before it; 0 = at once.
  // Streamline's pacer owns the presents while DLSS frame generation runs (SchedulesPresents).
  const int64_t due = source == PresentSource::kGuest && present_target_ != 0 && !dlssg_.active()
                          ? schedule_.Due(present_target_, ready_qpc)
                          : 0;
  if (due != 0) {
    deferred_ = {true, image, present_target_, due, ready_qpc, guest_presents_ + 1,
                 latency_frame};
  } else {
    PresentNow(image, source, present_target_, 0, ready_qpc, guest_presents_ + 1,
               latency_frame);
  }
  capture_.OnPresented();
  frames_.Advance();
  // The slot about to be recorded had its fence waited by Advance.
  upload_.ResetSlot(frames_.slot());
  // What its previous round counted for the guest's occlusion queries is readable now.
  occlusion_.OnSlotReady(frames_.slot());
  // Capture mode: in lock-step the take's readback of this frame is done before the present returns.
  capture_readback_.AfterAdvance();
  ++presented_;
  if (source == PresentSource::kGuest) {
    ++guest_presents_;
  } else {
    ++idle_presents_;
  }
  ReportFrameStats();
  overlay_host_->RequestPass();
  if (tail_open_ && source == PresentSource::kGuest) {
    tail_short_ = std::chrono::steady_clock::now() - tail_start_ <= kEarlyReleaseMaxTail;
  }
  tail_open_ = false;
}

bool Renderer::ApplyFrameGenerationSetting() {
  const FrameGeneration::Change change = fg_.Poll();
  if (change != FrameGeneration::Change::kNone) {
    return SwitchFrameGeneration(change);
  }
  // The two never run together: the setting names one of them (FSR's Poll ignores "dlss").
  const DlssFrameGeneration::Change dlss_change = dlssg_.Poll();
  if (dlss_change != DlssFrameGeneration::Change::kNone) {
    return SwitchDlssFrameGeneration(dlss_change, /*resize=*/false);
  }
  return false;
}

// CHECK: not run (written without a build). The log of a switch on: "[gpu] dlss frame generation:
// switching on", then Streamline's own sl.log lines for the plugin loading and the proxy factory,
// "on (Nx: ...)", and once Reflex has made a few sleeps "generating: Nx". The swap chain is made
// again twice around a resize (off, then on after the window has been still): the picture may flicker.
bool Renderer::SwitchDlssFrameGeneration(DlssFrameGeneration::Change change, bool resize) {
  // A held present (deferred_) names a back buffer of the chain about to be made again: it goes
  // out first, at its time.
  FlushDeferredPresent(/*wait=*/true);
  using Clock = std::chrono::steady_clock;
  const bool on = change == DlssFrameGeneration::Change::kOn;
  const auto begin = Clock::now();
  if (on) {
    // The plugin is loaded before the chain is made through its proxy of the DXGI factory.
    switch (dlssg_.PrepareSwitchOn()) {
      case DlssFrameGeneration::Prepared::kWait:
        return false;  // Streamline is making a feature: asked again at the next frame boundary
      case DlssFrameGeneration::Prepared::kFailed:
        return false;  // logged; off for the session
      case DlssFrameGeneration::Prepared::kReady:
        break;
    }
  }
  REXLOG_INFO("[gpu] dlss frame generation: switching {}{} (the swap chain is made again{})",
              on ? "on" : "off", resize ? " for a change of the window" : "",
              on ? " as Streamline's own" : ", plain");
  const bool hdr = swap_chain_.hdr();
  bool ok = true;
  frames_.WaitIdle();
  SwapChain::Recreated result;
  {
    // The latency reducer's calls that name the chain wait until the new one is in.
    latency::SwapChainChange latency_change;
    swap_chain_.SetFactoryWrapper(on ? &dlss::UpgradeFactory : nullptr);
    result = swap_chain_.RecreateOn(nullptr, hdr,
                                    on ? dlssg_.WantsVsync() : larecomp::settings::mcla_gpu_vsync.Get(),
                                    on ? std::function<void()>{}
                                       : std::function<void()>([this] { dlssg_.BeforePlainChain(); }));
    latency_change.Replaced(swap_chain_);
  }
  if (result == SwapChain::Recreated::kLost && on) {
    // Streamline's proxy chain could not be made: the plain chain is made again, and frame
    // generation is off for the session.
    ok = false;
    latency::SwapChainChange latency_change;
    swap_chain_.SetFactoryWrapper(nullptr);
    result = swap_chain_.RecreateOn(nullptr, hdr, larecomp::settings::mcla_gpu_vsync.Get(),
                                    [this] { dlssg_.BeforePlainChain(); });
    latency_change.Replaced(swap_chain_);
  }
  if (!on || !ok) {
    dlss::SwapChainRecreated();  // the plain new chain is offered to Streamline's wrapper again
  }
  if (result == SwapChain::Recreated::kLost) {
    REXLOG_ERROR("[gpu] dlss frame generation: switching {} left the window without a swap chain; "
                 "trying again each second (nothing is presented meanwhile)",
                 on ? "on" : "off");
    swap_chain_lost_logged_ = true;
    swap_chain_retry_ = Clock::now();
    ok = false;
  }
  // A new chain has no metadata (UpdateHdr sends it before the next present), and its size is what the
  // render scale is measured against.
  hdr_metadata_sent_ = false;
  PublishWindowSize(swap_chain_.width(), swap_chain_.height());
  dlssg_.CompleteSwitch(change, ok, resize);
  REXLOG_INFO("[gpu] dlss frame generation: switched {} in {:.0f} ms", on ? "on" : "off",
              std::chrono::duration<double, std::milli>(Clock::now() - begin).count());
  return true;
}

// CHECK: not run (written without a build). Look for, in the log of a switch: "frame generation:
// switching on/off", then "frame generation: on (FSR 3.1; ...)" or "off"; the first frames after it
// are real frames only until the context is made ("[gpu] frame generation (ffx): context for ... made
// in N ms"), then "first generation recorded". The swap chain is made again on the other queue, as an
// HDR switch does: the picture may flicker once. D3D12: the debug layer on the present queue's lists
// and the cross-queue use of the present images (the render queue wrote them, the present queue read
// them after a CPU wait on a fence); Vulkan: validation on the second queue (the swap chain's images
// are used on a queue other than the one that made the frame, the family is the same) and, on a GPU
// whose DIRECT family has one queue (AMD, Intel), that the shared VkQueue's lock does not hold the
// render thread's submissions behind a blocked present.
bool Renderer::SwitchFrameGeneration(FrameGeneration::Change change) {
  // A held present (deferred_) names a back buffer of the chain about to be made again: it goes
  // out first, at its time.
  FlushDeferredPresent(/*wait=*/true);
  using Clock = std::chrono::steady_clock;
  const bool on = change == FrameGeneration::Change::kOn;
  const auto begin = Clock::now();
  REXLOG_INFO("[gpu] frame generation: switching {} (the swap chain is made again on the {} queue)",
              on ? "on" : "off", on ? "present" : "render");
  if (on && !fg_.PrepareSwitchOn(swap_chain_.format())) {
    return false;  // logged; frame generation stays off for the session
  }
  // The swap chain on the other queue: nothing in flight may name the back buffers (on the render
  // queue by WaitIdle, on the present queue by the pacer's pause), and the latency reducer's calls
  // that name the chain wait until the new one is in.
  const bool hdr = swap_chain_.hdr();
  bool ok = true;
  {
    const FrameGeneration::PauseScope pause(fg_);
    frames_.WaitIdle();
    SwapChain::Recreated result;
    {
      latency::SwapChainChange latency_change;
      result = swap_chain_.RecreateOn(on ? fg_.present_queue() : nullptr, hdr,
                                      on ? fg_.WantsVsync() : larecomp::settings::mcla_gpu_vsync.Get());
      latency_change.Replaced(swap_chain_);
    }
    dlss::SwapChainRecreated();  // D3D12 with Streamline: the plain new chain is offered again
    if (result == SwapChain::Recreated::kLost && on) {
      // The present queue could not have it: the render queue's chain is made again, and frame
      // generation is off for the session.
      ok = false;
      latency::SwapChainChange latency_change;
      result = swap_chain_.RecreateOn(nullptr, hdr, larecomp::settings::mcla_gpu_vsync.Get());
      latency_change.Replaced(swap_chain_);
      dlss::SwapChainRecreated();
    }
    if (result == SwapChain::Recreated::kLost) {
      REXLOG_ERROR("[gpu] frame generation: switching {} left the window without a swap chain; "
                   "trying again each second (nothing is presented meanwhile)",
                   on ? "on" : "off");
      swap_chain_lost_logged_ = true;
      swap_chain_retry_ = Clock::now();
      ok = false;
    }
  }
  // A new chain has no metadata (UpdateHdr sends it before the next present), and its size is what the
  // render scale is measured against.
  hdr_metadata_sent_ = false;
  PublishWindowSize(swap_chain_.width(), swap_chain_.height());
  fg_.CompleteSwitch(change, ok);
  REXLOG_INFO("[gpu] frame generation: switched {} in {:.0f} ms", on ? "on" : "off",
              std::chrono::duration<double, std::milli>(Clock::now() - begin).count());
  return true;
}

void Renderer::DrawPresentInto(plume::RenderCommandList* list, uint32_t slot, HostImage& target,
                               uint32_t width, uint32_t height) {
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(target.texture.get(),
                                             plume::RenderTextureLayout::COLOR_WRITE));
  list->setFramebuffer(target.framebuffer.get());
  // Black around the letterboxed picture (and a first use of the image is initialised).
  list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
  present_.Draw(list, width, height);
  overlay_draws_ += overlay_.Draw(list, slot, width, height);
  list->setFramebuffer(nullptr);
  // The pacer's copy pass reads it next (on the present queue, after a CPU wait for the frame).
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(target.texture.get(),
                                             plume::RenderTextureLayout::SHADER_READ));
  target.layout = plume::RenderTextureLayout::SHADER_READ;
}

bool Renderer::RecordGeneratedPresent(plume::RenderCommandList* list, uint32_t slot,
                                      const FrameGeneration::Pair& pair, PresentSource source) {
  const uint32_t width = swap_chain_.width();
  const uint32_t height = swap_chain_.height();
  pass_timing_.Mark(list, "present");
  markers_.Push(list, "Present", PerfMarkers::kPhase);
  // The real frame, as ever, into the pair's real image: the front buffer through the display's
  // table, FSR1, the overlay.
  present_.Prepare(list, slot, width, height);
  if (source == PresentSource::kGuest && !swap_chain_.hdr()) {
    flicker_probe_.Record(list, present_, guest_presents_ + 1);
  }
  ReleaseGuest();
  FrameGeneration::GenerateArgs args;
  const bool have_front = present_.front_key(&args.front) && present_.source_ready();
  args.guest_frame = source == PresentSource::kGuest && have_front;
  args.front_width = present_.source_width();
  args.front_height = present_.source_height();
  args.window_width = width;
  args.window_height = height;
  timing_.Mark(list, slot, GpuTiming::kOverlayBegin);
  DrawPresentInto(list, slot, *pair.real, width, height);
  timing_.Mark(list, slot, GpuTiming::kOverlayEnd);
  // The generated frame, when this frame can be generated: the same present path over the generated
  // scene with this frame's UI on it, into the pair's other image.
  bool has_generated = false;
  const FrameGeneration::Generated generated = fg_.Generate(list, args);
  if (generated.image) {
    present_.PrepareImage(list, width, height, *generated.image, generated.float_scene);
    if (present_.source_ready()) {
      DrawPresentInto(list, slot, *pair.generated, width, height);
      has_generated = true;
    }
  }
  markers_.Pop(list, PerfMarkers::kPhase);  // Present
  markers_.PopAll(list);                    // Frame
  timing_.Mark(list, slot, GpuTiming::kFrameEnd);
  pass_timing_.CompleteFrame(list, slot);
  return has_generated;
}

void Renderer::ExecutePresentGenerated(PresentSource source) {
  // ExecutePresent's second half with frame generation running: the swap chain is the pacer's, so
  // nothing is acquired or presented here; a slot of present images is taken (waiting for the pacer
  // when it is a frame behind), the frame is painted into it and the pair is handed over.
  FrameGeneration::Pair pair;
  if (!fg_.AcquireSlot(swap_chain_.width(), swap_chain_.height(), swap_chain_.format(), &pair)) {
    // No free slot in time (the pacer is stalled), or no image: this frame is not shown.
    SubmitFrameWithoutPresent();
    ++skipped_;
    return;
  }
  // Already open when the guest frame had scene records.
  plume::RenderCommandList* list = OpenFrameList();
  if (!list) {
    fg_.ReturnSlot(pair);
    ++skipped_;
    return;
  }
  const uint32_t slot = frames_.slot();
  markers_.Pop(list, PerfMarkers::kPhase);  // Scene
  markers_.Push(list, "EndOfFrame", PerfMarkers::kPhase);
  scene_.EndFrame(list);
  markers_.Pop(list, PerfMarkers::kPhase);
  overlay_.Prepare(list, slot);
  const bool has_generated = RecordGeneratedPresent(list, slot, pair, source);

  if (!frames_.Submit(nullptr, 0, nullptr, 0)) {
    fg_.ReturnSlot(pair);
    frames_.Advance();
    ++skipped_;
    return;
  }
  timing_.OnSubmitted(slot);
  pass_timing_.OnSubmitted(slot);
  occlusion_.OnSubmitted(slot);
  gi_update_.OnSubmitted(slot);
  gi_pixel_.OnSubmitted(slot);
  latency::OnRenderSubmitted();
  // The present markers are the pacer's: it names the frame around the real image's present.
  const uint64_t latency_frame = latency::DetachRenderFrame();
  fg_.Submit(pair, has_generated, latency_frame, source == PresentSource::kGuest);
  capture_.OnPresented();
  frames_.Advance();
  // The slot about to be recorded had its fence waited by Advance.
  upload_.ResetSlot(frames_.slot());
  occlusion_.OnSlotReady(frames_.slot());
  ++presented_;
  if (source == PresentSource::kGuest) {
    ++guest_presents_;
  } else {
    ++idle_presents_;
  }
  ReportFrameStats();
  overlay_host_->RequestPass();
  if (tail_open_ && source == PresentSource::kGuest) {
    tail_short_ = std::chrono::steady_clock::now() - tail_start_ <= kEarlyReleaseMaxTail;
  }
  tail_open_ = false;
}

void Renderer::ReleaseGuest() {
  // From here the present reads no guest memory: the scene's end-of-frame work, the
  // front buffer's texture (present_.Prepare) and the gamma table are done; what follows
  // draws from the GPU's copies, submits and presents.
  tail_start_ = std::chrono::steady_clock::now();
  tail_open_ = true;
  if (release_sequence_ == 0 || !release_early_ || (!tail_short_ && !check_release_) ||
      larecomp::settings::mcla_gpu_vsync.Get()) {
    return;
  }
  thread_.stream().MarkExecuted(release_sequence_);
  release_sequence_ = 0;
  ++early_releases_;
  if (check_release_) {
    // From here to the end of the present, a read of guest memory is a bug of the release.
    GuestMemory::MarkReleased(true);
  }
}

void Renderer::RecordPresentPass(plume::RenderCommandList* list, uint32_t slot, uint32_t image,
                                 PresentSource source) {
  // Outside the back buffer's pass: the front buffer's texture (an upload when
  // it comes from memory), the gamma table and FSR1's passes
  // before the back buffer's.
  pass_timing_.Mark(list, "present");
  markers_.Push(list, "Present", PerfMarkers::kPhase);
  present_.Prepare(list, slot, swap_chain_.width(), swap_chain_.height());
  if (source == PresentSource::kGuest && !swap_chain_.hdr()) {
    // An idle present shows the same front buffer again: not a new frame. (Not with an HDR10 back
    // buffer: the probe records 8-bit SDR, and a live HDR switch can turn the chain HDR10 under it.)
    flicker_probe_.Record(list, present_, guest_presents_ + 1);
  }
  capture_readback_.Record(list, present_, gbuffer_, layers_);  // a no-op unless tagged
  ReleaseGuest();
  plume::RenderTexture* back = swap_chain_.texture(image);
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(back, plume::RenderTextureLayout::COLOR_WRITE));
  list->setFramebuffer(swap_chain_.framebuffer(image));
  // Black around the letterboxed front buffer.
  list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
  present_.Draw(list, swap_chain_.width(), swap_chain_.height());
  markers_.Pop(list, PerfMarkers::kPhase);  // Present
  timing_.Mark(list, slot, GpuTiming::kOverlayBegin);
  pass_timing_.Mark(list, "overlay");
  markers_.Push(list, "Overlay", PerfMarkers::kPhase);
  overlay_draws_ += overlay_.Draw(list, slot, swap_chain_.width(), swap_chain_.height());
  markers_.Pop(list, PerfMarkers::kPhase);
  timing_.Mark(list, slot, GpuTiming::kOverlayEnd);
  list->setFramebuffer(nullptr);
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(back, plume::RenderTextureLayout::PRESENT));
  if (dlssg_.active()) {
    // DLSS frame generation: what the plugin is given with the back buffer (the picture without the
    // UI, the UI, the depth and the vectors) and its options for this frame; the present pass's own
    // state (the front buffer it bound) is read first, the passes below overwrite it.
    DlssFrameGeneration::FrameArgs args;
    const bool have_front = present_.front_key(&args.front) && present_.source_ready();
    args.guest_frame = source == PresentSource::kGuest && have_front;
    args.front_width = present_.source_width();
    args.front_height = present_.source_height();
    args.window_width = swap_chain_.width();
    args.window_height = swap_chain_.height();
    args.back_buffers = swap_chain_.image_count();
    args.hdr = swap_chain_.hdr();
    dlssg_.Record(list, args);
  }
  markers_.PopAll(list);  // Frame
  timing_.Mark(list, slot, GpuTiming::kFrameEnd);
  pass_timing_.CompleteFrame(list, slot);
}

void Renderer::ReportCityCensus() {
  // The texts CityRtScene (with the hit data's pool inside it), CityRtShadow and RtSceneService define for a census line were never printed by anything: perf/scripts/city_rt_h3_report.py
  // reads their fragments by name ("city hit data: pool ...", "ray tracing scene: wanted by ...") from a line like this one. Each text starts with "; " and is empty where it has nothing
  // to say (no pack open, the shadows not ready, nobody wanting the scene).
  constexpr uint64_t kCityCensusPresents = 600;  // as car_selfrefl.cpp's kCensusFrames
  if (presented_ % kCityCensusPresents != 0) {
    return;
  }
  const std::string text = city_rt_.CensusText() + city_shadow_.CensusText() + rt_scene_.CensusText() + rt_world_.CensusText() + ssr_.RtCensusText() + gi_update_.CensusText() + gi_pixel_.CensusText() + rt_lights_.CensusText();
  if (!text.empty()) {
    REXLOG_INFO("[city rt] census of present {}{}", presented_, text);
  }
}

void Renderer::ReportFrameStats() {
  ReportCityCensus();
  // Once a minute at 60 fps: enough to see VRAM creep or a frame ring that
  // stops reclaiming, without a line per frame.
  if (presented_ % 3600 != 0) {
    return;
  }
  const auto vram = device_.QueryVideoMemory();
  // What the renderer itself holds, by kind: where the VRAM goes (the budget trim releases the first
  // four when memory runs short, vram_budget.cpp; the frame layers give theirs back after
  // FrameLayers::kReleaseAfterFrames guest frames without layers).
  const auto mib = [](uint64_t bytes) { return std::to_string((bytes + (1u << 19)) >> 20); };
  const std::string held = " (held: textures " + mib(textures_.HeldBytes()) + ", resolve products " +
                           mib(resolve_.HeldBytes()) + ", EDRAM targets " + mib(edram_.HeldBytes()) +
                           ", resolve memory " + mib(resolve_memory_.HeldBytes()) +
                           (layers_.active() ? ", frame layers " + mib(layers_.HeldBytes() +
                                                                      hdr_scene_.HeldBytes() +
                                                                      hdr_tone_.HeldBytes())
                                             : std::string()) +
                           ", ambient occlusion " + mib(ao_.HeldBytes()) +
                           ", sun shafts " + mib(shafts_.HeldBytes()) +
                           (volumetrics_.HeldBytes() != 0 ? ", volumetric light " + mib(volumetrics_.HeldBytes()) : std::string()) +
                           (gbuffer_.HeldBytes() != 0 ? ", G-buffer " + mib(gbuffer_.HeldBytes()) : std::string()) +
                           (screen_.HeldBytes() != 0 ? ", screen products " + mib(screen_.HeldBytes()) : std::string()) +
                           (ssr_.HeldBytes() != 0 ? ", screen-space reflections " + mib(ssr_.HeldBytes()) : std::string()) +
                           (gi_apply_.HeldBytes() != 0 ? ", ray traced bounce light's apply " + mib(gi_apply_.HeldBytes()) : std::string()) +
                           (gi_field_.HeldBytes() != 0 ? ", probe field " + mib(gi_field_.HeldBytes()) : std::string()) +
                           (rayregen::Current().context_bytes != 0 ? ", AMD FSR Ray Regeneration's context " + mib(rayregen::Current().context_bytes) : std::string()) +
                           (car_refl_.HeldBytes() != 0 ? ", car self-reflection " + mib(car_refl_.HeldBytes())
                                                       : std::string()) +
                           (car_shadow_.HeldBytes() != 0 ? ", car shadow map " + mib(car_shadow_.HeldBytes())
                                                         : std::string()) +
                           (mirror_view_.HeldBytes() != 0 ? ", mirror view " + mib(mirror_view_.HeldBytes()) : std::string()) +
                           (local_shadow_.HeldBytes() != 0 ? ", headlight shadow atlas " + mib(local_shadow_.HeldBytes())
                                                           : std::string()) +
                           (car_rt_.HeldBytes() != 0 ? ", car ray tracing " + mib(car_rt_.HeldBytes())
                                                     : std::string()) +
                           // CITY-RT BLOCK: the city's structures, their scratch and the traced shadow image (perf/CITY-RT-M1-RUNPLAN.md R4 reads this)
                           (city_rt_.HeldBytes() + city_shadow_.HeldBytes() != 0
                                ? ", city ray tracing " + mib(city_rt_.HeldBytes() + city_shadow_.HeldBytes())
                                : std::string()) +
                           // CITY-RT M2 H3: the hit data's pool, the tables and the material table
                           (city_hit_.HeldBytes() - city_hit_.texture_set().HeldBytes() != 0 ? ", city hit data " + mib(city_hit_.HeldBytes() - city_hit_.texture_set().HeldBytes())
                                                                                               : std::string()) +
                           // CITY-RT M2 H2: the resident city textures, as the allocator counts them (city_texture_rules.h AllocationBytes; the census line "[city tex]" has the rest)
                           (city_hit_.texture_set().HeldBytes() != 0 ? ", city textures " + mib(city_hit_.texture_set().HeldBytes()) : std::string()) +
                           (car_mirror_.HeldBytes() != 0 ? ", side mirrors " + mib(car_mirror_.HeldBytes())
                                                         : std::string()) +
                           (fg_.active() ? ", frame generation " + mib(fg_.HeldBytes())
                                         : std::string()) +
                           (dlssg_.active() ? ", DLSS frame generation " + mib(dlssg_.HeldBytes())
                                            : std::string()) +
                           ", upscaler (output, depth, vectors, its own context) " +
                           mib(temporal_.HeldBytes()) + " MiB)";
  const std::string vram_text =
      (vram.budget ? std::to_string(vram.used >> 20) + " of " + std::to_string(vram.budget >> 20) +
                         " MiB"
                   : std::string("not reported")) +
      held;
  CommandStream& stream = thread_.stream();
  const uint64_t fence_waits = g_fence_waits.exchange(0, std::memory_order_relaxed);
  const uint64_t fence_wait_ns = g_fence_wait_ns.exchange(0, std::memory_order_relaxed);
  const uint64_t locks = g_guest_locks.exchange(0, std::memory_order_relaxed);
  const uint64_t locks_on_destinations =
      g_guest_locks_on_destinations.exchange(0, std::memory_order_relaxed);
  REXLOG_INFO(
      "[gpu] {} presents ({} guest, {} idle), {} skipped | {} | overlay {} draws, {} textures | "
      "bindless {} of {}, samplers {} | upload {} chunks | {} batch flushes | {} retired pending "
      "| VRAM {} | {}x{} | records: {} draws, {} clears, {} resolves, {}/{} tiling, {} fences "
      "(last {}), {} locks | guest locks {} ({} on resolve destinations) | guest fence waits {} "
      "({:.1f} ms) | stream {} MiB total, {} consumer wakes, {} producer waits",
      presented_, guest_presents_, idle_presents_, skipped_, timing_.TakeSummary(),
      overlay_draws_, overlay_.texture_count(), heap_.textures_in_use(),
      kBindlessTextureCount - kFirstAllocatableTextureSlot, heap_.samplers_in_use(),
      upload_.chunk_count(), frames_.TakeBatchFlushes(), frames_.retired_count(), vram_text,
      swap_chain_.width(), swap_chain_.height(),
      records_[size_t(RecordType::kDraw)], records_[size_t(RecordType::kClear)],
      records_[size_t(RecordType::kResolve)], records_[size_t(RecordType::kTilingBegin)],
      records_[size_t(RecordType::kTilingEnd)], records_[size_t(RecordType::kFence)],
      last_fence_.load(std::memory_order_relaxed), records_[size_t(RecordType::kLock)], locks,
      locks_on_destinations, fence_waits, double(fence_wait_ns) / 1e6,
      stream.bytes_written() >> 20, stream.consumer_wakes(), stream.producer_waits());
  REXLOG_INFO("[gpu] {}", present_.TakeSummary());
  if (const std::string schedule = schedule_.TakeSummary(); !schedule.empty()) {
    REXLOG_INFO("[gpu] {}", schedule);
  }
  if (fg_.active()) {
    REXLOG_INFO("[gpu] {}", fg_.TakeSummary());
  }
  if (dlssg_.active()) {
    REXLOG_INFO("[gpu] {}", dlssg_.TakeSummary());
  }
  REXLOG_INFO("[gpu] {}", post_.TakeSummary());
  if (layers_.active()) {
    REXLOG_INFO("[gpu] {}", layers_.TakeSummary());
    REXLOG_INFO("[gpu] {}", hdr_scene_.TakeSummary());
    if (hdr_tone_.active()) {
      REXLOG_INFO("[gpu] {}", hdr_tone_.TakeSummary());
    }
  }
  if (flicker_probe_.enabled()) {
    REXLOG_INFO("[gpu] {}", flicker_probe_.TakeSummary());
  }
  if (pass_timing_.enabled()) {
    REXLOG_INFO("[gpu] {}", pass_timing_.TakeSummary());
  }
  overlay_draws_ = 0;
  for (uint64_t& count : records_) {
    count = 0;
  }
}

void Renderer::ReportDeviceLost() {
  if (device_lost_reported_ || !device_.IsLost()) {
    return;
  }
  device_lost_reported_ = true;
  // What the renderer does about it is what it already did on D3D12: nothing more is submitted or
  // presented (Device::IsLost stops every creation, submit and wait), the guest runs on.
  REXLOG_ERROR(
      "[gpu] device lost: no more work is issued and nothing more is presented (the game runs on "
      "without a picture; it has to be restarted) | {} presents ({} guest, {} idle), {} skipped, "
      "device frame {}",
      presented_, guest_presents_, idle_presents_, skipped_, device_.frame());
  const std::vector<PassTiming::RecordedFrame> frames = pass_timing_.RecordedFrames();
  if (frames.empty()) {
    REXLOG_ERROR(
        "[gpu] device lost: the passes of the last frames are not kept (mcla_gpu_pass_timing is "
        "off; with it on, they are listed here, oldest frame first)");
    return;
  }
  for (const PassTiming::RecordedFrame& frame : frames) {
    REXLOG_ERROR("[gpu] device lost: frame in slot {} ({}), {} passes: {}", frame.slot,
                 frame.open ? "being recorded" : "submitted", frame.labels.size(),
                 device_fault::FormatLabelTrail(frame.labels, 12));
  }
}

// ---------------------------------------------------------------------------
// Module entry points (gpu.h)
// ---------------------------------------------------------------------------

namespace {

// Never destroyed: a guest thread that loaded the pointer just before Shutdown
// can still call into it, a render thread that missed the stop deadline still
// runs inside it, and every entry point is a no-op once shut down. The process
// ends in _Exit right after (LarecompApp::OnShutdown).
Renderer* g_renderer = nullptr;
std::atomic<Renderer*> g_active{nullptr};
// Same lifetime as g_renderer: a guest thread inside a capture when Shutdown
// runs finishes on the stopped stream.
GuestCapture* g_capture = nullptr;
std::atomic<GuestCapture*> g_capture_active{nullptr};
// The render thread writes the guest's retired fence: from the
// startup hand-over until Shutdown gives the write back to the vblank thread.
std::atomic<bool> g_holds_retired_fence{false};

// UI thread. Made by Startup, shared with the renderer; Shutdown switches the host off.
std::shared_ptr<OverlayChannel>& g_overlay_channel = larecomp::Leak<std::shared_ptr<OverlayChannel>>();
std::shared_ptr<OverlayHost>& g_overlay_host = larecomp::Leak<std::shared_ptr<OverlayHost>>();
// What graphics_backend asks this renderer for (RequestedBackend), decided with
// Enabled().
Backend g_requested_backend = kDefaultBackend;
// Set only when "auto" chose Vulkan: the GPU that choice was made on, which the Vulkan device is
// created on. Empty otherwise.
std::string& g_auto_adapter = larecomp::Leak<std::string>();

// The SDK's graphics_backend -- the cvar that also picks the
// emulated GPU's backend -- as a backend this build has.
Backend RequestedBackend() {
  if (!larecomp::cvar::GetFlagInfo("graphics_backend")) {
    REXLOG_INFO("[gpu] this SDK has no graphics_backend cvar; {} runs",
                BackendName(kDefaultBackend));
    return kDefaultBackend;
  }
  const std::string text = larecomp::cvar::GetFlagByName("graphics_backend");
  Backend backend = kDefaultBackend;
  if (!ParseBackend(text, backend)) {
    REXLOG_ERROR("[gpu] graphics_backend = \"{}\" is not auto, d3d12 or vulkan; {} runs", text,
                 BackendName(backend));
  } else if (!BackendCompiled(backend)) {
    REXLOG_ERROR("[gpu] graphics_backend = \"{}\", but this build has no {}; {} runs", text,
                 BackendName(backend), BackendName(kDefaultBackend));
    backend = kDefaultBackend;
  } else if (text == "auto") {
    backend = ChooseAutoBackend(larecomp::settings::mcla_gpu_adapter.Get(), g_auto_adapter);
  }
  return backend;
}

static_assert(mcla::native_gfx::nocp::kDriverName == kLarecompDriverName,
              "renderer_choice.h and the driver of nocp_driver.h name the same driver");

// Which renderer runs is the SDK's gpu.driver (renderer_choice.h has the rule, and why the old mcla_gpu is not asked).
bool ResolveEnabled(const std::string& driver) {
  if (RendererChoiceOf(driver) != RendererChoice::kLarecomp) {
    REXLOG_INFO("[gpu] gpu.driver = {}: the SDK's Xenia GPU renders (the compatibility renderer); the Plume renderer stays off. "
                "The native renderer is back with gpu.driver = larecomp in larecomp.toml, or with the DEBUG menu's RENDERER row",
                driver);
    return false;
  }
  const char* missing = nullptr;
  if (larecomp::settings::mcla_native_gfx.Get()) {
    missing = "mcla_native_gfx = false (the legacy runtime would present into the same window)";
  }
  // No user_name is fine: the SDK's first-launch profile dialog is drawn by the
  // overlay drawer and presented by the idle tick before the guest exists
  // (tested on a scratch config).
  if (missing) {
    REXLOG_ERROR("[gpu] gpu.driver = {} but the Plume renderer needs {}; it stays off", driver, missing);
    return false;
  }
  g_requested_backend = RequestedBackend();
  REXLOG_INFO("[gpu] gpu.driver = {}: the Plume renderer ({}) owns presentation", driver,
              BackendName(g_requested_backend));
  return true;
}

}  // namespace

bool Enabled() {
  // -1: not decided. Decided on the first call that finds gpu.driver, and fixed for the process; the answer is read at the rate of
  // the guest's hooks, so the settled path is one load.
  static std::atomic<int> state{-1};
  if (const int known = state.load(std::memory_order_acquire); known >= 0) {
    return known == 1;
  }
  static std::mutex decide;
  std::lock_guard lock(decide);
  if (const int known = state.load(std::memory_order_relaxed); known >= 0) {
    return known == 1;
  }
  const std::string driver = larecomp::cvar::GetFlagByName("gpu.driver");
  if (RendererChoiceOf(driver) == RendererChoice::kNotSet) {
    return false;  // asked before the settings exist (not a decision: ask again later)
  }
  const bool enabled = ResolveEnabled(driver);
  state.store(enabled ? 1 : 0, std::memory_order_release);
  return enabled;
}

Device::Options PlannedDeviceOptions(const Backend* forced) {
  Device::Options options;
  options.preferred_adapter = larecomp::settings::mcla_gpu_adapter.Get();
  if (options.preferred_adapter.empty() && (!forced || *forced == g_requested_backend)) {
    // "auto" chose Vulkan for one GPU: render on it, not on the one Plume's Vulkan rule takes.
    options.preferred_adapter = g_auto_adapter;
  }
  options.debug_layer = larecomp::settings::mcla_gpu_debug_layer.Get();
  options.backend = forced ? *forced : g_requested_backend;
  return options;
}

std::vector<std::filesystem::path> ShaderPackSearchFolders() {
  return ShaderStore::SearchFolders(g_shader_pack_folder);
}

// MF: use the live device's identity and caps without opening a second device.
const Device* MachineFactsDevice() {
  const Renderer* renderer = g_active.load(std::memory_order_acquire);
  return renderer ? &renderer->device() : nullptr;
}

namespace {

#if defined(_WIN32)
constexpr bool kFallbackWindows = true;
#else
constexpr bool kFallbackWindows = false;
#endif

// Whether the player took the failure box's offer (StartupChoseCompatibilityRenderer). Set once, on the UI thread, before Startup returns.
std::atomic<bool> g_compatibility_chosen{false};

// The box for a native renderer that could not start: what each graphics API said, why the game needs what it needs, and one plain way on
// (renderer_fallback.h): a button that starts the game with the compatibility renderer, or, where a button cannot do it (a launch option
// sets gpu.driver and beats larecomp.toml; the box with buttons cannot be shown), the line to use instead. True when the player pressed
// the button.
bool ShowRefusalBox(rex::ui::IUISystem& ui, const std::string& refusals) {
  namespace fb = fallback;
  const larecomp::cvar::Source source = larecomp::cvar::GetFlagSource("gpu.driver");
  const bool launch_sets_driver =
      source == larecomp::cvar::Source::kCommandLine || source == larecomp::cvar::Source::kEnvironment;
  const fb::Way way = fb::WayFor(launch_sets_driver, true);
  if (way == fb::Way::kButton) {
    // The first button is the default (Enter), the last the escape (Esc, the X). Windows: LARecomp's own box (choice_box.h), which
    // sizes each button from its label and puts the action first; the SDK's box cut the long label and put it last.
    fb::TwoButtonBox own;
    own.icon = fb::BoxIcon::kError;
    own.title = "LARecomp";
    own.body = fb::RefusalText(refusals, way, kFallbackWindows);
    own.action = std::string(fb::kButtonText);
    own.way_out = "Close";
    REXLOG_ERROR("{}: {}", own.title, own.body);
    if (const std::optional<int> picked = fb::ShowTwoButtonBox(own)) {
      REXLOG_INFO("[gpu] the box with the compatibility renderer button: {}", *picked == 0 ? own.action : own.way_out);
      return *picked == 0;
    }
    rex::ui::ChoiceConfig box;  // no box of ours here (not Windows, or it could not be built): the SDK's
    box.type = rex::MessageType::kError;
    box.title = own.title;
    box.body = own.body;
    box.buttons = {own.action, own.way_out};  // the first is the default, the last the escape
    const auto answer = ui.ShowChoice(box);
    if (answer) {
      return *answer == 0;
    }
    REXLOG_WARN("[gpu] the box with the compatibility renderer button could not be shown ({})", answer.error().message);
  }
  // No button: the same box, with the line to use (or the option to give) in its text.
  const fb::Way plain = way == fb::Way::kButton ? fb::Way::kLine : way;
  rex::ShowMessageBox(rex::MessageType::kError, fb::RefusalText(refusals, plain, kFallbackWindows));
  return false;
}

}  // namespace

bool StartupChoseCompatibilityRenderer() { return g_compatibility_chosen.load(std::memory_order_acquire); }

bool Startup(rex::ui::IUISystem& ui, const rex::NativeWindow& window) {
  if (!Enabled()) {
    return false;
  }
  if (g_renderer) {
    return g_active.load(std::memory_order_acquire) != nullptr;
  }
  // The renderer asks the UI system for an overlay frame (overlay.h), so the first-launch dialogs (profile name,
  // ISO wizard, shader build) are drawn by it before the guest exists.
  g_overlay_channel = std::make_shared<OverlayChannel>();
  g_overlay_host = std::make_shared<OverlayHost>(ui, g_overlay_channel);

  Renderer::Options options;
  options.device = PlannedDeviceOptions(nullptr);  // shared with the pipeline prebuild
  options.device.force_refusal = larecomp::settings::mcla_gpu_force_refusal.Get();  // a test of the box below (not the prebuild's)
  options.swap_chain.vsync = larecomp::settings::mcla_gpu_vsync.Get();
  options.swap_chain.present_wait = larecomp::settings::mcla_gpu_present_wait.Get();
  g_renderer = new Renderer();
  if (!g_renderer->Initialize(window, options, g_overlay_channel, g_overlay_host)) {
    REXLOG_ERROR("[gpu] renderer startup failed; nothing will be presented");
    // The app closes next, with the log as the only word of why: say it here, in plain terms, with
    // what each backend said when it was refused (Device::refusals).
    const std::string& refusals = g_renderer->device().refusals();
    g_compatibility_chosen.store(ShowRefusalBox(ui, refusals), std::memory_order_release);
    g_overlay_host->Disconnect();
    return false;
  }
  if (g_renderer->device().fell_back() && !g_auto_adapter.empty()) {
    // "auto" picked Vulkan, not the player, and D3D12 is what "auto" ran before: no box.
    REXLOG_WARN("[gpu] Vulkan, which graphics_backend auto picked for '{}', could not be started "
                "(the reason is above); D3D12 runs",
                g_auto_adapter);
  } else if (g_renderer->device().fell_back()) {
    // The log has the reason; the player chose the API in the menu and would
    // otherwise never learn that another one runs.
    rex::ShowMessageBox(
        rex::MessageType::kWarning,
        std::string(BackendName(g_renderer->device().requested_backend())) +
            " could not be started on this machine, so the game runs on " + BackendName() +
            ". The reason is in the log. The API is chosen in the launcher (Graphics API) or with "
            "graphics_backend in larecomp.toml.");
  }
  g_active.store(g_renderer, std::memory_order_release);

  // The fence hand-over, in this order: (a) take the retired
  // fence from the vblank thread, so nothing retires ahead of the render
  // thread; (b) turn the capture on, after which every fence the guest inserts
  // becomes a record; (c) one record that covers the fences inserted before
  // (b). In the normal order the guest has no device yet and (c) is a no-op.
  mcla::native_gfx::nocp::AcquireFenceRetirement();
  g_holds_retired_fence.store(true, std::memory_order_release);
  g_capture = new GuestCapture(g_renderer->stream(), larecomp::settings::mcla_gpu_verify_constants.Get());
  g_capture_active.store(g_capture, std::memory_order_release);
  REXLOG_INFO("[gpu] guest capture on (constant verification {})",
              larecomp::settings::mcla_gpu_verify_constants.Get() ? "on" : "off");
  if (const uint32_t device = mcla::native_gfx::GraphicsDeviceVa()) {
    auto* runtime = REX_RUNTIME();
    const uint8_t* base = runtime ? runtime->GetMembase().GetBase() : nullptr;
    if (base && g_capture->FenceCatchUp(base, device)) {
      REXLOG_WARN("[gpu] the guest's device {:#010x} existed before the capture; one fence "
                  "record covers the fences it inserted until now",
                  device);
    }
  }

  return true;
}

GuestCallToken BeforeGuestDraw(const uint8_t* base, uint32_t device) {
  if (GuestCapture* c = g_capture_active.load(std::memory_order_acquire)) {
    return c->BeforeDraw(base, device);
  }
  return GuestCallToken{};
}

void AfterGuestDrawIndexed(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                           uint32_t primitive, int32_t base_vertex, uint32_t start_index,
                           uint32_t count) {
  if (!token.active) {
    return;
  }
  if (GuestCapture* c = g_capture_active.load(std::memory_order_acquire)) {
    c->AfterDraw(token, base, device, DrawKind::kIndexed, primitive, count, start_index,
                 base_vertex);
  }
}

void AfterGuestDrawVertices(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                            uint32_t primitive, uint32_t start_vertex, uint32_t count) {
  if (!token.active) {
    return;
  }
  if (GuestCapture* c = g_capture_active.load(std::memory_order_acquire)) {
    c->AfterDraw(token, base, device, DrawKind::kAuto, primitive, count, start_vertex, 0);
  }
}

void AfterGuestBeginVertices(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                             uint32_t primitive, uint32_t count, uint32_t stride,
                             uint32_t buffer) {
  if (!token.active) {
    return;
  }
  if (GuestCapture* c = g_capture_active.load(std::memory_order_acquire)) {
    c->AfterBeginVertices(token, base, device, primitive, count, stride, buffer);
  }
}

void AfterGuestEndVertices(const uint8_t* base, uint32_t device) {
  if (GuestCapture* c = g_capture_active.load(std::memory_order_acquire)) {
    c->AfterEndVertices(base, device);
  }
}

GuestCallToken BeforeGuestEvent(const uint8_t* base, uint32_t device, GuestEvent event,
                                uint32_t stack_pointer) {
  if (GuestCapture* c = g_capture_active.load(std::memory_order_acquire)) {
    return c->BeforeEvent(base, device, event, stack_pointer);
  }
  return GuestCallToken{};
}

// The After* of an event reaches the capture whenever its token is active, even
// once Shutdown began: it pops the pre-state Before* pushed on this thread, and
// a disabled capture writes nothing.
void AfterGuestClear(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                     uint32_t caller, uint32_t flags, uint32_t rect, uint32_t color, float z,
                     uint32_t stencil) {
  if (token.active && g_capture) {
    g_capture->AfterClear(token, base, device, caller, flags, rect, color, z, stencil);
  }
}

void AfterGuestResolve(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                       uint32_t caller, uint32_t flags, uint32_t source_rect,
                       uint32_t dest_texture, uint32_t dest_point, uint32_t dest_level,
                       uint32_t dest_slice, uint32_t clear_color, float clear_z) {
  if (token.active && g_capture) {
    g_capture->AfterResolve(token, base, device, caller, flags, source_rect, dest_texture,
                            dest_point, dest_level, dest_slice, clear_color, clear_z);
  }
}

void AfterGuestBeginTiling(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                           uint32_t caller, uint32_t flags, uint32_t tile_count, uint32_t rects,
                           uint32_t clear_color, float clear_z, uint32_t clear_stencil) {
  if (token.active && g_capture) {
    g_capture->AfterBeginTiling(token, base, device, caller, flags, tile_count, rects,
                                clear_color, clear_z, clear_stencil);
  }
}

void AfterGuestEndTiling(const GuestCallToken& token, const uint8_t* base, uint32_t device,
                         uint32_t caller, uint32_t flags, uint32_t rects, uint32_t dest_texture,
                         uint32_t clear_color, float clear_z, uint32_t clear_stencil,
                         uint32_t params) {
  if (token.active && g_capture) {
    g_capture->AfterEndTiling(token, base, device, caller, flags, rects, dest_texture,
                              clear_color, clear_z, clear_stencil, params);
  }
}

void OnGuestFrameEnd(uint8_t* base) {
  if (!Enabled() || base == nullptr) {
    return;
  }
  // The game's own no-cache path: its command-buffer replay writes
  // PM4 that no hook sees. Held every frame because the guest turns the cache
  // back on. (The legacy occlusion.h comment predates the correction: it is a
  // command-buffer cache, not an occlusion query.)
  mcla::native_gfx::DisableGuestOcclusionQueries(base);
}

void OnGuestSwap(const uint8_t* base, uint32_t device, uint32_t front_buffer) {
  fallback::Watch().NoteFrame();  // one load while nothing is armed; a start that presented its frames did not crash (renderer_fallback.h)
  const int64_t target =g_next_present_target.exchange(0, std::memory_order_relaxed);
  const uint64_t tag = g_next_present_tag.exchange(0, std::memory_order_relaxed);  // capture mode
  if (GuestCapture* c = g_capture_active.load(std::memory_order_acquire)) {
    c->Present(base, device, front_buffer, target, tag);
  }
}

void SetPresentObserver(PresentObserver observer) {
  g_present_observer.store(observer, std::memory_order_release);
}

void SetNextPresentTarget(int64_t qpc) {
  g_next_present_target.store(qpc, std::memory_order_relaxed);
}

void SetNextPresentTag(uint64_t tag) {
  g_next_present_tag.store(tag, std::memory_order_relaxed);
}

bool SchedulesPresents() {
#if defined(_WIN32)
  // Frame generation (FSR's or DLSS's) presents through a pacer of its own, which the game's
  // latency reducer paces: no frame is held to a target then.
  return g_capture_active.load(std::memory_order_acquire) != nullptr &&
         !larecomp::settings::mcla_gpu_vsync.Get() && !FrameGenerationRunning() &&
         !DlssFrameGenerationRunning();
#else
  return false;
#endif
}

int32_t LimitFrameRateForComposedPresents(int32_t limit) {
#if defined(_WIN32)
  const Renderer* renderer = g_active.load(std::memory_order_acquire);
  const uint32_t ceiling = renderer ? renderer->composed_present_ceiling() : 0;
  if (ceiling != 0) {
    // The generation pacers issue extra presents outside PresentNow. Budget them in the
    // rendered-frame limiter, using the active multiplier (FSR always generates one frame).
    const uint32_t multiplier = DlssFrameGenerationRunning() ? DlssFrameGenerationMultiplier()
                                : FrameGenerationRunning() ? 2u : 1u;
    return render_wait::LimitRenderRateForPresents(limit, ceiling, multiplier);
  }
#endif
  return limit;
}

void OnGuestFenceInserted(uint32_t device, uint32_t value) {
  if (GuestCapture* c = g_capture_active.load(std::memory_order_acquire)) {
    c->Fence(device, value);
  }
}

void OnGuestLock(uint32_t resource, uint32_t level, uint32_t address, uint32_t size) {
  if (GuestCapture* c = g_capture_active.load(std::memory_order_acquire)) {
    g_guest_locks.fetch_add(1, std::memory_order_relaxed);
    if (c->Lock(resource, level, address, size)) {
      g_guest_locks_on_destinations.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void OnGuestSceneComposited(uint32_t bucket, uint32_t tag) {
  if (GuestCapture* c = g_capture_active.load(std::memory_order_acquire)) {
    c->Composite(bucket, tag);
  }
}

void OnGuestOcclusionEvent(const uint8_t* base, uint32_t query, bool is_begin) {
  if (GuestCapture* c = g_capture_active.load(std::memory_order_acquire)) {
    c->OcclusionEvent(base, query, is_begin);
  }
}

void OnGuestQueryGetData(const uint8_t* base, uint32_t query) {
  if (g_capture_active.load(std::memory_order_acquire) != nullptr) {
    occlusion::OnGuestGetData(const_cast<uint8_t*>(base), query);
  }
}

bool RetiresGuestFences() {
  return g_holds_retired_fence.load(std::memory_order_acquire) &&
         g_capture_active.load(std::memory_order_acquire) != nullptr;
}

void OnGuestGpuHangCheck(const uint8_t* base, uint32_t device) {
  // The guest calls this from its fence spin after ~5000 ticks without the
  // retired value moving -- in this mode, a render thread busy for that long
  // (creating many pipelines the first time it sees a scene) or one that
  // stopped. Reported, never fatal: the first call of a stall, then every 10 s.
  using Clock = std::chrono::steady_clock;
  static std::atomic<int64_t> last_report_ms{0};
  static std::atomic<uint64_t> calls{0};
  const uint64_t call = calls.fetch_add(1, std::memory_order_relaxed) + 1;
  const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             Clock::now().time_since_epoch())
                             .count();
  int64_t last = last_report_ms.load(std::memory_order_relaxed);
  if (last != 0 && now_ms - last < 10000) {
    return;
  }
  if (!last_report_ms.compare_exchange_strong(last, now_ms, std::memory_order_relaxed)) {
    return;
  }
  const mcla::native_gfx::GuestFenceState fence = mcla::native_gfx::ReadFenceState(base, device);
  Renderer::FenceProgress progress;
  if (Renderer* r = g_active.load(std::memory_order_acquire)) {
    progress = r->fence_progress();
  }
  REXLOG_WARN("[gpu] the guest reports its GPU as hung (call {}): device {:#010x} issued {} "
              "retired {} | render thread: {} records executed, {} fences published, last {} | "
              "retired fence held by the renderer: {} | the guest keeps waiting",
              call, device, fence.issued, fence.retired, progress.records_executed,
              progress.fences_published, progress.last_fence,
              g_holds_retired_fence.load(std::memory_order_relaxed) ? "yes" : "no");
}

void NoteGuestFenceWait(uint64_t wait_ns) {
  g_fence_waits.fetch_add(1, std::memory_order_relaxed);
  g_fence_wait_ns.fetch_add(wait_ns, std::memory_order_relaxed);
}

void RequestResize(ResizeSource source) {
  if (Renderer* r = g_active.load(std::memory_order_acquire)) {
    r->RequestResize(source);
  }
}

// The hotkeys (LarecompApp::StartOverlays binds F11 / F10 / F2 as ui.bind.gpu_renderdoc_capture, gpu_trace_next and
// gpu_texture_dump). The SDK has no bind of its own on these keys any more, so nothing is unregistered first.
void RequestRenderDocCapture() {
  if (Renderer* r = g_active.load(std::memory_order_acquire)) {
    r->RequestCapture();
  }
}

// F10: raw trace of the next guest frame (frame_trace.h), for any scene the frame counter cannot be aimed at (menus,
// pause, photo mode).
void RequestFrameTrace() {
  if (Renderer* r = g_active.load(std::memory_order_acquire)) {
    r->RequestTrace();
    REXLOG_INFO("[gpu] trace: the frame after the next present will be written");
  }
}

// F2: every texture of the next frame to the dump folder, named after the game's texture and its dictionary, for
// texture mods (textures.h).
void RequestTextureDump() {
  if (Renderer* r = g_active.load(std::memory_order_acquire)) {
    r->RequestTextureDump();
  }
}

void RequestTextureDumpAll() {
  if (Renderer* r = g_active.load(std::memory_order_acquire)) {
    r->RequestTextureDumpAll();
  }
}

void RequestTextureReload() {
  if (Renderer* r = g_active.load(std::memory_order_acquire)) {
    r->RequestTextureReload();
  }
}

// The size the render scale is measured against: the picture's frame in the swap chain's size, for the
// ASPECT RATIO as it is now (so a change of the setting moves the scale at the next frame boundary, as a
// change of the window does). The window itself while the setting follows it; 0 before a size is published.
static mc::frame::Rect FrameOfPublishedWindow() {
  const uint64_t size = g_window_size.load(std::memory_order_relaxed);
  return mc::ultrawide::FrameFor(uint32_t(size >> 32), uint32_t(size));
}

RenderScaleChoice PreviewRenderScale(double asked, bool fsr, FsrPreset preset) {
  const mc::frame::Rect frame = FrameOfPublishedWindow();
  return ChooseRenderScale(asked, fsr, preset, frame.w, frame.h);
}

RenderScaleChoice WantedRenderScale() {
  const mc::frame::Rect frame = FrameOfPublishedWindow();
  return ChooseWantedRenderScale(frame.w, frame.h);
}

void SetShaderPackFolder(const std::filesystem::path& folder) {
  g_shader_pack_folder = folder;
}

void SetShaderPackGate(std::function<bool()> gate) {
  g_shader_pack_gate = std::move(gate);
}

std::filesystem::path ShaderPackFolder() {
  return ShaderStore::BuildFolder(g_shader_pack_folder);
}

void SetCityPackHooks(CityPackHooks hooks) {
  g_city_pack_hooks = std::move(hooks);
}

std::filesystem::path CityPackFolder() {
  return ShaderPackFolder() / "city";
}

bool ShaderPacksAttached() {
  Renderer* r = g_active.load(std::memory_order_acquire);
  return r && r->shader_packs_attached();
}

bool AttachShaderPacks(const std::filesystem::path& folder) {
  Renderer* r = g_active.load(std::memory_order_acquire);
  if (!r) {
    REXLOG_ERROR("[gpu] shader pack: cannot attach, the Plume renderer is not running");
    return false;
  }
  return r->AttachShaderPacks(folder.empty() ? g_shader_pack_folder : folder);
}

ShutdownProgress ReadShutdownProgress() {
  ShutdownProgress progress;
  const int state = g_shutdown_state.load(std::memory_order_acquire);
  if (state == kShutdownNotStarted) {
    return progress;
  }
  progress.started = true;
  progress.finished = state == kShutdownFinished;
  progress.stage = g_shutdown_stage.load(std::memory_order_relaxed);
  progress.driver_call = g_shutdown_driver_call.load(std::memory_order_relaxed);
  const int64_t moved = g_shutdown_moved.load(std::memory_order_relaxed);
  const int64_t ticks = SteadyTicks() - moved;
  using Tick = std::chrono::steady_clock::duration;
  progress.quiet_ms = ticks <= 0 ? 0 : std::chrono::duration_cast<std::chrono::milliseconds>(Tick(ticks)).count();
  return progress;
}

void Shutdown(bool fast_exit) {
  // No new records; a guest thread inside a capture finishes against the
  // stream, which the render thread's Stop releases.
  if (GuestCapture* c = g_capture_active.exchange(nullptr, std::memory_order_acq_rel)) {
    c->Disable();
  }
  // UI thread: no UI pass touches the SDK's ImGui objects after this, even one
  // already posted (the SDK destroys them after OnShutdown).
  if (g_overlay_host) {
    g_overlay_host->Disconnect();
  }
  Renderer* r = g_active.exchange(nullptr, std::memory_order_acq_rel);
  if (r) {
    r->Shutdown(fast_exit);
  }
  // After the render thread stopped (or was let go): the fence records still
  // queued were dropped with it, and a guest thread waiting on one of them is
  // released by the vblank thread's next tick.
  if (g_holds_retired_fence.exchange(false, std::memory_order_acq_rel)) {
    mcla::native_gfx::nocp::ReleaseFenceRetirement();
  }
}

}  // namespace mcla::gpu
