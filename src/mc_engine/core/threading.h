#pragma once

namespace mc {

void EnableHighResTimer();
void DisableHighResTimer();

// Restrict the process at game startup and log the result. 0 leaves the affinity alone.
void LimitLogicalProcessors(unsigned requested);

// The logical processors the game may run on: the process's affinity mask where it has one
// (a game held to some cores by a job object, `start /affinity` or a tool like Process Lasso has
// only those), else every active one. On a PC with more than 64 (several processor groups) the
// mask covers one group only, so there it is every active one. Linux: the calling thread's
// affinity mask (taskset, a cpuset), else the online processors; 0 on any other OS (not counted).
// A hardware thread counts as one: a Steam Deck is 8, though it has 4 cores.
// Every small-PC decision counts this way, so that a game held to 4 cores of a big CPU behaves
// as it does on a 4-core PC.
unsigned UsableLogicalProcessors();

// True on a PC with 5 or fewer usable logical processors. Decided once; the first call logs the
// count. The waits that spin on a bigger CPU sleep instead where this is true
// (hooks/pump_wake.cpp, gpu/stream.cpp).
bool FewLogicalProcessors();

// True on a PC with 4 or fewer usable logical processors. Decided once. The physics job threads
// run at a lower priority where this is true (hooks/job_workers.cpp, os/thread_priority.h).
bool FourOrFewerLogicalProcessors();

} // namespace mc
