// Check of the G-buffer's per-texture material table (gbuffer_materials.h, package GB-F2): the text format and its parser, the keys and their priority, what a row does to a draw's word,
// the files' hot reload, the census lines and the shader names file. A program of its own: it needs no renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_gbuffer_materials_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_gbuffer_materials_test [<tools/gbuffer_materials/sample_table.txt>]
//
// With the sample table of tools/gbuffer_materials (the file the Python tools' tests also read) it checks that the table parses without an error and has the rows the sample says.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "gbuffer_materials.h"

namespace gb = mcla::gpu::gbuffer;
namespace fs = std::filesystem;

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

gb::TextureTable Parse(const std::string& text, gb::TableParse* report = nullptr) {
  gb::TextureTable table;
  gb::TableParse local;
  gb::ParseTextureTable(text, &table, report != nullptr ? report : &local);
  return table;
}

const gb::TextureRow* FindRow(const gb::TextureTable& table, const char* dictionary, const char* name, uint64_t hash = 0) { return table.Find(dictionary, name, hash); }

// ---------------------------------------------------------------------------
// Names, keys, the FNV
// ---------------------------------------------------------------------------
void NameChecks() {
  Check(std::size(gb::kSurfaceClassNames) == 16, "sixteen class names");
  std::set<std::string_view> unique(std::begin(gb::kSurfaceClassNames), std::end(gb::kSurfaceClassNames));
  Check(unique.size() == 16, "the class names are all different");
  bool round_trip = true;
  for (uint32_t i = 0; i < 16; ++i) {
    gb::SurfaceClass cls = gb::SurfaceClass::kCount;
    round_trip = round_trip && gb::ParseSurfaceClass(gb::SurfaceClassName(gb::SurfaceClass(i)), &cls) && uint32_t(cls) == i;
    gb::SurfaceClass by_number = gb::SurfaceClass::kCount;
    round_trip = round_trip && gb::ParseSurfaceClass(std::to_string(i), &by_number) && uint32_t(by_number) == i;
  }
  Check(round_trip, "every class parses from its name and from its number");
  gb::SurfaceClass cls = gb::SurfaceClass::kNone;
  Check(gb::ParseSurfaceClass("GLASS", &cls) && cls == gb::SurfaceClass::kGlass, "a class name in capitals");
  Check(!gb::ParseSurfaceClass("16", &cls) && !gb::ParseSurfaceClass("", &cls) && !gb::ParseSurfaceClass("glas", &cls) && !gb::ParseSurfaceClass("-1", &cls), "no class 16, no empty, no near miss");
  Check(gb::SurfaceClassName(gb::SurfaceClass::kEmissive) == "emissive" && gb::SurfaceClassName(gb::SurfaceClass::kWetRoad) == "wetroad", "the names of the classes the survey names");

  Check(gb::Fnv1aLower("") == 0xcbf29ce484222325ull, "FNV-1a of the empty text is the offset basis");
  Check(gb::Fnv1aLower("a") == 0xaf63dc4c8601ec8cull, "FNV-1a 64 of 'a'");
  Check(gb::Fnv1aLower("foobar") == 0x85944171f73967e8ull, "FNV-1a 64 of 'foobar'");
  Check(gb::Fnv1aLower("FooBar") == gb::Fnv1aLower("foobar"), "the key ignores the case");
  Check(gb::PairKey("Shared", "Glass_01") == gb::PairKey("shared", "glass_01"), "a pair key ignores the case");
  Check(gb::PairKey("a", "bc") != gb::PairKey("ab", "c"), "the pair key keeps the dictionary and the name apart");
  Check(gb::PairKey("shared", "x") != gb::NameKey("x"), "a pair is not a bare name");
  Check(gb::IsUnnamed("unnamed") && gb::IsUnnamed("UNNAMED") && gb::IsUnnamed("") && !gb::IsUnnamed("unnamed_2") && !gb::IsUnnamed("glass"), "what counts as no name");
}

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------
void KeyChecks() {
  gb::TextureKeyRef key;
  std::string why;
  Check(gb::ParseTextureKey("shared/glass_01", &key, &why) && key.kind == gb::KeyKind::kPair && key.dictionary == "shared" && key.name == "glass_01", "dictionary/name");
  Check(gb::ParseTextureKey("*/trim", &key, &why) && key.kind == gb::KeyKind::kAnyDictionary && key.name == "trim" && key.dictionary.empty(), "*/name is any dictionary");
  Check(gb::ParseTextureKey("hash:00B2E7B3C830B0E7", &key, &why) && key.kind == gb::KeyKind::kContentHash && key.hash == 0x00B2E7B3C830B0E7ull, "hash:<16 hex>");
  Check(gb::ParseTextureKey("HASH:0xabc", &key, &why) && key.hash == 0xabcull, "hash: with 0x and in capitals");
  Check(gb::ParseTextureKey("hash:1", &key, &why) && key.hash == 1, "a one digit hash");
  Check(!gb::ParseTextureKey("hash:", &key, &why) && !why.empty(), "an empty hash is refused");
  Check(!gb::ParseTextureKey("hash:0", &key, &why) && !gb::ParseTextureKey("hash:0000000000000000", &key, &why), "the hash 0 is refused");
  Check(!gb::ParseTextureKey("hash:12345678901234567", &key, &why), "17 digits are refused");
  Check(!gb::ParseTextureKey("hash:12G4", &key, &why), "a non hex digit is refused");
  Check(!gb::ParseTextureKey("glass", &key, &why) && !gb::ParseTextureKey("a/b/c", &key, &why) && !gb::ParseTextureKey("/b", &key, &why) && !gb::ParseTextureKey("a/", &key, &why),
        "a key needs exactly one slash with text on both sides");
  Check(!gb::ParseTextureKey("common/unnamed", &key, &why) && why.find("hash:") != std::string::npos, "an unnamed texture is refused as a name key, with the way out in the message");
  Check(!gb::ParseTextureKey("*/UNNAMED", &key, &why), "*/unnamed is refused too");
}

// ---------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------
void ParseChecks() {
  gb::TableParse report;
  gb::TextureTable table = Parse(
      "# a table\n"
      "@version 1\n"
      "\n"
      "shared/glass_01   class=glass rough=0.05 ssr=1            # a window\n"
      "  common_props/rim  class=chrome smooth=0.95 metal=1\r\n"
      "*/bumper class=paint\n"
      "hash:00000000000000AB metal=0.5 lit=0 ssr=off\n"
      "\n",
      &report);
  Check(report.errors.empty(), "a clean table has no error");
  Check(report.rows == 4 && table.size() == 4, "four rows");
  const gb::TextureRow* glass = FindRow(table, "shared", "glass_01");
  Check(glass != nullptr && glass->fields == (gb::kRowClass | gb::kRowSmooth | gb::kRowSsr) && glass->cls == uint8_t(gb::SurfaceClass::kGlass) && glass->ssr, "the glass row's fields");
  Check(glass != nullptr && glass->smooth == gb::ToByte(0.95f), "rough=0.05 is smoothness 0.95");
  const gb::TextureRow* rim = FindRow(table, "common_props", "rim");
  Check(rim != nullptr && rim->fields == (gb::kRowClass | gb::kRowSmooth | gb::kRowMetal) && rim->smooth == gb::ToByte(0.95f) && rim->metal == 255, "smooth= and metal=1, a CRLF line, leading blanks");
  const gb::TextureRow* hashed = FindRow(table, "", "", 0xAB);
  Check(hashed != nullptr && hashed->fields == (gb::kRowMetal | gb::kRowLit | gb::kRowSsr) && !hashed->ssr && hashed->lit == 0 && hashed->metal == gb::ToByte(0.5f), "a hash row: metal, lit 0, ssr off");
  Check(table.has_hash_rows(), "the table knows it has a hash row");
  const gb::TextureRow* any = FindRow(table, "whatever", "bumper");
  Check(any != nullptr && any->fields == gb::kRowClass && any->cls == uint8_t(gb::SurfaceClass::kPaint), "*/name matches any dictionary");
  Check(FindRow(table, "SHARED", "GLASS_01") != nullptr, "the lookup ignores the case");
  Check(FindRow(table, "shared", "other") == nullptr && FindRow(table, "", "", 0xAC) == nullptr, "a texture with no row has none");
  Check(!Parse("# nothing\n").has_hash_rows() && Parse("").empty() && Parse("\n\n").empty(), "an empty file is an empty table");
}

void ErrorChecks() {
  struct Case {
    const char* text;
    const char* what;
  };
  const Case cases[] = {
      {"shared/x\n", "a key with no field"},
      {"shared/x class\n", "a field with no value"},
      {"shared/x class=\n", "a field with an empty value"},
      {"shared/x =glass\n", "a value with no field"},
      {"shared/x class=glas\n", "an unknown class"},
      {"shared/x rough=1.5\n", "a roughness above 1"},
      {"shared/x rough=-0.1\n", "a negative roughness"},
      {"shared/x rough=abc\n", "a roughness that is not a number"},
      {"shared/x rough=0.5x\n", "a roughness with text after it"},
      {"shared/x rough=nan\n", "a roughness of nan"},
      {"shared/x metal=inf\n", "a metalness of inf"},
      {"shared/x ssr=maybe\n", "a flag that is not 0 or 1"},
      {"shared/x lit=2\n", "a lit weight above 1"},
      {"shared/x speed=1\n", "an unknown field"},
      {"shared/x class=glass class=metal\n", "a field twice"},
      {"shared/x rough=0.5 smooth=0.5\n", "rough and smooth together (one field)"},
      {"common/unnamed class=glass\n", "an unnamed key"},
      {"glass class=glass\n", "a key with no slash"},
      {"@include other.txt\n", "an unknown directive"},
      {"@version\n", "a version with no number"},
  };
  for (const Case& c : cases) {
    gb::TableParse report;
    const gb::TextureTable table = Parse(c.text, &report);
    Check(report.errors.size() == 1 && report.errors[0].line == 1 && !report.errors[0].text.empty() && table.empty(), std::string("one error on line 1, no row: ") + c.what);
  }
  // the good rows around a bad one are used, and the line numbers are right
  gb::TableParse report;
  const gb::TextureTable table = Parse("shared/a class=glass\nshared/b class=nonsense\n# c\n\nshared/c metal=1\nshared/d rough=9\n", &report);
  Check(report.errors.size() == 2 && report.errors[0].line == 2 && report.errors[1].line == 6, "the bad lines are numbered");
  Check(table.size() == 2 && FindRow(table, "shared", "a") != nullptr && FindRow(table, "shared", "c") != nullptr && FindRow(table, "shared", "b") == nullptr, "the good rows around bad ones are kept");

  gb::TableParse versioned;
  const gb::TextureTable refused = Parse("shared/a class=glass\n@version 2\nshared/b class=glass\n", &versioned);
  Check(refused.empty() && versioned.errors.size() == 1 && versioned.errors[0].line == 2 && versioned.rows == 0, "a file of another version gives no rows, not even the ones before the line");
  gb::TableParse same;
  Parse("@version 1\nshared/a class=glass\n", &same);
  Check(same.errors.empty() && same.rows == 1, "version 1 is this one");
}

void LayerChecks() {
  gb::TextureTable table;
  gb::TableParse first;
  gb::ParseTextureTable("shared/a class=glass rough=0.1\nshared/b class=metal\nhash:FF metal=1\n", &table, &first);
  gb::TableParse second;
  gb::ParseTextureTable("shared/a class=chrome\nSHARED/B rough=0.5\nhash:00FF lit=0\n", &table, &second);
  Check(table.size() == 3 && second.replaced == 3, "a later file's rows replace the earlier rows of the same key (any case, any hex padding)");
  const gb::TextureRow* a = FindRow(table, "shared", "a");
  Check(a != nullptr && a->fields == gb::kRowClass && a->cls == uint8_t(gb::SurfaceClass::kChrome), "the later row replaces the whole row, not field by field");
  const gb::TextureRow* h = FindRow(table, "", "", 0xFF);
  Check(h != nullptr && h->fields == gb::kRowLit && h->lit == 0, "a hash row replaced the same way");
  gb::TextureTable one;
  gb::TableParse twice;
  gb::ParseTextureTable("shared/a class=glass\nshared/a class=metal\n", &one, &twice);
  Check(one.size() == 1 && twice.replaced == 1 && FindRow(one, "shared", "a")->cls == uint8_t(gb::SurfaceClass::kMetal), "the last row of a file wins");
}

void LookupChecks() {
  const gb::TextureTable table = Parse(
      "hash:1111 class=glass\n"
      "shared/tex class=metal\n"
      "*/tex class=chrome\n"
      "*/only_name class=paint\n");
  auto cls_of = [&](const gb::TextureRow* row) { return row != nullptr ? int(row->cls) : -1; };
  Check(cls_of(table.Find("shared", "tex", 0x1111)) == int(gb::SurfaceClass::kGlass), "the hash is the most specific key");
  Check(cls_of(table.Find("shared", "tex", 0x2222)) == int(gb::SurfaceClass::kMetal), "a hash with no row falls to dictionary/name");
  Check(cls_of(table.Find("other", "tex", 0)) == int(gb::SurfaceClass::kChrome), "another dictionary falls to */name");
  Check(cls_of(table.Find("other", "only_name", 0)) == int(gb::SurfaceClass::kPaint) && cls_of(table.Find("", "only_name", 0)) == int(gb::SurfaceClass::kPaint), "*/name with a dictionary or without");
  Check(cls_of(table.Find("shared", "unnamed", 0)) == -1 && cls_of(table.Find("shared", "", 0)) == -1, "an unnamed texture is not found by name");
  Check(cls_of(table.Find("shared", "unnamed", 0x1111)) == int(gb::SurfaceClass::kGlass), "an unnamed texture is found by its hash");
  Check(cls_of(table.Find("shared", "nothing", 0)) == -1, "no row");
  const gb::TextureTable empty;
  Check(empty.Find("shared", "tex", 0x1111) == nullptr && empty.empty(), "an empty table finds nothing");
}

// ---------------------------------------------------------------------------
// What a row does to a word
// ---------------------------------------------------------------------------
void ApplyChecks() {
  const uint32_t base = gb::MaterialWord(0.40f, gb::SurfaceClass::kWorld, gb::kMaterialFlagVehicle | gb::kMaterialFlagAlphaCovered, 1.0f);
  gb::TextureRow none;
  Check(gb::ApplyTextureRow(base, none) == base, "a row with no field changes nothing");
  gb::TextureRow row;
  row.fields = gb::kRowSmooth;
  row.smooth = 200;
  uint32_t word = gb::ApplyTextureRow(base, row);
  Check(gb::MaterialRt3Smoothness(word) == 200 && gb::MaterialClassOf(word) == gb::SurfaceClass::kWorld && gb::MaterialFlagsOf(word) == (gb::kMaterialFlagVehicle | gb::kMaterialFlagAlphaCovered) &&
            gb::MaterialLitByte(word) == 255 && gb::MaterialMetalByte(word) == 0,
        "rough alone changes the smoothness byte and nothing else");
  row = {};
  row.fields = gb::kRowClass;
  row.cls = uint8_t(gb::SurfaceClass::kGlass);
  word = gb::ApplyTextureRow(base, row);
  Check(gb::MaterialClassOf(word) == gb::SurfaceClass::kGlass && gb::MaterialRt3Smoothness(word) == gb::ToByte(gb::DefaultSmoothness(gb::SurfaceClass::kGlass)) &&
            gb::MaterialLitByte(word) == gb::ToByte(gb::DefaultLit(gb::SurfaceClass::kGlass)),
        "a class alone takes the class's default smoothness and lit weight");
  Check(gb::MaterialFlagsOf(word) == (gb::kMaterialFlagVehicle | gb::kMaterialFlagAlphaCovered), "a class keeps the draw's flags (a row's ssr is its own field)");
  row.cls = uint8_t(gb::SurfaceClass::kEmissive);
  word = gb::ApplyTextureRow(base, row);
  Check(gb::MaterialClassOf(word) == gb::SurfaceClass::kEmissive && gb::MaterialLitByte(word) == 0, "class=emissive makes the lit weight 0");
  row.fields = gb::kRowClass | gb::kRowSmooth | gb::kRowLit;
  row.smooth = 77;
  row.lit = 128;
  word = gb::ApplyTextureRow(base, row);
  Check(gb::MaterialRt3Smoothness(word) == 77 && gb::MaterialLitByte(word) == 128 && gb::MaterialClassOf(word) == gb::SurfaceClass::kEmissive, "a row's own smoothness and lit weight beat the class's defaults");
  row = {};
  row.fields = gb::kRowSsr;
  row.ssr = true;
  word = gb::ApplyTextureRow(base, row);
  Check((gb::MaterialFlagsOf(word) & gb::kMaterialFlagSsr) != 0 && (gb::MaterialFlagsOf(word) & gb::kMaterialFlagVehicle) != 0 && gb::MaterialRt3Smoothness(word) == gb::MaterialRt3Smoothness(base),
        "ssr=1 sets the flag and keeps the others");
  row.ssr = false;
  word = gb::ApplyTextureRow(gb::ApplyTextureRow(base, [] { gb::TextureRow r; r.fields = gb::kRowSsr; r.ssr = true; return r; }()), row);
  Check((gb::MaterialFlagsOf(word) & gb::kMaterialFlagSsr) == 0 && (gb::MaterialFlagsOf(word) & gb::kMaterialFlagVehicle) != 0, "ssr=0 clears the flag and keeps the others");
  row = {};
  row.fields = gb::kRowMetal;
  row.metal = 200;
  word = gb::ApplyTextureRow(base, row);
  Check(gb::MaterialMetalByte(word) == 200 && (word & 0x00FFFFFFu) == (base & 0x00FFFFFFu), "metal= sets bits 24..31 and nothing below them");
  // every field of a word that came out of a row reads back in the twin's readers
  row = {};
  row.fields = gb::kRowClass | gb::kRowSmooth | gb::kRowMetal | gb::kRowSsr | gb::kRowLit;
  row.cls = uint8_t(gb::SurfaceClass::kChrome);
  row.smooth = 240;
  row.metal = 255;
  row.ssr = true;
  row.lit = 255;
  word = gb::ApplyTextureRow(base, row);
  Check(gb::MaterialClassOf(word) == gb::SurfaceClass::kChrome && gb::MaterialRt3Smoothness(word) == 240 && gb::MaterialMetalByte(word) == 255 && gb::MaterialLitByte(word) == 255 &&
            gb::MaterialFlagsOf(word) == (gb::kMaterialFlagSsr | gb::kMaterialFlagVehicle | gb::kMaterialFlagAlphaCovered),
        "a full row");
  // a draw the family does not write is not written because of a texture
  const uint32_t no_data = gb::MaterialWord(0.0f, gb::SurfaceClass::kNone, 0u, 1.0f);
  Check(gb::ApplyTextureRow(no_data, row) == no_data, "a class 0 draw is left alone");
  // applying twice is the same as once
  Check(gb::ApplyTextureRow(word, row) == word, "a row applied twice changes nothing more");
  // the default word of an unknown shader takes rows
  Check(gb::MaterialClassOf(gb::ApplyTextureRow(gb::DefaultMaterialWord(), [] { gb::TextureRow r; r.fields = gb::kRowClass; r.cls = uint8_t(gb::SurfaceClass::kRoad); return r; }())) == gb::SurfaceClass::kRoad,
        "the default word (world class) takes a row");
}

// ---------------------------------------------------------------------------
// The files
// ---------------------------------------------------------------------------
struct TempDir {
  fs::path path;
  TempDir() {
    path = fs::temp_directory_path() / ("mcla_gbmat_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

void WriteFile(const fs::path& file, const std::string& text, int seconds) {
  {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
  }
  // a time of its own, so that two writes in one clock tick are two different stamps
  static const fs::file_time_type start = fs::file_time_type::clock::now();
  std::error_code ec;
  fs::last_write_time(file, start + std::chrono::seconds(seconds), ec);
}

void FilesChecks() {
  TempDir dir;
  const fs::path base = dir.path / "base.txt";
  const fs::path over = dir.path / "over.txt";
  const fs::path missing = dir.path / "missing.txt";
  gb::TextureTableFiles files;
  files.SetSources({base, missing, over});
  gb::TextureTableFiles::Report report;
  Check(files.Poll(0, &report) && files.table().empty() && report.files == 0 && files.epoch() == 1, "the first look reads what is there: no file, an empty table, epoch 1");
  Check(!files.Poll(1, &report) && !files.Poll(29, &report), "no look inside the poll period");

  WriteFile(base, "shared/a class=glass\nshared/b class=metal\n", 100);
  Check(!files.Poll(30, &report), "a new file is seen at the next look ...");
  Check(files.table().empty(), "... and not read yet");
  Check(files.Poll(60, &report) && files.table().size() == 2 && report.files == 1 && report.rows == 2 && report.errors == 0 && files.epoch() == 2, "... read at the look after, when it stayed the same");
  Check(!files.Poll(90, &report) && files.epoch() == 2, "an unchanged file is not read again");

  WriteFile(over, "shared/a class=chrome\nshared/c metal=1\nbroken line\n", 101);
  files.Poll(120, &report);
  Check(files.Poll(150, &report) && files.table().size() == 3 && report.files == 2 && report.errors == 1 && !report.lines.empty(), "a second file is layered on the first, its bad row reported");
  Check(report.lines[0].find("over.txt:3:") != std::string::npos, "the report names the file and the line of a bad row");
  const gb::TextureRow* a = files.table().Find("shared", "a", 0);
  Check(a != nullptr && a->cls == uint8_t(gb::SurfaceClass::kChrome), "the later file's row wins");
  Check(files.table().Find("shared", "b", 0) != nullptr && files.table().Find("shared", "c", 0) != nullptr, "the earlier file's other rows stay");

  // an edit that keeps changing is not read until it settles
  WriteFile(base, "shared/a class=glass\nshared/b class=metal\nshared/d class=road\n", 102);
  Check(!files.Poll(180, &report), "an edit is noticed ...");
  WriteFile(base, "shared/a class=glass\nshared/b class=metal\nshared/d class=road\nshared/e class=water\n", 103);
  Check(!files.Poll(210, &report), "... a second edit before the settle starts the wait again");
  Check(files.Poll(240, &report) && files.table().size() == 5, "and it is read once it stayed the same for two looks");

  // a file that goes away
  std::error_code ec;
  fs::remove(over, ec);
  files.Poll(270, &report);
  Check(files.Poll(300, &report) && files.table().size() == 4 && files.table().Find("shared", "c", 0) == nullptr, "a removed file's rows go");
  fs::remove(base, ec);
  files.Poll(330, &report);
  Check(files.Poll(360, &report) && files.table().empty(), "with every file gone the table is empty");

  // a file with a refused version
  WriteFile(base, "@version 9\nshared/a class=glass\n", 104);
  files.Poll(390, &report);
  Check(files.Poll(420, &report) && files.table().empty() && report.errors == 1, "a file of another version gives no row and one error");

  // SetSources starts again
  files.SetSources({base});
  Check(files.Poll(0, &report) && files.epoch() > 0, "SetSources makes the next Poll read at once");
}

void SourceChecks() {
  const fs::path pack = "pack";
  const fs::path mods = "mods";
  auto sources = gb::TextureTableSources(pack, mods, "");
  Check(sources.size() == 2 && sources[0] == pack / "mcla_gbuffer_texture_materials.txt" && sources[1] == mods / "gbuffer_texture_materials.txt", "the pack's table, then the mods folder's rows");
  sources = gb::TextureTableSources(pack, mods, "a.txt;b.txt;;c.txt");
  Check(sources.size() == 5 && sources[2] == fs::path("a.txt") && sources[3] == fs::path("b.txt") && sources[4] == fs::path("c.txt"), "the environment's paths come after, in order, empty pieces skipped");
  Check(gb::TextureTableSources(pack, mods, "off").empty() && gb::TextureTableSources(pack, mods, "OFF").empty(), "\"off\" reads no file");
  Check(gb::TextureTableSources(fs::path(), mods, "").size() == 1 && gb::TextureTableSources(fs::path(), fs::path(), "x.txt").size() == 1, "no pack folder or mods folder: no file for it");
}

// ---------------------------------------------------------------------------
// The census
// ---------------------------------------------------------------------------
std::vector<std::string> Split(const std::string& line, char separator) {
  std::vector<std::string> parts;
  std::string part;
  std::stringstream ss(line);
  while (std::getline(ss, part, separator)) parts.push_back(part);
  return parts;
}

void CensusChecks() {
  gb::TextureCensus census;
  const uint32_t a = census.Intern(100, "shared", "glass_01");
  const uint32_t b = census.Intern(200, "common", "unnamed");
  Check(a != b && census.Intern(100, "shared", "glass_01") == a && census.Intern(100, "shared", "other") != a && census.Intern(101, "shared", "glass_01") != a, "an id per (page, dictionary, name)");
  census.Describe(a, 0, 256, 128, 18);
  census.Describe(b, 0x00B2E7B3C830B0E7ull, 512, 512, 54);
  census.Describe(b, 0, 512, 512, 54);  // a later 0 does not forget the hash
  Check(census.Texture(b)->hash == 0x00B2E7B3C830B0E7ull && census.Texture(a)->width == 256, "the texture's facts, the hash kept");
  census.StartWindow(1000);
  census.Note(a, 0xAAAA, 0x01FF2040u, 300, false);
  census.Note(a, 0xAAAA, 0x01FF2040u, 120, false);
  census.Note(a, 0xAAAA, 0x01FF2041u, 30, true);
  census.Note(b, 0xBBBB, 0x00000000u, 6, false);
  census.Note(b, 0xBBBB, 0x00000000u, 6, false);
  census.Note(b, 0xCCCC, 0x00000000u, 6, false);
  Check(census.size() == 3 && !census.empty(), "three (texture, shader) pairs");
  Check(!census.WindowDue(1000) && !census.WindowDue(1299) && census.WindowDue(1300), "the window is due after 300 frames");
  const std::vector<std::string> lines = census.TakeLines(1300);
  Check(lines.size() == 4, "a header and three rows");
  Check(lines[0] == "texcensus tc1-window|300|4|3|6|468|0", "the header: frames, textures, pairs, draws, indices, left out (" + lines[0] + ")");
  Check(lines[1] == "texcensus tc1|300|shared|glass_01|0000000000000000|000000000000AAAA|256|128|18|3|450|01FF2040|1|1", "the busiest row, golden (" + lines[1] + ")");
  Check(lines[2] == "texcensus tc1|300|common|unnamed|00B2E7B3C830B0E7|000000000000BBBB|512|512|54|2|12|00000000|0|0", "the second row, golden (" + lines[2] + ")");
  const std::vector<std::string> fields = Split(lines[1].substr(std::string(gb::TextureCensus::kLinePrefix).size()), '|');
  Check(fields.size() == 14 && fields[0] == "tc1", "a row has 14 fields, the first the version tag");
  Check(census.empty() && census.window_start() == 1300 && !census.WindowDue(1400), "taking the lines starts the next window");
  const std::vector<std::string> next = census.TakeLines(1500);
  Check(next.size() == 1 && next[0] == "texcensus tc1-window|200|4|0|0|0|0", "an empty window is a header only");

  gb::TextureCensus cleaned;
  const uint32_t c = cleaned.Intern(1, "a|b c", "n\tm");
  cleaned.Note(c, 1, 0, 1, false);
  const std::vector<std::string> cl = cleaned.TakeLines(10);
  Check(cl.size() == 2 && Split(cl[1], '|').size() == 14 && cl[1].find("a_b_c|n_m|") != std::string::npos, "separators and blanks in a name become underscores, the field count stays");

  gb::TextureCensus many;
  const uint32_t t = many.Intern(1, "d", "n");
  for (uint64_t i = 0; i < gb::TextureCensus::kMaxLines + 50; ++i) many.Note(t, i + 1, 0, 1, false);
  const std::vector<std::string> capped = many.TakeLines(5);
  Check(capped.size() == gb::TextureCensus::kMaxLines + 1 && capped[0].find("|50") == capped[0].size() - 3, "at most kMaxLines rows, the rest counted in the header");
}

// ---------------------------------------------------------------------------
// The shader names file
// ---------------------------------------------------------------------------
void ShaderNamesChecks() {
  const std::array<gb::NamedShader, 6> shaders = {{
      {0x00000000000000BBull, true, "xCityRoad__PSMultiLight"},
      {0x00000000000000AAull, true, "xCarPaint__PSCarPaint"},
      {0x00000000000000CCull, false, "xCityDefault__VSCityDefaultDay"},
      {0x00000000000000AAull, true, "xOther__PSDuplicateKey"},
      {0x00000000000000DDull, true, "unnamed_00000000000000DD"},
      {0x00000000000000EEull, true, "xCityBumpSpec__PSMultiLightInterior"},
  }};
  const std::string tsv = gb::FormatShaderNamesTsv(shaders);
  const std::vector<std::string> lines = Split(tsv, '\n');
  Check(lines.size() == 6, "a header and five shaders (the second with identity AA is dropped)");
  Check(lines[0] == "key\tstage\tgroup\tentry\tclass\tflags\tsmooth\tlit\tgloss_bank\tgloss_reg", "the header line");
  Check(lines[1].rfind("00000000000000AA\tp\txCarPaint\tPSCarPaint\tpaint\t4\t", 0) == 0, "sorted by identity, the first of a duplicate wins, class and flags");
  Check(lines[2].rfind("00000000000000BB\tp\txCityRoad\tPSMultiLight\troad\t1\t", 0) == 0, "a road shader");
  Check(lines[3] == "00000000000000CC\tv\txCityDefault\tVSCityDefaultDay\t-\t-\t-\t-\tvertex\t73", "a vertex shader: no class, its gloss register");
  Check(lines[4] == "00000000000000DD\tp\tunnamed_00000000000000DD\t-\t-\t-\t-\t-\t-\t-", "an unnamed shader: no group, no entry, no class");
  Check(lines[5].find("\tinterior\t") != std::string::npos && lines[5].find("\tpixel\t101") != std::string::npos, "the interior entry makes a wall an interior one, its gloss register is the pixel shader's");
  Check(!tsv.empty() && tsv.back() == '\n', "the file ends with a newline");
}

// ---------------------------------------------------------------------------
// The sample table both languages read
// ---------------------------------------------------------------------------
void SampleChecks(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  Check(bool(in), "the sample table is readable: " + path);
  if (!in) return;
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  gb::TableParse report;
  const gb::TextureTable table = Parse(text, &report);
  Check(report.errors.empty(), "the sample table parses without an error (" + std::to_string(report.errors.size()) + ")");
  for (const gb::TableDiagnostic& e : report.errors) std::printf("  %u: %s\n", e.line, e.text.c_str());
  Check(table.size() == report.rows - report.replaced && table.size() >= 5, "the sample has rows (" + std::to_string(table.size()) + ")");
  const gb::TextureRow* glass = table.Find("", "", 0x00B2E7B3C830B0E7ull);
  Check(glass != nullptr && glass->cls == uint8_t(gb::SurfaceClass::kGlass), "the sample's first hash row is glass");
}

}  // namespace

int main(int argc, char** argv) {
  NameChecks();
  KeyChecks();
  ParseChecks();
  ErrorChecks();
  LayerChecks();
  LookupChecks();
  ApplyChecks();
  FilesChecks();
  SourceChecks();
  CensusChecks();
  ShaderNamesChecks();
  if (argc > 1) SampleChecks(argv[1]);
  std::printf("gbuffer_materials_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
