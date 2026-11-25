#include "citypack/texture_set.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#include "citypack/bytes.h"
#include "os/file_replace.h"
#include "citypack/guest_hash.h"
#include "citypack/sector_parse.h"
#include "mc_engine/modloader/rpf3.h"

namespace larecomp::citypack {

namespace {

double SecondsSince(std::chrono::steady_clock::time_point start) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); }

// The archives, searched the way the game does: the mod overlay first, then the shipped one (the same as the city pack's builder).
struct Archives {
  mc::modloader::Rpf3Reader source;
  mc::modloader::Rpf3Reader mods;
  bool has_mods = false;
  bool Read(const std::string& path, std::vector<uint8_t>* file) const {
    mc::modloader::Rpf3Entry entry;
    if (has_mods && mods.Find(path, entry) && !entry.is_directory() && mods.ReadFile(entry, *file)) return true;
    return source.Find(path, entry) && !entry.is_directory() && source.ReadFile(entry, *file);
  }
};

// What makes two dictionary entries one picture.
struct Signature {
  uint64_t key = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
  uint32_t levels = 0;
  uint32_t gamma = 0;
  bool operator==(const Signature& o) const { return key == o.key && width == o.width && height == o.height && format == o.format && levels == o.levels && gamma == o.gamma; }
};
struct SignatureHash {
  size_t operator()(const Signature& s) const {
    uint64_t h = s.key;
    h ^= (uint64_t(s.width) << 40) ^ (uint64_t(s.height) << 20) ^ (uint64_t(s.format) << 10) ^ (uint64_t(s.levels) << 4) ^ s.gamma;
    h *= 0x9E3779B97F4A7C15ull;
    return size_t(h ^ (h >> 29));
  }
};

struct Entry {
  DecodedTexture decoded;
  uint64_t content = 0;          // what the decoded levels are (blocks, size, format, levels, flags): equal contents are one picture
  uint32_t index = 0xFFFFFFFFu;  // the table's, assigned in the single threaded pass
};

uint64_t ContentOf(const DecodedTexture& d) {
  uint64_t h = HashGuestBytes(d.data.data(), d.data.size());
  const uint64_t shape = (uint64_t(d.width) << 48) ^ (uint64_t(d.height) << 32) ^ (uint64_t(d.mip_count) << 24) ^ (uint64_t(d.format) << 16) ^ uint64_t(d.flags);
  h ^= shape * 0x9E3779B97F4A7C15ull;
  return (h * 0xFF51AFD7ED558CCDull) ^ (h >> 31);
}

struct SourceResult {
  bool failed = false;
  DictionaryStats dictionary;
  uint32_t undecodable = 0;
  std::vector<std::pair<uint32_t, Entry*>> names;  // (name hash, picture) in the dictionary's order; null: the entry's picture could not be keyed
};

std::string SourcePath(uint32_t source, const std::vector<SectorInfo>& sectors) {
  if (source == kDictionaryCommon) return kCommonDictionaryPath;
  if (source == kDictionaryLod) return kLodDictionaryPath;
  return SectorArchivePath(sectors[source - kDictionarySectorFirst].name);
}

}  // namespace

ResolvedTexture TextureSet::ResolveExactEntry(uint32_t dictionary, uint32_t name_hash) const {
  ResolvedTexture r;
  const auto it = names.find((uint64_t(dictionary) << 32) | name_hash);
  if (it == names.end()) return r;
  r.index = it->second.index;
  r.key = it->second.key;
  r.dictionary = dictionary;
  return r;
}

ResolvedTexture TextureSet::ResolveEntry(uint32_t dictionary, uint32_t name_hash) const {
  ResolvedTexture r = ResolveExactEntry(dictionary, name_hash);
  if (r.found()) return r;
  if (dictionary >= kDictionarySectorFirst) {
    r = ResolveExactEntry(kDictionaryCommon, name_hash);
    return r.found() ? r : ResolveExactEntry(kDictionaryLod, name_hash);
  }
  return ResolveExactEntry(dictionary == kDictionaryLod ? kDictionaryCommon : kDictionaryLod, name_hash);
}

bool TextureSet::ReadChain(uint32_t index, std::vector<uint8_t>* chain, std::string* error) const {
  if (index >= textures.size() || index >= blobs.size()) {
    if (error) *error = "no such texture";
    return false;
  }
  std::vector<uint8_t> scratch;
  return UnpackTextureChain(textures[index], blobs[index].data(), blobs[index].size(), 0, chain, &scratch, error);
}

std::vector<uint32_t> TextureSet::PicturesOf(uint32_t dictionary) const {
  std::vector<uint32_t> out;
  std::vector<char> seen(textures.size(), 0);
  for (const auto& entry : dictionary_order) {
    if (entry.first != dictionary) continue;
    const uint32_t index = ResolveExact(dictionary, entry.second);
    if (index != kNoTexture && !seen[index]) {
      seen[index] = 1;
      out.push_back(index);
    }
  }
  return out;
}

TextureSetResult BuildTextureSet(const TextureSetRequest& request, ProgressSink* sink_in) {
  const auto started = std::chrono::steady_clock::now();
  ProgressSink null_sink;
  ProgressSink& sink = sink_in ? *sink_in : null_sink;
  TextureSetResult result;
  const auto fail = [&](const std::string& why) {
    result.ok = false;
    result.error = why;
    result.stats.seconds = SecondsSince(started);
    return std::move(result);
  };
  if (!request.lzx) return fail("no LZX decoder was given");

  Archives archives;
  if (!archives.source.Open(request.source_rpf)) return fail("the game archive " + request.source_rpf.string() + " could not be opened as an RPF archive");
  if (!request.mods_rpf.empty() && std::filesystem::exists(request.mods_rpf)) archives.has_mods = archives.mods.Open(request.mods_rpf);

  std::vector<uint8_t> file;
  std::string error;
  std::vector<SectorInfo> sectors;
  {
    RscImage table_image;
    if (!archives.Read(kCityTablePath, &file)) return fail("city.xct is not in the archive");
    if (!DecodeResourceFile(file, request.lzx, &table_image, &error)) return fail(std::string("city.xct: ") + error);
    if (!ParseCityTable(table_image, &sectors, &error)) return fail(error);
  }
  const uint32_t sector_count = uint32_t(sectors.size());
  const uint32_t source_count = kDictionarySectorFirst + sector_count;
  result.set.sector_count = sector_count;
  result.stats.sectors = sector_count;
  result.stats.sources = source_count;

  // The sources: workers read, decode and key them; the pictures are shared through one map (the first worker to meet a picture decodes it, the others only point at it).
  std::mutex map_mutex;
  std::unordered_map<Signature, std::unique_ptr<Entry>, SignatureHash> pictures;
  std::vector<SourceResult> results(source_count);
  std::vector<char> done(source_count, 0);
  std::mutex done_mutex;
  std::condition_variable done_cv;
  std::atomic<uint32_t> next{0};
  std::atomic<bool> stop{false};
  const uint32_t hardware = std::max(1u, std::thread::hardware_concurrency());
  const uint32_t threads = std::clamp(request.threads != 0 ? request.threads : (hardware > 1 ? hardware - 1 : 1), 1u, 64u);

  const auto process = [&](uint32_t source) {
    SourceResult& r = results[source];
    std::vector<uint8_t> bytes;
    RscImage image;
    std::string why;
    if (!archives.Read(SourcePath(source, sectors), &bytes) || !DecodeResourceFile(bytes, request.lzx, &image, &why)) {
      r.failed = true;
      return;
    }
    std::vector<DictionaryTexture> list;
    const bool ok = source >= kDictionarySectorFirst ? ParseSectorDictionary(image, &list, &r.dictionary, &why) : ParseDictionaryFile(image, &list, &r.dictionary, &why);
    if (!ok) {
      r.failed = true;
      return;
    }
    r.names.reserve(list.size());
    for (const DictionaryTexture& t : list) {
      uint64_t key = 0;
      if (!CityTextureKey(image, t, &key)) {
        ++r.undecodable;
        r.names.push_back({t.name_hash, nullptr});
        continue;
      }
      Signature sig;
      sig.key = key;
      sig.width = t.desc.width;
      sig.height = t.desc.height;
      sig.format = t.desc.format;
      sig.levels = t.desc.level_count();
      sig.gamma = t.desc.gamma ? 1u : 0u;
      Entry* entry = nullptr;
      bool is_new = false;
      {
        std::lock_guard lock(map_mutex);
        std::unique_ptr<Entry>& slot = pictures[sig];
        if (!slot) {
          slot = std::make_unique<Entry>();
          is_new = true;
        }
        entry = slot.get();
      }
      if (is_new) {
        entry->decoded = DecodeCityTexture(image, t);
        if (entry->decoded.ok) entry->content = ContentOf(entry->decoded);
      }
      r.names.push_back({t.name_hash, entry});
    }
  };

  sink.BeginPhase("city textures", int(source_count));
  std::vector<std::thread> workers;
  for (uint32_t w = 0; w < threads; ++w) {
    workers.emplace_back([&] {
      if (request.worker_start) request.worker_start();
      for (;;) {
        const uint32_t i = next.fetch_add(1);
        if (i >= source_count || stop.load()) return;
        process(i);
        {
          std::lock_guard lock(done_mutex);
          done[i] = 1;
        }
        done_cv.notify_all();
      }
    });
  }
  for (uint32_t i = 0; i < source_count; ++i) {
    {
      std::unique_lock lock(done_mutex);
      done_cv.wait(lock, [&] { return done[i] != 0; });
    }
    sink.Step(i < kDictionarySectorFirst ? (i == 0 ? "common.xtd" : "lod_0.xtd") : sectors[i - kDictionarySectorFirst].name);
    if (sink.Cancelled()) {
      stop.store(true);
      result.cancelled = true;
      break;
    }
  }
  for (std::thread& t : workers) t.join();
  if (result.cancelled) {
    result.stats.seconds = SecondsSince(started);
    return result;
  }

  // Single threaded, in the sources' order: the table, the names, the data. A picture is one content; the entries that decode to the same content share a table row, each keeping its key.
  TextureSetStats& stats = result.stats;
  TextureSet& set = result.set;
  std::unordered_map<uint32_t, uint32_t> first_by_name;  // name hash -> the first picture any dictionary gave it
  std::unordered_map<uint64_t, uint32_t> by_content;     // content -> table index
  std::vector<Entry*> table_entries;                     // the entry whose data a table row holds
  for (uint32_t source = 0; source < source_count; ++source) {
    SourceResult& r = results[source];
    if (r.failed) {
      ++stats.sources_failed;
      continue;
    }
    stats.dictionary.Merge(r.dictionary);
    stats.entries += r.dictionary.textures;
    stats.decode_failed += r.undecodable;
    for (const auto& [name_hash, entry] : r.names) {
      if (!entry) continue;
      if (!entry->decoded.ok) {
        ++stats.decode_failed;
        continue;
      }
      if (entry->index == 0xFFFFFFFFu) {
        ++stats.key_variants;
        const auto [cit, fresh] = by_content.try_emplace(entry->content, uint32_t(table_entries.size()));
        if (fresh) {
          if (table_entries.size() >= kMaxTextures) return fail("the texture set has more pictures than a 16 bit texture index holds");
          table_entries.push_back(entry);
        } else {
          stats.duplicate_bytes += entry->decoded.data.size();
          std::vector<uint8_t>().swap(entry->decoded.data);  // the same picture as an earlier entry: only its key is kept
        }
        entry->index = cit->second;
      }
      const uint64_t name_key = (uint64_t(source) << 32) | name_hash;
      const auto [it, inserted] = set.names.try_emplace(name_key, TextureSet::Entry{entry->index, entry->decoded.key});
      if (!inserted) {
        if (it->second.index != entry->index) ++stats.name_conflicts;
        continue;
      }
      set.dictionary_order.push_back({source, name_hash});
      const auto [fit, first] = first_by_name.try_emplace(name_hash, entry->index);
      if (first) ++stats.distinct_names;
      else if (fit->second != entry->index) ++stats.cross_conflicts;
    }
  }
  if (table_entries.empty()) return fail("no texture could be read from the city's dictionaries");

  const uint32_t picture_count = uint32_t(table_entries.size());
  set.textures.resize(picture_count);
  set.average.resize(picture_count);
  set.alpha_mean.resize(picture_count);
  set.blobs.resize(picture_count);
  for (uint32_t i = 0; i < picture_count; ++i) {
    const DecodedTexture& d = table_entries[i]->decoded;
    HitTexture& t = set.textures[i];
    t.key = d.key;
    t.width = uint16_t(d.width);
    t.height = uint16_t(d.height);
    t.format = d.format;
    t.mip_count = uint8_t(d.mip_count);
    t.flags = uint8_t(d.flags);
    t.data_size = uint32_t(d.data.size());
    set.average[i] = {d.average[0], d.average[1], d.average[2]};
    set.alpha_mean[i] = d.alpha_mean;
    ++stats.by_format[size_t(d.format)];
    if (d.mip_count == 1) ++stats.single_level;
    if ((d.flags & kTextureAlpha) != 0) ++stats.with_alpha;
    if ((d.flags & kTextureGamma) != 0) ++stats.gamma;
    if (d.damaged_levels != 0) ++stats.damaged;
  }

  // The chains as the file holds them: each picture compressed on its own (workers; the same bytes whatever the thread count), then laid out in the table's order.
  const bool zstd = request.zstd_level > 0;
  std::vector<std::vector<uint8_t>> stored(zstd ? picture_count : 0);
  if (zstd) {
    const auto compress_started = std::chrono::steady_clock::now();
    sink.BeginPhase("city texture files", int(picture_count));
    std::vector<char> compressed(picture_count, 0);
    std::vector<char> compress_failed(picture_count, 0);
    std::mutex compress_mutex;
    std::condition_variable compress_cv;
    std::atomic<uint32_t> next_picture{0};
    std::atomic<bool> stop_compress{false};
    std::vector<std::thread> pool;
    for (uint32_t w = 0; w < threads; ++w) {
      pool.emplace_back([&] {
        if (request.worker_start) request.worker_start();
        for (;;) {
          const uint32_t i = next_picture.fetch_add(1);
          if (i >= picture_count || stop_compress.load()) return;
          std::vector<uint8_t>& data = table_entries[i]->decoded.data;
          const bool ok = CompressTextureChain(set.textures[i], data.data(), request.zstd_level, &stored[i]);
          std::vector<uint8_t>().swap(data);  // the frame (or the chain, copied) is the picture now
          {
            std::lock_guard lock(compress_mutex);
            compressed[i] = 1;
            compress_failed[i] = ok ? 0 : 1;
          }
          compress_cv.notify_all();
        }
      });
    }
    bool broke = false;
    for (uint32_t i = 0; i < picture_count; ++i) {
      {
        std::unique_lock lock(compress_mutex);
        compress_cv.wait(lock, [&] { return compressed[i] != 0; });
      }
      if (i % 256 == 255 || i + 1 == picture_count) sink.Step(std::to_string(i + 1) + " of " + std::to_string(picture_count));
      if (compress_failed[i] != 0) {
        broke = true;
        stop_compress.store(true);
        break;
      }
      if (sink.Cancelled()) {
        stop_compress.store(true);
        result.cancelled = true;
        break;
      }
    }
    for (std::thread& t : pool) t.join();
    if (broke) return fail("a texture could not be compressed");
    if (result.cancelled) {
      result.stats.seconds = SecondsSince(started);
      return result;
    }
    stats.compress_seconds = SecondsSince(compress_started);
    stats.zstd_level = request.zstd_level;
  }
  uint64_t offset = 0;
  uint64_t plain_offset = 0;
  for (uint32_t i = 0; i < picture_count; ++i) {
    HitTexture& t = set.textures[i];
    offset = (offset + kTextureDataAlignment - 1) / kTextureDataAlignment * kTextureDataAlignment;
    plain_offset = (plain_offset + kTextureDataAlignment - 1) / kTextureDataAlignment * kTextureDataAlignment;
    t.data_offset = kTexHeaderBytes + offset;
    plain_offset += t.data_size;
    if (zstd) {
      set.blobs[i] = std::move(stored[i]);
      if (set.blobs[i].size() != t.data_size) {
        t.stored_size = uint32_t(set.blobs[i].size());  // a frame; 0 (the default) means the chain as it is
        ++stats.zstd_pictures;
      }
    } else {
      set.blobs[i] = std::move(table_entries[i]->decoded.data);  // the table owns it now
    }
    offset += set.blobs[i].size();
  }
  set.zstd = stats.zstd_pictures != 0;
  set.data_bytes = offset;
  stats.plain_bytes = plain_offset;
  stats.distinct = picture_count;
  stats.data_bytes = offset;
  stats.seconds = SecondsSince(started);
  result.ok = true;
  return result;
}

bool WriteTextureFile(const std::filesystem::path& path, const TextureSet& set, uint64_t hit_table_hash, std::string* error) {
  const auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return false;
  };
  TexHeader header;
  header.texture_count = uint32_t(set.textures.size());
  header.flags = set.zstd ? kTexFlagZstd : 0;
  header.hit_table_hash = hit_table_hash;
  header.data_offset = kTexHeaderBytes;
  header.file_size = kTexHeaderBytes + set.data_bytes;
  ByteWriter head;
  header.Write(head);
  std::error_code ec;
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
  const std::filesystem::path temp = os::TempPathFor(path);  // unique per process: parallel runs build the same file in one cache (file_replace.h)
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) return fail("the texture file could not be created: " + temp.string());
    out.write(reinterpret_cast<const char*>(head.bytes().data()), std::streamsize(head.size()));
    uint64_t written = kTexHeaderBytes;
    static const char zeros[kTextureDataAlignment] = {};
    for (size_t i = 0; i < set.textures.size(); ++i) {
      const uint64_t at = set.textures[i].data_offset;
      if (at > written) out.write(zeros, std::streamsize(at - written));  // the 16 byte alignment
      out.write(reinterpret_cast<const char*>(set.blobs[i].data()), std::streamsize(set.blobs[i].size()));
      written = at + set.blobs[i].size();
    }
    out.flush();
    if (!out) {
      out.close();
      std::filesystem::remove(temp, ec);
      return fail("the texture file could not be written completely (disk full?)");
    }
  }
  // One step over the old file, tried again while another process has it open; the same bytes already there (another process built it first) count as done.
  std::string why;
  if (os::MoveIntoPlace(temp, path, &why) == os::MoveResult::kFailed) return fail("the texture file could not be moved into place: " + why);
  return true;
}

bool TextureFileReader::Open(const std::filesystem::path& path, const std::vector<HitTexture>& table, uint64_t expected_hit_table_hash, std::string* error) {
  open_ = false;
  in_.reset();
  const auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return false;
  };
  auto in = std::make_unique<std::ifstream>(path, std::ios::binary);
  if (!*in) return fail("the texture file " + path.string() + " could not be opened");
  uint8_t raw[kTexHeaderBytes];
  in->read(reinterpret_cast<char*>(raw), sizeof(raw));
  if (in->gcount() != std::streamsize(sizeof(raw))) return fail("the texture file is shorter than its header");
  ByteReader r(raw, sizeof(raw));
  TexHeader header;
  std::string why;
  if (!TexHeader::Read(r, &header, &why)) return fail(why);
  std::error_code ec;
  const uint64_t actual = std::filesystem::file_size(path, ec);
  if (ec || actual != header.file_size) return fail("the texture file's size is not the one its header says (cut or replaced)");
  if ((header.flags & ~kTexFlagsKnown) != 0) return fail("the texture file has an encoding this build does not know (another version)");
  if (header.texture_count != table.size()) return fail("the texture file and the hit file's texture table have different counts (another build)");
  if (header.hit_table_hash != expected_hit_table_hash) return fail("the texture file belongs to another hit file (its table hash differs)");
  for (const HitTexture& t : table) {
    if (t.data_offset < header.data_offset || t.data_offset + t.StoredBytes() > header.file_size || t.data_size != t.ChainBytes(0) || t.stored_size > t.data_size) {
      return fail("a texture record points outside the texture file or has the wrong size");
    }
    if (t.IsCompressed() && (header.flags & kTexFlagZstd) == 0) return fail("a texture record says it is compressed in a file that says it is not");
  }
  path_ = path;
  table_ = table;
  file_size_ = header.file_size;
  in_ = std::move(in);
  open_ = true;
  return true;
}

bool TextureFileReader::ReadMips(uint32_t index, uint32_t first_mip, std::vector<uint8_t>* out, std::string* error) {
  const auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return false;
  };
  if (!open_ || index >= table_.size()) return fail("no such texture");
  const HitTexture& t = table_[index];
  if (first_mip >= t.mip_count) return fail("no such mip level");
  in_->clear();
  if (t.IsCompressed()) {  // one frame: read it whole, unpack the levels from first_mip on
    stored_.resize(size_t(t.stored_size));
    in_->seekg(std::streamoff(t.data_offset), std::ios::beg);
    in_->read(reinterpret_cast<char*>(stored_.data()), std::streamsize(stored_.size()));
    if (in_->gcount() != std::streamsize(stored_.size())) return fail("the texture file was cut short");
    return UnpackTextureChain(t, stored_.data(), stored_.size(), first_mip, out, &scratch_, error);
  }
  const uint64_t bytes = t.ChainBytes(first_mip);
  const uint64_t at = t.data_offset + t.MipOffset(first_mip);
  out->resize(size_t(bytes));
  in_->seekg(std::streamoff(at), std::ios::beg);
  in_->read(reinterpret_cast<char*>(out->data()), std::streamsize(bytes));
  if (in_->gcount() != std::streamsize(bytes)) return fail("the texture file was cut short");
  return true;
}

uint32_t ValidateTextureFile(const std::filesystem::path& path, const std::vector<HitTexture>& table, uint64_t expected_hit_table_hash, std::vector<std::string>* problems) {
  uint32_t count = 0;
  const auto note = [&](const std::string& text) {
    ++count;
    if (problems && problems->size() < 12) problems->push_back(text);
  };
  TextureFileReader reader;
  std::string error;
  if (!reader.Open(path, table, expected_hit_table_hash, &error)) {
    note(error);
    return count;
  }
  std::vector<uint8_t> bytes;
  uint64_t covered = 0;
  for (uint32_t i = 0; i < table.size(); ++i) {
    if (!reader.ReadMips(i, 0, &bytes, &error)) {
      note("texture " + std::to_string(i) + ": " + error);
      continue;
    }
    covered += table[i].StoredBytes();  // the file's bytes, not the unpacked chain's
    if (table[i].mip_count == 0 || table[i].width == 0 || table[i].height == 0) note("texture " + std::to_string(i) + " has no levels or no size");
  }
  if (covered > reader.file_size()) note("the textures cover more than the file");
  return count;
}

}  // namespace larecomp::citypack
