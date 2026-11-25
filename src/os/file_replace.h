#pragma once
// ===========================================================================
// LARecomp -- writing a file that parallel processes share
// ===========================================================================
// The caches and packs live in folders that every running game (and the launcher's setup, and a test batch's parallel runs) shares: the city pack, the hit file, the
// texture file and their stamp (src/citypack), the pipeline caches and their stamp (src/gpu, src/native_gfx), the shader packs and their stamp (src/shaderpack) and the
// mod loader's template cache. Each is written under a temporary name and then moved over the real name. Three things went wrong with the plain version (the temporary
// "<name>.tmp", remove the old file, rename):
//   * two processes that build the same data at the same time (a cold cache, parallel runs) wrote the SAME temporary file, so one moved the
//     other's file away under it ("the texture file could not be moved into place", 2026-11-22, integ-ab-vk-off2);
//   * the old file is open in another process (a reader keeps the hit file, the texture file and the shader pack open for its whole session) and Windows will
//     not replace or remove a file that is open without delete sharing: the move fails, and the build with it; between a remove and a rename there is no file
//     at all, and a process that starts then finds no cache (and builds its own);
//   * the data is the same in both cases (the builders are deterministic: the same inputs give the same bytes, with one thread or four), so a
//     failed move over an identical file is not a failure.
// So: a temporary name that is unique per process and per call (TempPathFor), a move that replaces the file in one step (MoveFileExW with
// MOVEFILE_REPLACE_EXISTING; rename on POSIX, which replaces) and tries again a few times when the old file is only busy, and, when it stays
// busy, a look at the file in place: the same bytes means another process wrote it first and the job is done (MoveResult::kAlreadyThere).
// WriteFileReplacing is the whole thing for a file that is written from memory.
//
// The readers' side: a file that is replaced in one step is never seen half written (a reader that has it open keeps the old one, a reader that opens it after gets the
// new one), so a reader's own checks (magic, version, sizes inside the file, a hash where the format has one) only fail for a file that is really damaged, and they
// have to fail cleanly: a file that is not what it should be is left out (and written again later), it is not a crash.
// Nothing here needs the game, the SDK or a renderer.
// ===========================================================================

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>

namespace larecomp::os {

// The temporary name for writing `target`: "<target>.<process id>.<n>.tmp", unique per process and per call, in the folder of `target`. No file is made or looked at.
std::filesystem::path UniqueTempPath(const std::filesystem::path& target);

// UniqueTempPath, after it removed the temporary files of `target` that a killed build left (SweepStaleTemps, 2 hours old or more: a live writer's file is younger, it
// is written to all the time).
std::filesystem::path TempPathFor(const std::filesystem::path& target);

// Removes the files "<target name>.tmp" and "<target name>.<anything>.tmp" next to `target` whose last write is older than `older_than`.
// Returns how many it removed.
int SweepStaleTemps(const std::filesystem::path& target, std::chrono::seconds older_than);

// Whether `name` (a file name, no folder) is a temporary file of the file called `base` ("<base>.tmp" or "<base>.<anything>.tmp").
bool IsTempNameOf(const std::string& name, const std::string& base);

// Why one try to move the temporary file over the target failed.
enum class MoveError {
  kNone,   // it moved
  kBusy,   // the target (or the temporary file) is open in another process: access denied, a sharing or lock violation; it may pass
  kOther,  // anything else: no use trying again
};

// After the try number `attempts_made` (1 = the first) failed with `error`: try again? Only a busy file is tried again, and only while
// attempts are left.
bool ShouldRetryMove(MoveError error, int attempts_made, int max_attempts);

// The same bytes in both files (sizes first, then the content in chunks). False when either cannot be read.
bool SameFileContent(const std::filesystem::path& a, const std::filesystem::path& b);

enum class MoveResult {
  kMoved,         // the temporary file is the target now
  kAlreadyThere,  // the move failed, but the target holds exactly the bytes of the temporary file (another process wrote it first); the temporary file is removed
  kFailed,        // the target is not (or not exactly) what was written; the temporary file is removed, the target untouched
};

// The file is in place: moved there by this call, or there already with the same bytes.
inline bool Succeeded(MoveResult result) { return result != MoveResult::kFailed; }

struct MoveOptions {
  int max_attempts = 6;  // tries in all; the waits between them grow (retry_ms, 2 x, 3 x ...): 0.6 s at most with the default
  int retry_ms = 40;
};

// What the tests replace: one try to move, and the wait between tries. Left empty they are the real ones.
struct MoveHooks {
  std::function<MoveError(const std::filesystem::path& from, const std::filesystem::path& to, std::string* why)> try_move;
  std::function<void(int ms)> wait;
};

// Moves `temp` over `target` as the header above says. `why` (may be null) gets the system's reason for a kFailed. `attempts` (may be null) gets the number of tries made.
MoveResult MoveIntoPlace(const std::filesystem::path& temp, const std::filesystem::path& target, std::string* why, const MoveOptions& options = {},
                         const MoveHooks& hooks = {}, int* attempts = nullptr);

// Writes `size` bytes at `data` to a temporary file of its own beside `target` (the folder is made when it is missing), then MoveIntoPlace. The temporary file is gone
// afterwards, whatever came out. `why` (may be null) gets the reason for a kFailed: what could not be written, or the move's. `data` may be null when `size` is 0 (an
// empty file).
MoveResult WriteFileReplacing(const std::filesystem::path& target, const void* data, size_t size, std::string* why, const MoveOptions& options = {},
                              const MoveHooks& hooks = {});

}  // namespace larecomp::os
