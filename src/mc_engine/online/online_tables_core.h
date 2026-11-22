#pragma once
//
// The two tables the online side reads out of the game's own memory, and the file that holds them (online_tables.json).
//
// A launcher (or anyone looking at a room or a leaderboard without the game running) cannot know what a stats view id or a
// session property id means: the names live in the game, not in LARecomp. The game builds both tables anyway, so it writes
// them down once into its log folder and the launcher reads that file. Nothing from the game is shipped with LARecomp and
// nothing here names a player; the file only exists on a PC that has the game.
//
//   stats       the title's leaderboard views, from the XVC2 section of its XDBF resource (at 0x82950000 in guest memory):
//               which views exist, which columns each carries (property id, attribute id, aggregation rule).
//   properties  the session property table at 0x827DEE20: each property's id, name and the words of its values
//               (GameMode: Race, Cruise, ...), what the server browser shows for a room.
//
// This file is pure (no game, no SDK): the parsers read through a GuestReader, so the unit test feeds them a made-up image.
// The game side (what reads real guest memory, when it runs, where the file goes) is online_tables.cpp.
//
// The file (UTF-8 JSON, schema 1; every id is a plain number, hex is only in "raw"):
//
//   {
//     "schema": 1,
//     "game_version": "PATCH 10 - PREVIEW 2",           LARecomp's version label when it was written
//     "stats": {
//       "available": true,                                false when the game's XVC2 section could not be read
//       "error": "...",                                   only when not available
//       "column_sets": [                                  shared by the views that point at them
//         [ {"prop": 268435457, "type": 1, "attr": 1, "agg": 3, "raw": "<64 hex digits>"}, ... ],
//         ...
//       ],
//       "views": [ {"id": 382, "column_set": 3, "raw": "<32 hex digits>"}, ... ]
//     },
//     "properties": {
//       "available": true,                                false when the game had not filled the table yet
//       "items": [ {"id": 1, "name": "GameMode", "values": ["Race", "Cruise", ...]}, ... ]
//     }
//   }
//
// prop is the XUSER_PROPERTY id, whose high nibble is "type" (1 int32, 2 int64, 3 double, 5 float, as lb::DataType); attr is the
// column's attribute id (the key the leaderboard rows carry); agg is the XLAST aggregation rule (1 last, 3 sum, 9 min, 11 max).
// "raw" is the record as the game stores it, big endian, so a field this code does not interpret (a string id for a view's or
// a column's name, say) is still there for whoever reads the file.
//
// CHECK: XVC2 has no view name that this parser knows; whether one of the raw words is a string id into the XDBF string table
// needs one real dump to see.
//

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace larecomp::online_tables {

inline constexpr int kSchemaVersion = 1;
inline constexpr char kFileName[] = "online_tables.json";

// Where the tables sit in guest memory.
inline constexpr uint32_t kSpaAddress = 0x82950000;    // the XEX's XDBF resource (the title's SPA)
inline constexpr uint32_t kMaxXdbfEntries = 0x10000; // the most entries of the XDBF's table looked at for the XVC2 one (a damaged count cannot make the scan walk on)
inline constexpr uint32_t kPropertyTable = 0x827DEE20;
inline constexpr int kPropertyRecords = 32;
inline constexpr uint32_t kPropertyRecordSize = 76;    // id, name pointer, 16 value-word pointers
inline constexpr int kPropertyValueSlots = 16;
inline constexpr uint32_t kPropertyTextMax = 32;       // the longest name or value word read

// Reads guest memory. False when [addr, addr + len) is not readable; the parsers then stop and say so.
class GuestReader {
 public:
  virtual ~GuestReader() = default;
  virtual bool Read(uint32_t addr, void* out, uint32_t len) const = 0;
};

// ---------------------------------------------------------------- stats ---

struct StatsColumn {
  uint32_t prop = 0;
  uint16_t attr = 0;
  uint8_t agg = 0;                  // low byte of the aggregation word
  std::array<uint8_t, 32> raw{};
};

struct StatsView {
  uint32_t id = 0;
  uint16_t column_set = 0;          // index into StatsSchema::column_sets, always in range
  std::array<uint8_t, 16> raw{};
};

struct StatsSchema {
  std::vector<std::vector<StatsColumn>> column_sets;
  std::vector<StatsView> views;
};

enum class StatsStatus {
  kOk,
  kUnreadable,      // the memory could not be read where the parser needed it
  kNoXdbf,          // no XDBF header at the address
  kNoXvc2,          // the XDBF has no XVC2 section
  kBadSharedView,   // a shared view is not followed by its XPBM block
};

const char* StatsStatusText(StatsStatus status);

// Parses the stats views of the XDBF at `xdbf`. On anything but kOk `out` is left empty.
StatsStatus ReadStatsSchema(const GuestReader& mem, uint32_t xdbf, StatsSchema* out);

// ------------------------------------------------------------ properties ---

struct PropertyDef {
  uint32_t id = 0;
  std::string name;
  std::vector<std::string> values;
};

// Reads the property table at `table`. False when it is not filled in yet (the first record's id is 0: the game fills the
// table at boot) or not readable; `out` is then empty.
bool ReadPropertyTable(const GuestReader& mem, uint32_t table, std::vector<PropertyDef>* out);

// ------------------------------------------------------------------ file ---

struct Tables {
  std::string game_version;
  bool stats_available = false;
  std::string stats_error;              // set when !stats_available
  StatsSchema stats;
  bool properties_available = false;
  std::vector<PropertyDef> properties;
};

// The file's text for `tables`: deterministic (the same tables give the same bytes), valid UTF-8 (a name or word that is not is
// written with '?' for the bad bytes), control characters escaped.
std::string ToJson(const Tables& tables);

}  // namespace larecomp::online_tables
