#include "game_guard.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

#include "../../../game_marker_core.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#endif

namespace larecomp::online_tool {

namespace {

namespace fs = std::filesystem;

// The program file's path as the marker names it (game_marker_core.h): absolute, the OS's own separators.
std::string AbsoluteUtf8(const std::string& exe) {
    const fs::path p(std::u8string(reinterpret_cast<const char8_t*>(exe.data()), exe.size()));
    std::error_code ec;
    fs::path abs = fs::absolute(p, ec);
    if (ec) abs = p;
    abs = abs.lexically_normal();  // no "." or ".." left, as the OS names a running program's path
    abs.make_preferred();
    const std::u8string u = abs.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

#ifdef _WIN32

std::string Narrow(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool MarkerHeld(const std::string& path) {
    const std::string name = marker::WindowsMutexName(marker::KeyOfPath(path, true));
    const std::wstring wide(name.begin(), name.end());  // the name is ASCII
    if (const HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, wide.c_str())) {
        CloseHandle(h);
        return true;
    }
    // A game that runs elevated has an object a normal process may not open: that is a running game.
    return GetLastError() == ERROR_ACCESS_DENIED;
}

// A process whose image is `path` (compared the way the marker names paths), or one called like it that cannot be opened.
void ScanProcesses(const std::string& path, GameLook& look) {
    const std::string want = marker::NormalizePath(path, true);
    const size_t last_slash = want.find_last_of('\\');
    const std::string file = want.substr(last_slash == std::string::npos ? 0 : last_slash + 1);
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof(e);
    for (BOOL more = Process32FirstW(snap, &e); more; more = Process32NextW(snap, &e)) {
        std::string name = Narrow(e.szExeFile);
        std::transform(name.begin(), name.end(), name.begin(),
                       [](char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; });
        if (name != file) continue;
        const HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.th32ProcessID);
        if (!p) {
            look.unsure = true;
            continue;
        }
        wchar_t buf[32768];
        DWORD size = DWORD(sizeof(buf) / sizeof(buf[0]));
        const bool got = QueryFullProcessImageNameW(p, 0, buf, &size) != 0;
        CloseHandle(p);
        if (!got) {
            look.unsure = true;
            continue;
        }
        if (marker::NormalizePath(Narrow(std::wstring(buf, size)), true) == want) {
            look.running = true;
            if (look.how.empty()) look.how = "process";
        }
    }
    CloseHandle(snap);
}

#else

bool MarkerHeld(const std::string& path) {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    const std::string file = marker::LinuxLockFolder(runtime ? runtime : "", uint32_t(getuid())) + "/" +
                             marker::LinuxLockFileName(marker::KeyOfPath(path, false));
    const int fd = open(file.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;  // no lock file: no game has ever run from this program file here
    // The game holds a shared lock for as long as it runs; an exclusive try fails exactly then.
    bool held = false;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) held = errno == EWOULDBLOCK;
    close(fd);  // releases a lock we took
    return held;
}

void ScanProcesses(const std::string& path, GameLook& look) {
    DIR* proc = opendir("/proc");
    if (!proc) return;
    while (const dirent* e = readdir(proc)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char link[4096];
        const std::string exe = std::string("/proc/") + e->d_name + "/exe";
        const ssize_t n = readlink(exe.c_str(), link, sizeof(link) - 1);
        if (n <= 0) continue;
        std::string target(link, size_t(n));
        static const char kDeleted[] = " (deleted)";
        const size_t d = sizeof(kDeleted) - 1;
        if (target.size() > d && target.compare(target.size() - d, d, kDeleted) == 0) target.resize(target.size() - d);
        if (target == path) {
            look.running = true;
            if (look.how.empty()) look.how = "process";
        }
    }
    closedir(proc);
}

#endif

}  // namespace

GameLook LookForGame(const std::string& exe) {
    GameLook look;
    const std::string path = AbsoluteUtf8(exe);
    if (MarkerHeld(path)) {
        look.running = true;
        look.how = "marker";
        return look;
    }
    ScanProcesses(path, look);
    return look;
}

}  // namespace larecomp::online_tool
