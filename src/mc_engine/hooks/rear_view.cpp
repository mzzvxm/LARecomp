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
//   alpha ramps up and the 0x1000000 phase is requested. Hidden: alpha ramps
//   down, the phase is dropped and the consumer's active byte is cleared, so a
//   hidden mirror costs nothing and leaves no black strip behind.
//
// The visible bit is not ours: sub_82652860 copies the form's own visibility into
// the child (vtbl+64) whenever the HUD is shown or hidden. So off is re-asserted
// every frame, and on mirrors the form's bit instead of remembering one.
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

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>

#include "hooks.h"
#include "hooks_internal.h"
#include "larecomp_log.h"

REXCVAR_DEFINE_BOOL(rearview, false, "MCLA/HUD",
    "Show the rear view mirror at the top right of the screen (a feature the retail game "
    "ships switched off). Takes effect immediately.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(rearview_position, "stock", "MCLA/HUD",
    "Where the mirror sits: stock (under the race position, top right), left_top, middle_top, "
    "left_down, middle_footer or right_down.")
    .allowed({"stock", "left_top", "middle_top", "left_down", "middle_footer", "right_down"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(rearview_frame, true, "MCLA/HUD",
    "Draw the mirror's frame. The retail game ships the frame meshes without their textures "
    "(black_small.dds and rearView.dds are gone), so the frame comes out solid white. Off hides "
    "it and leaves just the mirror view. Takes effect immediately.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(rearview_cars, false, "MCLA/HUD",
    "Show the racers' cars in the mirror. Retail's mirror phase leaves them out, and the game only "
    "treats a car as visible when the main camera sees it, so this runs the car's visibility test "
    "again with the mirror's camera and draws it with the colour pass. Don't combine it with "
    "rearview_traffic: traffic draws the same cars as black silhouettes. Needs rearview on.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(rearview_traffic, false, "MCLA/HUD",
    "Put ambient traffic back in the mirror's phase, where retail has it. It draws nothing useful "
    "there (cars show up as black silhouettes, or not at all) and a car close to the mirror's "
    "camera blacks the view out, so it is taken out of the phase unless this is on. Needs rearview on.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(rearview_diag, false, "MCLA/HUD",
    "Log, every couple of seconds, how many objects each render client holds when the mirror's "
    "phase begins. For working out why cars and traffic do not draw in it.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(rearview_shift_x, 0, "MCLA/HUD",
    "Fine-tunes the mirror and its frame horizontally on top of rearview_position, in 1280x720 "
    "pixels. Negative is left.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(rearview_shift_y, 0, "MCLA/HUD",
    "Fine-tunes the mirror and its frame vertically on top of rearview_position, in 1280x720 "
    "pixels. Positive is down.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(rearview_window_dx, 22, "MCLA/HUD",
    "Offset of the scene viewport from the frame window's outer corner, horizontally, in pixels. "
    "The window's left and right edges lean, so the viewport rectangle has to start inside the "
    "narrowest part or its corner pokes out of the frame.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(rearview_window_dy, 2, "MCLA/HUD",
    "Offset of the scene viewport from the frame window's outer corner, vertically, in pixels.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(rearview_window_w, 229, "MCLA/HUD",
    "Width of the scene viewport in pixels. The widest rectangle that fits inside the leaning "
    "window is 229 wide.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(rearview_window_h, 44, "MCLA/HUD",
    "Height of the scene viewport in pixels. The widest rectangle that fits inside the leaning "
    "window is 44 tall.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

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
    const std::string name = REXCVAR_GET(rearview_position);
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

static void ApplyMirrorMembership(uint8_t* base, bool mirror_on) {
    const uint32_t renderer = ReadGuestU32(base, kRendererPtr);
    if (!PlausibleGuestPtr(renderer)) return;

    static constexpr MirrorClient kCars{7, 0x12E183F3u, "cars"};
    static constexpr MirrorClient kTraffic{9, 0x00118002u, "traffic"};

    SetClientMirrorPhase(base, renderer, kCars, mirror_on && REXCVAR_GET(rearview_cars));
    SetClientMirrorPhase(base, renderer, kTraffic, mirror_on && REXCVAR_GET(rearview_traffic));
}

void TickRearView() {
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    uint32_t form = 0, widget = 0;
    if (!FindRearView(base, form, widget)) return;

    const bool want = REXCVAR_GET(rearview);
    ApplyMirrorMembership(base, want);
    const uint32_t widget_flags = ReadGuestU32(base, widget + kWidgetFlags);

    static int last_state = -1;  // -1 unknown, 0 hidden, 1 shown
    if (!want) {
        if (widget_flags & kWidgetVisible)
            WriteGuestU32(base, widget + kWidgetFlags, widget_flags & ~kWidgetVisible);
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
    const int shift_x = preset.x + REXCVAR_GET(rearview_shift_x);
    const int shift_y = preset.y + REXCVAR_GET(rearview_shift_y);

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
        const int w = REXCVAR_GET(rearview_window_w);
        const int h = REXCVAR_GET(rearview_window_h);
        const int x = kWindowX + shift_x + REXCVAR_GET(rearview_window_dx);
        const int y = kWindowY + shift_y + REXCVAR_GET(rearview_window_dy);
        if (w > 0 && h > 0) {
            const uint32_t want_rect[4] = {uint32_t(x), uint32_t(y), uint32_t(w), uint32_t(h)};
            for (uint32_t i = 0; i < 4; ++i) {
                if (ReadGuestU32(base, pass + 4u * i) != want_rect[i])
                    WriteGuestU32(base, pass + 4u * i, want_rect[i]);
            }
        }
    }

    if (last_state != 1) {
        LARECOMP_APP_INFO("[RearView] shown: viewport {},{} {}x{}, frame x offset {}",
                          kWindowX + shift_x + REXCVAR_GET(rearview_window_dx),
                          kWindowY + shift_y + REXCVAR_GET(rearview_window_dy),
                          REXCVAR_GET(rearview_window_w), REXCVAR_GET(rearview_window_h), frame_x);
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
};
static SavedVisibility g_saved_visibility[32];

static void RestoreCarVisibility(uint8_t* base, uint32_t item) {
    for (auto& s : g_saved_visibility) {
        if (s.item != item) continue;
        base[item + kCarVisibleWord]     = uint8_t(s.word >> 8);
        base[item + kCarVisibleWord + 1] = uint8_t(s.word);
        s.item = 0;
        return;
    }
}

static void SaveCarVisibility(uint8_t* base, uint32_t item) {
    SavedVisibility* slot = nullptr;
    for (auto& s : g_saved_visibility) {
        if (s.item == item) return;  // already holding the main view's value
        if (!slot && s.item == 0) slot = &s;
    }
    if (!slot) return;
    slot->item = item;
    slot->word = uint16_t((uint32_t(base[item + kCarVisibleWord]) << 8) | base[item + kCarVisibleWord + 1]);
}

void Patch_RearViewCarPhase(PPCRegister& r3, PPCRegister& r4) {
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    const uint32_t item = static_cast<uint32_t>(r3.u64);
    const uint32_t phase = static_cast<uint32_t>(r4.u64);

    if (phase != kMirrorPhase) {
        if (PlausibleGuestPtr(item)) RestoreCarVisibility(base, item);
        return;
    }
    if (!REXCVAR_GET(rearview_cars) || !PlausibleGuestPtr(item)) return;

    auto* rt = rex::Runtime::instance();
    PPCFunc* prep = rt->function_dispatcher() ? rt->function_dispatcher()->GetFunction(kCarVisibilityPrep) : nullptr;
    if (!prep) return;
    SaveCarVisibility(base, item);
    g_in_mirror_prep = true;
    rex::ppc::GuestToHostFunction<uint32_t>(prep, item);
    g_in_mirror_prep = false;

    const uint16_t visible =
        uint16_t((uint32_t(base[item + kCarVisibleWord]) << 8) | base[item + kCarVisibleWord + 1]);
    if (visible) r4.u64 = 0x10000;

    static std::atomic<uint64_t> calls{0};
    const uint64_t n = calls.fetch_add(1, std::memory_order_relaxed);
    if (REXCVAR_GET(rearview_diag) && n % 23 == 0)
        LARECOMP_APP_INFO("[RearView] car 0x{:08X}: visible in the mirror={}", item, visible);
}

// Hooked at the entry of the mirror phase's begin (sub_82308178, called from the phase
// callback sub_82306500 for 0x1000000). Counts what each client holds at that moment:
// client+8 is its list of render types and each type's +20 its list of instances.
void Patch_RearViewDiag() {
    if (!REXCVAR_GET(rearview_diag)) return;
    static std::atomic<uint32_t> calls{0};
    if (calls.fetch_add(1, std::memory_order_relaxed) % 120 != 0) return;
    auto* base = rex::Runtime::instance()->virtual_membase();
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

// Hooked at 0x822340F4 in mcRearView::Draw (sub_82233E18): the `lwzx` that loads the
// frame mesh just before its vtbl+32 draw call. True jumps to 0x82234108, past the
// draw, so the frame is not drawn; the mirror's scene viewport is a separate pass
// and is untouched.
bool Patch_RearViewFrameSkip() {
    return !REXCVAR_GET(rearview_frame);
}

#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

void TickRearView() {}
bool Patch_RearViewFrameSkip() { return false; }
void Patch_RearViewCarPhase(PPCRegister& r3, PPCRegister& r4) {}
void Patch_RearViewDiag() {}
void Patch_RearViewCullBits(PPCRegister& r3) {}
#endif // REXGLUE_HAS_XEO3_TARGET
