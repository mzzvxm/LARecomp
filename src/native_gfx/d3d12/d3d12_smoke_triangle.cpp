#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — D3D12 smoke test (colored triangle).
// Now a thin client of the permanent infrastructure (D3D12Context +
// PresenterOutput); only the PSO / vertex buffer / draw are smoke-test
// specific. Kept as a regression test until the first real MCLA draw runs.

#include "d3d12_smoke_triangle.h"

#include <cstring>

#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/d3d12/d3d12_util.h>

#include "context.h"
#include "presenter_output.h"
#include "triangle_shaders.h"

namespace mcla::native_gfx {

using Microsoft::WRL::ComPtr;

namespace {
constexpr uint32_t kWidth = 1280;
constexpr uint32_t kHeight = 720;
constexpr DXGI_FORMAT kFormat = rex::ui::d3d12::D3D12Presenter::kGuestOutputFormat;

struct Vertex {
  float x, y;
  float r, g, b, a;
};
}  // namespace

struct D3D12SmokeTriangle::State {
  D3D12Context context;
  PresenterOutput output;
  ComPtr<ID3D12RootSignature> root_signature;
  ComPtr<ID3D12PipelineState> pso;
  ComPtr<ID3D12Resource> vertex_buffer;
};

D3D12SmokeTriangle::~D3D12SmokeTriangle() { Shutdown(); }

bool D3D12SmokeTriangle::Initialize(const rex::ui::d3d12::D3D12Provider& provider) {
  if (initialized_) {
    return true;
  }
  state_ = new State();
  State& s = *state_;

  if (!s.context.Initialize(provider) || !s.output.Initialize(s.context, kWidth, kHeight)) {
    Shutdown();
    return false;
  }
  ID3D12Device* device = s.context.device();

  // Empty root signature: the triangle uses only vertex attributes.
  {
    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
        FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                           IID_PPV_ARGS(&s.root_signature)))) {
      REXLOG_ERROR("[native_gfx] smoke: root signature creation failed");
      Shutdown();
      return false;
    }
  }

  // PSO: embedded DXIL, one RTV, no depth, no culling.
  {
    D3D12_INPUT_ELEMENT_DESC input[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = s.root_signature.Get();
    desc.VS = {kTriangleVsDxil, sizeof(kTriangleVsDxil)};
    desc.PS = {kTrianglePsDxil, sizeof(kTrianglePsDxil)};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.InputLayout = {input, 2};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = kFormat;
    desc.SampleDesc.Count = 1;
    if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&s.pso)))) {
      REXLOG_ERROR("[native_gfx] smoke: CreateGraphicsPipelineState failed");
      Shutdown();
      return false;
    }
  }

  // Vertex buffer in an upload heap (3 vertices, written once).
  {
    const Vertex vertices[3] = {
        {0.0f, 0.6f, 1.0f, 0.1f, 0.1f, 1.0f},    // top, red
        {0.55f, -0.5f, 0.1f, 1.0f, 0.1f, 1.0f},  // bottom right, green
        {-0.55f, -0.5f, 0.1f, 0.1f, 1.0f, 1.0f}, // bottom left, blue
    };
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = sizeof(vertices);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(
            &rex::ui::d3d12::util::kHeapPropertiesUpload, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&s.vertex_buffer)))) {
      REXLOG_ERROR("[native_gfx] smoke: vertex buffer creation failed");
      Shutdown();
      return false;
    }
    void* mapped = nullptr;
    const D3D12_RANGE no_read = {0, 0};
    if (FAILED(s.vertex_buffer->Map(0, &no_read, &mapped))) {
      REXLOG_ERROR("[native_gfx] smoke: vertex buffer map failed");
      Shutdown();
      return false;
    }
    std::memcpy(mapped, vertices, sizeof(vertices));
    s.vertex_buffer->Unmap(0, nullptr);
  }

  initialized_ = true;
  REXLOG_INFO("[native_gfx] D3D12 smoke triangle initialized ({}x{})", kWidth, kHeight);
  return true;
}

bool D3D12SmokeTriangle::Present(rex::ui::Presenter* presenter) {
  if (!initialized_ || !presenter) {
    return false;
  }
  State& s = *state_;
  const auto record = +[](void* user, D3D12Context&, ID3D12GraphicsCommandList* cl,
                          PresenterOutput& output) {
    State& s = *static_cast<State*>(user);
    const float clear_color[4] = {0.05f, 0.05f, 0.08f, 1.0f};
    output.BindAndClear(cl, clear_color);
    cl->SetPipelineState(s.pso.Get());
    cl->SetGraphicsRootSignature(s.root_signature.Get());
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vbv = {s.vertex_buffer->GetGPUVirtualAddress(), 3 * sizeof(Vertex),
                                    sizeof(Vertex)};
    cl->IASetVertexBuffers(0, 1, &vbv);
    cl->DrawInstanced(3, 1, 0, 0);
  };
  const bool ok = s.output.PresentFrame(s.context, presenter, record, &s);
  if (ok) {
    ++frame_index_;
  }
  return ok;
}

void D3D12SmokeTriangle::Shutdown() {
  if (!state_) {
    return;
  }
  state_->output.Shutdown(state_->context);
  state_->context.Shutdown();
  delete state_;
  state_ = nullptr;
  initialized_ = false;
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET
