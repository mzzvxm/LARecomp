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
//   mcla_capture_dir <folder>      where the take folders go (captures, relative to the game's working folder). A take is a FOLDER (take_folder.h
//                                  lists what is in it: take.json, the video or the pictures, frames.csv, state.csv, the scene's copy).
//   mcla_capture_name <name>       the take's folder name under it when the scene gives none (`capture fps N name <take>`); else the scene's
//                                  own name, else "take". A name that is taken gets -2, -3 ... : a take never overwrites another.
//   mcla_capture_out <folder>      the take folder itself, exactly (the check chain and scene_run.py use it); it has to be empty or new.
//   mcla_capture_sink none|ffmpeg|png|raw   what happens to a frame once it is in a host buffer: nothing (the readback and its copies are
//                                  measured alone), a pipe to ffmpeg (FFV1 in video.mkv by default), a pipe to ffmpeg that writes one PNG
//                                  for every frame (frames/f000000.png ...) or a raw file of frames (video.bgra, rows packed, B8G8R8A8 as
//                                  the swap chain has them). A scene's `sink` wins. Read when a take starts.
//   mcla_capture_statelog          a state.csv in the take (the scene's `statelog on` does the same).
//   mcla_capture_ffmpeg <path>, mcla_capture_ffmpeg_args <text>   the program and its output options (the mkv sink's).
//   mcla_capture_ring <n>          host buffers between the readback and the writer (2 to 8, 4 default); fewer when the memory cap says.
//   mcla_capture_memory_mb <n>     the hard cap on the memory a take holds (GPU readback buffers and the ring, 2048 default); a take that
//                                  cannot fit at the least it needs (2 + 2 frames) does not run, and says so.
//   mcla_capture_ram_floor_mb <n>  free physical memory that has to stay free (1024 default): a take whose buffers would take it lower is
//                                  cut down or refused at its start, and a take stops with an error when the free memory falls below it.
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
// update, makes the take's rows and updates disagree, and the verdict at the end says so. A wait that runs out is a wait in which NOTHING moved
// for the timeout (no frame read back, lost or written): a writer that is slow but finishes frames never times the game out.
//
// THE TAKE FOLDER (M2b). A take is a folder (take_folder.h lists what is in it); take.json is written when the take starts and again when it ends, with the
// verdict: `complete` (one picture for every update, none dropped or repeated, the output is what the frames say), `inexact`, or `failed` with the reason.
//
// THE MEMORY BOUND (M2b). The memory a take holds is fixed when it starts: CaptureMemoryPlan (the render thread, at the first frame) fits the GPU readback
// buffers and the host ring to mcla_capture_memory_mb and to the PC's free memory less mcla_capture_ram_floor_mb (take_folder.h PlanMemory), or the take
// fails with what it needs. The ring is core::HostRing: the render thread takes a free buffer (CaptureAcquire) and WAITS when all are queued or being
// written, so a writer slower than the render slows the render, the gate and the game, and nothing is queued anywhere else; it gives up only when the
// writer has finished nothing for the gate's timeout (the take fails, ffmpeg is ended). The game thread watches the free memory every 15 updates and stops
// the take when it falls below the floor. A failed take (TakeFail) ends at the game's next gate; the scene's request is not taken up again until it has
// gone to 0.
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
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
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
#include "larecomp_version.h"
#include "scene_runtime.h"
#include "take_folder.h"

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
REXCVAR_DEFINE_STRING(mcla_capture_dir, "captures", "MCLA/Cinematic",
    "TRAILER TOOLKIT capture mode: the folder the take folders go in (a relative path is relative to the game's folder). Read when a "
    "take starts.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();
REXCVAR_DEFINE_STRING(mcla_capture_name, "", "MCLA/Cinematic",
    "TRAILER TOOLKIT capture mode: the take's folder name when the scene gives none (a name that is taken gets -2, -3 ...). Empty: "
    "the scene's own name, else take. Read when a take starts.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();
REXCVAR_DEFINE_STRING(mcla_capture_out, "", "MCLA/Cinematic",
    "TRAILER TOOLKIT capture mode: the take folder itself, exactly (it has to be new or empty); the take then ignores the dir and "
    "the name. Empty: <dir>/<name>. Read when a take starts.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();
REXCVAR_DEFINE_STRING(mcla_capture_sink, "none", "MCLA/Cinematic",
    "TRAILER TOOLKIT capture mode: what happens to a frame once it is read back: none (counted and dropped), ffmpeg (a pipe to the "
    "program in mcla_capture_ffmpeg: FFV1 in video.mkv), png (ffmpeg writes one PNG for every frame) or raw (video.bgra, B8G8R8A8 "
    "frames). A scene's sink wins. Read when a take starts.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();
REXCVAR_DEFINE_BOOL(mcla_capture_statelog, false, "MCLA/Cinematic",
    "TRAILER TOOLKIT capture mode: write state.csv in the take (the game's state for every frame: the scene's statelog on does the "
    "same). Read when a take starts.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();
REXCVAR_DEFINE_STRING(mcla_capture_ffmpeg, "ffmpeg", "MCLA/Cinematic",
    "TRAILER TOOLKIT capture mode (sink ffmpeg): the ffmpeg program, a full path or a name on PATH.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();
REXCVAR_DEFINE_STRING(mcla_capture_ffmpeg_args, "-c:v ffv1 -level 3 -g 1 -slices 24 -pix_fmt bgr0", "MCLA/Cinematic",
    "TRAILER TOOLKIT capture mode (sink ffmpeg): ffmpeg's output options, between the raw input and video.mkv.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();
REXCVAR_DEFINE_INT32(mcla_capture_ring, 4, "MCLA/Cinematic",
    "TRAILER TOOLKIT capture mode: host buffers between the readback and the writer (2 to 8): the most frames that wait for the writer. "
    "Fewer are used when mcla_capture_memory_mb or the free memory says so. Read when a take starts.")
    .range(2, 8)
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();
REXCVAR_DEFINE_INT32(mcla_capture_memory_mb, 2048, "MCLA/Cinematic",
    "TRAILER TOOLKIT capture mode: the hard cap, in MiB, on the memory a take holds (its GPU readback buffers and its ring, one frame "
    "each). The ring is cut first, then the GPU buffers; a take that does not fit with 2 + 2 frames does not run, and the log says what "
    "to change (a lower render scale). Nothing grows with the take's length or the writer's speed. Read when a take starts.")
    .range(64, 65536)
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();
REXCVAR_DEFINE_INT32(mcla_capture_ram_floor_mb, 1024, "MCLA/Cinematic",
    "TRAILER TOOLKIT capture mode: free physical memory, in MiB, that has to stay free. A take whose buffers would take it lower is cut "
    "down or refused at its start, and a take that sees it fall below stops with an error (an 8K take used up the PC's RAM). 0: no floor.")
    .range(0, 65536)
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
    // the folder
    std::string name;  // its leaf
    core::TakeLayout layout;
    std::string started_utc;
    core::SinkKind sink = core::SinkKind::kNone;
    double fps = 60.0;
    uint64_t ticks = 0;
    double step_ms = 16.666;
    uint64_t first_update = 0;
    double first_scene_t = 0.0;  // the scene time of the take's first update (the scene's time then, plus a step), when a scene runs
    bool scene_running = false;
    std::string scene_name, scene_source, scene_fnv;  // a copy of the file is in the folder when scene_fnv is set
    std::vector<std::pair<std::string, std::string>> cvars;  // the settings that differ from their defaults, at the start
    Clock::time_point t0;
    bool virtual_clock = false;
    int lag = 0;
    int ring = 4;  // as asked; the plan's is what the sink uses
    mc::cinematic::FrameFormat format;
    // the memory plan, made by the render thread at the take's first frame (CaptureMemoryPlan)
    bool plan_made = false;
    uint32_t plan_width = 0, plan_height = 0;
    core::MemoryPlan plan;
    uint64_t cap_bytes = 0;
    uint64_t floor_bytes = 0;
    double ram_start_mb = -1.0, ram_min_mb = -1.0;  // free physical memory (game thread)
    bool state_log = false;
    std::string ffmpeg_command;
    // rows are appended in capture order by the render thread (g_rows_mutex)
    core::TakeBook book;
    std::vector<Row> rows;
    std::vector<double> gate_ms;  // game thread: the wait at each gate of the take, for the frame before it
    uint64_t ring_stalls = 0;
    uint32_t ring_max = 0;
    uint32_t lost_in_a_row = 0;
    bool announced_rate_change = false;
};
Take g_take;
std::mutex g_rows_mutex;
bool g_announced_no_renderer = false;

// What a scene's `capture fps` key asked for besides the rate (CaptureSceneSpec); read when a take starts.
std::mutex g_spec_mutex;
CaptureSpec g_spec;

// A take that cannot go on (the memory plan does not fit, the writer is stuck, the free memory is gone): the first reason is kept, the game
// thread ends the take at its next gate, and the scene's request is not taken up again until it has gone to 0 (g_refused).
std::atomic<bool> g_take_failed{false};
std::atomic<bool> g_sink_stuck{false};  // the writer finished nothing for the gate's timeout: ending the take kills ffmpeg instead of waiting for it
std::mutex g_fail_mutex;
std::string g_fail_reason;  // g_fail_mutex
bool g_refused = false;     // game thread
// Counts every frame read back, lost or written: a wait on the writer or the readback that sees it move is not stuck.
std::atomic<uint64_t> g_progress{0};
// The state log: written on the game thread, at the hand-over of each frame of the take.
std::atomic<bool> g_state_log_on{false};
std::FILE* g_state_file = nullptr;  // game thread
uint64_t g_state_rows = 0;

void TakeFail(const std::string& reason) {
    bool first = false;
    {
        std::lock_guard<std::mutex> lock(g_fail_mutex);
        if (!g_take_failed.load(std::memory_order_relaxed)) {
            g_fail_reason = reason;
            first = true;
        }
        g_take_failed.store(true, std::memory_order_release);
    }
    if (first) LARECOMP_APP_ERROR("[cap] TAKE FAILED: {}", reason);
}

std::string FailReason() {
    std::lock_guard<std::mutex> lock(g_fail_mutex);
    return g_fail_reason;
}

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

    bool started() const {
#if defined(_WIN32)
        return process_ != nullptr;
#else
        return file_ != nullptr;
#endif
    }

    // Ends the program at once (a take that has failed): a write that is stuck on a pipe nobody reads returns, so the writer thread can finish.
    void Kill() {
#if defined(_WIN32)
        if (process_) TerminateProcess(process_, 1);
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

std::filesystem::path Path(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string Utf8(const std::filesystem::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

// A file for writing, named in UTF-8 (the narrow fopen of Windows would read the name in the ANSI code page).
std::FILE* OpenWrite(const std::string& utf8) {
#if defined(_WIN32)
    return _wfopen(Widen(utf8).c_str(), L"wb");
#else
    return std::fopen(utf8.c_str(), "wb");
#endif
}

// The whole text in `path`, written to a name of its own first and renamed over it: a reader (scene_run.py, a person) never sees half of it.
bool WriteTextFile(const std::string& path, const std::string& text) {
    const std::string tmp = path + ".tmp";
    std::FILE* f = OpenWrite(tmp);
    if (!f) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    std::fclose(f);
    std::error_code ec;
    if (!ok) {
        std::filesystem::remove(Path(tmp), ec);
        return false;
    }
    std::filesystem::rename(Path(tmp), Path(path), ec);
    return !ec;
}

// What the take writes frames to. The buffers and the writer thread are the host ring (take_folder.h HostRing): a fixed number of frames in
// memory, the render thread waits when they are all queued, and that wait is the whole of the back-pressure.
struct Sink {
    mc::cinematic::FrameFormat format;
    std::atomic<bool> open{false};
    std::atomic<bool> sink_failed{false};  // a write failed: from then on frames are counted and dropped
    uint64_t frame_bytes = 0;
    core::HostRing ring;
    Pipe pipe;
    std::FILE* raw = nullptr;
    core::SinkKind kind = core::SinkKind::kNone;
    std::atomic<uint64_t> bytes_written{0};
    int ffmpeg_exit = -2;
};
Sink& g_sink = *new Sink;  // never destroyed: the writer thread ends with its take, not with the process

// The host ring's writer thread calls this for every frame, in order. Once a write has failed the frames are counted and dropped (the take
// ends in error at its next gate, TakeFail).
bool WriteFrame(uint64_t row, const uint8_t* data, uint64_t bytes) {
    Sink& s = g_sink;
    const auto t0 = Clock::now();
    bool ok = !s.sink_failed.load(std::memory_order_relaxed);
    if (ok) {
        if (core::SinkUsesFfmpeg(s.kind)) {
            ok = s.pipe.Write(data, bytes);
        } else if (s.kind == core::SinkKind::kRaw && s.raw) {
            ok = std::fwrite(data, 1, static_cast<size_t>(bytes), s.raw) == bytes;
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    if (!ok && !s.sink_failed.exchange(true)) {
        TakeFail("the sink stopped taking frames at frame " + std::to_string(row) +
                 (core::SinkUsesFfmpeg(s.kind) ? " (ffmpeg is gone: see ffmpeg.log in the take folder)" : " (the disk is full, or the file is gone)"));
    }
    {
        std::lock_guard<std::mutex> lock(g_rows_mutex);
        if (row < g_take.rows.size()) g_take.rows[row].write_ms = ok ? ms : -2.0;
    }
    if (ok) s.bytes_written.fetch_add(bytes, std::memory_order_relaxed);
    g_progress.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

// The take's sink for frames of `f`; the render thread, at its first frame. False: it could not be made, and the take has failed (TakeFail).
bool OpenSink(const mc::cinematic::FrameFormat& f) {
    Sink& s = g_sink;
    Take& t = g_take;
    s.format = f;
    s.frame_bytes = uint64_t(f.width) * f.height * 4;
    s.kind = t.sink;
    s.sink_failed = false;
    s.bytes_written = 0;
    s.ffmpeg_exit = -2;
    const uint32_t ring = t.plan_made && t.plan.ok ? t.plan.ring : static_cast<uint32_t>(std::clamp(t.ring, 2, 8));
    std::error_code ec;
    if (s.kind == core::SinkKind::kPng) {
        std::filesystem::create_directories(Path(t.layout.frames_dir), ec);
        if (ec) {
            TakeFail("cannot make the folder " + t.layout.frames_dir + ": " + ec.message());
            return false;
        }
    }
    // The program or the file first: a take that cannot start ffmpeg or open its file never allocates the ring.
    if (core::SinkUsesFfmpeg(s.kind)) {
        // CHECK: `-pix_fmt bgr0` is taken by the ffv1 encoder of the ffmpeg 9.0 full build (E7's 4K take ran with it: 1224 MB mkv); the png
        // line (-c:v png -pix_fmt rgb24, image2 numbering from 0, -fps_mode passthrough) has not run.
        const std::string& program = REXCVAR_GET(mcla_capture_ffmpeg);
        const std::string cmd = s.kind == core::SinkKind::kPng
                                    ? core::FfmpegPngCommandLine(program, f.width, f.height, f.bgra, t.fps, t.layout.frame_pattern)
                                    : core::FfmpegCommandLine(program, f.width, f.height, f.bgra, t.fps, REXCVAR_GET(mcla_capture_ffmpeg_args),
                                                              t.layout.video_mkv);
        t.ffmpeg_command = cmd;
        if (!s.pipe.Start(cmd, t.layout.ffmpeg_log)) {
            TakeFail("cannot start ffmpeg (mcla_capture_ffmpeg = " + program + "): " + cmd);
            return false;
        }
        LARECOMP_APP_INFO("[cap] ffmpeg: {}", cmd);
    } else if (s.kind == core::SinkKind::kRaw) {
        s.raw = OpenWrite(t.layout.video_raw);
        if (!s.raw) {
            TakeFail("cannot open " + t.layout.video_raw);
            return false;
        }
        std::setvbuf(s.raw, nullptr, _IONBF, 0);
    }
    if (!s.ring.Open(ring, s.frame_bytes, &WriteFrame)) {
        TakeFail("could not allocate the ring: " + std::to_string(ring) + " host buffers of " + core::FormatMib(s.frame_bytes) + " (" +
                 std::to_string(f.width) + "x" + std::to_string(f.height) + ")");
        return false;
    }
    LARECOMP_APP_INFO("[cap] ring: {} host buffers of {} (back-pressure: the render thread waits for a free one, nothing is queued beyond them)",
                      ring, core::FormatMib(s.frame_bytes));
    s.open.store(true, std::memory_order_release);
    return true;
}

// Game thread, take end: the writer drains, ffmpeg finishes.
void CloseSink() {
    Sink& s = g_sink;
    s.open.store(false, std::memory_order_release);
    // A writer that is stuck on a pipe nobody reads is not waited for: ending ffmpeg unblocks the write, and the writer thread can finish.
    if (g_sink_stuck.load(std::memory_order_acquire) && s.pipe.started()) s.pipe.Kill();
    s.ring.Close();  // writes everything that was handed over, joins the writer, frees the buffers
    if (s.pipe.started()) s.ffmpeg_exit = s.pipe.Close(180000);
    if (s.raw) {
        std::fclose(s.raw);
        s.raw = nullptr;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// the host: free memory, the exe, the settings
// ---------------------------------------------------------------------------------------------------------------------
// Free physical memory, bytes; 0 when it cannot be read.
uint64_t AvailablePhysicalBytes() {
#if defined(_WIN32)
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    return GlobalMemoryStatusEx(&ms) ? static_cast<uint64_t>(ms.ullAvailPhys) : 0;
#else
    return 0;
#endif
}

double ToMb(uint64_t bytes) {
    return static_cast<double>(bytes) / 1048576.0;
}

int64_t UnixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

struct ExeInfo {
    std::string path;
    std::string commit;
    std::string version;
};
const ExeInfo& GetExeInfo() {
    static const ExeInfo info = [] {
        ExeInfo i;
        i.version = larecomp::kVersionLabel;
        std::filesystem::path exe;
#if defined(_WIN32)
        wchar_t buf[1024];
        const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
        if (n > 0 && n < sizeof(buf) / sizeof(buf[0])) exe = std::filesystem::path(std::wstring(buf, n));
#else
        std::error_code ec;
        exe = std::filesystem::read_symlink("/proc/self/exe", ec);
#endif
        if (!exe.empty()) {
            i.path = Utf8(exe);
            // The harness's build-set writes SET-SOURCE.txt (ref, sha, patches) next to the exe it makes.
            std::ifstream in(exe.parent_path() / "SET-SOURCE.txt", std::ios::binary);
            if (in) {
                const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                i.commit = core::ParseSetSourceSha(text);
            }
        }
        if (i.commit.empty()) {
            if (const char* env = std::getenv("LARECOMP_EXE_COMMIT")) i.commit = env;
        }
        return i;
    }();
    return info;
}

// The settings that differ from their defaults, sorted by name (a setting that holds a secret keeps its name and loses its value).
std::vector<std::pair<std::string, std::string>> SnapshotCvars() {
    std::vector<std::pair<std::string, std::string>> out;
    for (const std::string& name : rex::cvar::ListModifiedFlags()) {
        const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(name);
        if (!info || info->type == rex::cvar::FlagType::Command) continue;
        std::string value = core::IsSecretCvarName(name) ? std::string("(not recorded)") : rex::cvar::GetFlagByName(name);
        if (value.size() > 400) value.resize(400);
        out.emplace_back(name, std::move(value));
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// the files and the summary
// ---------------------------------------------------------------------------------------------------------------------
// One row of the state log, at the hand-over of take frame `h` (game thread, between two updates, the render thread idle): the game as the
// frame shows it. The player's car is the one the scene runner reads (SampleGameState); what cannot be read stays 0.
// CHECK: the guest addresses are the ones scene_runtime.cpp and map_warp.cpp use; no RNG state or traffic is in the row (their addresses are not
// known to this package), so a take that matches in this log matches in the player's car and the clock, not necessarily in the traffic.
void WriteStateRow(const core::HandOver& h) {
    if (!g_state_file) return;
    core::StateRow row;
    row.frame = g_handover.Handed() - 1;  // Note counted this frame already
    row.update = core::TagUpdate(h.tag);
    const SceneState state = CurrentSceneState();
    row.scene_t = (state == SceneState::kRunning || state == SceneState::kFinished) ? SceneTime() : -1.0;
    GameStateSample s;
    if (SampleGameState(&s)) {
        row.hour = s.hour;
        row.time_scale = s.time_scale;
        row.dt_ms = s.dt * 1000.0f;
        row.dt_unscaled_ms = s.dt_unscaled * 1000.0f;
        row.paused = s.paused;
        row.world_ready = s.world_ready;
        row.player = s.player;
        if (s.has_matrix) {
            row.x = s.matrix[12];
            row.y = s.matrix[13];
            row.z = s.matrix[14];
            row.matrix_hash = core::Fnv1a64(s.matrix, sizeof(s.matrix));
        }
    }
    const std::string text = core::StateCsvRow(row);
    std::fwrite(text.data(), 1, text.size(), g_state_file);
    ++g_state_rows;
}

// How a take ended; the running take.json has the defaults.
struct Outcome {
    std::string state = "running";
    std::string error;
    std::string finished_utc;
    double wall_s = 0.0;
    core::Verdict verdict;
    uint64_t end_update = 0;
    uint64_t handed = 0;
    core::GateBook gate;
    uint64_t output_bytes = 0;
    uint64_t output_files = 0;
    int ffmpeg_exit = -2;
    double ram_end_mb = -1.0;
};

const char* OutputName(core::SinkKind kind) {
    switch (kind) {
        case core::SinkKind::kFfmpeg: return core::kVideoMkvName;
        case core::SinkKind::kRaw: return core::kVideoRawName;
        case core::SinkKind::kPng: return core::kFramePatternName;
        case core::SinkKind::kNone: break;
    }
    return "";
}

// frames.csv: one row for every frame of the take (the frame/update map and what each frame cost).
void WriteFramesCsv(const Take& t) {
    std::string csv = "frame,update,scene_t,t_ms,game_wait_ms,record_ms,gpu_ms,copy_ms,stall_ms,write_ms,ring_fill,vram_mb,lost\n";
    char line[320];
    for (size_t k = 0; k < t.rows.size(); ++k) {
        const Row& r = t.rows[k];
        const double gate = k < t.gate_ms.size() ? t.gate_ms[k] : -1.0;
        const double scene_t = t.scene_running ? t.first_scene_t + static_cast<double>(k) * t.step_ms / 1000.0 : -1.0;
        std::snprintf(line, sizeof line, "%zu,%llu,%.6f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%.1f,%d\n", k, static_cast<unsigned long long>(r.update),
                      scene_t, r.t_ms, gate, r.c.record_ms, r.c.gpu_ms, r.c.copy_ms, r.c.stall_ms, r.write_ms, r.ring_fill, r.c.vram_mb,
                      r.lost ? 1 : 0);
        csv += line;
    }
    if (!WriteTextFile(t.layout.frames_csv, csv)) LARECOMP_APP_ERROR("[cap] cannot write {}", t.layout.frames_csv);
}

// take.json, at the start of a take (state "running") and at its end. The caller holds g_rows_mutex.
void WriteTakeJson(const Take& t, const Outcome& o) {
    core::TakeReport r;
    r.state = o.state;
    r.error = o.error;
    r.name = t.name;
    r.started_utc = t.started_utc;
    r.finished_utc = o.finished_utc;
    r.wall_s = o.wall_s;
    r.has_scene = t.scene_running;
    r.scene_name = t.scene_name;
    r.scene_source = t.scene_source;
    r.scene_copy = t.scene_fnv.empty() ? std::string() : std::string(core::kSceneCopyName);
    r.scene_fnv64 = t.scene_fnv;
    r.first_scene_t = t.first_scene_t;
    const ExeInfo& exe = GetExeInfo();
    r.exe_path = exe.path;
    r.exe_commit = exe.commit;
    r.version = exe.version;
    r.width = t.format.width;
    r.height = t.format.height;
    r.pixel_format = t.format.bgra ? "bgra8" : "rgba8";
    r.fps = t.fps;
    r.step_ticks = t.ticks;
    r.step_ms = t.step_ms;
    r.sink = core::SinkKindName(t.sink);
    r.output = OutputName(t.sink);
    r.output_bytes = o.output_bytes;
    r.output_files = o.output_files;
    r.ffmpeg_exit = o.ffmpeg_exit;
    r.ffmpeg_command = t.ffmpeg_command;
    r.first_update = t.first_update;
    r.last_update = t.rows.empty() ? t.first_update : t.rows.back().update;
    r.updates_run = t.book.UpdatesRun(o.end_update);
    r.frames = t.rows.size();
    r.lost = t.book.Lost();
    r.dropped = o.verdict.dropped;
    r.repeated = o.verdict.repeated;
    r.out_of_order = t.book.Anomalies();
    r.skipped = t.book.Skipped();
    r.handed = o.handed;
    r.exact = o.verdict.exact;
    r.verdict = o.verdict.text;
    r.gate_timeouts = o.gate.timeouts;
    r.gate_off = o.gate.off;
    r.lag = t.lag;
    r.ring = t.plan_made ? t.plan.ring : 0;
    r.gpu_buffers = t.plan_made ? t.plan.gpu_buffers : 0;
    r.ring_max_fill = t.ring_max;
    r.ring_stalls = t.ring_stalls;
    r.memory_plan_bytes = t.plan_made ? t.plan.total_bytes : 0;
    r.memory_cap_bytes = t.cap_bytes;
    r.memory_note = t.plan_made ? t.plan.text : std::string();
    r.ram_free_start_mb = t.ram_start_mb;
    r.ram_free_min_mb = t.ram_min_mb;
    r.ram_free_end_mb = o.ram_end_mb;
    if (t.state_log) {
        r.state_log = core::kStateCsvName;
        r.state_rows = g_state_rows;
    }
    r.cvars = t.cvars;
    if (!WriteTextFile(t.layout.take_json, core::BuildTakeJson(r))) LARECOMP_APP_ERROR("[cap] cannot write {}", t.layout.take_json);
}

void LogSummary(const Take& t, double wall_s, const core::GateBook& gate, const core::Verdict& verdict) {
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
        wall_s > 0 ? video_s / wall_s : 0.0, t.format.width, t.format.height, core::SinkKindName(t.sink), t.plan_made ? t.plan.ring : 0, t.lag,
        t.virtual_clock ? "virtual" : "steady", core::FormatStat(core::StatOf(gw)), core::FormatStat(core::StatOf(record)),
        core::FormatStat(core::StatOf(gpu)), core::FormatStat(core::StatOf(copy)), core::FormatStat(core::StatOf(stall)),
        core::FormatStat(wr), wall_s > 0 ? bytes / wall_s / 1048576.0 : 0.0, write_mbs, t.ring_max, t.ring_stalls, gate.timeouts,
        gate.off ? 1 : 0, t.book.Anomalies(), g_handover.Repeats(), vmin, vmax, g_anchors.load(), g_anchor_restarts.load(),
        g_anchor_gap_restarts.load(), g_sink.ffmpeg_exit);
    LARECOMP_APP_INFO("[cap] memory: {} GPU readback + {} host buffers = {} (cap {}); free RAM MB at start/min = {:.0f}/{:.0f}; dropped={} repeated={}",
                      t.plan_made ? t.plan.gpu_buffers : 0, t.plan_made ? t.plan.ring : 0,
                      core::FormatMib(t.plan_made ? t.plan.total_bytes : 0), core::FormatMib(t.cap_bytes), t.ram_start_mb, t.ram_min_mb,
                      verdict.dropped, verdict.repeated);
}

// The costs of take frame `k`, one line.
void LogFrame(const Take& t, size_t k) {
    std::lock_guard<std::mutex> lock(g_rows_mutex);
    if (k >= t.rows.size()) return;
    const Row& r = t.rows[k];
    const double gate = k < t.gate_ms.size() ? t.gate_ms[k] : 0.0;
    LARECOMP_APP_INFO("[cap] f={} u={} t={:.0f}ms game_wait={:.1f} record={:.2f} gpu_wait={:.1f} copy={:.1f} stall={:.1f} write={:.1f} ring={}/{} "
                      "vram={:.0f}MB",
                      k, r.update, r.t_ms, gate, r.c.record_ms, r.c.gpu_ms, r.c.copy_ms, r.c.stall_ms, r.write_ms, r.ring_fill,
                      t.plan_made ? t.plan.ring : t.ring, r.c.vram_mb);
}

// A copy of the scene file in the take folder, and its hash for take.json.
void CopySceneFile(Take& t) {
    const std::string src = SceneFilePath();
    if (src.empty()) return;
    std::ifstream in(Path(src), std::ios::binary);
    if (!in) {
        LARECOMP_APP_WARN("[cap] cannot read the scene file {} to copy it into the take folder", src);
        return;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::error_code ec;
    const std::filesystem::path abs = std::filesystem::absolute(Path(src), ec);
    t.scene_source = ec ? src : Utf8(abs);
    t.scene_name = SceneName();
    if (!WriteTextFile(t.layout.scene_copy, text)) {
        LARECOMP_APP_WARN("[cap] cannot write {}", t.layout.scene_copy);
        return;
    }
    t.scene_fnv = core::Hex64(core::Fnv1a64(text.data(), text.size()));
}

// The take's folder: mcla_capture_out exactly, else <mcla_capture_dir>/<name> with the first free name. False: `why` says what is wrong, and no
// take starts.
bool MakeTakeFolder(Take& t, const CaptureSpec& spec, std::string* why) {
    std::error_code ec;
    std::string folder;
    const std::string& out = REXCVAR_GET(mcla_capture_out);
    if (!out.empty()) {
        folder = out;
        while (folder.size() > 1 && (folder.back() == '/' || folder.back() == '\\')) folder.pop_back();
        if (std::filesystem::exists(Path(folder), ec)) {
            if (!std::filesystem::is_directory(Path(folder), ec)) {
                *why = folder + " (mcla_capture_out) is a file, not a folder";
                return false;
            }
            if (!std::filesystem::is_empty(Path(folder), ec)) {
                *why = "the take folder " + folder + " (mcla_capture_out) is not empty: a take never overwrites another";
                return false;
            }
        }
        t.name = Utf8(Path(folder).filename());
        if (t.name.empty()) t.name = "take";
    } else {
        const std::string& root = REXCVAR_GET(mcla_capture_dir);
        const std::string wanted = !spec.name.empty() ? spec.name : REXCVAR_GET(mcla_capture_name);
        const std::string base = core::SanitizeTakeName(wanted, SceneName());
        const std::string name = core::UniqueName(base, [&](const std::string& n) {
            std::error_code e2;
            return std::filesystem::exists(Path(core::JoinPath(root, n)), e2);
        });
        if (name.empty()) {
            *why = "999 take folders named " + base + " already exist in " + root;
            return false;
        }
        t.name = name;
        folder = core::JoinPath(root, name);
    }
    std::filesystem::create_directories(Path(folder), ec);
    if (ec) {
        *why = "cannot make the take folder " + folder + ": " + ec.message();
        return false;
    }
    const std::filesystem::path abs = std::filesystem::absolute(Path(folder), ec);
    t.layout = core::MakeLayout(!ec && !abs.empty() ? Utf8(abs) : folder);
    return true;
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

// Waits until `needed` take frames have been read back or reported lost. False: `timeout_ms` went by with nothing moving (no frame read back,
// lost or written): a slow writer that keeps finishing frames is not a timeout, however long a frame takes.
bool WaitForFrames(uint64_t needed, int timeout_ms) {
    auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    uint64_t seen = g_progress.load(std::memory_order_relaxed);
    std::unique_lock<std::mutex> lock(g_gate_mutex);
    while (!g_gate.Met(needed)) {
        const auto now = Clock::now();
        const uint64_t p = g_progress.load(std::memory_order_relaxed);
        if (p != seen) {
            seen = p;
            deadline = now + std::chrono::milliseconds(timeout_ms);
        }
        if (now >= deadline) return false;
        g_gate_cv.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(200)));
    }
    return true;
}

// A take that cannot start: nothing runs, and the scene's request is not taken up again until it has gone to 0.
void RefuseTake(const std::string& why) {
    LARECOMP_APP_ERROR("[cap] TAKE REFUSED: {}", why);
    g_refused = true;
}

void BeginTake(uint64_t u0, double fps) {
    Take& t = g_take;
    {
        std::lock_guard<std::mutex> lock(g_rows_mutex);
        t = Take{};
    }
    g_take_failed.store(false, std::memory_order_release);
    g_sink_stuck.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(g_fail_mutex);
        g_fail_reason.clear();
    }
    CaptureSpec spec;
    {
        std::lock_guard<std::mutex> lock(g_spec_mutex);
        spec = g_spec;
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
    t.cap_bytes = static_cast<uint64_t>(std::max(64, REXCVAR_GET(mcla_capture_memory_mb))) << 20;
    t.floor_bytes = static_cast<uint64_t>(std::max(0, REXCVAR_GET(mcla_capture_ram_floor_mb))) << 20;
    const std::string& sink_text = !spec.sink.empty() ? spec.sink : REXCVAR_GET(mcla_capture_sink);
    if (!core::ParseSinkKind(sink_text, &t.sink)) {
        LARECOMP_APP_ERROR("[cap] the sink '{}' is none, ffmpeg, png or raw: using none", sink_text);
    }
    std::string why;
    if (!MakeTakeFolder(t, spec, &why)) {
        RefuseTake(why);
        return;
    }
    t.started_utc = core::FormatUtc(UnixNow());
    CopySceneFile(t);
    t.cvars = SnapshotCvars();
    t.state_log = spec.state_log || REXCVAR_GET(mcla_capture_statelog);
    g_state_rows = 0;
    if (t.state_log) {
        g_state_file = OpenWrite(t.layout.state_csv);
        if (g_state_file) {
            const std::string header = core::StateCsvHeader();
            std::fwrite(header.data(), 1, header.size(), g_state_file);
        } else {
            LARECOMP_APP_WARN("[cap] cannot open {}: no state log in this take", t.layout.state_csv);
            t.state_log = false;
        }
    }
    if (const uint64_t avail = AvailablePhysicalBytes()) t.ram_start_mb = t.ram_min_mb = ToMb(avail);
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
    g_state_log_on.store(t.state_log, std::memory_order_release);
    g_take_active.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(g_rows_mutex);
        WriteTakeJson(t, Outcome{});
    }
    LARECOMP_APP_INFO("[cap] take starts at update {} ({:g} fps, step {:.4f} ms = {} ticks, sink {}, folder {}, ring {} asked, lag {}, virtual clock {}{}{})", u0,
                      fps, t.step_ms, t.ticks, core::SinkKindName(t.sink), t.layout.folder, t.ring, t.lag, t.virtual_clock ? "on" : "off",
                      t.scene_running ? std::string(", scene t of the first frame ") + std::to_string(t.first_scene_t) : std::string(),
                      t.state_log ? ", state log on" : "");
}

void EndTake(uint64_t u_end) {
    Take& t = g_take;
    Outcome o;
    o.wall_s = std::chrono::duration<double>(Clock::now() - t.t0).count();
    o.end_update = u_end;
    o.handed = g_handover.Handed();
    g_take_active.store(false, std::memory_order_release);
    g_state_log_on.store(false, std::memory_order_release);
    CloseSink();
    if (g_state_file) {
        std::fclose(g_state_file);
        g_state_file = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(g_gate_mutex);
        o.gate = g_gate;
    }
    if (const uint64_t avail = AvailablePhysicalBytes()) o.ram_end_mb = ToMb(avail);
    o.ffmpeg_exit = g_sink.ffmpeg_exit;
    o.finished_utc = core::FormatUtc(UnixNow());
    std::error_code ec;
    switch (t.sink) {
        case core::SinkKind::kFfmpeg:
            o.output_bytes = std::filesystem::exists(Path(t.layout.video_mkv), ec) ? std::filesystem::file_size(Path(t.layout.video_mkv), ec) : 0;
            break;
        case core::SinkKind::kRaw:
            o.output_bytes = std::filesystem::exists(Path(t.layout.video_raw), ec) ? std::filesystem::file_size(Path(t.layout.video_raw), ec) : 0;
            break;
        case core::SinkKind::kPng:
            if (std::filesystem::is_directory(Path(t.layout.frames_dir), ec)) {
                for (const auto& entry : std::filesystem::directory_iterator(Path(t.layout.frames_dir), ec)) {
                    if (entry.is_regular_file(ec) && entry.path().extension() == ".png") {
                        ++o.output_files;
                        o.output_bytes += entry.file_size(ec);
                    }
                }
            }
            break;
        case core::SinkKind::kNone:
            break;
    }
    {
        std::lock_guard<std::mutex> lock(g_rows_mutex);
        o.verdict = core::Judge(t.book, u_end, o.handed);
        if (o.verdict.exact) {
            LARECOMP_APP_INFO("[cap] frame count: {}", o.verdict.text);
        } else {
            LARECOMP_APP_ERROR("[cap] FRAME COUNT: {}", o.verdict.text);
        }
        // what the take is, in one word: failed when it could not run or the output is not what the frames say
        std::string error = FailReason();
        if (error.empty() && core::SinkUsesFfmpeg(t.sink) && o.ffmpeg_exit != 0) {
            error = "ffmpeg exited with code " + std::to_string(o.ffmpeg_exit) + " (see ffmpeg.log in the take folder)";
        }
        const uint64_t good = t.rows.size() - t.book.Lost();
        if (error.empty() && t.sink == core::SinkKind::kPng && o.output_files != good) {
            error = std::to_string(o.output_files) + " pictures in frames/ for " + std::to_string(good) + " frames read back";
        }
        if (error.empty() && t.sink == core::SinkKind::kRaw && o.output_bytes != good * g_sink.frame_bytes) {
            error = "video.bgra holds " + std::to_string(o.output_bytes) + " bytes for " + std::to_string(good) + " frames of " +
                    std::to_string(g_sink.frame_bytes);
        }
        if (error.empty() && t.rows.empty() && t.book.UpdatesRun(u_end) > 0) error = "no frame was read back";
        o.error = error;
        o.state = core::FinalState(o.verdict.exact, !error.empty());
        if (!error.empty()) LARECOMP_APP_ERROR("[cap] the take is FAILED: {}", error);
        if (g_take_failed.load(std::memory_order_acquire)) g_refused = true;  // the scene's request has to go to 0 before another take is tried
        WriteFramesCsv(t);
        WriteTakeJson(t, o);
        LogSummary(t, o.wall_s, o.gate, o.verdict);
    }
    LARECOMP_APP_INFO("[cap] take folder: {} (state {}, {} frames, dropped {}, repeated {})", t.layout.folder, o.state, t.rows.size(), o.verdict.dropped,
                      o.verdict.repeated);
}

// The gate of update `u` while a take runs (game thread).
void GateInTake(uint64_t u, double want_fps) {
    Take& t = g_take;
    // free memory, every 15th update: a take that sees the PC run out of it stops with an error instead of dragging everything down
    if (t.floor_bytes > 0 && (u - t.first_update) % 15 == 0) {
        if (const uint64_t avail = AvailablePhysicalBytes()) {
            const double mb = ToMb(avail);
            if (t.ram_min_mb < 0.0 || mb < t.ram_min_mb) t.ram_min_mb = mb;
            if (core::MemoryFloorHit(avail, t.floor_bytes)) {
                TakeFail("free physical memory fell to " + core::FormatMib(avail) + ", below mcla_capture_ram_floor_mb (" +
                         core::FormatMib(t.floor_bytes) + "): the take stops here (a lower render scale, or close other programs)");
            }
        }
    }
    const bool failed = g_take_failed.load(std::memory_order_acquire);
    const bool ending = !(want_fps > 0.0) || failed;
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
        LARECOMP_APP_ERROR("[cap] gate timeout ({} ms with nothing moving) at update {}: {} of {} take frames read back; books set straight (timeout {} in a row)",
                           timeout_ms, u, captured, needed, in_a_row);
        if (now_off) {
            LARECOMP_APP_ERROR("[cap] three gate timeouts in a row: the game is not held at the gate for the rest of the take");
            g_sink_stuck.store(true, std::memory_order_release);
        }
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
        if (g_refused) {
            // A take failed or was refused: the request that started it has to go away before another take is tried (the log has said why).
            if (!(want > 0.0)) g_refused = false;
        } else if (want > 0.0) {
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
    // CHECK: that the render thread is idle here and has swapped the frame before. frame_pacing.cpp's header says so of sub_82305B38 (read in
    // the generated code: the hand-over waits for "render done", which the render thread signals after D3DDevice_Swap, then signals it again);
    // nothing has run it. If a swap ever arrives with the stamp of the frame after it, every take shows "[cap] TAG" lines.
    if (!g_take_active.load(std::memory_order_acquire)) return;
    const core::HandOver h = g_handover.Note(g_updates.load(std::memory_order_relaxed), true, g_take_first.load(std::memory_order_acquire));
    g_stamp.store(h.tag, std::memory_order_release);
    if (h.take_frame && g_state_log_on.load(std::memory_order_acquire)) WriteStateRow(h);
}

void CaptureOnSwap() {
    // This runs on the guest's render thread BEFORE D3DDevice_Swap writes the present record (gpu.h says so of SetNextPresentTarget, which
    // frame pacing sets from the same hook): a tag that arrived after the record would be dropped. The stamp was set by the hand-over of
    // this very frame, while this thread was idle, so it can neither be older nor newer than the frame.
    // CHECK: that Patch_60FPS_Byte (sub_824199B0's swap-interval patch) runs on every swap before the present record is written; the pacer
    // relies on the same, and E7's CaptureOnSwap ran from here, but a swap that does not pass through it is a take frame with no tag (lost).
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

void CaptureSceneSpec(const CaptureSpec& spec) {
    std::lock_guard<std::mutex> lock(g_spec_mutex);
    g_spec = spec;
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

MemoryGrant CaptureMemoryPlan(uint32_t width, uint32_t height, uint32_t gpu_max, uint32_t gpu_min) {
    MemoryGrant grant;
    if (!g_take_active.load(std::memory_order_acquire) || g_take_failed.load(std::memory_order_acquire)) return grant;
    Take& t = g_take;
    if (!t.plan_made) {
        core::MemoryRequest r;
        r.width = width;
        r.height = height;
        r.gpu_max = gpu_max;
        r.gpu_min = gpu_min;
        r.ring_wanted = static_cast<uint32_t>(std::clamp(t.ring, 2, 8));
        r.ring_min = 2;
        r.cap_bytes = t.cap_bytes;
        r.available_bytes = AvailablePhysicalBytes();
        r.reserve_bytes = t.floor_bytes;
        r.fixed_bytes = core::SinkUsesFfmpeg(t.sink) ? (4ull << 20) : 0;  // the pipe's buffer (Pipe::Start)
        const core::MemoryPlan plan = core::PlanMemory(r);
        {
            std::lock_guard<std::mutex> lock(g_rows_mutex);
            t.plan = plan;
            t.plan_made = true;
            t.plan_width = width;
            t.plan_height = height;
        }
        if (plan.ok) {
            LARECOMP_APP_INFO("[cap] memory plan for {}x{}: {} GPU readback + {} host buffers = {} (cap {}, {}){}", width, height, plan.gpu_buffers,
                              plan.ring, core::FormatMib(plan.total_bytes), core::FormatMib(t.cap_bytes),
                              r.available_bytes ? "free RAM " + core::FormatMib(r.available_bytes) + ", " + core::FormatMib(t.floor_bytes) + " to stay free"
                                                : std::string("free RAM not known"),
                              plan.text.empty() ? std::string() : "; " + plan.text);
        } else {
            TakeFail(plan.text);
        }
    } else if (width != t.plan_width || height != t.plan_height) {
        TakeFail("the picture size changed during the take (" + std::to_string(t.plan_width) + "x" + std::to_string(t.plan_height) + " -> " +
                 std::to_string(width) + "x" + std::to_string(height) + "): a take keeps one size (a window resize or a render scale change ends it)");
        return grant;
    }
    grant.ok = t.plan.ok;
    grant.gpu_buffers = t.plan.gpu_buffers;
    grant.ring = t.plan.ring;
    return grant;
}

bool CaptureSinkOpen(const FrameFormat& format) {
    if (!g_take_active.load(std::memory_order_acquire) || g_take_failed.load(std::memory_order_acquire)) return false;
    Sink& s = g_sink;
    if (s.open.load(std::memory_order_acquire)) {
        if (s.format.width != format.width || s.format.height != format.height) {
            TakeFail("the frame size changed during the take (" + std::to_string(s.format.width) + "x" + std::to_string(s.format.height) + " -> " +
                     std::to_string(format.width) + "x" + std::to_string(format.height) + ")");
            return false;
        }
        return true;
    }
    g_take.format = format;
    return OpenSink(format);
}

HostBuffer CaptureAcquire(double* stall_ms) {
    Sink& s = g_sink;
    HostBuffer b;
    if (stall_ms) *stall_ms = 0;
    if (!s.open.load(std::memory_order_acquire)) return b;
    // Waits while every buffer is queued or being written: the back-pressure. It gives up only when the writer has finished nothing for the
    // gate's timeout (ffmpeg stuck, a disk that stopped taking data): the take then ends with an error, it never queues beyond its ring.
    double waited = 0.0;
    bool stuck = false;
    const core::HostRing::Slot slot = s.ring.Acquire(REXCVAR_GET(mcla_capture_gate_timeout_ms), &waited, &stuck);
    if (stall_ms) *stall_ms = waited;
    if (waited > 1.0) {
        std::lock_guard<std::mutex> rows(g_rows_mutex);
        ++g_take.ring_stalls;
    }
    if (!slot.data) {
        if (stuck) {
            g_sink_stuck.store(true, std::memory_order_release);
            TakeFail("the writer freed no buffer for " + std::to_string(REXCVAR_GET(mcla_capture_gate_timeout_ms) / 1000) +
                     " s: ffmpeg or the disk has stopped taking frames (see ffmpeg.log in the take folder)");
        }
        return b;
    }
    b.data = slot.data;
    b.slot = slot.index;
    b.bytes = slot.bytes;
    return b;
}

void CaptureFrameDone(const HostBuffer& buffer, const FrameCosts& costs) {
    Sink& s = g_sink;
    uint64_t row_index = 0;
    const uint32_t fill = s.ring.queued() + 1;
    {
        std::lock_guard<std::mutex> lock(g_rows_mutex);
        Take& t = g_take;
        Row r;
        r.update = costs.update;
        r.c = costs;
        r.t_ms = std::chrono::duration<double, std::milli>(Clock::now() - t.t0).count();
        r.ring_fill = fill;
        t.ring_max = std::max(t.ring_max, fill);
        t.lost_in_a_row = 0;
        row_index = t.rows.size();
        // the stamp is the update: the take's frames must be consecutive updates from the first
        const uint64_t want = t.rows.empty() ? t.first_update : t.rows.back().update + 1;
        if (!t.book.OnFrame(costs.update, false) && t.book.Anomalies() <= 8) {
            LARECOMP_APP_ERROR("[cap] TAG: take frame {} carries update {}, expected {} ({})", row_index, costs.update, want,
                               costs.update < want ? "a repeated frame" : "a frame is missing");
        }
        t.rows.push_back(r);
    }
    s.ring.Submit(core::HostRing::Slot{buffer.data, buffer.slot, buffer.bytes}, row_index);
    g_progress.fetch_add(1, std::memory_order_relaxed);
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
    uint32_t in_a_row = 0;
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
        in_a_row = ++t.lost_in_a_row;
    }
    g_progress.fetch_add(1, std::memory_order_relaxed);
    if (in_a_row <= 3 || in_a_row % 100 == 0) LARECOMP_APP_ERROR("[cap] frame of update {} lost: {}", update, why ? why : "?");
    // A take that cannot read frames back is not worth running to its end: ten in a row end it (the gate does, at its next update).
    if (in_a_row == 10) TakeFail(std::string("ten frames in a row could not be read back: ") + (why ? why : "?"));
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

void ApplierBegin(const Scene& scene) {
    CaptureSceneRequest(0.0);
    // what the `capture fps` key asked for besides the rate: read when the take starts
    CaptureSpec spec;
    spec.name = scene.capture_name;
    spec.sink = scene.capture_sink;
    spec.state_log = scene.capture_state_log;
    CaptureSceneSpec(spec);
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
void CaptureSceneSpec(const CaptureSpec&) {}
MemoryGrant CaptureMemoryPlan(uint32_t, uint32_t, uint32_t, uint32_t) { return MemoryGrant{}; }
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
