// ===========================================================================
// MCLA Native Graphics Runtime — GPU texture untiling & endian swap CS
// ===========================================================================
ByteAddressBuffer g_src_tiled : register(t0);
RWByteAddressBuffer g_dst_linear : register(u0);

cbuffer UntileParams : register(b0) {
    uint g_width_blocks;
    uint g_height_blocks;
    uint g_pitch_blocks;     // unpadded pitch in blocks
    uint g_dst_pitch_bytes;
    uint g_bytes_per_block;
    uint g_bpb_log2;
    uint g_endianness;       // 0=none, 1=k8in16, 2=k8in32, 3=k16in32
    uint g_src_size_bytes;
};

// Xenos 2D tiled address calculation (GetTiledOffset2D)
uint GetTiledOffset2D(uint x, uint y, uint pitch, uint bpb_log2) {
    uint aligned_pitch = (pitch + 31) & ~31;
    uint macro = ((x >> 5) + (y >> 5) * (aligned_pitch >> 5)) << (bpb_log2 + 7);
    uint micro = ((x & 7) + ((y & 0xE) << 2)) << bpb_log2;
    uint offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
    return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
           (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// Endian byte swapping
uint SwapBytes16(uint v) {
    return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
}

uint SwapBytes32(uint v) {
    return (v << 24) | ((v & 0x0000FF00u) << 8) | ((v & 0x00FF0000u) >> 8) | (v >> 24);
}

uint Swap16In32(uint v) {
    return (v >> 16) | (v << 16);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_width_blocks || id.y >= g_height_blocks) {
        return;
    }

    uint src_offset = GetTiledOffset2D(id.x, id.y, g_pitch_blocks, g_bpb_log2);
    if (src_offset + g_bytes_per_block > g_src_size_bytes) {
        return;
    }

    uint dst_offset = id.y * g_dst_pitch_bytes + id.x * g_bytes_per_block;

    if (g_bytes_per_block == 4) {
        // 4 bytes (32-bit: 8_8_8_8, 32_FLOAT, 24_8)
        uint val = g_src_tiled.Load(src_offset);
        if (g_endianness == 1) { // k8in16
            val = SwapBytes16(val);
        } else if (g_endianness == 2) { // k8in32
            val = SwapBytes32(val);
        } else if (g_endianness == 3) { // k16in32
            val = Swap16In32(val);
        }
        g_dst_linear.Store(dst_offset, val);
    } else if (g_bytes_per_block == 8) {
        // 8 bytes (DXT1, 16_16_16_16_FLOAT, 16_16_16_16_EXPAND)
        uint2 val = g_src_tiled.Load2(src_offset);
        if (g_endianness == 1) { // k8in16
            val.x = SwapBytes16(val.x);
            val.y = SwapBytes16(val.y);
        } else if (g_endianness == 2) { // k8in32
            val.x = SwapBytes32(val.x);
            val.y = SwapBytes32(val.y);
        } else if (g_endianness == 3) { // k16in32
            val.x = Swap16In32(val.x);
            val.y = Swap16In32(val.y);
        }
        g_dst_linear.Store2(dst_offset, val);
    } else if (g_bytes_per_block == 16) {
        // 16 bytes (DXT2/3, DXT4/5)
        uint4 val = g_src_tiled.Load4(src_offset);
        if (g_endianness == 1) { // k8in16
            val.x = SwapBytes16(val.x);
            val.y = SwapBytes16(val.y);
            val.z = SwapBytes16(val.z);
            val.w = SwapBytes16(val.w);
        } else if (g_endianness == 2) { // k8in32
            val.x = SwapBytes32(val.x);
            val.y = SwapBytes32(val.y);
            val.z = SwapBytes32(val.z);
            val.w = SwapBytes32(val.w);
        } else if (g_endianness == 3) { // k16in32
            val.x = Swap16In32(val.x);
            val.y = Swap16In32(val.y);
            val.z = Swap16In32(val.z);
            val.w = Swap16In32(val.w);
        }
        g_dst_linear.Store4(dst_offset, val);
    }
}
