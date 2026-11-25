// Check of the native LZX decoder (hooks/lzx_stream.h) against a byte-by-byte reference, on streams this file builds itself: no game,
// no SDK, no recompiled code. Exit code 0 = passed.
//
// What it covers: a verbatim block whose matches reach almost a whole window back. A match of off = window - 3 (the largest LZX
// offset of a 128 KiB window) wraps the source index to a position just AHEAD of the write position, so the source and destination
// ranges overlap. The decoder's fast path used memcpy there (AddressSanitizer: memcpy-param-overlap, 1359 reports on the ROG,
// 2026-11-25); it uses memmove now. The streams here also hold a match that wraps over the window's end (byte loop path) and a
// match with off < length (the overlapping run, byte loop path), so every branch of the copy meets the same reference.
//
// What it proves, and what not: the bytes of every frame equal the reference (an LZ77 copy of the output so far, byte by byte).
// The overlap itself is only DETECTABLE under AddressSanitizer: on x86-64 a memcpy over a source that is ahead of the destination
// happens to copy the right bytes, so a plain build passes with either memcpy or memmove. Build and run it as
//   clang++ -std=c++20 -fsanitize=address -g -I src/mc_engine/hooks src/mc_engine/hooks/lzx_stream_test.cpp -o lzx_stream_test
//   (on Windows put clang_rt.asan_dynamic-x86_64.dll next to it or on PATH) && ./lzx_stream_test
// Checked on 2026-11-25 against the header before the fix (commit 72bc8059): ASan reports memcpy-param-overlap at lzx_stream.h:386
// with ranges 3 bytes apart, the same finding as on the ROG; with the fix the run is clean. The check "the stream reaches the
// overlapping fast path" counts the matches whose source is ahead of the destination and inside it, from the stream's own
// parameters (not from the decoder), so the test cannot quietly stop covering it.
//
// The streams: one verbatim block (type 1) over 6 frames of 32768 bytes. Frames 1-4 are literals (they fill the window: the 5th frame
// writes at window position 0 again with a full history); frames 5 and 6 are literals mixed with the matches described above. The
// trees are the simplest valid ones: every main symbol has a 10-bit code, every length symbol an 8-bit code, the pretree 5 bits (an
// incomplete set of lengths is valid; the code of a symbol is then its own number).

#include "lzx_stream.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr uint32_t kWindow = 0x20000;
constexpr uint32_t kMask = kWindow - 1;
constexpr uint32_t kFrame = 0x8000;
constexpr int kSlots = 34;
constexpr uint32_t kMaxOff = kWindow - 3;  // the largest offset LZX allows in a window of this size
constexpr int kMainBits = 10;
constexpr int kLenBits = 8;
constexpr int kPretreeBits = 5;
constexpr int kMainDelta = 7;  // (0 - 10) mod 17: the pretree symbol that makes a length of 0 into 10
constexpr int kLenDelta = 9;   // (0 - 8) mod 17

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("FAIL: %s\n", what);
  }
}

// Bits first-in = most significant, in 16-bit little-endian words: the LZX bit order.
class BitWriter {
 public:
  void Put(uint32_t value, int count) {
    for (int i = count - 1; i >= 0; --i) bits_.push_back(static_cast<uint8_t>((value >> i) & 1u));
  }
  std::vector<uint8_t> Finish() {
    // Pad to a word, plus two zero words: the decoder reads up to a word ahead of the last symbol.
    while (bits_.size() % 16 != 0) bits_.push_back(0);
    for (int i = 0; i < 32; ++i) bits_.push_back(0);
    std::vector<uint8_t> out;
    for (size_t w = 0; w < bits_.size(); w += 16) {
      uint32_t word = 0;
      for (int i = 0; i < 16; ++i) word = (word << 1) | bits_[w + static_cast<size_t>(i)];
      out.push_back(static_cast<uint8_t>(word & 0xFF));
      out.push_back(static_cast<uint8_t>(word >> 8));
    }
    return out;
  }

 private:
  std::vector<uint8_t> bits_;
};

// The position slots of a 17-bit window (LZX: 34 of them): extra bits 0,0,0,0,1,1,2,2,3,3,... and each slot's base is the sum of the
// sizes of the slots before it. A match's offset is sent as slot + extra bits of (offset + 2 - base).
struct Slot {
  int extra;
  uint32_t base;
};

std::vector<Slot> MakeSlots() {
  std::vector<Slot> slots;
  uint32_t base = 0;
  for (int i = 0; i <= kSlots; ++i) {
    int extra = i / 2 > 1 ? i / 2 - 1 : 0;
    if (extra > 17) extra = 17;
    slots.push_back({extra, base});
    base += 1u << extra;
  }
  return slots;
}

// One frame of the test stream, with the reference output kept beside it.
class StreamBuilder {
 public:
  explicit StreamBuilder(uint32_t block_bytes) : slots_(MakeSlots()), block_bytes_(block_bytes) {}

  void BeginFrame(bool first) {
    bw_ = BitWriter();
    frame_start_ = ref_.size();
    if (first) {
      bw_.Put(0, 1);  // no E8 translation
      bw_.Put(1, 3);  // a verbatim block
      bw_.Put(block_bytes_ >> 8, 16);
      bw_.Put(block_bytes_ & 0xFF, 8);
      PutTree(256, kMainDelta);
      PutTree(528 - 256, kMainDelta);
      PutTree(249, kLenDelta);
    }
  }

  void Literal(uint8_t b) {
    bw_.Put(b, kMainBits);
    ref_.push_back(b);
  }
  void RandomLiterals(uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) Literal(NextByte());
  }
  // A match of `len` (2 to 257) bytes from `off` back. The reference copies byte by byte, as LZ77 defines it.
  void Match(uint32_t off, uint32_t len) {
    PutMatchSymbols(off, len);

    // The window geometry the decoder sees, worked out from the stream's own parameters.
    const uint32_t pos = static_cast<uint32_t>(ref_.size()) & kMask;
    const uint32_t from = (pos - off) & kMask;
    if (from > pos && from < pos + len && from + len <= kWindow) ++overlapping_fast_path_;
    if (from + len > kWindow) ++wrapping_end_;
    if (off < len) ++overlapping_run_;

    for (uint32_t i = 0; i < len; ++i) ref_.push_back(ref_[ref_.size() - off]);
  }
  // A match whose source wraps to `ahead` bytes AHEAD of the write position (off = window - ahead; ahead >= 3).
  void MatchAhead(uint32_t ahead, uint32_t len) { Match(kWindow - ahead, len); }
  // The symbols of a match with no reference output: for a stream that must be refused.
  void BadMatch(uint32_t off, uint32_t len) { PutMatchSymbols(off, len); }
  // A match whose source starts `before_end` bytes before the window's end and runs over the end (wraps to 0).
  void MatchOverWindowEnd(uint32_t before_end, uint32_t len) {
    const uint32_t pos = static_cast<uint32_t>(ref_.size()) & kMask;
    Match(pos + before_end, len);
  }

  void EndFrame() {
    frames_.push_back(bw_.Finish());
    frame_sizes_.push_back(static_cast<uint32_t>(ref_.size() - frame_start_));
  }

  size_t FrameCount() const { return frames_.size(); }
  const std::vector<uint8_t>& Frame(size_t i) const { return frames_[i]; }
  uint32_t FrameSize(size_t i) const { return frame_sizes_[i]; }
  const std::vector<uint8_t>& Ref() const { return ref_; }
  uint32_t FrameOut(size_t i) const {
    uint32_t at = 0;
    for (size_t k = 0; k < i; ++k) at += frame_sizes_[k];
    return at;
  }
  uint32_t OutputSoFar() const { return static_cast<uint32_t>(ref_.size() - frame_start_); }

  uint32_t overlapping_fast_path() const { return overlapping_fast_path_; }
  uint32_t wrapping_end() const { return wrapping_end_; }
  uint32_t overlapping_run() const { return overlapping_run_; }

 private:
  void PutMatchSymbols(uint32_t off, uint32_t len) {
    const uint32_t formatted = off + 2;
    int slot = 0;
    while (slot + 1 <= kSlots && slots_[static_cast<size_t>(slot) + 1].base <= formatted) ++slot;
    const Slot& s = slots_[static_cast<size_t>(slot)];
    const uint32_t lenhdr = len - 2;
    if (lenhdr < 7) {
      bw_.Put(256u + static_cast<uint32_t>(slot) * 8 + lenhdr, kMainBits);
    } else {
      bw_.Put(256u + static_cast<uint32_t>(slot) * 8 + 7, kMainBits);
      bw_.Put(lenhdr - 7, kLenBits);
    }
    bw_.Put(formatted - s.base, s.extra);
  }
  // A tree of `count` lengths, every one the same, sent as `count` copies of one pretree symbol (the pretree's code is its number).
  void PutTree(uint32_t count, int delta) {
    for (int i = 0; i < 20; ++i) bw_.Put(kPretreeBits, 4);  // the pretree: all 20 symbols have 5-bit codes
    for (uint32_t i = 0; i < count; ++i) bw_.Put(static_cast<uint32_t>(delta), kPretreeBits);
  }
  uint8_t NextByte() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return static_cast<uint8_t>(rng_ >> 11);
  }

  std::vector<Slot> slots_;
  uint32_t block_bytes_;
  BitWriter bw_;
  std::vector<uint8_t> ref_;
  size_t frame_start_ = 0;
  std::vector<std::vector<uint8_t>> frames_;
  std::vector<uint32_t> frame_sizes_;
  uint32_t rng_ = 0x1234567u;
  uint32_t overlapping_fast_path_ = 0;
  uint32_t wrapping_end_ = 0;
  uint32_t overlapping_run_ = 0;
};

// The matches of frames 5 and 6 (the window is full: every offset up to kMaxOff is in range). Literals fill the frame to 32768.
void ProbeFrame(StreamBuilder& b) {
  b.RandomLiterals(16);
  b.MatchAhead(3, 20);          // source 3 ahead of the destination, overlapping it: the ASan finding (off = kMaxOff)
  b.RandomLiterals(5);
  b.MatchAhead(3, 257);         // the longest match, the same overlap
  b.MatchAhead(4, 9);           // source 4 ahead, overlapping by 5
  b.MatchAhead(6, 6);           // source 6 ahead and 6 long: touching, not overlapping
  b.MatchAhead(3, 3);           // 3 ahead and 3 long: touching
  b.RandomLiterals(3);
  b.MatchAhead(100, 150);       // 100 ahead, 150 long: overlaps in its second half
  b.MatchOverWindowEnd(5, 12);  // the source runs over the window's end: the byte loop, circular
  b.Match(1, 40);               // a run: off < length, the byte loop
  b.Match(3, 17);               // another overlapping run
  b.MatchAhead(3, 9);
  b.RandomLiterals(kFrame - b.OutputSoFar());
}

void TestFarMatches() {
  constexpr uint32_t kFrames = 6;
  StreamBuilder b(kFrames * kFrame);
  for (uint32_t f = 0; f < kFrames; ++f) {
    b.BeginFrame(f == 0);
    if (f < 4) {
      b.RandomLiterals(kFrame);
    } else {
      ProbeFrame(b);
    }
    b.EndFrame();
  }
  Check(b.Ref().size() == kFrames * kFrame, "the reference holds six full frames");
  Check(b.overlapping_fast_path() >= 8, "the stream reaches the fast path with a source ahead of and inside the destination");
  Check(b.wrapping_end() >= 2, "the stream has matches whose source wraps over the window's end");
  Check(b.overlapping_run() >= 2, "the stream has matches with offset below length");

  lzx_native::Stream stream;
  std::vector<uint8_t> out(kFrame);
  for (size_t f = 0; f < b.FrameCount(); ++f) {
    const bool ok = stream.DecodeFrame(b.Frame(f).data(), static_cast<uint32_t>(b.Frame(f).size()), b.FrameSize(f), out.data());
    char what[96];
    std::snprintf(what, sizeof(what), "frame %zu decodes", f + 1);
    Check(ok, what);
    if (!ok) return;
    const uint32_t at = b.FrameOut(f);
    size_t bad = b.FrameSize(f);
    for (size_t i = 0; i < b.FrameSize(f); ++i) {
      if (out[i] != b.Ref()[at + i]) {
        bad = i;
        break;
      }
    }
    std::snprintf(what, sizeof(what), "frame %zu equals the byte-by-byte reference", f + 1);
    Check(bad == b.FrameSize(f), what);
    if (bad != b.FrameSize(f)) std::printf("  first difference at byte %zu of the frame\n", bad);
  }
}

// An offset beyond what has been written is refused, an offset of exactly the history is not: the first frame of a stream cannot
// reach back a window (this guard is what makes the wrapped source valid history in the later frames).
void TestHistoryGuard() {
  std::vector<uint8_t> out(kFrame);
  {
    StreamBuilder good(kFrame);
    good.BeginFrame(true);
    good.RandomLiterals(10);
    good.Match(10, 5);  // exactly the 10 bytes of history
    good.RandomLiterals(kFrame - good.OutputSoFar());
    good.EndFrame();
    lzx_native::Stream stream;
    const bool ok = stream.DecodeFrame(good.Frame(0).data(), static_cast<uint32_t>(good.Frame(0).size()), kFrame, out.data());
    Check(ok, "a match of exactly the history decodes");
    Check(ok && std::equal(out.begin(), out.end(), good.Ref().begin()), "and equals the reference");
  }
  {
    StreamBuilder bad(kFrame);
    bad.BeginFrame(true);
    bad.RandomLiterals(10);
    bad.BadMatch(kMaxOff, 20);  // 10 bytes written, a window asked for
    bad.RandomLiterals(kFrame - 30);
    bad.EndFrame();
    lzx_native::Stream stream;
    const bool ok = stream.DecodeFrame(bad.Frame(0).data(), static_cast<uint32_t>(bad.Frame(0).size()), kFrame, out.data());
    Check(!ok, "a match from beyond the history is refused");
    Check(stream.failed(), "and the stream is dead until Reset()");
  }
}

}  // namespace

int main() {
  TestFarMatches();
  TestHistoryGuard();
  if (g_failures != 0) {
    std::printf("lzx_stream_test: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("lzx_stream_test: all %d checks passed\n", g_checks);
  return 0;
}
