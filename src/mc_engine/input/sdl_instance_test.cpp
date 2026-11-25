// A check that needs no game: which SDL the exe's own SDL code talks to (sdl_instance.h): the one-SDL
// predicate, the wheel thread's two gates (wait for the SDK's gamepad layer, announce the pads present at
// launch) and the SDL_GAMECONTROLLER_IGNORE_DEVICES list with its command-line reader.
// Build: cmake --build <build dir> --target larecomp_sdl_instance_test, then run it; exit 0 = passed.
// What it cannot show (the SDL side, the process-start timing, a wheel) is in the report of the change.

#include <cstdio>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "mc_engine/input/sdl_instance.h"

using namespace larecomp::sdlinst;

namespace {

int g_checks = 0;
int g_failed = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(cond)) {                                                  \
            ++g_failed;                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

bool Contains(const std::string& list, std::string_view entry) {
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t comma = list.find(',', pos);
        if (comma == std::string::npos) comma = list.size();
        if (std::string_view(list).substr(pos, comma - pos) == entry) return true;
        pos = comma + 1;
    }
    return false;
}

int Count(const std::string& list, std::string_view entry) {
    int n = 0;
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t comma = list.find(',', pos);
        if (comma == std::string::npos) comma = list.size();
        if (std::string_view(list).substr(pos, comma - pos) == entry) ++n;
        pos = comma + 1;
    }
    return n;
}

// The four situations the predicate has to tell apart.
void TestOneSdl() {
    // 0.10 on Windows: the SDK's pad driver is SDL (the default), but it is the DLL's copy: NOT one SDL.
    CHECK(!OneSdl(false, "sdl"));
    // 0.11 and Linux: the driver is SDL and the exe calls the very same one.
    CHECK(OneSdl(true, "sdl"));
    // A driver that is not SDL (a larecomp.toml from Windows says xinput): never one SDL for the pad code.
    CHECK(!OneSdl(true, "xinput"));
    CHECK(!OneSdl(false, "xinput"));
    CHECK(!OneSdl(true, ""));
    CHECK(!OneSdl(true, "SDL"));  // the flag is compared as it is stored
    // The compile-time default follows the platform and the build's switch, nothing else.
#if defined(LARECOMP_SHARED_SDL)
    static_assert(kExeSdlIsSdks, "the 0.11 line shares SDL");
#elif defined(_WIN32)
    static_assert(!kExeSdlIsSdks, "0.10 on Windows has two SDLs");
#else
    static_assert(kExeSdlIsSdks, "Linux keeps the one-SDL assumption");
#endif
    CHECK(OneSdl("sdl") == kExeSdlIsSdks);
    CHECK(!OneSdl("xinput"));
    static_assert(OneSdl(true, "sdl") && !OneSdl(false, "sdl"), "the predicate is a constant expression");
}

// The bug: on 0.10 Windows the wheel thread waited for a gamepad layer that only a dialog starts.
void TestGate() {
    // Two SDLs: never wait, whether or not the exe's own gamepad layer is up.
    CHECK(!MustWaitForSdkGamepadLayer(false, false));
    CHECK(!MustWaitForSdkGamepadLayer(false, true));
    // One SDL: wait until the SDK's driver has started the gamepad layer, then go.
    CHECK(MustWaitForSdkGamepadLayer(true, false));
    CHECK(!MustWaitForSdkGamepadLayer(true, true));
}

void TestAnnounce() {
    // Two SDLs: never due, not even with the layer reported up for a minute.
    {
        PadAnnounceTimer t(false);
        CHECK(t.Finished());
        for (int64_t ms = 0; ms < 60000; ms += 100) CHECK(!t.Due(true, ms));
    }
    // One SDL: not due while the SDK's gamepad layer is down.
    {
        PadAnnounceTimer t(true);
        CHECK(!t.Finished());
        for (int64_t ms = 0; ms < 10000; ms += 100) CHECK(!t.Due(false, ms));
        CHECK(!t.Finished());
        // The delay counts from the first pass that saw the layer up (10000 here), not from the start.
        CHECK(!t.Due(true, 10000));
        CHECK(!t.Due(true, 12999));
        CHECK(t.Due(true, 13000));
        // Once: never again.
        CHECK(t.Finished());
        CHECK(!t.Due(true, 13001));
        CHECK(!t.Due(true, 60000));
    }
    // The layer dropping out and coming back does not restart the delay (the first sighting stays).
    {
        PadAnnounceTimer t(true, 1000);
        CHECK(!t.Due(true, 500));
        CHECK(!t.Due(false, 900));
        CHECK(t.Due(true, 1500));
        CHECK(!t.Due(true, 1600));
    }
    // A custom delay.
    {
        PadAnnounceTimer t(true, 0);
        CHECK(t.Due(true, 42));
    }
}

void TestIgnoreList() {
    const std::string known = IgnoreDevicesList("", "");
    CHECK(!known.empty());
    CHECK(known.find(' ') == std::string::npos);
    CHECK(known.front() != ',' && known.back() != ',');
    CHECK(Contains(known, "0x046d/0xc24f"));   // Logitech G29
    CHECK(Contains(known, "0x046d/0xc294"));   // Driving Force
    CHECK(Contains(known, "0x044f/0xb66e"));   // Thrustmaster T300 RS
    CHECK(Contains(known, "0x044f/0xb664"));
    const int entries = int(std::size(kKnownWheels));
    int commas = 0;
    for (char c : known) commas += c == ',';
    CHECK(commas + 1 == entries);

    // The user's own list is kept, first and unchanged.
    const std::string with_user = IgnoreDevicesList("0x1234/0x5678", "");
    CHECK(with_user.rfind("0x1234/0x5678,", 0) == 0);
    CHECK(Contains(with_user, "0x046d/0xc24f"));
    // Blanks and empty pieces in what the user had are dropped, not copied.
    const std::string messy = IgnoreDevicesList(" 0x1234/0x5678 ,, ,0x0001/0x0002", "");
    CHECK(Contains(messy, "0x1234/0x5678") && Contains(messy, "0x0001/0x0002"));
    CHECK(messy.find(",,") == std::string::npos && messy.find(' ') == std::string::npos);

    // A device named as vendor:product is added (in the SDL spelling, lower case).
    const std::string named = IgnoreDevicesList("", "ABCD:12ef");
    CHECK(Contains(named, "0xabcd/0x12ef"));
    CHECK(named.size() == known.size() + 1 + std::string("0xabcd/0x12ef").size());
    // A name, "auto", "off", the wrong length or non-hex digits add nothing.
    for (const char* not_an_id : {"auto", "off", "G29 Racing Wheel", "", "046d:c24", "046d-c24f", "046d:c24ff", "zzzz:c24f",
                                  "046d:c2g4"}) {
        CHECK(IgnoreDevicesList("", not_an_id) == known);
    }
    // A named device that is already a known wheel is not listed twice.
    const std::string g29 = IgnoreDevicesList("", "046d:c24f");
    CHECK(g29 == known && Count(g29, "0x046d/0xc24f") == 1);

    // Applying it to its own output changes nothing: the list is set at process start and again when the settings
    // are known, and the second call must not grow the value.
    CHECK(IgnoreDevicesList(known, "") == known);
    CHECK(IgnoreDevicesList(named, "abcd:12ef") == named);
    CHECK(IgnoreDevicesList(with_user, "") == with_user);
    // ... but a wheel that only the settings name is appended to what the start already set.
    const std::string late = IgnoreDevicesList(known, "1234:abcd");
    CHECK(late.rfind(known + ",", 0) == 0 && Contains(late, "0x1234/0xabcd"));
    // The user's spelling of an entry in another case counts as present.
    CHECK(Count(IgnoreDevicesList("0x046D/0xC24F", ""), "0x046d/0xc24f") == 0);
    CHECK(Count(IgnoreDevicesList("0x046D/0xC24F", ""), "0x046D/0xC24F") == 1);
}

void TestCommandLine() {
    CHECK(SplitCommandLine("").empty());
    CHECK(SplitCommandLine("   \t ").empty());
    const std::vector<std::string> w = SplitCommandLine("a  \"b c\"\t--x=\"y z\" \"\"");
    CHECK(w.size() == 4 && w[0] == "a" && w[1] == "b c" && w[2] == "--x=y z" && w[3].empty());

    CHECK(OptionValue("larecomp.exe", "wheel_device").empty());
    CHECK(OptionValue("larecomp.exe --wheel_device=046d:c24f", "wheel_device") == "046d:c24f");
    CHECK(OptionValue("larecomp.exe --wheel_device 046d:c24f --x", "wheel_device") == "046d:c24f");
    CHECK(OptionValue("\"C:\\Games\\MCLA\\larecomp.exe\" --wheel_device=\"046d:c24f\"", "wheel_device") == "046d:c24f");
    // The last one wins, as on the app's own command line.
    CHECK(OptionValue("x --wheel_device=1111:2222 --wheel_device=3333:4444", "wheel_device") == "3333:4444");
    // Another option that starts with the name is not it; a flag at the end or followed by another option has no value.
    CHECK(OptionValue("x --wheel_device_range=900", "wheel_device").empty());
    CHECK(OptionValue("x --wheel_device", "wheel_device").empty());
    CHECK(OptionValue("x --wheel_device --other=1", "wheel_device").empty());
    CHECK(OptionValue("x wheel_device=046d:c24f", "wheel_device").empty());  // no dashes: not an option
    // End to end: what the process-start step feeds the list builder.
    const std::string cmd = "larecomp.exe --wheel_device=0eb7:0e03";
    CHECK(Contains(IgnoreDevicesList("", OptionValue(cmd, "wheel_device")), "0x0eb7/0x0e03"));
}

}  // namespace

int main() {
    TestOneSdl();
    TestGate();
    TestAnnounce();
    TestIgnoreList();
    TestCommandLine();
    if (g_failed != 0) {
        std::printf("sdl_instance_test: %d of %d checks FAILED\n", g_failed, g_checks);
        return 1;
    }
    std::printf("sdl_instance_test: %d checks passed\n", g_checks);
    return 0;
}
