// Check of the pure half of the Linux crash reporter (crash_report_util.h): the text builder, the
// log timestamp, the /proc/self/maps parser and the ELF symbol lookup. A program of its own: it needs
// no Linux, no SDK and no game, and exits 0 when every check holds. It builds ELF files in memory,
// so it runs on Windows too.
//
//   clang++ -std=c++20 -O1 -Wall -Wextra -I src src/crash_report_test.cpp -o crash_report_test
//   ./crash_report_test [a real ELF64 file and a function name in it]
//
// With the two optional arguments (a shared object or executable made by a real linker, and the name
// of one of its functions) it also looks the function up in that file as the crash reporter would.

#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "crash_report_util.h"

using namespace larecomp::crash;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

void CheckEq(const std::string& got, const std::string& want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failed;
    std::printf("FAILED: %s: got '%s', want '%s'\n", what.c_str(), got.c_str(), want.c_str());
  }
}

std::string Str(const Text& t) { return std::string(t.data(), t.size()); }

// ---- Text -----------------------------------------------------------------------------------------

void TestText() {
  char buf[64];
  Text t(buf, sizeof(buf));
  t.Hex(0xBEEF, 8);
  CheckEq(Str(t), "0000BEEF", "Hex pads to 8");
  t.Clear();
  t.Hex(0, 1);
  CheckEq(Str(t), "0", "Hex of zero");
  t.Clear();
  t.Hex(0xFFFFFFFFFFFFFFFFull, 16);
  CheckEq(Str(t), "FFFFFFFFFFFFFFFF", "Hex of all ones");
  t.Clear();
  t.Hex(0x123456789ABCDEFull, 4);
  CheckEq(Str(t), "123456789ABCDEF", "Hex wider than the padding");
  t.Clear();
  t.Dec(-42);
  CheckEq(Str(t), "-42", "Dec negative");
  t.Clear();
  t.Dec(7, 3, '0');
  CheckEq(Str(t), "007", "Dec zero padded");
  t.Clear();
  t.Dec(-7, 4, '0');
  CheckEq(Str(t), "-007", "Dec negative zero padded");
  t.Clear();
  t.Dec(-7, 4, ' ');
  CheckEq(Str(t), "  -7", "Dec negative space padded");
  t.Clear();
  t.Dec(INT64_MIN);
  CheckEq(Str(t), "-9223372036854775808", "Dec of INT64_MIN");
  t.Clear();
  t.Dec(0);
  CheckEq(Str(t), "0", "Dec of zero");

  char small[5];
  Text s(small, sizeof(small));
  s.Str("abcdefgh");
  CheckEq(Str(s), "abcde", "Text stops at its capacity");
  Check(s.truncated(), "Text says it truncated");
  s.Clear();
  Check(!s.truncated() && s.size() == 0, "Clear resets");

  Check(CStrLen("abc") == 3 && CStrLen("") == 0, "CStrLen");
  Check(CStrEq("abc", "abc") && !CStrEq("abc", "abd") && !CStrEq("abc", "ab"), "CStrEq");
  char dst[4];
  CopyZ(dst, sizeof(dst), "abcdef", 6);
  CheckEq(dst, "abc", "CopyZ cuts and terminates");
  CheckEq(BaseName("/usr/lib/libc.so.6"), "libc.so.6", "BaseName");
  CheckEq(BaseName("[stack]"), "[stack]", "BaseName without a slash");
}

// ---- timestamp ------------------------------------------------------------------------------------

std::string Stamp(int64_t unix_s, int ms, int offset) {
  char buf[64];
  Text t(buf, sizeof(buf));
  AppendTimestamp(t, unix_s, ms, offset);
  return Str(t);
}

void TestTimestamp() {
  CheckEq(Stamp(0, 0, 0), "[1970-01-01 00:00:00.000]", "epoch");
  CheckEq(Stamp(1763042709, 123, 0), "[2025-11-13 14:05:09.123]", "a day in 2025");
  CheckEq(Stamp(1763042709, 5, -18000), "[2025-11-13 09:05:09.005]", "local offset west");
  CheckEq(Stamp(1709164800, 0, 0), "[2024-02-29 00:00:00.000]", "leap day");
  CheckEq(Stamp(1709164799, 0, 0), "[2024-02-28 23:59:59.000]", "the second before it");
  CheckEq(Stamp(4102444799LL, 999, 0), "[2099-12-31 23:59:59.999]", "end of 2099");
  CheckEq(Stamp(1000, 7, -3600), "[1969-12-31 23:16:40.007]", "before the epoch through the offset");
  CheckEq(Stamp(951782400, 0, 14 * 3600), "[2000-02-29 14:00:00.000]", "2000 is a leap year");
}

// ---- maps -----------------------------------------------------------------------------------------

std::string PathOf(const MapLine& m) { return std::string(m.path, m.path_len); }

void TestMapsLine() {
  MapLine m;
  const char* a = "55d0c8a01000-55d0c8e00000 r-xp 00001000 08:01 1000                      /usr/bin/larecomp";
  Check(ParseMapsLine(a, std::strlen(a), &m), "parse a file mapping");
  Check(m.start == 0x55d0c8a01000ull && m.end == 0x55d0c8e00000ull, "start and end");
  Check(m.r && !m.w && m.x, "perms r-x");
  Check(m.file_offset == 0x1000, "file offset");
  CheckEq(PathOf(m), "/usr/bin/larecomp", "path");

  const char* b = "7f0000000000-7f0000100000 rw-p 00000000 00:00 0 ";
  Check(ParseMapsLine(b, std::strlen(b), &m), "parse an anonymous mapping");
  Check(m.w && m.r && !m.x, "perms rw-");
  Check(m.path_len == 0, "no path");

  const char* c = "7ffd12300000-7ffd12321000 rw-p 00000000 00:00 0                          [stack]";
  Check(ParseMapsLine(c, std::strlen(c), &m), "parse [stack]");
  CheckEq(PathOf(m), "[stack]", "[stack] path");

  const char* d = "7f1c2d000000-7f1c2d021000 r-xp 00001000 08:01 55   /tmp/a b/libfoo.so (deleted)";
  Check(ParseMapsLine(d, std::strlen(d), &m), "parse a path with a space");
  CheckEq(PathOf(m), "/tmp/a b/libfoo.so (deleted)", "path with a space and a suffix");

  Check(!ParseMapsLine("garbage", 7, &m), "reject garbage");
  Check(!ParseMapsLine("", 0, &m), "reject an empty line");
  Check(!ParseMapsLine("1000-2000 r-x", 13, &m), "reject a short line");
}

struct MemSource {
  const char* p;
  size_t left;
  size_t step;  // the most one read hands out
};
long MemRead(void* ctx, void* buf, size_t n) {
  auto* s = static_cast<MemSource*>(ctx);
  size_t take = n < s->step ? n : s->step;
  if (take > s->left) take = s->left;
  std::memcpy(buf, s->p, take);
  s->p += take;
  s->left -= take;
  return static_cast<long>(take);
}

const char kMaps[] =
    "55d0c8a00000-55d0c8a01000 r--p 00000000 08:01 1000    /usr/bin/larecomp\n"
    "55d0c8a01000-55d0c8e00000 r-xp 00001000 08:01 1000    /usr/bin/larecomp\n"
    "55d0c8e00000-55d0c9000000 rw-p 00400000 08:01 1000    /usr/bin/larecomp\n"
    "7f0000000000-7f0000100000 rw-p 00000000 00:00 0 \n"
    "7ffd12300000-7ffd12321000 rw-p 00000000 00:00 0                    [stack]\n"
    "7f1c2d000000-7f1c2d021000 r-xp 00001000 08:01 55    /tmp/a b/libfoo.so (deleted)\n";

void CheckRegions(size_t step, size_t chunk_cap, const char* label) {
  const uint64_t addrs[] = {0x55d0c8a05000ull, 0x55d0c8e00010ull, 0x7f0000000010ull,
                            0x7ffd12310000ull, 0x7f1c2d000100ull, 0x10ull, 0x55d0c9000000ull};
  Region out[7];
  MemSource src{kMaps, sizeof(kMaps) - 1, step};
  std::vector<char> chunk(chunk_cap);
  FindRegions(&MemRead, &src, addrs, out, 7, chunk.data(), chunk.size());
  const std::string l = std::string(label) + ": ";
  Check(out[0].found && out[0].x && out[0].file_offset == 0x1000 && out[0].start == 0x55d0c8a01000ull,
        l + "address in the text mapping");
  CheckEq(out[0].path, "/usr/bin/larecomp", l + "its module");
  Check(out[1].found && out[1].w && !out[1].x && out[1].file_offset == 0x400000, l + "data mapping");
  Check(out[2].found && out[2].path[0] == 0, l + "anonymous mapping");
  Check(out[3].found && std::strcmp(out[3].path, "[stack]") == 0, l + "stack");
  Check(out[4].found && std::strcmp(out[4].path, "/tmp/a b/libfoo.so (deleted)") == 0,
        l + "deleted library");
  Check(!out[5].found, l + "an address in no mapping");
  Check(!out[6].found, l + "one past the last byte of a mapping is outside it");
}

void TestRegions() {
  CheckRegions(100000, 8192, "one read");
  CheckRegions(37, 8192, "37 bytes at a time");
  CheckRegions(1, 8192, "one byte at a time");
  // A buffer that holds a line but not two: the incomplete tail has to be carried over.
  CheckRegions(50, 300, "small chunk");

  // A line longer than the buffer is skipped, the lines after it still read.
  std::string maps = "1000-2000 r-xp 00000000 08:01 1    /";
  maps.append(600, 'x');
  maps += "\n3000-4000 rw-p 00000000 00:00 0    [heap]\n";
  const uint64_t addrs[] = {0x1100, 0x3100};
  Region out[2];
  MemSource src{maps.data(), maps.size(), 64};
  char chunk[300];
  FindRegions(&MemRead, &src, addrs, out, 2, chunk, sizeof(chunk));
  Check(!out[0].found, "the over-long line is skipped");
  Check(out[1].found && std::strcmp(out[1].path, "[heap]") == 0, "the line after it is read");
}

// ---- ELF ------------------------------------------------------------------------------------------

struct ByteBuf {
  std::vector<unsigned char> v;
  void At(size_t off, size_t size) {
    if (v.size() < off + size) v.resize(off + size, 0);
  }
  void P16(size_t off, uint16_t x) {
    At(off, 2);
    v[off] = x & 0xFF;
    v[off + 1] = x >> 8;
  }
  void P32(size_t off, uint32_t x) {
    At(off, 4);
    for (int i = 0; i < 4; ++i) v[off + i] = (x >> (8 * i)) & 0xFF;
  }
  void P64(size_t off, uint64_t x) {
    At(off, 8);
    for (int i = 0; i < 8; ++i) v[off + i] = (x >> (8 * i)) & 0xFF;
  }
  void Bytes(size_t off, const void* p, size_t n) {
    At(off, n);
    std::memcpy(&v[off], p, n);
  }
};

struct SymSpec {
  const char* name;
  unsigned type;  // STT_*
  unsigned shndx;
  uint64_t value;
  uint64_t size;
};

// A small ELF64 with two loaded segments, a .symtab and a .dynsym (each with its string table).
//   segment 1: file 0x1000..0x3000 at vaddr 0x401000   (code)
//   segment 2: file 0x3000..0x3800 at vaddr 0x604000   (data)
ByteBuf BuildElf(const std::vector<SymSpec>& symtab, const std::vector<SymSpec>& dynsym,
                 bool with_symtab = true) {
  ByteBuf b;
  b.At(0, 0x5000);
  const unsigned char ident[16] = {0x7F, 'E', 'L', 'F', 2, 1, 1, 0};
  b.Bytes(0, ident, 16);
  b.P16(16, 3);        // ET_DYN
  b.P16(18, 62);       // x86-64
  b.P32(20, 1);
  b.P64(32, 64);       // e_phoff
  b.P64(40, 0x4800);   // e_shoff
  b.P16(52, 64);
  b.P16(54, 56);       // e_phentsize
  b.P16(56, 2);        // e_phnum
  b.P16(58, 64);       // e_shentsize
  b.P16(60, 6);        // e_shnum
  b.P16(62, 0);
  // program headers
  b.P32(64 + 0, 1);
  b.P64(64 + 8, 0x1000);
  b.P64(64 + 16, 0x401000);
  b.P64(64 + 32, 0x2000);
  b.P32(64 + 56, 1);
  b.P64(64 + 56 + 8, 0x3000);
  b.P64(64 + 56 + 16, 0x604000);
  b.P64(64 + 56 + 32, 0x800);

  auto write_table = [&](const std::vector<SymSpec>& syms, size_t sym_off, size_t str_off,
                         size_t* sym_size, size_t* str_size) {
    size_t soff = 1;  // string 0 is the empty string
    size_t i = 1;     // symbol 0 is the null symbol
    for (const SymSpec& s : syms) {
      const size_t e = sym_off + i * 24;
      const size_t len = std::strlen(s.name) + 1;
      b.Bytes(str_off + soff, s.name, len);
      b.P32(e, static_cast<uint32_t>(soff));
      b.At(e, 24);
      b.v[e + 4] = static_cast<unsigned char>(s.type | (1 << 4));  // global
      b.P16(e + 6, static_cast<uint16_t>(s.shndx));
      b.P64(e + 8, s.value);
      b.P64(e + 16, s.size);
      soff += len;
      ++i;
    }
    *sym_size = i * 24;
    *str_size = soff;
  };
  size_t sym_size = 0, str_size = 0, dyn_size = 0, dynstr_size = 0;
  write_table(symtab, 0x4000, 0x4400, &sym_size, &str_size);
  write_table(dynsym, 0x4600, 0x4700, &dyn_size, &dynstr_size);

  // section headers: 0 null, 1 .text, 2 .symtab, 3 .strtab, 4 .dynsym, 5 .dynstr
  const size_t sh = 0x4800;
  b.At(sh, 6 * 64);
  auto section = [&](int idx, uint32_t type, uint64_t off, uint64_t size, uint32_t link) {
    const size_t e = sh + static_cast<size_t>(idx) * 64;
    b.P32(e + 4, type);
    b.P64(e + 24, off);
    b.P64(e + 32, size);
    b.P32(e + 40, link);
  };
  section(1, 1, 0x1000, 0x2000, 0);
  section(2, with_symtab ? 2 : 0, 0x4000, sym_size, 3);
  section(3, 3, 0x4400, str_size, 0);
  section(4, 11, 0x4600, dyn_size, 5);
  section(5, 3, 0x4700, dynstr_size, 0);
  return b;
}

struct MemFile {
  const std::vector<unsigned char>* bytes;
  int reads = 0;
};
bool MemPread(void* ctx, uint64_t off, void* buf, size_t n) {
  auto* f = static_cast<MemFile*>(ctx);
  ++f->reads;
  if (off + n > f->bytes->size()) return false;
  std::memcpy(buf, f->bytes->data() + off, n);
  return true;
}

void TestElf() {
  const std::string longname(300, 'q');
  const std::vector<SymSpec> symtab = {
      {"fn_alpha", 2, 1, 0x401100, 0x80},
      {"fn_beta", 2, 1, 0x401200, 0x40},
      {"alias_beta", 2, 1, 0x401200, 0x40},
      {"asm_label", 0, 1, 0x401300, 0},       // untyped and unsized: not a function
      {"asm_sized", 0, 1, 0x401320, 0x10},    // untyped but sized: counts
      {"undefined_fn", 2, 0, 0x401400, 0x10},  // shndx 0: imported, not defined here
      {"a_variable", 1, 1, 0x401440, 0x10},   // an object
      {"outer", 2, 1, 0x402000, 0x100},
      {"inner", 2, 1, 0x402040, 0x20},
      {"unsized_fn", 2, 1, 0x402200, 0},      // function with no size: only the next 256 bytes
      {longname.c_str(), 2, 1, 0x402400, 0x10},
  };
  const std::vector<SymSpec> dynsym = {
      {"fn_dyn", 2, 1, 0x401500, 0x10},
      {"fn_alpha_dyn", 2, 1, 0x401100, 0x80},  // the .symtab name wins for this one
  };
  ByteBuf elf = BuildElf(symtab, dynsym);
  MemFile mf{&elf.v};
  ElfIo io;
  io.read = &MemPread;
  io.ctx = &mf;
  static unsigned char scratch[65536];
  ElfInfo info;
  Check(ElfOpen(io, &info, scratch, sizeof(scratch)), "ElfOpen reads the synthetic file");
  Check(info.load_count == 2, "two loadable segments");
  Check(info.has_symtab && info.has_dynsym, "both symbol tables found");
  uint64_t va = 0;
  Check(info.VaddrOfFileOffset(0x1100, &va) && va == 0x401100, "file offset in segment 1");
  Check(info.VaddrOfFileOffset(0x3010, &va) && va == 0x604010, "file offset in segment 2");
  Check(!info.VaddrOfFileOffset(0x0, &va), "file offset before the first segment");
  Check(!info.VaddrOfFileOffset(0x3800, &va), "file offset past the second segment");

  const uint64_t q[] = {0x401100, 0x40117F, 0x401180, 0x401210, 0x401300, 0x401328, 0x401404,
                        0x401445, 0x402050, 0x402010, 0x4022FF, 0x402400, 0x401505, 0x401000};
  SymHit hits[14];
  const size_t found = ElfLookup(io, info, q, hits, 14, scratch, sizeof(scratch));
  CheckEq(hits[0].found ? hits[0].name : "-", "fn_alpha", "start of a function");
  Check(hits[0].value == 0x401100 && hits[0].size == 0x80, "its value and size");
  CheckEq(hits[1].found ? hits[1].name : "-", "fn_alpha", "last byte of a function");
  CheckEq(hits[2].found ? hits[2].name : "-", "-", "one past the end is outside");
  CheckEq(hits[3].found ? hits[3].name : "-", "fn_beta", "an alias: the first one stays");
  CheckEq(hits[4].found ? hits[4].name : "-", "-", "an untyped unsized label is not a function");
  CheckEq(hits[5].found ? hits[5].name : "-", "asm_sized", "an untyped sized label counts");
  CheckEq(hits[6].found ? hits[6].name : "-", "-", "an undefined symbol is skipped");
  CheckEq(hits[7].found ? hits[7].name : "-", "-", "an object is not a function");
  CheckEq(hits[8].found ? hits[8].name : "-", "inner", "the innermost of nested symbols");
  CheckEq(hits[9].found ? hits[9].name : "-", "outer", "outside the inner one");
  CheckEq(hits[10].found ? hits[10].name : "-", "unsized_fn", "an unsized function covers 256 bytes");
  Check(hits[11].found && std::strlen(hits[11].name) == sizeof(hits[11].name) - 1 &&
            hits[11].name[0] == 'q',
        "a long name is cut to the buffer");
  CheckEq(hits[12].found ? hits[12].name : "-", "fn_dyn", "the dynamic table answers what .symtab lacks");
  CheckEq(hits[13].found ? hits[13].name : "-", "-", "before the first symbol");
  Check(found == 9, "the count of found symbols (got " + std::to_string(found) + ")");

  // Stripped: only the dynamic table is left.
  ByteBuf stripped = BuildElf(symtab, dynsym, /*with_symtab=*/false);
  MemFile mf2{&stripped.v};
  io.ctx = &mf2;
  ElfInfo info2;
  Check(ElfOpen(io, &info2, scratch, sizeof(scratch)), "ElfOpen reads a stripped file");
  Check(!info2.has_symtab && info2.has_dynsym, "no .symtab left");
  const uint64_t q2[] = {0x401100, 0x401210};
  SymHit h2[2];
  ElfLookup(io, info2, q2, h2, 2, scratch, sizeof(scratch));
  CheckEq(h2[0].found ? h2[0].name : "-", "fn_alpha_dyn", "a stripped file answers from .dynsym");
  CheckEq(h2[1].found ? h2[1].name : "-", "-", "and not what only .symtab had");

  // Not an ELF file, and the wrong class.
  std::vector<unsigned char> junk(4096, 0x41);
  MemFile mf3{&junk};
  io.ctx = &mf3;
  ElfInfo info3;
  Check(!ElfOpen(io, &info3, scratch, sizeof(scratch)), "junk is refused");
  ByteBuf elf32 = BuildElf(symtab, dynsym);
  elf32.v[4] = 1;  // ELFCLASS32
  MemFile mf4{&elf32.v};
  io.ctx = &mf4;
  Check(!ElfOpen(io, &info3, scratch, sizeof(scratch)), "a 32-bit file is refused");
  Check(mf4.reads >= 1, "ElfOpen read the header");

  // A read that fails halfway through a table gives no answer and no crash.
  std::vector<unsigned char> truncated(elf.v.begin(), elf.v.begin() + 0x4100);
  MemFile mf5{&truncated};
  io.ctx = &mf5;
  ElfInfo info5;
  ElfOpen(io, &info5, scratch, sizeof(scratch));  // the section headers are cut: still opens
  SymHit h5[1];
  const uint64_t q5[] = {0x401100};
  ElfLookup(io, info5, q5, h5, 1, scratch, sizeof(scratch));
  Check(!h5[0].found, "a cut file finds nothing");
}

// A real ELF file from a real linker.
void TestRealElf(const char* path, const char* function) {
  std::FILE* f = std::fopen(path, "rb");
  if (!f) {
    Check(false, std::string("cannot open ") + path);
    return;
  }
  std::vector<unsigned char> bytes;
  unsigned char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
  std::fclose(f);
  MemFile mf{&bytes};
  ElfIo io;
  io.read = &MemPread;
  io.ctx = &mf;
  static unsigned char scratch[65536];
  ElfInfo info;
  Check(ElfOpen(io, &info, scratch, sizeof(scratch)), "the real file opens");
  Check(info.load_count >= 1, "the real file has loadable segments");
  Check(info.has_symtab || info.has_dynsym, "the real file has a symbol table");
  // Find the function by name the slow way, then ask for an address inside it.
  const ElfInfo::Table& t = info.has_symtab ? info.symtab : info.dynsym;
  uint64_t want_value = 0, want_size = 0;
  for (uint64_t i = 1; i < t.sym_size / 24; ++i) {
    unsigned char s[24];
    if (!MemPread(&mf, t.sym_off + i * 24, s, 24)) break;
    uint32_t name_off = elf::Rd32(s);
    const char* name = reinterpret_cast<const char*>(&bytes[t.str_off + name_off]);
    if (std::strcmp(name, function) == 0) {
      want_value = elf::Rd64(s + 8);
      want_size = elf::Rd64(s + 16);
      break;
    }
  }
  Check(want_value != 0, std::string("the function ") + function + " is in the table");
  if (want_value == 0) return;
  const uint64_t inside = want_value + (want_size > 1 ? want_size / 2 : 0);
  SymHit hit[1];
  ElfLookup(io, info, &inside, hit, 1, scratch, sizeof(scratch));
  Check(hit[0].found && std::strcmp(hit[0].name, function) == 0 && hit[0].value == want_value,
        std::string("lookup of an address inside ") + function);
  // The file offset of the function's start maps back to its address.
  uint64_t off = 0;
  bool mapped = false;
  for (size_t i = 0; i < info.load_count; ++i) {
    const ElfInfo::Load& l = info.loads[i];
    if (want_value >= l.vaddr && want_value < l.vaddr + l.filesz) {
      off = l.offset + (want_value - l.vaddr);
      mapped = true;
      break;
    }
  }
  uint64_t back = 0;
  Check(mapped && info.VaddrOfFileOffset(off, &back) && back == want_value,
        "file offset and virtual address agree on the real file");
}

}  // namespace

int main(int argc, char** argv) {
  TestText();
  TestTimestamp();
  TestMapsLine();
  TestRegions();
  TestElf();
  if (argc >= 3) TestRealElf(argv[1], argv[2]);
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
