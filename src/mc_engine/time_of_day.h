#pragma once
//
// Live time of day. See time_of_day.cpp for the RE map.
//

// Current world hour (0-24), or -1 if the lighting manager is not up yet.
float TimeOfDay_GetHour();

// Ask for a specific hour for as long as the caller keeps asking — used by the
// menu-camera shots, so a saved shot can carry the lighting it was framed in.
// Pass a negative hour to stop asking. The request is honoured on the next
// lighting update and outranks time_of_day_hold; it lapses on its own if the
// caller stops renewing it, so nothing has to be unwound on the way out.
void TimeOfDay_RequestHour(float hour);

// ── Weather ────────────────────────────────────────────────────────────
// Engine indices: 0 Nice, 1 Cloudy, 2 Stormy, 3 Foggy (name table 0x827E1B00).

// Current weather index, or -1 if the globals are not up yet.
int Weather_GetIndex();

// Same renew-or-lapse contract as TimeOfDay_RequestHour: a menu-camera shot can
// carry the sky it was framed under. Pass a negative index to stop asking.
void Weather_RequestIndex(int index);

// Lowercase name <-> index, for the shot file. Returns nullptr / -1 on a miss.
const char* Weather_IndexToName(int index);
int Weather_NameToIndex(const char* name);
