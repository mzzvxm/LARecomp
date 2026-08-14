#include "aes256.h"

#include <cstring>

namespace mc::modloader {

const uint8_t kRpfKey[32] = {
    0xAF, 0x7C, 0xD2, 0xE9, 0xFA, 0xAA, 0x45, 0xFD, 0x97, 0x28, 0xAC,
    0x24, 0x7D, 0xD0, 0xCE, 0x5E, 0xD6, 0xE4, 0xA1, 0x82, 0xFF, 0xE2,
    0x41, 0xDB, 0x8F, 0xF0, 0x70, 0x3B, 0x62, 0x9C, 0x47, 0x85,
};

namespace {

const uint8_t kSbox[256] = {
    0x63, 0x7C, 0x77, 0x7B, 0xF2, 0x6B, 0x6F, 0xC5, 0x30, 0x01, 0x67, 0x2B, 0xFE, 0xD7, 0xAB, 0x76,
    0xCA, 0x82, 0xC9, 0x7D, 0xFA, 0x59, 0x47, 0xF0, 0xAD, 0xD4, 0xA2, 0xAF, 0x9C, 0xA4, 0x72, 0xC0,
    0xB7, 0xFD, 0x93, 0x26, 0x36, 0x3F, 0xF7, 0xCC, 0x34, 0xA5, 0xE5, 0xF1, 0x71, 0xD8, 0x31, 0x15,
    0x04, 0xC7, 0x23, 0xC3, 0x18, 0x96, 0x05, 0x9A, 0x07, 0x12, 0x80, 0xE2, 0xEB, 0x27, 0xB2, 0x75,
    0x09, 0x83, 0x2C, 0x1A, 0x1B, 0x6E, 0x5A, 0xA0, 0x52, 0x3B, 0xD6, 0xB3, 0x29, 0xE3, 0x2F, 0x84,
    0x53, 0xD1, 0x00, 0xED, 0x20, 0xFC, 0xB1, 0x5B, 0x6A, 0xCB, 0xBE, 0x39, 0x4A, 0x4C, 0x58, 0xCF,
    0xD0, 0xEF, 0xAA, 0xFB, 0x43, 0x4D, 0x33, 0x85, 0x45, 0xF9, 0x02, 0x7F, 0x50, 0x3C, 0x9F, 0xA8,
    0x51, 0xA3, 0x40, 0x8F, 0x92, 0x9D, 0x38, 0xF5, 0xBC, 0xB6, 0xDA, 0x21, 0x10, 0xFF, 0xF3, 0xD2,
    0xCD, 0x0C, 0x13, 0xEC, 0x5F, 0x97, 0x44, 0x17, 0xC4, 0xA7, 0x7E, 0x3D, 0x64, 0x5D, 0x19, 0x73,
    0x60, 0x81, 0x4F, 0xDC, 0x22, 0x2A, 0x90, 0x88, 0x46, 0xEE, 0xB8, 0x14, 0xDE, 0x5E, 0x0B, 0xDB,
    0xE0, 0x32, 0x3A, 0x0A, 0x49, 0x06, 0x24, 0x5C, 0xC2, 0xD3, 0xAC, 0x62, 0x91, 0x95, 0xE4, 0x79,
    0xE7, 0xC8, 0x37, 0x6D, 0x8D, 0xD5, 0x4E, 0xA9, 0x6C, 0x56, 0xF4, 0xEA, 0x65, 0x7A, 0xAE, 0x08,
    0xBA, 0x78, 0x25, 0x2E, 0x1C, 0xA6, 0xB4, 0xC6, 0xE8, 0xDD, 0x74, 0x1F, 0x4B, 0xBD, 0x8B, 0x8A,
    0x70, 0x3E, 0xB5, 0x66, 0x48, 0x03, 0xF6, 0x0E, 0x61, 0x35, 0x57, 0xB9, 0x86, 0xC1, 0x1D, 0x9E,
    0xE1, 0xF8, 0x98, 0x11, 0x69, 0xD9, 0x8E, 0x94, 0x9B, 0x1E, 0x87, 0xE9, 0xCE, 0x55, 0x28, 0xDF,
    0x8C, 0xA1, 0x89, 0x0D, 0xBF, 0xE6, 0x42, 0x68, 0x41, 0x99, 0x2D, 0x0F, 0xB0, 0x54, 0xBB, 0x16,
};

uint8_t g_inv_sbox[256];
bool g_inv_sbox_ready = false;

const uint8_t* InvSbox() {
    if (!g_inv_sbox_ready) {
        for (int i = 0; i < 256; ++i) g_inv_sbox[kSbox[i]] = static_cast<uint8_t>(i);
        g_inv_sbox_ready = true;
    }
    return g_inv_sbox;
}

inline uint8_t XTime(uint8_t x) {
    return static_cast<uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1B : 0x00));
}

// AES-256: 14 rounds, 60-word expanded key.
void ExpandKey(const uint8_t key[32], uint8_t round_keys[240]) {
    std::memcpy(round_keys, key, 32);

    uint8_t rcon = 1;
    for (int i = 8; i < 60; ++i) {
        uint8_t t[4];
        std::memcpy(t, round_keys + (i - 1) * 4, 4);

        if (i % 8 == 0) {
            const uint8_t tmp = t[0];
            t[0] = static_cast<uint8_t>(kSbox[t[1]] ^ rcon);
            t[1] = kSbox[t[2]];
            t[2] = kSbox[t[3]];
            t[3] = kSbox[tmp];
            rcon = XTime(rcon);
        } else if (i % 8 == 4) {
            for (int j = 0; j < 4; ++j) t[j] = kSbox[t[j]];
        }

        for (int j = 0; j < 4; ++j)
            round_keys[i * 4 + j] = static_cast<uint8_t>(round_keys[(i - 8) * 4 + j] ^ t[j]);
    }
}

void EncryptBlock(uint8_t s[16], const uint8_t round_keys[240]) {
    for (int i = 0; i < 16; ++i) s[i] ^= round_keys[i];

    for (int round = 1; round <= 14; ++round) {
        for (int i = 0; i < 16; ++i) s[i] = kSbox[s[i]];

        // ShiftRows (column-major state: byte i is row i%4, column i/4).
        uint8_t t;
        t = s[1];  s[1]  = s[5];  s[5]  = s[9];  s[9]  = s[13]; s[13] = t;
        t = s[2];  s[2]  = s[10]; s[10] = t;
        t = s[6];  s[6]  = s[14]; s[14] = t;
        t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7]  = s[3];   s[3]  = t;

        if (round != 14) {
            for (int c = 0; c < 4; ++c) {
                uint8_t* col = s + c * 4;
                const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                const uint8_t all = static_cast<uint8_t>(a0 ^ a1 ^ a2 ^ a3);
                col[0] = static_cast<uint8_t>(a0 ^ all ^ XTime(a0 ^ a1));
                col[1] = static_cast<uint8_t>(a1 ^ all ^ XTime(a1 ^ a2));
                col[2] = static_cast<uint8_t>(a2 ^ all ^ XTime(a2 ^ a3));
                col[3] = static_cast<uint8_t>(a3 ^ all ^ XTime(a3 ^ a0));
            }
        }

        const uint8_t* rk = round_keys + round * 16;
        for (int i = 0; i < 16; ++i) s[i] ^= rk[i];
    }
}

inline uint8_t GfMul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = XTime(a);
        b >>= 1;
    }
    return r;
}

void DecryptBlock(uint8_t s[16], const uint8_t round_keys[240]) {
    const uint8_t* inv = InvSbox();

    const uint8_t* rk = round_keys + 14 * 16;
    for (int i = 0; i < 16; ++i) s[i] ^= rk[i];

    for (int round = 13; round >= 0; --round) {
        // InvShiftRows.
        uint8_t t;
        t = s[13]; s[13] = s[9];  s[9]  = s[5];  s[5]  = s[1];  s[1] = t;
        t = s[2];  s[2]  = s[10]; s[10] = t;
        t = s[6];  s[6]  = s[14]; s[14] = t;
        t = s[3];  s[3]  = s[7];  s[7]  = s[11]; s[11] = s[15]; s[15] = t;

        for (int i = 0; i < 16; ++i) s[i] = inv[s[i]];

        rk = round_keys + round * 16;
        for (int i = 0; i < 16; ++i) s[i] ^= rk[i];

        if (round != 0) {
            for (int c = 0; c < 4; ++c) {
                uint8_t* col = s + c * 4;
                const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                col[0] = static_cast<uint8_t>(GfMul(a0, 14) ^ GfMul(a1, 11) ^ GfMul(a2, 13) ^ GfMul(a3, 9));
                col[1] = static_cast<uint8_t>(GfMul(a0, 9)  ^ GfMul(a1, 14) ^ GfMul(a2, 11) ^ GfMul(a3, 13));
                col[2] = static_cast<uint8_t>(GfMul(a0, 13) ^ GfMul(a1, 9)  ^ GfMul(a2, 14) ^ GfMul(a3, 11));
                col[3] = static_cast<uint8_t>(GfMul(a0, 11) ^ GfMul(a1, 13) ^ GfMul(a2, 9)  ^ GfMul(a3, 14));
            }
        }
    }
}

}  // namespace

void Aes256EcbEncrypt(uint8_t* data, size_t len, const uint8_t key[32], int rounds) {
    uint8_t round_keys[240];
    ExpandKey(key, round_keys);

    const size_t blocks = (len & ~static_cast<size_t>(0xF)) / 16;
    for (int r = 0; r < rounds; ++r) {
        for (size_t b = 0; b < blocks; ++b) EncryptBlock(data + b * 16, round_keys);
    }
}

void Aes256EcbDecrypt(uint8_t* data, size_t len, const uint8_t key[32], int rounds) {
    uint8_t round_keys[240];
    ExpandKey(key, round_keys);

    const size_t blocks = (len & ~static_cast<size_t>(0xF)) / 16;
    for (int r = 0; r < rounds; ++r) {
        for (size_t b = 0; b < blocks; ++b) DecryptBlock(data + b * 16, round_keys);
    }
}

}  // namespace mc::modloader
