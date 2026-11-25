// Check of the copy pass's conversion table and of the list of its pipelines that the host builder makes at start
// (resolve_variants.h; ResolveCache::ListPrecache). A program of its own: it needs no renderer, no ReXGlue and no GPU
// (only Plume's types header, for the format names), and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_resolve_variants_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_resolve_variants_test [<src/gpu dir>]
// or by hand: clang++ -std=c++23 -I src/gpu -I thirdparty/plume src/gpu/resolve_variants_test.cpp -o resolve_variants_test
//
// The bug it covers (starter verify round of 2026-11-17, ports\run\sgv-high-race-circuit and sgv-high-menus-night): the copy
// into a k_1_5_5_5 texture from a multisampled source ("resolve colour msaa -> B5G5R5A1_UNORM") was not in the host
// builder's hand-written list, so the render thread compiled it when the game view's first scene began. The list now
// comes from the same table that Execute converts with, so a destination the pass can make is a destination the start
// lists. The expected rows, formats and numbers are written out again here on purpose, as in gpu_class_test.cpp: a row
// added to or changed in the table fails this program until the test says so too.
//
// The program includes format.cpp, so that the one source file builds standalone (unit-test.ps1 compiles one file).
// With the src/gpu folder as its argument it also checks that the mode numbers are the shader's (hlsl/resolve_common.hlsli)
// and that resolve.cpp takes its list and its conversion from the header and keeps no table of its own.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "format.cpp"
#include "resolve_variants.h"

using namespace mcla::gpu;
using namespace mcla::gpu::resolve_variants;
using plume::RenderFormat;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

std::string Describe(const Variant& v) {
  return std::string(v.depth ? "depth" : "colour") + (v.msaa ? " msaa" : "") + " -> " +
         format::PlumeFormatName(v.format);
}

bool Has(const std::vector<Variant>& list, const Variant& v) {
  return std::find(list.begin(), list.end(), v) != list.end();
}

// ---- the table, written out again ---------------------------------------------------------------------------------

struct Row {
  uint32_t guest;       // xenos::TextureFormat
  const char* name;
  bool depth;           // from a depth source (else colour)
  Mode mode;
  bool dest_float24;
  RenderFormat host;    // the product's host format
};
constexpr Row kRows[] = {
    {6, "k_8_8_8_8", false, kModeUnorm8888, false, RenderFormat::R8G8B8A8_UNORM},
    {6, "k_8_8_8_8", true, kModeDepthRaw8888, false, RenderFormat::R8G8B8A8_UNORM},
    {3, "k_1_5_5_5", false, kModeUnorm1555, false, RenderFormat::B5G5R5A1_UNORM},
    {2, "k_8", false, kModeUnorm8, false, RenderFormat::R8_UNORM},
    {29, "k_16_16_16_16_EXPAND", false, kModeFloat, false, RenderFormat::R16G16B16A16_FLOAT},
    {32, "k_16_16_16_16_FLOAT", false, kModeFloat, false, RenderFormat::R16G16B16A16_FLOAT},
    {36, "k_32_FLOAT", false, kModeRed32, false, RenderFormat::R32_FLOAT},
    {36, "k_32_FLOAT", true, kModeDepthFloatBits, false, RenderFormat::R32_FLOAT},
    {37, "k_32_32_FLOAT", false, kModeFloat32, false, RenderFormat::R32G32_FLOAT},
    {22, "k_24_8", true, kModeDepthFloat, false, RenderFormat::R32_FLOAT},
    {23, "k_24_8_FLOAT", true, kModeDepthFloat, true, RenderFormat::R32_FLOAT},
};

// The distinct (source kind, host format) pairs of the rows: 6 colour and 2 depth.
constexpr Variant kExpectedPairs[] = {
    {false, false, RenderFormat::R8G8B8A8_UNORM},
    {false, false, RenderFormat::B5G5R5A1_UNORM},
    {false, false, RenderFormat::R8_UNORM},
    {false, false, RenderFormat::R16G16B16A16_FLOAT},
    {false, false, RenderFormat::R32_FLOAT},
    {false, false, RenderFormat::R32G32_FLOAT},
    {true, false, RenderFormat::R8G8B8A8_UNORM},
    {true, false, RenderFormat::R32_FLOAT},
};

void CheckConversionTable() {
  for (uint32_t guest = 0; guest < kGuestFormatCount; ++guest) {
    for (const bool depth : {false, true}) {
      const Row* row = nullptr;
      for (const Row& r : kRows) {
        if (r.guest == guest && r.depth == depth) {
          row = &r;
        }
      }
      const Conversion c = CopyConversion(guest, depth);
      const std::string label = "guest format " + std::to_string(guest) + (depth ? " from depth" : " from colour");
      if (row) {
        Check(c.mode == row->mode, label + " (" + row->name + "): mode " + std::to_string(c.mode) + ", want " +
                                       std::to_string(row->mode));
        Check(c.dest_float24 == row->dest_float24, label + ": the 24-bit float destination flag");
        const format::TextureHostFormat host = format::TextureHost(guest);
        Check(host.status == format::Status::kOk, label + ": has a host format");
        Check(host.format == row->host, label + ": host format " + format::PlumeFormatName(host.format) +
                                            ", want " + format::PlumeFormatName(row->host));
      } else {
        Check(c.mode == kModeCount && !c.dest_float24, label + ": no conversion");
      }
    }
  }
}

void CheckTableVariants() {
  const std::vector<Variant> table = TableVariants();
  // Each pair, once with and once without the multisampled source.
  Check(table.size() == 2 * std::size(kExpectedPairs),
        "the table gives " + std::to_string(table.size()) + " variants, want " +
            std::to_string(2 * std::size(kExpectedPairs)));
  for (const Variant& pair : kExpectedPairs) {
    for (const bool msaa : {true, false}) {
      const Variant v{pair.depth, msaa, pair.format};
      Check(Has(table, v), "the table has " + Describe(v));
    }
  }
  std::set<std::tuple<bool, bool, uint32_t>> seen;
  for (const Variant& v : table) {
    Check(seen.emplace(v.depth, v.msaa, uint32_t(v.format)).second, "the table repeats " + Describe(v));
  }
  // The table is read from the formats of Execute's copies and nothing else: a texture format that is not a
  // conversion's destination (the block compressed ones, the vertex formats' numbers) gives no variant.
  for (uint32_t guest : {18u, 19u, 20u, 0u, 1u, 63u}) {
    for (const bool depth : {false, true}) {
      Check(CopyConversion(guest, depth).mode == kModeCount,
            "guest format " + std::to_string(guest) + " is no destination");
    }
  }
}

void CheckListedCopies() {
  const std::vector<Variant> table = TableVariants();
  const std::vector<Variant> list = ListedCopies();

  // Nothing the table gives is left out, nothing the table does not give is in, nothing twice.
  for (const Variant& v : table) {
    Check(Has(list, v), "the list has " + Describe(v));
  }
  for (const Variant& v : list) {
    Check(Has(table, v), "the list has only what the table gives: " + Describe(v));
  }
  Check(list.size() == table.size(), "the list has " + std::to_string(list.size()) + " entries, the table " +
                                         std::to_string(table.size()));
  std::set<uint32_t> keys;
  for (const Variant& v : list) {
    const uint32_t key = PipelineKeyOf(v);
    Check(uint32_t(v.format) < 256, "the format of " + Describe(v) + " fits in the key's 8 bits");
    Check(keys.insert(key).second, "the key of " + Describe(v) + " is the key of another entry");
    Check(key == PipelineKeyOf(v.depth, v.msaa, v.format), "both forms of PipelineKeyOf agree for " + Describe(v));
  }

  // The destination the starter round found missing: the copy into k_1_5_5_5, from a multisampled source and from a plain one.
  const Variant bgr_msaa{false, true, RenderFormat::B5G5R5A1_UNORM};
  const Variant bgr_plain{false, false, RenderFormat::B5G5R5A1_UNORM};
  Check(Has(list, bgr_msaa), "the list has 'resolve colour msaa -> B5G5R5A1_UNORM'");
  Check(Has(list, bgr_plain), "the list has 'resolve colour -> B5G5R5A1_UNORM'");
  Check(Describe(bgr_msaa) == "colour msaa -> B5G5R5A1_UNORM", "the name the log shows: " + Describe(bgr_msaa));

  // The order the measured drives' logs asked for (resolve.cpp's list of before): the first six, then the logged k_1_5_5_5
  // before the rarer formats.
  const Variant first[] = {
      {false, true, RenderFormat::R8G8B8A8_UNORM},
      {true, true, RenderFormat::R32_FLOAT},
      {false, true, RenderFormat::R16G16B16A16_FLOAT},
      {false, false, RenderFormat::R8G8B8A8_UNORM},
      {false, false, RenderFormat::R16G16B16A16_FLOAT},
      {true, true, RenderFormat::R8G8B8A8_UNORM},
      bgr_msaa,
  };
  for (size_t i = 0; i < std::size(first); ++i) {
    Check(i < list.size() && list[i] == first[i], "list entry " + std::to_string(i) + " is " + Describe(first[i]));
  }
  // Every entry of the list of before is still in it.
  const Variant before[] = {
      {false, true, RenderFormat::R8G8B8A8_UNORM},   {true, true, RenderFormat::R32_FLOAT},
      {false, true, RenderFormat::R16G16B16A16_FLOAT}, {false, false, RenderFormat::R8G8B8A8_UNORM},
      {false, false, RenderFormat::R16G16B16A16_FLOAT}, {true, true, RenderFormat::R8G8B8A8_UNORM},
      {false, false, RenderFormat::R32_FLOAT},       {false, true, RenderFormat::R32_FLOAT},
      {true, false, RenderFormat::R32_FLOAT},        {true, false, RenderFormat::R8G8B8A8_UNORM},
      {false, false, RenderFormat::R32G32_FLOAT},    {false, true, RenderFormat::R32G32_FLOAT},
      {false, false, RenderFormat::R8_UNORM},        {false, true, RenderFormat::R8_UNORM},
  };
  for (const Variant& v : before) {
    Check(Has(list, v), "the list of before is kept: " + Describe(v));
  }
  Check(list.size() == std::size(before) + 2, "the list is the 14 of before and the two k_1_5_5_5 copies");

  // The names of all of them (the host precache's log line shows these).
  std::string names;
  for (const Variant& v : list) {
    names += (names.empty() ? "" : "; ") + Describe(v);
  }
  std::printf("listed copy pipelines (%zu): %s\n", list.size(), names.c_str());
}

void CheckTextureCopy() {
  constexpr Variant rgba8{false, false, kRgba8};
  Check(CanCopyRgba8(rgba8, kRgba8, 0, kModeUnorm8888, 1.0f, 0), "plain RGBA8 can copy");
  Check(!CanCopyRgba8({true, false, kRgba8}, kRgba8, 0, kModeUnorm8888, 1.0f, 0),
        "depth uses the shader");
  Check(!CanCopyRgba8({false, true, kRgba8}, kRgba8, 0, kModeUnorm8888, 1.0f, 0),
        "multisampling uses the shader");
  Check(!CanCopyRgba8(rgba8, kRgba16F, 0, kModeUnorm8888, 1.0f, 0),
        "float source uses the shader");
  Check(!CanCopyRgba8({false, false, kRgba16F}, kRgba8, 0, kModeUnorm8888, 1.0f, 0),
        "float destination uses the shader");
  Check(!CanCopyRgba8(rgba8, kRgba8, 1, kModeUnorm8888, 1.0f, 0),
        "higher mip uses the shader");
  Check(!CanCopyRgba8(rgba8, kRgba8, 0, kModeDepthRaw8888, 1.0f, 0),
        "raw depth conversion uses the shader");
  Check(!CanCopyRgba8(rgba8, kRgba8, 0, kModeUnorm8888 | 0x100u, 1.0f, 0),
        "conversion flags use the shader");
  Check(!CanCopyRgba8(rgba8, kRgba8, 0, kModeUnorm8888, 0.5f, 0),
        "exponent bias uses the shader");
  Check(!CanCopyRgba8(rgba8, kRgba8, 0, kModeUnorm8888, 1.0f, 1),
        "half-pixel fill uses the shader");

  plume::RenderBox box;
  Check(Rgba8CopyBox({10, 8, 30, 20}, -4, 6, 26, 26, box) && box.left == 6 &&
            box.top == 14 && box.right == 26 && box.bottom == 26 &&
            box.front == 0 && box.back == 1,
        "offset partial copy can reach both source edges");
  Check(Rgba8CopyBox({0, 0, 30, 20}, 0, 0, 30, 20, box) && box.left == 0 &&
            box.top == 0 && box.right == 30 && box.bottom == 20,
        "full-size copy includes the source origin");
  Check(!Rgba8CopyBox({10, 8, 30, 20}, -11, 0, 40, 40, box), "source x underflow");
  Check(!Rgba8CopyBox({10, 8, 30, 20}, 0, -9, 40, 40, box), "source y underflow");
  Check(!Rgba8CopyBox({10, 8, 30, 20}, 0, 0, 29, 40, box), "source x overflow");
  Check(!Rgba8CopyBox({10, 8, 30, 20}, 0, 0, 40, 19, box), "source y overflow");
  Check(!Rgba8CopyBox({10, 8, 10, 20}, 0, 0, 40, 40, box), "empty width");
  Check(!Rgba8CopyBox({10, 8, 30, 8}, 0, 0, 40, 40, box), "empty height");

  // Compare the shader's XeQuantizeUNorm to copying the stored byte.
  for (int byte = 0; byte < 256; ++byte) {
    const float loaded = float(byte) / 255.0f;
    const float quantized = std::floor(std::clamp(loaded, 0.0f, 1.0f) * 255.0f + 0.5f) / 255.0f;
    Check(std::lround(quantized * 255.0f) == byte, "UNORM round trip of " + std::to_string(byte));
  }
}

// ---- the sources ---------------------------------------------------------------------------------------------------

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

void CheckSources(const std::string& folder) {
  // The shader's numbers: #define kModeName <n>u.
  const std::string common = ReadFile(folder + "/hlsl/resolve_common.hlsli");
  Check(!common.empty(), "hlsl/resolve_common.hlsli is readable");
  const std::pair<const char*, Mode> modes[] = {
      {"kModeUnorm8888", kModeUnorm8888}, {"kModeUnorm1555", kModeUnorm1555}, {"kModeUnorm8", kModeUnorm8},
      {"kModeFloat", kModeFloat},         {"kModeRed32", kModeRed32},         {"kModeDepthFloat", kModeDepthFloat},
      {"kModeDepthRaw8888", kModeDepthRaw8888}, {"kModeDepthFloatBits", kModeDepthFloatBits},
      {"kModeFloat32", kModeFloat32},
  };
  for (const auto& [name, mode] : modes) {
    const std::string line = std::string("#define ") + name + " " + std::to_string(unsigned(mode)) + "u";
    Check(common.find(line) != std::string::npos, std::string("resolve_common.hlsli says '") + line + "'");
  }
  Check(unsigned(kModeCount) == std::size(modes), "the modes are the nine the shader has");

  // resolve.cpp keeps no table of its own: it lists what the header gives and converts with the header's table.
  const std::string cpp = ReadFile(folder + "/resolve.cpp");
  Check(!cpp.empty(), "resolve.cpp is readable");
  Check(cpp.find("resolve_variants::ListedCopies()") != std::string::npos, "resolve.cpp lists ListedCopies()");
  Check(cpp.find("resolve_variants::CopyConversion(") != std::string::npos, "resolve.cpp converts with CopyConversion");
  Check(cpp.find("kListedResolves") == std::string::npos, "resolve.cpp has no list of its own (kListedResolves)");
  Check(cpp.find("case 36: mode =") == std::string::npos, "resolve.cpp has no conversion switch of its own");
  const std::string header = ReadFile(folder + "/resolve.h");
  Check(header.find("using enum resolve_variants::Mode;") != std::string::npos, "resolve.h takes the modes from the header");
}

}  // namespace

int main(int argc, char** argv) {
  CheckConversionTable();
  CheckTableVariants();
  CheckListedCopies();
  CheckTextureCopy();
  if (argc > 1) {
    CheckSources(argv[1]);
  } else {
    std::printf("(no src/gpu folder given: the shader and source checks are skipped)\n");
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
