// MCLA GPU renderer -- the guest shader store. See shaders.h.

#include "shaders.h"

#include <algorithm>
#include <cstdio>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <span>
#include <string>
#include <system_error>

#include "settings/larecomp_settings.h"
#include <rex/platform/file.h>
#include "xxhash_inline.h"
#include <rex/logging/log_category.h>
#include <rex/memory/virtual_memory.h>
#include <rex/string/text.h>

#include "backend.h"
#include "device.h"
#include "native_gfx/shader_identity.h"
#include "shaderpack/pack_format.h"
#include "shaderpack/variant_chain.h"
#include "shaderpack/variant_pack_format.h"

namespace mcla::gpu {

namespace {

namespace pack = larecomp::shaderpack;

// Whether a file can be created in `dir` (made if missing): Windows' Controlled Folder Access lets
// unknown programs read a folder but not write to it, and only a real write shows it.
bool CanWriteTo(const std::filesystem::path& dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (!std::filesystem::is_directory(dir, ec)) {
    return false;
  }
  const std::filesystem::path probe = dir / ".larecomp_write_test";
  {
    std::ofstream f(probe, std::ios::binary | std::ios::trunc);
    if (!f || !(f << 'x') || !f.flush()) {
      return false;
    }
  }
  std::filesystem::remove(probe, ec);
  return true;
}

// shader_pack_dir as a path; empty when it is not set.
std::filesystem::path CvarPackFolder() {
  const std::string dir = larecomp::settings::shader_pack_dir.Get();
  return std::filesystem::path(std::u8string(dir.begin(), dir.end()));
}

// %LOCALAPPDATA%/larecomp/cache/shaders on Windows; $XDG_CACHE_HOME or ~/.cache, then
// larecomp/shaders elsewhere. Empty when the environment names no place.
std::filesystem::path LocalPackFolder() {
#if defined(_WIN32)
  if (const wchar_t* local = _wgetenv(L"LOCALAPPDATA"); local && *local) {
    return std::filesystem::path(local) / L"larecomp" / L"cache" / L"shaders";
  }
#else
  if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) {
    return std::filesystem::path(xdg) / "larecomp" / "shaders";
  }
  if (const char* home = std::getenv("HOME"); home && *home) {
    return std::filesystem::path(home) / ".cache" / "larecomp" / "shaders";
  }
#endif
  return {};
}

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
    case ShaderResult::kNoPack: return "no shader pack attached";
  }
  return "?";
}

ShaderStore::ShaderStore() = default;

ShaderStore::~ShaderStore() {
  Shutdown();
}

bool ShaderStore::Initialize(Device& device) {
  device_ = &device;
  return true;  // pending until Attach: no pack is needed to start
}

std::filesystem::path ShaderStore::DefaultPackFolder() {
  const std::filesystem::path beside = rex::platform::GetExecutableFolder() / "cache" / "shaders";
  if (CanWriteTo(beside)) {
    return beside;
  }
  const std::filesystem::path local = LocalPackFolder();
  if (!local.empty() && CanWriteTo(local)) {
    return local;
  }
  return beside;  // neither can be written: the build reports it, with this path
}

std::filesystem::path ShaderStore::BuildFolder(const std::filesystem::path& configured) {
  if (std::filesystem::path dir = CvarPackFolder(); !dir.empty()) {
    return dir;
  }
  return configured.empty() ? DefaultPackFolder() : configured;
}

std::vector<std::filesystem::path> ShaderStore::SearchFolders(
    const std::filesystem::path& configured) {
  std::vector<std::filesystem::path> folders;
  if (std::filesystem::path dir = CvarPackFolder(); !dir.empty()) {
    folders.push_back(std::move(dir));
    return folders;
  }
  if (!configured.empty()) {
    folders.push_back(configured);
  } else {
    // Where DefaultPackFolder builds them, looked at without writing anything.
    folders.push_back(rex::platform::GetExecutableFolder() / "cache" / "shaders");
    if (const std::filesystem::path local = LocalPackFolder(); !local.empty()) {
      folders.push_back(local);
    }
  }
  // Where the pack was before it was built on the player's PC (and where a test setup still
  // puts it): last, so a pack built for this game wins over a stale copy left here.
  const std::filesystem::path legacy = rex::platform::GetExecutableFolder() / "assets";
  if (std::find(folders.begin(), folders.end(), legacy) == folders.end()) {
    folders.push_back(legacy);
  }
  return folders;
}

bool ShaderStore::AttachFirst(const std::vector<std::filesystem::path>& folders) {
  for (const std::filesystem::path& folder : folders) {
    if (Attach(folder)) {
      return true;
    }
  }
  std::string looked;
  for (const std::filesystem::path& folder : folders) {
    looked += (looked.empty() ? "" : ", ") + rex::string::PathToUTF8(folder);
  }
  REXLOG_INFO("[gpu] shader pack: none found (looked in {}); pending: draws of guest content are "
              "skipped until the packs are attached",
              looked);
  return false;
}

bool ShaderStore::Attach(const std::filesystem::path& folder) {
  if (!device_) {
    REXLOG_ERROR("[gpu] shader pack: Attach before Initialize");
    return false;
  }
  if (attached()) {
    REXLOG_WARN("[gpu] shader pack: already attached from {}; {} is not read",
                rex::string::PathToUTF8(path_), rex::string::PathToUTF8(folder));
    return true;
  }
  path_ = folder / kPackFileName;
  std::error_code ec;
  if (!std::filesystem::exists(path_, ec)) {
    REXLOG_DEBUG("[gpu] shader pack: none at {}", rex::string::PathToUTF8(path_));
    path_.clear();
    return false;
  }
  file_ = rex::memory::MappedMemory::Open(path_, rex::memory::MappedMemory::Mode::kRead);
  if (!file_ || !file_->GetData()) {
    REXLOG_ERROR("[gpu] shader pack: cannot open {}", rex::string::PathToUTF8(path_));
    file_.reset();
    path_.clear();
    return false;
  }
  if (!Validate()) {
    entries_.clear();
    file_.reset();
    path_.clear();
    return false;
  }
  folder_ = folder;
  // The exact size and hash name the build of the pack a log was made with.
  REXLOG_INFO("[gpu] shader pack: {} shaders from {} ({} bytes, xxh3 {:016x}, {})",
              entries_.size(), rex::string::PathToUTF8(path_), file_->GetSize(),
              XXH3_64bits(file_->GetData(), file_->GetSize()), Dxil() ? "DXIL" : "SPIR-V");
  if (const uint64_t pending = pending_lookups_.exchange(0)) {
    REXLOG_WARN("[gpu] shader pack: {} shader lookups were answered before it was attached "
                "(each skipped a draw)",
                pending);
  }
  attached_.store(true, std::memory_order_release);
  return true;
}

bool ShaderStore::Validate() {
  const uint8_t* data = file_->GetData();
  const uint64_t size = file_->GetSize();
  const auto fail = [&](const std::string& why) {
    REXLOG_ERROR("[gpu] shader pack {}: {}", rex::string::PathToUTF8(path_), why);
    return false;
  };
  if (size < pack::kPackHeaderBytes || std::memcmp(data, pack::kPackMagic, 4) != 0) {
    return fail("not a shader pack");
  }
  const uint32_t version = ReadU32(data + pack::kHeaderVersion);
  const uint32_t count = ReadU32(data + pack::kHeaderCount);
  const uint32_t formats = ReadU32(data + pack::kHeaderFormats);
  if (version != pack::kPackVersion) {
    return fail("version " + std::to_string(version) + ", expected " +
                std::to_string(pack::kPackVersion));
  }
  if ((formats & (Dxil() ? pack::kDxil : pack::kSpirv)) == 0) {
    return fail(std::string("carries no ") + (Dxil() ? "DXIL" : "SPIR-V"));
  }
  if (uint64_t(pack::kPackHeaderBytes) + uint64_t(count) * pack::kPackEntryBytes > size) {
    return fail("entry table past the end of the file");
  }
  const auto in_file = [&](uint64_t offset, uint64_t bytes) {
    return offset + bytes <= size;
  };
  entries_.reserve(count);
  uint64_t previous_key = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* e = data + pack::kPackHeaderBytes + uint64_t(i) * pack::kPackEntryBytes;
    Entry entry;
    entry.key = ReadU64(e + pack::kEntryKey);
    const uint8_t stage = e[pack::kEntryStage];
    entry.spec_mask = e[pack::kEntrySpecMask];
    entry.velem_count = uint16_t(e[pack::kEntryVertexElementCount] |
                                 e[pack::kEntryVertexElementCount + 1] << 8);
    entry.velem_offset = ReadU32(e + pack::kEntryVertexElementOffset);
    entry.flags = ReadU32(e + pack::kEntryFlags);
    entry.ps_outputs = static_cast<uint8_t>((entry.flags >> pack::kFlagPixelOutputsShift) &
                                            pack::kFlagPixelOutputsMask);
    entry.texture_fetch_mask = ReadU32(e + pack::kEntryTextureFetchMask);
    entry.dxil[0] = ReadBlob(e + pack::kEntryDxil0);
    entry.dxil[1] = ReadBlob(e + pack::kEntryDxil1);
    entry.spirv[0] = ReadBlob(e + pack::kEntrySpirv0);
    entry.spirv[1] = ReadBlob(e + pack::kEntrySpirv1);
    if (stage != pack::kStageVertex && stage != pack::kStagePixel) {
      return fail("entry " + std::to_string(i) + " has stage " + std::to_string(stage));
    }
    entry.stage = stage == pack::kStageVertex ? Stage::kVertex : Stage::kPixel;
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
  attached_.store(false, std::memory_order_release);
  if (const uint64_t pending = pending_lookups_.exchange(0)) {
    REXLOG_WARN("[gpu] shader pack: {} shader lookups were answered with no pack attached "
                "(each skipped a draw)",
                pending);
  }
  for (auto& slots : slots_) {
    slots.clear();
  }
  packed_slots_.clear();
  variants_ready_.store(false, std::memory_order_release);
  hdr_variant_count_ = 0;
  variants_.clear();
  variants_file_.reset();
  entries_.clear();
  file_.reset();
  device_ = nullptr;
}

const ShaderStore::Entry* ShaderStore::Find(uint64_t identity) const {
  if (!attached_.load(std::memory_order_acquire)) {
    pending_lookups_.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  const auto it = entries_.find(identity);
  return it == entries_.end() ? nullptr : &it->second;
}

void ShaderStore::GetVertexElements(const Entry& entry, std::vector<VertexElement>& out) const {
  out.clear();
  if (!file_ || entry.velem_count == 0) {
    return;
  }
  const uint8_t* p = file_->GetData() + entry.velem_offset;
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
  const bool magic = spirv ? blob.size >= pack::kSpirvMinBytes &&
                                 ReadU32(bytes) == pack::kSpirvMagic
                           : blob.size >= 4 && std::memcmp(bytes, pack::kDxbcMagic, 4) == 0;
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
  if (!attached_.load(std::memory_order_acquire)) {
    pending_lookups_.fetch_add(1, std::memory_order_relaxed);
    out = ShaderResult::kNoPack;
    return nullptr;
  }
  const bool packed = variant >= kFirstPackedVariant;
  if ((!packed && variant == 1 && entry.spec_mask == 0) || (packed && !entry.HasWord(variant))) {
    out = ShaderResult::kNoVariant;
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  Slot& slot = packed ? packed_slots_[WordKey{entry.key, variant}] : slots_[variant][entry.key];
  if (!slot.done) {
    slot.done = true;
    const Variant* packed_blobs = nullptr;
    if (packed) {
      const auto it = variants_.find(WordKey{entry.key, variant});
      packed_blobs = it == variants_.end() ? nullptr : &it->second;
    }
    // A packed variant that the entry says it has is in the table (LoadVariants set both); a missing one
    // is a bug, answered as "not compiled" instead of a null read.
    if (packed && (!packed_blobs || !variants_file_)) {
      out = ShaderResult::kNoVariant;
      packed_slots_.erase(WordKey{entry.key, variant});
      return nullptr;
    }
    const Blob& blob = packed ? (Dxil() ? packed_blobs->dxil : packed_blobs->spirv)
                              : (Dxil() ? entry.dxil[variant] : entry.spirv[variant]);
    const uint8_t* base = packed ? variants_file_->GetData() : file_->GetData();
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
// the variant pack
// ---------------------------------------------------------------------------
// Layout of mcla_shaders_variants.v3.pack (little-endian; documented in shaderpack/pack_format.h, read by
// shaderpack/variant_pack_format.h ParseVariantPack, written by src/shaderpack's variant step, variant_pack.h):
//   header : magic 'MSHV' | version u32 | entry_count u32 | formats u32 (bit 0 DXIL, bit 1 SPIR-V) |
//            contract u32 | main pack version u32 | 2 reserved u32                       (32 bytes)
//   entries: key u64 | vs keys 4 x u64 (0 = none) | variant word u32 | flags u32 | vs_features u32 | reserved u32 |
//            dxil {off u32, size u32, fnv u64} | spirv {...}                             (88 bytes)
//            in increasing (vertex flag, word, key) order; key is a pixel shader's, or a vertex shader's (flags bit 0)
//   blobs  : per entry, DXIL then SPIR-V
// A file of another version, contract or main pack is refused whole (ParseVariantPack's sentence is logged): the entries of
// version 2 were 80 bytes with one id each, so nothing of such a file can be read.

bool ShaderStore::LoadVariants(std::string* why) {
  if (variants_ready_.load(std::memory_order_acquire)) {
    return true;
  }
  // Beside the main pack: the folder it was attached from (the builder writes both there).
  // CHECK: a test setup with the main pack in cache/shaders and a hand-built variant pack only in
  // <exe>/assets finds none: put both in one folder.
  variants_path_ = folder_ / kVariantPackFileName;
  const auto fail = [&](const std::string& text) {
    variants_.clear();
    for (auto& [key, entry] : entries_) {
      entry.features = 0;
      entry.word_count = 0;
      entry.hdr_variant = false;
    }
    hdr_variant_count_ = 0;
    variants_file_.reset();
    if (why) {
      *why = text;
    }
    return false;
  };
  if (!attached() || entries_.empty()) {
    return fail("the shader pack is not attached");
  }
  variants_file_ = rex::memory::MappedMemory::Open(variants_path_, rex::memory::MappedMemory::Mode::kRead);
  if (!variants_file_ || !variants_file_->GetData()) {
    return fail("cannot open " + rex::string::PathToUTF8(variants_path_) +
                " (the shader pack build writes it beside the main pack)");
  }
  const uint64_t size = variants_file_->GetSize();
  larecomp::shaderpack::VariantPackExpect expect;
  expect.main_pack_version = kPackVersion;
  expect.formats_needed = Dxil() ? pack::kDxil : pack::kSpirv;
  larecomp::shaderpack::ParsedVariantPack parsed;
  std::string parse_why;
  if (!larecomp::shaderpack::ParseVariantPack(std::span<const uint8_t>(variants_file_->GetData(), size_t(size)), expect, &parsed,
                                              &parse_why)) {
    return fail(parse_why);
  }
  std::unordered_map<WordKey, Variant, WordKeyHash> variants;
  variants.reserve(parsed.entries.size());
  for (size_t i = 0; i < parsed.entries.size(); ++i) {
    const larecomp::shaderpack::VariantPackEntry& e = parsed.entries[i];
    Variant v;
    v.ps_key = e.key;
    v.variant = e.variant;
    for (uint32_t k = 0; k < kVariantVertexShaders; ++k) {
      v.vs_keys[k] = e.vs_keys[k];
    }
    v.vs_features = e.vs_features;
    v.vertex = e.vertex();
    v.dxil = Blob{e.dxil.offset, e.dxil.size, e.dxil.fnv};
    v.spirv = Blob{e.spirv.offset, e.spirv.size, e.spirv.fnv};
    // The pack must be the one this variant pack was built for: the shader is in it, of the stage the entry says, and so are
    // the vertex shaders it names.
    const auto main_entry = entries_.find(v.ps_key);
    bool vertex_shaders_ok = true;
    for (uint64_t vs_key : v.vs_keys) {
      vertex_shaders_ok &= vs_key == 0 || entries_.contains(vs_key);
    }
    const Stage want = v.vertex ? Stage::kVertex : Stage::kPixel;
    if (main_entry == entries_.end() || main_entry->second.stage != want || !vertex_shaders_ok) {
      return fail("entry " + std::to_string(i) + " names a shader the main pack does not have "
                  "(another build of the pack?)");
    }
    if (!variants.emplace(WordKey{v.ps_key, v.variant}, v).second) {
      return fail("entry " + std::to_string(i) + " repeats the variant " + std::to_string(v.variant) + " of a shader");
    }
  }
  // Each shader's table: its words, its features, what each word needs of the vertex shader.
  uint32_t per_feature[kMaxFeatures] = {};
  uint32_t total = 0;
  for (const auto& [wk, v] : variants) {
    Entry& entry = entries_.find(v.ps_key)->second;
    if (entry.word_count >= kMaxWordsPerShader) {
      return fail("a shader has more than " + std::to_string(kMaxWordsPerShader) + " variants");
    }
    entry.words[entry.word_count] = v.variant;
    entry.needs[entry.word_count] = v.vs_features;
    ++entry.word_count;
    entry.features |= larecomp::shaderpack::chain::FeaturesOf(v.variant);
    ++total;
    if (larecomp::shaderpack::chain::SpecOf(v.variant) == 0) {
      const uint32_t features = larecomp::shaderpack::chain::FeaturesOf(v.variant);
      for (uint32_t f = 0; f < kMaxFeatures; ++f) {
        if (features == FeatureBit(f)) {
          ++per_feature[f];
        }
      }
    }
  }
  for (auto& [key, entry] : entries_) {
    entry.hdr_variant = entry.HasWord(kHdrVariant);
  }
  hdr_variant_count_ = per_feature[larecomp::shaderpack::kFeatureHdrScene];
  variants_ = std::move(variants);
  variants_ready_.store(true, std::memory_order_release);  // last: has_variants()
  // One line: the count of each feature alone (spec variant 0) by its name in the chain's table, then the composed and twin words.
  std::string names;
  uint32_t composed = 0;
  uint32_t twins = 0;
  for (const auto& [wk, v] : variants_) {
    composed += std::popcount(larecomp::shaderpack::chain::FeaturesOf(v.variant)) > 1 ? 1 : 0;
    twins += larecomp::shaderpack::chain::SpecOf(v.variant);
  }
  for (const larecomp::shaderpack::chain::FeatureInfo& row : larecomp::shaderpack::chain::kFeatures) {
    if (per_feature[row.feature] != 0) {
      names += (names.empty() ? "" : ", ") + std::string(row.name) + " " + std::to_string(per_feature[row.feature]);
    }
  }
  REXLOG_INFO("[gpu] shader pack: {} shader variants from {} ({} bytes): {}; {} composed, {} with the spec variant 1 (alpha test)",
              total, rex::string::PathToUTF8(variants_path_), size, names.empty() ? "none" : names, composed, twins);
  return true;
}

const ShaderStore::Variant* ShaderStore::FindVariant(uint64_t identity, uint32_t variant) const {
  if (variant < kFirstPackedVariant) {
    return nullptr;
  }
  const auto it = variants_.find(WordKey{identity, variant});
  return it == variants_.end() ? nullptr : &it->second;
}

uint32_t ShaderStore::ChooseVariant(const Entry& ps, uint32_t wanted, uint32_t spec, const Entry* vs, uint32_t vs_spec,
                                    uint32_t* vs_word) const {
  *vs_word = larecomp::shaderpack::chain::Word(0, vs_spec);
  if (ps.word_count == 0 || (wanted & ps.features) == 0) {
    return larecomp::shaderpack::chain::Word(0, spec);
  }
  uint32_t needs_of_choice = 0;
  const uint32_t word = larecomp::shaderpack::chain::Pick(
      std::span<const uint32_t>(ps.words, ps.word_count), wanted, spec, [&](uint32_t candidate) {
        uint32_t needs = 0;
        for (uint32_t i = 0; i < ps.word_count; ++i) {
          if (ps.words[i] == candidate) {
            needs = ps.needs[i];
            break;
          }
        }
        // A word that reads a vertex stage's export is for a draw whose vertex shader has that stage (for the draw's own spec variant).
        const bool ok = needs == 0 || (vs && vs->HasWord(larecomp::shaderpack::chain::Word(needs, vs_spec)));
        if (ok) {
          needs_of_choice = needs;
        }
        return ok;
      });
  if (word >= kFirstPackedVariant && needs_of_choice != 0) {
    *vs_word = larecomp::shaderpack::chain::Word(needs_of_choice, vs_spec);
  }
  return word;
}

}  // namespace mcla::gpu
