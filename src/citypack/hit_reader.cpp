#include "citypack/hit_reader.h"

#include <fstream>

namespace larecomp::citypack {

BaseIdentity IdentityOf(const PackReader& base) {
  BaseIdentity id;
  id.table_hash = base.header().table_hash;
  id.file_size = base.header().file_size;
  id.sectors.reserve(base.sector_count());
  for (uint32_t i = 0; i < base.sector_count(); ++i) {
    const SectorRecord& s = base.sector(i);
    id.sectors.push_back({s.name_hash, s.has_hi() ? s.hi_triangles : 0u, s.has_lod() ? s.lod_triangles : 0u});
  }
  return id;
}

bool HitReader::Open(const std::filesystem::path& file, const BaseIdentity& base, std::string* error) {
  open_ = false;
  sectors_.clear();
  materials_.clear();
  textures_.clear();
  path_ = file;
  const auto fail = [&](std::string why) {
    if (error) *error = std::move(why);
    return false;
  };
  std::ifstream in(file, std::ios::binary);
  if (!in) return fail("the city hit file " + file.string() + " cannot be opened");
  std::vector<uint8_t> head(kHitHeaderBytes);
  in.read(reinterpret_cast<char*>(head.data()), std::streamsize(head.size()));
  if (size_t(in.gcount()) != head.size()) return fail("the city hit file is shorter than its header");
  ByteReader hr(head.data(), head.size());
  std::string why;
  if (!HitHeader::Read(hr, &header_, &why)) return fail(why);

  in.seekg(0, std::ios::end);
  const uint64_t file_size = uint64_t(in.tellg());
  if (header_.file_size != file_size) return fail("the city hit file is not the size its header says (cut short, or still being written)");
  if (header_.sector_count != base.sectors.size() || header_.base_table_hash != base.table_hash || header_.base_file_size != base.file_size) {
    return fail("the city hit file was built over another city pack than the one beside it (rebuild it)");
  }
  const uint64_t table_end = header_.blobs_offset;
  if (header_.table_offset < kHitHeaderBytes || table_end < header_.table_offset || table_end > file_size) return fail("the city hit file's tables run past its end");
  const uint64_t sector_bytes = uint64_t(header_.sector_count) * kHitSectorRecordBytes;
  const uint64_t material_bytes = uint64_t(header_.material_count) * kHitMaterialBytes;
  const uint64_t texture_bytes = uint64_t(header_.texture_count) * kHitTextureBytes;
  if (header_.table_offset + sector_bytes > table_end || header_.materials_offset < header_.table_offset || header_.materials_offset + material_bytes > table_end ||
      header_.textures_offset < header_.table_offset || header_.textures_offset + texture_bytes > table_end) {
    return fail("the city hit file's sections overlap or run past the tables' end");
  }
  if (header_.material_count == 0 || header_.material_count > kMaxMaterials || header_.texture_count > kMaxTextures) return fail("the city hit file's material or texture count is out of range");

  std::vector<uint8_t> tables(size_t(table_end - header_.table_offset));
  in.seekg(std::streamoff(header_.table_offset));
  in.read(reinterpret_cast<char*>(tables.data()), std::streamsize(tables.size()));
  if (!in) return fail("the city hit file's tables could not be read");
  if (Fnv1a64(tables.data(), tables.size()) != header_.table_hash) return fail("the city hit file's tables do not match their hash (the file is damaged)");

  ByteReader sr(tables.data(), tables.size());
  sectors_.resize(header_.sector_count);
  for (uint32_t i = 0; i < header_.sector_count; ++i) {
    HitSectorRecord& s = sectors_[i];
    if (!HitSectorRecord::Read(sr, &s)) return fail("the city hit file's sector table is cut short");
    const BaseIdentity::Sector& b = base.sectors[i];
    if (s.name_hash != b.name_hash) return fail("sector " + std::to_string(i) + " of the city hit file is not the base pack's sector of that place (another name)");
    if (s.hi_triangles != b.hi_triangles || s.lod_triangles != b.lod_triangles) return fail("sector " + std::to_string(i) + " of the city hit file has another triangle count than the base pack's");
    if ((s.hi_size != 0 && s.hi_offset + s.hi_size > file_size) || (s.lod_size != 0 && s.lod_offset + s.lod_size > file_size)) return fail("a hit blob is outside the city hit file");
    if ((s.hi_size != 0 && b.hi_triangles == 0) || (s.lod_size != 0 && b.lod_triangles == 0)) return fail("sector " + std::to_string(i) + " has a hit blob for a mesh the base pack does not have");
  }
  ByteReader mr(tables.data(), tables.size());
  mr.Seek(size_t(header_.materials_offset - header_.table_offset));
  materials_.resize(size_t(header_.material_count) * 8);
  for (uint32_t& w : materials_) w = mr.U32();
  ByteReader tr(tables.data(), tables.size());
  tr.Seek(size_t(header_.textures_offset - header_.table_offset));
  textures_.resize(header_.texture_count);
  for (HitTexture& t : textures_) {
    if (!HitTexture::Read(tr, &t)) return fail("the city hit file's texture table is cut short");
  }
  if (!mr.ok() || !tr.ok()) return fail("the city hit file's material or texture table is cut short");
  open_ = true;
  return true;
}

bool HitReader::ReadBlob(uint32_t sector, MeshKind kind, HitBlob* out, std::string* error) const {
  if (error) error->clear();
  if (!open_ || sector >= sectors_.size()) {
    if (error) *error = "no such sector";
    return false;
  }
  const HitSectorRecord& s = sectors_[sector];
  const bool hi = kind == MeshKind::kHi;
  const uint64_t offset = hi ? s.hi_offset : s.lod_offset;
  const uint32_t size = hi ? s.hi_size : s.lod_size;
  if (size == 0) return false;  // not an error: the file holds no hit blob for that mesh
  std::ifstream in(path_, std::ios::binary);
  if (!in) {
    if (error) *error = "the city hit file cannot be opened";
    return false;
  }
  std::vector<uint8_t> bytes(size);
  in.seekg(std::streamoff(offset));
  in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size));
  if (!in) {
    if (error) *error = "a blob of the city hit file could not be read";
    return false;
  }
  if (!DecodeHitBlob(bytes.data(), bytes.size(), out, error)) return false;
  if (out->triangle_count != (hi ? s.hi_triangles : s.lod_triangles)) {
    if (error) *error = "a hit blob's triangle count is not its record's";
    return false;
  }
  return true;
}

}  // namespace larecomp::citypack
