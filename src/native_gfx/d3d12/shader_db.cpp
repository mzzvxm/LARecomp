#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — shader database.
// Pack format documented in tools/build_shader_pack.py; keep in sync.

#include "shader_db.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/logging.h>

#include "../shader_identity.h"

namespace mcla::native_gfx {

namespace {

#pragma pack(push, 1)
struct PackHeader {
  char magic[4];  // 'MSPK'
  uint32_t version;
  uint32_t entry_count;
  uint32_t reserved;
};
struct PackEntry {
  uint64_t key;
  uint8_t stage;  // 'v' / 'p'
  uint8_t spec_mask;
  uint16_t velem_count;
  uint32_t off0, size0;
  uint32_t off1, size1;
  uint64_t fnv0, fnv1;
  uint32_t velem_off;
};
#pragma pack(pop)
static_assert(sizeof(PackHeader) == 16, "pack header layout");
static_assert(sizeof(PackEntry) == 48, "pack entry layout");

}  // namespace

struct ShaderDatabase::Impl {
  std::vector<uint8_t> file;
  std::unordered_map<uint64_t, const PackEntry*> entries;
  using VertexElementRef = ShaderDatabase::VertexElementRef;
  std::unordered_set<uint64_t> reported_misses;
  std::unordered_map<uint64_t, std::vector<VertexElementRef>> velems;
};

bool ShaderDatabase::Load() {
  if (loaded_) {
    return true;
  }
  impl_ = new Impl();

  const char* candidates[] = {"assets/mcla_shaders.pack", "mcla_shaders.pack"};
  std::filesystem::path path;
  for (const char* c : candidates) {
    std::error_code ec;
    if (std::filesystem::exists(c, ec)) {
      path = c;
      break;
    }
  }
  if (path.empty()) {
    REXLOG_ERROR("[native_gfx] shader pack not found (assets/mcla_shaders.pack); "
                 "run tools/build_shader_pack.py");
    return false;
  }

  std::ifstream f(path, std::ios::binary | std::ios::ate);
  const std::streamsize size = f.tellg();
  f.seekg(0);
  impl_->file.resize(size_t(size));
  if (!f.read(reinterpret_cast<char*>(impl_->file.data()), size)) {
    REXLOG_ERROR("[native_gfx] shader pack read failed: {}", path.string());
    return false;
  }

  if (size_t(size) < sizeof(PackHeader)) {
    REXLOG_ERROR("[native_gfx] shader pack truncated header");
    return false;
  }
  const auto* hdr = reinterpret_cast<const PackHeader*>(impl_->file.data());
  if (std::memcmp(hdr->magic, "MSPK", 4) != 0 || hdr->version != 2) {
    REXLOG_ERROR("[native_gfx] shader pack bad magic/version");
    return false;
  }
  const size_t need = sizeof(PackHeader) + size_t(hdr->entry_count) * sizeof(PackEntry);
  if (size_t(size) < need) {
    REXLOG_ERROR("[native_gfx] shader pack truncated entries");
    return false;
  }

  const auto* entries = reinterpret_cast<const PackEntry*>(impl_->file.data() + sizeof(PackHeader));
  uint32_t bad = 0;
  for (uint32_t i = 0; i < hdr->entry_count; ++i) {
    const PackEntry& e = entries[i];
    // Integrity: bounds, DXBC magic, builder-recorded FNV.
    const auto check = [&](uint32_t off, uint32_t sz, uint64_t fnv) -> bool {
      if (sz == 0) {
        return true;  // variant not present
      }
      if (uint64_t(off) + sz > uint64_t(size)) {
        return false;
      }
      const uint8_t* p = impl_->file.data() + off;
      if (std::memcmp(p, "DXBC", 4) != 0) {
        return false;
      }
      return Fnv1a64(p, sz) == fnv;
    };
    if (!check(e.off0, e.size0, e.fnv0) || !check(e.off1, e.size1, e.fnv1) || e.size0 == 0) {
      ++bad;
      continue;
    }
    impl_->entries.emplace(e.key, &e);
  }
  if (bad) {
    REXLOG_ERROR("[native_gfx] shader pack: {} corrupt entries rejected", bad);
  }
  entry_count_ = uint32_t(impl_->entries.size());
  loaded_ = entry_count_ != 0;
  REXLOG_INFO("[native_gfx] shader pack loaded: {} entries, {:.1f} MB{}", entry_count_,
              double(size) / 1e6, bad ? " (WITH REJECTS)" : "");
  return loaded_;
}

const ShaderDatabase::VertexElementRef* ShaderDatabase::GetVertexElements(uint64_t identity,
                                                                          uint32_t* out_count) {
  if (out_count) {
    *out_count = 0;
  }
  if (!loaded_) {
    return nullptr;
  }
  auto it = impl_->entries.find(identity);
  if (it == impl_->entries.end() || it->second->velem_count == 0) {
    return nullptr;
  }
  const PackEntry& e = *it->second;
  // Decode the packed VertexElement bitfields once, on first request.
  auto cached = impl_->velems.find(identity);
  if (cached == impl_->velems.end()) {
    std::vector<VertexElementRef> refs;
    refs.reserve(e.velem_count);
    const auto* raw = reinterpret_cast<const uint32_t*>(impl_->file.data() + e.velem_off);
    for (uint32_t i = 0; i < e.velem_count; ++i) {
      const uint32_t v = raw[i];
      refs.push_back(VertexElementRef{v & 0xFFFu, (v >> 12) & 0xFu, (v >> 16) & 0xFu});
    }
    cached = impl_->velems.emplace(identity, std::move(refs)).first;
  }
  if (out_count) {
    *out_count = uint32_t(cached->second.size());
  }
  return cached->second.data();
}

ShaderBytecode ShaderDatabase::Lookup(uint64_t identity, uint32_t spec_mask, bool is_pixel) {
  ShaderBytecode out;
  if (!loaded_) {
    return out;
  }
  auto it = impl_->entries.find(identity);
  if (it == impl_->entries.end()) {
    ++miss_count_;
    if (impl_->reported_misses.insert(identity).second) {
      REXLOG_ERROR("[native_gfx] shader MISS: key {:016X} ({}) — not in pack", identity,
                   is_pixel ? "ps" : "vs");
    }
    return out;
  }
  const PackEntry& e = *it->second;
  const bool entry_is_pixel = e.stage == 'p';
  if (entry_is_pixel != is_pixel) {
    if (impl_->reported_misses.insert(identity ^ 1).second) {
      REXLOG_ERROR("[native_gfx] shader stage mismatch for key {:016X}: pack says {}, draw wants {}",
                   identity, entry_is_pixel ? "ps" : "vs", is_pixel ? "ps" : "vs");
    }
    return out;
  }

  uint32_t off = e.off0, size = e.size0;
  if (spec_mask != 0) {
    if ((spec_mask & ~uint32_t(e.spec_mask)) == 0 && e.size1 != 0) {
      off = e.off1;
      size = e.size1;
    } else if (impl_->reported_misses.insert(identity ^ 2).second) {
      REXLOG_WARN("[native_gfx] spec mask {:#x} unavailable for key {:016X} (has {:#x}); "
                  "using variant 0",
                  spec_mask, identity, e.spec_mask);
    }
  }
  out.data = impl_->file.data() + off;
  out.size = size;
  return out;
}

}  // namespace mcla::native_gfx

#endif // REXGLUE_HAS_XEO3_TARGET
