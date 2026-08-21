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
void Patch_SingleTile(PPCRegister& r7, PPCRegister& r8, PPCRegister& r25, PPCRegister& r28);
bool Patch_EdramLimit(PPCRegister& r11);
bool Patch_DebugCamGate();
void Patch_DebugCam(PPCRegister& r3);

void Patch_BypassVehicleDLC(PPCRegister& r30);
void Patch_DevOptionRegistered(PPCRegister& r3);
bool Patch_ImpostorShadowGuard(PPCRegister& r3);

// NOTE: the online hooks (tournament, System Link content gate, Xbox LIVE) moved
// out of hooks.cpp into src/mc_engine/online/. The recompiler declares each
// midasm hook itself, so they need no declaration here.

// Forwards the player's current district index (r3 = return of mc::LookupDistrict
// via Racer_GetCurrentDistrict, hooked after the bl at 0x822ADEE0) to the Discord
// RPC, which updates the presence live while driving. Fires on the game's own
// district queries, so no separate tick is needed.
void Hook_CaptureDistrict(PPCRegister& r3);

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

// LZX streaming decompression probe (lzx_stats cvar): brackets the
// XMemDecompressStream call inside zlibInflater::InflateBegin (sub_821D5E10).
void Hook_LzxDecompressPre(PPCRegister& r1);
void Hook_LzxDecompressPost(PPCRegister& r1, PPCRegister& r3);

// Vinyl importer/exporter: cache the current car's vinyl regen args at the
// sub_8236D850 entry (r3=work-area, r4=paint, r5=tex, r6=player index) so the
// export/import can reach the layer block and replay a regen.
void Hook_CacheVinylPaint(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6);
void Hook_PhotoModeCapture(PPCRegister& r3);

// BadassBaboon's Recomp Adjustments: Continuous-time exponential camera smoothing & suspension damping
void MCLACameraPosSmoothing(PPCRegister& f13);
void MCLACameraLookAtSmoothing(PPCRegister& f0);
void MCLAChassisDepthSmoothing(PPCRegister& f0);
// Ambient density. Fires after the density_tuning.xml parse (hooking the
// constructor is pointless -- the parse overwrites it), once per ambient zone;
// r31 is the zone, whose base IS the mcAmbientDensityTuning.
void MCLAAmbientDensityTuning(PPCRegister& r3);

// Traffic (va_) vehicles used as player cars: the chassis-bound substitution. See
// hooks.cpp for the full story; the register pairs are (root-carrying reg, child reg).
void MCLA_TrafficChassisBound_8232D048(PPCRegister& r9, PPCRegister& r11);
void MCLA_TrafficChassisBound_8232D900(PPCRegister& r3, PPCRegister& r11);
void MCLA_TrafficChassisBound_8232E274(PPCRegister& r3, PPCRegister& r31);
// Diagnostic (tune_field_probe cvar): logs every tune field as it registers.
void MCLA_TuneFieldProbe(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6);
// Entry guard for phBoundComposite::ReleaseChildren; true skips the whole loop.
bool MCLA_TrafficBoundRelease_8259AA40(PPCRegister& r30);

// BadassBaboon's Recomp Adjustments: Foliage imposter shadows
bool Patch_DisableImposterShadows(PPCRegister& r11);

// BadassBaboon's Recomp Adjustments: Core 60 FPS clock delta pipeline
void MCLAFrameDelta(PPCRegister& r8);
void MCLA_GuestInterruptProbe(PPCRegister& r3, PPCRegister& r31);
// The two fixed-timestep substitution paths in sub_821BDA90. MCLAUseRealDelta
// jumps over loc_821BDB58 (0x821BDB58 -> loc_821BDC34) when real_frame_delta is on;
// MCLAFixedStepPath rewrites f11 on the loc_821BDB90 path, which that jump does
// not cover.
bool MCLAUseRealDelta();
void MCLAFixedStepPath(PPCRegister& r3, PPCRegister& f11);

inline double fpsCount;
inline bool showfps;
