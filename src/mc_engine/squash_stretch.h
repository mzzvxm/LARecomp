#pragma once

#include <rex/ppc/context.h>

// ─────────────────────────────────────────────────────────────────────────
// "Why's This Dealer?" squash & stretch (visual only).
// See squash_stretch.cpp for the reverse engineering notes.
// ─────────────────────────────────────────────────────────────────────────

// sub_82360518 entry (r3 = vehicle). Arms/disarms the effect per vehicle and
// resets the cached root matrix for this vehicle's render.
void Hook_SquashSelectVehicle(PPCRegister& r3);

// sub_823236C8 entry (r4 = bone matrix array, r6 = optional extra matrix).
// Injects the squash matrix into the engine's own "extra transform" slot.
void Hook_SquashInjectMatrix(PPCRegister& r4, PPCRegister& r6);

// Rigid (non-skinned) sink: sub_82322A10 entry and the two direct
// SetWorldMatrix call sites that submit without a palette — tyres and exhaust.
// r4 = Matrix44.
void Hook_SquashWorldMatrix(PPCRegister& r4);
