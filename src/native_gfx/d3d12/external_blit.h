#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — external-source blit into the guest output
// ===========================================================================
// The Presenter's guest output is UAV-capable but NOT render-target-capable
// (ALLOW_UNORDERED_ACCESS only upstream -- an RTV over it removes the device),
// so getting an arbitrary texture in there is a COMPUTE blit: sample the
// source SRV, write the guest output UAV. Same thing the emulated gamma pass
// does.
//
// This used to live in the SDK (rex/graphics/native_rhi.h ->
// NativeRhiBlitExternalToGuestOutput), reached through an RHI interface only
// because the implementation sat in the rexgpu-xenos plugin, which this
// executable never links. Recording it here instead drops that whole layer:
// it is plain D3D12 on the runtime's own command list.
// ===========================================================================

#include <cstdint>

#include <rex/ui/d3d12/d3d12_api.h>

namespace mcla::native_gfx {

// Records the blit of `source` (in PIXEL_SHADER_RESOURCE state, viewed as
// `source_srv_format`) into `guest_output` on `cl`. The guest output is
// transitioned out of `guest_output_state` and back before returning, so the
// Presenter's state contract holds. Builds the pipeline on first use.
//
// Returns false when the pipeline could not be built; nothing is recorded.
bool RecordExternalBlitToGuestOutput(ID3D12Device* device, ID3D12GraphicsCommandList* cl,
                                     ID3D12Resource* guest_output, ID3D12Resource* source,
                                     uint32_t source_srv_format, uint32_t width, uint32_t height,
                                     D3D12_RESOURCE_STATES guest_output_state);

}  // namespace mcla::native_gfx
