#include "texture.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// The image loader is vendored here rather than borrowed from the SDK, which
// exposes one built with STBI_ONLY_PNG for decoding icons. A mod's textures are
// whatever the exporter felt like writing -- a single character arrives as a mix
// of PNG and JPEG -- and against a PNG-only decoder every JPEG silently fails,
// the atlas comes out empty, and the model wears the driver's skin with no sign
// of anything having gone wrong beyond one line in the log.
//
// STB_IMAGE_STATIC keeps every symbol internal, so this cannot collide with the
// copy the SDK links in.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO  // buffers only, never a path
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace mc::modloader {

namespace {

// Decodes any format stb understands to tightly packed R8G8B8A8.
std::vector<uint8_t> DecodeImage(const std::vector<uint8_t>& encoded, uint32_t& width,
                                 uint32_t& height) {
    width = 0;
    height = 0;
    if (encoded.empty()) return {};

    int decoded_width = 0, decoded_height = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()),
                                            &decoded_width, &decoded_height, &channels, 4);
    if (!pixels || decoded_width <= 0 || decoded_height <= 0) {
        if (pixels) stbi_image_free(pixels);
        return {};
    }

    width = static_cast<uint32_t>(decoded_width);
    height = static_cast<uint32_t>(decoded_height);
    std::vector<uint8_t> out(pixels, pixels + static_cast<size_t>(width) * height * 4);
    stbi_image_free(pixels);
    return out;
}

// The tiling maths is a straight port of Rpf3Crypto/XGAddress2DTiled: given the
// index of a block in tiled order it returns where that block sits in the plain
// linear image. The extraction tools run it one way; writing a texture runs the
// same map the other way round.
struct TiledCommon {
    uint32_t aligned_width;
    uint32_t log_bpp;
    uint32_t offset_b;
    uint32_t offset_t;
    uint32_t offset_m;
};

TiledCommon TiledCommonOf(uint32_t offset, uint32_t width, uint32_t texel_pitch) {
    TiledCommon out;
    out.aligned_width = (width + 31u) & ~31u;
    out.log_bpp = (texel_pitch >> 2) + ((texel_pitch >> 1) >> (texel_pitch >> 2));
    out.offset_b = offset << out.log_bpp;
    out.offset_t = ((out.offset_b & ~4095u) >> 3) + ((out.offset_b & 1792u) >> 2) +
                   (out.offset_b & 63u);
    out.offset_m = out.offset_t >> (7 + out.log_bpp);
    return out;
}

uint32_t TiledX(uint32_t offset, uint32_t width, uint32_t texel_pitch) {
    const TiledCommon c = TiledCommonOf(offset, width, texel_pitch);
    const uint32_t macro_x = (c.offset_m % (c.aligned_width >> 5)) << 2;
    // The two terms really are summed before the mask; narrowing the second one
    // to a single bit first looks tidier and produces a checkerboard.
    const uint32_t tile = (((c.offset_t >> (5 + c.log_bpp)) & 2) + (c.offset_b >> 6)) & 3;
    const uint32_t macro = (macro_x + tile) << 3;
    const uint32_t micro =
        ((((c.offset_t >> 1) & ~15u) + (c.offset_t & 15u)) & ((texel_pitch << 3) - 1)) >> c.log_bpp;
    return macro + micro;
}

uint32_t TiledY(uint32_t offset, uint32_t width, uint32_t texel_pitch) {
    const TiledCommon c = TiledCommonOf(offset, width, texel_pitch);
    const uint32_t macro_y = (c.offset_m / (c.aligned_width >> 5)) << 2;
    const uint32_t tile = ((c.offset_t >> (6 + c.log_bpp)) & 1) + ((c.offset_b & 2048u) >> 10);
    const uint32_t macro = (macro_y + tile) << 3;
    const uint32_t micro =
        (((c.offset_t & ((texel_pitch << 6) - 1) & ~31u) + ((c.offset_t & 15u) << 1)) >>
         (3 + c.log_bpp)) & ~1u;
    return macro + micro + ((c.offset_t & 16u) >> 4);
}

uint32_t BlockBytes(BlockFormat format) { return format == BlockFormat::kBc1 ? 8u : 16u; }

// Sub-128 textures are still stored inside a 128-wide image; the tools call
// this the virtual size.
uint32_t VirtualSize(uint32_t size) { return (size % 128 != 0 && size < 128) ? 128u : size; }

// ---------------------------------------------------------------------------
// Block compression
// ---------------------------------------------------------------------------

uint16_t Pack565(int r, int g, int b) {
    return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

void Unpack565(uint16_t value, int out[3]) {
    const int r = (value >> 11) & 31, g = (value >> 5) & 63, b = value & 31;
    out[0] = (r << 3) | (r >> 2);
    out[1] = (g << 2) | (g >> 4);
    out[2] = (b << 3) | (b >> 2);
}

// Colour half of a BC1/BC3 block: the endpoints are the corners of the block's
// RGB bounding box, pulled in slightly because the two interpolated colours sit
// a third of the way in and a box that hugs the extremes wastes them.
void CompressColour(const uint8_t block[64], uint8_t out[8]) {
    int low[3] = {255, 255, 255}, high[3] = {0, 0, 0};
    for (int i = 0; i < 16; ++i) {
        for (int c = 0; c < 3; ++c) {
            const int value = block[i * 4 + c];
            low[c] = std::min(low[c], value);
            high[c] = std::max(high[c], value);
        }
    }
    for (int c = 0; c < 3; ++c) {
        const int inset = (high[c] - low[c]) >> 4;
        low[c] = std::min(255, low[c] + inset);
        high[c] = std::max(0, high[c] - inset);
    }

    uint16_t c0 = Pack565(high[0], high[1], high[2]);
    uint16_t c1 = Pack565(low[0], low[1], low[2]);
    if (c0 < c1) std::swap(c0, c1);

    int palette[4][3];
    Unpack565(c0, palette[0]);
    Unpack565(c1, palette[1]);
    for (int c = 0; c < 3; ++c) {
        palette[2][c] = (2 * palette[0][c] + palette[1][c]) / 3;
        palette[3][c] = (palette[0][c] + 2 * palette[1][c]) / 3;
    }

    uint32_t indices = 0;
    for (int i = 0; i < 16; ++i) {
        int best = 0, best_error = 1 << 30;
        for (int p = 0; p < 4; ++p) {
            int error = 0;
            for (int c = 0; c < 3; ++c) {
                const int delta = block[i * 4 + c] - palette[p][c];
                error += delta * delta;
            }
            if (error < best_error) {
                best_error = error;
                best = p;
            }
        }
        indices |= static_cast<uint32_t>(best) << (2 * i);
    }

    out[0] = static_cast<uint8_t>(c0);
    out[1] = static_cast<uint8_t>(c0 >> 8);
    out[2] = static_cast<uint8_t>(c1);
    out[3] = static_cast<uint8_t>(c1 >> 8);
    std::memcpy(out + 4, &indices, 4);
}

// Alpha half of a BC3 block: eight interpolated levels between the block's
// minimum and maximum.
void CompressAlpha(const uint8_t block[64], uint8_t out[8]) {
    int low = 255, high = 0;
    for (int i = 0; i < 16; ++i) {
        const int value = block[i * 4 + 3];
        low = std::min(low, value);
        high = std::max(high, value);
    }

    int palette[8];
    palette[0] = high;
    palette[1] = low;
    for (int k = 1; k <= 6; ++k) palette[1 + k] = ((7 - k) * high + k * low) / 7;

    uint64_t indices = 0;
    for (int i = 0; i < 16; ++i) {
        const int value = block[i * 4 + 3];
        int best = 0, best_error = 1 << 30;
        for (int p = 0; p < 8; ++p) {
            const int error = std::abs(value - palette[p]);
            if (error < best_error) {
                best_error = error;
                best = p;
            }
        }
        indices |= static_cast<uint64_t>(best) << (3 * i);
    }

    out[0] = static_cast<uint8_t>(high);
    out[1] = static_cast<uint8_t>(low);
    for (int i = 0; i < 6; ++i) out[2 + i] = static_cast<uint8_t>((indices >> (8 * i)) & 0xFF);
}

}  // namespace

uint32_t StoredTextureSize(uint32_t width, uint32_t height, BlockFormat format) {
    const uint32_t w = VirtualSize(width), h = VirtualSize(height);
    return format == BlockFormat::kBc1 ? w * h / 2 : w * h;
}

Image FlatNormalMap(uint32_t width, uint32_t height) {
    Image out;
    out.width = std::max(1u, width);
    out.height = std::max(1u, height);
    out.rgba.assign(static_cast<size_t>(out.width) * out.height * 4, 0);
    for (size_t i = 0; i < out.rgba.size(); i += 4) {
        out.rgba[i + 0] = 0;    // unused
        out.rgba[i + 1] = 128;  // Y
        out.rgba[i + 2] = 0;    // unused
        out.rgba[i + 3] = 128;  // X, where BC3 keeps its precision
    }
    return out;
}

void ResizeImage(const Image& source, uint32_t width, uint32_t height, Image& out) {
    out.width = width;
    out.height = height;
    out.rgba.assign(static_cast<size_t>(width) * height * 4, 0);
    if (source.empty() || width == 0 || height == 0) return;

    const float scale_x = static_cast<float>(source.width) / static_cast<float>(width);
    const float scale_y = static_cast<float>(source.height) / static_cast<float>(height);

    for (uint32_t y = 0; y < height; ++y) {
        const float sy = (static_cast<float>(y) + 0.5f) * scale_y - 0.5f;
        const int y0 = std::max(0, static_cast<int>(std::floor(sy)));
        const int y1 = std::min<int>(static_cast<int>(source.height) - 1, y0 + 1);
        const float fy = std::max(0.0f, sy - static_cast<float>(y0));

        for (uint32_t x = 0; x < width; ++x) {
            const float sx = (static_cast<float>(x) + 0.5f) * scale_x - 0.5f;
            const int x0 = std::max(0, static_cast<int>(std::floor(sx)));
            const int x1 = std::min<int>(static_cast<int>(source.width) - 1, x0 + 1);
            const float fx = std::max(0.0f, sx - static_cast<float>(x0));

            for (int c = 0; c < 4; ++c) {
                const auto at = [&](int px, int py) {
                    return static_cast<float>(
                        source.rgba[(static_cast<size_t>(py) * source.width + px) * 4 + c]);
                };
                const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * fx;
                const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * fx;
                const float value = top + (bottom - top) * fy;
                out.rgba[(static_cast<size_t>(y) * width + x) * 4 + c] =
                    static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 255.0f)));
            }
        }
    }
}

void HalveImage(const Image& source, Image& out) {
    const uint32_t width = std::max(1u, source.width / 2);
    const uint32_t height = std::max(1u, source.height / 2);
    out.width = width;
    out.height = height;
    out.rgba.assign(static_cast<size_t>(width) * height * 4, 0);

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            for (int c = 0; c < 4; ++c) {
                uint32_t sum = 0;
                int taps = 0;
                for (int dy = 0; dy < 2; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        const uint32_t sx = std::min(source.width - 1, x * 2 + dx);
                        const uint32_t sy = std::min(source.height - 1, y * 2 + dy);
                        sum += source.rgba[(static_cast<size_t>(sy) * source.width + sx) * 4 + c];
                        ++taps;
                    }
                }
                out.rgba[(static_cast<size_t>(y) * width + x) * 4 + c] =
                    static_cast<uint8_t>(sum / static_cast<uint32_t>(taps));
            }
        }
    }
}

void CompressBlocks(const Image& source, BlockFormat format, std::vector<uint8_t>& out) {
    const uint32_t blocks_x = (source.width + 3) / 4;
    const uint32_t blocks_y = (source.height + 3) / 4;
    const uint32_t block_bytes = BlockBytes(format);
    out.assign(static_cast<size_t>(blocks_x) * blocks_y * block_bytes, 0);

    for (uint32_t by = 0; by < blocks_y; ++by) {
        for (uint32_t bx = 0; bx < blocks_x; ++bx) {
            uint8_t texels[64];
            for (int j = 0; j < 4; ++j) {
                for (int i = 0; i < 4; ++i) {
                    // Blocks past the edge of a non-multiple-of-four image repeat
                    // the last real texel rather than sampling black.
                    const uint32_t sx = std::min(source.width - 1, bx * 4 + i);
                    const uint32_t sy = std::min(source.height - 1, by * 4 + j);
                    std::memcpy(texels + (j * 4 + i) * 4,
                                source.rgba.data() + (static_cast<size_t>(sy) * source.width + sx) * 4,
                                4);
                }
            }

            uint8_t* target = out.data() + (static_cast<size_t>(by) * blocks_x + bx) * block_bytes;
            if (format == BlockFormat::kBc3) {
                CompressAlpha(texels, target);
                CompressColour(texels, target + 8);
            } else {
                CompressColour(texels, target);
            }
        }
    }
}

void TileXbox360(const std::vector<uint8_t>& linear_blocks, uint32_t width, uint32_t height,
                 BlockFormat format, std::vector<uint8_t>& out) {
    const uint32_t texel_pitch = BlockBytes(format);
    const uint32_t virtual_width = VirtualSize(width);
    const uint32_t virtual_height = VirtualSize(height);
    const uint32_t vbw = virtual_width / 4, vbh = virtual_height / 4;
    const uint32_t abw = std::max(1u, width / 4), abh = std::max(1u, height / 4);

    // Lay the real blocks into the padded image first, repeating the edge into
    // the padding: the tiling scatters blocks across the whole virtual buffer,
    // so every block of it has to hold something, and an edge copy is the one
    // filling that cannot bleed a foreign colour if the GPU ever samples it.
    std::vector<uint8_t> padded(static_cast<size_t>(vbw) * vbh * texel_pitch, 0);
    for (uint32_t j = 0; j < vbh; ++j) {
        const uint32_t sj = std::min(abh - 1, j);
        for (uint32_t i = 0; i < vbw; ++i) {
            const uint32_t si = std::min(abw - 1, i);
            const size_t source = (static_cast<size_t>(sj) * abw + si) * texel_pitch;
            const size_t target = (static_cast<size_t>(j) * vbw + i) * texel_pitch;
            if (source + texel_pitch <= linear_blocks.size()) {
                std::memcpy(padded.data() + target, linear_blocks.data() + source, texel_pitch);
            }
        }
    }

    out.assign(padded.size(), 0);
    for (uint32_t j = 0; j < vbh; ++j) {
        for (uint32_t i = 0; i < vbw; ++i) {
            const uint32_t block = j * vbw + i;
            const uint32_t x = TiledX(block, vbw, texel_pitch);
            const uint32_t y = TiledY(block, vbw, texel_pitch);
            const size_t source = (static_cast<size_t>(y) * vbw + x) * texel_pitch;
            const size_t target = static_cast<size_t>(block) * texel_pitch;
            if (source + texel_pitch <= padded.size() && target + texel_pitch <= out.size()) {
                std::memcpy(out.data() + target, padded.data() + source, texel_pitch);
            }
        }
    }

    // The console reads texture memory 16 bits at a time, byte-swapped.
    for (size_t i = 0; i + 1 < out.size(); i += 2) std::swap(out[i], out[i + 1]);
}

namespace {

// What one cell of the atlas holds: an image, and how many times it has to be
// repeated across the cell to cover the UVs of the parts that use it.
struct Cell {
    int image = -1;
    Image decoded;
    float min_u = 0.0f, max_u = 1.0f;
    float min_v = 0.0f, max_v = 1.0f;
    int tiles_u = 1, tiles_v = 1;
    int origin_u = 0, origin_v = 0;  // the tile the range starts in
};

// A UV range wider than one unit means the material tiles its texture. The atlas
// cannot wrap -- everything outside the cell belongs to another part -- so the
// whole range is covered by repeating the image inside the cell instead, and the
// UVs are scaled into it. Clamping instead is what folds a whole model's texture
// onto one edge of itself.
void MeasureTiles(Cell& cell) {
    constexpr float kSlack = 1e-3f;  // exporters overshoot 1.0 by a rounding step
    cell.origin_u = static_cast<int>(std::floor(cell.min_u + kSlack));
    cell.origin_v = static_cast<int>(std::floor(cell.min_v + kSlack));
    cell.tiles_u = std::clamp(
        static_cast<int>(std::ceil(cell.max_u - kSlack)) - cell.origin_u, 1, 4);
    cell.tiles_v = std::clamp(
        static_cast<int>(std::ceil(cell.max_v - kSlack)) - cell.origin_v, 1, 4);
}

}  // namespace

bool BuildMeshAtlas(Mesh& mesh, uint32_t cell_size, Image& atlas, std::string& error,
                    uint32_t* out_cells) {
    if (out_cells) *out_cells = 0;
    if (mesh.images.empty()) {
        error = "the model carries no textures";
        return false;
    }

    // Only the images a part actually draws with earn a cell. A glTF carries the
    // whole material set -- metallic-roughness maps, normal maps -- and giving
    // those cells of their own costs every real texture resolution it cannot
    // spare: five images make a three by three grid where three make a two by
    // two, and the diffuse maps come out at a fifth of the size for nothing.
    std::vector<Cell> cells;
    std::vector<int> cell_of_image(mesh.images.size(), -1);
    for (const MeshPart& part : mesh.parts) {
        if (part.image < 0 || static_cast<size_t>(part.image) >= mesh.images.size()) continue;
        if (mesh.images[static_cast<size_t>(part.image)].empty()) continue;

        int& slot = cell_of_image[static_cast<size_t>(part.image)];
        if (slot < 0) {
            slot = static_cast<int>(cells.size());
            Cell fresh;
            fresh.image = part.image;
            fresh.min_u = fresh.min_v = 1e30f;
            fresh.max_u = fresh.max_v = -1e30f;
            cells.push_back(std::move(fresh));
        }

        // Parts sharing an image share its cell, so the cell has to cover the
        // union of what they ask for.
        Cell& target = cells[static_cast<size_t>(slot)];
        const uint32_t end = std::min<uint32_t>(part.first_vertex + part.vertex_count,
                                                static_cast<uint32_t>(mesh.vertices.size()));
        for (uint32_t v = part.first_vertex; v < end; ++v) {
            target.min_u = std::min(target.min_u, mesh.vertices[v].u);
            target.max_u = std::max(target.max_u, mesh.vertices[v].u);
            target.min_v = std::min(target.min_v, mesh.vertices[v].v);
            target.max_v = std::max(target.max_v, mesh.vertices[v].v);
        }
    }

    const size_t referenced = cells.size();
    for (auto it = cells.begin(); it != cells.end();) {
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> rgba =
            DecodeImage(mesh.images[static_cast<size_t>(it->image)], width, height);
        if (rgba.empty()) {
            cell_of_image[static_cast<size_t>(it->image)] = -1;
            it = cells.erase(it);
            continue;
        }
        it->decoded.rgba = std::move(rgba);
        it->decoded.width = width;
        it->decoded.height = height;
        if (it->max_u < it->min_u) { it->min_u = it->min_v = 0.0f; it->max_u = it->max_v = 1.0f; }
        MeasureTiles(*it);
        ++it;
    }
    if (cells.empty()) {
        // Worth telling apart: nothing to draw with because no material named a
        // texture, versus a texture that named itself and would not decode.
        error = referenced == 0
                    ? "no material references any of the model's " +
                          std::to_string(mesh.images.size()) + " textures"
                    : "none of the model's " + std::to_string(referenced) +
                          " referenced textures could be decoded";
        return false;
    }
    // Erasing renumbers, so the image-to-cell table is rebuilt from the survivors.
    std::fill(cell_of_image.begin(), cell_of_image.end(), -1);
    for (size_t i = 0; i < cells.size(); ++i)
        cell_of_image[static_cast<size_t>(cells[i].image)] = static_cast<int>(i);

    // Materials with no texture at all -- an eye shine, a glass pane -- would
    // otherwise keep raw UVs and read whatever happens to sit in the first cell.
    // One flat white cell of their own is cheap and predictable.
    int flat_slot = -1;
    for (const MeshPart& part : mesh.parts) {
        const bool mapped = part.image >= 0 &&
                            static_cast<size_t>(part.image) < cell_of_image.size() &&
                            cell_of_image[static_cast<size_t>(part.image)] >= 0;
        if (mapped) continue;
        flat_slot = static_cast<int>(cells.size());
        Cell flat;
        flat.decoded.width = 1;
        flat.decoded.height = 1;
        flat.decoded.rgba.assign(4, 0xFF);
        cells.push_back(std::move(flat));
        break;
    }

    // The grid is a power of two, and that is not tidiness.
    //
    // The atlas does not stay the size it is built at: it is resampled onto the
    // texture the template shipped, which is a power of two. Nine cells in a
    // three by three grid make a 768-pixel atlas, and 768 onto 512 is two
    // thirds -- so a cell boundary lands at 170.67 texels and every one of them
    // bleeds into its neighbour, which on a face is pieces of a shoe and a
    // jacket showing through the skin. Four cells in a two by two grid resample
    // one to one and never showed it, which is why this hid until a model
    // arrived with more than four materials.
    //
    // A power-of-two grid keeps the ratio a power of two, so a boundary stays on
    // a whole texel at every mip. It costs resolution -- nine cells take a four
    // by four grid and use a quarter of the sheet each instead of a third -- and
    // that is the cheaper half of the trade.
    uint32_t columns = 1;
    while (static_cast<size_t>(columns) * columns < cells.size()) columns *= 2;
    if (out_cells) *out_cells = static_cast<uint32_t>(cells.size());

    atlas.width = cell_size * columns;
    atlas.height = cell_size * columns;
    atlas.rgba.assign(static_cast<size_t>(atlas.width) * atlas.height * 4, 0);

    for (size_t i = 0; i < cells.size(); ++i) {
        const Cell& cell = cells[i];
        const uint32_t tile_width = std::max(1u, cell_size / static_cast<uint32_t>(cell.tiles_u));
        const uint32_t tile_height = std::max(1u, cell_size / static_cast<uint32_t>(cell.tiles_v));
        Image scaled;
        ResizeImage(cell.decoded, tile_width, tile_height, scaled);

        const uint32_t left = static_cast<uint32_t>(i % columns) * cell_size;
        const uint32_t top = static_cast<uint32_t>(i / columns) * cell_size;
        for (uint32_t y = 0; y < cell_size; ++y) {
            const uint32_t source_row = y % tile_height;
            for (uint32_t x = 0; x < cell_size; ++x) {
                std::memcpy(atlas.rgba.data() +
                                ((static_cast<size_t>(top + y) * atlas.width) + left + x) * 4,
                            scaled.rgba.data() +
                                (static_cast<size_t>(source_row) * tile_width + x % tile_width) * 4,
                            4);
            }
        }
    }

    // Send every part's UVs into its own cell. Half a texel is kept clear on
    // each side so bilinear filtering at a cell edge cannot reach into the
    // neighbour -- which at the atlas's mip levels is what smears a shoe across
    // a face.
    // A whole texel, not half: what is half a texel here is less than that
    // once the sheet has been resampled down onto the shipped texture.
    const float inset_u = 1.0f / static_cast<float>(atlas.width);
    const float inset_v = 1.0f / static_cast<float>(atlas.height);
    const float span = 1.0f / static_cast<float>(columns);

    for (const MeshPart& part : mesh.parts) {
        int slot = -1;
        if (part.image >= 0 && static_cast<size_t>(part.image) < cell_of_image.size())
            slot = cell_of_image[static_cast<size_t>(part.image)];
        const bool textured = slot >= 0;
        if (!textured) slot = flat_slot;
        if (slot < 0) continue;
        const Cell& cell = cells[static_cast<size_t>(slot)];

        const float origin_u = static_cast<float>(slot % static_cast<int>(columns)) * span;
        const float origin_v = static_cast<float>(slot / static_cast<int>(columns)) * span;

        const uint32_t end = std::min<uint32_t>(part.first_vertex + part.vertex_count,
                                                static_cast<uint32_t>(mesh.vertices.size()));
        for (uint32_t v = part.first_vertex; v < end; ++v) {
            MeshVertex& vertex = mesh.vertices[v];
            // An untextured part collapses onto the middle of its flat cell:
            // whatever its own UVs said, there is nothing for them to address.
            const float u = textured ? std::clamp((vertex.u - static_cast<float>(cell.origin_u)) /
                                                      static_cast<float>(cell.tiles_u),
                                                  0.0f, 1.0f)
                                     : 0.5f;
            const float w = textured ? std::clamp((vertex.v - static_cast<float>(cell.origin_v)) /
                                                      static_cast<float>(cell.tiles_v),
                                                  0.0f, 1.0f)
                                     : 0.5f;
            vertex.u = origin_u + inset_u + u * (span - 2.0f * inset_u);
            vertex.v = origin_v + inset_v + w * (span - 2.0f * inset_v);
        }
    }
    return true;
}

}  // namespace mc::modloader
