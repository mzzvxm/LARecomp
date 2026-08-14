// Minimal XCompress LZX writer: stored blocks only.
//
// The game streams every resource through XMemDecompressStream, and the
// uncompressed variant of that path is dead code in the shipped build -- no
// file in any archive uses it. So instead of relying on it, mod resources are
// emitted as a real LZX stream that happens to compress nothing.
//
// The framing was recovered from the shipped archives and confirmed against
// xcompress32.dll, which round-trips a 3.4 MB drawable byte for byte:
//
//   stream  = chunk*
//   chunk   = u16 big endian byte count, then that many bytes of LZX bitstream
//   chunk output = 32768 bytes (the last one holds the remainder)
//
// Inside a chunk the bitstream is 16-bit little-endian words consumed MSB
// first. The very first chunk of a stream -- and only that one -- opens with
// the 1-bit Intel E8 translation flag, cleared. Then per block:
//
//   3 bits  block type, 3 = stored
//   24 bits block size
//   pad to the next 16-bit boundary
//   12 bytes R0/R1/R2, little endian
//   the raw bytes, padded to an even length
//
// Repeating the E8 flag on later chunks makes the decoder reject the stream,
// which is the one thing that is easy to get wrong here.
#pragma once

#include <cstdint>
#include <vector>

namespace mc::modloader {

std::vector<uint8_t> LzxEncodeStored(const uint8_t* data, size_t size);

}  // namespace mc::modloader
