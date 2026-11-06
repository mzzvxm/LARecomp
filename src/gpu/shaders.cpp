// MCLA GPU renderer -- the guest shader store. See shaders.h.

#include "shaders.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <system_error>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/memory/mapped_memory.h>

#include "backend.h"
#include "device.h"
#include "native_gfx/shader_identity.h"
#include "shaderpack/pack_format.h"

REXCVAR_DEFINE_STRING(
    shader_pack_dir, "", "MCLA/GPU",
    "The folder the shader packs (mcla_shaders.v8.pack and mcla_shaders_hdr.v1.pack) are read from "
    "and built into. Empty: the cache/shaders folder next to the program or, where that folder "
    "cannot be written to, larecomp/cache/shaders in the local application data folder; a pack "
    "left in the program's assets folder by an older install is still used when there is none "
    "in that folder. When this is set, only this folder is used.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

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
  const std::string dir = REXCVAR_GET(shader_pack_dir);
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
  const std::filesystem::path beside = rex::filesystem::GetExecutableFolder() / "cache" / "shaders";
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
    folders.push_back(rex::filesystem::GetExecutableFolder() / "cache" / "shaders");
    if (const std::filesystem::path local = LocalPackFolder(); !local.empty()) {
      folders.push_back(local);
    }
  }
  // Where the pack was before it was built on the player's PC (and where a test setup still
  // puts it): last, so a pack built for this game wins over a stale copy left here.
  const std::filesystem::path legacy = rex::filesystem::GetExecutableFolder() / "assets";
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
    looked += (looked.empty() ? "" : ", ") + rex::path_to_utf8(folder);
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
                rex::path_to_utf8(path_), rex::path_to_utf8(folder));
    return true;
  }
  path_ = folder / kPackFileName;
  std::error_code ec;
  if (!std::filesystem::exists(path_, ec)) {
    REXLOG_DEBUG("[gpu] shader pack: none at {}", rex::path_to_utf8(path_));
    path_.clear();
    return false;
  }
  file_ = rex::memory::MappedMemory::Open(path_, rex::memory::MappedMemory::Mode::kRead);
  if (!file_ || !file_->data()) {
    REXLOG_ERROR("[gpu] shader pack: cannot open {}", rex::path_to_utf8(path_));
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
              entries_.size(), rex::path_to_utf8(path_), file_->size(),
              XXH3_64bits(file_->data(), file_->size()), Dxil() ? "DXIL" : "SPIR-V");
  if (const uint64_t pending = pending_lookups_.exchange(0)) {
    REXLOG_WARN("[gpu] shader pack: {} shader lookups were answered before it was attached "
                "(each skipped a draw)",
                pending);
  }
  attached_.store(true, std::memory_order_release);
  return true;
}

bool ShaderStore::Validate() {
  const uint8_t* data = file_->data();
  const uint64_t size = file_->size();
  const auto fail = [&](const std::string& why) {
    REXLOG_ERROR("[gpu] shader pack {}: {}", rex::path_to_utf8(path_), why);
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
  const uint8_t* p = file_->data() + entry.velem_offset;
  out.reserve(entry.velem_count);
  for (uint32_t i = 0; i < entry.velem_count; ++i) {
    // address:12 | usage:4 | usage_index:4, as the legacy pack reader decodes it.
    const uint32_t v = ReadU32(p + 4 * i);
    out.push_back(VertexElement{v & 0xFFFu, (v >> 12) & 0xFu, (v >> 16) & 0xFu});
  }
}

bool ShaderStore::VerifyBlob(const Entry& entry, const Blob& blob, bool spirv) {
  const uint8_t* bytes = file_->data() + blob.offset;
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
  if (variant > 1 || (variant == 1 && entry.spec_mask == 0)) {
    out = ShaderResult::kNoVariant;
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  Slot& slot = slots_[variant][entry.key];
  if (!slot.done) {
    slot.done = true;
    const Blob& blob = Dxil() ? entry.dxil[variant] : entry.spirv[variant];
    if (!VerifyBlob(entry, blob, !Dxil())) {
      slot.result = ShaderResult::kBadBlob;
    } else {
      char name[48];
      std::snprintf(name, sizeof(name), "guest %s %016llX/%u",
                    entry.stage == Stage::kVertex ? "vs" : "ps",
                    static_cast<unsigned long long>(entry.key), variant);
      slot.shader = device_->CreateShader(file_->data() + blob.offset, blob.size, "main", name);
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

}  // namespace mcla::gpu
