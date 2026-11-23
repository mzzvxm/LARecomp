#pragma once
// Online session size chosen by the host (cvar net_max_players).

namespace net_limits {

// The largest MaxPlayers the host menu offers: the net_max_players cvar clamped
// to 2..31, or 16 (the game's own limit) when the cvar is left at 16. While this
// returns 16 none of the max-players hooks change anything.
int MaxPlayers();

// True when MaxPlayers() differs from the game's own 16.
bool MaxPlayersChanged();

}  // namespace net_limits
