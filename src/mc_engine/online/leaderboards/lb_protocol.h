#pragma once
//
// Leaderboards: data model and wire codec.
//
// Shared by the game client (larecomp) and the standalone server
// (larecomp_rmr_server serves it next to Rate My Ride), so nothing here may
// depend on the rexglue SDK.
//
// The game reads and writes Xbox LIVE stats views (XUserReadStats,
// XUserCreateStatsEnumerator*, XSessionWriteStats). We serve them ourselves:
// a row per player per view, each column aggregated the way the title's SPA
// says (Sum, Min, Max or Last), ranked by the view's rating column.
//
// The service is schema-free. The client knows the title's view definitions
// (from the game's own SPA) and sends, with every write, each column's
// aggregation rule; the store applies it to what it already holds.
//
// Wire format: little-endian, framed as the body of one HTTP POST to /lb/v1,
// with the same codec as Rate My Ride.
//

#include <cstdint>
#include <string>
#include <vector>

#include "../rate_my_ride/rmr_protocol.h"

namespace lb {

inline constexpr uint32_t kWireMagic = 0x3144424C;  // "LBD1"
inline constexpr const char* kHttpPath = "/lb/v1";

// Stats view columns the service fills itself (XLAST attribute ids).
inline constexpr uint16_t kAttrRank = 0xFFFF;
inline constexpr uint16_t kAttrRating = 0xFFFE;
inline constexpr uint16_t kAttrGamerName = 0xFFFD;

inline constexpr uint32_t kMaxColumns = 64;
inline constexpr uint32_t kMaxRows = 100;
inline constexpr uint32_t kMaxOwners = 100;

enum class Op : uint32_t {
  kSize = 1,      // rows in `view`
  kByRank = 2,    // `count` rows from rank `start` (1-based)
  kByPivot = 3,   // `count` rows around `pivot`
  kByOwners = 4,  // the rows of `owners`
  kWrite = 5,     // aggregate `columns` into the requester's row of `view`
};

const char* OpName(Op op);

enum Err : uint32_t {
  kOk = 0,
  kInvalidArgs = 1,
  kConnection = 3,
  kNoService = 4,  // the server answered, but has no leaderboards (older build)
  kUnknown = 12,
  kTimedOut = 13,
};

// XLAST aggregation types, as the SPA's XVC2 section stores them (low byte).
enum Agg : uint8_t {
  kAggLast = 0x01,
  kAggSum = 0x03,
  kAggMin = 0x09,
  kAggMax = 0x0B,
};

// XUSER_DATA types (the high nibble of a property id).
enum DataType : uint8_t {
  kTypeContext = 0,
  kTypeInt32 = 1,
  kTypeInt64 = 2,
  kTypeDouble = 3,
  kTypeFloat = 5,
};

// One column value. `bits` holds the value as the guest stores it: an int32 or
// a float sign-extended / bit-copied into the low 32 bits, an int64 or a
// double as is. `agg` only travels on writes.
struct Value {
  uint16_t attr = 0;
  uint8_t type = 0;
  uint8_t agg = 0;
  int64_t bits = 0;
};

struct Row {
  uint64_t owner = 0;
  std::string gamertag;
  uint32_t rank = 0;      // 1-based
  int64_t rating = 0;
  int64_t updated = 0;    // unix seconds of the last write
  std::vector<Value> columns;
};

struct Request {
  Op op = Op::kSize;
  rmr::Token token{};     // requester (writes go to its row)
  std::string gamertag;   // requester's, refreshed on every write
  uint32_t view = 0;
  uint32_t start = 0;
  uint32_t count = 0;
  uint64_t pivot = 0;
  std::vector<uint64_t> owners;
  std::vector<Value> columns;
};

struct Response {
  uint32_t err = kOk;
  uint32_t total = 0;     // rows in the view
  std::vector<Row> rows;
};

std::vector<uint8_t> EncodeRequest(const Request& req);
bool DecodeRequest(const std::vector<uint8_t>& bytes, Request& req);
std::vector<uint8_t> EncodeResponse(const Response& resp);
bool DecodeResponse(const std::vector<uint8_t>& bytes, Response& resp);

// Orders two values of the same column: <0, 0, >0. Floats compare as numbers.
int CompareValues(uint8_t type, int64_t a, int64_t b);

// Applies one aggregation step to `current` (absent when `have` is false).
int64_t Aggregate(uint8_t type, uint8_t agg, bool have, int64_t current, int64_t incoming);

}  // namespace lb
