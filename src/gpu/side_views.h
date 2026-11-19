#pragma once
// ===========================================================================
// MCLA GPU renderer -- the side views: render passes of the game that feed the picture
// ===========================================================================
// Besides the picture itself the game draws three small views every frame, each into a target of
// its own size and each read back, through a resolve, by the lit shaders of the picture
// (feature-parity/GFX-UPGRADES-DESIGN.md, sections 1 and 6A):
//
//   - the sun's shadow cascades: four 640x640 depth views with 4 samples, resolved with one sample
//     each into the quadrants of a 1280x1280 atlas that the shadow collector looks up;
//   - the environment map: a 512x256 dual paraboloid view, with its mip levels made by three small
//     passes of the game, which the car paint reads;
//   - the road reflection: a 256x256 image of the whole screen drawn with mirrored geometry, which
//     every reflective city material reads with its screen position (roads, windows, water);
//
// and the HUD's own small render-to-texture views (the minimap's 220x220 picture and the 128x128 and 256x256 ones beside it, side_view_ui_map.cpp), the
// class that follows the render scale instead of a setting's level (SideViewRule::follows_output).
//
// None of them is "an image of the screen" (their sizes are not the screen's halvings), so at the
// render scale they stay at the guest's size, and a product made at another scale than its reader
// is reduced to the reader's (ResolveCache::ReducedSlot; more than one level: no texture at all).
// So the render scale could not make them sharper, and a larger one could not even be read.
//
// A SIDE VIEW CLASS is the set of EDRAM identities (colour and depth keys) of one such view, with
// a scale of its own and a host sample count of its own, both chosen by a setting and moved live:
//
//   - EdramTargets makes a member at its class's scale (ScaleForUse; SetSideScales moves them all
//     at a frame boundary, the same all-or-nothing step as SetScales) and with the class's host
//     samples (HostSamples). The viewport, scissor, clears and the resolve's rectangles follow
//     from the target's scale as for the screen's images (Scale::Edge). One scale for the whole
//     class: a draw with colour and depth needs both at the same scale and samples.
//   - A resolve product whose source is a member is READ AT ITS OWN SCALE (Product::own_scale):
//     the texture cache binds it as it is, whatever the reader's scale, with every level it has
//     (no reduced copy, no null texture for a product with mips). The fetches of these views are
//     UV lookups with the guest's own sampler, which need nothing else.
//   - The classes are told apart by identity alone (a key and, where one is known, the declared
//     size of the surface that binds it), and each class has one rule file of its own, so the
//     packages that follow share no file: side_view_reflection.cpp (the road reflection),
//     side_view_shadow.cpp (the cascades) and side_view_env.cpp (the environment map). A rule
//     says which identities are members, which setting chooses the scale and what `auto` is on a
//     weak, a middle and a strong GPU, and how many host samples a member has at a scale.
//
// ONE IDENTITY, TWO USES. The game reuses the EDRAM: a key of a class is also the key of another
// pass whose surface is declared another size. The night shadow (dusk and night: colour k_8_8_8_8
// at tile 832, declared 512x512) draws with the depth identity kD24FS8 at tile 0, pitch 520, 4
// samples, which is the environment map's depth (declared 512x256). A host target is made per
// identity, so that depth was the class's (2x) while the night shadow's colour, a key no class has,
// was at 1x: a framebuffer cannot mix them, and every draw of the pass was left out. The class
// owns an identity only for the uses that are its own (the declared size says which); another use
// of the key is a FOREIGN USE and gets a host target of its own (EdramKey::reserved =
// kForeignUseLayer, ForeignUse below), at the scale the rest of that use has: the guest only
// keeps what a pass writes until the next pass reuses the tiles, so two host targets for the one
// tile range lose nothing (the pause view's UI depth is the same kind of split, DepthLayer).
//
// A class's settings read `auto | game | high | ultra`: game = the guest's size (1x), high = 2x,
// ultra = 3x; `auto` is the cell of the GPU class table (gpu_class.h, GFX-UPGRADES-DESIGN.md 9).
// A class at `game` changes nothing about its targets (the guest's size and samples, the same
// pipelines): the picture is the game's own.
//
// Threading: the classification is pure; everything else is the render thread's.
// ===========================================================================

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "gpu_class.h"
#include "scale.h"

namespace mcla::gpu {

struct EdramKey;
class Device;

enum class SideView : uint8_t {
  kNone = 0,
  kShadow,      // the sun's cascades                  side_view_shadow.cpp
  kEnv,         // the environment map (mips included) side_view_env.cpp
  kReflection,  // the road reflection                 side_view_reflection.cpp
  kUiMap,       // the HUD's small views: minimap...   side_view_ui_map.cpp (follows the render scale)
  kCount,
};
inline constexpr size_t kSideViewCount = size_t(SideView::kCount) - 1;
inline constexpr size_t SideIndex(SideView view) {
  return size_t(view) - 1;
}
// Every class, in the order the loops of the registry and of EdramTargets visit them.
inline constexpr std::array<SideView, kSideViewCount> kAllSideViews = {SideView::kShadow, SideView::kEnv, SideView::kReflection, SideView::kUiMap};

// The scale each class is made at. 1x each (the guest's size) until a setting says otherwise.
struct SideScales {
  std::array<Scale, kSideViewCount> scale{};
  Scale operator[](SideView view) const { return scale[SideIndex(view)]; }
  Scale& operator[](SideView view) { return scale[SideIndex(view)]; }
  bool operator==(const SideScales&) const = default;
};

// The value of a class's setting once `auto` is resolved.
enum class SideLevel : uint8_t { kGame, kHigh, kUltra };
inline constexpr Scale ScaleOfLevel(SideLevel level) {
  return level == SideLevel::kUltra ? Scale::Whole(3)
         : level == SideLevel::kHigh ? Scale::Whole(2)
                                     : Scale{};
}
inline constexpr const char* LevelText(SideLevel level) {
  return level == SideLevel::kUltra ? "ultra" : level == SideLevel::kHigh ? "high" : "game";
}

// One class's rule. Each rule file defines exactly one (extern below); the registry in
// side_views.cpp lists them. A rule's functions are pure except `setting`, which reads its cvar.
struct SideViewRule {
  SideView view = SideView::kNone;
  // For logs and the menu: "road reflection".
  const char* name = "";
  // Whether the EDRAM identity `key` belongs to the class. `size_bits`: the declared size of the
  // surface that binds it (D3DSurface SizeBits: width - 1 in bits 31..18, height - 1 in 17..3), 0
  // when it is not known; a rule that wants the size accepts an unknown one when the key alone is
  // distinctive enough. Must give the same answer for a key whenever it is asked.
  bool (*matches)(const EdramKey& key, uint32_t size_bits) = nullptr;
  // The class's setting as typed: auto, game, high or ultra (any case).
  std::string (*setting)() = nullptr;
  // The class's row of the `auto` table (gpu_class.h): what `auto` stands for on a Low, a Mid and a
  // High GPU, and the label of the pause row.
  gpu_class::Effect effect = gpu_class::Effect::kReflectionResolution;
  // A test scale (a debug cvar) that replaces the setting's when it is set: Scale::None() when not.
  Scale (*test_scale)() = nullptr;
  // The host samples of a member made at `scale` (never none), the guest drawing it with
  // `guest_samples`. A class that is at the guest's size must say the guest's.
  uint32_t (*host_samples)(Scale scale, uint32_t guest_samples) = nullptr;
  // A class that has no level of its own: it is at the SCREEN's scale (the render scale the images of the screen are made at, never below the guest's
  // size) while `enabled` says so, and at the guest's size otherwise. `setting` and `test_scale` are not asked for it, and it has no `auto` row.
  bool follows_output = false;
  bool (*enabled)() = nullptr;
};

extern const SideViewRule kShadowRule;
extern const SideViewRule kEnvRule;
extern const SideViewRule kReflectionRule;
extern const SideViewRule kUiMapRule;

namespace side_views {

// The class an EDRAM identity belongs to (kNone: none), as a use of it declared `size_bits`.
SideView Classify(const EdramKey& key, uint32_t size_bits);

// The layer (EdramKey::reserved) of a foreign use of a class's identity (see ONE IDENTITY, TWO
// USES): 2, after the main view's depth (0 the scene's, 1 the UI's, EdramTargets::DepthLayer).
inline constexpr uint8_t kForeignUseLayer = 2;

// Whether a use is a foreign use of a class's identity: the key alone names a class (`by_key`:
// Classify with the size left out), the use with its declared size `size_bits` (known: not 0) does
// not (`by_use`: kNone). A use whose size is not known is the class's, as Classify takes it.
inline constexpr bool ForeignUse(SideView by_key, SideView by_use, uint32_t size_bits) {
  return size_bits != 0 && by_key != SideView::kNone && by_use == SideView::kNone;
}
// ForeignUse for an identity (its layer 0 form: the rules match no other) used through a surface
// declared `size_bits`.
bool IsForeignUse(const EdramKey& key, uint32_t size_bits);

const SideViewRule& RuleOf(SideView view);
const char* Name(SideView view);

// The scale every class is asked to be at now (the settings, `auto` resolved, a test scale over
// them; a class that follows the output at `output`, the screen's scale, when it is enabled). Reads cvars: the
// render thread, once a frame.
SideScales Wanted(Scale output);
// `side` with the classes that follow the output moved to `output` (or back to the guest's size when they are not enabled): what a change of the render
// scale makes of the scales the classes stand at, so that it is one step with the images of the screen.
SideScales FollowOutput(const SideScales& side, Scale output);
// The screen's scale a class that follows it must reach before it leaves the guest's size, in twentieths: 1.25x, the render scale from which the UI
// sharpening calls a bitmap magnified (ui_sharp.h kMinMagnification; side_view_ui_map.cpp asserts they agree). Round 2 took any scale above 1x; at 1.05x
// a 220 pixel view made 231 pixels wide buys nothing and costs a remade target, and at 720p (render scale 1x) the class never moves, so that picture is the
// game's own whatever the setting says.
inline constexpr uint32_t kFollowMinScale = 25;
// The scale a class that follows the output is made at for a screen scale of `output`: the screen's when that is at least kFollowMinScale, else the
// guest's size.
inline Scale OutputScale(Scale output) { return output.n >= kFollowMinScale ? output : Scale{}; }
// A class's setting resolved, and its text for the pause menu's row ("AUTO (HIGH)").
SideLevel LevelOf(SideView view);
std::string AutoLabel(SideView view);
// The setting as typed, lower case, for the logs.
std::string SettingText(SideView view);

// The host samples of a member of `view` at `scale`.
uint32_t HostSamples(SideView view, Scale scale, uint32_t guest_samples);

// The next scale below `scale` (for a class that cannot be made: video memory), 1x at the lowest.
Scale StepDown(Scale scale);

// A declared size (SizeBits) is `width` x `height`.
inline bool DeclaredSize(uint32_t size_bits, uint32_t width, uint32_t height) {
  return (size_bits >> 18) + 1 == width && ((size_bits >> 3) & 0x7FFFu) + 1 == height;
}

}  // namespace side_views
}  // namespace mcla::gpu
