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

#include "settings/larecomp_settings.h"
#include <rex/ppc/recompiled_code.h>
#include <rex/ppc/context.h>
#include "larecomp_guest.h"

REX_EXTERN(__imp__rex_sub_826BCDD8);  // part category unlocked?

extern "C" REX_FUNC(rex_sub_826BCDD8) {
    if (larecomp::settings::garage_unlock_parts.Get()) {
        ctx.r3.u64 = 1;
        return;
    }
    __imp__rex_sub_826BCDD8(ctx, base);
}

#endif  // REXGLUE_HAS_XEO3_TARGET
