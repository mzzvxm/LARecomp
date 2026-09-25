#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — root signature + PSO cache.
// See pipeline_cache.h for the binding convention this reproduces.

#include "pipeline_cache.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <rex/logging.h>

#include "../geometry.h"
#include "context.h"
#include "device_manager.h"
#include "shader_db.h"

REXCVAR_DECLARE(int32_t, mcla_native_gfx_shadow_bias);
REXCVAR_DECLARE(bool, mcla_native_gfx_rectlist_nocull);
REXCVAR_DECLARE(bool, mcla_native_gfx_pipeline_library);

namespace mcla::native_gfx {

namespace {

// Xenos -> D3D12 translations for the state that feeds the PSO.

D3D12_COMPARISON_FUNC CompareFunc(uint32_t xenos) {
  switch (xenos) {
    case 0: return D3D12_COMPARISON_FUNC_NEVER;
    case 1: return D3D12_COMPARISON_FUNC_LESS;
    case 2: return D3D12_COMPARISON_FUNC_EQUAL;
    case 3: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case 4: return D3D12_COMPARISON_FUNC_GREATER;
    case 5: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case 6: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    default: return D3D12_COMPARISON_FUNC_ALWAYS;
  }
}

// Xenos BlendFactor -> D3D12, for the COLOR equation.
//
//   12/13 are CONSTANT_COLOR / ONE_MINUS_CONSTANT_COLOR and 14/15 are
//   CONSTANT_ALPHA / ONE_MINUS_CONSTANT_ALPHA; D3D12 has a single constant so
//   all four map onto BLEND_FACTOR / INV_BLEND_FACTOR. SRC_ALPHA_SAT is 16,
//   not 14. Unknown values are ZERO, not ONE: guessing ONE would silently
//   turn an unrecognised factor into a full-strength contribution.
D3D12_BLEND BlendFactor(uint32_t xenos) {
  switch (xenos) {
    case 1: return D3D12_BLEND_ONE;
    case 4: return D3D12_BLEND_SRC_COLOR;
    case 5: return D3D12_BLEND_INV_SRC_COLOR;
    case 6: return D3D12_BLEND_SRC_ALPHA;
    case 7: return D3D12_BLEND_INV_SRC_ALPHA;
    case 8: return D3D12_BLEND_DEST_COLOR;
    case 9: return D3D12_BLEND_INV_DEST_COLOR;
    case 10: return D3D12_BLEND_DEST_ALPHA;
    case 11: return D3D12_BLEND_INV_DEST_ALPHA;
    case 12: case 14: return D3D12_BLEND_BLEND_FACTOR;
    case 13: case 15: return D3D12_BLEND_INV_BLEND_FACTOR;
    case 16: return D3D12_BLEND_SRC_ALPHA_SAT;
    default: return D3D12_BLEND_ZERO;
  }
}

// Same table for the ALPHA equation, with every colour mode replaced by its
// alpha counterpart. The Xenos uses one enum for both equations, so a colour
// factor can legitimately appear in the alpha slots — but D3D12 rejects
// SRC_COLOR / DEST_COLOR and their inverses there, failing PSO creation with
// a bare E_INVALIDARG. Observed on 4 draws of one shader pair per frame.
D3D12_BLEND BlendFactorAlpha(uint32_t xenos) {
  switch (xenos) {
    case 1: return D3D12_BLEND_ONE;
    case 4: case 6: return D3D12_BLEND_SRC_ALPHA;
    case 5: case 7: return D3D12_BLEND_INV_SRC_ALPHA;
    case 8: case 10: return D3D12_BLEND_DEST_ALPHA;
    case 9: case 11: return D3D12_BLEND_INV_DEST_ALPHA;
    case 12: case 14: return D3D12_BLEND_BLEND_FACTOR;
    case 13: case 15: return D3D12_BLEND_INV_BLEND_FACTOR;
    case 16: return D3D12_BLEND_SRC_ALPHA_SAT;
    default: return D3D12_BLEND_ZERO;
  }
}

D3D12_BLEND_OP BlendOp(uint32_t xenos) {
  switch (xenos) {
    case 0: return D3D12_BLEND_OP_ADD;
    case 1: return D3D12_BLEND_OP_SUBTRACT;
    case 2: return D3D12_BLEND_OP_MIN;
    case 3: return D3D12_BLEND_OP_MAX;
    case 4: return D3D12_BLEND_OP_REV_SUBTRACT;
    default: return D3D12_BLEND_OP_ADD;
  }
}

// Blending is disabled when the guest programs the identity equation
// (src ONE, dest ZERO, ADD) on both color and alpha.
bool BlendIsIdentity(uint32_t bc) {
  const uint32_t color_src = bc & 0x1Fu;
  const uint32_t color_op = (bc >> 5) & 0x7u;
  const uint32_t color_dst = (bc >> 8) & 0x1Fu;
  const uint32_t alpha_src = (bc >> 16) & 0x1Fu;
  const uint32_t alpha_op = (bc >> 21) & 0x7u;
  const uint32_t alpha_dst = (bc >> 24) & 0x1Fu;
  return color_src == 1 && color_dst == 0 && color_op == 0 && alpha_src == 1 &&
         alpha_dst == 0 && alpha_op == 0;
}

// xenos::StencilOp -> D3D12. The Xenos enum starts at 0 (kKeep) while the
// D3D12 one starts at 1, so a raw copy would be off by one AND would leave
// invalid zeros that make PSO creation fail with no message.
D3D12_STENCIL_OP StencilOp(uint32_t xenos) {
  switch (xenos) {
    case 0: return D3D12_STENCIL_OP_KEEP;
    case 1: return D3D12_STENCIL_OP_ZERO;
    case 2: return D3D12_STENCIL_OP_REPLACE;
    case 3: return D3D12_STENCIL_OP_INCR_SAT;
    case 4: return D3D12_STENCIL_OP_DECR_SAT;
    case 5: return D3D12_STENCIL_OP_INVERT;
    case 6: return D3D12_STENCIL_OP_INCR;
    default: return D3D12_STENCIL_OP_DECR;
  }
}

// PA_SU_POLY_OFFSET_* -> D3D12 rasterizer bias. D3D12 has one bias for both
// faces, so the face that survives culling is the one whose offset is used;
// this mirrors rex::graphics::draw_util::GetPreferredFacePolygonOffset, which
// works off a RegisterFile the native path does not have.
//
// The registers are read out of the device's own shadow, so a value that is
// not a small finite number means the read landed somewhere unexpected and is
// ignored rather than turned into a bias that would push geometry through
// whatever it is meant to sit on.
void PreferredFacePolygonOffset(const GuestRenderState& s, bool primitive_polygonal,
                                float* scale_out, float* offset_out) {
  float scale = 0.0f, offset = 0.0f;
  if (primitive_polygonal) {
    if (s.poly_offset_front_enable && !s.cull_front) {
      scale = s.poly_offset_front_scale;
      offset = s.poly_offset_front_offset;
    }
    if (s.poly_offset_back_enable && !s.cull_back && scale == 0.0f && offset == 0.0f) {
      scale = s.poly_offset_back_scale;
      offset = s.poly_offset_back_offset;
    }
  }
  constexpr float kPlausibleLimit = 1.0e4f;
  if (!std::isfinite(scale) || !std::isfinite(offset) || std::fabs(scale) > kPlausibleLimit ||
      std::fabs(offset) > kPlausibleLimit) {
    scale = 0.0f;
    offset = 0.0f;
  }
  *scale_out = scale;
  *offset_out = offset;
}

// Byte-for-byte the arithmetic of rex::graphics::draw_util::
// GetD3D10IntegerPolygonOffset and xenos::kPolygonOffsetScaleSubpixelUnit,
// spelled out here because rex/graphics/util/draw.h pulls in xenos.h, which
// does not compile standalone in this tree (see topology_expand.cpp).
constexpr float kPolygonOffsetScaleSubpixelUnit = 1.0f / 16.0f;

int32_t IntegerPolygonOffset(uint32_t depth_format, float polygon_offset) {
  // Depth formats: kD24S8 = 0, kD24FS8 = 1.
  const bool is_float24 = depth_format == 1;
  constexpr float kFactorUnorm24 = float((UINT32_C(1) << 24) - 1);
  constexpr float kFactorFloat24 = float(UINT32_C(1) << (21 + 3));
  // Ceil rather than floor: if the offset is used at all, the primitives are
  // meant to separate, and flooring a small offset to 0 defeats that.
  int32_t offset_int = int32_t(std::ceil(std::fabs(polygon_offset) *
                                         (is_float24 ? kFactorFloat24 * (1.0f / 8.0f)
                                                     : kFactorUnorm24)));
  if (is_float24) {
    // float24 ULPs -> float32 ULPs: 3 mantissa bits of difference.
    offset_int <<= 3;
  }
  return polygon_offset < 0.0f ? -offset_int : offset_int;
}

inline void HashCombine(uint64_t& h, uint64_t v) {
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
}

// FNV-1a over a shader's DXIL, 0 for no shader.
uint64_t BytecodeHash(const ShaderBytecode& code) {
  if (!code.valid()) {
    return 0;
  }
  uint64_t h = 0xCBF29CE484222325ull;
  const auto* bytes = static_cast<const uint8_t*>(code.data);
  for (uint32_t i = 0; i < code.size; ++i) {
    h = (h ^ bytes[i]) * 0x100000001B3ull;
  }
  return h;
}

struct InputLayoutRegistry {
  std::unordered_map<uint64_t, uint32_t> hash_to_id;
  std::vector<std::vector<PsoInputElement>> layouts;
  // Content hash of each layout, indexed by id - 1. Ids are handed out in the
  // order layouts first appear, which changes from boot to boot, so anything
  // that outlives the process has to key on this instead.
  std::vector<uint64_t> hashes;

  uint64_t ContentHash(uint32_t id) const {
    return (id == 0 || id > hashes.size()) ? 0 : hashes[id - 1];
  }

  uint32_t Intern(const std::vector<InputElement>& elements) {
    if (elements.empty()) {
      return 0;
    }
    uint64_t h = 0xCBF29CE484222325ull;
    for (const auto& e : elements) {
      uint32_t usage = 0;
      if (e.semantic_name) {
        for (int i = 0; i < 4 && e.semantic_name[i]; ++i) {
          usage = (usage << 8) | uint8_t(e.semantic_name[i]);
        }
      }
      HashCombine(h, (uint64_t(usage) << 32) | e.semantic_index);
      HashCombine(h, (uint64_t(e.dxgi_format) << 32) | e.input_slot);
      HashCombine(h, e.aligned_byte_offset);
    }
    auto it = hash_to_id.find(h);
    if (it != hash_to_id.end()) {
      return it->second;
    }
    uint32_t id = static_cast<uint32_t>(layouts.size() + 1);
    std::vector<PsoInputElement> pso_elements;
    pso_elements.reserve(elements.size());
    for (const auto& e : elements) {
      PsoInputElement p;
      p.usage = 0;
      if (e.semantic_name) {
        for (int i = 0; i < 4 && e.semantic_name[i]; ++i) {
          p.usage = (p.usage << 8) | uint8_t(e.semantic_name[i]);
        }
      }
      p.usage_index = e.semantic_index;
      p.dxgi_format = e.dxgi_format;
      p.input_slot = e.input_slot;
      p.aligned_byte_offset = e.aligned_byte_offset;
      pso_elements.push_back(p);
    }
    layouts.push_back(std::move(pso_elements));
    hashes.push_back(h);
    hash_to_id.emplace(h, id);
    return id;
  }
} g_layout_registry;

}  // namespace

size_t PsoKeyHash::operator()(const PsoKey& k) const {
  uint64_t h = 0xCBF29CE484222325ull;
  HashCombine(h, k.vs_identity);
  HashCombine(h, k.ps_identity);
  HashCombine(h, (uint64_t(k.vs_spec_mask) << 32) | k.ps_spec_mask);
  HashCombine(h, (uint64_t(k.topology_type) << 32) | k.sample_count);
  HashCombine(h, (uint64_t(k.rt_format) << 32) | k.ds_format);
  HashCombine(h, (uint64_t(k.rt1_format) << 32) | k.blend_control1);
  HashCombine(h, (uint64_t(k.blend_control0) << 32) | k.color_control);
  HashCombine(h, (uint64_t(k.color_mask) << 32) | k.depth_control);
  HashCombine(h, (uint64_t(k.pa_su_sc_mode_cntl) << 32) | k.stencil_mask);
  HashCombine(h, (uint64_t(k.y_flipped) << 32) | k.layout_id);
  {
    uint32_t slope_bits;
    std::memcpy(&slope_bits, &k.slope_scaled_depth_bias, 4);
    HashCombine(h, (uint64_t(uint32_t(k.depth_bias)) << 32) | slope_bits);
  }
  return size_t(h);
}

PsoKey PipelineCache::MakeKey(const GeometrySnapshot& geometry,
                              const GuestRenderState& render_state, uint64_t vs_identity,
                              uint64_t ps_identity, uint32_t vs_spec_mask,
                              uint32_t ps_spec_mask) {
  PsoKey k;
  MakeKeyInto(k, geometry, render_state, vs_identity, ps_identity, vs_spec_mask, ps_spec_mask);
  return k;
}

void PipelineCache::MakeKeyInto(PsoKey& out, const GeometrySnapshot& geometry,
                                const GuestRenderState& render_state, uint64_t vs_identity,
                                uint64_t ps_identity, uint32_t vs_spec_mask,
                                uint32_t ps_spec_mask) {
  out.vs_identity = vs_identity;
  out.ps_identity = ps_identity;
  out.vs_spec_mask = vs_spec_mask;
  out.ps_spec_mask = ps_spec_mask;

  switch (PrimitiveTypeToTopology(geometry.primitive_type)) {
    case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:
      out.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
      break;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
      out.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
      break;
    default:
      out.topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      break;
  }

  out.rt_format = ColorRenderTargetFormatToDxgi(render_state.color_format);
  out.ds_format = DepthRenderTargetFormatToDxgi(render_state.depth_format);
  out.sample_count = SampleCountFromMsaa(render_state.msaa_samples);
  out.blend_control0 = render_state.blend_control0;
  out.blend_control1 = render_state.blend_control1;
  out.color_control = render_state.color_control;
  out.color_mask = render_state.color_mask;
  out.depth_control = render_state.depth_control;
  // Dynamic stencil ref is bound via OMSetStencilRef(); key keeps read/write masks only.
  out.stencil_mask = (render_state.stencil_read_mask & 0xFFu) |
                     ((render_state.stencil_write_mask & 0xFFu) << 8);
  out.pa_su_sc_mode_cntl = render_state.pa_su_sc_mode_cntl;
  if (geometry.primitive_type == 8u && REXCVAR_GET(mcla_native_gfx_rectlist_nocull)) {
    out.pa_su_sc_mode_cntl &= ~0x3u;  // clear cull_front | cull_back
  }
  out.y_flipped = ComputeHostViewport(render_state).y_flipped ? 1u : 0u;

  {
    float poly_offset_scale = 0.0f, poly_offset = 0.0f;
    PreferredFacePolygonOffset(render_state,
                               out.topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
                               &poly_offset_scale, &poly_offset);
    out.depth_bias = IntegerPolygonOffset(render_state.depth_format, poly_offset);
    out.slope_scaled_depth_bias = poly_offset_scale * kPolygonOffsetScaleSubpixelUnit;
    if (int32_t extra = REXCVAR_GET(mcla_native_gfx_shadow_bias)) {
      const HostViewport shadow_vp = ComputeHostViewport(render_state);
      if (shadow_vp.width == 640.0f && shadow_vp.height == 640.0f) {
        out.depth_bias += extra;
      }
    }
  }

  out.layout_id = g_layout_registry.Intern(geometry.input_layout);
}

void PipelineCache::InitializePipelineLibrary(D3D12Context& context) {
  if (pipeline_library_ || !REXCVAR_GET(mcla_native_gfx_pipeline_library)) {
    return;
  }
  Microsoft::WRL::ComPtr<ID3D12Device1> dev1;
  if (FAILED(context.device()->QueryInterface(IID_PPV_ARGS(&dev1)))) {
    return;
  }

  // v2 names pipelines by what they contain (see GetOrCreate). No v1 name can
  // match again, and the library carries every entry into each Serialize, so
  // the v1 file is deleted instead of loaded and dragged along forever.
  library_path_ = "cache/d3d12_pso_v2.cache";
  std::error_code ec;
  std::filesystem::create_directories("cache", ec);
  std::filesystem::remove("cache/d3d12_pso.cache", ec);

  std::ifstream file(library_path_, std::ios::binary | std::ios::ate);
  if (file.is_open()) {
    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    if (size > 0) {
      // CreatePipelineLibrary does not copy the blob: "the pointer provided as
      // input to this method must remain valid for the lifetime of the object
      // returned". Read into a local vector it was freed as soon as this block
      // closed, and every PSO loaded out of the library afterwards -- and every
      // Serialize -- read released memory. The driver removed the device on
      // the first load (DXGI_ERROR_DRIVER_INTERNAL_ERROR): the black screen.
      // Only a boot that finds the file builds the library from it, so the
      // boot that writes it always looked fine.
      bool from_file = false;
      library_blob_.resize(static_cast<size_t>(size));
      if (file.read(reinterpret_cast<char*>(library_blob_.data()), size)) {
        HRESULT hr = dev1->CreatePipelineLibrary(library_blob_.data(), library_blob_.size(),
                                                 IID_PPV_ARGS(&pipeline_library_));
        if (SUCCEEDED(hr)) {
          from_file = true;
          REXLOG_INFO("[native_gfx] ID3D12PipelineLibrary loaded from {} ({} bytes)",
                      library_path_, size);
        } else if (hr == D3D12_ERROR_DRIVER_VERSION_MISMATCH) {
          REXLOG_WARN("[native_gfx] Pipeline library driver version mismatch; re-creating empty library");
          dev1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&pipeline_library_));
          library_dirty_ = true;
        } else {
          REXLOG_WARN("[native_gfx] Failed to load pipeline library (HRESULT 0x{:08X}); re-creating empty",
                      static_cast<uint32_t>(hr));
          dev1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&pipeline_library_));
          library_dirty_ = true;
        }
      }
      if (!from_file) {
        library_blob_ = std::vector<uint8_t>();  // an empty library points at nothing
      }
    }
  }

  if (!pipeline_library_) {
    HRESULT hr = dev1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&pipeline_library_));
    if (SUCCEEDED(hr)) {
      REXLOG_INFO("[native_gfx] Created new empty ID3D12PipelineLibrary ({})", library_path_);
    }
  }
}

void PipelineCache::SaveLibrary() {
  if (!pipeline_library_ || !library_dirty_ || library_path_.empty()) {
    return;
  }
  const SIZE_T size = pipeline_library_->GetSerializedSize();
  if (size == 0) {
    return;
  }
  std::vector<uint8_t> buffer(size);
  HRESULT hr = pipeline_library_->Serialize(buffer.data(), buffer.size());
  if (FAILED(hr)) {
    REXLOG_WARN("[native_gfx] Failed to serialize pipeline library: HRESULT 0x{:08X}",
                static_cast<uint32_t>(hr));
    return;
  }

  std::filesystem::path path(library_path_);
  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
  }

  std::filesystem::path temp_path = path;
  temp_path += ".tmp";

  FILE* f = std::fopen(temp_path.string().c_str(), "wb");
  if (!f) {
    REXLOG_WARN("[native_gfx] Failed to open temp file for pipeline library: {}", temp_path.string());
    return;
  }
  const size_t written = std::fwrite(buffer.data(), 1, buffer.size(), f);
  std::fclose(f);

  if (written == buffer.size()) {
    std::filesystem::rename(temp_path, path, ec);
    if (!ec) {
      library_dirty_ = false;
      REXLOG_INFO("[native_gfx] Saved ID3D12PipelineLibrary to {} ({} bytes, {} PSOs stored)",
                  path.string(), buffer.size(), stats_.library_stores);
    } else {
      REXLOG_WARN("[native_gfx] Failed to replace pipeline library file: {}", ec.message());
    }
  }
}

bool PipelineCache::Initialize(D3D12Context& context) {
  if (root_signature_) {
    InitializePipelineLibrary(context);
    return true;
  }

  bool use_v1_1 = false;
  if (DeviceManager::Instance().IsInitialized() &&
      DeviceManager::Instance().capabilities().highest_root_signature_version >=
          D3D_ROOT_SIGNATURE_VERSION_1_1) {
    use_v1_1 = true;
  }

  Microsoft::WRL::ComPtr<ID3DBlob> blob, error;
  if (use_v1_1) {
    D3D12_DESCRIPTOR_RANGE1 srv_ranges[3] = {};
    for (uint32_t i = 0; i < 3; ++i) {
      srv_ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
      srv_ranges[i].NumDescriptors = UINT_MAX;  // unbounded, as declared in HLSL
      srv_ranges[i].BaseShaderRegister = 0;     // t0
      srv_ranges[i].RegisterSpace = i;          // space0/1/2
      srv_ranges[i].OffsetInDescriptorsFromTableStart = 0;
      srv_ranges[i].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
    }
    D3D12_DESCRIPTOR_RANGE1 sampler_range = {};
    sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    sampler_range.NumDescriptors = UINT_MAX;
    sampler_range.BaseShaderRegister = 0;  // s0
    sampler_range.RegisterSpace = 3;
    sampler_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    sampler_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;

    D3D12_ROOT_PARAMETER1 params[kRootParameterCount] = {};
    const uint32_t cbv_registers[3] = {0, 1, 2};
    for (uint32_t i = 0; i < 3; ++i) {
      params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      params[i].Descriptor.ShaderRegister = cbv_registers[i];
      params[i].Descriptor.RegisterSpace = 4;
      params[i].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
      params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    const uint32_t srv_params[3] = {kRootTexture2DTable, kRootTexture3DTable,
                                    kRootTextureCubeTable};
    for (uint32_t i = 0; i < 3; ++i) {
      params[srv_params[i]].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      params[srv_params[i]].DescriptorTable.NumDescriptorRanges = 1;
      params[srv_params[i]].DescriptorTable.pDescriptorRanges = &srv_ranges[i];
      params[srv_params[i]].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    params[kRootSamplerTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kRootSamplerTable].DescriptorTable.NumDescriptorRanges = 1;
    params[kRootSamplerTable].DescriptorTable.pDescriptorRanges = &sampler_range;
    params[kRootSamplerTable].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC vdesc = {};
    vdesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    vdesc.Desc_1_1.NumParameters = kRootParameterCount;
    vdesc.Desc_1_1.pParameters = params;
    vdesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    if (SUCCEEDED(D3D12SerializeVersionedRootSignature(&vdesc, &blob, &error))) {
      if (SUCCEEDED(context.device()->CreateRootSignature(0, blob->GetBufferPointer(),
                                                         blob->GetBufferSize(),
                                                         IID_PPV_ARGS(&root_signature_)))) {
        REXLOG_INFO("[native_gfx] Root Signature 1.1 created (DATA_STATIC_WHILE_SET_AT_EXECUTE enabled)");
        root_signature_version_ = D3D_ROOT_SIGNATURE_VERSION_1_1;
        InitializePipelineLibrary(context);
        return true;
      }
    }
    REXLOG_WARN("[native_gfx] Root Signature 1.1 failed, falling back to 1.0");
  }

  // Fallback / standard Root Signature 1.0
  D3D12_DESCRIPTOR_RANGE srv_ranges[3] = {};
  for (uint32_t i = 0; i < 3; ++i) {
    srv_ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_ranges[i].NumDescriptors = UINT_MAX;  // unbounded, as declared in HLSL
    srv_ranges[i].BaseShaderRegister = 0;     // t0
    srv_ranges[i].RegisterSpace = i;          // space0/1/2
    srv_ranges[i].OffsetInDescriptorsFromTableStart = 0;
  }
  D3D12_DESCRIPTOR_RANGE sampler_range = {};
  sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
  sampler_range.NumDescriptors = UINT_MAX;
  sampler_range.BaseShaderRegister = 0;  // s0
  sampler_range.RegisterSpace = 3;
  sampler_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

  D3D12_ROOT_PARAMETER params[kRootParameterCount] = {};
  const uint32_t cbv_registers[3] = {0, 1, 2};
  for (uint32_t i = 0; i < 3; ++i) {
    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[i].Descriptor.ShaderRegister = cbv_registers[i];
    params[i].Descriptor.RegisterSpace = 4;
    params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  const uint32_t srv_params[3] = {kRootTexture2DTable, kRootTexture3DTable,
                                  kRootTextureCubeTable};
  for (uint32_t i = 0; i < 3; ++i) {
    params[srv_params[i]].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[srv_params[i]].DescriptorTable.NumDescriptorRanges = 1;
    params[srv_params[i]].DescriptorTable.pDescriptorRanges = &srv_ranges[i];
    params[srv_params[i]].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  params[kRootSamplerTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[kRootSamplerTable].DescriptorTable.NumDescriptorRanges = 1;
  params[kRootSamplerTable].DescriptorTable.pDescriptorRanges = &sampler_range;
  params[kRootSamplerTable].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  D3D12_ROOT_SIGNATURE_DESC desc = {};
  desc.NumParameters = kRootParameterCount;
  desc.pParameters = params;
  desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

  if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error))) {
    REXLOG_ERROR("[native_gfx] root signature serialization failed: {}",
                 error ? static_cast<const char*>(error->GetBufferPointer()) : "(no message)");
    return false;
  }
  if (FAILED(context.device()->CreateRootSignature(0, blob->GetBufferPointer(),
                                                   blob->GetBufferSize(),
                                                   IID_PPV_ARGS(&root_signature_)))) {
    REXLOG_ERROR("[native_gfx] CreateRootSignature failed");
    return false;
  }
  REXLOG_INFO("[native_gfx] root signature created (b0/b1/b2 space4, t0 space0-2, s0 space3)");
  root_signature_version_ = D3D_ROOT_SIGNATURE_VERSION_1;
  InitializePipelineLibrary(context);
  return true;
}

void PipelineCache::Shutdown(D3D12Context& context) {
  SaveLibrary();
  pipeline_library_.Reset();
  for (auto& [key, pso] : pipelines_) {
    if (pso) {
      context.DeferRelease(pso.Detach());
    }
  }
  pipelines_.clear();
  mru_pso_ = nullptr;
  mru_key_ = {};
  root_signature_.Reset();
}

ID3D12PipelineState* PipelineCache::GetOrCreate(D3D12Context& context, const PsoKey& key,
                                                const ShaderBytecode& vs,
                                                const ShaderBytecode& ps,
                                                const GeometrySnapshot& geometry) {
  if (mru_pso_ && key == mru_key_) {
    ++stats_.hits;
    return mru_pso_;
  }

  auto it = pipelines_.find(key);
  if (it != pipelines_.end()) {
    ++stats_.hits;
    mru_key_ = key;
    mru_pso_ = it->second.Get();
    return mru_pso_;
  }
  ++stats_.misses;

  if (!root_signature_ || !vs.valid()) {
    ++stats_.creation_failures;
    return nullptr;
  }

  std::vector<D3D12_INPUT_ELEMENT_DESC> input;
  input.reserve(geometry.input_layout.size());
  for (const InputElement& e : geometry.input_layout) {
    D3D12_INPUT_ELEMENT_DESC d = {};
    d.SemanticName = e.semantic_name;
    d.SemanticIndex = e.semantic_index;
    d.Format = DXGI_FORMAT(e.dxgi_format);
    d.InputSlot = e.input_slot;
    d.AlignedByteOffset = e.aligned_byte_offset;
    d.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    input.push_back(d);
  }

  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
  desc.pRootSignature = root_signature_.Get();
  desc.VS = {vs.data, vs.size};
  if (ps.valid()) {
    desc.PS = {ps.data, ps.size};
  }
  desc.InputLayout = {input.data(), UINT(input.size())};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE(key.topology_type);
  desc.SampleMask = UINT_MAX;
  desc.SampleDesc.Count = key.sample_count;

  // Blend.
  // RB_COLORCONTROL bit 4 is alpha_to_mask_enable: Xenos turns the alpha into a
  // coverage mask, which is what gives foliage a feathered edge instead of a
  // hard cutout. MCLA asks for it on the scene pass -- measured, colour control
  // 0x8700001C (with alpha test) and 0x87000014 (without) on the 1280x720
  // target -- and the runtime ignored the bit entirely, which is why the same
  // palm has 43 hard steps along its fronds against 1 in the emulated path.
  // It only does anything on a multisampled target; at one sample D3D12
  // coverage is binary and the flag is a no-op, so it is tied to
  // mcla_native_gfx_msaa by construction rather than by a condition.
  desc.BlendState.AlphaToCoverageEnable =
      (key.sample_count > 1 && (key.color_control & 0x10u) != 0) ? TRUE : FALSE;
  auto& rt0 = desc.BlendState.RenderTarget[0];
  rt0.BlendEnable = BlendIsIdentity(key.blend_control0) ? FALSE : TRUE;
  rt0.SrcBlend = BlendFactor(key.blend_control0 & 0x1Fu);
  rt0.BlendOp = BlendOp((key.blend_control0 >> 5) & 0x7u);
  rt0.DestBlend = BlendFactor((key.blend_control0 >> 8) & 0x1Fu);
  rt0.SrcBlendAlpha = BlendFactorAlpha((key.blend_control0 >> 16) & 0x1Fu);
  rt0.BlendOpAlpha = BlendOp((key.blend_control0 >> 21) & 0x7u);
  rt0.DestBlendAlpha = BlendFactorAlpha((key.blend_control0 >> 24) & 0x1Fu);

  // D3D12 ignores the blend factors for MIN and MAX, so whatever is written
  // above is already inert for those ops -- but the Xenos does NOT ignore them,
  // and the pixel shader has folded the source factor into its own output
  // (BlendPremultFor / applyBlendPremult). Stating ONE here makes the pipeline
  // describe what actually happens instead of carrying factors that no longer
  // mean anything, and keeps a driver that does honour them from applying the
  // source factor a second time.
  if (rt0.BlendOp == D3D12_BLEND_OP_MIN || rt0.BlendOp == D3D12_BLEND_OP_MAX) {
    rt0.SrcBlend = D3D12_BLEND_ONE;
    rt0.DestBlend = D3D12_BLEND_ONE;
  }
  if (rt0.BlendOpAlpha == D3D12_BLEND_OP_MIN || rt0.BlendOpAlpha == D3D12_BLEND_OP_MAX) {
    rt0.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt0.DestBlendAlpha = D3D12_BLEND_ONE;
  }
  // RB_COLOR_MASK holds one nibble per render target.
  rt0.RenderTargetWriteMask = UINT8(key.color_mask & 0xFu);

  // Rasterizer.
  const bool cull_front = (key.pa_su_sc_mode_cntl & 0x1u) != 0;
  const bool cull_back = (key.pa_su_sc_mode_cntl & 0x2u) != 0;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = cull_front ? D3D12_CULL_MODE_FRONT
                                             : (cull_back ? D3D12_CULL_MODE_BACK
                                                          : D3D12_CULL_MODE_NONE);
  // PA_SU_SC_MODE_CNTL.face: 0 = counter-clockwise is front. A Y-flipped
  // viewport mirrors screen space, which reverses the winding of every
  // triangle, so the front face has to be inverted to compensate.
  bool front_counter_clockwise = (key.pa_su_sc_mode_cntl & 0x4u) == 0;
  if (key.y_flipped) {
    front_counter_clockwise = !front_counter_clockwise;
  }
  desc.RasterizerState.FrontCounterClockwise = front_counter_clockwise ? TRUE : FALSE;
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.RasterizerState.DepthBias = key.depth_bias;
  desc.RasterizerState.DepthBiasClamp = 0.0f;
  desc.RasterizerState.SlopeScaledDepthBias = key.slope_scaled_depth_bias;

  // Depth / stencil.
  desc.DepthStencilState.DepthEnable = (key.depth_control & 0x2u) ? TRUE : FALSE;
  desc.DepthStencilState.DepthWriteMask =
      (key.depth_control & 0x4u) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
  desc.DepthStencilState.DepthFunc = CompareFunc((key.depth_control >> 4) & 0x7u);
  desc.DepthStencilState.StencilEnable = (key.depth_control & 0x1u) ? TRUE : FALSE;
  // FrontFace/BackFace must be valid whenever stencil is enabled: D3D12's
  // enums start at 1, so leaving the zero-initialised struct in place makes
  // CreateGraphicsPipelineState fail with E_INVALIDARG and no message.
  desc.DepthStencilState.StencilReadMask = UINT8(key.stencil_mask & 0xFF);
  desc.DepthStencilState.StencilWriteMask = UINT8((key.stencil_mask >> 8) & 0xFF);
  desc.DepthStencilState.FrontFace.StencilFunc = CompareFunc((key.depth_control >> 8) & 0x7u);
  desc.DepthStencilState.FrontFace.StencilFailOp = StencilOp((key.depth_control >> 11) & 0x7u);
  desc.DepthStencilState.FrontFace.StencilPassOp = StencilOp((key.depth_control >> 14) & 0x7u);
  desc.DepthStencilState.FrontFace.StencilDepthFailOp =
      StencilOp((key.depth_control >> 17) & 0x7u);
  // Back-face state only differs when RB_DEPTHCONTROL.backface_enable is set;
  // otherwise the Xenos applies the front-face ops to both.
  if (key.depth_control & 0x80u) {
    desc.DepthStencilState.BackFace.StencilFunc = CompareFunc((key.depth_control >> 20) & 0x7u);
    desc.DepthStencilState.BackFace.StencilFailOp = StencilOp((key.depth_control >> 23) & 0x7u);
    desc.DepthStencilState.BackFace.StencilPassOp = StencilOp((key.depth_control >> 26) & 0x7u);
    desc.DepthStencilState.BackFace.StencilDepthFailOp =
        StencilOp((key.depth_control >> 29) & 0x7u);
  } else {
    desc.DepthStencilState.BackFace = desc.DepthStencilState.FrontFace;
  }

  if (key.rt_format != DXGI_FORMAT_UNKNOWN) {
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT(key.rt_format);
    if (key.rt1_format != 0) {
      // Two targets. RB_COLOR_MASK's second nibble is target 1's write mask,
      // and RB_BLENDCONTROL1 is its own blend equation -- independent blending
      // has to be on for either to be honoured. The impostor bake writes both
      // opaquely, so this is the general shape rather than a special case.
      desc.NumRenderTargets = 2;
      desc.RTVFormats[1] = DXGI_FORMAT(key.rt1_format);
      desc.BlendState.IndependentBlendEnable = TRUE;
      auto& rt1 = desc.BlendState.RenderTarget[1];
      rt1.BlendEnable = BlendIsIdentity(key.blend_control1) ? FALSE : TRUE;
      rt1.SrcBlend = BlendFactor(key.blend_control1 & 0x1Fu);
      rt1.BlendOp = BlendOp((key.blend_control1 >> 5) & 0x7u);
      rt1.DestBlend = BlendFactor((key.blend_control1 >> 8) & 0x1Fu);
      rt1.SrcBlendAlpha = BlendFactorAlpha((key.blend_control1 >> 16) & 0x1Fu);
      rt1.BlendOpAlpha = BlendOp((key.blend_control1 >> 21) & 0x7u);
      rt1.DestBlendAlpha = BlendFactorAlpha((key.blend_control1 >> 24) & 0x1Fu);
      if (rt1.BlendOp == D3D12_BLEND_OP_MIN || rt1.BlendOp == D3D12_BLEND_OP_MAX) {
        rt1.SrcBlend = D3D12_BLEND_ONE;
        rt1.DestBlend = D3D12_BLEND_ONE;
      }
      if (rt1.BlendOpAlpha == D3D12_BLEND_OP_MIN || rt1.BlendOpAlpha == D3D12_BLEND_OP_MAX) {
        rt1.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt1.DestBlendAlpha = D3D12_BLEND_ONE;
      }
      rt1.RenderTargetWriteMask = UINT8((key.color_mask >> 4) & 0xFu);
    }
  }
  desc.DSVFormat = DXGI_FORMAT(key.ds_format);

  wchar_t pso_name[128] = {};
  if (pipeline_library_) {
    // The name has to mean the same pipeline on every boot, and layout_id does
    // not: streaming changes the order layouts first appear in, so a pipeline
    // the file already held came back under a new name and was compiled again
    // (measured: 103 of the 154 a boot needed, all differing only in the id).
    // The layout's content hash stands in for the id, in the name and the hash.
    PsoKey stable = key;
    stable.layout_id = 0;
    const uint64_t layout_hash = g_layout_registry.ContentHash(key.layout_id);
    uint64_t name_hash = PsoKeyHash{}(stable);
    HashCombine(name_hash, layout_hash);
    // So does the rest of what the runtime compares on load. A description
    // that differs from the stored one fails the load, and the store after the
    // compile is refused because the name is taken, so the pipeline is never
    // cached again (measured: 0 of 153 loaded and 128 refused, root signature
    // 1.0 against a file written under 1.1). The version follows
    // mcla_native_gfx_own_device, and a rebuilt shader pack can change the
    // DXIL under the same identity.
    HashCombine(name_hash, root_signature_version_);
    HashCombine(name_hash, BytecodeHash(vs));
    HashCombine(name_hash, BytecodeHash(ps));
    swprintf_s(pso_name, L"pso_%016llx_%016llx_%016llx_%08x_%08x_%016llx",
               (unsigned long long)key.vs_identity,
               (unsigned long long)key.ps_identity,
               (unsigned long long)layout_hash, key.rt_format, key.ds_format,
               (unsigned long long)name_hash);
    Microsoft::WRL::ComPtr<ID3D12PipelineState> lib_pso;
    HRESULT hr = pipeline_library_->LoadGraphicsPipeline(pso_name, &desc, IID_PPV_ARGS(&lib_pso));
    if (SUCCEEDED(hr)) {
      ++stats_.library_loads;
      auto [inserted, ok] = pipelines_.emplace(key, std::move(lib_pso));
      mru_key_ = key;
      mru_pso_ = inserted->second.Get();
      return mru_pso_;
    }
  }

  Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
  if (FAILED(context.device()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)))) {
    REXLOG_ERROR("[native_gfx] PSO creation failed (vs {:016X} ps {:016X} rt {} ds {} samples {})",
                 key.vs_identity, key.ps_identity, key.rt_format, key.ds_format,
                 key.sample_count);
    // D3D12 returns E_INVALIDARG with no detail. Bisect the description by
    // retrying with one suspect field neutralized at a time; whichever retry
    // succeeds names the offending field without needing the debug layer.
    const auto probe = [&](const char* what, auto&& mutate) {
      D3D12_GRAPHICS_PIPELINE_STATE_DESC d = desc;
      mutate(d);
      Microsoft::WRL::ComPtr<ID3D12PipelineState> tmp;
      if (SUCCEEDED(context.device()->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&tmp)))) {
        REXLOG_ERROR("[native_gfx]   -> succeeds when {} is neutralized", what);
        return true;
      }
      return false;
    };
    bool found = false;
    found |= probe("the pixel shader (removed)", [](auto& d) { d.PS = {nullptr, 0}; });
    found |= probe("the input layout (emptied)", [](auto& d) { d.InputLayout = {nullptr, 0}; });
    found |= probe("MSAA (sample count forced to 1)", [](auto& d) { d.SampleDesc.Count = 1; });
    found |= probe("the depth-stencil format (set to UNKNOWN)", [](auto& d) {
      d.DSVFormat = DXGI_FORMAT_UNKNOWN;
      d.DepthStencilState.DepthEnable = FALSE;
      d.DepthStencilState.StencilEnable = FALSE;
    });
    found |= probe("the render target format (forced R8G8B8A8_UNORM)",
                   [](auto& d) { d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; });
    found |= probe("blending (disabled)",
                   [](auto& d) { d.BlendState.RenderTarget[0].BlendEnable = FALSE; });
    if (!found) {
      REXLOG_ERROR("[native_gfx]   -> no single field explains it; enable the d3d12_debug cvar "
                   "for the validation-layer message");
    }
    ++stats_.creation_failures;
    return nullptr;
  }

  if (pipeline_library_) {
    HRESULT hr = pipeline_library_->StorePipeline(pso_name, pso.Get());
    if (SUCCEEDED(hr)) {
      library_dirty_ = true;
      ++stats_.library_stores;
    }
  }

  auto [inserted, ok] = pipelines_.emplace(key, std::move(pso));
  mru_key_ = key;
  mru_pso_ = inserted->second.Get();
  return mru_pso_;
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET
