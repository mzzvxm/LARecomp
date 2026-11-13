#ifndef REXGLUE_HAS_XEO3_TARGET
//
// Capture mode of the trailer toolkit (work package M2a; feature-parity/CINEMATIC-PLAN.md, CINEMATIC-DESIGN.md sections 1.4, 1.5 and 2.4).
// The maths and the books are in capture_core.h (with its test); the render thread's half is gpu/capture_readback.cpp. What is here:
// the settings, the fixed step, the frame gate, the take (a sink of host buffers with a writer thread, ffmpeg or a file), the scene
// clock and the capture applier of the scene runner.
//
// Settings (all debug-only and live; nothing changes while none of them and no scene asks):
//   mcla_capture_fps <n>           a take at n frames a second, from the next update (the debug row; a scene's `capture fps` does the
//                                  same). 0 ends it. The game steps by exactly 1/n, whatever a frame takes to render.
//   mcla_fixed_step_fps <n>        the fixed step alone, with no take and no gate (a repeatability test, the DEBUG row). 0: off.
//   mcla_capture_out <prefix>      the take's files: <prefix>.mkv (ffmpeg) or <prefix>.bgra (raw), .cap.csv (one row per frame: the
//                                  costs), .info.txt, .ffmpeg.log. A relative prefix is relative to the game's working folder.
//   mcla_capture_sink none|ffmpeg|raw   what happens to a frame once it is in a host buffer: nothing (the readback and its copies are
//                                  measured alone), a pipe to ffmpeg (FFV1 in mkv by default) or a raw file of frames (rows packed,
//                                  B8G8R8A8 as the swap chain has them). Read when a take starts.
//   mcla_capture_ffmpeg <path>, mcla_capture_ffmpeg_args <text>   the program and its output options.
//   mcla_capture_ring <n>          host buffers between the readback and the writer (2 to 8, 4 default).
//   mcla_capture_lag <n>           0 (default): lock-step, the game does not start update k + 1 before frame k is in the ring; n: it
//                                  may run n frames ahead of the readback.
//   mcla_capture_gate_timeout_ms   how long the game waits at the gate before it goes on and counts a timeout (20000).
//   mcla_capture_virtual_clock     on (default): during a take the upscaler's history rule and the UI movies' pacing read frames x
//                                  step instead of the wall clock. Off: a slow capture makes the upscaler start its history again on
//                                  every frame (more than 100 ms between frames) and the movies step at the wrong rate.
//   mcla_capture_delay_ms <ms>     the readback of every frame sleeps this long: a slow writer, for a run that has to show the clock's
//                                  effect at a render scale where the frames are quick.
//   mcla_capture_log_every <n>     a [cap] line every n frames (30; 0: only the summary).
//
// THE GATE. See capture_core.h for the frame's two threads. Three things keep the picture and the update in step:
//   updates   CaptureGate counts the update at the engine timer's first instruction (after its wait), on the game thread.
//   stamps    the hand-over of a frame to the render thread (hooks/frame_pacing.cpp rex_sub_82305B38: the render thread is idle, the
//             next hand-over comes after its "render done") calls CaptureOnHandOver, which stamps the frame with the update that just
//             ran and whether it is the take's. The guest's swap (CaptureOnSwap) hands the stamp to the present record. The first
//             capture tagged a frame at its swap with the update count of that moment: the swap of the frame before a take could come
//             after the take's first gate, so a take began with a repeated picture and its last picture was cut off (E7, 240 updates
//             gave 241 rows, the first repeated and the 240th lost). A stamp cannot be late.
//   gate      before every update of a take the game waits until every frame handed over has been read back (or reported lost);
//             before the update after the take's end it waits for the last one, so the take ends with its last frame in the sink.
// What can go wrong is counted, not hidden: a frame the renderer cannot read back is reported lost and released at the gate; a wait that
// runs out is counted, the books are set straight and the game goes on; three in a row switch the waiting off for the rest of the take
// (so a deadlock between the guest's render thread and the gate cannot freeze the run); an update with no frame, or a hand-over with no
// update, makes the take's rows and updates disagree, and the verdict at the end says so.
//
// Written without a build or a run. CHECK marks what rests on reading the code and the E7 log only.
//

#include "capture_mode.h"

#include <rex/cvar.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

#include "capture_core.h"
#include "larecomp_log.h"
#include "scene_runtime.h"

#if defined(LARECOMP_HAS_GPU)
#include "gpu/gpu.h"
#endif

REXCVAR_DEFINE_DOUBLE(mcla_capture_fps, 0.0, "MCLA/Cinematic",
    "TRAILER TOOLKIT capture mode: a take at this many frames a second, from the next update: the game steps by exactly 1 / this "
    "whatever a frame takes to render, every frame is read back and the game waits for it before the next update. 0 ends the take. "
    "10 to 1000. A scene's capture key does the same.")
    .range(0.0, 1000.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();
REXCVAR_DEFINE_DOUBLE(mcla_fixed_step_fps, 0.0, "MCLA/Cinematic",
    "TRAILER TOOLKIT: the game steps by exactly 1 / this many frames a second, whatever the frame takes (the engine timer is given "
    "the game's own tick count for it); the FPS limit, the step cap and frame pacing stand aside. No take, no frame gate. 0: off. "
    "10 to 1000.")
    .range(0.0, 1000.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();
REXCVAR_DEFINE_STRING(mcla_capture_out, "captures/take", "MCLA/Cinematic",
    "TRAILER TOOLKIT capture mode: the take's file prefix: <prefix>.mkv or .bgra, .cap.csv, .info.txt, .ffmpeg.log. A relative prefix "
    "is relative to the game's folder. Read when a take starts.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();
REXCVAR_DEFINE_STRING(mcla_capture_sink, "none", "MCLA/Cinematic",
    "TRAILER TOOLKIT capture mode: what happens to a frame once it is read back: none (counted and dropped), ffmpeg (a pipe to the "
    "program in mcla_capture_ffmpeg) or raw (a file of B8G8R8A8 frames). Read when a take starts.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();
REXCVAR_DEFINE_STRING(mcla_capture_ffmpeg, "ffmpeg", "MCLA/Cinematic",
    "TRAILER TOOLKIT capture mode (sink ffmpeg): the ffmpeg program, a full path or a name on PATH.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();
REXCVAR_DEFINE_STRING(mcla_capture_ffmpeg_args, "-c:v ffv1 -level 3 -g 1 -slices 24 -pix_fmt bgr0", "MCLA/Cinematic",
    "TRAILER TOOLKIT capture mode (sink ffmpeg): ffmpeg's output options, between the raw input and <prefix>.mkv.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();
REXCVAR_DEFINE_INT32(mcla_capture_ring, 4, "MCLA/Cinematic",
    "TRAILER TOOLKIT capture mode: host buffers between the readback and the writer (2 to 8). Read when a take starts.")
    .range(2, 8)
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();
REXCVAR_DEFINE_INT32(mcla_capture_lag, 0, "MCLA/Cinematic",
    "TRAILER TOOLKIT capture mode: how many frames the game may run ahead of the readback. 0: it waits for frame k to be read back "
    "before it starts update k + 1.")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();
REXCVAR_DEFINE_INT32(mcla_capture_gate_timeout_ms, 20000, "MCLA/Cinematic",
    "TRAILER TOOLKIT capture mode: how long the game waits at the frame gate before it goes on (counted in the summary; three in a "
    "row switch the waiting off for the take).")
    .range(100, 600000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();
REXCVAR_DEFINE_BOOL(mcla_capture_virtual_clock, true, "MCLA/Cinematic",
    "TRAILER TOOLKIT capture mode: during a take the upscaler's history rule and the UI movies' pacing read frames x step instead of "
    "the wall clock. Off, a slow capture makes the upscaler start its history again on every frame.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();
REXCVAR_DEFINE_DOUBLE(mcla_capture_delay_ms, 0.0, "MCLA/Cinematic",
    "TRAILER TOOLKIT capture mode: the readback of each frame sleeps this long (a slow writer, in milliseconds).")
    .range(0.0, 5000.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();
REXCVAR_DEFINE_INT32(mcla_capture_log_every, 30, "MCLA/Cinematic",
    "TRAILER TOOLKIT capture mode: a [cap] line with the costs of a frame every this many frames (0: the summary only).")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();

namespace mc::cinematic {

namespace {

namespace core = ::mc::cinematic::capture;
using Clock = std::chrono::steady_clock;

double SteadyMs() {
    return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------------------------------------------------
// the counters
// ---------------------------------------------------------------------------------------------------------------------
std::atomic<uint64_t> g_updates{0};         // updates that passed the gate (the game thread writes it)
std::atomic<bool> g_take_active{false};
std::atomic<uint64_t> g_take_first{0};      // the take's first update
std::atomic<uint64_t> g_take_ticks{0};      // the take's step in the game's ticks
std::atomic<double> g_scene_fps{0.0};       // the scene's request (the capture applier)
std::atomic<uint64_t> g_stamp{0};           // the hand-over's stamp for the swap that follows (0: none)
core::HandOverBook g_handover;              // game thread only
std::atomic<uint64_t> g_anchors{0}, g_anchor_restarts{0}, g_anchor_gap_restarts{0};  // the upscaler's (SceneClockNoteAnchor)

std::mutex g_gate_mutex;
std::condition_variable g_gate_cv;
core::GateBook g_gate;  // g_gate_mutex

// the scene clock
std::mutex g_clock_mutex;
core::VirtualClock g_clock;        // g_clock_mutex
std::atomic<bool> g_clock_on{false};
std::atomic<uint32_t> g_clock_epoch{0};

// ---------------------------------------------------------------------------------------------------------------------
// a take
// ---------------------------------------------------------------------------------------------------------------------
struct Row {
    uint64_t update = 0;
    mc::cinematic::FrameCosts c;
    double t_ms = 0;       // since the take started
    double write_ms = -1;  // -1: not written (yet); -2: the write failed
    uint32_t ring_fill = 0;
    bool lost = false;
};

struct Take {
    std::string out;
    core::SinkKind sink = core::SinkKind::kNone;
    double fps = 60.0;
    uint64_t ticks = 0;
    double step_ms = 16.666;
    uint64_t first_update = 0;
    double first_scene_t = 0.0;  // the scene time of the take's first update (the scene's time then, plus a step), when a scene runs
    bool scene_running = false;
    Clock::time_point t0;
    bool virtual_clock = false;
    int lag = 0;
    int ring = 4;
    mc::cinematic::FrameFormat format;
    // rows are appended in capture order by the render thread (g_rows_mutex)
    core::TakeBook book;
    std::vector<Row> rows;
    std::vector<double> gate_ms;  // game thread: the wait at each gate of the take, for the frame before it
    uint64_t ring_stalls = 0;
    uint32_t ring_max = 0;
    bool announced_rate_change = false;
};
Take g_take;
std::mutex g_rows_mutex;
bool g_announced_no_renderer = false;

// ---------------------------------------------------------------------------------------------------------------------
// the sink: a ring of host buffers, a writer thread, ffmpeg or a file
// ---------------------------------------------------------------------------------------------------------------------
#if defined(_WIN32)
std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
#endif

class Pipe {
  public:
    // Starts `command_line` with its standard input a pipe, its output and errors in `log_path`.
    bool Start(const std::string& command_line, const std::string& log_path) {
#if defined(_WIN32)
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof sa;
        sa.bInheritHandle = TRUE;
        HANDLE read = nullptr, write = nullptr;
        if (!CreatePipe(&read, &write, &sa, 4u << 20)) return false;
        SetHandleInformation(write, HANDLE_FLAG_INHERIT, 0);
        HANDLE log = CreateFileW(Widen(log_path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                 nullptr);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = read;
        si.hStdOutput = log != INVALID_HANDLE_VALUE ? log : GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = si.hStdOutput;
        PROCESS_INFORMATION pi{};
        std::wstring cmd = Widen(command_line);
        const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(read);
        if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
        if (!ok) {
            CloseHandle(write);
            return false;
        }
        CloseHandle(pi.hThread);
        process_ = pi.hProcess;
        write_ = write;
        return true;
#else
        file_ = popen((command_line + " >\"" + log_path + "\" 2>&1").c_str(), "w");
        return file_ != nullptr;
#endif
    }

    bool Write(const uint8_t* data, uint64_t bytes) {
#if defined(_WIN32)
        while (bytes > 0) {
            const DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(bytes, 1u << 20));
            DWORD written = 0;
            if (!WriteFile(write_, data, chunk, &written, nullptr) || written == 0) return false;
            data += written;
            bytes -= written;
        }
        return true;
#else
        return std::fwrite(data, 1, static_cast<size_t>(bytes), file_) == bytes;
#endif
    }

    // Closes the pipe and waits for the program to finish; its exit code (-1: it did not finish in time).
    int Close(int wait_ms) {
#if defined(_WIN32)
        if (write_) CloseHandle(write_);
        write_ = nullptr;
        int code = -1;
        if (process_) {
            if (WaitForSingleObject(process_, static_cast<DWORD>(wait_ms)) == WAIT_OBJECT_0) {
                DWORD c = 0;
                GetExitCodeProcess(process_, &c);
                code = static_cast<int>(c);
            }
            CloseHandle(process_);
        }
        process_ = nullptr;
        return code;
#else
        (void)wait_ms;
        int code = file_ ? pclose(file_) : -1;
        file_ = nullptr;
        return code;
#endif
    }

  private:
#if defined(_WIN32)
    HANDLE process_ = nullptr;
    HANDLE write_ = nullptr;
#else
    FILE* file_ = nullptr;
#endif
};

struct WriteItem {
    uint32_t slot = 0;
    uint64_t row = 0;
    uint64_t bytes = 0;
};

struct Sink {
    std::mutex mutex;
    std::condition_variable cv_work, cv_free;
    bool open = false;
    bool stop = false;
    bool failed = false;
    mc::cinematic::FrameFormat format;
    uint64_t frame_bytes = 0;
    std::vector<std::unique_ptr<uint8_t[]>> buffers;
    std::vector<uint32_t> free_slots;
    std::deque<WriteItem> queue;
    std::thread writer;
    Pipe pipe;
    FILE* raw = nullptr;
    core::SinkKind kind = core::SinkKind::kNone;
    uint64_t bytes_written = 0;
    int ffmpeg_exit = -2;
};
Sink& g_sink = *new Sink;  // never destroyed: the writer thread ends with its take, not with the process

void WriterMain() {
    Sink& s = g_sink;
    for (;;) {
        WriteItem item;
        {
            std::unique_lock<std::mutex> lock(s.mutex);
            s.cv_work.wait(lock, [&] { return s.stop || !s.queue.empty(); });
            if (s.queue.empty()) return;  // stop, and drained
            item = s.queue.front();
            s.queue.pop_front();
        }
        const auto t0 = Clock::now();
        bool ok = true;
        const uint8_t* data = s.buffers[item.slot].get();
        if (!s.failed) {
            if (s.kind == core::SinkKind::kFfmpeg) {
                ok = s.pipe.Write(data, item.bytes);
            } else if (s.kind == core::SinkKind::kRaw && s.raw) {
                ok = std::fwrite(data, 1, static_cast<size_t>(item.bytes), s.raw) == item.bytes;
            }
        }
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        if (!ok && !s.failed) {
            s.failed = true;
            LARECOMP_APP_ERROR("[cap] the sink stopped taking frames at row {} (ffmpeg gone, or the disk is full); the take goes on "
                               "counting and dropping",
                               item.row);
        }
        {
            std::lock_guard<std::mutex> lock(g_rows_mutex);
            if (item.row < g_take.rows.size()) g_take.rows[item.row].write_ms = ok ? ms : -2.0;
        }
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            if (ok) s.bytes_written += item.bytes;
            s.free_slots.push_back(item.slot);
        }
        s.cv_free.notify_all();
    }
}

// The take's sink for frames of `f`; the render thread, at its first frame.
bool OpenSink(const mc::cinematic::FrameFormat& f) {
    Sink& s = g_sink;
    Take& t = g_take;
    s.format = f;
    s.frame_bytes = uint64_t(f.width) * f.height * 4;
    s.kind = t.sink;
    s.failed = false;
    s.stop = false;
    s.bytes_written = 0;
    s.queue.clear();
    s.free_slots.clear();
    s.buffers.clear();
    const int ring = std::clamp(t.ring, 2, 8);
    for (int i = 0; i < ring; ++i) {
        s.buffers.emplace_back(new uint8_t[static_cast<size_t>(s.frame_bytes)]);
        s.free_slots.push_back(static_cast<uint32_t>(i));
    }
    char fps[32];
    std::snprintf(fps, sizeof fps, "%.6g", t.fps);
    if (s.kind == core::SinkKind::kFfmpeg) {
        // CHECK: `-pix_fmt bgr0` is taken by the ffv1 encoder of the ffmpeg 9.0 full build (E7's 4K take ran with it: 1224 MB mkv).
        const std::string cmd = core::FfmpegCommandLine(REXCVAR_GET(mcla_capture_ffmpeg), f.width, f.height, f.bgra, t.fps,
                                                        REXCVAR_GET(mcla_capture_ffmpeg_args), t.out + ".mkv");
        if (!s.pipe.Start(cmd, t.out + ".ffmpeg.log")) {
            LARECOMP_APP_ERROR("[cap] cannot start ffmpeg: {}", cmd);
            s.kind = core::SinkKind::kNone;
            s.failed = true;
        } else {
            LARECOMP_APP_INFO("[cap] ffmpeg: {}", cmd);
        }
    } else if (s.kind == core::SinkKind::kRaw) {
        s.raw = std::fopen((t.out + ".bgra").c_str(), "wb");
        if (!s.raw) {
            LARECOMP_APP_ERROR("[cap] cannot open {}.bgra", t.out);
            s.kind = core::SinkKind::kNone;
            s.failed = true;
        } else {
            std::setvbuf(s.raw, nullptr, _IONBF, 0);
        }
    }
    // a description next to the frames, so that a raw file can be read later and a take knows where in its scene it started
    if (FILE* info = std::fopen((t.out + ".info.txt").c_str(), "wb")) {
        std::fprintf(info, "width=%u\nheight=%u\nformat=%s\nfps=%s\nframe_bytes=%llu\nfirst_update=%llu\nfirst_scene_t=%s\nsink=%s\n",
                     f.width, f.height, f.bgra ? "bgra" : "rgba", fps, static_cast<unsigned long long>(s.frame_bytes),
                     static_cast<unsigned long long>(t.first_update),
                     t.scene_running ? std::to_string(t.first_scene_t).c_str() : "none", core::SinkKindName(t.sink));
        std::fclose(info);
    }
    s.writer = std::thread(WriterMain);
    s.open = true;
    return true;
}

// Game thread, take end: the writer drains, ffmpeg finishes.
void CloseSink() {
    Sink& s = g_sink;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.open) return;
        s.stop = true;
    }
    s.cv_work.notify_all();
    s.cv_free.notify_all();
    if (s.writer.joinable()) s.writer.join();
    if (s.kind == core::SinkKind::kFfmpeg) s.ffmpeg_exit = s.pipe.Close(180000);
    if (s.raw) {
        std::fclose(s.raw);
        s.raw = nullptr;
    }
    s.buffers.clear();
    s.open = false;
}

// ---------------------------------------------------------------------------------------------------------------------
// the files and the summary
// ---------------------------------------------------------------------------------------------------------------------
void WriteCsv(const Take& t) {
    FILE* f = std::fopen((t.out + ".cap.csv").c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "#capture v1 fps=%g step_ms=%.6f first_update=%llu sink=%s ring=%d lag=%d virtual_clock=%d width=%u height=%u\n", t.fps,
                 t.step_ms, static_cast<unsigned long long>(t.first_update), core::SinkKindName(t.sink), t.ring, t.lag,
                 t.virtual_clock ? 1 : 0, t.format.width, t.format.height);
    std::fprintf(f, "frame,update,t_ms,game_wait_ms,record_ms,gpu_ms,copy_ms,stall_ms,write_ms,ring_fill,vram_mb,lost\n");
    for (size_t k = 0; k < t.rows.size(); ++k) {
        const Row& r = t.rows[k];
        const double gate = k < t.gate_ms.size() ? t.gate_ms[k] : -1.0;
        std::fprintf(f, "%zu,%llu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%.1f,%d\n", k, static_cast<unsigned long long>(r.update), r.t_ms,
                     gate, r.c.record_ms, r.c.gpu_ms, r.c.copy_ms, r.c.stall_ms, r.write_ms, r.ring_fill, r.c.vram_mb, r.lost ? 1 : 0);
    }
    std::fclose(f);
}

void LogSummary(const Take& t, double wall_s, const core::GateBook& gate) {
    std::vector<double> gw, record, gpu, copy, stall, write, vram;
    for (size_t k = 0; k < t.rows.size(); ++k) {
        const Row& r = t.rows[k];
        if (r.lost) continue;
        if (k < t.gate_ms.size()) gw.push_back(t.gate_ms[k]);
        record.push_back(r.c.record_ms);
        gpu.push_back(r.c.gpu_ms);
        copy.push_back(r.c.copy_ms);
        stall.push_back(r.c.stall_ms);
        write.push_back(r.write_ms);
        if (r.c.vram_mb > 0) vram.push_back(r.c.vram_mb);
    }
    const uint64_t frames = t.rows.size() - t.book.Lost();
    const double video_s = double(frames) / t.fps;
    const double bytes = double(frames) * double(g_sink.frame_bytes);
    const double vmin = vram.empty() ? 0 : *std::min_element(vram.begin(), vram.end());
    const double vmax = vram.empty() ? 0 : *std::max_element(vram.begin(), vram.end());
    const core::Stat wr = core::StatOf(write);
    const double write_mbs = wr.mean > 0 ? double(g_sink.frame_bytes) / (wr.mean / 1000.0) / 1048576.0 : 0.0;  // the writer's own rate
    LARECOMP_APP_INFO(
        "[cap] summary frames={} lost={} updates={}..{} wall_s={:.2f} video_s={:.2f} speed={:.2f}x size={}x{} sink={} ring={} lag={} "
        "clock={} | ms mean/p95/max: game_wait={} record={} gpu_wait={} copy={} stall={} write={} | MB/s: delivered={:.0f} writer_max={:.0f} "
        "| ring: max_fill={} stalls={} | gate: timeouts={} off={} | tag_anomalies={} repeated_handovers={} | vram_mb min/max={:.0f}/{:.0f} | "
        "upscaler: anchors={} history_restarts={} (gap_over_100ms={}) | ffmpeg_exit={}",
        frames, t.book.Lost(), t.first_update, t.rows.empty() ? t.first_update : t.rows.back().update, wall_s, video_s,
        wall_s > 0 ? video_s / wall_s : 0.0, t.format.width, t.format.height, core::SinkKindName(t.sink), t.ring, t.lag,
        t.virtual_clock ? "virtual" : "steady", core::FormatStat(core::StatOf(gw)), core::FormatStat(core::StatOf(record)),
        core::FormatStat(core::StatOf(gpu)), core::FormatStat(core::StatOf(copy)), core::FormatStat(core::StatOf(stall)),
        core::FormatStat(wr), wall_s > 0 ? bytes / wall_s / 1048576.0 : 0.0, write_mbs, t.ring_max, t.ring_stalls, gate.timeouts,
        gate.off ? 1 : 0, t.book.Anomalies(), g_handover.Repeats(), vmin, vmax, g_anchors.load(), g_anchor_restarts.load(),
        g_anchor_gap_restarts.load(), g_sink.ffmpeg_exit);
}

// The costs of take frame `k`, one line.
void LogFrame(const Take& t, size_t k) {
    std::lock_guard<std::mutex> lock(g_rows_mutex);
    if (k >= t.rows.size()) return;
    const Row& r = t.rows[k];
    const double gate = k < t.gate_ms.size() ? t.gate_ms[k] : 0.0;
    LARECOMP_APP_INFO("[cap] f={} u={} t={:.0f}ms game_wait={:.1f} record={:.2f} gpu_wait={:.1f} copy={:.1f} stall={:.1f} write={:.1f} ring={}/{} "
                      "vram={:.0f}MB",
                      k, r.update, r.t_ms, gate, r.c.record_ms, r.c.gpu_ms, r.c.copy_ms, r.c.stall_ms, r.write_ms, r.ring_fill, t.ring,
                      r.c.vram_mb);
}

// ---------------------------------------------------------------------------------------------------------------------
// the take's start and end, and the gate
// ---------------------------------------------------------------------------------------------------------------------
// The rate a take is asked for now: the scene's, else the debug setting's; 0 = none.
double WantedFps() {
    const double scene = g_scene_fps.load(std::memory_order_relaxed);
    if (scene > 0.0) return core::ClampFps(scene);
    return core::ClampFps(REXCVAR_GET(mcla_capture_fps));
}

// Waits (at most `timeout_ms`) until `needed` take frames have been read back or reported lost. True: met.
bool WaitForFrames(uint64_t needed, int timeout_ms) {
    std::unique_lock<std::mutex> lock(g_gate_mutex);
    return g_gate_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return g_gate.Met(needed); });
}

void BeginTake(uint64_t u0, double fps) {
    Take& t = g_take;
    {
        std::lock_guard<std::mutex> lock(g_rows_mutex);
        t = Take{};
    }
    t.out = REXCVAR_GET(mcla_capture_out);
    if (!core::ParseSinkKind(REXCVAR_GET(mcla_capture_sink), &t.sink)) {
        LARECOMP_APP_ERROR("[cap] mcla_capture_sink '{}' is none, ffmpeg or raw: using none", REXCVAR_GET(mcla_capture_sink));
    }
    t.fps = fps;
    t.ticks = core::StepTicksForFps(fps);
    t.step_ms = 1000.0 / fps;
    t.first_update = u0;
    t.virtual_clock = REXCVAR_GET(mcla_capture_virtual_clock);
    t.lag = std::clamp<int>(REXCVAR_GET(mcla_capture_lag), 0, 2);
    t.ring = REXCVAR_GET(mcla_capture_ring);
    t.scene_running = CurrentSceneState() == SceneState::kRunning || CurrentSceneState() == SceneState::kFinished;
    // the scene's time at the update before this one, and this update's own step on top: the time this update's frame shows
    t.first_scene_t = SceneTime() + t.step_ms / 1000.0;
    {
        std::error_code ec;
        const std::filesystem::path p(t.out);
        if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    }
    t.book.Begin(u0);
    {
        std::lock_guard<std::mutex> lock(g_gate_mutex);
        g_gate = core::GateBook{};
    }
    g_handover.BeginTake();
    g_sink.ffmpeg_exit = -2;
    g_sink.frame_bytes = 0;
    g_anchors = 0;
    g_anchor_restarts = 0;
    g_anchor_gap_restarts = 0;
    t.t0 = Clock::now();
    g_take_first.store(u0, std::memory_order_release);
    g_take_ticks.store(t.ticks, std::memory_order_release);
    g_take_active.store(true, std::memory_order_release);
    LARECOMP_APP_INFO("[cap] take starts at update {} ({:g} fps, step {:.4f} ms = {} ticks, sink {}, out {}, ring {}, lag {}, virtual clock {}{})", u0,
                      fps, t.step_ms, t.ticks, core::SinkKindName(t.sink), t.out, t.ring, t.lag, t.virtual_clock ? "on" : "off",
                      t.scene_running ? std::string(", scene t of the first frame ") + std::to_string(t.first_scene_t) : std::string());
}

void EndTake(uint64_t u_end) {
    Take& t = g_take;
    const double wall_s = std::chrono::duration<double>(Clock::now() - t.t0).count();
    const uint64_t handed = g_handover.Handed();
    g_take_active.store(false, std::memory_order_release);
    CloseSink();
    core::GateBook gate;
    {
        std::lock_guard<std::mutex> lock(g_gate_mutex);
        gate = g_gate;
    }
    {
        std::lock_guard<std::mutex> lock(g_rows_mutex);
        const core::Verdict v = core::Judge(t.book, u_end, handed);
        if (v.exact) {
            LARECOMP_APP_INFO("[cap] frame count: {}", v.text);
        } else {
            LARECOMP_APP_ERROR("[cap] FRAME COUNT: {}", v.text);
        }
        WriteCsv(t);
        LogSummary(t, wall_s, gate);
    }
}

// The gate of update `u` while a take runs (game thread).
void GateInTake(uint64_t u, double want_fps) {
    Take& t = g_take;
    const bool ending = !(want_fps > 0.0);
    if (!ending && std::fabs(want_fps - t.fps) > 1e-9 && !t.announced_rate_change) {
        t.announced_rate_change = true;  // a take keeps its rate: the log says so once
        LARECOMP_APP_WARN("[cap] a take of {:g} fps is running: the request for {:g} is ignored until it ends", t.fps, want_fps);
    }
    const uint64_t handed = g_handover.Handed();
    const uint64_t needed = core::FramesNeeded(handed, static_cast<uint32_t>(t.lag), ending);
    const int timeout_ms = REXCVAR_GET(mcla_capture_gate_timeout_ms);
    const auto w0 = Clock::now();
    bool off;
    {
        std::lock_guard<std::mutex> lock(g_gate_mutex);
        off = g_gate.off;
    }
    bool ok = true;
    if (!off) ok = WaitForFrames(needed, timeout_ms);
    const double wait_ms = std::chrono::duration<double, std::milli>(Clock::now() - w0).count();
    if (!ok) {
        uint64_t captured = 0;
        uint32_t in_a_row = 0;
        bool now_off = false;
        {
            std::lock_guard<std::mutex> lock(g_gate_mutex);
            g_gate.TimedOut(needed);
            captured = g_gate.captured;
            in_a_row = g_gate.consecutive_timeouts;
            now_off = g_gate.off;
        }
        LARECOMP_APP_ERROR("[cap] gate timeout ({} ms) at update {}: {} of {} take frames read back; books set straight (timeout {} in a row)",
                           timeout_ms, u, captured, needed, in_a_row);
        if (now_off) LARECOMP_APP_ERROR("[cap] three gate timeouts in a row: the game is not held at the gate for the rest of the take");
    } else {
        std::lock_guard<std::mutex> lock(g_gate_mutex);
        g_gate.NoteMet();
    }
    t.gate_ms.push_back(wait_ms);
    if (const int every = REXCVAR_GET(mcla_capture_log_every); every > 0 && !t.gate_ms.empty()) {
        const uint64_t k_prev = t.gate_ms.size() - 1;
        if (k_prev % uint64_t(every) == 0 || k_prev < 3) LogFrame(t, static_cast<size_t>(k_prev));
    }
    if (ending) EndTake(u);
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// the interface: game thread and swap thread
// ---------------------------------------------------------------------------------------------------------------------
void CaptureGate() {
    const uint64_t before = g_updates.load(std::memory_order_relaxed);  // updates that ran
    const uint64_t u = before + 1;                                      // the update about to run
    if (!g_take_active.load(std::memory_order_relaxed)) {
        const double want = WantedFps();
        if (want > 0.0) {
#if defined(LARECOMP_HAS_GPU)
            if (!mcla::gpu::Enabled()) {
                if (!g_announced_no_renderer) {
                    g_announced_no_renderer = true;
                    LARECOMP_APP_ERROR("[cap] a take needs the Plume renderer (the native one is not in use): no take");
                }
                g_updates.store(u, std::memory_order_release);
                return;
            }
#endif
            BeginTake(u, want);
        }
        g_updates.store(u, std::memory_order_release);
        return;
    }
    GateInTake(u, WantedFps());
    g_updates.store(u, std::memory_order_release);
}

void CaptureOnHandOver() {
    if (!g_take_active.load(std::memory_order_acquire)) return;
    const core::HandOver h = g_handover.Note(g_updates.load(std::memory_order_relaxed), true, g_take_first.load(std::memory_order_acquire));
    g_stamp.store(h.tag, std::memory_order_release);
}

void CaptureOnSwap() {
    // This runs on the guest's render thread BEFORE D3DDevice_Swap writes the present record (gpu.h says so of SetNextPresentTarget, which
    // frame pacing sets from the same hook): a tag that arrived after the record would be dropped. The stamp was set by the hand-over of
    // this very frame, while this thread was idle, so it can neither be older nor newer than the frame.
    if (g_stamp.load(std::memory_order_acquire) == 0) return;
    const uint64_t tag = g_stamp.exchange(0, std::memory_order_acq_rel);
#if defined(LARECOMP_HAS_GPU)
    mcla::gpu::SetNextPresentTag(tag);
#else
    (void)tag;
#endif
}

bool CaptureTakeActive() {
    return g_take_active.load(std::memory_order_acquire);
}

void CaptureSceneRequest(double fps) {
    const double before = g_scene_fps.exchange(fps > 0.0 ? fps : 0.0, std::memory_order_relaxed);
    if ((before > 0.0) != (fps > 0.0)) {
        if (fps > 0.0) {
            LARECOMP_APP_INFO("[cap] the scene asks for a take at {:g} fps", fps);
        } else {
            LARECOMP_APP_INFO("[cap] the scene ends the take");
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// the fixed step
// ---------------------------------------------------------------------------------------------------------------------
uint64_t FixedStepTicks() {
    if (g_take_active.load(std::memory_order_acquire)) return g_take_ticks.load(std::memory_order_acquire);
    return core::StepTicksForFps(REXCVAR_GET(mcla_fixed_step_fps));
}

bool FixedStepActive() {
    return g_take_active.load(std::memory_order_acquire) || REXCVAR_GET(mcla_fixed_step_fps) > 0.0;
}

// ---------------------------------------------------------------------------------------------------------------------
// the scene clock
// ---------------------------------------------------------------------------------------------------------------------
namespace {
bool ClockWanted() {
    return g_take_active.load(std::memory_order_acquire) && REXCVAR_GET(mcla_capture_virtual_clock) &&
           g_take_ticks.load(std::memory_order_acquire) != 0;
}
}  // namespace

double SceneClockNowMs(uint64_t frame) {
    const double steady = SteadyMs();
    const bool want = ClockWanted();
    if (!want && !g_clock_on.load(std::memory_order_acquire)) return steady;  // the usual case: two loads
    std::lock_guard<std::mutex> lock(g_clock_mutex);
    if (g_clock.SetActive(want)) {
        g_clock_on.store(want, std::memory_order_release);
        g_clock_epoch.store(g_clock.epoch(), std::memory_order_release);
    }
    if (!g_clock.active()) return steady;
    const double step_ms = core::StepMsForTicks(g_take_ticks.load(std::memory_order_acquire));
    const bool by_update = frame == kNoFrame;
    return g_clock.NowMs(by_update ? 0 : 1, by_update ? g_updates.load(std::memory_order_relaxed) : frame, steady, step_ms);
}

int64_t SceneClockNowNs() {
    if (!ClockWanted() && !g_clock_on.load(std::memory_order_acquire)) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    }
    return static_cast<int64_t>(SceneClockNowMs() * 1e6);
}

uint32_t SceneClockEpoch() {
    const bool want = ClockWanted();
    if (want || g_clock_on.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(g_clock_mutex);
        if (g_clock.SetActive(want)) {
            g_clock_on.store(want, std::memory_order_release);
            g_clock_epoch.store(g_clock.epoch(), std::memory_order_release);
        }
    }
    return g_clock_epoch.load(std::memory_order_acquire);
}

void SceneClockNoteAnchor(uint32_t reset, uint32_t time_bit) {
    if (!g_take_active.load(std::memory_order_relaxed)) return;
    g_anchors.fetch_add(1, std::memory_order_relaxed);
    if (reset != 0) g_anchor_restarts.fetch_add(1, std::memory_order_relaxed);
    if ((reset & time_bit) != 0) g_anchor_gap_restarts.fetch_add(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------------------------------
// the interface: render thread
// ---------------------------------------------------------------------------------------------------------------------
int CaptureLag() {
    return std::clamp<int>(REXCVAR_GET(mcla_capture_lag), 0, 2);
}

double CaptureDelayMs() {
    return REXCVAR_GET(mcla_capture_delay_ms);
}

bool CaptureSinkOpen(const FrameFormat& format) {
    if (!g_take_active.load(std::memory_order_acquire)) return false;
    {
        std::lock_guard<std::mutex> lock(g_sink.mutex);
        if (g_sink.open) {
            if (g_sink.format.width != format.width || g_sink.format.height != format.height) {
                LARECOMP_APP_ERROR("[cap] the frame size changed during the take ({}x{} -> {}x{}): frames dropped", g_sink.format.width,
                                   g_sink.format.height, format.width, format.height);
                return false;
            }
            return true;
        }
    }
    g_take.format = format;
    return OpenSink(format);
}

HostBuffer CaptureAcquire(double* stall_ms) {
    Sink& s = g_sink;
    HostBuffer b;
    const auto t0 = Clock::now();
    std::unique_lock<std::mutex> lock(s.mutex);
    if (!s.open) {
        if (stall_ms) *stall_ms = 0;
        return b;
    }
    s.cv_free.wait(lock, [&] { return s.stop || !s.open || !s.free_slots.empty(); });
    if (s.free_slots.empty()) {
        if (stall_ms) *stall_ms = 0;
        return b;
    }
    b.slot = s.free_slots.back();
    s.free_slots.pop_back();
    b.data = s.buffers[b.slot].get();
    b.bytes = s.frame_bytes;
    lock.unlock();
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    if (stall_ms) *stall_ms = ms;
    if (ms > 1.0) {
        std::lock_guard<std::mutex> rows(g_rows_mutex);
        ++g_take.ring_stalls;
    }
    return b;
}

void CaptureFrameDone(const HostBuffer& buffer, const FrameCosts& costs) {
    Sink& s = g_sink;
    uint64_t row_index = 0;
    uint32_t fill = 0;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        fill = static_cast<uint32_t>(s.queue.size()) + 1;
    }
    {
        std::lock_guard<std::mutex> lock(g_rows_mutex);
        Take& t = g_take;
        Row r;
        r.update = costs.update;
        r.c = costs;
        r.t_ms = std::chrono::duration<double, std::milli>(Clock::now() - t.t0).count();
        r.ring_fill = fill;
        t.ring_max = std::max(t.ring_max, fill);
        row_index = t.rows.size();
        // the stamp is the update: the take's frames must be consecutive updates from the first
        const uint64_t want = t.rows.empty() ? t.first_update : t.rows.back().update + 1;
        if (!t.book.OnFrame(costs.update, false) && t.book.Anomalies() <= 8) {
            LARECOMP_APP_ERROR("[cap] TAG: take frame {} carries update {}, expected {} ({})", row_index, costs.update, want,
                               costs.update < want ? "a repeated frame" : "a frame is missing");
        }
        t.rows.push_back(r);
    }
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.queue.push_back(WriteItem{buffer.slot, row_index, buffer.bytes});
    }
    s.cv_work.notify_all();
    {
        std::lock_guard<std::mutex> lock(g_gate_mutex);
        ++g_gate.captured;
    }
    g_gate_cv.notify_all();
}

void CaptureFrameLost(uint64_t update, const char* why) {
    if (!g_take_active.load(std::memory_order_acquire)) {
        LARECOMP_APP_WARN("[cap] frame of update {} dropped after the take ended: {}", update, why ? why : "?");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_rows_mutex);
        Take& t = g_take;
        Row r;
        r.update = update;
        r.lost = true;
        r.t_ms = std::chrono::duration<double, std::milli>(Clock::now() - t.t0).count();
        const uint64_t want = t.rows.empty() ? t.first_update : t.rows.back().update + 1;
        if (!t.book.OnFrame(update, true) && t.book.Anomalies() <= 8) {
            LARECOMP_APP_ERROR("[cap] TAG: take frame {} carries update {}, expected {} ({})", t.rows.size(), update, want,
                               update < want ? "a repeated frame" : "a frame is missing");
        }
        t.rows.push_back(r);
    }
    LARECOMP_APP_ERROR("[cap] frame of update {} lost: {}", update, why ? why : "?");
    {
        std::lock_guard<std::mutex> lock(g_gate_mutex);
        ++g_gate.captured;
    }
    g_gate_cv.notify_all();
}

// ---------------------------------------------------------------------------------------------------------------------
// the capture applier: a scene's `capture` keys ask for the take
// ---------------------------------------------------------------------------------------------------------------------
namespace {

void ApplierBegin(const Scene&) {
    CaptureSceneRequest(0.0);
}

void ApplierApply(const Scene& scene, double t) {
    if (scene.capture_fps <= 0.0) return;  // a scene with no capture key costs nothing
    const CaptureWindow w = EvaluateCapture(scene, t);
    CaptureSceneRequest(w.on ? w.fps : 0.0);
}

void ApplierEnd() {
    CaptureSceneRequest(0.0);
}

const TrackApplier kCaptureApplier = {
    "capture",
    TrackBit(Track::kCapture),
    &ApplierBegin,
    &ApplierApply,
    &ApplierEnd,
};

// The applier is registered when the program starts; the runner's own tables are constant-initialised, so the order of the
// translation units does not matter (the camera's applier does the same).
struct Registrar {
    Registrar() {
        if (!RegisterTrackApplier(&kCaptureApplier)) {
            // no way to log yet: the runner would report the capture keys as "keys nobody applies"
        }
    }
} g_registrar;

}  // namespace

}  // namespace mc::cinematic

#else  // REXGLUE_HAS_XEO3_TARGET

#include "capture_mode.h"

#include <chrono>

namespace mc::cinematic {
void CaptureGate() {}
void CaptureOnHandOver() {}
void CaptureOnSwap() {}
bool CaptureTakeActive() { return false; }
void CaptureSceneRequest(double) {}
uint64_t FixedStepTicks() { return 0; }
bool FixedStepActive() { return false; }
double SceneClockNowMs(uint64_t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
int64_t SceneClockNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
uint32_t SceneClockEpoch() { return 0; }
void SceneClockNoteAnchor(uint32_t, uint32_t) {}
}  // namespace mc::cinematic

#endif  // REXGLUE_HAS_XEO3_TARGET
