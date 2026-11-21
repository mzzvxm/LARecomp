// A check that needs no game: the switch that skips the crash box (crash_options.h). Build:
// cmake --build <build dir> --target larecomp_crash_options_test, then run it; exit 0 = passed.
// The handler's side (the box not shown, the process ended with kNoDialogExitCode) needs a crashing process: the test harness
// runs it (ports\scripts\crash_watch.py, the harness's own selftest), and a deliberate crash of a build with the variable set shows it.

#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "crash_options.h"

using namespace larecomp::crash;

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

void SetEnv(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value ? value : "");   // an empty value removes the variable on Windows
#else
    if (value) setenv(name, value, 1); else unsetenv(name);
#endif
}

void TestValues() {
    CHECK(!NoDialogRequested(nullptr));
    CHECK(!NoDialogRequested(""));
    CHECK(!NoDialogRequested("0"));
    CHECK(!NoDialogRequested("false"));
    CHECK(!NoDialogRequested("no"));
    CHECK(!NoDialogRequested("off"));
    CHECK(!NoDialogRequested("2"));
    CHECK(!NoDialogRequested(" 1"));    // no trimming: a value with a stray space is not a yes
    CHECK(!NoDialogRequested("11"));
    CHECK(!NoDialogRequested("truee"));
    CHECK(NoDialogRequested("1"));
    CHECK(NoDialogRequested("true"));
    CHECK(NoDialogRequested("TRUE"));
    CHECK(NoDialogRequested("True"));
    CHECK(NoDialogRequested("yes"));
    CHECK(NoDialogRequested("YES"));
    CHECK(NoDialogRequested("on"));
    CHECK(NoDialogRequested("On"));
}

void TestEnvironment() {
    SetEnv(kNoDialogEnv, nullptr);
    CHECK(!NoDialogFromEnvironment());       // the default is the box
    SetEnv(kNoDialogEnv, "1");
    CHECK(NoDialogFromEnvironment());
    SetEnv(kNoDialogEnv, "0");
    CHECK(!NoDialogFromEnvironment());
    SetEnv(kNoDialogEnv, "true");
    CHECK(NoDialogFromEnvironment());
    SetEnv(kNoDialogEnv, nullptr);
    CHECK(!NoDialogFromEnvironment());
}

void TestConstants() {
    CHECK(std::string_view(kNoDialogEnv) == "LARECOMP_NO_CRASH_DIALOG");
    CHECK(kNoDialogExitCode == 0xC4A50001u);
    CHECK(kNoDialogExitCode != 0xC0000005u && kNoDialogExitCode != 3u);   // not the exception code, not the abort status
}

}  // namespace

int main() {
    TestValues();
    TestEnvironment();
    TestConstants();
    std::printf("crash_options: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
