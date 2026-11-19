#pragma once

#include <chrono>
#include <cstdint>
#include <thread>
#include <rex/logging/log_category.h>
#include <rex/kernel/title_runtime.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/file_open.h>
#include <rex/kernel/object/file_object.h>
#include <rex/kernel/object/object_pin.h>
#include <rex/kernel/object/object_table.h>
#include <rex/xam/content_manager.h>

// A career save being written: a file open for writing on a content package,
// which the SDK mounts at \Device\Content\<n>\ (xam/content_manager.cpp), or
// (an SDK that keeps the previous save aside while the game rewrites it) a save
// the game has not closed yet: ending then would bring the old save back at the
// next start.
inline bool MclaSaveInProgress() {
  auto* runtime = REX_RUNTIME();
  if (!runtime) {
    return false;
  }
#ifdef REX_CONTENT_MANAGER_HAS_UNCLOSED_BACKUPS
  if (runtime->GetContentManager().HasUnclosedBackups()) {
    return true;
  }
#endif
  // FILE_WRITE_DATA, FILE_APPEND_DATA (IFile::GetAccess already expands GENERIC_WRITE and GENERIC_ALL to these).
  constexpr uint32_t kWriteAccess = static_cast<uint32_t>(rex::filesystem::Access::kWriteData) |
                                    static_cast<uint32_t>(rex::filesystem::Access::kAppendData);
  // The open files: every FileObject of the table, on a mount whose path (FileStat::absolute_path is the mount path joined
  // with the file's) is a content package's.
  for (const auto& open_file : runtime->GetObjectTable().All<rex::kernel::FileObject>()) {
    if (!open_file || !open_file->file) {
      continue;
    }
    const rex::filesystem::IFile& file = *open_file->file;
    if ((static_cast<uint32_t>(file.GetAccess()) & kWriteAccess) &&
        file.GetInfo().absolute_path.starts_with("\\Device\\Content\\")) {
      return true;
    }
  }
  return false;
}

// Before the game ends (QUIT GAME, the window's close button, Alt+F4):
// a career save that is being written is let finish first, for up to ten
// seconds. Ending the title gives the game's threads only 200 ms, which could
// cut a save short. A save takes well under a second (50 ms measured), and
// the log says when closing waited for one.
inline void MclaWaitForSaves() {
  using Clock = std::chrono::steady_clock;
  const auto start = Clock::now();
  if (!MclaSaveInProgress()) {
    return;
  }
  REXLOG_INFO("closing: a save is being written; waiting for it");
  while (MclaSaveInProgress() && Clock::now() - start < std::chrono::seconds(10)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  const auto waited =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
  if (MclaSaveInProgress()) {
    REXLOG_WARN("closing: the save was still being written after {} ms", waited);
  } else {
    REXLOG_INFO("closing: the save finished after {} ms", waited);
  }
}
