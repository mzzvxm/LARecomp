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

#include <cstring>

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

// ---------------------------------------------------------------------------
// [challenge-probe] "Propose a Challenge" gates.
//
// Verified flow (IDA, MCLA default.xex):
//   sub_82280E38  builds the signal as a DEADLINE on the *local* network clock:
//                 now = sub_8226B2F0(netmgr) -> deadline = now + timeout (5..35s)
//   sub_8227B770  publishes it directly if we own broadcast-data group 0
//                 (sub_8227E750), otherwise messages the owner, who republishes.
//   sub_822837B8  the owner writes the group-0 broadcast properties
//                 (0=type 1=race 2=1 3=DEADLINE 5=index 7=challenger gamer id).
//   sub_82282EB8  every peer gets net event 24 and decides whether to raise the
//                 phone prompt "ol_accept_challenge" (via sub_82282C38).
//
// The prompt is gated on the RAGE network clock in four places. The player-list
// status is not - it rides the per-player netPlayer props (sub_82274120) - which
// is exactly why the other players see "in a race" but never get the invite.
//
// Gate order inside sub_82282EB8, every one a silent beq:
//   0x82282EFC  sub_8226B2A0  clock started (+92 & 0x80) AND synced (& 0x40)
//   0x82282F1C  sub_822952B0  property 3 replicated to this peer
//   0x82282F74  sub_82293EB0  deadline > my network time
//   0x82282FB0  sub_8227E670  challenger resolved from property 7
// plus the same deadline test on the owner before it republishes (0x82283850).
//
// The previous probes sat at 0x82282F14 / 0x82282F68, i.e. *after* the clock
// gate's branch: if the clock gate is the one failing they can never fire, which
// is why the earlier two-machine runs logged nothing conclusive. The set below
// covers the clock gate itself and prints the two times actually being compared.
// ---------------------------------------------------------------------------

namespace {

// Challenge timestamps are rage netTime: {int32 seconds, float fraction}.
struct ChallengeTime {
    uint32_t sec = 0;
    float frac = 0.0f;
    double value() const { return static_cast<double>(static_cast<int32_t>(sec)) + frac; }
};

bool ReadChallengeTime(rex::memory::Memory* mem, uint32_t ea, ChallengeTime& out) {
    if (!mem || !IsGuestPtr(ea)) return false;
    out.sec = GuestRead32(mem, ea + 0);
    uint32_t raw = GuestRead32(mem, ea + 4);
    std::memcpy(&out.frac, &raw, sizeof(out.frac));
    return true;
}

// mcNetManager (dword_8286F1D8) +8 = the network clock (mcNetworkClock, a
// rage netTimeSync subclass). There is no GuestRead8 helper, so the flag byte at
// +92 is taken from the top of the big-endian dword there.
struct ClockState {
    bool valid = false;
    uint32_t flags = 0;      // +92: 0x80 = started, 0x40 = has a synced time
    uint32_t offset_ms = 0;  // +16: netTimeSync offset applied to the raw timer
    uint32_t high_ms = 0;    // +84: monotonic high-water mark of the reported time
};

ClockState ReadClock(rex::memory::Memory* mem) {
    ClockState st;
    if (!mem) return st;
    uint32_t netmgr = GuestRead32(mem, 0x8286F1D8);
    if (!IsGuestPtr(netmgr)) return st;
    uint32_t clock = GuestRead32(mem, netmgr + 8);
    if (!IsGuestPtr(clock)) return st;
    st.flags = GuestRead32(mem, clock + 92) >> 24;
    st.offset_ms = GuestRead32(mem, clock + 16);
    st.high_ms = GuestRead32(mem, clock + 84);
    st.valid = true;
    return st;
}

}  // namespace

// Gate 1, receiver (0x82282EF4): r3 = sub_8226B2A0() = clock started AND synced.
// 0 here means this peer's network clock is unusable and NO challenge prompt can
// ever appear, no matter who sends it. The flag dump separates the two causes:
//   started=0            -> sub_8226D120 never resolved the group-0 owner, so the
//                           clock was never started in client mode (rec+11780 /
//                           sub_82482560 path).
//   started=1 synced=0   -> the netTimeSync request/response on connection
//                           channel 10 is not completing against the host.
void Hook_ChallengeClockGate(PPCRegister& r3) {
    static int last_ok = -1;
    static uint32_t last_flags = 0xFFFFFFFFu;
    int ok = static_cast<int>(r3.u64 & 0xFF);
    auto* rt = rex::Runtime::instance();
    ClockState st = ReadClock(rt ? rt->memory() : nullptr);
    if (ok == last_ok && st.flags == last_flags) return;
    last_ok = ok;
    last_flags = st.flags;
    LARECOMP_APP_INFO(
        "[challenge-probe] receiver clock gate = {} (started={} synced={} offset={}ms now={}ms)",
        ok, (st.flags & 0x80) ? 1 : 0, (st.flags & 0x40) ? 1 : 0,
        static_cast<int32_t>(st.offset_ms), st.high_ms);
}

// Gate 2, receiver (0x82282F14): r3 = sub_822952B0(challengeObj, 3) = did the
// group-0 broadcast property 3 (the deadline) replicate to this peer at all.
// 0 means the challenge never arrived here; non-zero means the time gate below
// decides.
void Hook_ChallengeProp3Gate(PPCRegister& r3) {
    static int last = -1;
    int v = static_cast<int>(r3.u64 & 0xFF);
    if (v == last) return;
    last = v;
    LARECOMP_APP_INFO("[challenge-probe] receiver property-3 (deadline) present = {}", v);
}

// Gate 3, receiver (0x82282F68): sub_82293EB0(deadline, now) - the prompt only
// shows while the challenge has not expired. r3 = &deadline (produced on the
// SENDER's clock), r4 = &now (this peer's clock). A negative delta larger than
// the challenge window (5..35s) means the two clocks are in different domains,
// i.e. netTimeSync never aligned them.
void Hook_ChallengeTimeGate(PPCRegister& r3, PPCRegister& r4) {
    auto* rt = rex::Runtime::instance();
    auto* mem = rt ? rt->memory() : nullptr;
    ChallengeTime deadline, now;
    if (!ReadChallengeTime(mem, static_cast<uint32_t>(r3.u64), deadline)) return;
    if (!ReadChallengeTime(mem, static_cast<uint32_t>(r4.u64), now)) return;
    LARECOMP_APP_INFO(
        "[challenge-probe] receiver time gate: deadline={:.3f}s now={:.3f}s delta={:+.3f}s pass={}",
        deadline.value(), now.value(), deadline.value() - now.value(),
        deadline.value() > now.value() ? 1 : 0);
}

// Sender/owner side (0x82283818): r3 = sub_8227E750() = do I own broadcast group
// 0. sub_822837B8 only publishes the challenge when this is 1; a guest's
// "Propose a Challenge" is messaged to the owner, which republishes. If this logs
// 1 on more than one machine the group ownership is ambiguous.
void Hook_ChallengeIsGroupHost(PPCRegister& r3) {
    static int last = -1;
    int v = static_cast<int>(r3.u64 & 0xFF);
    if (v == last) return;
    last = v;
    LARECOMP_APP_INFO("[challenge-probe] publish: am I the group-0 owner = {}", v);
}

// Sender/owner side (0x82283850): the same deadline test, run by the owner before
// it writes the properties. r3 = &deadline (from the requesting peer), r4 = &now
// (owner's clock). If this fails, nothing is published and NOBODY sees the invite
// while the requester's own player state has already flipped to "challenging" -
// the exact symptom being chased.
void Hook_ChallengeHostPublishGate(PPCRegister& r3, PPCRegister& r4) {
    auto* rt = rex::Runtime::instance();
    auto* mem = rt ? rt->memory() : nullptr;
    ChallengeTime deadline, now;
    if (!ReadChallengeTime(mem, static_cast<uint32_t>(r3.u64), deadline)) return;
    if (!ReadChallengeTime(mem, static_cast<uint32_t>(r4.u64), now)) return;
    LARECOMP_APP_INFO(
        "[challenge-probe] publish time gate: deadline={:.3f}s now={:.3f}s delta={:+.3f}s pass={}",
        deadline.value(), now.value(), deadline.value() - now.value(),
        deadline.value() > now.value() ? 1 : 0);
}

// Gate 4, receiver (0x82282FAC): r3 = sub_8227E670(mgr, &gamerId) with the gamer
// id taken from broadcast property 7. 0 = the challenger could not be resolved to
// a live player on this machine, and sub_82282C38 (the prompt) is never called.
// The gamer id is 8 bytes derived from abEnet ^ hash32(gamertag), NOT the XUID.
void Hook_ChallengeResolveChallenger(PPCRegister& r3) {
    static uint32_t last = 0xFFFFFFFFu;
    uint32_t v = static_cast<uint32_t>(r3.u64);
    if (v == last) return;
    last = v;
    LARECOMP_APP_INFO("[challenge-probe] challenger resolved from property 7 = 0x{:08X}", v);
}

// Everything below is inside sub_82282C38, the function that actually raises the
// phone prompt. It has four more gates after the ones in sub_82282EB8, and any of
// them silently swallows the challenge (the requester's player state has already
// flipped to "challenging", which is the reported symptom).

// 0x82282C78: r11 = *(mgr+0x10) = challenge state, must not be 4. Also dumps the
// global "a challenge prompt is already pending" byte at 0x82873D94, which blocks
// a second prompt until sub_82291508 clears it.
void Hook_PromptEntryGate(PPCRegister& r11) {
    static uint32_t last_state = 0xFFFFFFFFu;
    static uint32_t last_pending = 0xFFFFFFFFu;
    auto* rt = rex::Runtime::instance();
    auto* mem = rt ? rt->memory() : nullptr;
    uint32_t state = static_cast<uint32_t>(r11.u64);
    uint32_t pending = mem ? (GuestRead32(mem, 0x82873D94) >> 24) : 0xFFFFFFFFu;
    if (state == last_state && pending == last_pending) return;
    last_state = state;
    last_pending = pending;
    LARECOMP_APP_INFO("[challenge-probe] prompt entry: state={} (4 blocks) pending_flag={}", state,
                      pending);
}

// 0x82282C9C: r3 = sub_82293EB0(deadline, now) again, this time on the prompt path.
void Hook_PromptTimeGate(PPCRegister& r3) {
    static int last = -1;
    int v = static_cast<int>(r3.u64 & 0xFF);
    if (v == last) return;
    last = v;
    LARECOMP_APP_INFO("[challenge-probe] prompt time gate pass = {}", v);
}

// 0x82282DB0: r11 = *(mgr+0x10) once more, this time it must be exactly 0 or the
// prompt is replaced by sub_8268EE10(*mgr, 20, 3, -1) - no popup, no error.
void Hook_PromptStateIdle(PPCRegister& r11) {
    static uint32_t last = 0xFFFFFFFFu;
    uint32_t v = static_cast<uint32_t>(r11.u64);
    if (v == last) return;
    last = v;
    LARECOMP_APP_INFO("[challenge-probe] prompt state-idle gate: state={} (needs 0)", v);
}

// 0x82282DC8: r3 = sub_822090B8() = the "Invisible" or "Tutorial" flow state
// reports done (vfunc 0x138). 0 here means the game considers the player not ready
// to be prompted.
void Hook_PromptFlowGate(PPCRegister& r3) {
    static int last = -1;
    int v = static_cast<int>(r3.u64 & 0xFF);
    if (v == last) return;
    last = v;
    LARECOMP_APP_INFO("[challenge-probe] prompt flow gate (Invisible/Tutorial) = {}", v);
}

// 0x82282DE4: r3 = sub_821E80F8(dword_82874374, idx) - a bounds + non-null lookup
// in a table, with idx = *(challenger+0x88) carried in r25. 0 means the entry for
// this challenger is missing on the receiver, and the prompt is dropped.
void Hook_PromptTableGate(PPCRegister& r3, PPCRegister& r25) {
    static int last = -1;
    static uint32_t last_idx = 0xFFFFFFFFu;
    int v = static_cast<int>(r3.u64 & 0xFF);
    uint32_t idx = static_cast<uint32_t>(r25.u64);
    if (v == last && idx == last_idx) return;
    last = v;
    last_idx = idx;
    LARECOMP_APP_INFO("[challenge-probe] prompt table gate = {} (index={})", v,
                      static_cast<int32_t>(idx));
}

// 0x82282E8C: sub_8264F2F8 - the phone prompt is going up right now.
void Hook_PromptRaised() {
    LARECOMP_APP_INFO("[challenge-probe] PROMPT RAISED (ol_accept_challenge)");
}

// 0x82282EA8: the fallback taken whenever one of the three gates above fails -
// sub_8268EE10(*mgr, 20, 3, -1) instead of the popup.
void Hook_PromptDropped() {
    LARECOMP_APP_INFO("[challenge-probe] prompt DROPPED (fallback path, no popup)");
}

#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

bool Patch_BypassContentCheck(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) { return false; }
void Hook_DumpJoinBlob(PPCRegister& r4) {}
void Hook_InviteReceiveProbe(PPCRegister& r4, PPCRegister& r5) {}
void Hook_InviteSendProbe(PPCRegister& r3, PPCRegister& r4) {}
void Hook_InviteSendPrimitiveProbe(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) {}
void Hook_ChallengeClockGate(PPCRegister& r3) {}
void Hook_ChallengeProp3Gate(PPCRegister& r3) {}
void Hook_ChallengeTimeGate(PPCRegister& r3, PPCRegister& r4) {}
void Hook_ChallengeIsGroupHost(PPCRegister& r3) {}
void Hook_ChallengeHostPublishGate(PPCRegister& r3, PPCRegister& r4) {}
void Hook_ChallengeResolveChallenger(PPCRegister& r3) {}
void Hook_PromptEntryGate(PPCRegister& r11) {}
void Hook_PromptTimeGate(PPCRegister& r3) {}
void Hook_PromptStateIdle(PPCRegister& r11) {}
void Hook_PromptFlowGate(PPCRegister& r3) {}
void Hook_PromptTableGate(PPCRegister& r3, PPCRegister& r25) {}
void Hook_PromptRaised() {}
void Hook_PromptDropped() {}
#endif // REXGLUE_HAS_XEO3_TARGET
