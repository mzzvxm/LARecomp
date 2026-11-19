#pragma once
//
// Which SDL does this executable's own SDL code talk to?
//
// The SDK's window, its event pump and its SDL input driver live in the SDK's runtime. On the 0.10 line
// of the SDK on Windows that runtime is rexruntime.dll with SDL3 linked INTO it as a static library, and
// larecomp.exe has a second static SDL3 of its own: two separate SDLs in one process (own hint table,
// own environment snapshot, own device list and event queue). The exe's copy has no window and nothing
// pumps its queue. From 0.11 on, and on Linux, there is one SDL: the exe's calls reach the SDK's.
//
// Every place that used to say "the SDK's input_backend is sdl, so this is the same SDL" must ask
// OneSdl() instead. Where it says no, the exe's SDL code initialises and polls its own copy on purpose
// and never asks about, or pushes events into, a copy that has nobody listening.
//
// Pure functions only (no SDL include), so the rules have a check that needs no game:
// sdl_instance_test.cpp (cmake target larecomp_sdl_instance_test).

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace larecomp::sdlinst {

// Compile time: do the exe's SDL calls reach the same SDL the SDK uses? CMake defines LARECOMP_SHARED_SDL
// where the SDK ships SDL3 as one shared library (the 0.11 line: SDL3.dll next to the exe). Without it,
// Windows is the two-copies case; the other platforms keep the one-SDL assumption they always had
// (ELF symbol interposition makes the library's SDL calls bind to the exe's SDL).
#if defined(LARECOMP_SHARED_SDL)
inline constexpr bool kExeSdlIsSdks = true;
#elif defined(_WIN32)
inline constexpr bool kExeSdlIsSdks = false;
#else
inline constexpr bool kExeSdlIsSdks = true;
#endif

// True when the SDK's pad driver is SDL AND it is the very SDL the exe calls. (A driver that is not SDL,
// "xinput", leaves the exe's SDL to itself even when the library is shared.)
constexpr bool OneSdl(bool exe_sdl_is_sdks, std::string_view input_backend) {
    return exe_sdl_is_sdks && input_backend == "sdl";
}
constexpr bool OneSdl(std::string_view input_backend) {
    return OneSdl(kExeSdlIsSdks, input_backend);
}

// The wheel thread starts its joystick layer only after the SDK's SDL driver has started the gamepad
// layer (SDL posts GAMEPAD_ADDED only for pads present when the joystick layer was already up, so the
// driver would be blind to every pad otherwise). That wait can only be asked of the SDK's own SDL: with
// two copies the exe's SDL gamepad layer is not the driver's, nothing may wait for it, and it is never
// started by anyone but a dialog (waiting for it kept the wheel layer from ever coming up).
constexpr bool MustWaitForSdkGamepadLayer(bool one_sdl, bool gamepad_layer_up) {
    return one_sdl && !gamepad_layer_up;
}

// When to hand the SDK's driver the GAMEPAD_ADDED event for each pad that was connected at launch: only
// with one SDL (the event goes into the one queue the driver drains), once, delay_ms after the gamepad
// layer was first seen up (time for the driver to start and drain). With two SDLs it is never due.
class PadAnnounceTimer {
public:
    explicit PadAnnounceTimer(bool one_sdl, int64_t delay_ms = 3000) : done_(!one_sdl), delay_ms_(delay_ms) {}

    // Call it on every pass of the loop; true exactly once, on the pass that should send the events.
    bool Due(bool gamepad_layer_up, int64_t now_ms) {
        if (done_ || !gamepad_layer_up) return false;
        if (!seen_) {
            seen_ = true;
            first_seen_ms_ = now_ms;
        }
        if (now_ms - first_seen_ms_ < delay_ms_) return false;
        done_ = true;
        return true;
    }
    bool Finished() const { return done_; }

private:
    bool done_;
    bool seen_ = false;
    int64_t first_seen_ms_ = 0;
    int64_t delay_ms_;
};

// ---------------------------------------------------------------------------------------------------
// SDL_GAMECONTROLLER_IGNORE_DEVICES: the wheels the SDK's gamepad layer must not map as a stick.
// ---------------------------------------------------------------------------------------------------

struct WheelId {
    uint16_t vid;
    uint16_t pid;
};

// Models SDL is likely to also map as a gamepad: Logitech (G25 .. G29, Driving Force GT, G920 family) and
// Thrustmaster (T-series, TX, TMX). They stay visible as joysticks, which is how the wheel layer reads them.
inline constexpr WheelId kKnownWheels[] = {
    {0x046d, 0xc294}, {0x046d, 0xc295}, {0x046d, 0xc298}, {0x046d, 0xc299}, {0x046d, 0xc29a},
    {0x046d, 0xc29b}, {0x046d, 0xc24f}, {0x044f, 0xb65d}, {0x044f, 0xb65e}, {0x044f, 0xb66d},
    {0x044f, 0xb66e}, {0x044f, 0xb66f}, {0x044f, 0xb677}, {0x044f, 0xb67e}, {0x044f, 0xb689},
    {0x044f, 0xb669}, {0x044f, 0xb664},
};

inline std::string IgnoreEntry(uint16_t vid, uint16_t pid) {
    char b[32];
    std::snprintf(b, sizeof(b), "0x%04x/0x%04x", unsigned(vid), unsigned(pid));
    return b;
}

namespace detail {

inline std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

inline char LowerChar(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

inline std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = LowerChar(c);
    return out;
}

inline bool IsHex4(std::string_view s) {
    if (s.size() != 4) return false;
    for (char c : s) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!ok) return false;
    }
    return true;
}

}  // namespace detail

// The value for SDL_GAMECONTROLLER_IGNORE_DEVICES: what the user already had in it (kept, in order), then the
// known wheels, then the device the user named in `wheel_device` when that is a vendor:product pair in hex
// (046d:c24f; a name or "auto" or "off" is not one and adds nothing). An entry that is already there, in any
// case, is not added again, so applying the function to its own output changes nothing (the list is set at
// process start and again when the settings are known).
inline std::string IgnoreDevicesList(std::string_view existing, std::string_view wheel_device) {
    std::vector<std::string> entries;
    auto has = [&](const std::string& entry) {
        const std::string want = detail::Lower(entry);
        for (const std::string& e : entries) {
            if (detail::Lower(e) == want) return true;
        }
        return false;
    };
    for (size_t pos = 0; pos <= existing.size();) {
        size_t comma = existing.find(',', pos);
        if (comma == std::string_view::npos) comma = existing.size();
        const std::string_view piece = detail::Trim(existing.substr(pos, comma - pos));
        if (!piece.empty()) entries.emplace_back(piece);
        pos = comma + 1;
    }
    for (const WheelId& w : kKnownWheels) {
        const std::string e = IgnoreEntry(w.vid, w.pid);
        if (!has(e)) entries.push_back(e);
    }
    const std::string_view named = detail::Trim(wheel_device);
    if (named.size() == 9 && named[4] == ':' && detail::IsHex4(named.substr(0, 4)) &&
        detail::IsHex4(named.substr(5, 4))) {
        const std::string e = "0x" + detail::Lower(named.substr(0, 4)) + "/0x" + detail::Lower(named.substr(5, 4));
        if (!has(e)) entries.push_back(e);
    }
    std::string out;
    for (const std::string& e : entries) {
        if (!out.empty()) out += ',';
        out += e;
    }
    return out;
}

// The words of a command line: split at spaces and tabs, a double-quoted stretch is one word (the quotes are
// dropped). Enough for "--name=value" and "--name value"; it is not the C runtime's full rule set.
inline std::vector<std::string> SplitCommandLine(std::string_view line) {
    std::vector<std::string> words;
    std::string cur;
    bool in_quotes = false;
    bool have = false;
    for (char c : line) {
        if (c == '"') {
            in_quotes = !in_quotes;
            have = true;
        } else if ((c == ' ' || c == '\t') && !in_quotes) {
            if (have) words.push_back(cur);
            cur.clear();
            have = false;
        } else {
            cur += c;
            have = true;
        }
    }
    if (have) words.push_back(cur);
    return words;
}

// The value of `--<name>=value` or `--<name> value` on a command line; the last one wins (as on the
// command line of the app itself); empty when there is none. At process start the settings are not read
// yet, so the one setting the ignore list needs (wheel_device) comes from here.
inline std::string OptionValue(std::string_view command_line, std::string_view name) {
    const std::vector<std::string> words = SplitCommandLine(command_line);
    const std::string flag = "--" + std::string(name);
    std::string value;
    for (size_t i = 0; i < words.size(); ++i) {
        const std::string& w = words[i];
        if (w.size() > flag.size() && w.compare(0, flag.size(), flag) == 0 && w[flag.size()] == '=') {
            value = w.substr(flag.size() + 1);
        } else if (w == flag && i + 1 < words.size() && words[i + 1].rfind("--", 0) != 0) {
            value = words[i + 1];
        }
    }
    return value;
}

}  // namespace larecomp::sdlinst
