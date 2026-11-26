#pragma once

#include "gpu/cycles.h"
#include "guest_timing_rules.h"

namespace mc::guest_timing {

// Only the game update and graphics submission threads arm a frame. Hot hooks
// on other threads, or with timing off, do one TLS read and no timestamp.
extern thread_local constinit Frame* active_frame;

class Scope {
public:
    explicit Scope(Section section, bool enabled = true)
        : frame_(enabled ? active_frame : nullptr), section_(section),
          start_(frame_ ? mcla::gpu::cycles::Now() : 0) {}
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    ~Scope() {
        if (!frame_) return;
        const uint64_t end = mcla::gpu::cycles::Now();
        frame_->Add(section_, start_, end);
        if (also_ != Section::kNone) frame_->Add(also_, start_, end);
    }

    explicit operator bool() const { return frame_ != nullptr; }
    void Also(Section section) { also_ = section; }

private:
    Frame* frame_;
    Section section_;
    uint64_t start_;
    Section also_ = Section::kNone;
};

void EnterUpdate();
void BeginUpdate();
void EndUpdate();
void BeginSubmit();
void MarkEndFrame();
void EndSubmit();
void BeginPhases(uint32_t requested);
void EndPhase(uint32_t phase);
void EndPhases();

}  // namespace mc::guest_timing
