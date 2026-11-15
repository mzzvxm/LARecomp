// The per-update state log, pure half (state_log_core.h). No game, no SDK, no log: state_log_test.cpp builds it alone.

#include "state_log_core.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <utility>

namespace mc::cinematic::statelog {

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// FNV-1a, 64 bits, over the bits of what is logged (never over a wall clock, a label or the generator)
// ---------------------------------------------------------------------------------------------------------------------
struct Fnv {
    uint64_t h = 1469598103934665603ull;
    void Bytes(const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    }
    void U64(uint64_t v) { Bytes(&v, sizeof v); }
    void F(float f) {
        uint32_t b;
        std::memcpy(&b, &f, 4);
        U64(b);
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// The row: one function names and fills every column. In header mode it collects the names.
// ---------------------------------------------------------------------------------------------------------------------
struct Out {
    bool header = false;
    std::string text;
    std::vector<std::string> names;
    Fnv* group = nullptr;

    void Sep() {
        if (!text.empty()) text += ',';
    }
    bool Name(const char* n) {
        if (!header) return false;
        names.emplace_back(n);
        return true;
    }
    void Raw(const std::string& v) {
        Sep();
        text += v;
    }
    void U(const char* name, uint64_t v) {
        if (Name(name)) return;
        char b[32];
        std::snprintf(b, sizeof b, "%llu", static_cast<unsigned long long>(v));
        Raw(b);
        if (group) group->U64(v);
    }
    void I(const char* name, int64_t v) {
        if (Name(name)) return;
        char b[32];
        std::snprintf(b, sizeof b, "%lld", static_cast<long long>(v));
        Raw(b);
        if (group) group->U64(static_cast<uint64_t>(v));
    }
    void F(const char* name, float v) {
        if (Name(name)) return;
        char b[40];
        std::snprintf(b, sizeof b, "%.9g", static_cast<double>(v));  // 9 digits: a float comes back exactly
        Raw(b);
        if (group) group->F(v);
    }
    void D(const char* name, double v, const char* spec) {  // a double that is a clock or a time: printed, never hashed
        if (Name(name)) return;
        char b[48];
        std::snprintf(b, sizeof b, spec, v);
        Raw(b);
    }
    void Hex(const char* name, uint64_t v) {
        if (Name(name)) return;
        char b[24];
        std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
        Raw(b);
    }
    void Str(const char* name, const std::string& v) {
        if (Name(name)) return;
        Raw(CleanField(v, 64));
    }
    // The twelve words of a matrix (the fourth word of each row is padding and is not logged).
    void Pose(const char* prefix, const float m[kMatrixWords]) {
        char nm[48];
        for (int k = 0; k < kPoseWords; ++k) {
            const int i = PoseWordIndex(k);
            std::snprintf(nm, sizeof nm, "%s%d", prefix, i);
            F(nm, m[i]);
        }
    }
};

struct Hashes {
    Fnv player, cars, world, cam;
};

void Format(Out& o, const Sample& s, Hashes& h, uint64_t* master_out) {
    char nm[48];
    o.U("tick", s.tick);
    o.Str("mark", s.mark);
    o.D("wall_ms", s.wall_ms, "%.3f");
    o.U("lost", s.lost);

    o.group = nullptr;
    o.I("scene_state", s.scene_state);
    o.D("scene_t", s.scene_t, "%.6f");

    o.F("dt", s.dt);
    o.F("dt_unscaled", s.dt_unscaled);
    o.F("time_scale", s.time_scale);
    o.U("frame_ctr", s.frame_ctr);
    o.F("clock_total", s.clock_total);
    o.F("clock_game", s.clock_game);
    o.F("mode_timer", s.mode_timer);

    o.U("rng", s.rng);  // not hashed: the generator is the first thing that differs between two runs (clock-seeded)
    o.group = &h.world;
    o.F("hour", s.hour);
    o.I("weather", s.weather);
    o.U("paused", s.paused);
    o.U("live", s.live);

    o.group = &h.player;
    o.U("p_ok", s.p_ok ? 1 : 0);
    o.Pose("p_m", s.p_m);
    o.F("p_speed", s.p_speed);

    o.group = &h.cam;
    o.U("cam_ok", s.cam_ok ? 1 : 0);
    o.Pose("cam_m", s.cam);

    o.group = &h.cars;
    o.I("n_cars", s.n_cars);
    for (int c = 0; c < kMaxCars; ++c) {
        const CarSample& car = s.car[c];
        std::snprintf(nm, sizeof nm, "c%d_id", c);
        if (!o.Name(nm)) {
            o.Raw(CleanField(car.id, kIdChars));
            h.cars.Bytes(car.id, std::strlen(car.id));
        }
        std::snprintf(nm, sizeof nm, "c%d_ok", c);
        o.U(nm, car.ok ? 1 : 0);
        std::snprintf(nm, sizeof nm, "c%d_m", c);
        o.Pose(nm, car.m);
    }
    o.group = nullptr;

    if (o.header) {
        o.Name("h_player");
        o.Name("h_cars");
        o.Name("h_world");
        o.Name("h_cam");
        o.Name("hash");
        o.Name("chain");
        return;
    }
    // `hash` is the state of the cars, the world and the camera: it leaves the clocks and the generator out.
    Fnv master;
    master.U64(h.player.h);
    master.U64(h.cars.h);
    master.U64(h.world.h);
    master.U64(h.cam.h);
    *master_out = master.h;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// the public format
// ---------------------------------------------------------------------------------------------------------------------

std::vector<std::string> ColumnNames() {
    Out o;
    o.header = true;
    Hashes h;
    uint64_t master = 0;
    Format(o, Sample{}, h, &master);
    return o.names;
}

std::string ColumnLine() {
    std::string line;
    for (const std::string& n : ColumnNames()) {
        if (!line.empty()) line += ',';
        line += n;
    }
    return line;
}

std::string RowText(const Sample& sample, Chain* chain) {
    Out o;
    Hashes h;
    uint64_t master = 0;
    Format(o, sample, h, &master);
    Fnv next;
    next.U64(chain ? chain->value : 0);
    next.U64(master);
    if (chain) chain->value = next.h;
    o.Hex("h_player", h.player.h);
    o.Hex("h_cars", h.cars.h);
    o.Hex("h_world", h.world.h);
    o.Hex("h_cam", h.cam.h);
    o.Hex("hash", master);
    o.Hex("chain", next.h);
    o.text += '\n';
    return std::move(o.text);
}

std::string PreambleLine(const std::string& scene_name, double fixed_step_fps, const std::string& started) {
    constexpr double kTickSeconds = 2.0050125115744777e-8;  // the game's own 49.875 MHz timer tick
    const long long ticks = fixed_step_fps > 0.0 ? std::llround((1.0 / fixed_step_fps) / kTickSeconds) : 0;
    char b[256];
    std::snprintf(b, sizeof b, "#statelog %s scene=%s started=%s fixed_step_fps=%g step_ticks=%lld tick_seconds=%.10g\n", kFormatVersion,
                  CleanField(scene_name.empty() ? "-" : scene_name, 48).c_str(), CleanField(started.empty() ? "-" : started, 24).c_str(),
                  fixed_step_fps, ticks, kTickSeconds);
    return b;
}

std::string TrailerLine(uint64_t ticks, uint64_t written, uint64_t dropped) {
    char b[160];
    std::snprintf(b, sizeof b, "#end rows=%llu written=%llu dropped=%llu\n", static_cast<unsigned long long>(ticks),
                  static_cast<unsigned long long>(written), static_cast<unsigned long long>(dropped));
    return b;
}

std::string CleanField(const std::string& text, size_t max_chars) {
    std::string out;
    for (char c : text) {
        if (out.size() >= max_chars) break;
        const unsigned char u = static_cast<unsigned char>(c);
        out += (c == ',' || c == '"' || u < 0x20 || u == 0x7F) ? ';' : c;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// where the file goes
// ---------------------------------------------------------------------------------------------------------------------

namespace {

std::string TrimTrailingSlashes(std::string s) {
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    return s;
}

std::string Join(const std::string& dir, const std::string& leaf) {
    const std::string d = TrimTrailingSlashes(dir);
    if (d.empty()) return leaf;
    const char last = d.back();
    return (last == '/' || last == '\\' || last == ':') ? d + leaf : d + "/" + leaf;
}

}  // namespace

std::string CsvPathFor(const std::string& path) {
    if (path.size() >= 4) {
        std::string tail = path.substr(path.size() - 4);
        std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (tail == ".csv") return path;
    }
    return Join(path, "state.csv");
}

std::string SafeFileStem(const std::string& name) {
    std::string out;
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        out += (std::isalnum(u) || c == '_' || c == '-' || c == '.') ? c : '_';
    }
    // dots at the ends would make hidden or odd names
    while (!out.empty() && out.front() == '.') out.erase(out.begin());
    while (!out.empty() && out.back() == '.') out.pop_back();
    return out.empty() ? "scene" : out;
}

std::string ResolveLogPath(const std::string& cvar_value, const std::string& scene_dir, const std::string& take_dir,
                           const std::string& scene_name, const std::string& stamp) {
    if (!cvar_value.empty()) return CsvPathFor(cvar_value);
    if (!scene_dir.empty()) return Join(scene_dir, "state.csv");
    if (!take_dir.empty()) return Join(take_dir, "state.csv");
    return Join("statelogs", SafeFileStem(scene_name) + "-" + SafeFileStem(stamp.empty() ? "now" : stamp) + ".csv");
}

std::string FirstFreePath(const std::string& path, const std::function<bool(const std::string&)>& exists) {
    if (!exists(path)) return path;
    // split at the last dot of the last path component
    const size_t slash = path.find_last_of("/\\");
    const size_t dot = path.find_last_of('.');
    const bool has_ext = dot != std::string::npos && (slash == std::string::npos || dot > slash);
    const std::string stem = has_ext ? path.substr(0, dot) : path;
    const std::string ext = has_ext ? path.substr(dot) : std::string();
    for (int n = 2; n <= 999; ++n) {
        std::string candidate = stem + "-" + std::to_string(n) + ext;
        if (!exists(candidate)) return candidate;
    }
    return path;  // a thousand takes in one folder: the last one is overwritten
}

// ---------------------------------------------------------------------------------------------------------------------
// queue and writer
// ---------------------------------------------------------------------------------------------------------------------

bool RowQueue::Push(std::string row) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_.size() + row.size() > max_bytes_) {
        ++dropped_;
        return false;
    }
    pending_ += row;
    ++pushed_;
    return true;
}

void RowQueue::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.clear();
    pushed_ = 0;
    dropped_ = 0;
}

size_t RowQueue::PopAll(std::string* out) {
    std::string local;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        local.swap(pending_);
    }
    const size_t n = local.size();
    if (out) *out += local;
    return n;
}

size_t RowQueue::pending_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
}

uint64_t RowQueue::pushed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pushed_;
}

uint64_t RowQueue::dropped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
}

namespace {

std::filesystem::path PathFromUtf8(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

}  // namespace

LogWriter::LogWriter(size_t max_pending_bytes, int flush_ms) : queue_(max_pending_bytes), flush_ms_(flush_ms < 1 ? 1 : flush_ms) {}

LogWriter::~LogWriter() {
    if (IsOpen()) Close(written() + dropped());
}

bool LogWriter::Open(const std::string& path, const std::string& head) {
    if (IsOpen()) return false;
    const std::filesystem::path p = PathFromUtf8(path);
    std::error_code ec;
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    file_.clear();
    file_.open(p, std::ios::binary | std::ios::trunc);
    if (!file_) return false;
    queue_.Reset();
    file_.write(head.data(), static_cast<std::streamsize>(head.size()));
    file_.flush();
    path_ = path;
    stop_ = false;
    open_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { Run(); });
    return true;
}

bool LogWriter::Push(std::string row) {
    if (!IsOpen()) return false;
    return queue_.Push(std::move(row));
}

void LogWriter::Flush() {
    // The file's lock first: two flushes (the thread's and Close's) must write in the order they took the rows.
    std::lock_guard<std::mutex> file_lock(file_mutex_);
    std::string local;
    queue_.PopAll(&local);
    if (local.empty() || !file_) return;
    file_.write(local.data(), static_cast<std::streamsize>(local.size()));
    file_.flush();
}

void LogWriter::Run() {
    std::unique_lock<std::mutex> lock(wake_mutex_);
    while (!stop_) {
        wake_.wait_for(lock, std::chrono::milliseconds(flush_ms_), [this] { return stop_; });
        if (stop_) break;
        lock.unlock();
        Flush();
        lock.lock();
    }
}

void LogWriter::Close(uint64_t ticks) {
    if (!open_.exchange(false, std::memory_order_acq_rel)) return;
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    Flush();
    {
        std::lock_guard<std::mutex> file_lock(file_mutex_);
        const std::string trailer = TrailerLine(ticks, queue_.pushed(), queue_.dropped());
        file_.write(trailer.data(), static_cast<std::streamsize>(trailer.size()));
        file_.close();
    }
}

}  // namespace mc::cinematic::statelog
