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

}  // namespace mc::ultrawide
