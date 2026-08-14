#pragma once

#include <rex/ppc/context.h>

// ─────────────────────────────────────────────────────────────────────────
// Tire offset (per-car wheel track adjustment).
// See tire_offset.cpp for the reverse engineering notes; the garage row that
// edits the value lives in pause_menu.cpp.
// ─────────────────────────────────────────────────────────────────────────

// sub_82354230 epilogue (0x82354A1C, r31 = mcCarModel). Runs after the whole
// per-wheel matrix set has been rebuilt for this frame and shifts it sideways
// by the car's TireOffset0/1 bytes.
void Hook_TireOffsetWheels(PPCRegister& r31);
