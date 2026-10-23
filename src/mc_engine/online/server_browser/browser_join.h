#pragma once
//
// Joining the room the server browser picked, through the game's own Quick
// Match: the browser names a host by its vip,
// the game's search keeps only that host (System Link and Live), and the
// script that joins the first result does the rest. If the host is gone the
// script would make us host instead; that is vetoed into the game's own
// "networking problem" error.
//

#include <cstdint>
#include <string>
#include <vector>

#include <rex/ppc/context.h>

#include "../protocol/browser_protocol.h"

namespace larecomp::browser {

struct JoinRequest {
    // The room to join, by its host's vip. 0 with any_room = "play on this
    // server": the game's own Quick Match/Cruise, which joins any session of
    // that mode on the relay or, when there is none, hosts one for whoever
    // comes next.
    uint32_t host_vip = 0;
    bool any_room = false;
    bool cruise = false;  // the room's GameMode is Cruise (else a race/battle session)
    bool lan = true;      // System Link (else the XBOX LIVE menu)
    std::string label;    // for the log and the status line
};

enum class JoinPhase {
    kIdle,
    kGoingOnline,  // switching online on first
    kStarting,     // driving the menus to the search
    kSearching,    // the game is searching, only the picked host passes
    kJoining,      // the game asked to join it
    kFailed,
};

struct JoinStatus {
    JoinPhase phase = JoinPhase::kIdle;
    std::string text;  // what to show; empty when idle
};

// Any thread. Replaces a join still under way.
void RequestJoin(const JoinRequest& request);
void CancelJoin();
JoinStatus GetJoinStatus();

// The local player is in the city: the online menus can open. Their fill
// reads the player's car (sub_822A5680) and crashes on the boot screens.
// Updated by the guest tick; any thread.
bool WorldReady();

// Guest thread, once per frame (Patch_DeltaTimePre).
void TickServerBrowserJoin();

// A session property as the game names it, from its table at 0x827DEE20.
struct PropText {
    std::string name;
    std::string value;
};
// Empty until the game has filled the table (a few seconds into boot).
std::vector<PropText> DescribeProps(const std::vector<Prop>& props);
// The parts of a room the join needs, from its properties.
void ClassifyRoom(const std::vector<Prop>& props, bool* cruise, bool* lan);

}  // namespace larecomp::browser

// Midasm hooks (larecomp_config.toml).
// 0x824FD3A0 in sub_824FD178: a System Link search answer is about to be kept
// (r27 = 1); r1+0x58 holds the vip it came from.
void Hook_BrowserLanResult(PPCRegister& r1, PPCRegister& r27);
// 0x824FC8D0 in sub_824FC7B0: a Live search result passed its first check
// (r3); r30 = the XSESSION_SEARCHRESULT, host vip at +8.
void Hook_BrowserLiveResult(PPCRegister& r30, PPCRegister& r3);
// 0x8226BB2C in sub_8226BAD0 (CreateGame): true leaves through the function's
// own failure exit (loc_8226BCCC: posts net.error, returns 0).
bool Hook_BrowserCreateGameVeto(PPCRegister& r27);
// 0x8226BCF0, JoinSession: the script asked to join the found game.
void Hook_BrowserJoinSession(PPCRegister& r3);
