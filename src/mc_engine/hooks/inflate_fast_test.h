#pragma once
//
// Known-answer tests for the native inflate_fast (inflate_fast.h). Header only, standard library
// only: RunKnownAnswerTests() takes well under a millisecond and is run once, the first time
// the game calls inflate_fast. If any test fails the native decoder is not used.
//
// What they prove, on this machine, without the game's data and without the recompiled function:
//  1. A real deflate stream (a fixed-Huffman block of 3026 bytes of text, made by zlib with a
//     512-byte window) decodes to the exact bytes, in stages that end the way zlib's inflate
//     ends a call (the window ring is updated as zlib's updatewindow does it, so later stages
//     reach back into the window with wnext == 0, wrapped and contiguous), with the code tables
//     built the way zlib's inflate_table builds them (a port of inftrees.c below, used only
//     here), once with the full 9-bit root table and once with a 6-bit root (second-level
//     tables). The bit position after the end-of-block code must be inside the stream's last byte.
//  2. Random programs (literals and matches, 3..258 long, any distance the window and the output
//     allow) written with a bit writer, decoded with random window states (wsize 8 to 512,
//     whave and wnext anything the ring can hold, and 5 to 21 bits already in the bit buffer) and
//     compared with a plain "append from the history" oracle that knows nothing about the ring arithmetic.
//  3. The three error endings (invalid literal/length code, invalid distance code, distance
//     too far back) and an end of block, with the output length and ending checked.
// The recompiled function is compared with the native one on the game's real calls at run time
// (inflate_fast.cpp); these tests cover what those might not reach.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "inflate_fast.h"

namespace inflate_fast::test {

namespace tables {
inline constexpr uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                          31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
inline constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                          2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
inline constexpr uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                           33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                           1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
inline constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3,  3,  4,  4,  5,  5,  6,
                                           6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
}  // namespace tables

// ---------------------------------------------------------------------------------------------
// inflate_table() of zlib 1.2.3 (inftrees.c, Copyright (C) 1995-2005 Mark Adler, zlib license) for
// the LENS and DISTS code types, written out again: the two-level decoding tables as zlib builds them. Entries are written as the guest stores them: op, bits,
// then val big-endian.

struct TableEntry {
    uint8_t op;
    uint8_t bits;
    uint16_t val;
};

inline bool BuildTable(bool dists, const uint16_t* lens, unsigned codes, unsigned* bits_io,
                       std::vector<uint8_t>* out) {
    constexpr unsigned kMaxBits = 15;
    const unsigned enough = dists ? 592u : 852u;  // ENOUGH_DISTS, ENOUGH_LENS
    const int end = dists ? -1 : 256;
    unsigned len, sym, min, max, root, curr, drop, used, huff, incr, fill, low, mask, next;
    int left;
    uint16_t count[kMaxBits + 1], offs[kMaxBits + 1], work[320];
    std::vector<TableEntry> table(enough + 64, TableEntry{0, 0, 0});

    for (len = 0; len <= kMaxBits; ++len) count[len] = 0;
    for (sym = 0; sym < codes; ++sym) count[lens[sym]]++;

    root = *bits_io;
    for (max = kMaxBits; max >= 1; --max) {
        if (count[max] != 0) break;
    }
    if (root > max) root = max;
    if (max == 0) return false;  // zlib makes a table that forces an error; not needed here
    for (min = 1; min <= kMaxBits; ++min) {
        if (count[min] != 0) break;
    }
    if (root < min) root = min;

    left = 1;
    for (len = 1; len <= kMaxBits; ++len) {
        left <<= 1;
        left -= count[len];
        if (left < 0) return false;  // over-subscribed
    }
    if (left > 0 && max != 1) return false;  // incomplete set

    offs[1] = 0;
    for (len = 1; len < kMaxBits; ++len) offs[len + 1] = static_cast<uint16_t>(offs[len] + count[len]);
    for (sym = 0; sym < codes; ++sym) {
        if (lens[sym] != 0) work[offs[lens[sym]]++] = static_cast<uint16_t>(sym);
    }

    huff = 0;
    sym = 0;
    len = min;
    next = 0;
    curr = root;
    drop = 0;
    low = ~0u;
    used = 1u << root;
    mask = used - 1;
    if (used >= enough) return false;

    for (;;) {
        TableEntry here;
        here.bits = static_cast<uint8_t>(len - drop);
        if (static_cast<int>(work[sym]) < end) {
            here.op = 0;
            here.val = work[sym];
        } else if (static_cast<int>(work[sym]) > end) {
            if (dists) {
                here.op = work[sym] < 30 ? static_cast<uint8_t>(16 + tables::kDistExtra[work[sym]]) : 64;
                here.val = work[sym] < 30 ? tables::kDistBase[work[sym]] : 0;
            } else {
                const unsigned i = work[sym] - 257u;
                here.op = i < 29 ? static_cast<uint8_t>(16 + tables::kLenExtra[i]) : (i == 29 ? 201 : 196);
                here.val = i < 29 ? tables::kLenBase[i] : 0;
            }
        } else {
            here.op = 32 + 64;  // end of block
            here.val = 0;
        }

        incr = 1u << (len - drop);
        fill = 1u << curr;
        min = fill;  // the offset to the next table
        do {
            fill -= incr;
            table[next + (huff >> drop) + fill] = here;
        } while (fill != 0);

        incr = 1u << (len - 1);
        while (huff & incr) incr >>= 1;
        if (incr != 0) {
            huff &= incr - 1;
            huff += incr;
        } else {
            huff = 0;
        }

        sym++;
        if (--count[len] == 0) {
            if (len == max) break;
            len = lens[work[sym]];
        }

        if (len > root && (huff & mask) != low) {
            if (drop == 0) drop = root;
            next += min;
            curr = len - drop;
            left = static_cast<int>(1u << curr);
            while (curr + drop < max) {
                left -= count[curr + drop];
                if (left <= 0) break;
                curr++;
                left <<= 1;
            }
            used += 1u << curr;
            if (used >= enough) return false;
            low = huff & mask;
            table[low].op = static_cast<uint8_t>(curr);
            table[low].bits = static_cast<uint8_t>(root);
            table[low].val = static_cast<uint16_t>(next);
        }
    }

    TableEntry here;
    here.op = 64;  // invalid code marker
    here.bits = static_cast<uint8_t>(len - drop);
    here.val = 0;
    while (huff != 0) {
        if (drop != 0 && (huff & mask) != low) {
            drop = 0;
            len = root;
            next = 0;
            here.bits = static_cast<uint8_t>(len);
        }
        table[next + (huff >> drop)] = here;
        incr = 1u << (len - 1);
        while (huff & incr) incr >>= 1;
        if (incr != 0) {
            huff &= incr - 1;
            huff += incr;
        } else {
            huff = 0;
        }
    }

    *bits_io = root;
    out->clear();
    for (unsigned i = 0; i < used; ++i) {
        out->push_back(table[i].op);
        out->push_back(table[i].bits);
        out->push_back(static_cast<uint8_t>(table[i].val >> 8));
        out->push_back(static_cast<uint8_t>(table[i].val & 0xFF));
    }
    return true;
}

struct FixedTables {
    std::vector<uint8_t> lencode, distcode;
    unsigned lenbits = 0, distbits = 0;
};

// The fixed Huffman codes of RFC 1951 3.2.6, as zlib's fixedtables() builds them, with the
// given root size for the literal/length table (9 is zlib's; a smaller one makes second-level tables).
inline bool MakeFixedTables(unsigned len_root, FixedTables* t) {
    uint16_t lens[288];
    unsigned sym = 0;
    while (sym < 144) lens[sym++] = 8;
    while (sym < 256) lens[sym++] = 9;
    while (sym < 280) lens[sym++] = 7;
    while (sym < 288) lens[sym++] = 8;
    t->lenbits = len_root;
    if (!BuildTable(false, lens, 288, &t->lenbits, &t->lencode)) return false;
    uint16_t dl[32];
    for (sym = 0; sym < 32; ++sym) dl[sym] = 5;
    t->distbits = 5;
    return BuildTable(true, dl, 32, &t->distbits, &t->distcode);
}

// ---------------------------------------------------------------------------------------------
// A bit writer for fixed-Huffman deflate streams.

struct BitWriter {
    std::vector<uint8_t> bytes;
    uint32_t acc = 0;
    int nacc = 0;
    void Put(uint32_t v, int n) {  // n bits of v, least significant first
        for (int i = 0; i < n; ++i) {
            acc |= ((v >> i) & 1u) << nacc;
            if (++nacc == 8) {
                bytes.push_back(static_cast<uint8_t>(acc));
                acc = 0;
                nacc = 0;
            }
        }
    }
    void PutCode(uint32_t code, int n) {  // a Huffman code, most significant bit first
        for (int i = n - 1; i >= 0; --i) Put((code >> i) & 1u, 1);
    }
    void PutSymbol(unsigned sym) {  // the fixed literal/length code of `sym`
        if (sym < 144) PutCode(0x30 + sym, 8);
        else if (sym < 256) PutCode(0x190 + (sym - 144), 9);
        else if (sym < 280) PutCode(sym - 256, 7);
        else PutCode(0xC0 + (sym - 280), 8);
    }
    void PutLiteral(uint8_t b) { PutSymbol(b); }
    void PutEnd() { PutSymbol(256); }
    void PutMatch(unsigned len, unsigned dist) {
        unsigned i = 28;
        while (tables::kLenBase[i] > len) --i;
        PutSymbol(257 + i);
        Put(len - tables::kLenBase[i], tables::kLenExtra[i]);
        unsigned j = 29;
        while (tables::kDistBase[j] > dist) --j;
        PutCode(j, 5);
        Put(dist - tables::kDistBase[j], tables::kDistExtra[j]);
    }
    // Header of a final fixed block first; call before anything else.
    void PutFixedHeader() {
        Put(1, 1);
        Put(1, 2);
    }
    void Finish(int pad_bytes) {
        if (nacc) {
            bytes.push_back(static_cast<uint8_t>(acc));
            acc = 0;
            nacc = 0;
        }
        for (int i = 0; i < pad_bytes; ++i) bytes.push_back(0);
    }
};

// ---------------------------------------------------------------------------------------------

struct KatResult {
    bool ok = false;
    int tests = 0;
    uint32_t features = 0;  // rare paths the tests took
    char why[200] = {};
};

namespace detail {

struct Ring {  // a window as zlib keeps it
    std::vector<uint8_t> bytes;
    uint32_t wsize = 0, whave = 0, wnext = 0;
};

inline void UpdateWindow(Ring* w, const uint8_t* end, uint32_t copy) {  // zlib 1.2.3 updatewindow()
    if (copy >= w->wsize) {
        std::memcpy(w->bytes.data(), end - w->wsize, w->wsize);
        w->wnext = 0;
        w->whave = w->wsize;
    } else {
        uint32_t dist = w->wsize - w->wnext;
        if (dist > copy) dist = copy;
        std::memcpy(w->bytes.data() + w->wnext, end - copy, dist);
        copy -= dist;
        if (copy) {
            std::memcpy(w->bytes.data(), end - copy, copy);
            w->wnext = copy;
            w->whave = w->wsize;
        } else {
            w->wnext += dist;
            if (w->wnext == w->wsize) w->wnext = 0;
            if (w->whave < w->wsize) w->whave += dist;
        }
    }
}

// The bytes the window holds, oldest first.
inline std::vector<uint8_t> WindowHistory(const Ring& w) {
    std::vector<uint8_t> h;
    for (uint32_t i = 0; i < w.whave; ++i) h.push_back(w.bytes[(w.wnext + w.wsize - w.whave + i) % w.wsize]);
    return h;
}

inline Args MakeArgs(const std::vector<uint8_t>& stream, size_t first_in, uint32_t hold, uint32_t bits, uint8_t* out,
                     uint32_t avail_out, const Ring& w, const FixedTables& t) {
    Args a;
    a.in = stream.data() + first_in;
    a.avail_in = static_cast<uint32_t>(stream.size() - first_in);
    a.out = out;
    a.avail_out = avail_out;
    a.produced = 0;
    a.window = w.bytes.data();
    a.wsize = w.wsize;
    a.whave = w.whave;
    a.wnext = w.wnext;
    a.lcode = t.lencode.data();
    a.dcode = t.distcode.data();
    a.lenbits = t.lenbits;
    a.distbits = t.distbits;
    a.hold = hold;
    a.bits = bits;
    return a;
}

inline void Fail(KatResult* r, const char* what, int a = 0, int b = 0) {
    std::snprintf(r->why, sizeof(r->why), what, a, b);
}

// Test 1: the zlib-made stream, decoded in stages.
inline const uint8_t* FixedBlob(size_t* n);
inline std::vector<uint8_t> MakePlain();
inline uint64_t Fnv1a(const std::vector<uint8_t>& v) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint8_t b : v) {
        h ^= b;
        h *= 0x100000001b3ull;
    }
    return h;
}

inline bool TestBlob(unsigned len_root, KatResult* r) {
    const std::vector<uint8_t> plain = MakePlain();
    if (plain.size() != 3026 || Fnv1a(plain) != 0xde94118ecc20800dull) {
        Fail(r, "test text generator does not give the expected text (a bug in the test)");
        return false;
    }
    size_t blob_size;
    const uint8_t* blob = FixedBlob(&blob_size);
    std::vector<uint8_t> stream(blob, blob + blob_size);
    const size_t real_size = stream.size();
    stream.insert(stream.end(), 12, 0);  // the end-of-block code is reached inside the fast loop
    if ((stream[0] & 7) != 3) {
        Fail(r, "test stream is not one final fixed block (a bug in the test)");
        return false;
    }
    FixedTables t;
    if (!MakeFixedTables(len_root, &t)) {
        Fail(r, "could not build the fixed code tables with root %d (a bug in the test)", static_cast<int>(len_root));
        return false;
    }
    Ring w;
    w.wsize = 512;
    w.bytes.assign(512, 0);
    std::vector<uint8_t> out(plain.size() + 1024);
    size_t produced = 0;
    const uint8_t* in = stream.data() + 1;
    uint32_t hold = stream[0] >> 3;
    uint32_t bits = 5;
    static const uint32_t kTargets[] = {1000, 100, 120, 700, 0};
    for (int stage = 0;; ++stage) {
        const uint32_t want = stage < 4 ? kTargets[stage] : 0;
        Args a = MakeArgs(stream, static_cast<size_t>(in - stream.data()), hold, bits, out.data() + produced,
                          want ? want + 257 : static_cast<uint32_t>(out.size() - produced), w, t);
        const Result res = Run(a);
        r->features |= res.features;
        if (res.ending == kEndBlock) {
            produced += res.out_written;
            in += res.in_used;
            bits = res.bits;
            const long consumed = 8 * static_cast<long>(in - stream.data()) - static_cast<long>(bits);
            if (produced != plain.size() || std::memcmp(out.data(), plain.data(), plain.size()) != 0) {
                size_t i = 0;
                while (i < produced && i < plain.size() && out[i] == plain[i]) ++i;
                Fail(r, "the zlib stream decodes wrongly (root %d), first difference at byte %d",
                     static_cast<int>(len_root), static_cast<int>(i));
                return false;
            }
            if (consumed <= 8 * static_cast<long>(real_size - 1) || consumed > 8 * static_cast<long>(real_size)) {
                Fail(r, "the end-of-block code ends at bit %d, not in the stream's last byte (root %d)",
                     static_cast<int>(consumed), static_cast<int>(len_root));
                return false;
            }
            return true;
        }
        if (res.ending != kEndLimit || stage >= 4) {
            Fail(r, "the zlib stream stopped early in stage %d (ending %d)", stage, static_cast<int>(res.ending));
            return false;
        }
        UpdateWindow(&w, out.data() + produced + res.out_written, res.out_written);
        produced += res.out_written;
        in += res.in_used;
        hold = res.hold;
        bits = res.bits;
    }
}

// Test 2: random programs against the oracle.
struct Lcg {
    uint32_t s;
    uint32_t Next() {
        s = s * 1664525u + 1013904223u;
        return s >> 8;
    }
    uint32_t Below(uint32_t n) { return Next() % n; }
};

inline bool TestPrograms(unsigned len_root, KatResult* r) {
    FixedTables t;
    if (!MakeFixedTables(len_root, &t)) {
        Fail(r, "could not build the fixed code tables (a bug in the test)");
        return false;
    }
    Lcg rng{0xC0FFEEu + len_root};
    static const uint32_t kSizes[] = {8, 32, 64, 256, 512};
    for (int iter = 0; iter < 200; ++iter) {
        Ring w;
        w.wsize = kSizes[rng.Below(5)];
        w.bytes.resize(w.wsize);
        for (auto& b : w.bytes) b = static_cast<uint8_t>(rng.Next());
        switch (rng.Below(4)) {
            case 0: w.whave = 0; w.wnext = 0; break;                       // empty window
            case 1: w.whave = w.wsize; w.wnext = 0; break;                 // full, wnext == 0
            case 2: w.whave = w.wsize; w.wnext = 1 + rng.Below(w.wsize - 1); break;  // full ring
            default: w.whave = 1 + rng.Below(w.wsize - 1); w.wnext = w.whave; break; // filling up
        }
        std::vector<uint8_t> hist = WindowHistory(w);
        const size_t hist0 = hist.size();
        BitWriter bw;
        bw.PutFixedHeader();
        const uint32_t ops = 1 + rng.Below(14);
        for (uint32_t k = 0; k < ops; ++k) {
            if (rng.Below(4) == 0 || hist.empty()) {
                const uint8_t b = static_cast<uint8_t>(rng.Next());
                bw.PutLiteral(b);
                hist.push_back(b);
            } else {
                const uint32_t maxdist = static_cast<uint32_t>(hist.size() < w.wsize ? hist.size() : w.wsize);
                uint32_t dist = 1 + rng.Below(maxdist);
                if (rng.Below(5) == 0) dist = 1 + rng.Below(4 < maxdist ? 4 : maxdist);  // short distances: overlaps
                uint32_t len = 3 + (rng.Below(3) == 0 ? rng.Below(256) : rng.Below(12));
                bw.PutMatch(len, dist);
                for (uint32_t i = 0; i < len; ++i) hist.push_back(hist[hist.size() - dist]);
            }
        }
        bw.PutEnd();
        bw.Finish(12);
        std::vector<uint8_t> expected(hist.begin() + static_cast<long>(hist0), hist.end());
        std::vector<uint8_t> out(expected.size() + 400, 0xEE);
        // Start with 5 bits in the buffer, or with whole bytes more in it (inflate() can enter inflate_fast with
        // up to 14 bits left over from a code cut by the end of an input chunk): the same stream position.
        const size_t pre = rng.Below(3);  // extra bytes already in the bit buffer
        uint32_t hold0 = bw.bytes[0] >> 3;
        for (size_t k = 0; k < pre; ++k) hold0 |= static_cast<uint32_t>(bw.bytes[1 + k]) << (5 + 8 * k);
        Args a = MakeArgs(bw.bytes, 1 + pre, hold0, static_cast<uint32_t>(5 + 8 * pre), out.data(),
                          static_cast<uint32_t>(expected.size()) + 300, w, t);
        const Result res = Run(a);
        r->features |= res.features;
        if (res.ending != kEndBlock || res.out_written != expected.size() ||
            std::memcmp(out.data(), expected.data(), expected.size()) != 0) {
            Fail(r, "random program %d decodes wrongly (root %d)", iter, static_cast<int>(len_root));
            return false;
        }
        // Nothing past the output was touched.
        for (size_t i = expected.size(); i < out.size(); ++i) {
            if (out[i] != 0xEE) {
                Fail(r, "random program %d wrote past its output (root %d)", iter, static_cast<int>(len_root));
                return false;
            }
        }
    }
    return true;
}

// Test 3: the endings.
inline bool TestEndings(KatResult* r) {
    FixedTables t;
    if (!MakeFixedTables(9, &t)) {
        Fail(r, "could not build the fixed code tables (a bug in the test)");
        return false;
    }
    Ring w;
    w.wsize = 16;
    w.bytes.assign(16, 0);
    std::vector<uint8_t> out(1024);
    for (int kind = 0; kind < 4; ++kind) {
        BitWriter bw;
        bw.PutFixedHeader();
        bw.PutLiteral('A');
        bw.PutLiteral('B');
        Ending want;
        uint32_t want_out;
        switch (kind) {
            case 0:  // symbol 286: invalid literal/length code
                bw.PutSymbol(286);
                want = kEndBadLiteral;
                want_out = 2;
                break;
            case 1:  // length 3, distance code 30: invalid distance code
                bw.PutSymbol(257);
                bw.PutCode(30, 5);
                want = kEndBadDistCode;
                want_out = 2;
                break;
            case 2:  // length 3 at distance 4 with two bytes of output and an empty window: too far back
                bw.PutMatch(3, 4);
                want = kEndTooFar;
                want_out = 2;
                break;
            default:  // a plain end of block
                bw.PutEnd();
                want = kEndBlock;
                want_out = 2;
                break;
        }
        bw.Finish(12);
        Args a = MakeArgs(bw.bytes, 1, bw.bytes[0] >> 3, 5, out.data(), 600, w, t);
        const Result res = Run(a);
        if (res.ending != want || res.out_written != want_out || out[0] != 'A' || out[1] != 'B') {
            Fail(r, "ending test %d gives the wrong ending or length (ending %d)", kind, static_cast<int>(res.ending));
            return false;
        }
        r->features |= res.features;
    }
    // Ending on the limits: a long run of literals ends the call when the output room is below 258.
    {
        BitWriter bw;
        bw.PutFixedHeader();
        for (int i = 0; i < 100; ++i) bw.PutLiteral(static_cast<uint8_t>('a' + i % 26));
        bw.PutEnd();
        bw.Finish(12);
        Args a = MakeArgs(bw.bytes, 1, bw.bytes[0] >> 3, 5, out.data(), 258 + 40, w, t);  // out_limit after 41 bytes
        const Result res = Run(a);
        bool ok = res.ending == kEndLimit && res.out_written == 41;
        for (uint32_t i = 0; ok && i < res.out_written; ++i) ok = out[i] == 'a' + i % 26;
        if (!ok) {
            Fail(r, "output-limit test gives the wrong length (%d)", static_cast<int>(res.out_written));
            return false;
        }
    }
    return true;
}

}  // namespace detail

inline KatResult RunKnownAnswerTests() {
    KatResult r;
    using namespace detail;
    if (!TestBlob(9, &r)) return r;
    ++r.tests;
    if (!TestBlob(6, &r)) return r;
    ++r.tests;
    if (!TestPrograms(9, &r)) return r;
    ++r.tests;
    if (!TestPrograms(6, &r)) return r;
    ++r.tests;
    if (!TestEndings(&r)) return r;
    ++r.tests;
    r.ok = true;
    return r;
}

namespace detail {

// 3026 bytes of text from a small generator (the same one in the python that made the stream).
inline std::vector<uint8_t> MakePlain() {
    static const char* const kWords[16] = {"road ",  "night ",  "club ",  "city ",  "los ",   "angeles ",
                                           "race ",  "turbo ",  "drift ", "neon ",  "garage ", "chrome ",
                                           "ride ",  "lap ",    "boost ", "gold "};
    Lcg rng{0x1234567u};
    std::vector<uint8_t> out;
    while (out.size() < 3000) {
        const uint32_t v = rng.Next();
        const uint32_t k = v % 8;
        if (k == 0) {
            const uint8_t b = static_cast<uint8_t>('a' + ((v >> 4) % 26));
            const uint32_t n = 4 + ((v >> 8) % 60);
            out.insert(out.end(), n, b);
        } else if (k == 1) {
            for (int i = 0; i < 5; ++i) out.push_back(static_cast<uint8_t>(rng.Next() & 0xFF));
        } else {
            const char* word = kWords[(v >> 4) % 16];
            out.insert(out.end(), word, word + std::strlen(word));
        }
    }
    return out;
}

// zlib's output for that text: deflateInit2(level 9, raw, windowBits 9, memLevel 9, Z_FIXED), one final fixed block.
inline const uint8_t* FixedBlob(size_t* n) {
    static const uint8_t kBlob[] = {
    0x4B, 0x29, 0xCA, 0x4C, 0x2B, 0x51, 0x48, 0xCF, 0xCF, 0x49, 0x51, 0x98, 0x7C, 0xB9, 0xD9, 0x25,
    0x30, 0x31, 0x2F, 0x3D, 0x35, 0x27, 0xB5, 0x58, 0x21, 0x39, 0xA7, 0x34, 0x09, 0x22, 0x5C, 0x94,
    0x98, 0x9C, 0xAA, 0x90, 0x4C, 0x12, 0x80, 0x68, 0xC9, 0x2C, 0xA9, 0x54, 0x48, 0xCA, 0xCF, 0x2F,
    0x2E, 0x51, 0x48, 0x01, 0x5B, 0x52, 0x94, 0x9F, 0x98, 0x02, 0x31, 0x12, 0x66, 0x49, 0x5E, 0x66,
    0x7A, 0x46, 0x89, 0x42, 0xAE, 0xED, 0xE6, 0x5D, 0x4C, 0x8F, 0x66, 0x16, 0xC4, 0xD6, 0x82, 0xF5,
    0xA5, 0x27, 0x16, 0x25, 0xA6, 0xA7, 0x42, 0xE5, 0xF2, 0x52, 0xF3, 0xF3, 0x20, 0x26, 0xE5, 0xB5,
    0x98, 0xCE, 0x36, 0x4A, 0xC2, 0x0B, 0xC0, 0x66, 0x97, 0x94, 0x16, 0x25, 0xE5, 0x43, 0x6D, 0x2C,
    0xC4, 0x09, 0xC0, 0x46, 0x82, 0xAD, 0x2B, 0xA5, 0x04, 0xE4, 0xE4, 0x17, 0x2B, 0x24, 0x93, 0x0D,
    0x1E, 0x9D, 0x98, 0x7C, 0x22, 0x1E, 0xE2, 0x52, 0x88, 0xAB, 0x5B, 0xDD, 0x32, 0xDC, 0xB7, 0x27,
    0xE7, 0x94, 0x26, 0x41, 0xF9, 0x39, 0x89, 0x05, 0x0A, 0x79, 0xA9, 0xF9, 0x79, 0x0A, 0xD7, 0xBA,
    0x04, 0x1E, 0x48, 0x80, 0xBD, 0x07, 0x76, 0xF8, 0x5F, 0xA3, 0x2E, 0xA7, 0xF6, 0xC4, 0xBC, 0xF4,
    0xD4, 0x9C, 0xD4, 0x62, 0x88, 0x82, 0xF4, 0xC4, 0xA2, 0xC4, 0xF4, 0x54, 0x05, 0xB0, 0x12, 0x90,
    0x2E, 0x90, 0xC3, 0x1E, 0x34, 0xED, 0x7D, 0xF0, 0x35, 0xB1, 0xCF, 0x51, 0xE9, 0x1A, 0x88, 0x57,
    0x94, 0x9F, 0x98, 0xA2, 0x90, 0x4E, 0x34, 0x00, 0xDB, 0x95, 0x51, 0x94, 0x9F, 0x9B, 0xAA, 0x50,
    0x94, 0x99, 0x92, 0xAA, 0x50, 0x06, 0x03, 0x60, 0x19, 0xB0, 0x9D, 0x30, 0x07, 0x80, 0x4C, 0x07,
    0x3B, 0x0B, 0x6C, 0x05, 0x58, 0x0A, 0x24, 0x04, 0x93, 0x2E, 0x4A, 0x4C, 0x86, 0x39, 0x0C, 0xA6,
    0xF0, 0x36, 0x13, 0xAB, 0x97, 0x10, 0x58, 0x3C, 0x2F, 0x33, 0x3D, 0xA3, 0x44, 0x61, 0xC1, 0x76,
    0xA5, 0xB7, 0x27, 0x60, 0xEA, 0xC1, 0x4A, 0xE1, 0x9A, 0x41, 0x46, 0x66, 0x92, 0x0D, 0xC0, 0x4E,
    0x4F, 0xCA, 0xCF, 0x2F, 0x2E, 0x81, 0x58, 0x9C, 0x9E, 0x58, 0x94, 0x98, 0x0E, 0x75, 0x8D, 0x5B,
    0xBD, 0xDE, 0xCB, 0x7A, 0xB8, 0xBB, 0xD2, 0x2E, 0xC7, 0x35, 0xEF, 0x07, 0xF3, 0x20, 0xCA, 0xC1,
    0x3A, 0x53, 0x8A, 0x32, 0xD3, 0x4A, 0x20, 0x3E, 0x2A, 0x29, 0x2D, 0x4A, 0xCA, 0x87, 0x3B, 0xCB,
    0x8C, 0xFB, 0xDA, 0xC3, 0xCA, 0x1C, 0x12, 0x00, 0xD8, 0x1F, 0x50, 0xDB, 0x61, 0x86, 0x88, 0xD6,
    0xF2, 0x08, 0xD9, 0x17, 0x00, 0x01, 0x58, 0x12, 0x8F, 0xE6, 0x42, 0x32, 0xC1, 0xE7, 0x63, 0xBD,
    0x7F, 0x2E, 0x26, 0x62, 0x00, 0xBE, 0x0B, 0xAF, 0x66, 0xFC, 0x2A, 0x4A, 0x4C, 0x4E, 0x55, 0x90,
    0x73, 0x71, 0x5A, 0xC0, 0x10, 0xB3, 0x39, 0xC0, 0xE5, 0x05, 0xD8, 0x09, 0x60, 0xB1, 0xA8, 0xBA,
    0x0B, 0x3A, 0x29, 0xC9, 0x99, 0x25, 0x95, 0x10, 0x6E, 0x51, 0x66, 0x4A, 0xAA, 0x42, 0x7A, 0x7E,
    0x0E, 0xDC, 0xF5, 0x60, 0xA9, 0xBC, 0xCC, 0xF4, 0x8C, 0x12, 0xB8, 0x47, 0xC0, 0x0A, 0xA1, 0xD2,
    0x60, 0x76, 0x52, 0x7E, 0x7E, 0x31, 0x42, 0x1A, 0xA2, 0x03, 0x0F, 0x80, 0x29, 0x2C, 0xC5, 0x0D,
    0xDC, 0xD6, 0x2F, 0x8D, 0x99, 0x0D, 0xB1, 0x15, 0xEC, 0x54, 0x98, 0x96, 0x92, 0xD2, 0xA2, 0xA4,
    0x7C, 0x85, 0x4A, 0xF2, 0x41, 0x72, 0x46, 0x51, 0x7E, 0x6E, 0xAA, 0x42, 0x09, 0x09, 0x00, 0xEC,
    0xC5, 0xE4, 0x9C, 0xD2, 0x24, 0x85, 0xA2, 0xCC, 0x14, 0xA0, 0x67, 0xC9, 0x02, 0x60, 0xFD, 0x10,
    0xD7, 0xA7, 0x27, 0x16, 0x25, 0xA6, 0xA7, 0x2A, 0xA4, 0x91, 0x0B, 0xA0, 0x5E, 0xF8, 0xD4, 0x9C,
    0xB3, 0x69, 0x6A, 0x5E, 0x66, 0x7A, 0x46, 0x89, 0x02, 0x54, 0x04, 0xEC, 0x3C, 0x14, 0xA7, 0x27,
    0xE6, 0xA5, 0xA7, 0xE6, 0xA4, 0x16, 0x2B, 0x80, 0xBD, 0x00, 0xE3, 0x40, 0xED, 0xAF, 0xC2, 0x09,
    0x52, 0x8A, 0x32, 0xD3, 0x4A, 0x14, 0xF2, 0x52, 0xF3, 0xF3, 0x14, 0x20, 0xE6, 0x97, 0x12, 0x02,
    0x39, 0xF9, 0x40, 0x63, 0xF3, 0x73, 0x52, 0x20, 0x04, 0x58, 0x67, 0x72, 0x66, 0x49, 0x25, 0x84,
    0x80, 0xD9, 0x0B, 0x35, 0x16, 0x6C, 0xE2, 0xBA, 0x2F, 0x6F, 0xFC, 0xFA, 0xFF, 0x5D, 0x78, 0x7C,
    0xF5, 0x5C, 0x49, 0x69, 0x51, 0x52, 0xBE, 0x42, 0x11, 0x41, 0x00, 0xF5, 0x63, 0x21, 0x4E, 0x00,
    0x72, 0xC3, 0xE3, 0x9F, 0x71, 0xCE, 0x5B, 0x11, 0xD6, 0x97, 0xE0, 0x01, 0x50, 0xF3, 0x90, 0xFC,
    0x5A, 0x94, 0x9F, 0x98, 0xA2, 0x90, 0x93, 0x58, 0xA0, 0x10, 0x73, 0x82, 0x7D, 0xF5, 0x61, 0xB0,
    0x01, 0xE9, 0x89, 0x45, 0x89, 0xE9, 0xA9, 0x50, 0x37, 0x83, 0x45, 0x62, 0x0B, 0x5B, 0x18, 0x9E,
    0x15, 0x25, 0x26, 0xC3, 0x74, 0x82, 0xD4, 0xC3, 0x3C, 0x98, 0x4C, 0x01, 0x28, 0x29, 0x2D, 0x4A,
    0xCA, 0x57, 0x80, 0x90, 0x10, 0xEB, 0x10, 0xE6, 0x43, 0x9D, 0x0A, 0x75, 0x8C, 0xFB, 0xBE, 0xB7,
    0xF3, 0x8E, 0xA5, 0xE7, 0xE7, 0xA4, 0x28, 0x24, 0xE5, 0xE7, 0x17, 0x97, 0x28, 0x80, 0x99, 0x10,
    0xC5, 0x60, 0x26, 0x98, 0x28, 0x20, 0x05, 0x40, 0xB4, 0x41, 0x4C, 0x47, 0xB5, 0x0B, 0x64, 0x7B,
    0x21, 0x51, 0xA0, 0x92, 0x08, 0x90, 0x9C, 0x59, 0x52, 0x09, 0xF5, 0x5C, 0x51, 0x7E, 0x22, 0xDC,
    0xCA, 0xBC, 0xD4, 0xFC, 0x3C, 0x85, 0x62, 0x24, 0x00, 0x75, 0xC3, 0xBF, 0xDD, 0xBA, 0x79, 0x5F,
    0x72, 0xF2, 0x8B, 0xA1, 0x9E, 0x03, 0xB9, 0x05, 0x6C, 0x82, 0xEE, 0xBC, 0x9B, 0x73, 0x0E, 0x43,
    0x8C, 0xE1, 0xD1, 0xBB, 0x93, 0xDE, 0x5A, 0x44, 0x02, 0x58, 0xF0, 0x27, 0xE0, 0xD8, 0x7E, 0xA8,
    0xBD, 0x60, 0x37, 0x24, 0xE6, 0xA5, 0xA7, 0xE6, 0xA4, 0x16, 0x2B, 0x94, 0x92, 0x0E, 0x36, 0xFF,
    0xFB, 0x3F, 0x23, 0x3E, 0x87, 0x00, 0xC8, 0xC6, 0x0A, 0x20, 0x3E, 0x2A, 0x29, 0x2D, 0x4A, 0xCA,
    0x57, 0x48, 0xCE, 0x29, 0x4D, 0x52, 0xC8, 0xC7, 0x03, 0x72, 0x31, 0x41, 0x5E, 0x6A, 0x7E, 0x1E,
    0xC4, 0xFD, 0x49, 0xF9, 0xF9, 0xC5, 0x25, 0x0A, 0x45, 0x89, 0xC9, 0xA9, 0x0A, 0xC9, 0x19, 0x45,
    0xF9, 0xB9, 0xA9, 0x0A, 0x39, 0x89, 0x05, 0x30, 0x26, 0xC4, 0x82, 0xE8, 0xA9, 0xFA, 0x8B, 0x36,
    0x41, 0x98, 0xC5, 0x3A, 0x0B, 0x2B, 0x3C, 0x61, 0x5E, 0x06, 0x1B, 0x92, 0x82, 0x0E, 0x72, 0xF2,
    0x81, 0x32, 0x99, 0xE9, 0x19, 0x25, 0x10, 0x87, 0x41, 0xCC, 0x2F, 0x73, 0xDF, 0xDA, 0x7D, 0x14,
    0x24, 0x13, 0x35, 0x7B, 0xEE, 0xF1, 0xC4, 0xA2, 0xCC, 0x94, 0x54, 0xA8, 0x1A, 0x96, 0xDC, 0xBD,
    0xBC, 0xCF, 0x41, 0x36, 0x82, 0xC5, 0x40, 0x2A, 0x20, 0x1A, 0x20, 0x1E, 0x04, 0x0B, 0xDA, 0x17,
    0x17, 0x4F, 0xB4, 0xBE, 0xB6, 0xAB, 0x3D, 0xC8, 0x24, 0x8F, 0x1C, 0x90, 0x4C, 0x32, 0xC8, 0x2C,
    0xA9, 0x54, 0x88, 0x7F, 0xC8, 0xA7, 0xDF, 0x02, 0xF1, 0xF3, 0x94, 0x5D, 0xD9, 0xEE, 0xCD, 0x10,
    0x66, 0x7A, 0x62, 0x51, 0x62, 0x7A, 0x2A, 0xD4, 0x71, 0xF9, 0xA4, 0x80, 0xA2, 0xC4, 0x64, 0x88,
    0xF7, 0xF2, 0x52, 0xF3, 0xF3, 0x14, 0x92, 0xC8, 0x04, 0x10, 0x8B, 0x93, 0x33, 0x8A, 0xF2, 0x73,
    0x61, 0xAE, 0x48, 0xCF, 0xCF, 0x49, 0x51, 0x28, 0xCA, 0x4F, 0x4C, 0x81, 0x98, 0x9C, 0x47, 0x3A,
    0x00, 0x00,
    };
    *n = sizeof(kBlob);
    return kBlob;
}

}  // namespace detail

}  // namespace inflate_fast::test
