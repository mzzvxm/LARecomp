#pragma once
// The pure half of the Linux crash reporter (crash_handler.cpp, crash_report_linux.h): text
// building, a timestamp, the /proc/self/maps parser and an ELF symbol lookup.
//
// Everything here may run inside a signal handler, so it uses no heap, no locks, no iostreams and
// no libc function that is not async-signal-safe: not even memcpy (the compiler's builtin is
// used). The only outside calls are the two reader callbacks the caller passes in (read(2) and
// pread(2) on Linux, a memory buffer in the unit test, src/crash_report_test.cpp, which compiles
// on any host).

#include <stddef.h>
#include <stdint.h>

namespace larecomp::crash {

// ---------------------------------------------------------------------------------------------
// Text: appends into a buffer the caller owns. Never writes past the end and never allocates.
// There is no terminating NUL: pass data() and size() to write(2).
// ---------------------------------------------------------------------------------------------
class Text {
 public:
  Text(char* buffer, size_t capacity) : buf_(buffer), cap_(capacity) {}

  void Clear() {
    len_ = 0;
    truncated_ = false;
  }
  const char* data() const { return buf_; }
  size_t size() const { return len_; }
  bool truncated() const { return truncated_; }

  void Char(char c) {
    if (len_ < cap_) {
      buf_[len_++] = c;
    } else {
      truncated_ = true;
    }
  }
  void Chars(const char* s, size_t n) {
    for (size_t i = 0; i < n; ++i) Char(s[i]);
  }
  void Str(const char* s) {
    if (!s) return;
    while (*s) Char(*s++);
  }
  // Upper-case hex, at least `digits` digits (zero padded).
  void Hex(uint64_t v, int digits) {
    char tmp[16];
    int n = 0;
    do {
      tmp[n++] = "0123456789ABCDEF"[v & 15];
      v >>= 4;
    } while (v != 0 && n < 16);
    for (int i = n; i < digits; ++i) Char('0');
    while (n > 0) Char(tmp[--n]);
  }
  // Signed decimal, at least `width` characters (left padded with `pad`).
  void Dec(int64_t v, int width = 0, char pad = ' ') {
    char tmp[24];
    int n = 0;
    const bool neg = v < 0;
    uint64_t u = neg ? (0 - static_cast<uint64_t>(v)) : static_cast<uint64_t>(v);
    do {
      tmp[n++] = static_cast<char>('0' + (u % 10));
      u /= 10;
    } while (u != 0 && n < 22);
    int total = n + (neg ? 1 : 0);
    if (pad == '0') {
      if (neg) Char('-');
      for (int i = total; i < width; ++i) Char('0');
    } else {
      for (int i = total; i < width; ++i) Char(pad);
      if (neg) Char('-');
    }
    while (n > 0) Char(tmp[--n]);
  }

 private:
  char* buf_;
  size_t cap_;
  size_t len_ = 0;
  bool truncated_ = false;
};

// "[2026-11-13 14:05:09.123]" in the log file's own format (spdlog pattern %Y-%m-%d %H:%M:%S.%e).
// `unix_seconds` is UTC; `utc_offset_seconds` is the local offset (taken once at start: localtime_r
// is not async-signal-safe).
inline void AppendTimestamp(Text& t, int64_t unix_seconds, int millis, int utc_offset_seconds) {
  int64_t local = unix_seconds + utc_offset_seconds;
  int64_t days = local / 86400;
  int64_t rem = local % 86400;
  if (rem < 0) {
    rem += 86400;
    --days;
  }
  // Civil date from days since 1970-01-01 (H. Hinnant's days_from_civil, inverted).
  int64_t z = days + 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  int64_t doe = z - era * 146097;
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = yoe + era * 400;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int64_t mp = (5 * doy + 2) / 153;
  int64_t d = doy - (153 * mp + 2) / 5 + 1;
  int64_t m = mp < 10 ? mp + 3 : mp - 9;
  if (m <= 2) ++y;
  t.Char('[');
  t.Dec(y, 4, '0');
  t.Char('-');
  t.Dec(m, 2, '0');
  t.Char('-');
  t.Dec(d, 2, '0');
  t.Char(' ');
  t.Dec(rem / 3600, 2, '0');
  t.Char(':');
  t.Dec((rem / 60) % 60, 2, '0');
  t.Char(':');
  t.Dec(rem % 60, 2, '0');
  t.Char('.');
  t.Dec(millis, 3, '0');
  t.Char(']');
}

inline size_t CStrLen(const char* s) {
  size_t n = 0;
  while (s[n]) ++n;
  return n;
}

inline bool CStrEq(const char* a, const char* b) {
  while (*a && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

// Copies at most cap-1 characters and always terminates. Returns the length copied.
inline size_t CopyZ(char* dst, size_t cap, const char* src, size_t n) {
  if (cap == 0) return 0;
  size_t i = 0;
  for (; i < n && i + 1 < cap; ++i) dst[i] = src[i];
  dst[i] = 0;
  return i;
}

// The part after the last '/' (the whole string when there is none).
inline const char* BaseName(const char* path) {
  const char* base = path;
  for (const char* p = path; *p; ++p) {
    if (*p == '/') base = p + 1;
  }
  return base;
}

// ---------------------------------------------------------------------------------------------
// /proc/self/maps
// ---------------------------------------------------------------------------------------------

struct MapLine {
  uint64_t start = 0;
  uint64_t end = 0;
  uint64_t file_offset = 0;
  bool r = false, w = false, x = false;
  const char* path = nullptr;  // inside the parsed line, not terminated
  size_t path_len = 0;
};

namespace detail {
inline bool HexValue(char c, unsigned* out) {
  if (c >= '0' && c <= '9') {
    *out = static_cast<unsigned>(c - '0');
  } else if (c >= 'a' && c <= 'f') {
    *out = static_cast<unsigned>(c - 'a' + 10);
  } else if (c >= 'A' && c <= 'F') {
    *out = static_cast<unsigned>(c - 'A' + 10);
  } else {
    return false;
  }
  return true;
}
// Reads hex digits at line[*i...]; false when there is none.
inline bool ReadHex(const char* line, size_t len, size_t* i, uint64_t* out) {
  uint64_t v = 0;
  size_t n = 0;
  unsigned d;
  while (*i < len && HexValue(line[*i], &d)) {
    v = (v << 4) | d;
    ++*i;
    ++n;
  }
  *out = v;
  return n > 0;
}
inline void SkipSpaces(const char* line, size_t len, size_t* i) {
  while (*i < len && line[*i] == ' ') ++*i;
}
inline void SkipToken(const char* line, size_t len, size_t* i) {
  while (*i < len && line[*i] != ' ') ++*i;
}
}  // namespace detail

// "7f1c2d000000-7f1c2d021000 r-xp 00001000 08:01 1234567      /usr/lib/libfoo.so"
// `line` has no trailing newline. The path is empty for an anonymous mapping.
inline bool ParseMapsLine(const char* line, size_t len, MapLine* out) {
  size_t i = 0;
  if (!detail::ReadHex(line, len, &i, &out->start)) return false;
  if (i >= len || line[i] != '-') return false;
  ++i;
  if (!detail::ReadHex(line, len, &i, &out->end)) return false;
  if (i >= len || line[i] != ' ') return false;
  ++i;
  if (i + 4 > len) return false;
  out->r = line[i] == 'r';
  out->w = line[i + 1] == 'w';
  out->x = line[i + 2] == 'x';
  i += 4;
  if (i >= len || line[i] != ' ') return false;
  ++i;
  if (!detail::ReadHex(line, len, &i, &out->file_offset)) return false;
  detail::SkipSpaces(line, len, &i);
  detail::SkipToken(line, len, &i);  // device
  detail::SkipSpaces(line, len, &i);
  detail::SkipToken(line, len, &i);  // inode
  detail::SkipSpaces(line, len, &i);
  out->path = line + i;
  out->path_len = len - i;
  while (out->path_len > 0 && (out->path[out->path_len - 1] == ' ' ||
                               out->path[out->path_len - 1] == '\r')) {
    --out->path_len;
  }
  return true;
}

// The mapping that holds an address.
struct Region {
  bool found = false;
  uint64_t start = 0;
  uint64_t end = 0;
  uint64_t file_offset = 0;  // of `start` in the file
  bool r = false, w = false, x = false;
  char path[256] = {};  // empty for an anonymous mapping, "[stack]" and the like otherwise
};

// read(2)-like: bytes read, 0 at the end, negative on an error.
using ReadFn = long (*)(void* ctx, void* buf, size_t n);

// One pass over a maps file: fills out[i] for every addrs[i]. `chunk` is the working buffer (8 KiB
// or more; a line longer than it is skipped).
inline void FindRegions(ReadFn read_fn, void* ctx, const uint64_t* addrs, Region* out, size_t n,
                        char* chunk, size_t chunk_cap) {
  for (size_t i = 0; i < n; ++i) out[i] = Region{};
  if (chunk_cap < 256) return;
  size_t have = 0;       // bytes in chunk
  bool skipping = false;  // inside a line that did not fit
  for (;;) {
    long got = read_fn(ctx, chunk + have, chunk_cap - have);
    if (got < 0) return;
    const bool eof = got == 0;
    have += static_cast<size_t>(got);
    size_t pos = 0;
    for (;;) {
      size_t nl = pos;
      while (nl < have && chunk[nl] != '\n') ++nl;
      if (nl >= have) break;  // an incomplete line: needs more
      if (!skipping) {
        MapLine ml;
        if (ParseMapsLine(chunk + pos, nl - pos, &ml)) {
          for (size_t i = 0; i < n; ++i) {
            if (!out[i].found && addrs[i] >= ml.start && addrs[i] < ml.end) {
              Region& r = out[i];
              r.found = true;
              r.start = ml.start;
              r.end = ml.end;
              r.file_offset = ml.file_offset;
              r.r = ml.r;
              r.w = ml.w;
              r.x = ml.x;
              CopyZ(r.path, sizeof(r.path), ml.path, ml.path_len);
            }
          }
        }
      }
      skipping = false;
      pos = nl + 1;
    }
    // Keep the incomplete tail for the next read.
    size_t tail = have - pos;
    if (tail >= chunk_cap) {  // a line longer than the buffer: drop it up to its end
      skipping = true;
      tail = 0;
    } else {
      for (size_t i = 0; i < tail; ++i) chunk[i] = chunk[pos + i];
    }
    have = tail;
    if (eof) return;
  }
}

// ---------------------------------------------------------------------------------------------
// ELF symbols (64-bit little-endian, the only kind a Linux x86-64 or arm64 game process holds)
// ---------------------------------------------------------------------------------------------

// pread(2)-like: true when all n bytes at file offset `off` were read.
struct ElfIo {
  bool (*read)(void* ctx, uint64_t off, void* buf, size_t n) = nullptr;
  void* ctx = nullptr;
};

struct SymHit {
  bool found = false;
  uint64_t value = 0;  // the symbol's address (ELF virtual address)
  uint64_t size = 0;
  char name[192] = {};  // as stored: C++ names stay mangled (c++filt reads them)
};

namespace elf {
inline uint16_t Rd16(const unsigned char* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
inline uint32_t Rd32(const unsigned char* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t Rd64(const unsigned char* p) {
  return static_cast<uint64_t>(Rd32(p)) | (static_cast<uint64_t>(Rd32(p + 4)) << 32);
}
constexpr uint32_t kPtLoad = 1;
constexpr uint32_t kShtSymtab = 2;
constexpr uint32_t kShtDynsym = 11;
constexpr size_t kMaxLoads = 16;
constexpr size_t kMaxSections = 1024;
constexpr size_t kSymSize = 24;
}  // namespace elf

struct ElfInfo {
  struct Load {
    uint64_t offset, vaddr, filesz;
  };
  Load loads[elf::kMaxLoads];
  size_t load_count = 0;
  struct Table {
    uint64_t sym_off = 0, sym_size = 0;  // the symbol table in the file
    uint64_t str_off = 0, str_size = 0;  // its string table
  };
  Table symtab;  // .symtab (the full table; stripped files have none)
  Table dynsym;  // .dynsym
  bool has_symtab = false;
  bool has_dynsym = false;

  // ELF virtual address of a byte of the file; false when no loaded segment holds it.
  bool VaddrOfFileOffset(uint64_t file_off, uint64_t* vaddr) const {
    for (size_t i = 0; i < load_count; ++i) {
      const Load& l = loads[i];
      if (file_off >= l.offset && file_off < l.offset + l.filesz) {
        *vaddr = l.vaddr + (file_off - l.offset);
        return true;
      }
    }
    return false;
  }
};

// Reads the headers. `scratch` is a working buffer of at least 64 KiB.
inline bool ElfOpen(const ElfIo& io, ElfInfo* info, unsigned char* scratch, size_t scratch_cap) {
  if (scratch_cap < 65536) return false;
  unsigned char eh[64];
  if (!io.read(io.ctx, 0, eh, sizeof(eh))) return false;
  if (eh[0] != 0x7F || eh[1] != 'E' || eh[2] != 'L' || eh[3] != 'F') return false;
  if (eh[4] != 2 || eh[5] != 1) return false;  // ELFCLASS64, little-endian
  const uint64_t phoff = elf::Rd64(eh + 32);
  const uint64_t shoff = elf::Rd64(eh + 40);
  const size_t phentsize = elf::Rd16(eh + 54);
  size_t phnum = elf::Rd16(eh + 56);
  const size_t shentsize = elf::Rd16(eh + 58);
  size_t shnum = elf::Rd16(eh + 60);

  info->load_count = 0;
  info->has_symtab = info->has_dynsym = false;

  if (phoff != 0 && phentsize >= 56 && phnum != 0) {
    if (phnum > 64) phnum = 64;
    if (phnum * phentsize > scratch_cap) return false;
    if (!io.read(io.ctx, phoff, scratch, phnum * phentsize)) return false;
    for (size_t i = 0; i < phnum && info->load_count < elf::kMaxLoads; ++i) {
      const unsigned char* ph = scratch + i * phentsize;
      if (elf::Rd32(ph) != elf::kPtLoad) continue;
      ElfInfo::Load& l = info->loads[info->load_count++];
      l.offset = elf::Rd64(ph + 8);
      l.vaddr = elf::Rd64(ph + 16);
      l.filesz = elf::Rd64(ph + 32);
    }
  }

  if (shoff != 0 && shentsize >= 64 && shnum != 0) {
    if (shnum > elf::kMaxSections) shnum = elf::kMaxSections;
    if (shnum * shentsize > scratch_cap) return true;  // too many sections: no symbols, still usable
    if (!io.read(io.ctx, shoff, scratch, shnum * shentsize)) return true;
    for (size_t i = 0; i < shnum; ++i) {
      const unsigned char* sh = scratch + i * shentsize;
      const uint32_t type = elf::Rd32(sh + 4);
      if (type != elf::kShtSymtab && type != elf::kShtDynsym) continue;
      const uint32_t link = elf::Rd32(sh + 40);
      if (link >= shnum) continue;
      const unsigned char* str = scratch + static_cast<size_t>(link) * shentsize;
      ElfInfo::Table t;
      t.sym_off = elf::Rd64(sh + 24);
      t.sym_size = elf::Rd64(sh + 32);
      t.str_off = elf::Rd64(str + 24);
      t.str_size = elf::Rd64(str + 32);
      if (type == elf::kShtSymtab) {
        info->symtab = t;
        info->has_symtab = true;
      } else {
        info->dynsym = t;
        info->has_dynsym = true;
      }
    }
  }
  return true;
}

namespace detail {
struct Candidate {
  bool found = false;
  uint64_t value = 0, size = 0, name_off = 0;
  int table = 0;  // 0 symtab, 1 dynsym
};

// One pass over a symbol table: for every address still wanted, the function symbol that holds it
// (the one that starts last, so the innermost of nested or aliased ones).
inline void ScanSymbols(const ElfIo& io, const ElfInfo::Table& table, int which,
                        const uint64_t* vaddrs, Candidate* cand, const bool* want, size_t n,
                        unsigned char* scratch, size_t scratch_cap) {
  uint64_t lo = ~uint64_t(0), hi = 0;
  bool any = false;
  for (size_t i = 0; i < n; ++i) {
    if (!want[i]) continue;
    any = true;
    if (vaddrs[i] < lo) lo = vaddrs[i];
    if (vaddrs[i] > hi) hi = vaddrs[i];
  }
  if (!any) return;
  const size_t per_chunk = scratch_cap / elf::kSymSize;
  const uint64_t total = table.sym_size / elf::kSymSize;
  // A table of more than 8M symbols is not a table of a game: stop rather than read for seconds.
  const uint64_t limit = total < (uint64_t(8) << 20) ? total : (uint64_t(8) << 20);
  for (uint64_t first = 0; first < limit; first += per_chunk) {
    uint64_t count = limit - first;
    if (count > per_chunk) count = per_chunk;
    if (!io.read(io.ctx, table.sym_off + first * elf::kSymSize, scratch, count * elf::kSymSize)) {
      return;
    }
    for (uint64_t k = 0; k < count; ++k) {
      const unsigned char* s = scratch + k * elf::kSymSize;
      const uint64_t value = elf::Rd64(s + 8);
      if (value == 0 || value > hi) continue;
      const unsigned info = s[4];
      const unsigned type = info & 0xF;
      const unsigned shndx = elf::Rd16(s + 6);
      if (shndx == 0 || shndx >= 0xFF00) continue;
      const uint64_t size = elf::Rd64(s + 16);
      // 2 = function, 10 = indirect function, 0 = untyped (assembler labels: only when sized).
      if (type != 2 && type != 10 && !(type == 0 && size != 0)) continue;
      if (size != 0 && value + size <= lo) continue;
      for (size_t i = 0; i < n; ++i) {
        if (!want[i] || vaddrs[i] < value) continue;
        const bool inside = size != 0 ? vaddrs[i] < value + size : vaddrs[i] - value < 256;
        if (!inside) continue;
        Candidate& c = cand[i];
        if (!c.found || value > c.value) {
          c.found = true;
          c.value = value;
          c.size = size;
          c.name_off = elf::Rd32(s);
          c.table = which;
        }
      }
    }
  }
}
}  // namespace detail

// Looks up n ELF virtual addresses. `scratch` is a working buffer of at least 64 KiB, the same one
// ElfOpen used. Returns how many were found.
inline size_t ElfLookup(const ElfIo& io, const ElfInfo& info, const uint64_t* vaddrs, SymHit* hits,
                        size_t n, unsigned char* scratch, size_t scratch_cap) {
  constexpr size_t kMax = 64;
  if (n > kMax || scratch_cap < 65536) return 0;
  detail::Candidate cand[kMax];
  bool want[kMax];
  for (size_t i = 0; i < n; ++i) {
    hits[i] = SymHit{};
    want[i] = true;
  }
  if (info.has_symtab) {
    detail::ScanSymbols(io, info.symtab, 0, vaddrs, cand, want, n, scratch, scratch_cap);
  }
  if (info.has_dynsym) {
    for (size_t i = 0; i < n; ++i) want[i] = !cand[i].found;
    detail::ScanSymbols(io, info.dynsym, 1, vaddrs, cand, want, n, scratch, scratch_cap);
  }
  size_t found = 0;
  for (size_t i = 0; i < n; ++i) {
    if (!cand[i].found) continue;
    const ElfInfo::Table& t = cand[i].table == 0 ? info.symtab : info.dynsym;
    SymHit& h = hits[i];
    h.value = cand[i].value;
    h.size = cand[i].size;
    if (cand[i].name_off >= t.str_size) continue;
    size_t want_bytes = sizeof(h.name) - 1;
    if (t.str_size - cand[i].name_off < want_bytes) {
      want_bytes = static_cast<size_t>(t.str_size - cand[i].name_off);
    }
    if (!io.read(io.ctx, t.str_off + cand[i].name_off, h.name, want_bytes)) continue;
    h.name[want_bytes] = 0;  // a name longer than the buffer is cut
    h.found = true;
    ++found;
  }
  return found;
}

}  // namespace larecomp::crash
