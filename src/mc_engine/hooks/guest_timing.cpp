#include "guest_timing.h"

#include <chrono>
#include <format>
#include <iterator>
#include <memory>
#include <string>

#include "larecomp_log.h"
#include "settings/generated/settings_diagnostics.h"

namespace mc::guest_timing {
namespace {

using mcla::gpu::cycles::Now;
using Clock = std::chrono::steady_clock;

struct Lane {
    Frame frame;
    Window window;
    uint64_t begin = 0;
    uint64_t end_frame = 0;
    uint64_t clock_ticks = 0;
    Clock::time_point clock_wall;
    uint64_t completed = 0;
};

struct Session {
    Lane update;
    Lane submit;
    Phases phases;
    bool in_update = false;
};

// Keep the off path's TLS small. A thread's windows are released on toggle or thread exit.
thread_local std::unique_ptr<Session> session;

Session* ReadSetting() {
    if (larecomp::settings::mcla_guest_timing.Get()) {
        if (!session) session.reset(new Session);
    } else if (!active_frame && session) {
        session.reset();
    }
    return session.get();
}

void Begin(Lane& lane) {
    lane.frame = {};
    lane.end_frame = 0;
    lane.begin = Now();
    if (!lane.clock_ticks) {
        lane.clock_wall = Clock::now();
        lane.clock_ticks = lane.begin;
    }
    active_frame = &lane.frame;
}

void Finish(Lane& lane, Section section, const char* name, uint64_t end) {
    active_frame = nullptr;
    lane.frame.Add(section, lane.begin, end);
    lane.begin = 0;
    ++lane.completed;
    if (!lane.window.Push(lane.frame)) return;

    // This conversion belongs to this lane. The render thread's TSC calibration is render-thread only.
    const uint64_t tick_span = Elapsed(lane.clock_ticks, Now());
    const double wall_ms = std::chrono::duration<double, std::milli>(Clock::now() - lane.clock_wall).count();
    if (tick_span) {
        const double ms_per_tick = wall_ms / double(tick_span);
        std::string line;
        for (size_t i = 0; i < kSections; ++i) {
            const auto result = lane.window.Read(i);
            if (result.calls == 0) continue;
            std::format_to(std::back_inserter(line), " | {} {:.4f}/{:.4f} ms {:.1f} calls/frame",
                           kNames[i], result.mean * ms_per_tick, double(result.p95) * ms_per_tick, result.calls);
        }
        LARECOMP_APP_INFO("[guest timing] {} frames {}-{} (mean/p95){}", name,
                          lane.completed - kWindowFrames + 1, lane.completed, line);
    }
    lane.window.Reset();
    lane.clock_ticks = 0;
}

}  // namespace

thread_local constinit Frame* active_frame = nullptr;

void EnterUpdate() {
    if (Session* state = ReadSetting()) state->in_update = true;
}

void BeginUpdate() {
    if (session && session->in_update && !active_frame) Begin(session->update);
}

void EndUpdate() {
    if (!session) return;
    session->in_update = false;
    if (active_frame == &session->update.frame) Finish(session->update, Section::kUpdate, "update", Now());
}

void BeginSubmit() {
    if (!active_frame) {
        if (Session* state = ReadSetting()) Begin(state->submit);
    }
}

void MarkEndFrame() {
    if (active_frame && session && active_frame == &session->submit.frame && !session->submit.end_frame) {
        session->submit.end_frame = Now();
    }
}

void EndSubmit() {
    if (!active_frame || !session || active_frame != &session->submit.frame) return;
    Lane& lane = session->submit;
    session->phases = {};
    // CHECK: confirm the first world pass, grcDevice::EndFrame and Swap have
    // the same guest owner in roam, race and pause. Ignore incomplete brackets.
    if (lane.end_frame) {
        Finish(lane, Section::kSubmit, "submit", lane.end_frame);
    } else {
        active_frame = nullptr;
    }
}

void BeginPhases(uint32_t requested) {
    if (active_frame && session && active_frame == &session->submit.frame) {
        session->phases = {Now(), requested};
    }
}

void EndPhase(uint32_t phase) {
    if (active_frame && session && session->phases.cursor) {
        session->phases.End(*active_frame, phase, Now());
    }
}

void EndPhases() {
    if (active_frame && session) session->phases = {};
}

}  // namespace mc::guest_timing
