// Check of the two shader input readers (rpf_shaders, image_scan) against files
// that were produced another way, for the main session and for CI on a machine
// that has the game. Not part of the game's own start-up.
//
// Reference folders (this PC):
//   native\work\rpf_shaders    407 files, written by extract_rpf_shaders.py
//   native\work\xex_containers 119 files named <guest address>.xsc, written by carve_xex.py
//   native\work\image.bin      the flat guest image those 119 were carved from
#pragma once

#include <filesystem>

namespace larecomp::shaderpack {

struct InputSelfTestArgs {
  std::filesystem::path game_root;          // folder with xarchive_cache.rpf; empty skips the RPF half
  std::filesystem::path image_file;         // flat guest image at 0x82000000; empty skips the image half
  std::filesystem::path rpf_reference_dir;  // optional: compare every file byte for byte
  std::filesystem::path xex_reference_dir;  // optional: compare every container byte for byte
};

// Prints a report on stdout. Returns 0 when every requested check passed, 1
// when one failed, 2 when nothing was requested.
int RunInputSelfTest(const InputSelfTestArgs& args);

}  // namespace larecomp::shaderpack
