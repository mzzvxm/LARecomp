// BadassBaboon's Recomp Adjustments: RAGE Jenkins Hash (atStringHash) and Symbol Resolver
// Provides zero-overhead asset and symbol diagnostics derived from CodeX (146,000+ resolved strings).

#pragma once

#include <cstdint>
#include <string_view>
#include <string>
#include <unordered_map>
#include <fstream>
#include <mutex>
#include <vector>
#include <cstdlib>

namespace rage {

// ---------------------------------------------------------------------------
// RAGE Jenkins One-At-A-Time Hash (atStringHash)
// Computes canonical RAGE 32-bit hash with lowercase and slash normalization.
// ---------------------------------------------------------------------------

constexpr inline uint32_t atStringHashConst(std::string_view str) {
  uint32_t hash = 0;
  for (char c : str) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c + ('a' - 'A'));
    } else if (c == '\\') {
      c = '/';
    }
    hash += static_cast<uint8_t>(c);
    hash += (hash << 10);
    hash ^= (hash >> 6);
  }
  hash += (hash << 3);
  hash ^= (hash >> 11);
  hash += (hash << 15);
  return hash;
}

inline uint32_t atStringHash(const char* str) {
  if (!str) return 0;
  uint32_t hash = 0;
  while (*str) {
    char c = *str++;
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c + ('a' - 'A'));
    } else if (c == '\\') {
      c = '/';
    }
    hash += static_cast<uint8_t>(c);
    hash += (hash << 10);
    hash ^= (hash >> 6);
  }
  hash += (hash << 3);
  hash ^= (hash >> 11);
  hash += (hash << 15);
  return hash;
}

// ---------------------------------------------------------------------------
// Embedded Core RAGE Engine & Shader Hashes
// Instant O(1) lookups for standard shader samplers, textures, and tables.
// ---------------------------------------------------------------------------

inline const char* ResolveWellKnownHash(uint32_t hash) {
  switch (hash) {
    case atStringHashConst("diffusesampler"): return "diffusesampler";
    case atStringHashConst("bumpmapsampler"): return "bumpmapsampler";
    case atStringHashConst("specularsampler"): return "specularsampler";
    case atStringHashConst("reflectsampler"): return "reflectsampler";
    case atStringHashConst("neonsampler"): return "neonsampler";
    case atStringHashConst("wavefoamsampler"): return "wavefoamsampler";
    case atStringHashConst("depthtexture0"): return "depthtexture0";
    case atStringHashConst("racestartgridmapping.tbl"): return "racestartgridmapping.tbl";
    case atStringHashConst("arrestsafe.zones"): return "arrestsafe.zones";
    case atStringHashConst("common.xtd"): return "common.xtd";
    case atStringHashConst("city.xct"): return "city.xct";
    case atStringHashConst("traffic.xtbl"): return "traffic.xtbl";
    case atStringHashConst("handling.xml"): return "handling.xml";
    case atStringHashConst("vehicles.xml"): return "vehicles.xml";
    case atStringHashConst("damage.xml"): return "damage.xml";
    case atStringHashConst("camera.xml"): return "camera.xml";
    case atStringHashConst("hud.xtd"): return "hud.xtd";
    case atStringHashConst("fonts.xtd"): return "fonts.xtd";
    default: return nullptr;
  }
}

// ---------------------------------------------------------------------------
// Diagnostic Symbol Resolver (Lazy On-Demand)
// Enabled only when MCLA_RESOLVE_SYMBOLS=1 or REX_LOG_LEVEL=debug is active.
// ---------------------------------------------------------------------------

class SymbolResolver {
 public:
  static SymbolResolver& Instance() {
    static SymbolResolver instance;
    return instance;
  }

  const char* Resolve(uint32_t hash) {
    // 1. Fast path: check well-known embedded symbols first
    if (const char* known = ResolveWellKnownHash(hash)) {
      return known;
    }

    // 2. If diagnostics are disabled, return null with zero overhead
    if (!enabled_) {
      return nullptr;
    }

    // 3. Thread-safe lookup in dynamically loaded dictionary
    std::lock_guard<std::mutex> lock(mutex_);
    EnsureLoaded();
    auto it = hash_map_.find(hash);
    if (it != hash_map_.end()) {
      return it->second.c_str();
    }
    return nullptr;
  }

  std::string Format(uint32_t hash) {
    if (const char* name = Resolve(hash)) {
      char buf[256];
      std::snprintf(buf, sizeof(buf), "%s (0x%08X)", name, hash);
      return std::string(buf);
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%08X", hash);
    return std::string(buf);
  }

 private:
  SymbolResolver() {
    const char* env = std::getenv("MCLA_RESOLVE_SYMBOLS");
    const char* log = std::getenv("REX_LOG_LEVEL");
    enabled_ = (env && std::string_view(env) == "1") ||
               (log && std::string_view(log) == "debug");
  }

  void EnsureLoaded() {
    if (loaded_ || !enabled_) return;
    loaded_ = true;

    // Search candidate dictionary file paths.
    //
    // MCLA_STRINGS_FILE takes priority so this works on any machine. The
    // remaining entries are relative to the working directory (the build dir),
    // walking outwards toward a sibling CodeX checkout. There is deliberately
    // no absolute path here - the previous "E:/MCLA/..." entry only ever
    // resolved on one developer's machine.
    std::vector<std::string> candidate_paths;
    if (const char* p = std::getenv("MCLA_STRINGS_FILE")) {
      if (*p) candidate_paths.emplace_back(p);
    }
    for (const char* rel : {
             "Codex.Games.MCLA.strings.txt",
             "user_data/mcla_strings.txt",
             "../CodeX.Games.MCLA/Codex.Games.MCLA.strings.txt",
             "../../CodeX.Games.MCLA/Codex.Games.MCLA.strings.txt",
             "../../../CodeX.Games.MCLA/Codex.Games.MCLA.strings.txt",
             "../../../../CodeX.Games.MCLA/Codex.Games.MCLA.strings.txt"}) {
      candidate_paths.emplace_back(rel);
    }

    for (const std::string& path : candidate_paths) {
      std::ifstream infile(path);
      if (infile.is_open()) {
        std::string line;
        while (std::getline(infile, line)) {
          // Strip whitespace and carriage return
          while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
          }
          if (!line.empty()) {
            uint32_t h = atStringHash(line.c_str());
            hash_map_.emplace(h, std::move(line));
          }
        }
        break;
      }
    }
  }

  bool enabled_ = false;
  bool loaded_ = false;
  std::mutex mutex_;
  std::unordered_map<uint32_t, std::string> hash_map_;
};

inline const char* ResolveJenkinsHash(uint32_t hash) {
  return SymbolResolver::Instance().Resolve(hash);
}

inline std::string FormatJenkinsHash(uint32_t hash) {
  return SymbolResolver::Instance().Format(hash);
}

} // namespace rage
