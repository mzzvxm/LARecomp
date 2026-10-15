// MCLA GPU renderer -- the resolve bridge. See resolve.h.
//
// Semantics: the guest's D3DDevice_Resolve (sub_82420BA8, read in the IDB,
// and 24.28) for the arguments, the rectangle and the clears;
// Xenia's resolve (ReXGlue SDK src/graphics/util/draw.cpp GetResolveInfo and
// SanitizeCopySampleSelect; Xenia's shaders resolve.xesli, resolve_full_*.xesli
// and host_depth_store_*.xesli for the sample order; Copyright Ben Vanik and
// the Xenia authors, BSD-3-Clause) for what the copy writes.

#include "resolve.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

#include <chrono>
#include <filesystem>
#include <fstream>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>

#include "descriptors.h"
#include "device.h"
#include "draw_decode.h"
#include "draw_state.h"
#include "edram.h"
#include "format.h"
#include "pass_timing.h"
#include "frame.h"
#include "host_passes.h"
#include "host_shaders.h"
#include "texture_decode.h"

REXCVAR_DEFINE_BOOL(mcla_gpu_lock_dump, false, "MCLA/GPU",
                    "Diagnostic: every level a Lock write-back puts into guest memory (8888 "
                    "products) is also written as mcla_gpu_lock_<serial>_<base>_<w>x<h>_L<level>.tga "
                    "next to the executable (the vinyl's mip chain).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gpu {

namespace xe = rex::graphics::xenos;
namespace xreg = rex::graphics::reg;
namespace tu = rex::graphics::texture_util;
using rex::graphics::FormatInfo;

namespace {

const char* FailureName(uint32_t f) {
  static const char* const kNames[] = {
      "device lost",           "record unreadable",       "source surface not bound",
      "source target not made", "destination not handled", "no conversion for the pair",
      "product not made",      "source views not made",   "pipeline not made",
  };
  return f < std::size(kNames) ? kNames[f] : "?";
}

const char* ModeName(uint32_t m) {
  static const char* const kNames[] = {"8888",       "1555",      "8",    "float",
                                       "red32",      "depth",     "depth->8888 raw",
                                       "depth->32f bits"};
  return m < std::size(kNames) ? kNames[m] : "?";
}

int32_t FloorTo8(int32_t v) {
  return v >= 0 ? (v & ~7) : -((-v + 7) & ~7);
}

using Clock = std::chrono::steady_clock;

uint64_t AlignUp(uint64_t v, uint64_t a) {
  return (v + a - 1) / a * a;
}

// Where level `level` of `key` lives in guest memory and how its blocks are
// laid out there: what TextureCache::Upload reads, for the write-back. The request's data and readable are unused.
struct StoredLevel {
  uint32_t address = 0;  // physical origin of the level's storage (base, mip or packed tail)
  uint64_t extent = 0;   // bytes the storage spans from the origin
  texture_decode::Request request;
};

bool StoredLevelOf(const TextureKey& key, uint32_t level, StoredLevel* out) {
  const FormatInfo* info = FormatInfo::Get(key.format);
  if (!info || info->bytes_per_block() == 0) {
    return false;
  }
  const tu::TextureGuestLayout layout = tu::GetGuestTextureLayout(
      xe::DataDimension::k2DOrStacked, key.pitch, key.width, key.height, 1, key.tiled != 0,
      xe::TextureFormat(key.format), key.packed_mips != 0, key.base_page != 0, key.mip_max_level);
  const tu::TextureGuestLayout::Level* stored;
  if (level == 0) {
    if (!key.base_page) {
      return false;
    }
    stored = &layout.base;
    out->address = key.base_page << 12;
  } else {
    if (!key.mip_page) {
      return false;
    }
    const uint32_t stored_level = std::min(level, layout.packed_level);
    stored = &layout.mips[stored_level];
    out->address = (key.mip_page << 12) + layout.mip_offsets_bytes[stored_level];
  }
  out->extent = stored->level_data_extent_bytes;
  texture_decode::Request& r = out->request;
  r.source.row_pitch_bytes = stored->row_pitch_bytes;
  r.source.bytes_per_block = info->bytes_per_block();
  r.source.tiled = key.tiled != 0;
  r.source.endian = key.endian;
  if (key.packed_mips && level >= layout.packed_level) {
    uint32_t ox = 0, oy = 0, oz = 0;
    tu::GetPackedMipOffset(key.width, key.height, 1, xe::TextureFormat(key.format), level, ox, oy,
                           oz);
    r.source.offset_x = ox;
    r.source.offset_y = oy;
  }
  r.width = std::max(uint32_t(key.width) >> level, 1u);
  r.height = std::max(uint32_t(key.height) >> level, 1u);
  r.width_blocks = (r.width + info->block_width - 1) / info->block_width;
  r.height_blocks = (r.height + info->block_height - 1) / info->block_height;
  r.load = format::TextureHost(key.format).load;
  r.decompress = format::TextureDecompress::kNone;  // a product is a render target
  return true;
}

// mcla_gpu_lock_dump: one written-back level of an R8G8B8A8 product as a
// 32-bit top-down TGA (BGRA), from the readback rows.
void DumpLevelTga(uint32_t serial, const TextureKey& key, uint32_t level, uint32_t width,
                  uint32_t height, const uint8_t* rgba, uint32_t row_pitch) {
  const std::filesystem::path path =
      rex::filesystem::GetExecutableFolder() /
      std::format("mcla_gpu_lock_{:04}_{:08x}_{}x{}_L{}.tga", serial, key.base_page << 12,
                  key.width, key.height, level);
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    REXLOG_ERROR("[gpu] lock dump: {} could not be written", path.string());
    return;
  }
  uint8_t header[18] = {};
  header[2] = 2;  // uncompressed true colour
  header[12] = uint8_t(width);
  header[13] = uint8_t(width >> 8);
  header[14] = uint8_t(height);
  header[15] = uint8_t(height >> 8);
  header[16] = 32;
  header[17] = 0x28;  // 8 alpha bits, top-left origin
  out.write(reinterpret_cast<const char*>(header), sizeof(header));
  std::vector<uint8_t> row(size_t(width) * 4);
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* src = rgba + size_t(y) * row_pitch;
    for (uint32_t x = 0; x < width; ++x) {
      row[x * 4 + 0] = src[x * 4 + 2];
      row[x * 4 + 1] = src[x * 4 + 1];
      row[x * 4 + 2] = src[x * 4 + 0];
      row[x * 4 + 3] = src[x * 4 + 3];
    }
    out.write(reinterpret_cast<const char*>(row.data()), std::streamsize(row.size()));
  }
}

}  // namespace

ResolveCache::~ResolveCache() {
  Shutdown();
}

bool ResolveCache::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap,
                              GuestMemory& memory, TextureCache& textures, EdramTargets& edram,
                              HostPasses& passes, PassTiming& pass_timing) {
  pass_timing_ = &pass_timing;
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  memory_ = &memory;
  textures_ = &textures;
  edram_ = &edram;
  passes_ = &passes;
  subscriber_ = memory.Subscribe("resolve products");
  for (uint32_t depth = 0; depth < 2; ++depth) {
    for (uint32_t msaa = 0; msaa < 2; ++msaa) {
      const HostShaderBlob blob = ResolvePixelShader(depth != 0, msaa != 0);
      shaders_[depth][msaa] = passes.CreateShader(blob.data, blob.size, blob.name);
      if (!shaders_[depth][msaa]) {
        return false;
      }
    }
  }
  return true;
}

void ResolveCache::Shutdown() {
  // The GPU is idle and the heap goes next: nothing is handed back to it.
  for (auto& [key, product] : products_) {
    product->views.Drop();
  }
  products_.clear();
  active_by_image_.clear();
  regions_.Clear();
  region_owners_.clear();
  pipelines_.clear();
  readback_buffer_.reset();
  readback_bytes_ = 0;
  readback_fence_.reset();
  readback_list_.reset();
  for (auto& row : shaders_) {
    for (auto& shader : row) {
      shader.reset();
    }
  }
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
  memory_ = nullptr;
  textures_ = nullptr;
  edram_ = nullptr;
  passes_ = nullptr;
}

void ResolveCache::BeginFrame() {
  ++frame_;
  if (frame_ % 60 == 0) {
    EvictSome();
  }
}

void ResolveCache::Fail(Failure failure, const std::string& detail) {
  ++stats_.failures[size_t(failure)];
  uint32_t& logged = failures_logged_[size_t(failure)];
  if (logged < 4) {
    ++logged;
    REXLOG_ERROR("[gpu] resolve: not copied -- {}{}{}{}", FailureName(uint32_t(failure)),
                 detail.empty() ? "" : ": ", detail,
                 logged == 4 ? " (further ones counted, not logged)" : "");
  }
}

// ---------------------------------------------------------------------------
// products
// ---------------------------------------------------------------------------

void ResolveCache::IndexRanges(Product& p) {
  for (uint32_t i = 0; i < 2; ++i) {
    if (!p.ranges[i][1]) {
      continue;
    }
    RegionIndex::Id id = regions_.Find(p.ranges[i][0], p.ranges[i][1]);
    if (id == RegionIndex::kNone) {
      id = regions_.Insert(p.ranges[i][0], p.ranges[i][1]);
    }
    if (region_owners_.size() < regions_.id_bound()) {
      region_owners_.resize(regions_.id_bound());
    }
    region_owners_[id].push_back(&p);
    p.region_ids[i] = id;
  }
}

void ResolveCache::UnindexRanges(Product& p) {
  for (RegionIndex::Id& id : p.region_ids) {
    if (id == RegionIndex::kNone) {
      continue;
    }
    if (id < region_owners_.size()) {
      std::vector<Product*>& owners = region_owners_[id];
      owners.erase(std::remove(owners.begin(), owners.end(), &p), owners.end());
      if (owners.empty()) {
        regions_.Remove(id);
      }
    }
    id = RegionIndex::kNone;
  }
}

TextureKey ResolveCache::ImageKey(const TextureKey& key) {
  TextureKey image = key;
  image.mip_page = 0;
  image.mip_max_level = 0;
  image.packed_mips = 0;
  return image;
}

void ResolveCache::Deactivate(Product& p) {
  if (!p.active) {
    return;
  }
  p.active = false;
  // Memory is the CPU's (or another product's) from now on: what the levels
  // still hold is not what a later write-back may put back.
  for (auto& d : p.dirty) {
    d = {};
  }
  UnindexRanges(p);
  const auto it = active_by_image_.find(ImageKey(p.key));
  if (it != active_by_image_.end() && it->second == &p) {
    active_by_image_.erase(it);
  }
  BumpGenerations(p);
}

void ResolveCache::BumpGenerations(const Product& p) {
  // A fetch is memoized by its base page, or by its mip page when it has no
  // base (Scene::FillTextureTables): both kinds may bind this product.
  textures_->BumpProductGeneration(p.key.base_page);
  if (p.key.mip_page) {
    textures_->BumpProductGeneration(p.key.mip_page);
  }
}

void ResolveCache::DrainInvalidations() {
  if (!memory_->HasInvalidations(subscriber_)) {
    return;
  }
  drained_.clear();
  memory_->Drain(subscriber_, drained_);
  scratch_hits_.clear();
  for (const GuestMemory::Range& range : drained_) {
    regions_.ForEachOverlap(range.start, range.length, [&](RegionIndex::Id id) {
      if (id < region_owners_.size()) {
        for (Product* p : region_owners_[id]) {
          scratch_hits_.push_back(p);
        }
      }
    });
  }
  for (Product* p : scratch_hits_) {
    if (p->active) {
      // The guest wrote into what it rendered: its own bytes win (M5 writes the
      // image back so a partial write merges instead). MCLA's vinyl work area
      // is locked and distilled in place by the CPU: what it read
      // there was not the image, since memory does not hold it until M5.
      ++stats_.deactivated_by_cpu;
      if (cpu_writes_logged_ < 4) {
        ++cpu_writes_logged_;
        REXLOG_WARN("[gpu] resolve: the CPU wrote into the product {}; it is deactivated and the "
                    "texture is decoded from memory again -- whatever the CPU read there was not "
                    "the image (write-back on Lock, M5){}",
                    p->key.Describe(),
                    cpu_writes_logged_ == 4 ? " (further ones counted, not logged)" : "");
      }
      Deactivate(*p);
    }
  }
}

ResolveCache::Product* ResolveCache::FindImage(const TextureKey& key) {
  const auto it = products_.find(key);
  if (it != products_.end() && it->second->active) {
    return it->second.get();
  }
  const auto image = active_by_image_.find(ImageKey(key));
  if (image == active_by_image_.end()) {
    return nullptr;
  }
  Product* candidate = image->second;
  // The fetch's levels must be among the product's, read from the same mip
  // memory (a fetch without mips reads the base alone).
  if (key.mip_max_level <= candidate->key.mip_max_level &&
      (key.mip_page == 0 || (key.mip_page == candidate->key.mip_page &&
                             key.packed_mips == candidate->key.packed_mips))) {
    ++stats_.found_by_image;
    return candidate;
  }
  return nullptr;
}

ResolveCache::Product* ResolveCache::Find(const TextureKey& key, uint32_t image_base_page) {
  ++stats_.finds;
  Product* p = FindImage(key);
  if (!p && key.base_page == 0 && key.mip_page != 0 && image_base_page != 0) {
    // The SDK dropped the base because the fetch's mips start above level 0;
    // the guest's image still has it, and so does its product's key.
    TextureKey with_base = key;
    with_base.base_page = image_base_page;
    p = FindImage(with_base);
    if (p) {
      ++stats_.found_by_image_base;
    }
  }
  if (!p) {
    return nullptr;
  }
  ++stats_.found;
  p->last_frame = frame_;
  return p;
}

const ResolveCache::Product* ResolveCache::FindOverlap(uint32_t start, uint32_t length) {
  const Product* found = nullptr;
  regions_.ForEachOverlap(start, length, [&](RegionIndex::Id id) {
    if (!found && id < region_owners_.size() && !region_owners_[id].empty()) {
      found = region_owners_[id].front();
    }
  });
  return found;
}

void ResolveCache::OnLock(const LockRecord& record,
                          const std::function<void()>& submit_recorded) {
  // A CPU write since the resolve already took the range from the product.
  DrainInvalidations();
  ++stats_.locks;
  lock_hits_.clear();
  regions_.ForEachOverlap(record.address, record.size, [&](RegionIndex::Id id) {
    if (id >= region_owners_.size()) {
      return;
    }
    for (Product* p : region_owners_[id]) {
      if (p->active && std::find(lock_hits_.begin(), lock_hits_.end(), p) == lock_hits_.end()) {
        lock_hits_.push_back(p);
      }
    }
  });
  if (!lock_hits_.empty()) {
    ++stats_.locks_on_products;
  }
  // Every level of the products hit that holds texels memory lacks, not only
  // the levels in the range: the CPU's next write anywhere in a product
  // deactivates all of it (the watch covers its whole range), and MCLA's vinyl
  // compressor locks level 0, distills it in place, then locks level 1 -- by
  // then the product is gone and only memory has the levels.
  write_back_.clear();
  uint64_t bytes = 0;
  for (Product* p : lock_hits_) {
    for (uint32_t level = 0; level < p->dirty.size(); ++level) {
      const std::array<uint32_t, 4>& d = p->dirty[level];
      if (d[2] <= d[0] || d[3] <= d[1]) {
        continue;
      }
      StoredLevel stored;
      if (!StoredLevelOf(p->key, level, &stored)) {
        continue;
      }
      WriteBackItem item;
      item.product = p;
      item.level = level;
      item.width = stored.request.width;
      item.height = stored.request.height;
      const uint32_t texel_bytes = plume::RenderFormatSize(p->format);
      item.row_pitch = uint32_t(AlignUp(uint64_t(item.width) * texel_bytes, 256));
      item.row_width = item.row_pitch / texel_bytes;
      item.offset = AlignUp(bytes, 512);
      bytes = item.offset + uint64_t(item.row_pitch) * item.height;
      write_back_.push_back(item);
    }
  }
  const uint64_t id = uint64_t(record.address) << 32 | record.size;
  if (locks_logged_.size() < 32 && locks_logged_.insert(id).second) {
    std::string what = "no active product (the CPU's memory already)";
    if (!lock_hits_.empty()) {
      what = std::format("{} level(s) of {} to write back", write_back_.size(),
                         lock_hits_.front()->key.Describe());
    }
    REXLOG_INFO("[gpu] lock: {:#010x} +{} (level {}, resource {:#010x}): {}", record.address,
                record.size, record.level, record.resource, what);
  }
  if (write_back_.empty() || device_->IsLost()) {
    return;
  }
  const auto t0 = Clock::now();
  submit_recorded();
  if (EnsureReadback(bytes) && WriteBack(write_back_)) {
    ++stats_.locks_written_back;
  }
  const uint64_t ns = uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
  stats_.write_back_ns += ns;
  stats_.write_back_max_ns = std::max(stats_.write_back_max_ns, ns);
  if (write_back_logged_ < 8) {
    ++write_back_logged_;
    REXLOG_INFO("[gpu] lock write-back: {:#010x} +{}: {} level(s) of {} in {:.2f} ms{}",
                record.address, record.size, write_back_.size(),
                write_back_.front().product->key.Describe(), double(ns) / 1e6,
                write_back_logged_ == 8 ? " (further ones counted, not logged)" : "");
  }
}

bool ResolveCache::EnsureReadback(uint64_t bytes) {
  if (!readback_list_) {
    readback_list_ = device_->queue()->createCommandList();
    readback_fence_ = device_->device()->createCommandFence();
    if (!readback_list_ || !readback_fence_) {
      readback_list_.reset();
      readback_fence_.reset();
      ++stats_.write_back_failures;
      REXLOG_ERROR("[gpu] lock write-back: the readback list or fence could not be made; the CPU "
                   "reads memory without the GPU's image");
      return false;
    }
  }
  if (bytes > readback_bytes_) {
    // No copy is in flight: every write-back waited for its fence.
    readback_buffer_.reset();
    const uint64_t size = AlignUp(bytes, uint64_t(4) << 20);
    readback_buffer_ = device_->CreateBuffer(plume::RenderBufferDesc::ReadbackBuffer(size),
                                             "resolve write-back readback");
    if (!readback_buffer_) {
      readback_bytes_ = 0;
      ++stats_.write_back_failures;
      REXLOG_ERROR("[gpu] lock write-back: a readback buffer of {} bytes could not be made; the "
                   "CPU reads memory without the GPU's image",
                   size);
      return false;
    }
    readback_bytes_ = size;
  }
  return true;
}

bool ResolveCache::WriteBack(std::vector<WriteBackItem>& items) {
  ++write_back_serial_;
  plume::RenderCommandList* list = readback_list_.get();
  list->begin();
  for (const WriteBackItem& item : items) {
    Product& p = *item.product;
    if (p.layout != plume::RenderTextureLayout::COPY_SOURCE) {
      list->barriers(plume::RenderBarrierStage::COPY,
                     plume::RenderTextureBarrier(p.texture.get(),
                                                 plume::RenderTextureLayout::COPY_SOURCE));
      p.layout = plume::RenderTextureLayout::COPY_SOURCE;
    }
    list->copyTextureRegion(
        plume::RenderTextureCopyLocation::PlacedFootprint(readback_buffer_.get(), p.format,
                                                          item.width, item.height, 1,
                                                          item.row_width, item.offset),
        plume::RenderTextureCopyLocation::Subresource(p.texture.get(), item.level, 0));
  }
  // Back to the layout every reader of a product expects (Execute leaves it so).
  for (const WriteBackItem& item : items) {
    TransitionProduct(list, *item.product, plume::RenderTextureLayout::SHADER_READ);
  }
  list->end();
  const plume::RenderCommandList* lists[] = {list};
  device_->queue()->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, readback_fence_.get());
  if (device_->CheckLost("lock write-back")) {
    return false;
  }
  device_->queue()->waitForCommandFence(readback_fence_.get());

  const auto* mapped = static_cast<const uint8_t*>(readback_buffer_->map());
  if (!mapped) {
    ++stats_.write_back_failures;
    REXLOG_ERROR("[gpu] lock write-back: the readback buffer could not be mapped");
    return false;
  }
  bool ok = true;
  for (const WriteBackItem& item : items) {
    Product& p = *item.product;
    std::array<uint32_t, 4>& d = p.dirty[item.level];
    const FormatInfo* info = FormatInfo::Get(p.key.format);
    StoredLevel stored;
    std::string why;
    if (!StoredLevelOf(p.key, item.level, &stored)) {
      why = "no storage for the level";
    } else if (plume::RenderFormatSize(p.format) != stored.request.source.bytes_per_block) {
      why = std::format("host texel of {} bytes for a guest block of {}",
                        plume::RenderFormatSize(p.format), stored.request.source.bytes_per_block);
    } else if (!texture_decode::CanEncode(stored.request)) {
      why = std::format("format {} has no inverse load yet (TODO M5.1b)",
                        format::TextureHost(p.key.format).guest_name);
    }
    uint8_t* out = why.empty() ? memory_->WritablePhysical(stored.address, uint32_t(stored.extent))
                               : nullptr;
    if (why.empty() && !out) {
      why = std::format("{} bytes at {:#010x} not writable", stored.extent, stored.address);
    }
    if (why.empty()) {
      // Only the blocks the resolves wrote: memory keeps the rest.
      const uint32_t bw = info->block_width, bh = info->block_height;
      const uint32_t bx0 = d[0] / bw, by0 = d[1] / bh;
      const uint32_t bx1 = (d[2] + bw - 1) / bw, by1 = (d[3] + bh - 1) / bh;
      texture_decode::Request r = stored.request;
      r.source.offset_x += bx0;
      r.source.offset_y += by0;
      r.width_blocks = bx1 - bx0;
      r.height_blocks = by1 - by0;
      const uint8_t* in = mapped + item.offset + uint64_t(by0) * item.row_pitch +
                          uint64_t(bx0) * r.source.bytes_per_block;
      if (texture_decode::Encode(r, in, item.row_pitch, out, stored.extent)) {
        ++stats_.levels_written_back;
        stats_.write_back_bytes +=
            uint64_t(r.width_blocks) * r.height_blocks * r.source.bytes_per_block;
        d = {};
        if (REXCVAR_GET(mcla_gpu_lock_dump) && p.format == plume::RenderFormat::R8G8B8A8_UNORM) {
          DumpLevelTga(write_back_serial_, p.key, item.level, item.width, item.height,
                       mapped + item.offset, item.row_pitch);
        }
        continue;
      }
      why = "a block beyond the level's storage";
    }
    ok = false;
    ++stats_.write_back_failures;
    if (write_back_failures_logged_ < 8) {
      ++write_back_failures_logged_;
      REXLOG_ERROR("[gpu] lock write-back: level {} of {} not written ({}); the CPU reads memory "
                   "without the GPU's image{}",
                   item.level, p.key.Describe(), why,
                   write_back_failures_logged_ == 8 ? " (further ones counted, not logged)" : "");
    }
  }
  readback_buffer_->unmap();
  return ok;
}

void ResolveCache::TransitionProduct(plume::RenderCommandList* list, Product& p,
                                     plume::RenderTextureLayout layout) {
  if (p.layout == layout) {
    return;
  }
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(p.texture.get(), layout));
  p.layout = layout;
}

plume::RenderFramebuffer* ResolveCache::LevelFramebuffer(Product& p, uint32_t level) {
  if (level >= p.level_framebuffers.size()) {
    return nullptr;
  }
  if (!p.level_framebuffers[level]) {
    const plume::RenderTexture* textures[1] = {p.texture.get()};
    const plume::RenderTextureView* views[1] = {p.level_views[level].get()};
    plume::RenderFramebufferDesc desc(textures, 1);
    desc.colorAttachmentViews = views;
    p.level_framebuffers[level] = device_->CreateFramebuffer(desc, "resolve product level");
  }
  return p.level_framebuffers[level].get();
}

ResolveCache::Product* ResolveCache::Activate(const TextureKey& key, plume::RenderFormat format,
                                              plume::RenderCommandList* list, std::string* why) {
  auto it = products_.find(key);
  Product* p = it != products_.end() ? it->second.get() : nullptr;
  if (!p) {
    auto product = std::make_unique<Product>();
    product->key = key;
    product->format = format;
    plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(
        key.width, key.height, key.levels(), format, plume::RenderTextureFlag::RENDER_TARGET);
    const std::string name = std::format("resolve product {}", key.Describe());
    product->texture = device_->CreateTexture(desc, name.c_str());
    if (!product->texture) {
      *why = std::format("{} {}x{} with {} levels", format::PlumeFormatName(format), key.width,
                         key.height, key.levels());
      return nullptr;
    }
    product->level_views.resize(key.levels());
    product->level_framebuffers.resize(key.levels());
    product->dirty.assign(key.levels(), {});
    for (uint32_t level = 0; level < key.levels(); ++level) {
      plume::RenderTextureViewDesc view = plume::RenderTextureViewDesc::Texture2D(format);
      view.mipSlice = level;
      view.mipLevels = 1;
      product->level_views[level] =
          device_->CreateTextureView(*product->texture, view, "resolve product level view");
      if (!product->level_views[level]) {
        *why = std::format("render target view of level {}", level);
        return nullptr;
      }
    }
    TextureGuestRanges(key, product->ranges);
    // Every level cleared before anything reads or writes it: a render target
    // from an unzeroed heap must be (D3D12 debug layer id 1422), and memory the
    // guest never wrote reads as zeros here.
    TransitionProduct(list, *product, plume::RenderTextureLayout::COLOR_WRITE);
    for (uint32_t level = 0; level < key.levels(); ++level) {
      plume::RenderFramebuffer* framebuffer = LevelFramebuffer(*product, level);
      if (!framebuffer) {
        list->setFramebuffer(nullptr);
        *why = std::format("framebuffer of level {}", level);
        return nullptr;
      }
      list->setFramebuffer(framebuffer);
      list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
    }
    list->setFramebuffer(nullptr);
    ++stats_.created;
    p = product.get();
    products_.emplace(key, std::move(product));
  }
  p->last_frame = frame_;
  if (!p->active) {
    // Versioned in time: whatever product covers these bytes with another key
    // is what the guest had there before this resolve.
    scratch_hits_.clear();
    for (const auto& r : p->ranges) {
      if (!r[1]) {
        continue;
      }
      regions_.ForEachOverlap(r[0], r[1], [&](RegionIndex::Id id) {
        if (id < region_owners_.size()) {
          for (Product* other : region_owners_[id]) {
            if (other != p) {
              scratch_hits_.push_back(other);
            }
          }
        }
      });
    }
    for (Product* other : scratch_hits_) {
      if (other->active) {
        ++stats_.deactivated_by_overlap;
        Deactivate(*other);
      }
    }
    p->active = true;
    IndexRanges(*p);
    active_by_image_[ImageKey(p->key)] = p;
    // A CPU write into these bytes from now on deactivates the product.
    for (const auto& r : p->ranges) {
      if (r[1]) {
        memory_->Arm(r[0], r[1]);
      }
    }
    ++stats_.activated;
    BumpGenerations(*p);
  }
  return p;
}

plume::RenderPipeline* ResolveCache::Pipeline(bool depth, bool msaa, plume::RenderFormat format) {
  const uint32_t key = (uint32_t(depth) << 9) | (uint32_t(msaa) << 8) | uint32_t(format);
  auto it = pipelines_.find(key);
  if (it != pipelines_.end()) {
    return it->second.get();
  }
  const std::string name = std::format("resolve {}{} -> {}", depth ? "depth" : "colour",
                                       msaa ? " msaa" : "", format::PlumeFormatName(format));
  std::unique_ptr<plume::RenderPipeline> pipeline =
      passes_->CreatePipeline(*shaders_[depth][msaa], format, name.c_str());
  plume::RenderPipeline* raw = pipeline.get();
  pipelines_.emplace(key, std::move(pipeline));
  return raw;
}

// ---------------------------------------------------------------------------
// execution
// ---------------------------------------------------------------------------

void ResolveCache::Execute(const ResolveRecord& r, plume::RenderCommandList* list) {
  ++stats_.resolves;
  if (!list) {
    Fail(Failure::kDeviceLost, "");
    return;
  }
  if (r.flags & (kEventSurfaceUnreadable | kEventStackUnreadable)) {
    Fail(Failure::kUnreadable, std::format("event flags {:#x}", r.flags));
    return;
  }
  const uint32_t flags = r.resolve_flags;
  const uint32_t source = flags & 7;
  const bool depth_source = source == 4;
  const bool copy = (flags & 0x2000) == 0;
  const RegisterWindow pre(r.state.pre, guest::kEventWindowBegin, guest::kEventWindowBytes);
  const uint32_t surfaces[guest::kSurfaceCount] = {
      pre.DeviceDword(guest::kDevColorSurfaces), pre.DeviceDword(guest::kDevColorSurfaces + 4),
      pre.DeviceDword(guest::kDevColorSurfaces + 8),
      pre.DeviceDword(guest::kDevColorSurfaces + 12), pre.DeviceDword(guest::kDevDepthSurface)};
  const auto header_dword = [&](uint32_t surface, uint32_t offset) {
    return LoadBe32(r.state.surfaces[surface] + offset);
  };
  // The EDRAM identity of a surface slot. Pitch and samples: the device's
  // RB_SURFACE_INFO (dev+10368) for every source -- the copy reads the
  // registers the call flushes, and SetRenderTarget(0) writes that one from
  // RT0's header +0x18, SetDepthStencilSurface from the depth's only while no
  // RT0 is bound (sub_824165E0). The source's own +0x18 is the pitch it was
  // made with, which a depth surface shared between passes does not keep;
  // Resolve (sub_82420BA8) reads it only for the default sample select
  // (*header_samples). Base and format: the bound surface's header +0x1C,
  // which SetRenderTarget/SetDepthStencilSurface copy into
  // RB_COLOR*_INFO/RB_DEPTH_INFO; with no surface bound there, those registers.
  const auto info_register = [](uint32_t surface) -> uint32_t {
    if (surface == 4) return guest::reg::kRbDepthInfo;
    return surface == 0 ? guest::reg::kRbColorInfo : guest::reg::kRbColor1Info + surface - 1;
  };
  const auto key_of = [&](uint32_t surface, EdramKey* key, uint32_t* format, int32_t* exp_bias,
                          uint32_t* header_samples = nullptr) {
    const bool bound = surfaces[surface] != 0;
    if (!bound) {
      ++stats_.from_registers;
    }
    const auto header_or_register = [&](uint32_t offset, uint32_t reg) {
      return bound ? header_dword(surface, offset) : pre.Register(reg);
    };
    xreg::RB_SURFACE_INFO surface_info;
    surface_info.value = pre.Register(guest::reg::kRbSurfaceInfo);
    key->pitch = uint16_t(surface_info.surface_pitch);
    key->samples = uint8_t(MsaaSampleCount(surface_info.msaa_samples));
    xreg::RB_SURFACE_INFO own;
    own.value = header_or_register(0x18, guest::reg::kRbSurfaceInfo);
    const bool other_pitch = own.surface_pitch != surface_info.surface_pitch ||
                             own.msaa_samples != surface_info.msaa_samples;
    if (header_samples) {
      *header_samples = MsaaSampleCount(own.msaa_samples);
    }
    if (surface == 4) {
      xreg::RB_DEPTH_INFO depth_info;
      depth_info.value = header_or_register(0x1C, info_register(surface));
      key->base_tiles = uint16_t(depth_info.depth_base | (depth_info.depth_base_bit_11 << 11));
      key->depth = 1;
      key->format = uint8_t(depth_info.depth_format);
      *exp_bias = 0;
    } else {
      xreg::RB_COLOR_INFO color_info;
      color_info.value = header_or_register(0x1C, info_register(surface));
      key->base_tiles = uint16_t(color_info.color_base | (color_info.color_base_bit_11 << 11));
      key->depth = 0;
      key->format = uint8_t(FoldColorFormat(uint32_t(color_info.color_format)));
      *exp_bias = color_info.color_exp_bias;
    }
    *format = key->format;
    if (other_pitch) {
      ++stats_.other_pitch;
      const uint64_t id = uint64_t(surface) | uint64_t(key->base_tiles) << 4 |
                          uint64_t(key->format) << 16 | uint64_t(own.surface_pitch) << 24 |
                          uint64_t(own.msaa_samples) << 38 | uint64_t(key->pitch) << 40 |
                          uint64_t(surface_info.msaa_samples) << 54;
      if (other_pitch_logged_.size() < 16 && other_pitch_logged_.insert(id).second) {
        const char* name = surface == 4 ? format::DepthTargetFormat(key->format).guest_name
                                        : format::ColorTargetFormat(key->format).guest_name;
        REXLOG_INFO(
            "[gpu] resolve: source {} ({}@{}) was made at pitch {} {}x and is read at the "
            "device's pitch {} {}x",
            surface == 4 ? "depth" : std::format("colour {}", surface), name, key->base_tiles,
            uint32_t(own.surface_pitch), MsaaSampleCount(own.msaa_samples), key->pitch,
            key->samples);
      }
    }
  };

  // The rectangle: the guest's, or the destination level's whole extent.
  TextureFetch dest;
  bool have_dest = false;
  if ((r.flags & kEventHasDest) && !(r.flags & kEventDestUnreadable)) {
    uint32_t d[6];
    for (uint32_t i = 0; i < 6; ++i) {
      d[i] = LoadBe32(r.dest_header + guest::kTextureFetchOffset + 4 * i);
    }
    dest = ReadTextureFetch(d);
    have_dest = dest.is_texture;
  }
  const uint32_t level = r.dest_level;
  const uint32_t level_width = have_dest ? std::max(uint32_t(dest.key.width) >> level, 1u) : 0;
  const uint32_t level_height = have_dest ? std::max(uint32_t(dest.key.height) >> level, 1u) : 0;
  int32_t x0 = 0, y0 = 0, x1 = int32_t(level_width), y1 = int32_t(level_height);
  if (r.flags & kEventHasRect) {
    x0 = int32_t(LoadBe32(r.src_rect + 0));
    y0 = int32_t(LoadBe32(r.src_rect + 4));
    x1 = int32_t(LoadBe32(r.src_rect + 8));
    y1 = int32_t(LoadBe32(r.src_rect + 12));
  } else if (!have_dest) {
    Fail(Failure::kUnreadable, "no rectangle and no destination to size it");
    return;
  }
  int32_t px = 0, py = 0;
  if (r.flags & kEventHasPoint) {
    px = int32_t(LoadBe32(r.dest_point + 0));
    py = int32_t(LoadBe32(r.dest_point + 4));
  }
  // The hardware's 8x8 granularity (Xenia; the IDB rounds x1/y1 up itself).
  const int32_t ax0 = std::max(FloorTo8(x0), 0);
  const int32_t ay0 = std::max(FloorTo8(y0), 0);
  const int32_t ax1 = (x1 + 7) & ~7;
  const int32_t ay1 = (y1 + 7) & ~7;

  if (copy) {
    do {
      if (source > 4) {
        Fail(Failure::kNoSource, std::format("source {}", source));
        break;
      }
      if (!have_dest) {
        Fail(Failure::kUnreadable, "destination texture header");
        break;
      }
      const TextureKey& key = dest.key;
      if (key.dimension != uint8_t(xe::DataDimension::k2DOrStacked) || key.stacked ||
          r.dest_slice != 0 || level > key.mip_max_level) {
        Fail(Failure::kDestination,
             std::format("{}: dimension {}, stacked {}, slice {}, level {} (TODO M5)",
                         key.Describe(), key.dimension, key.stacked, r.dest_slice, level));
        break;
      }
      EdramKey src_key;
      uint32_t src_format = 0;
      int32_t src_bias = 0;
      uint32_t src_header_samples = 1;
      key_of(source, &src_key, &src_format, &src_bias, &src_header_samples);
      // The conversion: what the destination's fetch reads.
      uint32_t mode = kModeCount;
      switch (key.format) {
        case 6: mode = depth_source ? kModeDepthRaw8888 : kModeUnorm8888; break;
        case 3: mode = depth_source ? kModeCount : kModeUnorm1555; break;
        case 2: mode = depth_source ? kModeCount : kModeUnorm8; break;
        case 29:
        case 32: mode = depth_source ? kModeCount : kModeFloat; break;
        case 36: mode = depth_source ? kModeDepthFloatBits : kModeRed32; break;
        case 22: mode = depth_source ? kModeDepthFloat : kModeCount; break;
        case 23:
          mode = depth_source ? uint32_t(kModeDepthFloat) | uint32_t(kFlagDestFloat24)
                              : uint32_t(kModeCount);
          break;
        default: break;
      }
      const format::TextureHostFormat host = format::TextureHost(key.format);
      if (mode == kModeCount || host.status != format::Status::kOk) {
        Fail(Failure::kConversion, std::format("{} from {} format {}", key.Describe(),
                                               depth_source ? "depth" : "colour", src_format));
        break;
      }
      uint32_t full_mode = mode;
      if (!depth_source) {
        // copy_dest_swap: the destination's swizzle does not start with X (IDB).
        if ((dest.swizzle & 7) != 0) {
          full_mode |= kFlagSwapRedBlue;
        }
        if (src_format == 1) {
          full_mode |= kFlagGammaSource;  // k_8_8_8_8_GAMMA, kept linear on the host
        }
      } else if (src_format == uint32_t(xe::DepthRenderTargetFormat::kD24FS8)) {
        full_mode |= kFlagSourceFloat24;
      }

      // Samples: bits 4-6 of the flags, 0 = by the MSAA of the source
      // surface's own header (IDB: 1x k0, 2x k01, 4x k0123), else the value
      // - 1; Xenia's sanitizing by the MSAA the copy reads; guest -> host
      // order (2x: guest 0 is host 1; 4x: guest s is host
      // ((s & 1) << 1) | (s >> 1)).
      const uint32_t samples = src_key.samples;
      uint32_t select = (flags >> 4) & 7;
      if (select == 0) {
        select = src_header_samples >= 4 ? 6 : (src_header_samples >= 2 ? 4 : 0);
      } else {
        select -= 1;
      }
      if (samples >= 4) {
        select = std::min(select, 6u);
        if (depth_source) {
          if (select == 4 || select == 6) select = 0;
          if (select == 5) select = 2;
        }
      } else if (samples >= 2) {
        if (select == 2) select = 0;
        else if (select == 3) select = 1;
        else if (select > 4) select = 4;
        if (depth_source && select == 4) select = 0;
      } else {
        select = 0;
      }
      uint32_t host_samples[4] = {};
      uint32_t count = 1;
      const auto host_of = [&](uint32_t s) -> uint32_t {
        if (samples >= 4) return ((s & 1) << 1) | (s >> 1);
        if (samples >= 2) return s == 0 ? 1u : 0u;
        return 0u;
      };
      if (select <= 3) {
        host_samples[0] = host_of(select);
      } else if (select == 4) {
        host_samples[0] = host_of(0);
        host_samples[1] = host_of(1);
        count = 2;
      } else if (select == 5) {
        host_samples[0] = host_of(2);
        host_samples[1] = host_of(3);
        count = 2;
      } else {
        for (uint32_t s = 0; s < 4; ++s) host_samples[s] = host_of(s);
        count = 4;
      }

      // Destination rectangle in the level: source - (x0, y0) + point.
      const int32_t dx = px - x0, dy = py - y0;
      if ((dx & 31) != 0 || (dy & 31) != 0) {
        ++stats_.misaligned;
        if (stats_.misaligned == 1) {
          REXLOG_WARN("[gpu] resolve: destination offset ({}, {}) is not whole 32x32 tiles; "
                      "copied as a plain offset (the console's tiled addressing would scatter "
                      "it; reported once)",
                      dx, dy);
        }
      }
      // Source columns stop at the target's pitch (its width on the host): the
      // pass never loads outside the texture.
      const int32_t sx1 = std::min(ax1, int32_t(src_key.pitch));
      const int32_t tx0 = std::max(ax0 + dx, 0);
      const int32_t ty0 = std::max(ay0 + dy, 0);
      const int32_t tx1 = std::min(sx1 + dx, int32_t(level_width));
      const int32_t ty1 = std::min(ay1 + dy, int32_t(level_height));
      if (tx1 <= tx0 || ty1 <= ty0) {
        break;  // nothing lands in the level
      }
      if (tx0 > 0 || ty0 > 0 || tx1 < int32_t(level_width) || ty1 < int32_t(level_height)) {
        ++stats_.partial;
      }

      // The source target, at least as tall as the copy reads.
      bool unset = false;
      EdramTargets::Target* src = edram_->Get(src_key, uint32_t(ty1 - dy), list, &unset);
      if (!src) {
        Fail(Failure::kSourceFormat, std::format("{} format {} at tile {} pitch {} {}x",
                                                 depth_source ? "depth" : "colour", src_format,
                                                 src_key.base_tiles, src_key.pitch,
                                                 src_key.samples));
        break;
      }
      uint32_t sample_slot = 0, stencil_slot = 0;
      if (!edram_->SampleSlots(*src, &sample_slot, &stencil_slot)) {
        Fail(Failure::kSourceView, std::format("tile {}", src_key.base_tiles));
        break;
      }
      std::string why;
      Product* p = Activate(key, host.format, list, &why);
      if (!p) {
        Fail(Failure::kProduct, std::format("{}: {}", key.Describe(), why));
        break;
      }
      plume::RenderFramebuffer* framebuffer = LevelFramebuffer(*p, level);
      plume::RenderPipeline* pipeline = Pipeline(depth_source, samples > 1, host.format);
      if (!framebuffer || !pipeline) {
        Fail(framebuffer ? Failure::kPipeline : Failure::kProduct, key.Describe());
        break;
      }
      edram_->Transition(list, *src, plume::RenderTextureLayout::SHADER_READ);
      TransitionProduct(list, *p, plume::RenderTextureLayout::COLOR_WRITE);
      if (pass_timing_->enabled()) {
        const char* from = depth_source ? format::DepthTargetFormat(src_format).guest_name
                                        : format::ColorTargetFormat(src_format).guest_name;
        const char* to = format::TextureHost(key.format).guest_name;
        pass_timing_->Mark(list, std::format("resolve {} {}x -> {} {}x{}", from, samples, to,
                                             key.width, key.height));
      }
      list->setFramebuffer(framebuffer);
      const plume::RenderViewport viewport(0.0f, 0.0f, float(level_width), float(level_height));
      list->setViewports(&viewport, 1);
      const plume::RenderRect scissor(tx0, ty0, tx1, ty1);
      list->setScissors(&scissor, 1);
      passes_->Bind(list);
      list->setPipeline(pipeline);
      Push push{};
      push.source = sample_slot;
      push.stencil = depth_source ? stencil_slot : sample_slot;
      push.offset_x = -dx;
      push.offset_y = -dy;
      push.samples = count;
      for (uint32_t i = 0; i < count; ++i) {
        push.samples |= host_samples[i] << (8 + 2 * i);
      }
      float scale = std::ldexp(1.0f, int32_t(int8_t(uint8_t((flags >> 26) << 2)) >> 2));
      if (!depth_source && (src_format == 4 || src_format == 5)) {
        scale *= 32.0f;  // k_16_16(_16_16): the host's SNORM +-1 is +-32 (Xenia: bias + 5)
      }
      push.scale = scale / float(count);
      push.mode = full_mode;
      list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
      list->drawInstanced(3, 1, 0, 0);
      list->setFramebuffer(nullptr);
      TransitionProduct(list, *p, plume::RenderTextureLayout::SHADER_READ);
      // What a write-back of this level owes guest memory.
      std::array<uint32_t, 4>& d = p->dirty[level];
      if (d[2] <= d[0] || d[3] <= d[1]) {
        d = {uint32_t(tx0), uint32_t(ty0), uint32_t(tx1), uint32_t(ty1)};
      } else {
        d = {std::min(d[0], uint32_t(tx0)), std::min(d[1], uint32_t(ty0)),
             std::max(d[2], uint32_t(tx1)), std::max(d[3], uint32_t(ty1))};
      }
      ++stats_.copies;
      ++stats_.modes[mode & 0xFF];
      stats_.pixels += uint64_t(tx1 - tx0) * uint64_t(ty1 - ty0);
    } while (false);
  } else {
    ++stats_.clear_only;
  }

  // The clears the call asked for, after the copy, on the aligned rectangle.
  if (flags & 0x100) {
    if (depth_source || source > 3) {
      Fail(Failure::kNoSource, std::format("colour clear of source {}", source));
    } else {
      EdramKey key;
      uint32_t format = 0;
      int32_t bias = 0;
      key_of(source, &key, &format, &bias);
      bool unset = false;
      EdramTargets::Target* target = edram_->Get(key, uint32_t(ay1), list, &unset);
      if (target) {
        float color[4] = {};
        if (r.flags & kEventHasColor) {
          for (uint32_t i = 0; i < 4; ++i) {
            const uint32_t bits = LoadBe32(r.clear_color + 4 * i);
            std::memcpy(&color[i], &bits, 4);
          }
        }
        float value[4];
        EncodeColorClear(format, bias, color, value);
        if (edram_->ClearColor(list, *target, plume::RenderRect(ax0, ay0, ax1, ay1), value)) {
          ++stats_.color_clears;
        }
        list->setFramebuffer(nullptr);
      } else {
        Fail(Failure::kSourceFormat, "colour clear target");
      }
    }
  }
  if (flags & 0x200) {
    {
      EdramKey key;
      uint32_t format = 0;
      int32_t bias = 0;
      key_of(4, &key, &format, &bias);
      bool unset = false;
      EdramTargets::Target* target = edram_->Get(key, uint32_t(ay1), list, &unset);
      if (target) {
        if (edram_->ClearDepthStencil(list, *target, plume::RenderRect(ax0, ay0, ax1, ay1), true,
                                      true, r.clear_z, r.clear_stencil)) {
          ++stats_.depth_clears;
        }
        list->setFramebuffer(nullptr);
      } else {
        Fail(Failure::kSourceFormat, "depth clear target");
      }
    }
  }
}

// ---------------------------------------------------------------------------
// housekeeping
// ---------------------------------------------------------------------------

void ResolveCache::EvictSome() {
  for (auto it = products_.begin(); it != products_.end();) {
    Product& p = *it->second;
    if (!p.active && p.last_frame + kEvictFrames < frame_) {
      p.views.Release(*frames_, *heap_);
      for (auto& framebuffer : p.level_framebuffers) {
        frames_->Retire(std::move(framebuffer));
      }
      for (auto& view : p.level_views) {
        frames_->Retire(std::move(view));
      }
      frames_->Retire(std::move(p.texture));
      ++stats_.evicted;
      it = products_.erase(it);
    } else {
      ++it;
    }
  }
}

std::string ResolveCache::TakeSummary(uint64_t frames) {
  const double f = frames ? double(frames) : 1.0;
  size_t active = 0;
  for (const auto& [key, p] : products_) {
    active += p->active;
  }
  std::string modes;
  for (uint32_t m = 0; m < kModeCount; ++m) {
    if (stats_.modes[m]) {
      modes += std::format("{}{} {}", modes.empty() ? "" : ", ", ModeName(m), stats_.modes[m]);
    }
  }
  std::string failures;
  for (uint32_t i = 0; i < uint32_t(Failure::kCount); ++i) {
    if (stats_.failures[i]) {
      failures += std::format("{}{} {}", failures.empty() ? "" : ", ", FailureName(i),
                              stats_.failures[i]);
    }
  }
  std::string text = std::format(
      "resolves {:.1f}/frame: {} copied ({:.2f} Mpixel/frame), {} clear only, clears {} colour {} "
      "depth | conversions [{}] | failed [{}] | products {} ({} active), created {}, activated "
      "{}, deactivated by overlap {} / CPU {}, evicted {} | partial {}, misaligned {}, source "
      "from registers {}, made at another pitch {} | fetch lookups {}, found {} ({} by image, {} "
      "by image base) | "
      "locks {} ({} of an active product, {} written back: {} levels, {:.2f} MiB, {:.2f} ms "
      "total, {:.2f} ms max, {} failed)",
      double(stats_.resolves) / f, stats_.copies, double(stats_.pixels) / f / 1e6,
      stats_.clear_only, stats_.color_clears, stats_.depth_clears,
      modes.empty() ? "none" : modes, failures.empty() ? "none" : failures, products_.size(),
      active, stats_.created, stats_.activated, stats_.deactivated_by_overlap,
      stats_.deactivated_by_cpu, stats_.evicted, stats_.partial, stats_.misaligned,
      stats_.from_registers, stats_.other_pitch, stats_.finds, stats_.found,
      stats_.found_by_image, stats_.found_by_image_base, stats_.locks, stats_.locks_on_products,
      stats_.locks_written_back,
      stats_.levels_written_back, double(stats_.write_back_bytes) / 1048576.0,
      double(stats_.write_back_ns) / 1e6, double(stats_.write_back_max_ns) / 1e6,
      stats_.write_back_failures);
  stats_ = Stats{};
  return text;
}

}  // namespace mcla::gpu
