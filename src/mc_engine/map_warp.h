#pragma once
//
// Warp the player's car to a named point a mod declares (models/<mod>/warps.txt),
// to any point, and back. See map_warp.cpp for the RE map.
//
//   console:  warp <point> | warp back | warp <x> <y> <z> [heading] | warp
//   key:      bind_map_warp (F9) toggles the first point (or warp_spot) <-> back
//

// Once per frame on the game thread (Patch_DeltaTimePre): carries out a warp
// the console or the key asked for, when the game is not paused.
void TickMapWarp();
