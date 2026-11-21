#include "citypack/pack_reader.h"

#include <fstream>

namespace larecomp::citypack {

bool PackReader::Open(const std::filesystem::path& file, std::string* error) {
  open_ = false;
  sectors_.clear();
  names_.clear();
  path_ = file;
  const auto fail = [&](std::string why) {
    if (error) *error = std::move(why);
    return false;
  };
  std::ifstream in(file, std::ios::binary);
  if (!in) return fail("the city pack " + file.string() + " cannot be opened");
  std::vector<uint8_t> head(kHeaderBytes);
  in.read(reinterpret_cast<char*>(head.data()), std::streamsize(head.size()));
  if (size_t(in.gcount()) != head.size()) return fail("the city pack is shorter than its header");
  ByteReader hr(head.data(), head.size());
  std::string why;
  if (!PackHeader::Read(hr, &header_, &why)) return fail(why);

  in.seekg(0, std::ios::end);
  const uint64_t file_size = uint64_t(in.tellg());
  if (header_.file_size != file_size) return fail("the city pack is not the size its header says (cut short, or still being written)");
  const uint64_t table_bytes = uint64_t(header_.sector_count) * kSectorRecordBytes;
  if (header_.table_offset + table_bytes > file_size || header_.names_offset + header_.names_bytes > file_size) return fail("the city pack's table runs past its end");

  std::vector<uint8_t> table(static_cast<size_t>(table_bytes));
  in.seekg(std::streamoff(header_.table_offset));
  in.read(reinterpret_cast<char*>(table.data()), std::streamsize(table.size()));
  names_.resize(header_.names_bytes);
  in.seekg(std::streamoff(header_.names_offset));
  in.read(names_.data(), std::streamsize(names_.size()));
  if (!in) return fail("the city pack's table could not be read");
  uint64_t hash = Fnv1a64(table.data(), table.size());
  hash = Fnv1a64(names_.data(), names_.size(), hash);
  if (hash != header_.table_hash) return fail("the city pack's table does not match its hash (the file is damaged)");

  ByteReader tr(table.data(), table.size());
  sectors_.resize(header_.sector_count);
  for (SectorRecord& s : sectors_) {
    if (!SectorRecord::Read(tr, &s)) return fail("the city pack's table is cut short");
    if (uint64_t(s.name_offset) + s.name_length > names_.size()) return fail("a sector's name is outside the name heap");
    if ((s.has_hi() && s.hi_offset + s.hi_size > file_size) || (s.has_lod() && s.lod_offset + s.lod_size > file_size)) return fail("a sector's blob is outside the file");
  }
  open_ = true;
  return true;
}

std::string_view PackReader::name(uint32_t index) const {
  const SectorRecord& s = sectors_[index];
  return std::string_view(names_).substr(s.name_offset, s.name_length);
}

bool PackReader::ReadBlob(uint32_t sector, MeshKind kind, std::vector<uint8_t>* out, std::string* error) const {
  if (error) error->clear();
  if (!open_ || sector >= sectors_.size()) {
    if (error) *error = "no such sector";
    return false;
  }
  const SectorRecord& s = sectors_[sector];
  const bool hi = kind == MeshKind::kHi;
  if (hi ? !s.has_hi() : !s.has_lod()) return false;  // not an error: the sector has no such mesh
  const uint64_t offset = hi ? s.hi_offset : s.lod_offset;
  const uint32_t size = hi ? s.hi_size : s.lod_size;
  std::ifstream in(path_, std::ios::binary);
  if (!in) {
    if (error) *error = "the city pack cannot be opened";
    return false;
  }
  out->resize(size);
  in.seekg(std::streamoff(offset));
  in.read(reinterpret_cast<char*>(out->data()), std::streamsize(size));
  if (!in) {
    if (error) *error = "a blob of the city pack could not be read";
    return false;
  }
  return true;
}

bool PackReader::ReadMesh(uint32_t sector, MeshKind kind, MeshData* out, std::string* error) const {
  std::vector<uint8_t> blob;
  if (!ReadBlob(sector, kind, &blob, error)) return false;
  return DecodeMesh(blob.data(), blob.size(), out, error);
}

uint32_t ValidatePack(const PackReader& reader, std::vector<std::string>* problems) {
  uint32_t count = 0;
  const auto note = [&](uint32_t sector, const char* what) {
    ++count;
    if (problems && problems->size() < 20) problems->push_back(std::string(reader.name(sector)) + ": " + what);
  };
  for (uint32_t i = 0; i < reader.sector_count(); ++i) {
    const SectorRecord& s = reader.sector(i);
    for (const MeshKind kind : {MeshKind::kHi, MeshKind::kLod}) {
      const bool hi = kind == MeshKind::kHi;
      if (hi ? !s.has_hi() : !s.has_lod()) continue;
      MeshData mesh;
      std::string error;
      if (!reader.ReadMesh(i, kind, &mesh, &error)) {
        note(i, error.empty() ? "a blob could not be read" : error.c_str());
        continue;
      }
      if (mesh.triangle_count() != (hi ? s.hi_triangles : s.lod_triangles) || mesh.vertex_count() != (hi ? s.hi_vertices : s.lod_vertices)) note(i, "a blob's counts are not its record's");
      if (hi && mesh.opaque_triangles != s.hi_opaque_triangles) note(i, "the opaque triangle count is not the record's");
      uint64_t in_runs = 0;
      for (const GeometryRun& run : mesh.runs) in_runs += run.triangle_count;
      if (in_runs != mesh.triangle_count()) note(i, "the runs do not cover the triangles");
    }
  }
  return count;
}

}  // namespace larecomp::citypack
