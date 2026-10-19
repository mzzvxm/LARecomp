#ifndef REXGLUE_HAS_XEO3_TARGET
//
// XSession glue (game-side), shared by Xbox LIVE and System Link.
//
// The Xbox LIVE emulation layer itself (XLIVEBASE / XGI presence, invites,
// storage, matchmaking) lives in the rexglue SDK; this module holds the
// game-side midasm hooks that keep the game and that layer from hurting each
// other.
//
// XSessionCreate's guest wrapper (sub_8274B440, every online session the game
// makes: System Link and LIVE, host and joiner) does
//
//   XamSessionCreateHandle(&h); XamSessionRefObjByHandle(h, &obj);
//   buffer = { obj, flags, public, private, xuid, &session_info, &nonce };
//   XMsgStartIORequest(0xFB, 0xB0010, overlapped, buffer, 0x1C);
//
// XAM's session object is a placeholder here: XamSessionRefObjByHandle hands
// back 0xDEADF00D, and the 0xB0010 handler then stores 0xDEADF00D *at* that
// address. 0xDEADF00D is not an invalid address on this memory map: the
// 0xC0000000 physical view puts it on physical 0x1EADF00D, which the game's own
// heaps also reach through the 0xA0000000 view as 0xBEADF00D. Every session the
// game created wrote four bytes into whatever game object lived there. With
// the mods in models/, that was a race prop's owner (mcProp at 0xBEADF000, its
// inst pointer at +16): the next online race start re-synced the prop and
// dereferenced the broken pointer (race_props.cpp has the measurement and a
// guard that stays as a safety net).
//
// The handler only stores to that pointer when it is non-zero, and nothing on
// either side reads the object, so the buffer goes out with 0 instead.
//
// Registered as a midasm hook in larecomp_config.toml:
//   0x8274B584  `stw r11, buffer+0` (r11 = obj)   Hook_SessionCreateObject
//

#include "online_common.h"

#include <atomic>

void Hook_SessionCreateObject(PPCRegister& r11) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        LARECOMP_APP_INFO(
            "[xsession] XSessionCreate: placeholder session object {:08X} left out of the "
            "request, so XAM no longer writes into guest memory there",
            static_cast<uint32_t>(r11.u64));
    }
    r11.u64 = 0;
}

#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

void Hook_SessionCreateObject(PPCRegister& r11) {}

#endif // REXGLUE_HAS_XEO3_TARGET
