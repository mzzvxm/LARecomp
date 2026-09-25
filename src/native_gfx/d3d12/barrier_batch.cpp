#ifndef REXGLUE_HAS_XEO3_TARGET
// ===========================================================================
// MCLA Native Graphics Runtime — D3D12 Enhanced Barriers & Resource Transitions
// ===========================================================================

#include "barrier_batch.h"
#include "device_manager.h"

namespace mcla::native_gfx {

namespace {

void MapLegacyState(D3D12_RESOURCE_STATES state,
                    bool is_before,
                    D3D12_BARRIER_SYNC& out_sync,
                    D3D12_BARRIER_ACCESS& out_access,
                    D3D12_BARRIER_LAYOUT& out_layout) {
  out_sync = D3D12_BARRIER_SYNC_NONE;
  out_access = D3D12_BARRIER_ACCESS_COMMON;
  out_layout = D3D12_BARRIER_LAYOUT_COMMON;

  if (state == D3D12_RESOURCE_STATE_COMMON || state == D3D12_RESOURCE_STATE_PRESENT) {
    out_sync = D3D12_BARRIER_SYNC_ALL;
    out_access = D3D12_BARRIER_ACCESS_COMMON;
    out_layout = (state == D3D12_RESOURCE_STATE_PRESENT) ? D3D12_BARRIER_LAYOUT_PRESENT
                                                         : D3D12_BARRIER_LAYOUT_COMMON;
    return;
  }

  if (state & D3D12_RESOURCE_STATE_RENDER_TARGET) {
    out_sync |= D3D12_BARRIER_SYNC_RENDER_TARGET;
    out_access |= D3D12_BARRIER_ACCESS_RENDER_TARGET;
    out_layout = D3D12_BARRIER_LAYOUT_RENDER_TARGET;
  }
  if (state & D3D12_RESOURCE_STATE_DEPTH_WRITE) {
    out_sync |= D3D12_BARRIER_SYNC_DEPTH_STENCIL;
    out_access |= D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE;
    out_layout = D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE;
  }
  if (state & D3D12_RESOURCE_STATE_DEPTH_READ) {
    out_sync |= D3D12_BARRIER_SYNC_DEPTH_STENCIL;
    out_access |= D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ;
    out_layout = D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ;
  }
  if (state & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
    out_sync |= D3D12_BARRIER_SYNC_ALL_SHADING;
    out_access |= D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
    out_layout = D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
  }
  if (state & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
    out_sync |= D3D12_BARRIER_SYNC_PIXEL_SHADING;
    out_access |= D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
    out_layout = D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
  }
  if (state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
    out_sync |= D3D12_BARRIER_SYNC_NON_PIXEL_SHADING;
    out_access |= D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
    out_layout = D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
  }
  if (state & D3D12_RESOURCE_STATE_COPY_DEST) {
    out_sync |= D3D12_BARRIER_SYNC_COPY;
    out_access |= D3D12_BARRIER_ACCESS_COPY_DEST;
    out_layout = D3D12_BARRIER_LAYOUT_COPY_DEST;
  }
  if (state & D3D12_RESOURCE_STATE_COPY_SOURCE) {
    out_sync |= D3D12_BARRIER_SYNC_COPY;
    out_access |= D3D12_BARRIER_ACCESS_COPY_SOURCE;
    out_layout = D3D12_BARRIER_LAYOUT_COPY_SOURCE;
  }
  if (state & D3D12_RESOURCE_STATE_RESOLVE_DEST) {
    out_sync |= D3D12_BARRIER_SYNC_RESOLVE;
    out_access |= D3D12_BARRIER_ACCESS_RESOLVE_DEST;
    out_layout = D3D12_BARRIER_LAYOUT_RESOLVE_DEST;
  }
  if (state & D3D12_RESOURCE_STATE_RESOLVE_SOURCE) {
    out_sync |= D3D12_BARRIER_SYNC_RESOLVE;
    out_access |= D3D12_BARRIER_ACCESS_RESOLVE_SOURCE;
    out_layout = D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE;
  }
  if (state & D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER) {
    out_sync |= D3D12_BARRIER_SYNC_VERTEX_SHADING;
    out_access |= D3D12_BARRIER_ACCESS_VERTEX_BUFFER | D3D12_BARRIER_ACCESS_CONSTANT_BUFFER;
  }
  if (state & D3D12_RESOURCE_STATE_INDEX_BUFFER) {
    out_sync |= D3D12_BARRIER_SYNC_INDEX_INPUT;
    out_access |= D3D12_BARRIER_ACCESS_INDEX_BUFFER;
  }

  if (out_sync == D3D12_BARRIER_SYNC_NONE) {
    out_sync = D3D12_BARRIER_SYNC_ALL;
  }
  if (out_access == D3D12_BARRIER_ACCESS_COMMON && (state & D3D12_RESOURCE_STATE_GENERIC_READ)) {
    out_sync = D3D12_BARRIER_SYNC_ALL_SHADING | D3D12_BARRIER_SYNC_COPY;
    out_access = D3D12_BARRIER_ACCESS_SHADER_RESOURCE | D3D12_BARRIER_ACCESS_COPY_SOURCE;
    out_layout = D3D12_BARRIER_LAYOUT_GENERIC_READ;
  }
}

}  // namespace

void BarrierBatch::AddTexture(ID3D12Resource* resource,
                              D3D12_RESOURCE_STATES state_before,
                              D3D12_RESOURCE_STATES state_after,
                              UINT subresource) {
  if (!resource || state_before == state_after) {
    return;
  }
  textures_.push_back({resource, state_before, state_after, subresource});
}

void BarrierBatch::AddBuffer(ID3D12Resource* resource,
                             D3D12_RESOURCE_STATES state_before,
                             D3D12_RESOURCE_STATES state_after) {
  if (!resource || state_before == state_after) {
    return;
  }
  buffers_.push_back({resource, state_before, state_after});
}

void BarrierBatch::AddGlobal(D3D12_RESOURCE_STATES state_before,
                             D3D12_RESOURCE_STATES state_after) {
  if (state_before == state_after) {
    return;
  }
  globals_.push_back({state_before, state_after});
}

void BarrierBatch::AddUav(ID3D12Resource* resource) {
  uavs_.push_back(resource);
}

void BarrierBatch::Flush(ID3D12GraphicsCommandList* cl, ID3D12GraphicsCommandList7* cl7) {
  if (!cl || empty()) {
    Clear();
    return;
  }

  const bool enhanced_supported = DeviceManager::Instance().capabilities().enhanced_barriers_supported;
  const bool enhanced_enabled = REXCVAR_GET(mcla_native_gfx_enhanced_barriers);

  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList7> local_cl7;
  if (!cl7 && enhanced_supported && enhanced_enabled) {
    cl->QueryInterface(IID_PPV_ARGS(&local_cl7));
    cl7 = local_cl7.Get();
  }

  if (cl7 && enhanced_supported && enhanced_enabled) {
    // --- Enhanced Barriers Path (ID3D12GraphicsCommandList7) ---
    std::vector<D3D12_TEXTURE_BARRIER> tex_barriers;
    tex_barriers.reserve(textures_.size());
    for (const auto& t : textures_) {
      D3D12_TEXTURE_BARRIER tb = {};
      MapLegacyState(t.state_before, true, tb.SyncBefore, tb.AccessBefore, tb.LayoutBefore);
      MapLegacyState(t.state_after, false, tb.SyncAfter, tb.AccessAfter, tb.LayoutAfter);
      tb.pResource = t.resource;
      if (t.subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
        tb.Subresources.IndexOrFirstMipLevel = 0xFFFFFFFF;
      } else {
        tb.Subresources.IndexOrFirstMipLevel = t.subresource;
        tb.Subresources.NumMipLevels = 1;
        tb.Subresources.NumArraySlices = 1;
        tb.Subresources.NumPlanes = 1;
      }
      tb.Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;
      tex_barriers.push_back(tb);
    }

    std::vector<D3D12_BUFFER_BARRIER> buf_barriers;
    buf_barriers.reserve(buffers_.size());
    for (const auto& b : buffers_) {
      D3D12_BUFFER_BARRIER bb = {};
      D3D12_BARRIER_LAYOUT unused_layout;
      MapLegacyState(b.state_before, true, bb.SyncBefore, bb.AccessBefore, unused_layout);
      MapLegacyState(b.state_after, false, bb.SyncAfter, bb.AccessAfter, unused_layout);
      bb.pResource = b.resource;
      bb.Offset = 0;
      bb.Size = UINT64_MAX;
      buf_barriers.push_back(bb);
    }

    std::vector<D3D12_GLOBAL_BARRIER> glob_barriers;
    glob_barriers.reserve(globals_.size() + uavs_.size());
    for (const auto& g : globals_) {
      D3D12_GLOBAL_BARRIER gb = {};
      D3D12_BARRIER_LAYOUT unused_layout;
      MapLegacyState(g.state_before, true, gb.SyncBefore, gb.AccessBefore, unused_layout);
      MapLegacyState(g.state_after, false, gb.SyncAfter, gb.AccessAfter, unused_layout);
      glob_barriers.push_back(gb);
    }
    for (size_t i = 0; i < uavs_.size(); ++i) {
      D3D12_GLOBAL_BARRIER gb = {};
      gb.SyncBefore = D3D12_BARRIER_SYNC_ALL_SHADING;
      gb.SyncAfter = D3D12_BARRIER_SYNC_ALL_SHADING;
      gb.AccessBefore = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
      gb.AccessAfter = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
      glob_barriers.push_back(gb);
    }

    D3D12_BARRIER_GROUP groups[3] = {};
    UINT num_groups = 0;

    if (!tex_barriers.empty()) {
      groups[num_groups].Type = D3D12_BARRIER_TYPE_TEXTURE;
      groups[num_groups].NumBarriers = static_cast<UINT32>(tex_barriers.size());
      groups[num_groups].pTextureBarriers = tex_barriers.data();
      ++num_groups;
    }
    if (!buf_barriers.empty()) {
      groups[num_groups].Type = D3D12_BARRIER_TYPE_BUFFER;
      groups[num_groups].NumBarriers = static_cast<UINT32>(buf_barriers.size());
      groups[num_groups].pBufferBarriers = buf_barriers.data();
      ++num_groups;
    }
    if (!glob_barriers.empty()) {
      groups[num_groups].Type = D3D12_BARRIER_TYPE_GLOBAL;
      groups[num_groups].NumBarriers = static_cast<UINT32>(glob_barriers.size());
      groups[num_groups].pGlobalBarriers = glob_barriers.data();
      ++num_groups;
    }

    if (num_groups > 0) {
      cl7->Barrier(num_groups, groups);
    }
  } else {
    // --- Legacy ResourceBarrier Fallback ---
    std::vector<D3D12_RESOURCE_BARRIER> legacy;
    legacy.reserve(textures_.size() + buffers_.size() + uavs_.size());

    for (const auto& t : textures_) {
      D3D12_RESOURCE_BARRIER b = {};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
      b.Transition.pResource = t.resource;
      b.Transition.StateBefore = t.state_before;
      b.Transition.StateAfter = t.state_after;
      b.Transition.Subresource = t.subresource;
      legacy.push_back(b);
    }
    for (const auto& bu : buffers_) {
      D3D12_RESOURCE_BARRIER b = {};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
      b.Transition.pResource = bu.resource;
      b.Transition.StateBefore = bu.state_before;
      b.Transition.StateAfter = bu.state_after;
      b.Transition.Subresource = 0;
      legacy.push_back(b);
    }
    for (ID3D12Resource* uav_res : uavs_) {
      D3D12_RESOURCE_BARRIER b = {};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
      b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
      b.UAV.pResource = uav_res;
      legacy.push_back(b);
    }

    if (!legacy.empty()) {
      cl->ResourceBarrier(static_cast<UINT>(legacy.size()), legacy.data());
    }
  }

  Clear();
}

void BarrierBatch::Transition(ID3D12GraphicsCommandList* cl,
                              ID3D12Resource* resource,
                              D3D12_RESOURCE_STATES state_before,
                              D3D12_RESOURCE_STATES state_after,
                              UINT subresource,
                              ID3D12GraphicsCommandList7* cl7) {
  if (!cl || !resource || state_before == state_after) {
    return;
  }
  BarrierBatch batch;
  batch.AddTexture(resource, state_before, state_after, subresource);
  batch.Flush(cl, cl7);
}

void BarrierBatch::TransitionBuffer(ID3D12GraphicsCommandList* cl,
                                    ID3D12Resource* resource,
                                    D3D12_RESOURCE_STATES state_before,
                                    D3D12_RESOURCE_STATES state_after,
                                    ID3D12GraphicsCommandList7* cl7) {
  if (!cl || !resource || state_before == state_after) {
    return;
  }
  BarrierBatch batch;
  batch.AddBuffer(resource, state_before, state_after);
  batch.Flush(cl, cl7);
}

void BarrierBatch::UavBarrier(ID3D12GraphicsCommandList* cl,
                              ID3D12Resource* resource,
                              ID3D12GraphicsCommandList7* cl7) {
  if (!cl) {
    return;
  }
  BarrierBatch batch;
  batch.AddUav(resource);
  batch.Flush(cl, cl7);
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET
