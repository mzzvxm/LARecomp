#pragma once
//
// Host versions of small helper functions of the game (hooks/host_helpers.cpp).
//
// The recompiled helpers copy, fill and pack guest memory one VMX register or one word at a time
// through volatile loads and stores (every guest access is a real access, so the host compiler can
// neither merge nor vectorise the loops), and each vector access costs a byte-swap shuffle on both
// sides. On big-endian data that is only a copy: lvx128 and stvx128 swap the same way, so a run of
// them moves the bytes unchanged. The functions here do the same work on the host with memcpy and
// memset, on pointers into the guest's memory (`base`), and write exactly what the recompiled code
// writes. They know nothing of the rest of the program (the standard library only), so the unit
// test (host_helpers_test.cpp) compiles them on their own.
//
// Contract of every function: it returns true when it did the work, and false when it did NOTHING
// and the recompiled function must run instead. It declines whatever it cannot reproduce exactly:
//  - guest ranges outside plain guest RAM. The recompiled code reaches the physical mirror at
//    0xE0000000 through an offset (REX_PHYS_HOST_OFFSET) and may reach the register blocks at
//    0x7FC80000 / 0x7FEA0000 through the MMIO handlers; both, and everything near them, go to the
//    recompiled code (PlainRange);
//  - a source range that overlaps a destination range: the originals copy forward in blocks and
//    zero cache lines ahead of the copy (dcbzl), so what an overlapping call leaves behind depends
//    on the block order, and memcpy's overlap is undefined;
//  - arguments the original reads in a different way (counts of zero or negative, a row size that
//    is not a multiple of 4, pointers whose arithmetic wraps).
// Guest accesses below are big-endian: Ld32 / Ld64 / St32 / St64 swap, byte copies do not.
//
// What each one is (read from the recompiled code in generated/; the arguments' meaning for the
// D3D ones is INFERENCE from the code, the semantics are what the generated function does):
//   Memcpy      rex_sub_8244D5C0 memcpy(dst, src, n) -> dst. Head and tail with stvlx / stvrx, 128-byte
//               blocks with dcbzl on the destination lines; n < 16 copies words and then bytes in steps
//               of the word at 0x827F3890 (1 in the image; a different value declines).
//   CrtMemcpy   rex_sub_823DA950 memcpy(dst, src, n) -> dst, the C runtime's: forward copy in 8-byte steps with 4- and 1-byte
//               ends, no overlap handling (so an overlapping call is declined, not moved).
//   Memset      rex_sub_823D9890 memset(dst, (u8)value, n).
//   RowWrite    rex_sub_8218A6E0 (object, table, index + 1, source, bytes per row, rows): copies
//               `rows` rows of `bytes` bytes from a linear source into the row pointers of an object
//               (265 call sites), the row pitch from a byte table at 0x827D5EC0.
//   ShaderConstF  rex_sub_82417678 (device, first register, source, count, mask): copies `count`
//               float4 from any alignment to the device's shader-constant shadow, then ORs the
//               64-bit dirty mask into the device's first doubleword (5 call sites).
//   FlushBank   D3DDevice_FlushConstantBank (device, 64-bit dirty mask, first register, bank): writes
//               SET_CONSTANT packets (NOP padding, header, 64 bytes per set bit) into the command ring
//               at device+48 and stores the new pointer. When any run would not fit before the limit
//               at device+52 the original calls the ring-wrap routine: this declines the whole call
//               before it writes a byte.
//
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace host_helpers {

inline constexpr uint32_t kPhysicalWindow = 0xE0000000u;  // addresses from here are the mirrored physical window
inline constexpr uint32_t kMmioLow = 0x7F000000u;         // the GPU and APU register blocks sit at 0x7FC80000 / 0x7FEA0000
inline constexpr uint32_t kMmioHigh = 0x80000000u;
inline constexpr uint32_t kMemcpyStepAddr = 0x827F3890u;  // the word the byte loop of memcpy subtracts (1)
inline constexpr uint32_t kRowPitchTable = 0x827D5EC0u;   // byte table: pitch / 16 of a surface format
inline constexpr uint32_t kNopWord = 0x80000000u;         // the packet padding word

// [addr, addr + len) is ordinary guest RAM: below the physical window, clear of the register blocks.
inline bool PlainRange(uint32_t addr, uint64_t len) {
    const uint64_t end = static_cast<uint64_t>(addr) + len;
    if (end > kPhysicalWindow) return false;
    return end <= kMmioLow || addr >= kMmioHigh;
}

inline bool Disjoint(uint32_t a, uint64_t an, uint32_t b, uint64_t bn) {
    return static_cast<uint64_t>(a) + an <= b || static_cast<uint64_t>(b) + bn <= a;
}

inline uint32_t Ld8(const uint8_t* base, uint32_t a) { return base[a]; }
inline uint32_t Ld32(const uint8_t* base, uint32_t a) {
    uint32_t v;
    std::memcpy(&v, base + a, 4);
    return std::byteswap(v);
}
inline uint64_t Ld64(const uint8_t* base, uint32_t a) {
    uint64_t v;
    std::memcpy(&v, base + a, 8);
    return std::byteswap(v);
}
inline void St32(uint8_t* base, uint32_t a, uint32_t v) {
    v = std::byteswap(v);
    std::memcpy(base + a, &v, 4);
}
inline void St64(uint8_t* base, uint32_t a, uint64_t v) {
    v = std::byteswap(v);
    std::memcpy(base + a, &v, 8);
}

// ---- memcpy -------------------------------------------------------------------------------------
// CrtMemcpy (rex_sub_823DA950, the C runtime's forward-only memcpy with 8- and 4-byte steps): a plain copy.
inline bool CrtMemcpy(uint8_t* base, uint32_t dst, uint32_t src, uint32_t n) {
    if (n == 0) return true;
    if (!PlainRange(dst, n) || !PlainRange(src, n) || !Disjoint(dst, n, src, n)) return false;
    std::memcpy(base + dst, base + src, n);
    return true;
}

// Memcpy (rex_sub_8244D5C0, the console's fast memcpy): the same, and for n < 16 only while the word at 0x827F3890 is 1.
inline bool Memcpy(uint8_t* base, uint32_t dst, uint32_t src, uint32_t n) {
    if (n == 0) return true;
    if (n < 16) {
        // The tail loop of the original subtracts this word from the count per byte: only the image's 1 is a plain copy.
        if (!PlainRange(kMemcpyStepAddr, 4) || Ld32(base, kMemcpyStepAddr) != 1) return false;
    }
    return CrtMemcpy(base, dst, src, n);
}

// ---- memset -------------------------------------------------------------------------------------
inline bool Memset(uint8_t* base, uint32_t dst, uint32_t value, uint32_t n) {
    if (n == 0) return true;
    if (!PlainRange(dst, n)) return false;
    std::memset(base + dst, static_cast<int>(value & 0xFF), n);
    return true;
}

// ---- row writer (rex_sub_8218A6E0) --------------------------------------------------------------
// r3 object (its word at +8 points at the table of per-slot format bytes, 32 bytes apart), r4 pointer to
// a pointer to the table of row pointers (4 bytes a slot), r5 slot + 1, r6 source, r7 bytes per row (a
// multiple of 4), r8 rows (signed). Each row goes to the slot's row pointer plus k * (r7 + gap) where gap
// = ((pitch - r7) >> 2) << 2 with pitch = table[format] * 16 (arithmetic shift: a pitch below r7 goes back).
inline bool RowWrite(uint8_t* base, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8) {
    if (r5 == 0 || static_cast<int32_t>(r8) <= 0) return true;  // the original returns at once
    const uint32_t slot = r5 - 1;
    if (slot >= (1u << 24)) return false;
    if (r7 == 0 || (r7 & 3) != 0 || r7 > (1u << 24)) return false;  // the original's word loop would not end, or runs absurdly
    if (!PlainRange(r3 + 8u, 4) || r3 > 0x7FFFFFF0u || !PlainRange(r4, 4)) return false;
    const uint32_t formats = Ld32(base, r3 + 8u);
    const uint32_t fmt_addr = formats + (slot << 5);
    const uint32_t rows_ptr = Ld32(base, r4);
    const uint32_t row_slot_addr = rows_ptr + (slot << 2);
    if (!PlainRange(fmt_addr, 1) || !PlainRange(row_slot_addr, 4)) return false;
    const uint32_t fmt = Ld8(base, fmt_addr);
    const uint32_t row0 = Ld32(base, row_slot_addr);
    if (!PlainRange(kRowPitchTable + fmt, 1)) return false;
    const uint32_t pitch = Ld8(base, kRowPitchTable + fmt) << 4;
    const int32_t diff = static_cast<int32_t>(pitch - r7);
    const uint32_t gap = static_cast<uint32_t>(diff >> 2) << 2;
    const uint32_t stride = r7 + gap;
    const uint32_t rows = r8;
    const uint64_t src_bytes = static_cast<uint64_t>(r7) * rows;
    if (!PlainRange(r6, src_bytes)) return false;
    // Check every row before the first write.
    uint32_t dst = row0;
    for (uint32_t k = 0; k < rows; ++k, dst += stride) {
        if (!PlainRange(dst, r7) || !Disjoint(dst, r7, r6, src_bytes)) return false;
    }
    dst = row0;
    const uint8_t* s = base + r6;
    for (uint32_t k = 0; k < rows; ++k, dst += stride, s += r7) std::memcpy(base + dst, s, r7);
    return true;
}

// ---- shader constant copy (rex_sub_82417678) ----------------------------------------------------
// The destination is the 16-byte aligned (device + rotl(reg + 120, 4) & ~15): stvx drops the low bits.
inline bool ShaderConstF(uint8_t* base, uint32_t dev, uint32_t reg, uint32_t src, uint32_t count, uint64_t mask) {
    if (count > (1u << 20)) return false;
    const uint32_t offset = std::rotl(reg + 120u, 4) & 0xFFFFFFF0u;
    const uint32_t dst = (dev + offset) & ~0xFu;
    const uint64_t bytes = static_cast<uint64_t>(count) * 16;
    if (!PlainRange(dev, 8)) return false;
    if (count != 0 && (!PlainRange(dst, bytes) || !PlainRange(src, bytes) || !Disjoint(dst, bytes, src, bytes))) return false;
    if (count != 0) std::memcpy(base + dst, base + src, bytes);
    // After the copy: the original reads the doubleword at the device after storing the vectors.
    St64(base, dev, Ld64(base, dev) | mask);
    return true;
}

// ---- constant bank flush (D3DDevice_FlushConstantBank) --------------------------------------------
inline bool FlushBank(uint8_t* base, uint32_t dev, uint64_t mask, uint32_t first_reg, uint32_t bank) {
    if (mask == 0 || dev > kPhysicalWindow - 64 || !PlainRange(dev + 48u, 8)) return false;
    const uint32_t ptr0 = Ld32(base, dev + 48u);
    const uint32_t limit = Ld32(base, dev + 52u);
    if ((ptr0 & 3) != 0) return false;
    struct Run {
        uint32_t src;    // guest address of the first 64-byte block
        uint32_t first;  // first register of the run
        uint32_t blocks;
        uint32_t pads;
    };
    Run runs[64];
    uint32_t n = 0;
    const uint32_t bank_base = bank & ~0xFu;
    uint32_t p = ptr0;
    uint32_t reg = first_reg;
    uint32_t bit = 0;
    uint64_t m = mask;
    while (m != 0) {
        const uint32_t lz = static_cast<uint32_t>(std::countl_zero(m));  // 0..63
        m <<= lz;
        const uint32_t run = static_cast<uint32_t>(std::countl_zero(~m));  // 1..64
        bit += lz;
        reg += 16u * lz;
        const uint32_t count = run * 16u;
        if (!(p + (count + 3u) * 4u < limit)) return false;  // the original calls the ring-wrap routine
        uint32_t pads = 0;
        switch (p & 0xF) {
            case 4: pads = 1; break;
            case 0: pads = 2; break;
            case 12: pads = 3; break;
            default: break;  // 8
        }
        runs[n++] = {bank_base + 64u * bit, reg, run, pads};
        p += 4u * (pads + 1u) + 64u * run;
        m = run >= 64 ? 0 : (m << run);
        reg += count;
        bit += run;
    }
    // One range for all the packets, one per run for the bank.
    if (p < ptr0 || !PlainRange(ptr0, static_cast<uint64_t>(p - ptr0))) return false;
    if (!Disjoint(ptr0, static_cast<uint64_t>(p - ptr0), dev + 48u, 8)) return false;
    for (uint32_t i = 0; i < n; ++i) {
        const uint64_t bytes = 64ull * runs[i].blocks;
        if (!PlainRange(runs[i].src, bytes) || !Disjoint(runs[i].src, bytes, ptr0, static_cast<uint64_t>(p - ptr0))) return false;
    }
    p = ptr0;
    for (uint32_t i = 0; i < n; ++i) {
        for (uint32_t k = 0; k < runs[i].pads; ++k) {
            St32(base, p + 4u, kNopWord);
            p += 4;
        }
        St32(base, p + 4u, ((runs[i].blocks * 16u - 1u) << 16) | runs[i].first);
        p += 4;
        std::memcpy(base + p + 4u, base + runs[i].src, 64u * runs[i].blocks);
        p += 64u * runs[i].blocks;
    }
    St32(base, dev + 48u, p);
    return true;
}

}  // namespace host_helpers
