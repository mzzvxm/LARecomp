// A check that needs no game: the stats-view and session-property readers and the JSON writer of online_tables_core.h, fed a
// made-up guest image (nothing from the game's own files is in here). Build:
//   cmake --build <build dir> --target larecomp_online_tables_test, then run it [sample.json]; exit 0 = passed.
// With a file name it also writes the JSON of the made-up tables there (for a launcher test to read).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "online_tables_core.h"

using namespace larecomp::online_tables;

namespace {

int g_checks = 0;
int g_failed = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    ++g_checks;                                                           \
    if (!(cond)) {                                                        \
      ++g_failed;                                                         \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
    }                                                                     \
  } while (0)

// ------------------------------------------------------------- guest image ---

// Memory made of separate segments: a read must fall inside one.
class FakeGuest final : public GuestReader {
 public:
  void Put(uint32_t addr, std::vector<uint8_t> bytes) { segs_[addr] = std::move(bytes); }
  std::vector<uint8_t>& Seg(uint32_t addr) { return segs_[addr]; }
  void Drop(uint32_t addr) { segs_.erase(addr); }
  bool Read(uint32_t addr, void* out, uint32_t len) const override {
    for (const auto& [base, bytes] : segs_) {
      if (addr >= base && uint64_t(addr) + len <= uint64_t(base) + bytes.size()) {
        std::memcpy(out, bytes.data() + (addr - base), len);
        return true;
      }
    }
    return false;
  }

 private:
  std::map<uint32_t, std::vector<uint8_t>> segs_;
};

void P16(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(uint8_t(x >> 8));
  v.push_back(uint8_t(x));
}
void P32(std::vector<uint8_t>& v, uint32_t x) {
  P16(v, x >> 16);
  P16(v, x & 0xFFFF);
}
void P64(std::vector<uint8_t>& v, uint64_t x) {
  P32(v, uint32_t(x >> 32));
  P32(v, uint32_t(x));
}
void PStr(std::vector<uint8_t>& v, const char* s) {
  v.insert(v.end(), s, s + std::strlen(s));
}
void Pad(std::vector<uint8_t>& v, size_t n) { v.insert(v.end(), n, 0); }

struct Col {
  uint32_t prop;
  uint16_t attr;
  uint16_t agg;
};

// One XVC2 section: shared views (columns, `rows` skipped rows, an XPBM block of `xpbm` words) and views.
std::vector<uint8_t> MakeXvc2(const std::vector<std::vector<Col>>& shared, uint32_t rows, uint32_t xpbm,
                              const std::vector<std::pair<uint32_t, uint16_t>>& views, bool break_xpbm = false) {
  std::vector<uint8_t> v;
  PStr(v, "XVC2");
  Pad(v, 8);
  P16(v, uint32_t(shared.size()));
  for (const auto& cols : shared) {
    P16(v, uint32_t(cols.size()));
    P16(v, rows);
    Pad(v, 8);
    for (const Col& c : cols) {
      const size_t at = v.size();
      Pad(v, 32);
      v[at + 3] = 0x77;  // a word this parser does not interpret: it must survive in raw
      v[at + 4] = uint8_t(c.prop >> 24); v[at + 5] = uint8_t(c.prop >> 16); v[at + 6] = uint8_t(c.prop >> 8); v[at + 7] = uint8_t(c.prop);
      v[at + 12] = uint8_t(c.attr >> 8); v[at + 13] = uint8_t(c.attr);
      v[at + 16] = uint8_t(c.agg >> 8); v[at + 17] = uint8_t(c.agg);
    }
    Pad(v, 32 * rows);
    PStr(v, break_xpbm ? "XPBX" : "XPBM");
    Pad(v, 12);
    P32(v, xpbm);
    Pad(v, 4 * xpbm);
  }
  P16(v, uint32_t(views.size()));
  for (const auto& [id, set] : views) {
    const size_t at = v.size();
    Pad(v, 16);
    v[at + 0] = uint8_t(id >> 24); v[at + 1] = uint8_t(id >> 16); v[at + 2] = uint8_t(id >> 8); v[at + 3] = uint8_t(id);
    v[at + 4] = 0xAB;  // another uninterpreted word
    v[at + 8] = uint8_t(set >> 8); v[at + 9] = uint8_t(set);
    v[at + 15] = 0x5A;
  }
  return v;
}

// An XDBF holding a made-up section 2 entry and the XVC2 entry (when `xvc2_id` is its real id), a free space table of
// `free_count` entries, then the data.
std::vector<uint8_t> MakeXdbf(const std::vector<uint8_t>& xvc2, uint64_t xvc2_id = 0x58564332ull, uint32_t free_count = 1) {
  const uint32_t entries = 2;
  std::vector<uint8_t> v;
  PStr(v, "XDBF");
  P32(v, 0x10000);          // version
  P32(v, entries + 3);      // entry table length (room for more than are used)
  P32(v, entries);          // entries in use
  P32(v, free_count);       // free space table length
  P32(v, free_count);       // free space entries
  // entry 0: an image resource, 4 bytes at the start of the data
  P16(v, 2); P64(v, 1); P32(v, 0); P32(v, 4);
  // entry 1: the XVC2 resource right after it
  P16(v, 1); P64(v, xvc2_id); P32(v, 4); P32(v, uint32_t(xvc2.size()));
  Pad(v, 3 * 18);           // the unused entries
  Pad(v, 8 * free_count);   // free space table
  Pad(v, 4);                // entry 0's data
  v.insert(v.end(), xvc2.begin(), xvc2.end());
  return v;
}

std::vector<uint8_t> SampleXvc2() {
  return MakeXvc2({{{0x10000001u, 0x0001, 0x0003}, {0x20000005u, 0x0002, 0x010B}},  // set 0: int32 sum, int64 max (+0x0100 in the agg word)
                   {{0x30000007u, 0x0063, 0x0001}}},                                 // set 1: double last
                  /*rows*/ 1, /*xpbm*/ 2,
                  {{382, 0}, {383, 1}, {400, 7} /* no such shared view: skipped */, {0xFFFFFFF0u, 0}});
}

// ------------------------------------------------------------ JSON checker ---

// A strict parser into a small tree, so the writer's output is checked as JSON and not as a string.
struct J {
  enum Kind { kNull, kBool, kNum, kStr, kArr, kObj } kind = kNull;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<J> a;
  std::vector<std::pair<std::string, J>> o;
  const J* Get(const char* key) const {
    for (const auto& kv : o)
      if (kv.first == key) return &kv.second;
    return nullptr;
  }
};

struct Parser {
  const std::string& t;
  size_t i = 0;
  bool ok = true;
  explicit Parser(const std::string& text) : t(text) {}
  void Ws() {
    while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) ++i;
  }
  bool Lit(const char* w) {
    const size_t n = std::strlen(w);
    if (t.compare(i, n, w) != 0) return ok = false;
    i += n;
    return true;
  }
  static bool Hex4(const std::string& s, size_t at, uint32_t* v) {
    if (at + 4 > s.size()) return false;
    *v = 0;
    for (size_t k = 0; k < 4; ++k) {
      const char c = s[at + k];
      uint32_t d;
      if (c >= '0' && c <= '9') d = c - '0';
      else if (c >= 'a' && c <= 'f') d = 10 + c - 'a';
      else if (c >= 'A' && c <= 'F') d = 10 + c - 'A';
      else return false;
      *v = *v * 16 + d;
    }
    return true;
  }
  static void PutUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out.push_back(char(cp));
    else if (cp < 0x800) { out.push_back(char(0xC0 | (cp >> 6))); out.push_back(char(0x80 | (cp & 63))); }
    else { out.push_back(char(0xE0 | (cp >> 12))); out.push_back(char(0x80 | ((cp >> 6) & 63))); out.push_back(char(0x80 | (cp & 63))); }
  }
  // The bytes of a string must be valid UTF-8 (checked on the way: lead + the right number of continuation bytes).
  bool Str(std::string* out) {
    if (i >= t.size() || t[i] != '"') return ok = false;
    ++i;
    out->clear();
    while (i < t.size() && t[i] != '"') {
      const unsigned char c = static_cast<unsigned char>(t[i]);
      if (c < 0x20) return ok = false;
      if (c == '\\') {
        if (i + 1 >= t.size()) return ok = false;
        const char e = t[i + 1];
        i += 2;
        switch (e) {
          case '"': out->push_back('"'); break;
          case '\\': out->push_back('\\'); break;
          case '/': out->push_back('/'); break;
          case 'n': out->push_back('\n'); break;
          case 'r': out->push_back('\r'); break;
          case 't': out->push_back('\t'); break;
          case 'b': out->push_back('\b'); break;
          case 'f': out->push_back('\f'); break;
          case 'u': {
            uint32_t cp;
            if (!Hex4(t, i, &cp)) return ok = false;
            i += 4;
            PutUtf8(*out, cp);
            break;
          }
          default: return ok = false;
        }
        continue;
      }
      size_t len = 1;
      if (c >= 0xC2 && c <= 0xDF) len = 2;
      else if (c >= 0xE0 && c <= 0xEF) len = 3;
      else if (c >= 0xF0 && c <= 0xF4) len = 4;
      else if (c >= 0x80) return ok = false;
      for (size_t k = 1; k < len; ++k) {
        if (i + k >= t.size() || (static_cast<unsigned char>(t[i + k]) & 0xC0) != 0x80) return ok = false;
      }
      out->append(t, i, len);
      i += len;
    }
    if (i >= t.size()) return ok = false;
    ++i;
    return true;
  }
  bool Val(J* v) {
    Ws();
    if (i >= t.size()) return ok = false;
    const char c = t[i];
    if (c == '{') {
      v->kind = J::kObj;
      ++i;
      Ws();
      if (i < t.size() && t[i] == '}') { ++i; return true; }
      for (;;) {
        Ws();
        std::string k;
        if (!Str(&k)) return false;
        Ws();
        if (i >= t.size() || t[i] != ':') return ok = false;
        ++i;
        J child;
        if (!Val(&child)) return false;
        v->o.emplace_back(std::move(k), std::move(child));
        Ws();
        if (i < t.size() && t[i] == ',') { ++i; continue; }
        if (i < t.size() && t[i] == '}') { ++i; return true; }
        return ok = false;
      }
    }
    if (c == '[') {
      v->kind = J::kArr;
      ++i;
      Ws();
      if (i < t.size() && t[i] == ']') { ++i; return true; }
      for (;;) {
        J child;
        if (!Val(&child)) return false;
        v->a.push_back(std::move(child));
        Ws();
        if (i < t.size() && t[i] == ',') { ++i; continue; }
        if (i < t.size() && t[i] == ']') { ++i; return true; }
        return ok = false;
      }
    }
    if (c == '"') { v->kind = J::kStr; return Str(&v->s); }
    if (c == 't') { v->kind = J::kBool; v->b = true; return Lit("true"); }
    if (c == 'f') { v->kind = J::kBool; v->b = false; return Lit("false"); }
    if (c == 'n') { v->kind = J::kNull; return Lit("null"); }
    // a number: -?digits, no fraction needed here
    size_t j = i;
    if (j < t.size() && t[j] == '-') ++j;
    const size_t d0 = j;
    while (j < t.size() && t[j] >= '0' && t[j] <= '9') ++j;
    if (j == d0 || (t[d0] == '0' && j - d0 > 1)) return ok = false;
    v->kind = J::kNum;
    v->n = std::stod(t.substr(i, j - i));
    i = j;
    return true;
  }
};

bool ParseJson(const std::string& text, J* out) {
  Parser p(text);
  if (!p.Val(out)) return false;
  p.Ws();
  return p.ok && p.i == text.size();
}

// ------------------------------------------------------------------ tests ---

void TestStatsParse() {
  FakeGuest g;
  g.Put(kSpaAddress, MakeXdbf(SampleXvc2()));
  StatsSchema s;
  CHECK(ReadStatsSchema(g, kSpaAddress, &s) == StatsStatus::kOk);
  CHECK(s.column_sets.size() == 2);
  CHECK(s.column_sets[0].size() == 2 && s.column_sets[1].size() == 1);
  CHECK(s.column_sets[0][0].prop == 0x10000001u && s.column_sets[0][0].attr == 1 && s.column_sets[0][0].agg == 3);
  CHECK(s.column_sets[0][1].prop == 0x20000005u && s.column_sets[0][1].attr == 2);
  CHECK(s.column_sets[0][1].agg == 0x0B);  // the low byte of 0x010B: the game's own reader keeps the low byte
  CHECK(s.column_sets[1][0].prop == 0x30000007u && s.column_sets[1][0].attr == 0x63 && s.column_sets[1][0].agg == 1);
  CHECK(s.column_sets[0][0].raw[3] == 0x77);  // the word nothing here reads is still there
  // the view with an index past the shared views is dropped, as the game's reader does
  CHECK(s.views.size() == 3);
  CHECK(s.views[0].id == 382 && s.views[0].column_set == 0);
  CHECK(s.views[1].id == 383 && s.views[1].column_set == 1);
  CHECK(s.views[2].id == 0xFFFFFFF0u && s.views[2].column_set == 0);
  CHECK(s.views[0].raw[4] == 0xAB && s.views[0].raw[15] == 0x5A);

  // no free space table, a different table length: the data offset follows the header
  FakeGuest g2;
  g2.Put(kSpaAddress, MakeXdbf(SampleXvc2(), 0x58564332ull, 0));
  StatsSchema s2;
  CHECK(ReadStatsSchema(g2, kSpaAddress, &s2) == StatsStatus::kOk && s2.views.size() == 3);
}

void TestStatsFailures() {
  StatsSchema s;
  FakeGuest none;
  CHECK(ReadStatsSchema(none, kSpaAddress, &s) == StatsStatus::kUnreadable);
  CHECK(s.views.empty() && s.column_sets.empty());

  FakeGuest not_xdbf;
  std::vector<uint8_t> junk(64, 0x11);
  not_xdbf.Put(kSpaAddress, junk);
  CHECK(ReadStatsSchema(not_xdbf, kSpaAddress, &s) == StatsStatus::kNoXdbf);

  FakeGuest no_xvc2;  // the entry's id is not "XVC2"
  no_xvc2.Put(kSpaAddress, MakeXdbf(SampleXvc2(), 0x58564333ull));
  CHECK(ReadStatsSchema(no_xvc2, kSpaAddress, &s) == StatsStatus::kNoXvc2);

  FakeGuest bad_magic;  // the entry points at something that is not an XVC2 block
  auto bytes = MakeXdbf(SampleXvc2());
  const size_t magic_at = 0x18 + 5 * 18 + 8 + 4;
  CHECK(std::memcmp(bytes.data() + magic_at, "XVC2", 4) == 0);  // the layout the test assumes
  bytes[magic_at] = 'Z';
  bad_magic.Put(kSpaAddress, bytes);
  CHECK(ReadStatsSchema(bad_magic, kSpaAddress, &s) == StatsStatus::kNoXvc2);

  FakeGuest bad_xpbm;
  bad_xpbm.Put(kSpaAddress, MakeXdbf(MakeXvc2({{{0x10000001u, 1, 3}}}, 0, 0, {{1, 0}}, /*break_xpbm*/ true)));
  CHECK(ReadStatsSchema(bad_xpbm, kSpaAddress, &s) == StatsStatus::kBadSharedView);
  CHECK(s.views.empty());

  // the memory ends in the middle of the views
  FakeGuest cut;
  auto whole = MakeXdbf(SampleXvc2());
  whole.resize(whole.size() - 20);
  cut.Put(kSpaAddress, whole);
  CHECK(ReadStatsSchema(cut, kSpaAddress, &s) == StatsStatus::kUnreadable);
  CHECK(s.views.empty() && s.column_sets.empty());

  CHECK(std::strlen(StatsStatusText(StatsStatus::kNoXvc2)) > 0);
  CHECK(std::strlen(StatsStatusText(StatsStatus::kOk)) > 0);

  // a null out is fine
  FakeGuest ok;
  ok.Put(kSpaAddress, MakeXdbf(SampleXvc2()));
  CHECK(ReadStatsSchema(ok, kSpaAddress, nullptr) == StatsStatus::kOk);
}

// A property record: id, name pointer, up to 16 value pointers.
void PutRecord(std::vector<uint8_t>& table, int index, uint32_t id, uint32_t name_ptr, const std::vector<uint32_t>& values) {
  uint8_t* r = table.data() + size_t(index) * kPropertyRecordSize;
  auto put = [&](size_t off, uint32_t v) {
    r[off] = uint8_t(v >> 24); r[off + 1] = uint8_t(v >> 16); r[off + 2] = uint8_t(v >> 8); r[off + 3] = uint8_t(v);
  };
  put(0, id);
  put(4, name_ptr);
  for (size_t k = 0; k < values.size() && k < 16; ++k) put(8 + 4 * k, values[k]);
}

void TestProperties() {
  constexpr uint32_t kText = 0x82900000;
  FakeGuest g;
  // empty (not yet filled) table
  g.Put(kPropertyTable, std::vector<uint8_t>(kPropertyRecords * kPropertyRecordSize, 0));
  std::vector<PropertyDef> defs;
  CHECK(!ReadPropertyTable(g, kPropertyTable, &defs) && defs.empty());

  // strings: "GameMode" at +0, "Race" +16, "Cruise" +32, a 40-byte word +48 (cut at 32), "Style" +100...
  std::vector<uint8_t> text(256, 0);
  auto put_str = [&](size_t at, const char* s) { std::memcpy(text.data() + at, s, std::strlen(s)); };
  put_str(0, "GameMode");
  put_str(16, "Race");
  put_str(32, "Cruise");
  put_str(48, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ABCD");  // 40 characters
  put_str(100, "GameStyle");
  put_str(120, "Tournament");
  // a name that ends right at the end of the segment: only a byte by byte read can get it
  std::vector<uint8_t> tail(5, 0);
  std::memcpy(tail.data(), "Edge", 4);  // "Edge\0" at the very end
  g.Put(kText, text);
  g.Put(0x82910000, tail);

  std::vector<uint8_t> table(kPropertyRecords * kPropertyRecordSize, 0);
  PutRecord(table, 0, 0x10000001u, kText, {kText + 16, kText + 32});
  PutRecord(table, 1, 0, 0, {});                                  // unused
  PutRecord(table, 2, 0x10000002u, kText + 100, {kText + 120, kText + 48, 0x1234 /* not a pointer: ends the values */, kText + 16});
  PutRecord(table, 3, 0xFFFFFFFFu, kText, {});                    // the game's "none"
  PutRecord(table, 4, 0x10000009u, 0x82910000, {0x82910000});     // the end-of-segment strings
  PutRecord(table, 5, 0x1000000Au, 0x83000000 /* nowhere */, {});  // a name that cannot be read: empty, the record stays
  g.Put(kPropertyTable, table);

  CHECK(ReadPropertyTable(g, kPropertyTable, &defs));
  CHECK(defs.size() == 4);
  CHECK(defs[0].id == 0x10000001u && defs[0].name == "GameMode");
  CHECK(defs[0].values.size() == 2 && defs[0].values[0] == "Race" && defs[0].values[1] == "Cruise");
  CHECK(defs[1].id == 0x10000002u && defs[1].name == "GameStyle");
  CHECK(defs[1].values.size() == 2 && defs[1].values[0] == "Tournament");
  CHECK(defs[1].values[1] == "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345");  // 32 characters, no more
  CHECK(defs[2].id == 0x10000009u && defs[2].name == "Edge" && defs[2].values.size() == 1 && defs[2].values[0] == "Edge");
  CHECK(defs[3].id == 0x1000000Au && defs[3].name.empty() && defs[3].values.empty());

  // the table in memory is not readable: false, and nothing out
  g.Drop(kPropertyTable);
  CHECK(!ReadPropertyTable(g, kPropertyTable, &defs) && defs.empty());

  // only unused records after a set first one: still a table (GameMode alone), but a first record with id 0 is not
  FakeGuest one;
  std::vector<uint8_t> t1(kPropertyRecords * kPropertyRecordSize, 0);
  PutRecord(t1, 0, 7, kText, {});
  one.Put(kText, text);
  one.Put(kPropertyTable, t1);
  CHECK(ReadPropertyTable(one, kPropertyTable, &defs) && defs.size() == 1 && defs[0].name == "GameMode" && defs[0].values.empty());
  std::vector<uint8_t> t2(kPropertyRecords * kPropertyRecordSize, 0);
  PutRecord(t2, 1, 7, kText, {});  // record 1 set, record 0 not
  one.Put(kPropertyTable, t2);
  CHECK(!ReadPropertyTable(one, kPropertyTable, &defs));
}

Tables SampleTables() {
  Tables t;
  t.game_version = "PATCH 10 - PREVIEW 2";
  FakeGuest g;
  g.Put(kSpaAddress, MakeXdbf(SampleXvc2()));
  t.stats_available = ReadStatsSchema(g, kSpaAddress, &t.stats) == StatsStatus::kOk;
  t.properties_available = true;
  t.properties.push_back({0x10000001u, "GameMode", {"Race", "Cruise"}});
  t.properties.push_back({0x20000002u, "Say \"hi\"\\", {"a\tb", std::string("c\x01" "d"), "caf\xC3\xA9", "\xE2\x82\xAC"}});
  t.properties.push_back({0xFFFFFFF0u, "", {}});
  return t;
}

void TestJson() {
  const Tables t = SampleTables();
  const std::string text = ToJson(t);
  CHECK(ToJson(t) == text);  // deterministic
  J root;
  CHECK(ParseJson(text, &root));
  CHECK(root.kind == J::kObj);
  if (root.kind != J::kObj) return;
  CHECK(root.Get("schema") && root.Get("schema")->n == kSchemaVersion);
  CHECK(root.Get("game_version") && root.Get("game_version")->s == "PATCH 10 - PREVIEW 2");

  const J* stats = root.Get("stats");
  CHECK(stats && stats->kind == J::kObj);
  if (stats) {
    CHECK(stats->Get("available") && stats->Get("available")->kind == J::kBool && stats->Get("available")->b);
    CHECK(!stats->Get("error"));
    const J* sets = stats->Get("column_sets");
    CHECK(sets && sets->kind == J::kArr && sets->a.size() == 2);
    if (sets && sets->a.size() == 2) {
      CHECK(sets->a[0].a.size() == 2 && sets->a[1].a.size() == 1);
      const J& c = sets->a[0].a[1];
      CHECK(c.Get("prop") && c.Get("prop")->n == 0x20000005);
      CHECK(c.Get("type") && c.Get("type")->n == 2);
      CHECK(c.Get("attr") && c.Get("attr")->n == 2);
      CHECK(c.Get("agg") && c.Get("agg")->n == 0x0B);
      CHECK(c.Get("raw") && c.Get("raw")->s.size() == 64);
      CHECK(sets->a[0].a[0].Get("raw")->s.substr(0, 8) == "00000077");  // the uninterpreted byte, as hex
    }
    const J* views = stats->Get("views");
    CHECK(views && views->a.size() == 3);
    if (views && views->a.size() == 3) {
      CHECK(views->a[0].Get("id")->n == 382 && views->a[0].Get("column_set")->n == 0);
      CHECK(views->a[1].Get("id")->n == 383 && views->a[1].Get("column_set")->n == 1);
      CHECK(views->a[2].Get("id")->n == 4294967280.0);  // above 2^31: unsigned
      CHECK(views->a[0].Get("raw")->s.size() == 32);
    }
  }

  const J* props = root.Get("properties");
  CHECK(props && props->Get("available") && props->Get("available")->b);
  const J* items = props ? props->Get("items") : nullptr;
  CHECK(items && items->a.size() == 3);
  if (items && items->a.size() == 3) {
    CHECK(items->a[0].Get("id")->n == 0x10000001 && items->a[0].Get("name")->s == "GameMode");
    CHECK(items->a[0].Get("values")->a.size() == 2 && items->a[0].Get("values")->a[1].s == "Cruise");
    CHECK(items->a[1].Get("name")->s == "Say \"hi\"\\");
    const auto& vals = items->a[1].Get("values")->a;
    CHECK(vals.size() == 4 && vals[0].s == "a\tb" && vals[1].s == std::string("c\x01" "d"));
    CHECK(vals[2].s == "caf\xC3\xA9" && vals[3].s == "\xE2\x82\xAC");  // valid UTF-8 goes through
    CHECK(items->a[2].Get("name")->s.empty() && items->a[2].Get("values")->a.empty());
  }
  CHECK(text.find("\\u0001") != std::string::npos);  // a control character is escaped, not written raw
  CHECK(text.find('\t') == std::string::npos || text.find("\\t") != std::string::npos);
}

void TestJsonHostileText() {
  Tables t;
  t.properties_available = true;
  // bytes that are not UTF-8 (a lone continuation byte, a truncated sequence, an overlong, a surrogate, DEL, a NUL inside)
  t.properties.push_back({1, std::string("\xFF" "A\x80" "B\xC3" "C\xC0\xAF" "D\xED\xA0\x80" "E\x7F" "F\0" "G", 17),
                          {"\xF0\x9F\x98", "ok", "\xF4\x90\x80\x80"}});
  const std::string text = ToJson(t);
  J root;
  CHECK(ParseJson(text, &root));  // valid JSON with valid UTF-8 whatever went in
  const J* items = root.Get("properties") ? root.Get("properties")->Get("items") : nullptr;
  CHECK(items && items->a.size() == 1);
  if (items && items->a.size() == 1) {
    const std::string& name = items->a[0].Get("name")->s;
    CHECK(name.find('\xFF') == std::string::npos && name.find('\x80') == std::string::npos);
    CHECK(name.find('A') != std::string::npos && name.find('G') != std::string::npos);
    CHECK(name.find('\0') != std::string::npos);  // the NUL is kept as an escape, not dropped
    const auto& vals = items->a[0].Get("values")->a;
    CHECK(vals.size() == 3 && vals[1].s == "ok");
    CHECK(vals[0].s.find('\xF0') == std::string::npos && vals[2].s.find('\xF4') == std::string::npos);
  }
}

void TestJsonUnavailable() {
  Tables t;
  t.game_version = "x";
  t.stats_error = "no XDBF resource at the expected address";
  const std::string text = ToJson(t);
  J root;
  CHECK(ParseJson(text, &root));
  const J* stats = root.Get("stats");
  CHECK(stats && stats->Get("available") && !stats->Get("available")->b);
  CHECK(stats && stats->Get("error") && stats->Get("error")->s == t.stats_error);
  CHECK(stats && stats->Get("column_sets") && stats->Get("column_sets")->a.empty());
  CHECK(stats && stats->Get("views") && stats->Get("views")->a.empty());
  const J* props = root.Get("properties");
  CHECK(props && props->Get("available") && !props->Get("available")->b);
  CHECK(props && props->Get("items") && props->Get("items")->a.empty());

  // golden text: the file's layout is what a launcher reads, so a change here is a change to say out loud
  Tables g;
  g.game_version = "v";
  g.properties_available = true;
  g.properties.push_back({5, "Mode", {"A", "B"}});
  const std::string want =
      "{\n  \"schema\": 1,\n  \"game_version\": \"v\",\n  \"stats\": {\n    \"available\": false,\n    \"error\": \"\",\n"
      "    \"column_sets\": [],\n    \"views\": []\n  },\n  \"properties\": {\n    \"available\": true,\n    \"items\": [\n"
      "      {\"id\": 5, \"name\": \"Mode\", \"values\": [\"A\", \"B\"]}\n    ]\n  }\n}\n";
  CHECK(ToJson(g) == want);
}

void TestParserSelfCheck() {
  // the checker above has to say no to bad JSON, or its yes means nothing
  J j;
  CHECK(!ParseJson("{\"a\": 1,}", &j));
  CHECK(!ParseJson("{\"a\": 01}", &j));
  CHECK(!ParseJson("[1, 2", &j));
  CHECK(!ParseJson("{\"a\": \"b\nc\"}", &j));
  CHECK(!ParseJson("{\"a\": \"\xFF\"}", &j));
  CHECK(!ParseJson("{} x", &j));
  CHECK(ParseJson("{\"a\": [1, -2, true, null, \"\\u00e9\"]}", &j));
}

}  // namespace

int main(int argc, char** argv) {
  TestParserSelfCheck();
  TestStatsParse();
  TestStatsFailures();
  TestProperties();
  TestJson();
  TestJsonHostileText();
  TestJsonUnavailable();
  if (argc > 1) {
    const std::string text = ToJson(SampleTables());
    if (FILE* f = std::fopen(argv[1], "wb")) {
      std::fwrite(text.data(), 1, text.size(), f);
      std::fclose(f);
    } else {
      std::printf("cannot write %s\n", argv[1]);
      ++g_failed;
    }
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
