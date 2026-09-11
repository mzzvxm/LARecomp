#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — root signature + PSO cache
// ===========================================================================
// The root signature is fixed by what the translated shaders declare. Every
// one of the 2333 shaders uses exactly the same registers and spaces (no
// exceptions found), so this is the FINAL layout, not a bring-up placeholder:
//
//   b0 space4  VertexShaderConstants   -> root CBV
//   b1 space4  PixelShaderConstants    -> root CBV
//   b2 space4  SharedConstants         -> root CBV
//   t0 space0  Texture2D[]   (unbounded)  \
//   t0 space1  Texture3D[]   (unbounded)   > one SRV descriptor table
//   t0 space2  TextureCube[] (unbounded)  /
//   s0 space3  SamplerState[] (unbounded) -> sampler descriptor table
//
// The texture/sampler tables are declared now even though the texture cache
// is not wired into a draw yet: leaving them out would mean changing the
// root signature later, which invalidates every cached PSO. The shaders
// index those arrays with descriptor indices carried in SharedConstants
// c0..c31, so nothing here needs to change when textures land.
//
// PSO key: everything that can alter the pipeline state object. Shaders are
// identified by their normalized identity + spec variant rather than by
// bytecode pointer, so the key is stable across pack reloads.
// ===========================================================================

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <rex/ui/d3d12/d3d12_api.h>

#include "../guest/render_state.h"
#include "../vertex_declaration.h"

namespace mcla::native_gfx {

class D3D12Context;
struct GeometrySnapshot;
struct ShaderBytecode;

// Root parameter order. Keep in sync with PipelineCache::Initialize.
//
// Each texture space gets its OWN descriptor table: an unbounded range must
// be the last range in its table (D3D12 rejects "append range with implicit
// lower bound after an unbounded range"), so the three unbounded SRV arrays
// the shaders declare cannot share one table.
enum RootParameter : uint32_t {
  kRootVsConstants = 0,
  kRootPsConstants = 1,
  kRootSharedConstants = 2,
  kRootTexture2DTable = 3,  // t0 space0
  kRootTexture3DTable = 4,  // t0 space1
  kRootTextureCubeTable = 5,  // t0 space2
  kRootSamplerTable = 6,    // s0 space3
  kRootParameterCount = 7,
};

// One input element, flattened for hashing (the semantic name is a pointer
// to a literal, so it is compared by string identity of the usage instead).
struct PsoInputElement {
  uint32_t usage = 0;
  uint32_t usage_index = 0;
  uint32_t dxgi_format = 0;
  uint32_t input_slot = 0;
  uint32_t aligned_byte_offset = 0;
  bool operator==(const PsoInputElement& o) const {
    return usage == o.usage && usage_index == o.usage_index &&
           dxgi_format == o.dxgi_format && input_slot == o.input_slot &&
           aligned_byte_offset == o.aligned_byte_offset;
  }
};

struct PsoKey {
  uint64_t vs_identity = 0;
  uint64_t ps_identity = 0;  // 0 when no pixel shader is bound
  uint32_t vs_spec_mask = 0;
  uint32_t ps_spec_mask = 0;
  uint32_t topology_type = 0;  // D3D12_PRIMITIVE_TOPOLOGY_TYPE
  uint32_t rt_format = 0;      // DXGI
  // Second render target's DXGI format, 0 when the draw writes only oC0. The
  // PSO's render-target count has to match the bound set exactly.
  uint32_t rt1_format = 0;
  uint32_t blend_control1 = 0;  // RB_BLENDCONTROL1, target 1's own equation
  uint32_t ds_format = 0;      // DXGI
  uint32_t sample_count = 1;
  // Raw guest registers that drive blend / raster / depth. Kept raw so the
  // key cannot silently ignore a field the translation does not use yet.
  uint32_t blend_control0 = 0;
  uint32_t color_control = 0;
  uint32_t color_mask = 0;
  uint32_t depth_control = 0;
  uint32_t stencil_ref_mask = 0;  // RB_STENCILREFMASK: ref + read/write masks
  uint32_t pa_su_sc_mode_cntl = 0;
  // Set when the guest viewport has a positive Y scale, which D3D12 cannot
  // express as a viewport and which reverses the screen-space winding.
  uint32_t y_flipped = 0;
  // Polygon offset, already converted to what D3D12's rasterizer wants. Both
  // stay zero unless PA_SU_SC_MODE_CNTL enables the offset for the face that
  // survives culling.
  int32_t depth_bias = 0;
  float slope_scaled_depth_bias = 0.0f;
  std::vector<PsoInputElement> input_layout;

  bool operator==(const PsoKey& o) const;
};

struct PsoKeyHash {
  size_t operator()(const PsoKey& k) const;
};

class PipelineCache {
 public:
  struct Stats {
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t creation_failures = 0;
  };

  bool Initialize(D3D12Context& context);
  void Shutdown(D3D12Context& context);

  ID3D12RootSignature* root_signature() const { return root_signature_.Get(); }

  // Builds the key for a draw from the pieces already captured.
  static PsoKey MakeKey(const GeometrySnapshot& geometry, const GuestRenderState& render_state,
                        uint64_t vs_identity, uint64_t ps_identity, uint32_t vs_spec_mask,
                        uint32_t ps_spec_mask);

  // Returns a PSO for the key, creating it on a miss. `vs` / `ps` supply the
  // bytecode for creation; `ps` may be empty for depth-only draws.
  ID3D12PipelineState* GetOrCreate(D3D12Context& context, const PsoKey& key,
                                   const ShaderBytecode& vs, const ShaderBytecode& ps,
                                   const GeometrySnapshot& geometry);

  const Stats& stats() const { return stats_; }

  // Live PSOs, for the memory census. Their byte cost is driver-private, but a
  // count that keeps climbing means the key varies per draw.
  size_t pipeline_count() const { return pipelines_.size(); }

 private:
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
  std::unordered_map<PsoKey, Microsoft::WRL::ComPtr<ID3D12PipelineState>, PsoKeyHash> pipelines_;
  Stats stats_;
};

}  // namespace mcla::native_gfx
