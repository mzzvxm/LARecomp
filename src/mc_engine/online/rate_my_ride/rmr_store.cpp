#include "rmr_store.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <system_error>

namespace rmr {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kDbMagic = 0x44524D52;  // "RMRD"
constexpr uint32_t kDbVersion = 1;
constexpr uint32_t kMaxSeq = 0x00FFFFFF;    // ids are kIdTag | seq
constexpr size_t kMaxRandomCandidates = 32;
constexpr size_t kMaxPage = 50;

int64_t Now() { return static_cast<int64_t>(std::time(nullptr)); }

std::string Hex32(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof(b), "%08X", v);
  return b;
}

std::string Hex64(uint64_t v) {
  char b[24];
  std::snprintf(b, sizeof(b), "%016llX", static_cast<unsigned long long>(v));
  return b;
}

bool IsJpeg(const std::vector<uint8_t>& d) {
  return d.size() >= 4 && d[0] == 0xFF && d[1] == 0xD8;
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

}  // namespace

Store::Store(fs::path dir) : dir_(std::move(dir)) {
  std::random_device rd;
  rng_.seed((static_cast<uint64_t>(rd()) << 32) ^ rd() ^ static_cast<uint64_t>(Now()));
}

bool Store::Open(std::string* error) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::error_code ec;
  fs::create_directories(dir_ / "files", ec);
  if (ec) {
    if (error) *error = "cannot create " + (dir_ / "files").string() + ": " + ec.message();
    return false;
  }
  return Load(error);
}

size_t Store::record_count() {
  std::lock_guard<std::mutex> lock(mutex_);
  return records_.size();
}

std::string Store::Summary() {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t ratings = 0, rated = 0;
  for (const auto& [id, r] : ratings_) {
    ratings += r.size();
    rated += r.empty() ? 0 : 1;
  }
  return std::to_string(records_.size()) + " rides, " + std::to_string(ratings) + " ratings on " +
         std::to_string(rated) + " rides, " + std::to_string(Ranked(kDefaultMinRanked).size()) +
         " ranked";
}

Record* Store::FindById(uint32_t id) {
  if (!id) return nullptr;
  for (auto& r : records_)
    if (r.id == id) return &r;
  return nullptr;
}

Record* Store::FindByOwner(uint64_t owner) {
  if (!owner) return nullptr;
  for (auto& r : records_)
    if (r.owner == owner) return &r;
  return nullptr;
}

std::vector<const Record*> Store::Ranked(uint32_t min_ratings) const {
  const uint32_t floor = std::max<uint32_t>(min_ratings, 1);
  std::vector<const Record*> out;
  for (const auto& r : records_)
    if (r.num_ratings >= floor) out.push_back(&r);
  std::stable_sort(out.begin(), out.end(), [](const Record* a, const Record* b) {
    if (a->average != b->average) return a->average > b->average;
    if (a->num_ratings != b->num_ratings) return a->num_ratings > b->num_ratings;
    return a->id < b->id;
  });
  return out;
}

void Store::FillRanks(std::vector<Record>& out, uint32_t min_ratings) const {
  const auto ranked = Ranked(min_ratings);
  for (auto& r : out) {
    r.rank = 0;
    for (size_t i = 0; i < ranked.size(); ++i) {
      if (ranked[i]->id == r.id) {
        r.rank = static_cast<uint32_t>(i + 1);
        break;
      }
    }
  }
}

void Store::Recount(Record& r) {
  auto it = ratings_.find(r.id);
  r.num_ratings = 0;
  r.average = 0.0f;
  if (it == ratings_.end() || it->second.empty()) return;
  uint32_t sum = 0;
  for (const auto& [rater, v] : it->second) sum += v;
  r.num_ratings = static_cast<uint32_t>(it->second.size());
  r.average = static_cast<float>(sum) / static_cast<float>(r.num_ratings);
}

// ------------------------------------------------------------ files -------

bool Store::WriteFile(uint32_t id, const std::vector<uint8_t>& data, bool is_photo) {
  return AtomicWrite(dir_ / "files" / (Hex32(id) + (is_photo ? ".jpg" : ".car")), data);
}

bool Store::ReadFile(uint32_t id, std::vector<uint8_t>& data) {
  for (const char* ext : {".car", ".jpg"}) {
    const fs::path p = dir_ / "files" / (Hex32(id) + ext);
    std::error_code ec;
    if (fs::exists(p, ec)) return ReadAll(p, data);
  }
  return false;
}

void Store::DeleteFiles(const Record& r) {
  std::error_code ec;
  for (uint32_t i = 0; i < r.file_count && i < kMaxFiles; ++i) {
    for (const char* ext : {".car", ".jpg"})
      fs::remove(dir_ / "files" / (Hex32(r.file_ids[i]) + ext), ec);
  }
}

// File 0 (the car) is mandatory; photos are kept only while each one is a
// non-empty JPEG, because the game requires the listed files to be contiguous
// with non-zero ids and sizes.
bool Store::StoreFiles(const Request& req, Record& r, std::string* why) {
  if (req.files.empty() || req.files[0].empty() || req.files[0].size() > kMaxCarBytes) {
    if (why) *why = "missing or oversized vehicle data";
    return false;
  }
  r.file_count = 0;
  r.file_ids.fill(0);
  r.file_sizes.fill(0);
  for (size_t i = 0; i < req.files.size() && i < kMaxFiles; ++i) {
    const auto& d = req.files[i];
    if (i > 0 && (d.empty() || d.size() > kMaxPhotoBytes || !IsJpeg(d))) break;
    if (next_file_ > kMaxSeq) {
      if (why) *why = "file id space exhausted";
      return false;
    }
    const uint32_t id = kIdTag | next_file_++;
    if (!WriteFile(id, d, i > 0)) {
      DeleteFiles(r);
      if (why) *why = "cannot write " + Hex32(id);
      return false;
    }
    r.file_ids[r.file_count] = id;
    r.file_sizes[r.file_count] = static_cast<uint32_t>(d.size());
    ++r.file_count;
  }
  return true;
}

// ------------------------------------------------------------ index -------

bool Store::Save() {
  Writer w;
  w.U32(kDbMagic);
  w.U32(kDbVersion);
  w.U32(next_record_);
  w.U32(next_file_);
  w.U32(static_cast<uint32_t>(records_.size()));
  for (const auto& r : records_) WriteRecord(w, r);
  w.U32(static_cast<uint32_t>(ratings_.size()));
  for (const auto& [id, raters] : ratings_) {
    w.U32(id);
    w.U32(static_cast<uint32_t>(raters.size()));
    for (const auto& [rater, v] : raters) {
      w.U64(rater);
      w.U8(v);
    }
  }
  return AtomicWrite(dir_ / "rmr.db", w.buffer());
}

bool Store::Load(std::string* error) {
  records_.clear();
  ratings_.clear();
  next_record_ = next_file_ = 1;
  const fs::path path = dir_ / "rmr.db";
  std::error_code ec;
  if (!fs::exists(path, ec)) return true;

  std::vector<uint8_t> bytes;
  if (!ReadAll(path, bytes)) {
    if (error) *error = "cannot read " + path.string();
    return false;
  }
  Reader rd(bytes.data(), bytes.size());
  uint32_t magic, version, nrec, nrated;
  bool ok = rd.U32(magic) && magic == kDbMagic && rd.U32(version) && version == kDbVersion &&
            rd.U32(next_record_) && rd.U32(next_file_) && rd.U32(nrec);
  for (uint32_t i = 0; ok && i < nrec; ++i) {
    Record r;
    ok = ReadRecord(rd, r);
    if (ok) records_.push_back(std::move(r));
  }
  ok = ok && rd.U32(nrated);
  for (uint32_t i = 0; ok && i < nrated; ++i) {
    uint32_t id, n;
    ok = rd.U32(id) && rd.U32(n);
    Ratings& raters = ratings_[id];
    for (uint32_t j = 0; ok && j < n; ++j) {
      uint64_t rater;
      uint8_t v;
      ok = rd.U64(rater) && rd.U8(v);
      if (ok) raters[rater] = v;
    }
  }
  if (!ok || !rd.AtEnd()) {
    records_.clear();
    ratings_.clear();
    if (error) *error = "corrupt index " + path.string();
    return false;
  }
  for (auto& r : records_) Recount(r);
  return true;
}

// ------------------------------------------------------------ writes ------

Response Store::Upload(const Request& req, uint64_t requester, std::string& log) {
  if (Record* existing = FindByOwner(requester)) return Overwrite(req, requester, *existing, log);

  Response resp;
  if (req.userdata.size() > kUserDataMax || next_record_ > kMaxSeq) {
    resp.err = kInvalidArgs;
    return resp;
  }
  Record r;
  r.owner = requester;
  r.gamertag = SanitizeGamertag(req.gamertag);
  r.content = static_cast<uint8_t>(req.content & 31);
  r.userdata = req.userdata;
  std::string why;
  if (!StoreFiles(req, r, &why)) {
    log += " rejected: " + why;
    resp.err = kInvalidArgs;
    return resp;
  }
  r.id = kIdTag | next_record_++;
  r.created = r.updated = Now();
  records_.push_back(r);
  Save();
  log += " -> new ride " + Hex32(r.id) + " '" + r.gamertag + "' files=" + std::to_string(r.file_count);
  resp.records.push_back(r);
  FillRanks(resp.records, kDefaultMinRanked);
  return resp;
}

Response Store::Overwrite(const Request& req, uint64_t requester, Record& r, std::string& log) {
  Response resp;
  if (r.owner != requester || req.userdata.size() > kUserDataMax) {
    resp.err = kNotAllowed;
    return resp;
  }
  Record nr = r;  // keeps id, owner and creation time
  nr.gamertag = SanitizeGamertag(req.gamertag);
  nr.content = static_cast<uint8_t>(req.content & 31);
  nr.userdata = req.userdata;
  // A new upload starts with no sales; keep paying out sales of the old ride
  // that the owner has not collected yet.
  if (r.userdata.size() >= kUdUsedBytes && nr.userdata.size() >= kUdUsedBytes) {
    const uint32_t owed = LoadBe32(&r.userdata[kUdPendingReward]) +
                          LoadBe32(&nr.userdata[kUdPendingReward]);
    StoreBe32(&nr.userdata[kUdPendingReward], owed);
  }
  std::string why;
  if (!StoreFiles(req, nr, &why)) {
    log += " rejected: " + why;
    resp.err = kInvalidArgs;
    return resp;
  }
  DeleteFiles(r);
  ratings_.erase(r.id);
  nr.num_ratings = 0;
  nr.average = 0.0f;
  nr.updated = Now();
  r = nr;
  Save();
  log += " -> replaced ride " + Hex32(r.id) + " '" + r.gamertag + "' files=" +
         std::to_string(r.file_count) + " (ratings reset)";
  resp.records.push_back(r);
  FillRanks(resp.records, kDefaultMinRanked);
  return resp;
}

// ------------------------------------------------------------ dispatch ----

Response Store::Handle(const Request& req, std::string* log_line) {
  std::lock_guard<std::mutex> lock(mutex_);
  const uint64_t me = OwnerKeyFromToken(req.token);
  Response resp;
  std::string log = std::string(OpName(req.op)) + " by " + Hex64(me).substr(0, 8);

  switch (req.op) {
    case Op::kTableSize:
      resp.count = static_cast<uint32_t>(Ranked(req.min_ratings).size());
      log += " -> " + std::to_string(resp.count);
      break;

    case Op::kByOwner: {
      const size_t max = std::clamp<size_t>(req.count, 1, kMaxPage);
      for (const auto& r : records_)
        if (r.owner == req.owner && resp.records.size() < max) resp.records.push_back(r);
      FillRanks(resp.records, kDefaultMinRanked);
      log += " owner=" + Hex64(req.owner).substr(0, 8) + " -> " + std::to_string(resp.records.size());
      break;
    }

    case Op::kRandom: {
      std::vector<const Record*> fresh, seen;
      for (const auto& r : records_) {
        if (r.owner == me) continue;
        if (req.content_mask && r.content < 32 && !((req.content_mask >> r.content) & 1)) continue;
        if (r.num_ratings < req.min_ratings) continue;
        if (req.max_ratings && r.num_ratings > req.max_ratings) continue;
        auto it = ratings_.find(r.id);
        const bool rated = it != ratings_.end() && it->second.count(me);
        (rated ? seen : fresh).push_back(&r);
      }
      std::shuffle(fresh.begin(), fresh.end(), rng_);
      std::shuffle(seen.begin(), seen.end(), rng_);
      const size_t want = std::clamp<size_t>(req.count, 1, kMaxRandomCandidates);
      for (const auto* list : {&fresh, &seen})
        for (const Record* r : *list)
          if (resp.records.size() < want) resp.records.push_back(*r);
      FillRanks(resp.records, kDefaultMinRanked);
      log += " max_ratings=" + std::to_string(req.max_ratings) + " -> " +
             std::to_string(resp.records.size()) + " (" + std::to_string(fresh.size()) +
             " unrated by requester)";
      break;
    }

    case Op::kByRank: {
      const auto ranked = Ranked(req.min_ratings);
      const size_t start = req.start ? req.start : 1;
      const size_t count = std::clamp<size_t>(req.count, 1, kMaxPage);
      for (size_t i = start - 1; i < ranked.size() && resp.records.size() < count; ++i) {
        Record r = *ranked[i];
        r.rank = static_cast<uint32_t>(i + 1);
        resp.records.push_back(std::move(r));
      }
      log += " from #" + std::to_string(start) + " -> " + std::to_string(resp.records.size()) +
             " of " + std::to_string(ranked.size());
      break;
    }

    case Op::kRate: {
      Record* r = FindById(req.record_id);
      if (!r) r = FindByOwner(req.owner);
      if (!r) {
        resp.err = kFileNotFound;  // soft failure in the game
        log += " " + Hex32(req.record_id) + " -> no such ride";
        break;
      }
      if (r->owner == me) {
        resp.err = kInvalidArgs;  // soft failure; the game never offers it
        log += " -> own ride refused";
        break;
      }
      const uint8_t v = static_cast<uint8_t>(std::clamp<int>(req.rating, 1, 10));
      ratings_[r->id][me] = v;
      Recount(*r);
      Save();
      resp.num_ratings = r->num_ratings;
      resp.average = r->average;
      resp.records.push_back(*r);
      FillRanks(resp.records, kDefaultMinRanked);
      char buf[96];
      std::snprintf(buf, sizeof(buf), " %s = %u -> %.2f over %u", Hex32(r->id).c_str(), v,
                    r->average, r->num_ratings);
      log += buf;
      break;
    }

    case Op::kUpload:
      resp = Upload(req, me, log);
      break;

    case Op::kOverwrite: {
      Record* r = FindById(req.record_id);
      if (!r || r->owner != me) r = FindByOwner(me);
      resp = r ? Overwrite(req, me, *r, log) : Upload(req, me, log);
      break;
    }

    case Op::kUserData: {
      Record* r = FindById(req.record_id);
      if (!r) r = FindByOwner(req.owner);
      if (!r) {
        resp.err = kFileNotFound;
        log += " " + Hex32(req.record_id) + " -> no such ride";
        break;
      }
      if (r->owner == me) {
        if (req.userdata.size() > kUserDataMax) {
          resp.err = kInvalidArgs;
          break;
        }
        r->userdata = req.userdata;
        log += " owner update " + Hex32(r->id);
      } else {
        // Purchase. Count it here instead of trusting the buyer's copy of the
        // counters, so two buyers at once both count.
        if (r->userdata.size() < kUdUsedBytes || !LoadBe32(&r->userdata[kUdForSale])) {
          resp.err = kNotAllowed;
          log += " purchase of " + Hex32(r->id) + " refused: not for sale";
          break;
        }
        StoreBe32(&r->userdata[kUdPurchases], LoadBe32(&r->userdata[kUdPurchases]) + 1);
        StoreBe32(&r->userdata[kUdPendingReward], LoadBe32(&r->userdata[kUdPendingReward]) + 1);
        log += " purchase of " + Hex32(r->id) + " (sold " +
               std::to_string(LoadBe32(&r->userdata[kUdPurchases])) + "x)";
      }
      r->updated = Now();
      Save();
      resp.records.push_back(*r);
      FillRanks(resp.records, kDefaultMinRanked);
      break;
    }

    case Op::kDownload:
      if (!IsRmrId(req.file_id) || !ReadFile(req.file_id, resp.data)) {
        resp.err = kFileNotFound;
        log += " " + Hex32(req.file_id) + " -> not found";
      } else if (req.max_bytes && resp.data.size() > req.max_bytes) {
        resp.err = kBufferOverflow;
        resp.data.clear();
        log += " " + Hex32(req.file_id) + " -> larger than the game buffer";
      } else {
        log += " " + Hex32(req.file_id) + " -> " + std::to_string(resp.data.size()) + " bytes";
      }
      break;
  }

  if (resp.err != kOk) log += " [err " + std::to_string(resp.err) + "]";
  if (log_line) *log_line = log;
  return resp;
}

std::vector<uint8_t> Store::HandleWire(const std::vector<uint8_t>& body, std::string* log_line) {
  Request req;
  Response resp;
  if (!DecodeRequest(body, req)) {
    resp.err = kInvalidArgs;
    if (log_line) *log_line = "malformed request (" + std::to_string(body.size()) + " bytes)";
  } else {
    resp = Handle(req, log_line);
  }
  return EncodeResponse(resp);
}

}  // namespace rmr
