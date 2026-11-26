// Check of what a colour clear stores in a render target of the host (color_clear_rules.h, EncodeColorClear in edram.cpp), at the edge of a 7e3 target's
// range and beyond it. A program of its own: no renderer, no ReXGlue, no GPU.
//
//   cmake --build <build dir> --target larecomp_color_clear_rules_test      (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_color_clear_rules_test [<src/gpu dir>]
// or by hand: unit-test.ps1 -Test <this file>
//
// The console's 7e3 target (k_2_10_10_10_FLOAT) stores the colour times 2^exp_bias in 0 to 31.875 for red, green and blue and nothing else: no NaN, no
// infinity, no negative number (Xenia UnclampedFloat32To7e3). The host's R16G16B16A16_FLOAT holds all of those, and a NaN left in it ruins every blend after it
// (ROAD-VIEW-OVERFLOW), so the clear says what the console stores. With the folder of src/gpu as the argument the program also reads edram.cpp and checks that
// EncodeColorClear takes its numbers from the header and keeps no rules of its own.

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

#include "color_clear_rules.h"

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

using mcla::gpu::color_clear::EncodeComponent;
using mcla::gpu::color_clear::kMax7e3;

constexpr float kInf = std::numeric_limits<float>::infinity();
const float kNan = std::numeric_limits<float>::quiet_NaN();
const float kSNan = std::numeric_limits<float>::signaling_NaN();
const float kNegNan = std::bit_cast<float>(0xFFC00001u);

bool SameBits(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }

void Check7e3() {
  for (const uint32_t format : {3u, 12u}) {  // k_2_10_10_10_FLOAT and its _AS_16_16_16_16 variant
    const std::string f = "format " + std::to_string(format) + ": ";
    for (uint32_t c = 0; c < 3; ++c) {
      const std::string ch = f + "channel " + "RGB"[c] + ": ";
      // the game's bias is 4: a colour of 1.9921875 is 31.875 stored
      Check(EncodeComponent(format, 4, 1.0f, c) == 16.0f, ch + "1.0 at bias 4 is 16");
      Check(EncodeComponent(format, 4, 1.9921875f, c) == kMax7e3, ch + "1.9921875 at bias 4 is exactly the range's end");
      Check(EncodeComponent(format, 4, 2.0f, c) == kMax7e3, ch + "2.0 at bias 4 (32 stored) is clamped to 31.875");
      Check(EncodeComponent(format, 4, 1000.0f, c) == kMax7e3, ch + "a colour far past the range is clamped");
      Check(EncodeComponent(format, 0, 31.875f, c) == kMax7e3, ch + "31.875 at bias 0 is kept");
      Check(EncodeComponent(format, 0, std::nextafter(31.875f, 100.0f), c) == kMax7e3, ch + "the float above 31.875 is clamped");
      Check(EncodeComponent(format, 0, std::nextafter(31.875f, 0.0f), c) < kMax7e3, ch + "the float below 31.875 is kept below it");
      Check(EncodeComponent(format, 0, kInf, c) == kMax7e3, ch + "+inf is the largest");
      Check(EncodeComponent(format, 0, -kInf, c) == 0.0f, ch + "-inf is 0");
      Check(SameBits(EncodeComponent(format, 0, kNan, c), 0.0f), ch + "a NaN is +0");
      Check(SameBits(EncodeComponent(format, 0, kSNan, c), 0.0f), ch + "a signalling NaN is +0");
      Check(SameBits(EncodeComponent(format, 0, kNegNan, c), 0.0f), ch + "a negative NaN is +0");
      Check(EncodeComponent(format, 0, -1.0f, c) == 0.0f, ch + "-1 is 0");
      Check(EncodeComponent(format, 0, -0.0f, c) == 0.0f, ch + "-0 is 0");
      Check(EncodeComponent(format, 4, -0.5f, c) == 0.0f, ch + "a negative colour at bias 4 is 0");
      Check(EncodeComponent(format, 0, 0.0f, c) == 0.0f, ch + "0 is 0");
      // a bias that takes a finite colour past float's range: the product is an infinity, and the clamp has it
      Check(EncodeComponent(format, 100, 1.0e10f, c) == kMax7e3, ch + "a colour times 2^100 that overflows to infinity is the largest");
      Check(EncodeComponent(format, -4, 1020.0f, c) == kMax7e3, ch + "a negative bias scales down first, then clamps (63.75 stored at bias -4 from 1020)");
      Check(EncodeComponent(format, -4, 16.0f, c) == 1.0f, ch + "a negative bias scales down: 16 at bias -4 is 1");
    }
    // alpha: not clamped to the range; a NaN is 0; an infinity stays finite
    Check(EncodeComponent(format, 0, 100.0f, 3) == 100.0f, f + "alpha 100 is kept (alpha is 2 bits on the console; the host keeps what the draws that blend with it expect)");
    Check(SameBits(EncodeComponent(format, 0, kNan, 3), 0.0f), f + "a NaN alpha is 0");
    Check(EncodeComponent(format, 0, -2.0f, 3) == 0.0f, f + "a negative alpha is 0");
    Check(std::isfinite(EncodeComponent(format, 0, kInf, 3)), f + "an infinite alpha is made finite");
  }
}

void CheckOtherFormats() {
  // k_8_8_8_8 (0) and the formats not named: the colour times 2^bias, nothing else
  Check(EncodeComponent(0, 0, 5.0f, 0) == 5.0f, "k_8_8_8_8: 5 is kept (the 8 bit target clamps in the pipeline)");
  Check(EncodeComponent(0, 2, 1.5f, 1) == 6.0f, "k_8_8_8_8: times 2^2");
  Check(std::isnan(EncodeComponent(0, 0, kNan, 0)), "k_8_8_8_8: a NaN is not made into anything here");
  Check(EncodeComponent(7, 0, -3.0f, 0) == -3.0f, "k_16_16_16_16_FLOAT: -3 is kept (its floor is -65504)");
  Check(EncodeComponent(7, 0, 100000.0f, 0) == 100000.0f, "k_16_16_16_16_FLOAT: not clamped here");
  Check(EncodeComponent(14, 0, 1.0e20f, 0) == 1.0e20f, "k_32_FLOAT: not clamped");
  // k_8_8_8_8_GAMMA: 0 to 1, kept linear
  Check(EncodeComponent(1, 0, 2.0f, 0) == 1.0f, "k_8_8_8_8_GAMMA: 2 is 1");
  Check(EncodeComponent(1, 0, -2.0f, 0) == 0.0f, "k_8_8_8_8_GAMMA: -2 is 0");
  Check(EncodeComponent(1, 0, 0.25f, 2) == 0.25f, "k_8_8_8_8_GAMMA: 0.25 is kept");
  Check(EncodeComponent(1, 1, 0.75f, 2) == 1.0f, "k_8_8_8_8_GAMMA: 0.75 at bias 1 is 1");
  // the SNORM 16-bit formats: +-1 on the host is +-32
  Check(EncodeComponent(4, 5, 1.0f, 0) == 1.0f, "k_16_16: 1 at bias 5 is +1 (SNORM)");
  Check(EncodeComponent(5, 0, -32.0f, 0) == -1.0f, "k_16_16_16_16: -32 is -1");
}

std::string ReadAll(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::ostringstream s;
  s << f.rdbuf();
  return s.str();
}

void CheckSources(const std::string& dir) {
  const std::string edram = ReadAll(dir + "/edram.cpp");
  Check(!edram.empty(), "edram.cpp is readable");
  const size_t at = edram.find("void EncodeColorClear(");
  Check(at != std::string::npos, "edram.cpp has EncodeColorClear");
  if (at == std::string::npos) {
    return;
  }
  const size_t end = edram.find("\n}\n", at);
  const std::string body = edram.substr(at, end - at);
  Check(body.find("color_clear::EncodeComponent") != std::string::npos, "EncodeColorClear takes its numbers from color_clear_rules.h");
  Check(body.find("std::clamp") == std::string::npos && body.find("ldexp") == std::string::npos, "EncodeColorClear keeps no rule of its own");
  Check(edram.find("#include \"color_clear_rules.h\"") != std::string::npos, "edram.cpp includes color_clear_rules.h");
}

}  // namespace

int main(int argc, char** argv) {
  Check7e3();
  CheckOtherFormats();
  if (argc > 1) {
    CheckSources(argv[1]);
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
