#pragma once
//
// Rate My Ride: data model and wire codec.
//
// Shared by the game client (larecomp) and the standalone server
// (larecomp_rmr_server), so nothing here may depend on the rexglue SDK.
//
// The game talks to Rockstar's dead rage::rlUpload service (a GameSpy Sake
// table named "RateMyRide"). We replace that service with our own: the same
// operations the game issues, served either in-process from a local store or
// by a server shared between players. A record here is the host-side view of
// the game's 152-byte rlUpload record; rate_my_ride.cpp converts between the
// two.
//
// Wire format: little-endian, every field always present (no per-op layouts),
// framed as the body of one HTTP POST to /rmr/v1.
//

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace rmr {

inline constexpr uint32_t kWireMagic = 0x31524D52;  // "RMR1"
inline constexpr uint16_t kDefaultPort = 31002;
inline constexpr const char* kHttpPath = "/rmr/v1";

// Limits of the game's record (see the layout in rate_my_ride.cpp).
inline constexpr uint32_t kMaxFiles = 3;
inline constexpr uint32_t kUserDataMax = 64;
inline constexpr uint32_t kGamertagMax = 15;

// File 0 is the serialized CarConfig (the game's buffer is 0x2000 bytes);
// files 1 and 2 are the two JPEG snapshots, uploaded at a fixed 204799 bytes.
inline constexpr uint32_t kMaxCarBytes = 0x2000;
inline constexpr uint32_t kMaxPhotoBytes = 204799;
inline constexpr uint32_t kMaxRequestBytes = 1024 * 1024;

// Record and file ids handed to the game carry 'R' in the top byte, so the
// DownloadFile hook can tell our ids from the other rlUpload tables' ids.
inline constexpr uint32_t kIdTag = 0x52000000;
inline constexpr uint32_t kIdTagMask = 0xFF000000;
inline bool IsRmrId(uint32_t id) { return (id & kIdTagMask) == kIdTag && id != kIdTag; }

// The game only ranks rides rated at least this many times (it passes 3 to
// both GetTableSize and ReadRecordsByRank, and shows "--" as its own rank below
// 3 ratings).
inline constexpr uint32_t kDefaultMinRanked = 3;

// The guest XUID is the same constant for every player of the recomp, so it
// cannot tell owners apart. Owners are keyed by a hash of a per-profile secret
// token instead; the key never equals this XUID (the game's own handle).
inline constexpr uint64_t kSharedGuestXuid = 0xB13EBABEBABEBABEull;

enum class Op : uint32_t {
  kTableSize = 1,  // ranked record count
  kByOwner = 2,    // records owned by `owner`
  kRandom = 3,     // candidates for "rate rides" (never the requester's own)
  kByRank = 4,     // leaderboard page
  kRate = 5,       // rate `record_id` with `rating` (1..10)
  kUpload = 6,     // create the requester's ride
  kOverwrite = 7,  // replace the requester's ride (ratings reset)
  kUserData = 8,   // owner: replace user data; anyone else: purchase
  kDownload = 9,   // fetch a file blob
};

const char* OpName(Op op);

// Result codes are the game's Sake codes (sub_826B0D50), so they map straight
// onto its error dialog: 1 and 10 are "soft" failures for rating.
enum Err : uint32_t {
  kOk = 0,
  kInvalidArgs = 1,
  kConnection = 3,  // "DNS, socket, or Gamespy server error"
  kBufferOverflow = 6,
  kNotAllowed = 7,
  kFileNotFound = 10,
  kUnknown = 12,
  kTimedOut = 13,
};

using Token = std::array<uint8_t, 16>;

// Stable, public owner key derived from the secret token (FNV-1a 64). Low 32
// bits are never zero (the game rejects such handles) and the key never
// collides with the shared guest XUID.
uint64_t OwnerKeyFromToken(const Token& token);

struct Record {
  uint32_t id = 0;
  uint64_t owner = 0;
  std::string gamertag;
  uint32_t file_count = 0;
  std::array<uint32_t, kMaxFiles> file_ids{};
  std::array<uint32_t, kMaxFiles> file_sizes{};
  uint32_t num_ratings = 0;
  float average = 0.0f;
  uint32_t rank = 0;              // 1-based leaderboard position, 0 = unranked
  std::vector<uint8_t> userdata;  // opaque to the protocol, <= kUserDataMax
  uint8_t content = 0;            // DLC content bit index of the vehicle
  int64_t created = 0;            // unix seconds
  int64_t updated = 0;
};

struct Request {
  Op op = Op::kTableSize;
  Token token{};
  uint32_t min_ratings = 0;   // kTableSize, kRandom, kByRank
  uint32_t max_ratings = 0;   // kRandom: 0 = no cap
  uint32_t content_mask = 0;  // kRandom: the requester's owned-content mask
  uint32_t start = 0;         // kByRank: 1-based first rank
  uint32_t count = 0;         // kByOwner/kByRank: max records; kRandom: candidates
  uint64_t owner = 0;         // kByOwner target; owner hint for record ops
  uint32_t record_id = 0;     // kRate, kOverwrite, kUserData
  uint32_t file_id = 0;       // kDownload
  uint32_t max_bytes = 0;     // kDownload: the game's buffer size
  uint8_t rating = 0;         // kRate
  uint8_t content = 0;        // kUpload, kOverwrite
  std::string gamertag;       // kUpload, kOverwrite
  std::vector<std::vector<uint8_t>> files;  // kUpload, kOverwrite
  std::vector<uint8_t> userdata;            // kUpload, kOverwrite, kUserData
};

struct Response {
  uint32_t err = kOk;
  uint32_t count = 0;          // kTableSize
  uint32_t num_ratings = 0;    // kRate
  float average = 0.0f;        // kRate
  std::vector<Record> records;
  std::vector<uint8_t> data;   // kDownload
};

// Byte codec. Readers fail (return false) instead of reading out of bounds.
class Writer {
 public:
  void U8(uint8_t v) { buf_.push_back(v); }
  void U32(uint32_t v);
  void U64(uint64_t v);
  void I64(int64_t v) { U64(static_cast<uint64_t>(v)); }
  void F32(float v);
  void Bytes(const uint8_t* data, size_t size);
  void Bytes(const std::vector<uint8_t>& v) { Bytes(v.data(), v.size()); }
  void Str(const std::string& s) { Bytes(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }
  std::vector<uint8_t>& buffer() { return buf_; }

 private:
  std::vector<uint8_t> buf_;
};

class Reader {
 public:
  Reader(const uint8_t* data, size_t size) : p_(data), end_(data + size) {}
  bool U8(uint8_t& v);
  bool U32(uint32_t& v);
  bool U64(uint64_t& v);
  bool I64(int64_t& v);
  bool F32(float& v);
  bool Bytes(std::vector<uint8_t>& v, size_t max_size);
  bool Str(std::string& s, size_t max_size);
  bool AtEnd() const { return p_ == end_; }

 private:
  const uint8_t* p_;
  const uint8_t* end_;
};

void WriteRecord(Writer& w, const Record& r);
bool ReadRecord(Reader& rd, Record& r);

std::vector<uint8_t> EncodeRequest(const Request& req);
bool DecodeRequest(const std::vector<uint8_t>& bytes, Request& req);
std::vector<uint8_t> EncodeResponse(const Response& resp);
bool DecodeResponse(const std::vector<uint8_t>& bytes, Response& resp);

// Printable-ASCII gamertag, at most kGamertagMax characters.
std::string SanitizeGamertag(const std::string& raw);

// Big-endian helpers for the guest-layout user data the server inspects.
uint32_t LoadBe32(const uint8_t* p);
void StoreBe32(uint8_t* p, uint32_t v);

// RMR user data (64 bytes, guest big-endian): +0 vehicle id, +4 sale price,
// +8 times purchased, +12 purchases not yet paid out, +16 for-sale flag.
inline constexpr size_t kUdVehicle = 0;
inline constexpr size_t kUdPrice = 4;
inline constexpr size_t kUdPurchases = 8;
inline constexpr size_t kUdPendingReward = 12;
inline constexpr size_t kUdForSale = 16;
inline constexpr size_t kUdUsedBytes = 20;

}  // namespace rmr
