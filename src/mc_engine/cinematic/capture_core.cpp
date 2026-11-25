//
// The pure half of capture mode (trailer toolkit M2a). See capture_core.h.
//

#include "capture_core.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdio>

namespace mc::cinematic::capture {

// ---------------------------------------------------------------------------------------------------------------- the step

double ClampFps(double fps) {
    if (!(fps > 0.0)) return 0.0;  // also NaN
    return std::clamp(fps, kMinFps, kMaxFps);
}

uint64_t StepTicksForFps(double fps) {
    const double f = ClampFps(fps);
    if (f == 0.0) return 0;
    return static_cast<uint64_t>(std::llround((1.0 / f) / kGameTickSeconds));
}

double FpsForStepTicks(uint64_t ticks) {
    if (ticks == 0) return 0.0;
    return std::round(1.0 / (static_cast<double>(ticks) * kGameTickSeconds) * 1e6) / 1e6;
}

double StepMsForTicks(uint64_t ticks) {
    return static_cast<double>(ticks) * kGameTickSeconds * 1000.0;
}

TakeSceneClock PlanTakeSceneClock(double request_time, double fps, double requested_t0) {
    const double step = 1.0 / fps;
    if (requested_t0 >= 0.0) return {requested_t0, step};
    // A decimal scene key on the grid can round just below the corresponding integer in the division.
    const double grid = std::nextafter(request_time / step, std::numeric_limits<double>::infinity());
    return {(std::floor(grid) + 1.0) / fps, step};
}

double StepToTakeKey(double scene_t, double dt, double key, bool ready_opened) {
    if (ready_opened && key == 0.0) return 0.0;
    return scene_t < key ? std::min(dt, key - scene_t) : dt;
}

// ---------------------------------------------------------------------------------------------------------------- the hand-over

HandOver HandOverBook::Note(uint64_t update, bool take_active, uint64_t take_first) {
    HandOver h;
    if (update == 0) return h;  // before the first update: nothing was simulated, nothing to show
    if (update == last_update_) {
        // No update since the last hand-over: a screen the game draws without running its timer. Not an update's frame, so not
        // a frame of the take (a take has exactly one picture for each update).
        h.repeat = true;
        ++repeats_;
        return h;
    }
    last_update_ = update;
    h.take_frame = take_active && update >= take_first;
    h.tag = MakeTag(update, h.take_frame);
    if (h.take_frame) ++handed_;
    return h;
}

// ---------------------------------------------------------------------------------------------------------------- the gate

uint64_t FramesNeeded(uint64_t handed, uint32_t lag, bool ending) {
    const uint64_t allowed = ending ? 0 : lag;
    return handed > allowed ? handed - allowed : 0;
}

int64_t GateBook::TimedOut(uint64_t needed) {
    ++timeouts;
    ++consecutive_timeouts;
    const int64_t short_by = static_cast<int64_t>(needed) - (static_cast<int64_t>(captured) + resync);
    if (short_by > 0) resync += short_by;
    if (consecutive_timeouts >= 3) off = true;
    return short_by > 0 ? short_by : 0;
}

// ---------------------------------------------------------------------------------------------------------------- the take

void TakeBook::Begin(uint64_t first_update) {
    *this = TakeBook{};
    first_ = first_update;
}

bool TakeBook::OnFrame(uint64_t update, bool lost) {
    const uint64_t want = have_last_ ? last_ + 1 : first_;
    const bool ok = update == want;
    if (!ok) {
        ++anomalies_;
        if (update < want) ++repeated_;
        else skipped_ += update - want;
    }
    ++rows_;
    if (lost) ++lost_;
    last_ = update;
    have_last_ = true;
    return ok;
}

Verdict Judge(const TakeBook& book, uint64_t end_update, uint64_t handed) {
    Verdict v;
    const uint64_t updates = book.UpdatesRun(end_update);
    const uint64_t last = end_update > 0 ? end_update - 1 : 0;
    char buf[256];
    const bool exact = book.Rows() == updates && book.Lost() == 0 && book.Anomalies() == 0 && handed == updates;
    v.exact = exact;
    // Updates that got a picture: the rows that were read back and were not a repeat. The rest of the updates have none.
    const uint64_t good = book.Rows() > book.Lost() + book.Repeated() ? book.Rows() - book.Lost() - book.Repeated() : 0;
    v.dropped = updates > good ? updates - good : 0;
    v.repeated = book.Repeated();
    if (exact) {
        std::snprintf(buf, sizeof(buf), "%llu frames for %llu updates (%llu..%llu): exact", static_cast<unsigned long long>(book.Rows()),
                      static_cast<unsigned long long>(updates), static_cast<unsigned long long>(book.FirstUpdate()),
                      static_cast<unsigned long long>(last));
    } else {
        std::snprintf(buf, sizeof(buf),
                      "%llu frames captured (%llu lost, %llu out of order) for %llu updates (%llu..%llu), %llu frames handed over",
                      static_cast<unsigned long long>(book.Rows()), static_cast<unsigned long long>(book.Lost()),
                      static_cast<unsigned long long>(book.Anomalies()), static_cast<unsigned long long>(updates),
                      static_cast<unsigned long long>(book.FirstUpdate()), static_cast<unsigned long long>(last),
                      static_cast<unsigned long long>(handed));
    }
    v.text = buf;
    return v;
}

// ---------------------------------------------------------------------------------------------------------------- the scene clock

bool VirtualClock::SetActive(bool active) {
    if (active == active_) return false;
    active_ = active;
    ++epoch_;
    return true;
}

double VirtualClock::NowMs(int kind, uint64_t frame, double steady_ms, double step_ms) {
    if (!active_) return steady_ms;
    Anchor& a = anchors_[kind == 0 ? 0 : 1];
    if (a.epoch != epoch_) {
        a.epoch = epoch_;
        a.frame = frame;
        a.ms = steady_ms;
    }
    return a.ms + (static_cast<double>(frame) - static_cast<double>(a.frame)) * step_ms;
}

// ---------------------------------------------------------------------------------------------------------------- statistics

Stat StatOf(std::vector<double> v) {
    Stat s;
    v.erase(std::remove_if(v.begin(), v.end(), [](double x) { return x < 0.0; }), v.end());
    s.n = v.size();
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    double sum = 0.0;
    for (const double x : v) sum += x;
    s.mean = sum / static_cast<double>(v.size());
    s.p50 = v[v.size() / 2];
    s.p95 = v[std::min(v.size() - 1, static_cast<size_t>(static_cast<double>(v.size()) * 0.95))];
    s.max = v.back();
    return s;
}

std::string FormatStat(const Stat& s) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%.2f/%.2f/%.2f", s.mean, s.p95, s.max);
    return buf;
}

// ---------------------------------------------------------------------------------------------------------------- the sink

bool ParseSinkKind(const std::string& text, SinkKind* out) {
    *out = SinkKind::kNone;
    if (text == "none") return true;
    if (text == "ffmpeg") {
        *out = SinkKind::kFfmpeg;
        return true;
    }
    if (text == "raw") {
        *out = SinkKind::kRaw;
        return true;
    }
    if (text == "png") {
        *out = SinkKind::kPng;
        return true;
    }
    return false;
}

const char* SinkKindName(SinkKind kind) {
    switch (kind) {
        case SinkKind::kNone: return "none";
        case SinkKind::kFfmpeg: return "ffmpeg";
        case SinkKind::kRaw: return "raw";
        case SinkKind::kPng: return "png";
        case SinkKind::kLuma: return "luma";
    }
    return "none";
}

bool SinkUsesFfmpeg(SinkKind kind) {
    return kind == SinkKind::kFfmpeg || kind == SinkKind::kPng;
}

std::string FfmpegCommandLine(const std::string& program, uint32_t width, uint32_t height, bool bgra, double fps,
                              const std::string& args, const std::string& out_file) {
    char rate[32];
    std::snprintf(rate, sizeof(rate), "%.6g", fps);
    std::string cmd = "\"" + program + "\" -y -hide_banner -loglevel warning -f rawvideo -pixel_format ";
    cmd += bgra ? "bgra" : "rgba";
    cmd += " -video_size " + std::to_string(width) + "x" + std::to_string(height);
    cmd += std::string(" -framerate ") + rate + " -i - ";
    if (!args.empty()) cmd += args + " ";
    cmd += "\"" + out_file + "\"";
    return cmd;
}

std::string FfmpegPngCommandLine(const std::string& program, uint32_t width, uint32_t height, bool bgra, double fps,
                                 const std::string& out_pattern) {
    return FfmpegCommandLine(program, width, height, bgra, fps,
                             "-fps_mode passthrough -c:v png -compression_level 3 -pix_fmt rgb24 -start_number 0", out_pattern);
}

}  // namespace mc::cinematic::capture
