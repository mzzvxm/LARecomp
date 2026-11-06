// MCLA GPU renderer -- the guest shader store. See shaders.h.

#include "shaders.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <rex/filesystem.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/memory/mapped_memory.h>

#include "backend.h"
#include "device.h"
#include "native_gfx/shader_identity.h"

namespace mcla::gpu {

namespace {

constexpr uint32_t kHeaderBytes = 32;
constexpr uint32_t kEntryBytes = 88;
constexpr uint32_t kFormatDxil = 1u << 0;
constexpr uint32_t kFormatSpirv = 1u << 1;
constexpr uint32_t kSpirvMagic = 0x07230203u;

uint32_t ReadU32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;  // the pack is little-endian, as the host
}
uint64_t ReadU64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return v;
}

ShaderStore::Blob ReadBlob(const uint8_t* p) {
  return ShaderStore::Blob{ReadU32(p), ReadU32(p + 4), ReadU64(p + 8)};
}

// The pack carries both formats; the active backend's is the one used.
bool Dxil() {
  return HostShaderFormat() == plume::RenderShaderFormat::DXIL;
}

}  // namespace

const char* ShaderStore::ResultName(ShaderResult result) {
  switch (result) {
    case ShaderResult::kOk: return "ok";
    case ShaderResult::kNoVariant: return "variant not compiled";
    case ShaderResult::kBadBlob: return "bad blob";
    case ShaderResult::kCreateFailed: return "creation failed";
  }
  return "?";
}

ShaderStore::ShaderStore() = default;

ShaderStore::~ShaderStore() {
  Shutdown();
}

bool ShaderStore::Initialize(Device& device) {
  device_ = &device;
  path_ = rex::filesystem::GetExecutableFolder() / "assets" / kPackFileName;
  file_ = rex::memory::MappedMemory::Open(path_, rex::memory::MappedMemory::Mode::kRead);
  if (!file_ || !file_->data()) {
    REXLOG_ERROR("[gpu] shader pack: cannot open {} (build it with "
                 "tools/build_shader_pack_v6.py; the build copies src/assets next to the exe)",
                 rex::path_to_utf8(path_));
    file_.reset();
    return false;
  }
  if (!Validate()) {
    entries_.clear();
    file_.reset();
    return false;
  }
  // The exact size and hash name the build of the pack a log was made with.
  REXLOG_INFO("[gpu] shader pack: {} shaders from {} ({} bytes, xxh3 {:016x}, {})",
              entries_.size(), rex::path_to_utf8(path_), file_->size(),
              XXH3_64bits(file_->data(), file_->size()), Dxil() ? "DXIL" : "SPIR-V");
  return true;
}

bool ShaderStore::Validate() {
  const uint8_t* data = file_->data();
  const uint64_t size = file_->size();
  const auto fail = [&](const std::string& why) {
    REXLOG_ERROR("[gpu] shader pack {}: {}", rex::path_to_utf8(path_), why);
    return false;
  };
  if (size < kHeaderBytes || std::memcmp(data, "MSPK", 4) != 0) {
    return fail("not a shader pack");
  }
  const uint32_t version = ReadU32(data + 4);
  const uint32_t count = ReadU32(data + 8);
  const uint32_t formats = ReadU32(data + 12);
  if (version != kPackVersion) {
    return fail("version " + std::to_string(version) + ", expected " + std::to_string(kPackVersion));
  }
  if ((formats & (Dxil() ? kFormatDxil : kFormatSpirv)) == 0) {
    return fail(std::string("carries no ") + (Dxil() ? "DXIL" : "SPIR-V"));
  }
  if (uint64_t(kHeaderBytes) + uint64_t(count) * kEntryBytes > size) {
    return fail("entry table past the end of the file");
  }
  const auto in_file = [&](uint64_t offset, uint64_t bytes) {
    return offset + bytes <= size;
  };
  entries_.reserve(count);
  uint64_t previous_key = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* e = data + kHeaderBytes + uint64_t(i) * kEntryBytes;
    Entry entry;
    entry.key = ReadU64(e);
    const uint8_t stage = e[8];
    entry.spec_mask = e[9];
    entry.velem_count = uint16_t(e[10] | e[11] << 8);
    entry.velem_offset = ReadU32(e + 12);
    entry.flags = ReadU32(e + 16);
    entry.ps_outputs = static_cast<uint8_t>((entry.flags >> 8) & 0x1Fu);
    entry.texture_fetch_mask = ReadU32(e + 20);
    entry.dxil[0] = ReadBlob(e + 24);
    entry.dxil[1] = ReadBlob(e + 40);
    entry.spirv[0] = ReadBlob(e + 56);
    entry.spirv[1] = ReadBlob(e + 72);
    if (stage != 'v' && stage != 'p') {
      return fail("entry " + std::to_string(i) + " has stage " + std::to_string(stage));
    }
    entry.stage = stage == 'v' ? Stage::kVertex : Stage::kPixel;
    if (i != 0 && entry.key <= previous_key) {
      return fail("keys are not strictly increasing at entry " + std::to_string(i));
    }
    previous_key = entry.key;
    if (entry.velem_count != 0 &&
        !in_file(entry.velem_offset, uint64_t(entry.velem_count) * 4)) {
      return fail("vertex elements of entry " + std::to_string(i) + " past the end");
    }
    for (const Blob* blob : {&entry.dxil[0], &entry.dxil[1], &entry.spirv[0], &entry.spirv[1]}) {
      if (blob->size != 0 && !in_file(blob->offset, blob->size)) {
        return fail("a blob of entry " + std::to_string(i) + " is past the end");
      }
    }
    // What this backend needs must be present: variant 0 always, variant 1
    // whenever the shader has spec bits.
    const Blob* blobs = Dxil() ? entry.dxil : entry.spirv;
    if (blobs[0].size == 0 || (entry.spec_mask != 0 && blobs[1].size == 0)) {
      return fail("entry " + std::to_string(i) + " lacks a blob this backend needs");
    }
    entries_.emplace(entry.key, entry);
  }
  return true;
}

void ShaderStore::Shutdown() {
  for (auto& slots : slots_) {
    slots.clear();
  }
  hdr_variants_.clear();
  hdr_file_.reset();
  entries_.clear();
  file_.reset();
  device_ = nullptr;
}

const ShaderStore::Entry* ShaderStore::Find(uint64_t identity) const {
  const auto it = entries_.find(identity);
  return it == entries_.end() ? nullptr : &it->second;
}

void ShaderStore::GetVertexElements(const Entry& entry, std::vector<VertexElement>& out) const {
  out.clear();
  if (!file_ || entry.velem_count == 0) {
    return;
  }
  const uint8_t* p = file_->data() + entry.velem_offset;
  out.reserve(entry.velem_count);
  for (uint32_t i = 0; i < entry.velem_count; ++i) {
    // address:12 | usage:4 | usage_index:4, as the legacy pack reader decodes it.
    const uint32_t v = ReadU32(p + 4 * i);
    out.push_back(VertexElement{v & 0xFFFu, (v >> 12) & 0xFu, (v >> 16) & 0xFu});
  }
}

bool ShaderStore::VerifyBlob(const Entry& entry, const Blob& blob, bool spirv,
                             const uint8_t* base) {
  const uint8_t* bytes = base + blob.offset;
  const bool magic = spirv ? blob.size >= 20 && ReadU32(bytes) == kSpirvMagic
                           : blob.size >= 4 && std::memcmp(bytes, "DXBC", 4) == 0;
  const bool fnv = mcla::native_gfx::Fnv1a64(bytes, blob.size) == blob.fnv;
  ++stats_.blobs_verified;
  if (!magic || !fnv) {
    ++stats_.blob_failures;
    REXLOG_ERROR("[gpu] shader pack: blob of {:016X} at {} ({} bytes) fails its {} check; the "
                 "shader will not be used",
                 entry.key, blob.offset, blob.size, !magic ? "magic" : "FNV");
    return false;
  }
  return true;
}

plume::RenderShader* ShaderStore::GetShader(const Entry& entry, uint32_t variant,
                                            ShaderResult* result) {
  ShaderResult local;
  ShaderResult& out = result ? *result : local;
  const bool hdr = variant == kHdrVariant;
  if (variant > kHdrVariant || (variant == 1 && entry.spec_mask == 0) ||
      (hdr && !entry.hdr_variant)) {
    out = ShaderResult::kNoVariant;
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  Slot& slot = slots_[variant][entry.key];
  if (!slot.done) {
    slot.done = true;
    const HdrVariant* hdr_blobs = nullptr;
    if (hdr) {
      const auto it = hdr_variants_.find(entry.key);
      hdr_blobs = it == hdr_variants_.end() ? nullptr : &it->second;
    }
    const Blob& blob = hdr ? (Dxil() ? hdr_blobs->dxil : hdr_blobs->spirv)
                           : (Dxil() ? entry.dxil[variant] : entry.spirv[variant]);
    const uint8_t* base = hdr ? hdr_file_->data() : file_->data();
    if (!VerifyBlob(entry, blob, !Dxil(), base)) {
      slot.result = ShaderResult::kBadBlob;
    } else {
      char name[48];
      std::snprintf(name, sizeof(name), "guest %s %016llX/%u",
                    entry.stage == Stage::kVertex ? "vs" : "ps",
                    static_cast<unsigned long long>(entry.key), variant);
      slot.shader = device_->CreateShader(base + blob.offset, blob.size, "main", name);
      if (slot.shader) {
        ++stats_.shaders_created;
        slot.result = ShaderResult::kOk;
      } else {
        ++stats_.create_failures;
        slot.result = ShaderResult::kCreateFailed;
      }
    }
  }
  out = slot.result;
  return slot.shader.get();
}

// ---------------------------------------------------------------------------
// the HDR scene variants
// ---------------------------------------------------------------------------
// Layout of mcla_shaders_hdr.v1.pack (little-endian), written by tools/build_hdr_variants.py:
//   header : magic 'MSHV' | version u32 | entry_count u32 | formats u32 (bit 0 DXIL, bit 1 SPIR-V) |
//            contract u32 | main pack version u32 | 2 reserved u32                       (32 bytes)
//   entries: ps key u64 | vs keys 4 x u64 (0 = none) | flags u32 | reserved u32 |
//            dxil {off u32, size u32, fnv u64} | spirv {...}                             (80 bytes)
//   blobs  : per entry, DXIL then SPIR-V

bool ShaderStore::LoadHdrVariants(std::string* why) {
  constexpr uint32_t kHdrHeaderBytes = 32;
  constexpr uint32_t kHdrEntryBytes = 80;
  hdr_path_ = rex::filesystem::GetExecutableFolder() / "assets" / kHdrPackFileName;
  const auto fail = [&](const std::string& text) {
    hdr_variants_.clear();
    for (auto& [key, entry] : entries_) {
      entry.hdr_variant = false;
    }
    hdr_file_.reset();
    if (why) {
      *why = text;
    }
    return false;
  };
  if (entries_.empty()) {
    return fail("the shader pack is not open");
  }
  hdr_file_ = rex::memory::MappedMemory::Open(hdr_path_, rex::memory::MappedMemory::Mode::kRead);
  if (!hdr_file_ || !hdr_file_->data()) {
    return fail("cannot open " + rex::path_to_utf8(hdr_path_) +
                " (build it with tools/build_hdr_variants.py)");
  }
  const uint8_t* data = hdr_file_->data();
  const uint64_t size = hdr_file_->size();
  if (size < kHdrHeaderBytes || std::memcmp(data, "MSHV", 4) != 0) {
    return fail("not an HDR variant pack");
  }
  const uint32_t version = ReadU32(data + 4);
  const uint32_t count = ReadU32(data + 8);
  const uint32_t formats = ReadU32(data + 12);
  const uint32_t contract = ReadU32(data + 16);
  const uint32_t main_version = ReadU32(data + 20);
  if (version != kHdrPackVersion || contract != kHdrContract || main_version != kPackVersion) {
    return fail("version " + std::to_string(version) + ", constants contract " +
                std::to_string(contract) + ", built for shader pack " +
                std::to_string(main_version) + "; this build reads version " +
                std::to_string(kHdrPackVersion) + ", contract " + std::to_string(kHdrContract) +
                ", pack " + std::to_string(kPackVersion));
  }
  if ((formats & (Dxil() ? kFormatDxil : kFormatSpirv)) == 0) {
    return fail(std::string("carries no ") + (Dxil() ? "DXIL" : "SPIR-V"));
  }
  if (uint64_t(kHdrHeaderBytes) + uint64_t(count) * kHdrEntryBytes > size) {
    return fail("entry table past the end of the file");
  }
  const auto in_file = [&](const Blob& blob) {
    return uint64_t(blob.offset) + blob.size <= size;
  };
  std::unordered_map<uint64_t, HdrVariant> variants;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* e = data + kHdrHeaderBytes + uint64_t(i) * kHdrEntryBytes;
    HdrVariant v;
    v.ps_key = ReadU64(e);
    for (uint32_t k = 0; k < kHdrVertexShaders; ++k) {
      v.vs_keys[k] = ReadU64(e + 8 + 8 * k);
    }
    v.flags = ReadU32(e + 40);
    v.dxil = ReadBlob(e + 48);
    v.spirv = ReadBlob(e + 64);
    const Blob& needed = Dxil() ? v.dxil : v.spirv;
    if (needed.size == 0 || !in_file(v.dxil) || !in_file(v.spirv)) {
      return fail("entry " + std::to_string(i) + " lacks a blob this backend needs or has one "
                  "past the end");
    }
    // The pack must be the one this HDR pack was built for: the shader is in it, a pixel shader,
    // and so are the vertex shaders it names (at least one).
    const auto main_entry = entries_.find(v.ps_key);
    bool vertex_shaders_ok = v.vs_keys[0] != 0;
    for (uint64_t vs_key : v.vs_keys) {
      vertex_shaders_ok &= vs_key == 0 || entries_.contains(vs_key);
    }
    if (main_entry == entries_.end() || main_entry->second.stage != Stage::kPixel ||
        !vertex_shaders_ok) {
      return fail("entry " + std::to_string(i) + " names a shader the main pack does not have "
                  "(another build of the pack?)");
    }
    variants.emplace(v.ps_key, v);
  }
  for (const auto& [key, v] : variants) {
    entries_.find(key)->second.hdr_variant = true;
  }
  hdr_variants_ = std::move(variants);
  REXLOG_INFO("[gpu] shader pack: {} HDR scene variants from {} ({} bytes)", hdr_variants_.size(),
              rex::path_to_utf8(hdr_path_), size);
  return true;
}

const ShaderStore::HdrVariant* ShaderStore::FindHdrVariant(uint64_t identity) const {
  const auto it = hdr_variants_.find(identity);
  return it == hdr_variants_.end() ? nullptr : &it->second;
}

}  // namespace mcla::gpu
