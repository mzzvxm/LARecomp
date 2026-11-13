#pragma once
// ===========================================================================
// MCLA GPU renderer -- the sun shafts setting, for the pause menu (shafts.h)
// ===========================================================================
// What mcla_gpu_sun_shafts = auto comes to on this graphics card, and the row's text for it, without
// the renderer's types (the pause menu includes this header, as it includes latency.h). Both answers
// are the renderer's: they are known once the device exists and "off" before it.
//
// CHECK: GT -- the class of the graphics card and the table of what each effect's `auto` is belong
// to package GT (src/gpu/gpu_class.h: Resolve(Effect, value), AutoLabel(Effect)). Until it is
// merged shafts.cpp keeps a small stand-in of its own (a UMA GPU is Low, a discrete GPU with less
// than 6 GiB of video memory Mid, the rest High; Low off, Mid low, High high); these two functions
// then call GT's instead and nothing else changes.
// ===========================================================================

#include <string>

namespace mcla::gpu::sun_shafts {

// "off", "low" or "high": what `auto` is on this graphics card.
const char* AutoName();

// The row's text for `auto`: "AUTO (HIGH)".
std::string AutoLabel();

}  // namespace mcla::gpu::sun_shafts
