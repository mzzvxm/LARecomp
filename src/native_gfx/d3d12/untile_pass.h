#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — GPU texture untiling & endian swap pass
// ===========================================================================
// Dispatches a compute shader (untile_cs) to until and endian-swap raw Xenos
// textures on the GPU, moving the ~16% render-thread decode stall entirely to
// the GPU ALUs.
// ===========================================================================

#include <cstdint>
#include <rex/cvar.h>
#include <rex/ui/d3d12/d3d12_api.h>

REXCVAR_DECLARE(bool, mcla_native_gfx_gpu_untile);

namespace mcla::native_gfx {

class D3D12Context;

class UntilePass {
 public:
  bool Initialize(D3D12Context& context);
  void Shutdown();
  bool initialized() const { return pso_ != nullptr; }

  // Dispatches compute shader untiling + endian swapping on GPU.
  // Both src and dst are byte address buffers (ByteAddressBuffer / RWByteAddressBuffer).
  // Returns true on successful dispatch.
  bool Dispatch(ID3D12GraphicsCommandList* cl,
                D3D12_GPU_VIRTUAL_ADDRESS src_tiled_gpu,
                uint64_t src_size_bytes,
                D3D12_GPU_VIRTUAL_ADDRESS dst_linear_gpu,
                uint32_t width_blocks,
                uint32_t height_blocks,
                uint32_t pitch_blocks,
                uint32_t dst_pitch_bytes,
                uint32_t bytes_per_block,
                uint32_t endianness);

 private:
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_;
};

}  // namespace mcla::native_gfx
