#include "citypack/stamp.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "citypack/pack_format.h"

namespace larecomp::citypack {

namespace {
std::string Trim(std::string_view text) {
  size_t b = 0, e = text.size();
  while (b < e && (text[b] == ' ' || text[b] == '\t' || text[b] == '\r')) ++b;
  while (e > b && (text[e - 1] == ' ' || text[e - 1] == '\t' || text[e - 1] == '\r')) --e;
  return std::string(text.substr(b, e - b));
}
}  // namespace

void Stamp::Set(std::string_view key, std::string value) {
  for (StampLine& line : lines_) {
    if (line.key == key) {
      line.value = std::move(value);
      return;
    }
  }
  lines_.push_back({std::string(key), std::move(value)});
}

void Stamp::Set(std::string_view key, uint64_t value) { Set(key, std::to_string(value)); }

void Stamp::SetHex(std::string_view key, uint64_t value) {
  char text[24];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
  Set(key, std::string(text));
}

const std::string* Stamp::Get(std::string_view key) const {
  for (const StampLine& line : lines_) {
    if (line.key == key) return &line.value;
  }
  return nullptr;
}

bool Stamp::GetU64(std::string_view key, uint64_t* value) const {
  const std::string* text = Get(key);
  if (!text || text->empty()) return false;
  uint64_t v = 0;
  for (const char c : *text) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + uint64_t(c - '0');
  }
  *value = v;
  return true;
}

bool Stamp::GetHex(std::string_view key, uint64_t* value) const {
  const std::string* text = Get(key);
  if (!text || text->size() != 16) return false;
  uint64_t v = 0;
  for (const char c : *text) {
    uint64_t d = 0;
    if (c >= '0' && c <= '9') d = uint64_t(c - '0');
    else if (c >= 'a' && c <= 'f') d = uint64_t(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') d = uint64_t(c - 'A' + 10);
    else return false;
    v = (v << 4) | d;
  }
  *value = v;
  return true;
}

std::string Stamp::ToText() const {
  std::string text;
  for (const StampLine& line : lines_) {
    text += line.key;
    text += '=';
    text += line.value;
    text += '\n';
  }
  return text;
}

Stamp Stamp::Parse(std::string_view text) {
  Stamp stamp;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string_view::npos) end = text.size();
    const std::string line = Trim(text.substr(pos, end - pos));
    pos = end + 1;
    if (line.empty() || line[0] == '#') continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    stamp.Set(Trim(std::string_view(line).substr(0, eq)), Trim(std::string_view(line).substr(eq + 1)));
  }
  return stamp;
}

bool Stamp::Read(const std::filesystem::path& path, Stamp* out, std::string* error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (error) *error = "the stamp " + path.string() + " cannot be opened";
    return false;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  *out = Parse(buffer.str());
  return true;
}

bool Stamp::Write(const std::filesystem::path& path, std::string* error) const {
  std::filesystem::path temp = path;
  temp += ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
      if (error) *error = "the stamp " + temp.string() + " cannot be written";
      return false;
    }
    const std::string text = ToText();
    out.write(text.data(), std::streamsize(text.size()));
    if (!out) {
      if (error) *error = "the stamp " + temp.string() + " could not be written completely";
      return false;
    }
  }
  std::error_code ec;
  std::filesystem::rename(temp, path, ec);
  if (ec) {
    if (error) *error = "the stamp could not be moved into place: " + ec.message();
    return false;
  }
  return true;
}

FileStat StatFile(const std::filesystem::path& path) {
  FileStat stat;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec) return stat;
  const uint64_t size = std::filesystem::file_size(path, ec);
  if (ec) return stat;
  const auto time = std::filesystem::last_write_time(path, ec);
  if (ec) return stat;
  stat.exists = true;
  stat.size = size;
  stat.mtime_us = std::chrono::duration_cast<std::chrono::microseconds>(time.time_since_epoch()).count();
  return stat;
}

Stamp MakeStamp(const StampInputs& in) {
  Stamp stamp;
  stamp.Set("stamp_format", uint64_t(kStampFormat));
  stamp.Set("builder_version", uint64_t(kBuilderVersion));
  stamp.Set("pack_format_version", uint64_t(kPackFormatVersion));
  stamp.Set("source_size", in.source.exists ? in.source.size : 0);
  stamp.Set("source_mtime_us", in.source.exists ? std::to_string(in.source.mtime_us) : std::string("0"));
  stamp.Set("mods_size", in.mods.exists ? in.mods.size : 0);
  stamp.Set("mods_mtime_us", in.mods.exists ? std::to_string(in.mods.mtime_us) : std::string("0"));
  stamp.Set("drop_mask", uint64_t(in.drop_mask));
  stamp.Set("positions", uint64_t(in.positions));
  stamp.Set("pack_size", in.pack_size);
  stamp.SetHex("pack_hash", in.pack_hash);
  stamp.Set("sectors", uint64_t(in.sectors));
  stamp.Set("hi_triangles", in.hi_triangles);
  stamp.Set("lod_triangles", in.lod_triangles);
  stamp.Set("app_version", in.app_version);
  return stamp;
}

StampCheck CheckStamp(const std::filesystem::path& out_dir, const StampInputs& expect) {
  StampCheck check;
  const auto no = [&](std::string why) {
    check.current = false;
    check.why = std::move(why);
    return check;
  };
  Stamp stamp;
  std::string error;
  if (!Stamp::Read(out_dir / kStampFileName, &stamp, &error)) return no("there is no stamp: the pack was never built here");
  const auto number = [&](const char* key, uint64_t want) {
    uint64_t have = 0;
    return stamp.GetU64(key, &have) && have == want;
  };
  if (!number("stamp_format", kStampFormat)) return no("the stamp's format is not this build's");
  if (!number("builder_version", kBuilderVersion)) return no("the pack was built by another version of the builder");
  if (!number("pack_format_version", kPackFormatVersion)) return no("the pack is another format version");
  const FileStat pack = StatFile(out_dir / kPackFileName);
  uint64_t pack_size = 0;
  if (!pack.exists || !stamp.GetU64("pack_size", &pack_size) || pack.size != pack_size) return no("the pack file is missing or is not the size the stamp says");
  if (!number("source_size", expect.source.exists ? expect.source.size : 0)) return no("the game archive has another size than the pack was built from");
  if (stamp.Get("source_mtime_us") == nullptr || *stamp.Get("source_mtime_us") != (expect.source.exists ? std::to_string(expect.source.mtime_us) : std::string("0")))
    return no("the game archive has another time than the pack was built from");
  if (!number("mods_size", expect.mods.exists ? expect.mods.size : 0)) return no("the mod archive has changed since the pack was built");
  if (stamp.Get("mods_mtime_us") == nullptr || *stamp.Get("mods_mtime_us") != (expect.mods.exists ? std::to_string(expect.mods.mtime_us) : std::string("0")))
    return no("the mod archive has changed since the pack was built");
  if (!number("drop_mask", expect.drop_mask)) return no("the pack leaves out other classes than were asked for");
  if (!number("positions", expect.positions)) return no("the pack has another position encoding than was asked for");
  check.current = true;
  return check;
}

}  // namespace larecomp::citypack
