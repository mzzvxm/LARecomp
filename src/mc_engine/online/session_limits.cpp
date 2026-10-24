// Online session size limits. MCLA treats a session of more than 8 players as
// "big" and switches parts of the world off for it; this lifts those switches.
//
// Where the "MaxPlayers > 8" tests live (IDB):
//   sub_8226BAD0 (CreateGame) @0x8226BBE4  stb r11,0x2F4(r27): netMgr+756 =
//       MaxPlayers > 8. Read by sub_82280430 (world props / traffic), sub_822A9D98
//       (damage deformation) and sub_822F0A38 (debris).
//   sub_82289F20 (session defaults), jumptable case 3 @0x8228A0F0: cmpwi r11,8
//       on the MaxPlayers value; above 8 the default of that property becomes 0.
// Neither one touches the Change Vehicle row of the pause menu (that is
// sub_82661CC0 and depends on the net object's +0x10/+0x18, not on this).

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_BOOL(net_lift_big_session_limits, false, "MCLA/Online",
    "Sessions of more than 8 players keep damage, debris, world props and the "
    "default session settings that MCLA turns off above 8. Applies to the next "
    "session you host.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// 0x8226BBE4, before the store: r11 is the "more than 8 players" flag.
void Hook_NetBigSessionFlag(PPCRegister& r11) {
    if (REXCVAR_GET(net_lift_big_session_limits)) r11.u64 = 0;
}

// 0x8228A0F0, before cmpwi r11,8: r11 is the MaxPlayers value being compared.
void Hook_NetBigSessionDefault(PPCRegister& r11) {
    if (REXCVAR_GET(net_lift_big_session_limits)) r11.u64 = 0;
}
#else
// XEO3 stubs: empty implementations so the linker resolves codegen calls.
#include <rex/ppc/context.h>
void Hook_NetBigSessionFlag(PPCRegister&) {}
void Hook_NetBigSessionDefault(PPCRegister&) {}
#endif
