#pragma once
//
// Prop owners at the start of an online race (race_props.cpp).
//

// Per-frame: polls the word online_race_prop_watch is watching, so a change
// made while the game is paused (lobby) is timestamped too.
void TickRaceProps();
