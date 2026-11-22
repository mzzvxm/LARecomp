//
// The parsers and the JSON writer of online_tables_core.h. Pure: no game, no SDK.
//
#include "online_tables_core.h"

#include <algorithm>
#include <cstring>

namespace larecomp::online_tables {
namespace {

bool IsPtr(uint32_t ea) { return ea >= 0x10000u && ea < 0xFFFF0000u; }

uint16_t Be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
uint64_t Be64(const uint8_t* p) { return (uint64_t(Be32(p)) << 32) | Be32(p + 4); }

// A NUL-terminated string of at most `max` bytes. Whatever is readable up to the NUL counts; an unreadable byte ends it.
std::string ReadCStr(const GuestReader& mem, uint32_t addr, uint32_t max) {
  char buf[kPropertyTextMax + 1] = {};
  if (max > kPropertyTextMax) max = kPropertyTextMax;
  uint32_t got = 0;
  if (mem.Read(addr, buf, max)) {
    got = max;
  } else {
    // Near the end of a mapped range: byte by byte.
    while (got < max && mem.Read(addr + got, buf + got, 1)) ++got;
  }
  uint32_t n = 0;
  while (n < got && buf[n] != '\0') ++n;
  return std::string(buf, n);
}

}  // namespace

const char* StatsStatusText(StatsStatus status) {
  switch (status) {
    case StatsStatus::kOk: return "ok";
    case StatsStatus::kUnreadable: return "the stats resource could not be read";
    case StatsStatus::kNoXdbf: return "no XDBF resource at the expected address";
    case StatsStatus::kNoXvc2: return "the XDBF has no XVC2 section";
    case StatsStatus::kBadSharedView: return "an XVC2 shared view is not followed by its XPBM block";
  }
  return "unknown";
}

// XDBF: "XDBF", version, entry table length, entry count, free space table length, free space count (0x18 bytes), then the
// entries (18 bytes each: u16 section, u64 id, u32 offset, u32 length), the free space table (8 bytes each) and the data.
// The XVC2 entry is section 1, id "XVC2". Inside it (offsets from its start): magic, 8 bytes, u16 shared view count, then each
// shared view: u16 columns, u16 rows, 8 bytes, 32 bytes per column (+4 property id, +12 u16 attribute id, +16 aggregation
// word), 32 bytes per row (skipped), an "XPBM" block (20 bytes + 4 per u32 at +16); then u16 view count and 16 bytes per
// view (+0 view id, +8 u16 shared view index).
//
// Every count in the resource is the title's own data, but a damaged install could make one huge: the entry scan stops at
// kMaxXdbfEntries, and every read after the XVC2 entry is held inside that entry's own [start, start + length) (one place, `rd`),
// so no loop can walk on past the section. Anything the section does not hold is kUnreadable, as a cut memory is.
// CHECK: not run against the title's real XDBF (no game here). The bound trusts the XVC2 entry's length to cover the whole section, as the
// view loop below already did (`p + 16 <= end`); if the real length were short, the tables file would say "unreadable" (or list fewer
// views) where it listed them before.
StatsStatus ReadStatsSchema(const GuestReader& mem, uint32_t xdbf, StatsSchema* out) {
  if (out) *out = {};
  uint8_t head[0x18];
  if (!mem.Read(xdbf, head, sizeof(head))) return StatsStatus::kUnreadable;
  if (std::memcmp(head, "XDBF", 4) != 0) return StatsStatus::kNoXdbf;
  const uint32_t table_len = Be32(head + 8);
  const uint32_t entry_count = Be32(head + 12);
  const uint32_t free_len = Be32(head + 16);
  const uint64_t data = uint64_t(xdbf) + 0x18 + uint64_t(table_len) * 18 + uint64_t(free_len) * 8;

  uint64_t xvc2 = 0;
  uint32_t xvc2_len = 0;
  const uint32_t scan = std::min({entry_count, table_len, kMaxXdbfEntries});
  for (uint32_t i = 0; i < scan; ++i) {
    uint8_t e[18];
    const uint64_t at = uint64_t(xdbf) + 0x18 + uint64_t(i) * 18;  // 64 bits: a 32-bit product wraps around the address space
    if (at + sizeof(e) > 0xFFFFFFFFull || !mem.Read(static_cast<uint32_t>(at), e, sizeof(e))) return StatsStatus::kUnreadable;
    if (Be16(e) == 1 && Be64(e + 2) == 0x58564332ull) {  // section 1, "XVC2"
      xvc2 = data + Be32(e + 10);
      xvc2_len = Be32(e + 14);
      break;
    }
  }
  if (!xvc2 || xvc2 > 0xFFFFFFFFull) return StatsStatus::kNoXvc2;
  uint8_t magic[4];
  if (!mem.Read(static_cast<uint32_t>(xvc2), magic, 4) || std::memcmp(magic, "XVC2", 4) != 0) {
    return StatsStatus::kNoXvc2;
  }

  const uint64_t end = xvc2 + xvc2_len;
  uint64_t p = xvc2 + 12;
  // The only way the rest of the parser reads: inside the section the XDBF entry gave, and inside the 32-bit address space.
  auto rd = [&](uint64_t at, void* buf, uint32_t n) {
    return at >= xvc2 && at + n <= end && at + n <= 0xFFFFFFFFull && mem.Read(static_cast<uint32_t>(at), buf, n);
  };

  uint8_t two[2];
  if (!rd(p, two, 2)) return StatsStatus::kUnreadable;
  const uint16_t shared_count = Be16(two);
  p += 2;
  StatsSchema schema;
  for (uint16_t s = 0; s < shared_count && p < end; ++s) {
    uint8_t h[4];
    if (!rd(p, h, 4)) return StatsStatus::kUnreadable;
    const uint16_t columns = Be16(h), rows = Be16(h + 2);
    p += 12;
    // The columns and the rows must fit in the section: refused before any is read, so a count of 65535 costs nothing.
    if (p + 32ull * columns + 32ull * rows > end) return StatsStatus::kUnreadable;
    std::vector<StatsColumn> set;
    set.reserve(columns);
    for (uint16_t c = 0; c < columns; ++c, p += 32) {
      StatsColumn col;
      if (!rd(p, col.raw.data(), 32)) return StatsStatus::kUnreadable;
      col.prop = Be32(col.raw.data() + 4);
      col.attr = Be16(col.raw.data() + 12);
      col.agg = static_cast<uint8_t>(Be16(col.raw.data() + 16) & 0xFF);
      set.push_back(col);
    }
    p += 32ull * rows;
    uint8_t xpbm[20];
    if (!rd(p, xpbm, 20)) return StatsStatus::kUnreadable;
    if (std::memcmp(xpbm, "XPBM", 4) != 0) return StatsStatus::kBadSharedView;
    p += 20 + 4ull * Be32(xpbm + 16);
    if (p > end) return StatsStatus::kUnreadable;  // the block's words do not fit in the section
    schema.column_sets.push_back(std::move(set));
  }

  if (!rd(p, two, 2)) return StatsStatus::kUnreadable;
  const uint16_t view_count = Be16(two);
  p += 2;
  for (uint16_t v = 0; v < view_count && p + 16 <= end; ++v, p += 16) {
    StatsView view;
    if (!rd(p, view.raw.data(), 16)) return StatsStatus::kUnreadable;
    view.id = Be32(view.raw.data());
    const uint16_t set = Be16(view.raw.data() + 8);
    if (set >= schema.column_sets.size()) continue;  // the game's own reader skips these too
    view.column_set = set;
    schema.views.push_back(view);
  }
  if (out) *out = std::move(schema);
  return StatsStatus::kOk;
}

bool ReadPropertyTable(const GuestReader& mem, uint32_t table, std::vector<PropertyDef>* out) {
  if (out) out->clear();
  std::vector<PropertyDef> defs;
  bool first = true;
  for (int i = 0; i < kPropertyRecords; ++i) {
    uint8_t rec[kPropertyRecordSize];
    if (!mem.Read(table + uint32_t(i) * kPropertyRecordSize, rec, sizeof(rec))) return false;
    PropertyDef d;
    d.id = Be32(rec);
    if (first) {
      if (d.id == 0) return false;  // not filled in yet: GameMode, the first record, is always set once it is
      first = false;
    }
    const uint32_t name = Be32(rec + 4);
    if (IsPtr(name)) d.name = ReadCStr(mem, name, kPropertyTextMax);
    for (int k = 0; k < kPropertyValueSlots; ++k) {
      const uint32_t v = Be32(rec + 8 + k * 4);
      if (!IsPtr(v)) break;
      d.values.push_back(ReadCStr(mem, v, kPropertyTextMax));
    }
    // An unused record (no id yet, or the game's "none") says nothing a reader could use.
    if (d.id == 0 || d.id == 0xFFFFFFFFu) continue;
    defs.push_back(std::move(d));
  }
  if (defs.empty()) return false;
  if (out) *out = std::move(defs);
  return true;
}

// ------------------------------------------------------------------ JSON ---

namespace {

void AppendHex(std::string& out, const uint8_t* data, size_t n) {
  static const char* kDigits = "0123456789abcdef";
  for (size_t i = 0; i < n; ++i) {
    out.push_back(kDigits[data[i] >> 4]);
    out.push_back(kDigits[data[i] & 15]);
  }
}

// How many bytes the UTF-8 sequence at s[i] takes, 0 when it is not a valid one (overlongs, surrogates and anything above
// U+10FFFF included).
size_t Utf8Length(const std::string& s, size_t i) {
  const unsigned char c = static_cast<unsigned char>(s[i]);
  auto cont = [&](size_t k) { return i + k < s.size() && (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80; };
  if (c < 0x80) return 1;
  if (c >= 0xC2 && c <= 0xDF) return cont(1) ? 2 : 0;
  if (c >= 0xE0 && c <= 0xEF) {
    if (!cont(1) || !cont(2)) return 0;
    const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
    if (c == 0xE0 && c1 < 0xA0) return 0;   // overlong
    if (c == 0xED && c1 >= 0xA0) return 0;  // a surrogate
    return 3;
  }
  if (c >= 0xF0 && c <= 0xF4) {
    if (!cont(1) || !cont(2) || !cont(3)) return 0;
    const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
    if (c == 0xF0 && c1 < 0x90) return 0;   // overlong
    if (c == 0xF4 && c1 >= 0x90) return 0;  // above U+10FFFF
    return 4;
  }
  return 0;
}

void AppendString(std::string& out, const std::string& s) {
  static const char* kDigits = "0123456789abcdef";
  out.push_back('"');
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == '"') { out += "\\\""; ++i; continue; }
    if (c == '\\') { out += "\\\\"; ++i; continue; }
    if (c < 0x20 || c == 0x7F) {
      out += "\\u00";
      out.push_back(kDigits[c >> 4]);
      out.push_back(kDigits[c & 15]);
      ++i;
      continue;
    }
    const size_t n = Utf8Length(s, i);
    if (n == 0) {
      out.push_back('?');
      ++i;
      continue;
    }
    out.append(s, i, n);
    i += n;
  }
  out.push_back('"');
}

}  // namespace

std::string ToJson(const Tables& t) {
  std::string out;
  out += "{\n  \"schema\": " + std::to_string(kSchemaVersion) + ",\n  \"game_version\": ";
  AppendString(out, t.game_version);
  out += ",\n  \"stats\": {\n    \"available\": ";
  out += t.stats_available ? "true" : "false";
  if (!t.stats_available) {
    out += ",\n    \"error\": ";
    AppendString(out, t.stats_error);
  }
  out += ",\n    \"column_sets\": [";
  for (size_t s = 0; s < t.stats.column_sets.size(); ++s) {
    out += s ? ",\n      [" : "\n      [";
    const auto& cols = t.stats.column_sets[s];
    for (size_t c = 0; c < cols.size(); ++c) {
      out += c ? ",\n        " : "\n        ";
      out += "{\"prop\": " + std::to_string(cols[c].prop) + ", \"type\": " + std::to_string(cols[c].prop >> 28) +
             ", \"attr\": " + std::to_string(cols[c].attr) + ", \"agg\": " + std::to_string(cols[c].agg) +
             ", \"raw\": \"";
      AppendHex(out, cols[c].raw.data(), cols[c].raw.size());
      out += "\"}";
    }
    out += cols.empty() ? "]" : "\n      ]";
  }
  out += t.stats.column_sets.empty() ? "],\n    \"views\": [" : "\n    ],\n    \"views\": [";
  for (size_t v = 0; v < t.stats.views.size(); ++v) {
    out += v ? ",\n      " : "\n      ";
    out += "{\"id\": " + std::to_string(t.stats.views[v].id) + ", \"column_set\": " +
           std::to_string(t.stats.views[v].column_set) + ", \"raw\": \"";
    AppendHex(out, t.stats.views[v].raw.data(), t.stats.views[v].raw.size());
    out += "\"}";
  }
  out += t.stats.views.empty() ? "]\n  },\n" : "\n    ]\n  },\n";
  out += "  \"properties\": {\n    \"available\": ";
  out += t.properties_available ? "true" : "false";
  out += ",\n    \"items\": [";
  for (size_t i = 0; i < t.properties.size(); ++i) {
    out += i ? ",\n      " : "\n      ";
    out += "{\"id\": " + std::to_string(t.properties[i].id) + ", \"name\": ";
    AppendString(out, t.properties[i].name);
    out += ", \"values\": [";
    for (size_t k = 0; k < t.properties[i].values.size(); ++k) {
      if (k) out += ", ";
      AppendString(out, t.properties[i].values[k]);
    }
    out += "]}";
  }
  out += t.properties.empty() ? "]\n  }\n}\n" : "\n    ]\n  }\n}\n";
  return out;
}

}  // namespace larecomp::online_tables
