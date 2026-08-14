#pragma once

void InitHooks();

#include <rex/ppc/context.h>
bool Patch_AspectRatio_82233EB4(PPCRegister& f0);
bool Patch_AspectRatio_82214BB8(PPCRegister& f10);
bool Patch_AspectRatio_822E5E68(PPCRegister& f12);
bool Patch_AspectRatio_8223E5E0(PPCRegister& f13);

bool Patch_DisableDoF();
void Patch_DofComposite(PPCRegister& r3);

void Patch_ScaleTrafficLOD(PPCRegister& f0);

// Removes ride height from the wheel-fit validator sub_82392F68 so the shop can
// reach the full stock table (rh+300 .. rh_800, i.e. +3 .. -8). See hooks.cpp.
void Patch_RideHeightFit(PPCRegister& f30);

// Skips the whole sub_82392F68 fit test, unlocking every stock rim size, tire
// profile, tire width and ride height. Supersedes Patch_RideHeightFit.
bool Patch_WheelFitBypass();
void Patch_ScaleCityLOD(PPCRegister& f13);
void UpdateCityLODMemory();

void Patch_FOVScale(PPCRegister& f1, PPCRegister& r24);

void Patch_DeltaTimePre();
void Patch_DeltaTime(PPCRegister& r24);
bool Hook_IntroHalfRate();
void Patch_SingleTile(PPCRegister& r7, PPCRegister& r8, PPCRegister& r25, PPCRegister& r28);
bool Patch_EdramLimit(PPCRegister& r11);
bool Patch_DebugCamGate();
void Patch_DebugCam(PPCRegister& r3);

bool Patch_SpeedUnits(PPCRegister& r11);

// Button prompt glyphs (button_prompts cvar). The UI movies carry both the 360
// and the PS3 art and choose with the mcRegistry int "platform"; these own that
// flag. Hook_PlatformVarInit captures the registry entry as mcUIManager builds
// it, Patch_PlatformPush feeds the per-movie push in sub_821F8038, and
// TickButtonPrompts (called from Patch_DeltaTimePre) applies live cvar changes.
void TickButtonPrompts();
void Hook_PlatformVarInit(PPCRegister& r27);
bool Patch_PlatformPush(PPCRegister& r5);
// AVM action-buffer entry (sub_825EE970, r3 = swf context). Pushes
// _global.platform into each live movie so the glyph set can change without a
// reboot — the movies only read the registry value on their own frame 1.
void Hook_SwfContextEnter(PPCRegister& r3);

// BadassBaboon's Recomp Adjustments: Continuous-time exponential camera boom smoothing & ambient density tuning
void MCLACameraBoomSmoothing(PPCRegister& f1);
// Ambient density. Fires after the density_tuning.xml parse (hooking the
// constructor is pointless -- the parse overwrites it), once per ambient zone;
// r31 is the zone, whose base IS the mcAmbientDensityTuning.
void MCLAAmbientDensityTuning(PPCRegister& r31);

// BadassBaboon's Recomp Adjustments: Foliage imposter shadows and steering sensitivity
bool Patch_DisableImposterShadows(PPCRegister& r11);
void Patch_SteeringSensitivity(PPCRegister& f0);

// BadassBaboon's Recomp Adjustments: Core 60 FPS clock delta pipeline
void MCLAFrameDelta(PPCRegister& r8);
// The two fixed-timestep substitution paths in sub_821BDA90. MCLAUseRealDelta
// jumps over loc_821BDB58 (0x821BDB58 -> loc_821BDC34) when fps_60 is on;
// MCLAFixedStepPath rewrites f11 on the loc_821BDB90 path, which that jump does
// not cover.
bool MCLAUseRealDelta();
void MCLAFixedStepPath(PPCRegister& r3, PPCRegister& f11);

inline double fpsCount;
inline bool showfps;
