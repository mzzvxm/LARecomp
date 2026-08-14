#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — fullscreen blit. See blit_pass.h.

#include "blit_pass.h"

#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_util.h>

#include "blit_shaders.h"
#include "context.h"

namespace mcla::native_gfx {

using Microsoft::WRL::ComPtr;

bool BlitPass::Initialize(D3D12Context& context, uint32_t dest_format) {
  dest_format_ = dest_format;
  ID3D12Device* device = context.device();

  // One SRV table (t0) plus a static linear-clamp sampler (s0).
  {
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 1;
    param.DescriptorTable.pDescriptorRanges = &range;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 1;
    desc.pParameters = &param;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
        FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                           IID_PPV_ARGS(&root_signature_)))) {
      REXLOG_ERROR("[native_gfx] blit: root signature creation failed");
      return false;
    }
  }

  {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = root_signature_.Get();
    desc.VS = {kBlitVsDxil, kBlitVsDxilLen};
    desc.PS = {kBlitPsDxil, kBlitPsDxilLen};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT(dest_format_);
    desc.SampleDesc.Count = 1;
    if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso_)))) {
      REXLOG_ERROR("[native_gfx] blit: CreateGraphicsPipelineState failed");
      return false;
    }
  }

  {
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 1;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srv_heap_)))) {
      REXLOG_ERROR("[native_gfx] blit: SRV heap creation failed");
      return false;
    }
    srv_increment_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  }
  return true;
}

void BlitPass::Shutdown() {
  srv_heap_.Reset();
  pso_.Reset();
  root_signature_.Reset();
}

void BlitPass::Record(D3D12Context& context, ID3D12GraphicsCommandList* cl, ID3D12Resource* source,
                      uint32_t source_srv_format) {
  if (!pso_ || !source) {
    return;
  }
  // Refresh the single SRV to point at this frame's source.
  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = DXGI_FORMAT(source_srv_format);
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  const D3D12_CPU_DESCRIPTOR_HANDLE cpu = srv_heap_->GetCPUDescriptorHandleForHeapStart();
  context.device()->CreateShaderResourceView(source, &srv, cpu);

  ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
  cl->SetDescriptorHeaps(1, heaps);
  cl->SetGraphicsRootSignature(root_signature_.Get());
  cl->SetPipelineState(pso_.Get());
  cl->SetGraphicsRootDescriptorTable(0, srv_heap_->GetGPUDescriptorHandleForHeapStart());
  cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cl->IASetVertexBuffers(0, 0, nullptr);
  cl->IASetIndexBuffer(nullptr);
  cl->DrawInstanced(3, 1, 0, 0);
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET
