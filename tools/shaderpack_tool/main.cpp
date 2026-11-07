// larecomp_shaderpack_tool: the shader pack builder from the command line.
//
// A developer's tool (not shipped): it runs the same BuildShaderPacks the game runs at its first
// start, from a flat copy of the guest image instead of guest memory, so the builder can be tested
// and compared with the Python chain's packs without booting the game.
//
//   build         builds the packs into a folder (the stamp too; a second run says "up to date"); the guest
//                 image comes from default.xex (--xex) or from a flat dump of it (--image)
//   xex-image     decodes default.xex to the guest image, as larecomp.exe --build_shader_packs does
//   check-inputs  reads the RPF's Shaders folder and scans the image, compares with the Python tools' files
//   dxc-selftest  compiles translated .hlsl files with the in-process dxc and compares with an existing pack
//   validate      the renderer's own checks on a pack, plus the magic and FNV of every blob
//   stamp         what the stamp in a folder says about the packs, without building anything
//
// Run without arguments for the options. Exit code 0 means success.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "shaderpack/builder.h"
#include "shaderpack/dxc_selftest.h"
#include "shaderpack/hash.h"
#include "shaderpack/image_scan.h"
#include "shaderpack/input_selftest.h"
#include "shaderpack/pack_writer.h"
#include "shaderpack/rpf_shaders.h"
#include "shaderpack/xex_image.h"

// The HDR variant step: the tool builds the HDR pack too when the build has the HDR curve embedded
// (--no-hdr leaves it out).
#include "shaderpack/hdr_variant.h"

namespace sp = larecomp::shaderpack;
namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Arguments
// ---------------------------------------------------------------------------

struct Args {
  std::string command;
  std::map<std::string, std::string> options;  // "--game" -> value
  std::set<std::string> flags;                 // "--force"
  std::vector<std::string> positional;

  bool Has(const std::string& name) const { return options.count(name) != 0; }
  std::string Get(const std::string& name) const {
    const auto it = options.find(name);
    return it == options.end() ? std::string() : it->second;
  }
  fs::path Path(const std::string& name) const { return fs::path(Get(name)); }
  bool Flag(const std::string& name) const { return flags.count(name) != 0; }
};

const std::set<std::string>& KnownFlags() {
  static const std::set<std::string> flags = {"--force", "--no-blobs", "--no-contract", "--no-hdr"};
  return flags;
}

bool ParseArgs(int argc, char** argv, Args* args, std::string* error) {
  if (argc < 2) {
    *error = "no command";
    return false;
  }
  args->command = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind("--", 0) != 0) {
      args->positional.push_back(arg);
      continue;
    }
    if (KnownFlags().count(arg) != 0) {
      args->flags.insert(arg);
      continue;
    }
    if (i + 1 >= argc) {
      *error = "the option " + arg + " needs a value";
      return false;
    }
    args->options[arg] = argv[++i];
  }
  return true;
}

// "dxil", "spirv", "both"; "header" (validate) means whatever the pack's header lists.
bool ParseFormats(const std::string& text, uint32_t* formats) {
  if (text == "dxil") {
    *formats = sp::kDxil;
  } else if (text == "spirv") {
    *formats = sp::kSpirv;
  } else if (text == "both") {
    *formats = sp::kDxil | sp::kSpirv;
  } else if (text == "header") {
    *formats = 0;
  } else {
    return false;
  }
  return true;
}

std::string FormatsText(uint32_t formats) {
  std::string text;
  if (formats & sp::kDxil) text += "DXIL";
  if (formats & sp::kSpirv) text += text.empty() ? "SPIR-V" : "+SPIR-V";
  return text.empty() ? "none" : text;
}

bool Require(const Args& args, std::initializer_list<const char*> names) {
  bool ok = true;
  for (const char* name : names) {
    if (!args.Has(name)) {
      std::fprintf(stderr, "missing %s\n", name);
      ok = false;
    }
  }
  return ok;
}

unsigned UnsignedOption(const Args& args, const std::string& name, unsigned fallback) {
  if (!args.Has(name)) return fallback;
  return unsigned(std::strtoul(args.Get(name).c_str(), nullptr, 10));
}

void Usage() {
  std::fputs(
      "usage: larecomp_shaderpack_tool <command> [options]\n"
      "\n"
      "  build         --game <dir> (--xex <default.xex> | --image <flat image>) --out <dir> --dxc <dir>\n"
      "                [--formats dxil|spirv|both (default both)] [--threads N] [--force]\n"
      "                [--position-stride N] [--no-hdr]\n"
      "                Builds mcla_shaders.v8.pack (and the HDR pack when built in) and the stamp.\n"
      "                --game   the folder with xarchive_cache.rpf and default.xex\n"
      "                --xex    default.xex itself: its image is decoded here, as larecomp.exe\n"
      "                         --build_shader_packs does it\n"
      "                --image  or the guest image of default.xex, flat, based at 0x82000000\n"
      "                --dxc    the folder with dxcompiler.dll and dxil.dll\n"
      "  xex-image     --xex <default.xex> [--out <file>] [--compare <flat image>]\n"
      "                Decodes the image of default.xex (AES-128 and basic compression, no runtime), writes\n"
      "                it to --out and/or checks it byte for byte against a flat image (native\\work\\image.bin).\n"
      "                Runs the AES self test first.\n"
      "  check-inputs  --game <dir> [--image <flat image>] [--rpf-ref <dir>] [--xex-ref <dir>]\n"
      "                Reads the Shaders folder / scans the image and compares with the files the\n"
      "                Python tools wrote (native\\work\\rpf_shaders, native\\work\\xex_containers).\n"
      "  dxc-selftest  --dxc <dir> --pack <pack> --hlsl <dir> [--sample N] [--threads N]\n"
      "                [--dump <dir>] [--no-contract]\n"
      "                Compiles the .hlsl files below <dir> and compares every blob with <pack>.\n"
      "  validate      <pack> [--formats dxil|spirv|both|header (default header)] [--no-blobs]\n"
      "                The renderer's checks on a pack, and the magic and FNV of every blob.\n"
      "  stamp         --out <dir> --game <dir> --dxc <dir> [--formats dxil|spirv|both]\n"
      "                What the stamp in <dir> says; builds nothing.\n",
      stderr);
}

// ---------------------------------------------------------------------------
// Progress on the console
// ---------------------------------------------------------------------------

class ConsoleSink : public sp::ProgressSink {
 public:
  void BeginPhase(const std::string& name, int total) override {
    total_ = total;
    done_ = 0;
    next_print_ = 0;
    if (total > 0) {
      std::printf("%s (%d)\n", name.c_str(), total);
    } else {
      std::printf("%s\n", name.c_str());
    }
    std::fflush(stdout);
  }
  void Step(const std::string&) override {
    ++done_;
    if (total_ > 0 && done_ >= next_print_) {
      std::printf("  %d / %d\n", done_, total_);
      std::fflush(stdout);
      next_print_ = done_ + std::max(1, total_ / 10);
    }
  }
  void Log(sp::LogLevel level, const std::string& text) override {
    const char* prefix = level == sp::LogLevel::kError ? "error: " : level == sp::LogLevel::kWarn ? "warning: " : "";
    std::printf("  %s%s\n", prefix, text.c_str());
    std::fflush(stdout);
  }
  bool Cancelled() const override { return false; }

 private:
  int total_ = 0;
  int done_ = 0;
  int next_print_ = 0;
};

std::unique_ptr<sp::HdrStep> MakeHdrStep(const Args& args) {
  if (args.Flag("--no-hdr")) return nullptr;
  std::unique_ptr<sp::HdrStep> step = sp::CreateHdrStep();
  if (!step) std::printf("no HDR step: %s\n", sp::HdrStepUnavailableReason().c_str());
  return step;
}

// ---------------------------------------------------------------------------
// build
// ---------------------------------------------------------------------------

int CommandBuild(const Args& args) {
  if (!Require(args, {"--game", "--out", "--dxc"})) return 2;
  if (args.Has("--xex") == args.Has("--image")) {
    std::fprintf(stderr, "give the guest image as --xex <default.xex> or as --image <flat image> (one of them)\n");
    return 2;
  }
  uint32_t formats = sp::kDxil | sp::kSpirv;
  if (args.Has("--formats") && (!ParseFormats(args.Get("--formats"), &formats) || formats == 0)) {
    std::fprintf(stderr, "--formats takes dxil, spirv or both\n");
    return 2;
  }

  std::vector<uint8_t> image;
  if (args.Has("--xex")) {
    // As larecomp.exe --build_shader_packs: the image is decoded from default.xex, and the part the
    // game scans (0x82000000 to 0x829E0000) is what the builder sees.
    sp::XexImage decoded;
    std::string why;
    if (!sp::LoadXexImage(args.Path("--xex"), &decoded, &why)) {
      std::fprintf(stderr, "%s\n", why.c_str());
      return 1;
    }
    image = std::move(decoded.data);
    image.resize(sp::kGuestImageSize, 0);
  } else if (!sp::LoadFlatImage(args.Path("--image"), image)) {
    std::fprintf(stderr, "cannot read the image file %s\n", args.Get("--image").c_str());
    return 1;
  }

  ConsoleSink sink;
  const std::unique_ptr<sp::HdrStep> hdr = MakeHdrStep(args);
  sp::BuildRequest request;
  request.game_root = args.Path("--game");
  request.out_dir = args.Path("--out");
  request.dxc_dir = args.Path("--dxc");
  request.image.data = image.data();
  request.image.size = image.size();
  request.image.base_address = sp::kGuestImageBase;
  request.formats = formats;
  request.threads = UnsignedOption(args, "--threads", 0);
  request.force = args.Flag("--force");
  request.position_proof_stride = UnsignedOption(args, "--position-stride", 1);
  request.sink = &sink;
  request.hdr_step = hdr.get();
  request.app_version = "shaderpack_tool";

  std::printf("building %s from %s into %s\n", FormatsText(formats).c_str(), args.Get("--game").c_str(),
              args.Get("--out").c_str());
  const sp::BuildResult result = sp::BuildShaderPacks(request);

  std::printf("\nstatus     : %s\n", sp::StatusName(result.status));
  if (!result.message.empty()) std::printf("message    : %s\n", result.message.c_str());
  if (!result.first_shader.empty()) std::printf("shader     : %s\n", result.first_shader.c_str());
  for (const std::string& problem : result.problems) std::printf("also       : %s\n", problem.c_str());
  if (result.ok() && result.status != sp::Status::kUpToDate) {
    std::printf("shaders    : %zu (%zu vertex, %zu pixel) from %zu containers\n", result.shaders,
                result.vertex_shaders, result.pixel_shaders, result.containers_seen);
    std::printf("formats    : %s\n", FormatsText(result.formats_built).c_str());
    std::printf("blobs      : %u compiled, %u taken from the pack that was there\n", result.blobs_compiled,
                result.blobs_reused);
    std::printf("pack       : %llu bytes, xxh3 %s\n", static_cast<unsigned long long>(result.pack_bytes),
                sp::Hex64(result.pack_xxh3).c_str());
    std::printf("hdr pack   : %s%s%s\n", result.hdr_built ? "built" : "not built",
                result.hdr_message.empty() ? "" : " - ", result.hdr_message.c_str());
    std::printf("threads    : %u\n", result.threads);
    std::printf("time       : %.2f s (read %.2f, compile %.2f, write %.2f, hdr %.2f)\n", result.seconds_total,
                result.seconds_read, result.seconds_compile, result.seconds_write, result.seconds_hdr);
  }
  return result.ok() ? 0 : 1;
}

// ---------------------------------------------------------------------------
// xex-image
// ---------------------------------------------------------------------------

int CommandXexImage(const Args& args) {
  if (!Require(args, {"--xex"})) return 2;
  std::string why;
  if (!sp::XexAesSelfTest(&why)) {
    std::printf("AES SELF TEST: FAIL: %s\n", why.c_str());
    return 1;
  }
  std::printf("AES self test: pass\n");

  sp::XexImage image;
  if (!sp::LoadXexImage(args.Path("--xex"), &image, &why)) {
    std::printf("cannot decode %s: %s\n", args.Get("--xex").c_str(), why.c_str());
    return 1;
  }
  std::printf("image: %zu bytes at 0x%08X, %s key; xxh3 %s\n", image.data.size(), image.base_address,
              image.dev_kit_key ? "dev kit" : "retail",
              sp::Hex64(sp::Xxh3(image.data.data(), image.data.size())).c_str());

  bool ok = true;
  if (args.Has("--out")) {
    std::ofstream out(args.Path("--out"), std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(image.data.data()), static_cast<std::streamsize>(image.data.size()));
    if (!out) {
      std::printf("cannot write %s\n", args.Get("--out").c_str());
      ok = false;
    } else {
      std::printf("written to %s\n", args.Get("--out").c_str());
    }
  }
  if (args.Has("--compare")) {
    std::vector<uint8_t> reference;
    if (!sp::LoadFlatImage(args.Path("--compare"), reference)) {
      std::printf("cannot read %s\n", args.Get("--compare").c_str());
      ok = false;
    } else {
      size_t different = 0;
      size_t first = 0;
      const size_t common = std::min(reference.size(), image.data.size());
      for (size_t i = 0; i < common; ++i) {
        if (reference[i] != image.data[i]) {
          if (different == 0) first = i;
          ++different;
        }
      }
      const bool same = different == 0 && reference.size() == image.data.size();
      std::printf("compared with %s (%zu bytes): %s", args.Get("--compare").c_str(), reference.size(),
                  same ? "identical\n" : "DIFFERENT");
      if (!same) std::printf(" - %zu bytes differ (first at 0x%zX), sizes %zu / %zu\n", different, first,
                             image.data.size(), reference.size());
      ok = ok && same;
    }
  }
  return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// check-inputs
// ---------------------------------------------------------------------------

int CommandCheckInputs(const Args& args) {
  if (!args.Has("--game") && !args.Has("--image")) {
    std::fprintf(stderr, "give --game and/or --image\n");
    return 2;
  }
  sp::InputSelfTestArgs test;
  test.game_root = args.Path("--game");
  test.image_file = args.Path("--image");
  test.rpf_reference_dir = args.Path("--rpf-ref");
  test.xex_reference_dir = args.Path("--xex-ref");
  return sp::RunInputSelfTest(test);
}

// ---------------------------------------------------------------------------
// dxc-selftest
// ---------------------------------------------------------------------------

int CommandDxcSelfTest(const Args& args) {
  if (!Require(args, {"--dxc", "--pack", "--hlsl"})) return 2;

  std::vector<fs::path> files;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(args.Path("--hlsl"), ec), end; !ec && it != end; it.increment(ec)) {
    if (it->is_regular_file(ec) && it->path().extension() == ".hlsl") files.push_back(it->path());
  }
  if (files.empty()) {
    std::fprintf(stderr, "no .hlsl files below %s\n", args.Get("--hlsl").c_str());
    return 1;
  }
  std::sort(files.begin(), files.end());

  // N files spread evenly over the sorted list.
  const unsigned sample = UnsignedOption(args, "--sample", 0);
  std::vector<fs::path> chosen;
  if (sample == 0 || sample >= files.size()) {
    chosen = files;
  } else {
    for (unsigned i = 0; i < sample; ++i) chosen.push_back(files[size_t(i) * files.size() / sample]);
  }

  sp::SelfTestOptions options;
  options.dxc_dir = args.Path("--dxc");
  options.pack = args.Path("--pack");
  for (const fs::path& file : chosen) options.files.push_back(sp::SelfTestFile{file, std::nullopt});
  options.threads = UnsignedOption(args, "--threads", 1);
  options.check_contract = !args.Flag("--no-contract");
  options.dump_dir = args.Path("--dump");
  options.log = [](std::string_view line) { std::printf("%.*s\n", int(line.size()), line.data()); };

  const sp::SelfTestResult result = sp::RunDxcSelfTest(options);
  if (!result.error.empty()) std::fprintf(stderr, "self test could not run: %s\n", result.error.c_str());
  std::printf("%s\n", result.ok ? "DXC SELF TEST: PASS" : "DXC SELF TEST: FAIL");
  return result.ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// validate
// ---------------------------------------------------------------------------

int CommandValidate(const Args& args) {
  if (args.positional.empty()) {
    std::fprintf(stderr, "give the pack to check\n");
    return 2;
  }
  sp::ValidateOptions options;
  if (args.Has("--formats") && !ParseFormats(args.Get("--formats"), &options.formats)) {
    std::fprintf(stderr, "--formats takes dxil, spirv, both or header\n");
    return 2;
  }
  options.verify_blobs = !args.Flag("--no-blobs");

  const fs::path pack = fs::path(args.positional[0]);
  const sp::ValidateResult result = sp::ValidatePack(pack, options);
  std::printf("%s: %llu bytes, xxh3 %s, version %u, formats %s, %u entries (%u vertex, %u pixel), %u blobs; "
              "bad magic %u, bad FNV %u\n",
              args.positional[0].c_str(), static_cast<unsigned long long>(result.bytes),
              sp::Hex64(result.xxh3).c_str(), result.version, FormatsText(result.formats).c_str(),
              result.entries, result.vertex_shaders, result.pixel_shaders, result.blobs, result.bad_magic,
              result.bad_fnv);
  for (const std::string& problem : result.problems) std::printf("ERROR %s\n", problem.c_str());
  if (result.problem_count > result.problems.size()) {
    std::printf("... and %zu more\n", result.problem_count - result.problems.size());
  }
  std::printf("%s\n", result.ok ? "OK: the renderer's Validate would accept it" : "NOT OK");
  return result.ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// stamp
// ---------------------------------------------------------------------------

int CommandStamp(const Args& args) {
  if (!Require(args, {"--out", "--game", "--dxc"})) return 2;
  uint32_t formats = sp::kDxil | sp::kSpirv;
  if (args.Has("--formats") && (!ParseFormats(args.Get("--formats"), &formats) || formats == 0)) {
    std::fprintf(stderr, "--formats takes dxil, spirv or both\n");
    return 2;
  }
  const std::unique_ptr<sp::HdrStep> hdr = MakeHdrStep(args);
  const sp::StampCheck check = sp::CheckStamp(args.Path("--out"), formats,
                                              sp::GameFilesQuick{args.Path("--game"), args.Path("--dxc")}, hdr.get());
  for (const sp::StampLine& line : check.stamp.lines()) std::printf("  %s=%s\n", line.key.c_str(), line.value.c_str());
  std::printf("main pack   : %s\n", check.main_ok ? "current" : "stale");
  std::printf("hdr pack    : %s\n", check.hdr_ok ? "current" : "stale");
  std::printf("formats     : stamp %s, asked %s\n", FormatsText(check.stamped_formats).c_str(),
              FormatsText(formats).c_str());
  if (check.content_check_needed) std::printf("note        : only the game files' times differ\n");
  if (!check.why.empty()) std::printf("why         : %s\n", check.why.c_str());
  return check.main_ok && check.hdr_ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  std::string error;
  if (!ParseArgs(argc, argv, &args, &error)) {
    if (argc >= 2) std::fprintf(stderr, "%s\n\n", error.c_str());
    Usage();
    return 2;
  }
  if (args.command == "build") return CommandBuild(args);
  if (args.command == "xex-image") return CommandXexImage(args);
  if (args.command == "check-inputs") return CommandCheckInputs(args);
  if (args.command == "dxc-selftest") return CommandDxcSelfTest(args);
  if (args.command == "validate") return CommandValidate(args);
  if (args.command == "stamp") return CommandStamp(args);
  std::fprintf(stderr, "unknown command %s\n\n", args.command.c_str());
  Usage();
  return 2;
}
