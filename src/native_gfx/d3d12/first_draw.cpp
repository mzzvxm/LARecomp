#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — first real draw.
// See first_draw.h for why this renders to its own target instead of
// presenting.

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "first_draw.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/ui/d3d12/d3d12_util.h>

#include "../draw_slicing.h"
#include "../geometry.h"
#include "../guest/guest_constants.h"
#include "../guest/guest_resources.h"
#include "../guest/render_state.h"
#include "../shader_identity.h"
#include "constant_upload.h"
#include "context.h"
#include "pipeline_cache.h"
#include "resource_cache.h"
#include "shader_db.h"
#include "texture_binding.h"
#include "texture_cache.h"

namespace mcla::native_gfx {

namespace {

constexpr uint32_t kWidth = 1280;
constexpr uint32_t kHeight = 720;
// Must match the CreateCommittedResource optimized clear value exactly, or
// D3D12 warns and drops the fast clear path.
constexpr float kClearColor[4] = {0.02f, 0.02f, 0.04f, 1.0f};
bool g_done = false;
FILE* g_log = nullptr;

inline uint32_t R32(const uint8_t* base, uint32_t ea) {
  if (ea < 0x1000u) {
    return 0;
  }
  uint32_t v;
  std::memcpy(&v, rex::memory::GuestPtr(const_cast<uint8_t*>(base), ea), 4);
  return __builtin_bswap32(v);
}

float HalfToFloat(uint16_t h) {
  const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
  if (e == 0) {
    return (s ? -1.0f : 1.0f) * float(m) / 1024.0f * 6.103515625e-5f;
  }
  if (e == 31) {
    return s ? -65504.0f : 65504.0f;
  }
  return (s ? -1.0f : 1.0f) * (1.0f + float(m) / 1024.0f) *
         ((e >= 15) ? float(1u << (e - 15)) : 1.0f / float(1u << (15 - e)));
}

inline uint8_t ToByte(float v) {
  const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
  return uint8_t(c * 255.0f + 0.5f);
}

// Writes an uncompressed 32-bit TGA. Deliberately dependency-free.
void WriteTga(const std::filesystem::path& path, uint32_t w, uint32_t h, const uint8_t* bgra) {
  FILE* f = std::fopen(path.string().c_str(), "wb");
  if (!f) {
    return;
  }
  uint8_t hdr[18] = {};
  hdr[2] = 2;  // uncompressed true-colour
  hdr[12] = uint8_t(w & 0xFF);
  hdr[13] = uint8_t(w >> 8);
  hdr[14] = uint8_t(h & 0xFF);
  hdr[15] = uint8_t(h >> 8);
  hdr[16] = 32;
  hdr[17] = 0x20;  // top-left origin
  std::fwrite(hdr, 1, sizeof(hdr), f);
  std::fwrite(bgra, 1, size_t(w) * h * 4, f);
  std::fclose(f);
}

// Reads the guest data the draw will actually consume, applying the same swap
// the buffer upload applies, and reports the model-space bounds. If the
// positions are absurd the problem is upstream of the GPU; if they are sane
// the problem is state or transform. Without this the two are indistinguishable
// from a black image.
void DumpGuestGeometry(const GeometrySnapshot& geom, FILE* log) {
  if (!log || geom.streams.empty()) {
    return;
  }
  const InputElement* pos = nullptr;
  for (const InputElement& e : geom.input_layout) {
    if (e.semantic_name && std::strcmp(e.semantic_name, "POSITION") == 0 &&
        e.semantic_index == 0) {
      pos = &e;
      break;
    }
  }
  if (!pos || pos->dxgi_format != DXGI_FORMAT_R32G32B32_FLOAT) {
    std::fprintf(log, "\nGuest vertex data:\n  POSITION0 is %s; skipping the dump\n",
                 pos ? "not R32G32B32_FLOAT" : "absent");
    return;
  }
  const VertexStream& s = geom.streams[pos->input_slot];
  const uint8_t* vb = TranslatePhysicalGuest(s.guest_base);
  if (!vb || s.stride == 0) {
    std::fprintf(log, "\nGuest vertex data:\n  stream not translatable\n");
    return;
  }
  const uint32_t vertex_count = s.guest_size / s.stride;
  const uint32_t sample = vertex_count < 256 ? vertex_count : 256;
  std::fprintf(log, "\nGuest vertex data (stream %u, %u vertices, sampling %u):\n",
               pos->input_slot, vertex_count, sample);

  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  uint32_t non_finite = 0;
  for (uint32_t v = 0; v < sample; ++v) {
    float p[3];
    for (uint32_t c = 0; c < 3; ++c) {
      uint32_t raw;
      std::memcpy(&raw, vb + size_t(v) * s.stride + pos->aligned_byte_offset + c * 4, 4);
      raw = __builtin_bswap32(raw);  // same k8in32 swap the upload applies
      std::memcpy(&p[c], &raw, 4);
      if (!(p[c] > -1e30f && p[c] < 1e30f)) {
        ++non_finite;
      } else {
        if (p[c] < lo[c]) lo[c] = p[c];
        if (p[c] > hi[c]) hi[c] = p[c];
      }
    }
    if (v < 6) {
      std::fprintf(log, "  v[%u] POSITION0 = %12.4f %12.4f %12.4f\n", v, p[0], p[1], p[2]);
    }
  }
  std::fprintf(log, "  model-space bounds: x [%.4f .. %.4f]  y [%.4f .. %.4f]  z [%.4f .. %.4f]\n",
               lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
  std::fprintf(log, "  non-finite components: %u\n", non_finite);

  if (geom.indexed && geom.index_guest_base) {
    const uint8_t* ib = TranslatePhysicalGuest(geom.index_guest_base);
    if (ib && !geom.index_32bit) {
      uint32_t max_index = 0;
      const uint32_t n = geom.index_buffer_bytes / 2;
      std::fprintf(log, "  first indices:");
      for (uint32_t i = 0; i < n; ++i) {
        uint16_t idx;
        std::memcpy(&idx, ib + size_t(i) * 2, 2);
        idx = uint16_t((idx >> 8) | (idx << 8));  // k8in16
        if (idx > max_index) max_index = idx;
        if (i < 12) std::fprintf(log, " %u", idx);
      }
      std::fprintf(log, "\n  max index = %u  (vertices available = %u) %s\n", max_index,
                   vertex_count, max_index < vertex_count ? "in range" : "OUT OF RANGE");
    }
  }
}

// Reads the four rows of one matrix out of the constant bank.
void DumpMatrix(FILE* log, const uint8_t* bank, const char* name, uint32_t first_register) {
  if (!log) {
    return;
  }
  std::fprintf(log, "  %s (c%u..c%u):\n", name, first_register, first_register + 3);
  for (uint32_t r = 0; r < 4; ++r) {
    float v[4];
    std::memcpy(v, bank + size_t(first_register + r) * 16, 16);
    std::fprintf(log, "    [%u] %12.5f %12.5f %12.5f %12.5f\n", r, v[0], v[1], v[2], v[3]);
  }
}

#define LOGF(...)                     \
  do {                                \
    if (g_log) {                      \
      std::fprintf(g_log, __VA_ARGS__); \
      std::fflush(g_log);             \
    }                                 \
  } while (0)

}  // namespace

bool FirstRealDrawDone() { return g_done; }

bool TryFirstRealDraw(const uint8_t* base, uint32_t dev, uint32_t primitive_type,
                      uint32_t element_count, uint32_t start_element, int32_t base_vertex,
                      bool indexed, D3D12Context& context, ShaderDatabase& shaders,
                      BufferCache& buffers, TextureCache& textures, TextureBinder& binder,
                      PipelineCache& pipelines) {
  if (g_done) {
    return true;
  }

  // --- selection: only a draw that exercises the whole pipeline is useful.
  if (!indexed || element_count == 0) {
    return false;
  }
  if (PrimitiveTypeToTopology(primitive_type) == 0) {
    return false;  // needs index expansion; not the first draw to try
  }
  const uint32_t vs_obj = R32(base, dev + kDevVertexShaderOffset);
  const uint32_t ps_obj = R32(base, dev + kDevPixelShaderOffset);
  if (!vs_obj || !ps_obj) {
    return false;  // want a draw with both stages
  }

  const GeometrySnapshot geom =
      BuildGeometrySnapshot(base, dev, primitive_type, element_count, start_element, base_vertex,
                            indexed, shaders, nullptr, nullptr, nullptr);
  if (!geom.complete || geom.streams.empty() || !geom.unsupplied.empty()) {
    return false;
  }

  const GuestRenderState rs = ReadRenderState(base, dev);
  const uint32_t rt_format = ColorRenderTargetFormatToDxgi(rs.color_format);
  const uint32_t ds_format = DepthRenderTargetFormatToDxgi(rs.depth_format);
  if (rt_format == DXGI_FORMAT_UNKNOWN) {
    return false;
  }
  // Reject the auxiliary passes that run before the main scene. The first
  // draw of a frame is a 256x256 reflection (xCityLOD__VSCityLODReflect),
  // whose matrix mirrors the world: winding is inverted there by construction,
  // so it says nothing about whether the pipeline is correct. Shadow maps and
  // cube faces are small for the same reason. Judge the pipeline on a
  // full-size pass instead.
  const HostViewport selection_viewport = ComputeHostViewport(rs);
  if (selection_viewport.width < 640.0f || selection_viewport.height < 360.0f) {
    return false;
  }

  // Shader identities + bytecode.
  const ShaderUcodeRef vsr =
      ReadVertexShaderUcode(base, vs_obj, SelectVertexShaderVariant(base, vs_obj, ps_obj));
  const ShaderUcodeRef psr = ReadPixelShaderUcode(base, ps_obj);
  if (!vsr.valid() || !psr.valid() ||
      !IsGuestRangeReadable(vsr.guest_address, vsr.size_bytes) ||
      !IsGuestRangeReadable(psr.guest_address, psr.size_bytes)) {
    return false;
  }
  const uint64_t vs_id = ShaderIdentity(
      reinterpret_cast<const uint8_t*>(
          rex::memory::GuestPtr(const_cast<uint8_t*>(base), vsr.guest_address)),
      vsr.size_bytes);
  const uint64_t ps_id = ShaderIdentity(
      reinterpret_cast<const uint8_t*>(
          rex::memory::GuestPtr(const_cast<uint8_t*>(base), psr.guest_address)),
      psr.size_bytes);
  const uint32_t ps_spec = rs.alpha_test_enable ? 2u : 0u;
  const ShaderBytecode vs_code = shaders.Lookup(vs_id, 0, /*is_pixel=*/false);
  const ShaderBytecode ps_code = shaders.Lookup(ps_id, ps_spec, /*is_pixel=*/true);
  if (!vs_code.valid() || !ps_code.valid()) {
    return false;
  }

  // From here on this IS the attempt; latch so a failure is not retried
  // forever on the render thread.
  g_done = true;

  std::error_code ec;
  auto dir = std::filesystem::current_path(ec);
  if (ec) {
    dir = std::filesystem::path(".");
  }
  g_log = std::fopen((dir / "mcla_native_gfx_firstdraw.txt").string().c_str(), "wb");

  LOGF("=== FIRST REAL DRAW ===\n\n");
  LOGF("Shader:\n  vs_identity=%016llX  ucode=0x%08X %uB  dxil=%uB\n",
       (unsigned long long)vs_id, vsr.guest_address, vsr.size_bytes, vs_code.size);
  LOGF("  ps_identity=%016llX  ucode=0x%08X %uB  dxil=%uB  spec=%u\n\n",
       (unsigned long long)ps_id, psr.guest_address, psr.size_bytes, ps_code.size, ps_spec);

  LOGF("Geometry:\n  primitive_type=%u topology=%u element_count=%u start=%u base_vertex=%d\n",
       primitive_type, PrimitiveTypeToTopology(primitive_type), element_count, start_element,
       base_vertex);
  LOGF("  index_format=%s index_guest=0x%08X bytes=%u\n",
       geom.index_32bit ? "R32_UINT" : "R16_UINT", geom.index_guest_base,
       geom.index_buffer_bytes);
  for (size_t i = 0; i < geom.input_layout.size(); ++i) {
    const InputElement& e = geom.input_layout[i];
    LOGF("  element[%zu] %s%u slot=%u offset=%u dxgi=%u\n", i, e.semantic_name, e.semantic_index,
         e.input_slot, e.aligned_byte_offset, e.dxgi_format);
  }

  LOGF("\nRender Target:\n  rt_format=%u ds_format=%u %ux%u sample_count=%u\n", rt_format,
       ds_format, kWidth, kHeight, SampleCountFromMsaa(rs.msaa_samples));

  LOGF("\nRender State:\n  depth_control=0x%08X depth[enable=%d write=%d func=%u]\n",
       rs.depth_control, rs.depth_enable ? 1 : 0, rs.depth_write ? 1 : 0, rs.depth_func);
  LOGF("  stencil[enable=%d backface=%d ref=%u read=0x%02X write=0x%02X]\n",
       rs.stencil_enable ? 1 : 0, rs.backface_enable ? 1 : 0, rs.stencil_ref, rs.stencil_read_mask,
       rs.stencil_write_mask);
  LOGF("  stencil front[func=%u fail=%u zpass=%u zfail=%u] back[func=%u fail=%u zpass=%u "
       "zfail=%u]\n",
       rs.stencil_func, rs.stencil_fail, rs.stencil_zpass, rs.stencil_zfail, rs.stencil_func_bf,
       rs.stencil_fail_bf, rs.stencil_zpass_bf, rs.stencil_zfail_bf);
  LOGF("  blend_control0=0x%08X color_control=0x%08X color_mask=0x%08X\n", rs.blend_control0,
       rs.color_control, rs.color_mask);
  LOGF("  pa_su_sc_mode_cntl=0x%08X cull[front=%d back=%d] front_face_cw=%d\n",
       rs.pa_su_sc_mode_cntl, rs.cull_front ? 1 : 0, rs.cull_back ? 1 : 0,
       rs.front_face_is_cw ? 1 : 0);
  LOGF("  alpha_test[enable=%d func=%u] edram_mode=%u\n", rs.alpha_test_enable ? 1 : 0,
       rs.alpha_func, rs.edram_mode);
  {
    const HostViewport hv_log = ComputeHostViewport(rs);
    LOGF("  vte_cntl=0x%08X guest viewport: x[scale=%.3f offset=%.3f] y[scale=%.3f offset=%.3f] "
         "z[scale=%.3f offset=%.3f]\n",
         rs.vte_cntl, rs.vport_x_scale, rs.vport_x_offset, rs.vport_y_scale, rs.vport_y_offset,
         rs.vport_z_scale, rs.vport_z_offset);
    LOGF("  -> D3D12 viewport: x=%.2f y=%.2f w=%.2f h=%.2f depth[%.3f..%.3f] y_flipped=%d\n\n",
         hv_log.top_left_x, hv_log.top_left_y, hv_log.width, hv_log.height, hv_log.min_depth,
         hv_log.max_depth, hv_log.y_flipped ? 1 : 0);
  }

  const uint32_t sample_count = SampleCountFromMsaa(rs.msaa_samples);
  ID3D12Device* device = context.device();

  // --- state bisection.
  //
  // A black frame with a successful submit has three possible causes that are
  // indistinguishable from the image alone: the triangles are back-face
  // culled, they are rejected by the depth/stencil test, or the transform puts
  // them off-screen. Rendering the same geometry into three targets, each with
  // one rejection mechanism neutralized, names the cause in a single run.
  //
  // The variants mutate the RAW GUEST REGISTERS in a copy of the PSO key
  // rather than the translated D3D12 state, so each probe still goes through
  // the exact same translation path the real draw uses.
  struct Pass {
    const char* label;
    PsoKey key;
    Microsoft::WRL::ComPtr<ID3D12Resource> rt, ds, resolved, readback;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap, dsv_heap;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 readback_bytes = 0;
    ID3D12PipelineState* pso = nullptr;
  };
  constexpr uint32_t kPassCount = 4;
  Pass passes[kPassCount];
  passes[0].label = "guest state (unmodified)";
  passes[1].label = "culling disabled";
  passes[2].label = "culling + depth/stencil disabled";
  passes[3].label = "winding reversed (culling kept)";

  for (Pass& pass : passes) {
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = kWidth;
    d.Height = kHeight;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT(rt_format);
    d.SampleDesc.Count = sample_count;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv = {};
    cv.Format = d.Format;
    std::memcpy(cv.Color, kClearColor, sizeof(cv.Color));
    if (FAILED(device->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesDefault,
                                               D3D12_HEAP_FLAG_NONE, &d,
                                               D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
                                               IID_PPV_ARGS(&pass.rt)))) {
      LOGF("FAILED: render target creation (fmt %u samples %u)\n", rt_format, sample_count);
      return true;
    }
    d.Format = DXGI_FORMAT(ds_format);
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    cv.Format = d.Format;
    cv.DepthStencil.Depth = 0.0f;  // reverse-Z: the guest clears to 0 and uses GEQUAL
    if (FAILED(device->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesDefault,
                                               D3D12_HEAP_FLAG_NONE, &d,
                                               D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                                               IID_PPV_ARGS(&pass.ds)))) {
      LOGF("FAILED: depth target creation (fmt %u)\n", ds_format);
      return true;
    }
    D3D12_DESCRIPTOR_HEAP_DESC h = {};
    h.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    h.NumDescriptors = 1;
    device->CreateDescriptorHeap(&h, IID_PPV_ARGS(&pass.rtv_heap));
    h.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    device->CreateDescriptorHeap(&h, IID_PPV_ARGS(&pass.dsv_heap));
    device->CreateRenderTargetView(pass.rt.Get(), nullptr,
                                   pass.rtv_heap->GetCPUDescriptorHandleForHeapStart());
    device->CreateDepthStencilView(pass.ds.Get(), nullptr,
                                   pass.dsv_heap->GetCPUDescriptorHandleForHeapStart());
  }

  ID3D12GraphicsCommandList* cl = context.BeginFrame();
  if (!cl) {
    LOGF("FAILED: BeginFrame\n");
    return true;
  }
  // Drop whatever the Xenia backend left in the InfoQueue so the drain after
  // this draw only reports validation messages this draw caused.
  context.ClearDebugMessages();

  // --- geometry resources
  const GeometrySnapshot bound =
      BuildGeometrySnapshot(base, dev, primitive_type, element_count, start_element, base_vertex,
                            indexed, shaders, &buffers, &context, cl);
  if (!bound.complete) {
    const BufferCache::Stats& bs = buffers.stats();
    LOGF("FAILED: geometry resolution: %s\n", bound.failure ? bound.failure : "?");
    LOGF("  BufferCache: hits=%llu uploads=%llu reuploads=%llu merges=%llu "
         "upload_failures=%llu unreadable=%llu\n",
         (unsigned long long)bs.hits, (unsigned long long)bs.uploads,
         (unsigned long long)bs.reuploads, (unsigned long long)bs.merges,
         (unsigned long long)bs.upload_failures, (unsigned long long)bs.unreadable);
    LOGF("  last failure: %s  (addr=0x%08X size=%u)\n",
         bs.last_failure ? bs.last_failure : "(none recorded)", bs.last_failure_addr,
         bs.last_failure_size);
    context.EndFrame();
    return true;
  }
  for (size_t i = 0; i < bound.streams.size(); ++i) {
    const VertexStream& s = bound.streams[i];
    LOGF("  stream[%zu] slot=%u guest=0x%08X size=%u stride=%u endian=%u -> region 0x%08X+%u "
         "view_offset=%u gpu=0x%llX\n",
         i, s.fetch_slot, s.guest_base, s.guest_size, s.stride, s.endian, s.resource_base,
         s.resource_size, s.view_offset, (unsigned long long)s.gpu_address);
  }
  LOGF("  index gpu=0x%llX endian=%u\n", (unsigned long long)bound.index_gpu_address,
       bound.index_endian);
  DumpGuestGeometry(bound, g_log);

  // --- constants
  std::vector<uint8_t> vs_bank(kAluBankBytes), ps_bank(kAluBankBytes);
  const bool vs_ok = ReadConstantBank(base, dev + kDevVsConstantBankOffset, vs_bank.data());
  const bool ps_ok = ReadConstantBank(base, dev + kDevPsConstantBankOffset, ps_bank.data());
  LOGF("\nConstants:\n  vs_bank_read=%d ps_bank_read=%d (%u bytes each)\n", vs_ok ? 1 : 0,
       ps_ok ? 1 : 0, kAluBankBytes);
  // The translated shaders declare gWorld at c0, gWorldViewProj at c8 and
  // gViewInverse at c12 (packoffset in the generated HLSL), so these are the
  // registers the position actually depends on.
  DumpMatrix(g_log, vs_bank.data(), "gWorld", 0);
  DumpMatrix(g_log, vs_bank.data(), "gWorldViewProj", 8);
  DumpMatrix(g_log, vs_bank.data(), "gViewInverse", 12);

  // --- textures + samplers, filling the shared constant tables
  std::vector<uint8_t> shared(kSharedConstantsBytes, 0);
  BoundTexture bound_tex[16];
  uint32_t bound_tex_count = 0;
  binder.BindAll(context, cl, base, dev, textures, shared.data(), bound_tex, &bound_tex_count,
                 16);
  LOGF("\nTextures + Samplers (%u bound):\n", bound_tex_count);
  for (uint32_t i = 0; i < bound_tex_count; ++i) {
    const BoundTexture& b = bound_tex[i];
    const uint32_t tex_byte = SharedTextureIndexByteOffset(0, b.fetch_slot);
    const uint32_t smp_byte = SharedSamplerIndexByteOffset(b.fetch_slot);
    uint32_t written_tex = 0, written_smp = 0;
    std::memcpy(&written_tex, shared.data() + tex_byte, 4);
    std::memcpy(&written_smp, shared.data() + smp_byte, 4);
    LOGF("  slot=%2u guest=0x%08X %ux%u fmt=%u tiled=%u\n", b.fetch_slot, b.fetch.base_address,
         b.fetch.width, b.fetch.height, b.fetch.format, b.fetch.tiled ? 1u : 0u);
    LOGF("      ID3D12Resource=%p  srv_descriptor=%u  sampler_descriptor=%u\n",
         (void*)b.resource, b.srv_descriptor_index, b.sampler_descriptor_index);
    // THE THIRD LINK: what the shader will read must equal what we handed out.
    LOGF("      SharedConstants byte %u = %u  (descriptor index) %s\n", tex_byte, written_tex,
         written_tex == b.srv_descriptor_index ? "== srv_descriptor OK" : "MISMATCH");
    LOGF("      SharedConstants byte %u = %u  (sampler index)    %s\n", smp_byte, written_smp,
         written_smp == b.sampler_descriptor_index ? "== sampler_descriptor OK" : "MISMATCH");
  }

  SharedConstantValues shared_values;
  shared_values.alpha_threshold = rs.alpha_test_enable ? 0.5f : 0.0f;
  ConstantBindings cbv;
  {
    // The shared buffer already holds the descriptor tables; upload it as-is
    // plus the scalars.
    std::memcpy(shared.data() + kSharedBooleansByteOffset, &shared_values.booleans, 4);
    std::memcpy(shared.data() + kSharedAlphaThresholdByteOffset, &shared_values.alpha_threshold,
                4);
    D3D12Context::UploadAlloc a;
    if (!context.AllocateUpload(kAluBankBytes, 256, a)) {
      LOGF("FAILED: vs constant upload\n");
      context.EndFrame();
      return true;
    }
    std::memcpy(a.cpu, vs_bank.data(), kAluBankBytes);
    cbv.vs = a.gpu;
    if (!context.AllocateUpload(kAluBankBytes, 256, a)) {
      LOGF("FAILED: ps constant upload\n");
      context.EndFrame();
      return true;
    }
    std::memcpy(a.cpu, ps_bank.data(), kAluBankBytes);
    cbv.ps = a.gpu;
    if (!context.AllocateUpload(kSharedConstantsBytes, 256, a)) {
      LOGF("FAILED: shared constant upload\n");
      context.EndFrame();
      return true;
    }
    std::memcpy(a.cpu, shared.data(), kSharedConstantsBytes);
    cbv.shared = a.gpu;
  }

  // --- PSO, one per bisection variant.
  passes[0].key = PipelineCache::MakeKey(bound, rs, vs_id, ps_id, 0, ps_spec);
  // PA_SU_SC_MODE_CNTL bits 0/1 are cull_front / cull_back.
  passes[1].key = passes[0].key;
  passes[1].key.pa_su_sc_mode_cntl &= ~0x3u;
  // RB_DEPTHCONTROL bit 0 stencil_enable, bit 1 z_enable, bit 2 z_write_enable.
  passes[2].key = passes[1].key;
  passes[2].key.depth_control &= ~0x7u;
  // Culling kept, only PA_SU_SC_MODE_CNTL.face inverted. If this covers the
  // same pixels as the culling-disabled pass, every triangle faces the same
  // way and the inversion is systematic; a partial match means the mesh is
  // genuinely two-sided and the guest state is right.
  passes[3].key = passes[0].key;
  passes[3].key.pa_su_sc_mode_cntl ^= 0x4u;

  LOGF("\nPSO:\n");
  for (Pass& pass : passes) {
    const uint64_t before_misses = pipelines.stats().misses;
    pass.pso = pipelines.GetOrCreate(context, pass.key, vs_code, ps_code, bound);
    LOGF("  %-34s key_hash=0x%016llX  %s%s\n", pass.label,
         (unsigned long long)PsoKeyHash{}(pass.key),
         pipelines.stats().misses > before_misses ? "MISS (created)" : "HIT",
         pass.pso ? "" : "  -- CREATION FAILED");
  }
  if (!passes[0].pso) {
    LOGF("FAILED: PSO creation for the unmodified guest state\n");
    context.EndFrame();
    return true;
  }

  // --- record: shared bindings once, then one clear+draw per pass.
  // The guest's own viewport, not a full-target assumption: the translated
  // shaders emit raw guest clip space, so the viewport transform is the only
  // place the guest's scale/offset can be reproduced.
  const HostViewport hv = ComputeHostViewport(rs);
  D3D12_VIEWPORT vp = {hv.top_left_x, hv.top_left_y, hv.width,
                       hv.height,     hv.min_depth,  hv.max_depth};
  D3D12_RECT sc = {0, 0, LONG(kWidth), LONG(kHeight)};
  cl->RSSetViewports(1, &vp);
  cl->RSSetScissorRects(1, &sc);

  ID3D12DescriptorHeap* heaps[] = {binder.srv_heap(), binder.sampler_heap()};
  cl->SetDescriptorHeaps(2, heaps);
  cl->SetGraphicsRootSignature(pipelines.root_signature());
  // Stencil reference is command-list state, not part of the PSO, so it has
  // to be set separately or the stencil test compares against 0.
  cl->OMSetStencilRef(rs.stencil_ref);
  cl->OMSetBlendFactor(rs.blend_constant);
  cl->SetGraphicsRootConstantBufferView(kRootVsConstants, cbv.vs);
  cl->SetGraphicsRootConstantBufferView(kRootPsConstants, cbv.ps);
  cl->SetGraphicsRootConstantBufferView(kRootSharedConstants, cbv.shared);
  // One table per texture space; all three index the same SRV heap, and the
  // shader picks the right array via the descriptor index it reads from
  // SharedConstants.
  const D3D12_GPU_DESCRIPTOR_HANDLE srv_base =
      binder.srv_heap()->GetGPUDescriptorHandleForHeapStart();
  cl->SetGraphicsRootDescriptorTable(kRootTexture2DTable, srv_base);
  cl->SetGraphicsRootDescriptorTable(kRootTexture3DTable, srv_base);
  cl->SetGraphicsRootDescriptorTable(kRootTextureCubeTable, srv_base);
  cl->SetGraphicsRootDescriptorTable(kRootSamplerTable,
                                     binder.sampler_heap()->GetGPUDescriptorHandleForHeapStart());

  cl->IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY(PrimitiveTypeToTopology(primitive_type)));
  std::vector<D3D12_VERTEX_BUFFER_VIEW> vbvs;
  for (const VertexStream& vertex_stream : bound.streams) {
    D3D12_VERTEX_BUFFER_VIEW v = {};
    v.BufferLocation = vertex_stream.gpu_address;
    v.SizeInBytes = vertex_stream.guest_size;
    v.StrideInBytes = vertex_stream.stride;
    vbvs.push_back(v);
  }
  cl->IASetVertexBuffers(0, UINT(vbvs.size()), vbvs.data());
  D3D12_INDEX_BUFFER_VIEW ibv = {};
  ibv.BufferLocation = bound.index_gpu_address;
  ibv.SizeInBytes = bound.index_buffer_bytes;
  ibv.Format = bound.index_32bit ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
  cl->IASetIndexBuffer(&ibv);

  LOGF("\nDraw:\n");
  for (Pass& pass : passes) {
    if (!pass.pso) {
      continue;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = pass.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = pass.dsv_heap->GetCPUDescriptorHandleForHeapStart();
    cl->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    cl->ClearRenderTargetView(rtv, kClearColor, 0, nullptr);
    cl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 0,
                              nullptr);
    cl->SetPipelineState(pass.pso);
    DrawSlicer slicer(primitive_type, element_count);
    DrawSlice slice;
    uint32_t n = 0;
    while (slicer.Next(slice)) {
      cl->DrawIndexedInstanced(slice.count, 1, start_element + slice.start, base_vertex, 0);
      ++n;
    }
    LOGF("  %-34s DrawIndexedInstanced(count=%u, start=%u, base_vertex=%d) slices=%u\n",
         pass.label, element_count, start_element, base_vertex, n);
  }

  // --- resolve + readback, per pass
  for (Pass& pass : passes) {
    if (!pass.pso) {
      continue;
    }
    ID3D12Resource* src = pass.rt.Get();
    if (sample_count > 1) {
      D3D12_RESOURCE_DESC d = pass.rt->GetDesc();
      d.SampleDesc.Count = 1;
      d.SampleDesc.Quality = 0;
      // GetDesc() on an MSAA resource reports the 4 MiB MSAA placement
      // alignment. Carrying it over to a single-sampled resource is rejected
      // ("Alignment is invalid. The value is 4194304"); 0 lets the runtime pick.
      d.Alignment = 0;
      d.Flags = D3D12_RESOURCE_FLAG_NONE;
      if (FAILED(device->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesDefault,
                                                 D3D12_HEAP_FLAG_NONE, &d,
                                                 D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr,
                                                 IID_PPV_ARGS(&pass.resolved)))) {
        // Without the resolve target there is nothing valid to copy from: the
        // readback footprint is single-sampled and CopyTextureRegion requires
        // matching sample counts, so attempting it would fail the whole list.
        LOGF("FAILED: resolve target creation for pass '%s'\n", pass.label);
        context.EndFrame();
        context.WaitForIdle();
        context.DrainDebugMessages("first draw (resolve target creation)");
        return true;
      }
      D3D12_RESOURCE_BARRIER b = {};
      b.Transition.pResource = pass.rt.Get();
      b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
      b.Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
      b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      cl->ResourceBarrier(1, &b);
      cl->ResolveSubresource(pass.resolved.Get(), 0, pass.rt.Get(), 0, DXGI_FORMAT(rt_format));
      b.Transition.pResource = pass.resolved.Get();
      b.Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_DEST;
      b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
      cl->ResourceBarrier(1, &b);
      src = pass.resolved.Get();
    } else {
      D3D12_RESOURCE_BARRIER b = {};
      b.Transition.pResource = pass.rt.Get();
      b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
      b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
      b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      cl->ResourceBarrier(1, &b);
    }

    D3D12_RESOURCE_DESC src_desc = src->GetDesc();
    device->GetCopyableFootprints(&src_desc, 0, 1, 0, &pass.footprint, nullptr, nullptr,
                                  &pass.readback_bytes);
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = pass.readback_bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    device->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesReadback,
                                    D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST,
                                    nullptr, IID_PPV_ARGS(&pass.readback));
    if (pass.readback) {
      D3D12_TEXTURE_COPY_LOCATION dst_loc = {}, src_loc = {};
      dst_loc.pResource = pass.readback.Get();
      dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dst_loc.PlacedFootprint = pass.footprint;
      src_loc.pResource = src;
      src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      src_loc.SubresourceIndex = 0;
      cl->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
    }
  }

  const bool submitted = context.EndFrame();
  LOGF("\nSubmit:\n  EndFrame=%s\n", submitted ? "ok" : "FAILED (command list not executed)");
  context.WaitForIdle();
  // Anything the validation layer complained about during recording only
  // exists in the InfoQueue; drain it whether or not the submit succeeded so
  // warnings show up next to the draw they belong to.
  context.DrainDebugMessages("first draw");

  uint64_t drawn_per_pass[kPassCount] = {};
  for (uint32_t pi = 0; pi < kPassCount; ++pi) {
    Pass& pass = passes[pi];
    if (!pass.readback) {
      continue;
    }
    void* mapped = nullptr;
    const D3D12_RANGE range = {0, size_t(pass.readback_bytes)};
    if (FAILED(pass.readback->Map(0, &range, &mapped))) {
      continue;
    }
    std::vector<uint8_t> bgra(size_t(kWidth) * kHeight * 4);
    const auto* rows = static_cast<const uint8_t*>(mapped);
    // Coverage tells apart the three ways this can look like a black image:
    // nothing executed (no clear either), everything culled or transformed
    // off-screen (clear only), or a real draw (pixels differing from clear).
    const uint8_t clear_bgra[4] = {ToByte(kClearColor[2]), ToByte(kClearColor[1]),
                                   ToByte(kClearColor[0]), ToByte(kClearColor[3])};
    uint64_t cleared_pixels = 0, drawn_pixels = 0, black_pixels = 0;
    uint32_t min_x = kWidth, min_y = kHeight, max_x = 0, max_y = 0;
    for (uint32_t y = 0; y < kHeight; ++y) {
      const uint8_t* row = rows + size_t(y) * pass.footprint.Footprint.RowPitch;
      for (uint32_t x = 0; x < kWidth; ++x) {
        float r = 0, g = 0, b = 0, a = 1;
        if (rt_format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
          const auto* px = reinterpret_cast<const uint16_t*>(row) + x * 4;
          r = HalfToFloat(px[0]); g = HalfToFloat(px[1]);
          b = HalfToFloat(px[2]); a = HalfToFloat(px[3]);
        } else {
          const uint8_t* px = row + x * 4;
          r = px[0] / 255.0f; g = px[1] / 255.0f; b = px[2] / 255.0f; a = px[3] / 255.0f;
        }
        uint8_t* o = bgra.data() + (size_t(y) * kWidth + x) * 4;
        o[0] = ToByte(b); o[1] = ToByte(g); o[2] = ToByte(r); o[3] = ToByte(a);
        if (o[0] == 0 && o[1] == 0 && o[2] == 0 && o[3] == 0) {
          ++black_pixels;
        } else if (std::memcmp(o, clear_bgra, 4) == 0) {
          ++cleared_pixels;
        } else {
          ++drawn_pixels;
          if (x < min_x) min_x = x;
          if (x > max_x) max_x = x;
          if (y < min_y) min_y = y;
          if (y > max_y) max_y = y;
        }
      }
    }
    pass.readback->Unmap(0, nullptr);
    drawn_per_pass[pi] = drawn_pixels;

    char name[128];
    std::snprintf(name, sizeof(name), "mcla_native_gfx_firstdraw%s.tga",
                  pi == 0 ? "" : (pi == 1 ? "_nocull" : (pi == 2 ? "_nocull_nodepth" : "_reversed")));
    WriteTga(dir / name, kWidth, kHeight, bgra.data());
    const double total_px = double(kWidth) * kHeight;
    LOGF("\n  [%s] -> %s\n", pass.label, name);
    LOGF("    drawn=%llu (%.3f%%)  clear=%llu (%.3f%%)  untouched_black=%llu (%.3f%%)\n",
         (unsigned long long)drawn_pixels, 100.0 * double(drawn_pixels) / total_px,
         (unsigned long long)cleared_pixels, 100.0 * double(cleared_pixels) / total_px,
         (unsigned long long)black_pixels, 100.0 * double(black_pixels) / total_px);
    if (drawn_pixels) {
      LOGF("    bounding box: x %u..%u  y %u..%u\n", min_x, max_x, min_y, max_y);
    }
  }

  LOGF("\nVERDICT:\n");
  if (drawn_per_pass[0]) {
    LOGF("  the unmodified guest state rasterizes; the pipeline is end to end connected\n");
  } else if (drawn_per_pass[3] && drawn_per_pass[1] &&
             drawn_per_pass[3] * 10 >= drawn_per_pass[1] * 9) {
    LOGF("  reversing the winding recovers %.1f%% of what disabling culling does -> every\n"
         "  triangle faces the same way, so the inversion is SYSTEMATIC, not a two-sided\n"
         "  mesh. Something between clip space and screen space mirrors an axis.\n",
         100.0 * double(drawn_per_pass[3]) / double(drawn_per_pass[1]));
  } else if (drawn_per_pass[1]) {
    LOGF("  pixels appear only with culling disabled, and reversing the winding recovers\n"
         "  just %llu of %llu pixels -> the mesh is genuinely two-sided and the guest\n"
         "  state is being translated correctly; this draw is simply back-facing.\n",
         (unsigned long long)drawn_per_pass[3], (unsigned long long)drawn_per_pass[1]);
  } else if (drawn_per_pass[2]) {
    LOGF("  pixels appear only with the depth/stencil test off -> the DEPTH TEST rejects\n"
         "  everything. Check the clear value against the compare function (guest func=%u,\n"
         "  depth cleared to 0.0) and the clip-space Z range.\n",
         rs.depth_func);
  } else {
    LOGF("  no pass produced a pixel with culling AND depth/stencil disabled -> nothing is\n"
         "  rejected downstream: the geometry never reaches the viewport. The remaining\n"
         "  candidates are the vertex TRANSFORM (constants feeding gWorldViewProj) and the\n"
         "  vertex attribute decoding. Compare the model-space bounds dumped above against\n"
         "  the matrix rows.\n");
  }

  LOGF("\n=== FIRST REAL DRAW COMPLETED ===\n");
  REXLOG_INFO("[native_gfx] first real draw executed; see mcla_native_gfx_firstdraw.txt/.tga");
  if (g_log) {
    std::fclose(g_log);
    g_log = nullptr;
  }
  return true;
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET
