#include "run_ready.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mc::run_ready {
namespace {

bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("LARECOMP_RUN_READY_V1");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

Marks marks;
TitleGate title;

}  // namespace

bool Pending(Event event) { return Enabled() && marks.Pending(event); }

void Note(Event event) {
    if (!Enabled() || !marks.Claim(event)) return;
    // opt_run captures stdout. Flush the marker even when the regular game log is at warn level.
    using namespace std::chrono;
    const auto time_ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    std::fprintf(stdout, "[run-ready-v1] %s %" PRId64 "\n", Name(event), static_cast<int64_t>(time_ms));
    std::fflush(stdout);
}

void NoteTitleDraw() { title.Draw(); }

void NotePadPoll() {
    if (Pending(Event::Title) && title.ReadyOnPoll()) {
        Note(Event::Title);
    }
}

void NoteEntered() { title.Enter(); }

bool Entered() { return title.Entered(); }

}  // namespace mc::run_ready
