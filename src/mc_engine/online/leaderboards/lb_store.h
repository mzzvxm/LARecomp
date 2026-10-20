#pragma once
//
// Leaderboard database: one row per player per stats view.
//
//   leaderboards.db   every view, rewritten atomically after each write
//
// The same class backs the standalone server and the in-process local mode, so
// both behave the same:
//   - a write only touches the requester's row (the owner key comes from the
//     requester's token, never from the request), and aggregates each column
//     with the rule the request carries;
//   - rows rank by the view's rating column (attribute 0xFFFE), highest first;
//     ties go to whoever reached that rating first;
//   - a pivot read returns the window of rows around the pivot, nothing when
//     the pivot has no row in that view.
//

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "lb_protocol.h"

namespace lb {

class Store {
 public:
  explicit Store(std::filesystem::path dir);

  // Loads the database (or starts empty). False when the directory cannot be
  // created or an existing database is unreadable.
  bool Open(std::string* error);

  Response Handle(const Request& req, std::string* log_line = nullptr);

  // Decode, handle, encode: what both the server and the local client run.
  std::vector<uint8_t> HandleWire(const std::vector<uint8_t>& body, std::string* log_line = nullptr);

  std::string Summary();
  const std::filesystem::path& dir() const { return dir_; }

 private:
  struct Cell {
    uint8_t type = 0;
    int64_t bits = 0;
  };
  struct Entry {
    std::string gamertag;
    int64_t updated = 0;
    int64_t reached = 0;  // when the current rating was first reached (tie break)
    std::map<uint16_t, Cell> cells;
  };
  using View = std::map<uint64_t, Entry>;  // owner key -> row

  // Owners of `view` in rank order.
  std::vector<uint64_t> Ranked(const View& view) const;
  Row MakeRow(uint64_t owner, const Entry& e, uint32_t rank) const;

  Response Write(const Request& req, uint64_t owner, std::string& log);
  bool Save();
  bool Load(std::string* error);

  std::filesystem::path dir_;
  std::mutex mutex_;
  std::map<uint32_t, View> views_;
};

}  // namespace lb
