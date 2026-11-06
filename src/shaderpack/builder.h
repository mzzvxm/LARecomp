#pragma once
// ===========================================================================
// LARecomp shader pack builder -- the public interface
// ===========================================================================
// LARecomp builds its shader packs itself, on the player's PC, from the player's own game files:
//
//   <game_root>/xarchive_cache.rpf  (the Shaders folder: 2214 of the 2333 shader containers)
//   the guest image of default.xex  (the other 119, found in guest memory: no XEX decryption)
//   dxcompiler.dll + dxil.dll       in `dxc_dir`, loaded at run time
//
// The result is <out_dir>/mcla_shaders.v8.pack (what src/gpu/shaders.cpp reads) and, when an HDR
// step is given, mcla_shaders_hdr.v1.pack, plus the stamp that says what they were built from.
// With the same dxc the bytes are those of native/tools/build_shader_pack_v8.py (the Python chain
// by mzzvxm that this replaces: v2, v3 and v8 in one pass).
//
// Usage (the app, at LaunchModule; the tool, from a flat image file):
//   StampCheck check = CheckStamp(out_dir, formats, {game_root, dxc_dir}, hdr_step);   // stat only
//   if (!(check.main_ok && check.hdr_ok)) {
//     BuildRequest request; ... ; BuildResult result = BuildShaderPacks(request);
//   }
//
// One call builds one set of packs. It runs its own workers (one dxc compiler instance each, the
// result order never depends on which finishes first: the pack is byte-identical with 1 or 16
// threads), reports through the sink and polls it for cancel.
//
// Which formats: the first start builds the active backend's format only (DXIL for D3D12, SPIR-V
// for Vulkan). When a pack that is otherwise up to date lacks a format the start needs, the build
// adds it: the formats it already has are taken from the existing pack as they are (blobs are a
// pure function of their inputs), only the new format is compiled.
//
// Failures never leave a pack that validates: the files are written under temporary names,
// validated, moved into place, and the stamp is written last. A cancelled or failed build leaves no
// stamp.
// ===========================================================================

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "shaderpack/containers.h"
#include "shaderpack/dxc.h"
#include "shaderpack/log.h"
#include "shaderpack/pack_format.h"
#include "shaderpack/stamp.h"

namespace larecomp::shaderpack {

// What the build reports to. The app forwards it to mc::boot::BeginPhase/Step; the tool prints.
// Every call comes from the thread that called BuildShaderPacks.
class ProgressSink {
 public:
  virtual ~ProgressSink() = default;
  // A new phase of `total` steps (0: unknown).
  virtual void BeginPhase(const std::string& name, int total) = 0;
  // One step done; `item` names it (a shader's name).
  virtual void Step(const std::string& item) = 0;
  virtual void Log(LogLevel level, const std::string& text) = 0;
  // Polled about every 25 ms and between phases; true stops the build (a cancelled build leaves
  // nothing behind).
  virtual bool Cancelled() const = 0;
};

// A flat copy of the guest image, or guest memory itself: default.xex loaded at 0x82000000.
struct ImageView {
  const uint8_t* data = nullptr;
  size_t size = 0;
  uint32_t base_address = kGuestImageBase;  // the guest address of data[0]; only labels containers
};

// ---------------------------------------------------------------------------
// The HDR variant step (written separately: hdr_variant.{h,cpp})
// ---------------------------------------------------------------------------
// Builds mcla_shaders_hdr.v1.pack after the main pack. The builder calls it at one place (the
// "HDR VARIANT STEP" in builder.cpp), with the containers it has already read, a loaded compiler
// and the main pack's fingerprint, and records the lines it returns in the stamp.

struct HdrStepInput {
  const ContainerSet* containers = nullptr;  // for an HDR-only rebuild only the image's shaders are in it
  std::shared_ptr<DxcRuntime> dxc;           // loaded; make a DxcWorker per thread
  std::filesystem::path out_dir;             // mcla_shaders_hdr.v1.pack goes here (kHdrPackFileName)
  uint32_t formats = 0;                      // build these formats (the main pack's)
  unsigned threads = 1;
  ProgressSink* sink = nullptr;              // never null
};

struct HdrStepResult {
  bool built = false;                  // the file is in place
  std::string message;                 // why not, or a one line summary
  std::vector<StampLine> stamp_lines;  // what it was built from (hlsli hash, contract, ...)
};

class HdrStep {
 public:
  virtual ~HdrStep() = default;
  // The lines (hlsli hash, contract...) that are true for THIS build of larecomp: CheckStamp
  // compares them with the stamp's, so a changed curve rebuilds the HDR pack alone.
  virtual std::vector<StampLine> ExpectedStampLines() const = 0;
  // Writes the HDR pack to input.out_dir under its final name (a .tmp then a move is its own
  // business). A failure here fails the HDR step, not the build: HDR then stays at stage 2, which
  // the reader allows.
  virtual HdrStepResult Build(const HdrStepInput& input) = 0;
};

// ---------------------------------------------------------------------------
// The request and the result
// ---------------------------------------------------------------------------

struct BuildRequest {
  std::filesystem::path game_root;  // holds xarchive_cache.rpf and default.xex (stat for the stamp)
  std::filesystem::path out_dir;    // the packs and the stamp
  std::filesystem::path dxc_dir;    // dxcompiler.dll (+ dxil.dll)
  ImageView image;
  uint32_t formats = kDxil | kSpirv;  // the Format mask this start needs
  unsigned threads = 0;               // 0: automatic (cores, and ~300 MB of free memory each), at most 16
  bool force = false;                 // build even when the stamp says the packs are current
  // Run the DXIL position proof (disassemble and walk the fp ops that reach SV_Position) on one vertex
  // shader in this many; 1 is every one, as the Python chain does. The SPIR-V check is cheap and
  // always done.
  uint32_t position_proof_stride = 1;
  ProgressSink* sink = nullptr;
  HdrStep* hdr_step = nullptr;  // null: no HDR pack, no hdr_* lines
  std::string app_version;      // goes into the stamp, informational
};

enum class Status {
  kOk,
  kUpToDate,        // nothing to do: the stamp says the packs are current
  kCancelled,
  kBadRequest,      // no formats asked, no image, DXIL off Windows, out_dir not creatable
  kMissingInput,    // xarchive_cache.rpf unreadable / no Shaders folder / no containers
  kMissingDxc,      // dxcompiler.dll or dxil.dll missing or not loadable
  kDxilNotSigned,   // dxil.dll was not found by dxcompiler: the DXIL would be rejected
  kTranslateFailed,
  kCompileFailed,
  kContractFailed,  // an HLSL text, blob or position check of the shader contract failed
  kIoError,
};
const char* StatusName(Status status);

struct BuildResult {
  Status status = Status::kOk;
  std::string message;       // plain English, for the dialog; ends with the first failing shader
  std::string first_shader;  // "name [key]" of the shader that failed, else empty
  std::vector<std::string> problems;  // more of the failures (at most 10 lines)

  uint32_t formats_built = 0;   // the Format mask of the pack on disk
  size_t containers_seen = 0;
  size_t shaders = 0;           // distinct keys: 2056 on the retail game
  size_t vertex_shaders = 0;
  size_t pixel_shaders = 0;
  uint32_t blobs_compiled = 0;
  uint32_t blobs_reused = 0;    // taken from the existing pack (a format that was already there)
  uint64_t pack_bytes = 0;
  uint64_t pack_xxh3 = 0;
  unsigned threads = 0;
  bool hdr_built = false;
  std::string hdr_message;
  bool hdr_only = false;        // only the HDR pack was rebuilt

  double seconds_read = 0;      // game files and containers
  double seconds_compile = 0;   // translate, check, compile, prove
  double seconds_write = 0;     // pack, validate, stamp
  double seconds_hdr = 0;
  double seconds_total = 0;

  bool ok() const { return status == Status::kOk || status == Status::kUpToDate; }
};

// The files a start can look at without opening them.
struct GameFilesQuick {
  std::filesystem::path game_root;
  std::filesystem::path dxc_dir;
};

// What the stamp says (stat only; the hash of a DLL is read only when its size or time moved).
struct StampCheck {
  bool main_ok = false;  // the main pack is current and carries the formats asked
  // The HDR pack is current too (true when there is no HDR step). Judged whenever inputs_match, so
  // it can be true while main_ok is false (the game files' times moved); "nothing to do" is
  // main_ok && hdr_ok.
  bool hdr_ok = false;
  // Everything but `formats` and the game files' times matches: the pack is the right one in every
  // respect that does not need the game files read.
  bool inputs_match = false;
  bool formats_ok = false;            // the stamp's formats cover the ones asked
  bool content_check_needed = false;  // only a time of the game files moved: reading them decides
  uint32_t stamped_formats = 0;
  std::string why;  // for a log line; names the first line that is stale
  Stamp stamp;      // what was read (empty when there is no stamp)
};

StampCheck CheckStamp(const std::filesystem::path& out_dir, uint32_t formats_needed,
                      const GameFilesQuick& files, const HdrStep* hdr_step);

BuildResult BuildShaderPacks(const BuildRequest& request);

}  // namespace larecomp::shaderpack
