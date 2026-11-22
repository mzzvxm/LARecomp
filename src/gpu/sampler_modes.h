#pragma once
// ===========================================================================
// MCLA GPU renderer -- the sampler address modes the device can make (pure rules, no Plume)
// ===========================================================================
// The Xbox 360 clamps texture coordinates five ways (xenos::ClampMode); the host has four of them everywhere: wrap, mirrored wrap, clamp to the
// edge and clamp to a border colour. The fifth, mirror once (mirror, then clamp: the 360's kMirrorClampToEdge, and what its two other mirror-clamp
// modes fall back to), is a Direct3D 12 mode on every device but on Vulkan needs VK_KHR_sampler_mirror_clamp_to_edge (Vulkan 1.2's
// samplerMirrorClampToEdge). Plume reads that as RenderDeviceCapabilities::samplerMirrorClampToEdge and only asserts on it (a release build asks
// the driver for the mode anyway, which is not allowed without it). A device that lacks it gets mirrored wrap for the guest's mirror once: the
// same for every coordinate within one tile of the texture (the common use, a reflection or a decal that overshoots a little) and a mirrored
// repeat instead of the clamp beyond that, which is closer than any other mode. samplers.cpp maps the guest's modes through here.
// sampler_modes_test.cpp checks it alone.
// ===========================================================================

#include <cstdint>

namespace mcla::gpu::sampler_modes {

enum class Mode : uint8_t { kWrap, kMirror, kClamp, kMirrorOnce, kBorder };

// The mode to make for `wanted` on a device that can (or cannot) make mirror once.
constexpr Mode ForDevice(Mode wanted, bool mirror_once_supported) {
  return wanted == Mode::kMirrorOnce && !mirror_once_supported ? Mode::kMirror : wanted;
}

}  // namespace mcla::gpu::sampler_modes
