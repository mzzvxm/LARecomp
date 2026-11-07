#pragma once
// ===========================================================================
// default.xex -> the loaded guest image, without the runtime
// ===========================================================================
// The shader builder reads 119 of its shader containers out of the game's executable, from the
// image the runtime loads at 0x82000000. Inside the game that is guest memory (the app builds at
// LaunchModule). A program that runs before there is a runtime or a window -- larecomp.exe
// --build_shader_packs, which the launcher runs during setup, and the developer tool -- has to make
// the same bytes itself. This is that: the XEX2 container read as the SDK's XexModule::ReadImage
// reads it (src/system/xex_module.cpp), for the layouts a retail game uses:
//
//   encryption   none, or AES-128-CBC (zero IV) with the session key = the file key in the security
//                info decrypted with the retail key (the dev kit key, all zeros, is tried second)
//   compression  none, or "basic" (data blocks with runs of zeros between them)
//
// LZX ("normal") compression and delta patches are not read: the result says so, and the caller
// leaves the shaders to the game's first start. Midnight Club: Los Angeles' default.xex is
// encrypted and basic compressed (two blocks, 0x9E0000 bytes loaded), which is why this is enough;
// the output equals the flat image the earlier Python chain used (native\work\image.bin, 10,354,688
// bytes), which tools/shaderpack_tool's xex-image command checks.
//
// No dependency on the runtime or on a crypto library (a small AES-128 decrypt is included), so
// the same file builds into larecomp.exe and into the command line tool.
// ===========================================================================

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace larecomp::shaderpack {

struct XexImage {
  std::vector<uint8_t> data;    // the loaded image: data[0] is at base_address
  uint32_t base_address = 0;    // 0x82000000 for this game
  bool dev_kit_key = false;     // the image only opened with the dev kit key
};

// Reads default.xex and decodes its image. False with a plain English reason in *error: not an XEX2
// file, an unsupported layout (LZX, patch), damaged or cut off, or a key that does not open it.
bool LoadXexImage(const std::filesystem::path& xex_file, XexImage* image, std::string* error);

// The same from a file already in memory.
bool DecodeXexImage(const uint8_t* xex, size_t size, XexImage* image, std::string* error);

// AES-128 against the FIPS-197 test vector (Appendix C.1). For the tool's self test; false with the
// reason in *why.
bool XexAesSelfTest(std::string* why);

}  // namespace larecomp::shaderpack
