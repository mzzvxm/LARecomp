#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — display gamma ramp. See header.

#include "gamma_pass.h"

#include <cstring>

#include <rex/logging.h>

#include "context.h"
#include "gamma_ramp_dxil.inc"  // kGammaRampCsDxil

namespace mcla::native_gfx {

namespace {
// sub_82426B68 fills 1024 big-endian 16-bit entries here, ten bits used. The
// second table at +2048 is the inverse and stays zero in this title.
constexpr uint32_t kGuestRampAddress = 0x828CDA48u;
constexpr uint32_t kRampEntries = 1024u;
constexpr uint32_t kRampBytes = kRampEntries * 2u;
}  // namespace

bool GammaPass::Initialize(D3D12Context& context) {
  if (pso_) {
    return true;
  }
  ID3D12Device* device = context.device();
  if (!device) {
    return false;
  }

  D3D12_DESCRIPTOR_RANGE ranges[2] = {};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = 2;  // t0 = source, t1 = ramp
  ranges[0].OffsetInDescriptorsFromTableStart = 0;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = 1;
  ranges[1].OffsetInDescriptorsFromTableStart = 2;
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable.NumDescriptorRanges = 2;
  params[0].DescriptorTable.pDescriptorRanges = ranges;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants.Num32BitValues = 4;  // w, h, enabled, pad
  params[1].Constants.ShaderRegister = 0;
  D3D12_ROOT_SIGNATURE_DESC rs_desc = {};
  rs_desc.NumParameters = 2;
  rs_desc.pParameters = params;
  Microsoft::WRL::ComPtr<ID3DBlob> rs_blob, err;
  if (FAILED(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &err))) {
    REXLOG_ERROR("[native_gfx] gamma root sig serialize failed: {}",
                 err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
    return false;
  }
  if (FAILED(device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                         IID_PPV_ARGS(&root_signature_)))) {
    REXLOG_ERROR("[native_gfx] gamma CreateRootSignature failed");
    return false;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc = {};
  pso_desc.pRootSignature = root_signature_.Get();
  pso_desc.CS = {kGammaRampCsDxil, sizeof(kGammaRampCsDxil)};
  if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso_)))) {
    REXLOG_ERROR("[native_gfx] gamma CS PSO failed");
    root_signature_.Reset();
    return false;
  }

  D3D12_DESCRIPTOR_HEAP_DESC hd = {};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = 3;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)))) {
    REXLOG_ERROR("[native_gfx] gamma descriptor heap failed");
    pso_.Reset();
    root_signature_.Reset();
    return false;
  }
  inc_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  D3D12_HEAP_PROPERTIES hp = {};
  D3D12_RESOURCE_DESC rd = {};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width = kRampBytes;
  rd.Height = 1;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.Format = DXGI_FORMAT_UNKNOWN;
  rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&ramp_)))) {
    REXLOG_ERROR("[native_gfx] gamma ramp buffer failed");
    return false;
  }
  hp.Type = D3D12_HEAP_TYPE_UPLOAD;
  if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                             IID_PPV_ARGS(&ramp_upload_)))) {
    REXLOG_ERROR("[native_gfx] gamma ramp upload buffer failed");
    return false;
  }
  REXLOG_INFO("[native_gfx] gamma ramp pass initialized");
  return true;
}

void GammaPass::Shutdown() {
  ramp_upload_.Reset();
  ramp_.Reset();
  heap_.Reset();
  pso_.Reset();
  root_signature_.Reset();
  ramp_ready_ = false;
}

bool GammaPass::UpdateRamp(D3D12Context& context, ID3D12GraphicsCommandList* cl,
                           const uint8_t* base) {
  if (!pso_ || !base || !cl) {
    return false;
  }
  const uint8_t* src = base + kGuestRampAddress;

  // Cheap change test on eight spread-out entries. The title rebuilds the ramp
  // only when the display gamma changes, so this is a copy per boot in practice.
  uint16_t probe[8];
  bool any_nonzero = false;
  for (uint32_t i = 0; i < 8; ++i) {
    const uint32_t index = i * (kRampEntries / 8u) + 7u;
    probe[i] = uint16_t(uint16_t(src[index * 2] << 8) | src[index * 2 + 1]);
    any_nonzero = any_nonzero || probe[i] != 0;
  }
  if (!any_nonzero) {
    // Not built yet. Applying an all-zero table would present a black frame.
    return false;
  }
  if (ramp_ready_ && std::memcmp(probe, last_, sizeof(probe)) == 0) {
    return true;
  }
  std::memcpy(last_, probe, sizeof(probe));

  void* mapped = nullptr;
  D3D12_RANGE none = {0, 0};
  if (FAILED(ramp_upload_->Map(0, &none, &mapped)) || !mapped) {
    return false;
  }
  // Guest entries are big-endian; the shader reads R16_UINT little-endian.
  auto* dst = static_cast<uint16_t*>(mapped);
  for (uint32_t i = 0; i < kRampEntries; ++i) {
    dst[i] = uint16_t(uint16_t(src[i * 2] << 8) | src[i * 2 + 1]);
  }
  D3D12_RANGE written = {0, kRampBytes};
  ramp_upload_->Unmap(0, &written);

  if (ramp_ready_) {
    D3D12_RESOURCE_BARRIER to_copy = {};
    to_copy.Transition.pResource = ramp_.Get();
    to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &to_copy);
  }
  cl->CopyBufferRegion(ramp_.Get(), 0, ramp_upload_.Get(), 0, kRampBytes);
  D3D12_RESOURCE_BARRIER to_read = {};
  to_read.Transition.pResource = ramp_.Get();
  to_read.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  to_read.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  to_read.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  cl->ResourceBarrier(1, &to_read);
  ramp_ready_ = true;
  return true;
}

void GammaPass::Record(D3D12Context& context, ID3D12GraphicsCommandList* cl, ID3D12Resource* src,
                       uint32_t src_dxgi_format, ID3D12Resource* dst, uint32_t width,
                       uint32_t height) {
  if (!pso_ || !src || !dst || !cl || !ramp_ready_) {
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

  D3D12_SHADER_RESOURCE_VIEW_DESC ramp_srv = {};
  ramp_srv.Format = DXGI_FORMAT_R16_UINT;
  ramp_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  ramp_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  ramp_srv.Buffer.NumElements = kRampEntries;
  device->CreateShaderResourceView(ramp_.Get(), &ramp_srv, cpu);
  cpu.ptr += inc_;

  D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
  uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device->CreateUnorderedAccessView(dst, nullptr, &uav, cpu);

  ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
  cl->SetDescriptorHeaps(1, heaps);
  cl->SetComputeRootSignature(root_signature_.Get());
  cl->SetPipelineState(pso_.Get());
  cl->SetComputeRootDescriptorTable(0, heap_->GetGPUDescriptorHandleForHeapStart());
  const uint32_t c[4] = {width, height, 1u, 0u};
  cl->SetComputeRoot32BitConstants(1, 4, c, 0);
  cl->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
}

}  // namespace mcla::native_gfx

#endif  // REXGLUE_HAS_XEO3_TARGET
