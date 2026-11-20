#pragma once
//
// A PNG writer for the Director's stills (`shot`): the engine's frame as it was read back (B8G8R8A8 or R8G8B8A8, rows packed) as an 8-bit RGB picture. Pure:
// no game, no library. The pixels are not compressed (zlib "stored" blocks): a 1280 x 720 still is 2.6 MB, and the writer needs no ffmpeg and takes about
// a millisecond a megabyte. Every PNG reader opens it.
//

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mc::cinematic::director {

// The whole file for a picture of `width` x `height` pixels, 4 bytes each (`bgra`: the first byte is blue, else red; the alpha byte is dropped), rows
// `stride` bytes apart (0 = width * 4). False when the size is zero or absurd (more than 16384 on a side).
bool EncodePng(const uint8_t* pixels, uint32_t width, uint32_t height, size_t stride, bool bgra, std::vector<uint8_t>* out);

// The same, written to `path` through a temporary name and a rename, so a reader never sees half a file. `error` says why not.
bool WritePngFile(const std::string& path_utf8, const uint8_t* pixels, uint32_t width, uint32_t height, size_t stride, bool bgra, std::string* error);

// For the tests: reads back what EncodePng writes (stored blocks only) into packed RGB rows. False when it is not such a PNG.
bool DecodeStoredPng(const std::vector<uint8_t>& png, uint32_t* width, uint32_t* height, std::vector<uint8_t>* rgb);

uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0);
uint32_t Adler32(const uint8_t* data, size_t size, uint32_t adler = 1);

}  // namespace mc::cinematic::director
