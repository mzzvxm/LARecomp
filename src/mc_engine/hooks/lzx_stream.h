// A streaming LZX decoder for the game's XCompress streams, and the chunk layer of the game's
// XMemDecompressStream on top of it (lzx_native.cpp uses both).
//
// Why this is not rex/system/lzx.h: the SDK's lzx_decompress() decodes a whole bitstream in one call
// and starts every call with a fresh decoder (R0-R2 = 1, no block, empty tree lengths). The game feeds
// a resource in pieces of at most 32 KiB (sub_821BC140 reads 32 KiB at a time and calls
// zlibInflater::InflateBegin, sub_821D5E10, which calls XMemDecompressStream, sub_8244FF20, with
// whatever it has) and the decoder keeps its state between the calls: the window, R0-R2, the block
// in progress and the tree lengths. A block is not tied to a frame, so a frame cannot be decoded on
// its own. The libmspack decoder that lzx_decompress wraps (lzxd_*) is linked privately into
// rexruntime.dll and its header is not in LARecomp's include path, so there is no streaming entry
// point to call. This file therefore carries a small decoder of its own, written to the same rules as
// libmspack's lzxd.c (the one the mod loader checked against all 14,928 resources of
// xarchive_cache.rpf, xcompress.cpp). It is checked against the recompiled decoder at run time
// (lzx_native.cpp) and the recompiled decoder takes over on any difference.
//
// The format as read from the game (VERIFIED = read in the generated code, INFERENCE = from the
// libmspack rules and not seen in the game's code):
//  - the context has a 128 KiB window (VERIFIED: sub_821D5D60 passes {0, 0x20000, 0x80000} to
//    XMemCreateDecompressionContext; the window size is at ctx + 24 and its mask at ctx + 28);
//  - the stream is a run of chunks, each one LZX frame: [len u16 BE][len bytes] for a frame of 32768
//    output bytes, or [0xFF][out u16 BE][len u16 BE][len bytes] for a shorter one, which ends the
//    stream (VERIFIED: sub_82461010 parses exactly this);
//  - every frame starts with a fresh bit reader on its own bytes (VERIFIED: sub_82461530 sets the
//    source cursor and end, sub_82461E68 primes the bit buffer), the LZX state carries on (VERIFIED:
//    block type at +11984 + 20, bytes left in the block at +11980 + 20, R0-R2 in the context);
//  - the bit order, trees, block types (1 verbatim, 2 aligned, 3 uncompressed), the position slots
//    and the E8 translation follow the LZX format as libmspack's lzxd.c has it (INFERENCE for the
//    details the checks do not reach: the pad byte after an odd-sized uncompressed block is skipped
//    in the frame that holds the block's last byte, E8 translation is applied as libmspack does).
// A match never crosses a frame boundary (libmspack rejects it, and the encoder does not make one).

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

namespace lzx_native {

constexpr uint32_t kWindowSize = 0x20000;  // 128 KiB, 17 bits
constexpr uint32_t kWindowMask = kWindowSize - 1;
constexpr uint32_t kFrameSize = 0x8000;
constexpr int kPosSlots = 34;                          // for a 17-bit window
constexpr int kMainSymbols = 256 + kPosSlots * 8;      // 528
constexpr int kLengthSymbols = 249;
constexpr int kPretreeSymbols = 20;
constexpr int kAlignedSymbols = 8;
constexpr int kLenSafety = 64;  // libmspack writes a pretree run past `last` into slack; so do we
constexpr int kFastBits = 10;
constexpr int kMaxCodeLen = 16;

namespace detail {

struct SlotTables {
    uint8_t extra[kPosSlots + 4];
    uint32_t base[kPosSlots + 4];
};

inline const SlotTables& Slots() {
    static const SlotTables t = [] {
        SlotTables s{};
        for (int i = 0, j = 0; i < kPosSlots + 4; i += 2) {
            s.extra[i] = static_cast<uint8_t>(j);
            if (i + 1 < kPosSlots + 4) s.extra[i + 1] = static_cast<uint8_t>(j);
            if (i != 0 && j < 17) ++j;
        }
        uint32_t pos = 0;
        for (int i = 0; i < kPosSlots + 4; ++i) {
            s.base[i] = pos;
            pos += 1u << s.extra[i];
        }
        return s;
    }();
    return t;
}

// A canonical Huffman code (codes assigned by increasing length, then symbol; read most significant bit
// first). Codes of up to kFastBits bits are one table lookup; longer ones are walked length by length.
struct Huff {
    uint16_t fast[1 << kFastBits];       // symbol | length << 10, or 0
    uint16_t count[kMaxCodeLen + 1];     // codes of each length
    uint16_t first[kMaxCodeLen + 1];     // first code of each length
    uint16_t index[kMaxCodeLen + 1];     // position of that length's first symbol in sorted[]
    uint16_t sorted[kMainSymbols];
    bool empty = true;

    // false for an over-subscribed set of lengths. An incomplete set is accepted; a bit pattern that
    // matches no code is an error when it is met.
    bool Build(const uint8_t* lens, int n) {
        std::memset(count, 0, sizeof(count));
        for (int i = 0; i < n; ++i) {
            if (lens[i] > kMaxCodeLen) return false;
            ++count[lens[i]];
        }
        empty = static_cast<int>(count[0]) == n;
        count[0] = 0;
        int left = 1;
        for (int l = 1; l <= kMaxCodeLen; ++l) {
            left <<= 1;
            left -= count[l];
            if (left < 0) return false;
        }
        uint16_t offs[kMaxCodeLen + 2];
        offs[1] = 0;
        for (int l = 1; l <= kMaxCodeLen; ++l) offs[l + 1] = static_cast<uint16_t>(offs[l] + count[l]);
        for (int l = 1; l <= kMaxCodeLen; ++l) index[l] = offs[l];
        for (int i = 0; i < n; ++i) {
            if (lens[i]) sorted[offs[lens[i]]++] = static_cast<uint16_t>(i);
        }
        std::memset(fast, 0, sizeof(fast));
        uint32_t code = 0;
        for (int l = 1; l <= kMaxCodeLen; ++l) {
            first[l] = static_cast<uint16_t>(code);
            if (l <= kFastBits) {
                const uint32_t span = 1u << (kFastBits - l);
                for (uint32_t k = 0; k < count[l]; ++k) {
                    const uint32_t start = (code + k) << (kFastBits - l);
                    const uint16_t entry = static_cast<uint16_t>(sorted[index[l] + k] | (l << 10));
                    for (uint32_t j = 0; j < span; ++j) fast[start + j] = entry;
                }
            }
            code = (code + count[l]) << 1;
        }
        return true;
    }
};

// The LZX bit reader: little-endian 16-bit words, bits taken from the top. Reading past the end of the
// frame's bytes gives zeros (the decoder looks ahead by up to a word; a frame that really needs more
// than 4 bytes beyond its end is reported by Overran()).
struct Bits {
    const uint8_t* data;
    size_t len;
    size_t pos = 0;
    uint32_t buf = 0;
    int bits = 0;

    Bits(const uint8_t* d, size_t n) : data(d), len(n) {}

    void Ensure(int n) {
        while (bits < n) {
            uint32_t w = 0;
            if (pos + 1 < len) {
                w = static_cast<uint32_t>(data[pos]) | (static_cast<uint32_t>(data[pos + 1]) << 8);
            } else if (pos < len) {
                w = data[pos];
            }
            pos += 2;
            buf |= w << (16 - bits);
            bits += 16;
        }
    }
    uint32_t Read(int n) {  // 0 to 16 bits
        if (n == 0) return 0;
        Ensure(n);
        const uint32_t v = buf >> (32 - n);
        buf <<= n;
        bits -= n;
        return v;
    }
    int Symbol(const Huff& h) {  // -1 for a pattern that matches no code
        Ensure(16);
        const uint32_t top = buf >> 16;
        const uint16_t e = h.fast[top >> (16 - kFastBits)];
        if (e) {
            const int l = e >> 10;
            buf <<= l;
            bits -= l;
            return e & 0x3FF;
        }
        for (int l = kFastBits + 1; l <= kMaxCodeLen; ++l) {
            const uint32_t code = top >> (16 - l);
            const uint32_t d = code - h.first[l];
            if (d < h.count[l]) {
                buf <<= l;
                bits -= l;
                return h.sorted[h.index[l] + d];
            }
        }
        return -1;
    }
    bool Overran() const { return pos > len + 4; }
};

}  // namespace detail

// One LZX stream: the decoder state that XMemDecompressStream's context keeps between frames.
class Stream {
public:
    Stream() : window_(new uint8_t[kWindowSize]) { Reset(); }

    // A new stream (XMemResetDecompressionContext).
    void Reset() {
        wpos_ = 0;
        total_out_ = 0;
        r0_ = r1_ = r2_ = 1;
        block_type_ = 0;
        block_remaining_ = block_length_ = 0;
        header_read_ = false;
        intel_filesize_ = 0;
        intel_started_ = false;
        intel_curpos_ = 0;
        frames_ = 0;
        failed_ = false;
        std::memset(main_len_, 0, sizeof(main_len_));
        std::memset(len_len_, 0, sizeof(len_len_));
        std::memset(aligned_len_, 0, sizeof(aligned_len_));
        std::memset(window_.get(), 0, kWindowSize);
    }

    bool failed() const { return failed_; }

    // Decodes one frame: `src[0, src_len)` are the frame's compressed bytes (the chunk without its
    // header), `out_size` the bytes it decodes to (32768, or less for the last frame). Writes them to
    // `dst`. False if the data is not a valid frame; the stream is then dead until Reset().
    bool DecodeFrame(const uint8_t* src, uint32_t src_len, uint32_t out_size, uint8_t* dst) {
        if (failed_) return false;
        if (!Decode(src, src_len, out_size, dst)) {
            failed_ = true;
            return false;
        }
        return true;
    }

private:
    bool ReadLens(detail::Bits& br, uint8_t* lens, int first, int last) {
        uint8_t pre_len[kPretreeSymbols];
        for (int i = 0; i < kPretreeSymbols; ++i) pre_len[i] = static_cast<uint8_t>(br.Read(4));
        if (!pretree_.Build(pre_len, kPretreeSymbols)) return false;
        for (int x = first; x < last;) {
            int z = br.Symbol(pretree_);
            if (z < 0) return false;
            if (z == 17) {
                int y = static_cast<int>(br.Read(4)) + 4;
                while (y--) lens[x++] = 0;
            } else if (z == 18) {
                int y = static_cast<int>(br.Read(5)) + 20;
                while (y--) lens[x++] = 0;
            } else if (z == 19) {
                int y = static_cast<int>(br.Read(1)) + 4;
                z = br.Symbol(pretree_);
                if (z < 0) return false;
                z = lens[x] - z;
                if (z < 0) z += 17;
                while (y--) lens[x++] = static_cast<uint8_t>(z);
            } else {
                z = lens[x] - z;
                if (z < 0) z += 17;
                lens[x++] = static_cast<uint8_t>(z);
            }
        }
        return true;
    }

    bool Decode(const uint8_t* src, uint32_t src_len, uint32_t out_size, uint8_t* dst) {
        if (out_size == 0 || out_size > kFrameSize || wpos_ + out_size > kWindowSize) return false;
        detail::Bits br(src, src_len);
        const detail::SlotTables& slots = detail::Slots();
        uint8_t* const window = window_.get();

        if (!header_read_) {
            uint32_t hi = 0, lo = 0;
            if (br.Read(1)) {
                hi = br.Read(16);
                lo = br.Read(16);
            }
            intel_filesize_ = (hi << 16) | lo;
            header_read_ = true;
        }

        const uint32_t frame_start = wpos_;
        uint32_t pos = wpos_;
        uint32_t todo = out_size;
        while (todo > 0) {
            if (block_remaining_ == 0) {
                block_type_ = static_cast<int>(br.Read(3));
                const uint32_t hi = br.Read(16);
                const uint32_t lo = br.Read(8);
                block_remaining_ = block_length_ = (hi << 8) | lo;
                switch (block_type_) {
                    case 2:
                        for (int i = 0; i < kAlignedSymbols; ++i) aligned_len_[i] = static_cast<uint8_t>(br.Read(3));
                        if (!aligned_.Build(aligned_len_, kAlignedSymbols)) return false;
                        [[fallthrough]];
                    case 1:
                        if (!ReadLens(br, main_len_, 0, 256)) return false;
                        if (!ReadLens(br, main_len_, 256, kMainSymbols)) return false;
                        if (!main_.Build(main_len_, kMainSymbols)) return false;
                        if (main_len_[0xE8] != 0) intel_started_ = true;
                        if (!ReadLens(br, len_len_, 0, kLengthSymbols)) return false;
                        if (!length_.Build(len_len_, kLengthSymbols)) return false;
                        break;
                    case 3: {
                        intel_started_ = true;
                        if (br.bits == 0) br.Ensure(16);  // 1 to 16 bits of alignment
                        br.bits = 0;
                        br.buf = 0;
                        if (br.pos + 12 > br.len) return false;
                        const uint8_t* r = br.data + br.pos;
                        r0_ = Le32(r);
                        r1_ = Le32(r + 4);
                        r2_ = Le32(r + 8);
                        br.pos += 12;
                        break;
                    }
                    default:
                        return false;
                }
            }

            int32_t run = static_cast<int32_t>(std::min(block_remaining_, todo));
            todo -= static_cast<uint32_t>(run);
            block_remaining_ -= static_cast<uint32_t>(run);

            if (block_type_ == 3) {
                while (run > 0) {
                    if (br.pos >= br.len) return false;
                    const uint32_t n = static_cast<uint32_t>(std::min<size_t>(br.len - br.pos, static_cast<size_t>(run)));
                    std::memcpy(window + pos, br.data + br.pos, n);
                    br.pos += n;
                    pos += n;
                    run -= static_cast<int32_t>(n);
                }
                if (block_remaining_ == 0 && (block_length_ & 1) && br.pos < br.len) ++br.pos;  // the pad byte
                continue;
            }

            const bool aligned = block_type_ == 2;
            while (run > 0) {
                int sym = br.Symbol(main_);
                if (sym < 0) return false;
                if (sym < 256) {
                    window[pos++] = static_cast<uint8_t>(sym);
                    --run;
                    continue;
                }
                sym -= 256;
                int mlen = sym & 7;
                if (mlen == 7) {
                    const int footer = br.Symbol(length_);
                    if (footer < 0) return false;
                    mlen += footer;
                }
                mlen += 2;
                uint32_t off = static_cast<uint32_t>(sym >> 3);
                if (off > 2) {
                    if (off != 3) {
                        const uint32_t extra = slots.extra[off];
                        if (aligned && extra >= 3) {
                            uint32_t verbatim = 0;
                            if (extra > 3) verbatim = br.Read(static_cast<int>(extra) - 3) << 3;
                            const int a = br.Symbol(aligned_);
                            if (a < 0) return false;
                            off = slots.base[off] - 2 + verbatim + static_cast<uint32_t>(a);
                        } else if (extra > 0) {
                            off = slots.base[off] - 2 + br.Read(static_cast<int>(extra));
                        } else {
                            off = 1;
                        }
                    } else {
                        off = 1;
                    }
                    r2_ = r1_;
                    r1_ = r0_;
                    r0_ = off;
                } else if (off == 0) {
                    off = r0_;
                } else if (off == 1) {
                    off = r1_;
                    r1_ = r0_;
                    r0_ = off;
                } else {
                    off = r2_;
                    r2_ = r0_;
                    r0_ = off;
                }

                const uint64_t history = std::min<uint64_t>(total_out_ + (pos - frame_start), kWindowSize);
                if (off == 0 || off > history) return false;
                if (pos + static_cast<uint32_t>(mlen) > kWindowSize) return false;  // would run over the window's end
                const uint32_t from = (pos - off) & kWindowMask;
                if (off >= static_cast<uint32_t>(mlen) && from + static_cast<uint32_t>(mlen) <= kWindowSize) {
                    std::memcpy(window + pos, window + from, static_cast<size_t>(mlen));
                } else {
                    for (int i = 0; i < mlen; ++i) window[pos + i] = window[(from + i) & kWindowMask];
                }
                pos += static_cast<uint32_t>(mlen);
                run -= mlen;
            }
            if (run < 0) {  // the last match ran past the run
                if (static_cast<uint32_t>(-run) > block_remaining_) return false;
                block_remaining_ -= static_cast<uint32_t>(-run);
            }
        }

        if (pos - frame_start != out_size) return false;  // streams do not extend over frame boundaries
        if (br.Overran()) return false;

        std::memcpy(dst, window + frame_start, out_size);
        if (intel_started_ && intel_filesize_ != 0 && frames_ < 32768 && out_size > 10) {
            uint8_t* p = dst;
            uint8_t* const end = dst + out_size - 10;
            int32_t curpos = intel_curpos_;
            const int32_t filesize = static_cast<int32_t>(intel_filesize_);
            while (p < end) {
                if (*p++ != 0xE8) {
                    ++curpos;
                    continue;
                }
                const int32_t abs_off = static_cast<int32_t>(Le32(p));
                if (abs_off >= -curpos && abs_off < filesize) {
                    const int32_t rel_off = abs_off >= 0 ? abs_off - curpos : abs_off + filesize;
                    p[0] = static_cast<uint8_t>(rel_off);
                    p[1] = static_cast<uint8_t>(rel_off >> 8);
                    p[2] = static_cast<uint8_t>(rel_off >> 16);
                    p[3] = static_cast<uint8_t>(rel_off >> 24);
                }
                p += 4;
                curpos += 5;
            }
        }
        intel_curpos_ += static_cast<int32_t>(out_size);

        wpos_ += out_size;
        if (wpos_ >= kWindowSize) wpos_ = 0;
        total_out_ += out_size;
        ++frames_;
        return true;
    }

    static uint32_t Le32(const uint8_t* p) {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
    }

    std::unique_ptr<uint8_t[]> window_;
    uint32_t wpos_ = 0;
    uint64_t total_out_ = 0;
    uint32_t r0_ = 1, r1_ = 1, r2_ = 1;
    int block_type_ = 0;
    uint32_t block_remaining_ = 0, block_length_ = 0;
    bool header_read_ = false;
    uint32_t intel_filesize_ = 0;
    bool intel_started_ = false;
    int32_t intel_curpos_ = 0;
    uint32_t frames_ = 0;
    bool failed_ = false;
    uint8_t main_len_[kMainSymbols + kLenSafety];
    uint8_t len_len_[kLengthSymbols + kLenSafety];
    uint8_t aligned_len_[kAlignedSymbols];
    detail::Huff main_, length_, aligned_, pretree_;
};

// The chunk layer of XMemDecompressStream (the game's sub_82461010, read line by line). One of these
// per decompression context. Step() does what one call of the game's function does with the same
// arguments: the same bytes written, the same number of source bytes consumed.
//
// The game's context fields it mirrors (offsets from ctx + 20): a 32 KiB buffer for a frame that does
// not fit the caller's output (+12264) with the position (+12268) and the bytes left (+12272) in it, a
// buffer for a chunk that was cut by the end of the caller's input (+12276) with the bytes held
// (+12280), and the flag that the 0xFF (last) chunk was seen (+12284).
class Chunks {
public:
    Chunks() : frame_buf_(new uint8_t[kFrameBuf]), chunk_buf_(new uint8_t[kChunkBuf]) {}

    void Reset() {
        lz_.Reset();
        fb_pos_ = fb_remaining_ = 0;
        chunk_held_ = 0;
        ended_ = false;
        failed_ = false;
    }
    bool failed() const { return failed_; }

    // `src` / `src_len`: the caller's input; `dst` / `dst_cap`: the caller's output space. On return
    // `written` and `consumed` are what the game stores into *pDstSize and *pSrcSize.
    void Step(const uint8_t* src, uint32_t src_len, uint8_t* dst, uint32_t dst_cap, uint32_t& written,
              uint32_t& consumed) {
        written = 0;
        consumed = 0;
        uint32_t limit = ended_ ? 0 : src_len;  // the game ignores the input once the last chunk was seen

        if (fb_remaining_ != 0) {  // output the last call could not deliver
            const uint32_t n = std::min(fb_remaining_, dst_cap);
            std::memcpy(dst, frame_buf_.get() + fb_pos_, n);
            fb_remaining_ -= n;
            fb_pos_ += n;
            written = n;
        }
        if (limit == 0 && chunk_held_ <= 1) return;

        const uint8_t* cur = src;  // the span the chunk loop reads from
        uint32_t rem = limit;
        if (chunk_held_ != 0) {  // finish the chunk the last call left half-read
            uint8_t* const cb = chunk_buf_.get();
            cur = cb;
            const bool ff = cb[0] == 0xFF;
            const uint32_t hdr = ff ? 5 : 2;
            if (chunk_held_ < hdr) {
                const uint32_t n = std::min(hdr - chunk_held_, limit);
                std::memcpy(cb + chunk_held_, src, n);
                chunk_held_ += n;
                consumed = n;
                rem = limit - n;
            }
            if (rem != 0) {
                const uint8_t* lenp = ff ? cb + 3 : cb;
                const uint32_t total = ((static_cast<uint32_t>(lenp[0]) << 8) | lenp[1]) + hdr;
                const uint32_t take = std::min(total - chunk_held_, limit - consumed);
                std::memcpy(cb + chunk_held_, src + consumed, take);
                chunk_held_ += take;
                consumed += take;
                rem = chunk_held_ < total ? 0 : total;  // from here `rem` is the bytes in the span `cur`
            }
        }

        while (rem != 0 && fb_remaining_ == 0) {
            const uint8_t first = cur[0];
            uint32_t hdr, total, out_size = kFrameSize;
            if (first == 0xFF) {
                hdr = 5;
                total = 5;
                if (rem >= 5) {
                    out_size = (static_cast<uint32_t>(cur[1]) << 8) | cur[2];
                    total = ((static_cast<uint32_t>(cur[3]) << 8) | cur[4]) + 5;
                }
            } else {
                hdr = 2;
                total = 2;
                if (rem >= 2) total = ((static_cast<uint32_t>(first) << 8) | cur[1]) + 2;
            }
            if (rem < total) {  // cut by the end of the input: keep it for the next call
                std::memcpy(chunk_buf_.get(), cur, rem);
                chunk_held_ = rem;
                consumed += rem;
                rem = 0;
                break;
            }
            const uint32_t space = dst_cap - written;
            uint8_t* const out = space < out_size ? frame_buf_.get() : dst + written;
            uint32_t produced = 0;
            if (!failed_ && total > hdr) {
                if (lz_.DecodeFrame(cur + hdr, total - hdr, out_size, out)) {
                    produced = out_size;
                } else {
                    failed_ = true;  // the game's decoder reports 0 bytes for a frame it cannot decode
                }
            }
            if (out == frame_buf_.get()) {
                if (produced != 0) {  // (a frame that failed to decode gives nothing; the game's code would copy stale bytes)
                    std::memcpy(dst + written, frame_buf_.get(), space);
                    fb_pos_ = space;
                    written += space;
                    fb_remaining_ = produced - space;
                }
            } else {
                written += produced;
            }
            if (cur == chunk_buf_.get()) {
                cur = src + consumed;
                chunk_held_ = 0;
            } else {
                cur += total;
                consumed += total;
            }
            if (first == 0xFF) {
                limit = consumed;
                ended_ = true;
            }
            rem = limit - consumed;
        }
    }

private:
    static constexpr uint32_t kChunkBuf = 0x10000 + 16;  // a chunk is at most 65535 bytes plus its 5-byte header
    static constexpr uint32_t kFrameBuf = 0x10000;       // a frame is 32768 bytes; room for a bad 0xFF header's 16-bit size

    Stream lz_;
    std::unique_ptr<uint8_t[]> frame_buf_;
    std::unique_ptr<uint8_t[]> chunk_buf_;
    uint32_t fb_pos_ = 0, fb_remaining_ = 0;
    uint32_t chunk_held_ = 0;
    bool ended_ = false;
    bool failed_ = false;
};

}  // namespace lzx_native
