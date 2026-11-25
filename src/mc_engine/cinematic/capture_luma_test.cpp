#include "capture_luma.h"

#include <array>
#include <cstdlib>
#include <iostream>

using namespace mc::cinematic::capture;

namespace {
void Check(bool passed, const char* what) {
    if (passed) return;
    std::cerr << what << '\n';
    std::exit(1);
}

template <bool Bgra>
void TestPredict() {
    std::array<uint8_t, 4 * 3 * 3> frame{};
    auto put = [&](size_t pixel, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        frame[pixel * 4] = Bgra ? b : r;
        frame[pixel * 4 + 1] = g;
        frame[pixel * 4 + 2] = Bgra ? r : b;
        frame[pixel * 4 + 3] = a;
    };
    put(4, 255, 0, 0, 123);
    put(5, 0, 0, 255, 42);
    put(7, 0, 255, 0, 0);
    put(8, 0, 0, 1, 255);
    const LumaCrop crop{1, 1, 2, 2};
    std::array<uint8_t, 4> delta{}, previous{1, 1, 1, 1};
    PredictLuma<Bgra, true>(frame.data(), 3, crop, previous.data(), delta.data());
    Check(previous == std::array<uint8_t, 4>{76, 29, 150, 0}, "cropped luma and channel order");
    Check(delta == std::array<uint8_t, 4>{76, 209, 150, 106}, "wrapped left prediction resets each row");
    put(4, 0, 0, 0, 0);
    put(7, 255, 255, 255, 0);
    put(8, 0, 1, 0, 0);
    PredictLuma<Bgra, false>(frame.data(), 3, crop, previous.data(), delta.data());
    Check(previous == std::array<uint8_t, 4>{0, 29, 255, 1}, "temporal predictor updates the reference plane");
    Check(delta == std::array<uint8_t, 4>{180, 0, 105, 1}, "wrapped temporal prediction");
}
}  // namespace

int main() {
    LumaCrop crop;
    Check(ParseLumaCrop("12,23,40,60", crop), "parse crop");
    Check(crop.x == 12 && crop.y == 23 && crop.width == 40 && crop.height == 60 && crop.Pixels() == 2400, "crop coordinates");
    Check(crop.Fits(52, 83) && !crop.Fits(51, 83) && !crop.Fits(52, 82)
          && !crop.Fits(1, 83) && !crop.Fits(52, 1), "crop fit and overflow");
    for (auto invalid : {"", "0,0,0,1", "0,0,1,0", "0,0,1,", "0,0,1", "0,0,1,1,", "0,0,1,1,1",
                         "0,0,1,1a", "-1,0,1,1", "4294967296,0,1,1", "full "}) {
        Check(!ParseLumaCrop(invalid, crop), "bad crop rejected");
    }
    Check(ParseLumaCrop("full", crop) && !crop.width && !crop.height, "full crop sentinel");
    Check(LumaWord(0x12345678) == std::array<uint8_t, 4>{0x78, 0x56, 0x34, 0x12}, "little-endian words");
    for (uint64_t frame = 0; frame < 250; ++frame) {
        Check(LumaPreview(frame) == (frame == 0 || frame == 120 || frame == 239), "preview spacing");
    }
    Check(CaptureLuma(0, 0, 250) == 28 && CaptureLuma(0, 0, 255) == 29, "Pillow blue coefficient");
    Check(CaptureLuma(0, 1, 0) == 1, "round to nearest luma");
    const TakeSceneClock clock{8 + 1.0 / 60, 1.0 / 60};
    Check(!LumaLastFrame(clock, 90, 100, -1.0, 12.0), "ignore a tag before this take");
    Check(!LumaLastFrame(clock, 338, 100, -1.0, 12.0), "keep the penultimate frame pipelined");
    Check(LumaLastFrame(clock, 339, 100, -1.0, 12.0), "wait for a scene's final readback");
    Check(LumaLastFrame(clock, 338, 100, 12.0, -1.0), "exclusive Director range");
    Check(!LumaLastFrame(clock, 339, 100, -1.0, -1.0), "open take has no authored end");
    TestPredict<false>();
    TestPredict<true>();
    std::cout << "Capture luma unit passed\n";
}
