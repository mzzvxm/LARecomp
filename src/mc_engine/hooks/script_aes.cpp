// The game's script decryption, with AES-NI.
//
// What the guest does (read from the recompiled code, and run: a translation of the generated
// functions, driven with the game's own key, agrees with everything below):
//   sub_821E1658(ctx, data, length)  = sub_821E1598(data, length & ~15, ctx, sub_821E0FF0)
//   sub_821E1598(data, length, ctx, fn): for every 16-byte block of `data`, in place, calls
//       fn(block, block, ctx) 16 times in a row (a block is decrypted 16 times over). fn
//       returns 1; a 0 would end the function with 0. The function returns 1.
//   sub_821E0FF0(in, out, ctx): one block of plain AES decryption, the table algorithm
//       (Td0 at 0x820171A0, the inverse S-box words at 0x820175A0), round keys from the
//       decryption schedule at ctx + 240 (big-endian words), the round count at ctx + 480.
//       It reads nothing else and writes only the 16 output bytes.
//   sub_821E1610(ctx) builds the context from a fixed 32-byte key with sub_821E0678: the
//       encryption schedule at ctx + 0, the decryption schedule (the equivalent inverse
//       cipher) at ctx + 240, Nr at ctx + 480 (14 for this key; 10 and 12 are handled too).
// sub_821E1658 is called from sub_82557A68 four times (reached from mcScript::Load, sub_82728010,
// through sub_82557F10 and sub_82556BA8) and from sub_821CD7A0 once. The 16 passes make it the
// hot spot of a script load: 50 to 66 ms of the main thread at the first world entry of a
// session, in software (measured with the slow-frame sampler).
//
// The override takes over sub_821E1598 when its callback is sub_821E0FF0 and it can do the whole
// job: the length a multiple of 16, the round count 10, 12 or 14, the data and the context in
// the low guest range and not overlapping. Anything else goes to the recompiled function
// untouched. The AES itself is in script_aes.h (AES-NI when the CPU has it, the portable table
// algorithm otherwise).
//
// Checking it against the original, on this machine, with the game's real data:
//  - the first time the AES is needed, FIPS-197's AES-128/192/256 decryption vectors are run
//    through both implementations, and the two are compared with each other;
//  - for every key schedule not seen before (the game builds one key, so one time), the start
//    of the buffer (4 KB the first time, 256 bytes after that) is decrypted by the native code
//    on a copy and by the recompiled function on the guest's bytes, and the two are compared.
//    The recompiled result is what the guest keeps. On a difference an error is logged and the
//    native path is switched off for the rest of the run: everything goes to the recompiled
//    code from then on (the blocks are independent, so the rest of that call is done by it too).
//    The first check also decrypts the sample with the portable path, whichever path is in use,
//    so that one is compared with the game's code as well. The log says how long each took.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/ppc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "larecomp_log.h"
#include "script_aes.h"

REX_EXTERN(__imp__rex_sub_821E1598);  // the recompiled block loop

namespace {

constexpr uint32_t kBlockDecrypt = 0x821E0FF0;  // the callback the override knows (rex_sub_821E0FF0)
constexpr uint32_t kDecryptKeysOffset = 240;    // the decryption schedule in the context
constexpr uint32_t kRoundsOffset = 480;         // Nr in the context
constexpr int kPasses = 16;                     // times the loop decrypts each block
// Guest addresses from here up are the physical mirror, which the generated code reaches through
// an extra host offset on Windows; the override leaves those to the recompiled function.
constexpr uint64_t kLowGuestLimit = 0xE0000000ull;
constexpr uint32_t kFirstCheckBytes = 4096;
constexpr uint32_t kLaterCheckBytes = 256;
constexpr int kMaxChecks = 32;  // new key schedules checked before the rest are trusted

enum Mode : int { kUnset = 0, kAesNi, kPortable, kOff };

std::atomic<int> g_mode{kUnset};
std::atomic<uint64_t> g_calls{0};
std::atomic<uint64_t> g_bytes{0};
std::atomic<uint64_t> g_native_ns{0};

struct CheckedSchedule {
    bool used = false;
    int rounds = 0;
    uint8_t keys[script_aes::kMaxScheduleBytes] = {};
};
std::mutex g_checked_mutex;
CheckedSchedule g_checked[8];
size_t g_checked_next = 0;
std::atomic<int> g_checks_done{0};

// Picks the path once: the known-answer tests, then the CPU. A path that fails its vectors is
// never used; if both fail the override stays out of the way.
Mode ChooseMode() {
    static std::once_flag once;
    std::call_once(once, [] {
        const script_aes::KnownAnswerResult kat = script_aes::RunKnownAnswerTests();
        Mode mode = kOff;
        if (kat.aesni_available && kat.aesni_ok) {
            mode = kAesNi;
        } else if (kat.portable_ok) {
            mode = kPortable;
        }
        LARECOMP_APP_INFO("[ScriptAES] known-answer tests (FIPS-197 decryption, AES-128/192/256): portable {}, "
                          "AES-NI {}; the game's script decryption will use {}",
                          kat.portable_ok ? "ok" : "FAILED",
                          kat.aesni_available ? (kat.aesni_ok ? "ok" : "FAILED") : "not available",
                          mode == kAesNi ? "AES-NI" : mode == kPortable ? "the portable code" : "the recompiled code");
        if (!kat.portable_ok || (kat.aesni_available && !kat.aesni_ok)) {
            LARECOMP_APP_ERROR("[ScriptAES] a known-answer test FAILED: this is a bug in script_aes.h; the {} path is "
                               "not used",
                               !kat.portable_ok ? "portable" : "AES-NI");
        }
        g_mode.store(mode);
    });
    return static_cast<Mode>(g_mode.load());
}

void Decrypt(Mode mode, const uint8_t* keys, int rounds, uint8_t* data, size_t blocks) {
    if (mode == kAesNi) {
        script_aes::DecryptAesNi(keys, rounds, data, blocks, kPasses);
    } else {
        script_aes::DecryptPortable(keys, rounds, data, blocks, kPasses);
    }
}

bool WasChecked(const uint8_t* keys, int rounds) {
    const size_t size = 16 * static_cast<size_t>(rounds + 1);
    std::lock_guard<std::mutex> lock(g_checked_mutex);
    if (g_checks_done.load() >= kMaxChecks) return true;
    for (const CheckedSchedule& c : g_checked) {
        if (c.used && c.rounds == rounds && std::memcmp(c.keys, keys, size) == 0) return true;
    }
    return false;
}

void MarkChecked(const uint8_t* keys, int rounds) {
    std::lock_guard<std::mutex> lock(g_checked_mutex);
    CheckedSchedule& c = g_checked[g_checked_next];
    g_checked_next = (g_checked_next + 1) % (sizeof(g_checked) / sizeof(g_checked[0]));
    c.used = true;
    c.rounds = rounds;
    std::memcpy(c.keys, keys, 16 * static_cast<size_t>(rounds + 1));
    g_checks_done.fetch_add(1);
}

// Runs the recompiled function on [data, data + length) with the registers set as the caller
// had them. Its volatile registers are free to change, as in any call.
void RunRecompiled(PPCContext& ctx, uint8_t* base, uint32_t data, uint32_t length, uint32_t keys,
                   uint32_t callback) {
    ctx.r3.u64 = data;
    ctx.r4.u64 = length;
    ctx.r5.u64 = keys;
    ctx.r6.u64 = callback;
    __imp__rex_sub_821E1598(ctx, base);
}

}  // namespace

extern "C" REX_FUNC(rex_sub_821E1598) {
    const uint32_t data = ctx.r3.u32;
    const uint32_t length = ctx.r4.u32;
    const uint32_t keys = ctx.r5.u32;
    const uint32_t callback = ctx.r6.u32;

    // CHECK: sub_821E1658 is the only caller found that sets the callback (r6 = 0x821E0FF0); any
    // other caller, direct or through a function pointer, is told apart by r6 and by the checks
    // below, and runs the recompiled function.
    const bool ours = callback == kBlockDecrypt && (length & 15u) == 0 && data >= 0x10000u;
    const uint64_t data_end = static_cast<uint64_t>(data) + length;
    const uint64_t keys_end = static_cast<uint64_t>(keys) + kRoundsOffset + 4;
    if (!ours || data_end > kLowGuestLimit || keys_end > kLowGuestLimit || keys < 0x10000u ||
        (data < keys_end && keys < data_end)) {
        __imp__rex_sub_821E1598(ctx, base);
        return;
    }
    if (length == 0) {  // the recompiled function returns 1 without touching anything
        ctx.r3.u64 = 1;
        return;
    }
    const uint32_t rounds = script_aes::detail::LoadBe32(base + keys + kRoundsOffset);
    if (rounds != 10 && rounds != 12 && rounds != 14) {
        __imp__rex_sub_821E1598(ctx, base);
        return;
    }
    const Mode mode = ChooseMode();
    if (mode == kOff) {
        __imp__rex_sub_821E1598(ctx, base);
        return;
    }

    // CHECK: stores into guest memory the SDK write-watches (an SSE 16-byte store here, byte stores in the
    // recompiled code) behave the same: not observed, a script buffer is ordinary heap.
    // CHECK: the recompiled function also writes its stack frame below r1 (saved registers, a scratch
    // slot at -160(r1) in the callee); the override does not, on the reading that nothing looks at
    // that memory after the call returns (the ABI's rule for the area below r1).
    // The schedule is read once; the recompiled function reads it for every block, and nothing
    // changes it during the call (it is the guest's context, built before the call and not
    // written by it; the ranges do not overlap the data).
    uint8_t schedule[script_aes::kMaxScheduleBytes];
    std::memcpy(schedule, base + keys + kDecryptKeysOffset, 16 * static_cast<size_t>(rounds + 1));
    uint8_t* const bytes = base + data;
    uint32_t done = 0;  // bytes the recompiled function has already done

    if (!WasChecked(schedule, static_cast<int>(rounds))) {
        const bool first = g_checks_done.load() == 0;
        const uint32_t sample = std::min<uint32_t>(length, first ? kFirstCheckBytes : kLaterCheckBytes);
        std::vector<uint8_t> expected(bytes, bytes + sample);  // off the stack: this thread may be a guest thread
        std::vector<uint8_t> portable(bytes, bytes + sample);
        const auto t0 = std::chrono::steady_clock::now();
        Decrypt(mode, schedule, static_cast<int>(rounds), expected.data(), sample / 16);
        const auto t1 = std::chrono::steady_clock::now();
        if (mode == kAesNi) script_aes::DecryptPortable(schedule, static_cast<int>(rounds), portable.data(), sample / 16, kPasses);
        const auto t2 = std::chrono::steady_clock::now();
        RunRecompiled(ctx, base, data, sample, keys, callback);
        const auto t3 = std::chrono::steady_clock::now();
        done = sample;

        const uint32_t result = ctx.r3.u32;
        const bool same = std::memcmp(bytes, expected.data(), sample) == 0 && result == 1;
        const bool portable_same = mode != kAesNi || std::memcmp(bytes, portable.data(), sample) == 0;
        const auto ns = [](auto a, auto b) {
            return static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
        };
        if (!same) {
            uint32_t first_diff = 0;
            while (first_diff < sample && bytes[first_diff] == expected[first_diff]) ++first_diff;
            g_mode.store(kOff);
            LARECOMP_APP_ERROR("[ScriptAES] MISMATCH: the native decryption differs from the recompiled one (Nr {}, "
                               "{} bytes checked, first difference at byte {}, recompiled returned {}); the native "
                               "path is switched off, the recompiled code does all script decryption from now on",
                               rounds, sample, first_diff, result);
            if (length > done) RunRecompiled(ctx, base, data + done, length - done, keys, callback);
            return;
        }
        if (!portable_same) {
            LARECOMP_APP_ERROR("[ScriptAES] the portable path differs from the recompiled code on the same {} bytes "
                               "(AES-NI agrees): a bug in DecryptPortable, which a CPU without AES-NI would hit",
                               sample);
        }
        MarkChecked(schedule, static_cast<int>(rounds));
        LARECOMP_APP_INFO("[ScriptAES] key schedule checked against the recompiled code: Nr {}, {} bytes identical; "
                          "{}: {} us, recompiled: {} us{}",
                          rounds, sample, mode == kAesNi ? "AES-NI" : "portable", ns(t0, t1) / 1000,
                          ns(t2, t3) / 1000,
                          mode == kAesNi ? (portable_same ? "; the portable path agrees as well" : "") : "");
    }

    if (length > done) {
        const auto t0 = std::chrono::steady_clock::now();
        Decrypt(mode, schedule, static_cast<int>(rounds), bytes + done, (length - done) / 16);
        const auto t1 = std::chrono::steady_clock::now();
        g_native_ns.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()),
                              std::memory_order_relaxed);
    }
    g_bytes.fetch_add(length, std::memory_order_relaxed);
    const uint64_t calls = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (calls == 1 || calls == 10 || calls == 100 || calls == 1000 || calls == 10000) {
        LARECOMP_APP_INFO("[ScriptAES] {} calls so far, {} KB decrypted, {} us in the native code (not counting the "
                          "checked samples)",
                          calls, g_bytes.load(std::memory_order_relaxed) / 1024,
                          g_native_ns.load(std::memory_order_relaxed) / 1000);
    }
    ctx.r3.u64 = 1;
}
// CHECK: speed. On the dev PC (Ryzen 7 9800X3D) a standalone harness decrypts 20000 blocks (16 passes,
// AES-256) in 0.49 ms with AES-NI and 11.8 ms with the portable path; the game's saving (50 to 66 ms
// of recompiled code before) is expected to be about that, not measured in the game.
#endif  // !REXGLUE_HAS_XEO3_TARGET
