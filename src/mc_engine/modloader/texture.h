// Texture side of the model swap: source images -> one atlas -> the block
// compressed, Xbox-360-tiled bytes the drawable already has room for.
//
// The geometry rewrite folds every primitive of the mod into the drawable's
// single largest submesh, so the whole model ends up drawn by one shader with
// one diffuse texture. An exporter, though, splits a character into one image
// per material -- CJ arrives as body/head/shoes/legs. Packing them into a grid
// and remapping each primitive's UVs into its own cell is what lets a single
// texture slot carry all of them.
//
// Nothing here grows anything: the atlas is resized to whatever the shipped
// texture measures and compressed to the format it already uses, so the bytes
// land in place, same address, same size.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "objmesh.h"

namespace mc::modloader {

struct Image {
    std::vector<uint8_t> rgba;  // tightly packed R8G8B8A8
    uint32_t width = 0;
    uint32_t height = 0;

    bool empty() const { return rgba.empty() || width == 0 || height == 0; }
};

// The two block formats MCLA's character textures use.
enum class BlockFormat { kBc1, kBc3 };

// Packs `mesh.images` into a square grid of `cell` by `cell` pixels and rewrites
// the UVs of every part so each keeps pointing at its own image. One image
// means a straight copy with the UVs left alone. Fails only if nothing decodes.
//
// `out_cells` reports how many cells were filled. The grid is square, so two
// cells still make a two by two atlas and the last two are left untouched --
// anything measuring the atlas has to know where the real content stops, or it
// reads the padding as part of the image.
bool BuildMeshAtlas(Mesh& mesh, uint32_t cell, Image& atlas, std::string& error,
                    uint32_t* out_cells = nullptr);

// Bilinear resample. Used to fit the atlas to whatever the target texture is.
void ResizeImage(const Image& source, uint32_t width, uint32_t height, Image& out);

// Box filter down to half, the mip chain one step at a time.
void HalveImage(const Image& source, Image& out);

// BC1/BC3 encode. `out` comes back as ceil(w/4) * ceil(h/4) blocks in linear
// (untiled) order, 8 bytes per block for BC1 and 16 for BC3.
void CompressBlocks(const Image& source, BlockFormat format, std::vector<uint8_t>& out);

// Applies the Xbox 360 texture tiling and the 16-bit byte swap the console GPU
// expects -- the exact inverse of the unswizzle the extraction tools do.
//
// `width`/`height` are the real texel dimensions; anything under 128 is stored
// inside a 128-wide virtual image, so the buffer that comes out is sized from
// the padded dimensions and the padding is filled by clamping the edge blocks
// rather than left blank.
void TileXbox360(const std::vector<uint8_t>& linear_blocks, uint32_t width, uint32_t height,
                 BlockFormat format, std::vector<uint8_t>& out);

// The size a level occupies once stored: padded to 128 in each axis, then
// blocks. Matches Rpf3Crypto.GetVirtualSize + the per-format block maths.
uint32_t StoredTextureSize(uint32_t width, uint32_t height, BlockFormat format);

// A normal map that perturbs nothing, in the encoding MCLA's character normal
// maps use. Decoding a shipped one shows red and blue flat at zero and green and
// alpha averaging 127: it is DXT5nm, the X of the normal carried in the alpha
// channel and the Y in green, which is what BC3 devotes its extra precision to.
// Flat is therefore mid-grey in both of those and nothing anywhere else.
Image FlatNormalMap(uint32_t width, uint32_t height);

}  // namespace mc::modloader
