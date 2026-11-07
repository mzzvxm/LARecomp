#include "shaderpack/xex_image.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <utility>

#include "shaderpack/image_scan.h"  // LoadFlatImage: the whole file into memory

namespace larecomp::shaderpack {
namespace {

// The key Xenia and the ReXGlue SDK open retail XEX files with (xex_module.cpp). The file's own AES key,
// stored in the security info, is encrypted with it; the dev kit key is all zeros.
constexpr uint8_t kRetailKey[16] = {0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
                                    0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91};
constexpr uint8_t kDevKitKey[16] = {};

// XEX2 header (all fields big endian):
//   0x00 magic "XEX2", 0x04 module flags, 0x08 header size (where the image data starts), 0x0C reserved,
//   0x10 security info offset, 0x14 optional header count, 0x18 the optional headers {key, value}.
// Security info: 0x04 image size, 0x110 load address, 0x150 the (encrypted) AES key.
constexpr size_t kHeaderFixedSize = 0x18;
constexpr size_t kSecurityImageSize = 0x04;
constexpr size_t kSecurityLoadAddress = 0x110;
constexpr size_t kSecurityAesKey = 0x150;
constexpr size_t kSecurityNeeded = 0x160;

constexpr uint32_t kOptFileFormatInfo = 0x000003FF;   // value = offset of {info size, encryption, compression, ...}
constexpr uint32_t kOptImageBaseAddress = 0x00010201;  // value = the address itself

constexpr uint16_t kEncryptionNone = 0;
constexpr uint16_t kEncryptionNormal = 1;
constexpr uint16_t kCompressionNone = 0;
constexpr uint16_t kCompressionBasic = 1;
constexpr uint16_t kCompressionNormal = 2;  // LZX
constexpr uint16_t kCompressionDelta = 3;

// A sanity bound on what a header may claim (the retail image is 10 MB).
constexpr uint64_t kMaxImageBytes = 512ull << 20;

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
uint16_t Be16(const uint8_t* p) {
  return uint16_t((uint32_t(p[0]) << 8) | uint32_t(p[1]));
}

// ---------------------------------------------------------------------------
// AES-128, decryption only. The tables are made once from the field's arithmetic (no typed-in
// S-box to get wrong); the speed (about a megabyte in a few milliseconds) is plenty for 10 MB.
// ---------------------------------------------------------------------------

uint8_t Rotl8(uint8_t x, int n) { return uint8_t((x << n) | (x >> (8 - n))); }
uint8_t XTime(uint8_t x) { return uint8_t((x << 1) ^ ((x & 0x80) ? 0x1B : 0x00)); }

struct AesTables {
  uint8_t sbox[256];
  uint8_t inv_sbox[256];
  uint8_t mul9[256];
  uint8_t mul11[256];
  uint8_t mul13[256];
  uint8_t mul14[256];

  AesTables() {
    // The S-box: the multiplicative inverse in GF(2^8) followed by the affine map, walking p through
    // the powers of 3 and q through their inverses (the walk visits every non-zero element once).
    uint8_t p = 1;
    uint8_t q = 1;
    do {
      p = uint8_t(p ^ XTime(p));  // p * 3
      q = uint8_t(q ^ (q << 1));  // q / 3
      q = uint8_t(q ^ (q << 2));
      q = uint8_t(q ^ (q << 4));
      if (q & 0x80) q = uint8_t(q ^ 0x09);
      sbox[p] = uint8_t(q ^ Rotl8(q, 1) ^ Rotl8(q, 2) ^ Rotl8(q, 3) ^ Rotl8(q, 4) ^ 0x63);
    } while (p != 1);
    sbox[0] = 0x63;
    for (int i = 0; i < 256; ++i) inv_sbox[sbox[i]] = uint8_t(i);

    for (int i = 0; i < 256; ++i) {
      const uint8_t x = uint8_t(i);
      const uint8_t x2 = XTime(x);
      const uint8_t x4 = XTime(x2);
      const uint8_t x8 = XTime(x4);
      mul9[i] = uint8_t(x8 ^ x);
      mul11[i] = uint8_t(x8 ^ x2 ^ x);
      mul13[i] = uint8_t(x8 ^ x4 ^ x);
      mul14[i] = uint8_t(x8 ^ x4 ^ x2);
    }
  }
};

const AesTables& Tables() {
  static const AesTables tables;
  return tables;
}

class Aes128 {
 public:
  explicit Aes128(const uint8_t key[16]) {
    const AesTables& t = Tables();
    std::memcpy(round_keys_, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
      uint8_t word[4] = {round_keys_[i - 4], round_keys_[i - 3], round_keys_[i - 2], round_keys_[i - 1]};
      if (i % 16 == 0) {  // RotWord, SubWord, Rcon
        const uint8_t first = word[0];
        word[0] = uint8_t(t.sbox[word[1]] ^ rcon);
        word[1] = t.sbox[word[2]];
        word[2] = t.sbox[word[3]];
        word[3] = t.sbox[first];
        rcon = XTime(rcon);
      }
      for (int j = 0; j < 4; ++j) round_keys_[i + j] = uint8_t(round_keys_[i - 16 + j] ^ word[j]);
    }
  }

  // One block, ECB. `in` and `out` may be the same.
  void DecryptBlock(const uint8_t* in, uint8_t* out) const {
    const AesTables& t = Tables();
    uint8_t s[16];
    for (int i = 0; i < 16; ++i) s[i] = uint8_t(in[i] ^ round_keys_[160 + i]);
    for (int round = 9; round >= 0; --round) {
      // InvShiftRows (row r moves right by r; the state is column major) and InvSubBytes.
      uint8_t m[16];
      for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) m[4 * ((c + r) & 3) + r] = t.inv_sbox[s[4 * c + r]];
      }
      for (int i = 0; i < 16; ++i) m[i] = uint8_t(m[i] ^ round_keys_[round * 16 + i]);
      if (round == 0) {
        std::memcpy(out, m, 16);
        return;
      }
      // InvMixColumns.
      for (int c = 0; c < 4; ++c) {
        const uint8_t a0 = m[4 * c], a1 = m[4 * c + 1], a2 = m[4 * c + 2], a3 = m[4 * c + 3];
        s[4 * c + 0] = uint8_t(t.mul14[a0] ^ t.mul11[a1] ^ t.mul13[a2] ^ t.mul9[a3]);
        s[4 * c + 1] = uint8_t(t.mul9[a0] ^ t.mul14[a1] ^ t.mul11[a2] ^ t.mul13[a3]);
        s[4 * c + 2] = uint8_t(t.mul13[a0] ^ t.mul9[a1] ^ t.mul14[a2] ^ t.mul11[a3]);
        s[4 * c + 3] = uint8_t(t.mul11[a0] ^ t.mul13[a1] ^ t.mul9[a2] ^ t.mul14[a3]);
      }
    }
  }

 private:
  uint8_t round_keys_[176];
};

// CBC decryption with a zero initial vector whose chain carries on from call to call, which is how
// the SDK reads the data blocks of a basic compressed image (one chain over all of them).
class CbcDecryptor {
 public:
  explicit CbcDecryptor(const uint8_t key[16]) : aes_(key) { std::memset(iv_, 0, sizeof(iv_)); }

  // `length` is a multiple of 16. `in` and `out` do not overlap.
  void Decrypt(const uint8_t* in, uint8_t* out, size_t length) {
    for (size_t n = 0; n + 16 <= length; n += 16) {
      uint8_t plain[16];
      aes_.DecryptBlock(in + n, plain);
      for (int i = 0; i < 16; ++i) {
        out[n + i] = uint8_t(plain[i] ^ iv_[i]);
        iv_[i] = in[n + i];
      }
    }
  }

 private:
  Aes128 aes_;
  uint8_t iv_[16];
};

// The session key: the file key (security info) decrypted with the master key.
void SessionKey(const uint8_t master[16], const uint8_t* file_key, uint8_t session[16]) {
  Aes128(master).DecryptBlock(file_key, session);
}

// The image starts with the PE header, "MZ": whether the first block opens to that with this session key.
bool OpensToPe(const uint8_t session[16], const uint8_t* first_block) {
  uint8_t plain[16];
  Aes128(session).DecryptBlock(first_block, plain);  // a zero IV: the first CBC block is a plain decrypt
  return plain[0] == 'M' && plain[1] == 'Z';
}

bool FindOptionalHeader(const uint8_t* xex, size_t size, uint32_t count, uint32_t key, uint32_t* value) {
  for (uint32_t i = 0; i < count; ++i) {
    const size_t at = kHeaderFixedSize + size_t(i) * 8;
    if (at + 8 > size) return false;
    if (Be32(xex + at) == key) {
      *value = Be32(xex + at + 4);
      return true;
    }
  }
  return false;
}

}  // namespace

bool DecodeXexImage(const uint8_t* xex, size_t size, XexImage* image, std::string* error) {
  std::string scratch;
  std::string& why = error ? *error : scratch;
  const auto fail = [&](std::string text) {
    why = std::move(text);
    return false;
  };
  if (image == nullptr) return fail("No place to put the image.");
  *image = XexImage{};

  if (xex == nullptr || size < kHeaderFixedSize) return fail("default.xex is too short to be an Xbox 360 executable.");
  if (std::memcmp(xex, "XEX2", 4) != 0) return fail("default.xex is not an XEX2 file (its first four bytes are not \"XEX2\").");

  const uint32_t header_size = Be32(xex + 0x08);
  const uint32_t security_offset = Be32(xex + 0x10);
  const uint32_t header_count = Be32(xex + 0x14);
  if (header_count > 0x1000 || kHeaderFixedSize + size_t(header_count) * 8 > size) {
    return fail("default.xex has a damaged header (the list of optional headers runs past the file).");
  }
  if (header_size < kHeaderFixedSize || header_size > size) {
    return fail("default.xex has a damaged header (the image would start outside the file).");
  }
  if (size_t(security_offset) + kSecurityNeeded > size) {
    return fail("default.xex has a damaged header (the security info is outside the file).");
  }
  const uint8_t* security = xex + security_offset;

  // How the image is stored.
  uint32_t format_offset = 0;
  if (!FindOptionalHeader(xex, size, header_count, kOptFileFormatInfo, &format_offset) ||
      size_t(format_offset) + 8 > size) {
    return fail("default.xex does not say how its image is stored (no file format header).");
  }
  const uint8_t* format = xex + format_offset;
  const uint32_t info_size = Be32(format);
  const uint16_t encryption = Be16(format + 4);
  const uint16_t compression = Be16(format + 6);
  if (encryption != kEncryptionNone && encryption != kEncryptionNormal) {
    return fail(std::format("default.xex is encrypted in a way this reader does not know (type {}).", encryption));
  }
  if (compression == kCompressionNormal) {
    return fail("default.xex uses LZX compression, which this reader does not decode.");
  }
  if (compression == kCompressionDelta) {
    return fail("default.xex is a delta patch, not a full executable.");
  }
  if (compression != kCompressionNone && compression != kCompressionBasic) {
    return fail(std::format("default.xex is compressed in a way this reader does not know (type {}).", compression));
  }

  uint32_t base_address = 0;
  if (!FindOptionalHeader(xex, size, header_count, kOptImageBaseAddress, &base_address)) {
    base_address = Be32(security + kSecurityLoadAddress);
  }
  if (base_address == 0) return fail("default.xex does not say where its image is loaded.");
  const uint32_t declared_size = Be32(security + kSecurityImageSize);

  const uint8_t* data = xex + header_size;
  const size_t data_available = size - header_size;

  // The session key. The image starts with "MZ": that tells the right master key.
  uint8_t session[16] = {};
  bool encrypted = encryption == kEncryptionNormal;
  if (encrypted) {
    if (data_available < 16) return fail("default.xex has no image data after its header.");
    bool opened = false;
    for (int dev = 0; dev < 2 && !opened; ++dev) {
      SessionKey(dev == 0 ? kRetailKey : kDevKitKey, security + kSecurityAesKey, session);
      if (OpensToPe(session, data)) {
        opened = true;
        image->dev_kit_key = dev == 1;
      }
    }
    if (!opened) {
      return fail("default.xex did not decrypt to an executable with the retail or the dev kit key.");
    }
  }

  // The layout: (bytes stored, bytes the image takes) for each block.
  struct Block {
    size_t stored;
    size_t span;
  };
  std::vector<Block> blocks;
  if (compression == kCompressionNone) {
    blocks.push_back({data_available, data_available});
  } else {
    if (info_size < 8 || (info_size - 8) % 8 != 0 || size_t(format_offset) + info_size > size) {
      return fail("default.xex has a damaged file format header.");
    }
    const uint32_t count = (info_size - 8) / 8;
    for (uint32_t n = 0; n < count; ++n) {
      const uint8_t* entry = format + 8 + size_t(n) * 8;
      blocks.push_back({size_t(Be32(entry)), size_t(Be32(entry)) + size_t(Be32(entry + 4))});
    }
  }

  uint64_t stored_total = 0;
  uint64_t span_total = 0;
  for (const Block& block : blocks) {
    stored_total += block.stored;
    span_total += block.span;
  }
  if (stored_total > data_available) {
    return fail("default.xex is cut short: its image data runs past the end of the file.");
  }
  const uint64_t image_bytes = std::max<uint64_t>(span_total, declared_size);
  if (span_total == 0 || image_bytes > kMaxImageBytes) {
    return fail("default.xex claims an image of a size that cannot be right.");
  }

  image->base_address = base_address;
  image->data.assign(size_t(image_bytes), 0);
  uint8_t* out = image->data.data();
  const uint8_t* in = data;
  CbcDecryptor chain(session);  // one chain over all the data blocks, as the SDK reads them
  for (const Block& block : blocks) {
    if (encrypted) {
      if (block.stored % 16 != 0) return fail("default.xex has an image block that is not a whole number of cipher blocks.");
      chain.Decrypt(in, out, block.stored);
    } else {
      std::memcpy(out, in, block.stored);
    }
    in += block.stored;
    out += block.span;
  }

  if (image->data.size() < 2 || image->data[0] != 'M' || image->data[1] != 'Z') {
    image->data.clear();
    return fail("default.xex decoded to something that is not an executable image.");
  }
  return true;
}

bool LoadXexImage(const std::filesystem::path& xex_file, XexImage* image, std::string* error) {
  std::vector<uint8_t> bytes;
  if (!LoadFlatImage(xex_file, bytes)) {
    if (error) *error = "default.xex could not be read from " + xex_file.string() + ".";
    return false;
  }
  return DecodeXexImage(bytes.data(), bytes.size(), image, error);
}

bool XexAesSelfTest(std::string* why) {
  // FIPS-197 Appendix C.1: key 000102...0f, plaintext 00112233...eeff.
  uint8_t key[16];
  uint8_t plain[16];
  for (int i = 0; i < 16; ++i) {
    key[i] = uint8_t(i);
    plain[i] = uint8_t(i * 0x11);
  }
  const uint8_t cipher[16] = {0x69, 0xC4, 0xE0, 0xD8, 0x6A, 0x7B, 0x04, 0x30,
                              0xD8, 0xCD, 0xB7, 0x80, 0x70, 0xB4, 0xC5, 0x5A};
  uint8_t out[16];
  Aes128(key).DecryptBlock(cipher, out);
  if (std::memcmp(out, plain, 16) != 0) {
    if (why) *why = "AES-128 decryption does not give the FIPS-197 plaintext.";
    return false;
  }
  // CBC with a chain across calls: two blocks in one call and in two calls decrypt alike.
  uint8_t two[32];
  std::memcpy(two, cipher, 16);
  std::memcpy(two + 16, cipher, 16);
  uint8_t whole[32];
  uint8_t split[32];
  CbcDecryptor(key).Decrypt(two, whole, 32);
  CbcDecryptor chain(key);
  chain.Decrypt(two, split, 16);
  chain.Decrypt(two + 16, split + 16, 16);
  if (std::memcmp(whole, split, 32) != 0 || std::memcmp(whole, plain, 16) != 0) {
    if (why) *why = "AES-128 CBC does not carry its chain from one call to the next.";
    return false;
  }
  return true;
}

}  // namespace larecomp::shaderpack
