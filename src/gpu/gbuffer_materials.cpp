// MCLA GPU renderer -- the G-buffer's per-texture material table: the runtime's glue (the texture names, the texture cache, the log). See gbuffer_materials.h.

#include "gbuffer_materials.h"

#include <string>

#include <rex/logging/log_category.h>
#include <rex/platform/file.h>

#include "mc_engine/modloader/modloader.h"
#include "os/environment.h"
#include "texture_names.h"
#include "textures.h"

namespace mcla::gpu::gbuffer {

namespace {

// Frames a name lookup is kept after the registry of names changed (the registry changes with every texture the game streams in: asking again at each change would be a lookup per
// texture per frame). A page whose texture was replaced by another is wrong for at most this long.
constexpr uint64_t kRefreshFrames = 30;

std::string EnvText(const char* name) {
  return larecomp::os::ReadEnvironment(name).value_or("");
}

}  // namespace

void TextureMaterials::Prepare(const std::filesystem::path& pack_folder, uint64_t frame) {
  if (!initialized_) {
    if (pack_folder.empty()) {
      return;  // the shader store has no pack yet: no G-buffer draw either
    }
    initialized_ = true;
    std::vector<std::filesystem::path> sources =
        TextureTableSources(pack_folder, mc::modloader::ModRootDir(rex::platform::GetExecutableFolder()), EnvText(kTextureTableEnvironment));
    const std::string census = EnvText(kCensusEnvironment);
    census_on_ = !census.empty() && census != "0";
    std::string listing;
    for (const std::filesystem::path& source : sources) listing += (listing.empty() ? "" : ", ") + source.string();
    REXLOG_INFO("[gbuffer] texture materials: rows are read from {} (each looked at again every {} frames); census {}", listing.empty() ? std::string("no file (switched off)") : listing,
                TextureTableFiles::kPollFrames, census_on_ ? "ON (every 300 frames, 'texcensus' lines)" : "off");
    files_.SetSources(std::move(sources));
  }
  TextureTableFiles::Report report;
  if (files_.Poll(frame, &report)) {
    if (report.files != 0 || report.errors != 0) {
      REXLOG_INFO("[gbuffer] texture materials: {} rows from {} file(s), {} bad row(s)", report.rows, report.files, report.errors);
    }
    for (const std::string& line : report.lines) {
      REXLOG_WARN("[gbuffer] texture materials: {}", line);
    }
  }
}

TextureMaterials::Hit TextureMaterials::Resolve(TextureCache& textures, const std::filesystem::path& pack_folder, const TextureKey& key, uint64_t frame) {
  Hit hit;
  Prepare(pack_folder, frame);
  if (!initialized_) {
    return hit;
  }
  if (census_on_ && frame != last_frame_) {
    if (census_.WindowDue(frame)) {
      for (const std::string& line : census_.TakeLines(frame)) {
        REXLOG_INFO("[gbuffer] {}", line);
      }
    } else if (census_.empty()) {
      census_.StartWindow(frame);  // a window starts with its first draw
    }
  }
  last_frame_ = frame;
  const bool have_rows = !files_.table().empty();
  if (!have_rows && !census_on_) {
    return hit;  // nothing to look up and nothing to count: a draw costs nothing here
  }
  const uint32_t page = key.base_page != 0 ? key.base_page : key.mip_page;
  if (page == 0) {
    return hit;
  }
  if (files_.epoch() != cache_epoch_) {
    cache_.clear();
    cache_epoch_ = files_.epoch();
  }
  const uint64_t generation = texture_names::Generation();
  const auto found = cache_.find(page);
  if (found != cache_.end()) {
    const CacheEntry& c = found->second;
    const bool fresh = c.settled ? (c.generation == generation || frame - c.frame < kRefreshFrames) : c.frame == frame;
    if (fresh) {
      hit.row = c.row;
      hit.census = c.census;
      return hit;
    }
  }
  texture_names::Name name;
  const bool named = texture_names::Lookup(page, &name);
  // The content hash keys the unnamed textures (most of the city's): wanted by a table that has hash rows and by the census, which lists it for every texture. The cache has none for
  // a texture's very first draw (it binds the texture after this), so such an answer is only kept for the frame.
  const bool wants_hash = census_on_ || files_.table().has_hash_rows();
  const uint64_t content_hash = wants_hash ? textures.ContentHashOf(key) : 0;
  CacheEntry entry;
  if (const TextureRow* row = files_.table().Find(named ? std::string_view(name.dictionary) : std::string_view(), named ? std::string_view(name.texture) : std::string_view(), content_hash)) {
    entry.row = *row;
  }
  entry.frame = frame;
  entry.generation = generation;
  entry.settled = !wants_hash || content_hash != 0;
  if (census_on_) {
    entry.census = census_.Intern(page, named ? std::string_view(name.dictionary) : std::string_view("unknown"), named ? std::string_view(name.texture) : std::string_view("unnamed"));
    census_.Describe(entry.census, content_hash, key.width, key.height, key.format);
  }
  cache_[page] = entry;
  hit.row = entry.row;
  hit.census = entry.census;
  return hit;
}

uint32_t TextureMaterials::Finish(uint32_t word, const Hit& hit, uint64_t ps_key, uint32_t indices) {
  const uint32_t result = ApplyTextureRow(word, hit.row);
  if (hit.census != kNoCensus) {
    census_.Note(hit.census, ps_key, result, indices, hit.row.fields != 0);
  }
  return result;
}

}  // namespace mcla::gpu::gbuffer
