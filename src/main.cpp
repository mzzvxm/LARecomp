#ifndef REXGLUE_HAS_XEO3_TARGET
// larecomp - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#include "generated/larecomp_init.h"

#include "larecomp_app.h"

#include <rex/cvar.h>

#include <cmath>
#include <cstdint>

#if defined(_WIN32)
// Direct3D 12 Agility SDK exports.
// The D3D12 runtime checks these exported symbols in the main executable.
// 619 corresponds to D3D12Core.dll version 1.619.3.0 shipped in assets/D3D12.
extern "C" {
__declspec(dllexport) extern const uint32_t D3D12SDKVersion = 619;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}
#endif

uint8_t* g_guest_mem = nullptr;

extern "C" float roundevenf(float x) {
    return std::nearbyint(x);
}

extern "C" double roundeven(double x) {
    return std::nearbyint(x);
}

REX_DEFINE_APP(larecomp, LarecompApp::Create)
#endif // REXGLUE_HAS_XEO3_TARGET
