// Discord Rich Presence: the district the player is driving in.

#ifndef REXGLUE_HAS_XEO3_TARGET

#include "hooks.h"
#include "discord_rpc/discord_rpc.h"
#include "larecomp_log.h"

// The player's current district (return of Racer_GetCurrentDistrict). Fires on
// the game's own district queries -> the RPC updates the area live while driving.
void Hook_CaptureDistrict(PPCRegister& r3) {
    // rpc-diag: confirm the hook fires + what district it sees. Remove later.
    static int last_diag = -999;
    int idx = static_cast<int>(r3.u64);
    if (idx != last_diag) {
        last_diag = idx;
        LARECOMP_APP_INFO("[rpc-diag] district hook fired, idx={}", idx);
    }
    RpcOnDistrictChanged(idx);
}
#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

void Hook_CaptureDistrict(PPCRegister& r3) {}
#endif // REXGLUE_HAS_XEO3_TARGET
