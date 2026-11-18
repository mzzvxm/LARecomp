#pragma once
//
// Free look on the gameplay chase camera.
//
// ── Why the shipped camera only stops at a few angles ───────────────────
//
// It does have a continuous lookaround. sub_82314838 parses it out of the
// camera tune ("camera/default" and friends, built by sub_82314C30):
//
//     +40  LookaroundDelayTime      +60  LookaroundPanLeft
//     +48  LookaroundLerpFactor     +64  LookaroundPanRight
//     +52  LookaroundApproachRate   +68  LookaroundLimitLeft
//     +56  LookaroundDecayRate      +72  LookaroundLimitRight
//                                   +76  LookaroundLimitUp
//                                   +80  LookaroundLimitDown
//
// What feeds it is not continuous. The control table carries CAM_PAN_LEFT,
// CAM_PAN_RIGHT, CAM_PAN_UP and CAM_PAN_DOWN as four separate digital actions,
// each worth one fixed step. The step eases toward a target clamped by the four
// Limit fields, and after DelayTime it decays back to centre. The angles the
// player sees are the limits; the travel between them is an interpolation
// rather than anything being steered.
//
// ── Two approaches, one of them measured to be wrong ────────────────────
//
// The first collapsed the Limit window onto a single value to pin the
// accumulator there. It held for about a second and then stopped, and the
// reason turned up later: the look is recomputed from the pad every frame.
// Kept behind cam_freelook_tune, off, because pinning the limits is still a
// reasonable thing to combine with the real fix.
//
// The second, which is what runs, came out of measuring rather than reading.
// The field probe below diffed the camera system and everything it points at
// while a look was held, and fourteen floats out of 6656 moved -- all in the
// object at camsys+860:
//
//     +96   right    unit vector
//     +112  up       unit vector
//     +128  forward  unit vector
//     +144  world position
//     +140  the look angle in radians   (w lane of the forward row)
//     +156  orbit distance, signed      (w lane of the position row)
//
// Writing an angle into +140 before the update was then measured too, by
// reading it back on the next frame: the game had put 0.0000 there every time,
// whatever went in. So +140 is a result the camera computes, not a setting it
// reads, and nothing written ahead of the update survives it.
//
// What does work is turning the finished matrix, at the end of the camera tick.
// The point it turns about is not a guess either -- in the sample the basis
// rotated -71.7 degrees about Y while the position moved 9.05 units, and
//
//     pos + forward * (the float at +156)
//
// lands on the same world point before and after to within half a unit. So the
// camera orbits `pos + forward * d`, and once the rotated forward is known the
// new position follows from it.
//
// This makes the feature independent of the game's look state entirely: it
// takes whatever chase matrix the camera produced and turns it.
//

#include <cstdint>

#include <rex/ppc/context.h>

// Called from MCLA_TuneFieldProbe for every tune field the game registers, hook
// or no hook, cvar or no cvar. Picks out the camera lookaround fields and
// remembers where they live.
void CameraLookOnTuneField(const char* name, uint32_t owner, uint32_t field_addr);

// Per-frame, from Patch_DeltaTimePre(). Reads the mouse and writes the tune.
void TickCameraLook();

// sub_822C0320 @0x822C0614 -- the camera system tick, with r20 = the camera
// system (set by `mr r20, r3` in its prologue) and r24 = the camera rig the
// system settled on this frame.
//
// Two earlier hooks were aimed at what looked like the gameplay camera and
// neither fired once, though both were emitted and verified in generated/:
// sub_82320298 ("mcPlayerCamera::Update", whose only cross-reference is a
// vtable slot) and sub_82316148 (the preset switcher). The whole 0x8231xxxx
// camera family looks dead in this build -- which also means the two
// camera-smoothing hooks living inside sub_82320298 have probably never done
// anything, and are worth re-checking.
//
// This function certainly runs: Patch_DebugCamGate and Patch_DebugCam are
// inside it and the free-fly camera works.
void Hook_CameraSystem(PPCRegister& r20, PPCRegister& r24);

// sub_822C0320 @0x822C0970 -- the camera tick's single epilogue, r20 still the
// camera system. Where the free look is actually applied.
//
// Writing an angle into the camera object BEFORE the update was measured and
// does not work: the read-back came out 0.0000 every frame no matter what went
// in, so the game recomputes the look from its own input and the field at +140
// is a result, not a setting. Turning the finished matrix is what is left, and
// it has the advantage of needing nothing from the game's look state at all.
void Hook_CameraPostUpdate(PPCRegister& r20);

// sub_822C0320 @0x822C0790 -- in the gameplay path, immediately before the
// `bl sub_8220AA68` at 0x822C07A4 that takes five stack out-params. r20 is
// still the camera system.
//
// Applying at the epilogue was tried and the write does land: the position
// reads back changed, and a yaw of nearly pi still moved nothing on screen.
// Something consumes the matrix before the function returns, and a call taking
// five out-params right after the update is the obvious candidate for it.
//
// cam_look_phase picks which of the two sites is live, so both can be tried
// without a rebuild.
void Hook_CameraPrePublish(PPCRegister& r20);

// sub_8220AA68 @0x8220AA88 -- r30 is the camera matrix itself, one instruction
// after `mr r30, r4` and before a single byte of it has been read. THE place
// to turn the view.
//
// Everything written to before this was a copy. sub_823CC2E8 turned out to be
// a plain setter:
//
//     obj+96..+156 = src[0..63];  *(float*)(obj+160) = fov;
//
// so the object at camsys+0x35C -- the one the field probe found, the only one
// in 25k floats whose basis rotates -- is written FROM this matrix for the cine
// director's benefit. Turning the mirror could never have moved the screen.
//
// Turning it here lets the game carry the rotation the rest of the way itself:
// sub_8220AA68 saves the matrix to dword_8286D804+4208, blends in whatever
// cutscene, spectator and shake work is due, and publishes to
// dword_8286D804+4272, unk_8286D900 and flt_8286D8FC.
void Hook_CameraMatrixPublish(PPCRegister& r30);

// sub_8231CE30 @0x8231CEB0 -- r31 is the camera state, one instruction after
// `bl sub_823228F0` and before the game resolves the camera against the world.
// The site that makes free look collide.
//
// sub_8231CE30 is the shared body of the camera-state update (mcTrackCS and
// vehTrackCS both reach it from vtable slot 10) and runs, in order:
//
//     sub_8231C4F8(cs)                        builds position and basis
//     sub_8231B368(cs)
//     sub_823228F0(cs)
//     if (cs+480) sub_8231B4E0(cs, old)
//     if (cs+464) sub_8231B740(cs, cs+64)     the camera collision
//
// sub_8231B740 sphere-tests the target with sub_82574CA0 and segment-tests
// target -> camera with sub_82574C38, both against the physics world at
// dword_828DA464, then rewrites the camera position as `target + dir * d` with
// d shortened to the hit. Its tune fields sit in the .rdata block beside the
// vtable: CollideType, CollideMinDist, CollideFudge, CollideHeight.
//
// Turning the camera before that call means the game's own collision resolves
// the rotated position. Turning the finished matrix at sub_8220AA68 --
// everything before this -- happens after the collision has run, which is
// exactly why free look went through walls and buildings.
//
// Fields, confirmed against sub_822AFA10 and sub_8231B740's own reads:
//
//     cs+16   right   cs+32 up   cs+48 forward   cs+64 position
//     cs+80   the same matrix again, as collided (cs+156 says which is live)
//     cs+156  collided-this-frame flag
//     cs+192  what is being followed; sub_8231CE30 early-outs when null
//     cs+320  the point the camera looks at and orbits, and the probe origin
//     cs+464  CollideType   cs+468 CollideFudge   cs+476 CollideMinDist
//     cs+672  the smoothed orbit distance the collision maintains
void Hook_CameraStateUpdate(PPCRegister& r31);

// ── Finding what the look actually moves ────────────────────────────────
//
// The Limit-pinning above is a hypothesis and behaves like one: it bites for a
// moment and then stops. Rather than guess again, this measures.
//
// Two buttons. `cam_probe_mark` snapshots the camera object; `cam_probe_dump`
// snapshots it again, diffs the two and writes every float that moved to
// <exe>/cam_fields.txt. Mark with the view centred and the car still, then hold
// a look, then dump: what comes out is the look's own state, with the noise of
// a moving car left out by not moving.
//
// It also reports mcPlayerCamera+784, the active camera preset index. That
// matters because sub_82316148 turns out to be a preset switcher -- it copies a
// 112-byte block out of a table at camera+800 into the live fields whenever the
// index changes, and CAMERA_LEFT / CAMERA_RIGHT / LOOKBACK are what change it.
// If that index moves while looking around, the "specific angles" are presets
// and no amount of lookaround tuning will unlock them; if it stays put, the
// angles are the lookaround's and the fields that moved are what to drive.

void CameraProbeMark();
void CameraProbeDump();

// The two above cannot be used for the measurement they exist for: reaching
// them means opening the settings overlay, and opening it takes the game out
// of the camera state being held. This arms both on a timer instead, so the
// overlay is only open while the camera is idle and both samples land with it
// shut. The log counts down, since there is nothing on screen to watch.
void CameraProbeRun();

// ── The DEBUG CAMERA rows ──────────────────────────────────────────────
//
// The pause menu's DEBUG CAMERA tab is filled out from two places: the rows
// pause_menu.cpp owns (the shipped debug camera and FOV switches) and these,
// which belong to the free look and therefore live next to it. Returns a table
// the menu appends to the end of that tab; `count` comes back as its length,
// and 0 with a null pointer is a valid answer.
namespace mcla_menu { struct ItemDef; }
const mcla_menu::ItemDef* CameraLookMenuItems(int& count);
