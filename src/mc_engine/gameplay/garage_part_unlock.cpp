#ifndef REXGLUE_HAS_XEO3_TARGET
// Test switch: opens the garage parts the career has not unlocked yet.
//
//   sub_826BCDD8  "is this part category unlocked for this car class". Its one
//                 caller is the garage parts menu (sub_826A92E0, 0x826A9664),
//                 which greys out the entries past the first ones when it says
//                 no. Read only: the career's unlock table is not touched, so
//                 turning the switch off brings the locks back. A part bought
//                 while it is on is a normal purchase and stays in the save.
#include <cstdint>

#include <rex/cvar.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_BOOL(garage_unlock_parts, false, "MCLA/Patches",
    "For testing: every entry of the garage part menus can be picked, whatever "
    "the career has unlocked. Parts bought with it on stay bought.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__rex_sub_826BCDD8);  // part category unlocked?

extern "C" REX_FUNC(rex_sub_826BCDD8) {
    if (REXCVAR_GET(garage_unlock_parts)) {
        ctx.r3.u64 = 1;
        return;
    }
    __imp__rex_sub_826BCDD8(ctx, base);
}

#endif  // REXGLUE_HAS_XEO3_TARGET
