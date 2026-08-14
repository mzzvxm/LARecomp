#pragma once
//
// Real-world weather for the MCLA sky. See real_weather.cpp.
//

// Turn the background poller on or off. Safe to call every frame; the network
// thread is only started the first time it is enabled, so nothing leaves the
// machine unless the feature is actually in use.
void RealWeather_SetEnabled(bool enabled);

// Latest fetched weather as an MCLA index (0 Nice, 1 Cloudy, 2 Stormy,
// 3 Foggy), or -1 while no successful fetch has landed yet.
int RealWeather_GetIndex();

// Where the last fetch resolved to, for the log/UI. Empty until a fetch lands.
const char* RealWeather_GetLocation();
