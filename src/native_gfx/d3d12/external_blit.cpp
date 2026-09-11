#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — external-source blit. See external_blit.h.

#include "external_blit.h"

#include <rex/logging.h>

#include "blit_cs_dxil.inc"

namespace mcla::native_gfx {
namespace {

// Lazily-built pipeline for the blit. Own shader-visible SRV/UAV heap so it
// never disturbs the runtime's own descriptor slots.
struct ExternalBlit {
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;  // [0]=SRV(source) [1]=UAV(guest)
  uint32_t inc = 0;
  bool tried = false;
  bool ok = false;
};
ExternalBlit g_blit;

bool EnsurePipeline(ID3D12Device* device) {
  if (g_blit.tried) {
    return g_blit.ok;
  }
  g_blit.tried = true;

  D3D12_DESCRIPTOR_RANGE ranges[2]{};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = 1;
  ranges[0].OffsetInDescriptorsFromTableStart = 0;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = 1;
  ranges[1].OffsetInDescriptorsFromTableStart = 1;

  D3D12_ROOT_PARAMETER params[2]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable.NumDescriptorRanges = 2;
  params[0].DescriptorTable.pDescriptorRanges = ranges;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants.Num32BitValues = 2;
  params[1].Constants.ShaderRegister = 0;

  D3D12_ROOT_SIGNATURE_DESC rs_desc{};
  rs_desc.NumParameters = 2;
  rs_desc.pParameters = params;

  Microsoft::WRL::ComPtr<ID3DBlob> rs_blob;
  Microsoft::WRL::ComPtr<ID3DBlob> err;
  HRESULT hr =
      D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &err);
  if (FAILED(hr)) {
    REXLOG_ERROR("[native_gfx] external blit root sig serialize failed 0x{:08X}: {}", uint32_t(hr),
                 err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
    return false;
  }
  hr = device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                   IID_PPV_ARGS(&g_blit.root_signature));
  if (FAILED(hr)) {
    REXLOG_ERROR("[native_gfx] external blit CreateRootSignature failed 0x{:08X} (removed=0x{:08X})",
                 uint32_t(hr), uint32_t(device->GetDeviceRemovedReason()));
    return false;
  }

  // Precompiled DXIL: runtime D3DCompile is not usable on this thread, so the
  // compute shader is built offline with dxc and embedded (blit_cs_dxil.inc).
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
  pso_desc.pRootSignature = g_blit.root_signature.Get();
  pso_desc.CS = {kBlitCsDxil, kBlitCsDxilLen};
  if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&g_blit.pso)))) {
    REXLOG_ERROR("[native_gfx] external blit CS PSO failed");
    return false;
  }

  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = 2;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_blit.heap)))) {
    REXLOG_ERROR("[native_gfx] external blit descriptor heap failed");
    return false;
  }
  g_blit.inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  g_blit.ok = true;
  REXLOG_INFO("[native_gfx] external blit pipeline ready");
  return true;
}

}  // namespace

bool RecordExternalBlitToGuestOutput(ID3D12Device* device, ID3D12GraphicsCommandList* cl,
                                     ID3D12Resource* guest_output, ID3D12Resource* source,
                                     uint32_t source_srv_format, uint32_t width, uint32_t height,
                                     D3D12_RESOURCE_STATES guest_output_state) {
  if (device == nullptr || cl == nullptr || guest_output == nullptr || source == nullptr) {
    return false;
  }
  if (!EnsurePipeline(device)) {
    return false;
  }

  // Slot 0: SRV over the external source. Slot 1: UAV over the guest output.
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_blit.heap->GetCPUDescriptorHandleForHeapStart();
  D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
  srv_desc.Format = DXGI_FORMAT(source_srv_format);
  srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv_desc.Texture2D.MipLevels = 1;
  device->CreateShaderResourceView(source, &srv_desc, cpu);

  cpu.ptr += g_blit.inc;
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
  uav_desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
  uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device->CreateUnorderedAccessView(guest_output, nullptr, &uav_desc, cpu);

  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = guest_output;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = guest_output_state;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  cl->ResourceBarrier(1, &barrier);

  ID3D12DescriptorHeap* heaps[] = {g_blit.heap.Get()};
  cl->SetDescriptorHeaps(1, heaps);
  cl->SetComputeRootSignature(g_blit.root_signature.Get());
  cl->SetPipelineState(g_blit.pso.Get());
  cl->SetComputeRootDescriptorTable(0, g_blit.heap->GetGPUDescriptorHandleForHeapStart());
  const uint32_t size[2] = {width, height};
  cl->SetComputeRoot32BitConstants(1, 2, size, 0);
  cl->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

  // Back to the state the Presenter expects on return.
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  barrier.Transition.StateAfter = guest_output_state;
  cl->ResourceBarrier(1, &barrier);
  return true;
}

}  // namespace mcla::native_gfx

#endif  // REXGLUE_HAS_XEO3_TARGET
