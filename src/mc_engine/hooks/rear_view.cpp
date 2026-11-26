// The rear view mirror MCLA shipped switched off.
//
// Everything is still in the retail binary: mcRearView (a child of mcHUDForm at
// +4048), the 0x1000000 render phase, the mirror camera and the four
// rear_view*.xdr frame meshes. What was stripped is the `rearview` dev option
// (node+4 at 0x8287E0AC): all five of its readers -- the mcRearView ctor,
// dtor, update and draw, and the renderer ctor's phase mask -- fall through
// when it is zero.
//
// The option is therefore forced on once at InitHooks (see
// ApplyPerfDebugOptions) and never touched again: the ctor only loads the four
// meshes when it is non-zero, and the dtor frees them on the same test, so it
// must read the same value for the whole session. Showing and hiding is done
// per frame through the widget's own visible bit instead.
//
//   mcHUDForm::update (sub_8264F100) calls the child's update unconditionally.
//   mcRearView::update (sub_82233C18) reads vtbl+60 (flags+16 & 0x20). Visible:
//   alpha (+1216) ramps up by 4 a second and the 0x1000000 phase is requested
//   (sub_822E5B00). Hidden: alpha ramps down, the phase is dropped (sub_822E5B10)
//   and the consumer's active byte is cleared, so a hidden mirror costs nothing
//   and leaves no black strip behind. The frame is drawn by sub_82227428 through
//   sub_82234128, which tests only the option and alpha > 0, never the visible bit.
//
// The visible bit is not ours: sub_82652860 (mcHUDForm vtbl+176, the form's refresh,
// reached from the HUD's invalidate calls whenever something in it changes) copies the
// form's own visibility into the child (vtbl+64). That can happen at any point of a
// frame, so clearing the bit once per frame from TickRearView (which runs at the top of
// the frame, after the clock update) is not enough: a refresh between the clear and the
// mirror's update shows the mirror for that update -- its scene phase for a frame, its
// white frame at a growing alpha for as long as refreshes keep coming and 0.25 s after.
//
// So a switched-off mirror is made hidden where the game decides, not by writing the bit
// back: Patch_RearViewUpdateVisible / Patch_RearViewUpdateActive answer "not visible" at
// both of mcRearView::update's visibility tests, and Patch_RearViewDrawAlpha reads the
// frame's alpha as 0 in sub_82234128. The bit is left alone while off. On mirrors the
// form's bit every frame (below) and the three hooks pass everything through.
//
// Placement is live as well. The viewport the scene is drawn into is four ints
// at [dword_8288D004]+0..12 (ctor default 960,57,256,51); sub_82307F28 and
// sub_82308178 read them every frame, and sub_82307F28 rebuilds the camera
// aspect from them. The frame meshes are placed by two floats at widget+160 /
// +164 (ctor default 209, 85): x = screen_width - [+160], y = [+164], read by
// sub_82233E18 each frame.
//
// Measured on a race capture at 1280x720: the frame ring (rear_view_1bg) spans
// x 882..1235, y 55..112, with its window at x 894..1168, y 60..108. The window's
// left and right edges lean, so only x 916..1145, y 62..106 is inside it all the
// way: the viewport defaults to that rectangle, 229x44, close to the 256x51 the
// game used. rearview_position moves the frame and the viewport together.
//
// The frame is rear_view_1bg.xdr, material UnlitRuntimeAlpha with the texture
// black_small.dds, which is not in any archive: it falls back to the runtime's white
// 32x32 and the frame comes out solid white. rearview_frame skips its draw (see
// Patch_RearViewFrameSkip).
//
// THE CHEAP OPTIONS (perf/MIRRORS-SURVEY.md section 4, B to E; the rules are in rear_view_cheap.h and
// have a test). The mirror costs DRAWS: it is one more run of the renderer's phase loop (sub_822E6408)
// for the phase 0x1000000, with the game's own culling by the mirror's camera. Each option is a debug
// setting that is off by default and changes nothing then; the cost chain
// (perf/scripts/chain-mirror-cost.ps1, tier "cheap") has one arm for each and one with all on.
//   rearview_fov_scale, rearview_far   Patch_RearViewCamera: sub_82307F28 (the colour pass's begin
//                                      step for the mirror, called from sub_82306500 for phase 0x10000)
//                                      makes the mirror's camera with sub_8217D460 (camera, field of
//                                      view = [pass+20], aspect = w/h, near, far, the last two the main
//                                      camera's); the hook right before that call narrows f1 and pulls
//                                      in f4. The camera's frustum is built from them (sub_8217CB68).
//   rearview_lod_distance              Patch_RearViewCityLod: the city's per-sector walk (sub_822D5618)
//                                      passes the hi detail radius (mcCity+36 x mcCity+24) in f1 to
//                                      sub_822D7120 for every sector; while the mirror's phase runs
//                                      (Patch_RearViewDiag at its begin sets it, Patch_RearViewPhaseEnd
//                                      clears it) the hook cuts that radius. CHECK: that the walk runs
//                                      inside the mirror's phase; if the city's draw list of the phase
//                                      is made by the main view's walk the hook finds nothing to cut
//                                      (the mirror draw count in the log says).
//   rearview_cars_max, rearview_cars_lod  Patch_RearViewCarPhase, below.
//   rearview_every                     Patch_RearViewCadence, at the phase loop's start (0x822E6634, after
//                                      `and r21,r17,r11`): on a held frame the mirror's bit leaves r17,
//                                      r21 and renderer+364, which is everything the begin steps, the
//                                      clients and the phase's end test, so no part of the phase runs;
//                                      Patch_RearViewPhaseEnd (0x822E6908, every phase, r30 = the phase)
//                                      puts renderer+364 back. A camera cut draws at once. The picture is
//                                      NOT held (the host does not keep the stamp's pixels yet): a held
//                                      frame shows the main view in the rectangle.
// The host side (gpu/mirror_draws.h): TickRearView publishes the rectangle every frame the mirror is up;
// the Scene counts the draws whose viewport is that rectangle ("mirror: N draws/frame" in the log) and,
// with mcla_gpu_mirror_class, keeps them out of the main view's per-draw effects.
//
// THE SHARED VIEW, the guest half (perf/MIRROR-VIEW.md, package MV2, joined with the host half in section 12; rules and tests in rear_view_shared.h / rear_view_shared_test.cpp, the contract with the
// host in gpu/mirror_view_link.h, the one link header). The host's mode selects the game's visible HUD mirror at 0 and the shared view at 1 to 4:
//   markers          Patch_RearViewDiag (the entry of the phase's begin step, sub_82308178) sends a BEGIN and Patch_RearViewPhaseEnd (0x822E6908, r30 = 0x1000000) an END through
//                    the composite record's side channel, including for a buffered hidden phase while the view is off. The host routes the draws between them.
//                    A lost END is closed at the next frame's start (Patch_RearViewCadence). A held frame (the cadence) sends the two back to back, an empty phase:
//                    the host stamps the picture it kept (MV1's rule).
//   camera           Patch_RearViewCamera sets the mirror camera's field and aspect (f1 = the vertical field in degrees, f2 = w/h) to what the host asks for, and tells the host the
//                    game's own field (the retail HUD mirror's), so the stamp can keep the retail look.
//   three states     the HUD mirror on (as before), the phase run for the car's side mirrors with the HUD form hidden (the host's side_need: no frame, no stamp), off. The two visibility
//                    hooks answer for the phase request, Patch_RearViewDrawAlpha keeps the frame hidden, TickRearView keeps the rectangle and the racers for the side-only case.
//   the need rule's  TickRearView publishes the game's camera rig (camera_look.h CurrentCameraRig) with the other guest facts; the host decides the need (glass pair found, its size
//   input            on the picture) and publishes it back.
//   cadence          the shared view holds the last picture between timed refreshes.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include "settings/larecomp_settings.h"
#include <rex/ppc/call.h>
#include <rex/ppc/recompiled_code.h>
#include <rex/ppc/context.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/function_dispatcher.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>

#include "hooks.h"
#include "hooks_internal.h"
#include "larecomp_log.h"
#include "larecomp_guest.h"
#include "gpu/cabin_mirror_rules.h"
#include "gpu/car_model_walk_tap.h"
#include "gpu/mirror_draws.h"
#include "gpu/mirror_view_link.h"
#include "mc_engine/camera/camera_look.h"
#include "mc_engine/input/adaptive_triggers.h"
#include "rear_view_cheap.h"
#include "rear_view_shared.h"
#if defined(LARECOMP_HAS_GPU)
#include "gpu/gpu.h"
#endif

// Guest layout (see the file comment for where each was read).
static constexpr uint32_t kHudManagerPtr    = 0x8286D8D4u;  // dword_8286D8D4
static constexpr uint32_t kHudListOffset    = 81792u;       // sub_82641CB0(manager + 81792, 109)
static constexpr uint32_t kHudListArray     = 68u;          //   [list+68] = array of widgets
static constexpr uint32_t kHudListCount     = 72u;          //   u16 at [list+72]
static constexpr uint32_t kHudFormIndex     = 109u;
static constexpr uint32_t kRearViewInForm   = 4048u;        // mcRearView inside mcHUDForm
static constexpr uint32_t kHudFormVtable    = 0x8209D30Cu;  // mcHUDForm::`vftable'
static constexpr uint32_t kRearViewVtable   = 0x82027A54u;  // mcRearView::`vftable'
static constexpr uint32_t kWidgetFlags      = 16u;          // vtbl+60 tests (flags & 0x20)
static constexpr uint32_t kWidgetVisible    = 0x20u;
static constexpr uint32_t kFrameOffsetX     = 160u;         // floats, ctor defaults 209 / 85
static constexpr uint32_t kFrameOffsetY     = 164u;
static constexpr uint32_t kMirrorPassPtr    = 0x8288D004u;  // dword_8288D004: viewport ints at +0..12

static constexpr float kFrameDefaultX = 209.0f;
static constexpr float kFrameDefaultY = 85.0f;

// The window's top-left in the unshifted layout, measured from rear_view_1bg.
static constexpr int kWindowX = 894;
static constexpr int kWindowY = 60;

// Where each position puts the frame, as a shift from its unshifted layout (centre
// at 1058.5, 83.5 in 1280x720). "stock" is 60 px under the race position digits,
// which sit where the frame used to; the others centre the frame on the boxes drawn
// over a 1920x1080 screenshot (every number scaled by 2/3).
struct RearViewPreset {
    const char* name;
    int x;
    int y;
};
static constexpr RearViewPreset kPresets[] = {
    {"stock",         0,    60},
    {"left_top",      -852, 78},
    {"middle_top",    -436, 2},
    {"left_down",     -857, 298},
    {"middle_footer", -425, 565},
    {"right_down",    24,   349},
};

static RearViewPreset ResolvePreset() {
    const std::string name = larecomp::settings::rearview_position.Get();
    for (const auto& p : kPresets)
        if (name == p.name) return p;
    return kPresets[0];
}

static bool PlausibleGuestPtr(uint32_t p) { return p >= 0x10000u && p < 0xE0000000u; }

// Resolves the mcHUDForm and its mcRearView, or false while the HUD is not built.
// Both vtables are checked: the form is destroyed and rebuilt with the HUD, so
// nothing here is cached.
static bool FindRearView(const uint8_t* base, uint32_t& form, uint32_t& widget) {
    const uint32_t manager = ReadGuestU32(base, kHudManagerPtr);
    if (!PlausibleGuestPtr(manager)) return false;
    const uint32_t list = manager + kHudListOffset;
    const uint32_t array = ReadGuestU32(base, list + kHudListArray);
    if (!PlausibleGuestPtr(array)) return false;
    const uint32_t count = ReadGuestU32(base, list + kHudListCount) >> 16;
    if (kHudFormIndex >= count) return false;
    form = ReadGuestU32(base, array + 4u * kHudFormIndex);
    if (!PlausibleGuestPtr(form) || ReadGuestU32(base, form) != kHudFormVtable) return false;
    widget = form + kRearViewInForm;
    return ReadGuestU32(base, widget) == kRearViewVtable;
}

// What the mirror draws. The renderer keeps 13 render-list clients at renderer+128
// (sub_822E5138 registers one per slot); each is an mcRenderClass<Class, slot, mask>
// whose [client+4] is the mask of phases it takes part in, and the phase loop
// (sub_822E6408) only calls a client for a phase whose bit is in that mask. The RTTI
// names carry the template arguments:
//   slot 0 mcCityModelClass          0x010E7FFE   has 0x1000000 -- the only thing in retail's mirror
//   slot 1 mcPropClass               0x00189FF2   (draws nothing in the mirror phase, tried)
//   slot 2 mcMultiPropManager        0x000809F0
//   slot 7 mcCarModelClass           0x12E183F3   (the player's and the racers' cars)
//   slot 9 mcAmbientCarRenderClass   0x01118002   has 0x1000000 -- traffic already is in
//   slot 10 mcAmbientPedRenderClass  0x000105F0   (draws nothing in the mirror phase, tried)
// The groups below add or remove the mirror's phase in those masks on demand. Only a client
// whose mask is exactly what the RTTI says (with or without the bit) is touched.
static constexpr uint32_t kRendererPtr      = 0x8287E064u;
static constexpr uint32_t kRendererClients  = 128u;
static constexpr uint32_t kClientMask       = 4u;
static constexpr uint32_t kMirrorPhase      = 0x1000000u;

struct MirrorClient {
    int slot;
    uint32_t stock_mask;  // without kMirrorPhase
    const char* name;
};

static void SetClientMirrorPhase(uint8_t* base, uint32_t renderer, const MirrorClient& c, bool enable) {
    const uint32_t client = ReadGuestU32(base, renderer + kRendererClients + 4u * uint32_t(c.slot));
    if (!PlausibleGuestPtr(client)) return;
    const uint32_t mask = ReadGuestU32(base, client + kClientMask);
    if ((mask & ~kMirrorPhase) != c.stock_mask) return;  // not the class the RTTI says
    const uint32_t want = enable ? (c.stock_mask | kMirrorPhase) : c.stock_mask;
    if (mask == want) return;
    WriteGuestU32(base, client + kClientMask, want);
    LARECOMP_APP_INFO("[RearView] {} {} the mirror phase (mask 0x{:08X} -> 0x{:08X})", c.name,
                      enable ? "joins" : "leaves", mask, want);
}

// --- the cheap options: guest layout and state (the file comment says what each hook is) ---------------------------------------------------------------
static constexpr uint32_t kRendererPhaseMask    = 364u;         // renderer + 364: the phases the loop may run (the begin steps test it too)
static constexpr uint32_t kRendererMainViewports = 180u;        // renderer + 180 + 4 * buffer: the main viewport of the render buffer (traffic_shadow.cpp)
static constexpr uint32_t kRenderBufferGlobal   = 0x827E5640u;  // dword_827E5640: the render buffer, 0..3
static constexpr uint32_t kViewportEye          = 112u;         // a viewport's eye: three floats (traffic_shadow.cpp)
static constexpr uint32_t kPassActiveByte       = 32u;          // [pass + buffer + 32]: the mirror's consumer is active (sub_82308178 tests it)
static constexpr uint32_t kCarLodWord           = 6004u;        // the vehicle's chosen model level 0..2 (RT-MODEL-SURVEY.md 2.1)

namespace cheap = larecomp::rearview_cheap;

// All of these are touched by the thread that runs the renderer's phase loop, except the one atomic (the city's per-sector walk may also run on the game's update thread: that walk is the
// main view's and must never see the mirror's radius, so the "inside the mirror's phase" flag is per thread).
static thread_local bool g_in_mirror_phase = false;   // between the begin of the mirror's phase and its end, on the phase loop's thread
static std::atomic<uint64_t> g_city_radius_cuts{0};   // sectors whose hi detail radius the hook cut
static cheap::Cadence g_cadence;
static cheap::RefreshClock g_refresh_clock;
static uint32_t g_masked_renderer = 0;                // the renderer whose +364 lost the mirror's bit on a held frame (put back at the phase's end)
static float g_last_eye[3] = {0.0f, 0.0f, 0.0f};
static bool g_have_last_eye = false;

struct CheapStats {
    uint64_t frames = 0;           // frames the mirror was up and the cadence was asked
    uint64_t held = 0;
    uint64_t cut_draws = 0;
    uint64_t cars_seen = 0;        // racers the mirror's camera saw
    uint64_t cars_drawn = 0;
    uint64_t cars_turned_away = 0;
    uint64_t cars_coarser = 0;
    uint64_t camera_builds = 0;
    uint64_t city_radius_cuts = 0;
};
static CheapStats g_stats;
static CheapStats g_stats_logged;

struct CheapSwitches {
    int every = 1;
    int hz = 0;
    double far_m = 0.0;
    double fov_scale = 1.0;
    double lod_m = 0.0;
    int cars_max = 0;
    int cars_lod = 0;
    bool operator==(const CheapSwitches&) const = default;
    bool AnyOn() const { return every > 1 || far_m > 0.0 || fov_scale < 1.0 || lod_m > 0.0 || cars_max > 0 || cars_lod > 0; }
};

static CheapSwitches ReadCheapSwitches() {
    CheapSwitches s;
    s.hz = larecomp::settings::rearview_refresh_hz.Get();
    s.every = larecomp::settings::rearview_every.Get();
    s.far_m = larecomp::settings::rearview_far.Get();
    s.fov_scale = larecomp::settings::rearview_fov_scale.Get();
    s.lod_m = larecomp::settings::rearview_lod_distance.Get();
    s.cars_max = larecomp::settings::rearview_cars_max.Get();
    s.cars_lod = larecomp::settings::rearview_cars_lod.Get();
    return s;
}

// One line every 600 frames the mirror was up (and one when the switches change), so a run's log says what each option did: the cost chain reads the
// host's "mirror: N draws/frame" line for the draws and this one for what the guest side decided. Cheap on the frames in between: a few compares.
static void LogCheapState() {
    static CheapSwitches last{-1, -1, -1.0, -1.0, -1.0, -1, -1};
    const CheapSwitches now = ReadCheapSwitches();
    if (!(now == last)) {
        last = now;
        LARECOMP_APP_INFO("[RearView] cheap options: every {} ({} Hz), far {:g} m, field of view x{:g}, city radius {:g} m, cars max {}, cars level {}{}", now.every, now.hz, now.far_m,
                          now.fov_scale, now.lod_m, now.cars_max, now.cars_lod, now.AnyOn() ? "" : " (all off)");
    }
    g_stats.city_radius_cuts = g_city_radius_cuts.load(std::memory_order_relaxed);
    const CheapStats& n = g_stats;
    const CheapStats& o = g_stats_logged;
    if (n.frames - o.frames < 600) return;
    const double frames = double(n.frames - o.frames);
    LARECOMP_APP_INFO("[RearView] cheap ({} mirror frames): drawn {}, held {}, cut draws {}; cars seen {:.2f}/frame, drawn {:.2f}/frame, turned away {}, made coarser {}; "
                      "camera builds {}; city sectors cut to the radius {}",
                      n.frames - o.frames, (n.frames - o.frames) - (n.held - o.held), n.held - o.held, n.cut_draws - o.cut_draws, double(n.cars_seen - o.cars_seen) / frames,
                      double(n.cars_drawn - o.cars_drawn) / frames, n.cars_turned_away - o.cars_turned_away, n.cars_coarser - o.cars_coarser,
                      n.camera_builds - o.camera_builds, n.city_radius_cuts - o.city_radius_cuts);
    g_stats_logged = n;
}

// Is the mirror's consumer active in this render buffer (what sub_82308178 tests before it does anything)?
static bool MirrorConsumerActive(const uint8_t* base) {
    const uint32_t pass = ReadGuestU32(base, kMirrorPassPtr);
    const uint32_t buffer = ReadGuestU32(base, kRenderBufferGlobal);
    if (!PlausibleGuestPtr(pass) || buffer > 3u) return false;
    return base[pass + buffer + kPassActiveByte] != 0;
}

// The eye of the main viewport of the render buffer: [renderer + 180 + 4 * buffer] + 112, three floats. False if a link is not plausible.
static bool MainEye(const uint8_t* base, uint32_t renderer, float eye[3]) {
    const uint32_t buffer = ReadGuestU32(base, kRenderBufferGlobal);
    if (!PlausibleGuestPtr(renderer) || buffer > 3u) return false;
    const uint32_t viewport = ReadGuestU32(base, renderer + kRendererMainViewports + 4u * buffer);
    if (!PlausibleGuestPtr(viewport)) return false;
    for (uint32_t i = 0; i < 3; ++i) eye[i] = ReadGuestF32(base, viewport + kViewportEye + 4u * i);
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(eye[i]) || std::fabs(eye[i]) > 1.0e6f) return false;
    return true;
}

// --- the shared view's guest half (the file comment says what it is; rules in rear_view_shared.h) -----------------------------------------------------------
namespace shared = larecomp::rearview_shared;
namespace cabin = mcla::gpu::cabin_mirror;
namespace vlink = mcla::gpu::mirror_view;  // the link header's namespace; not `link`: unistd.h declares a function of that name

// Sharing the view uses the Plume renderer and the host's live mode (gpu.h MirrorViewMode). The begin hook can also mark a buffered hidden phase so the host can discard it after a mode change.
static bool SharedViewOn() {
#if defined(LARECOMP_HAS_GPU)
    return mcla::gpu::MirrorViewMode() != 0 && mcla::gpu::Enabled();
#else
    return false;
#endif
}

// What decides the three states, read live where a hook runs. The host's request is read only while the guest half is on (off: a request nobody withdrew changes nothing). The side mirrors'
// need is the request's: the host copies the glass finder's flag (MV3) into it once a frame (MirrorView::BeginFrame), so this half has one source for it.
static shared::Mode CurrentMode() {
    shared::Mode m;
    m.shared = SharedViewOn();
    m.hud = larecomp::settings::rearview.Get();
    m.side_need = shared::EffectiveSideNeed(m.shared, vlink::CurrentRequest());
    return m;
}

// A marker to the host, in order with the draws. The tracker (rear_view_shared.h) decides when; 0 is nothing to send.
static void SendMirrorMarker(uint32_t marker, bool hud_stamp = false) {
    if (marker == 0) return;
#if defined(LARECOMP_HAS_GPU)
    mcla::gpu::NoteMirrorPhase(marker == vlink::kMarkerBegin, hud_stamp);
#endif
}

// The phase loop's thread only (Patch_RearViewDiag, Patch_RearViewPhaseEnd, Patch_RearViewCadence, Patch_RearViewCamera run on it).
static shared::MarkerTracker g_markers;
static bool g_frame_held = false;                     // this frame is a held one: the cadence took the mirror's phase out (the host keeps its picture; Patch_RearViewPhaseEnd sends the empty phase)
// The guest's frame tick writes, the HUD draw's hook reads: is the HUD form visible (the game's own bit, mirrored into the widget while the HUD mirror is on)?
static std::atomic<bool> g_form_visible{true};

static bool CabinRigActive() {
    return CurrentCameraRig() == cabin::kCockpitRig && !larecomp::triggers::IsGamePaused() &&
           larecomp::settings::rearview_interior.Get();
}

static uint32_t CabinEyeRequest() {
    const auto request = vlink::CurrentRequest();
    return request.redirect && (request.side_mask & cabin::kReader) && CabinRigActive()
               ? vlink::CabinOffset() : 0;
}

static bool HideHudWithCabin() {
    return larecomp::settings::rearview_hide_with_interior.Get() && vlink::CabinDrawn() &&
           SharedViewOn() && CabinEyeRequest() != 0;
}

static bool HudStampWanted() {
    return larecomp::settings::rearview.Get() && g_form_visible.load(std::memory_order_relaxed) && !HideHudWithCabin();
}

struct SharedStats {
    uint64_t frames = 0;           // frames the mirror was up with the guest half on (the cadence hook's count: drawn or held)
    uint64_t side_only = 0;        // ... for the side mirrors only
    uint64_t camera_builds = 0;
    uint64_t camera_applied = 0;   // builds where the host's field was used
};
static SharedStats g_shared_stats;
static SharedStats g_shared_logged;

// One line every 600 mirror frames with the guest half on, and one when it turns on or off.
static void LogSharedState() {
    static int last_on = 0;  // quiet while it has never been on: a player with the switch off sees nothing of this
    const bool on = SharedViewOn();
    if (int(on) != last_on) {
        last_on = int(on);
        LARECOMP_APP_INFO("[RearView] shared view, guest half: {}", on ? "on (markers, the host's field of view, the side mirrors' phase)" : "off");
    }
    if (!on) return;
    const SharedStats& n = g_shared_stats;
    const SharedStats& o = g_shared_logged;
    if (n.frames - o.frames < 600) return;
    LARECOMP_APP_INFO("[RearView] shared view ({} mirror frames, {} for the side mirrors only; camera builds {}, host field applied {}); markers so far: {} begin, {} end, "
                      "{} closed late, {} nested, {} empty phases sent for held frames, {} begin steps that did not run the phase (not asked for, or inactive)",
                      n.frames - o.frames, n.side_only - o.side_only, n.camera_builds - o.camera_builds, n.camera_applied - o.camera_applied, g_markers.begun(), g_markers.ended(),
                      g_markers.closed_late(), g_markers.nested(), g_markers.held_pairs(), g_markers.not_run());
    g_shared_logged = n;
}

static void ApplyMirrorMembership(uint8_t* base, bool mirror_on, const shared::Mode& mode) {
    const uint32_t renderer = ReadGuestU32(base, kRendererPtr);
    if (!PlausibleGuestPtr(renderer)) return;

    static constexpr MirrorClient kCars{7, 0x12E183F3u, "cars"};
    static constexpr MirrorClient kTraffic{9, 0x00118002u, "traffic"};

    SetClientMirrorPhase(base, renderer, kCars, mirror_on && (mode.shared || larecomp::settings::rearview_cars.Get()));
    // CHECK: the close-traffic blackout must be isolated before this debug arm is enabled by default.
    SetClientMirrorPhase(base, renderer, kTraffic, mirror_on && larecomp::settings::rearview_traffic.Get());
}

void TickRearView() {
    auto* base = larecomp::guest::Membase();
    if (!base) return;

    // The guest's facts for the host (gpu/mirror_view_link.h), published on every way out, the rig even when no phase runs. The stamp's flag goes through its own channel (SetStampWanted: the
    // host reads that one). With the guest half off the blocks are kept all zero (nothing is read from the host either) and the stamp's flag is back at its default.
    const shared::Mode mode = CurrentMode();
    vlink::GuestFacts facts;
    if (mode.shared) facts.rig = larecomp::triggers::IsGamePaused() ? -1 : CurrentCameraRig();
    const auto publish = [&](bool hud_stamp, bool phase) {
        if (!mode.shared) return;
        vlink::SetStampWanted(hud_stamp && !HideHudWithCabin());
        facts.phase_asked = phase;
        facts.side_only = phase && shared::SideOnly(mode);
        vlink::PublishGuest(facts);
    };
    if (!mode.shared) {
        vlink::PublishGuest(vlink::GuestFacts{});
        vlink::WithdrawCamera();
        vlink::SetStampWanted(true);
    }

    uint32_t form = 0, widget = 0;
    if (!FindRearView(base, form, widget)) {
        mcla::gpu::mirror_draws::Withdraw();  // no HUD: no mirror on the screen (the host counts nothing)
        publish(false, false);
        return;
    }

    // The HUD mirror (rearview), or the side mirrors' view while the guest half is on and the host asks for it: the phase is wanted. Both off: as it always was.
    const bool want = shared::PhaseWanted(mode);
    ApplyMirrorMembership(base, want, mode);
    const uint32_t widget_flags = ReadGuestU32(base, widget + kWidgetFlags);

    static int last_state = -1;  // -1 unknown, 0 hidden, 1 shown, 2 the side mirrors' view only
    if (!want) {
        // Nothing to write: the update and draw tests are overridden while off (see the
        // Patch_RearView* hooks below), whatever the bit says.
        mcla::gpu::mirror_draws::Withdraw();
        publish(false, false);
        if (last_state != 0) {
            LARECOMP_APP_INFO("[RearView] hidden");
            last_state = 0;
        }
        return;
    }

    // Follow the form's own visibility, so the mirror disappears with the HUD. (The side mirrors' view, with the HUD mirror off, does not follow it: the car's mirrors work with the
    // HUD hidden; the visibility hooks answer for it and the frame stays hidden.)
    const uint32_t form_flags = ReadGuestU32(base, form + kWidgetFlags);
    const bool form_visible = (form_flags & kWidgetVisible) != 0;
    g_form_visible.store(form_visible, std::memory_order_relaxed);
    const RearViewPreset preset = ResolvePreset();
    const int shift_x = preset.x + larecomp::settings::rearview_shift_x.Get();
    const int shift_y = preset.y + larecomp::settings::rearview_shift_y.Get();
    const float frame_x = kFrameDefaultX - float(shift_x);  // Frame: x = screen_width - [+160], so moving it left means a larger value.
    const float frame_y = kFrameDefaultY + float(shift_y);
    if (mode.hud) {
        const uint32_t next = (widget_flags & ~kWidgetVisible) | (form_flags & kWidgetVisible);
        if (next != widget_flags) WriteGuestU32(base, widget + kWidgetFlags, next);

        if (ReadGuestF32(base, widget + kFrameOffsetX) != frame_x)
            WriteGuestF32(base, widget + kFrameOffsetX, frame_x);
        if (ReadGuestF32(base, widget + kFrameOffsetY) != frame_y)
            WriteGuestF32(base, widget + kFrameOffsetY, frame_y);
    }

    // Scene viewport, into the mirror pass object. The side-only case gets the same rectangle (the HUD mirror's place): the phase's draws are redirected by the host and the camera's
    // aspect is ours, so any valid rectangle will do, and this one keeps the host's "mirror draws" count and the retail field of view's meaning.
    const uint32_t pass = ReadGuestU32(base, kMirrorPassPtr);
    if (PlausibleGuestPtr(pass)) {
        const int w = larecomp::settings::rearview_window_w.Get();
        const int h = larecomp::settings::rearview_window_h.Get();
        const int x = kWindowX + shift_x + larecomp::settings::rearview_window_dx.Get();
        const int y = kWindowY + shift_y + larecomp::settings::rearview_window_dy.Get();
        if (w > 0 && h > 0) {
            const uint32_t want_rect[4] = {uint32_t(x), uint32_t(y), uint32_t(w), uint32_t(h)};
            for (uint32_t i = 0; i < 4; ++i) {
                if (ReadGuestU32(base, pass + 4u * i) != want_rect[i])
                    WriteGuestU32(base, pass + 4u * i, want_rect[i]);
            }
            // The host tells the mirror's draws by this rectangle (gpu/mirror_draws.h): the same numbers, in guest pixels.
            mcla::gpu::mirror_draws::Publish({x, y, w, h});
        } else {
            mcla::gpu::mirror_draws::Withdraw();
        }
    } else {
        mcla::gpu::mirror_draws::Withdraw();
    }

    publish(shared::HudStamp(mode, form_visible), true);

    const int state = shared::SideOnly(mode) ? 2 : 1;
    if (last_state != state) {
        if (state == 2) {
            LARECOMP_APP_INFO("[RearView] side mirrors' view only (the HUD mirror is off): viewport {},{} {}x{}",
                              kWindowX + shift_x + larecomp::settings::rearview_window_dx.Get(),
                              kWindowY + shift_y + larecomp::settings::rearview_window_dy.Get(),
                              larecomp::settings::rearview_window_w.Get(), larecomp::settings::rearview_window_h.Get());
        } else {
            LARECOMP_APP_INFO("[RearView] shown: viewport {},{} {}x{}, frame x offset {}",
                              kWindowX + shift_x + larecomp::settings::rearview_window_dx.Get(),
                              kWindowY + shift_y + larecomp::settings::rearview_window_dy.Get(),
                              larecomp::settings::rearview_window_w.Get(), larecomp::settings::rearview_window_h.Get(), frame_x);
        }
        last_state = state;
    }
}

// Hooked at 0x8236051C, the second instruction of mcCarModel's draw (sub_82360518, vtbl+8 of
// the items in the mcCarModelClass client; r3 = the item, r4 = the phase).
//
// Two things keep a car out of the mirror, and both are fixed here.
//
// 1. The draw is a ladder over the phases it knows -- 0x10000 (the colour pass), 0x2,
//    0x400000, 0x800000, 0x8000 and so on -- and for 0x1000000, the mirror's, it falls through
//    every rung and draws nothing. Telling it the phase is 0x10000 makes it draw the car the
//    way it does in the main view.
// 2. The colour pass only draws a car whose [item+116] is non-zero, and that word is the car's
//    visibility, worked out by sub_8235F738 in phase 0x200 against the camera that is current
//    then: the main one. A car behind you is outside that frustum, so [item+116] is 0 and no
//    pass draws it (measured: every car in a race has [+116]=0 except the one almost on top of
//    the camera, whatever its distance). So in the mirror phase, with the mirror's camera now
//    current, sub_8235F738 is run again and the car drawn only if it is in the mirror's view.
//
// [item+116] is put back on the car's next call, whatever phase that is, because the phases
// after the mirror's (the car blur ones) are meant to see the main view's value.
static constexpr uint32_t kCarVisibilityPrep = 0x8235F738u;
static constexpr uint32_t kCarVisibleWord    = 116u;

// True only while the mirror's own run of sub_8235F738 is in progress (same thread).
static thread_local bool g_in_mirror_prep = false;

// Hooked at 0x8235F9EC, right after `bl sub_82175F20` in sub_8235F738 (r3 = its result).
// That call tests the car's sphere against the frustums the renderer keeps for the MAIN view
// (the per-view block at [mgr]+57792*viewIndex) and answers 0x4000000 (outside) or 0x10000000
// (occluded); sub_8235F738 then zeroes [item+116] on either. Those frustums are the main
// camera's, whatever camera is current, so a car behind you always fails it. During the
// mirror's own run the two bits are cleared, which leaves the frustum test against the current
// (mirror) camera, done earlier in the same function, as the only visibility test.
void Patch_RearViewCullBits(PPCRegister& r3) {
    if (g_in_mirror_prep) r3.u64 &= ~uint64_t(0x14000000u);
}

// Vehicle layout and render client links match the census in car_model_walk.cpp.
static constexpr uint32_t kPlayerVehicle = 0x8288DCF8u;
static constexpr uint32_t kVehicleVtable = 0x82047B14u;
static constexpr uint32_t kVehicleEntity = 20u;
static constexpr uint32_t kEntityRenderPosition = 208u + 48u;
static constexpr uint32_t kVehicleForcedLod = 5996u;
static constexpr uint32_t kVehicleBodyLod = 6004u;
static constexpr uint32_t kVehiclePartLod = 6008u;
static constexpr size_t kRacerListLimit = 256;

struct CarState {
    uint32_t item;
    uint16_t visible;
    uint32_t forced;
    uint32_t body;
    uint32_t parts;
};
static std::array<CarState, kRacerListLimit> g_mirror_cars{};
static size_t g_mirror_car_count = 0;
static bool g_cars_selected = false;

static uint16_t CarVisibility(const uint8_t* base, uint32_t item) {
    if (!base) return 0;
    return uint16_t((uint32_t(base[item + kCarVisibleWord]) << 8) | base[item + kCarVisibleWord + 1]);
}

static void RestoreCar(uint8_t* base, const CarState& car) {
    if (!base || !PlausibleGuestPtr(car.item)) return;
    base[car.item + kCarVisibleWord] = uint8_t(car.visible >> 8);
    base[car.item + kCarVisibleWord + 1] = uint8_t(car.visible);
    WriteGuestU32(base, car.item + kVehicleForcedLod, car.forced);
    WriteGuestU32(base, car.item + kVehicleBodyLod, car.body);
    WriteGuestU32(base, car.item + kVehiclePartLod, car.parts);
}

static void RestoreMirrorCars(uint8_t* base) {
    for (size_t i = 0; i < g_mirror_car_count; ++i) RestoreCar(base, g_mirror_cars[i]);
    g_mirror_car_count = 0;
    g_cars_selected = false;
}

// Called from the first racer draw, when the rear camera is current. Rank
// the renderer's list once and test visibility in order until the budget fills.
// CHECK: the live mirror phase visits the same slot-7 items on both backends.
static void SelectMirrorCars(uint8_t* base, PPCFunc* prep, bool shared_view) {
    if (!base || !prep) return;
    g_cars_selected = true;
    const uint32_t renderer = ReadGuestU32(base, kRendererPtr);
    if (!PlausibleGuestPtr(renderer)) return;
    const uint32_t client = ReadGuestU32(base, renderer + kRendererClients + 4u * 7u);
    if (!PlausibleGuestPtr(client)) return;
    const uint32_t player = ReadGuestU32(base, kPlayerVehicle);
    if (!PlausibleGuestPtr(player)) return;
    float eye[3]{};
    if (!MainEye(base, renderer, eye)) return;

    const double far_m = shared_view ? larecomp::settings::rearview_far.Get() : 0.0;
    std::array<cheap::RankedCar, kRacerListLimit> cars;
    size_t count = 0;
    size_t visited = 0;
    size_t types = 0;
    for (uint32_t t = ReadGuestU32(base, client + 8); PlausibleGuestPtr(t) && types++ < 64; t = ReadGuestU32(base, t + 12)) {
        for (uint32_t item = ReadGuestU32(base, t + 20); PlausibleGuestPtr(item) && visited++ < cars.size(); item = ReadGuestU32(base, item + 8)) {
            if (item == player || ReadGuestU32(base, item) != kVehicleVtable) continue;
            const uint32_t entity = ReadGuestU32(base, item + kVehicleEntity);
            if (!PlausibleGuestPtr(entity)) continue;
            float metres_squared = 0.0f;
            for (uint32_t axis = 0; axis < 3; ++axis) {
                const float delta = ReadGuestF32(base, entity + kEntityRenderPosition + 4u * axis) - eye[axis];
                metres_squared += delta * delta;
            }
            if (cheap::InRange(metres_squared, far_m)) cars[count++] = {metres_squared, item};
        }
    }
    std::sort(cars.begin(), cars.begin() + count);

    const int cap = shared_view ? larecomp::settings::rearview_cars_max.Get() : 0;
    const int lod = shared_view ? larecomp::settings::rearview_cars_lod.Get() : 0;
    for (size_t i = 0; i < count && (cap == 0 || g_mirror_car_count < size_t(cap)); ++i) {
        const uint32_t item = cars[i].item;
        if (i > 0 && cars[i - 1].item == item) continue;
        const CarState saved{item, CarVisibility(base, item), ReadGuestU32(base, item + kVehicleForcedLod),
                            ReadGuestU32(base, item + kVehicleBodyLod), ReadGuestU32(base, item + kVehiclePartLod)};
        if (lod > 0) {
            const int forced = int(int32_t(saved.forced));
            WriteGuestU32(base, item + kVehicleForcedLod, uint32_t(std::max(lod, std::min(forced, cheap::kLodCoarsest))));
        }
        g_in_mirror_prep = true;
        larecomp::guest::Call<uint32_t>(prep, item);
        g_in_mirror_prep = false;
        if (CarVisibility(base, item) == 0) {
            RestoreCar(base, saved);
            continue;
        }

        // Some parts have a separate forced LOD. Keep that override out of the
        // shared view and put both chosen levels back as soon as the phase ends.
        const int parts = int(int32_t(ReadGuestU32(base, item + kVehiclePartLod)));
        const int coarse_parts = cheap::CarLevel(parts, lod);
        if (parts != coarse_parts) WriteGuestU32(base, item + kVehiclePartLod, uint32_t(coarse_parts));
        if (ReadGuestU32(base, item + kVehicleBodyLod) != saved.body || parts != coarse_parts) ++g_stats.cars_coarser;
        g_mirror_cars[g_mirror_car_count++] = saved;
    }
    g_stats.cars_seen += count;
    g_stats.cars_turned_away += count - g_mirror_car_count;
}

void Patch_RearViewCarPhase(PPCRegister& r3, PPCRegister& r4) {
    const uint32_t item = r3.u32;
    const uint32_t phase = r4.u32;
    mcla::gpu::car_model_walk::tap::NoteVehicle(item, phase);
    if (phase != kMirrorPhase || !PlausibleGuestPtr(item)) return;

    auto* base = larecomp::guest::Membase();
    if (!base) return;
    if (!g_cars_selected) {
        auto* rt = REX_RUNTIME();
        if (rt) SelectMirrorCars(base, rt->FindFunction(kCarVisibilityPrep).value_or(nullptr), SharedViewOn());
    }
    for (size_t i = 0; i < g_mirror_car_count; ++i) {
        if (g_mirror_cars[i].item != item) continue;
        r4.u64 = 0x10000;
        ++g_stats.cars_drawn;
        break;
    }
}

// Hooked at the entry of the mirror phase's begin (sub_82308178, called from the phase
// callback sub_82306500 for 0x1000000). Counts what each client holds at that moment:
// client+8 is its list of render types and each type's +20 its list of instances.
void Patch_RearViewDiag() {
    // Debug city radius cuts apply until the phase ends.
    g_in_mirror_phase = true;
    // The caller has tested the renderer's mirror bit. The begin clear and end sky use the consumer byte even if r21 masks out the client loop.
#if defined(LARECOMP_HAS_GPU)
    if (mcla::gpu::Enabled()) {
        auto* base = larecomp::guest::Membase();
        const bool hud_stamp = HudStampWanted();
        SendMirrorMarker(g_markers.OnBegin(SharedViewOn() || !hud_stamp, true, base && MirrorConsumerActive(base)), hud_stamp);
    }
#endif
    if (!larecomp::settings::rearview_diag.Get()) return;
    static std::atomic<uint32_t> calls{0};
    if (calls.fetch_add(1, std::memory_order_relaxed) % 120 != 0) return;
    auto* base = larecomp::guest::Membase();
    if (!base) return;
    const uint32_t renderer = ReadGuestU32(base, kRendererPtr);
    if (!PlausibleGuestPtr(renderer)) return;
    std::string out;
    for (int slot : {0, 7, 9, 10}) {
        const uint32_t client = ReadGuestU32(base, renderer + kRendererClients + 4u * uint32_t(slot));
        if (!PlausibleGuestPtr(client)) continue;
        int types = 0, items = 0;
        for (uint32_t t = ReadGuestU32(base, client + 8); PlausibleGuestPtr(t) && types < 256;
             t = ReadGuestU32(base, t + 12)) {
            ++types;
            for (uint32_t i = ReadGuestU32(base, t + 20); PlausibleGuestPtr(i) && items < 4096;
                 i = ReadGuestU32(base, i + 8))
                ++items;
        }
        out += " slot" + std::to_string(slot) + "(mask 0x" + [&] {
            char b[16]; snprintf(b, sizeof b, "%08X", ReadGuestU32(base, client + kClientMask)); return std::string(b);
        }() + ") types=" + std::to_string(types) + " items=" + std::to_string(items);
    }
    LARECOMP_APP_INFO("[RearView] diag at mirror phase begin:{}", out);
}

// Hooked at 0x82308120, the `bl sub_8217D460` in sub_82307F28 (f1 = the camera's field of view, [pass+20]: the VERTICAL field in DEGREES, as the main camera's call from sub_822E5E10
// passes it; f2 = the aspect, the stamp's w/h, `fdivs f2,f10,f9` of the rectangle's ints [pass+8] / [pass+12]; f3 = the near plane and f4 = the far plane, the main camera's, `lfs f3/f4,704/708`
// of the camera object at 0x82839704). sub_82307F28 is the mirror's camera step, run from the phase callback sub_82306500 at the colour pass's begin whenever the mirror is requested;
// sub_8217D460 stores the four into the camera (+696 .. +708) and builds its frustum (sub_8217CB68: tan(f1 x half-angle constant) = the half height, x f2 = the half width), which is all the
// game's own culling of the mirror's phase has to go by. The registers hold single floats as doubles.
//
// With the guest half on (mcla_gpu_mirror_view) the hook sets f1 and f2 to the field the host asks for (gpu/mirror_view_link.h Request: its view is drawn with that field, and the
// host's lookup and crop math assume it) and publishes the game's own field first (CameraFacts::retail_hfov_deg: what the retail HUD mirror shows across, 2 atan(tan(f1/2) x f2), which
// the host's HUD stamp keeps), then the field used: the facts of THIS build go to the host with the begin marker of the phase it is for (gpu NoteMirrorPhase reads them: vlink::PhaseField, MV-J2),
// so the host never mistakes a later camera for the one a queued phase was drawn with. CHECK (MV-J2): the camera step runs before the begin step in the same frame, on this thread (the facts of the
// begin's tag are then this phase's); the host logs "[mirror view] phase field" at every change of the tag, which must follow the "[RearView] camera" line one to three frames later. Without a request the game's own field stays (the retail look). rearview_fov_scale is then NOT applied to a field the host asked for
// (it would break the host's math); rearview_far still is (it changes what is drawn, not the picture's geometry).
// Without the guest half: rearview_fov_scale narrows f1 and rearview_far pulls in f4 (rear_view_cheap.h), and both leave the registers alone while off.
//
// CHECK: that f1 is the vertical field in DEGREES that sub_8217CB68 halves and takes the tangent of (the constant at 0x820086A8, read by `lfs f0,-31064(r11)`, should be pi / 360; the
// main camera's field is in degrees by its callers' own notes: menu_camera.cpp, the Vert+ hook at 0x822E5E68), and that the factor between [cam+712] and [cam+716] is f2 times the constant at
// 0x827D4390 (`lfs f0,17296(r10)`, should be 1.0); the log's "[RearView] camera" line says what the game's own field was and what was applied. The game's default field is the float at
// 0x82007F9C that the pass object's constructor (sub_82307D68, `lfs f0,32668(r10)` then `stfs f0,20(r31)`) stores: its value is CHECK 11 of the design.
void Patch_RearViewCamera(PPCRegister& f1, PPCRegister& f2, PPCRegister& f4) {
    const double fov_scale = larecomp::settings::rearview_fov_scale.Get();
    const double far_m = larecomp::settings::rearview_far.Get();
    const bool shared_on = SharedViewOn();
    if (!shared_on) return;
    ++g_stats.camera_builds;
    const float fov_was = float(f1.f64);
    const float aspect_was = float(f2.f64);
    const float far_was = float(f4.f64);
    const shared::CameraBuild cam = shared::BuildCamera(fov_was, aspect_was, far_was, shared_on, shared_on ? vlink::CurrentRequest() : vlink::Request{}, fov_scale, far_m);
    if (shared_on) {
        ++g_shared_stats.camera_builds;
        if (cam.applied) ++g_shared_stats.camera_applied;
        vlink::CameraFacts facts;
        facts.retail_hfov_deg = shared::HudFieldDeg(fov_was, aspect_was, fov_scale);  // the HUD mirror's field without a request (the host's stamp cuts it out of a wider view)
        facts.used_hfov_deg = shared::HorizontalFovDeg(cam.fov_deg, cam.aspect);
        facts.used_vfov_deg = cam.fov_deg;
        facts.from_request = cam.applied;  // the host's plan follows a requested field, the rectangle otherwise: it rides on this phase's begin marker (NoteMirrorPhase, vlink::PhaseField)
        facts.seq = uint32_t(g_shared_stats.camera_builds & 0xFFu);
        vlink::PublishCamera(facts);
        static float logged_fov = -1.0f, logged_aspect = -1.0f, logged_used = -1.0f;
        if (fov_was != logged_fov || aspect_was != logged_aspect || cam.fov_deg != logged_used) {
            logged_fov = fov_was;
            logged_aspect = aspect_was;
            logged_used = cam.fov_deg;
            LARECOMP_APP_INFO("[RearView] camera: the game's own field {:.2f} deg up, aspect {:.3f} ({:.1f} deg across); built with {:.2f} deg up, {:.1f} deg across{}", fov_was, aspect_was,
                              shared::HorizontalFovDeg(fov_was, aspect_was), cam.fov_deg, facts.used_hfov_deg, cam.applied ? " (the host's request)" : " (the game's own)");
        }
    }
    f1.f64 = double(cam.fov_deg);
    if (cam.applied) f2.f64 = double(cam.aspect);
    f4.f64 = double(cam.far_m);
    if (!shared_on) {
        static float logged_fov = -1.0f, logged_far = -1.0f;
        if (cam.fov_deg != logged_fov || cam.far_m != logged_far) {
            logged_fov = cam.fov_deg;
            logged_far = cam.far_m;
            LARECOMP_APP_INFO("[RearView] cheap camera: field of view {:.3f} -> {:.3f}, far plane {:.0f} -> {:.0f} m", fov_was, cam.fov_deg, far_was, cam.far_m);
        }
    }
}

// CHECK: compare this rear image and its culling with a matched virtual-eye
// reference. r4 is the scratch copy of the camera matrix, used both by
// sub_8217CE80 and the pass. The main camera object stays untouched.
void Patch_RearViewCabinEye(PPCRegister& r4) {
    vlink::g_phase_cabin_offset = 0;
    const uint32_t tag = SharedViewOn() ? CabinEyeRequest() : 0;
    auto* base = larecomp::guest::Membase();
    if (!tag || !base || !PlausibleGuestPtr(r4.u32) || (r4.u32 & 15u)) return;
    const auto row = [&](uint32_t at) {
        return cabin::V3{ReadGuestF32(base, at), ReadGuestF32(base, at + 4), ReadGuestF32(base, at + 8)};
    };
    const uint32_t matrix = r4.u32;
    const auto delta = cabin::OffsetInRearAxes(cabin::UnpackOffset(tag), row(matrix), row(matrix + 16), row(matrix + 32));
    const auto eye = row(matrix + 48) + delta;
    if (!std::isfinite(eye.x) || !std::isfinite(eye.y) || !std::isfinite(eye.z) || !(cabin::Length(delta) < 8.0)) return;
    WriteGuestF32(base, matrix + 48, float(eye.x));
    WriteGuestF32(base, matrix + 52, float(eye.y));
    WriteGuestF32(base, matrix + 56, float(eye.z));
    vlink::g_phase_cabin_offset = tag;
}

// Hooked at 0x82307F8C, the first `bl sub_82178C40` of sub_82307F28 (the mirror's camera step, called from the colour pass's begin): a D3D Clear(1, &rect, colour + depth + stencil flags) of
// the rectangle at [pass+0..12] (r3 = x, r4 = y, r5 = w, r6 = h, flags r7 = 1, r9 = 1 and the stack byte 87 = 1, f1 = the depth 0.0 from `lfs f31,3796(r11)`, r8 = 0xFFFF0000) into the MAIN
// target, before the main world draws. True jumps to 0x82307F90, past the call. The phase's own clear is the second sub_82178C40 call, in sub_82308178, with black and depth 1.0.
// With the guest half on and the host redirecting the phase (SkipCameraClear) the call is skipped: the rectangle of the main target is not the phase's any more (the HUD stamp covers its
// colour, and a clear left under it would leave depth 0 there for the host's effects). A visible HUD mirror falling back to the game still gets its own clear.
// CHECK: that this call is a Clear of the rectangle with those values (the design's CHECK 1 and 2), and that skipping it changes nothing but what is said here: the main world then draws in the rectangle.
bool Patch_RearViewCameraClear() {
#if defined(LARECOMP_HAS_GPU)
    // A buffered phase can remain active while the HUD or raster view is being switched off.
    if (mcla::gpu::Enabled() && !HudStampWanted()) return true;
#endif
    return shared::SkipCameraClear(SharedViewOn(), vlink::CurrentRequest());
}

// Hooked at 0x822D5704, the `bl sub_822D7120` in the city's per-sector walk (sub_822D5618; f1 = the sector's hi detail radius, mcCity+36 x mcCity+24, 300 m times the
// scale of lod_city_scale and a speed factor). Inside the mirror's phase (Patch_RearViewDiag .. Patch_RearViewPhaseEnd) rearview_lod_distance cuts it, so a sector
// beyond that is the coarse one-model-a-sector city. A flag of the phase loop's own thread (the game's update thread walks the sectors for the main view and must not see it),
// not a write to the game's float, so nothing has to be put back. Called for every sector of every walk: one flag test while it is down.
//
// CHECK: that the walk runs inside the mirror's phase for the mirror's camera. sub_822DD790 (the city client's phase step) calls sub_822D5618 for its phase 1
// only, so the mirror's sectors may be those of the main view's walk; then this changes nothing and "city sectors cut" stays 0 in the log.
void Patch_RearViewCityLod(PPCRegister& f1) {
    if (!g_in_mirror_phase) return;
    const double wanted = larecomp::settings::rearview_lod_distance.Get();
    if (!(wanted > 0.0)) return;
    const float stock = float(f1.f64);
    const float cut = cheap::LodRadius(stock, wanted);
    if (cut != stock) {
        f1.f64 = double(cut);
        g_city_radius_cuts.fetch_add(1, std::memory_order_relaxed);
    }
}

// Hooked at 0x822E6634 in the renderer's phase loop (sub_822E6408), the `cmpwi cr6,r10,0` right after `and r21,r17,r11` (r17 = [r27+364], the phases the renderer
// may run; r21 = r17 & the requested ones; r27 = the renderer). Once a frame, before the first phase. With rearview_every above 1 the frames in between the mirror's
// turns are held: the mirror's bit leaves r17 and r21 (the clients' test) and renderer+364 (the begin steps' test, sub_82306500 for 0x10000 and 0x1000000), so no
// part of the phase runs; Patch_RearViewPhaseEnd puts +364 back. A rig change, camera cut or newly visible reader redraws immediately.
//
// Without the guest half this does NOT keep the picture: the stamp's pixels are the main view's on a held frame (the host has no copy to put back yet), so the switch measures
// the saving, it does not show the finished feature. With the guest half on the host keeps its last view image and both readers go on drawing from it, and the rate is the larger of
// this setting's and the host's (rear_view_shared.h EffectiveEvery). The mirror is "up" for the cadence when the HUD mirror is on or the side mirrors ask for the view.
// CHECK: that +364 is what the begin steps test and that nothing else re-derives the bit mid-frame.
static void RearViewCadenceStep(PPCRegister& r17, PPCRegister& r21, PPCRegister& r27) {
    g_in_mirror_phase = false;  // a new frame: the city's radius cut never carries over
    g_frame_held = false;
    auto* base = larecomp::guest::Membase();
    if (!base || !PlausibleGuestPtr(r27.u32)) return;
    RestoreMirrorCars(base);  // also recovers a phase whose end hook was missed
    const uint32_t renderer = r27.u32;
    if (g_masked_renderer != 0) {  // the end hook did not run last frame: put the bit back before anything reads it
        const uint32_t now = ReadGuestU32(base, g_masked_renderer + kRendererPhaseMask);
        WriteGuestU32(base, g_masked_renderer + kRendererPhaseMask, now | kMirrorPhase);
        g_masked_renderer = 0;
    }
    // The mirror is up when the HUD mirror is on or, with the guest half, the side mirrors ask for the view (a held frame is the view's too: the host keeps its image).
    const shared::Mode mode = CurrentMode();
    const auto request = vlink::CurrentRequest();
    if (!mode.shared || !request.redirect || (r21.u32 & kMirrorPhase) == 0 || !shared::PhaseWanted(mode) || !MirrorConsumerActive(base)) {
        g_cadence.Reset();
        g_refresh_clock.Reset();
        g_have_last_eye = false;
        return;
    }
    float eye[3] = {0.0f, 0.0f, 0.0f};
    const bool have_eye = MainEye(base, renderer, eye);
    static int last_rig = -1;
    static bool was_stamp_wanted = false;
    static uint8_t previous_side_mask = 0;
    static bool had_cabin_eye = false;
    const int rig = CurrentCameraRig();
    const bool stamp_wanted = HudStampWanted();
    const bool cabin_eye = CabinEyeRequest() != 0;
    const bool cut = rig != last_rig || cabin_eye != had_cabin_eye || (stamp_wanted && !was_stamp_wanted) ||
                     ((request.side_mask & ~previous_side_mask) != 0) ||
                     (have_eye && cheap::EyeJumped(g_last_eye, eye, g_have_last_eye));
    last_rig = rig;
    was_stamp_wanted = stamp_wanted;
    previous_side_mask = request.side_mask;
    had_cabin_eye = cabin_eye;
    if (have_eye) {
        for (int i = 0; i < 3; ++i) g_last_eye[i] = eye[i];
    }
    g_have_last_eye = have_eye;

    const int hz = larecomp::settings::rearview_refresh_hz.Get();
    const bool draw = hz > 0 ? g_refresh_clock.Draw(cheap::RefreshClock::Clock::now(), hz, cut)
                             : g_cadence.Decide(larecomp::settings::rearview_every.Get(), cut);
    ++g_stats.frames;
    if (draw && cut) ++g_stats.cut_draws;
    if (!draw) ++g_stats.held;
    if (mode.shared) {
        ++g_shared_stats.frames;
        if (shared::SideOnly(mode)) ++g_shared_stats.side_only;
    }
    LogCheapState();
    LogSharedState();
    if (draw) return;

    // A held frame: the phase is not asked for.
    g_frame_held = true;
    r17.u64 &= ~uint64_t(kMirrorPhase);
    r21.u64 &= ~uint64_t(kMirrorPhase);
    const uint32_t mask = ReadGuestU32(base, renderer + kRendererPhaseMask);
    if (mask & kMirrorPhase) {
        WriteGuestU32(base, renderer + kRendererPhaseMask, mask & ~kMirrorPhase);
        g_masked_renderer = renderer;
    }
}

// A bracket still open at the frame's first look at the phase loop lost its END. Close it before applying the cadence.
void Patch_RearViewCadence(PPCRegister& r17, PPCRegister& r21, PPCRegister& r27) {
    SendMirrorMarker(g_markers.OnFrameStart());
    ApplyEnvViewHold(r21, r27);
    RearViewCadenceStep(r17, r21, r27);
}

// Hooked at 0x822E6908, before the phase increment. The mirror end callback (sub_82306758 -> sub_823082B0) draws its sky via sub_82494258 while the rear viewport
// is still current, then restores the main camera. Keep the host redirect and the held frame's mask in force through the callback.
// CHECK: confirm the trailing sky draw lands in the private view on both APIs and the held frame emits only an empty marker pair.
void Patch_RearViewPhaseEnd(PPCRegister& r30) {
    FinishEnvViewPhase(r30);
    if (r30.u32 != kMirrorPhase) return;
    RestoreMirrorCars(larecomp::guest::Membase());
    g_in_mirror_phase = false;
    SendMirrorMarker(g_markers.OnEnd());  // the END of the draws the host redirects (one for every BEGIN sent; none for a phase that did not run)
    if (g_markers.OnHeldFrame(SharedViewOn(), g_frame_held)) {  // a held frame: the phase was not run, the host stamps the picture it kept: BEGIN and END back to back, where the phase would have ended
        SendMirrorMarker(vlink::kMarkerBegin, HudStampWanted());
        SendMirrorMarker(vlink::kMarkerEnd);
    }
    g_frame_held = false;
    if (g_masked_renderer != 0) {
        auto* base = larecomp::guest::Membase();
        if (base) {
            const uint32_t now = ReadGuestU32(base, g_masked_renderer + kRendererPhaseMask);
            WriteGuestU32(base, g_masked_renderer + kRendererPhaseMask, now | kMirrorPhase);
        }
        g_masked_renderer = 0;
    }
}

// What a switched-off mirror must never do is draw or request its phase, and both
// follow from three tests the game makes itself (two in the update, one in the draw wrapper).
// These hooks answer them while `rearview` is off and leave them alone while it is on.
//
// THE THREE STATES (MIRROR-VIEW.md 2.4, rear_view_shared.h PhaseVisible / FrameHidden): with the guest half on (mcla_gpu_mirror_view) the host's request for the side mirrors' view
// makes both visibility answers "visible" whatever `rearview` and the HUD form say (the phase is asked for and the consumer active: the game draws the mirror's rear view), while the
// draw hook keeps the frame hidden unless the HUD mirror is on and its form visible (no white frame, and the host draws no stamp). Off, nothing of this exists.
//
// mcRearView::update (sub_82233C18) asks the widget whether it is visible twice, through
// vtbl+60 (flags+16 & 0x20), and uses only the low byte of r3 each time:
//   0x82233C54  bctrl                  the ramp test: visible = alpha up and the 0x1000000
//   0x82233C58  lis r11,-32256   <--   phase requested; hidden = alpha down and the phase
//                                      dropped
//   0x82233CE4  bctrl                  the consumer's active byte (sub_82308298 stores
//   0x82233CE8  mr r4,r3         <--   r4's low byte at [pass + 32])
// Hooked before the instruction after each call, r3 = the answer.
//
// The HUD form's refresh (sub_82652860) can set the bit between TickRearView's frame-start
// tick and this update, which is how an off mirror used to get through. The first site counts
// how often the bit is found set while off and says so in the log (at 1, 2, 4, 8, ... times),
// which is the proof in a test drive.
//
// CHECK: read from the generated code only, not run. Which HUD event refreshes the form
// (sub_82652860 is only reached through vtables) is not traced; a scripted drive with
// rearview off should show the "saw the visible bit" line at the moments the mirror used
// to pop up, and no mirror view or white frame at all.
void Patch_RearViewUpdateVisible(PPCRegister& r3) {
    const shared::Mode mode = CurrentMode();
    const bool game_visible = (r3.u32 & 0xFFu) != 0;
    const bool visible = shared::PhaseVisible(mode, game_visible);
    if (visible == game_visible) return;  // the HUD mirror on: the game's own answer; nothing to change
    if (!visible && !mode.hud) {          // the HUD mirror off: the game's bit was found set (see above)
        static std::atomic<uint32_t> blocked{0};
        const uint32_t n = blocked.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((n & (n - 1)) == 0)
            LARECOMP_APP_INFO("[RearView] mirror update saw the visible bit while off, answered hidden (#{})", n);
    }
    r3.u64 = visible ? 1 : 0;  // hidden, or (the guest half, the host's side mirror need) visible whatever the HUD form says
}

void Patch_RearViewUpdateActive(PPCRegister& r3) {
    const shared::Mode mode = CurrentMode();
    const bool game_visible = (r3.u32 & 0xFFu) != 0;
    const bool visible = shared::PhaseVisible(mode, game_visible);
    if (visible != game_visible) r3.u64 = visible ? 1 : 0;
}

// Hooked at 0x82234148, the `fcmpu cr6, f13, f0` of sub_82234128 (called from sub_82227428,
// the HUD draw, with r3 = the mcRearView). The wrapper draws the frame when the option is on
// and alpha ([r3+1216], in f13) > 0 -- it never looks at the visible bit. Off, alpha reads 0
// (f0 is 0.0 there), so the wrapper returns before the frame's meshes and their render
// state are touched.
//
// CHECK: that the recompiled wrapper takes the blelr with f13 forced to 0.0 (read from the
// generated sub_82234128, not run); turning rearview off while the mirror is up should
// drop the frame at once instead of fading it.
void Patch_RearViewDrawAlpha(PPCRegister& f13) {
    if (HideHudWithCabin() || shared::FrameHidden(CurrentMode(), g_form_visible.load(std::memory_order_relaxed))) f13.f64 = 0.0;
}

// Hooked at 0x822340F4 in mcRearView::Draw (sub_82233E18): the `lwzx` that loads the
// frame mesh just before its vtbl+32 draw call. True jumps to 0x82234108, past the
// draw, so the frame is not drawn; the mirror's scene viewport is a separate pass
// and is untouched.
bool Patch_RearViewFrameSkip() {
    return !larecomp::settings::rearview_frame.Get();
}

#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

void TickRearView() {}
bool Patch_RearViewFrameSkip() { return false; }
void Patch_RearViewUpdateVisible(PPCRegister& r3) {}
void Patch_RearViewUpdateActive(PPCRegister& r3) {}
void Patch_RearViewDrawAlpha(PPCRegister& f13) {}
void Patch_RearViewCarPhase(PPCRegister& r3, PPCRegister& r4) {}
void Patch_RearViewDiag() {}
void Patch_RearViewCullBits(PPCRegister& r3) {}
void Patch_RearViewCamera(PPCRegister& f1, PPCRegister& f2, PPCRegister& f4) {}
void Patch_RearViewCabinEye(PPCRegister& r4) {}
bool Patch_RearViewCameraClear() { return false; }
void Patch_RearViewCityLod(PPCRegister& f1) {}
void Patch_RearViewCadence(PPCRegister& r17, PPCRegister& r21, PPCRegister& r27) {}
void Patch_RearViewPhaseEnd(PPCRegister& r30) {}
#endif // REXGLUE_HAS_XEO3_TARGET
