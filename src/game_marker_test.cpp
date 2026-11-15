// A check that needs no game: the names the game-running marker uses (game_marker_core.h). Build:
// cmake --build <build dir> --target larecomp_game_marker_test, then run it; exit 0 = passed.
// The OS side (the mutex, the flock) is not in here: it needs a second process, see the launcher's
// LARecomp.Launcher.Core.Tests (GameMarkerTests) and perf\scripts\game-marker-check.ps1.
//
// The expected values are the same ones the launcher's tests hold (Os\GameMarkerName.cs): the two
// sides must agree byte for byte, so a change here that does not change them there is a bug.

#include <cstdio>
#include <string>

#include "game_marker_core.h"

using namespace larecomp::marker;

namespace {

int g_checks = 0;
int g_failed = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failed;                                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
        }                                                                              \
    } while (0)

void TestFnv() {
    // The published FNV-1a 64 test values.
    CHECK(Fnv1a64("") == 0xcbf29ce484222325ull);
    CHECK(Fnv1a64("a") == 0xaf63dc4c8601ec8cull);
    CHECK(Fnv1a64("foobar") == 0x85944171f73967e8ull);
}

void TestWindowsKeys() {
    // One program file, however its path is spelled.
    CHECK(KeyOfPath("C:\\Games\\LARecomp\\game\\larecomp.exe", true) == "549b7e4a06308e95");
    CHECK(KeyOfPath("c:\\games\\larecomp\\game\\larecomp.exe", true) == "549b7e4a06308e95");
    CHECK(KeyOfPath("C:/Games/LARecomp/game/larecomp.exe", true) == "549b7e4a06308e95");
    CHECK(KeyOfPath("\\\\?\\C:\\Games\\LARecomp\\game\\larecomp.exe", true) == "549b7e4a06308e95");
    // Another folder, another key.
    CHECK(KeyOfPath("C:\\mcv\\larecomp\\build\\larecomp.exe", true) == "1e6df9da91dddc1e");
    // Only A-Z fold: the U-umlaut is kept as UTF-8 (C3 9C) and is not lower-cased.
    CHECK(KeyOfPath("D:\\Spiele\\\xC3\x9C" "ber\\larecomp.exe", true) == "dadb5d53d40c8b6a");
    CHECK(NormalizePath("D:\\Spiele\\\xC3\x9C" "ber\\larecomp.exe", true) == "d:\\spiele\\\xC3\x9C" "ber\\larecomp.exe");
}

void TestLinuxKeys() {
    // Linux paths are case sensitive and used as they are.
    CHECK(KeyOfPath("/home/deck/LARecomp/game/larecomp", false) == "2ff928db0fc5b4b8");
    CHECK(KeyOfPath("/home/Deck/LARecomp/game/larecomp", false) == "b0e1f95d785f3718");
    CHECK(KeyOfPath("/home/deck/LARecomp/game/larecomp", false) != KeyOfPath("/home/deck/larecomp/game/larecomp", false));
}

void TestNames() {
    CHECK(WindowsMutexName("549b7e4a06308e95") == "Local\\LARecomp.Game.549b7e4a06308e95");
    CHECK(LinuxLockFileName("549b7e4a06308e95") == "game-549b7e4a06308e95.lock");
    CHECK(LinuxLockFolder("/run/user/1000", 1000) == "/run/user/1000/larecomp");
    CHECK(LinuxLockFolder("/run/user/1000/", 1000) == "/run/user/1000/larecomp");
    // No runtime folder, or a relative one: the per-user folder below /tmp.
    CHECK(LinuxLockFolder("", 1000) == "/tmp/larecomp-1000");
    CHECK(LinuxLockFolder("run/user/1000", 1000) == "/tmp/larecomp-1000");
}

}  // namespace

int main() {
    TestFnv();
    TestWindowsKeys();
    TestLinuxKeys();
    TestNames();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
