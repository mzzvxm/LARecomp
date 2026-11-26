#include "capture_visual.h"

#include <algorithm>
#include <bit>
#include <fstream>

namespace mc::cinematic::capture::visual {

static_assert(std::endian::native == std::endian::little);

bool ParseBuffers(std::string_view text, uint32_t& mask) {
    mask = 0;
    if (text.empty() || text == "off") return true;
    if (text == "all") {
        mask = (1u << kCount) - 1;
        return true;
    }
    for (;;) {
        const size_t comma = text.find(',');
        const auto name = text.substr(0, comma);
        const auto it = std::find_if(kStreams.begin(), kStreams.end(),
                                    [name](const Stream& stream) { return name == stream.name; });
        if (it == kStreams.end()) return false;
        const uint32_t bit = 1u << (it - kStreams.begin());
        if (mask & bit) return false;
        mask |= bit;
        if (comma == text.npos) return true;
        text.remove_prefix(comma + 1);
    }
}

LumaCrop ScaleCrop(LumaCrop crop, uint32_t from_w, uint32_t from_h,
                   uint32_t to_w, uint32_t to_h) {
    if (!crop.width) return {0, 0, to_w, to_h};
    if (!crop.Fits(from_w, from_h)) return {};
    const uint32_t x = uint64_t(crop.x) * to_w / from_w;
    const uint32_t y = uint64_t(crop.y) * to_h / from_h;
    const uint32_t right = (uint64_t(crop.x + crop.width) * to_w + from_w - 1) / from_w;
    const uint32_t bottom = (uint64_t(crop.y + crop.height) * to_h + from_h - 1) / from_h;
    return {x, y, right - x, bottom - y};
}

Plane Place(LumaCrop crop, uint32_t width, uint32_t height, uint32_t pixel_bytes,
            uint32_t channel_offset, bool swap_rb, uint64_t& end) {
    const uint32_t pitch = Align(uint64_t(crop.width) * pixel_bytes, 256);
    const uint64_t offset = Align(end, 512);
    end = offset + uint64_t(pitch) * crop.height;
    return {crop, width, height, pitch, pixel_bytes, channel_offset, offset, swap_rb};
}

void Writer::Reset(const std::filesystem::path& take) {
    root_ = take / "buffers";
    streams_ = {};
    row_ = {};
}

bool Writer::Write(const Frame& frame, std::span<const uint8_t> data, std::string& why) {
    for (size_t i = 0; i < kCount; ++i) {
        const Plane& plane = frame.planes[i];
        if (!plane.crop.width) continue;
        const Stream& stream = kStreams[i];
        Written& written = streams_[i];
        const auto folder = root_ / stream.name;
        const uint32_t output_bytes = stream.channels * stream.channel_bytes;
        const uint64_t bytes = uint64_t(plane.row_pitch) * plane.crop.height;
        if (!plane.crop.Fits(plane.width, plane.height) ||
            uint64_t(plane.channel_offset) + output_bytes > plane.pixel_bytes ||
            (plane.swap_rb && stream.channels != 3) ||
            uint64_t(plane.crop.width) * plane.pixel_bytes > plane.row_pitch ||
            plane.offset > data.size() || bytes > data.size() - plane.offset) {
            why = std::string(stream.name) + " has an invalid readback layout";
            return false;
        }
        if (written.frames.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(folder, ec);
            if (ec) {
                why = std::string("cannot create buffers/") + stream.name + ": " + ec.message();
                return false;
            }
            written.plane = plane;
        } else {
            const auto& old = written.plane;
            if (frame.index <= written.frames.back() || plane.width != old.width ||
                plane.height != old.height || plane.crop.x != old.crop.x || plane.crop.y != old.crop.y ||
                plane.crop.width != old.crop.width || plane.crop.height != old.crop.height ||
                plane.alternate != old.alternate) {
                why = std::string(stream.name) + " changed layout or source, or had an out-of-order index";
                return false;
            }
        }
        const auto path = folder / ("frame_" + std::to_string(frame.index) + ".bin");
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        const size_t row_bytes = size_t(plane.crop.width) * output_bytes;
        for (uint32_t y = 0; y < plane.crop.height && file; ++y) {
            const uint8_t* src = data.data() + plane.offset + size_t(y) * plane.row_pitch;
            if (!plane.swap_rb && output_bytes == plane.pixel_bytes) {
                file.write(reinterpret_cast<const char*>(src), row_bytes);
                continue;
            }
            row_.resize(row_bytes);
            for (uint32_t x = 0; x < plane.crop.width; ++x) {
                const auto* pixel = src + size_t(x) * plane.pixel_bytes + plane.channel_offset;
                auto* dst = row_.data() + size_t(x) * output_bytes;
                if (plane.swap_rb) {
                    const uint32_t n = stream.channel_bytes;
                    std::copy_n(pixel + 2 * n, n, dst);
                    std::copy_n(pixel + n, n, dst + n);
                    std::copy_n(pixel, n, dst + 2 * n);
                } else {
                    std::copy_n(pixel, output_bytes, dst);
                }
            }
            file.write(reinterpret_cast<const char*>(row_.data()), row_bytes);
        }
        file.close();
        if (!file) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
            why = std::string("cannot write buffers/") + stream.name + "/" + path.filename().string();
            return false;
        }
        written.frames.push_back(frame.index);
    }
    return true;
}

bool Writer::Finish(std::string& why) {
    for (size_t i = 0; i < kCount; ++i) {
        const Written& written = streams_[i];
        if (written.frames.empty()) continue;
        const Stream& stream = kStreams[i];
        const auto folder = root_ / stream.name;
        const auto temp = folder / "meta.json.tmp";
        std::ofstream file(temp, std::ios::binary);
        file << "{\"w\":" << written.plane.crop.width << ",\"h\":" << written.plane.crop.height
             << ",\"channels\":" << stream.channels << ",\"encoding\":\"" << stream.encoding
             << "\",\"space\":\"" << stream.space << "\",\"frames\":[";
        for (size_t n = 0; n < written.frames.size(); ++n) {
            if (n) file << ',';
            file << written.frames[n];
        }
        file << ']';
        if (i == kRefl) {
            file << ",\"source\":\"" << (written.plane.alternate ? "car-rt-history" : "ssr-resolve-input") << '"';
        }
        if (i == kReflHit) {
            const auto& plane = written.plane;
            file << ",\"domain\":\"reflection-hit-v1\",\"source\":\""
                 << (plane.alternate ? "native-material" : "surface-trace") << '"'
                 << ",\"source_w\":" << plane.width << ",\"source_h\":" << plane.height;
            if (plane.crop.x || plane.crop.y || plane.crop.width != plane.width || plane.crop.height != plane.height) {
                file << ",\"x\":" << plane.crop.x << ",\"y\":" << plane.crop.y;
            }
            file << ",\"channel_names\":[\"visible\",\"rt_query\"],\"ids\":{"
                    "\"0\":\"none\",\"1\":\"ssr_vehicle\",\"2\":\"ssr_building\",\"3\":\"ssr_road\","
                    "\"4\":\"ssr_sky\",\"5\":\"ssr_emissive\",\"6\":\"ssr_other\","
                    "\"7\":\"road_view\",\"8\":\"environment\","
                    "\"16\":\"rt_city_unclassified\",\"17\":\"rt_city_world\",\"18\":\"rt_city_road\","
                    "\"19\":\"rt_city_wet_road\",\"20\":\"rt_city_sidewalk\",\"21\":\"rt_city_terrain\","
                    "\"22\":\"rt_city_lod\",\"23\":\"rt_city_foliage\",\"24\":\"rt_city_paint\","
                    "\"25\":\"rt_city_chrome\",\"26\":\"rt_city_metal\",\"27\":\"rt_city_glass\","
                    "\"28\":\"rt_city_skin\",\"29\":\"rt_city_emissive\",\"30\":\"rt_city_interior\","
                    "\"31\":\"rt_city_water\",\"32\":\"rt_car\",\"33\":\"rt_miss\",\"34\":\"rt_city_unresolved\","
                    "\"35\":\"rt_car_shaded\",\"36\":\"ssr_reuse\"}";
        }
        file << "}\n";
        file.close();
        std::error_code ec;
        if (file) std::filesystem::rename(temp, folder / "meta.json", ec);
        if (!file || ec) {
            why = std::string("cannot publish buffers/") + stream.name + "/meta.json";
            std::filesystem::remove(temp, ec);
            return false;
        }
    }
    return true;
}

}  // namespace mc::cinematic::capture::visual
