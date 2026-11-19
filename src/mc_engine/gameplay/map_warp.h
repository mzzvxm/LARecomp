#pragma once
//
// Warp the player's car to a named point a mod declares (models/<mod>/warps.txt),
// to any point, and back. See map_warp.cpp for the RE map.
//
//   console:  warp <point> | warp back | warp <x> <y> <z> [heading] | warp
//   key:      ui.bind.map_warp (F9; bind_map_warp in a 0.10 larecomp.toml) toggles the first point (or warp_spot) <-> back
//

// Once per frame on the game thread (Patch_DeltaTimePre): carries out a warp
// the console or the key asked for, when the game is not paused.
void TickMapWarp();

// The key was pressed (UI thread, from the hotkey overlay LarecompApp::StartOverlays registers): files the toggle,
// which TickMapWarp carries out.
void MapWarpKeyPressed();
