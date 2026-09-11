#ifndef REXGLUE_HAS_XEO3_TARGET
// MCLA Native Graphics Runtime — guest texture identity.
// See texture_registry.h for the reverse-engineering provenance.

#include "texture_registry.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include "d3d_structs.h"
#include "guest_resources.h"
#include "texture_format.h"
#include "texture_ownership.h"

REXCVAR_DEFINE_BOOL(mcla_native_gfx_texture_registry, false, "MCLA/NativeGfx",
                    "Record every texture the game creates: name, dimensions, format, and "
                    "which road built it (a header placed over streamed resource memory, or a "
                    "real D3DDevice_CreateTexture). Observation only -- the guest still owns "
                    "and creates everything, the hook just reads the result. Answers what the "
                    "address-keyed texture cache cannot: which asset a given guest address is. "
                    "Overwrites mcla_native_gfx_textures.txt every 600 frame boundaries, so "
                    "the file always reflects the current state rather than the boot state.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::native_gfx {

namespace {

// grcTextureXenon instance layout, from grcTextureXenon::Init (sub_821841B0).
constexpr uint32_t kGrcTexLevelsMinusOne = 14;  // u16
constexpr uint32_t kGrcTexD3DTexture = 28;      // u32, the D3DTexture pointer
constexpr uint32_t kGrcTexTotalBytes = 20;      // u32
constexpr uint32_t kGrcTexWidth = 32;           // u16
constexpr uint32_t kGrcTexHeight = 34;          // u16
constexpr uint32_t kGrcTexName = 24;            // u32, char* (fixed up by the resource ctor)

struct Registry {
  std::mutex mutex;
  std::unordered_map<uint32_t, TextureIdentity> by_d3d_texture;
  std::unordered_map<uint32_t, uint32_t> base_to_d3d;  // fetch base -> D3DTexture
  uint64_t frames = 0;
  uint64_t created = 0;  // cumulative, survives destruction
  uint64_t destroyed = 0;
  size_t peak_live = 0;
};

// Overwrite the census every this many frame boundaries. Dumping once at the
// first boundary is worthless: at that point the streaming system has not run
// and the file only shows the handful of render targets that exist at boot.
constexpr uint64_t kDumpEveryFrames = 600;

Registry& registry() {
  static Registry r;
  return r;
}

inline uint32_t LoadBe32At(const uint8_t* base, uint32_t ea) {
  uint32_t v;
  std::memcpy(&v, rex::memory::GuestPtr(const_cast<uint8_t*>(base), ea), 4);
  return __builtin_bswap32(v);
}

inline uint16_t LoadBe16At(const uint8_t* base, uint32_t ea) {
  uint16_t v;
  std::memcpy(&v, rex::memory::GuestPtr(const_cast<uint8_t*>(base), ea), 2);
  return uint16_t((v >> 8) | (v << 8));
}

// The name is a guest char*. Bounded and readability-checked: a texture built
// from a malformed resource can carry anything here, and this runs on the
// game's own thread.
std::string ReadGuestString(const uint8_t* base, uint32_t ea, uint32_t max_len = 96) {
  if (!base || ea < 0x1000u) {
    return {};
  }
  std::string out;
  out.reserve(32);
  for (uint32_t i = 0; i < max_len; ++i) {
    if (!IsGuestRangeReadable(ea + i, 1)) {
      break;
    }
    const char c = *reinterpret_cast<const char*>(
        rex::memory::GuestPtr(const_cast<uint8_t*>(base), ea + i));
    if (c == '\0') {
      break;
    }
    out.push_back(c);
  }
  return out;
}

// The shared recorder. `forced_origin` is kUnknown for the Init road, where
// the Common flags say which of its two branches ran, and kResource for the
// deserialized road, where those flags describe the resource that baked the
// header rather than the road that produced the object.
void RecordTexture(const uint8_t* base, uint32_t grc_texture_va, uint32_t name_va,
                   TextureOrigin forced_origin) {
  if (!REXCVAR_GET(mcla_native_gfx_texture_registry)) {
    return;
  }
  if (!base || grc_texture_va < 0x1000u ||
      !IsGuestRangeReadable(grc_texture_va, kGrcTexHeight + 2)) {
    return;
  }

  TextureIdentity id;
  id.grc_texture_va = grc_texture_va;
  id.d3d_texture_va = LoadBe32At(base, grc_texture_va + kGrcTexD3DTexture);
  if (id.d3d_texture_va < 0x1000u ||
      !IsGuestRangeReadable(id.d3d_texture_va, sizeof(D3DTexture))) {
    return;  // Init bailed out, or the object is not what we think it is.
  }

  id.width = LoadBe16At(base, grc_texture_va + kGrcTexWidth);
  id.height = LoadBe16At(base, grc_texture_va + kGrcTexHeight);
  id.levels = uint32_t(LoadBe16At(base, grc_texture_va + kGrcTexLevelsMinusOne)) + 1;
  id.total_bytes = LoadBe32At(base, grc_texture_va + kGrcTexTotalBytes);
  id.name = ReadGuestString(base, name_va);

  // The fetch constant sits at D3DTexture + 0x1C; dword[1] carries format and
  // base page, exactly as DecodeTextureFetch reads them.
  const uint32_t fetch_dword1 =
      LoadBe32At(base, id.d3d_texture_va + offsetof(D3DTexture, Format) + 4);
  id.format = fetch_dword1 & 0x3Fu;
  id.base_address = ((fetch_dword1 >> 12) & 0xFFFFFu) << 12;

  if (forced_origin != TextureOrigin::kUnknown) {
    id.origin = forced_origin;
  } else {
    // Which of Init's two branches ran. The placed one sets 0x200000 on the
    // header it just built; the allocated one goes through
    // D3DDevice_CreateTexture, whose object is refcounted (0x100000) -- and
    // that is the flag grcTextureFactoryXenon::DestroyTexture branches on.
    const uint32_t common = LoadBe32At(base, id.d3d_texture_va);
    if (common & kCommonRefcounted) {
      id.origin = TextureOrigin::kAllocated;
    } else if (common & kCommonPlaced) {
      id.origin = TextureOrigin::kPlaced;
    }
  }

  Registry& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  if (id.base_address) {
    r.base_to_d3d[id.base_address] = id.d3d_texture_va;
  }
  ++r.created;
  r.by_d3d_texture[id.d3d_texture_va] = std::move(id);
  if (r.by_d3d_texture.size() > r.peak_live) {
    r.peak_live = r.by_d3d_texture.size();
  }
}

}  // namespace

void NoteTextureCreated(const uint8_t* base, uint32_t grc_texture_va, uint32_t name_va) {
  RecordTexture(base, grc_texture_va, name_va, TextureOrigin::kUnknown);
}

void NoteTextureFromResource(const uint8_t* base, uint32_t grc_texture_va) {
  if (!REXCVAR_GET(mcla_native_gfx_texture_registry)) {
    return;
  }
  // The resource constructor fixes up grc+24 only when it is non-null, so a
  // nameless texture is normal here and just records an empty name.
  uint32_t name_va = 0;
  if (base && grc_texture_va >= 0x1000u &&
      IsGuestRangeReadable(grc_texture_va + kGrcTexName, 4)) {
    name_va = LoadBe32At(base, grc_texture_va + kGrcTexName);
  }
  RecordTexture(base, grc_texture_va, name_va, TextureOrigin::kResource);
}

void NoteTextureDestroyed(const uint8_t* base, uint32_t grc_texture_va) {
  if (!REXCVAR_GET(mcla_native_gfx_texture_registry)) {
    return;
  }
  if (!base || grc_texture_va < 0x1000u ||
      !IsGuestRangeReadable(grc_texture_va + kGrcTexD3DTexture, 4)) {
    return;
  }
  const uint32_t d3d_va = LoadBe32At(base, grc_texture_va + kGrcTexD3DTexture);
  if (!d3d_va) {
    return;
  }
  Registry& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  const auto it = r.by_d3d_texture.find(d3d_va);
  if (it == r.by_d3d_texture.end()) {
    return;
  }
  // Only drop the address mapping if it still points at this object: the
  // guest reuses addresses, and a newer texture may already own it.
  const auto base_it = r.base_to_d3d.find(it->second.base_address);
  if (base_it != r.base_to_d3d.end() && base_it->second == d3d_va) {
    r.base_to_d3d.erase(base_it);
  }
  ++r.destroyed;
  r.by_d3d_texture.erase(it);
}

const TextureIdentity* LookupByBaseAddress(uint32_t base_address) {
  if (!base_address) {
    return nullptr;
  }
  Registry& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  const auto base_it = r.base_to_d3d.find(base_address);
  if (base_it == r.base_to_d3d.end()) {
    return nullptr;
  }
  const auto it = r.by_d3d_texture.find(base_it->second);
  return it == r.by_d3d_texture.end() ? nullptr : &it->second;
}

const TextureIdentity* LookupByD3DTexture(uint32_t d3d_texture_va) {
  Registry& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  const auto it = r.by_d3d_texture.find(d3d_texture_va);
  return it == r.by_d3d_texture.end() ? nullptr : &it->second;
}

uint64_t TextureRegistryLiveCount() {
  Registry& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  return uint64_t(r.by_d3d_texture.size());
}

void DumpTextureRegistry() {
  if (!REXCVAR_GET(mcla_native_gfx_texture_registry)) {
    return;
  }
  Registry& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  if ((r.frames++ % kDumpEveryFrames) != 0 || r.by_d3d_texture.empty()) {
    return;
  }

  FILE* f = std::fopen("mcla_native_gfx_textures.txt", "w");
  if (!f) {
    return;
  }
  std::fprintf(f, "live textures: %zu\n", r.by_d3d_texture.size());
  // The '*' column marks a header whose storage the runtime owns
  // (mcla_native_gfx_own_textures = 2).
  std::fprintf(f, "%-8s %s %-10s %6s %6s %4s %5s %-9s %s\n", "d3dtex", "o", "base", "w", "h",
               "lv", "fmt", "origin", "name");
  uint32_t placed = 0, allocated = 0, resource = 0, unknown = 0;
  for (const auto& [va, id] : r.by_d3d_texture) {
    const char* origin = "unknown";
    switch (id.origin) {
      case TextureOrigin::kPlaced: origin = "placed"; ++placed; break;
      case TextureOrigin::kAllocated: origin = "allocated"; ++allocated; break;
      case TextureOrigin::kResource: origin = "resource"; ++resource; break;
      default: ++unknown; break;
    }
    std::fprintf(f, "%08X %c %08X   %6u %6u %4u %5u %-9s %s\n", va,
                 IsOwnedTexture(va) ? '*' : ' ', id.base_address, id.width, id.height,
                 id.levels, id.format, origin, id.name.c_str());
  }
  std::fprintf(f, "\nplaced=%u allocated=%u resource=%u unknown=%u\n", placed, allocated,
               resource, unknown);
  std::fprintf(f, "%s\n", TextureOwnershipSummary().c_str());
  std::fclose(f);
  REXLOG_INFO(
      "[native_gfx] texture registry @frame {}: live={} peak={} created={} destroyed={} "
      "(placed={} allocated={} resource={} unknown={})",
      r.frames, r.by_d3d_texture.size(), r.peak_live, r.created, r.destroyed, placed, allocated,
      resource, unknown);
  REXLOG_INFO("[native_gfx] texture {}", TextureOwnershipSummary());
}

}  // namespace mcla::native_gfx

#endif  // REXGLUE_HAS_XEO3_TARGET
