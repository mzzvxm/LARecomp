#pragma once
// ===========================================================================
// MCLA GPU renderer -- contact shadows: what the pause menu asks of them
// ===========================================================================
// The effect itself is part of the ambient occlusion's passes (ao.h). This header is only the
// small surface the pause menu's CONTACT SHADOWS row needs, so that the menu does not include the
// renderer: what `auto` comes to on this GPU, and the row's text for it.
//
// CHECK: GT -- package GT (src/gpu/gpu_class.h) owns the GPU class and the `auto` table
// (Resolve(Effect, value), AutoLabel(Effect)). Until it is merged, ao.cpp keeps this effect's cell
// of the table itself (a stub that reads the class from the device) and these two functions answer
// from it. When GT lands, both go away: the renderer calls
// gpu_class::Resolve(gpu_class::Effect::kContactShadows, value) and the pause row's label function
// gpu_class::AutoLabel(gpu_class::Effect::kContactShadows).

#include <string>

namespace mcla::gpu::contact_shadows {

// What `auto` is on this GPU: "off", "low" or "high" (an integrated GPU, a discrete one with under
// 6 GiB, the rest). "high" until the renderer has looked at the device.
std::string ResolveAuto();

// The pause row's text for the auto value: "AUTO (OFF)", "AUTO (LOW)" or "AUTO (HIGH)".
std::string AutoLabel();

}  // namespace mcla::gpu::contact_shadows
