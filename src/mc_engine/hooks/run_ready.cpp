#include "run_ready.h"

#include "os/environment.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mc::run_ready {
namespace {

bool Enabled() {
    static const bool enabled = [] {
        // read through os::ReadEnvironment (a copy), like the other runtime readers (static analysis: getenv is not thread safe)
        return larecomp::os::ReadEnvironment("LARECOMP_RUN_READY_V1").value_or("") == "1";
    }();
    return enabled;
}

Marks marks;
TitleGate title;
std::atomic<bool> title_clock_requested{false};
std::atomic<std::chrono::steady_clock::duration::rep> title_started{0};

bool WantsTitleClock(Event event) {
    return event == Event::Title && title_clock_requested.load(std::memory_order_relaxed);
}

}  // namespace

bool Pending(Event event) { return (Enabled() || WantsTitleClock(event)) && marks.Pending(event); }

void Note(Event event) {
    const bool report = Enabled();
    if ((!report && !WantsTitleClock(event)) || !marks.Claim(event)) return;
    // opt_run captures stdout. Flush the marker even when the regular game log is at warn level.
    using namespace std::chrono;
    if (event == Event::Title) {
        title_started.store(steady_clock::now().time_since_epoch().count(), std::memory_order_release);
    }
    if (!report) return;
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

void RequestTitleClock() {
    title_clock_requested.store(true, std::memory_order_relaxed);
}

std::chrono::steady_clock::time_point TitleStart() {
    if (Entered()) return {};
    using Clock = std::chrono::steady_clock;
    return Clock::time_point(Clock::duration(title_started.load(std::memory_order_acquire)));
}

}  // namespace mc::run_ready
