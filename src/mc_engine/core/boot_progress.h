// Progress of the host-side content build that runs before the guest starts.
//
// The build (mod archive + custom music) happens on a worker thread while the
// UI thread pumps events and paints, so the two sides need a place to meet.
// Every function here is safe to call from either.
//
// A phase is one line in the popup: a name, a running count and a total. The
// build declares a phase, steps it once per item, and ends it. Nothing here
// knows about ImGui -- the overlay reads a snapshot and draws it.
#pragma once

#include <string>
#include <vector>

namespace mc::boot {

struct Phase {
    std::string name;
    std::string item;  // what is being worked on right now
    int done = 0;
    int total = 0;  // 0 while the count is not known yet
    bool finished = false;
};

// Declares a phase and makes it the current one. `total` may be 0 and filled in
// later with SetTotal.
void BeginPhase(std::string name, int total);

// Corrects the current phase's total once the scan knows it.
void SetTotal(int total);

// One item done. `item` is what was just finished, shown under the bar.
void Step(std::string item);

// Marks the current phase complete. The line stays in the popup.
void EndPhase();

// Everything declared so far, oldest first.
std::vector<Phase> Read();

// The last few items that went by, newest last. The popup shows them because a
// count alone says nothing about WHICH song or file is going in -- and the one
// that is missing afterwards is the interesting one.
std::vector<std::string> Recent();

// Whether anything has been declared at all. Cheap enough to poll.
bool Any();

// Drops every phase. Called once the build is over and the popup is gone.
void Reset();

}  // namespace mc::boot
