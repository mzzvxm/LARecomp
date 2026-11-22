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
//                                      Patch_RearViewPhaseEnd (0x822E68D8, every phase, r30 = the phase)
//                                      puts renderer+364 back. A camera cut draws at once. The picture is
//                                      NOT held (the host does not keep the stamp's pixels yet): a held
//                                      frame shows the main view in the rectangle.
// The host side (gpu/mirror_draws.h): TickRearView publishes the rectangle every frame the mirror is up;
// the Scene counts the draws whose viewport is that rectangle ("mirror: N draws/frame" in the log) and,
// with mcla_gpu_mirror_class, keeps them out of the main view's per-draw effects.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include "settings/larecomp_settings.h"
#include <rex/ppc/call.h>
#include <rex/ppc/recompiled_code.h>
#include <rex/ppc/context.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/function_dispatcher.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>

#include "hooks.h"
#include "hooks_internal.h"
#include "larecomp_log.h"
#include "larecomp_guest.h"
#include "gpu/car_model_walk_tap.h"
#include "gpu/mirror_draws.h"
#include "rear_view_cheap.h"

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
static cheap::CarBudget g_car_budget;
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
    static CheapSwitches last{-1, -1.0, -1.0, -1.0, -1, -1};
    const CheapSwitches now = ReadCheapSwitches();
    if (!(now == last)) {
        last = now;
        LARECOMP_APP_INFO("[RearView] cheap options: every {}, far {:g} m, field of view x{:g}, city radius {:g} m, cars max {}, cars level {}{}", now.every, now.far_m,
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

static void ApplyMirrorMembership(uint8_t* base, bool mirror_on) {
    const uint32_t renderer = ReadGuestU32(base, kRendererPtr);
    if (!PlausibleGuestPtr(renderer)) return;

    static constexpr MirrorClient kCars{7, 0x12E183F3u, "cars"};
    static constexpr MirrorClient kTraffic{9, 0x00118002u, "traffic"};

    SetClientMirrorPhase(base, renderer, kCars, mirror_on && larecomp::settings::rearview_cars.Get());
    SetClientMirrorPhase(base, renderer, kTraffic, mirror_on && larecomp::settings::rearview_traffic.Get());
}

void TickRearView() {
    auto* base = larecomp::guest::Membase();
    if (!base) return;

    uint32_t form = 0, widget = 0;
    if (!FindRearView(base, form, widget)) {
        mcla::gpu::mirror_draws::Withdraw();  // no HUD: no mirror on the screen (the host counts nothing)
        return;
    }

    const bool want = larecomp::settings::rearview.Get();
    ApplyMirrorMembership(base, want);
    const uint32_t widget_flags = ReadGuestU32(base, widget + kWidgetFlags);

    static int last_state = -1;  // -1 unknown, 0 hidden, 1 shown
    if (!want) {
        // Nothing to write: the update and draw tests are overridden while off (see the
        // Patch_RearView* hooks below), whatever the bit says.
        mcla::gpu::mirror_draws::Withdraw();
        if (last_state != 0) {
            LARECOMP_APP_INFO("[RearView] hidden");
            last_state = 0;
        }
        return;
    }

    // Follow the form's own visibility, so the mirror disappears with the HUD.
    const uint32_t form_flags = ReadGuestU32(base, form + kWidgetFlags);
    const uint32_t next = (widget_flags & ~kWidgetVisible) | (form_flags & kWidgetVisible);
    if (next != widget_flags) WriteGuestU32(base, widget + kWidgetFlags, next);

    const RearViewPreset preset = ResolvePreset();
    const int shift_x = preset.x + larecomp::settings::rearview_shift_x.Get();
    const int shift_y = preset.y + larecomp::settings::rearview_shift_y.Get();

    // Frame: x = screen_width - [+160], so moving it left means a larger value.
    const float frame_x = kFrameDefaultX - float(shift_x);
    const float frame_y = kFrameDefaultY + float(shift_y);
    if (ReadGuestF32(base, widget + kFrameOffsetX) != frame_x)
        WriteGuestF32(base, widget + kFrameOffsetX, frame_x);
    if (ReadGuestF32(base, widget + kFrameOffsetY) != frame_y)
        WriteGuestF32(base, widget + kFrameOffsetY, frame_y);

    // Scene viewport, into the mirror pass object.
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

    if (last_state != 1) {
        LARECOMP_APP_INFO("[RearView] shown: viewport {},{} {}x{}, frame x offset {}",
                          kWindowX + shift_x + larecomp::settings::rearview_window_dx.Get(),
                          kWindowY + shift_y + larecomp::settings::rearview_window_dy.Get(),
                          larecomp::settings::rearview_window_w.Get(), larecomp::settings::rearview_window_h.Get(), frame_x);
        last_state = 1;
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

struct SavedVisibility {
    uint32_t item = 0;
    uint16_t word = 0;
    bool lod_saved = false;  // rearview_cars_lod made this car coarser: the main view's level is put back with the visibility word
    uint32_t lod = 0;
};
static SavedVisibility g_saved_visibility[32];

static void RestoreCarVisibility(uint8_t* base, uint32_t item) {
    for (auto& s : g_saved_visibility) {
        if (s.item != item) continue;
        base[item + kCarVisibleWord]     = uint8_t(s.word >> 8);
        base[item + kCarVisibleWord + 1] = uint8_t(s.word);
        if (s.lod_saved) WriteGuestU32(base, item + kCarLodWord, s.lod);
        s.item = 0;
        s.lod_saved = false;
        return;
    }
}

// `keep_lod`: rearview_cars_lod is on, so the car's model level is kept too (before the nested visibility run below, which works the level out again for the
// mirror's camera; with the option off that level is left as it always was).
static void SaveCarVisibility(uint8_t* base, uint32_t item, bool keep_lod) {
    SavedVisibility* slot = nullptr;
    for (auto& s : g_saved_visibility) {
        if (s.item == item) return;  // already holding the main view's value
        if (!slot && s.item == 0) slot = &s;
    }
    if (!slot) return;
    slot->item = item;
    slot->word = uint16_t((uint32_t(base[item + kCarVisibleWord]) << 8) | base[item + kCarVisibleWord + 1]);
    slot->lod_saved = false;
    if (keep_lod) {
        slot->lod = ReadGuestU32(base, item + kCarLodWord);
        slot->lod_saved = true;
    }
}

void Patch_RearViewCarPhase(PPCRegister& r3, PPCRegister& r4) {
    // The debug car model walk (gpu/car_model_walk.h, mcla_gpu_car_model_walk) learns which vehicles the game has from every call of Vehicle::Draw, in any
    // phase and for any vehicle, culled or not: the entry itself carries Hook_SquashSelectVehicle (one hook per address), and this is the next instruction.
    // One relaxed atomic load while the setting is off; reads and writes no guest memory.
    mcla::gpu::car_model_walk::tap::NoteVehicle(static_cast<uint32_t>(r3.u64), static_cast<uint32_t>(r4.u64));

    auto* base = larecomp::guest::Membase();
    if (!base) return;
    const uint32_t item = static_cast<uint32_t>(r3.u64);
    const uint32_t phase = static_cast<uint32_t>(r4.u64);

    if (phase != kMirrorPhase) {
        if (PlausibleGuestPtr(item)) RestoreCarVisibility(base, item);
        return;
    }
    if (!larecomp::settings::rearview_cars.Get() || !PlausibleGuestPtr(item)) return;

    auto* rt = REX_RUNTIME();
    PPCFunc* prep = rt->FindFunction(kCarVisibilityPrep).value_or(nullptr);
    if (!prep) return;
    const int level_at_least = larecomp::settings::rearview_cars_lod.Get();
    SaveCarVisibility(base, item, level_at_least > 0);
    g_in_mirror_prep = true;
    larecomp::guest::Call<uint32_t>(prep, item);
    g_in_mirror_prep = false;

    const uint16_t visible =
        uint16_t((uint32_t(base[item + kCarVisibleWord]) << 8) | base[item + kCarVisibleWord + 1]);
    if (visible) {
        ++g_stats.cars_seen;
        // The racers the mirror may draw (rearview_cars_max: the first N the camera sees, in the game's list order) and the coarsest model they may
        // have (rearview_cars_lod: at least that level, never finer). A car turned away keeps the phase 0x1000000, for which the car draw draws nothing.
        if (g_car_budget.Allow(larecomp::settings::rearview_cars_max.Get())) {
            r4.u64 = 0x10000;
            ++g_stats.cars_drawn;
            if (level_at_least > 0) {
                const int chosen = int(int32_t(ReadGuestU32(base, item + kCarLodWord)));
                const int level = cheap::CarLevel(chosen, level_at_least);
                if (level != chosen) {
                    WriteGuestU32(base, item + kCarLodWord, uint32_t(level));
                    ++g_stats.cars_coarser;
                }
            }
        } else {
            ++g_stats.cars_turned_away;
        }
    }

    static std::atomic<uint64_t> calls{0};
    const uint64_t n = calls.fetch_add(1, std::memory_order_relaxed);
    if (larecomp::settings::rearview_diag.Get() && n % 23 == 0)
        LARECOMP_APP_INFO("[RearView] car 0x{:08X}: visible in the mirror={}", item, visible);
}

// Hooked at the entry of the mirror phase's begin (sub_82308178, called from the phase
// callback sub_82306500 for 0x1000000). Counts what each client holds at that moment:
// client+8 is its list of render types and each type's +20 its list of instances.
void Patch_RearViewDiag() {
    // The begin of the mirror's phase, whatever the diag setting says: the racers' budget starts again and the city's radius cut (rearview_lod_distance) is in
    // force until Patch_RearViewPhaseEnd.
    g_car_budget.Reset();
    g_in_mirror_phase = true;
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

// Hooked at 0x82308120, the `bl sub_8217D460` in sub_82307F28 (f1 = the camera's field of view, [pass+20]; f2 = the aspect, the stamp's w/h; f3 = the near plane
// and f4 = the far plane, the main camera's). sub_82307F28 is the mirror's camera step, run from the phase callback sub_82306500 at the colour pass's begin
// whenever the mirror is requested; sub_8217D460 stores the four into the camera (+696 .. +708) and builds its frustum, which is all the game's own culling of the
// mirror's phase has to go by. rearview_fov_scale narrows the field of view and rearview_far pulls the far plane in (rear_view_cheap.h); both leave the registers
// alone while off. The registers hold single floats as doubles.
//
// CHECK: that f1 is a field of view in radians that the projection halves (sub_8217CB68 multiplies it by a constant and takes its tangent), so a scale of it
// narrows the view roughly in proportion; the log's "camera builds" counts the calls and the line "[RearView] cheap camera" says what was applied.
void Patch_RearViewCamera(PPCRegister& f1, PPCRegister& f4) {
    const double fov_scale = larecomp::settings::rearview_fov_scale.Get();
    const double far_m = larecomp::settings::rearview_far.Get();
    if (!(fov_scale < 1.0) && !(far_m > 0.0)) return;
    ++g_stats.camera_builds;
    const float fov_was = float(f1.f64);
    const float far_was = float(f4.f64);
    const float fov_now = cheap::FieldOfView(fov_was, fov_scale);
    const float far_now = cheap::FarPlane(far_was, far_m);
    f1.f64 = double(fov_now);
    f4.f64 = double(far_now);
    static float logged_fov = -1.0f, logged_far = -1.0f;
    if (fov_now != logged_fov || far_now != logged_far) {
        logged_fov = fov_now;
        logged_far = far_now;
        LARECOMP_APP_INFO("[RearView] cheap camera: field of view {:.3f} -> {:.3f}, far plane {:.0f} -> {:.0f} m", fov_was, fov_now, far_was, far_now);
    }
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
// part of the phase runs; Patch_RearViewPhaseEnd puts +364 back. A camera cut (the eye moved more than 20 m) draws the mirror at once.
//
// What this does NOT do is keep the picture: the stamp's pixels are the main view's on a held frame (the host has no copy to put back yet), so the switch measures
// the saving, it does not show the finished feature. CHECK: that +364 is what the begin steps test and that nothing else re-derives the bit mid-frame.
void Patch_RearViewCadence(PPCRegister& r17, PPCRegister& r21, PPCRegister& r27) {
    g_in_mirror_phase = false;  // a new frame: the city's radius cut never carries over
    auto* base = larecomp::guest::Membase();
    if (!base) return;
    const uint32_t renderer = r27.u32;
    if (g_masked_renderer != 0) {  // the end hook did not run last frame: put the bit back before anything reads it
        const uint32_t now = ReadGuestU32(base, g_masked_renderer + kRendererPhaseMask);
        WriteGuestU32(base, g_masked_renderer + kRendererPhaseMask, now | kMirrorPhase);
        g_masked_renderer = 0;
    }
    if ((r17.u32 & kMirrorPhase) == 0 || !larecomp::settings::rearview.Get() || !MirrorConsumerActive(base)) {
        g_cadence.Reset();
        return;
    }
    float eye[3] = {0.0f, 0.0f, 0.0f};
    const bool have_eye = MainEye(base, renderer, eye);
    const bool cut = !have_eye || cheap::EyeJumped(g_last_eye, eye, g_have_last_eye);
    if (have_eye) {
        for (int i = 0; i < 3; ++i) g_last_eye[i] = eye[i];
    }
    g_have_last_eye = have_eye;

    const uint64_t cut_before = g_cadence.cut_draws;
    const bool draw = g_cadence.Decide(larecomp::settings::rearview_every.Get(), cut);
    ++g_stats.frames;
    g_stats.cut_draws += g_cadence.cut_draws - cut_before;
    if (!draw) ++g_stats.held;
    LogCheapState();
    if (draw) return;

    // A held frame: the phase is not asked for.
    r17.u64 &= ~uint64_t(kMirrorPhase);
    r21.u64 &= ~uint64_t(kMirrorPhase);
    const uint32_t mask = ReadGuestU32(base, renderer + kRendererPhaseMask);
    if (mask & kMirrorPhase) {
        WriteGuestU32(base, renderer + kRendererPhaseMask, mask & ~kMirrorPhase);
        g_masked_renderer = renderer;
    }
}

// Hooked at 0x822E68D8, the `mr r4,r30` before the phase's end call (sub_822F7570) in the renderer's phase loop: reached for every phase, r30 = the phase. For the
// mirror's it ends the city's radius cut and puts back the bit a held frame took out of renderer+364.
void Patch_RearViewPhaseEnd(PPCRegister& r30) {
    if (r30.u32 != kMirrorPhase) return;
    g_in_mirror_phase = false;
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
    if (larecomp::settings::rearview.Get()) return;
    if (r3.u32 & 0xFFu) {
        static std::atomic<uint32_t> blocked{0};
        const uint32_t n = blocked.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((n & (n - 1)) == 0)
            LARECOMP_APP_INFO("[RearView] mirror update saw the visible bit while off, answered hidden (#{})", n);
    }
    r3.u64 = 0;
}

void Patch_RearViewUpdateActive(PPCRegister& r3) {
    if (!larecomp::settings::rearview.Get()) r3.u64 = 0;
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
    if (!larecomp::settings::rearview.Get()) f13.f64 = 0.0;
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
void Patch_RearViewCamera(PPCRegister& f1, PPCRegister& f4) {}
void Patch_RearViewCityLod(PPCRegister& f1) {}
void Patch_RearViewCadence(PPCRegister& r17, PPCRegister& r21, PPCRegister& r27) {}
void Patch_RearViewPhaseEnd(PPCRegister& r30) {}
#endif // REXGLUE_HAS_XEO3_TARGET
