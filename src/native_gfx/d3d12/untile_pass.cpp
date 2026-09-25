#ifndef REXGLUE_HAS_XEO3_TARGET
// ===========================================================================
// MCLA Native Graphics Runtime — GPU texture untiling & endian swap pass
// ===========================================================================

#include "untile_pass.h"

#include <rex/logging.h>
#include <rex/math.h>

#include "context.h"
#include "untile_cs_dxil.inc"

namespace mcla::native_gfx {

bool UntilePass::Initialize(D3D12Context& context) {
  if (pso_) {
    return true;
  }
  ID3D12Device* device = context.device();
  if (!device) {
    return false;
  }

  // Root Parameters:
  // 0: b0 (UntileParams constant buffer - 8 uints)
  // 1: t0 (g_src_tiled ByteAddressBuffer SRV)
  // 2: u0 (g_dst_linear RWByteAddressBuffer UAV)
  D3D12_ROOT_PARAMETER params[3] = {};

  // Param 0: 32-bit constants (b0)
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.ShaderRegister = 0;
  params[0].Constants.RegisterSpace = 0;
  params[0].Constants.Num32BitValues = 8;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  // Param 1: Root SRV (t0)
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  params[1].Descriptor.ShaderRegister = 0;
  params[1].Descriptor.RegisterSpace = 0;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  // Param 2: Root UAV (u0)
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  params[2].Descriptor.ShaderRegister = 0;
  params[2].Descriptor.RegisterSpace = 0;
  params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  D3D12_ROOT_SIGNATURE_DESC rs_desc = {};
  rs_desc.NumParameters = 3;
  rs_desc.pParameters = params;
  rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

  Microsoft::WRL::ComPtr<ID3DBlob> rs_blob, err_blob;
  if (FAILED(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &err_blob))) {
    REXLOG_ERROR("[native_gfx] untile root signature serialize failed: {}",
                 err_blob ? static_cast<const char*>(err_blob->GetBufferPointer()) : "?");
    return false;
  }

  if (FAILED(device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                         IID_PPV_ARGS(&root_signature_)))) {
    REXLOG_ERROR("[native_gfx] untile CreateRootSignature failed");
    return false;
  }

  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc = {};
  pso_desc.pRootSignature = root_signature_.Get();
  pso_desc.CS = {kUntileCsDxil, sizeof(kUntileCsDxil)};

  if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso_)))) {
    REXLOG_ERROR("[native_gfx] untile CreateComputePipelineState failed");
    root_signature_.Reset();
    return false;
  }

  return true;
}

void UntilePass::Shutdown() {
  pso_.Reset();
  root_signature_.Reset();
}

bool UntilePass::Dispatch(ID3D12GraphicsCommandList* cl,
                          D3D12_GPU_VIRTUAL_ADDRESS src_tiled_gpu,
                          uint64_t src_size_bytes,
                          D3D12_GPU_VIRTUAL_ADDRESS dst_linear_gpu,
                          uint32_t width_blocks,
                          uint32_t height_blocks,
                          uint32_t pitch_blocks,
                          uint32_t dst_pitch_bytes,
                          uint32_t bytes_per_block,
                          uint32_t endianness) {
  if (!pso_ || !cl || !src_tiled_gpu || !dst_linear_gpu || width_blocks == 0 || height_blocks == 0) {
    return false;
  }

  struct {
    uint32_t width_blocks;
    uint32_t height_blocks;
    uint32_t pitch_blocks;
    uint32_t dst_pitch_bytes;
    uint32_t bytes_per_block;
    uint32_t bpb_log2;
    uint32_t endianness;
    uint32_t src_size_bytes;
  } cb;

  cb.width_blocks = width_blocks;
  cb.height_blocks = height_blocks;
  cb.pitch_blocks = pitch_blocks;
  cb.dst_pitch_bytes = dst_pitch_bytes;
  cb.bytes_per_block = bytes_per_block;
  cb.bpb_log2 = rex::log2_floor(bytes_per_block);
  cb.endianness = endianness;
  cb.src_size_bytes = static_cast<uint32_t>(src_size_bytes);

  cl->SetComputeRootSignature(root_signature_.Get());
  cl->SetPipelineState(pso_.Get());
  cl->SetComputeRoot32BitConstants(0, 8, &cb, 0);
  cl->SetComputeRootShaderResourceView(1, src_tiled_gpu);
  cl->SetComputeRootUnorderedAccessView(2, dst_linear_gpu);

  const uint32_t thread_group_x = (width_blocks + 7) / 8;
  const uint32_t thread_group_y = (height_blocks + 7) / 8;
  cl->Dispatch(thread_group_x, thread_group_y, 1);

  NoteCommandListStateDisturbed();
  return true;
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET
