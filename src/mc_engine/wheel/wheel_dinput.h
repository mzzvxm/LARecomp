#pragma once
//
// A wheel's axes, buttons and hat read straight through DirectInput, for the wheels SDL's own DirectInput
// driver gets nothing out of (a Fanatec showed every axis at exactly 0 inside the game while the setup, which
// reads the same way, saw it fine). Non-exclusive and in the background, so it sits next to the exclusive
// force-feedback handle in wheel_ffb.cpp. The axes come in DirectInput's own order (X, Y, Z, Rx, Ry, Rz, two
// sliders), which is the order SDL numbers them in, scaled to -32768..32767 like SDL does.
//

#include <cstdint>

namespace larecomp::wheel::dinput {

struct Pad;

// Opens every DirectInput device listed for vendor:product, logs what each step returned, and keeps the first
// one whose state is not all zero (live = true) or else the first that opened at all (live = false). nullptr
// when nothing opens.
Pad* Open(uint16_t vid, uint16_t pid, bool* live);

void Poll(Pad* p);  // reads the state; call once per tick before Axis / Button / Hat
int Axis(const Pad* p, int index);     // -32768..32767, 0 for an axis the data format does not carry
bool Button(const Pad* p, int index);  // 0..31
int Hat(const Pad* p, int index);      // SDL_HAT_* bits (1 up, 2 right, 4 down, 8 left); 0 centred
void Close(Pad* p);

}  // namespace larecomp::wheel::dinput
