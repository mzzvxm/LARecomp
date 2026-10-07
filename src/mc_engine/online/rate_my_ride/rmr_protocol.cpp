#include "rmr_protocol.h"

#include <cstring>

namespace rmr {

const char* OpName(Op op) {
  switch (op) {
    case Op::kTableSize: return "table_size";
    case Op::kByOwner: return "by_owner";
    case Op::kRandom: return "random";
    case Op::kByRank: return "by_rank";
    case Op::kRate: return "rate";
    case Op::kUpload: return "upload";
    case Op::kOverwrite: return "overwrite";
    case Op::kUserData: return "user_data";
    case Op::kDownload: return "download";
  }
  return "?";
}

uint64_t OwnerKeyFromToken(const Token& token) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (uint8_t b : token) {
    h ^= b;
    h *= 0x100000001B3ull;
  }
  if ((h & 0xFFFFFFFFull) == 0) h |= 1;
  if (h == kSharedGuestXuid) h ^= 0x10;
  return h;
}

// ---------------------------------------------------------------- codec ----

void Writer::U32(uint32_t v) {
  for (int i = 0; i < 4; ++i) buf_.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void Writer::U64(uint64_t v) {
  for (int i = 0; i < 8; ++i) buf_.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void Writer::F32(float v) {
  uint32_t bits;
  std::memcpy(&bits, &v, 4);
  U32(bits);
}

void Writer::Bytes(const uint8_t* data, size_t size) {
  U32(static_cast<uint32_t>(size));
  buf_.insert(buf_.end(), data, data + size);
}

bool Reader::U8(uint8_t& v) {
  if (end_ - p_ < 1) return false;
  v = *p_++;
  return true;
}

bool Reader::U32(uint32_t& v) {
  if (end_ - p_ < 4) return false;
  v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p_[i]) << (8 * i);
  p_ += 4;
  return true;
}

bool Reader::U64(uint64_t& v) {
  if (end_ - p_ < 8) return false;
  v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p_[i]) << (8 * i);
  p_ += 8;
  return true;
}

bool Reader::I64(int64_t& v) {
  uint64_t u;
  if (!U64(u)) return false;
  v = static_cast<int64_t>(u);
  return true;
}

bool Reader::F32(float& v) {
  uint32_t bits;
  if (!U32(bits)) return false;
  std::memcpy(&v, &bits, 4);
  return true;
}

bool Reader::Bytes(std::vector<uint8_t>& v, size_t max_size) {
  uint32_t size;
  if (!U32(size)) return false;
  if (size > max_size || static_cast<size_t>(end_ - p_) < size) return false;
  v.assign(p_, p_ + size);
  p_ += size;
  return true;
}

bool Reader::Str(std::string& s, size_t max_size) {
  std::vector<uint8_t> v;
  if (!Bytes(v, max_size)) return false;
  s.assign(v.begin(), v.end());
  return true;
}

void WriteRecord(Writer& w, const Record& r) {
  w.U32(r.id);
  w.U64(r.owner);
  w.Str(r.gamertag);
  w.U32(r.file_count);
  for (uint32_t id : r.file_ids) w.U32(id);
  for (uint32_t size : r.file_sizes) w.U32(size);
  w.U32(r.num_ratings);
  w.F32(r.average);
  w.U32(r.rank);
  w.Bytes(r.userdata);
  w.U8(r.content);
  w.I64(r.created);
  w.I64(r.updated);
}

bool ReadRecord(Reader& rd, Record& r) {
  if (!rd.U32(r.id) || !rd.U64(r.owner) || !rd.Str(r.gamertag, 64) || !rd.U32(r.file_count))
    return false;
  for (uint32_t& id : r.file_ids)
    if (!rd.U32(id)) return false;
  for (uint32_t& size : r.file_sizes)
    if (!rd.U32(size)) return false;
  if (!rd.U32(r.num_ratings) || !rd.F32(r.average) || !rd.U32(r.rank) ||
      !rd.Bytes(r.userdata, kUserDataMax) || !rd.U8(r.content) || !rd.I64(r.created) ||
      !rd.I64(r.updated))
    return false;
  return r.file_count <= kMaxFiles;
}

std::vector<uint8_t> EncodeRequest(const Request& req) {
  Writer w;
  w.U32(kWireMagic);
  w.U32(static_cast<uint32_t>(req.op));
  w.Bytes(req.token.data(), req.token.size());
  w.U32(req.min_ratings);
  w.U32(req.max_ratings);
  w.U32(req.content_mask);
  w.U32(req.start);
  w.U32(req.count);
  w.U64(req.owner);
  w.U32(req.record_id);
  w.U32(req.file_id);
  w.U32(req.max_bytes);
  w.U8(req.rating);
  w.U8(req.content);
  w.Str(req.gamertag);
  w.U32(static_cast<uint32_t>(req.files.size()));
  for (const auto& f : req.files) w.Bytes(f);
  w.Bytes(req.userdata);
  return std::move(w.buffer());
}

bool DecodeRequest(const std::vector<uint8_t>& bytes, Request& req) {
  Reader rd(bytes.data(), bytes.size());
  uint32_t magic, op, nfiles;
  std::vector<uint8_t> token;
  if (!rd.U32(magic) || magic != kWireMagic || !rd.U32(op)) return false;
  if (op < static_cast<uint32_t>(Op::kTableSize) || op > static_cast<uint32_t>(Op::kDownload))
    return false;
  req.op = static_cast<Op>(op);
  if (!rd.Bytes(token, req.token.size()) || token.size() != req.token.size()) return false;
  std::memcpy(req.token.data(), token.data(), token.size());
  if (!rd.U32(req.min_ratings) || !rd.U32(req.max_ratings) || !rd.U32(req.content_mask) ||
      !rd.U32(req.start) || !rd.U32(req.count) || !rd.U64(req.owner) ||
      !rd.U32(req.record_id) || !rd.U32(req.file_id) || !rd.U32(req.max_bytes) ||
      !rd.U8(req.rating) || !rd.U8(req.content) || !rd.Str(req.gamertag, 64) || !rd.U32(nfiles))
    return false;
  if (nfiles > kMaxFiles) return false;
  req.files.resize(nfiles);
  for (auto& f : req.files)
    if (!rd.Bytes(f, kMaxPhotoBytes)) return false;
  if (!rd.Bytes(req.userdata, kUserDataMax)) return false;
  return rd.AtEnd();
}

std::vector<uint8_t> EncodeResponse(const Response& resp) {
  Writer w;
  w.U32(kWireMagic);
  w.U32(resp.err);
  w.U32(resp.count);
  w.U32(resp.num_ratings);
  w.F32(resp.average);
  w.U32(static_cast<uint32_t>(resp.records.size()));
  for (const auto& r : resp.records) WriteRecord(w, r);
  w.Bytes(resp.data);
  return std::move(w.buffer());
}

bool DecodeResponse(const std::vector<uint8_t>& bytes, Response& resp) {
  Reader rd(bytes.data(), bytes.size());
  uint32_t magic, nrec;
  if (!rd.U32(magic) || magic != kWireMagic || !rd.U32(resp.err) || !rd.U32(resp.count) ||
      !rd.U32(resp.num_ratings) || !rd.F32(resp.average) || !rd.U32(nrec))
    return false;
  if (nrec > 256) return false;
  resp.records.resize(nrec);
  for (auto& r : resp.records)
    if (!ReadRecord(rd, r)) return false;
  if (!rd.Bytes(resp.data, kMaxPhotoBytes)) return false;
  return rd.AtEnd();
}

std::string SanitizeGamertag(const std::string& raw) {
  std::string out;
  for (char c : raw) {
    if (out.size() >= kGamertagMax) break;
    if (c >= 0x20 && c < 0x7F) out.push_back(c);
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out.empty() ? std::string("Player") : out;
}

uint32_t LoadBe32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void StoreBe32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}

}  // namespace rmr
