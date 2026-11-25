#include "capture_visual.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>

#include "../../gpu/gbuffer_types.h"

namespace capture = mc::cinematic::capture;
namespace vis = capture::visual;
namespace fs = std::filesystem;
using SurfaceClass = mcla::gpu::gbuffer::SurfaceClass;
using uint = uint32_t;
#define GB_CLASS_NONE uint(SurfaceClass::kNone)
#define GB_CLASS_ROAD uint(SurfaceClass::kRoad)
#define GB_CLASS_WET_ROAD uint(SurfaceClass::kWetRoad)
#define GB_CLASS_FOLIAGE uint(SurfaceClass::kFoliage)
#define GB_CLASS_INTERIOR uint(SurfaceClass::kInterior)
#define GB_CLASS_GLASS uint(SurfaceClass::kGlass)
#define GB_CLASS_PAINT uint(SurfaceClass::kPaint)
#define GB_CLASS_CHROME uint(SurfaceClass::kChrome)
#define GB_CLASS_METAL uint(SurfaceClass::kMetal)
#include "../../gpu/hlsl/visual_class.hlsli"

namespace {
int checks, failed;

void Check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        ++failed;
        if (failed <= 12) std::cerr << "FAIL " << label << '\n';
    }
}

std::string Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

void Options() {
    uint32_t mask = 0;
    Check(vis::ParseBuffers("", mask) && !mask, "default disabled");
    Check(vis::ParseBuffers("off", mask) && !mask, "explicit off");
    Check(vis::ParseBuffers("all", mask) && mask == 127, "all streams");
    Check(vis::ParseBuffers("class,hdr", mask) && mask == (vis::Bit(vis::kClass) | vis::Bit(vis::kHdr)), "named subset");
    for (auto bad : {"motion,motion", "final,", ",class", "unknown"}) {
        Check(!vis::ParseBuffers(bad, mask), "invalid list");
    }
    for (uint64_t i = 0; i < 12; ++i) Check(vis::Selected(i, 3, 4) == (i == 3 || i == 7 || i == 11), "take-relative cadence");
    Check(!vis::Selected(3, 3, 0), "zero cadence guard");
    capture::LumaCrop crop;
    Check(vis::ParseRects("5,3,10,12;1,10,5,14", crop) &&
          crop.x == 1 && crop.y == 3 && crop.width == 14 && crop.height == 21, "rectangle union bounds");
    Check(!vis::ParseRects("1,2,3,4;", crop) && !vis::ParseRects("4294967295,0,1,1", crop), "invalid rectangles");
    crop = vis::ScaleCrop({1, 3, 4, 2}, 10, 10, 5, 5);
    Check(crop.x == 0 && crop.y == 1 && crop.width == 3 && crop.height == 2, "scale uses outward rounding");
    Check(!vis::ScaleCrop({1, 2, 4, 9}, 10, 10, 5, 5).width, "outside crop fails");
    crop = vis::ScaleCrop({}, 1280, 720, 640, 360);
    Check(crop.width == 640 && crop.height == 360, "native full-frame size");
}

void Classes() {
    constexpr std::array<uint8_t, 16> world{255, 255, 1, 1, 255, 255, 255, 6, 2, 255, 255, 255, 255, 255, 5, 255};
    constexpr std::array<uint8_t, 16> vehicle{255, 255, 1, 1, 255, 255, 255, 6, 2, 2, 2, 3, 255, 255, 4, 255};
    for (uint32_t material = 0; material < 256; ++material) {
        const auto& lut = (material & 128) ? vehicle : world;
        const uint32_t cls = material % 32;
        const uint32_t expected = cls < lut.size() ? lut[cls] : 255;
        Check(VisualClass(material, false, false) == expected, "material classification");
        Check(VisualClass(material, true, false) == (cls == 0 ? 0 : expected), "sky only for clear material");
        Check(VisualClass(material, true, true) == 7, "UI overrides scene class");
    }
}

void Writers(const fs::path& take) {
    vis::Writer writer;
    writer.Reset(take);
    vis::Frame frame{};
    uint64_t end = 0;
    for (size_t i = 0; i < vis::kCount; ++i) {
        frame.planes[i] = vis::Place({2, 1, 2, 2}, 8, 4, vis::kStreams[i].max_source_bytes,
                                    i == vis::kGi ? 2 : 0, i == vis::kFinal, end);
    }
    Check(frame.planes[vis::kHdr].offset == 512 && end == 3584, "D3D12 placement alignment");
    uint64_t odd_end = 1;
    const auto odd = vis::Place({0, 0, 257, 1}, 257, 1, 1, 0, false, odd_end);
    Check(odd.offset == 512 && odd.row_pitch == 512 && odd_end == 1024, "unaligned footprint");
    std::vector<uint8_t> bytes(end, 0xee);
    for (const auto& plane : frame.planes) {
        for (uint32_t y = 0; y < 2; ++y) {
            for (uint32_t x = 0; x < 2; ++x) {
                auto* pixel = bytes.data() + plane.offset + size_t(y) * plane.row_pitch + x * plane.pixel_bytes;
                for (uint32_t n = 0; n < plane.pixel_bytes; ++n) pixel[n] = uint8_t(1 + y * 32 + x * 8 + n);
            }
        }
    }
    std::string why;
    Check(writer.Write(frame, bytes, why), "write seven streams");
    frame.index = 4;
    frame.planes[vis::kFinal].swap_rb = false;
    frame.planes[vis::kRefl] = {};
    Check(writer.Write(frame, bytes, why) && writer.Finish(why), "publish sparse metadata");
    const auto root = take / "buffers";
    Check(Read(root / "final/frame_0.bin") == std::string("\3\2\1\13\12\11\43\42\41\53\52\51", 12), "BGRA to RGB, row padding stripped");
    Check(Read(root / "final/frame_4.bin") == std::string("\1\2\3\11\12\13\41\42\43\51\52\53", 12), "RGBA to RGB");
    Check(Read(root / "hdr/frame_0.bin").substr(0, 12) == std::string("\1\2\3\4\5\6\11\12\13\14\15\16", 12), "HDR half byte order and channels");
    Check(Read(root / "ao/frame_0.bin") == std::string("\1\2\11\12\41\42\51\52", 8), "AO first half channel");
    Check(Read(root / "gi/frame_0.bin").substr(0, 6) == std::string("\3\4\5\6\7\10", 6), "GI extracts yzw");
    Check(Read(root / "class/frame_0.bin") == std::string("\1\11\41\51", 4), "packed class bytes");
    Check(Read(root / "motion/frame_0.bin").substr(0, 8) == std::string("\1\2\3\4\11\12\13\14", 8), "packed two-channel motion");
    Check(Read(root / "motion/meta.json") == "{\"w\":2,\"h\":2,\"channels\":2,\"encoding\":\"f16le\",\"space\":\"motion-px\",\"frames\":[0,4]}\n", "exact motion metadata");
    Check(Read(root / "refl/meta.json") == "{\"w\":2,\"h\":2,\"channels\":3,\"encoding\":\"f16le\",\"space\":\"linear-scene\",\"frames\":[0]}\n" &&
          !fs::exists(root / "refl/frame_4.bin"), "missing frame stays absent");
    Check(!writer.Write(frame, bytes, why), "duplicate frame index refused");
    frame.index++;
    frame.planes[vis::kFinal].alternate = true;
    Check(!writer.Write(frame, bytes, why), "changing the signal source is refused");
    frame.planes[vis::kFinal].alternate = false;
    frame.planes[vis::kFinal].crop.x++;
    Check(!writer.Write(frame, bytes, why), "moving crop refused");
    writer.Reset(take / "truncated");
    Check(!writer.Write(frame, std::span(bytes).first(255), why), "truncated readback refused");
    writer.Reset(take / "empty");
    Check(writer.Write({}, {}, why) && writer.Finish(why) && !fs::exists(take / "empty"), "absent streams produce no files");
}
}  // namespace

int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto take = fs::temp_directory_path() / ("larecomp-visual-unit-" + std::to_string(stamp));
    Options();
    Classes();
    Writers(take);
    std::error_code ec;
    fs::remove_all(take, ec);
    std::cout << "capture_visual_test: " << checks << " checks, " << failed << " failed\n";
    return failed != 0;
}
