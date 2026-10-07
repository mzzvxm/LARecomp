#pragma once
//
// Rate My Ride database: the records, who rated what, and the uploaded files.
//
// One directory holds everything:
//   rmr.db           records + ratings, rewritten atomically on every change
//   files/<id>.car   serialized CarConfig the game downloads to show a ride
//   files/<id>.jpg   the two snapshots taken at upload (kept for a future site)
//
// The same class backs the standalone server and the in-process local mode, so
// both behave identically. Rules the game relies on:
//   - one ride per player; uploading again replaces it and resets its ratings
//     ("your current vehicle ranking will be lost", RMR_ChangeRideConf_desc);
//   - a player rates a ride once, re-rating replaces the old score, and nobody
//     rates their own ride;
//   - only rides rated at least `min_ratings` times are ranked (the game sends 3);
//   - "rate rides" never offers the requester's own ride, prefers rides the
//     requester has not rated yet, and honors the owned-DLC mask;
//   - a user-data write from anyone but the owner is a purchase: it only counts
//     the sale (times purchased, reward owed to the owner), and only while the
//     ride is for sale.
//

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "rmr_protocol.h"

namespace rmr {

class Store {
 public:
  explicit Store(std::filesystem::path dir);

  // Loads the index (or starts empty). False only when the directory cannot be
  // created or an existing index is unreadable.
  bool Open(std::string* error);

  Response Handle(const Request& req, std::string* log_line = nullptr);

  // Decode, handle, encode: what both the server and the local client run.
  std::vector<uint8_t> HandleWire(const std::vector<uint8_t>& body, std::string* log_line = nullptr);

  size_t record_count();
  std::string Summary();
  const std::filesystem::path& dir() const { return dir_; }

 private:
  using Ratings = std::map<uint64_t, uint8_t>;  // rater owner key -> 1..10

  Record* FindById(uint32_t id);
  Record* FindByOwner(uint64_t owner);
  std::vector<const Record*> Ranked(uint32_t min_ratings) const;
  void FillRanks(std::vector<Record>& out, uint32_t min_ratings) const;
  void Recount(Record& r);

  bool WriteFile(uint32_t id, const std::vector<uint8_t>& data, bool is_photo);
  bool ReadFile(uint32_t id, std::vector<uint8_t>& data);
  void DeleteFiles(const Record& r);
  bool StoreFiles(const Request& req, Record& r, std::string* why);
  bool Save();
  bool Load(std::string* error);

  Response Upload(const Request& req, uint64_t requester, std::string& log);
  Response Overwrite(const Request& req, uint64_t requester, Record& r, std::string& log);

  std::filesystem::path dir_;
  std::mutex mutex_;
  std::vector<Record> records_;
  std::map<uint32_t, Ratings> ratings_;
  uint32_t next_record_ = 1;
  uint32_t next_file_ = 1;
  std::mt19937_64 rng_;
};

}  // namespace rmr
