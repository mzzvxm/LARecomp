#pragma once
//
// The game's zlib inflate_fast, natively (hooks/inflate_fast.cpp).
//
// The game links zlib 1.2.3 (the version string at 0x82012CA0; "invalid distance too far back"
// and the other messages at 0x82016938 .. are zlib's). Its z_stream is the 32-bit 56-byte one
// and its inflate_state is 1.2.3's: the fields this file reads sit at the offsets the recompiled
// rex_sub_821E4B20 uses. rex_sub_821E4B20(strm, start) is inflate_fast(strm, start) from inffast.c;
// its only caller is the LEN state of inflate (rex_sub_821DEE40), when at least 6 input bytes
// and 258 output bytes are left.
//
// Run() is the same loop on host pointers: the same reads, the same bytes written, the same
// final bit buffer and positions, the same ending (end of block, one of three errors, or out of
// input/output room). It follows the algorithm and structure of zlib 1.2.3's inffast.c
// (Copyright (C) 1995-2003 Mark Adler, zlib license), written out again here, not copied, with
// the game's compiled behaviour as the reference. It is free of the rest of the program (the
// standard library only), so it and the known-answer tests (inflate_fast_test.h) can be compiled
// and tested on their own.
//
// The wrapper half (LoadCall, MakeArgs, ApplyResult, RunNative, RunChecked) reads and writes the
// guest's z_stream and inflate_state through a base pointer, refuses anything it cannot
// reproduce exactly (LoadCall returns false: the recompiled function does that call), and can
// run a call on a copy of the output next to the recompiled function and compare everything.
//
// Everything the recompiled function writes: the output bytes at next_out, strm->next_in,
// avail_in, next_out, avail_out, strm->msg and state->mode on an end of block or an error,
// state->hold and state->bits. Its stack frame (below r1) and the volatile registers are free
// after a call (the ABI); the non-volatile ones are restored. The native version writes the
// same guest fields and nothing else.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <memory>
#include <new>

#if defined(_MSC_VER) && !defined(__clang__)
#include <stdlib.h>
#endif

namespace inflate_fast {

// inflate_state.mode values (zlib 1.2.3 inflate.h) and the message strings in the guest image.
inline constexpr uint32_t kModeType = 11;
inline constexpr uint32_t kModeBad = 27;
inline constexpr uint32_t kMsgBadLiteral = 0x820168B0;   // "invalid literal/length code"
inline constexpr uint32_t kMsgBadDistCode = 0x820168CC;  // "invalid distance code"
inline constexpr uint32_t kMsgTooFar = 0x82016938;       // "invalid distance too far back"

enum Ending : uint32_t {
    kEndLimit = 0,      // ran out of input (fewer than 6 bytes) or output (fewer than 258) room
    kEndBlock,          // end-of-block code: state->mode = TYPE
    kEndBadLiteral,     // invalid literal/length code: msg, state->mode = BAD
    kEndBadDistCode,    // invalid distance code: msg, state->mode = BAD
    kEndTooFar,         // distance too far back: msg, state->mode = BAD
};

// Rare paths the decode took (a union over the whole call), so a test can say what it covered.
enum Feature : uint32_t {
    kFeatLenSubTable = 1u << 0,   // second-level literal/length table
    kFeatDistSubTable = 1u << 1,  // second-level distance table
    kFeatWindowFlat = 1u << 2,    // match reaching into the window, wnext == 0
    kFeatWindowWrap = 1u << 3,    // ... wnext < distance back (wraps around the window)
    kFeatWindowContig = 1u << 4,  // ... wnext >= distance back (contiguous)
    kFeatWindowSplit = 1u << 5,   // a window match that continues past the window into the output
    kFeatWindowTail = 1u << 6,    // a wrapped window match that continues from the window's start
};

namespace detail {

inline uint32_t Bswap32(uint32_t v) {
#if defined(_MSC_VER) && !defined(__clang__)
    return _byteswap_ulong(v);
#else
    return __builtin_bswap32(v);
#endif
}

inline uint32_t LoadBe32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return Bswap32(v);
}

inline void StoreBe32(uint8_t* p, uint32_t v) {
    v = Bswap32(v);
    std::memcpy(p, &v, 4);
}

// PowerPC slw/srw: a count of 32 to 63 gives 0. (Every count in a valid zlib table is below 16,
// so this is the same as the plain shift; it keeps a corrupt table's result identical too.)
inline uint32_t Slw(uint32_t x, uint32_t n) { return (n & 0x20u) ? 0u : (x << (n & 31u)); }
inline uint32_t Srw(uint32_t x, uint32_t n) { return (n & 0x20u) ? 0u : (x >> (n & 31u)); }

// A code table entry {op u8, bits u8, val u16}, stored big-endian in guest memory.
inline uint32_t Code(const uint8_t* table, uint32_t index) { return LoadBe32(table + 4u * index); }

// Appends `len` bytes (len >= 1) copied from `dist` bytes back in the output itself, exactly as
// the game's byte-by-byte loop does (an overlapping copy repeats the last `dist` bytes), and
// writes nothing past out + len. With dist >= 8 every 8-byte chunk's source lies wholly before
// the chunk, so chunks can be moved whole; the last chunk is moved back to end exactly at len
// (it rewrites bytes the chunk before it wrote, with the same values).
inline uint8_t* CopyFromOutput(uint8_t* out, uint32_t dist, uint32_t len) {
    const uint8_t* from = out - dist;
    if (dist >= 8 && len >= 8) {
        uint32_t i = 0;
        for (; i + 8 <= len; i += 8) std::memcpy(out + i, from + i, 8);
        if (i < len) std::memcpy(out + len - 8, from + len - 8, 8);
    } else if (dist == 1) {
        std::memset(out, from[0], len);
    } else {
        for (uint32_t i = 0; i < len; ++i) out[i] = from[i];
    }
    return out + len;
}

}  // namespace detail

struct Args {
    const uint8_t* in;       // strm->next_in
    uint32_t avail_in;       // strm->avail_in, at least 6
    uint8_t* out;            // strm->next_out
    uint32_t avail_out;      // strm->avail_out, at least 258
    uint32_t produced;       // start - avail_out: bytes this inflate() call has already written, just below `out`
    const uint8_t* window;   // state->window
    uint32_t wsize, whave, wnext;
    const uint8_t* lcode;    // state->lencode
    const uint8_t* dcode;    // state->distcode
    uint32_t lenbits, distbits;
    uint32_t hold, bits;     // bits < 32
};

struct Result {
    uint32_t in_used;        // strm->next_in advances by this (avail_in shrinks by it)
    uint32_t out_written;    // strm->next_out advances by this (avail_out shrinks by it)
    uint32_t hold, bits;
    Ending ending;
    uint32_t features;
};

// inflate_fast() of zlib 1.2.3 (inffast.c), without INFLATE_STRICT (the game's build has no
// dmax check), on host pointers. `a.in`/`a.avail_in` and `a.out`/`a.avail_out` must describe
// readable and writable memory, with `a.produced` bytes of this call's earlier output readable
// below `a.out`; `a.window` holds wsize bytes when whave > 0.
inline Result Run(const Args& a) {
    using namespace detail;
    const uint8_t* in = a.in;                 // next byte to read (zlib's `in` points one before)
    uint8_t* out = a.out;                     // next byte to write
    uint32_t hold = a.hold;
    uint32_t bits = a.bits;
    const uint8_t* const in_limit = a.in + (a.avail_in - 5u);    // zlib: in < last
    uint8_t* const out_limit = a.out + (a.avail_out - 257u);     // zlib: out < end
    uint8_t* const out_begin = a.out - a.produced;               // zlib: beg
    const uint8_t* const lcode = a.lcode;
    const uint8_t* const dcode = a.dcode;
    const uint32_t lmask = Slw(1u, a.lenbits) - 1u;
    const uint32_t dmask = Slw(1u, a.distbits) - 1u;
    Ending ending = kEndLimit;
    uint32_t features = 0;

    uint32_t here, op, n, len, dist, have_out;

    do {
        if (bits < 15) {
            hold += (static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8)) << bits;
            in += 2;
            bits += 16;
        }
        here = Code(lcode, hold & lmask);
    dolen:
        n = (here >> 16) & 0xFFu;
        hold = Srw(hold, n);
        bits -= n;
        op = here >> 24;
        if (op == 0) {  // literal
            *out++ = static_cast<uint8_t>(here);
        } else if (op & 16) {  // length base
            len = here & 0xFFFFu;
            op &= 15u;
            if (op) {
                if (bits < op) {
                    hold += static_cast<uint32_t>(*in++) << bits;
                    bits += 8;
                }
                len += hold & (Slw(1u, op) - 1u);
                hold = Srw(hold, op);
                bits -= op;
            }
            if (bits < 15) {
                hold += (static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8)) << bits;
                in += 2;
                bits += 16;
            }
            here = Code(dcode, hold & dmask);
        dodist:
            n = (here >> 16) & 0xFFu;
            hold = Srw(hold, n);
            bits -= n;
            op = here >> 24;
            if (op & 16) {  // distance base
                dist = here & 0xFFFFu;
                op &= 15u;
                if (bits < op) {
                    hold += static_cast<uint32_t>(*in++) << bits;
                    bits += 8;
                    if (bits < op) {
                        hold += static_cast<uint32_t>(*in++) << bits;
                        bits += 8;
                    }
                }
                dist += hold & (Slw(1u, op) - 1u);
                hold = Srw(hold, op);
                bits -= op;
                have_out = static_cast<uint32_t>(out - out_begin);  // zlib: max distance in the output
                if (dist > have_out) {  // copy from the window
                    op = dist - have_out;  // distance back in the window
                    if (op > a.whave) {
                        ending = kEndTooFar;
                        break;
                    }
                    const uint8_t* from;  // window bytes still to copy, or null when the rest is in the output
                    if (a.wnext == 0) {  // very common case
                        features |= kFeatWindowFlat;
                        from = a.window + (a.wsize - op);
                        if (op < len) {  // some from the window
                            features |= kFeatWindowSplit;
                            len -= op;
                            std::memcpy(out, from, op);
                            out += op;
                            from = nullptr;  // rest from output
                        }
                    } else if (a.wnext < op) {  // wrap around the window
                        features |= kFeatWindowWrap;
                        from = a.window + (a.wsize + a.wnext - op);
                        op -= a.wnext;
                        if (op < len) {  // some from the end of the window
                            features |= kFeatWindowSplit;
                            len -= op;
                            std::memcpy(out, from, op);
                            out += op;
                            from = a.window;
                            if (a.wnext < len) {  // some from the start of the window
                                features |= kFeatWindowTail;
                                op = a.wnext;
                                len -= op;
                                std::memcpy(out, from, op);
                                out += op;
                                from = nullptr;  // rest from output
                            }
                        }
                    } else {  // contiguous in the window
                        features |= kFeatWindowContig;
                        from = a.window + (a.wnext - op);
                        if (op < len) {  // some from the window
                            features |= kFeatWindowSplit;
                            len -= op;
                            std::memcpy(out, from, op);
                            out += op;
                            from = nullptr;  // rest from output
                        }
                    }
                    if (from) {
                        std::memcpy(out, from, len);
                        out += len;
                    } else {
                        out = CopyFromOutput(out, dist, len);
                    }
                } else {  // copy direct from the output (the minimum length is three)
                    out = CopyFromOutput(out, dist, len);
                }
            } else if ((op & 64) == 0) {  // second-level distance code
                features |= kFeatDistSubTable;
                here = Code(dcode, (here & 0xFFFFu) + (hold & (Slw(1u, op) - 1u)));
                goto dodist;
            } else {
                ending = kEndBadDistCode;
                break;
            }
        } else if ((op & 64) == 0) {  // second-level literal/length code
            features |= kFeatLenSubTable;
            here = Code(lcode, (here & 0xFFFFu) + (hold & (Slw(1u, op) - 1u)));
            goto dolen;
        } else if (op & 32) {  // end of block
            ending = kEndBlock;
            break;
        } else {
            ending = kEndBadLiteral;
            break;
        }
    } while (in < in_limit && out < out_limit);

    // Return the unused bytes. zlib's comment says bits < 8 on entry, so `in` cannot go back past where
    // it started, but inflate() can also be entered in the LEN state with up to 14 bits left over from
    // a code that ran out of input (a 10 to 15 bit code cut by the end of a chunk). `in` can then end
    // one or two bytes before a.in; the recompiled code does the same arithmetic (modulo 2^32), and
    // this does not read there, so in_used simply wraps.
    len = bits >> 3;
    in -= len;
    bits -= len << 3;
    hold &= Slw(1u, bits) - 1u;

    Result r;
    r.in_used = static_cast<uint32_t>(in - a.in);
    r.out_written = static_cast<uint32_t>(out - a.out);
    r.hold = hold;
    r.bits = bits;
    r.ending = ending;
    r.features = features;
    return r;
}

// ---------------------------------------------------------------------------------------------
// The guest side.

inline constexpr uint64_t kLowGuestLimit = 0xE0000000ull;  // above: the physical mirror, left to the recompiled code
inline constexpr uint32_t kLowGuestFloor = 0x10000u;
inline constexpr uint32_t kStrmBytes = 56;       // z_stream
inline constexpr uint32_t kStateHead = 112;      // inflate_state up to lens[]: mode .. next
inline constexpr uint32_t kStateBytes = 7104;    // sizeof(inflate_state) of zlib 1.2.3 (CHECK: from the layout, only used for range and overlap tests)
inline constexpr uint32_t kTableBytes = 1444 * 4;  // ENOUGH_LENS + ENOUGH_DISTS code entries: the most a table set holds
inline constexpr uint32_t kMaxWindow = 1u << 16;

struct GuestCall {
    uint32_t strm = 0, state = 0, start = 0;
    uint32_t next_in = 0, avail_in = 0, next_out = 0, avail_out = 0;
    uint32_t produced = 0, out_begin = 0;
    uint32_t window = 0, wsize = 0, whave = 0, wnext = 0;
    uint32_t lencode = 0, distcode = 0, lenbits = 0, distbits = 0;
    uint32_t hold = 0, bits = 0;
    int window_class = 0;  // 0: nothing in the window yet, 1: window full and wnext == 0, 2: wnext != 0
};

namespace detail {
inline bool InRange(uint32_t addr, uint64_t size) {
    return addr >= kLowGuestFloor && static_cast<uint64_t>(addr) + size <= kLowGuestLimit;
}
inline bool Overlap(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1) { return a0 < b1 && b0 < a1; }
}  // namespace detail

// Reads the call's arguments from the guest. False when the native decoder is not meant to take
// the call (the recompiled function runs it): anything outside what zlib's own caller produces,
// or that this code has no way to reproduce byte for byte.
inline bool LoadCall(const uint8_t* base, uint32_t strm, uint32_t start, GuestCall& c) {
    using namespace detail;
    if (!InRange(strm, kStrmBytes)) return false;
    const uint8_t* s = base + strm;
    c.strm = strm;
    c.start = start;
    c.next_in = LoadBe32(s + 0);
    c.avail_in = LoadBe32(s + 4);
    c.next_out = LoadBe32(s + 12);
    c.avail_out = LoadBe32(s + 16);
    c.state = LoadBe32(s + 28);
    if (!InRange(c.state, kStateBytes)) return false;
    const uint8_t* st = base + c.state;
    c.wsize = LoadBe32(st + 40);
    c.whave = LoadBe32(st + 44);
    c.wnext = LoadBe32(st + 48);
    c.window = LoadBe32(st + 52);
    c.hold = LoadBe32(st + 56);
    c.bits = LoadBe32(st + 60);
    c.lencode = LoadBe32(st + 76);
    c.distcode = LoadBe32(st + 80);
    c.lenbits = LoadBe32(st + 84);
    c.distbits = LoadBe32(st + 88);

    // What inflate() guarantees when it calls inflate_fast.
    if (c.avail_in < 6 || c.avail_out < 258 || c.bits >= 32 || start < c.avail_out) return false;
    c.produced = start - c.avail_out;
    if (c.produced > c.next_out) return false;
    c.out_begin = c.next_out - c.produced;
    const uint64_t out_total = static_cast<uint64_t>(c.produced) + c.avail_out;
    if (!InRange(c.next_in, c.avail_in) || !InRange(c.out_begin, out_total)) return false;
    if (c.lenbits < 1 || c.lenbits > 15 || c.distbits < 1 || c.distbits > 15) return false;
    if (!InRange(c.lencode, kTableBytes) || !InRange(c.distcode, kTableBytes)) return false;
    // The window: nothing in it yet, or a ring of wsize bytes with whave valid and the next write at wnext.
    if (c.wsize == 0) {
        if (c.whave != 0 || c.wnext != 0) return false;
    } else {
        if (c.wsize > kMaxWindow || c.whave > c.wsize || c.wnext >= c.wsize) return false;
        if (!InRange(c.window, c.wsize)) return false;
    }
    // The decoder writes only the output. If that overlapped anything it reads, the recompiled
    // function would read its own writes.
    const uint64_t o0 = c.out_begin, o1 = o0 + out_total;
    if (Overlap(o0, o1, c.next_in, static_cast<uint64_t>(c.next_in) + c.avail_in)) return false;
    if (Overlap(o0, o1, c.strm, static_cast<uint64_t>(c.strm) + kStrmBytes)) return false;
    if (Overlap(o0, o1, c.state, static_cast<uint64_t>(c.state) + kStateBytes)) return false;
    if (Overlap(o0, o1, c.lencode, static_cast<uint64_t>(c.lencode) + kTableBytes)) return false;
    if (Overlap(o0, o1, c.distcode, static_cast<uint64_t>(c.distcode) + kTableBytes)) return false;
    if (c.wsize != 0 && Overlap(o0, o1, c.window, static_cast<uint64_t>(c.window) + c.wsize)) return false;
    // The input is only read, but its pages must not be the stream/state either (no sane caller does this).
    if (Overlap(c.next_in, static_cast<uint64_t>(c.next_in) + c.avail_in, c.strm, static_cast<uint64_t>(c.strm) + kStrmBytes))
        return false;

    c.window_class = c.whave == 0 ? 0 : (c.wnext == 0 ? 1 : 2);
    return true;
}

inline Args MakeArgs(uint8_t* base, const GuestCall& c) {
    Args a;
    a.in = base + c.next_in;
    a.avail_in = c.avail_in;
    a.out = base + c.next_out;
    a.avail_out = c.avail_out;
    a.produced = c.produced;
    a.window = base + c.window;
    a.wsize = c.wsize;
    a.whave = c.whave;
    a.wnext = c.wnext;
    a.lcode = base + c.lencode;
    a.dcode = base + c.distcode;
    a.lenbits = c.lenbits;
    a.distbits = c.distbits;
    a.hold = c.hold;
    a.bits = c.bits;
    return a;
}

// Writes what the recompiled function writes (other than the output bytes) into a z_stream
// image and the start of an inflate_state image: either the guest's or a copy.
inline void ApplyResult(uint8_t* strm, uint8_t* state, const GuestCall& c, const Result& r) {
    using namespace detail;
    StoreBe32(strm + 0, c.next_in + r.in_used);
    StoreBe32(strm + 4, c.avail_in - r.in_used);
    StoreBe32(strm + 12, c.next_out + r.out_written);
    StoreBe32(strm + 16, c.avail_out - r.out_written);
    StoreBe32(state + 56, r.hold);
    StoreBe32(state + 60, r.bits);
    switch (r.ending) {
        case kEndLimit:
            break;
        case kEndBlock:
            StoreBe32(state + 0, kModeType);
            break;
        case kEndBadLiteral:
            StoreBe32(strm + 24, kMsgBadLiteral);
            StoreBe32(state + 0, kModeBad);
            break;
        case kEndBadDistCode:
            StoreBe32(strm + 24, kMsgBadDistCode);
            StoreBe32(state + 0, kModeBad);
            break;
        case kEndTooFar:
            StoreBe32(strm + 24, kMsgTooFar);
            StoreBe32(state + 0, kModeBad);
            break;
    }
}

// The whole call, natively, on the guest's memory.
inline Result RunNative(uint8_t* base, const GuestCall& c) {
    const Result r = Run(MakeArgs(base, c));
    ApplyResult(base + c.strm, base + c.state, c, r);
    return r;
}

struct CheckReport {
    bool ok = false;
    bool skipped = false;       // no scratch memory: the recompiled function ran, nothing was compared
    Result result{};            // the native decoder's
    int64_t native_ns = 0;
    int64_t original_ns = 0;
    char what[160] = {};        // first difference, when !ok
};

// Runs the call twice: the native decoder on a copy of the output, then `original()` (the
// recompiled function, which does the call on the guest's memory). Compares the z_stream, the
// start of the inflate_state and the whole output area. The guest ends up with the recompiled
// function's result either way.
template <class Original>
CheckReport RunChecked(uint8_t* base, const GuestCall& c, Original&& original) {
    using namespace detail;
    CheckReport rep;
    const size_t total = static_cast<size_t>(c.produced) + c.avail_out;
    std::unique_ptr<uint8_t[]> scratch(new (std::nothrow) uint8_t[total]);
    if (!scratch) {
        original();
        rep.ok = true;
        rep.skipped = true;
        return rep;
    }
    std::memcpy(scratch.get(), base + c.out_begin, total);
    Args a = MakeArgs(base, c);
    a.out = scratch.get() + c.produced;

    const auto t0 = std::chrono::steady_clock::now();
    rep.result = Run(a);
    const auto t1 = std::chrono::steady_clock::now();

    uint8_t strm_img[kStrmBytes];
    uint8_t state_img[kStateHead];
    std::memcpy(strm_img, base + c.strm, kStrmBytes);
    std::memcpy(state_img, base + c.state, kStateHead);
    ApplyResult(strm_img, state_img, c, rep.result);

    original();
    const auto t2 = std::chrono::steady_clock::now();
    rep.native_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    rep.original_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();

    auto first_diff = [](const uint8_t* x, const uint8_t* y, size_t n) {
        size_t i = 0;
        while (i < n && x[i] == y[i]) ++i;
        return i;
    };
    if (std::memcmp(strm_img, base + c.strm, kStrmBytes) != 0) {
        const size_t i = first_diff(strm_img, base + c.strm, kStrmBytes);
        std::snprintf(rep.what, sizeof(rep.what), "z_stream differs at byte %zu (native %02X, recompiled %02X)", i,
                      strm_img[i], base[c.strm + i]);
    } else if (std::memcmp(state_img, base + c.state, kStateHead) != 0) {
        const size_t i = first_diff(state_img, base + c.state, kStateHead);
        std::snprintf(rep.what, sizeof(rep.what), "inflate_state differs at byte %zu (native %02X, recompiled %02X)", i,
                      state_img[i], base[c.state + i]);
    } else if (std::memcmp(scratch.get(), base + c.out_begin, total) != 0) {
        const size_t i = first_diff(scratch.get(), base + c.out_begin, total);
        std::snprintf(rep.what, sizeof(rep.what), "output differs at byte %zu of %zu (native %02X, recompiled %02X)", i,
                      total, scratch[i], base[c.out_begin + i]);
    } else {
        rep.ok = true;
    }
    return rep;
}

}  // namespace inflate_fast
