#pragma once
//
// Ultrawide: the shape of the picture the game is shown in.
//
// The game draws a 1280x720 screen. For a wider window the cameras are built with the window's
// aspect ratio (the guest's own camera code, through the hooks in hooks/graphics.cpp: the vertical
// field of view stays the 16:9 one, so the view widens, "Hor+"), the 3D picture is drawn into the
// same 1280x720 image squeezed sideways, and the present stretches that image over the whole window,
// which undoes the squeeze. What the game draws on top of the picture after the scene is composed
// (the HUD and every Flash menu) would be stretched with it, so the renderer draws those squeezed by
// the same factor around the centre of the screen: they come out 16:9-correct, in the middle
// (gpu/scene.cpp, UiSqueeze). A frame with no 3D scene in it (a movie, a loading screen, a menu on
// a flat background) is not squeezed or stretched at all: the present shows it 16:9 in the middle
// of the window with black on both sides, as it always did (gpu/present.h, display aspect).
//
// CHECK: that the four camera hooks and the constant at 0x8201E7EC are every place the game takes its
// 3D aspect ratio from (the rear-view mirror, garage, showroom, map and photo cameras especially); a
// camera that keeps 16:9 shows its picture stretched.
//
// The aspect ratio is the window's: it follows the window as it is resized, goes fullscreen or
// changes monitor, with nothing to set. It is never below 16:9 (a taller window keeps 16:9 and the
// black bars above and below) and never above 32:9 (a wider window has black bars at the sides).
// The setting `aspect_ratio` (hooks/graphics.cpp) is "auto" unless the player picks one.
//
// Threading: every function can be called from any thread.
//

#include <cstdint>

namespace mc::ultrawide {

// The game's own picture and the widest one built (a 32:9 monitor, 5120x1440 or 3840x1080).
inline constexpr double kBaseAspect = 16.0 / 9.0;
inline constexpr double kMaxAspect = 32.0 / 9.0;

// The window's size in pixels: from the app on every size change (UI thread), once the window
// exists. 0 in either is ignored (a minimised window keeps the last size).
void SetWindowSize(uint32_t width, uint32_t height);

// The aspect ratio the cameras are built with and the picture is shown at, between kBaseAspect and
// kMaxAspect; kBaseAspect when there is nothing to widen.
double DisplayAspect();

// DisplayAspect() is wider than 16:9.
inline bool Active() { return DisplayAspect() > kBaseAspect * 1.001; }

// What fraction of the screen's width the UI keeps (16:9 over the aspect ratio): 1 when Active() is
// false, 3/4 at 21:9 (2560x1080 is 0.75; 3440x1440 is 0.744), 1/2 at 32:9.
double UiWidthFraction();

// From InitHooks, once, before any guest code: reads the setting, follows its changes while the
// game runs and writes the aspect ratio into the constant the game's cameras load it from (guest
// memory). The camera hooks read DisplayAspect() themselves.
void Init();

// --- markers the game places on the picture itself ----------------------------------------------
//
// Two kinds of marker are placed from the game's own 3D view (the widened camera):
//
// 1. A Flash clip the game moves to a projected position (the floating gamertag above a car,
//    sub_82289530): its x is a 1280-wide stage coordinate taken from the widened camera's NDC,
//    x = 640 + 640 * ndc_x. The UI pass draws the stage squeezed around the middle by
//    k = UiWidthFraction(), so x ends up at 640 + (x - 640) * k in the image and the present's
//    stretch puts the clip nearer the middle than the car. The hook gives the game
//    x' = 640 + (x - 640) / k, which the squeeze brings back to x. (MarkerStageX.)
//
// 2. A world icon the game draws as a 3D movie with the camera's own view and projection while it
//    draws the UI (the queued icon list, sub_82236558: racer, checkpoint and place icons): its
//    position is already the widened camera's, so the renderer must not squeeze those draws (and
//    their depth test against the scene's depth needs the unmoved position). The two hooks around
//    the list's draw tell the renderer which draws they are (NoteWorldIcons, gpu/scene.h SetUiWorld).
//
// CHECK: both are from reading the guest code only; see ULTRAWIDE-TEST.md items 6 and 8.

// Half the width of the stage the game's Flash movies are laid out on (the game's screen is 1280).
inline constexpr float kStageHalfWidth = 640.0f;

// A Flash stage x the game worked out from the widened camera, as the game must be given it so
// that the squeezed UI draws the clip where the 3D point is (the same x when the picture is 16:9).
float MarkerStageX(float stage_x);

// The values of a composite record's bucket that mean "the draws that follow, until the end value,
// are world icons" to the renderer (Renderer::kComposite): a side channel on the guest-to-render
// thread record stream that needs no new record type. Real buckets are single bits (0x10000000).
inline constexpr uint32_t kUiWorldBegin = 0x7F000001u;
inline constexpr uint32_t kUiWorldEnd = 0x7F000002u;

// Around the guest's draw of the queued world icons (guest render thread). A no-op unless the
// aspect ratio is wider than 16:9 and the Plume renderer is on.
void NoteWorldIcons(bool begin);

}  // namespace mc::ultrawide
