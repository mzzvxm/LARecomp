#include "lb_store.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>

namespace lb {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kDbMagic = 0x4244424C;  // "LBDB"
constexpr uint32_t kDbVersion = 1;
constexpr uint32_t kMaxViewId = 0xFFFF;    // the title's views are 1..500

int64_t Now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool AtomicWrite(const fs::path& path, const std::vector<uint8_t>& data) {
  fs::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!f) return false;
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return false;
  }
  return true;
}

bool ReadAll(const fs::path& path, std::vector<uint8_t>& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  f.seekg(0, std::ios::end);
  const std::streamoff size = f.tellg();
  if (size < 0) return false;
  f.seekg(0, std::ios::beg);
  out.resize(static_cast<size_t>(size));
  if (size > 0) f.read(reinterpret_cast<char*>(out.data()), size);
  return static_cast<bool>(f);
}

std::string Hex64(uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(v));
  return buf;
}

// Columns the service owns; a client never writes them.
bool ServiceColumn(uint16_t attr) { return attr == kAttrRank || attr == kAttrGamerName; }

bool KnownAgg(uint8_t agg) {
  return agg == kAggLast || agg == kAggSum || agg == kAggMin || agg == kAggMax;
}

}  // namespace

Store::Store(fs::path dir) : dir_(std::move(dir)) {}

bool Store::Open(std::string* error) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::error_code ec;
  fs::create_directories(dir_, ec);
  if (ec) {
    if (error) *error = "cannot create " + dir_.string() + ": " + ec.message();
    return false;
  }
  return Load(error);
}

std::vector<uint64_t> Store::Ranked(const View& view) const {
  std::vector<uint64_t> owners;
  owners.reserve(view.size());
  for (const auto& [owner, e] : view) {
    if (e.cells.count(kAttrRating)) owners.push_back(owner);
  }
  std::sort(owners.begin(), owners.end(), [&view](uint64_t a, uint64_t b) {
    const Entry& ea = view.at(a);
    const Entry& eb = view.at(b);
    const Cell& ra = ea.cells.at(kAttrRating);
    const Cell& rb = eb.cells.at(kAttrRating);
    const int c = CompareValues(ra.type, ra.bits, rb.bits);
    if (c != 0) return c > 0;
    if (ea.reached != eb.reached) return ea.reached < eb.reached;
    return a < b;
  });
  return owners;
}

Row Store::MakeRow(uint64_t owner, const Entry& e, uint32_t rank) const {
  Row r;
  r.owner = owner;
  r.gamertag = e.gamertag;
  r.rank = rank;
  r.updated = e.updated;
  if (auto it = e.cells.find(kAttrRating); it != e.cells.end()) r.rating = it->second.bits;
  for (const auto& [attr, cell] : e.cells) {
    Value v;
    v.attr = attr;
    v.type = cell.type;
    v.bits = cell.bits;
    r.columns.push_back(v);
  }
  return r;
}

Response Store::Write(const Request& req, uint64_t owner, std::string& log) {
  Response resp;
  if (req.view == 0 || req.view > kMaxViewId || req.columns.empty()) {
    resp.err = kInvalidArgs;
    log += " rejected (view/columns)";
    return resp;
  }
  Entry& e = views_[req.view][owner];
  const std::string tag = rmr::SanitizeGamertag(req.gamertag);
  if (!tag.empty()) e.gamertag = tag;
  const int64_t now = Now();
  bool rating_moved = false;
  for (const Value& v : req.columns) {
    if (ServiceColumn(v.attr)) continue;
    if (!KnownAgg(v.agg)) continue;
    auto it = e.cells.find(v.attr);
    const bool have = it != e.cells.end() && it->second.type == v.type;
    const int64_t before = have ? it->second.bits : 0;
    const int64_t after = Aggregate(v.type, v.agg, have, before, v.bits);
    e.cells[v.attr] = Cell{v.type, after};
    if (v.attr == kAttrRating && (!have || after != before)) rating_moved = true;
    char buf[64];
    std::snprintf(buf, sizeof(buf), " %u=%lld", v.attr, static_cast<long long>(after));
    log += buf;
  }
  e.updated = now;
  if (rating_moved || e.reached == 0) e.reached = now;
  if (!Save()) {
    resp.err = kUnknown;
    log += " (SAVE FAILED)";
  }
  return resp;
}

Response Store::Handle(const Request& req, std::string* log_line) {
  std::lock_guard<std::mutex> lock(mutex_);
  const uint64_t requester = rmr::OwnerKeyFromToken(req.token);
  std::string log = std::string(OpName(req.op)) + " view " + std::to_string(req.view) + " by " +
                    Hex64(requester);
  Response resp;

  if (req.op == Op::kWrite) {
    resp = Write(req, requester, log);
  } else {
    auto vit = views_.find(req.view);
    const std::vector<uint64_t> order =
        vit == views_.end() ? std::vector<uint64_t>{} : Ranked(vit->second);
    resp.total = static_cast<uint32_t>(order.size());
    const uint32_t count = std::min<uint32_t>(req.count, kMaxRows);
    auto emit = [&](size_t index) {
      const uint64_t owner = order[index];
      resp.rows.push_back(MakeRow(owner, vit->second.at(owner), static_cast<uint32_t>(index + 1)));
    };
    switch (req.op) {
      case Op::kSize:
        break;
      case Op::kByRank: {
        const size_t first = req.start > 0 ? req.start - 1 : 0;
        for (size_t i = first; i < order.size() && resp.rows.size() < count; ++i) emit(i);
        break;
      }
      case Op::kByPivot: {
        auto it = std::find(order.begin(), order.end(), req.pivot);
        if (it == order.end() || count == 0) break;
        const size_t at = static_cast<size_t>(it - order.begin());
        size_t first = at > count / 2 ? at - count / 2 : 0;
        if (first + count > order.size()) first = order.size() > count ? order.size() - count : 0;
        for (size_t i = first; i < order.size() && resp.rows.size() < count; ++i) emit(i);
        break;
      }
      case Op::kByOwners: {
        for (uint64_t owner : req.owners) {
          auto it = std::find(order.begin(), order.end(), owner);
          if (it != order.end()) emit(static_cast<size_t>(it - order.begin()));
        }
        break;
      }
      default:
        resp.err = kInvalidArgs;
        break;
    }
    log += " -> " + std::to_string(resp.rows.size()) + " of " + std::to_string(resp.total);
  }
  if (log_line) *log_line = std::move(log);
  return resp;
}

std::vector<uint8_t> Store::HandleWire(const std::vector<uint8_t>& body, std::string* log_line) {
  Request req;
  if (!DecodeRequest(body, req)) {
    if (log_line) *log_line = "undecodable request (" + std::to_string(body.size()) + " bytes)";
    Response resp;
    resp.err = kInvalidArgs;
    return EncodeResponse(resp);
  }
  return EncodeResponse(Handle(req, log_line));
}

std::string Store::Summary() {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t rows = 0;
  std::map<uint64_t, bool> players;
  for (const auto& [id, view] : views_) {
    rows += view.size();
    for (const auto& [owner, e] : view) players[owner] = true;
  }
  return std::to_string(views_.size()) + " leaderboard(s), " + std::to_string(rows) + " row(s), " +
         std::to_string(players.size()) + " player(s)";
}

bool Store::Save() {
  rmr::Writer w;
  w.U32(kDbMagic);
  w.U32(kDbVersion);
  w.U32(static_cast<uint32_t>(views_.size()));
  for (const auto& [id, view] : views_) {
    w.U32(id);
    w.U32(static_cast<uint32_t>(view.size()));
    for (const auto& [owner, e] : view) {
      w.U64(owner);
      w.Str(e.gamertag);
      w.I64(e.updated);
      w.I64(e.reached);
      w.U32(static_cast<uint32_t>(e.cells.size()));
      for (const auto& [attr, cell] : e.cells) {
        w.U32(attr);
        w.U8(cell.type);
        w.I64(cell.bits);
      }
    }
  }
  return AtomicWrite(dir_ / "leaderboards.db", w.buffer());
}

bool Store::Load(std::string* error) {
  views_.clear();
  const fs::path path = dir_ / "leaderboards.db";
  std::error_code ec;
  if (!fs::exists(path, ec)) return true;
  std::vector<uint8_t> bytes;
  if (!ReadAll(path, bytes)) {
    if (error) *error = "cannot read " + path.string();
    return false;
  }
  rmr::Reader rd(bytes.data(), bytes.size());
  uint32_t magic, version, nviews;
  bool ok = rd.U32(magic) && magic == kDbMagic && rd.U32(version) && version == kDbVersion &&
            rd.U32(nviews);
  for (uint32_t v = 0; ok && v < nviews; ++v) {
    uint32_t id, nrows;
    ok = rd.U32(id) && rd.U32(nrows);
    View& view = views_[id];
    for (uint32_t r = 0; ok && r < nrows; ++r) {
      uint64_t owner;
      Entry e;
      uint32_t ncells;
      ok = rd.U64(owner) && rd.Str(e.gamertag, rmr::kGamertagMax) && rd.I64(e.updated) &&
           rd.I64(e.reached) && rd.U32(ncells) && ncells <= kMaxColumns;
      for (uint32_t c = 0; ok && c < ncells; ++c) {
        uint32_t attr;
        Cell cell;
        ok = rd.U32(attr) && attr <= 0xFFFF && rd.U8(cell.type) && rd.I64(cell.bits);
        if (ok) e.cells[static_cast<uint16_t>(attr)] = cell;
      }
      if (ok) view[owner] = std::move(e);
    }
  }
  if (!ok) {
    views_.clear();
    if (error) *error = path.string() + " is damaged";
    return false;
  }
  return true;
}

}  // namespace lb
