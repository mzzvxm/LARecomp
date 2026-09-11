#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — FXAA. See header.

#include "fxaa_pass.h"

#include <cstring>

#include <rex/logging.h>

#include "context.h"
#include "fxaa_shaders.h"  // kFxaaCsDxil

namespace mcla::native_gfx {

bool FxaaPass::Initialize(D3D12Context& context) {
  if (pso_) {
    return true;
  }
  ID3D12Device* device = context.device();
  if (!device) {
    return false;
  }

  D3D12_DESCRIPTOR_RANGE ranges[2] = {};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = 1;  // t0 = source
  ranges[0].OffsetInDescriptorsFromTableStart = 0;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = 1;  // u0 = destination
  ranges[1].OffsetInDescriptorsFromTableStart = 1;
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable.NumDescriptorRanges = 2;
  params[0].DescriptorTable.pDescriptorRanges = ranges;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants.Num32BitValues = 4;  // w, h, threshold, subpixel
  params[1].Constants.ShaderRegister = 0;

  // Linear, or the sub-texel offset FXAA computes never resolves anything: a
  // point sampler snaps the final fetch back to the texel it started on and the
  // whole pass becomes a very expensive copy.
  D3D12_STATIC_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ShaderRegister = 0;

  D3D12_ROOT_SIGNATURE_DESC rs_desc = {};
  rs_desc.NumParameters = 2;
  rs_desc.pParameters = params;
  rs_desc.NumStaticSamplers = 1;
  rs_desc.pStaticSamplers = &sampler;
  Microsoft::WRL::ComPtr<ID3DBlob> rs_blob, err;
  if (FAILED(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &err))) {
    REXLOG_ERROR("[native_gfx] fxaa root sig serialize failed: {}",
                 err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
    return false;
  }
  if (FAILED(device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                         IID_PPV_ARGS(&root_signature_)))) {
    REXLOG_ERROR("[native_gfx] fxaa CreateRootSignature failed");
    return false;
  }

  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc = {};
  pso_desc.pRootSignature = root_signature_.Get();
  pso_desc.CS = {kFxaaCsDxil, kFxaaCsDxilLen};
  if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso_)))) {
    REXLOG_ERROR("[native_gfx] fxaa CS PSO failed");
    root_signature_.Reset();
    return false;
  }

  D3D12_DESCRIPTOR_HEAP_DESC hd = {};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = 2;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)))) {
    REXLOG_ERROR("[native_gfx] fxaa descriptor heap failed");
    pso_.Reset();
    root_signature_.Reset();
    return false;
  }
  inc_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  REXLOG_INFO("[native_gfx] fxaa pass initialized");
  return true;
}

void FxaaPass::Shutdown() {
  heap_.Reset();
  pso_.Reset();
  root_signature_.Reset();
}

void FxaaPass::Record(D3D12Context& context, ID3D12GraphicsCommandList* cl, ID3D12Resource* src,
                      uint32_t src_dxgi_format, ID3D12Resource* dst, uint32_t dst_dxgi_format,
                      uint32_t width, uint32_t height, float edge_threshold, float subpixel) {
  if (!pso_ || !src || !dst || !cl || !width || !height) {
    return;
  }
  ID3D12Device* device = context.device();
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap_->GetCPUDescriptorHandleForHeapStart();

  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = DXGI_FORMAT(src_dxgi_format);
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  device->CreateShaderResourceView(src, &srv, cpu);
  cpu.ptr += inc_;

  D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
  uav.Format = DXGI_FORMAT(dst_dxgi_format);
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device->CreateUnorderedAccessView(dst, nullptr, &uav, cpu);

  ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
  cl->SetDescriptorHeaps(1, heaps);
  cl->SetComputeRootSignature(root_signature_.Get());
  cl->SetPipelineState(pso_.Get());
  cl->SetComputeRootDescriptorTable(0, heap_->GetGPUDescriptorHandleForHeapStart());
  // Two uints then two floats, packed into the same four root constants the
  // shader's cbuffer declares in that order.
  uint32_t c[4] = {width, height, 0u, 0u};
  std::memcpy(&c[2], &edge_threshold, sizeof(float));
  std::memcpy(&c[3], &subpixel, sizeof(float));
  cl->SetComputeRoot32BitConstants(1, 4, c, 0);
  cl->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
}

}  // namespace mcla::native_gfx

#endif  // REXGLUE_HAS_XEO3_TARGET
