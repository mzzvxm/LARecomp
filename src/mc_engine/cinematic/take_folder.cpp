//
// The pure half of the take folder (trailer toolkit M2b). See take_folder.h.
//

#include "take_folder.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <new>

namespace mc::cinematic::capture {

// ---------------------------------------------------------------------------------------------------------------- names

namespace {

bool IsAsciiAlnum(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

std::string Clean(std::string_view in) {
    std::string out;
    bool dash = false;
    for (const char ch : in) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (IsAsciiAlnum(c) || c == '_' || c == '.' || c == '-') {
            out += static_cast<char>(c);
            dash = false;
        } else if (!dash && !out.empty()) {
            out += '-';
            dash = true;
        }
    }
    const auto trim = [&out] {
        while (!out.empty() && (out.back() == '-' || out.back() == '.')) out.pop_back();
        size_t b = 0;
        while (b < out.size() && (out[b] == '-' || out[b] == '.')) ++b;
        out.erase(0, b);
    };
    trim();
    if (out.size() > kMaxTakeNameLength) {
        out.resize(kMaxTakeNameLength);
        trim();
    }
    return out;
}

bool IsDeviceName(const std::string& name) {
    // Windows keeps these (with or without an extension) for devices.
    std::string stem = name.substr(0, name.find('.'));
    for (char& c : stem) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") return true;
    return stem.size() == 4 && (stem.compare(0, 3, "com") == 0 || stem.compare(0, 3, "lpt") == 0) && stem[3] >= '1' && stem[3] <= '9';
}

}  // namespace

std::string SanitizeTakeName(std::string_view text, std::string_view fallback) {
    std::string name = Clean(text);
    if (name.empty()) name = Clean(fallback);
    if (name.empty()) name = "take";
    if (IsDeviceName(name)) name = "_" + name;
    return name;
}

std::string UniqueName(const std::string& base, const std::function<bool(const std::string&)>& taken) {
    if (!taken(base)) return base;
    for (int i = 2; i < 1000; ++i) {
        const std::string candidate = base + "-" + std::to_string(i);
        if (!taken(candidate)) return candidate;
    }
    return std::string();
}

std::string JoinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '/' || a.back() == '\\') return a + b;
    const bool backslash = a.find('\\') != std::string::npos && a.find('/') == std::string::npos;
    return a + (backslash ? '\\' : '/') + b;
}

// ---------------------------------------------------------------------------------------------------------------- the files

TakeLayout MakeLayout(const std::string& folder) {
    TakeLayout l;
    l.folder = folder;
    l.take_json = JoinPath(folder, kTakeJsonName);
    l.frames_csv = JoinPath(folder, kFramesCsvName);
    l.state_csv = JoinPath(folder, kStateCsvName);
    l.scene_copy = JoinPath(folder, kSceneCopyName);
    l.ffmpeg_log = JoinPath(folder, kFfmpegLogName);
    l.video_mkv = JoinPath(folder, kVideoMkvName);
    l.video_raw = JoinPath(folder, kVideoRawName);
    l.audio_wav = JoinPath(folder, kAudioWavName);
    l.audio_csv = JoinPath(folder, kAudioCsvName);
    l.frames_dir = JoinPath(folder, kFramesDirName);
    l.frame_pattern = JoinPath(l.frames_dir, "f%06d.png");
    return l;
}

std::string FrameFileName(uint64_t frame) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "f%06llu.png", static_cast<unsigned long long>(frame));
    return buf;
}

// ---------------------------------------------------------------------------------------------------------------- memory

std::string FormatMib(uint64_t bytes) {
    // tenths of a MiB, rounded half up in integers: the printf of two C libraries disagree on an exact tie (506.25)
    const uint64_t tenths = (bytes * 10 + 524288) / 1048576;
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%llu.%llu MiB", static_cast<unsigned long long>(tenths / 10), static_cast<unsigned long long>(tenths % 10));
    return buf;
}

MemoryPlan PlanMemory(const MemoryRequest& r) {
    MemoryPlan p;
    const uint64_t row_pitch = (static_cast<uint64_t>(r.width) * 4 + 255) / 256 * 256;
    p.gpu_frame_bytes = (r.legacy_frame ? row_pitch * r.height : 0) + r.extra_gpu_frame_bytes;
    p.host_frame_bytes = (r.legacy_frame ? static_cast<uint64_t>(r.width) * r.height * 4 : 0) + r.extra_host_frame_bytes;
    const uint32_t gpu_min = std::max<uint32_t>(1, r.gpu_min);
    const uint32_t ring_min = std::max<uint32_t>(1, r.ring_min);
    uint32_t gpu = std::max(gpu_min, r.gpu_max);
    uint32_t ring = std::max(ring_min, r.ring_wanted);
    const uint32_t gpu_asked = gpu;
    const uint32_t ring_asked = ring;

    // The limit: the cap, and the free memory less what has to stay free. Whichever is smaller; "which" is for the message.
    uint64_t limit = 0;
    const char* limited_by = "";
    if (r.cap_bytes > 0) {
        limit = r.cap_bytes;
        limited_by = "the memory cap";
    }
    if (r.available_bytes > 0) {
        const uint64_t usable = r.available_bytes > r.reserve_bytes ? r.available_bytes - r.reserve_bytes : 0;
        if (limit == 0 || usable < limit) {
            limit = usable;
            limited_by = "the free memory";
        }
        if (limit == 0) limit = 1;  // nothing usable: no plan fits (0 would read as "no limit")
    }
    p.limit_bytes = limit;

    const auto total = [&](uint32_t g, uint32_t h) { return g * p.gpu_frame_bytes + h * p.host_frame_bytes + r.fixed_bytes; };
    if (limit > 0) {
        while (total(gpu, ring) > limit && ring > ring_min) --ring;
        while (total(gpu, ring) > limit && gpu > gpu_min) --gpu;
    }
    p.gpu_buffers = gpu;
    p.ring = ring;
    p.total_bytes = total(gpu, ring);
    if (limit > 0 && p.total_bytes > limit) {
        p.ok = false;
        p.text = "a " + std::to_string(r.width) + "x" + std::to_string(r.height) + " take needs at least " + FormatMib(p.total_bytes) + " (" +
                 std::to_string(gpu) + " GPU readback + " + std::to_string(ring) + " host buffers of " + FormatMib(p.host_frame_bytes) +
                 "), over " + limited_by + " (" + FormatMib(limit) + "): lower the render scale, raise mcla_capture_memory_mb, or free memory";
        return p;
    }
    p.ok = true;
    if (gpu != gpu_asked || ring != ring_asked) {
        p.text = "buffers cut to fit " + std::string(limited_by) + " (" + FormatMib(limit) + "): " + std::to_string(gpu) + " GPU readback (asked " +
                 std::to_string(gpu_asked) + "), ring " + std::to_string(ring) + " (asked " + std::to_string(ring_asked) + ")";
    }
    return p;
}

bool MemoryFloorHit(uint64_t available_bytes, uint64_t floor_bytes) {
    return floor_bytes > 0 && available_bytes > 0 && available_bytes < floor_bytes;
}

void SlotPool::Reset(uint32_t capacity) {
    in_use_.assign(capacity, 0);
    in_use_count_ = 0;
    max_in_use_ = 0;
}

bool SlotPool::TryAcquire(uint32_t* slot) {
    for (uint32_t i = 0; i < in_use_.size(); ++i) {
        if (in_use_[i] == 0) {
            in_use_[i] = 1;
            ++in_use_count_;
            max_in_use_ = std::max(max_in_use_, in_use_count_);
            *slot = i;
            return true;
        }
    }
    return false;
}

bool SlotPool::Release(uint32_t slot) {
    if (slot >= in_use_.size() || in_use_[slot] == 0) return false;
    in_use_[slot] = 0;
    --in_use_count_;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------- the host ring

bool HostRing::Open(uint32_t slots, uint64_t frame_bytes, WriteFn write) {
    Close();
    if (slots == 0 || frame_bytes == 0 || !write) return false;
    std::vector<std::unique_ptr<uint8_t[]>> buffers;
    buffers.reserve(slots);
    for (uint32_t i = 0; i < slots; ++i) {
        std::unique_ptr<uint8_t[]> b(new (std::nothrow) uint8_t[static_cast<size_t>(frame_bytes)]);
        if (!b) return false;  // what was made goes with `buffers`
        buffers.push_back(std::move(b));
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        buffers_ = std::move(buffers);
        pool_.Reset(slots);
        queue_.clear();
        slots_ = slots;
        frame_bytes_ = frame_bytes;
        write_ = std::move(write);
        stop_ = false;
        open_ = true;
        max_queued_ = 0;
    }
    written_ = 0;
    failures_ = 0;
    first_failed_row_ = 0;
    progress_ = 0;
    writer_ = std::thread([this] { WriterMain(); });
    return true;
}

bool HostRing::is_open() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return open_ && !stop_;
}

uint32_t HostRing::queued() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<uint32_t>(queue_.size());
}

uint32_t HostRing::max_in_use() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pool_.max_in_use();
}

uint32_t HostRing::max_queued() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return max_queued_;
}

HostRing::Slot HostRing::Acquire(int timeout_ms, double* waited_ms, bool* stuck) {
    using Clock = std::chrono::steady_clock;
    Slot out;
    if (stuck) *stuck = false;
    const auto t0 = Clock::now();
    auto deadline = t0 + std::chrono::milliseconds(timeout_ms);
    uint64_t seen = progress_.load(std::memory_order_relaxed);
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        if (!open_ || stop_) break;
        uint32_t index = 0;
        if (pool_.TryAcquire(&index)) {
            out.data = buffers_[index].get();
            out.index = index;
            out.bytes = frame_bytes_;
            break;
        }
        const auto now = Clock::now();
        const uint64_t p = progress_.load(std::memory_order_relaxed);
        if (p != seen) {  // a write finished since: the writer is working, however slowly
            seen = p;
            deadline = now + std::chrono::milliseconds(timeout_ms);
        }
        if (now >= deadline) {
            if (stuck) *stuck = true;
            break;
        }
        free_.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(100)));
    }
    lock.unlock();
    if (waited_ms) *waited_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    return out;
}

void HostRing::Submit(const Slot& slot, uint64_t row) {
    if (!slot.data) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(Item{slot.index, row});
        max_queued_ = std::max<uint32_t>(max_queued_, static_cast<uint32_t>(queue_.size()));
    }
    work_.notify_one();
}

void HostRing::Cancel(const Slot& slot) {
    if (!slot.data) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pool_.Release(slot.index);
    }
    progress_.fetch_add(1, std::memory_order_relaxed);
    free_.notify_all();
}

void HostRing::WriterMain() {
    for (;;) {
        Item item{};
        const uint8_t* data = nullptr;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            work_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            if (queue_.empty()) return;  // stopped, and everything handed over is written
            item = queue_.front();
            queue_.pop_front();
            data = buffers_[item.slot].get();
        }
        const bool ok = write_(item.row, data, frame_bytes_);
        if (!ok && failures_.fetch_add(1, std::memory_order_relaxed) == 0) first_failed_row_.store(item.row, std::memory_order_relaxed);
        written_.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pool_.Release(item.slot);
        }
        progress_.fetch_add(1, std::memory_order_relaxed);
        free_.notify_all();
    }
}

void HostRing::Close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_) return;
        stop_ = true;
    }
    work_.notify_all();
    free_.notify_all();
    if (writer_.joinable()) writer_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    buffers_.clear();
    queue_.clear();
    write_ = nullptr;
    open_ = false;
}

// ---------------------------------------------------------------------------------------------------------------- JSON

std::string JsonEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c >= 0x80) {
            // UTF-8 passes through when it is well formed; a byte that is not (a path in the ANSI code page) becomes U+FFFD, so that the file is
            // always valid JSON and valid UTF-8 for whoever reads it.
            const size_t n = (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
            bool ok = n != 0 && i + n <= text.size();
            for (size_t k = 1; ok && k < n; ++k) ok = (static_cast<unsigned char>(text[i + k]) & 0xC0) == 0x80;
            if (ok && n == 3) {
                const uint32_t cp = (uint32_t(c & 0x0F) << 12) | (uint32_t(static_cast<unsigned char>(text[i + 1]) & 0x3F) << 6) |
                                    uint32_t(static_cast<unsigned char>(text[i + 2]) & 0x3F);
                ok = cp >= 0x800 && !(cp >= 0xD800 && cp <= 0xDFFF);
            } else if (ok && n == 4) {
                const uint32_t cp = (uint32_t(c & 0x07) << 18) | (uint32_t(static_cast<unsigned char>(text[i + 1]) & 0x3F) << 12) |
                                    (uint32_t(static_cast<unsigned char>(text[i + 2]) & 0x3F) << 6) | uint32_t(static_cast<unsigned char>(text[i + 3]) & 0x3F);
                ok = cp >= 0x10000 && cp <= 0x10FFFF;
            }
            if (!ok) {
                out += "\\ufffd";
                ++i;
                continue;
            }
            out.append(text.substr(i, n));
            i += n;
            continue;
        }
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
        ++i;
    }
    return out;
}

void JsonWriter::Newline() {
    out_ += '\n';
    out_.append(first_.size() * 2, ' ');
}

void JsonWriter::Before() {
    if (after_key_) {
        after_key_ = false;
        return;
    }
    if (!first_.empty()) {
        if (!first_.back()) out_ += ',';
        first_.back() = false;
        Newline();
    }
}

void JsonWriter::BeginObject() {
    Before();
    out_ += '{';
    first_.push_back(true);
}

void JsonWriter::EndObject() {
    const bool empty = first_.back();
    first_.pop_back();
    if (!empty) Newline();
    out_ += '}';
}

void JsonWriter::BeginArray() {
    Before();
    out_ += '[';
    first_.push_back(true);
}

void JsonWriter::EndArray() {
    const bool empty = first_.back();
    first_.pop_back();
    if (!empty) Newline();
    out_ += ']';
}

void JsonWriter::Key(std::string_view name) {
    Before();
    out_ += '"';
    out_ += JsonEscape(name);
    out_ += "\": ";
    after_key_ = true;
}

void JsonWriter::String(std::string_view value) {
    Before();
    out_ += '"';
    out_ += JsonEscape(value);
    out_ += '"';
}

void JsonWriter::Int(int64_t value) {
    Before();
    out_ += std::to_string(value);
}

void JsonWriter::UInt(uint64_t value) {
    Before();
    out_ += std::to_string(value);
}

void JsonWriter::Double(double value) {
    Before();
    if (!std::isfinite(value)) {
        out_ += "null";
        return;
    }
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.17g", value);
    out_ += buf;
}

void JsonWriter::Bool(bool value) {
    Before();
    out_ += value ? "true" : "false";
}

void JsonWriter::Null() {
    Before();
    out_ += "null";
}

std::string JsonWriter::Take() {
    out_ += '\n';
    return std::move(out_);
}

// ---------------------------------------------------------------------------------------------------------------- take.json

const char* FinalState(bool exact, bool failed) {
    if (failed) return "failed";
    return exact ? "complete" : "inexact";
}

std::string BuildTakeJson(const TakeReport& r) {
    JsonWriter j;
    j.BeginObject();
    j.Field("take_json_version", kTakeJsonVersion);
    j.Field("name", r.name);
    j.Field("state", r.state);
    j.Field("error", r.error);
    j.Field("started_utc", r.started_utc);
    j.Field("finished_utc", r.finished_utc);
    j.Field("wall_s", r.wall_s);

    j.Key("scene");
    if (r.has_scene) {
        j.BeginObject();
        j.Field("name", r.scene_name);
        j.Field("source", r.scene_source);
        j.Field("copy", r.scene_copy);
        j.Field("fnv64", r.scene_fnv64);
        j.Field("first_frame_scene_t", r.first_scene_t);
        j.Field("step_s", r.scene_step_s);
        j.EndObject();
    } else {
        j.Null();  // a take from the debug setting, no scene
    }

    j.Key("exe");
    j.BeginObject();
    j.Field("path", r.exe_path);
    j.Field("commit", r.exe_commit);
    j.Field("version", r.version);
    j.EndObject();

    j.Key("picture");
    j.BeginObject();
    j.Field("width", r.width);
    j.Field("height", r.height);
    j.Field("readback_format", r.pixel_format);
    j.Field("fps", r.fps);
    j.Field("step_ticks", r.step_ticks);
    j.Field("step_ms", r.step_ms);
    j.EndObject();

    j.Key("sink");
    j.BeginObject();
    j.Field("kind", r.sink);
    j.Field("output", r.output);
    j.Field("output_bytes", r.output_bytes);
    j.Field("output_files", r.output_files);
    j.Field("ffmpeg_exit", r.ffmpeg_exit);
    j.Field("ffmpeg_command", r.ffmpeg_command);
    j.EndObject();

    j.Key("frames");
    j.BeginObject();
    j.Field("count", r.frames);
    j.Field("first_update", r.first_update);
    j.Field("last_update", r.last_update);
    j.Field("updates_run", r.updates_run);
    j.Field("handed_over", r.handed);
    j.Field("lost", r.lost);
    j.Field("dropped", r.dropped);
    j.Field("repeated", r.repeated);
    j.Field("out_of_order", r.out_of_order);
    j.Field("skipped_updates", r.skipped);
    j.Field("exact", r.exact);
    j.Field("verdict", r.verdict);
    j.Field("map", "frame k shows update first_update + k when exact; one row for every frame, with its update and scene time, in the frames file");
    j.Field("frames_file", std::string_view(r.frames == 0 && r.state == "running" ? "" : kFramesCsvName));
    j.EndObject();

    j.Key("gate");
    j.BeginObject();
    j.Field("lag", r.lag);
    j.Field("timeouts", r.gate_timeouts);
    j.Field("switched_off", r.gate_off);
    j.EndObject();

    j.Key("memory");
    j.BeginObject();
    j.Field("ring_buffers", r.ring);
    j.Field("gpu_readback_buffers", r.gpu_buffers);
    j.Field("plan_bytes", r.memory_plan_bytes);
    j.Field("cap_bytes", r.memory_cap_bytes);
    j.Field("note", r.memory_note);
    j.Field("ring_max_fill", r.ring_max_fill);
    j.Field("ring_stalls", r.ring_stalls);
    j.Field("ram_free_mb_start", r.ram_free_start_mb);
    j.Field("ram_free_mb_min", r.ram_free_min_mb);
    j.Field("ram_free_mb_end", r.ram_free_end_mb);
    j.EndObject();

    j.Key("state_log");
    if (r.state_log.empty()) {
        j.Null();
    } else {
        j.BeginObject();
        j.Field("file", r.state_log);
        j.Field("rows", r.state_rows);
        j.EndObject();
    }

    j.Key("cvars");
    j.BeginObject();
    for (const auto& kv : r.cvars) j.Field(kv.first, std::string_view(kv.second));
    j.EndObject();

    j.EndObject();
    return j.Take();
}

// ---------------------------------------------------------------------------------------------------------------- the state log

std::string StateCsvHeader() {
    return "frame,update,scene_t,hour,time_scale,dt_ms,dt_unscaled_ms,paused,world_ready,player,x,y,z,matrix_fnv64\n";
}

std::string StateCsvRow(const StateRow& r) {
    char buf[320];
    std::snprintf(buf, sizeof(buf), "%llu,%llu,%.6f,%.5f,%.5f,%.5f,%.5f,%d,%d,%08X,%.6f,%.6f,%.6f,%s\n", static_cast<unsigned long long>(r.frame),
                  static_cast<unsigned long long>(r.update), r.scene_t, static_cast<double>(r.hour), static_cast<double>(r.time_scale),
                  static_cast<double>(r.dt_ms), static_cast<double>(r.dt_unscaled_ms), r.paused ? 1 : 0, r.world_ready ? 1 : 0, r.player,
                  static_cast<double>(r.x), static_cast<double>(r.y), static_cast<double>(r.z), Hex64(r.matrix_hash).c_str());
    return buf;
}

uint64_t Fnv1a64(const void* data, size_t bytes, uint64_t seed) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < bytes; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

std::string Hex64(uint64_t value) {
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(value));
    return buf;
}

// ---------------------------------------------------------------------------------------------------------------- small things

bool IsSecretCvarName(std::string_view name) {
    std::string lower;
    for (const char c : name) lower += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    for (const char* word : {"password", "passwd", "token", "secret", "apikey", "api_key", "credential", "private_key"}) {
        if (lower.find(word) != std::string::npos) return true;
    }
    return false;
}

std::string FormatUtc(int64_t t) {
    // Days since 1970-01-01 to a civil date (Howard Hinnant's algorithm), no C library clock and no time zone.
    int64_t days = t / 86400;
    int64_t secs = t % 86400;
    if (secs < 0) {
        secs += 86400;
        --days;
    }
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const int64_t doe = days - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const int64_t m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) ++y;
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04lld-%02lld-%02lldT%02lld:%02lld:%02lldZ", static_cast<long long>(y), static_cast<long long>(m),
                  static_cast<long long>(d), static_cast<long long>(secs / 3600), static_cast<long long>(secs % 3600 / 60),
                  static_cast<long long>(secs % 60));
    return buf;
}

std::string ParseSetSourceSha(std::string_view text) {
    size_t pos = 0;
    while ((pos = text.find("sha=", pos)) != std::string_view::npos) {
        if (pos == 0 || text[pos - 1] == ' ' || text[pos - 1] == '\n' || text[pos - 1] == '\t') {
            size_t end = pos + 4;
            std::string sha;
            while (end < text.size()) {
                const char c = text[end];
                const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
                if (!hex) break;
                sha += static_cast<char>(c >= 'A' && c <= 'F' ? c - 'A' + 'a' : c);
                ++end;
            }
            if (sha.size() >= 7) return sha;
        }
        pos += 4;
    }
    return std::string();
}

}  // namespace mc::cinematic::capture
