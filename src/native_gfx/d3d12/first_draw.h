#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — first real draw
// ===========================================================================
// Executes ONE real MCLA draw through the native pipeline, end to end:
//
//   guest draw -> DrawSnapshot -> BufferCache / ShaderDatabase / TextureCache
//              -> constants -> descriptors -> PSO -> DrawIndexedInstanced
//              -> readback -> mcla_native_gfx_firstdraw.tga
//
// It renders into a DEDICATED render target matching the guest's colour
// format and sample count, not the presenter's frame RT, for two reasons:
//   * the PSO is built from the guest render state, so the render target has
//     to match it — bending the PSO to fit the presenter's R10G10B10A2 1x
//     target would validate a pipeline the game never asked for;
//   * presenting would require suppressing the guest swap, which TDRs the
//     GPU while the Xenia command processor is still consuming the guest
//     command stream (observed: DEVICE_HUNG 0x887A0006).
//
// The result is read back to a file instead, which is a visual validation
// that costs nothing in risk. Runs once, then latches off.
// ===========================================================================

#include <cstdint>

namespace rex::ui {
class Presenter;
}

namespace mcla::native_gfx {

class D3D12Context;
class BufferCache;
class TextureCache;
class TextureBinder;
class PipelineCache;
class ShaderDatabase;

// Attempts the first real draw. Returns true once it has run (successfully
// or not) so the caller can stop trying. `cl` must be null: this opens and
// submits its own frame.
bool TryFirstRealDraw(const uint8_t* base, uint32_t dev, uint32_t primitive_type,
                      uint32_t element_count, uint32_t start_element, int32_t base_vertex,
                      bool indexed, D3D12Context& context, ShaderDatabase& shaders,
                      BufferCache& buffers, TextureCache& textures, TextureBinder& binder,
                      PipelineCache& pipelines);

// True once the attempt has been made.
bool FirstRealDrawDone();

}  // namespace mcla::native_gfx
