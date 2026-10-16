// MCLA GPU renderer -- the resolve memory. See resolve_memory.h.
//
// Model followed: Xenia's shared memory (ReXGlue SDK src/graphics/
// shared_memory.cpp and the resolve and texture-load shaders it feeds;
// Copyright Ben Vanik and the Xenia authors, BSD-3-Clause): a resolve's bytes
// live in GPU memory, in the guest's layout, and a texture is loaded from
// there. Here it serves only what the resolve products cannot.
// The layout arithmetic is the SDK's texture_util on the CPU and its port in
// hlsl/resolve_memory.hlsli on the GPU.

#include "resolve_memory.h"

#include <algorithm>
#include <cstring>
#include <format>

#include <rex/graphics/pipeline/texture/util.h>
#include <rex/logging.h>

#include "descriptors.h"
#include "device.h"
#include "format.h"
#include "frame.h"
#include "host_passes.h"
#include "host_shaders.h"
#include "upload.h"

namespace mcla::gpu {

namespace tu = rex::graphics::texture_util;

namespace {

// hlsl/resolve_memory_cs.hlsl EncodePush, dword for dword.
struct EncodePush {
  uint32_t source;
  uint32_t stencil;
  int32_t offset_x;
  int32_t offset_y;
  uint32_t samples;
  float scale;
  uint32_t mode;
  uint32_t level;
  uint32_t address;
  uint32_t pitch;
  uint32_t layout;
  uint32_t tail;
  uint32_t rect0;
  uint32_t rect1;
  int32_t group_x0;
  uint32_t reserved;
};
static_assert(sizeof(EncodePush) == 64);

// hlsl/resolve_memory_ps.hlsl DecodePush.
struct DecodePush {
  uint32_t address;
  uint32_t pitch;
  uint32_t layout;
  uint32_t tail;
  uint32_t reserved[4];
};
static_assert(sizeof(DecodePush) == 32);

// hlsl/resolve_memory_ps.hlsl kDecode*.
enum DecodeKind : uint32_t {
  kDecodeR8 = 0,
  kDecodeB5G5R5A1 = 1,
  kDecodeR8G8B8A8 = 2,
  kDecodeR16G16B16A16F = 3,
  kDecodeR32F = 4,
  kDecodeDepthUnorm24 = 5,
  kDecodeDepthFloat20e4 = 6,
};

uint32_t BlockLog2(uint32_t bytes_per_block) {
  switch (bytes_per_block) {
    case 1: return 0;
    case 2: return 1;
    case 4: return 2;
    case 8: return 3;
    default: return UINT32_MAX;
  }
}

// The decode of a texture format: its host format and the shader's kind, as
// format.h TextureHost loads it from guest memory.
bool DecodeOf(const TextureKey& key, uint32_t* kind, plume::RenderFormat* format,
              std::string* why) {
  const format::TextureHostFormat host = format::TextureHost(key.format);
  if (host.status != format::Status::kOk) {
    *why = std::format("format {} has no host format", uint32_t(key.format));
    return false;
  }
  if (host.decompress != format::TextureDecompress::kNone) {
    *why = std::format("{} is block-compressed, which the decode does not read (TODO M5.2)",
                       host.guest_name);
    return false;
  }
  *format = host.format;
  switch (host.load) {
    case format::TextureLoad::kR5G5B5A1ToB5G5R5A1:
      *kind = kDecodeB5G5R5A1;
      return true;
    case format::TextureLoad::kDepthUnorm24:
      *kind = kDecodeDepthUnorm24;
      return true;
    case format::TextureLoad::kDepthFloat20e4:
      *kind = kDecodeDepthFloat20e4;
      return true;
    case format::TextureLoad::kCopy:
      switch (host.format) {
        case plume::RenderFormat::R8_UNORM: *kind = kDecodeR8; return true;
        case plume::RenderFormat::R8G8B8A8_UNORM: *kind = kDecodeR8G8B8A8; return true;
        case plume::RenderFormat::R16G16B16A16_FLOAT: *kind = kDecodeR16G16B16A16F; return true;
        case plume::RenderFormat::R32_FLOAT: *kind = kDecodeR32F; return true;
        default: break;
      }
      break;
  }
  *why = std::format("{} ({}) has no decode from the resolve memory (TODO M5.2)", host.guest_name,
                     format::PlumeFormatName(host.format));
  return false;
}

// The push layout word of a level: bits 0-1 bpb_log2, bit 2 tiled, bits 3-4 endian.
uint32_t LayoutBits(const texture_decode::Source& source, uint32_t bpb_log2) {
  return bpb_log2 | (source.tiled ? 4u : 0u) | ((source.endian & 3u) << 3);
}

uint32_t LevelPitch(const texture_decode::Source& source, uint32_t bpb_log2) {
  // Tiled: in blocks (texture_decode::BlockOffset); linear: in bytes.
  return source.tiled ? source.row_pitch_bytes >> bpb_log2 : source.row_pitch_bytes;
}

}  // namespace

ResolveMemory::~ResolveMemory() {
  Shutdown();
}

bool ResolveMemory::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap,
                               UploadRing& upload, GuestMemory& memory, HostPasses& passes,
                               TextureCache& textures) {
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  upload_ = &upload;
  memory_ = &memory;
  passes_ = &passes;
  textures_ = &textures;
  subscriber_ = memory.Subscribe("resolve memory");
  if (subscriber_ == GuestMemory::kNoSubscriber) {
    // Without the watch a CPU write would never reach the pages: no silent
    // staleness, the renderer does not start.
    REXLOG_ERROR("[gpu] resolve memory: no write-watch queue; it cannot follow CPU writes");
    return false;
  }
  chunk_slot_.assign(kChunkCount, kNoChunk);
  page_flags_.assign(kPageCount, 0);
  page_sequence_.assign(kPageCount, 0);

  encode_set_builder_.begin();
  encode_pool_index_ = encode_set_builder_.addReadWriteByteAddressBuffer(0);  // u0
  encode_table_index_ = encode_set_builder_.addByteAddressBuffer(1);          // t1
  encode_set_builder_.end();
  decode_set_builder_.begin();
  decode_pool_index_ = decode_set_builder_.addByteAddressBuffer(0);   // t0
  decode_table_index_ = decode_set_builder_.addByteAddressBuffer(1);  // t1
  decode_set_builder_.end();

  // The encode: the bindless texture set four times (spaces 0-3, the EDRAM's
  // and the products' views, as the host passes bind them), the memory
  // (space 4), the push range (b0, space 5).
  plume::RenderPipelineLayoutBuilder encode_builder;
  encode_builder.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  for (uint32_t set = 0; set < 4; ++set) {
    encode_builder.addDescriptorSet(heap.texture_set_builder());
  }
  encode_builder.addDescriptorSet(encode_set_builder_);
  encode_builder.addPushConstant(0, 5, sizeof(EncodePush), plume::RenderShaderStageFlag::COMPUTE);
  encode_builder.end();
  encode_layout_ = device.CreatePipelineLayout(encode_builder, "resolve memory encode layout");
  // The decode: the memory (space 0), the push range (b0, space 1).
  plume::RenderPipelineLayoutBuilder decode_builder;
  decode_builder.begin(/*isLocal=*/false, /*allowInputLayout=*/false);
  decode_builder.addDescriptorSet(decode_set_builder_);
  decode_builder.addPushConstant(0, 1, sizeof(DecodePush), plume::RenderShaderStageFlag::PIXEL);
  decode_builder.end();
  decode_layout_ = device.CreatePipelineLayout(decode_builder, "resolve memory decode layout");
  if (!encode_layout_ || !decode_layout_) {
    return false;
  }
  for (uint32_t i = 0; i < 3; ++i) {
    const HostShaderBlob blob = ResolveMemoryEncodeShader(i > 0, i == 2);
    encode_shaders_[i] = device.CreateShader(blob.data, blob.size, "main", blob.name);
    if (!encode_shaders_[i]) {
      return false;
    }
    encode_pipelines_[i] = device.CreateComputePipeline(
        plume::RenderComputePipelineDesc(encode_layout_.get(), encode_shaders_[i].get(), 8, 8, 1),
        blob.name);
    if (!encode_pipelines_[i]) {
      return false;
    }
  }
  const HostShaderBlob decode = ResolveMemoryDecodeShader();
  decode_shader_ = passes.CreateShader(decode.data, decode.size, decode.name);
  if (!decode_shader_) {
    return false;
  }

  pool_ = device.CreateBuffer(
      plume::RenderBufferDesc::DefaultBuffer(
          uint64_t(kInitialPoolChunks) * kChunkBytes,
          plume::RenderBufferFlag::STORAGE | plume::RenderBufferFlag::UNORDERED_ACCESS),
      "resolve memory pool");
  table_ = device.CreateBuffer(plume::RenderBufferDesc::DefaultBuffer(
                                   uint64_t(kChunkCount) * 4, plume::RenderBufferFlag::STORAGE),
                               "resolve memory table");
  if (!pool_ || !table_) {
    return false;
  }
  pool_chunks_ = kInitialPoolChunks;
  return MakeSets();
}

void ResolveMemory::Shutdown() {
  // The GPU is idle and the heap goes next: nothing is handed back to it.
  for (auto& [key, d] : decoded_) {
    d->views.Drop();
  }
  pending_.clear();
  readback_buffer_.reset();
  readback_bytes_ = 0;
  readback_fence_.reset();
  readback_list_.reset();
  decoded_.clear();
  decoded_regions_.Clear();
  decoded_owners_.clear();
  decode_pipelines_.clear();
  decode_shader_.reset();
  for (auto& pipeline : encode_pipelines_) {
    pipeline.reset();
  }
  for (auto& shader : encode_shaders_) {
    shader.reset();
  }
  encode_set_.reset();
  decode_set_.reset();
  encode_layout_.reset();
  decode_layout_.reset();
  pool_.reset();
  table_.reset();
  chunk_slot_.clear();
  page_flags_.clear();
  page_sequence_.clear();
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
  upload_ = nullptr;
  memory_ = nullptr;
  passes_ = nullptr;
  textures_ = nullptr;
}

bool ResolveMemory::MakeSets() {
  std::unique_ptr<plume::RenderDescriptorSet> encode =
      device_->CreateDescriptorSet(encode_set_builder_, 2, 0, "resolve memory encode set");
  std::unique_ptr<plume::RenderDescriptorSet> decode =
      device_->CreateDescriptorSet(decode_set_builder_, 2, 0, "resolve memory decode set");
  if (!encode || !decode) {
    return false;
  }
  const uint64_t pool_bytes = uint64_t(pool_chunks_) * kChunkBytes;
  encode->setBuffer(encode_pool_index_, pool_.get(), pool_bytes);
  encode->setBuffer(encode_table_index_, table_.get(), uint64_t(kChunkCount) * 4);
  decode->setBuffer(decode_pool_index_, pool_.get(), pool_bytes);
  decode->setBuffer(decode_table_index_, table_.get(), uint64_t(kChunkCount) * 4);
  if (encode_set_) {
    // Lists in flight may still name the old sets.
    frames_->Retire(std::move(encode_set_));
    frames_->Retire(std::move(decode_set_));
  }
  encode_set_ = std::move(encode);
  decode_set_ = std::move(decode);
  return true;
}

void ResolveMemory::BeginFrame(uint32_t slot) {
  slot_ = slot;
  ++frame_;
  if (frame_ % 60 == 0) {
    EvictSome();
  }
}

void ResolveMemory::Fail(const char* what, const std::string& detail) {
  uint32_t& logged = failures_logged_[what];
  if (logged < 4) {
    ++logged;
    REXLOG_ERROR("[gpu] resolve memory: {}{}{}{}", what, detail.empty() ? "" : ": ", detail,
                 logged == 4 ? " (further ones counted, not logged)" : "");
  }
}

// ---------------------------------------------------------------------------
// pages and chunks
// ---------------------------------------------------------------------------

void ResolveMemory::Sync() {
  if (!memory_->HasInvalidations(subscriber_)) {
    return;
  }
  drained_.clear();
  memory_->Drain(subscriber_, drained_);
  for (const GuestMemory::Range& range : drained_) {
    if (range.length == 0) {
      continue;
    }
    const uint32_t first = range.start >> kPageShift;
    const uint32_t last =
        std::min((uint64_t(range.start) + range.length - 1) >> kPageShift, uint64_t(kPageCount - 1));
    bool any = false;
    for (uint32_t page = first; page <= last; ++page) {
      uint8_t& flags = page_flags_[page];
      if (!flags) {
        continue;
      }
      // The CPU's bytes win: the page goes back to guest memory, and is
      // uploaded again before the next pass that needs it.
      ++stats_.cpu_writes;
      if (flags & kNewer) {
        ++stats_.cpu_writes_newer;
      }
      flags = 0;
      page_sequence_[page] = ++sequence_;
      any = true;
    }
    if (any) {
      TouchDecoded(range.start, range.length);
    }
  }
}

bool ResolveMemory::EnsureChunk(plume::RenderCommandList* list, uint32_t chunk) {
  if (chunk >= kChunkCount) {
    Fail("an address beyond the physical memory", std::format("chunk {}", chunk));
    return false;
  }
  if (chunk_slot_[chunk] != kNoChunk) {
    return true;
  }
  if (used_chunks_ == pool_chunks_ && !GrowPool(list)) {
    return false;
  }
  chunk_slot_[chunk] = used_chunks_++;
  table_dirty_ = true;
  ++stats_.chunks_placed;
  return true;
}

bool ResolveMemory::GrowPool(plume::RenderCommandList* list) {
  const uint32_t chunks = std::min(pool_chunks_ * 2, kChunkCount);
  if (chunks == pool_chunks_) {
    Fail("the pool holds every chunk already", "");
    return false;
  }
  std::unique_ptr<plume::RenderBuffer> bigger = device_->CreateBuffer(
      plume::RenderBufferDesc::DefaultBuffer(
          uint64_t(chunks) * kChunkBytes,
          plume::RenderBufferFlag::STORAGE | plume::RenderBufferFlag::UNORDERED_ACCESS),
      "resolve memory pool");
  if (!bigger) {
    Fail("the pool could not grow", std::format("{} MiB", uint64_t(chunks) * kChunkBytes >> 20));
    return false;
  }
  // The old pool's bytes into the new one, after everything recorded so far.
  list->barriers(plume::RenderBarrierStage::COPY,
                 plume::RenderBufferBarrier(pool_.get(), plume::RenderBufferAccess::READ));
  list->barriers(plume::RenderBarrierStage::COPY,
                 plume::RenderBufferBarrier(bigger.get(), plume::RenderBufferAccess::WRITE));
  list->copyBufferRegion(plume::RenderBufferReference(bigger.get(), 0),
                         plume::RenderBufferReference(pool_.get(), 0),
                         uint64_t(used_chunks_) * kChunkBytes);
  frames_->Retire(std::move(pool_));
  pool_ = std::move(bigger);
  pool_chunks_ = chunks;
  pool_access_ = PoolAccess::kCopyWrite;
  ++stats_.pool_growths;
  REXLOG_INFO("[gpu] resolve memory: pool grown to {} MiB ({} chunks placed)",
              uint64_t(chunks) * kChunkBytes >> 20, used_chunks_);
  return MakeSets();
}

void ResolveMemory::UploadTable(plume::RenderCommandList* list) {
  const uint64_t bytes = uint64_t(kChunkCount) * 4;
  const UploadSpan span = upload_->Allocate(slot_, bytes, 256);
  if (!span) {
    Fail("no upload memory for the chunk table", "");
    return;
  }
  std::memcpy(span.cpu, chunk_slot_.data(), size_t(bytes));
  list->barriers(plume::RenderBarrierStage::COPY,
                 plume::RenderBufferBarrier(table_.get(), plume::RenderBufferAccess::WRITE));
  list->copyBufferRegion(plume::RenderBufferReference(table_.get(), 0), span.reference(), bytes);
  table_dirty_ = false;
  table_read_barrier_ = true;
}

void ResolveMemory::PoolBarrier(plume::RenderCommandList* list, PoolAccess access) {
  switch (access) {
    case PoolAccess::kCopyWrite:
      // Uploads write disjoint pages: one barrier ahead of a run of them.
      if (pool_access_ != PoolAccess::kCopyWrite) {
        list->barriers(plume::RenderBarrierStage::COPY,
                       plume::RenderBufferBarrier(pool_.get(), plume::RenderBufferAccess::WRITE));
      }
      break;
    case PoolAccess::kComputeWrite:
      // Every time: two copies into the same pages keep their order (on D3D12
      // a UAV barrier once the pool is in the UAV state).
      list->barriers(plume::RenderBarrierStage::COMPUTE,
                     plume::RenderBufferBarrier(pool_.get(), plume::RenderBufferAccess::READ |
                                                                 plume::RenderBufferAccess::WRITE));
      break;
    case PoolAccess::kGraphicsRead:
      if (pool_access_ != PoolAccess::kGraphicsRead) {
        list->barriers(plume::RenderBarrierStage::GRAPHICS,
                       plume::RenderBufferBarrier(pool_.get(), plume::RenderBufferAccess::READ));
      }
      break;
    case PoolAccess::kCopyRead:
      if (pool_access_ != PoolAccess::kCopyRead) {
        list->barriers(plume::RenderBarrierStage::COPY,
                       plume::RenderBufferBarrier(pool_.get(), plume::RenderBufferAccess::READ));
      }
      break;
    case PoolAccess::kNone:
      break;
  }
  pool_access_ = access;
  if (table_read_barrier_) {
    list->barriers(plume::RenderBarrierStage::GRAPHICS | plume::RenderBarrierStage::COMPUTE,
                   plume::RenderBufferBarrier(table_.get(), plume::RenderBufferAccess::READ));
    table_read_barrier_ = false;
  }
}

void ResolveMemory::UploadPages(plume::RenderCommandList* list,
                                const std::vector<uint32_t>& pages) {
  if (pages.empty()) {
    return;
  }
  const UploadSpan span = upload_->Allocate(slot_, uint64_t(pages.size()) * kPageBytes, 256);
  if (!span) {
    Fail("no upload memory for guest pages", std::format("{} pages", pages.size()));
    return;
  }
  PoolBarrier(list, PoolAccess::kCopyWrite);
  // Runs of pages that sit next to each other in the pool go in one copy.
  size_t run_begin = 0;
  uint64_t run_pool = 0;
  uint32_t run_pages = 0;
  const auto flush = [&]() {
    if (run_pages) {
      list->copyBufferRegion(plume::RenderBufferReference(pool_.get(), run_pool),
                             plume::RenderBufferReference(span.buffer,
                                                          span.offset + run_begin * kPageBytes),
                             uint64_t(run_pages) * kPageBytes);
    }
  };
  for (size_t i = 0; i < pages.size(); ++i) {
    const uint32_t page = pages[i];
    const uint8_t* src = memory_->Read(page << kPageShift, kPageBytes);
    uint8_t* dst = span.cpu + i * kPageBytes;
    if (src) {
      std::memcpy(dst, src, kPageBytes);
      page_flags_[page] |= kValid;
      ++stats_.pages_uploaded;
    } else {
      // Not committed for the guest either; what the GPU writes there stays
      // the only content (logged: nothing should read such a page).
      std::memset(dst, 0, kPageBytes);
      Fail("a guest page that cannot be read", std::format("{:#010x}", page << kPageShift));
    }
    page_sequence_[page] = ++sequence_;
    const uint64_t pool = (uint64_t(chunk_slot_[page >> (kChunkShift - kPageShift)])
                           << kChunkShift) |
                          (uint64_t(page & (kPagesPerChunk - 1)) << kPageShift);
    if (run_pages && pool == run_pool + uint64_t(run_pages) * kPageBytes) {
      ++run_pages;
    } else {
      flush();
      run_begin = i;
      run_pool = pool;
      run_pages = 1;
    }
  }
  flush();
}

void ResolveMemory::ArmPages(const std::vector<uint32_t>& pages) {
  // Runs of consecutive pages, one Arm each.
  uint32_t run_start = 0, run_count = 0;
  const auto flush = [&]() {
    if (run_count) {
      memory_->Arm(run_start << kPageShift, run_count << kPageShift);
    }
  };
  for (uint32_t page : pages) {
    uint8_t& flags = page_flags_[page];
    if ((flags & kArmed) || !(flags & kValid)) {
      continue;
    }
    flags |= kArmed;
    if (run_count && page == run_start + run_count) {
      ++run_count;
    } else {
      flush();
      run_start = page;
      run_count = 1;
    }
  }
  flush();
}

void ResolveMemory::TouchDecoded(uint32_t start, uint32_t length) {
  decoded_regions_.ForEachOverlap(start, length, [&](RegionIndex::Id id) {
    if (id >= decoded_owners_.size()) {
      return;
    }
    for (Decoded* d : decoded_owners_[id]) {
      if (d->key.base_page) {
        textures_->BumpProductGeneration(d->key.base_page);
      }
      if (d->key.mip_page) {
        textures_->BumpProductGeneration(d->key.mip_page);
      }
    }
  });
}

// ---------------------------------------------------------------------------
// encode
// ---------------------------------------------------------------------------

bool ResolveMemory::Encode(plume::RenderCommandList* list, const Copy& copy) {
  const TextureKey& key = *copy.key;
  const auto fail = [&](const char* what) {
    ++stats_.encode_failures;
    Fail(what, std::format("{} level {}", key.Describe(), copy.level));
    return false;
  };
  if (!list) {
    return fail("no command list");
  }
  if (copy.x1 <= copy.x0 || copy.y1 <= copy.y0) {
    return true;
  }
  if (copy.depth) {
    // Recorded now, after every colour encode waiting: their pages may overlap.
    Flush(list);
  }
  TextureLevelStorage storage;
  if (!GetTextureLevelStorage(key, copy.level, &storage)) {
    return fail("the destination level has no storage");
  }
  const texture_decode::Source& source = storage.request.source;
  const uint32_t bpb_log2 = BlockLog2(source.bytes_per_block);
  if (bpb_log2 == UINT32_MAX) {
    return fail("a destination block size the encode does not write");
  }
  if (copy.depth && bpb_log2 != 2) {
    return fail("a depth copy into a destination that is not 32 bpp");
  }
  const uint32_t pitch = LevelPitch(source, bpb_log2);

  // The pages the rectangle writes, and how many of their bytes: a page every
  // byte of which is written needs no upload. Storage coordinates (blocks,
  // with the packed tail's offset).
  const int32_t sx0 = copy.x0 + int32_t(source.offset_x);
  const int32_t sx1 = copy.x1 + int32_t(source.offset_x);
  const int32_t sy0 = copy.y0 + int32_t(source.offset_y);
  const int32_t sy1 = copy.y1 + int32_t(source.offset_y);
  const uint32_t first_page = storage.address >> kPageShift;
  const uint32_t page_span =
      uint32_t(((uint64_t(storage.address) + storage.extent + kPageBytes - 1) >> kPageShift) -
               first_page);
  touched_.assign(page_span, {0u, 0u});  // (touched, bytes covered)
  const auto touch = [&](uint64_t address, uint32_t covered) {
    const uint64_t page = address >> kPageShift;
    if (page < first_page || page >= uint64_t(first_page) + page_span) {
      return false;
    }
    auto& t = touched_[size_t(page - first_page)];
    t.first = 1;
    t.second += covered;
    return true;
  };
  bool inside = true;
  if (source.tiled) {
    // A 32x32 tile sits in one page (8 and 16 bpp share a page with 3 and 1
    // other tiles), or in two whole pages at 64 bpp.
    const uint32_t tile_bytes = 1024u << bpb_log2;
    for (int32_t ty = sy0 >> 5; ty <= (sy1 - 1) >> 5; ++ty) {
      for (int32_t tx = sx0 >> 5; tx <= (sx1 - 1) >> 5; ++tx) {
        const bool whole = tx * 32 >= sx0 && tx * 32 + 32 <= sx1 && ty * 32 >= sy0 &&
                           ty * 32 + 32 <= sy1;
        const uint64_t origin =
            uint64_t(storage.address) +
            uint32_t(tu::GetTiledOffset2D(tx * 32, ty * 32, pitch, bpb_log2));
        if (tile_bytes > kPageBytes) {
          inside &= touch(origin, whole ? kPageBytes : 0u);
          inside &= touch(origin + kPageBytes, whole ? kPageBytes : 0u);
        } else {
          inside &= touch(origin, whole ? tile_bytes : 0u);
        }
      }
    }
  } else {
    for (int32_t y = sy0; y < sy1; ++y) {
      const uint64_t row = uint64_t(storage.address) + uint64_t(y) * pitch;
      const uint64_t begin = row + (uint64_t(sx0) << bpb_log2);
      const uint64_t end = row + (uint64_t(sx1) << bpb_log2);
      for (uint64_t page = begin >> kPageShift; page <= (end - 1) >> kPageShift; ++page) {
        const uint64_t lo = std::max(begin, page << kPageShift);
        const uint64_t hi = std::min(end, (page + 1) << kPageShift);
        inside &= touch(page << kPageShift, uint32_t(hi - lo));
      }
    }
  }
  if (!inside) {
    return fail("a rectangle beyond the destination level's storage");
  }
  pages_.clear();
  for (uint32_t i = 0; i < page_span; ++i) {
    if (!touched_[i].first) {
      continue;
    }
    const uint32_t page = first_page + i;
    if (!EnsureChunk(list, page >> (kChunkShift - kPageShift))) {
      return fail("no pool chunk for the destination");
    }
    if (touched_[i].second < kPageBytes && !(page_flags_[page] & kValid)) {
      pages_.push_back(page);
    }
  }
  if (table_dirty_) {
    UploadTable(list);
  }
  UploadPages(list, pages_);

  EncodePush push{};
  push.mode = copy.mode;
  if (copy.depth) {
    push.source = copy.source_slot;
    push.stencil = copy.stencil_slot;
    push.offset_x = copy.offset_x;
    push.offset_y = copy.offset_y;
    push.samples = copy.samples;
  } else {
    push.source = copy.product_slot;
  }
  push.level = copy.level;
  push.address = storage.address;
  push.pitch = pitch;
  push.layout = LayoutBits(source, bpb_log2);
  push.tail = (source.offset_x & 0xFFFFu) | (source.offset_y << 16);
  push.rect0 = (uint32_t(copy.x0) & 0xFFFFu) | (uint32_t(copy.y0) << 16);
  push.rect1 = (uint32_t(copy.x1) & 0xFFFFu) | (uint32_t(copy.y1) << 16);
  // The first dword group starts at a storage x that is a multiple of the
  // group: 4 blocks at 8 bpp, 2 at 16, 1 otherwise.
  const int32_t group = bpb_log2 >= 2 ? 1 : int32_t(4u >> bpb_log2);
  push.group_x0 = ((copy.x0 + int32_t(source.offset_x)) & ~(group - 1)) - int32_t(source.offset_x);
  const uint32_t groups_x =
      (uint32_t(copy.x1 - push.group_x0) + uint32_t(group) - 1) / uint32_t(group);
  const uint32_t dispatch_x = (groups_x + 7) / 8;
  const uint32_t dispatch_y = (uint32_t(copy.y1 - copy.y0) + 7) / 8;
  uint32_t low_page = UINT32_MAX, high_page = 0;
  for (uint32_t i = 0; i < page_span; ++i) {
    if (touched_[i].first) {
      low_page = std::min(low_page, first_page + i);
      high_page = std::max(high_page, first_page + i);
    }
  }
  if (copy.depth) {
    PoolBarrier(list, PoolAccess::kComputeWrite);
    list->setComputePipelineLayout(encode_layout_.get());
    for (uint32_t set = 0; set < 4; ++set) {
      list->setComputeDescriptorSet(heap_->texture_set(), set);
    }
    list->setComputeDescriptorSet(encode_set_.get(), 4);
    list->setPipeline(encode_pipelines_[copy.msaa ? 2 : 1].get());
    list->setComputePushConstants(0, &push, 0, sizeof(push));
    list->dispatch(dispatch_x, dispatch_y, 1);
  } else {
    // The product keeps these texels until a resolve writes the level again
    // (BeforeResolve): the dispatch joins the batch.
    Pending p;
    p.key = key;
    p.level = copy.level;
    p.x0 = copy.x0;
    p.y0 = copy.y0;
    p.x1 = copy.x1;
    p.y1 = copy.y1;
    p.first_page = low_page;
    p.last_page = high_page;
    p.groups_x = dispatch_x;
    p.groups_y = dispatch_y;
    static_assert(sizeof(p.push) == sizeof(push));
    std::memcpy(p.push.data(), &push, sizeof(push));
    pending_.push_back(p);
  }

  // Every page written: the resolve's bytes are there and not in guest
  // memory; a page written whole, or uploaded first, holds the console's bytes.
  pages_.clear();
  for (uint32_t i = 0; i < page_span; ++i) {
    if (!touched_[i].first) {
      continue;
    }
    const uint32_t page = first_page + i;
    uint8_t& flags = page_flags_[page];
    if (touched_[i].second >= kPageBytes) {
      flags |= kValid;
    }
    flags |= kNewer;
    page_sequence_[page] = ++sequence_;
    newer_low_page_ = std::min(newer_low_page_, page);
    newer_high_page_ = std::max(newer_high_page_, page);
    pages_.push_back(page);
  }
  ArmPages(pages_);
  TouchDecoded(storage.address, uint32_t(std::min<uint64_t>(storage.extent, UINT32_MAX)));
  ++stats_.encodes;
  stats_.encoded_bytes += (uint64_t(copy.x1 - copy.x0) * uint64_t(copy.y1 - copy.y0)) << bpb_log2;
  return true;
}

void ResolveMemory::BeforeResolve(plume::RenderCommandList* list, const TextureKey& key,
                                  uint32_t level, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
  bool overlap = false;
  size_t kept = 0;
  for (size_t i = 0; i < pending_.size(); ++i) {
    const Pending& p = pending_[i];
    if (p.level == level && p.key == key && x0 < p.x1 && p.x0 < x1 && y0 < p.y1 && p.y0 < y1) {
      if (x0 <= p.x0 && y0 <= p.y0 && x1 >= p.x1 && y1 >= p.y1) {
        // Every byte it would write, the new resolve writes again, later.
        ++stats_.encodes_dropped;
        continue;
      }
      overlap = true;
    }
    pending_[kept++] = pending_[i];
  }
  pending_.resize(kept);
  if (overlap) {
    // The product is about to lose texels a waiting encode still has to read.
    Flush(list);
  }
}

void ResolveMemory::Flush(plume::RenderCommandList* list) {
  if (pending_.empty() || !list) {
    return;
  }
  ++stats_.flushes;
  PoolBarrier(list, PoolAccess::kComputeWrite);
  list->setComputePipelineLayout(encode_layout_.get());
  for (uint32_t set = 0; set < 4; ++set) {
    list->setComputeDescriptorSet(heap_->texture_set(), set);
  }
  list->setComputeDescriptorSet(encode_set_.get(), 4);
  list->setPipeline(encode_pipelines_[0].get());
  wave_.clear();
  for (const Pending& p : pending_) {
    // Encodes over the same pages keep the order the resolves had.
    bool overlaps = false;
    for (const auto& range : wave_) {
      if (p.first_page <= range.second && range.first <= p.last_page) {
        overlaps = true;
        break;
      }
    }
    if (overlaps) {
      PoolBarrier(list, PoolAccess::kComputeWrite);
      wave_.clear();
      ++stats_.waves;
    }
    wave_.emplace_back(p.first_page, p.last_page);
    list->setComputePushConstants(0, p.push.data(), 0, uint32_t(sizeof(p.push)));
    list->dispatch(p.groups_x, p.groups_y, 1);
  }
  pending_.clear();
}

// ---------------------------------------------------------------------------
// decode
// ---------------------------------------------------------------------------

bool ResolveMemory::Holds(const TextureKey& key) {
  ++stats_.holds_checks;
  if (newer_low_page_ > newer_high_page_) {
    return false;
  }
  uint32_t ranges[2][2];
  TextureGuestRanges(key, ranges);
  for (const auto& r : ranges) {
    if (!r[1]) {
      continue;
    }
    const uint32_t first = std::max(r[0] >> kPageShift, newer_low_page_);
    const uint32_t last = std::min(
        uint32_t((uint64_t(r[0]) + r[1] - 1) >> kPageShift), std::min(newer_high_page_, kPageCount - 1));
    for (uint32_t page = first; page <= last; ++page) {
      if (page_flags_[page] & kNewer) {
        return true;
      }
    }
  }
  return false;
}

plume::RenderPipeline* ResolveMemory::DecodePipeline(plume::RenderFormat format) {
  const uint32_t id = uint32_t(format);
  auto it = decode_pipelines_.find(id);
  if (it != decode_pipelines_.end()) {
    return it->second.get();
  }
  const std::string name =
      std::format("resolve memory decode -> {}", format::PlumeFormatName(format));
  std::unique_ptr<plume::RenderPipeline> pipeline =
      passes_->CreatePipeline(*decode_layout_, *decode_shader_, format, name.c_str());
  plume::RenderPipeline* raw = pipeline.get();
  decode_pipelines_.emplace(id, std::move(pipeline));
  return raw;
}

ResolveMemory::BindResult ResolveMemory::Bind(const TextureKey& key, uint16_t host_swizzle,
                                              plume::RenderCommandList* list, uint32_t* slot,
                                              bool* state_changed, std::string* why) {
  ++stats_.binds;
  *state_changed = false;
  uint32_t kind = 0;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  if (!DecodeOf(key, &kind, &format, why)) {
    ++stats_.bind_unsupported;
    return BindResult::kUnsupported;
  }
  if (!list) {
    *why = "no command list to decode with";
    ++stats_.bind_failures;
    return BindResult::kFailed;
  }
  auto it = decoded_.find(key);
  Decoded* d = it != decoded_.end() ? it->second.get() : nullptr;
  if (!d) {
    auto fresh = std::make_unique<Decoded>();
    fresh->key = key;
    fresh->format = format;
    fresh->kind = kind;
    const std::string name = std::format("resolve memory texture {}", key.Describe());
    fresh->texture = device_->CreateTexture(
        plume::RenderTextureDesc::Texture2D(key.width, key.height, key.levels(), format,
                                            plume::RenderTextureFlag::RENDER_TARGET),
        name.c_str());
    if (!fresh->texture) {
      *why = std::format("{} {}x{} with {} levels", format::PlumeFormatName(format), key.width,
                         key.height, key.levels());
      ++stats_.bind_failures;
      return BindResult::kFailed;
    }
    fresh->level_views.resize(key.levels());
    fresh->level_framebuffers.resize(key.levels());
    for (uint32_t level = 0; level < key.levels(); ++level) {
      plume::RenderTextureViewDesc view = plume::RenderTextureViewDesc::Texture2D(format);
      view.mipSlice = level;
      view.mipLevels = 1;
      fresh->level_views[level] =
          device_->CreateTextureView(*fresh->texture, view, "resolve memory level view");
      const plume::RenderTexture* textures[1] = {fresh->texture.get()};
      const plume::RenderTextureView* views[1] = {fresh->level_views[level].get()};
      plume::RenderFramebufferDesc desc(textures, 1);
      desc.colorAttachmentViews = views;
      if (fresh->level_views[level]) {
        fresh->level_framebuffers[level] =
            device_->CreateFramebuffer(desc, "resolve memory level framebuffer");
      }
      if (!fresh->level_framebuffers[level]) {
        *why = std::format("the render target of level {}", level);
        ++stats_.bind_failures;
        return BindResult::kFailed;
      }
    }
    // Every level cleared before the first use: a render target in an unzeroed
    // heap must be (D3D12 debug layer id 1422), and a level without storage
    // (the base of a mips-only key) is never drawn.
    list->barriers(plume::RenderBarrierStage::GRAPHICS,
                   plume::RenderTextureBarrier(fresh->texture.get(),
                                               plume::RenderTextureLayout::COLOR_WRITE));
    fresh->layout = plume::RenderTextureLayout::COLOR_WRITE;
    for (uint32_t level = 0; level < key.levels(); ++level) {
      list->setFramebuffer(fresh->level_framebuffers[level].get());
      list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
    }
    list->setFramebuffer(nullptr);
    *state_changed = true;
    TextureGuestRanges(key, fresh->ranges);
    for (uint32_t i = 0; i < 2; ++i) {
      if (!fresh->ranges[i][1]) {
        continue;
      }
      RegionIndex::Id id = decoded_regions_.Find(fresh->ranges[i][0], fresh->ranges[i][1]);
      if (id == RegionIndex::kNone) {
        id = decoded_regions_.Insert(fresh->ranges[i][0], fresh->ranges[i][1]);
      }
      if (decoded_owners_.size() < decoded_regions_.id_bound()) {
        decoded_owners_.resize(decoded_regions_.id_bound());
      }
      decoded_owners_[id].push_back(fresh.get());
      fresh->region_ids[i] = id;
    }
    d = fresh.get();
    decoded_.emplace(key, std::move(fresh));
  }
  d->last_frame = frame_;

  // The pages of its range: the ones not valid come from guest memory first,
  // and the newest write says whether the texture is current.
  pages_.clear();
  uint64_t newest = 0;
  for (const auto& r : d->ranges) {
    if (!r[1]) {
      continue;
    }
    const uint32_t first = r[0] >> kPageShift;
    const uint32_t last = std::min(uint32_t((uint64_t(r[0]) + r[1] - 1) >> kPageShift),
                                   kPageCount - 1);
    for (uint32_t page = first; page <= last; ++page) {
      if (!(page_flags_[page] & kValid)) {
        pages_.push_back(page);
      }
      newest = std::max(newest, page_sequence_[page]);
    }
  }
  if (d->decoded_sequence != 0 && pages_.empty() && newest <= d->decoded_sequence) {
    *slot = d->views.Slot(*device_, *heap_, *d->texture, d->format, key.levels(), host_swizzle,
                          "resolve memory view");
    return *slot == kInvalidBindlessSlot ? BindResult::kFailed : BindResult::kOk;
  }
  // Encodes still waiting write the bytes this reads.
  Flush(list);
  for (uint32_t page : pages_) {
    if (!EnsureChunk(list, page >> (kChunkShift - kPageShift))) {
      *why = "no pool chunk for the texture's pages";
      ++stats_.bind_failures;
      return BindResult::kFailed;
    }
  }
  if (table_dirty_) {
    UploadTable(list);
  }
  UploadPages(list, pages_);
  ArmPages(pages_);
  for (uint32_t page : pages_) {
    newest = std::max(newest, page_sequence_[page]);
  }
  PoolBarrier(list, PoolAccess::kGraphicsRead);

  plume::RenderPipeline* pipeline = DecodePipeline(d->format);
  if (!pipeline) {
    *why = "the decode pipeline";
    ++stats_.bind_failures;
    return BindResult::kFailed;
  }
  if (d->layout != plume::RenderTextureLayout::COLOR_WRITE) {
    list->barriers(plume::RenderBarrierStage::GRAPHICS,
                   plume::RenderTextureBarrier(d->texture.get(),
                                               plume::RenderTextureLayout::COLOR_WRITE));
    d->layout = plume::RenderTextureLayout::COLOR_WRITE;
  }
  for (uint32_t level = 0; level < key.levels(); ++level) {
    TextureLevelStorage storage;
    if (!GetTextureLevelStorage(key, level, &storage)) {
      continue;  // no storage: never sampled (the base of a mips-only key)
    }
    const texture_decode::Source& source = storage.request.source;
    const uint32_t bpb_log2 = BlockLog2(source.bytes_per_block);
    list->setFramebuffer(d->level_framebuffers[level].get());
    list->setGraphicsPipelineLayout(decode_layout_.get());
    list->setGraphicsDescriptorSet(decode_set_.get(), 0);
    list->setPipeline(pipeline);
    const uint32_t width = storage.request.width;
    const uint32_t height = storage.request.height;
    const plume::RenderViewport viewport(0.0f, 0.0f, float(width), float(height));
    list->setViewports(&viewport, 1);
    const plume::RenderRect scissor(0, 0, int32_t(width), int32_t(height));
    list->setScissors(&scissor, 1);
    DecodePush push{};
    push.address = storage.address;
    push.pitch = LevelPitch(source, bpb_log2);
    push.layout = LayoutBits(source, bpb_log2) | (d->kind << 8);
    push.tail = (source.offset_x & 0xFFFFu) | (source.offset_y << 16);
    list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
    list->drawInstanced(3, 1, 0, 0);
    ++stats_.decoded_levels;
  }
  list->setFramebuffer(nullptr);
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(d->texture.get(),
                                             plume::RenderTextureLayout::SHADER_READ));
  d->layout = plume::RenderTextureLayout::SHADER_READ;
  d->decoded_sequence = std::max<uint64_t>(newest, 1);
  *state_changed = true;
  ++stats_.decodes;
  *slot = d->views.Slot(*device_, *heap_, *d->texture, d->format, key.levels(), host_swizzle,
                        "resolve memory view");
  return *slot == kInvalidBindlessSlot ? BindResult::kFailed : BindResult::kOk;
}

// ---------------------------------------------------------------------------
// lock write-back
// ---------------------------------------------------------------------------

bool ResolveMemory::EnsureReadback(uint64_t bytes) {
  if (!readback_list_) {
    readback_list_ = device_->queue()->createCommandList();
    readback_fence_ = device_->device()->createCommandFence();
    if (!readback_list_ || !readback_fence_) {
      readback_list_.reset();
      readback_fence_.reset();
      Fail("the lock write-back's list or fence could not be made", "");
      return false;
    }
  }
  if (bytes > readback_bytes_) {
    // No copy is in flight: every write-back waited for its fence.
    readback_buffer_.reset();
    const uint64_t size = (bytes + (uint64_t(4) << 20) - 1) / (uint64_t(4) << 20) * (uint64_t(4) << 20);
    readback_buffer_ = device_->CreateBuffer(plume::RenderBufferDesc::ReadbackBuffer(size),
                                             "resolve memory lock readback");
    if (!readback_buffer_) {
      readback_bytes_ = 0;
      Fail("the lock write-back's readback buffer could not be made", std::format("{} bytes", size));
      return false;
    }
    readback_bytes_ = size;
  }
  return true;
}

int64_t ResolveMemory::WriteBack(uint32_t address, uint32_t size,
                                 const std::function<void()>& submit_recorded) {
  Sync();
  if (size == 0) {
    return 0;
  }
  const uint32_t first = address >> kPageShift;
  const uint32_t last =
      std::min(uint32_t((uint64_t(address) + size - 1) >> kPageShift), kPageCount - 1);
  pages_.clear();
  for (uint32_t page = first; page <= last; ++page) {
    const uint8_t flags = page_flags_[page];
    if (!(flags & kNewer)) {
      continue;
    }
    if (!(flags & kValid)) {
      // Its bytes outside what the resolves wrote never reached the pool (the
      // guest page could not be read): guest memory keeps them.
      ++stats_.lock_pages_unknown;
      Fail("a locked page the resolve memory does not hold whole",
           std::format("{:#010x}", page << kPageShift));
      continue;
    }
    pages_.push_back(page);
  }
  if (pages_.empty()) {
    return 0;
  }
  if (device_->IsLost()) {
    return -1;
  }
  submit_recorded();
  if (!EnsureReadback(uint64_t(pages_.size()) * kPageBytes)) {
    return -1;
  }
  plume::RenderCommandList* list = readback_list_.get();
  list->begin();
  PoolBarrier(list, PoolAccess::kCopyRead);
  // Runs of pages next to each other in the pool, one copy each.
  size_t run_begin = 0;
  uint64_t run_pool = 0;
  uint32_t run_pages = 0;
  const auto flush = [&]() {
    if (run_pages) {
      list->copyBufferRegion(plume::RenderBufferReference(readback_buffer_.get(),
                                                          uint64_t(run_begin) * kPageBytes),
                             plume::RenderBufferReference(pool_.get(), run_pool),
                             uint64_t(run_pages) * kPageBytes);
    }
  };
  for (size_t i = 0; i < pages_.size(); ++i) {
    const uint32_t page = pages_[i];
    const uint64_t pool = (uint64_t(chunk_slot_[page >> (kChunkShift - kPageShift)])
                           << kChunkShift) |
                          (uint64_t(page & (kPagesPerChunk - 1)) << kPageShift);
    if (run_pages && pool == run_pool + uint64_t(run_pages) * kPageBytes) {
      ++run_pages;
    } else {
      flush();
      run_begin = i;
      run_pool = pool;
      run_pages = 1;
    }
  }
  flush();
  list->end();
  const plume::RenderCommandList* lists[] = {list};
  device_->queue()->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, readback_fence_.get());
  if (device_->CheckLost("lock write-back")) {
    return -1;
  }
  device_->queue()->waitForCommandFence(readback_fence_.get());
  const auto* mapped = static_cast<const uint8_t*>(readback_buffer_->map());
  if (!mapped) {
    Fail("the lock write-back's readback buffer could not be mapped", "");
    return -1;
  }
  int64_t written = 0;
  for (size_t i = 0; i < pages_.size(); ++i) {
    const uint32_t page = pages_[i];
    // The physical view: the watch does not see this write, and guest memory
    // now holds what the pool holds.
    uint8_t* out = memory_->WritablePhysical(page << kPageShift, kPageBytes);
    if (!out) {
      Fail("a locked page that cannot be written", std::format("{:#010x}", page << kPageShift));
      continue;
    }
    std::memcpy(out, mapped + i * kPageBytes, kPageBytes);
    page_flags_[page] &= uint8_t(~kNewer);
    ++written;
  }
  readback_buffer_->unmap();
  ++stats_.lock_write_backs;
  stats_.lock_pages += uint64_t(written);
  return written;
}

// ---------------------------------------------------------------------------
// housekeeping
// ---------------------------------------------------------------------------

void ResolveMemory::DestroyDecoded(Decoded& d, bool for_shutdown) {
  for (RegionIndex::Id& id : d.region_ids) {
    if (id == RegionIndex::kNone) {
      continue;
    }
    if (id < decoded_owners_.size()) {
      std::vector<Decoded*>& owners = decoded_owners_[id];
      owners.erase(std::remove(owners.begin(), owners.end(), &d), owners.end());
      if (owners.empty()) {
        decoded_regions_.Remove(id);
      }
    }
    id = RegionIndex::kNone;
  }
  if (for_shutdown) {
    d.views.Drop();
    return;
  }
  d.views.Release(*frames_, *heap_);
  for (auto& framebuffer : d.level_framebuffers) {
    frames_->Retire(std::move(framebuffer));
  }
  for (auto& view : d.level_views) {
    frames_->Retire(std::move(view));
  }
  frames_->Retire(std::move(d.texture));
}

void ResolveMemory::EvictSome() {
  bool any = false;
  for (auto it = decoded_.begin(); it != decoded_.end();) {
    Decoded& d = *it->second;
    if (d.last_frame + kEvictFrames < frame_) {
      DestroyDecoded(d, false);
      ++stats_.evicted;
      any = true;
      it = decoded_.erase(it);
    } else {
      ++it;
    }
  }
  if (any) {
    textures_->BumpEpoch();  // no memoized binding may name a released slot
  }
}

std::string ResolveMemory::TakeSummary(uint64_t frames) {
  const double f = frames ? double(frames) : 1.0;
  std::string text = std::format(
      "resolve memory: encodes {:.1f}/frame ({:.2f} MiB/frame), {} failed, {} replaced before "
      "running | batches {:.2f}/frame, {} barriers inside them | pool {} of {} chunks ({:.1f} of "
      "{} MiB), grown {}, chunks placed {}, pages uploaded {} | fetches over it {} checked, {} "
      "bound, {} decoded ({} levels), {} unsupported, {} failed | decoded textures {} live, {} "
      "evicted | CPU writes into its pages {} ({} with bytes guest memory lacked) | locks "
      "written back {} ({} pages, {} not held whole)",
      double(stats_.encodes) / f, double(stats_.encoded_bytes) / f / 1048576.0,
      stats_.encode_failures, stats_.encodes_dropped, double(stats_.flushes) / f, stats_.waves,
      used_chunks_, pool_chunks_,
      double(uint64_t(used_chunks_) * kChunkBytes) / 1048576.0,
      uint64_t(pool_chunks_) * kChunkBytes >> 20, stats_.pool_growths, stats_.chunks_placed,
      stats_.pages_uploaded, stats_.holds_checks, stats_.binds, stats_.decodes,
      stats_.decoded_levels, stats_.bind_unsupported, stats_.bind_failures, decoded_.size(),
      stats_.evicted, stats_.cpu_writes, stats_.cpu_writes_newer, stats_.lock_write_backs,
      stats_.lock_pages, stats_.lock_pages_unknown);
  stats_ = Stats{};
  return text;
}

}  // namespace mcla::gpu
