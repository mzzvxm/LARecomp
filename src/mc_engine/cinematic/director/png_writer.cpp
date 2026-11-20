// A PNG writer for the Director's stills: see png_writer.h.

#include "png_writer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace mc::cinematic::director {

namespace {

const uint32_t* CrcTable() {
    static uint32_t table[256];
    static bool made = [] {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        return true;
    }();
    (void)made;
    return table;
}

void PutBe32(std::vector<uint8_t>* v, uint32_t x) {
    v->push_back(static_cast<uint8_t>(x >> 24));
    v->push_back(static_cast<uint8_t>(x >> 16));
    v->push_back(static_cast<uint8_t>(x >> 8));
    v->push_back(static_cast<uint8_t>(x));
}

void Chunk(std::vector<uint8_t>* out, const char type[4], const uint8_t* data, size_t size) {
    PutBe32(out, static_cast<uint32_t>(size));
    const size_t begin = out->size();
    out->insert(out->end(), type, type + 4);
    if (size) out->insert(out->end(), data, data + size);
    PutBe32(out, Crc32(out->data() + begin, out->size() - begin));
}

}  // namespace

uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc) {
    const uint32_t* t = CrcTable();
    uint32_t c = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) c = t[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint32_t Adler32(const uint8_t* data, size_t size, uint32_t adler) {
    uint32_t a = adler & 0xFFFF, b = adler >> 16;
    constexpr uint32_t kMod = 65521;
    size_t i = 0;
    while (i < size) {
        const size_t n = std::min<size_t>(size - i, 5552);  // the most that cannot overflow 32 bits
        for (size_t k = 0; k < n; ++k) {
            a += data[i + k];
            b += a;
        }
        a %= kMod;
        b %= kMod;
        i += n;
    }
    return (b << 16) | a;
}

bool EncodePng(const uint8_t* pixels, uint32_t width, uint32_t height, size_t stride, bool bgra, std::vector<uint8_t>* out) {
    if (!pixels || width == 0 || height == 0 || width > 16384 || height > 16384) return false;
    if (stride == 0) stride = static_cast<size_t>(width) * 4;
    if (stride < static_cast<size_t>(width) * 4) return false;
    const size_t row_bytes = 1 + static_cast<size_t>(width) * 3;  // the filter byte and the RGB pixels
    const size_t raw_size = row_bytes * height;
    out->clear();
    out->reserve(raw_size + raw_size / 65535 * 5 + 256);
    static const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    out->insert(out->end(), kSignature, kSignature + 8);
    uint8_t ihdr[13];
    ihdr[0] = static_cast<uint8_t>(width >> 24), ihdr[1] = static_cast<uint8_t>(width >> 16), ihdr[2] = static_cast<uint8_t>(width >> 8), ihdr[3] = static_cast<uint8_t>(width);
    ihdr[4] = static_cast<uint8_t>(height >> 24), ihdr[5] = static_cast<uint8_t>(height >> 16), ihdr[6] = static_cast<uint8_t>(height >> 8), ihdr[7] = static_cast<uint8_t>(height);
    ihdr[8] = 8;   // bit depth
    ihdr[9] = 2;   // colour type: RGB
    ihdr[10] = 0;  // deflate
    ihdr[11] = 0;  // filter method 0
    ihdr[12] = 0;  // no interlace
    Chunk(out, "IHDR", ihdr, sizeof(ihdr));

    // The zlib stream: 0x78 0x01, stored blocks of up to 65535 bytes, the Adler-32 of the raw data. Built in a buffer of its own (the chunk's CRC needs it whole).
    std::vector<uint8_t> z;
    z.reserve(raw_size + raw_size / 65535 * 5 + 16);
    z.push_back(0x78);
    z.push_back(0x01);
    std::vector<uint8_t> raw(raw_size);
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* dst = raw.data() + static_cast<size_t>(y) * row_bytes;
        const uint8_t* src = pixels + static_cast<size_t>(y) * stride;
        *dst++ = 0;  // filter: none
        for (uint32_t x = 0; x < width; ++x, src += 4, dst += 3) {
            if (bgra) {
                dst[0] = src[2], dst[1] = src[1], dst[2] = src[0];
            } else {
                dst[0] = src[0], dst[1] = src[1], dst[2] = src[2];
            }
        }
    }
    size_t at = 0;
    while (at < raw_size) {
        const size_t n = std::min<size_t>(raw_size - at, 65535);
        const bool last = at + n == raw_size;
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n & 0xFF));
        z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n & 0xFF));
        z.push_back(static_cast<uint8_t>((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
    }
    PutBe32(&z, Adler32(raw.data(), raw.size()));
    Chunk(out, "IDAT", z.data(), z.size());
    Chunk(out, "IEND", nullptr, 0);
    return true;
}

bool WritePngFile(const std::string& path_utf8, const uint8_t* pixels, uint32_t width, uint32_t height, size_t stride, bool bgra, std::string* error) {
    std::vector<uint8_t> png;
    if (!EncodePng(pixels, width, height, stride, bgra, &png)) {
        if (error) *error = "the picture has no usable size (" + std::to_string(width) + "x" + std::to_string(height) + ")";
        return false;
    }
    const std::filesystem::path final_path(std::u8string(path_utf8.begin(), path_utf8.end()));
    std::error_code ec;
    if (final_path.has_parent_path()) std::filesystem::create_directories(final_path.parent_path(), ec);
    std::filesystem::path tmp = final_path;
    tmp += ".tmp";
    bool wrote = false;
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            if (error) *error = "cannot write " + path_utf8;
            return false;
        }
        f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        f.flush();
        wrote = static_cast<bool>(f);
    }
    if (!wrote) {
        std::filesystem::remove(tmp, ec);
        if (error) *error = "the disk did not take " + path_utf8;
        return false;
    }
    std::filesystem::rename(tmp, final_path, ec);
    if (ec) {
        // rename over an existing file can fail on some systems: replace it
        std::filesystem::remove(final_path, ec);
        std::error_code ec2;
        std::filesystem::rename(tmp, final_path, ec2);
        if (ec2) {
            std::filesystem::remove(tmp, ec);
            if (error) *error = "cannot replace " + path_utf8 + ": " + ec2.message();
            return false;
        }
    }
    return true;
}

bool DecodeStoredPng(const std::vector<uint8_t>& png, uint32_t* width, uint32_t* height, std::vector<uint8_t>* rgb) {
    static const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (png.size() < 8 || std::memcmp(png.data(), kSignature, 8) != 0) return false;
    size_t at = 8;
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> z;
    bool got_end = false;
    const auto be32 = [&](size_t i) { return (uint32_t(png[i]) << 24) | (uint32_t(png[i + 1]) << 16) | (uint32_t(png[i + 2]) << 8) | uint32_t(png[i + 3]); };
    while (at + 12 <= png.size()) {
        const uint32_t len = be32(at);
        if (at + 12 + len > png.size()) return false;
        const char* type = reinterpret_cast<const char*>(&png[at + 4]);
        if (be32(at + 8 + len) != Crc32(&png[at + 4], 4 + len)) return false;
        if (std::memcmp(type, "IHDR", 4) == 0) {
            if (len != 13 || png[at + 16] != 8 || png[at + 17] != 2) return false;
            w = be32(at + 8);
            h = be32(at + 12);
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            z.insert(z.end(), png.begin() + static_cast<std::ptrdiff_t>(at + 8), png.begin() + static_cast<std::ptrdiff_t>(at + 8 + len));
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            got_end = true;
            break;
        }
        at += 12 + len;
    }
    if (!got_end || w == 0 || h == 0 || z.size() < 6 || z[0] != 0x78) return false;
    std::vector<uint8_t> raw;
    size_t i = 2;
    while (true) {
        if (i + 5 > z.size()) return false;
        const bool last = (z[i] & 1) != 0;
        if ((z[i] >> 1) != 0) return false;  // not a stored block
        const size_t n = z[i + 1] | (size_t(z[i + 2]) << 8);
        const size_t nn = z[i + 3] | (size_t(z[i + 4]) << 8);
        if ((n ^ 0xFFFF) != nn) return false;
        i += 5;
        if (i + n > z.size()) return false;
        raw.insert(raw.end(), z.begin() + static_cast<std::ptrdiff_t>(i), z.begin() + static_cast<std::ptrdiff_t>(i + n));
        i += n;
        if (last) break;
    }
    if (i + 4 != z.size()) return false;
    const uint32_t adler = (uint32_t(z[i]) << 24) | (uint32_t(z[i + 1]) << 16) | (uint32_t(z[i + 2]) << 8) | uint32_t(z[i + 3]);
    if (adler != Adler32(raw.data(), raw.size())) return false;
    const size_t row = 1 + static_cast<size_t>(w) * 3;
    if (raw.size() != row * h) return false;
    rgb->clear();
    for (uint32_t y = 0; y < h; ++y) {
        if (raw[y * row] != 0) return false;
        rgb->insert(rgb->end(), raw.begin() + static_cast<std::ptrdiff_t>(y * row + 1), raw.begin() + static_cast<std::ptrdiff_t>((y + 1) * row));
    }
    *width = w;
    *height = h;
    return true;
}

}  // namespace mc::cinematic::director
