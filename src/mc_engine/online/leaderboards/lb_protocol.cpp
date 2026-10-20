#include "lb_protocol.h"

#include <cstring>

namespace lb {

namespace {

using rmr::Reader;
using rmr::Writer;

void WriteValue(Writer& w, const Value& v) {
  w.U32(v.attr);
  w.U8(v.type);
  w.U8(v.agg);
  w.I64(v.bits);
}

bool ReadValue(Reader& rd, Value& v) {
  uint32_t attr;
  if (!rd.U32(attr) || attr > 0xFFFF) return false;
  v.attr = static_cast<uint16_t>(attr);
  return rd.U8(v.type) && rd.U8(v.agg) && rd.I64(v.bits);
}

void WriteRow(Writer& w, const Row& r) {
  w.U64(r.owner);
  w.Str(r.gamertag);
  w.U32(r.rank);
  w.I64(r.rating);
  w.I64(r.updated);
  w.U32(static_cast<uint32_t>(r.columns.size()));
  for (const Value& v : r.columns) WriteValue(w, v);
}

bool ReadRow(Reader& rd, Row& r) {
  uint32_t n;
  if (!rd.U64(r.owner) || !rd.Str(r.gamertag, rmr::kGamertagMax) || !rd.U32(r.rank) ||
      !rd.I64(r.rating) || !rd.I64(r.updated) || !rd.U32(n) || n > kMaxColumns) {
    return false;
  }
  r.columns.resize(n);
  for (Value& v : r.columns) {
    if (!ReadValue(rd, v)) return false;
  }
  return true;
}

double AsDouble(uint8_t type, int64_t bits) {
  if (type == kTypeDouble) {
    double d;
    std::memcpy(&d, &bits, sizeof(d));
    return d;
  }
  float f;
  const uint32_t lo = static_cast<uint32_t>(bits);
  std::memcpy(&f, &lo, sizeof(f));
  return f;
}

int64_t FromDouble(uint8_t type, double d) {
  if (type == kTypeDouble) {
    int64_t bits;
    std::memcpy(&bits, &d, sizeof(bits));
    return bits;
  }
  const float f = static_cast<float>(d);
  uint32_t lo;
  std::memcpy(&lo, &f, sizeof(lo));
  return static_cast<int64_t>(lo);
}

bool IsFloat(uint8_t type) { return type == kTypeDouble || type == kTypeFloat; }

}  // namespace

const char* OpName(Op op) {
  switch (op) {
    case Op::kSize: return "Size";
    case Op::kByRank: return "ByRank";
    case Op::kByPivot: return "ByPivot";
    case Op::kByOwners: return "ByOwners";
    case Op::kWrite: return "Write";
  }
  return "?";
}

int CompareValues(uint8_t type, int64_t a, int64_t b) {
  if (IsFloat(type)) {
    const double x = AsDouble(type, a), y = AsDouble(type, b);
    return x < y ? -1 : (x > y ? 1 : 0);
  }
  if (type == kTypeInt32) {
    const int32_t x = static_cast<int32_t>(a), y = static_cast<int32_t>(b);
    return x < y ? -1 : (x > y ? 1 : 0);
  }
  return a < b ? -1 : (a > b ? 1 : 0);
}

int64_t Aggregate(uint8_t type, uint8_t agg, bool have, int64_t current, int64_t incoming) {
  if (!have) return incoming;
  switch (agg) {
    case kAggSum:
      if (IsFloat(type)) return FromDouble(type, AsDouble(type, current) + AsDouble(type, incoming));
      if (type == kTypeInt32) {
        return static_cast<int64_t>(static_cast<int32_t>(
            static_cast<uint32_t>(current) + static_cast<uint32_t>(incoming)));
      }
      return static_cast<int64_t>(static_cast<uint64_t>(current) + static_cast<uint64_t>(incoming));
    case kAggMin:
      return CompareValues(type, incoming, current) < 0 ? incoming : current;
    case kAggMax:
      return CompareValues(type, incoming, current) > 0 ? incoming : current;
    case kAggLast:
    default:
      return incoming;
  }
}

std::vector<uint8_t> EncodeRequest(const Request& req) {
  Writer w;
  w.U32(kWireMagic);
  w.U32(static_cast<uint32_t>(req.op));
  w.Bytes(req.token.data(), req.token.size());
  w.Str(req.gamertag);
  w.U32(req.view);
  w.U32(req.start);
  w.U32(req.count);
  w.U64(req.pivot);
  w.U32(static_cast<uint32_t>(req.owners.size()));
  for (uint64_t o : req.owners) w.U64(o);
  w.U32(static_cast<uint32_t>(req.columns.size()));
  for (const Value& v : req.columns) WriteValue(w, v);
  return std::move(w.buffer());
}

bool DecodeRequest(const std::vector<uint8_t>& bytes, Request& req) {
  Reader rd(bytes.data(), bytes.size());
  uint32_t magic, op, nowners, ncols;
  std::vector<uint8_t> token;
  if (!rd.U32(magic) || magic != kWireMagic || !rd.U32(op) || op < 1 || op > 5 ||
      !rd.Bytes(token, req.token.size()) || token.size() != req.token.size() ||
      !rd.Str(req.gamertag, rmr::kGamertagMax) || !rd.U32(req.view) || !rd.U32(req.start) ||
      !rd.U32(req.count) || !rd.U64(req.pivot) || !rd.U32(nowners) || nowners > kMaxOwners) {
    return false;
  }
  req.op = static_cast<Op>(op);
  std::memcpy(req.token.data(), token.data(), token.size());
  req.owners.resize(nowners);
  for (uint64_t& o : req.owners) {
    if (!rd.U64(o)) return false;
  }
  if (!rd.U32(ncols) || ncols > kMaxColumns) return false;
  req.columns.resize(ncols);
  for (Value& v : req.columns) {
    if (!ReadValue(rd, v)) return false;
  }
  return rd.AtEnd();
}

std::vector<uint8_t> EncodeResponse(const Response& resp) {
  Writer w;
  w.U32(kWireMagic);
  w.U32(resp.err);
  w.U32(resp.total);
  w.U32(static_cast<uint32_t>(resp.rows.size()));
  for (const Row& r : resp.rows) WriteRow(w, r);
  return std::move(w.buffer());
}

bool DecodeResponse(const std::vector<uint8_t>& bytes, Response& resp) {
  Reader rd(bytes.data(), bytes.size());
  uint32_t magic, nrows;
  if (!rd.U32(magic) || magic != kWireMagic || !rd.U32(resp.err) || !rd.U32(resp.total) ||
      !rd.U32(nrows) || nrows > kMaxRows + kMaxOwners) {
    return false;
  }
  resp.rows.resize(nrows);
  for (Row& r : resp.rows) {
    if (!ReadRow(rd, r)) return false;
  }
  return rd.AtEnd();
}

}  // namespace lb
