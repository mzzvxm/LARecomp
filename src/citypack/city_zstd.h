#pragma once
// ===========================================================================
// LARecomp city pack -- zstd on disk (CITY-HIT-SIZE, after CITY-RT M2 packages H1 and H2)
// ===========================================================================
// WHY. The city hit pack (hit_builder.h: 174 MB) and the city texture file (texture_set.h: 244 MB at full detail) were 419 MB together, over the owner's 200 to 300 MB. Both are
// game-derived files built on the player's PC and read back piece by piece (a sector's blob, a texture's chain), so each piece is compressed ON ITS OWN with zstd, a frame per piece:
// the readers keep their random access, and the loader decompresses one piece into the memory it was going to read it into anyway. Measured on the retail archive (zstd 1.5.7):
//
//   the hit pack                    174 MB -> 112 MB at level 3, 110 MB at level 6 (63 percent: the corner streams are already tokens and varints, what is left is their statistics)
//   the texture file                244 MB -> 151 MB at level 3 on the plain chains, 143 MB with the BC block planes below, 138 MB at level 6, 130 MB at level 19
//   both                            419 MB -> 255 MB at level 3, 248 MB at level 6 (the owner's limit is 300), 230 MB at level 19
//   the cost                        the frames are made by the build's workers (CPU seconds, whole set: level 3: 2.5, 6: 6.6, 9: 9.2, 12: 16, 19: 86); unpacking is fast: all 8,342 chains
//                                   (file read included) take 0.3 s of one thread, and zstd's share of decoding all 877 blobs is about 0.2 s (a sector's blob: a quarter of a millisecond)
//
// WHAT. Three small things, all pure (no file, no GPU), used by hit_corner_codec.cpp (the hit blob's corner stream), texture_set.cpp (the texture chains) and the tests:
//   * ZstdCompress / ZstdDecompress: one frame with its content size and a checksum; the decoder is told the exact size it must give (the tables say it), never trusts the frame's own.
//   * BcSplitPlanes / BcMergePlanes: the BC blocks of one mip level split into planes (all the endpoints, then all the indices; BC3: the alpha endpoints, the alpha indices, the colour
//     endpoints, the colour indices), which zstd compresses 5 percent better than the interleaved blocks; exactly reversible.
//   * CompressTextureChain / UnpackTextureChain: a texture's whole mip chain (each level split into planes, the levels one after the other) as ONE frame, or the chain as it is when
//     the frame would not be smaller; and the way back, from a first mip on (the level cap of a graphics class decodes the whole frame and keeps the levels it wants: a chain is 29 KB
//     on average, the decode is microseconds).
// ===========================================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "citypack/city_hit_format.h"

namespace larecomp::citypack {

// The level the builders use. 6 is the knee on the retail data (both files together; zstd 1.5.7): levels 1, 3, 6, 9, 12, 15 and 19 give 267, 255, 248, 247, 246, 240 and 230 MB, the first
// five at 1.5, 2.5, 6.6, 9.2 and 16 seconds of the workers' time (level 19: 86). It is a builder's choice, not the format's: any level's frame decodes the same.
inline constexpr int kZstdLevelDefault = 6;
inline constexpr int kZstdLevelMax = 19;

// One zstd frame of `src` (content size and checksum in the frame). `level` is clamped to 1..kZstdLevelMax. False only when the library fails (out of memory).
bool ZstdCompress(const uint8_t* src, size_t size, int level, std::vector<uint8_t>* out);
// The frame must decode to EXACTLY `dest_size` bytes, and its checksum must hold. False (with `error`) for a damaged, cut or mismatched frame.
bool ZstdDecompress(const uint8_t* src, size_t size, uint8_t* dest, size_t dest_size, std::string* error);
// The same for a frame whose size nothing else says (a hit blob's corner stream): the size the frame states, at most `max_size`.
bool ZstdDecompressSelfSized(const uint8_t* src, size_t size, size_t max_size, std::vector<uint8_t>* out, std::string* error);

// The block planes of `format` (BC1, BC2, BC3: other formats are copied as they are). `bytes` must be a whole number of blocks (the chain's levels are: HitMipBytes).
void BcSplitPlanes(HitTextureFormat format, const uint8_t* in, size_t bytes, uint8_t* out);
void BcMergePlanes(HitTextureFormat format, const uint8_t* in, size_t bytes, uint8_t* out);

// The chain of `texture` (texture.data_size tightly packed bytes: the levels, the full size first) as it is stored: a zstd frame of the planed levels when that is smaller than the chain,
// else the chain itself (`stored->size() == texture.data_size`). The caller records stored->size() in HitTexture::stored_size.
bool CompressTextureChain(const HitTexture& texture, const uint8_t* chain, int level, std::vector<uint8_t>* stored);
// The levels [first_mip, mip_count) of the chain, tightly packed (the bytes a texture of the remaining levels is uploaded from), from `stored` as the file holds it
// (texture.StoredBytes() bytes). `scratch` is the whole chain's room (reused between calls). False (with `error`) for a damaged frame or a stored size that does not fit.
bool UnpackTextureChain(const HitTexture& texture, const uint8_t* stored, size_t stored_size, uint32_t first_mip, std::vector<uint8_t>* out, std::vector<uint8_t>* scratch,
                        std::string* error);

}  // namespace larecomp::citypack
