#pragma once
// ===========================================================================
// MCLA Native Graphics Runtime — recording command list
// ===========================================================================
// An ID3D12GraphicsCommandList that records every call into a byte stream
// instead of issuing it, and replays the stream onto a real command list
// later.
//
// It exists so the D3D12 half of a frame -- the command recording, the Close,
// the submission -- can be moved off the guest's render thread without moving
// any of the logic that decides WHAT is recorded. Every cache, every state
// tracker and every bridge decision keeps running exactly where and in exactly
// the order it runs today; the only thing that changes is when the driver hears
// about it. Replayed in the same thread right before Close, the result is the
// same command list call for call, which is the property the first step has to
// prove before a second thread is involved.
//
// Every argument that is a pointer is deep-copied at record time: the callers
// pass stack arrays (barriers, viewports, vertex buffer views) that are gone by
// the time the stream is replayed. Resource and object pointers are stored as
// they are -- a real command list does not hold references either, and the
// runtime already keeps anything recorded alive until the GPU has finished with
// it (DeferRelease).
// ===========================================================================

#include <cstdint>
#include <cstring>
#include <vector>

#include <rex/ui/d3d12/d3d12_api.h>

namespace mcla::native_gfx {

class RecordingCommandList final : public ID3D12GraphicsCommandList {
 public:
  void BeginRecording(ID3D12Device* device) {
    device_ = device;
    stream_.clear();
    commands_ = 0;
  }
  size_t command_count() const { return commands_; }
  size_t byte_size() const { return stream_.size(); }
  // Calls the replay had no way to express faithfully. Nothing in the runtime
  // makes them; a non-zero count means a new call site needs support here.
  uint64_t unsupported() const { return unsupported_; }

  // Issues everything recorded since BeginRecording onto `target`, in order.
  void Replay(ID3D12GraphicsCommandList* target) const;

  // ---- IUnknown ----------------------------------------------------------
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (!out) {
      return E_POINTER;
    }
    if (riid == __uuidof(ID3D12GraphicsCommandList) || riid == __uuidof(ID3D12CommandList) ||
        riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(IUnknown)) {
      *out = static_cast<ID3D12GraphicsCommandList*>(this);
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  // Owned by D3D12Context for its whole life; nothing may free it.
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }

  // ---- ID3D12Object / ID3D12DeviceChild / ID3D12CommandList -------------
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** out) override {
    return device_ ? device_->QueryInterface(riid, out) : E_FAIL;
  }
  D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override {
    return D3D12_COMMAND_LIST_TYPE_DIRECT;
  }

  // ---- ID3D12GraphicsCommandList ----------------------------------------
  // The context opens and closes the real list; callers never do.
  HRESULT STDMETHODCALLTYPE Close() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator*, ID3D12PipelineState*) override {
    return S_OK;
  }

  void STDMETHODCALLTYPE ClearState(ID3D12PipelineState* pso) override {
    Op(kClearState);
    Put(pso);
  }
  void STDMETHODCALLTYPE DrawInstanced(UINT a, UINT b, UINT c, UINT d) override {
    Op(kDrawInstanced);
    Put(a), Put(b), Put(c), Put(d);
  }
  void STDMETHODCALLTYPE DrawIndexedInstanced(UINT a, UINT b, UINT c, INT d, UINT e) override {
    Op(kDrawIndexedInstanced);
    Put(a), Put(b), Put(c), Put(d), Put(e);
  }
  void STDMETHODCALLTYPE Dispatch(UINT x, UINT y, UINT z) override {
    Op(kDispatch);
    Put(x), Put(y), Put(z);
  }
  void STDMETHODCALLTYPE CopyBufferRegion(ID3D12Resource* dst, UINT64 dst_off, ID3D12Resource* src,
                                          UINT64 src_off, UINT64 n) override {
    Op(kCopyBufferRegion);
    Put(dst), Put(dst_off), Put(src), Put(src_off), Put(n);
  }
  void STDMETHODCALLTYPE CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y,
                                           UINT z, const D3D12_TEXTURE_COPY_LOCATION* src,
                                           const D3D12_BOX* box) override {
    Op(kCopyTextureRegion);
    Put(*dst), Put(x), Put(y), Put(z), Put(*src);
    PutOpt(box);
  }
  void STDMETHODCALLTYPE CopyResource(ID3D12Resource* dst, ID3D12Resource* src) override {
    Op(kCopyResource);
    Put(dst), Put(src);
  }
  void STDMETHODCALLTYPE CopyTiles(ID3D12Resource* tiled, const D3D12_TILED_RESOURCE_COORDINATE* start,
                                   const D3D12_TILE_REGION_SIZE* size, ID3D12Resource* buffer,
                                   UINT64 offset, D3D12_TILE_COPY_FLAGS flags) override {
    Op(kCopyTiles);
    Put(tiled), Put(*start), Put(*size), Put(buffer), Put(offset), Put(flags);
  }
  void STDMETHODCALLTYPE ResolveSubresource(ID3D12Resource* dst, UINT dst_sub, ID3D12Resource* src,
                                            UINT src_sub, DXGI_FORMAT format) override {
    Op(kResolveSubresource);
    Put(dst), Put(dst_sub), Put(src), Put(src_sub), Put(format);
  }
  void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY t) override {
    Op(kIASetPrimitiveTopology);
    Put(t);
  }
  void STDMETHODCALLTYPE RSSetViewports(UINT n, const D3D12_VIEWPORT* v) override {
    Op(kRSSetViewports);
    PutArray(n, v);
  }
  void STDMETHODCALLTYPE RSSetScissorRects(UINT n, const D3D12_RECT* r) override {
    Op(kRSSetScissorRects);
    PutArray(n, r);
  }
  void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT factor[4]) override {
    Op(kOMSetBlendFactor);
    PutOptArray(factor ? 4u : 0u, factor);
  }
  void STDMETHODCALLTYPE OMSetStencilRef(UINT ref) override {
    Op(kOMSetStencilRef);
    Put(ref);
  }
  void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState* pso) override {
    Op(kSetPipelineState);
    Put(pso);
  }
  void STDMETHODCALLTYPE ResourceBarrier(UINT n, const D3D12_RESOURCE_BARRIER* b) override {
    Op(kResourceBarrier);
    PutArray(n, b);
  }
  void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList* bundle) override {
    Op(kExecuteBundle);
    Put(bundle);
  }
  void STDMETHODCALLTYPE SetDescriptorHeaps(UINT n, ID3D12DescriptorHeap* const* heaps) override {
    Op(kSetDescriptorHeaps);
    PutArray(n, heaps);
  }
  void STDMETHODCALLTYPE SetComputeRootSignature(ID3D12RootSignature* rs) override {
    Op(kSetComputeRootSignature);
    Put(rs);
  }
  void STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature* rs) override {
    Op(kSetGraphicsRootSignature);
    Put(rs);
  }
  void STDMETHODCALLTYPE SetComputeRootDescriptorTable(UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) override {
    Op(kSetComputeRootDescriptorTable);
    Put(i), Put(h);
  }
  void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(UINT i,
                                                        D3D12_GPU_DESCRIPTOR_HANDLE h) override {
    Op(kSetGraphicsRootDescriptorTable);
    Put(i), Put(h);
  }
  void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT i, UINT data, UINT off) override {
    Op(kSetComputeRoot32BitConstant);
    Put(i), Put(data), Put(off);
  }
  void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(UINT i, UINT data, UINT off) override {
    Op(kSetGraphicsRoot32BitConstant);
    Put(i), Put(data), Put(off);
  }
  void STDMETHODCALLTYPE SetComputeRoot32BitConstants(UINT i, UINT n, const void* data,
                                                      UINT off) override {
    Op(kSetComputeRoot32BitConstants);
    Put(i), Put(off);
    PutArray(n, static_cast<const uint32_t*>(data));
  }
  void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(UINT i, UINT n, const void* data,
                                                       UINT off) override {
    Op(kSetGraphicsRoot32BitConstants);
    Put(i), Put(off);
    PutArray(n, static_cast<const uint32_t*>(data));
  }
  void STDMETHODCALLTYPE SetComputeRootConstantBufferView(UINT i,
                                                          D3D12_GPU_VIRTUAL_ADDRESS a) override {
    Op(kSetComputeRootConstantBufferView);
    Put(i), Put(a);
  }
  void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(UINT i,
                                                           D3D12_GPU_VIRTUAL_ADDRESS a) override {
    Op(kSetGraphicsRootConstantBufferView);
    Put(i), Put(a);
  }
  void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT i,
                                                          D3D12_GPU_VIRTUAL_ADDRESS a) override {
    Op(kSetComputeRootShaderResourceView);
    Put(i), Put(a);
  }
  void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(UINT i,
                                                           D3D12_GPU_VIRTUAL_ADDRESS a) override {
    Op(kSetGraphicsRootShaderResourceView);
    Put(i), Put(a);
  }
  void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT i,
                                                           D3D12_GPU_VIRTUAL_ADDRESS a) override {
    Op(kSetComputeRootUnorderedAccessView);
    Put(i), Put(a);
  }
  void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(UINT i,
                                                            D3D12_GPU_VIRTUAL_ADDRESS a) override {
    Op(kSetGraphicsRootUnorderedAccessView);
    Put(i), Put(a);
  }
  void STDMETHODCALLTYPE IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* view) override {
    Op(kIASetIndexBuffer);
    PutOpt(view);
  }
  void STDMETHODCALLTYPE IASetVertexBuffers(UINT start, UINT n,
                                            const D3D12_VERTEX_BUFFER_VIEW* views) override {
    Op(kIASetVertexBuffers);
    Put(start), Put(n);
    PutOptArray(views ? n : 0u, views);
  }
  void STDMETHODCALLTYPE SOSetTargets(UINT start, UINT n,
                                      const D3D12_STREAM_OUTPUT_BUFFER_VIEW* views) override {
    Op(kSOSetTargets);
    Put(start), Put(n);
    PutOptArray(views ? n : 0u, views);
  }
  void STDMETHODCALLTYPE OMSetRenderTargets(UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs,
                                            BOOL single_range,
                                            const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) override {
    Op(kOMSetRenderTargets);
    Put(n), Put(single_range);
    // A single-handle range passes one handle, the start of the range.
    const UINT handles = rtvs ? (single_range ? (n ? 1u : 0u) : n) : 0u;
    PutOptArray(handles, rtvs);
    PutOpt(dsv);
  }
  void STDMETHODCALLTYPE ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags,
                                               FLOAT depth, UINT8 stencil, UINT n,
                                               const D3D12_RECT* rects) override {
    Op(kClearDepthStencilView);
    Put(dsv), Put(flags), Put(depth), Put(stencil);
    PutOptArray(rects ? n : 0u, rects);
  }
  void STDMETHODCALLTYPE ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT color[4],
                                               UINT n, const D3D12_RECT* rects) override {
    Op(kClearRenderTargetView);
    Put(rtv);
    PutArray(4u, color);
    PutOptArray(rects ? n : 0u, rects);
  }
  void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE gpu,
                                                      D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                                      ID3D12Resource* res, const UINT values[4],
                                                      UINT n, const D3D12_RECT* rects) override {
    Op(kClearUnorderedAccessViewUint);
    Put(gpu), Put(cpu), Put(res);
    PutArray(4u, values);
    PutOptArray(rects ? n : 0u, rects);
  }
  void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE gpu,
                                                       D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                                       ID3D12Resource* res, const FLOAT values[4],
                                                       UINT n, const D3D12_RECT* rects) override {
    Op(kClearUnorderedAccessViewFloat);
    Put(gpu), Put(cpu), Put(res);
    PutArray(4u, values);
    PutOptArray(rects ? n : 0u, rects);
  }
  void STDMETHODCALLTYPE DiscardResource(ID3D12Resource* res,
                                         const D3D12_DISCARD_REGION* region) override {
    Op(kDiscardResource);
    Put(res);
    // The region carries its own rect pointer, so it is flattened.
    const UINT rects = region && region->pRects ? region->NumRects : 0u;
    Put(uint8_t(region ? 1 : 0));
    if (region) {
      Put(region->FirstSubresource), Put(region->NumSubresources);
    }
    PutOptArray(rects, region ? region->pRects : nullptr);
  }
  void STDMETHODCALLTYPE BeginQuery(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT i) override {
    Op(kBeginQuery);
    Put(heap), Put(type), Put(i);
  }
  void STDMETHODCALLTYPE EndQuery(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT i) override {
    Op(kEndQuery);
    Put(heap), Put(type), Put(i);
  }
  void STDMETHODCALLTYPE ResolveQueryData(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT start,
                                          UINT n, ID3D12Resource* dst, UINT64 off) override {
    Op(kResolveQueryData);
    Put(heap), Put(type), Put(start), Put(n), Put(dst), Put(off);
  }
  void STDMETHODCALLTYPE SetPredication(ID3D12Resource* buf, UINT64 off,
                                        D3D12_PREDICATION_OP op) override {
    Op(kSetPredication);
    Put(buf), Put(off), Put(op);
  }
  void STDMETHODCALLTYPE SetMarker(UINT meta, const void* data, UINT size) override {
    Op(kSetMarker);
    Put(meta);
    PutOptArray(data ? size : 0u, static_cast<const uint8_t*>(data));
  }
  void STDMETHODCALLTYPE BeginEvent(UINT meta, const void* data, UINT size) override {
    Op(kBeginEvent);
    Put(meta);
    PutOptArray(data ? size : 0u, static_cast<const uint8_t*>(data));
  }
  void STDMETHODCALLTYPE EndEvent() override { Op(kEndEvent); }
  void STDMETHODCALLTYPE ExecuteIndirect(ID3D12CommandSignature* sig, UINT max, ID3D12Resource* args,
                                         UINT64 args_off, ID3D12Resource* count,
                                         UINT64 count_off) override {
    Op(kExecuteIndirect);
    Put(sig), Put(max), Put(args), Put(args_off), Put(count), Put(count_off);
  }

 private:
  enum OpCode : uint16_t {
    kClearState = 1,
    kDrawInstanced,
    kDrawIndexedInstanced,
    kDispatch,
    kCopyBufferRegion,
    kCopyTextureRegion,
    kCopyResource,
    kCopyTiles,
    kResolveSubresource,
    kIASetPrimitiveTopology,
    kRSSetViewports,
    kRSSetScissorRects,
    kOMSetBlendFactor,
    kOMSetStencilRef,
    kSetPipelineState,
    kResourceBarrier,
    kExecuteBundle,
    kSetDescriptorHeaps,
    kSetComputeRootSignature,
    kSetGraphicsRootSignature,
    kSetComputeRootDescriptorTable,
    kSetGraphicsRootDescriptorTable,
    kSetComputeRoot32BitConstant,
    kSetGraphicsRoot32BitConstant,
    kSetComputeRoot32BitConstants,
    kSetGraphicsRoot32BitConstants,
    kSetComputeRootConstantBufferView,
    kSetGraphicsRootConstantBufferView,
    kSetComputeRootShaderResourceView,
    kSetGraphicsRootShaderResourceView,
    kSetComputeRootUnorderedAccessView,
    kSetGraphicsRootUnorderedAccessView,
    kIASetIndexBuffer,
    kIASetVertexBuffers,
    kSOSetTargets,
    kOMSetRenderTargets,
    kClearDepthStencilView,
    kClearRenderTargetView,
    kClearUnorderedAccessViewUint,
    kClearUnorderedAccessViewFloat,
    kDiscardResource,
    kBeginQuery,
    kEndQuery,
    kResolveQueryData,
    kSetPredication,
    kSetMarker,
    kBeginEvent,
    kEndEvent,
    kExecuteIndirect,
  };

  void Op(OpCode op) {
    ++commands_;
    Put(uint16_t(op));
  }
  template <typename T>
  void Put(const T& v) {
    const size_t at = stream_.size();
    stream_.resize(at + sizeof(T));
    std::memcpy(stream_.data() + at, &v, sizeof(T));
  }
  // Presence byte, then the value if present.
  template <typename T>
  void PutOpt(const T* v) {
    Put(uint8_t(v ? 1 : 0));
    if (v) {
      Put(*v);
    }
  }
  // Count, then that many elements. The count is the element count the caller
  // passed; the pointer must be non-null when it is non-zero.
  template <typename T>
  void PutArray(UINT n, const T* v) {
    Put(n);
    if (n) {
      const size_t at = stream_.size();
      stream_.resize(at + sizeof(T) * n);
      std::memcpy(stream_.data() + at, v, sizeof(T) * n);
    }
  }
  // Like PutArray but the pointer itself may be null, which a replay has to
  // pass through as null rather than as an empty array.
  template <typename T>
  void PutOptArray(UINT n, const T* v) {
    Put(uint8_t(v ? 1 : 0));
    PutArray(v ? n : 0u, v);
  }

  ID3D12Device* device_ = nullptr;
  std::vector<uint8_t> stream_;
  size_t commands_ = 0;
  uint64_t unsupported_ = 0;
};

namespace detail {
// Sequential reader over a recorded stream. Reads mirror the Put* calls one
// for one; every replayed element is copied out, so nothing is read unaligned.
class StreamReader {
 public:
  StreamReader(const uint8_t* p, size_t n) : p_(p), end_(p + n) {}
  bool done() const { return p_ >= end_; }
  template <typename T>
  T Get() {
    T v;
    std::memcpy(&v, p_, sizeof(T));
    p_ += sizeof(T);
    return v;
  }
  // Returns a pointer into scratch holding `n` elements, or null for none.
  template <typename T>
  const T* GetArray(UINT* out_n, std::vector<uint8_t>& scratch) {
    const UINT n = Get<UINT>();
    *out_n = n;
    if (!n) {
      return nullptr;
    }
    scratch.resize(sizeof(T) * n);
    std::memcpy(scratch.data(), p_, sizeof(T) * n);
    p_ += sizeof(T) * n;
    return reinterpret_cast<const T*>(scratch.data());
  }
  template <typename T>
  const T* GetOptArray(UINT* out_n, std::vector<uint8_t>& scratch) {
    const bool present = Get<uint8_t>() != 0;
    UINT n = 0;
    const T* v = GetArray<T>(&n, scratch);
    *out_n = n;
    if (!present) {
      return nullptr;
    }
    // A present-but-empty array still has to be a non-null pointer.
    if (!v) {
      scratch.resize(sizeof(T));
      return reinterpret_cast<const T*>(scratch.data());
    }
    return v;
  }
  template <typename T>
  bool GetOpt(T* out) {
    if (!Get<uint8_t>()) {
      return false;
    }
    *out = Get<T>();
    return true;
  }

 private:
  const uint8_t* p_;
  const uint8_t* end_;
};
}  // namespace detail

inline void RecordingCommandList::Replay(ID3D12GraphicsCommandList* t) const {
  detail::StreamReader r(stream_.data(), stream_.size());
  std::vector<uint8_t> s0, s1;
  UINT n = 0;
  while (!r.done()) {
    switch (OpCode(r.Get<uint16_t>())) {
      case kClearState:
        t->ClearState(r.Get<ID3D12PipelineState*>());
        break;
      case kDrawInstanced: {
        const UINT a = r.Get<UINT>(), b = r.Get<UINT>(), c = r.Get<UINT>(), d = r.Get<UINT>();
        t->DrawInstanced(a, b, c, d);
        break;
      }
      case kDrawIndexedInstanced: {
        const UINT a = r.Get<UINT>(), b = r.Get<UINT>(), c = r.Get<UINT>();
        const INT d = r.Get<INT>();
        const UINT e = r.Get<UINT>();
        t->DrawIndexedInstanced(a, b, c, d, e);
        break;
      }
      case kDispatch: {
        const UINT x = r.Get<UINT>(), y = r.Get<UINT>(), z = r.Get<UINT>();
        t->Dispatch(x, y, z);
        break;
      }
      case kCopyBufferRegion: {
        ID3D12Resource* dst = r.Get<ID3D12Resource*>();
        const UINT64 dst_off = r.Get<UINT64>();
        ID3D12Resource* src = r.Get<ID3D12Resource*>();
        const UINT64 src_off = r.Get<UINT64>();
        const UINT64 bytes = r.Get<UINT64>();
        t->CopyBufferRegion(dst, dst_off, src, src_off, bytes);
        break;
      }
      case kCopyTextureRegion: {
        const auto dst = r.Get<D3D12_TEXTURE_COPY_LOCATION>();
        const UINT x = r.Get<UINT>(), y = r.Get<UINT>(), z = r.Get<UINT>();
        const auto src = r.Get<D3D12_TEXTURE_COPY_LOCATION>();
        D3D12_BOX box;
        const bool has_box = r.GetOpt(&box);
        t->CopyTextureRegion(&dst, x, y, z, &src, has_box ? &box : nullptr);
        break;
      }
      case kCopyResource: {
        ID3D12Resource* dst = r.Get<ID3D12Resource*>();
        ID3D12Resource* src = r.Get<ID3D12Resource*>();
        t->CopyResource(dst, src);
        break;
      }
      case kCopyTiles: {
        ID3D12Resource* tiled = r.Get<ID3D12Resource*>();
        const auto start = r.Get<D3D12_TILED_RESOURCE_COORDINATE>();
        const auto size = r.Get<D3D12_TILE_REGION_SIZE>();
        ID3D12Resource* buffer = r.Get<ID3D12Resource*>();
        const UINT64 off = r.Get<UINT64>();
        const auto flags = r.Get<D3D12_TILE_COPY_FLAGS>();
        t->CopyTiles(tiled, &start, &size, buffer, off, flags);
        break;
      }
      case kResolveSubresource: {
        ID3D12Resource* dst = r.Get<ID3D12Resource*>();
        const UINT dst_sub = r.Get<UINT>();
        ID3D12Resource* src = r.Get<ID3D12Resource*>();
        const UINT src_sub = r.Get<UINT>();
        const DXGI_FORMAT format = r.Get<DXGI_FORMAT>();
        t->ResolveSubresource(dst, dst_sub, src, src_sub, format);
        break;
      }
      case kIASetPrimitiveTopology:
        t->IASetPrimitiveTopology(r.Get<D3D12_PRIMITIVE_TOPOLOGY>());
        break;
      case kRSSetViewports: {
        const D3D12_VIEWPORT* v = r.GetArray<D3D12_VIEWPORT>(&n, s0);
        t->RSSetViewports(n, v);
        break;
      }
      case kRSSetScissorRects: {
        const D3D12_RECT* v = r.GetArray<D3D12_RECT>(&n, s0);
        t->RSSetScissorRects(n, v);
        break;
      }
      case kOMSetBlendFactor: {
        const FLOAT* f = r.GetOptArray<FLOAT>(&n, s0);
        t->OMSetBlendFactor(f);
        break;
      }
      case kOMSetStencilRef:
        t->OMSetStencilRef(r.Get<UINT>());
        break;
      case kSetPipelineState:
        t->SetPipelineState(r.Get<ID3D12PipelineState*>());
        break;
      case kResourceBarrier: {
        const D3D12_RESOURCE_BARRIER* b = r.GetArray<D3D12_RESOURCE_BARRIER>(&n, s0);
        t->ResourceBarrier(n, b);
        break;
      }
      case kExecuteBundle:
        t->ExecuteBundle(r.Get<ID3D12GraphicsCommandList*>());
        break;
      case kSetDescriptorHeaps: {
        ID3D12DescriptorHeap* const* h = r.GetArray<ID3D12DescriptorHeap*>(&n, s0);
        t->SetDescriptorHeaps(n, h);
        break;
      }
      case kSetComputeRootSignature:
        t->SetComputeRootSignature(r.Get<ID3D12RootSignature*>());
        break;
      case kSetGraphicsRootSignature:
        t->SetGraphicsRootSignature(r.Get<ID3D12RootSignature*>());
        break;
      case kSetComputeRootDescriptorTable: {
        const UINT i = r.Get<UINT>();
        t->SetComputeRootDescriptorTable(i, r.Get<D3D12_GPU_DESCRIPTOR_HANDLE>());
        break;
      }
      case kSetGraphicsRootDescriptorTable: {
        const UINT i = r.Get<UINT>();
        t->SetGraphicsRootDescriptorTable(i, r.Get<D3D12_GPU_DESCRIPTOR_HANDLE>());
        break;
      }
      case kSetComputeRoot32BitConstant: {
        const UINT i = r.Get<UINT>(), data = r.Get<UINT>(), off = r.Get<UINT>();
        t->SetComputeRoot32BitConstant(i, data, off);
        break;
      }
      case kSetGraphicsRoot32BitConstant: {
        const UINT i = r.Get<UINT>(), data = r.Get<UINT>(), off = r.Get<UINT>();
        t->SetGraphicsRoot32BitConstant(i, data, off);
        break;
      }
      case kSetComputeRoot32BitConstants: {
        const UINT i = r.Get<UINT>(), off = r.Get<UINT>();
        const uint32_t* d = r.GetArray<uint32_t>(&n, s0);
        t->SetComputeRoot32BitConstants(i, n, d, off);
        break;
      }
      case kSetGraphicsRoot32BitConstants: {
        const UINT i = r.Get<UINT>(), off = r.Get<UINT>();
        const uint32_t* d = r.GetArray<uint32_t>(&n, s0);
        t->SetGraphicsRoot32BitConstants(i, n, d, off);
        break;
      }
      case kSetComputeRootConstantBufferView: {
        const UINT i = r.Get<UINT>();
        t->SetComputeRootConstantBufferView(i, r.Get<D3D12_GPU_VIRTUAL_ADDRESS>());
        break;
      }
      case kSetGraphicsRootConstantBufferView: {
        const UINT i = r.Get<UINT>();
        t->SetGraphicsRootConstantBufferView(i, r.Get<D3D12_GPU_VIRTUAL_ADDRESS>());
        break;
      }
      case kSetComputeRootShaderResourceView: {
        const UINT i = r.Get<UINT>();
        t->SetComputeRootShaderResourceView(i, r.Get<D3D12_GPU_VIRTUAL_ADDRESS>());
        break;
      }
      case kSetGraphicsRootShaderResourceView: {
        const UINT i = r.Get<UINT>();
        t->SetGraphicsRootShaderResourceView(i, r.Get<D3D12_GPU_VIRTUAL_ADDRESS>());
        break;
      }
      case kSetComputeRootUnorderedAccessView: {
        const UINT i = r.Get<UINT>();
        t->SetComputeRootUnorderedAccessView(i, r.Get<D3D12_GPU_VIRTUAL_ADDRESS>());
        break;
      }
      case kSetGraphicsRootUnorderedAccessView: {
        const UINT i = r.Get<UINT>();
        t->SetGraphicsRootUnorderedAccessView(i, r.Get<D3D12_GPU_VIRTUAL_ADDRESS>());
        break;
      }
      case kIASetIndexBuffer: {
        D3D12_INDEX_BUFFER_VIEW v;
        const bool has = r.GetOpt(&v);
        t->IASetIndexBuffer(has ? &v : nullptr);
        break;
      }
      case kIASetVertexBuffers: {
        const UINT start = r.Get<UINT>(), count = r.Get<UINT>();
        const D3D12_VERTEX_BUFFER_VIEW* v = r.GetOptArray<D3D12_VERTEX_BUFFER_VIEW>(&n, s0);
        t->IASetVertexBuffers(start, count, v);
        break;
      }
      case kSOSetTargets: {
        const UINT start = r.Get<UINT>(), count = r.Get<UINT>();
        const D3D12_STREAM_OUTPUT_BUFFER_VIEW* v =
            r.GetOptArray<D3D12_STREAM_OUTPUT_BUFFER_VIEW>(&n, s0);
        t->SOSetTargets(start, count, v);
        break;
      }
      case kOMSetRenderTargets: {
        const UINT count = r.Get<UINT>();
        const BOOL single = r.Get<BOOL>();
        const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs =
            r.GetOptArray<D3D12_CPU_DESCRIPTOR_HANDLE>(&n, s0);
        D3D12_CPU_DESCRIPTOR_HANDLE dsv;
        const bool has_dsv = r.GetOpt(&dsv);
        t->OMSetRenderTargets(count, rtvs, single, has_dsv ? &dsv : nullptr);
        break;
      }
      case kClearDepthStencilView: {
        const auto dsv = r.Get<D3D12_CPU_DESCRIPTOR_HANDLE>();
        const auto flags = r.Get<D3D12_CLEAR_FLAGS>();
        const FLOAT depth = r.Get<FLOAT>();
        const UINT8 stencil = r.Get<UINT8>();
        const D3D12_RECT* rects = r.GetOptArray<D3D12_RECT>(&n, s0);
        t->ClearDepthStencilView(dsv, flags, depth, stencil, n, rects);
        break;
      }
      case kClearRenderTargetView: {
        const auto rtv = r.Get<D3D12_CPU_DESCRIPTOR_HANDLE>();
        UINT four = 0;
        const FLOAT* color = r.GetArray<FLOAT>(&four, s1);
        const D3D12_RECT* rects = r.GetOptArray<D3D12_RECT>(&n, s0);
        t->ClearRenderTargetView(rtv, color, n, rects);
        break;
      }
      case kClearUnorderedAccessViewUint: {
        const auto gpu = r.Get<D3D12_GPU_DESCRIPTOR_HANDLE>();
        const auto cpu = r.Get<D3D12_CPU_DESCRIPTOR_HANDLE>();
        ID3D12Resource* res = r.Get<ID3D12Resource*>();
        UINT four = 0;
        const UINT* values = r.GetArray<UINT>(&four, s1);
        const D3D12_RECT* rects = r.GetOptArray<D3D12_RECT>(&n, s0);
        t->ClearUnorderedAccessViewUint(gpu, cpu, res, values, n, rects);
        break;
      }
      case kClearUnorderedAccessViewFloat: {
        const auto gpu = r.Get<D3D12_GPU_DESCRIPTOR_HANDLE>();
        const auto cpu = r.Get<D3D12_CPU_DESCRIPTOR_HANDLE>();
        ID3D12Resource* res = r.Get<ID3D12Resource*>();
        UINT four = 0;
        const FLOAT* values = r.GetArray<FLOAT>(&four, s1);
        const D3D12_RECT* rects = r.GetOptArray<D3D12_RECT>(&n, s0);
        t->ClearUnorderedAccessViewFloat(gpu, cpu, res, values, n, rects);
        break;
      }
      case kDiscardResource: {
        ID3D12Resource* res = r.Get<ID3D12Resource*>();
        const bool has_region = r.Get<uint8_t>() != 0;
        D3D12_DISCARD_REGION region = {};
        if (has_region) {
          region.FirstSubresource = r.Get<UINT>();
          region.NumSubresources = r.Get<UINT>();
        }
        region.pRects = r.GetOptArray<D3D12_RECT>(&n, s0);
        region.NumRects = n;
        t->DiscardResource(res, has_region ? &region : nullptr);
        break;
      }
      case kBeginQuery: {
        ID3D12QueryHeap* heap = r.Get<ID3D12QueryHeap*>();
        const auto type = r.Get<D3D12_QUERY_TYPE>();
        t->BeginQuery(heap, type, r.Get<UINT>());
        break;
      }
      case kEndQuery: {
        ID3D12QueryHeap* heap = r.Get<ID3D12QueryHeap*>();
        const auto type = r.Get<D3D12_QUERY_TYPE>();
        t->EndQuery(heap, type, r.Get<UINT>());
        break;
      }
      case kResolveQueryData: {
        ID3D12QueryHeap* heap = r.Get<ID3D12QueryHeap*>();
        const auto type = r.Get<D3D12_QUERY_TYPE>();
        const UINT start = r.Get<UINT>(), count = r.Get<UINT>();
        ID3D12Resource* dst = r.Get<ID3D12Resource*>();
        t->ResolveQueryData(heap, type, start, count, dst, r.Get<UINT64>());
        break;
      }
      case kSetPredication: {
        ID3D12Resource* buf = r.Get<ID3D12Resource*>();
        const UINT64 off = r.Get<UINT64>();
        t->SetPredication(buf, off, r.Get<D3D12_PREDICATION_OP>());
        break;
      }
      case kSetMarker: {
        const UINT meta = r.Get<UINT>();
        const uint8_t* d = r.GetOptArray<uint8_t>(&n, s0);
        t->SetMarker(meta, d, n);
        break;
      }
      case kBeginEvent: {
        const UINT meta = r.Get<UINT>();
        const uint8_t* d = r.GetOptArray<uint8_t>(&n, s0);
        t->BeginEvent(meta, d, n);
        break;
      }
      case kEndEvent:
        t->EndEvent();
        break;
      case kExecuteIndirect: {
        ID3D12CommandSignature* sig = r.Get<ID3D12CommandSignature*>();
        const UINT max = r.Get<UINT>();
        ID3D12Resource* args = r.Get<ID3D12Resource*>();
        const UINT64 args_off = r.Get<UINT64>();
        ID3D12Resource* count = r.Get<ID3D12Resource*>();
        t->ExecuteIndirect(sig, max, args, args_off, count, r.Get<UINT64>());
        break;
      }
      default:
        // A corrupt stream: stop rather than feed the driver garbage.
        return;
    }
  }
}

}  // namespace mcla::native_gfx
