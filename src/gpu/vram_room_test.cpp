// Check of the room rules of vram_room.h (the line a context asks before it is made, the upscaler's need,
// the wait after a refusal, the upscaler that fits). A program of its own: it needs no renderer, no SDK and
// no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_vram_room_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_vram_room_test
//
// The card sizes are real ones: a 4 GB laptop card (4096 MiB, a budget a little under it), a 2 GB card, a
// 16 GB card, and the budget of an adapter that reports none.

#include <cstdint>
#include <cstdio>
#include <string>

#include "vram_room.h"

using namespace mcla::gpu::vram_room;

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

void CheckNum(uint64_t got, uint64_t want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failed;
    std::printf("FAILED: %s: got %llu, want %llu\n", what.c_str(), (unsigned long long)got,
                (unsigned long long)want);
  }
}

constexpr uint64_t M(uint64_t mib) {
  return mib << 20;
}

// ---- the line --------------------------------------------------------------------------------------

void TestHasRoom() {
  // 4 GB laptop card: budget 3,900 MiB, the line is 3,510 MiB.
  const uint64_t budget = M(3900);
  Check(HasRoom(M(2000), budget, M(500)), "2000 + 500 of 3900 has room");
  Check(HasRoom(M(3000), budget, M(500)), "3000 + 500 = 3500 is under the 3510 line");
  Check(!HasRoom(M(3000), budget, M(511)), "3000 + 511 = 3511 is over the 3510 line");
  Check(!HasRoom(M(3510), budget, 0), "exactly on the line is not room (the response starts there)");
  Check(HasRoom(M(3510) - 1, budget, 0), "one byte under the line is room");
  Check(HasRoom(M(3600), budget, 0) == false, "already over the line: no room for nothing");
  // The line follows the percent given.
  Check(HasRoom(M(3000), budget, M(500), 95), "the same 3500 is under a 95% line (3705)");
  Check(!HasRoom(M(3000), budget, M(800), 90), "3800 is over a 90% line");
  // An adapter that reports no budget: there is nothing to compare with.
  Check(HasRoom(M(9000), 0, M(9000)), "no budget reported: always room");
  Check(HasRoom(0, 0, 0), "no budget, nothing used");
  // A 16 GB card has room for a 4K context on top of a busy scene.
  Check(HasRoom(M(6000), M(15200), M(700)), "16 GB card");
  // A 2 GB card at 1440p: 1,700 MiB in use of a 1,900 MiB budget has no room for even a small context.
  Check(!HasRoom(M(1700), M(1900), M(200)), "2 GB card, 1700 + 200 of 1900 (line 1710)");
  Check(HasRoom(M(1400), M(1900), M(200)), "2 GB card, 1400 + 200 of 1900");
  // Sums in the range of 8 GiB budgets do not overflow.
  Check(HasRoom(uint64_t(8) << 30, uint64_t(24) << 30, uint64_t(1) << 30), "24 GiB budget");
}

// ---- the need ---------------------------------------------------------------------------------------

void TestNeed() {
  CheckNum(OutputImageBytes(1920, 1080), 1920ull * 1080 * 8, "1080p output image");
  CheckNum(OutputImageBytes(3840, 2160), 3840ull * 2160 * 8, "4K output image bytes");
  Check(OutputImageBytes(3840, 2160) >> 20 == 63, "4K output image is 63 MiB (RGBA16F)");
  CheckNum(UpscalerNeed(M(300), 1920, 1080), M(300) + 1920ull * 1080 * 8, "context plus output");
  CheckNum(UpscalerNeed(0, 3840, 2160), 0, "an unknown context size asks for nothing: nothing is refused");
  // The estimate from the last context.
  CheckNum(ScaledEstimate(M(100), 1920ull * 1080, 3840ull * 2160), M(400), "4x the pixels, 4x the bytes");
  CheckNum(ScaledEstimate(M(100), 1920ull * 1080, 1920ull * 1080), M(100), "same size");
  CheckNum(ScaledEstimate(0, 1920ull * 1080, 3840ull * 2160), 0, "nothing measured");
  CheckNum(ScaledEstimate(M(100), 0, 3840ull * 2160), 0, "no measured size");
  CheckNum(ScaledEstimate(M(100), 1920ull * 1080, 0), 0, "no size asked");
  // The decision a worker makes: the 4 GB card at 4K with the scene already in.
  const uint64_t need4k = UpscalerNeed(M(330), 3840, 2160);
  Check(!HasRoom(M(3250), M(3900), need4k), "4 GB card at 4K with 3250 MiB in use: no room");
  Check(HasRoom(M(2500), M(3900), need4k), "4 GB card at 4K with 2500 MiB in use: room");
  // And 1080p on the same card.
  const uint64_t need1080 = UpscalerNeed(M(120), 1920, 1080);
  Check(HasRoom(M(3200), M(3900), need1080), "4 GB card at 1080p with 3200 MiB in use: room");
}

// ---- the wait after a refusal ------------------------------------------------------------------------

void TestWaits() {
  CheckNum(NextWaitMs(0), kFirstWaitMs, "first wait");
  CheckNum(NextWaitMs(5000), 10000, "doubles");
  CheckNum(NextWaitMs(10000), 20000, "doubles again");
  CheckNum(NextWaitMs(40000), kMaxWaitMs, "capped");
  CheckNum(NextWaitMs(kMaxWaitMs), kMaxWaitMs, "stays at the cap");
}

void TestBook() {
  RefusalBook<int, 4> book;
  Check(!book.Blocked(1, 0), "nothing refused: not blocked");
  Check(!book.Known(1), "nothing refused: not known");
  CheckNum(book.Refuse(1, 1000), 5000, "first refusal waits 5 s");
  Check(book.Blocked(1, 1000), "blocked at once");
  Check(book.Blocked(1, 5999), "blocked 1 ms before the end");
  Check(!book.Blocked(1, 6000), "free at the end of the wait");
  Check(book.Known(1), "still known after the wait (the next refusal doubles)");
  CheckNum(book.LeftMs(1, 2000), 4000, "left");
  CheckNum(book.LeftMs(1, 7000), 0, "none left");
  // Asked again after the wait and refused again: twice as long.
  CheckNum(book.Refuse(1, 6000), 10000, "second refusal waits 10 s");
  Check(book.Blocked(1, 15999), "second wait: blocked");
  Check(!book.Blocked(1, 16000), "second wait: over");
  CheckNum(book.Refuse(1, 16000), 20000, "third refusal waits 20 s");
  CheckNum(book.Refuse(1, 36000), 40000, "fourth 40 s");
  CheckNum(book.Refuse(1, 76000), kMaxWaitMs, "fifth capped at 60 s");
  CheckNum(book.Refuse(1, 136000), kMaxWaitMs, "stays at 60 s");
  // Another request is its own series.
  Check(!book.Blocked(2, 136000), "another request is not blocked");
  CheckNum(book.Refuse(2, 136000), 5000, "another request starts at 5 s");
  // Made: the series ends and the next refusal starts again at the first wait.
  book.Clear(1);
  Check(!book.Known(1), "cleared");
  Check(!book.Blocked(1, 136000), "cleared: not blocked");
  CheckNum(book.Refuse(1, 140000), 5000, "after a clear, 5 s again");
  CheckNum(book.Count(), 2, "two entries");
  book.ClearAll();
  CheckNum(book.Count(), 0, "none after ClearAll");
  // Capacity: the oldest entry makes room.
  RefusalBook<int, 3> small;
  small.Refuse(10, 0);
  small.Refuse(11, 1);
  small.Refuse(12, 2);
  small.Refuse(10, 3);  // 10 is the newest now; 11 is the oldest
  small.Refuse(13, 4);
  Check(!small.Known(11), "the oldest entry (11) was dropped");
  Check(small.Known(10) && small.Known(12) && small.Known(13), "the others stay");
  CheckNum(small.Count(), 3, "three entries");
}

// ---- the upscaler that fits --------------------------------------------------------------------------

void TestFit() {
  const auto all_run = [](uint8_t) { return true; };
  const auto none_refused = [](uint8_t) { return false; };
  CheckNum(FitKind(kKindOff, none_refused, all_run), kKindOff, "off stays off");
  CheckNum(FitKind(kKindFsr31, none_refused, all_run), kKindFsr31, "FSR 3.1 that fits");
  CheckNum(FitKind(kKindFsr4, none_refused, all_run), kKindFsr4, "FSR 4 that fits");
  CheckNum(FitKind(kKindDlss, none_refused, all_run), kKindDlss, "DLSS that fits");
  // FSR 4 refused: FSR 3.1 when it runs and is not refused.
  const auto fsr4_refused = [](uint8_t k) { return k == kKindFsr4; };
  CheckNum(FitKind(kKindFsr4, fsr4_refused, all_run), kKindFsr31, "FSR 4 refused: FSR 3.1");
  // Both refused: none.
  const auto both_refused = [](uint8_t k) { return k == kKindFsr4 || k == kKindFsr31; };
  CheckNum(FitKind(kKindFsr4, both_refused, all_run), kKindOff, "FSR 4 and FSR 3.1 refused: none");
  CheckNum(FitKind(kKindFsr31, both_refused, all_run), kKindOff, "FSR 3.1 refused: none");
  // FSR 4 refused and FSR 3.1 cannot run on this PC: none.
  const auto no_fsr31 = [](uint8_t k) { return k != kKindFsr31; };
  CheckNum(FitKind(kKindFsr4, fsr4_refused, no_fsr31), kKindOff, "FSR 3.1 cannot run: none");
  // FSR 4 cannot run at all (not offered) and is not refused: the caller never asks for it, but the
  // chain still ends sensibly.
  const auto no_fsr4 = [](uint8_t k) { return k != kKindFsr4; };
  CheckNum(FitKind(kKindFsr4, none_refused, no_fsr4), kKindFsr31, "FSR 4 cannot run: FSR 3.1");
  // DLSS is never lowered to an FSR: a refusal of it (never made) would end at none.
  const auto dlss_refused = [](uint8_t k) { return k == kKindDlss; };
  CheckNum(FitKind(kKindDlss, dlss_refused, all_run), kKindOff, "DLSS refused: none");
  CheckNum(LowerKind(kKindFsr4), kKindFsr31, "lower of FSR 4");
  CheckNum(LowerKind(kKindFsr31), kKindOff, "lower of FSR 3.1");
  CheckNum(LowerKind(kKindOff), kKindOff, "lower of none");
  CheckNum(LowerKind(kKindDlss), kKindOff, "lower of DLSS");
}

}  // namespace

int main() {
  TestHasRoom();
  TestNeed();
  TestWaits();
  TestBook();
  TestFit();
  std::printf("vram_room_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
