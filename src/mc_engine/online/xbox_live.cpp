#ifndef REXGLUE_HAS_XEO3_TARGET
//
// Xbox LIVE online hooks (game-side).
//
// The Xbox LIVE emulation layer itself (XLIVEBASE / XGI presence, invites,
// storage, matchmaking) lives in the rexglue SDK, not here. This module is the
// home for any *game-side* midasm hooks that touch the LIVE path (e.g. presence
// invite prompts, friends UI glue). None are needed yet -- placeholder so the
// online module split has a clear slot for them.
//

#include "online_common.h"

#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

#endif // REXGLUE_HAS_XEO3_TARGET
