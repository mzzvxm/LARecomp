#include "shaderpack/input_selftest.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "shaderpack/image_scan.h"
#include "shaderpack/rpf_shaders.h"

namespace larecomp::shaderpack {

namespace {

namespace fs = std::filesystem;

enum class Compare { kEqual, kDiffers, kMissing };

Compare CompareWithFile(const fs::path& reference, const std::vector<uint8_t>& bytes) {
  std::error_code ec;
  const std::uintmax_t size = fs::file_size(reference, ec);
  if (ec) return Compare::kMissing;
  if (size != bytes.size()) return Compare::kDiffers;
  std::ifstream input(reference, std::ios::binary);
  if (!input) return Compare::kMissing;
  std::vector<uint8_t> expected(bytes.size());
  input.read(reinterpret_cast<char*>(expected.data()), static_cast<std::streamsize>(expected.size()));
  if (static_cast<size_t>(input.gcount()) != expected.size()) return Compare::kMissing;
  return expected == bytes ? Compare::kEqual : Compare::kDiffers;
}

// Files below `dir`, as '/' paths relative to it.
std::set<std::string> ListFiles(const fs::path& dir) {
  std::set<std::string> names;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    names.insert(fs::relative(it->path(), dir, ec).generic_string());
  }
  return names;
}

double Seconds(std::chrono::steady_clock::time_point since) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

// Compares `items` (name, bytes) with the files of `reference`, one file per
// item, and with the reference's own file list. Prints the first few problems
// and returns true when everything matches.
struct Item {
  std::string name;
  const std::vector<uint8_t>* bytes;
};

bool CompareWithFolder(const std::vector<Item>& items, const fs::path& reference) {
  size_t equal = 0, differs = 0, missing = 0, only_in_reference = 0;
  std::set<std::string> produced;
  for (const Item& item : items) {
    produced.insert(item.name);
    switch (CompareWithFile(reference / fs::path(item.name), *item.bytes)) {
      case Compare::kEqual:
        ++equal;
        break;
      case Compare::kDiffers:
        if (differs++ < 10) std::printf("  differs: %s\n", item.name.c_str());
        break;
      case Compare::kMissing:
        if (missing++ < 10) std::printf("  not in the reference: %s\n", item.name.c_str());
        break;
    }
  }
  for (const std::string& name : ListFiles(reference)) {
    if (produced.count(name)) continue;
    if (only_in_reference++ < 10) std::printf("  only in the reference: %s\n", name.c_str());
  }
  std::printf("against %s: %zu equal, %zu differ, %zu missing there, %zu only there\n",
              reference.string().c_str(), equal, differs, missing, only_in_reference);
  return differs == 0 && missing == 0 && only_in_reference == 0;
}

bool CheckRpf(const InputSelfTestArgs& args) {
  std::printf("== Shaders folder of xarchive_cache.rpf (%s)\n", args.game_root.string().c_str());
  const auto started = std::chrono::steady_clock::now();
  RpfShaders shaders;
  const RpfShadersResult result = ReadRpfShaders(args.game_root, shaders);
  if (!result.ok()) {
    std::printf("FAIL: %s\n", result.message.c_str());
    return false;
  }
  std::printf("read %zu files, %llu bytes (%.2f MB): %zu stored, %zu inflated, %.2f s\n",
              shaders.files.size(), static_cast<unsigned long long>(shaders.total_bytes),
              static_cast<double>(shaders.total_bytes) / 1.0e6, shaders.stored_count,
              shaders.deflated_count, Seconds(started));

  bool pass = true;
  if (shaders.files.size() != kExpectedRpfShaderFiles) {
    std::printf("FAIL: expected %zu files\n", kExpectedRpfShaderFiles);
    pass = false;
  }
  for (size_t i = 1; i < shaders.files.size(); ++i) {
    if (!(shaders.files[i - 1].path < shaders.files[i].path)) {
      std::printf("FAIL: files are not in strict path order at %s\n", shaders.files[i].path.c_str());
      pass = false;
      break;
    }
  }

  if (!args.rpf_reference_dir.empty()) {
    std::vector<Item> items;
    items.reserve(shaders.files.size());
    for (const RpfShaderFile& file : shaders.files) items.push_back(Item{file.path, &file.bytes});
    pass = CompareWithFolder(items, args.rpf_reference_dir) && pass;
  }
  std::printf("%s\n", pass ? "RPF: PASS" : "RPF: FAIL");
  return pass;
}

bool CheckImage(const InputSelfTestArgs& args) {
  std::printf("== shader containers in the guest image (%s)\n", args.image_file.string().c_str());
  std::vector<uint8_t> image;
  if (!LoadFlatImage(args.image_file, image)) {
    std::printf("FAIL: cannot read the image file\n");
    return false;
  }
  const auto started = std::chrono::steady_clock::now();
  const ImageScan scan = ScanImage(image.data(), image.size());
  size_t total = 0;
  for (const ImageContainer& container : scan.containers) total += container.bytes.size();
  std::printf("image %zu bytes (the guest range is %zu), %zu containers, %zu bytes, %.3f s\n",
              image.size(), kGuestImageSize, scan.containers.size(), total, Seconds(started));

  bool pass = true;
  if (scan.containers.size() != kExpectedImageContainers) {
    std::printf("FAIL: expected %zu containers\n", kExpectedImageContainers);
    pass = false;
  }

  if (!args.xex_reference_dir.empty()) {
    std::vector<Item> items;
    items.reserve(scan.containers.size());
    for (const ImageContainer& container : scan.containers) {
      char name[32];
      std::snprintf(name, sizeof(name), "%08X.xsc", static_cast<unsigned>(container.guest_address));
      items.push_back(Item{name, &container.bytes});
    }
    pass = CompareWithFolder(items, args.xex_reference_dir) && pass;
  }
  std::printf("%s\n", pass ? "IMAGE: PASS" : "IMAGE: FAIL");
  return pass;
}

}  // namespace

int RunInputSelfTest(const InputSelfTestArgs& args) {
  const bool rpf = !args.game_root.empty();
  const bool image = !args.image_file.empty();
  if (!rpf && !image) return 2;

  bool pass = true;
  if (rpf) pass = CheckRpf(args) && pass;
  if (image) pass = CheckImage(args) && pass;
  std::fflush(stdout);
  return pass ? 0 : 1;
}

}  // namespace larecomp::shaderpack
