// The "a game is running" marker: see game_marker.h for what it is and how the launcher asks.
#include "game_marker.h"
#include "settings/immortal.h"

#include <mutex>
#include <string>

#include "game_marker_core.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#endif

namespace larecomp::marker {

namespace {

std::mutex g_mutex;
bool g_done = false;
bool g_held = false;
std::string& g_what = larecomp::Leak<std::string>("not created");

#if defined(_WIN32)

// The running program's path as UTF-8, or "" when the OS will not say.
std::string ProgramPathUtf8() {
    std::wstring wide;
    DWORD size = MAX_PATH;
    for (;;) {
        wide.resize(size);
        const DWORD length = GetModuleFileNameW(nullptr, wide.data(), size);
        if (length == 0) return {};
        if (length < size) {
            wide.resize(length);
            break;
        }
        if (size >= 65536) return {};
        size *= 2;  // the path did not fit: it is cut off, ask again with room
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string utf8(size_t(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), utf8.data(), bytes, nullptr, nullptr);
    return utf8;
}

bool CreateMarker(std::string& what) {
    const std::string path = ProgramPathUtf8();
    if (path.empty()) {
        what = "the program's path could not be read";
        return false;
    }
    const std::string name = WindowsMutexName(KeyOfPath(path, true));
    const std::wstring wide_name(name.begin(), name.end());  // the name is ASCII
    // Not owned, default security, not inheritable: a process the game starts does not keep it alive. The handle is
    // never closed: the object lives until this process ends (and "already exists" is fine, that is a restarted
    // game that overlaps the one it replaces).
    const HANDLE handle = CreateMutexW(nullptr, FALSE, wide_name.c_str());
    if (!handle) {
        what = "CreateMutexW failed for " + name + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    what = "named mutex " + name;
    return true;
}

#else

std::string ProgramPath() {
    char buffer[4096];
    const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length <= 0) return {};
    std::string path(buffer, size_t(length));
    // A program replaced while it runs (an update) reads " (deleted)" after its path.
    static const char kDeleted[] = " (deleted)";
    const size_t deleted = sizeof(kDeleted) - 1;
    if (path.size() > deleted && path.compare(path.size() - deleted, deleted, kDeleted) == 0) path.resize(path.size() - deleted);
    return path;
}

// Makes the folder (0700) when it is not there, and checks it is ours and nobody else can write to it: the fallback
// below /tmp is a place other users can create folders in.
bool EnsureOwnFolder(const std::string& folder) {
    if (mkdir(folder.c_str(), 0700) != 0 && errno != EEXIST) return false;
    struct stat info;
    if (lstat(folder.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) return false;
    return info.st_uid == getuid() && (info.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

int g_lock_fd = -1;

bool CreateMarker(std::string& what) {
    const std::string path = ProgramPath();
    if (path.empty()) {
        what = "/proc/self/exe could not be read";
        return false;
    }
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    const std::string folder = LinuxLockFolder(runtime ? runtime : "", uint32_t(getuid()));
    if (!EnsureOwnFolder(folder)) {
        what = "no usable folder for the lock file (" + folder + ")";
        return false;
    }
    const std::string file = folder + "/" + LinuxLockFileName(KeyOfPath(path, false));
    // Close-on-exec: the games and tools this one starts must not inherit the lock and keep it after the game is gone.
    const int fd = open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        what = "could not open " + file + ": " + std::strerror(errno);
        return false;
    }
    // A shared lock: a restarted game holds it beside the old one. The only thing that can refuse it is the launcher's
    // exclusive try, which lasts microseconds, so a short retry is enough.
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (flock(fd, LOCK_SH | LOCK_NB) == 0) {
            g_lock_fd = fd;  // kept open until this process ends
            what = "shared lock on " + file;
            return true;
        }
        if (errno != EWOULDBLOCK && errno != EINTR) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    what = "could not lock " + file + ": " + std::strerror(errno);
    close(fd);
    return false;
}

#endif

}  // namespace

bool Create() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_done) return g_held;
    g_done = true;
    g_held = CreateMarker(g_what);
    return g_held;
}

std::string Describe() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_held ? g_what : "game-running marker not created: " + g_what;
}

}  // namespace larecomp::marker
