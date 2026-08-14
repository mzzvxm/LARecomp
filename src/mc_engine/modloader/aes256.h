// AES-256 ECB.
//
// MCLA's RPF3 table of contents is protected by the rage crypto layer: the key
// schedule is built in sub_821E1610 from the 32 bytes at 0x827D8AC4 and the
// block loop in sub_821E1658 runs the *decrypt* pass sixteen times over
// (size & ~0xF) bytes. Reading an archive means repeating that; producing one
// the game accepts means running the inverse, encrypt sixteen times.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mc::modloader {

// The archive key, lifted from the XEX (.data @ 0x827D8AC4).
extern const uint8_t kRpfKey[32];

// Encrypts `len & ~0xF` bytes of `data` in place, ECB, `rounds` times over.
void Aes256EcbEncrypt(uint8_t* data, size_t len, const uint8_t key[32], int rounds);

// Decrypts `len & ~0xF` bytes of `data` in place, ECB, `rounds` times over.
void Aes256EcbDecrypt(uint8_t* data, size_t len, const uint8_t key[32], int rounds);

}  // namespace mc::modloader
