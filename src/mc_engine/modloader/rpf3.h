// RPF3 archive reader/writer for MCLA.
//
// Layout was recovered from fiPackfile::Init (sub_821CD7A0) and
// fiPackfile::FindEntry (sub_821CBFC0), then verified byte for byte against
// the shipped xarchive_cache.rpf:
//
//   header (20 bytes, little endian, offset 0)
//     +0  magic 'RPF3'   (RPF0..RPF3 are all accepted by the engine)
//     +4  toc size in bytes
//     +8  entry count
//     +12 0
//     +16 0xFFFFFFFF
//
//   toc (offset 2048, `toc size` bytes, AES-256 ECB decrypted sixteen times)
//     entry[count], 16 bytes each, then the name heap (only the root's "/"
//     actually lives there -- every other lookup goes through the hash).
//
//   file entry     { hash, size, (offset / 2048) << 11 | resourceType, flag }
//   directory entry{ hash, 0,    0x80000000 | firstChild,               childCount }
//
// Children of a directory are contiguous and sorted ascending by hash, because
// FindEntry binary-searches them. `flag` is the same word the RSC5 header
// carries: bit31 marks a resource, bit30 marks LZX compression, and the
// remaining bits encode the virtual/physical segment sizes.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mc::modloader {

// rage's string hash (sub_821C9790): lowercases A-Z, folds '\' to '/'.
uint32_t RageHash(std::string_view text);

struct Rpf3Entry {
    uint32_t hash = 0;
    uint32_t size = 0;
    uint32_t offset_type = 0;
    uint32_t flag = 0;

    bool is_directory() const { return (offset_type & 0x80000000u) != 0; }
    uint32_t first_child() const { return offset_type & 0x7FFFFFFFu; }
    uint32_t child_count() const { return flag & 0x3FFFFFFFu; }
    uint64_t data_offset() const { return static_cast<uint64_t>(offset_type >> 11) * 2048ull; }
    uint32_t resource_type() const { return offset_type & 0x7FFu; }
};

// Read-only view over an existing archive; only what the template extractor
// needs (open, resolve a path, pull the raw bytes out).
class Rpf3Reader {
 public:
    bool Open(const std::filesystem::path& path);

    // `path` is archive-relative, e.g. "resources/character/x/x.xrsc".
    bool Find(std::string_view path, Rpf3Entry& out) const;

    // Raw on-disk bytes of a file entry (still LZX-compressed for resources).
    bool ReadFile(const Rpf3Entry& entry, std::vector<uint8_t>& out) const;

    const std::filesystem::path& path() const { return path_; }

 private:
    std::filesystem::path path_;
    std::vector<Rpf3Entry> entries_;
};

// Builds a fresh archive from a set of resource files held in memory.
class Rpf3Writer {
 public:
    // `path` is archive-relative and uses '/' separators. `data` is the exact
    // payload the engine will stream: for an uncompressed resource that is the
    // virtual segment followed by the physical segment, and `flag` must have
    // bit30 clear.
    void Add(std::string path, std::vector<uint8_t> data, uint32_t flag, uint32_t resource_type);

    bool Write(const std::filesystem::path& out_path) const;

    bool empty() const { return files_.empty(); }
    size_t size() const { return files_.size(); }

 private:
    struct PendingFile {
        std::string path;
        std::vector<uint8_t> data;
        uint32_t flag = 0;
        uint32_t resource_type = 0;
    };

    std::vector<PendingFile> files_;
};

}  // namespace mc::modloader
