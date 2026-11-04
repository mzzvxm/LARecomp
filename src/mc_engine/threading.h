#pragma once

namespace mc {

void EnableHighResTimer();
void DisableHighResTimer();

// True on a PC with 5 or fewer logical processors, counted as audio_mixer.cpp counts them
// (GetActiveProcessorCount: a job object's or an affinity mask's limit is not seen). Decided
// once; the first call logs the count. The waits that spin on a bigger CPU sleep instead where
// this is true (hooks/pump_wake.cpp, gpu/stream.cpp).
bool FewLogicalProcessors();

} // namespace mc
