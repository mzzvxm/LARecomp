#pragma once
//
// The pure half of the "a game is running" marker (game_marker.h): which name a game of a given
// program file uses. No OS calls in here, so game_marker_test.cpp compiles it alone and runs it
// without a game.
//
// The launcher computes the same name from the same path (LARecomp.Launcher.Core, Os\GameMarkerName.cs,
// with the same test vectors): change either side and the other one stops finding the game. The
// rules, kept this plain on purpose so that two languages agree:
//   1. the path of the running program file, as the OS names it (Windows: GetModuleFileNameW,
//      without a \\?\ prefix; Linux: where /proc/self/exe points, which has no symbolic links left);
//   2. Windows only: "/" becomes "\", and the letters A-Z become a-z (nothing else changes case:
//      two spellings that differ in a non-ASCII letter's case are two installs here; the launcher's
//      process-list check, which it still makes before it refuses to write a setting, covers that);
//   3. FNV-1a, 64 bit, over the UTF-8 bytes, written as 16 lower-case hex digits.
//

#include <cstdint>
#include <string>
#include <string_view>

namespace larecomp::marker {

// The path as it is hashed (rule 2). The input is UTF-8.
inline std::string NormalizePath(std::string_view utf8_path, bool windows) {
    std::string path(utf8_path);
    if (!windows) return path;
    // A leading \\?\ (the long-path prefix) is not part of the program's identity.
    if (path.size() >= 4 && path.compare(0, 4, "\\\\?\\") == 0) path.erase(0, 4);
    for (char& c : path) {
        if (c == '/') c = '\\';
        else if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    }
    return path;
}

// FNV-1a over bytes, 64 bit.
inline uint64_t Fnv1a64(std::string_view bytes) {
    uint64_t hash = 14695981039346656037ull;
    for (unsigned char byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

// 16 lower-case hex digits: the identity of one install's program file.
inline std::string KeyOfPath(std::string_view utf8_path, bool windows) {
    static const char kDigits[] = "0123456789abcdef";
    const uint64_t hash = Fnv1a64(NormalizePath(utf8_path, windows));
    std::string key(16, '0');
    for (int i = 15; i >= 0; --i) {
        key[size_t(i)] = kDigits[(hash >> ((15 - i) * 4)) & 0xF];
    }
    return key;
}

// Windows: the named mutex a running game holds a handle to. "Local\" is the logon session's
// namespace: the launcher and the game it starts are in the same one.
inline std::string WindowsMutexName(std::string_view key) {
    return "Local\\LARecomp.Game." + std::string(key);
}

// Linux: the lock file a running game holds a shared flock on. Below $XDG_RUNTIME_DIR/larecomp
// when that is set, else /tmp/larecomp-<uid> (the caller makes the folder, mode 0700).
inline std::string LinuxLockFolder(std::string_view runtime_dir, uint32_t uid) {
    if (!runtime_dir.empty() && runtime_dir.front() == '/') {
        std::string folder(runtime_dir);
        while (folder.size() > 1 && folder.back() == '/') folder.pop_back();
        return folder + "/larecomp";
    }
    return "/tmp/larecomp-" + std::to_string(uid);
}

inline std::string LinuxLockFileName(std::string_view key) {
    return "game-" + std::string(key) + ".lock";
}

}  // namespace larecomp::marker
