#pragma once

void InitHooks();

#include <rex/ppc/context.h>
bool Patch_AspectRatio_82233EB4(PPCRegister& f0);
bool Patch_AspectRatio_82214BB8(PPCRegister& f10);
bool Patch_AspectRatio_822E5E68(PPCRegister& f12);
bool Patch_AspectRatio_8223E5E0(PPCRegister& f13);

bool Patch_DisableDoF();
void Patch_ScaleTrafficLOD(PPCRegister& f0);

void Patch_ScaleCityLOD(PPCRegister& f13);
void UpdateCityLODMemory();

void Patch_DeltaTimePre();
void Patch_DeltaTime(PPCRegister& r24);
void Patch_SingleTile(PPCRegister& r7, PPCRegister& r8, PPCRegister& r25, PPCRegister& r28);
bool Patch_EdramLimit(PPCRegister& r11);
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
