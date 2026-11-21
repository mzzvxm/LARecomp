// The city texture set's checks (CITY-RT M2 package H2): no GPU, and no game files unless one is named (then the retail archive's golden counts are checked).
//
//   build: cmake --build <dir> --target larecomp_texture_set_test (src/citypack/CMakeLists.txt); it needs src/mc_engine/modloader/{rpf3,aes256,texture}.cpp (the archive reader and writer, and
//   the mod loader's own tiler, which writes the synthetic textures here independently of the reader being tested) and xxhash.h.
//   run: larecomp_texture_set_test [<scratch folder> [<game folder with xarchive_cache.rpf>]]   (exit 0 = passed)
//
// What it checks: the fetch constant read (on the dwords of two real textures), the tiling and the mip layout against the mod loader's port of XGAddress2DTiled (base level of many sizes,
// the mip chains, a packed tail laid out by hand from Xenia's diagram), the byte swaps, the BC decoders and the means, the piecewise linear gamma, the key, the dictionary reader and the
// whole set builder on a synthetic archive (the same bytes with one thread and with four, the pictures shared between sectors made once, a name that means two pictures, the fallbacks of
// the name resolution, a missing sector), the texture file and its reader (damage, another hit file, the levels from a first mip), and the residency rules (cap, budget, priority, upload
// budget, eviction, a changed cap, failed reads). With a game folder: the retail archive's counts, every texture decoded, the mip chains' agreement, and the sizes at each class's cap.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "citypack/builder.h"
#include "citypack/city_hit_format.h"
#include "citypack/city_texture.h"
#include "citypack/guest_hash.h"
#include "citypack/material_class.h"
#include "citypack/rsc_image.h"
#include "citypack/sector_parse.h"
#include "citypack/texture_residency.h"
#include "citypack/texture_set.h"
#include "citypack/xenos_texture.h"
#include "citypack/xtd_reader.h"
#include "mc_engine/modloader/rpf3.h"
#include "mc_engine/modloader/texture.h"
#if __has_include("mc_engine/modloader/xcompress.h") && defined(LARECOMP_TEXTURE_SET_TEST_REAL_LZX)
#include "mc_engine/modloader/xcompress.h"
#endif

using namespace larecomp::citypack;
namespace fs = std::filesystem;
namespace mm = mc::modloader;

namespace {

int g_failures = 0;
int g_checks = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_failures;                                                          \
      std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                        \
  } while (0)

bool Near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

uint32_t Align(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }
uint32_t NextPow2(uint32_t v) {
  uint32_t p = 1;
  while (p < v) p <<= 1;
  return p;
}

std::vector<uint8_t> RandomBytes(uint32_t seed, size_t count) {
  std::vector<uint8_t> out(count);
  uint32_t s = seed * 2654435761u + 12345u;
  for (size_t i = 0; i < count; ++i) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    out[i] = uint8_t(s >> 11);
  }
  return out;
}

// ---------------------------------------------------------------------------
// synthetic resources in the game's layout (the same helpers as citypack_test.cpp: the virtual segment only)
// ---------------------------------------------------------------------------

struct Img {
  std::vector<uint8_t> d;
  uint32_t Alloc(size_t n, size_t align = 16) {
    while (d.size() % align) d.push_back(0);
    const uint32_t at = uint32_t(d.size());
    d.resize(d.size() + n, 0);
    return at;
  }
  static uint32_t P(uint32_t off) { return 0x50000000u + off; }
  void U32(uint32_t off, uint32_t v) {
    d[off] = uint8_t(v >> 24);
    d[off + 1] = uint8_t(v >> 16);
    d[off + 2] = uint8_t(v >> 8);
    d[off + 3] = uint8_t(v);
  }
  void U16(uint32_t off, uint16_t v) {
    d[off] = uint8_t(v >> 8);
    d[off + 1] = uint8_t(v);
  }
  void F32(uint32_t off, float v) {
    uint32_t b;
    std::memcpy(&b, &v, 4);
    U32(off, b);
  }
  uint32_t Str(const std::string& s) {
    const uint32_t at = Alloc(s.size() + 1, 4);
    std::memcpy(&d[at], s.data(), s.size());
    return at;
  }
};

uint32_t FlagFor(uint32_t size) {
  uint32_t shift = 0;
  while ((1u << (shift + 8)) < size) ++shift;
  return 1u | (shift << 11);
}

std::vector<uint8_t> StoredFile(Img& img, uint32_t type) {
  uint32_t size = 2048;
  while (size < img.d.size()) size <<= 1;
  img.d.resize(size, 0xCD);
  const uint32_t flag = FlagFor(size) | 0x80000000u;
  std::vector<uint8_t> file(12 + img.d.size());
  const auto be = [&](size_t at, uint32_t v) {
    file[at] = uint8_t(v >> 24);
    file[at + 1] = uint8_t(v >> 16);
    file[at + 2] = uint8_t(v >> 8);
    file[at + 3] = uint8_t(v);
  };
  be(0, 0x05435352);
  be(4, type);
  be(8, flag);
  std::memcpy(&file[12], img.d.data(), img.d.size());
  return file;
}

struct SynthSector {
  std::string name;
};
std::vector<uint8_t> MakeCityTable(const std::vector<SynthSector>& sectors) {
  Img img;
  img.Alloc(0x30);
  const uint32_t array = img.Alloc(304 * sectors.size());
  for (size_t i = 0; i < sectors.size(); ++i) {
    const uint32_t e = array + 304 * uint32_t(i);
    for (uint32_t a = 0; a < 3; ++a) {
      img.F32(e + 208 + 4 * a, 0.0f);
      img.F32(e + 224 + 4 * a, 100.0f);
    }
    img.U32(e + 240, Img::P(img.Str(sectors[i].name)));
    img.U32(e + 244, uint32_t(i));
  }
  img.U32(20, Img::P(array));
  img.U16(24, uint16_t(sectors.size()));
  return StoredFile(img, 1086);
}

// ---- synthetic textures: written with the mod loader's tiler (an independent derivation of the tiling), levels laid out as Xenia's rule says ----

struct SynthTex {
  std::string name;
  uint32_t w = 128, h = 128;
  uint32_t format = xenos::kFormatDxt1;
  uint32_t levels = 1;
  bool gamma = false;
  uint32_t seed = 1;
  bool tweak_padding = false;  // change a byte of the tile's padding: the same picture with another guest key
};

uint32_t BytesPerBlock(uint32_t format) { return format == xenos::kFormatDxt1 ? 8u : 16u; }
mm::BlockFormat HostFormat(uint32_t format) { return format == xenos::kFormatDxt1 ? mm::BlockFormat::kBc1 : mm::BlockFormat::kBc3; }

// The linear blocks of level `l` of a synthetic texture (what the reader must give back).
std::vector<uint8_t> LevelBlocks(const SynthTex& t, uint32_t l) {
  const uint32_t w = std::max(t.w >> l, 1u), h = std::max(t.h >> l, 1u);
  return RandomBytes(t.seed * 977u + l * 31u + 7u, size_t((w + 3) / 4) * ((h + 3) / 4) * BytesPerBlock(t.format));
}

// Adds a texture with levels 0..levels-1 (every level on its own tile image: the shorter side must be above 16 << (levels - 1) / 2 so that no level is in a packed tail, which the caller
// asks for by choosing sizes), returns the grcTexture's pointer.
uint32_t AddTexture(Img& img, const SynthTex& t, uint32_t* base_storage_bytes = nullptr, uint32_t* base_offset = nullptr) {
  std::vector<uint8_t> base;
  mm::TileXbox360(LevelBlocks(t, 0), t.w, t.h, HostFormat(t.format), base);
  const uint32_t base_at = img.Alloc(base.size(), 4096);
  std::memcpy(&img.d[base_at], base.data(), base.size());
  if (t.tweak_padding) img.d[base_at + uint32_t(xenos::GetTiledOffset2D(20, 20, 32, t.format == xenos::kFormatDxt1 ? 3 : 4))] ^= 0x5A;  // block (20, 20) is outside a 64 x 64 texture's 16 x 16 blocks
  if (base_storage_bytes) *base_storage_bytes = uint32_t(base.size());
  if (base_offset) *base_offset = base_at;
  uint32_t mip_at = 0;
  if (t.levels > 1) {
    const uint32_t bpb = BytesPerBlock(t.format);
    std::vector<uint32_t> strides(t.levels, 0);
    uint32_t total = 0;
    for (uint32_t l = 1; l < t.levels; ++l) {
      const uint32_t pitch_blocks = Align(Align(std::max(NextPow2(t.w) >> l, 1u), 4) / 4, 32);
      const uint32_t rows = Align(Align(std::max(NextPow2(t.h) >> l, 1u), 4) / 4, 32);
      strides[l] = Align(pitch_blocks * bpb * rows, 4096);
      total += strides[l];
    }
    mip_at = img.Alloc(total, 4096);
    uint32_t offset = 0;
    for (uint32_t l = 1; l < t.levels; ++l) {
      std::vector<uint8_t> tiled;
      mm::TileXbox360(LevelBlocks(t, l), std::max(t.w >> l, 1u), std::max(t.h >> l, 1u), HostFormat(t.format), tiled);
      std::memcpy(&img.d[mip_at + offset], tiled.data(), std::min<size_t>(tiled.size(), strides[l]));
      offset += strides[l];
    }
  }
  const uint32_t d3d = img.Alloc(0x40);
  const uint32_t virtual_width = (t.w < 128 && t.w % 128 != 0) ? 128u : t.w;
  uint32_t d0 = 2u | (1u << 31) | ((virtual_width / 32) << 22);
  if (t.gamma) d0 |= (3u << 2) | (3u << 4) | (3u << 6);
  const uint32_t fetch = d3d + 0x1C;
  img.U32(fetch + 0, d0);
  img.U32(fetch + 4, Img::P(base_at) | t.format | (1u << 6));
  img.U32(fetch + 8, (t.w - 1) | ((t.h - 1) << 13));
  img.U32(fetch + 12, xenos::kIdentitySwizzle << 1);
  img.U32(fetch + 16, t.levels > 1 ? ((t.levels - 1) << 6) : 0u);
  img.U32(fetch + 20, (1u << 9) | (t.levels > 1 ? Img::P(mip_at) : 0u));
  const uint32_t grc = img.Alloc(0x50);
  img.U32(grc + 28, Img::P(d3d));
  img.U16(grc + 32, uint16_t(t.w));
  img.U16(grc + 34, uint16_t(t.h));
  return Img::P(grc);
}

// A dictionary object at `at` (the root itself for a .xtd), listing `textures`.
void WriteDictionary(Img& img, uint32_t at, const std::vector<SynthTex>& textures) {
  const uint32_t hashes = img.Alloc(4 * textures.size());
  const uint32_t array = img.Alloc(4 * textures.size());
  for (size_t i = 0; i < textures.size(); ++i) {
    img.U32(hashes + 4 * uint32_t(i), TextureNameHash(textures[i].name));
    img.U32(array + 4 * uint32_t(i), AddTexture(img, textures[i]));
  }
  img.U32(at + 16, Img::P(hashes));
  img.U16(at + 20, uint16_t(textures.size()));
  img.U16(at + 22, uint16_t(textures.size()));
  img.U32(at + 24, Img::P(array));
}

std::vector<uint8_t> MakeDictionaryFile(const std::vector<SynthTex>& textures) {
  Img img;
  img.Alloc(0x30);
  WriteDictionary(img, 0, textures);
  return StoredFile(img, 1);
}

std::vector<uint8_t> MakeSectorFile(const std::vector<SynthTex>& textures) {
  Img img;
  img.Alloc(0x30);
  const uint32_t dictionary = img.Alloc(0x30);
  WriteDictionary(img, dictionary, textures);
  img.U32(8, Img::P(dictionary));
  return StoredFile(img, 1);
}

// ---------------------------------------------------------------------------
// the tests
// ---------------------------------------------------------------------------

void TestHashAndGamma() {
  CHECK(HashGuestBytes("", 0) == 0x2D06800538D394C2ull);  // XXH3-64 of nothing (the reference value)
  const std::vector<uint8_t> a = RandomBytes(1, 4096), b = RandomBytes(2, 4096);
  CHECK(HashGuestBytes(a.data(), a.size()) == HashGuestBytes(a.data(), a.size()) && HashGuestBytes(a.data(), a.size()) != HashGuestBytes(b.data(), b.size()));

  CHECK(PwlGammaToLinear(0.0f) == 0.0f && Near(PwlGammaToLinear(1.0f), 1.0f, 1e-6));
  CHECK(Near(PwlGammaToLinear(0.5f), 254.0 / 1023.0, 1e-6));  // worked by hand: 0.5 is in the third segment, 4/1024 and -256
  float last = -1.0f;
  for (int v = 0; v <= 255; ++v) {
    const float lin = PwlGammaToLinear(float(v) / 255.0f);
    CHECK(lin >= last && lin >= 0.0f && lin <= 1.0f);
    last = lin;
  }
  CHECK(PwlGammaToLinear(-3.0f) == 0.0f && Near(PwlGammaToLinear(7.0f), 1.0f, 1e-6));

  CHECK(TextureNameHash("l_be_sb_ground_sand_01.dds") == TextureNameHash("l_be_sb_ground_sand_01"));
  CHECK(TextureNameHash("L_BE_SB_Ground_Sand_01.DDS") == TextureNameHash("l_be_sb_ground_sand_01.dds"));
  CHECK(TextureNameHash("a") == RageHashText("a.dds") && TextureNameHash("a") != TextureNameHash("b"));
}

void TestFetchConstants() {
  // The six dwords of a real texture of common.xtd (128 x 128 DXT1 with 3 levels, gamma) and of lod_0.xtd (256 x 256 DXT1, one level, gamma): read from the retail files.
  const uint32_t common[6] = {0x810000fe, 0x518d6052, 0x000fe07f, 0x00000d10, 0x00000080, 0x5106ba00};
  xenos::TextureDesc t;
  std::string why;
  CHECK(xenos::DescribeTexture(common, &t, &why));
  CHECK(t.tiled && t.gamma && t.width == 128 && t.height == 128 && t.format == xenos::kFormatDxt1 && t.endian == xenos::kEndian8in16);
  CHECK(t.base_address == 0x518d6000u && t.mip_address == 0x5106b000u && t.mip_max_level == 2 && t.level_count() == 3 && t.packed_mips && t.pitch_div32 == 4);
  CHECK(t.swizzle == xenos::kIdentitySwizzle && t.dimension == 1 && !t.stacked && t.has_base());
  const uint32_t lod[6] = {0x820000fe, 0x51040052, 0x001fe0ff, 0x00000d10, 0x00000000, 0x00000200};
  CHECK(xenos::DescribeTexture(lod, &t, &why));
  CHECK(t.width == 256 && t.height == 256 && t.mip_address == 0 && t.mip_max_level == 0 && t.level_count() == 1 && !t.packed_mips && t.base_address == 0x51040000u);
  const uint32_t linear[6] = {0x81000002, 0x518d6052, 0x000fe07f, 0x00000d10, 0x00000080, 0x5106ba00};
  CHECK(xenos::DescribeTexture(linear, &t, &why) && !t.gamma);  // sign 0: a data texture
  // what it refuses
  uint32_t bad[6];
  std::memcpy(bad, common, sizeof(bad));
  bad[0] = (bad[0] & ~3u) | 3u;  // another fetch constant type
  CHECK(!xenos::DescribeTexture(bad, &t, &why));
  std::memcpy(bad, common, sizeof(bad));
  bad[5] = (bad[5] & ~(3u << 9)) | (3u << 9);  // a cube
  CHECK(!xenos::DescribeTexture(bad, &t, &why));
  std::memcpy(bad, common, sizeof(bad));
  bad[1] |= 1u << 10;  // stacked
  CHECK(!xenos::DescribeTexture(bad, &t, &why));
  // the mip range's clean up: a mip address with max level 0 means no mips; a base of 0 with mips starts the range at level 1
  std::memcpy(bad, common, sizeof(bad));
  bad[4] = 0;
  CHECK(xenos::DescribeTexture(bad, &t, &why) && t.mip_address == 0 && t.level_count() == 1);
  std::memcpy(bad, common, sizeof(bad));
  bad[1] &= 0x00000FFFu;  // no base address
  CHECK(xenos::DescribeTexture(bad, &t, &why) && !t.has_base() && t.mip_min_level == 1);

  CHECK(xenos::GetFormatInfo(xenos::kFormatDxt1).block_bytes == 8 && xenos::GetFormatInfo(xenos::kFormatDxt45).block_bytes == 16 && xenos::GetFormatInfo(xenos::kFormatDxt23).block_bytes == 16);
  CHECK(xenos::GetFormatInfo(xenos::kFormat8888).block_width == 1 && !xenos::GetFormatInfo(49).supported() && !xenos::GetFormatInfo(52).supported());
}

void TestEndian() {
  uint8_t a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  uint8_t b[8];
  std::memcpy(b, a, 8);
  xenos::SwapEndian(xenos::kEndianNone, b, 8);
  CHECK(std::memcmp(a, b, 8) == 0);
  xenos::SwapEndian(xenos::kEndian8in16, b, 8);
  const uint8_t e1[8] = {2, 1, 4, 3, 6, 5, 8, 7};
  CHECK(std::memcmp(b, e1, 8) == 0);
  std::memcpy(b, a, 8);
  xenos::SwapEndian(xenos::kEndian8in32, b, 8);
  const uint8_t e2[8] = {4, 3, 2, 1, 8, 7, 6, 5};
  CHECK(std::memcmp(b, e2, 8) == 0);
  std::memcpy(b, a, 8);
  xenos::SwapEndian(xenos::kEndian16in32, b, 8);
  const uint8_t e3[8] = {3, 4, 1, 2, 7, 8, 5, 6};
  CHECK(std::memcmp(b, e3, 8) == 0);
}

// A texture description of a tiled DXT texture for the layout tests.
xenos::TextureDesc MakeDesc(uint32_t w, uint32_t h, uint32_t format, uint32_t levels, bool packed) {
  xenos::TextureDesc d;
  const uint32_t virtual_width = (w < 128 && w % 128 != 0) ? 128u : w;
  d.dwords[0] = 2u | (1u << 31) | ((virtual_width / 32) << 22);
  d.type = 2;
  d.tiled = true;
  d.pitch_div32 = virtual_width / 32;
  d.format = format;
  d.endian = xenos::kEndian8in16;
  d.dimension = 1;
  d.packed_mips = packed;
  d.swizzle = xenos::kIdentitySwizzle;
  d.width = w;
  d.height = h;
  d.base_address = 0x50001000u;
  d.mip_address = levels > 1 ? 0x50400000u : 0u;
  d.mip_max_level = levels - 1;
  return d;
}

void TestTilingAgainstModLoader() {
  // The base level of many sizes: the mod loader's tiler writes it, the reader must give the linear blocks back. BC1 and BC3, widths below 128 (stored in a 128 wide image), tall, wide.
  const uint32_t sizes[][2] = {{128, 128}, {256, 64}, {64, 64}, {32, 32}, {512, 256}, {1024, 128}, {128, 512}, {64, 256}, {256, 256}, {2048, 512}, {16, 16}, {8, 8}, {32, 128}, {128, 32}, {4, 128}};
  uint32_t seed = 1;
  for (const auto& s : sizes) {
    for (const uint32_t format : {xenos::kFormatDxt1, xenos::kFormatDxt45}) {
      SynthTex t;
      t.w = s[0];
      t.h = s[1];
      t.format = format;
      t.seed = seed++;
      std::vector<uint8_t> tiled;
      const std::vector<uint8_t> linear = LevelBlocks(t, 0);
      mm::TileXbox360(linear, t.w, t.h, HostFormat(format), tiled);
      xenos::TextureDesc d = MakeDesc(t.w, t.h, format, 1, false);
      const xenos::GuestLayout layout = xenos::GetGuestLayout(d);
      CHECK(layout.ok);
      const xenos::LevelPlan plan = xenos::PlanLevel(d, layout, 0);
      CHECK(plan.ok && plan.width_blocks == t.w / 4 && plan.height_blocks == t.h / 4 && plan.in_base_region);
      std::vector<uint8_t> back;
      CHECK(xenos::DecodeLevel(d, plan, tiled.data(), tiled.size(), &back));
      CHECK(back == linear);
      if (back != linear) std::fprintf(stderr, "  size %ux%u format %u\n", t.w, t.h, format);
      CHECK(layout.base.data_extent_bytes <= tiled.size());  // what the level reaches is inside what the tiler wrote
      // too little memory: the blocks beyond are zero and the call says so
      std::vector<uint8_t> cut;
      CHECK(!xenos::DecodeLevel(d, plan, tiled.data(), 8, &cut));
    }
  }
  // GetTiledOffset2D against the mod loader's tiler on its own terms: a permutation of the blocks (no two blocks at one place) over a whole 128 x 128 texel image
  std::set<int32_t> seen;
  for (int y = 0; y < 32; ++y) {
    for (int x = 0; x < 32; ++x) seen.insert(xenos::GetTiledOffset2D(x, y, 32, 3));
  }
  CHECK(seen.size() == 1024 && *seen.begin() == 0 && *seen.rbegin() == 1023 * 8);
}

void TestMipChain() {
  // 256 x 256 BC1 and 512 x 256 BC3, each level on its own tile image (no packed tail: the shorter side is above 16 << levels).
  struct Case {
    uint32_t w, h, format, levels;
  };
  const Case cases[] = {{256, 256, xenos::kFormatDxt1, 4}, {512, 256, xenos::kFormatDxt45, 4}, {1024, 512, xenos::kFormatDxt1, 5}, {256, 128, xenos::kFormatDxt45, 3}};
  uint32_t seed = 100;
  for (const Case& c : cases) {
    SynthTex t;
    t.w = c.w;
    t.h = c.h;
    t.format = c.format;
    t.levels = c.levels;
    t.seed = seed++;
    Img img;
    img.Alloc(0x30);
    const uint32_t grc = AddTexture(img, t);
    RscImage image;
    {
      const std::vector<uint8_t> file = StoredFile(img, 1);
      std::string error;
      CHECK(DecodeResourceFile(file, nullptr, &image, &error));
    }
    RscCursor cursor(image);
    const uint32_t d3d = cursor.U32(grc + 28);
    uint32_t dwords[6];
    for (uint32_t k = 0; k < 6; ++k) dwords[k] = cursor.U32(d3d + 0x1C + 4 * k);
    xenos::TextureDesc d;
    std::string why;
    CHECK(xenos::DescribeTexture(dwords, &d, &why));
    const xenos::GuestLayout layout = xenos::GetGuestLayout(d);
    CHECK(layout.ok && layout.max_level == c.levels - 1);
    for (uint32_t l = 0; l < c.levels; ++l) {
      const xenos::LevelPlan plan = xenos::PlanLevel(d, layout, l);
      CHECK(plan.ok && plan.width == std::max(c.w >> l, 1u) && plan.height == std::max(c.h >> l, 1u));
      size_t size = 0;
      const uint8_t* region = ResourceBytesAt(image, plan.region_address, &size);
      CHECK(region != nullptr);
      std::vector<uint8_t> back;
      CHECK(region && xenos::DecodeLevel(d, plan, region, size, &back));
      CHECK(back == LevelBlocks(t, l));
      if (back != LevelBlocks(t, l)) std::fprintf(stderr, "  chain %ux%u level %u\n", c.w, c.h, l);
    }
  }
}

void TestPackedTail() {
  // Xenia's diagram of the tail (texels, a 32 x 32 tile): a square texture of 16 x 16 and its levels: 16 x 16 at x = 16, 8 x 8 at x = 8, 4 x 4 at x = 4, 2 x 2 at y = 8, 1 x 1 at y = 4.
  uint32_t x = 0, y = 0;
  CHECK(xenos::GetPackedLevel(16, 16) == 0 && xenos::GetPackedLevel(128, 128) == 3 && xenos::GetPackedLevel(256, 32) == 1 && xenos::GetPackedLevel(2048, 512) == 5);
  CHECK(xenos::GetPackedMipOffset(16, 16, 4, 4, 0, &x, &y) && x == 4 && y == 0);
  CHECK(xenos::GetPackedMipOffset(16, 16, 4, 4, 1, &x, &y) && x == 2 && y == 0);
  CHECK(xenos::GetPackedMipOffset(16, 16, 4, 4, 2, &x, &y) && x == 1 && y == 0);
  CHECK(xenos::GetPackedMipOffset(16, 16, 4, 4, 3, &x, &y) && x == 0 && y == 2);
  CHECK(xenos::GetPackedMipOffset(16, 16, 4, 4, 4, &x, &y) && x == 0 && y == 1);
  CHECK(!xenos::GetPackedMipOffset(128, 128, 4, 4, 2, &x, &y) && x == 0 && y == 0);  // level 2 of 128 x 128 is stored on its own
  CHECK(xenos::GetPackedMipOffset(128, 128, 4, 4, 3, &x, &y) && x == 4 && y == 0);   // the tail starts at level 3 (16 x 16)
  CHECK(xenos::GetPackedMipOffset(256, 32, 4, 4, 1, &x, &y) && x == 0 && y == 4);    // wider than tall: laid out vertically, level 1 (128 x 16) at y = 16 texels

  // A 16 x 16 BC1 texture with all five levels in the tail, laid out by hand from the diagram on a 32 x 32 block image, tiled by the mod loader's tiler.
  std::vector<uint8_t> image(32 * 32 * 8, 0);
  const auto put = [&](uint32_t bx, uint32_t by, uint32_t w_blocks, uint32_t h_blocks, uint32_t seed) {
    const std::vector<uint8_t> blocks = RandomBytes(seed, size_t(w_blocks) * h_blocks * 8);
    for (uint32_t j = 0; j < h_blocks; ++j) {
      for (uint32_t i = 0; i < w_blocks; ++i) std::memcpy(&image[(size_t(by + j) * 32 + bx + i) * 8], &blocks[(size_t(j) * w_blocks + i) * 8], 8);
    }
    return blocks;
  };
  const std::vector<uint8_t> l0 = put(4, 0, 4, 4, 41), l1 = put(2, 0, 2, 2, 42), l2 = put(1, 0, 1, 1, 43), l3 = put(0, 2, 1, 1, 44), l4 = put(0, 1, 1, 1, 45);
  std::vector<uint8_t> tiled;
  mm::TileXbox360(image, 128, 128, mm::BlockFormat::kBc1, tiled);
  xenos::TextureDesc d = MakeDesc(16, 16, xenos::kFormatDxt1, 5, true);
  d.base_address = 0x50001000u;
  d.mip_address = 0x50080000u;
  const xenos::GuestLayout layout = xenos::GetGuestLayout(d);
  CHECK(layout.ok && layout.packed_level == 0);
  const std::vector<uint8_t>* expect[5] = {&l0, &l1, &l2, &l3, &l4};
  for (uint32_t l = 0; l < 5; ++l) {
    const xenos::LevelPlan plan = xenos::PlanLevel(d, layout, l);
    CHECK(plan.ok);
    // the base tail holds level 0; the other levels' tail is the mip region's (the same picture, stored again): both are the hand made image here
    CHECK(plan.in_base_region == (l == 0));
    std::vector<uint8_t> back;
    CHECK(xenos::DecodeLevel(d, plan, tiled.data(), tiled.size(), &back) && back == *expect[l]);
    if (back != *expect[l]) std::fprintf(stderr, "  tail level %u\n", l);
  }
  // a 128 x 128 texture: levels 0 to 2 on their own images, level 3 (16 x 16) and 4 (8 x 8) in the tail, which is the level 3 mip image
  xenos::TextureDesc d2 = MakeDesc(128, 128, xenos::kFormatDxt1, 5, true);
  const xenos::GuestLayout l2layout = xenos::GetGuestLayout(d2);
  CHECK(l2layout.packed_level == 3 && l2layout.mip_offsets_bytes[1] == 0 && l2layout.mip_offsets_bytes[2] == 8192 && l2layout.mip_offsets_bytes[3] == 8192 + 8192);
  const xenos::LevelPlan p3 = xenos::PlanLevel(d2, l2layout, 3), p4 = xenos::PlanLevel(d2, l2layout, 4);
  CHECK(p3.ok && p4.ok && p3.offset_bytes == p4.offset_bytes && p3.tail_x == 4 && p3.tail_y == 0 && p4.tail_x == 2 && p4.tail_y == 0);
  // too many levels for the size are clamped, a texture without mips has its base only
  CHECK(xenos::GetGuestLayout(MakeDesc(16, 16, xenos::kFormatDxt1, 14, true)).max_level == 4);
  const xenos::TextureDesc single = MakeDesc(256, 256, xenos::kFormatDxt1, 1, false);
  CHECK(xenos::PlanLevel(single, xenos::GetGuestLayout(single), 1).ok == false);
}

void TestBlockDecode() {
  // BC1: both endpoints the same colour, every index 0: a solid colour. (r5, g6, b5) = (16, 32, 8) expands to (132, 130, 66).
  const uint16_t c = uint16_t((16u << 11) | (32u << 5) | 8u);
  uint8_t bc1[8] = {uint8_t(c), uint8_t(c >> 8), uint8_t(c), uint8_t(c >> 8), 0, 0, 0, 0};
  std::vector<uint8_t> rgba;
  DecodeBlocksToRgba8(HitTextureFormat::kBc1, bc1, 4, 4, &rgba);
  CHECK(rgba.size() == 64 && rgba[0] == 132 && rgba[1] == 130 && rgba[2] == 66 && rgba[3] == 255 && rgba[60] == 132);
  // c0 < c1 (the 3 colour mode) and an index of 3: transparent black
  const uint16_t lo = 0x0000, hi = uint16_t((31u << 11) | (63u << 5) | 31u);
  uint8_t punch[8] = {uint8_t(lo), uint8_t(lo >> 8), uint8_t(hi), uint8_t(hi >> 8), 0xFF, 0xFF, 0xFF, 0xFF};
  DecodeBlocksToRgba8(HitTextureFormat::kBc1, punch, 4, 4, &rgba);
  CHECK(rgba[3] == 0 && rgba[0] == 0);
  const LevelMean punched = MeanOfLevel(HitTextureFormat::kBc1, false, punch, 4, 4);
  CHECK(punched.alpha_min == 0 && Near(punched.alpha_mean, 0.0, 1e-6));  // nothing counted: the colours as they are (black)
  // c0 > c1: the 4 colour mode, index 2 = (2 c0 + c1) / 3
  uint8_t four[8] = {uint8_t(hi), uint8_t(hi >> 8), uint8_t(lo), uint8_t(lo >> 8), 0xAA, 0xAA, 0xAA, 0xAA};  // all indices 2
  DecodeBlocksToRgba8(HitTextureFormat::kBc1, four, 4, 4, &rgba);
  CHECK(rgba[0] == 170 && rgba[1] == 170 && rgba[2] == 170 && rgba[3] == 255);
  // BC3: alpha 200 for every texel (a0 = a1 = 200, indices 0), the colour half solid
  uint8_t bc3[16] = {200, 200, 0, 0, 0, 0, 0, 0, uint8_t(c), uint8_t(c >> 8), uint8_t(c), uint8_t(c >> 8), 0, 0, 0, 0};
  DecodeBlocksToRgba8(HitTextureFormat::kBc3, bc3, 4, 4, &rgba);
  CHECK(rgba[3] == 200 && rgba[0] == 132 && rgba[63] == 200);
  // BC3 alpha with a0 > a1: index 1 is a1, index 7 is the last interpolated; a0 <= a1: 0 and 255 at 6 and 7
  uint8_t ramp[16] = {255, 0, 0xF8, 0xFF, 0xFF, 0, 0, 0, uint8_t(c), uint8_t(c >> 8), uint8_t(c), uint8_t(c >> 8), 0, 0, 0, 0};
  DecodeBlocksToRgba8(HitTextureFormat::kBc3, ramp, 4, 4, &rgba);
  CHECK(rgba[3] == 255 && rgba[7] == 36);  // texel 0 has index 0 (a0), texel 1 index 7 = (1 * 255 + 6 * 0) / 7
  uint8_t six[16] = {0, 255, 0x3E, 0, 0, 0, 0, 0, uint8_t(c), uint8_t(c >> 8), uint8_t(c), uint8_t(c >> 8), 0, 0, 0, 0};
  DecodeBlocksToRgba8(HitTextureFormat::kBc3, six, 4, 4, &rgba);
  CHECK(rgba[3] == 0 && rgba[7] == 255);  // a0 <= a1: indices 6 and 7 are 0 and 255
  // BC2: explicit 4 bit alpha, nibble 0xA = 170
  uint8_t bc2[16] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, uint8_t(c), uint8_t(c >> 8), uint8_t(c), uint8_t(c >> 8), 0, 0, 0, 0};
  DecodeBlocksToRgba8(HitTextureFormat::kBc2, bc2, 4, 4, &rgba);
  CHECK(rgba[3] == 170 && rgba[0] == 132);
  // a non multiple of 4 level (2 x 2) keeps its own size
  DecodeBlocksToRgba8(HitTextureFormat::kBc1, bc1, 2, 2, &rgba);
  CHECK(rgba.size() == 16 && rgba[0] == 132 && rgba[12] == 132);
  // the mean: a solid texture is its colour, linear (the gamma path through the piecewise function)
  const LevelMean plain = MeanOfLevel(HitTextureFormat::kBc1, false, bc1, 4, 4);
  CHECK(Near(plain.rgb[0], 132.0 / 255.0, 1e-5) && Near(plain.rgb[1], 130.0 / 255.0, 1e-5) && Near(plain.rgb[2], 66.0 / 255.0, 1e-5) && plain.alpha_min == 255);
  const LevelMean gamma = MeanOfLevel(HitTextureFormat::kBc1, true, bc1, 4, 4);
  CHECK(Near(gamma.rgb[0], PwlGammaToLinear(132.0f / 255.0f), 1e-5) && gamma.rgb[0] < plain.rgb[0]);
  // the BC3 alpha is a data channel as far as the colour mean goes: all its texels count
  const LevelMean m3 = MeanOfLevel(HitTextureFormat::kBc3, false, bc3, 4, 4);
  CHECK(Near(m3.rgb[0], 132.0 / 255.0, 1e-5) && m3.alpha_min == 200 && Near(m3.alpha_mean, 200.0 / 255.0, 1e-5));
}

void TestDecodeCityTexture() {
  // A synthetic dictionary resource with one texture of each kind; decode and compare every level with what was written.
  SynthTex t;
  t.name = "wall.dds";
  t.w = 256;
  t.h = 128;
  t.levels = 3;
  t.gamma = true;
  t.seed = 9;
  Img img;
  img.Alloc(0x30);
  uint32_t storage = 0, at = 0;
  const uint32_t grc = AddTexture(img, t, &storage, &at);
  const std::vector<uint8_t> file = StoredFile(img, 1);
  RscImage image;
  std::string error;
  CHECK(DecodeResourceFile(file, nullptr, &image, &error));
  // wrap the texture in a dictionary by hand
  Img dict;
  dict.d = img.d;
  const uint32_t hashes = dict.Alloc(4), array = dict.Alloc(4);
  dict.U32(hashes, TextureNameHash(t.name));
  dict.U32(array, grc);
  dict.U32(16, Img::P(hashes));
  dict.U16(20, 1);
  dict.U32(24, Img::P(array));
  RscImage dimage;
  CHECK(DecodeResourceFile(StoredFile(dict, 1), nullptr, &dimage, &error));
  std::vector<DictionaryTexture> list;
  DictionaryStats stats;
  CHECK(ParseDictionaryFile(dimage, &list, &stats, &error) && list.size() == 1 && stats.textures == 1 && stats.entries == 1);
  CHECK(list[0].name_hash == TextureNameHash("wall") && list[0].grc_width == 256 && list[0].desc.gamma && list[0].desc.level_count() == 3);
  CHECK(stats.size_mismatch == 0 && stats.odd_swizzle == 0 && stats.odd_endian == 0 && stats.not_tiled == 0);
  const DecodedTexture d = DecodeCityTexture(dimage, list[0]);
  CHECK(d.ok && d.width == 256 && d.height == 128 && d.format == HitTextureFormat::kBc1 && d.mip_count == 3 && d.stored_levels == 3 && d.damaged_levels == 0);
  CHECK((d.flags & kTextureGamma) != 0 && (d.flags & kTextureAlpha) == 0);
  size_t offset = 0;
  for (uint32_t l = 0; l < 3; ++l) {
    const std::vector<uint8_t> expect = LevelBlocks(t, l);
    CHECK(d.data.size() >= offset + expect.size() && std::memcmp(d.data.data() + offset, expect.data(), expect.size()) == 0);
    offset += expect.size();
  }
  CHECK(offset == d.data.size());
  // the key is the XXH3 of the base level's storage: the extent GetGuestLayout gives from its address
  const xenos::GuestLayout layout = xenos::GetGuestLayout(list[0].desc);
  CHECK(d.key_complete && d.key == HashGuestBytes(&dimage.data[at], layout.base.data_extent_bytes));
  uint64_t key2 = 0;
  CHECK(CityTextureKey(dimage, list[0], &key2) && key2 == d.key);
  CHECK(layout.base.data_extent_bytes <= storage);
  // the mean is made from a level that is at most 64 texels on its longer side: 256 x 128, 128 x 64, 64 x 32: level 2
  CHECK(d.average_level == 2);
  // a texture whose mip region is cut: the chain ends at the last whole level, the base is kept
  RscImage cut = dimage;
  const xenos::GuestLayout lay = xenos::GetGuestLayout(list[0].desc);
  const size_t mip_at = list[0].desc.mip_address & 0x0FFFFFFFu;
  cut.data.resize(mip_at + lay.mip_offsets_bytes[2] + 16);  // the second mip level (level 2) starts here: it is cut to 16 bytes
  const DecodedTexture damaged = DecodeCityTexture(cut, list[0]);
  CHECK(damaged.ok && damaged.mip_count == 2 && damaged.damaged_levels == 1);
  cut.data.resize(at + 100);  // the base itself is cut
  CHECK(!DecodeCityTexture(cut, list[0]).ok);
  // a dictionary of three entries: a pointer outside the resource, a texture whose format is not one the reader decodes (k_DXN, 49), and a cube
  Img odd;
  odd.d = img.d;
  RscCursor cc(dimage);
  const uint32_t fetch = cc.U32(grc + 28) + 0x1C;  // the fetch constant of the first texture, copied twice below
  const uint32_t grc2 = odd.Alloc(0x50), grc3 = odd.Alloc(0x50), d2 = odd.Alloc(0x40), d3 = odd.Alloc(0x40);
  for (uint32_t k = 0; k < 24; ++k) {
    odd.d[d2 + 0x1C + k] = dimage.data[fetch - 0x50000000u + k];
    odd.d[d3 + 0x1C + k] = dimage.data[fetch - 0x50000000u + k];
  }
  odd.U32(d2 + 0x1C + 4, (cc.U32(fetch + 4) & ~63u) | 49u);
  odd.U32(d3 + 0x1C + 20, (cc.U32(fetch + 20) & ~(3u << 9)) | (3u << 9));
  odd.U32(grc2 + 28, Img::P(d2));
  odd.U32(grc3 + 28, Img::P(d3));
  const uint32_t oh = odd.Alloc(12), oa = odd.Alloc(12);
  odd.U32(oh, 1);
  odd.U32(oh + 4, 2);
  odd.U32(oh + 8, 3);
  odd.U32(oa, 0x50FFFFF0u);  // outside the resource
  odd.U32(oa + 4, Img::P(grc2));
  odd.U32(oa + 8, Img::P(grc3));
  odd.U32(16, Img::P(oh));
  odd.U16(20, 3);
  odd.U32(24, Img::P(oa));
  RscImage oimage;
  CHECK(DecodeResourceFile(StoredFile(odd, 1), nullptr, &oimage, &error));
  std::vector<DictionaryTexture> olist;
  DictionaryStats ostats;
  CHECK(ParseDictionaryFile(oimage, &olist, &ostats, &error));
  CHECK(olist.empty() && ostats.entries == 3 && ostats.bad_pointer == 1 && ostats.unsupported_format == 1 && ostats.not_supported == 1 && ostats.unsupported_formats == std::vector<uint32_t>{49});
  // no dictionary where it should be
  RscImage plain;
  CHECK(DecodeResourceFile(StoredFile(img, 1), nullptr, &plain, &error));
  CHECK(!ParseSectorDictionary(plain, &olist, &ostats, &error));
}

// A raw k_8_8_8_8 texture (the retail city has none: this is the only check of that path): linear storage (rows on the fetch constant's pitch), the fetch swizzle picks each channel.
void TestRaw8888() {
  const uint32_t w = 8, h = 4;
  const auto make = [&](uint32_t swizzle, uint32_t endian) {
    Img img;
    img.Alloc(0x30);
    const uint32_t base = img.Alloc(128 * h, 4096);  // 32 texel rows: 128 bytes
    for (uint32_t y = 0; y < h; ++y) {
      for (uint32_t x = 0; x < w; ++x) {
        const uint32_t at = base + y * 128 + x * 4;
        img.d[at] = uint8_t(10 + x);       // raw channel 0
        img.d[at + 1] = uint8_t(50 + y);   // 1
        img.d[at + 2] = uint8_t(100 + x);  // 2
        img.d[at + 3] = uint8_t(200 + y);  // 3
      }
    }
    const uint32_t d3d = img.Alloc(0x40);
    const uint32_t fetch = d3d + 0x1C;
    img.U32(fetch + 0, 2u | (1u << 22));  // linear, pitch 32 texels
    img.U32(fetch + 4, Img::P(base) | xenos::kFormat8888 | (endian << 6));
    img.U32(fetch + 8, (w - 1) | ((h - 1) << 13));
    img.U32(fetch + 12, swizzle << 1);
    img.U32(fetch + 20, 1u << 9);
    const uint32_t grc = img.Alloc(0x50);
    img.U32(grc + 28, Img::P(d3d));
    img.U16(grc + 32, uint16_t(w));
    img.U16(grc + 34, uint16_t(h));
    const uint32_t hashes = img.Alloc(4), array = img.Alloc(4);
    img.U32(hashes, 7);
    img.U32(array, Img::P(grc));
    img.U32(16, Img::P(hashes));
    img.U16(20, 1);
    img.U32(24, Img::P(array));
    RscImage image;
    std::string error;
    CHECK(DecodeResourceFile(StoredFile(img, 1), nullptr, &image, &error));
    std::vector<DictionaryTexture> list;
    DictionaryStats stats;
    CHECK(ParseDictionaryFile(image, &list, &stats, &error) && list.size() == 1);
    return std::make_pair(std::move(image), list.empty() ? DictionaryTexture{} : list[0]);
  };
  // identity: the raw bytes
  {
    const auto [image, tex] = make(xenos::kIdentitySwizzle, 0);
    CHECK(!tex.desc.tiled && tex.desc.format == xenos::kFormat8888);
    const DecodedTexture d = DecodeCityTexture(image, tex);
    CHECK(d.ok && d.format == HitTextureFormat::kRgba8 && d.mip_count == 1 && d.data.size() == w * h * 4);
    bool same = d.data.size() == w * h * 4;
    for (uint32_t y = 0; y < h && same; ++y) {
      for (uint32_t x = 0; x < w; ++x) {
        const uint8_t* p = &d.data[(y * w + x) * 4];
        same = same && p[0] == 10 + x && p[1] == 50 + y && p[2] == 100 + x && p[3] == 200 + y;
      }
    }
    CHECK(same);
    CHECK((d.flags & kTextureAlpha) == 0);  // alpha 200..203: not coverage
  }
  // x y z w = channels 2 1 0 3 (swap red and blue), and w = constant one; z = zero
  {
    const uint32_t swizzle = 2u | (1u << 3) | (0u << 6) | (3u << 9);
    const auto [image, tex] = make(swizzle, 0);
    const DecodedTexture d = DecodeCityTexture(image, tex);
    CHECK(d.ok && d.data[0] == 100 && d.data[1] == 50 && d.data[2] == 10 && d.data[3] == 200);
    const uint32_t constants = 0u | (4u << 3) | (5u << 6) | (3u << 9);  // r, zero, one, a
    const auto [image2, tex2] = make(constants, 0);
    const DecodedTexture e = DecodeCityTexture(image2, tex2);
    CHECK(e.ok && e.data[0] == 10 && e.data[1] == 0 && e.data[2] == 255 && e.data[3] == 200);
  }
  // 8in32: the four bytes of a texel arrive reversed
  {
    const auto [image, tex] = make(xenos::kIdentitySwizzle, xenos::kEndian8in32);
    const DecodedTexture d = DecodeCityTexture(image, tex);
    CHECK(d.ok && d.data[0] == 200 && d.data[1] == 100 && d.data[2] == 50 && d.data[3] == 10);
  }
}

// ---------------------------------------------------------------------------
// the set builder, on a synthetic archive
// ---------------------------------------------------------------------------

std::vector<uint8_t> ReadAll(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

class CountingSink final : public ProgressSink {
 public:
  void BeginPhase(const std::string&, int total) override {
    ++phases;
    planned = total;
  }
  void Step(const std::string&) override { ++steps; }
  bool Cancelled() const override { return cancel_after >= 0 && steps >= cancel_after; }
  int phases = 0, planned = 0, steps = 0, cancel_after = -1;
};

SynthTex Tex(const char* name, uint32_t w, uint32_t h, uint32_t format, uint32_t levels, uint32_t seed, bool gamma = false, bool tweak = false) {
  SynthTex t;
  t.tweak_padding = tweak;
  t.name = name;
  t.w = w;
  t.h = h;
  t.format = format;
  t.levels = levels;
  t.seed = seed;
  t.gamma = gamma;
  return t;
}

void TestSetBuilder(const fs::path& scratch) {
  fs::create_directories(scratch);
  const fs::path rpf = scratch / "xarchive_cache.rpf";
  mm::Rpf3Writer writer;
  const auto add = [&](const std::string& path, std::vector<uint8_t> file, uint32_t type) {
    const uint32_t flag = (uint32_t(file[8]) << 24) | (uint32_t(file[9]) << 16) | (uint32_t(file[10]) << 8) | uint32_t(file[11]);
    writer.Add(path, std::move(file), flag, type & 0xFF);
  };
  add(kCityTablePath, MakeCityTable({{"s_a"}, {"s_b"}, {"s_missing"}}), 50);
  const SynthTex shared = Tex("shared", 128, 128, xenos::kFormatDxt45, 3, 7);
  add(kCommonDictionaryPath, MakeDictionaryFile({Tex("common_t", 128, 128, xenos::kFormatDxt1, 1, 1), Tex("dup", 128, 128, xenos::kFormatDxt1, 1, 5)}), 1);
  add(kLodDictionaryPath, MakeDictionaryFile({Tex("lod_t", 256, 256, xenos::kFormatDxt1, 1, 2, true)}), 1);
  add(SectorArchivePath("s_a"), MakeSectorFile({Tex("a", 256, 256, xenos::kFormatDxt1, 4, 3, true), shared, Tex("unique_a", 64, 64, xenos::kFormatDxt1, 1, 4), Tex("dup", 128, 128, xenos::kFormatDxt1, 1, 6)}), 1);
  add(SectorArchivePath("s_b"), MakeSectorFile({shared, Tex("b", 512, 256, xenos::kFormatDxt1, 4, 8), Tex("alias", 64, 64, xenos::kFormatDxt1, 1, 4, false, true)}), 1);
  CHECK(writer.Write(rpf));

  TextureSetRequest request;
  request.source_rpf = rpf;
  request.lzx = [](const uint8_t*, size_t, std::vector<uint8_t>&, size_t) { return false; };
  CountingSink sink;
  request.threads = 1;
  TextureSetResult one = BuildTextureSet(request, &sink);
  CHECK(one.ok);
  if (!one.ok) {
    std::fprintf(stderr, "build failed: %s\n", one.error.c_str());
    return;
  }
  request.threads = 4;
  const TextureSetResult four = BuildTextureSet(request, nullptr);
  CHECK(four.ok);
  CHECK(sink.phases == 1 && sink.planned == 5 && sink.steps == 5);  // common, lod, three sectors

  const TextureSetStats& s = one.stats;
  CHECK(s.sources == 5 && s.sectors == 3 && s.sources_failed == 1);                  // s_missing has no file
  CHECK(s.entries == 10 && s.distinct == 8 && s.key_variants == 9 && s.decode_failed == 0 && s.damaged == 0);  // common 2 + lod 1 + s_a 4 + s_b 3; "shared" is one entry, "alias" a
                                                                                                                // second key of "unique_a"'s picture
  CHECK(s.duplicate_bytes == 2048 && s.data_bytes > 0);                                                         // the alias would have taken a 64 x 64 BC1 level
  CHECK(s.distinct_names == 8 && s.cross_conflicts == 1 && s.name_conflicts == 0);     // "dup" means two pictures (common's and s_a's)
  CHECK(s.by_format[size_t(HitTextureFormat::kBc1)] == 7 && s.by_format[size_t(HitTextureFormat::kBc3)] == 1);  // BC3: shared; BC1: the other seven pictures
  CHECK(s.by_format[0] + s.by_format[1] + s.by_format[2] + s.by_format[3] == 8);

  // the same bytes whatever the thread count
  CHECK(one.set.blobs == four.set.blobs && one.set.data_bytes == four.set.data_bytes && one.set.names == four.set.names && one.set.textures.size() == four.set.textures.size());
  for (size_t i = 0; i < one.set.textures.size(); ++i) {
    const HitTexture &x = one.set.textures[i], &y = four.set.textures[i];
    CHECK(x.key == y.key && x.width == y.width && x.height == y.height && x.mip_count == y.mip_count && x.data_offset == y.data_offset && x.data_size == y.data_size && x.flags == y.flags);
  }

  // the table's order: common, lod_0, then the sectors in the city table's order
  const TextureSet& set = one.set;
  CHECK(set.textures.size() == 8);
  CHECK(set.textures[0].width == 128 && set.textures[1].width == 128 && set.textures[2].width == 256 && set.textures[2].format == HitTextureFormat::kBc1);  // common_t, dup(5), lod_t
  CHECK((set.textures[2].flags & kTextureGamma) != 0 && (set.textures[0].flags & kTextureGamma) == 0);
  const uint32_t h_common = TextureNameHash("common_t"), h_dup = TextureNameHash("dup"), h_shared = TextureNameHash("shared"), h_lod = TextureNameHash("lod_t");
  const uint32_t s_a = DictionaryOfSector(0), s_b = DictionaryOfSector(1), s_missing = DictionaryOfSector(2);
  CHECK(set.ResolveExact(kDictionaryCommon, h_common) == 0 && set.ResolveExact(kDictionaryCommon, h_dup) == 1 && set.ResolveExact(kDictionaryLod, h_lod) == 2);
  const uint32_t dup_a = set.ResolveExact(s_a, h_dup);
  CHECK(dup_a != kNoTexture && dup_a != 1 && set.Resolve(s_a, h_dup) == dup_a);        // a sector's own picture first
  CHECK(set.Resolve(s_b, h_dup) == 1);                                                  // none of its own: common's
  CHECK(set.Resolve(s_b, h_lod) == 2 && set.Resolve(s_a, h_common) == 0);               // lod_0 and common are the fallbacks
  CHECK(set.Resolve(kDictionaryLod, h_common) == 0 && set.Resolve(kDictionaryCommon, h_lod) == 2);
  CHECK(set.Resolve(kDictionaryLod, h_shared) == kNoTexture && set.Resolve(s_missing, h_shared) == kNoTexture && set.Resolve(s_a, TextureNameHash("nothing")) == kNoTexture);
  CHECK(set.ResolveExact(s_a, h_shared) == set.ResolveExact(s_b, h_shared) && set.ResolveExact(s_a, h_shared) != kNoTexture);  // one picture, two sectors
  CHECK(set.PicturesOf(s_a).size() == 4 && set.PicturesOf(s_b).size() == 3 && set.PicturesOf(kDictionaryLod).size() == 1 && set.PicturesOf(s_missing).empty());

  // every picture's levels are what was written; the keys are the XXH3 of the base level's storage; the table's records agree with the data
  struct Expect {
    SynthTex t;
    uint32_t dictionary;
  };
  const std::vector<Expect> expect = {{Tex("common_t", 128, 128, xenos::kFormatDxt1, 1, 1), kDictionaryCommon},
                                      {Tex("dup", 128, 128, xenos::kFormatDxt1, 1, 5), kDictionaryCommon},
                                      {Tex("lod_t", 256, 256, xenos::kFormatDxt1, 1, 2, true), kDictionaryLod},
                                      {Tex("a", 256, 256, xenos::kFormatDxt1, 4, 3, true), s_a},
                                      {shared, s_a},
                                      {Tex("unique_a", 64, 64, xenos::kFormatDxt1, 1, 4), s_a},
                                      {Tex("dup", 128, 128, xenos::kFormatDxt1, 1, 6), s_a},
                                      {Tex("b", 512, 256, xenos::kFormatDxt1, 4, 8), s_b}};
  for (const Expect& e : expect) {
    const uint32_t index = set.ResolveExact(e.dictionary, TextureNameHash(e.t.name));
    CHECK(index < set.textures.size());
    if (index >= set.textures.size()) continue;
    const HitTexture& t = set.textures[index];
    CHECK(t.width == e.t.w && t.height == e.t.h && t.mip_count == e.t.levels && t.data_offset >= kTexHeaderBytes && t.data_offset % kTextureDataAlignment == 0);
    CHECK(t.data_size == t.ChainBytes(0) && t.data_offset - kTexHeaderBytes + t.data_size <= set.data_bytes);
    uint64_t offset = 0;
    bool same = set.blobs[index].size() == t.data_size;
    for (uint32_t l = 0; l < e.t.levels; ++l) {
      const std::vector<uint8_t> blocks = LevelBlocks(e.t, l);
      same = same && offset + blocks.size() <= set.blobs[index].size() && std::memcmp(set.blobs[index].data() + offset, blocks.data(), blocks.size()) == 0;
      offset += blocks.size();
    }
    CHECK(same);
    CHECK(t.key != 0 && ((t.flags & kTextureGamma) != 0) == e.t.gamma);
    CHECK(set.average[index][0] >= 0.0f && set.average[index][0] <= 1.0f);
  }
  CHECK(set.file_bytes() == kTexHeaderBytes + set.data_bytes);
  // the alias: the same picture, its own key
  const ResolvedTexture alias = set.ResolveEntry(s_b, TextureNameHash("alias")), unique = set.ResolveEntry(s_a, TextureNameHash("unique_a"));
  CHECK(alias.found() && unique.found() && alias.index == unique.index && alias.key != unique.key && alias.key != 0 && alias.dictionary == s_b && unique.dictionary == s_a);
  CHECK(set.textures[unique.index].key == unique.key);  // the table row has the key of the picture's first entry

  // the texture file: written, read back whole and from a first mip, and refused when it is another build's
  const fs::path file_path = scratch / "out" / kTexFileName;
  const uint64_t hit_hash = 0x1122334455667788ull;
  std::string error;
  CHECK(WriteTextureFile(file_path, one.set, hit_hash, &error));
  CHECK(fs::exists(file_path) && !fs::exists(file_path.string() + ".tmp") && fs::file_size(file_path) == set.file_bytes());
  std::vector<std::string> problems;
  CHECK(ValidateTextureFile(file_path, set.textures, hit_hash, &problems) == 0);
  TextureFileReader reader;
  CHECK(reader.Open(file_path, set.textures, hit_hash, &error) && reader.is_open());
  const uint32_t a_index = set.ResolveExact(s_a, TextureNameHash("a"));
  std::vector<uint8_t> bytes;
  CHECK(reader.ReadMips(a_index, 0, &bytes, &error) && bytes.size() == set.textures[a_index].data_size);
  CHECK(std::memcmp(bytes.data(), set.blobs[a_index].data(), bytes.size()) == 0);
  std::vector<uint8_t> tail;
  CHECK(reader.ReadMips(a_index, 2, &tail, &error) && tail.size() == set.textures[a_index].ChainBytes(2) && tail.size() < bytes.size());
  CHECK(std::memcmp(tail.data(), bytes.data() + set.textures[a_index].MipOffset(2), tail.size()) == 0);
  CHECK(!reader.ReadMips(a_index, 4, &tail, &error) && !reader.ReadMips(9999, 0, &tail, &error));
  TextureFileReader other;
  CHECK(!other.Open(file_path, set.textures, hit_hash + 1, &error));  // another hit file's table
  std::vector<HitTexture> wrong = set.textures;
  wrong.pop_back();
  CHECK(!other.Open(file_path, wrong, hit_hash, &error));              // another count
  wrong = set.textures;
  wrong[1].data_size += 16;
  CHECK(!other.Open(file_path, wrong, hit_hash, &error));              // a record that does not match its levels
  {
    std::vector<uint8_t> raw = ReadAll(file_path);
    raw.resize(raw.size() - 100);
    const fs::path cut_path = scratch / "out" / "cut.pack";
    std::ofstream(cut_path, std::ios::binary).write(reinterpret_cast<const char*>(raw.data()), std::streamsize(raw.size()));
    CHECK(!other.Open(cut_path, set.textures, hit_hash, &error));      // cut
    raw[0] ^= 0xFF;
    std::ofstream(cut_path, std::ios::binary).write(reinterpret_cast<const char*>(raw.data()), std::streamsize(raw.size()));
    CHECK(!other.Open(cut_path, set.textures, hit_hash, &error));      // not a texture file
  }
  CHECK(ValidateTextureFile(scratch / "out" / "missing.pack", set.textures, hit_hash, &problems) == 1);
  // writing twice gives the same file; the same set from one and four threads gives the same file
  const fs::path file4 = scratch / "out" / "four.pack";
  CHECK(WriteTextureFile(file4, four.set, hit_hash, &error) && ReadAll(file4) == ReadAll(file_path));

  // a cancelled build leaves nothing and says so
  CountingSink stopper;
  stopper.cancel_after = 2;
  request.threads = 2;
  const TextureSetResult stopped = BuildTextureSet(request, &stopper);
  CHECK(stopped.cancelled && !stopped.ok);
  // refusals
  TextureSetRequest bad = request;
  bad.lzx = nullptr;
  CHECK(!BuildTextureSet(bad, nullptr).ok);
  bad = request;
  bad.source_rpf = scratch / "no_such_archive.rpf";
  CHECK(!BuildTextureSet(bad, nullptr).ok);
}

// ---------------------------------------------------------------------------
// residency
// ---------------------------------------------------------------------------

HitTexture MakeRecord(uint32_t w, uint32_t h, uint32_t mips, HitTextureFormat format = HitTextureFormat::kBc1) {
  HitTexture t;
  t.width = uint16_t(w);
  t.height = uint16_t(h);
  t.mip_count = uint8_t(mips);
  t.format = format;
  t.data_size = uint32_t(t.ChainBytes(0));
  return t;
}

void TestResidency() {
  // the cap
  const HitTexture big = MakeRecord(512, 256, 7), small = MakeRecord(64, 64, 4), single = MakeRecord(256, 256, 1), flat = MakeRecord(128, 128, 3);
  CHECK(FirstResidentMip(big, 0) == 0 && FirstResidentMip(big, 512) == 0 && FirstResidentMip(big, 256) == 1 && FirstResidentMip(big, 128) == 2 && FirstResidentMip(big, 1) == 6);
  CHECK(FirstResidentMip(small, 128) == 0 && FirstResidentMip(single, 128) == 0 && FirstResidentMip(flat, 64) == 1 && FirstResidentMip(flat, 32) == 2);
  CHECK(ResidentBytes(big, 0) == big.data_size && ResidentBytes(big, 1) == big.ChainBytes(1) && ResidentBytes(big, 1) < ResidentBytes(big, 0));
  CHECK(small.ChainBytes(0) == 2048 + 512 + 128 + 32 && AccountedBytes(small, 0, 4096) == 4096);  // 64 x 64 BC1 with four levels
  CHECK(AccountedBytes(small, 0, 1) == small.ChainBytes(0) && AccountedBytes(small, 0, 0) == small.ChainBytes(0) && AccountedBytes(big, 0, 4096) % 4096 == 0 && AccountedBytes(big, 0, 4096) >= big.data_size);
  const std::vector<HitTexture> table = {big, small, single, flat};
  const ResidentTotals full = TotalResidentBytes(table, 0), c256 = TotalResidentBytes(table, 256), c128 = TotalResidentBytes(table, 128);
  CHECK(full.textures == 4 && full.capped == 0 && c256.capped == 1 && c128.capped == 1);  // only the 512 x 256 one has a level to give up (the single level one has none)
  CHECK(full.bytes > c256.bytes && c256.bytes > c128.bytes);
  CHECK(full.bytes == big.data_size + small.data_size + single.data_size + flat.data_size);

  // the upload layout: rows on 256 bytes, levels in the file's tight order
  {
    const std::vector<UploadLevel> up = PlanUploadLevels(big, 0);  // 512 x 256 BC1, 7 levels
    CHECK(up.size() == 7 && up[0].width == 512 && up[0].height == 256 && up[0].rows == 64 && up[0].row_bytes == 1024 && up[0].pitch == 1024 && up[0].source_offset == 0);
    CHECK(up[6].width == 8 && up[6].height == 4 && up[6].rows == 1 && up[6].row_bytes == 16 && up[6].pitch == 256 && up[6].staging_bytes() == 256);
    uint64_t tight = 0;
    for (const UploadLevel& l : up) {
      CHECK(l.source_offset == tight && l.pitch % kUploadPitchAlignment == 0 && l.pitch >= l.row_bytes);
      tight += uint64_t(l.row_bytes) * l.rows;
    }
    CHECK(tight == big.ChainBytes(0));
    const std::vector<UploadLevel> from2 = PlanUploadLevels(big, 2);
    CHECK(from2.size() == 5 && from2[0].width == 128 && from2[0].source_offset == 0 && uint64_t(from2[0].row_bytes) * from2[0].rows + 0 == HitMipBytes(HitTextureFormat::kBc1, 128, 64));
    CHECK(PlanUploadLevels(big, 7).empty());
    // levels smaller than a block count as one; BC3 has 16 byte blocks; the raw format has one texel blocks
    HitTexture tiny = MakeRecord(8, 8, 4, HitTextureFormat::kBc3);  // 8, 4, 2, 1
    const std::vector<UploadLevel> small_levels = PlanUploadLevels(tiny, 0);
    CHECK(small_levels.size() == 4 && small_levels[2].rows == 1 && small_levels[2].row_bytes == 16 && small_levels[3].rows == 1 && small_levels[3].row_bytes == 16 && small_levels[0].row_bytes == 32);
    HitTexture raw = MakeRecord(30, 10, 2, HitTextureFormat::kRgba8);
    const std::vector<UploadLevel> raw_levels = PlanUploadLevels(raw, 0);
    CHECK(raw_levels.size() == 2 && raw_levels[0].rows == 10 && raw_levels[0].row_bytes == 120 && raw_levels[0].pitch == 256 && raw_levels[1].width == 15 && raw_levels[1].rows == 5 && raw_levels[1].source_offset == 1200);
    CHECK(PlanUploadLevels(raw, 0, 1)[0].pitch == 120 && PlanUploadLevels(raw, 0, 0)[0].pitch == 120);
  }

  // textures of a sector from its runs' materials
  std::vector<HitMaterial> materials(4);
  materials[1].texture = 2;
  materials[2].texture = 0;
  materials[2].layer = 3;
  materials[3].texture = 2;
  std::vector<HitRun> runs(4);
  runs[0].material = 0;
  runs[1].material = 1;
  runs[2].material = 2;
  runs[3].material = 3;
  CHECK(CollectTextures(runs, materials) == (std::vector<uint16_t>{0, 2, 3}));
  runs[3].material = 99;  // a material that is not there is skipped, not read
  CHECK(CollectTextures(runs, materials) == (std::vector<uint16_t>{0, 2, 3}));
  CHECK(CollectTextures({}, materials).empty());

  // the loader's side, played by the test: reads finish when told
  std::vector<HitTexture> t;
  for (uint32_t i = 0; i < 6; ++i) t.push_back(MakeRecord(256, 256, 1));  // 32 KiB each (BC1)
  const uint64_t one = t[0].data_size;
  CHECK(one == 32768);
  {
    TextureResidency r(t);
    ResidencyConfig c;
    c.upload_bytes_per_frame = one * 2;
    c.max_reads_in_flight = 3;
    r.Configure(c);
    const uint16_t near_set[] = {2, 0}, far_set[] = {4, 1, 0};
    r.AddSector(10, near_set, 2, 1.0f);
    r.AddSector(11, far_set, 3, 5.0f);
    TextureResidency::Plan p = r.Step(1);
    // 3 reads at once; the nearest sector's textures first (0 is shared: the better priority wins), then the far one's in index order
    CHECK(p.reads.size() == 3 && p.reads[0].index == 0 && p.reads[1].index == 2 && p.reads[2].index == 1);
    CHECK(p.uploads.empty() && p.evictions.empty() && r.state(0) == TextureState::kReading && r.referenced_by(0) == 2 && r.committed_bytes() == 3 * one);
    CHECK(r.ReadFinished(0, true) && r.ReadFinished(2, true) && r.ReadFinished(1, true));
    p = r.Step(2);
    // two uploads a frame at most, nearest first; and the next read starts
    CHECK(p.uploads == (std::vector<uint32_t>{0, 2}) && r.state(0) == TextureState::kResident && r.state(1) == TextureState::kReady);
    CHECK(p.reads.size() == 1 && p.reads[0].index == 4);
    p = r.Step(3);
    CHECK(p.uploads == (std::vector<uint32_t>{1}) && r.resident_count() == 3 && r.resident_bytes() == 3 * one);
    r.ReadFinished(4, true);
    p = r.Step(4);
    CHECK(p.uploads == (std::vector<uint32_t>{4}) && p.reads.empty() && r.resident_count() == 4);
    // a sector leaves: its own textures stay until memory or time asks (idle), the shared one stays as the other sector keeps it
    r.RemoveSector(10);
    p = r.Step(5);
    CHECK(p.evictions.empty() && r.referenced_by(2) == 0 && r.referenced_by(0) == 1);
    p = r.Step(c.idle_frames + 3);  // 4 was the last frame the texture was wanted: not idle long enough yet
    CHECK(p.evictions.empty());
    p = r.Step(c.idle_frames + 5);
    CHECK(p.evictions == (std::vector<uint32_t>{2}) && r.state(2) == TextureState::kAbsent && r.resident_count() == 3 && r.evicted_total() == 1);
    // a sector comes back: the texture is read again
    r.AddSector(12, near_set, 2, 0.5f);
    p = r.Step(c.idle_frames + 6);
    CHECK(p.reads.size() == 1 && p.reads[0].index == 2);
  }
  {
    // the budget: room for 2 textures. The near sector's two textures load; the far sector's third waits; when the near sector leaves, its textures (unreferenced) make room first.
    TextureResidency r(t);
    ResidencyConfig c;
    c.budget_bytes = one * 2;
    c.upload_bytes_per_frame = one * 8;
    c.idle_frames = 100000;
    r.Configure(c);
    const uint16_t near_set[] = {0, 1}, far_set[] = {2};
    r.AddSector(1, near_set, 2, 1.0f);
    r.AddSector(2, far_set, 1, 9.0f);
    TextureResidency::Plan p = r.Step(1);
    CHECK(p.reads.size() == 2 && p.reads[0].index == 0 && p.reads[1].index == 1 && r.committed_bytes() == 2 * one);
    r.ReadFinished(0, true);
    r.ReadFinished(1, true);
    p = r.Step(2);
    CHECK(p.uploads.size() == 2 && p.reads.empty() && p.evictions.empty() && r.state(2) == TextureState::kAbsent);  // full: the third waits, nothing is evicted for a worse priority
    r.RemoveSector(1);
    p = r.Step(3);
    CHECK(p.evictions == (std::vector<uint32_t>{0}) && p.reads.size() == 1 && p.reads[0].index == 2);  // the longest unreferenced goes (ties by index)
    CHECK(r.committed_bytes() == 2 * one);
    // a better wanted texture takes room from a worse resident one that is still referenced (the worst priority goes first)
    r.ReadFinished(2, true);
    r.Step(4);
    const uint16_t keeps_one[] = {1};
    r.AddSector(4, keeps_one, 1, 8.0f);  // texture 1 is wanted again (priority 8), texture 2 by the far sector (9)
    const uint16_t urgent[] = {5};
    r.AddSector(3, urgent, 1, 0.1f);
    p = r.Step(5);
    CHECK(p.reads.size() == 1 && p.reads[0].index == 5 && p.evictions == (std::vector<uint32_t>{2}) && r.state(1) == TextureState::kResident);
    // a texture bigger than the whole budget is never read, and does not block the others
    TextureResidency tiny(t);
    c.budget_bytes = one / 2;
    tiny.Configure(c);
    tiny.AddSector(1, near_set, 2, 1.0f);
    CHECK(tiny.Step(1).reads.empty());
  }
  {
    // a changed cap gives back what is held at another first level; a read that is in flight when it changes is dropped when it lands
    const std::vector<HitTexture> chained = {MakeRecord(512, 512, 5), MakeRecord(512, 512, 5)};
    TextureResidency r(chained);
    ResidencyConfig c;
    r.Configure(c);
    const uint16_t both[] = {0, 1};
    r.AddSector(1, both, 2, 1.0f);
    TextureResidency::Plan p = r.Step(1);
    CHECK(p.reads.size() == 2 && p.reads[0].first_mip == 0 && p.reads[0].bytes == chained[0].data_size);
    r.ReadFinished(0, true);
    p = r.Step(2);
    CHECK(p.uploads.size() == 1 && r.state(0) == TextureState::kResident);
    c.cap_px = 128;
    r.Configure(c);
    p = r.Step(3);
    CHECK(p.evictions == (std::vector<uint32_t>{0}));                          // resident at the full size: given back
    CHECK(p.reads.size() == 1 && p.reads[0].index == 0 && p.reads[0].first_mip == 2 && p.reads[0].bytes == chained[0].ChainBytes(2));  // and wanted again at the new cap
    CHECK(!r.ReadFinished(1, true) && r.state(1) == TextureState::kAbsent);  // the read for the old cap landed: dropped
    p = r.Step(4);
    CHECK(p.reads.size() == 1 && p.reads[0].index == 1 && p.reads[0].first_mip == 2);
    CHECK(r.ReadFinished(0, true) && r.ReadFinished(1, true));
    p = r.Step(5);
    const uint64_t accounted = AccountedBytes(chained[0], 2, 4096);
    CHECK(accounted == 12288 && chained[0].ChainBytes(2) == 10752);  // 8192 + 2048 + 512, each texture counted in whole 4 KiB
    CHECK(p.uploads.size() == 2 && r.resident_bytes() == 2 * accounted);
    // an upload that found no memory is tried again
    const uint32_t failed = p.uploads[0];
    r.UploadFailed(failed);
    CHECK(r.state(failed) == TextureState::kReady && r.resident_bytes() == accounted);
    p = r.Step(6);
    CHECK(p.uploads == (std::vector<uint32_t>{failed}) && r.resident_bytes() == 2 * accounted);
    // the device refused to make a texture: it is given back, counted, and tried again after the retry time
    r.CreationFailed(failed);
    CHECK(r.state(failed) == TextureState::kAbsent && r.failed_creations() == 1 && r.resident_bytes() == accounted && r.committed_bytes() == accounted);
    CHECK(r.Step(7).reads.empty());
    CHECK(r.Step(6 + c.retry_frames).reads.size() == 1);
  }
  {
    // a failed read is tried again after a while, and counted
    TextureResidency r(t);
    ResidencyConfig c;
    c.retry_frames = 100;
    r.Configure(c);
    const uint16_t one_tex[] = {3};
    r.AddSector(1, one_tex, 1, 1.0f);
    CHECK(r.Step(1).reads.size() == 1);
    CHECK(!r.ReadFinished(3, false) && r.failed_reads() == 1 && r.state(3) == TextureState::kAbsent && r.committed_bytes() == 0);
    CHECK(r.Step(50).reads.empty());
    CHECK(r.Step(101).reads.size() == 1);
    // a sector that leaves while its texture is read: the bytes are not uploaded
    r.ReadFinished(3, true);
    r.RemoveSector(1);
    const TextureResidency::Plan p = r.Step(102);
    CHECK(p.uploads.empty() && p.evictions == (std::vector<uint32_t>{3}) && r.state(3) == TextureState::kAbsent && r.committed_bytes() == 0);
  }
  {
    // the same inputs give the same plans
    const auto run = [&] {
      TextureResidency r(t);
      ResidencyConfig c;
      c.upload_bytes_per_frame = one;
      r.Configure(c);
      const uint16_t a[] = {5, 3, 1}, b[] = {1, 4};
      r.AddSector(7, a, 3, 2.0f);
      r.AddSector(8, b, 2, 2.0f);
      std::vector<uint32_t> order;
      for (uint64_t f = 1; f < 12; ++f) {
        const TextureResidency::Plan p = r.Step(f);
        for (const auto& rd : p.reads) {
          order.push_back(rd.index);
          r.ReadFinished(rd.index, true);
        }
        for (uint32_t u : p.uploads) order.push_back(100 + u);
      }
      return order;
    };
    CHECK(run() == run() && !run().empty());
  }
}

// ---------------------------------------------------------------------------
// the retail archive
// ---------------------------------------------------------------------------

#if defined(LARECOMP_TEXTURE_SET_TEST_REAL_LZX)
void TestRetailArchive(const fs::path& game, const fs::path& scratch) {
  const fs::path rpf = game / "xarchive_cache.rpf";
  if (!fs::exists(rpf)) {
    std::printf("no archive at %s: the retail checks are skipped\n", rpf.string().c_str());
    return;
  }
  TextureSetRequest request;
  request.source_rpf = rpf;
  request.lzx = [](const uint8_t* src, size_t n, std::vector<uint8_t>& dst, size_t dn) { return mm::LzxDecompress(src, n, dst, dn); };
  CountingSink sink;
  const TextureSetResult r = BuildTextureSet(request, &sink);
  CHECK(r.ok);
  if (!r.ok) {
    std::fprintf(stderr, "retail build failed: %s\n", r.error.c_str());
    return;
  }
  const TextureSetStats& s = r.stats;
  const bool retail = fs::file_size(rpf) == 2130739200ull;
  std::printf("retail texture set: %u sources (%u failed), %llu entries, %u distinct pictures, %u distinct names, %u name conflicts, %u cross conflicts, %.1f MB, %.1f s\n", s.sources, s.sources_failed,
              (unsigned long long)s.entries, s.distinct, s.distinct_names, s.name_conflicts, s.cross_conflicts, double(s.data_bytes) / 1048576.0, s.seconds);
  std::printf("  formats: BC1 %u, BC2 %u, BC3 %u, RGBA8 %u; single level %u; with alpha %u; gamma %u; damaged %u; decode failed %u\n", s.by_format[0], s.by_format[1], s.by_format[2], s.by_format[3], s.single_level,
              s.with_alpha, s.gamma, s.damaged, s.decode_failed);
  CHECK(s.sources_failed == 0 && s.sources == 441 && s.decode_failed == 0 && s.damaged == 0);
  CHECK(s.dictionary.bad_pointer == 0 && s.dictionary.not_supported == 0 && s.dictionary.unsupported_format == 0 && s.dictionary.no_base_level == 0 && s.dictionary.size_mismatch == 0);
  CHECK(s.dictionary.not_tiled == 0 && s.dictionary.odd_swizzle == 0 && s.dictionary.odd_endian == 0);  // every texture: tiled, 8in16, r g b a
  CHECK(s.by_format[size_t(HitTextureFormat::kBc2)] == 0 && s.by_format[size_t(HitTextureFormat::kRgba8)] == 0);
  CHECK(s.name_conflicts == 0);
  // the names of each dictionary (entries of one name twice in a dictionary are one name)
  std::map<uint32_t, uint32_t> names_of;
  for (const auto& e : r.set.dictionary_order) ++names_of[e.first];
  uint32_t sector_names = 0, empty_sectors = 0, max_sector = 0;
  for (uint32_t i = 0; i < r.set.sector_count; ++i) {
    const uint32_t n = names_of[DictionaryOfSector(i)];
    sector_names += n;
    if (n == 0) ++empty_sectors;
    max_sector = std::max(max_sector, n);
  }
  std::printf("  names: common %u, lod_0 %u, sectors %u (at most %u in one, %u empty)\n", names_of[kDictionaryCommon], names_of[kDictionaryLod], sector_names, max_sector, empty_sectors);
  CHECK(empty_sectors == 0);
  if (retail) {
    // the golden numbers of the retail archive (measured 2026-11-21): the dictionaries, the pictures, the keys
    CHECK(s.entries == 40984 && names_of[kDictionaryCommon] == 508 && names_of[kDictionaryLod] == 1240 && sector_names == 39236 && max_sector == 252);
    CHECK(s.distinct == 8342 && s.key_variants == 14946 && s.distinct_names == 8421 && s.cross_conflicts == 27);
    CHECK(s.by_format[size_t(HitTextureFormat::kBc1)] == 5192 && s.by_format[size_t(HitTextureFormat::kBc3)] == 3150 && s.single_level == 1291 && s.gamma == 6250 && s.with_alpha == 568);
    CHECK(s.data_bytes == 244060928ull && s.duplicate_bytes == 32908440ull);
  }

  // sizes: the whole set resident at each class's cap, against the budgets of M2 section 8 (200 to 300 MB is the owner's limit for the file)
  const uint64_t file_mb = r.set.file_bytes() >> 20;
  const ResidentTotals full = TotalResidentBytes(r.set.textures, 0), c512 = TotalResidentBytes(r.set.textures, 512), c256 = TotalResidentBytes(r.set.textures, 256),
                       c128 = TotalResidentBytes(r.set.textures, 128);
  std::printf("  file %llu MB; resident at full %.1f MB, cap 512 %.1f MB (%u capped), cap 256 %.1f MB (%u), cap 128 %.1f MB (%u)\n", (unsigned long long)file_mb, double(full.bytes) / 1048576.0,
              double(c512.bytes) / 1048576.0, c512.capped, double(c256.bytes) / 1048576.0, c256.capped, double(c128.bytes) / 1048576.0, c128.capped);
  CHECK(full.bytes <= r.set.data_bytes);  // the data section also holds the 16 byte alignment
  CHECK(file_mb <= 300 && file_mb >= 150);  // the owner's 200 to 300 MB (full detail)
  CHECK(c512.bytes < full.bytes && c256.bytes < c512.bytes && c128.bytes < c256.bytes);
  if (retail) CHECK(full.bytes == 244056960ull && c512.bytes == 227869568ull && c256.bytes == 163554176ull && c128.bytes == 92789632ull && c256.capped == 830 && c128.capped == 2965);
  CHECK((c128.bytes >> 20) <= 100 && (c256.bytes >> 20) <= 170);  // Mid and High hold the whole set well inside their video memory share (plan section 8: 10 to 17 and 30 to 45 MB were the old estimates)

  // The layout, guarded on the real data: the levels of a texture are filtered copies of the base, so a level's mean colour stays near the base's. common.xtd: 1,300 levels
  // (1.2 percent are off by more than 0.05 linear: the artists' own mips), including the packed tails; a wrong layout would scatter them.
  {
    uint32_t levels = 0, off = 0;
    for (uint32_t index : r.set.PicturesOf(kDictionaryCommon)) {
      const HitTexture& t = r.set.textures[index];
      if (t.mip_count < 2) continue;
      const bool gamma = (t.flags & kTextureGamma) != 0;
      const LevelMean base = MeanOfLevel(t.format, gamma, r.set.blobs[index].data(), t.width, t.height);
      for (uint32_t l = 1; l < t.mip_count; ++l) {
        const LevelMean m = MeanOfLevel(t.format, gamma, r.set.blobs[index].data() + t.MipOffset(l), std::max<uint32_t>(t.width >> l, 1), std::max<uint32_t>(t.height >> l, 1));
        double dev = 0;
        for (int k = 0; k < 3; ++k) dev = std::max(dev, std::fabs(double(m.rgb[k]) - double(base.rgb[k])));
        ++levels;
        if (dev > 0.05) ++off;
      }
    }
    std::printf("  common.xtd mip chains: %u levels, %u off by more than 0.05 in mean colour\n", levels, off);
    CHECK(levels > 1000 && off * 100 < levels * 3);
  }

  // the keys and the averages
  uint32_t zero_keys = 0, flat = 0;
  for (size_t i = 0; i < r.set.textures.size(); ++i) {
    if (r.set.textures[i].key == 0) ++zero_keys;
    for (int k = 0; k < 3; ++k) {
      if (!(r.set.average[i][size_t(k)] >= 0.0f && r.set.average[i][size_t(k)] <= 1.0f)) ++flat;
    }
  }
  CHECK(zero_keys == 0 && flat == 0);

  // CHECK C1 of the plan: the shaders of every sector name their textures (the shader's parameters of type texture hold a grcTexture whose name is the file name); every name must be a
  // picture of the set, as the sector's own dictionary or common.xtd (the retail files: 89,895 names over the sectors, all of them; the far level of detail's shaders name 1,238, 1,222 of
  // them in lod_0.xtd and 16 that no dictionary has).
  {
    mm::Rpf3Reader rpf_reader;
    CHECK(rpf_reader.Open(rpf));
    const auto load = [&](const std::string& path, RscImage* image) {
      mm::Rpf3Entry e;
      std::vector<uint8_t> file;
      std::string why;
      return rpf_reader.Find(path, e) && rpf_reader.ReadFile(e, file) &&
             DecodeResourceFile(file, [](const uint8_t* src, size_t n, std::vector<uint8_t>& dst, size_t dn) { return mm::LzxDecompress(src, n, dst, dn); }, image, &why);
    };
    // The names a shader group references: shader +16 the parameter pointers, +24 their count, +28 the types (0 = texture); a texture parameter points at a grcTexture whose +24 is the name.
    const auto names_of = [](RscCursor& c, uint32_t group) {
      std::set<uint32_t> names;
      const uint32_t array = c.U32(group + 8);
      const uint32_t count = c.U16(group + 12);
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t shader = c.U32(array + 4 * i), data = c.U32(shader + 16), parameters = c.U16(shader + 24), types = c.U32(shader + 28);
        for (uint32_t p = 0; p < parameters; ++p) {
          if (c.U8(types + p) != 0) continue;
          const uint32_t value = c.U32(data + 4 * p);
          if (!c.IsPointer(value)) continue;
          const std::string name = c.CString(c.U32(value + 24));
          if (!name.empty()) names.insert(TextureNameHash(name));
        }
      }
      return names;
    };
    RscImage table_image, lod_image;
    std::vector<SectorInfo> sector_list;
    std::string why;
    CHECK(load(kCityTablePath, &table_image) && ParseCityTable(table_image, &sector_list, &why));
    uint64_t pairs = 0, resolved = 0, own = 0;
    for (const SectorInfo& sector : sector_list) {
      RscImage image;
      if (!load(SectorArchivePath(sector.name), &image)) continue;
      RscCursor c(image);
      const uint32_t group = c.U32(c.U32(0x50000000 + 16) + 8);
      for (const uint32_t hash : names_of(c, group)) {
        ++pairs;
        const ResolvedTexture t = r.set.ResolveEntry(DictionaryOfSector(sector.index), hash);
        if (t.found()) {
          ++resolved;
          if (t.dictionary == DictionaryOfSector(sector.index)) ++own;
        }
      }
    }
    uint64_t lod_names = 0, lod_resolved = 0;
    if (load(kLodPath, &lod_image)) {
      RscCursor c(lod_image);
      for (const uint32_t hash : names_of(c, c.U32(0x50000000 + 8))) {
        ++lod_names;
        if (r.set.ResolveEntry(kDictionaryLod, hash).found()) ++lod_resolved;
      }
    }
    std::printf("  shader texture names: %llu (sector, name) pairs, %llu resolved (%llu in the sector's own dictionary); far lod %llu names, %llu resolved\n", (unsigned long long)pairs,
                (unsigned long long)resolved, (unsigned long long)own, (unsigned long long)lod_names, (unsigned long long)lod_resolved);
    CHECK(pairs > 0 && resolved * 1000 >= pairs * 999);  // all but, at most, one in a thousand
    if (retail) CHECK(pairs == 89895 && resolved == 89895 && own == 39235 && lod_names == 1238 && lod_resolved == 1222);
  }

  // the texture file of the whole set: written, validated, read at a cap
  const fs::path file = scratch / "retail" / kTexFileName;
  std::string error;
  CHECK(WriteTextureFile(file, r.set, 0xABCDEFull, &error));
  std::vector<std::string> problems;
  CHECK(ValidateTextureFile(file, r.set.textures, 0xABCDEFull, &problems) == 0);
  fs::remove_all(scratch / "retail");
}
#endif

}  // namespace

int main(int argc, char** argv) {
  const fs::path scratch = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "larecomp_texture_set_test";
  std::error_code ec;
  fs::remove_all(scratch, ec);
  TestHashAndGamma();
  TestFetchConstants();
  TestEndian();
  TestTilingAgainstModLoader();
  TestMipChain();
  TestPackedTail();
  TestBlockDecode();
  TestDecodeCityTexture();
  TestRaw8888();
  TestSetBuilder(scratch / "synthetic");
  TestResidency();
#if defined(LARECOMP_TEXTURE_SET_TEST_REAL_LZX)
  if (argc > 2) TestRetailArchive(argv[2], scratch);
#else
  (void)argc;
#endif
  fs::remove_all(scratch, ec);
  std::printf("texture set test: %d checks, %d failed\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
