// Check of the guards for the structs the renderer hashes or compares as bytes (key_guard.h) and of the list of the places that do.
// A program of its own: it needs no renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_key_guard_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_key_guard_test [<src/gpu dir>]
// or by hand: clang++ -std=c++23 -I src/gpu src/gpu/key_guard_test.cpp -o key_guard_test
//
// The bug it covers (HEADLIGHT-FIX-2, 2026-11-20): the headlight shadow replay's pipeline key had four bytes of tail padding and was
// hashed and compared as raw bytes, so whatever the stack held there made the same draw a new key: 98 759 to 543 441 pipelines in a
// minute of a night street on some runs, 230 on others. The structs of the renderer's own caches (PipelineKey, TextureKey, EdramKey,
// FramebufferKey) were audited for the same fault the same day: none has it, and each now says so with a static_assert that fails the
// build when a member is added the wrong way. This program checks that the guards tell a clean key from one with padding or a member
// nobody sets (on made-up structs: the real ones are asserted where they are declared), that a clean key built over storage that held
// other bytes has the same bytes, and, given the src/gpu folder, that
//   - every place in src/gpu that hashes or compares a struct as bytes is one of the audited ones below (a new one fails here until
//     somebody audits it and adds it to the list: field by field is the better answer, a guard the other),
//   - the headers keep their guards and the pipeline cache file's record size is the one tools/pipelines/pipeline_list.py reads.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <new>
#include <regex>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "key_guard.h"

using namespace mcla::gpu::key_guard;

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

// ---- made-up keys ---------------------------------------------------------------------------------------------------------

// A key the way PipelineKey and TextureKey are: no padding, every member with an initializer.
struct CleanKey {
  uint64_t vs = 0;
  uint64_t ps = 0;
  uint32_t variant = 0;
  uint32_t layout = 0;
  uint32_t topology = 0;
  uint32_t samples = 1;
  uint32_t formats[4] = {};
  float slope = 0.0f;
  uint32_t reserved = 0;
  bool operator==(const CleanKey& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
};
constexpr size_t kCleanBytes = 2 * sizeof(uint64_t) + sizeof(uint32_t) * (4 + 4 + 1 + 1);

// The headlight replay's key as it was: 36 bytes of members (two 8-byte words, four words, two bytes and two named bytes of pad) in 40:
// four bytes of tail padding the compiler keeps to line the next one up.
struct TailPaddedKey {
  uint64_t vs = 0;
  uint64_t ps = 0;
  uint32_t vs_variant = 0;
  uint32_t ps_variant = 0;
  uint32_t layout = 0;
  uint32_t topology = 0;
  uint8_t cull = 0;
  uint8_t front_ccw = 0;
  uint8_t pad[2] = {};
  bool operator==(const TailPaddedKey& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
};
constexpr size_t kTailPaddedMemberBytes = 2 * sizeof(uint64_t) + 4 * sizeof(uint32_t) + 4;

// A padding hole in the middle: a 4-byte word before an 8-byte one.
struct HoleKey {
  uint32_t a = 0;
  uint64_t b = 0;
};

// A member nobody sets.
struct UnsetMemberKey {
  uint32_t a = 0;
  uint32_t b;
  uint8_t c[3] = {};
};

// An array nobody sets, among members that are.
struct UnsetArrayKey {
  uint32_t a = 0;
  uint32_t formats[4];
  uint32_t z = 0;
};

// A key of integers only, as TextureKey: the standard's trait covers it.
struct IntegerKey {
  uint32_t base_page = 0;
  uint32_t mip_page = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t depth = 0;
  uint16_t pitch = 0;
  uint8_t format = 0;
  uint8_t endian = 0;
  uint8_t levels = 0;
  uint8_t dimension = 0;
  uint8_t tiled = 0;
  uint8_t packed = 0;
  uint8_t stacked = 0;
  uint8_t reserved = 0;
};

// Pointers and counters, as FramebufferKey.
struct PointerKey {
  const void* targets[5] = {};
  uint32_t generations[5] = {};
  uint32_t color_count = 0;
};

template <class T>
concept AllSet = requires { typename std::bool_constant<AllMembersSet<T>()>; };

// ---- the guards ---------------------------------------------------------------------------------------------------------

void CheckPaddingGuard() {
  Check(sizeof(CleanKey) == kCleanBytes, "the clean key is as big as its members");
  Check(SizeIsMemberSum<CleanKey>(kCleanBytes), "SizeIsMemberSum: a clean key with a float passes");
  Check(!kNoPadding<CleanKey>, "kNoPadding says no for a key with a float (the trait is false for floats: use SizeIsMemberSum)");

  Check(sizeof(TailPaddedKey) == 40 && kTailPaddedMemberBytes == 36, "the old headlight key: 36 bytes of members in 40");
  Check(!SizeIsMemberSum<TailPaddedKey>(kTailPaddedMemberBytes), "SizeIsMemberSum: the tail padding of the old headlight key is found");
  Check(!kNoPadding<TailPaddedKey>, "kNoPadding: the tail padding of the old headlight key is found");

  Check(!kNoPadding<HoleKey>, "kNoPadding: a hole before an 8-byte member is found");
  Check(!SizeIsMemberSum<HoleKey>(sizeof(uint32_t) + sizeof(uint64_t)), "SizeIsMemberSum: a hole before an 8-byte member is found");

  Check(kNoPadding<IntegerKey> && sizeof(IntegerKey) == 24, "kNoPadding: an integer key as TextureKey passes (24 bytes)");
  Check(kNoPadding<PointerKey> && sizeof(PointerKey) == 64, "kNoPadding: pointers and counters as FramebufferKey pass (64 bytes)");
  Check(kNoPadding<uint64_t> && kNoPadding<uint8_t[8]>, "kNoPadding: plain integers pass");

  // A key that is not trivially copyable (a std::string member) is never a byte key.
  Check(!kNoPadding<std::string> && !SizeIsMemberSum<std::string>(sizeof(std::string)), "a type with a destructor is not a byte key");
}

void CheckInitializerGuard() {
  Check(AllSet<CleanKey>, "AllMembersSet: the clean key passes");
  Check(AllSet<TailPaddedKey>, "AllMembersSet: the old headlight key had every member set (its fault was the padding)");
  Check(AllSet<IntegerKey> && AllSet<PointerKey>, "AllMembersSet: the integer and pointer keys pass");
  Check(!AllSet<UnsetMemberKey>, "AllMembersSet: a member with no initializer is found");
  Check(!AllSet<UnsetArrayKey>, "AllMembersSet: an array with no initializer is found");
}

void CheckCanonicalZero() {
  const auto bits = [](float v) {
    uint32_t b;
    std::memcpy(&b, &v, 4);
    return b;
  };
  Check(bits(-0.0f) == 0x80000000u && bits(0.0f) == 0u, "the two zeros are different bytes");
  Check(bits(CanonicalZero(-0.0f)) == 0u && bits(CanonicalZero(0.0f)) == 0u, "CanonicalZero: both zeros come out as +0.0");
  Check(CanonicalZero(1.5f) == 1.5f && CanonicalZero(-2.25f) == -2.25f, "CanonicalZero: other values stay");
  Check(bits(CanonicalZero(1.5f)) == bits(1.5f), "CanonicalZero: other values keep their bits");
  Check(std::isnan(CanonicalZero(std::nanf(""))), "CanonicalZero: a NaN stays a NaN (no key holds one)");
  // The slope a draw gives: a zero scale or a negative zero one times the unit, both as the same key.
  CleanKey a, b;
  a.slope = CanonicalZero(-0.0f * 0.5f);
  b.slope = CanonicalZero(0.0f * 0.5f);
  Check(a == b, "keys built from a +0.0 and a -0.0 slope are the same key once the slope is canonical");
  CleanKey c, d;
  c.slope = -0.0f;
  d.slope = 0.0f;
  Check(!(c == d), "(and without CanonicalZero they would be two keys: the byte compare sees the sign)");
}

// ---- a key over storage that held other bytes -------------------------------------------------------------------------------

// The bytes of a T default-constructed in storage that was first filled with `fill`.
template <class T>
std::vector<uint8_t> ImageOver(uint8_t fill) {
  alignas(T) unsigned char storage[sizeof(T)];
  std::memset(storage, fill, sizeof(storage));
  T* made = new (storage) T;
  std::vector<uint8_t> image(sizeof(T));
  std::memcpy(image.data(), made, sizeof(T));
  made->~T();
  return image;
}

uint64_t Fnv(const std::vector<uint8_t>& bytes) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (uint8_t b : bytes) {
    h = (h ^ b) * 0x100000001B3ull;
  }
  return h;
}

// What a stack that held old values does to a key: the bytes of the padding take whatever was there.
template <class T>
void Scribble(T* key, size_t from, size_t to, uint8_t value) {
  std::memset(reinterpret_cast<unsigned char*>(key) + from, value, to - from);
}

void CheckOverOtherBytes() {
  // A clean key made over storage that held 00, AA, 55 or FF is the same bytes and so the same hash.
  const std::vector<uint8_t> zeros = ImageOver<CleanKey>(0x00);
  bool same = true;
  for (uint8_t fill : {uint8_t(0xAA), uint8_t(0x55), uint8_t(0xFF)}) {
    same &= ImageOver<CleanKey>(fill) == zeros && Fnv(ImageOver<CleanKey>(fill)) == Fnv(zeros);
  }
  Check(same, "a clean key made over other bytes is the same bytes (and the same hash)");
  bool integer_same = true;
  for (uint8_t fill : {uint8_t(0xAA), uint8_t(0x55), uint8_t(0xFF)}) {
    integer_same &= ImageOver<IntegerKey>(fill) == ImageOver<IntegerKey>(0x00);
    integer_same &= ImageOver<PointerKey>(fill) == ImageOver<PointerKey>(0x00);
  }
  Check(integer_same, "the integer and pointer keys made over other bytes are the same bytes");

  // The old headlight key: the fields are the same, the tail takes what the stack held, and the byte compare and the byte hash say two
  // keys. 40 shapes of draw, 500 draws each, over four stack contents: 40 keys when the compare is right, up to 160 when it is not.
  std::map<uint64_t, int> by_bytes;  // hash of the bytes -> keys
  std::map<std::pair<uint64_t, uint64_t>, int> by_fields;
  const uint8_t stacks[4] = {0x00, 0xAA, 0x55, 0xFF};
  for (int shape = 0; shape < 40; ++shape) {
    for (int draw = 0; draw < 500; ++draw) {
      TailPaddedKey key;
      key.vs = 0x1000 + uint64_t(shape);
      key.ps = shape % 2 ? 0x2000u + uint64_t(shape) : 0u;
      key.layout = uint32_t(shape / 4);
      key.topology = 4;
      key.cull = uint8_t(shape % 3);
      key.front_ccw = uint8_t(shape % 2);
      Scribble(&key, kTailPaddedMemberBytes, sizeof(key), stacks[(shape + draw) % 4]);  // the bytes after the last member
      std::vector<uint8_t> image(sizeof(key));
      std::memcpy(image.data(), &key, sizeof(key));
      ++by_bytes[Fnv(image)];
      ++by_fields[{key.vs, (uint64_t(key.layout) << 16) | (uint64_t(key.cull) << 8) | key.front_ccw}];
    }
  }
  Check(by_fields.size() == 40, "the old headlight key, read field by field: 40 shapes of draw are 40 keys");
  Check(by_bytes.size() > 40, "the old headlight key, read as bytes: the stack's contents make the same 40 shapes more keys (the bug)");
}

// ---- where the renderer reads a struct as bytes ----------------------------------------------------------------------------

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

bool Has(const std::string& text, const std::string& what) {
  return text.find(what) != std::string::npos;
}

// A way of reading a struct as bytes, and the lines of src/gpu (not tests, not comments) that match it.
struct Way {
  const char* name;
  const char* pattern;
};
constexpr Way kWays[] = {
    {"memcmp(this", R"(memcmp\(\s*this\b)"},
    {"memcmp(&", R"(memcmp\(\s*&)"},
    {"XXH3(&", R"(XXH3_64bits\w*\(\s*&)"},
    {"bytes of &", R"(reinterpret_cast<const (uint8_t|uint64_t|uint32_t|char|void)\s*\*>\(\s*(&|this\b))"},
};

// What was audited on 2026-11-20 (HEADLIGHT-FIX-2's finding applied to origin/main 5acdc2b0).
struct Audited {
  const char* file;
  const char* way;
  int count;
  const char* why;
};
constexpr Audited kAudited[] = {
    {"pipelines.h", "memcmp(this", 1, "PipelineKey::operator==: 104 bytes, no padding (SizeIsMemberSum), every member set, the slope canonical"},
    {"textures.cpp", "memcmp(this", 1, "TextureKey::operator==: 24 bytes, no padding (kNoPadding), every member set"},
    {"pipelines.cpp", "XXH3(&", 1, "PipelineKeyHash: the same key"},
    {"textures.cpp", "XXH3(&", 1, "TextureKeyHash: the same key"},
    {"edram.cpp", "XXH3(&", 1, "FramebufferKeyHash: 104 bytes (eight pointers, five generations, the counts), no padding, every member set"},
    {"scene.cpp", "memcmp(&", 2,
     "the last uploaded SharedConstants (the plain draws' and the variant draws' kept copies): members of 4 bytes only, so no padding is possible; a difference costs one upload "
     "(the variant blocks are compared as arrays of bytes: memcmp(block.data(), ...), not as structs)"},
    {"local_shadow.cpp", "memcmp(&", 1,
     "the headlight shadow replay's collected SharedConstants (consecutive draws repeat the block): members of 4 bytes only, so no padding is possible; a difference costs one more copy"},
    {"car_rt_scene_rules.h", "memcmp(&", 1, "a float compared bit for bit on purpose (the check reads back what the shader read)"},
    {"light_manager_rules.h", "memcmp(&", 4,
     "the passive light manager: three lines compare single floats bit for bit on purpose (a volume lamp's shape), one compares the replay's published frame with the last one as bytes "
     "(ShadowCarry::Differs): a padding byte or a member left unset can only make it say 'differs', which costs one refresh of the carry and a line in the census; nothing reads the manager"},
    {"pipelines.cpp", "bytes of &", 4, "the cache file's writer: a uint32, the 104-byte key record, the layout hash"},
};

void CheckSources(const std::filesystem::path& gpu) {
  std::map<std::pair<std::string, std::string>, int> found;  // (file, way) -> lines
  std::map<std::pair<std::string, std::string>, std::string> first;
  std::vector<std::regex> patterns;
  for (const Way& way : kWays) {
    patterns.emplace_back(way.pattern);
  }
  for (const auto& entry : std::filesystem::directory_iterator(gpu)) {
    const std::string name = entry.path().filename().string();
    const std::string ext = entry.path().extension().string();
    if ((ext != ".h" && ext != ".cpp") || name.find("_test.") != std::string::npos || name == "key_guard.h") {
      continue;
    }
    std::istringstream lines(ReadFile(entry.path()));
    std::string line;
    int number = 0;
    while (std::getline(lines, line)) {
      ++number;
      const size_t at = line.find_first_not_of(" \t");
      if (at == std::string::npos || line.compare(at, 2, "//") == 0) {
        continue;
      }
      for (size_t w = 0; w < std::size(kWays); ++w) {
        if (std::regex_search(line, patterns[w])) {
          ++found[{name, kWays[w].name}];
          if (!first.count({name, kWays[w].name})) {
            first[{name, kWays[w].name}] = name + ":" + std::to_string(number);
          }
        }
      }
    }
  }
  for (const auto& [site, count] : found) {
    int expected = 0;
    for (const Audited& a : kAudited) {
      if (site.first == a.file && site.second == a.way) {
        expected = a.count;
      }
    }
    Check(count == expected,
          "src/gpu/" + site.first + " has " + std::to_string(count) + " line(s) of `" + site.second + "` (audited: " + std::to_string(expected) +
              ", first at " + first[site] + "): a struct read as bytes must have no padding and every member set (key_guard.h: add the guard "
              "next to the struct, or compare and hash it field by field) and be added to kAudited here");
  }
  for (const Audited& a : kAudited) {
    Check(found.count({a.file, a.way}) != 0, std::string("audited site gone: src/gpu/") + a.file + " `" + a.way + "` (" + a.why + "): update kAudited");
  }
}

void CheckGuardsStay(const std::filesystem::path& gpu) {
  const std::string pipelines_h = ReadFile(gpu / "pipelines.h");
  const std::string pipelines_cpp = ReadFile(gpu / "pipelines.cpp");
  const std::string textures_h = ReadFile(gpu / "textures.h");
  const std::string edram_h = ReadFile(gpu / "edram.h");
  const std::string edram_cpp = ReadFile(gpu / "edram.cpp");
  Check(Has(pipelines_h, "key_guard::SizeIsMemberSum<PipelineKey>(") && Has(pipelines_h, "key_guard::AllMembersSet<PipelineKey>()"),
        "pipelines.h keeps the guards of PipelineKey");
  Check(Has(textures_h, "key_guard::kNoPadding<TextureKey>") && Has(textures_h, "key_guard::AllMembersSet<TextureKey>()"),
        "textures.h keeps the guards of TextureKey");
  Check(Has(edram_h, "key_guard::kNoPadding<EdramKey>") && Has(edram_h, "key_guard::AllMembersSet<EdramKey>()"),
        "edram.h keeps the guards of EdramKey");
  Check(Has(edram_cpp, "key_guard::kNoPadding<FramebufferKey>") && Has(edram_cpp, "key_guard::AllMembersSet<FramebufferKey>()"),
        "edram.cpp keeps the guards of FramebufferKey (beside its byte hash)");
  Check(Has(pipelines_cpp, "key_guard::CanonicalZero(bias.slope)") && Has(pipelines_cpp, "key_guard::CanonicalZero(r.key.slope_scaled_depth_bias)"),
        "pipelines.cpp keeps the slope canonical, in BuildKey and in the records it loads");

  // The cache file's record is the key as bytes, and the Python tool that merges the lists reads it by size.
  std::smatch size;
  const bool have_size = std::regex_search(pipelines_h, size, std::regex(R"(static_assert\(sizeof\(PipelineKey\) == (\d+),)"));
  Check(have_size, "pipelines.h states the PipelineKey record size");
  const std::filesystem::path tool = gpu / ".." / ".." / "tools" / "pipelines" / "pipeline_list.py";
  if (std::filesystem::exists(tool) && have_size) {
    std::smatch tool_size;
    const std::string py = ReadFile(tool);
    const bool have_tool = std::regex_search(py, tool_size, std::regex(R"(KEY_SIZE\s*=\s*(\d+))"));
    Check(have_tool && tool_size[1] == size[1],
          "tools/pipelines/pipeline_list.py reads the record size pipelines.h writes (KEY_SIZE = " + (have_tool ? tool_size[1].str() : "?") + ", header " +
              size[1].str() + ")");
  } else {
    std::printf("(tools/pipelines/pipeline_list.py not found next to the src folder: the record size check is skipped)\n");
  }
}

}  // namespace

int main(int argc, char** argv) {
  CheckPaddingGuard();
  CheckInitializerGuard();
  CheckCanonicalZero();
  CheckOverOtherBytes();
  if (argc > 1) {
    CheckSources(argv[1]);
    CheckGuardsStay(argv[1]);
  } else {
    std::printf("(no src/gpu folder given: the source checks are skipped)\n");
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
