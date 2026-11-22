// Check of the runtime's reader of the city hit file (hit_reader.h): CITY-RT M2 package H3. A program of its own: it writes a small hit file with city_hit_format.h's writers into a scratch
// folder, opens it beside a made-up base pack identity, and checks what it reads back and what it refuses. No game files, no renderer, no GPU; exit 0 = passed.
//
//   build: cmake --build <dir> --target larecomp_hit_reader_test, then run src/citypack/larecomp_hit_reader_test [<scratch folder>]
//   or by hand: clang++ -std=c++23 -O1 -I src src/citypack/hit_reader_test.cpp src/citypack/hit_reader.cpp src/citypack/city_hit_format.cpp src/citypack/pack_format.cpp src/citypack/pack_reader.cpp

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "citypack/hit_reader.h"

using namespace larecomp::citypack;

namespace {

int g_failures = 0;
int g_checks = 0;
void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::fprintf(stderr, "FAILED: %s\n", what.c_str());
  }
}

struct Lcg {
  uint64_t s = 12345;
  uint32_t Next() {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return uint32_t(s >> 33);
  }
};

constexpr uint32_t kSectors = 6;

BaseIdentity MakeBase() {
  BaseIdentity b;
  b.table_hash = 0x1122334455667788ull;
  b.file_size = 987654321;
  for (uint32_t i = 0; i < kSectors; ++i) {
    BaseIdentity::Sector s;
    s.name_hash = 0xA0000000u + i * 17u;
    s.hi_triangles = i == 4 ? 0 : 30 + i * 11;  // sector 4 has no hi mesh
    s.lod_triangles = i == 5 ? 0 : 6 + i;       // sector 5 has no lod
    b.sectors.push_back(s);
  }
  return b;
}

HitBlob MakeBlob(Lcg& rng, uint32_t triangles) {
  HitBlob blob;
  blob.triangle_count = triangles;
  blob.opaque_triangles = triangles / 2;
  blob.flags = kHitBlobHasCorners;
  const auto add = [&](uint32_t first, uint32_t count, bool alpha) {
    if (count == 0) return;
    HitRun r;
    r.first_triangle = first;
    r.triangle_count = count;
    r.material = rng.Next() % 3;
    r.flags = (alpha ? kRunAlphaTested : 0u) | kRunHasUv | kRunHasColour;
    blob.runs.push_back(r);
  };
  add(0, blob.opaque_triangles, false);
  add(blob.opaque_triangles, triangles - blob.opaque_triangles, true);
  blob.corners.resize(size_t(triangles) * 6);
  for (uint32_t& w : blob.corners) w = rng.Next();
  return blob;
}

struct Built {
  std::vector<uint8_t> file;
  std::vector<HitBlob> hi, lod;
  std::vector<HitMaterial> materials;
  std::vector<HitTexture> textures;
};

// A whole hit file over `base`.
Built BuildFile(const BaseIdentity& base) {
  Lcg rng;
  Built out;
  const uint32_t material_count = 3, texture_count = 2;
  out.materials.resize(material_count);
  out.materials[1].word = PackMaterialWord(200, 5, 1, 255, 10);
  out.materials[1].texture = 0;
  out.materials[1].emissive = 4.5f;
  out.materials[2].word = PackMaterialWord(10, 2, 0, 200, 0);
  out.materials[2].texture = 1;
  out.textures.resize(texture_count);
  out.textures[0].key = 0xAAAA;
  out.textures[0].width = 256;
  out.textures[0].height = 128;
  out.textures[0].mip_count = 9;
  out.textures[1].key = 0xBBBB;
  out.textures[1].width = 64;
  out.textures[1].height = 64;
  out.textures[1].format = HitTextureFormat::kBc3;
  out.textures[1].mip_count = 7;

  const HitFileLayout layout = PlanHitFile(kSectors, material_count, texture_count);
  std::vector<uint8_t> tables(size_t(layout.blobs_offset - layout.table_offset), 0);
  std::vector<uint8_t> blobs;
  uint64_t blob_at = layout.blobs_offset;
  std::vector<HitSectorRecord> records(kSectors);
  out.hi.resize(kSectors);
  out.lod.resize(kSectors);
  const auto put_blob = [&](const HitBlob& blob, uint64_t* offset, uint32_t* size) {
    const std::vector<uint8_t> bytes = EncodeHitBlob(blob);
    while (blob_at % kHitSectionAlignment != 0) {
      blobs.push_back(0);
      ++blob_at;
    }
    *offset = blob_at;
    *size = uint32_t(bytes.size());
    blobs.insert(blobs.end(), bytes.begin(), bytes.end());
    blob_at += bytes.size();
  };
  for (uint32_t i = 0; i < kSectors; ++i) {
    HitSectorRecord& r = records[i];
    r.name_hash = base.sectors[i].name_hash;
    r.hi_triangles = base.sectors[i].hi_triangles;
    r.lod_triangles = base.sectors[i].lod_triangles;
    if (r.hi_triangles != 0) {
      out.hi[i] = MakeBlob(rng, r.hi_triangles);
      r.hi_runs = uint32_t(out.hi[i].runs.size());
      put_blob(out.hi[i], &r.hi_offset, &r.hi_size);
    }
    if (r.lod_triangles != 0 && i != 2) {  // sector 2 has no hit blob for its lod although the base has the mesh
      out.lod[i] = MakeBlob(rng, r.lod_triangles);
      r.lod_runs = uint32_t(out.lod[i].runs.size());
      put_blob(out.lod[i], &r.lod_offset, &r.lod_size);
    }
    ByteWriter w;
    r.Write(w);
    std::memcpy(tables.data() + size_t(i) * kHitSectorRecordBytes, w.bytes().data(), w.size());
  }
  for (uint32_t m = 0; m < material_count; ++m) {
    ByteWriter w;
    out.materials[m].Write(w);
    std::memcpy(tables.data() + size_t(layout.materials_offset - layout.table_offset) + size_t(m) * kHitMaterialBytes, w.bytes().data(), w.size());
  }
  for (uint32_t t = 0; t < texture_count; ++t) {
    ByteWriter w;
    out.textures[t].Write(w);
    std::memcpy(tables.data() + size_t(layout.textures_offset - layout.table_offset) + size_t(t) * kHitTextureBytes, w.bytes().data(), w.size());
  }
  HitHeader h;
  h.sector_count = kSectors;
  h.material_count = material_count;
  h.texture_count = texture_count;
  h.base_table_hash = base.table_hash;
  h.base_file_size = base.file_size;
  h.table_offset = layout.table_offset;
  h.materials_offset = layout.materials_offset;
  h.textures_offset = layout.textures_offset;
  h.blobs_offset = layout.blobs_offset;
  h.file_size = layout.blobs_offset + blobs.size();
  h.table_hash = Fnv1a64(tables.data(), tables.size());
  ByteWriter hw;
  h.Write(hw);
  out.file = hw.bytes();
  out.file.insert(out.file.end(), tables.begin(), tables.end());
  out.file.insert(out.file.end(), blobs.begin(), blobs.end());
  return out;
}

void WriteFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path scratch = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::temp_directory_path() / "larecomp-hit-reader-test";
  std::filesystem::create_directories(scratch);
  const std::filesystem::path file = scratch / "mcla_city_hit.v1.pack";
  const BaseIdentity base = MakeBase();
  const Built built = BuildFile(base);
  WriteFile(file, built.file);

  HitReader reader;
  std::string error;
  Check(reader.Open(file, base, &error), "a whole hit file opens: " + error);
  Check(reader.is_open() && reader.sector_count() == kSectors && reader.material_count() == 3 && reader.textures().size() == 2, "the counts");
  Check(reader.material_words().size() == 24, "eight words a material");
  // the material table is the records' own eight words
  bool words_ok = true;
  for (uint32_t m = 0; m < 3; ++m) {
    uint32_t expect[8];
    built.materials[m].Pack(expect);
    for (int k = 0; k < 8; ++k) words_ok = words_ok && reader.material_words()[size_t(m) * 8 + size_t(k)] == expect[k];
  }
  Check(words_ok, "the material words are HitMaterial::Pack's");
  Check(WordClass(reader.material_words()[8]) == 5 && WordSmoothness(reader.material_words()[8]) == 200 && WordMetal(reader.material_words()[8]) == 10, "material 1's word");
  Check(reader.textures()[0].key == 0xAAAA && reader.textures()[0].width == 256 && reader.textures()[1].format == HitTextureFormat::kBc3 && reader.textures()[1].mip_count == 7, "the texture records");

  // the blobs
  for (uint32_t i = 0; i < kSectors; ++i) {
    HitBlob hi, lod;
    const bool has_hi = base.sectors[i].hi_triangles != 0;
    const bool has_lod = base.sectors[i].lod_triangles != 0 && i != 2;
    error.clear();
    const bool got_hi = reader.ReadBlob(i, MeshKind::kHi, &hi, &error);
    Check(got_hi == has_hi && error.empty(), "sector " + std::to_string(i) + ": the hi blob is there when the base has the mesh");
    if (got_hi) {
      Check(hi.triangle_count == base.sectors[i].hi_triangles && hi.runs.size() == built.hi[i].runs.size() && hi.corners == built.hi[i].corners && hi.opaque_triangles == built.hi[i].opaque_triangles,
            "sector " + std::to_string(i) + ": the hi blob is what was written");
    }
    error.clear();
    const bool got_lod = reader.ReadBlob(i, MeshKind::kLod, &lod, &error);
    Check(got_lod == has_lod && error.empty(), "sector " + std::to_string(i) + ": the lod blob is there when it was written");
    if (got_lod) Check(lod.corners == built.lod[i].corners && lod.runs.size() == built.lod[i].runs.size(), "sector " + std::to_string(i) + ": the lod blob is what was written");
  }
  HitBlob none;
  Check(!reader.ReadBlob(kSectors, MeshKind::kHi, &none, &error) && !error.empty(), "a sector that does not exist is an error");

  // what it refuses
  {
    BaseIdentity other = base;
    other.table_hash ^= 1;
    HitReader r;
    Check(!r.Open(file, other, &error) && error.find("another city pack") != std::string::npos, "a file built over another pack is refused: " + error);
    other = base;
    other.file_size += 1;
    Check(!r.Open(file, other, &error), "another size of the base pack is refused");
    other = base;
    other.sectors[3].name_hash ^= 1;
    Check(!r.Open(file, other, &error) && error.find("another name") != std::string::npos, "a sector of another name is refused: " + error);
    other = base;
    other.sectors[1].hi_triangles += 1;
    Check(!r.Open(file, other, &error) && error.find("triangle count") != std::string::npos, "a sector of another triangle count is refused: " + error);
    other = base;
    other.sectors.pop_back();
    Check(!r.Open(file, other, &error), "a base pack of fewer sectors is refused");
    other = base;
    other.sectors[2].lod_triangles = 0;  // the base has no lod mesh, the file has no lod blob for it either: fine
    Check(r.Open(file, other, &error) == false, "(a changed triangle count is refused even where the file holds no blob)");
    Check(!r.Open(scratch / "missing.pack", base, &error) && error.find("cannot be opened") != std::string::npos, "a file that is not there");
  }
  {
    // a cut file, a damaged table, another magic, a blob for a mesh the base lacks
    std::vector<uint8_t> cut(built.file.begin(), built.file.end() - 5);
    WriteFile(scratch / "cut.pack", cut);
    HitReader r;
    Check(!r.Open(scratch / "cut.pack", base, &error) && error.find("size its header says") != std::string::npos, "a cut file is refused: " + error);
    std::vector<uint8_t> damaged = built.file;
    damaged[kHitHeaderBytes + 3] ^= 0x40;  // a byte of the sector table
    WriteFile(scratch / "damaged.pack", damaged);
    Check(!r.Open(scratch / "damaged.pack", base, &error) && error.find("damaged") != std::string::npos, "a damaged table is refused: " + error);
    std::vector<uint8_t> magic = built.file;
    magic[0] = 'X';
    WriteFile(scratch / "magic.pack", magic);
    Check(!r.Open(scratch / "magic.pack", base, &error), "another magic is refused: " + error);
    WriteFile(scratch / "short.pack", std::vector<uint8_t>(40, 0));
    Check(!r.Open(scratch / "short.pack", base, &error) && error.find("shorter") != std::string::npos, "a file shorter than its header is refused: " + error);
  }
  {
    // a blob that is damaged is found when it is read, not when the file is opened
    std::vector<uint8_t> damaged = built.file;
    HitReader good;
    Check(good.Open(file, base, &error), "reopened");
    const uint64_t at = good.sector(1).hi_offset + good.sector(1).hi_size - 3;
    damaged[size_t(at)] ^= 0x55;
    WriteFile(scratch / "blob.pack", damaged);
    HitReader r;
    Check(r.Open(scratch / "blob.pack", base, &error), "a file with one damaged blob still opens (the tables are whole)");
    HitBlob blob;
    Check(!r.ReadBlob(1, MeshKind::kHi, &blob, &error) && !error.empty(), "the damaged blob is refused when it is read: " + error);
    Check(r.ReadBlob(0, MeshKind::kHi, &blob, &error), "and the others still read");
  }
  std::error_code ec;
  std::filesystem::remove_all(scratch, ec);
  std::printf("hit_reader_test: %d checks, %d failed\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
