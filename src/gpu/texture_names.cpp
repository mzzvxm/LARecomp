// ===========================================================================
// MCLA GPU renderer -- the names of the guest's textures (texture_names.h)
// ===========================================================================

#include "texture_names.h"

#include <mutex>
#include <unordered_map>

#include <rex/logging.h>

#include "native_gfx/guest/guest_resources.h"
#include "records.h"
#include "textures.h"

namespace mcla::gpu::texture_names {

namespace {

namespace nfx = mcla::native_gfx;

constexpr uint32_t kGrcName = 24;          // char*, fixed up by the resource constructor
constexpr uint32_t kGrcD3DTexture = 28;    // D3DTexture*
constexpr uint32_t kD3DFetchConstant = 28; // D3DBaseTexture: the fetch constant, 6 dwords
constexpr uint32_t kTlsCurrentResource = 60;  // *(r13) + 60: the datResource being fixed up
constexpr uint32_t kResourceName = 8;         // datResource: char* name
constexpr uint32_t kMaxName = 160;

struct Registry {
  std::mutex mutex;
  std::unordered_map<uint32_t, Name> by_page;          // base page -> name
  std::unordered_map<uint32_t, uint32_t> page_of_grc;  // grcTexture -> base page
  uint64_t recorded = 0;
  bool first_dictionary_logged = false;
};

Registry& registry() {
  static Registry r;
  return r;
}

bool Readable(uint32_t va, uint32_t bytes) {
  return va >= 0x1000u && nfx::IsGuestRangeReadable(va, bytes);
}

uint32_t Load32(const uint8_t* base, uint32_t va) {
  return Readable(va, 4) ? LoadBe32(base + va) : 0u;
}

std::string ReadString(const uint8_t* base, uint32_t va) {
  std::string s;
  if (!Readable(va, 1)) {
    return s;
  }
  for (uint32_t i = 0; i < kMaxName && Readable(va + i, 1); ++i) {
    const char c = char(base[va + i]);
    if (c == 0) {
      break;
    }
    s.push_back(c);
  }
  return s;
}

// The physical page of the base level of the grcTexture's D3DTexture; 0 when
// it has none or cannot be read.
uint32_t BasePageOf(const uint8_t* base, uint32_t grc_texture) {
  const uint32_t d3d = Load32(base, grc_texture + kGrcD3DTexture);
  if (!Readable(d3d + kD3DFetchConstant, 24)) {
    return 0;
  }
  uint32_t dwords[6];
  for (uint32_t i = 0; i < 6; ++i) {
    dwords[i] = LoadBe32(base + d3d + kD3DFetchConstant + 4 * i);
  }
  const TextureFetch fetch = ReadTextureFetch(dwords);
  return fetch.is_texture ? fetch.key.base_page : 0u;
}

// "platform:/textures/shared.xtd" -> "shared": the file stem, whatever the
// separators.
std::string Stem(const std::string& path) {
  const size_t slash = path.find_last_of("/\\:");
  std::string s = slash == std::string::npos ? path : path.substr(slash + 1);
  const size_t dot = s.find_last_of('.');
  if (dot != std::string::npos && dot > 0) {
    s.resize(dot);
  }
  return s;
}

void Record(const uint8_t* base, uint32_t grc_texture, std::string texture,
            std::string dictionary, const std::string& raw_dictionary) {
  const uint32_t page = BasePageOf(base, grc_texture);
  if (page == 0) {
    return;
  }
  Registry& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  if (!r.first_dictionary_logged && !raw_dictionary.empty()) {
    r.first_dictionary_logged = true;
    // The format of the resource's name has not been seen running before.
    REXLOG_INFO("[gpu] texture names: first resource name '{}' (folder '{}'), texture '{}'",
                raw_dictionary, dictionary, texture);
  }
  r.by_page[page] = Name{std::move(texture), std::move(dictionary)};
  r.page_of_grc[grc_texture] = page;
  ++r.recorded;
}

}  // namespace

std::string FileSafe(const std::string& s, const char* fallback) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '_' || c == '-' || c == '.';
    out.push_back(ok ? c : '_');
  }
  while (!out.empty() && out.back() == '.') {
    out.pop_back();  // Windows drops trailing dots
  }
  return out.empty() ? std::string(fallback) : out;
}

void NoteInit(const uint8_t* base, uint32_t grc_texture, uint32_t name_va) {
  if (!base || grc_texture < 0x1000u) {
    return;
  }
  Record(base, grc_texture, FileSafe(ReadString(base, name_va), "unnamed"), "runtime", {});
}

void NoteResource(const uint8_t* base, uint32_t grc_texture, uint32_t r13) {
  if (!base || grc_texture < 0x1000u) {
    return;
  }
  const std::string texture =
      FileSafe(Stem(ReadString(base, Load32(base, grc_texture + kGrcName))), "unnamed");
  const uint32_t resource = Load32(base, Load32(base, r13) + kTlsCurrentResource);
  const std::string raw = resource ? ReadString(base, Load32(base, resource + kResourceName))
                                   : std::string();
  Record(base, grc_texture, texture, FileSafe(Stem(raw), "unknown_resource"), raw);
}

void NoteDestroyed(const uint8_t* base, uint32_t grc_texture) {
  (void)base;
  Registry& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  const auto it = r.page_of_grc.find(grc_texture);
  if (it == r.page_of_grc.end()) {
    return;
  }
  r.by_page.erase(it->second);
  r.page_of_grc.erase(it);
}

bool Lookup(uint32_t base_page, Name* out) {
  Registry& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  const auto it = r.by_page.find(base_page);
  if (it == r.by_page.end()) {
    return false;
  }
  *out = it->second;
  return true;
}

}  // namespace mcla::gpu::texture_names
