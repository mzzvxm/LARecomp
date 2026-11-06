// LARecomp shader pack builder -- the stamp (see stamp.h and shaderpack/PIPELINE-PLAN.md 3.1).

#include "shaderpack/stamp.h"

#include <algorithm>
#include <chrono>
#include <charconv>
#include <format>
#include <fstream>
#include <system_error>

#include "shaderpack/builder.h"
#include "shaderpack/hash.h"
#include "shaderpack/hlsl_source.h"
#include "shaderpack/pack_format.h"
#include "shaderpack/pack_writer.h"

namespace larecomp::shaderpack {

namespace {

std::string PathText(const std::filesystem::path& path) {
  const auto u8 = path.u8string();
  return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// A stamp value is one line.
std::string OneLine(std::string value) {
  for (char& c : value) {
    if (c == '\n' || c == '\r') c = ' ';
  }
  return value;
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
  return text;
}

constexpr const char* kStampHeader =
    "# LARecomp shader pack stamp. Written last, after the packs: a pack is used only while every\n"
    "# line here still matches. Delete this file to make the next start rebuild the packs.\n";

}  // namespace

// ---------------------------------------------------------------------------
// Stamp
// ---------------------------------------------------------------------------

void Stamp::Set(std::string_view key, std::string value) {
  value = OneLine(std::move(value));
  for (StampLine& line : lines_) {
    if (line.key == key) {
      line.value = std::move(value);
      return;
    }
  }
  lines_.push_back(StampLine{std::string(key), std::move(value)});
}

void Stamp::Set(std::string_view key, uint64_t value) {
  Set(key, std::to_string(value));
}

void Stamp::SetHex(std::string_view key, uint64_t value) {
  Set(key, Hex64(value));
}

void Stamp::RemovePrefix(std::string_view prefix) {
  std::erase_if(lines_, [&](const StampLine& line) { return line.key.starts_with(prefix); });
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
  uint64_t parsed = 0;
  const auto [end, ec] = std::from_chars(text->data(), text->data() + text->size(), parsed);
  if (ec != std::errc() || end != text->data() + text->size()) return false;
  *value = parsed;
  return true;
}

bool Stamp::GetHex(std::string_view key, uint64_t* value) const {
  const std::string* text = Get(key);
  return text && ParseHex64(*text, value);
}

std::string Stamp::ToText() const {
  std::string text = kStampHeader;
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
  while (!text.empty()) {
    const size_t end = text.find('\n');
    std::string_view line = Trim(text.substr(0, end));
    text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
    if (line.empty() || line.front() == '#') continue;
    const size_t equals = line.find('=');
    if (equals == std::string_view::npos || equals == 0) continue;
    stamp.Set(Trim(line.substr(0, equals)), std::string(Trim(line.substr(equals + 1))));
  }
  return stamp;
}

bool Stamp::Read(const std::filesystem::path& path, Stamp* out, std::string* error) {
  *out = Stamp();
  std::error_code ec;
  const uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec) {
    *error = "no stamp (" + PathText(path) + ")";
    return false;
  }
  if (size > (1u << 20)) {
    *error = "the stamp is too large to be one";
    return false;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    *error = "cannot open the stamp " + PathText(path);
    return false;
  }
  std::string text(size_t(size), '\0');
  file.read(text.data(), std::streamsize(text.size()));
  if (size_t(file.gcount()) != text.size()) {
    *error = "cannot read the stamp " + PathText(path);
    return false;
  }
  *out = Parse(text);
  return true;
}

bool Stamp::Write(const std::filesystem::path& path, std::string* error) const {
  std::filesystem::path temporary = path;
  temporary += ".tmp";
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) {
      *error = "cannot create " + PathText(temporary);
      return false;
    }
    const std::string text = ToText();
    file.write(text.data(), std::streamsize(text.size()));
    file.flush();
    file.close();
    if (file.fail()) {
      *error = "cannot write " + PathText(temporary);
      std::error_code ec;
      std::filesystem::remove(temporary, ec);
      return false;
    }
  }
  if (!CommitFile(temporary, path, error)) {
    std::error_code ec;
    std::filesystem::remove(temporary, ec);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

// file_time_type's epoch and tick differ between standard libraries; the value is only ever
// compared with one this same build wrote, so the stamp is not portable between builds that use
// another library (the worst case is one rebuild: the game files are compared by content next).
FileStat StatFile(const std::filesystem::path& path) {
  FileStat stat;
  std::error_code ec;
  const uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec) return stat;
  const auto time = std::filesystem::last_write_time(path, ec);
  if (ec) return stat;
  stat.exists = true;
  stat.size = size;
  stat.mtime_us =
      std::chrono::duration_cast<std::chrono::microseconds>(time.time_since_epoch()).count();
  return stat;
}

bool HashFile(const std::filesystem::path& path, uint64_t* xxh3) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  Xxh3Stream hash;
  std::vector<char> buffer(1 << 20);
  while (file) {
    file.read(buffer.data(), std::streamsize(buffer.size()));
    const std::streamsize got = file.gcount();
    if (got > 0) hash.Update(buffer.data(), size_t(got));
  }
  if (!file.eof()) return false;
  *xxh3 = hash.Digest();
  return true;
}

std::string DxcCompilerFileName() {
#if defined(_WIN32)
  return "dxcompiler.dll";
#elif defined(__APPLE__)
  return "libdxcompiler.dylib";
#else
  return "libdxcompiler.so";
#endif
}

std::string DxcDilFileName() {
#if defined(_WIN32)
  return "dxil.dll";
#else
  return std::string();  // only the Windows build signs DXIL
#endif
}

// ---------------------------------------------------------------------------
// The main stamp
// ---------------------------------------------------------------------------

Stamp MakeMainStamp(const StampInputs& in) {
  Stamp stamp;
  stamp.Set("stamp_format", uint64_t(kStampFormat));
  stamp.Set("builder_version", uint64_t(kBuilderVersion));
  stamp.Set("pack_version", uint64_t(kPackVersion));
  stamp.Set("generator_revision", GeneratorRevision());
  stamp.SetHex("shader_common_xxh3", ShaderCommonHash());
  if (!in.dxc_version.empty()) stamp.Set("dxc_version", in.dxc_version);
  if (in.dxc_compiler.exists) {
    stamp.Set("dxcompiler_size", in.dxc_compiler.size);
    stamp.Set("dxcompiler_mtime", std::to_string(in.dxc_compiler.mtime_us));
    stamp.SetHex("dxcompiler_xxh3", in.dxc_compiler_xxh3);
  }
  if (in.dxc_dil.exists) {
    stamp.Set("dxil_size", in.dxc_dil.size);
    stamp.Set("dxil_mtime", std::to_string(in.dxc_dil.mtime_us));
    stamp.SetHex("dxil_xxh3", in.dxc_dil_xxh3);
  }
  stamp.Set("formats", uint64_t(in.formats));
  stamp.Set("xex_size", in.xex.size);
  stamp.Set("xex_mtime", std::to_string(in.xex.mtime_us));
  stamp.Set("rpf_size", in.rpf.size);
  stamp.Set("rpf_mtime", std::to_string(in.rpf.mtime_us));
  stamp.SetHex("rpf_shaders_xxh3", in.rpf_fingerprint);
  stamp.SetHex("xex_containers_xxh3", in.image_fingerprint);
  stamp.Set("pack_size", in.pack_size);
  stamp.SetHex("pack_xxh3", in.pack_xxh3);
  if (!in.app_version.empty()) stamp.Set("larecomp_version", in.app_version);
  return stamp;
}

// ---------------------------------------------------------------------------
// The check
// ---------------------------------------------------------------------------

namespace {

// One DLL against the stamp's <prefix>_size / _mtime / _xxh3 lines. A file that is not there passes:
// there is nothing to compare, the pack stays usable, and a build reports the missing dxc itself.
// A stamp without the lines passes only when `needed` is false (a SPIR-V only build has no dxil lines).
bool DllMatches(const Stamp& stamp, const std::string& prefix, const std::filesystem::path& file,
                bool needed, std::string* why) {
  const FileStat now = StatFile(file);
  if (!now.exists) return true;
  const std::string* size = stamp.Get(prefix + "_size");
  const std::string* mtime = stamp.Get(prefix + "_mtime");
  uint64_t stamped_hash = 0;
  if (!size || !mtime || !stamp.GetHex(prefix + "_xxh3", &stamped_hash)) {
    if (needed) *why = std::format("the stamp has no {} lines", prefix);
    return !needed;
  }
  if (*size == std::to_string(now.size) && *mtime == std::to_string(now.mtime_us)) return true;
  // The file moved on: a copy of the same DLL (new time) is fine, another DLL is not.
  uint64_t actual = 0;
  if (HashFile(file, &actual) && actual == stamped_hash) return true;
  *why = std::format("{} is not the one the packs were built with", PathText(file));
  return false;
}

}  // namespace

StampCheck CheckStamp(const std::filesystem::path& out_dir, uint32_t formats_needed,
                      const GameFilesQuick& files, const HdrStep* hdr_step) {
  StampCheck check;
  std::string error;
  if (!Stamp::Read(out_dir / kStampFileName, &check.stamp, &error)) {
    check.why = error;
    return check;
  }
  const Stamp& stamp = check.stamp;

  bool inputs_match = true;
  const auto stale = [&](std::string why) {
    if (inputs_match) check.why = std::move(why);
    inputs_match = false;
  };
  const auto equals = [&](std::string_view key, const std::string& expected) {
    const std::string* value = stamp.Get(key);
    if (!value || *value != expected) {
      stale(std::format("{} is {} in the stamp, expected {}", key, value ? *value : std::string("missing"),
                        expected));
    }
  };

  // The builder's own rules and the contract.
  equals("stamp_format", std::to_string(kStampFormat));
  equals("builder_version", std::to_string(kBuilderVersion));
  equals("pack_version", std::to_string(kPackVersion));
  equals("generator_revision", GeneratorRevision());
  equals("shader_common_xxh3", Hex64(ShaderCommonHash()));

  // The formats the pack carries.
  uint64_t stamped_formats = 0;
  if (!stamp.GetU64("formats", &stamped_formats)) stale("the stamp has no formats line");
  check.stamped_formats = uint32_t(stamped_formats);
  check.formats_ok = (check.stamped_formats & formats_needed) == formats_needed;

  // The pack itself.
  const FileStat pack = StatFile(out_dir / kPackFileName);
  uint64_t pack_size = 0;
  if (!pack.exists) {
    stale("the pack " + std::string(kPackFileName) + " is missing");
  } else if (!stamp.GetU64("pack_size", &pack_size) || pack_size != pack.size) {
    stale("the pack's size is not the one in the stamp");
  }

  // The game files: another size is another game; only a time that moved can be a copy.
  if (files.game_root.empty()) {
    stale("no game folder to compare with");
  } else {
    struct GameFile {
      const char* name;
      const char* size_key;
      const char* mtime_key;
    };
    for (const GameFile& game_file : {GameFile{"default.xex", "xex_size", "xex_mtime"},
                                      GameFile{"xarchive_cache.rpf", "rpf_size", "rpf_mtime"}}) {
      const FileStat now = StatFile(files.game_root / game_file.name);
      if (!now.exists) {
        stale(std::string(game_file.name) + " is missing from the game folder");
        continue;
      }
      const std::string* size = stamp.Get(game_file.size_key);
      const std::string* mtime = stamp.Get(game_file.mtime_key);
      if (!size || !mtime || *size != std::to_string(now.size)) {
        stale(std::string(game_file.name) + " is not the file the packs were built from (size)");
      } else if (*mtime != std::to_string(now.mtime_us)) {
        check.content_check_needed = true;
        if (check.why.empty()) check.why = std::string(game_file.name) + " has another time than in the stamp";
      }
    }
  }

  // The compiler and the library that signs its DXIL.
  if (!files.dxc_dir.empty()) {
    std::string why;
    if (!DllMatches(stamp, "dxcompiler", files.dxc_dir / DxcCompilerFileName(), true, &why)) stale(why);
    const std::string dil = DxcDilFileName();
    if (!dil.empty() && (check.stamped_formats & kDxil) != 0 &&
        !DllMatches(stamp, "dxil", files.dxc_dir / dil, true, &why)) {
      stale(why);
    }
  }

  check.inputs_match = inputs_match;
  check.main_ok = inputs_match && check.formats_ok && !check.content_check_needed;
  if (inputs_match && !check.formats_ok) check.why = "the pack lacks a format this start needs";

  // The HDR pack (judged whenever the main pack is the right one for these inputs, so that a build
  // can tell "only the HDR pack is stale" from "everything is").
  if (!inputs_match) {
    check.hdr_ok = false;
  } else if (!hdr_step) {
    check.hdr_ok = true;
  } else {
    bool hdr_ok = true;
    const auto hdr_stale = [&](std::string why) {
      if (hdr_ok) check.why = std::move(why);
      hdr_ok = false;
    };
    for (const StampLine& line : hdr_step->ExpectedStampLines()) {
      const std::string* value = stamp.Get(line.key);
      if (!value || *value != line.value) {
        hdr_stale(std::format("{} is {} in the stamp, expected {}", line.key,
                              value ? *value : std::string("missing"), line.value));
      }
    }
    uint64_t hdr_formats = 0;
    if (!stamp.GetU64(kStampHdrFormats, &hdr_formats) || (uint32_t(hdr_formats) & formats_needed) != formats_needed) {
      hdr_stale("the HDR pack lacks a format this start needs");
    }
    uint64_t main_xxh3 = 0;
    uint64_t pack_xxh3 = 0;
    if (!stamp.GetHex(kStampHdrMainPackXxh3, &main_xxh3) || !stamp.GetHex("pack_xxh3", &pack_xxh3) ||
        main_xxh3 != pack_xxh3) {
      hdr_stale("the HDR pack was built beside another main pack");
    }
    const FileStat hdr_pack = StatFile(out_dir / kHdrPackFileName);
    uint64_t hdr_size = 0;
    if (!hdr_pack.exists || !stamp.GetU64(kStampHdrPackSize, &hdr_size) || hdr_size != hdr_pack.size) {
      hdr_stale("the HDR pack is missing or is not the one in the stamp");
    }
    check.hdr_ok = hdr_ok;
  }
  return check;
}

}  // namespace larecomp::shaderpack
