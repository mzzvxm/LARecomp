#pragma once
// Display gamma (hooks/display_gamma.cpp).

#include <cstdint>

#include <rex/ppc.h>

// Called at the start of the D3DDevice_Swap hook, on the thread that owns the device, before
// the original runs. If display_gamma changed since the game last built its ramp, it gives the
// device the ramp again; the Swap then sends it to the display with the frame.
void DisplayGammaOnSwap(PPCContext& ctx, uint8_t* base);
