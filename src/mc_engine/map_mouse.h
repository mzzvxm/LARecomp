#pragma once
//
// Absolute mouse cursor on the full map screen.
//
// The map is not a 2D screen: sub_82625AC0 hands the NAVSYSMOVIE the matrix at
// screen+1056 (vfunc648) plus a look-at (vfunc636), so the movie is a plane
// placed in the world and the cursor clip is positioned in world space --
//
//     cur_cursor_Position[i] = screen[136 + i] + screen[112 + i]
//     cur_cursor_Menuscale   = (screen[97] * dist + 0.04) * screen[125]
//
// the Menuscale term being what keeps the clip the same size on screen no
// matter how far the camera is. Driving that from a mouse therefore means
// unprojecting the pointer onto the map plane, not moving a 2D sprite.
//
// Everything needed for the unprojection is already in the screen object,
// written every time the map moves (sub_82625F50):
//
//     +1056 / +1072 / +1088   camera basis rows
//     +1104                   camera position
//     +1124 / +1128           camera height min/max (the zoom range)
//     +1132                   distance camera -> target
//     +1488                   camera pitch, asin(camY / +1132)
//
// The one thing the screen does not carry is the vertical FOV of the movie's
// camera, so that is a cvar (map_mouse_fov) -- a single number, calibrated once
// by eye, stable across the whole game because zooming moves the camera rather
// than changing the FOV.
//
// The cursor itself is set through sub_826263C0, which is the same entry the
// analog stick path uses (sub_82629910 calls it). Going through it rather than
// writing +448 directly is what buys the bounds clamp at +572/+580/+576/+584,
// the Flash publish and the visibility handling for free, and guarantees a
// mouse-moved cursor is indistinguishable from a stick-moved one.
//

#include <rex/ppc/context.h>

// Attaches the window mouse listener. Safe to call before the window exists --
// the attach is retried from the tick until it takes.
void InitMapMouse();

// Per-frame housekeeping, driven from Patch_DeltaTimePre() in hooks.cpp. Only
// job is noticing that the map screen went away so the OS cursor can be put
// back; the cursor work itself happens in the hook below.
void TickMapMouse();

// True while the full map is on screen and the mouse is allowed to drive it --
// map_mouse on, the map's own input and cursor enables up, the screen active.
//
// Anything else that takes the pointer has to stand down on this. Free look
// re-centres the pointer every time it reads a delta, and with the map up that
// reads as the cursor being dragged to the middle of the screen; worse, the map
// never sees a clean motion, so it cannot claim the mouse and hand it back
// either. Deliberately NOT the same question as "is the mouse currently driving
// the map" -- by the time that is true it is already too late.
bool MapMouseWantsPointer();

// sub_8262A688 @0x8262A6AC -- the full map's input tick, entered with r31 = the
// map screen, f1 still holding the frame delta and the frame already set up.
// Runs before the game reads the pad, so a stick that is inside its deadzone
// leaves our position standing and a stick that is not simply takes over for
// that frame.
void Hook_MapInputTick(PPCRegister& r31, PPCRegister& f1);

// Suppresses the map's cursor magnet. Shared by two call sites, because the
// map snaps the cursor onto the selected icon from two directions:
//
//   sub_8262A688 @0x8262A90C   every frame something is selected --
//                              `if (viewStickIdle && sub_82387A18() &&
//                               screen[629] && screen[600])
//                                   sub_826263C0(screen, posOf(screen[600]))`
//   sub_826290C0 @0x826291F0   on the selection itself, right after
//                              sub_8261F970(screen, 1) raises it
//
// Together that is what makes a thumbstick feel sticky over waypoints, and it
// is exactly wrong for a pointer: the first icon the mouse brushes captures
// the cursor and no amount of mouse movement gets it back, because both snaps
// run after the hook that placed the cursor.
//
// Each site jumps past only its own call -- loc_8262A95C is the block's own
// skip target, the one its three guards already branch to when nothing is
// selected, and 0x826291F4 reloads r3 from the screen so no result is lost.
// The selection and the `iconisselected` highlight it drives are untouched, so
// hovering still works. Only the snap goes, and only while a mouse is the
// thing driving the cursor -- put it down and the stick gets its magnet back.
bool MCLA_MapCursorMagnetGuard();

// sub_82625368 @0x82625424 -- guard for a hole in the boundary sweep.
//
// sub_82624EC8(boundary, from, to, &node, &neighbour) always writes the node
// but only writes the neighbour on the paths that resolve which side of it the
// segment crosses. When neither sub_82621FF8 test hits, it returns having left
// the caller's pointer at the zero it was initialised to, and two instructions
// later sub_82625368 does `lvx v53, r0, r31` on it -- a read of guest 0.
//
// The stick never reaches it: it moves the cursor by velocity * dt, so every
// segment is short and starts from a position that is already on the boundary
// walk. A mouse jumps, and a long enough chord misses both neighbours.
//
// Steps small enough to look like the stick's are the real fix and are what
// map_mouse.cpp does; this exists so that the residual case degrades into an
// unclamped position for one frame instead of taking the process down.
bool MCLA_MapCursorSweepNullGuard(PPCRegister& r31, PPCRegister& r30,
                                  PPCRegister& r26);
