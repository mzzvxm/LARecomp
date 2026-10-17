#pragma once
//
// Online play switched on and off in a running game (system_link.cpp). The
// ONLINE checkbox in the SYSTEM LINK menu and the F4 overlay's row
// (pause_menu.cpp) are the callers.
//

// Queues a switch; the game's next net update carries it out. Lock-free, so
// it can be called from the cvar change callback, which runs with the cvar
// registry locked. Switching on can still end offline: the SDK gives up on a
// server that does not answer (mcla_online_connect_timeout_ms).
void SystemLinkOnlineRequest(bool on);

// +1 while a switch on waits for the net update, -1 a switch off, 0 none.
int SystemLinkOnlinePending();

// Online is on and the game's sockets were opened with it on.
bool SystemLinkOnlineConnected();

// True while system_link.cpp itself is setting mcla_online_enable, which the
// change callback must not take for the player's own switch.
bool SystemLinkOnlineSettingCvar();
