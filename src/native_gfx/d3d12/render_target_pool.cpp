#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — render targets and the resolve bridge.
// See render_target_pool.h for the guest-side reverse engineering.

#include "render_target_pool.h"

#include <cstring>
#include <set>
#include <vector>

#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_util.h>

#include "context.h"
#include "image_dump.h"

REXCVAR_DECLARE(uint32_t, mcla_native_gfx_mrt);
namespace mcla::native_gfx {

namespace {

constexpr float kClearColor[4] = {0.02f, 0.02f, 0.04f, 1.0f};

// A depth resource cannot carry both a DSV and an SRV through the same
// format: the resource has to be TYPELESS, with the depth format on the view
// and the readable format on the shader view. Creating it this way means a
// depth resolve needs no conversion pass at all — the depth texture IS the
// resource the fetch resolves to.
//
// D3D12 cannot resolve or copy a multisampled depth surface into a colour
// format, and there is no ResolveSubresource path for depth. The guest's own
// resolve always lands a SINGLE-SAMPLED image in main memory (that is what a
// texture fetch reads; MSAA only ever exists inside EDRAM), so pooled targets
// are created single-sampled. That is a deliberate simplification of the
// intermediate, not of the result.
uint32_t TypelessForDepth(uint32_t ds_dxgi) {
  switch (ds_dxgi) {
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
      return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
      return DXGI_FORMAT_R32G8X24_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT:
      return DXGI_FORMAT_R32_TYPELESS;
    default:
      return DXGI_FORMAT_R24G8_TYPELESS;
  }
}

// The shader-readable view of that typeless depth.
uint32_t ShaderFormatForDepth(uint32_t ds_dxgi) {
  switch (ds_dxgi) {
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
      return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
      return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT:
      return DXGI_FORMAT_R32_FLOAT;
    default:
      return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
  }
}

}  // namespace

void RenderTargetPool::DumpResolved(D3D12Context& context,
                                    const std::filesystem::path& dir, FILE* log) {
  ID3D12Device* device = context.device();

  // Everything is copied in ONE command list with ONE stall. Doing a
  // BeginFrame/EndFrame/WaitForIdle per destination hung the game: this runs
  // on the guest's draw thread inside a hook, so every stall blocks the guest,
  // and runs with the dump on timed out where the same runs without it
  // finished in ~30 s.
  struct Pending {
    uint32_t address = 0;
    ResolvedCopy* copy = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 bytes = 0;
    bool is_depth = false;
  };
  std::vector<Pending> pending;

  ID3D12GraphicsCommandList* cl = context.BeginFrame();
  if (!cl) {
    return;
  }
  for (auto& [addr, r] : resolved_) {
    // TEMP: also dump the tonemap scene input (0x07FE0000) and the 1x1 exposure
    // (0x02D6C000), which the size filter would otherwise skip.
    // TEMP: force-dump the luminance/exposure reduction chain to find the
    // stage where it collapses to zero (which blacks out the tonemap).
    const bool forced = addr == 0x07FE0000u || addr == 0x02D6C000u || addr == 0x073ED000u ||
                        addr == 0x073FD000u || addr == 0x07401000u || addr == 0x02D6D000u;
    if (!r.resource || (!forced && (r.width < 512 || r.height < 512)) || pending.size() >= 8) {
      continue;
    }
    Pending p;
    p.address = addr;
    p.copy = &r;
    const D3D12_RESOURCE_DESC src_desc = r.resource->GetDesc();
    p.is_depth = src_desc.Format == DXGI_FORMAT_R24G8_TYPELESS ||
                 src_desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS ||
                 src_desc.Format == DXGI_FORMAT_R32_TYPELESS;
    device->GetCopyableFootprints(&src_desc, 0, 1, 0, &p.footprint, nullptr, nullptr, &p.bytes);

    D3D12_RESOURCE_DESC rb = {};
    rb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rb.Width = p.bytes;
    rb.Height = 1;
    rb.DepthOrArraySize = 1;
    rb.MipLevels = 1;
    rb.SampleDesc.Count = 1;
    rb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesReadback,
                                               D3D12_HEAP_FLAG_NONE, &rb,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&p.readback)))) {
      continue;
    }

    const bool needs_transition = r.state != D3D12_RESOURCE_STATE_COPY_SOURCE;
    D3D12_RESOURCE_BARRIER b = {};
    b.Transition.pResource = r.resource.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (needs_transition) {
      b.Transition.StateBefore = r.state;
      b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
      cl->ResourceBarrier(1, &b);
    }
    D3D12_TEXTURE_COPY_LOCATION dst_loc = {}, src_loc = {};
    dst_loc.pResource = p.readback.Get();
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst_loc.PlacedFootprint = p.footprint;
    src_loc.pResource = r.resource.Get();
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_loc.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
    if (needs_transition) {
      b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
      b.Transition.StateAfter = r.state;
      cl->ResourceBarrier(1, &b);
    }
    pending.push_back(std::move(p));
  }
  context.EndFrame();
  context.WaitForIdle();

  for (Pending& p : pending) {
    void* mapped = nullptr;
    const D3D12_RANGE range = {0, size_t(p.bytes)};
    if (FAILED(p.readback->Map(0, &range, &mapped))) {
      continue;
    }
    const uint32_t w = p.copy->width, h = p.copy->height;
    std::vector<uint8_t> bgra(size_t(w) * h * 4);
    uint64_t nonzero = 0;
    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    const auto* rows = static_cast<const uint8_t*>(mapped);
    for (uint32_t y = 0; y < h; ++y) {
      const uint8_t* row = rows + size_t(y) * p.footprint.Footprint.RowPitch;
      for (uint32_t x = 0; x < w; ++x) {
        uint32_t v;
        std::memcpy(&v, row + size_t(x) * 4, 4);
        uint8_t* o = bgra.data() + (size_t(y) * w + x) * 4;
        if (p.is_depth) {
          // R24G8: depth in the low 24 bits.
          const uint32_t d24 = v & 0x00FFFFFFu;
          if (d24) ++nonzero;
          if (d24 < lo) lo = d24;
          if (d24 > hi) hi = d24;
          const uint8_t g = uint8_t(d24 >> 16);
          o[0] = o[1] = o[2] = g;
          o[3] = 255;
        } else {
          if (v & 0x00FFFFFFu) ++nonzero;
          o[0] = uint8_t((v >> 16) & 0xFF);
          o[1] = uint8_t((v >> 8) & 0xFF);
          o[2] = uint8_t(v & 0xFF);
          o[3] = 255;
        }
      }
    }
    // TEMP: for the float reduction chain, the low-24-bit non-zero test lies
    // (a float like 0.5f has zero low bytes). Report the actual float values of
    // the first row so a genuine zero can be told from a small positive value.
    if (log) {
      float f0, f1, f2;
      std::memcpy(&f0, rows, 4);
      std::memcpy(&f1, rows + (w > 1 ? 4 : 0), 4);
      std::memcpy(&f2, rows + (w > 2 ? 8 : 0), 4);
      std::fprintf(log, "    [floatprobe] 0x%08X %ux%u  px0=%.6g px1=%.6g px2=%.6g\n", p.address, w,
                   h, f0, f1, f2);
      std::fflush(log);
    }
    p.readback->Unmap(0, nullptr);

    char name[160];
    std::snprintf(name, sizeof(name), "mcla_native_gfx_resolved_%08X_%ux%u_%s.tga", p.address, w,
                  h, p.is_depth ? "depth" : "color");
    WriteBgraTga(dir / name, w, h, bgra.data());
    if (log) {
      const double pct = 100.0 * double(nonzero) / (double(w) * h);
      if (p.is_depth) {
        std::fprintf(log, "    %s  non-zero=%.1f%%  depth24 range [%u .. %u]\n", name, pct,
                     lo == 0xFFFFFFFFu ? 0u : lo, hi);
      } else {
        std::fprintf(log, "    %s  non-zero=%.1f%%\n", name, pct);
      }
      std::fflush(log);
    }
  }
}

bool RenderTargetKey::operator==(const RenderTargetKey& o) const {
  return rt_format == o.rt_format && ds_format == o.ds_format &&
         sample_count == o.sample_count && width == o.width && height == o.height;
}

bool RenderTargetKey::operator<(const RenderTargetKey& o) const {
  if (width != o.width) return width < o.width;
  if (height != o.height) return height < o.height;
  if (rt_format != o.rt_format) return rt_format < o.rt_format;
  if (ds_format != o.ds_format) return ds_format < o.ds_format;
  return sample_count < o.sample_count;
}

void RenderTargetPool::Shutdown(D3D12Context& context) {
  for (auto& [key, t] : targets_) {
    if (t.color) context.DeferRelease(t.color.Detach());
    if (t.color1) context.DeferRelease(t.color1.Detach());
    if (t.depth) context.DeferRelease(t.depth.Detach());
  }
  targets_.clear();
  // Colour copies are owned here; depth entries only borrow the pooled target
  // that was already released above, so releasing them again would double-free.
  for (auto& [addr, r] : resolved_) {
    if (r.resource && r.owned) {
      context.DeferRelease(r.resource.Detach());
    } else {
      r.resource.Detach();
    }
  }
  resolved_.clear();
}

RenderTargetPool::Census RenderTargetPool::TakeCensus(ID3D12Device* device) const {
  Census c;
  c.targets = targets_.size();
  c.target_bytes = stats_.bytes_allocated;
  c.resolved = resolved_.size();
  c.orphaned = orphaned_resources_.size();
  c.pending_copies = pending_copies_.size();
  c.gpu_produced = gpu_produced_.size();
  for (const auto& [addr, r] : resolved_) {
    if (!r.resource || !r.owned) {
      continue;  // borrowed entries are already counted in the pool
    }
    if (device) {
      const D3D12_RESOURCE_DESC desc = r.resource->GetDesc();
      c.resolved_bytes += device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
    }
  }
  return c;
}

bool RenderTargetPool::IsGpuProduced(uint32_t guest_address, uint32_t width,
                                     uint32_t height) const {
  auto it = gpu_produced_.find(guest_address & 0x1FFFFFFFu);
  if (it == gpu_produced_.end()) {
    return false;
  }
  // The extent has to match what was resolved there. Without this the test is
  // just "this address was once a resolve destination", which stays true after
  // the guest reuses the memory for something else.
  return it->second.width == width && it->second.height == height;
}

bool RenderTargetPool::IsStaleGpuAddress(uint32_t guest_address, uint32_t width,
                                         uint32_t height) const {
  auto it = gpu_produced_.find(guest_address & 0x1FFFFFFFu);
  return it != gpu_produced_.end() &&
         (it->second.width != width || it->second.height != height);
}

void RenderTargetPool::NoteDestination(uint32_t dest_address, uint32_t width,
                                       uint32_t height) {
  if (dest_address && width && height) {
    gpu_produced_[dest_address & 0x1FFFFFFFu] = ProducedExtent{width, height};
  }
}

RenderTarget* RenderTargetPool::Find(const RenderTargetKey& key) {
  auto it = targets_.find(key);
  return it == targets_.end() ? nullptr : &it->second;
}

void RenderTargetPool::NoteResolve(RenderTarget& source, bool from_depth,
                                   uint32_t dest_address, uint32_t dest_width,
                                   uint32_t dest_height, const ResolveRegion& region,
                                   uint32_t color_index) {
  // Target 1 only exists on a pass that declared it. A resolve naming it on a
  // pass that has one surface would otherwise fall through to target 0 and
  // duplicate it, which is the bug this parameter exists to stop.
  if (color_index == 1 && !source.color1) {
    ++stats_.resolves;
    return;
  }
  ++stats_.resolves;
  if (from_depth) {
    ++stats_.resolves_depth;
  }
  ID3D12Resource* src = from_depth ? source.depth.Get() : source.color.Get();
  // TEMP DIAG (remove after): the composite's exposure input at 0x02D6C000 misses
  // the RT lookup; trace every resolve targeting it (or failing to).
  // TEMP DIAG (remove after): every DISTINCT resolve destination, once each.
  // Scoping this to the single exposure address answered "is the exposure
  // resolved" but could not answer the question that followed it: which of the
  // composite's inputs the native runtime resolves AT ALL. A screen-sized
  // k_8_8_8_8 input (0x06ACD000) reaches the composite as a guest-memory decode,
  // and only this list can say whether that is a missing resolve or a failed
  // registration.
  {
    static std::set<uint32_t> seen;
    if (seen.insert(dest_address).second) {
      if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
        std::fprintf(f, "RESOLVE_DEST dest=0x%08X %ux%u from_depth=%d src_fmt=%u\n", dest_address,
                     dest_width, dest_height, from_depth ? 1 : 0,
                     from_depth ? source.depth_shader_format : source.key.rt_format);
        std::fflush(f);
        std::fclose(f);
      }
    }
  }
  if (dest_address == 0x02D6C000u) {
    if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
      std::fprintf(f, "NOTE_RESOLVE dest=0x%08X %ux%u from_depth=%d src=%p src_fmt=%u\n",
                   dest_address, dest_width, dest_height, from_depth ? 1 : 0, (void*)src,
                   from_depth ? source.depth_shader_format : source.key.rt_format);
      std::fflush(f);
      std::fclose(f);
    }
  }
  if (!src || dest_address == 0 || dest_width == 0 || dest_height == 0) {
    return;
  }

  // The destination is its own texture, sized as the FETCH sees it. Handing
  // back the source instead cannot work when they differ: the shadow map
  // resolves 640x640 cascades into a 1280x1280 atlas, and a lookup for the
  // atlas would be rejected for being larger than what was registered.
  // The destination's format has to be recreated too, not just its size: the
  // guest reuses one address for resolves of different formats (an R8G8B8A8
  // colour target and, later, the R16G16B16A16 HDR target), and a stale
  // resource made for the first format makes CopyTextureRegion fail with
  // "source and destination resource formats are incompatible", which aborts
  // the whole command list. Continuous mode, running many frames, hits this.
  const uint32_t want_format =
      from_depth ? source.depth_shader_format
                 : (color_index == 1 ? source.rt1_format : source.key.rt_format);
  auto it = resolved_.find(dest_address);
  if (it == resolved_.end() || it->second.width != dest_width ||
      it->second.height != dest_height || it->second.from_depth != from_depth ||
      it->second.dxgi_format != want_format) {
    if (it != resolved_.end()) {
      if (it->second.resource && it->second.owned) {
        // Retire (do NOT leak): the guest reused this address with a different
        // format/size. No context here to defer-release, so park it; the next
        // FlushPendingCopies drains this list through the fence-gated
        // DeferRelease. The old Detach()-and-leak grew VRAM without bound
        // (measured 12 GB on a 4 GB card).
        orphaned_resources_.push_back(std::move(it->second.resource));
      }
      resolved_.erase(it);
    }
    ResolvedCopy copy;
    copy.width = dest_width;
    copy.height = dest_height;
    copy.dxgi_format = want_format;
    copy.state = D3D12_RESOURCE_STATE_COPY_DEST;
    copy.owned = true;
    copy.from_depth = from_depth;
    // Created lazily on the next flush, where a device is available.
    it = resolved_.emplace(dest_address, std::move(copy)).first;
    ++stats_.resolve_copies_created;
  }

  // A resolve must not submit work (hundreds per frame, on the queue shared
  // with the Xenia command processor), so the copy is queued and issued on the
  // next draw's command list.
  pending_copies_.push_back(PendingCopy{&source, dest_address, region, from_depth});
}

void RenderTargetPool::FlushPendingCopies(D3D12Context& context,
                                          ID3D12GraphicsCommandList* cl) {
  // Drain resolve destinations retired by NoteResolve (format/size reuse) even
  // if there is nothing to copy this call; DeferRelease is fence-gated so this
  // is safe against in-flight command lists and keeps VRAM bounded.
  if (!orphaned_resources_.empty()) {
    for (auto& r : orphaned_resources_) {
      context.DeferRelease(r.Detach());
    }
    orphaned_resources_.clear();
  }
  if (!cl || pending_copies_.empty()) {
    return;
  }
  for (const PendingCopy& pc : pending_copies_) {
    auto it = resolved_.find(pc.dest_address);
    if (it == resolved_.end() || !pc.source) {
      continue;
    }
    const bool from_color1 = !pc.from_depth && pc.color_index == 1 && pc.source->color1 &&
                             (REXCVAR_GET(mcla_native_gfx_mrt) & 0x10u);
    ID3D12Resource* src_res = pc.from_depth ? pc.source->depth.Get()
                                            : (from_color1 ? pc.source->color1.Get()
                                                           : pc.source->color.Get());
    if (!src_res) {
      continue;
    }
    ResolvedCopy& dst = it->second;

    // A different-format resolve may have targeted this SAME address earlier in
    // this same flush and already created dst.resource. Copying the current
    // source (a different format) into it is exactly the failure the debug layer
    // reported: CopyTextureRegion "source and destination formats are
    // incompatible" (e.g. R8G8B8A8_TYPELESS into R16G16B16A16_TYPELESS), which
    // aborts Close() and permanently freezes the capture. If the existing
    // resource's format no longer matches this copy's source, retire it
    // (fence-gated; the earlier copy still references it) and recreate below.
    const uint32_t want_dxgi = pc.from_depth ? TypelessForDepth(pc.source->key.ds_format)
                                             : pc.source->key.rt_format;
    if (dst.resource && dst.resource->GetDesc().Format != DXGI_FORMAT(want_dxgi)) {
      context.DeferRelease(dst.resource.Detach());
      dst.dxgi_format = want_dxgi;
    }

    if (!dst.resource) {
      D3D12_RESOURCE_DESC d = {};
      d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      d.Width = dst.width;
      d.Height = dst.height;
      d.DepthOrArraySize = 1;
      d.MipLevels = 1;
      // The destination must be format-compatible with the source or the copy
      // is rejected: the same typeless family for depth, the identical colour
      // format otherwise.
      d.Format = DXGI_FORMAT(pc.from_depth ? TypelessForDepth(pc.source->key.ds_format)
                                           : pc.source->key.rt_format);
      d.SampleDesc.Count = 1;
      if (FAILED(context.device()->CreateCommittedResource(
              &rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &d,
              D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&dst.resource)))) {
        REXLOG_ERROR("[native_gfx] resolve destination creation failed ({:#010x} {}x{})",
                     pc.dest_address, dst.width, dst.height);
        continue;
      }
      dst.state = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    const uint32_t w = pc.region.width ? pc.region.width : pc.source->key.width;
    const uint32_t h = pc.region.height ? pc.region.height : pc.source->key.height;
    if (w == 0 || h == 0) {
      continue;
    }

    D3D12_RESOURCE_BARRIER barriers[2] = {};
    uint32_t barrier_count = 0;
    const D3D12_RESOURCE_STATES src_state =
        pc.from_depth ? pc.source->depth_state : pc.source->color_state;
    if (src_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
      barriers[barrier_count].Transition.pResource = src_res;
      barriers[barrier_count].Transition.StateBefore = src_state;
      barriers[barrier_count].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
      barriers[barrier_count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      ++barrier_count;
    }
    if (dst.state != D3D12_RESOURCE_STATE_COPY_DEST) {
      barriers[barrier_count].Transition.pResource = dst.resource.Get();
      barriers[barrier_count].Transition.StateBefore = dst.state;
      barriers[barrier_count].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
      barriers[barrier_count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      ++barrier_count;
    }
    if (barrier_count) {
      cl->ResourceBarrier(barrier_count, barriers);
    }
    if (pc.from_depth) {
      pc.source->depth_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    } else {
      pc.source->color_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    dst.state = D3D12_RESOURCE_STATE_COPY_DEST;

    D3D12_TEXTURE_COPY_LOCATION dst_loc = {}, src_loc = {};
    dst_loc.pResource = dst.resource.Get();
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_loc.SubresourceIndex = 0;
    src_loc.pResource = src_res;
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_loc.SubresourceIndex = 0;
    D3D12_BOX box = {};
    box.left = uint32_t(pc.region.src_x < 0 ? 0 : pc.region.src_x);
    box.top = uint32_t(pc.region.src_y < 0 ? 0 : pc.region.src_y);
    box.front = 0;
    box.right = box.left + w;
    box.bottom = box.top + h;
    box.back = 1;
    // Clamp to both surfaces; a rect that runs past either one is rejected.
    if (box.right > pc.source->key.width) box.right = pc.source->key.width;
    if (box.bottom > pc.source->key.height) box.bottom = pc.source->key.height;
    const uint32_t dx = uint32_t(pc.region.dst_x < 0 ? 0 : pc.region.dst_x);
    const uint32_t dy = uint32_t(pc.region.dst_y < 0 ? 0 : pc.region.dst_y);
    if (box.right <= box.left || box.bottom <= box.top || dx >= dst.width ||
        dy >= dst.height) {
      continue;
    }
    if (dx + (box.right - box.left) > dst.width) {
      box.right = box.left + (dst.width - dx);
    }
    if (dy + (box.bottom - box.top) > dst.height) {
      box.bottom = box.top + (dst.height - dy);
    }
    cl->CopyTextureRegion(&dst_loc, dx, dy, 0, &src_loc, &box);

    D3D12_RESOURCE_BARRIER to_srv = {};
    to_srv.Transition.pResource = dst.resource.Get();
    to_srv.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    to_srv.Transition.StateAfter = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    to_srv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &to_srv);
    dst.state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;

    // A colour target is bound again by the next draw of its pass, so it has
    // to go back to RENDER_TARGET; leaving it in COPY_SOURCE would be an
    // invalid bind. Depth is restored by PrepareForRendering instead.
    if (!pc.from_depth) {
      D3D12_RESOURCE_BARRIER back = {};
      back.Transition.pResource = src_res;
      back.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
      back.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
      back.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      cl->ResourceBarrier(1, &back);
      pc.source->color_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
  }
  pending_copies_.clear();
}

void RenderTargetPool::FlushPendingTransitions(ID3D12GraphicsCommandList* cl) {
  if (!cl) {
    return;
  }
  for (RenderTarget* t : pending_to_shader_) {
    if (!t->depth || t->depth_state == D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE) {
      continue;
    }
    D3D12_RESOURCE_BARRIER b = {};
    b.Transition.pResource = t->depth.Get();
    b.Transition.StateBefore = t->depth_state;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &b);
    t->depth_state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
  }
  pending_to_shader_.clear();
}

void RenderTargetPool::MarkAllUncleared() {
  for (auto& [key, target] : targets_) {
    target.cleared = false;
  }
}

bool RenderTargetPool::EnsureSecondTarget(D3D12Context& context, RenderTarget& target,
                                          uint32_t dxgi_format) {
  if (dxgi_format == 0) {
    return false;
  }
  if (target.color1 && target.rt1_format == dxgi_format) {
    return true;
  }
  if (target.color1) {
    // The guest reused this shape with a different second format. Retire the
    // old surface through the fence-gated queue rather than dropping it while a
    // submitted copy may still read it.
    context.DeferRelease(target.color1.Detach());
  }
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = target.key.width;
  d.Height = target.key.height;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = DXGI_FORMAT(dxgi_format);
  d.SampleDesc.Count = target.key.sample_count;
  d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE cv = {};
  cv.Format = d.Format;
  std::memcpy(cv.Color, kClearColor, sizeof(cv.Color));
  const HRESULT hr = context.device()->CreateCommittedResource(
      &rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &d,
      D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&target.color1));
  if (FAILED(hr)) {
    context.ReportCreateFailure("pooled render target 1", hr);
    REXLOG_ERROR("[native_gfx] second render target creation failed ({}x{} fmt {})",
                 target.key.width, target.key.height, dxgi_format);
    target.rt1_format = 0;
    return false;
  }
  target.rt1_format = dxgi_format;
  target.color1_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv1 = target.rtv_heap->GetCPUDescriptorHandleForHeapStart();
  rtv1.ptr += target.rtv_descriptor_size;
  context.device()->CreateRenderTargetView(target.color1.Get(), nullptr, rtv1);
  // A freshly attached surface has never been cleared; make the next draw of
  // this pass run the clear path so it does not start from driver garbage.
  target.cleared = false;
  return true;
}

void RenderTargetPool::PrepareForRendering(ID3D12GraphicsCommandList* cl, RenderTarget& target,
                                           bool depth_read_only) {
  if (!cl) {
    return;
  }
  // Colour back to RENDER_TARGET before it is bound as an RTV (OMSetRenderTargets).
  // In continuous mode PrepareContinuousDisplay leaves the chosen display target's
  // colour in ALL_SHADER_RESOURCE, and a resolve leaves a target in COPY_SOURCE;
  // binding either as an RTV WITHOUT this barrier is an invalid resource state that
  // faults the GPU -> device removal -> the SDK's host-GPU-loss handler calls
  // FatalError/abort (0xC0000409 subcode 7). This only began crashing once the
  // capture stopped dying mid-frame (the resolve-format fix), so display targets
  // are reused as render targets again the next frame. PrepareForRendering ran
  // depth-only before, so the colour was left in the wrong state.
  if (target.color && target.color_state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
    // TEMP DIAG (remove after): prove the invalid-state RTV rebind actually fires
    // and with which prior state (ALL_SHADER_RESOURCE == the display path).
    {
      static unsigned n = 0;
      if (n++ < 8) {
        if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
          std::fprintf(f, "RTV_REBIND color_state=0x%X -> RENDER_TARGET %ux%u\n",
                       uint32_t(target.color_state), target.key.width, target.key.height);
          std::fflush(f);
          std::fclose(f);
        }
      }
    }
    D3D12_RESOURCE_BARRIER c = {};
    c.Transition.pResource = target.color.Get();
    c.Transition.StateBefore = target.color_state;
    c.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    c.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &c);
    target.color_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
  }
  // Target 1 is bound as an RTV in the same call as target 0, so it needs the
  // same state; a resolve leaves it in COPY_SOURCE.
  if (target.color1 && (REXCVAR_GET(mcla_native_gfx_mrt) & 0x8u) &&
      target.color1_state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
    D3D12_RESOURCE_BARRIER c1 = {};
    c1.Transition.pResource = target.color1.Get();
    c1.Transition.StateBefore = target.color1_state;
    c1.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    c1.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &c1);
    target.color1_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
  }
  // A pass that samples this same depth buffer needs a state that is readable
  // from a shader, which DEPTH_WRITE is not. DEPTH_READ|ALL_SHADER_RESOURCE is,
  // and it pairs with the read-only DSV the caller binds in that case.
  const D3D12_RESOURCE_STATES want_depth =
      depth_read_only ? (D3D12_RESOURCE_STATE_DEPTH_READ |
                         D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE)
                      : D3D12_RESOURCE_STATE_DEPTH_WRITE;
  if (target.depth && target.depth_state != want_depth) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Transition.pResource = target.depth.Get();
    b.Transition.StateBefore = target.depth_state;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &b);
    target.depth_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
  }
}

RenderTarget* RenderTargetPool::Acquire(D3D12Context& context, const RenderTargetKey& key,
                                        float clear_depth) {
  auto it = targets_.find(key);
  if (it != targets_.end()) {
    return &it->second;
  }

  // Bytes this target would cost: colour (widest format assumed 8 B/px) plus
  // depth (4 B/px). Approximate on purpose — it is a budget, not an allocator.
  const uint64_t cost = uint64_t(key.width) * key.height * 12ull;
  if (targets_.size() >= kMaxTargets || stats_.bytes_allocated + cost > kMaxTargetBytes) {
    if (++stats_.targets_refused <= 4) {
      REXLOG_ERROR("[native_gfx] render target budget reached ({} targets, {} MiB); "
                   "refusing {}x{}",
                   targets_.size(), stats_.bytes_allocated / (1024 * 1024), key.width,
                   key.height);
    }
    return nullptr;
  }

  RenderTarget t;
  t.key = key;
  t.clear_depth = clear_depth;
  ID3D12Device* device = context.device();

  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = key.width;
  d.Height = key.height;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = DXGI_FORMAT(key.rt_format);
  d.SampleDesc.Count = key.sample_count;
  d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE cv = {};
  cv.Format = d.Format;
  std::memcpy(cv.Color, kClearColor, sizeof(cv.Color));
  const HRESULT hr_color = device->CreateCommittedResource(
      &rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &d,
      D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&t.color));
  if (FAILED(hr_color)) {
    context.ReportCreateFailure("pooled render target", hr_color);
    REXLOG_ERROR("[native_gfx] render target creation failed ({}x{} fmt {} samples {})",
                 key.width, key.height, key.rt_format, key.sample_count);
    return nullptr;
  }

  // Typeless so the same resource can be a depth target and a shader
  // resource; the DSV below supplies the depth format explicitly.
  d.Format = DXGI_FORMAT(TypelessForDepth(key.ds_format));
  d.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  cv.Format = DXGI_FORMAT(key.ds_format);
  // Must match what the pass actually clears to, or D3D12 drops the fast
  // clear path and warns.
  cv.DepthStencil.Depth = clear_depth;
  cv.DepthStencil.Stencil = 0;
  if (FAILED(device->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesDefault,
                                             D3D12_HEAP_FLAG_NONE, &d,
                                             D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                                             IID_PPV_ARGS(&t.depth)))) {
    REXLOG_ERROR("[native_gfx] depth target creation failed ({}x{} fmt {})", key.width,
                 key.height, key.ds_format);
    return nullptr;
  }

  D3D12_DESCRIPTOR_HEAP_DESC h = {};
  h.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  // Slot 1 is reserved for the second colour target, which EnsureSecondTarget
  // attaches later; one spare RTV descriptor per pooled target is free.
  h.NumDescriptors = 2;
  device->CreateDescriptorHeap(&h, IID_PPV_ARGS(&t.rtv_heap));
  t.rtv_descriptor_size =
      device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  h.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  device->CreateDescriptorHeap(&h, IID_PPV_ARGS(&t.dsv_heap));
  device->CreateRenderTargetView(t.color.Get(), nullptr,
                                 t.rtv_heap->GetCPUDescriptorHandleForHeapStart());
  // A typeless resource needs the format spelled out on the view.
  D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
  dsv.Format = DXGI_FORMAT(key.ds_format);
  dsv.ViewDimension = key.sample_count > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS
                                           : D3D12_DSV_DIMENSION_TEXTURE2D;
  device->CreateDepthStencilView(t.depth.Get(), &dsv,
                                 t.dsv_heap->GetCPUDescriptorHandleForHeapStart());
  t.depth_shader_format = ShaderFormatForDepth(key.ds_format);

  ++stats_.targets_created;
  stats_.bytes_allocated += cost;
  auto [inserted, ok] = targets_.emplace(key, std::move(t));
  return &inserted->second;
}

void RenderTargetPool::RecordResolve(D3D12Context& context, ID3D12GraphicsCommandList* cl,
                                     RenderTarget& source, bool from_depth,
                                     uint32_t dest_address, uint32_t width, uint32_t height) {
  ++stats_.resolves;
  if (from_depth) {
    ++stats_.resolves_depth;
  }
  if (!cl || dest_address == 0 || width == 0 || height == 0) {
    return;
  }

  ID3D12Resource* src = from_depth ? source.depth.Get() : source.color.Get();
  if (!src) {
    return;
  }
  if (from_depth) {
    // The typeless depth target already holds exactly what the fetch wants, so
    // it is registered as-is instead of being copied. It has to leave
    // DEPTH_WRITE first: a resource sampled while still in that state is a
    // GPU fault, not a validation warning.
    if (source.depth_state != D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE) {
      D3D12_RESOURCE_BARRIER b = {};
      b.Transition.pResource = src;
      b.Transition.StateBefore = source.depth_state;
      b.Transition.StateAfter = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
      b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      cl->ResourceBarrier(1, &b);
      source.depth_state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    }
    RegisterDirect(dest_address, src, source.key.width, source.key.height,
                   source.depth_shader_format, source.depth_state);
    return;
  }
  const D3D12_RESOURCE_DESC src_desc = src->GetDesc();
  const uint32_t dxgi = source.key.rt_format;

  auto it = resolved_.find(dest_address);
  if (it == resolved_.end() || it->second.width != width || it->second.height != height ||
      it->second.dxgi_format != dxgi) {
    if (it != resolved_.end() && it->second.resource) {
      context.DeferRelease(it->second.resource.Detach());
      resolved_.erase(it);
    }
    ResolvedCopy copy;
    copy.width = width;
    copy.height = height;
    copy.dxgi_format = dxgi;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT(dxgi);
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (FAILED(context.device()->CreateCommittedResource(
            &rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&copy.resource)))) {
      REXLOG_ERROR("[native_gfx] resolve copy creation failed ({:#010x} {}x{} fmt {})",
                   dest_address, width, height, dxgi);
      return;
    }
    copy.state = D3D12_RESOURCE_STATE_COPY_DEST;
    copy.owned = true;
    ++stats_.resolve_copies_created;
    it = resolved_.emplace(dest_address, std::move(copy)).first;
  }

  ResolvedCopy& dst = it->second;
  if (dst.state != D3D12_RESOURCE_STATE_COPY_DEST &&
      dst.state != D3D12_RESOURCE_STATE_RESOLVE_DEST) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Transition.pResource = dst.resource.Get();
    b.Transition.StateBefore = dst.state;
    b.Transition.StateAfter = src_desc.SampleDesc.Count > 1
                                  ? D3D12_RESOURCE_STATE_RESOLVE_DEST
                                  : D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &b);
    dst.state = b.Transition.StateAfter;
  }

  D3D12_RESOURCE_BARRIER sb = {};
  sb.Transition.pResource = src;
  sb.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  sb.Transition.StateAfter = src_desc.SampleDesc.Count > 1
                                 ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE
                                 : D3D12_RESOURCE_STATE_COPY_SOURCE;
  sb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  cl->ResourceBarrier(1, &sb);

  if (src_desc.SampleDesc.Count > 1) {
    if (dst.state != D3D12_RESOURCE_STATE_RESOLVE_DEST) {
      D3D12_RESOURCE_BARRIER b = {};
      b.Transition.pResource = dst.resource.Get();
      b.Transition.StateBefore = dst.state;
      b.Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_DEST;
      b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      cl->ResourceBarrier(1, &b);
      dst.state = D3D12_RESOURCE_STATE_RESOLVE_DEST;
    }
    cl->ResolveSubresource(dst.resource.Get(), 0, src, 0, DXGI_FORMAT(dxgi));
  } else {
    cl->CopyResource(dst.resource.Get(), src);
  }

  // Leave both sides in the states the next user expects.
  sb.Transition.StateBefore = sb.Transition.StateAfter;
  sb.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  cl->ResourceBarrier(1, &sb);

  D3D12_RESOURCE_BARRIER db = {};
  db.Transition.pResource = dst.resource.Get();
  db.Transition.StateBefore = dst.state;
  db.Transition.StateAfter = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
  db.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  cl->ResourceBarrier(1, &db);
  dst.state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
}

void RenderTargetPool::RegisterDirect(uint32_t dest_address, ID3D12Resource* resource,
                                      uint32_t width, uint32_t height, uint32_t shader_format,
                                      D3D12_RESOURCE_STATES state) {
  if (!resource || dest_address == 0) {
    return;
  }
  ResolvedCopy& c = resolved_[dest_address];
  if (!c.resource) {
    ++stats_.resolve_copies_created;
  }
  c.resource = resource;  // borrowed: the pool owns it through targets_
  c.owned = false;
  c.width = width;
  c.height = height;
  c.dxgi_format = shader_format;
  c.state = state;
}

ID3D12Resource* RenderTargetPool::FindResolvedTarget(uint32_t guest_address, uint32_t width,
                                                     uint32_t height, bool want_depth,
                                                     D3D12_RESOURCE_STATES* out_state) {
  // TEMP DIAG (remove after): why the exposure input 0x02D6C000 misses.
  if (guest_address == 0x02D6C000u) {
    static unsigned n = 0;
    ++n;
    // See the matching note in texture_cache.cpp: six samples only ever showed
    // the first frame, where NO_ENTRY is expected.
    if (n <= 6 || (n % 600) == 0) {
      if (FILE* f = std::fopen("native_gfx_diag.txt", "ab")) {
        auto e = resolved_.find(guest_address);
        std::fprintf(f, "FIND 0x02D6C000 w=%u h=%u want_depth=%d -> %s",
                     width, height, want_depth ? 1 : 0,
                     e == resolved_.end() ? "NO_ENTRY\n" : "");
        if (e != resolved_.end()) {
          std::fprintf(f, "entry %ux%u fmt=%u from_depth=%d res=%p\n", e->second.width,
                       e->second.height, e->second.dxgi_format, e->second.from_depth ? 1 : 0,
                       (void*)e->second.resource.Get());
        }
        std::fflush(f);
        std::fclose(f);
      }
    }
  }
  auto it = resolved_.find(guest_address);
  if (it == resolved_.end()) {
    ++stats_.lookup_misses;
    ++stats_.miss_no_entry;
    return nullptr;
  }
  if (!it->second.resource) {
    ++stats_.lookup_misses;
    ++stats_.miss_no_resource;
    return nullptr;
  }
  // Kind mismatch is COUNTED but not rejected: reading a depth resolve through
  // a colour fetch is a deliberate Xbox 360 idiom (a shader unpacks the depth
  // buffer as k_8_8_8_8), and k_32_FLOAT is used for BOTH colour render targets
  // (the exposure/bloom chain) and depth reads, so the fetch format cannot tell
  // the two apart. An earlier version rejected on mismatch and broke the whole
  // format-36 bloom chain, turning the composite black. The guest aliases the
  // address on purpose; matching its aliasing is the correct behaviour.
  if (it->second.from_depth != want_depth) {
    ++stats_.miss_kind_mismatch;
  }
  // Size is not required to match exactly: the guest samples a shadow atlas
  // with the atlas dimensions while each resolve fills one tile of it. A
  // mismatch that is not a superset would be a different resource, though.
  if (width > it->second.width || height > it->second.height) {
    ++stats_.lookup_misses;
    ++stats_.miss_too_small;
    return nullptr;
  }
  ++stats_.lookup_hits;
  if (out_state) {
    *out_state = it->second.state;
  }
  return it->second.resource.Get();
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET
