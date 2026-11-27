// Check of the compiled-in texture rows (gbuffer_texture_rows.h, PBR level 2, package PBR-2): the sorted arrays and their binary search, the order of the keys (hash, dictionary/name, name),
// the layers (a higher layer's row beats a lower one's, whole), the setting's layers, the fingerprint the city hit data's stamp carries, the shipped data (sorted, sound rows), and, with the
// sample the Python tools make, that the emitter's keys and rows are the ones the renderer's parser and its FNV give. A program of its own: it needs no renderer, no ReXGlue and no GPU, and exits
// 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_gbuffer_texture_rows_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_gbuffer_texture_rows_test [<tools/gbuffer_materials/sample_table.txt>]
//
// With the sample table of tools/gbuffer_materials (the file the Python tools' tests read, and tools/gbuffer_materials/sample_rows.inc, which emit_rows.py makes from it) it checks that every row of
// the sample is found in the compiled-in sample with the same fields, by the same key, and that nothing else is in it.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "gbuffer_texture_rows.h"

namespace gb = mcla::gpu::gbuffer;

// The sample emit_rows.py made from tools/gbuffer_materials/sample_table.txt (+ sample_overrides.txt), in a namespace of its own: the same source text the real table is.
namespace sample {
using namespace mcla::gpu::gbuffer;
#include "../../tools/gbuffer_materials/sample_rows.inc"
}  // namespace sample

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

// A table made at run time, with its sentinel, sorted by key.
struct Built {
  std::vector<gb::CompiledRow> hash{{}};
  std::vector<gb::CompiledRow> pair{{}};
  std::vector<gb::CompiledRow> name{{}};
  void Sort() {
    for (std::vector<gb::CompiledRow>* v : {&hash, &pair, &name}) {
      std::sort(v->begin() + 1, v->end(), [](const gb::CompiledRow& a, const gb::CompiledRow& b) { return a.key < b.key; });
    }
  }
  gb::CompiledView View() const {
    return {std::span<const gb::CompiledRow>(hash).subspan(1), std::span<const gb::CompiledRow>(pair).subspan(1), std::span<const gb::CompiledRow>(name).subspan(1)};
  }
};

gb::TextureRow ClassRow(gb::SurfaceClass cls) {
  gb::TextureRow row;
  row.fields = gb::kRowClass;
  row.cls = uint8_t(cls);
  return row;
}
int ClassOf(const gb::TextureRow* row) { return row == nullptr ? -1 : int(row->cls); }

// ---------------------------------------------------------------------------
// The shipped data
// ---------------------------------------------------------------------------
void ShippedChecks() {
  Check(gb::CompiledRowsSorted(gb::kCompiledHashRows) && gb::CompiledRowsSorted(gb::kCompiledPairRows) && gb::CompiledRowsSorted(gb::kCompiledNameRows), "the shipped arrays are sorted, with their sentinels");
  Check(gb::kCompiledRowCount == gb::kCompiledHashRowSpan.size() + gb::kCompiledPairRowSpan.size() + gb::kCompiledNameRowSpan.size(), "the row count is the three arrays'");
  Check(gb::kCompiledHasHashRows == !gb::kCompiledHashRowSpan.empty(), "the hash flag follows the hash array");
  Check(gb::kCompiledRowsFormat == 1, "the data's format is 1");
  bool sound = true;
  for (const std::span<const gb::CompiledRow> table : {gb::kCompiledHashRowSpan, gb::kCompiledPairRowSpan, gb::kCompiledNameRowSpan}) {
    for (const gb::CompiledRow& r : table) {
      const gb::TextureRow& row = r.row;
      sound = sound && r.key != 0 && row.fields != 0 && (row.fields & ~(gb::kRowClass | gb::kRowSmooth | gb::kRowMetal | gb::kRowSsr | gb::kRowLit)) == 0;
      // a field the row does not give holds the default, so that a row is the same whether it was parsed or emitted
      if ((row.fields & gb::kRowClass) == 0) sound = sound && row.cls == 0;
      if ((row.fields & gb::kRowClass) != 0) sound = sound && row.cls < uint8_t(gb::SurfaceClass::kCount);
      if ((row.fields & gb::kRowSmooth) == 0) sound = sound && row.smooth == 0;
      if ((row.fields & gb::kRowMetal) == 0) sound = sound && row.metal == 0;
      if ((row.fields & gb::kRowLit) == 0) sound = sound && row.lit == 255;
      if ((row.fields & gb::kRowSsr) == 0) sound = sound && !row.ssr;
    }
  }
  Check(sound, "every shipped row has a key, gives at least one field, a class that exists, and the defaults in the fields it does not give");
  Check(gb::kCompiledRowsStamp == gb::CompiledRowsFingerprint(gb::kCompiledView), "the stamp is the shipped table's fingerprint");
}

// ---------------------------------------------------------------------------
// Sortedness
// ---------------------------------------------------------------------------
void SortedChecks() {
  using R = gb::CompiledRow;
  const std::array<R, 1> only_sentinel = {R{}};
  Check(gb::CompiledRowsSorted(only_sentinel), "a table of the sentinel alone is sound (an empty table)");
  const std::array<R, 3> good = {R{}, R{5, {}}, R{9, {}}};
  Check(gb::CompiledRowsSorted(good), "strictly increasing keys after a sentinel");
  const std::array<R, 3> unsorted = {R{}, R{9, {}}, R{5, {}}};
  Check(!gb::CompiledRowsSorted(unsorted), "a table out of order is refused");
  const std::array<R, 3> repeated = {R{}, R{5, {}}, R{5, {}}};
  Check(!gb::CompiledRowsSorted(repeated), "a repeated key is refused");
  const std::array<R, 2> no_sentinel = {R{5, {}}, R{9, {}}};
  Check(!gb::CompiledRowsSorted(no_sentinel), "a table without its sentinel is refused");
  const std::array<R, 3> zero_key = {R{}, R{}, R{9, {}}};
  Check(!gb::CompiledRowsSorted(zero_key), "a real row with key 0 is refused");
  Check(!gb::CompiledRowsSorted(std::span<const R>()), "an empty array is refused (no sentinel)");
}

// ---------------------------------------------------------------------------
// The lookups
// ---------------------------------------------------------------------------
void LookupChecks() {
  Built t;
  t.hash.push_back({0x1111, ClassRow(gb::SurfaceClass::kGlass)});
  t.pair.push_back({gb::PairKey("shared", "tex"), ClassRow(gb::SurfaceClass::kMetal)});
  t.name.push_back({gb::NameKey("tex"), ClassRow(gb::SurfaceClass::kChrome)});
  t.name.push_back({gb::NameKey("only_name"), ClassRow(gb::SurfaceClass::kPaint)});
  t.Sort();
  const gb::CompiledView v = t.View();
  Check(ClassOf(gb::FindCompiledRow(v, "shared", "tex", 0x1111)) == int(gb::SurfaceClass::kGlass), "the content hash is the most specific key");
  Check(ClassOf(gb::FindCompiledRow(v, "shared", "tex", 0x2222)) == int(gb::SurfaceClass::kMetal), "a hash with no row falls to dictionary/name");
  Check(ClassOf(gb::FindCompiledRow(v, "shared", "tex", 0)) == int(gb::SurfaceClass::kMetal), "hash 0 (not known yet) falls to dictionary/name");
  Check(ClassOf(gb::FindCompiledRow(v, "other", "tex", 0)) == int(gb::SurfaceClass::kChrome), "another dictionary falls to the bare name");
  Check(ClassOf(gb::FindCompiledRow(v, "", "tex", 0)) == int(gb::SurfaceClass::kChrome), "no dictionary falls to the bare name");
  Check(ClassOf(gb::FindCompiledRow(v, "x", "only_name", 0)) == int(gb::SurfaceClass::kPaint) && ClassOf(gb::FindCompiledRow(v, "", "only_name", 0)) == int(gb::SurfaceClass::kPaint),
        "a bare name row, with a dictionary or without");
  Check(ClassOf(gb::FindCompiledRow(v, "SHARED", "TEX", 0)) == int(gb::SurfaceClass::kMetal), "the keys ignore the case");
  Check(ClassOf(gb::FindCompiledRow(v, "shared", "unnamed", 0)) == -1 && ClassOf(gb::FindCompiledRow(v, "shared", "", 0)) == -1, "an unnamed texture is not found by name");
  Check(ClassOf(gb::FindCompiledRow(v, "shared", "unnamed", 0x1111)) == int(gb::SurfaceClass::kGlass), "an unnamed texture is found by its hash");
  Check(ClassOf(gb::FindCompiledRow(v, "shared", "nothing", 0x7777)) == -1, "no row");
  Check(ClassOf(gb::FindCompiledRow(gb::CompiledView{}, "shared", "tex", 0x1111)) == -1, "an empty view finds nothing");
  Check(gb::FindCompiledRow("shared", "tex", 0x1111) == nullptr || gb::kCompiledRowCount != 0, "the shipped table is the default view");

  // a big table: every key is found, every other number is not (boundaries and the middle of the binary search)
  Built big;
  uint64_t state = 0x9E3779B97F4A7C15ull;
  std::vector<uint64_t> keys;
  for (int i = 0; i < 3000; ++i) {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    const uint64_t key = (state >> 1) | 1ull;  // odd, so that key + 1 is a certain miss
    if (std::find(keys.begin(), keys.end(), key) != keys.end()) continue;
    keys.push_back(key);
    gb::TextureRow row;
    row.fields = gb::kRowSmooth;
    row.smooth = uint8_t(i & 255);
    big.hash.push_back({key, row});
  }
  big.Sort();
  const gb::CompiledView bv = big.View();
  bool all_found = true;
  bool none_wrong = true;
  for (const uint64_t key : keys) {
    const gb::TextureRow* row = gb::SearchCompiledRows(bv.hash, key);
    all_found = all_found && row != nullptr;
    none_wrong = none_wrong && gb::SearchCompiledRows(bv.hash, key + 1) == nullptr && gb::SearchCompiledRows(bv.hash, key - 1) == nullptr;
  }
  Check(all_found, "every key of a 3000 row table is found");
  Check(none_wrong, "the numbers next to every key are not");
  Check(gb::SearchCompiledRows(bv.hash, bv.hash.front().key) != nullptr && gb::SearchCompiledRows(bv.hash, bv.hash.back().key) != nullptr, "the first and the last row are found");
  Check(gb::SearchCompiledRows(bv.hash, 0) == nullptr && gb::SearchCompiledRows(bv.hash, ~0ull) == nullptr, "key 0 and a key past the end are not");
  Check(gb::SearchCompiledRows({}, 5) == nullptr, "an empty table finds nothing");
  // the row that comes back is the row of that key, not a neighbour's
  bool exact = true;
  for (const gb::CompiledRow& r : bv.hash) exact = exact && gb::SearchCompiledRows(bv.hash, r.key) == &r.row;
  Check(exact, "the row found is the key's own");
}

// ---------------------------------------------------------------------------
// The layers
// ---------------------------------------------------------------------------
void LayerChecks() {
  Check(gb::RowLayersOfSetting(0) == 0 && gb::RowLayersOfSetting(1) == gb::kRowLayersAll && gb::RowLayersOfSetting(2) == gb::kRowLayerCompiled && gb::RowLayersOfSetting(3) == gb::kRowLayerFiles,
        "the setting's values: 0 none, 1 both, 2 the compiled-in table, 3 the files");
  Check(gb::RowLayersOfSetting(-1) == gb::kRowLayersAll && gb::RowLayersOfSetting(9) == gb::kRowLayersAll, "a value out of range reads both");

  Built t;
  t.hash.push_back({0xAA, ClassRow(gb::SurfaceClass::kGlass)});
  t.name.push_back({gb::NameKey("trim"), ClassRow(gb::SurfaceClass::kChrome)});
  t.Sort();
  const gb::CompiledView v = t.View();
  gb::TextureTable files;
  {
    gb::TableParse report;
    gb::ParseTextureTable("@version 1\n*/trim class=paint\nhash:BB class=metal\n", &files, &report);
    Check(report.errors.empty() && files.size() == 2, "the files' table parses");
  }
  const auto cls = [&](uint32_t layers, const char* dictionary, const char* name, uint64_t hash) { return ClassOf(gb::FindTextureRow(v, files, layers, dictionary, name, hash)); };
  Check(cls(gb::kRowLayersAll, "d", "trim", 0) == int(gb::SurfaceClass::kPaint), "a file's row beats the compiled-in row of the same texture");
  Check(cls(gb::kRowLayerCompiled, "d", "trim", 0) == int(gb::SurfaceClass::kChrome), "the compiled-in layer alone: its own row");
  Check(cls(gb::kRowLayerFiles, "d", "trim", 0) == int(gb::SurfaceClass::kPaint), "the files alone: their own row");
  Check(cls(0, "d", "trim", 0) == -1, "no layer: no row");
  Check(cls(gb::kRowLayersAll, "d", "unnamed", 0xAA) == int(gb::SurfaceClass::kGlass), "a texture only the compiled-in table knows: its row");
  Check(cls(gb::kRowLayersAll, "d", "unnamed", 0xBB) == int(gb::SurfaceClass::kMetal), "a texture only the files know: their row");
  Check(cls(gb::kRowLayerFiles, "d", "unnamed", 0xAA) == -1 && cls(gb::kRowLayerCompiled, "d", "unnamed", 0xBB) == -1, "a layer that is off is not read");
  // a file's NAME row beats a compiled-in HASH row of the same texture: the layer decides before the key does
  Built mixed;
  mixed.hash.push_back({0xCC, ClassRow(gb::SurfaceClass::kGlass)});
  mixed.Sort();
  gb::TextureTable named;
  gb::TableParse report;
  gb::ParseTextureTable("@version 1\n*/wall class=world\n", &named, &report);
  Check(ClassOf(gb::FindTextureRow(mixed.View(), named, gb::kRowLayersAll, "d", "wall", 0xCC)) == int(gb::SurfaceClass::kWorld), "the player's */name row beats a compiled-in hash row");
  // the whole row is the higher layer's, not a merge of the two
  Built whole;
  gb::TextureRow rich;
  rich.fields = gb::kRowClass | gb::kRowSmooth | gb::kRowMetal;
  rich.cls = uint8_t(gb::SurfaceClass::kChrome);
  rich.smooth = 240;
  rich.metal = 255;
  whole.hash.push_back({0xDD, rich});
  whole.Sort();
  gb::TextureTable small;
  gb::ParseTextureTable("hash:DD rough=0.5\n", &small, &report);
  const gb::TextureRow* got = gb::FindTextureRow(whole.View(), small, gb::kRowLayersAll, "", "", 0xDD);
  Check(got != nullptr && got->fields == gb::kRowSmooth && got->metal == 0 && got->cls == 0, "an override row replaces the compiled-in row whole: it gives every field it wants");
}

// ---------------------------------------------------------------------------
// From the family's word to the texture's: the defaults and the row on top
// ---------------------------------------------------------------------------
void WordChecks() {
  // a wall (world, matte) whose texture a compiled-in row says is chrome: class, smoothness and metalness all come from the row's class unless it gives them
  Built t;
  gb::TextureRow chrome = ClassRow(gb::SurfaceClass::kChrome);
  t.hash.push_back({0x10, chrome});
  gb::TextureRow rough_chrome = chrome;
  rough_chrome.fields |= gb::kRowSmooth;
  rough_chrome.smooth = 140;
  t.hash.push_back({0x11, rough_chrome});
  gb::TextureRow wet = ClassRow(gb::SurfaceClass::kWetRoad);
  wet.fields |= gb::kRowSsr;
  wet.ssr = true;
  t.hash.push_back({0x12, wet});
  t.Sort();
  const gb::CompiledView v = t.View();
  const gb::TextureTable none;
  const uint32_t wall = gb::MaterialWord(0.15f, gb::SurfaceClass::kWorld, 0u, 1.0f);
  const uint32_t as_chrome = gb::ApplyTextureRow(wall, *gb::FindTextureRow(v, none, gb::kRowLayersAll, "", "", 0x10));
  Check(gb::MaterialClassOf(as_chrome) == gb::SurfaceClass::kChrome && gb::MaterialRt3Smoothness(as_chrome) == gb::ToByte(gb::DefaultSmoothness(gb::SurfaceClass::kChrome)) &&
            gb::MaterialMetalByte(as_chrome) == 255 && gb::MaterialLitByte(as_chrome) == 255,
        "class=chrome from a compiled-in row: the chrome class's smoothness and metalness 1");
  const uint32_t as_rough = gb::ApplyTextureRow(wall, *gb::FindTextureRow(v, none, gb::kRowLayersAll, "", "", 0x11));
  Check(gb::MaterialRt3Smoothness(as_rough) == 140 && gb::MaterialMetalByte(as_rough) == 255, "its own smoothness beats the class's; the metalness is still the class's");
  const uint32_t as_wet = gb::ApplyTextureRow(gb::MaterialWord(0.30f, gb::SurfaceClass::kRoad, 0u, 1.0f), *gb::FindTextureRow(v, none, gb::kRowLayersAll, "", "", 0x12));
  Check(gb::MaterialClassOf(as_wet) == gb::SurfaceClass::kWetRoad && (gb::MaterialFlagsOf(as_wet) & gb::kMaterialFlagSsr) != 0 && gb::MaterialMetalByte(as_wet) == 0 &&
            gb::MaterialRt3Smoothness(as_wet) == gb::ToByte(gb::DefaultSmoothness(gb::SurfaceClass::kWetRoad)),
        "a wet road row: the wet road's smoothness, a dielectric, SSR on");
  // a draw the G-buffer does not write (class 0) is left alone by a row
  const uint32_t no_data = gb::MaterialWord(0.0f, gb::SurfaceClass::kNone, 0u, 1.0f);
  Check(gb::ApplyTextureRow(no_data, *gb::FindTextureRow(v, none, gb::kRowLayersAll, "", "", 0x10)) == no_data, "a class 0 draw keeps its word whatever its texture says");
}

// ---------------------------------------------------------------------------
// The fingerprint
// ---------------------------------------------------------------------------
void FingerprintChecks() {
  const uint64_t empty = gb::CompiledRowsFingerprint(gb::CompiledView{});
  Check(empty != 0, "an empty table has a fingerprint");
  Built a;
  a.hash.push_back({0x5, ClassRow(gb::SurfaceClass::kGlass)});
  a.name.push_back({0x6, ClassRow(gb::SurfaceClass::kMetal)});
  a.Sort();
  Built same = a;
  Check(gb::CompiledRowsFingerprint(a.View()) == gb::CompiledRowsFingerprint(same.View()) && gb::CompiledRowsFingerprint(a.View()) != empty, "the same rows, the same fingerprint, not the empty one's");
  const uint64_t base = gb::CompiledRowsFingerprint(a.View());
  Built other_class = a;
  other_class.hash[1].row.cls = uint8_t(gb::SurfaceClass::kRoad);
  Check(gb::CompiledRowsFingerprint(other_class.View()) != base, "another class");
  Built other_key = a;
  other_key.hash[1].key = 0x55;
  Check(gb::CompiledRowsFingerprint(other_key.View()) != base, "another key");
  Built other_value = a;
  other_value.hash[1].row.fields |= gb::kRowMetal;
  other_value.hash[1].row.metal = 1;
  Check(gb::CompiledRowsFingerprint(other_value.View()) != base, "another field");
  Built other_ssr = a;
  other_ssr.hash[1].row.fields |= gb::kRowSsr;
  other_ssr.hash[1].row.ssr = true;
  Check(gb::CompiledRowsFingerprint(other_ssr.View()) != base, "the ssr bit");
  Built other_lit = a;
  other_lit.hash[1].row.fields |= gb::kRowLit;
  other_lit.hash[1].row.lit = 0;
  Check(gb::CompiledRowsFingerprint(other_lit.View()) != base, "the lit weight");
  Built metal_only = a;  // the same fields, another metalness number
  metal_only.hash[1].row.fields |= gb::kRowMetal;
  metal_only.hash[1].row.metal = 100;
  Built metal_other = metal_only;
  metal_other.hash[1].row.metal = 101;
  Check(gb::CompiledRowsFingerprint(metal_only.View()) != gb::CompiledRowsFingerprint(metal_other.View()), "another metalness with the same fields");
  Built other_smooth = a;
  other_smooth.hash[1].row.fields |= gb::kRowSmooth;
  other_smooth.hash[1].row.smooth = 9;
  Check(gb::CompiledRowsFingerprint(other_smooth.View()) != base, "the smoothness");
  Built moved = a;  // the same row in another array: the arrays are told apart
  moved.name.push_back(moved.hash[1]);
  moved.hash.pop_back();
  moved.Sort();
  Check(gb::CompiledRowsFingerprint(moved.View()) != base, "a row in another kind of array");
  Built more = a;
  more.pair.push_back({0x7, ClassRow(gb::SurfaceClass::kPaint)});
  more.Sort();
  Check(gb::CompiledRowsFingerprint(more.View()) != base, "one more row");
}

// ---------------------------------------------------------------------------
// The sample both languages read
// ---------------------------------------------------------------------------
void SampleChecks(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  Check(bool(in), "the sample table is readable: " + path);
  if (!in) return;
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  gb::TextureTable table;
  gb::TableParse report;
  gb::ParseTextureTable(text, &table, &report);
  Check(report.errors.empty() && table.size() >= 5, "the sample table parses (" + std::to_string(table.size()) + " rows)");
  Check(sample::CompiledRowsSorted(sample::kCompiledHashRows) && sample::CompiledRowsSorted(sample::kCompiledPairRows) && sample::CompiledRowsSorted(sample::kCompiledNameRows),
        "the emitter's arrays are sorted (its keys are the renderer's FNV order)");
  const gb::CompiledView view = {std::span<const gb::CompiledRow>(sample::kCompiledHashRows).subspan(1), std::span<const gb::CompiledRow>(sample::kCompiledPairRows).subspan(1),
                                 std::span<const gb::CompiledRow>(sample::kCompiledNameRows).subspan(1)};
  const size_t total = view.hash.size() + view.pair.size() + view.name.size();
  Check(total > 0, "the sample has rows");

  // every row the emitter wrote is a row of the table the sample says, found by the renderer's own keys; the sample table is the generated rows with the overrides on top, so the rows of the
  // table file (the generated ones) that the overrides replaced differ: the test compares with the rows of sample_overrides.txt applied the way the emitter does
  std::string overrides_text;
  {
    std::string overrides_path = path;
    const size_t at = overrides_path.rfind("sample_table.txt");
    if (at != std::string::npos) {
      overrides_path.replace(at, std::string("sample_table.txt").size(), "sample_overrides.txt");
      std::ifstream o(overrides_path, std::ios::binary);
      if (o) overrides_text.assign((std::istreambuf_iterator<char>(o)), std::istreambuf_iterator<char>());
    }
  }
  gb::TextureTable expected;
  gb::TableParse expected_report;
  gb::ParseTextureTable(text, &expected, &expected_report);
  gb::ParseTextureTable(overrides_text, &expected, &expected_report);  // the later rows replace the earlier of the same key: the emitter's merge
  Check(expected.size() == total, "the emitter wrote as many rows as the merged table has (" + std::to_string(total) + " of " + std::to_string(expected.size()) + ")");

  // the keys in the sample, as the tool printed them in its comments, are not parsed: instead every kind of key of the merged table is looked up in the compiled arrays
  std::vector<std::string> lines;
  {
    std::string merged = text + "\n" + overrides_text;
    size_t at = 0;
    while (at < merged.size()) {
      size_t end = merged.find('\n', at);
      if (end == std::string::npos) end = merged.size();
      lines.push_back(merged.substr(at, end - at));
      at = end + 1;
    }
  }
  int checked = 0;
  bool all_equal = true;
  for (const std::string& line : lines) {
    gb::TextureTable one;
    gb::TableParse r;
    gb::ParseTextureTable(line, &one, &r);
    if (one.size() != 1) continue;
    // the line's key text is its first token
    const std::string key_text = line.substr(0, line.find_first_of(" \t"));
    gb::TextureKeyRef key;
    std::string why;
    if (!gb::ParseTextureKey(key_text, &key, &why)) continue;
    const std::string dictionary = key.kind == gb::KeyKind::kPair ? key.dictionary : std::string();
    const std::string name = key.kind == gb::KeyKind::kContentHash ? std::string("unnamed") : key.name;
    const uint64_t hash = key.kind == gb::KeyKind::kContentHash ? key.hash : 0;
    const gb::TextureRow* want = expected.Find(dictionary, name, hash);
    const gb::TextureRow* have = gb::FindCompiledRow(view, dictionary, name, hash);
    ++checked;
    all_equal = all_equal && want != nullptr && have != nullptr && *want == *have;
    if (want == nullptr || have == nullptr || !(*want == *have)) std::printf("  differs: %s\n", key_text.c_str());
  }
  Check(checked >= int(total) && all_equal, "every key of the sample is found in the compiled-in sample with the parsed row (" + std::to_string(checked) + " keys)");
}

}  // namespace

int main(int argc, char** argv) {
  ShippedChecks();
  SortedChecks();
  LookupChecks();
  LayerChecks();
  WordChecks();
  FingerprintChecks();
  if (argc > 1) {
    SampleChecks(argv[1]);
  } else {
    std::printf("(no sample table given: the emitter's sample is not compared)\n");
  }
  std::printf("gbuffer_texture_rows_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
