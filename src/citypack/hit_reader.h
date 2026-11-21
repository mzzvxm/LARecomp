#pragma once
// ===========================================================================
// LARecomp city hit data -- the runtime's reader of mcla_city_hit.v1.pack (CITY-RT M2 package H3)
// ===========================================================================
// city_hit_format.h is the frozen file format and writes/reads each piece; H1's builder writes the file. This is the other end, the one the renderer's streamer uses: Open reads the
// header, the three tables (sectors, materials, textures) and checks them against the BASE PACK the file must sit beside (the hash of the base pack's table, its size, the sectors'
// name hashes and triangle counts: a hit file built over another pack is refused, not misread); ReadBlob reads and decodes ONE sector detail's hit blob and may be called from any
// thread (each call opens the file itself, as PackReader::ReadMesh does: nothing is kept open, so the file can be replaced while the game runs and the next Open sees the new one).
//
// The base pack is given as a BaseIdentity (what the check needs of it) so that the test makes one by hand; IdentityOf takes it from a PackReader.
// ===========================================================================

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "citypack/city_hit_format.h"
#include "citypack/pack_format.h"
#include "citypack/pack_reader.h"

namespace larecomp::citypack {

struct BaseIdentity {
  uint64_t table_hash = 0;  // PackHeader::table_hash
  uint64_t file_size = 0;   // PackHeader::file_size
  struct Sector {
    uint32_t name_hash = 0;
    uint32_t hi_triangles = 0;  // 0: no hi mesh
    uint32_t lod_triangles = 0;
  };
  std::vector<Sector> sectors;
};
BaseIdentity IdentityOf(const PackReader& base);

class HitReader {
 public:
  // False (with `error`, in plain words) for a file that is missing, is not a hit file, is cut short or damaged (the tables' hash), or does not belong to `base`.
  bool Open(const std::filesystem::path& file, const BaseIdentity& base, std::string* error);
  bool is_open() const { return open_; }
  const std::filesystem::path& path() const { return path_; }

  const HitHeader& header() const { return header_; }
  uint32_t sector_count() const { return uint32_t(sectors_.size()); }
  const HitSectorRecord& sector(uint32_t index) const { return sectors_[index]; }
  uint32_t material_count() const { return header_.material_count; }
  // The material table as the GPU reads it: eight words a material (HitMaterial::Pack), uploaded as they are. Material 0 is the default.
  const std::vector<uint32_t>& material_words() const { return materials_; }
  const std::vector<HitTexture>& textures() const { return textures_; }

  // The decoded hit blob of a sector's hi detail or lod. A sector without that blob: false with an empty `error`.
  bool ReadBlob(uint32_t sector, MeshKind kind, HitBlob* out, std::string* error) const;

 private:
  bool open_ = false;
  std::filesystem::path path_;
  HitHeader header_;
  std::vector<HitSectorRecord> sectors_;
  std::vector<uint32_t> materials_;
  std::vector<HitTexture> textures_;
};

}  // namespace larecomp::citypack
