#pragma once
//
// AES decryption of 16-byte blocks, on the guest's own round keys (hooks/script_aes.cpp).
//
// The game's AES (an unmodified Rijndael, "rijndael-alg-fst" style: one 1 KB Td0 table used
// with rotations, a byte table for the last round) keeps its decryption round keys as the
// "equivalent inverse cipher" schedule: round key 0 is the last encryption round key, rounds
// 1 .. Nr-1 are the encryption round keys in reverse order with InvMixColumns applied, round
// key Nr is the first encryption round key. Each is four 32-bit words, and the guest stores
// the words big-endian, so the 16*(Nr+1) bytes of the schedule, read as bytes in memory order,
// are exactly the round keys of AES-NI's AESDEC / AESDECLAST (which expect that same schedule).
// A block is the 16 bytes in memory order too. Nothing is swapped.
//
// Two implementations with the same result:
//   DecryptAesNi      AES-NI, 8 independent blocks at a time (x86-64 with the AES feature bit).
//   DecryptPortable   the same table algorithm as the game's code, in plain C++.
// `passes` decrypts every block that many times in a row, in place (the game does it 16 times).
//
// Header only and free of the rest of the program, so the tables and both paths can be
// compiled and tested on their own. RunKnownAnswerTests() checks both against the FIPS-197
// appendix C decryption vectors (AES-128, AES-192, AES-256) and against each other.

#include <initializer_list>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__x86_64__) || defined(_M_X64)
#define SCRIPT_AES_X86 1
#include <emmintrin.h>
#include <wmmintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#define SCRIPT_AES_TARGET_AESNI __attribute__((target("aes,sse2")))
#else
#define SCRIPT_AES_X86 0
#define SCRIPT_AES_TARGET_AESNI
#endif

namespace script_aes {

inline constexpr int kMaxRounds = 14;
inline constexpr size_t kMaxScheduleBytes = 16 * (kMaxRounds + 1);

namespace detail {

constexpr uint8_t XTime(uint8_t a) {
    return static_cast<uint8_t>((a << 1) ^ ((a & 0x80) ? 0x1B : 0x00));
}

constexpr uint8_t GMul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b != 0) {
        if (b & 1) r = static_cast<uint8_t>(r ^ a);
        a = XTime(a);
        b = static_cast<uint8_t>(b >> 1);
    }
    return r;
}

constexpr uint8_t Rotl8(uint8_t v, int n) {
    return static_cast<uint8_t>((v << n) | (v >> (8 - n)));
}

struct Tables {
    uint8_t sbox[256];
    uint8_t isbox[256];
    // Td0[x] = { 0e, 09, 0d, 0b } * InvSbox[x], most significant byte first (Td1..Td3 are its
    // rotations right by 8, 16 and 24 bits).
    uint32_t td0[256];
};

constexpr Tables MakeTables() {
    Tables t{};
    // The S-box from its definition (multiplicative inverse in GF(2^8), then the affine map);
    // p walks the powers of 3 and q the powers of 3's inverse.
    uint8_t p = 1;
    uint8_t q = 1;
    do {
        p = static_cast<uint8_t>(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0x00));
        q = static_cast<uint8_t>(q ^ (q << 1));
        q = static_cast<uint8_t>(q ^ (q << 2));
        q = static_cast<uint8_t>(q ^ (q << 4));
        if (q & 0x80) q = static_cast<uint8_t>(q ^ 0x09);
        const uint8_t x = static_cast<uint8_t>(q ^ Rotl8(q, 1) ^ Rotl8(q, 2) ^ Rotl8(q, 3) ^ Rotl8(q, 4));
        t.sbox[p] = static_cast<uint8_t>(x ^ 0x63);
    } while (p != 1);
    t.sbox[0] = 0x63;
    for (int i = 0; i < 256; ++i) t.isbox[t.sbox[i]] = static_cast<uint8_t>(i);
    for (int i = 0; i < 256; ++i) {
        const uint8_t s = t.isbox[i];
        t.td0[i] = (uint32_t(GMul(s, 0x0E)) << 24) | (uint32_t(GMul(s, 0x09)) << 16) |
                   (uint32_t(GMul(s, 0x0D)) << 8) | uint32_t(GMul(s, 0x0B));
    }
    return t;
}

inline constexpr Tables kTables = MakeTables();
// The two known entries of each table the game's image holds (Td0[0] is 51f4a750).
static_assert(kTables.sbox[0x00] == 0x63 && kTables.sbox[0x01] == 0x7C && kTables.sbox[0x53] == 0xED &&
              kTables.sbox[0xFF] == 0x16);
static_assert(kTables.isbox[0x00] == 0x52 && kTables.isbox[0x63] == 0x00 && kTables.isbox[0x16] == 0xFF);
static_assert(kTables.td0[0] == 0x51F4A750u && kTables.td0[1] == 0x7E416553u && kTables.td0[255] == 0xD0B85742u);

inline uint32_t LoadBe32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

inline void StoreBe32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

inline uint32_t Ror(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }

}  // namespace detail

// Builds the decryption schedule (as the game's context holds it) from an AES key of 16, 24
// or 32 bytes: 16*(Nr+1) bytes into `out` (at most kMaxScheduleBytes), Nr into `*rounds`.
// Used by the known-answer tests; the game builds its own.
inline bool BuildDecryptSchedule(const uint8_t* key, size_t key_bytes, uint8_t* out, int* rounds) {
    if (key_bytes != 16 && key_bytes != 24 && key_bytes != 32) return false;
    const int nk = static_cast<int>(key_bytes / 4);
    const int nr = nk + 6;
    uint32_t w[4 * (kMaxRounds + 1)];
    for (int i = 0; i < nk; ++i) w[i] = detail::LoadBe32(key + 4 * i);
    auto sub_word = [](uint32_t v) {
        return (uint32_t(detail::kTables.sbox[v >> 24]) << 24) |
               (uint32_t(detail::kTables.sbox[(v >> 16) & 0xFF]) << 16) |
               (uint32_t(detail::kTables.sbox[(v >> 8) & 0xFF]) << 8) | uint32_t(detail::kTables.sbox[v & 0xFF]);
    };
    uint8_t rcon = 1;
    for (int i = nk; i < 4 * (nr + 1); ++i) {
        uint32_t t = w[i - 1];
        if (i % nk == 0) {
            t = sub_word((t << 8) | (t >> 24)) ^ (uint32_t(rcon) << 24);
            rcon = detail::XTime(rcon);
        } else if (nk > 6 && i % nk == 4) {
            t = sub_word(t);
        }
        w[i] = w[i - nk] ^ t;
    }
    for (int i = 0; i <= nr; ++i) {
        uint8_t* rk = out + 16 * i;
        for (int c = 0; c < 4; ++c) detail::StoreBe32(rk + 4 * c, w[4 * (nr - i) + c]);
        if (i == 0 || i == nr) continue;
        for (int c = 0; c < 4; ++c) {  // InvMixColumns of the column
            const uint8_t a0 = rk[4 * c + 0], a1 = rk[4 * c + 1], a2 = rk[4 * c + 2], a3 = rk[4 * c + 3];
            using detail::GMul;
            rk[4 * c + 0] = static_cast<uint8_t>(GMul(a0, 14) ^ GMul(a1, 11) ^ GMul(a2, 13) ^ GMul(a3, 9));
            rk[4 * c + 1] = static_cast<uint8_t>(GMul(a0, 9) ^ GMul(a1, 14) ^ GMul(a2, 11) ^ GMul(a3, 13));
            rk[4 * c + 2] = static_cast<uint8_t>(GMul(a0, 13) ^ GMul(a1, 9) ^ GMul(a2, 14) ^ GMul(a3, 11));
            rk[4 * c + 3] = static_cast<uint8_t>(GMul(a0, 11) ^ GMul(a1, 13) ^ GMul(a2, 9) ^ GMul(a3, 14));
        }
    }
    *rounds = nr;
    return true;
}

// Decrypts `blocks` 16-byte blocks of `data` in place, `passes` times each, with the schedule
// of `nr` rounds (10, 12 or 14). The table algorithm of the game's own code.
inline void DecryptPortable(const uint8_t* schedule, int nr, uint8_t* data, size_t blocks, int passes) {
    using detail::LoadBe32;
    using detail::Ror;
    using detail::StoreBe32;
    uint32_t rk[4 * (kMaxRounds + 1)];
    for (int i = 0; i < 4 * (nr + 1); ++i) rk[i] = LoadBe32(schedule + 4 * i);
    const uint32_t* td0 = detail::kTables.td0;
    const uint8_t* si = detail::kTables.isbox;
    for (size_t b = 0; b < blocks; ++b) {
        uint8_t* blk = data + 16 * b;
        uint32_t s0 = LoadBe32(blk);
        uint32_t s1 = LoadBe32(blk + 4);
        uint32_t s2 = LoadBe32(blk + 8);
        uint32_t s3 = LoadBe32(blk + 12);
        for (int pass = 0; pass < passes; ++pass) {
            uint32_t x0 = s0 ^ rk[0];
            uint32_t x1 = s1 ^ rk[1];
            uint32_t x2 = s2 ^ rk[2];
            uint32_t x3 = s3 ^ rk[3];
            for (int r = 1; r < nr; ++r) {
                const uint32_t* k = rk + 4 * r;
                const uint32_t t0 = td0[x0 >> 24] ^ Ror(td0[(x3 >> 16) & 0xFF], 8) ^
                                    Ror(td0[(x2 >> 8) & 0xFF], 16) ^ Ror(td0[x1 & 0xFF], 24) ^ k[0];
                const uint32_t t1 = td0[x1 >> 24] ^ Ror(td0[(x0 >> 16) & 0xFF], 8) ^
                                    Ror(td0[(x3 >> 8) & 0xFF], 16) ^ Ror(td0[x2 & 0xFF], 24) ^ k[1];
                const uint32_t t2 = td0[x2 >> 24] ^ Ror(td0[(x1 >> 16) & 0xFF], 8) ^
                                    Ror(td0[(x0 >> 8) & 0xFF], 16) ^ Ror(td0[x3 & 0xFF], 24) ^ k[2];
                const uint32_t t3 = td0[x3 >> 24] ^ Ror(td0[(x2 >> 16) & 0xFF], 8) ^
                                    Ror(td0[(x1 >> 8) & 0xFF], 16) ^ Ror(td0[x0 & 0xFF], 24) ^ k[3];
                x0 = t0;
                x1 = t1;
                x2 = t2;
                x3 = t3;
            }
            const uint32_t* k = rk + 4 * nr;
            s0 = (uint32_t(si[x0 >> 24]) << 24) ^ (uint32_t(si[(x3 >> 16) & 0xFF]) << 16) ^
                 (uint32_t(si[(x2 >> 8) & 0xFF]) << 8) ^ uint32_t(si[x1 & 0xFF]) ^ k[0];
            s1 = (uint32_t(si[x1 >> 24]) << 24) ^ (uint32_t(si[(x0 >> 16) & 0xFF]) << 16) ^
                 (uint32_t(si[(x3 >> 8) & 0xFF]) << 8) ^ uint32_t(si[x2 & 0xFF]) ^ k[1];
            s2 = (uint32_t(si[x2 >> 24]) << 24) ^ (uint32_t(si[(x1 >> 16) & 0xFF]) << 16) ^
                 (uint32_t(si[(x0 >> 8) & 0xFF]) << 8) ^ uint32_t(si[x3 & 0xFF]) ^ k[2];
            s3 = (uint32_t(si[x3 >> 24]) << 24) ^ (uint32_t(si[(x2 >> 16) & 0xFF]) << 16) ^
                 (uint32_t(si[(x1 >> 8) & 0xFF]) << 8) ^ uint32_t(si[x0 & 0xFF]) ^ k[3];
        }
        StoreBe32(blk, s0);
        StoreBe32(blk + 4, s1);
        StoreBe32(blk + 8, s2);
        StoreBe32(blk + 12, s3);
    }
}

// True if this CPU has the AES instructions (CPUID leaf 1, ECX bit 25).
inline bool CpuHasAesNi() {
#if SCRIPT_AES_X86
#if defined(_MSC_VER)
    int r[4] = {};
    __cpuid(r, 1);
    return ((static_cast<unsigned>(r[2]) >> 25) & 1u) != 0;
#else
    unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return false;
    return ((ecx >> 25) & 1u) != 0;
#endif
#else
    return false;
#endif
}

#if SCRIPT_AES_X86
// Same as DecryptPortable, with AES-NI. Only call it when CpuHasAesNi(). Eight blocks go
// through the rounds together (each block's rounds depend on each other, so one block alone
// would wait out the instruction latency every round); what is left over goes one at a time.
SCRIPT_AES_TARGET_AESNI inline void DecryptAesNi(const uint8_t* schedule, int nr, uint8_t* data, size_t blocks,
                                                 int passes) {
    __m128i rk[kMaxRounds + 1];
    for (int i = 0; i <= nr; ++i) rk[i] = _mm_loadu_si128(reinterpret_cast<const __m128i*>(schedule + 16 * i));
    constexpr int kLanes = 8;
    size_t b = 0;
    for (; b + kLanes <= blocks; b += kLanes) {
        __m128i s[kLanes];
        for (int k = 0; k < kLanes; ++k)
            s[k] = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 16 * (b + k)));
        for (int pass = 0; pass < passes; ++pass) {
            for (int k = 0; k < kLanes; ++k) s[k] = _mm_xor_si128(s[k], rk[0]);
            for (int r = 1; r < nr; ++r) {
                for (int k = 0; k < kLanes; ++k) s[k] = _mm_aesdec_si128(s[k], rk[r]);
            }
            for (int k = 0; k < kLanes; ++k) s[k] = _mm_aesdeclast_si128(s[k], rk[nr]);
        }
        for (int k = 0; k < kLanes; ++k)
            _mm_storeu_si128(reinterpret_cast<__m128i*>(data + 16 * (b + k)), s[k]);
    }
    for (; b < blocks; ++b) {
        __m128i s = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 16 * b));
        for (int pass = 0; pass < passes; ++pass) {
            s = _mm_xor_si128(s, rk[0]);
            for (int r = 1; r < nr; ++r) s = _mm_aesdec_si128(s, rk[r]);
            s = _mm_aesdeclast_si128(s, rk[nr]);
        }
        _mm_storeu_si128(reinterpret_cast<__m128i*>(data + 16 * b), s);
    }
}
#else
inline void DecryptAesNi(const uint8_t* schedule, int nr, uint8_t* data, size_t blocks, int passes) {
    DecryptPortable(schedule, nr, data, blocks, passes);
}
#endif

struct KnownAnswerResult {
    bool portable_ok = false;
    bool aesni_available = false;
    bool aesni_ok = false;
};

namespace detail {

inline bool HexEquals(const uint8_t* got, const char* hex) {
    for (int i = 0; i < 16; ++i) {
        auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
        if (got[i] != ((nib(hex[2 * i]) << 4) | nib(hex[2 * i + 1]))) return false;
    }
    return true;
}

// FIPS-197 appendix C: key 000102..., plaintext 00112233445566778899aabbccddeeff.
// `use_aesni` picks the path. `passes` > 1 is checked against the portable path instead.
inline bool CheckFips197(bool use_aesni) {
    static constexpr const char* kCipher[3] = {"69c4e0d86a7b0430d8cdb78070b4c55a",
                                               "dda97ca4864cdfe06eaf70a0ec0d7191",
                                               "8ea2b7ca516745bfeafc49904b496089"};
    static constexpr const char* kPlain = "00112233445566778899aabbccddeeff";
    static constexpr size_t kKeyBytes[3] = {16, 24, 32};
    for (int v = 0; v < 3; ++v) {
        uint8_t key[32];
        for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(i);
        uint8_t schedule[kMaxScheduleBytes];
        int nr = 0;
        if (!BuildDecryptSchedule(key, kKeyBytes[v], schedule, &nr)) return false;
        if (nr != static_cast<int>(kKeyBytes[v] / 4) + 6) return false;
        uint8_t blk[16];
        for (int i = 0; i < 16; ++i) {
            auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
            blk[i] = static_cast<uint8_t>((nib(kCipher[v][2 * i]) << 4) | nib(kCipher[v][2 * i + 1]));
        }
        if (use_aesni) {
            DecryptAesNi(schedule, nr, blk, 1, 1);
        } else {
            DecryptPortable(schedule, nr, blk, 1, 1);
        }
        if (!HexEquals(blk, kPlain)) return false;
    }
    return true;
}

// Both paths on 13 blocks (one batch of 8 and a tail of 5) of a fixed pattern, 16 passes.
inline bool PathsAgree() {
    for (size_t key_bytes : {size_t(16), size_t(24), size_t(32)}) {
        uint8_t key[32];
        for (size_t i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(0xA5 ^ (i * 7));
        uint8_t schedule[kMaxScheduleBytes];
        int nr = 0;
        if (!BuildDecryptSchedule(key, key_bytes, schedule, &nr)) return false;
        uint8_t a[13 * 16], b[13 * 16];
        for (size_t i = 0; i < sizeof(a); ++i) a[i] = b[i] = static_cast<uint8_t>(i * 29 + 3);
        DecryptPortable(schedule, nr, a, 13, 16);
        DecryptAesNi(schedule, nr, b, 13, 16);
        if (std::memcmp(a, b, sizeof(a)) != 0) return false;
    }
    return true;
}

}  // namespace detail

inline KnownAnswerResult RunKnownAnswerTests() {
    KnownAnswerResult r;
    r.portable_ok = detail::CheckFips197(false);
    r.aesni_available = CpuHasAesNi();
    if (r.aesni_available) r.aesni_ok = detail::CheckFips197(true) && detail::PathsAgree();
    return r;
}

}  // namespace script_aes
