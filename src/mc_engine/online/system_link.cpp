#ifndef REXGLUE_HAS_XEO3_TARGET
//
// System Link (LAN) online session hooks.
//
// The peer-to-peer System Link session works (host/join/cruise) via the RAGE
// session layer relayed by the online client. These hooks smooth over the
// session join content (DLC) compatibility check, which is symmetric and wrongly
// demands DLC even when nobody owns any.
//
// Registered as midasm hooks in larecomp_config.toml:
//   sub_823882B8 region  Patch_BypassContentCheck
//   sub_8238B7E0         Hook_DumpJoinBlob (diagnostic)
//

#include "online_common.h"

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(online_ignore_content_check, true, "MCLA/Multiplayer",
                    "Ignore the session DLC/content compatibility check when joining or hosting "
                    "online. The check is symmetric and fails on any mask difference, so it "
                    "wrongly demands DLC (South Central) even when nobody owns any. Turn off to "
                    "restore the stock behaviour.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(online_diag, false, "MCLA/Multiplayer",
                    "Log online session content masks and the valid-vehicle count at the join "
                    "content check. Used to confirm whether the host's session properties "
                    "transported. Verbose; leave off for normal play.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Online session content (DLC) compatibility gate. sub_8238B7E0 runs a symmetric
// "mine is a superset of theirs" mask test twice on join; any mask difference
// fails both directions, so it wrongly demands DLC (South Central) even when
// nobody owns any. Reporting compatible unconditionally kills both directions.
// Only reachable from sub_8238B7E0, so singleplayer is untouched.
//
// Args at entry: r3 = ptr to local mask dword, r4 = remote mask value (test A)
// or local mask value (test B), r5 = mode (0 = 32-bit compare, 1 = low byte).
// When online_diag is on we dump both masks plus the global content mask
// (0x827E9170) and the valid-vehicle-profile count (obj 0x8288E920 +0x53C).
bool Patch_BypassContentCheck(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) {
    if (REXCVAR_GET(online_diag)) {
        auto* rt = rex::Runtime::instance();
        auto* mem = rt ? rt->memory() : nullptr;
        if (mem) {
            uint32_t local_ptr = static_cast<uint32_t>(r3.u64);
            uint32_t local_mask = IsGuestPtr(local_ptr) ? GuestRead32(mem, local_ptr) : 0;
            uint32_t other = static_cast<uint32_t>(r4.u64);
            uint32_t mode = static_cast<uint32_t>(r5.u64);
            uint32_t global_mask = GuestRead32(mem, 0x827E9170);
            uint32_t valid_count = GuestRead32(mem, 0x8288E920 + 0x53C);
            LARECOMP_APP_INFO(
                "[online-diag] ContentCheck mode={} localMask=0x{:08X} other=0x{:08X} "
                "globalMask=0x{:08X} validVehicles={}",
                mode, local_mask, other, global_mask, valid_count);
        }
    }
    if (!REXCVAR_GET(online_ignore_content_check)) return false;
    r3.u64 = 1;
    return true;  // jump to the blr at 0x823882E4
}

// Diagnostic: dump the session-settings blob the join content check reads.
// sub_8238B7E0(a1=r3=obj, a2=r4=blob) reads MaxPlayers (id 0x10000008) and the
// content mask (ids 0x1000002D+) from this blob. On the joiner the blob is built
// from the found session's search-result properties; if it is empty/garbage the
// join fails (wrong MaxPlayers). This dumps the count word (blob[0]) and the
// parsed key fields. Read-only; gated by online_diag.
void Hook_DumpJoinBlob(PPCRegister& r4) {
    if (!REXCVAR_GET(online_diag)) return;
    auto* rt = rex::Runtime::instance();
    auto* mem = rt ? rt->memory() : nullptr;
    if (!mem) return;

    uint32_t blob = static_cast<uint32_t>(r4.u64);
    if (!IsGuestPtr(blob)) {
        LARECOMP_APP_INFO("[online-diag] JoinBlob ptr=0x{:08X} (not a guest ptr)", blob);
        return;
    }

    // Header is 12 bytes: count@0 then two dwords. Entries start at blob+12,
    // each {id@0, value@4}; stride is 8, or 12 for 64-bit ids (high nibble 2/3/7).
    uint32_t count = GuestRead32(mem, blob + 0);

    uint32_t vehicle_class = 0xFFFFFFFF;  // id 0x10000008
    uint32_t max_players = 0xFFFFFFFF;    // id 0x1000000F
    uint32_t content_mask = 0;            // rebuilt from Content0..7 (0x1000002D+)

    uint32_t off = blob + 12;
    for (uint32_t i = 0; i < count && i < 64; ++i) {
        uint32_t id = GuestRead32(mem, off);
        uint32_t val = GuestRead32(mem, off + 4);
        uint32_t nibble = id >> 28;
        if (id == 0x10000008) vehicle_class = val;
        else if (id == 0x1000000F) max_players = val;
        else if (id >= 0x1000002D && id <= 0x10000034) {
            if (val == 1) content_mask |= (1u << (id - 0x1000002D));
        }
        off += (nibble == 2 || nibble == 3 || nibble == 7) ? 12u : 8u;
    }

    LARECOMP_APP_INFO(
        "[online-diag] JoinBlob ptr=0x{:08X} count={} VehicleClass={} MaxPlayers={} "
        "ContentMask=0x{:02X}",
        blob, count, static_cast<int32_t>(vehicle_class), static_cast<int32_t>(max_players),
        content_mask);
}

// [invite-probe] The online session net-event handler (guest sub_8226D8F8,
// registered on the RAGE session by mcNetManager) raises the in-cruise race
// invite prompt "ol_invite_request_msg" on event id 13. Logging the event ids
// (deduped) shows whether an invite actually reaches the invitee: if id 13 never
// appears when the other player sends a challenge, the invite is lost upstream
// (the XInviteSend / XLIVEBASE 0x0002 no-op, see [invite-probe] in the SDK).
// r4 = event source, r5 = event id. Read-only. Remove once the flow is mapped.
void Hook_InviteReceiveProbe(PPCRegister& r4, PPCRegister& r5) {
    uint32_t src = static_cast<uint32_t>(r4.u64);
    uint32_t ev = static_cast<uint32_t>(r5.u64);
    static uint32_t last = 0xFFFFFFFFu;
    if (ev != last) {
        last = ev;
        LARECOMP_APP_INFO("[invite-probe] net event id={} src=0x{:08X} (13 = race invite)", ev, src);
    }
}

// [invite-probe] SEND side. sub_8228B4A0 is the race-invite send handler: it calls
// sub_82486A58(session+144, target, 1, "ol_invite_request_msg", 0). Logging its
// entry confirms whether challenging a player actually triggers an invite send.
// r3 = handler obj, r4 = target player object.
void Hook_InviteSendProbe(PPCRegister& r3, PPCRegister& r4) {
    LARECOMP_APP_INFO("[invite-probe] SEND handler sub_8228B4A0 fired: obj=0x{:08X} target=0x{:08X}",
                      static_cast<uint32_t>(r3.u64), static_cast<uint32_t>(r4.u64));
}

// [invite-probe] The session message-send primitive (sub_82486A58). a1=r3 msg mgr,
// a2=r4 target xuid list, a3=r5 target count. Internally it drops targets whose
// xuid matches a local player (sub_824F92F8) -- if System Link peers share a
// default xuid the invite target gets filtered out and nothing is sent. Log the
// count and the first target xuid so we can see if a recipient survives.
void Hook_InviteSendPrimitiveProbe(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) {
    auto* rt = rex::Runtime::instance();
    auto* mem = rt ? rt->memory() : nullptr;
    uint32_t targets = static_cast<uint32_t>(r4.u64);
    uint32_t count = static_cast<uint32_t>(r5.u64);
    uint32_t xhi = 0, xlo = 0;
    if (mem && IsGuestPtr(targets)) {
        xhi = GuestRead32(mem, targets + 0);
        xlo = GuestRead32(mem, targets + 4);
    }
    LARECOMP_APP_INFO("[invite-probe] msg-send count={} target0_xuid=0x{:08X}{:08X}", count, xhi,
                      xlo);
}

// [invite-probe] Receiver challenge gate A (guest sub_82282EB8 @0x82282F14). The
// in-cruise "Propose a Challenge" is signalled by session property 3 = the
// challenger's identity, which must replicate to this peer. r3 = the result of
// sub_822952B0(challengeObj, 3): 0 means property 3 never arrived here (the
// challenge signal isn't replicating). This event (24) fires, so if this logs 0
// the signal is lost upstream; if non-zero, the identity gate below decides.
void Hook_InviteGateProp3(PPCRegister& r3) {
    static int last = -1;
    int v = static_cast<int>(r3.u64 & 0xFF);
    if (v != last) {
        last = v;
        LARECOMP_APP_INFO("[invite-probe] receiver gate: property-3(challenge) present = {}", v);
    }
}

// [invite-probe] Receiver challenge gate B (before sub_82293EB0 @0x82282F68). The
// tie-breaker only shows the prompt when challenger > local identity, so equal
// identities (a shared/default gamer handle) block it on BOTH peers. r3 =
// &challenger identity, r4 = &local identity; each is {u32@0, f32@4}. Log both.
void Hook_InviteGateIdentity(PPCRegister& r3, PPCRegister& r4) {
    auto* rt = rex::Runtime::instance();
    auto* mem = rt ? rt->memory() : nullptr;
    uint32_t cp = static_cast<uint32_t>(r3.u64);
    uint32_t lp = static_cast<uint32_t>(r4.u64);
    if (!mem || !IsGuestPtr(cp) || !IsGuestPtr(lp)) return;
    uint32_t c0 = GuestRead32(mem, cp + 0), c4 = GuestRead32(mem, cp + 4);
    uint32_t l0 = GuestRead32(mem, lp + 0), l4 = GuestRead32(mem, lp + 4);
    LARECOMP_APP_INFO(
        "[invite-probe] receiver identity gate: challenger={:08X}:{:08X} local={:08X}:{:08X} equal={}",
        c0, c4, l0, l4, (c0 == l0 && c4 == l4) ? 1 : 0);
}

#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

bool Patch_BypassContentCheck(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) { return false; }
void Hook_DumpJoinBlob(PPCRegister& r4) {}
void Hook_InviteReceiveProbe(PPCRegister& r4, PPCRegister& r5) {}
void Hook_InviteSendProbe(PPCRegister& r3, PPCRegister& r4) {}
void Hook_InviteSendPrimitiveProbe(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) {}
void Hook_InviteGateProp3(PPCRegister& r3) {}
void Hook_InviteGateIdentity(PPCRegister& r3, PPCRegister& r4) {}
#endif // REXGLUE_HAS_XEO3_TARGET
