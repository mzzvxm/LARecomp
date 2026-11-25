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
//   mcla_capture_luma <crop>      for a PNG take, store a lossless luma stream instead (full or x,y,width,height). Empty: PNG.
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
// THE SOUND (M2c). The take's audio is capture_audio.h's: at the take's start (BeginTake) the game's mixer is taken over, at every gate
// (GateInTake) it is given the tickets that cover the video frames so far and its frames are written to the take folder's audio.wav, at the end
// (EndTake) the real output gets its mixer back. Nothing else of the take knows about it.
//
// Written without a build or a run. CHECK marks what rests on reading the code and the E7 log only.
//

#include "capture_mode.h"
#include "settings/immortal.h"

#include "settings/larecomp_settings.h"

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
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>

extern char** environ;
#endif

#include "capture_audio.h"
#include "capture_core.h"
#include "capture_luma.h"
#include "citypack/city_zstd.h"
#include "director/png_writer.h"
#include "larecomp_log.h"
#include "larecomp_version.h"
#include "scene_runtime.h"
#include "spots.h"
#include "take_folder.h"

#if defined(LARECOMP_HAS_GPU)
#include "gpu/gpu.h"
#include "gpu/sync_builds.h"
#endif


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
// The game is closing (CaptureCloseNow): the gate stops holding the game thread (the render thread that reads the frames back is going away, and a frame
// handed over before it stopped is never read back), and no take or still begins.
std::atomic<bool> g_closing{false};

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
    core::LumaCrop luma_crop;
    double fps = 60.0;
    uint64_t ticks = 0;
    double step_ms = 16.666;
    uint64_t first_update = 0;
    core::TakeSceneClock scene_clock;
    double end_scene_t = -1.0;  // the Director's exclusive end, when present
    double luma_scene_duration = -1.0;
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
    bool still = false;  // the Director's still (CaptureStillRequest): one frame, a PNG, no take folder
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
Take& g_take = larecomp::Leak<Take>();
std::mutex g_rows_mutex;
bool g_announced_no_renderer = false;

// What a scene's `capture fps` key asked for besides the rate (CaptureSceneSpec); read when a take starts.
std::mutex g_spec_mutex;
CaptureSpec& g_spec = larecomp::Leak<CaptureSpec>();

// The Director (capture_mode.h): a take it asks for (take.render), the still it asks for (shot), and what became of the takes.
std::atomic<double> g_dir_fps{0.0};  // the Director's request for a take (0: none); a scene's request outranks it
CaptureSpec& g_dir_spec = larecomp::Leak<CaptureSpec>();  // g_spec_mutex
std::mutex g_still_mutex;
bool g_still_pending = false;        // g_still_mutex: asked for, the next gate starts it
std::string& g_still_path = larecomp::Leak<std::string>();  // g_still_mutex
StillResult& g_still_res = larecomp::Leak<StillResult>();  // g_still_mutex: the result of the still that ran last (done when `done`)
std::atomic<bool> g_still_active{false};  // the running take is a still: no virtual clock
std::mutex g_last_mutex;
struct LastTake {
    std::string name, folder, state, error;
    double fps = 0.0;
    uint64_t frames = 0;
};
LastTake& g_last_status = larecomp::Leak<LastTake>();  // g_last_mutex: the take that ended last
std::atomic<uint64_t> g_takes_finished{0};

// A take that cannot go on (the memory plan does not fit, the writer is stuck, the free memory is gone): the first reason is kept, the game
// thread ends the take at its next gate, and the scene's request is not taken up again until it has gone to 0 (g_refused).
std::atomic<bool> g_take_failed{false};
std::atomic<bool> g_sink_stuck{false};  // the writer finished nothing for the gate's timeout: ending the take kills ffmpeg instead of waiting for it
std::mutex g_fail_mutex;
std::string& g_fail_reason = larecomp::Leak<std::string>();  // g_fail_mutex
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
        // The program runs through the shell (the command line is one string with its quotes), standard input the pipe, output
        // and errors the log. A write to a pipe whose reader is gone has to fail with EPIPE (the take then fails with
        // ffmpeg.log to read) and not end the game with SIGPIPE.
        signal(SIGPIPE, SIG_IGN);
        int fds[2] = {-1, -1};
        if (pipe(fds) != 0) return false;
        fcntl(fds[0], F_SETFD, FD_CLOEXEC);  // only the dup2'd standard input reaches the program, so it sees the end of the data
        fcntl(fds[1], F_SETFD, FD_CLOEXEC);
#if defined(F_SETPIPE_SZ)
        // As the 4 MiB of the Windows pipe, as far as the system allows (/proc/sys/fs/pipe-max-size, 1 MiB by default).
        if (fcntl(fds[1], F_SETPIPE_SZ, 4 << 20) < 0) fcntl(fds[1], F_SETPIPE_SZ, 1 << 20);
#endif
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, fds[0], 0);
        posix_spawn_file_actions_addopen(&actions, 1, log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        posix_spawn_file_actions_adddup2(&actions, 1, 2);
        const char* argv[] = {"sh", "-c", command_line.c_str(), nullptr};
        pid_t pid = 0;
        const int rc = posix_spawn(&pid, "/bin/sh", &actions, nullptr, const_cast<char* const*>(argv), environ);
        posix_spawn_file_actions_destroy(&actions);
        close(fds[0]);
        if (rc != 0) {
            close(fds[1]);
            return false;
        }
        pid_ = pid;
        fd_ = fds[1];
        return true;
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
        while (bytes > 0) {
            const ssize_t n = ::write(fd_, data, static_cast<size_t>(std::min<uint64_t>(bytes, 1u << 20)));
            if (n < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (n == 0) return false;
            data += n;
            bytes -= static_cast<uint64_t>(n);
        }
        return true;
#endif
    }

    bool started() const {
#if defined(_WIN32)
        return process_ != nullptr;
#else
        return pid_ > 0;
#endif
    }

    // Ends the program at once (a take that has failed): a write that is stuck on a pipe nobody reads returns, so the writer thread can finish.
    void Kill() {
#if defined(_WIN32)
        if (process_) TerminateProcess(process_, 1);
#else
        if (pid_ > 0) kill(pid_, SIGKILL);
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
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
        int code = -1;
        if (pid_ > 0) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
            for (;;) {
                int status = 0;
                const pid_t done = waitpid(pid_, &status, WNOHANG);
                if (done == pid_) {
                    code = WIFEXITED(status) ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
                    break;
                }
                if (done < 0 && errno != EINTR) break;  // not ours to wait for (a SIGCHLD that is ignored reaps it): the code is unknown
                if (std::chrono::steady_clock::now() >= deadline) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        pid_ = -1;
        return code;
#endif
    }

  private:
#if defined(_WIN32)
    HANDLE process_ = nullptr;
    HANDLE write_ = nullptr;
#else
    pid_t pid_ = -1;
    int fd_ = -1;
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
    core::LumaCrop luma_crop;
    std::vector<uint8_t> luma_previous, luma_delta, luma_compressed;
    uint64_t luma_frames = 0;
    core::SinkKind kind = core::SinkKind::kNone;
    std::atomic<uint64_t> bytes_written{0};
    int ffmpeg_exit = -2;
};
Sink& g_sink = *new Sink;  // never destroyed: the writer thread ends with its take, not with the process

bool WriteLumaWord(std::FILE* file, uint32_t value) {
    const auto word = core::LumaWord(value);
    return file && std::fwrite(word.data(), 1, word.size(), file) == word.size();
}

bool WriteLumaFrame(Sink& s, uint64_t row, const uint8_t* data) {
    if (!data || !s.raw || row != s.luma_frames) return false;
    const bool key = row % core::kLumaKeyInterval == 0;
    const auto predict = [&]<bool Bgra, bool Key>() {
        core::PredictLuma<Bgra, Key>(data, s.format.width, s.luma_crop, s.luma_previous.data(), s.luma_delta.data());
    };
    if (s.format.bgra) {
        if (key) predict.template operator()<true, true>();
        else predict.template operator()<true, false>();
    } else {
        if (key) predict.template operator()<false, true>();
        else predict.template operator()<false, false>();
    }
    if (!larecomp::citypack::ZstdCompress(s.luma_delta.data(), s.luma_delta.size(), 1, &s.luma_compressed) ||
        !WriteLumaWord(s.raw, static_cast<uint32_t>(s.luma_compressed.size())) ||
        std::fwrite(s.luma_compressed.data(), 1, s.luma_compressed.size(), s.raw) != s.luma_compressed.size()) return false;

    if (core::LumaPreview(row)) {
        // Keep the full view in three small color PNGs. Use the same channel order as the readback.
        constexpr uint32_t step = 3;
        const uint32_t w = (s.format.width + step - 1) / step, h = (s.format.height + step - 1) / step;
        std::vector<uint8_t> preview(size_t(w) * h * 4);
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const size_t pixel = size_t(y * step) * s.format.width + x * step;
                std::memcpy(preview.data() + (size_t(y) * w + x) * 4, data + pixel * 4, 4);
            }
        }
        const auto path = core::JoinPath(g_take.layout.frames_dir, core::FrameFileName(row));
        std::string error;
        if (!director::WritePngFile(path, preview.data(), w, h, 0, s.format.bgra, &error)) {
            TakeFail("cannot write the luma preview: " + error);
            return false;
        }
    }
    ++s.luma_frames;
    return true;
}

// The host ring's writer thread calls this for every frame, in order. Once a write has failed the frames are counted and dropped (the take
// ends in error at its next gate, TakeFail).
bool WriteFrame(uint64_t row, const uint8_t* data, uint64_t bytes) {
    Sink& s = g_sink;
    const auto t0 = Clock::now();
    if (g_take.still) {
        // The Director's still: the one frame is a PNG. g_take is not changed while the writer runs (a take is begun when none runs); the game thread waits for
        // this frame at the gate.
        std::string path;
        {
            std::lock_guard<std::mutex> lock(g_still_mutex);
            path = g_still_path;
        }
        std::string err;
        const bool ok = director::WritePngFile(path, data, s.format.width, s.format.height, 0, s.format.bgra, &err);
        std::error_code ec;
        const uintmax_t size = ok ? std::filesystem::file_size(Path(path), ec) : 0;
        {
            std::lock_guard<std::mutex> lock(g_still_mutex);
            g_still_res.ok = ok;
            g_still_res.error = err;
            g_still_res.width = s.format.width;
            g_still_res.height = s.format.height;
            g_still_res.bytes = ec ? 0 : static_cast<uint64_t>(size);
        }
        if (!ok) TakeFail("the still could not be written: " + err);
        g_progress.fetch_add(1, std::memory_order_relaxed);
        return ok;
    }
    bool ok = !s.sink_failed.load(std::memory_order_relaxed);
    if (ok) {
        if (core::SinkUsesFfmpeg(s.kind)) {
            ok = s.pipe.Write(data, bytes);
        } else if (s.kind == core::SinkKind::kLuma) {
            ok = WriteLumaFrame(s, row, data);
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
    if (s.kind == core::SinkKind::kPng || s.kind == core::SinkKind::kLuma) {
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
        const std::string& program = larecomp::settings::mcla_capture_ffmpeg.Get();
        const std::string cmd = s.kind == core::SinkKind::kPng
                                    ? core::FfmpegPngCommandLine(program, f.width, f.height, f.bgra, t.fps, t.layout.frame_pattern)
                                    : core::FfmpegCommandLine(program, f.width, f.height, f.bgra, t.fps, larecomp::settings::mcla_capture_ffmpeg_args.Get(),
                                                              t.layout.video_mkv);
        t.ffmpeg_command = cmd;
        if (!s.pipe.Start(cmd, t.layout.ffmpeg_log)) {
            TakeFail("cannot start ffmpeg (mcla_capture_ffmpeg = " + program + "): " + cmd);
            return false;
        }
        LARECOMP_APP_INFO("[cap] ffmpeg: {}", cmd);
    } else if (s.kind == core::SinkKind::kLuma) {
        s.luma_crop = t.luma_crop.width ? t.luma_crop : core::LumaCrop{0, 0, f.width, f.height};
        if (!s.luma_crop.Fits(f.width, f.height)) {
            TakeFail("mcla_capture_luma crop is outside the readback");
            return false;
        }
        const auto path = core::JoinPath(t.layout.folder, core::kLumaName);
        s.raw = OpenWrite(path);
        const auto& crop = s.luma_crop;
        if (!s.raw || std::fwrite(core::kLumaMagic.data(), 1, core::kLumaMagic.size(), s.raw) != core::kLumaMagic.size() ||
            !WriteLumaWord(s.raw, crop.x) || !WriteLumaWord(s.raw, crop.y) ||
            !WriteLumaWord(s.raw, crop.width) || !WriteLumaWord(s.raw, crop.height)) {
            TakeFail("cannot write the header of " + path);
            return false;
        }
        s.luma_frames = 0;
        s.luma_previous.resize(crop.Pixels());
        s.luma_delta.resize(crop.Pixels());
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
        if (std::fclose(s.raw) != 0 && s.kind == core::SinkKind::kLuma) TakeFail("cannot finish writing video.luma");
        s.raw = nullptr;
    }
    if (s.kind == core::SinkKind::kLuma) {
        std::vector<uint8_t>().swap(s.luma_previous);
        std::vector<uint8_t>().swap(s.luma_delta);
        std::vector<uint8_t>().swap(s.luma_compressed);
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
    // MemAvailable: what a program can get without the system swapping (free memory and the caches it gives back).
    std::ifstream in("/proc/meminfo");
    std::string key;
    uint64_t kib = 0;
    std::string rest;
    while (in >> key >> kib) {
        std::getline(in, rest);
        if (key == "MemAvailable:") return kib * 1024;
    }
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
    static const ExeInfo& info = larecomp::Leak<const ExeInfo>([] {
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
    }());
    return info;
}

// The settings that differ from their defaults, sorted by name (a setting that holds a secret keeps its name and loses its value).
std::vector<std::pair<std::string, std::string>> SnapshotCvars() {
    std::vector<std::pair<std::string, std::string>> out;
    for (const std::string& name : larecomp::cvar::ListModifiedFlags()) {
        const larecomp::cvar::FlagEntry* info = larecomp::cvar::GetFlagInfo(name);
        if (!info || info->type == larecomp::cvar::FlagType::Command) continue;
        std::string value = core::IsSecretCvarName(name) ? std::string("(not recorded)") : larecomp::cvar::GetFlagByName(name);
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
        case core::SinkKind::kLuma: return core::kLumaName;
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
        const double scene_t = t.scene_running ? t.scene_clock.Time(r.update - t.first_update) : -1.0;
        std::snprintf(line, sizeof line, "%zu,%llu,%.17g,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%.1f,%d\n", k, static_cast<unsigned long long>(r.update),
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
    r.first_scene_t = t.scene_clock.t0;
    r.scene_step_s = t.scene_clock.step;
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
    // A scene that names places (`spot:<name>`) is run from the take folder with the places it had: the spots files it read, beside the copy (spots.h).
    if (text.find("spot:") != std::string::npos) {
        const std::string spots = SpotsTextFor(src);
        const std::string spots_copy = core::JoinPath(t.layout.folder, core::kSpotsCopyName);
        if (!spots.empty() && !WriteTextFile(spots_copy, spots)) LARECOMP_APP_WARN("[cap] cannot write {}", spots_copy);
    }
}

// The take's folder: mcla_capture_out exactly, else <mcla_capture_dir>/<name> with the first free name. False: `why` says what is wrong, and no
// take starts.
bool MakeTakeFolder(Take& t, const CaptureSpec& spec, std::string* why) {
    std::error_code ec;
    std::string folder;
    const std::string& out = larecomp::settings::mcla_capture_out.Get();
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
        const std::string& root = larecomp::settings::mcla_capture_dir.Get();
        const std::string wanted = !spec.name.empty() ? spec.name : larecomp::settings::mcla_capture_name.Get();
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
// The rate a take is asked for now: an explicit Director request, the scene, then the debug setting.
double WantedFps() {
    const double director = g_dir_fps.load(std::memory_order_relaxed);
    if (director > 0.0) return core::ClampFps(director);
    const double scene = g_scene_fps.load(std::memory_order_relaxed);
    if (scene > 0.0) return core::ClampFps(scene);
    return core::ClampFps(larecomp::settings::mcla_capture_fps.Get());
}

// Waits until `needed` take frames have been read back or reported lost, or the game is closing (capture_core.h WaitAtGate says why that ends it).
// kTimedOut: `timeout_ms` went by with nothing moving (no frame read back, lost or written): a slow writer that keeps finishing frames is not a
// timeout, however long a frame takes.
core::GateEnd WaitForFrames(uint64_t needed, int timeout_ms) {
    std::unique_lock<std::mutex> lock(g_gate_mutex);
    return core::WaitAtGate(
        lock, g_gate_cv, [&] { return g_gate.Met(needed); }, [] { return g_progress.load(std::memory_order_relaxed); },
        [] { return g_closing.load(std::memory_order_acquire); }, timeout_ms);
}

// A take that cannot start: nothing runs, and the scene's request is not taken up again until it has gone to 0.
void RefuseTake(const std::string& why) {
    LARECOMP_APP_ERROR("[cap] TAKE REFUSED: {}", why);
    g_refused = true;
    {
        std::lock_guard<std::mutex> lock(g_last_mutex);
        g_last_status = LastTake{};
        g_last_status.state = "failed";
        g_last_status.error = why;
    }
    g_takes_finished.fetch_add(1, std::memory_order_release);
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
        if (g_dir_fps.load(std::memory_order_relaxed) > 0.0) spec = g_dir_spec;
        else if (g_scene_fps.load(std::memory_order_relaxed) > 0.0) spec = g_spec;
    }
    t.fps = fps;
    t.ticks = core::StepTicksForFps(fps);
    const SceneTransport scene = GetSceneTransport();
    t.scene_running = scene.state == SceneState::kRunning;
    // Normally the runner visits the authored key exactly. If it was explicitly scrubbed past it, start after the scrub.
    const double request_time = spec.request_scene_t >= 0.0 && scene.t <= spec.request_scene_t + 1e-9 ? spec.request_scene_t : scene.t;
    t.scene_clock = core::PlanTakeSceneClock(request_time, fps, spec.t0);
    t.step_ms = t.scene_clock.step * 1000.0;
    t.end_scene_t = spec.end_scene_t;
    t.first_update = u0;
    t.virtual_clock = larecomp::settings::mcla_capture_virtual_clock.Get();
    t.lag = std::clamp<int>(larecomp::settings::mcla_capture_lag.Get(), 0, 2);
    t.ring = larecomp::settings::mcla_capture_ring.Get();
    if ((spec.t0 >= 0.0 && !t.scene_running) ||
        (t.scene_running && t.scene_clock.PastEnd(0, t.end_scene_t, scene.duration))) {
        RefuseTake("the take's first frame falls outside a running scene");
        return;
    }
    t.cap_bytes = static_cast<uint64_t>(std::max(64, larecomp::settings::mcla_capture_memory_mb.Get())) << 20;
    t.floor_bytes = static_cast<uint64_t>(std::max(0, larecomp::settings::mcla_capture_ram_floor_mb.Get())) << 20;
    const std::string& sink_text = !spec.sink.empty() ? spec.sink : larecomp::settings::mcla_capture_sink.Get();
    if (!core::ParseSinkKind(sink_text, &t.sink)) {
        LARECOMP_APP_ERROR("[cap] the sink '{}' is none, ffmpeg, png or raw: using none", sink_text);
    }
    const auto& luma = larecomp::settings::mcla_capture_luma.Get();
    if (t.sink == core::SinkKind::kPng && !luma.empty()) {
        if (!core::ParseLumaCrop(luma, t.luma_crop)) {
            RefuseTake("mcla_capture_luma needs full or x,y,width,height");
            return;
        }
        t.sink = core::SinkKind::kLuma;
        if (t.scene_running) t.luma_scene_duration = scene.duration;
    }
    std::string why;
    if (!MakeTakeFolder(t, spec, &why)) {
        RefuseTake(why);
        return;
    }
    t.started_utc = core::FormatUtc(UnixNow());
    CopySceneFile(t);
    t.cvars = SnapshotCvars();
    t.state_log = spec.state_log || larecomp::settings::mcla_capture_statelog.Get();
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
    // CHECK: on a live take the first video update owns the first WAV samples, with no pre-roll or drift.
    capture_audio::Begin(fps, t.layout.audio_wav, t.layout.audio_csv, u0);
    t.t0 = Clock::now();
    g_take_first.store(u0, std::memory_order_release);
    g_take_ticks.store(t.ticks, std::memory_order_release);
    g_state_log_on.store(t.state_log, std::memory_order_release);
    g_take_active.store(true, std::memory_order_release);
#if defined(LARECOMP_HAS_GPU)
    // No draw is left out for a pipeline that is still being made while the take runs (gpu/sync_builds.h, M2d): the renderer waits for it.
    ::mcla::gpu::sync_builds::SetCaptureActive(true);
#endif
    {
        std::lock_guard<std::mutex> lock(g_rows_mutex);
        WriteTakeJson(t, Outcome{});
    }
    LARECOMP_APP_INFO("[cap] take starts at update {} ({:g} fps, step {:.4f} ms = {} ticks, sink {}, folder {}, ring {} asked, lag {}, virtual clock {}{}{})", u0,
                      fps, t.step_ms, t.ticks, core::SinkKindName(t.sink), t.layout.folder, t.ring, t.lag, t.virtual_clock ? "on" : "off",
                      t.scene_running ? std::string(", scene t of the first frame ") + std::to_string(t.scene_clock.t0) : std::string(),
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
#if defined(LARECOMP_HAS_GPU)
    ::mcla::gpu::sync_builds::SetCaptureActive(false);  // the renderer may leave a draw out for a pending pipeline again (gpu/sync_builds.h)
#endif
    capture_audio::End(o.handed);  // the WAV is finished and the real output has its mixer back before the sink's flush
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
        case core::SinkKind::kLuma: {
            const auto bytes = std::filesystem::file_size(Path(core::JoinPath(t.layout.folder, core::kLumaName)), ec);
            if (!ec) {
                o.output_files = 1;
                o.output_bytes = bytes;
            }
            break;
        }
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
        if (error.empty() && t.sink == core::SinkKind::kLuma && (o.output_files != 1 || g_sink.luma_frames != good)) {
            error = "video.luma did not receive all " + std::to_string(good) + " frames read back";
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
    {
        std::lock_guard<std::mutex> lock(g_last_mutex);
        g_last_status = LastTake{};
        g_last_status.name = t.name;
        g_last_status.folder = t.layout.folder;
        g_last_status.fps = t.fps;
        g_last_status.frames = t.rows.size();
        g_last_status.state = o.state;
        g_last_status.error = o.error;
    }
    g_takes_finished.fetch_add(1, std::memory_order_release);
}

// ---------------------------------------------------------------------------------------------------------------------
// the Director's still: a take of one frame, with no folder, no audio and no virtual clock, that the writer turns into a PNG
// ---------------------------------------------------------------------------------------------------------------------
// The step of the still's one update: the fixed step already in force, else the scene's own rate, else 60.
double StillFps() {
    const double fixed = larecomp::settings::mcla_fixed_step_fps.Get();
    if (fixed > 0.0) return core::ClampFps(fixed);
    const double scene = g_scene_fps.load(std::memory_order_relaxed);
    if (scene > 0.0) return core::ClampFps(scene);
    return 60.0;
}

void BeginStill(uint64_t u0) {
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
    {
        std::lock_guard<std::mutex> lock(g_still_mutex);
        g_still_pending = false;
        g_still_res.done = false;
        g_still_res.ok = false;
        g_still_res.error.clear();
        g_still_res.width = g_still_res.height = 0;
        g_still_res.bytes = 0;
        g_still_res.update = u0;
        g_still_res.path = g_still_path;
    }
    t.still = true;
    t.sink = core::SinkKind::kNone;
    t.fps = StillFps();
    t.ticks = core::StepTicksForFps(t.fps);
    t.step_ms = 1000.0 / t.fps;
    t.first_update = u0;
    t.virtual_clock = false;
    t.lag = 0;
    t.ring = 2;
    t.scene_running = CurrentSceneState() == SceneState::kRunning;
    t.scene_clock.t0 = SceneTime() + t.step_ms / 1000.0;
    t.cap_bytes = static_cast<uint64_t>(std::max(64, larecomp::settings::mcla_capture_memory_mb.Get())) << 20;
    t.floor_bytes = 0;
    t.book.Begin(u0);
    {
        std::lock_guard<std::mutex> lock(g_gate_mutex);
        g_gate = core::GateBook{};
    }
    g_handover.BeginTake();
    g_sink.ffmpeg_exit = -2;
    g_sink.frame_bytes = 0;
    t.t0 = Clock::now();
    g_take_first.store(u0, std::memory_order_release);
    g_take_ticks.store(t.ticks, std::memory_order_release);
    g_state_log_on.store(false, std::memory_order_release);
    g_still_active.store(true, std::memory_order_release);
    g_take_active.store(true, std::memory_order_release);
#if defined(LARECOMP_HAS_GPU)
    ::mcla::gpu::sync_builds::SetCaptureActive(true);
#endif
    LARECOMP_APP_INFO("[cap] still starts at update {} (step {:.4f} ms, scene t of the frame {:.4f}): {}", u0, t.step_ms, t.scene_clock.t0, g_still_res.path);
}

void EndStill(uint64_t u_end) {
    Take& t = g_take;
    g_take_active.store(false, std::memory_order_release);
#if defined(LARECOMP_HAS_GPU)
    ::mcla::gpu::sync_builds::SetCaptureActive(false);
#endif
    CloseSink();  // the writer finishes the PNG before this returns
    g_still_active.store(false, std::memory_order_release);
    const std::string reason = FailReason();
    {
        std::lock_guard<std::mutex> lock(g_still_mutex);
        g_still_res.done = true;
        g_still_res.update = t.first_update;
        g_still_res.scene_t = t.scene_running ? t.scene_clock.t0 : -1.0;
        if (!g_still_res.ok && g_still_res.error.empty()) {
            g_still_res.error = !reason.empty() ? reason : std::string("the frame was not read back");
        }
        if (g_still_res.ok) {
            LARECOMP_APP_INFO("[cap] still done: {} ({}x{}, {} bytes, update {})", g_still_res.path, g_still_res.width, g_still_res.height, g_still_res.bytes, t.first_update);
        } else {
            LARECOMP_APP_ERROR("[cap] STILL FAILED: {} ({})", g_still_res.path, g_still_res.error);
        }
    }
    if (g_take_failed.load(std::memory_order_acquire)) g_refused = false;  // a failed still is not a refused take request
    (void)u_end;
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
    const double duration = t.scene_running ? GetSceneTransport().duration : -1.0;
    const bool at_end = t.scene_running && t.scene_clock.PastEnd(u - t.first_update, t.end_scene_t, duration);
    const bool ending = t.still || !(want_fps > 0.0) || failed || at_end;
    if (!ending && std::fabs(want_fps - t.fps) > 1e-9 && !t.announced_rate_change) {
        t.announced_rate_change = true;  // a take keeps its rate: the log says so once
        LARECOMP_APP_WARN("[cap] a take of {:g} fps is running: the request for {:g} is ignored until it ends", t.fps, want_fps);
    }
    const uint64_t handed = g_handover.Handed();
    const uint64_t needed = core::FramesNeeded(handed, static_cast<uint32_t>(t.lag), ending);
    const int timeout_ms = larecomp::settings::mcla_capture_gate_timeout_ms.Get();
    if (!t.still) capture_audio::StepBegin(handed);  // the mixer runs for the frames so far while the readback is waited for
    const auto w0 = Clock::now();
    bool off;
    {
        std::lock_guard<std::mutex> lock(g_gate_mutex);
        off = g_gate.off;
    }
    core::GateEnd end = core::GateEnd::kMet;
    if (!off) end = WaitForFrames(needed, timeout_ms);
    const bool ok = end != core::GateEnd::kTimedOut;
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
    } else if (end == core::GateEnd::kMet) {  // a gate left because the game is closing is neither met nor timed out: the books stay as they are
        std::lock_guard<std::mutex> lock(g_gate_mutex);
        g_gate.NoteMet();
    }
    if (!t.still) capture_audio::StepEnd();  // the mixer's frames are in, the video frames' samples are in the WAV
    t.gate_ms.push_back(wait_ms);
    if (const int every = larecomp::settings::mcla_capture_log_every.Get(); every > 0 && !t.gate_ms.empty()) {
        const uint64_t k_prev = t.gate_ms.size() - 1;
        if (k_prev % uint64_t(every) == 0 || k_prev < 3) LogFrame(t, static_cast<size_t>(k_prev));
    }
    if (ending) {
        if (t.still) EndStill(u);
        else EndTake(u);
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// the interface: game thread and swap thread
// ---------------------------------------------------------------------------------------------------------------------
void CaptureGate() {
    const uint64_t before = g_updates.load(std::memory_order_relaxed);  // updates that ran
    const uint64_t u = before + 1;                                      // the update about to run
    if (!g_take_active.load(std::memory_order_relaxed)) {
        if (g_closing.load(std::memory_order_acquire)) {  // a game that is closing starts no take and no still (the sound's mixer would be held again)
            g_updates.store(u, std::memory_order_release);
            return;
        }
        bool still = false;
        {
            std::lock_guard<std::mutex> lock(g_still_mutex);
            still = g_still_pending;
        }
        if (still) {  // the Director's still: one frame, then the take that was waiting (if any) starts at the next gate
#if defined(LARECOMP_HAS_GPU)
            if (mcla::gpu::Enabled()) {
                BeginStill(u);
                g_updates.store(u, std::memory_order_release);
                return;
            }
#endif
            {
                std::lock_guard<std::mutex> lock(g_still_mutex);
                g_still_pending = false;
                g_still_res.done = true;
                g_still_res.ok = false;
                g_still_res.error = "a still needs the native renderer";
            }
        }
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

void CaptureCloseNow() {
    g_closing.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(g_gate_mutex);  // a game thread between its question and its wait has the mutex: it asks again or is waiting when the notify comes
    }
    g_gate_cv.notify_all();
    capture_audio::Abort();  // the take's sound: the mixer's gate opens, the device gets its mix back, and the audio steps stop waiting for it
}

void CaptureOnHandOver() {
    // CHECK: that the render thread is idle here and has swapped the frame before. frame_pacing.cpp's header says so of sub_82305B38 (read in
    // the generated code: the hand-over waits for "render done", which the render thread signals after D3DDevice_Swap, then signals it again);
    // nothing has run it. If a swap ever arrives with the stamp of the frame after it, every take shows "[cap] TAG" lines.
    if (!g_take_active.load(std::memory_order_acquire)) return;
    const core::HandOver h = g_handover.Note(g_updates.load(std::memory_order_relaxed), true, g_take_first.load(std::memory_order_acquire));
    if (h.take_frame) {
        const double pinned = CapturePinnedSceneTime();
        if (pinned >= 0.0 && SceneTime() != pinned) CaptureSceneInterrupted();
    }
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

double CapturePinnedSceneTime(uint64_t* frame) {
    if (!g_take_active.load(std::memory_order_acquire) || g_take.still || !g_take.scene_running) return -1.0;
    const uint64_t index = g_updates.load(std::memory_order_relaxed) - g_take.first_update;
    if (frame) *frame = index;
    return g_take.scene_clock.Time(index);
}

void CaptureSceneInterrupted() {
    if (CapturePinnedSceneTime() >= 0.0) TakeFail("the scene was interrupted during a pinned take");
}

void CaptureSceneSpec(const CaptureSpec& spec) {
    std::lock_guard<std::mutex> lock(g_spec_mutex);
    g_spec = spec;
}

bool CaptureStillRequest(const StillRequest& request, std::string* why) {
    const auto refuse = [&](const char* text) {
        if (why) *why = text;
        return false;
    };
#if defined(LARECOMP_HAS_GPU)
    if (!mcla::gpu::Enabled()) return refuse("a still needs the native renderer");
#else
    return refuse("a still needs the native renderer");
#endif
    if (g_take_active.load(std::memory_order_acquire)) return refuse("a take is running");
    if (request.path.empty()) return refuse("no path");
    std::lock_guard<std::mutex> lock(g_still_mutex);
    if (g_still_pending) return refuse("another still is waiting");
    g_still_pending = true;
    g_still_path = request.path;
    g_still_res = StillResult{};
    g_still_res.id = request.id;
    g_still_res.path = request.path;
    return true;
}

bool CaptureStillPoll(uint64_t id, StillResult* out) {
    std::lock_guard<std::mutex> lock(g_still_mutex);
    if (g_still_res.id != id || !g_still_res.done) return false;
    if (out) *out = g_still_res;
    return true;
}

void CaptureDirectorRequest(double fps, const CaptureSpec& spec) {
    {
        std::lock_guard<std::mutex> lock(g_spec_mutex);
        g_dir_spec = spec;
    }
    const double before = g_dir_fps.exchange(fps > 0.0 ? fps : 0.0, std::memory_order_relaxed);
    if ((before > 0.0) != (fps > 0.0)) {
        if (fps > 0.0) LARECOMP_APP_INFO("[cap] the Director asks for a take at {:g} fps", fps);
        else LARECOMP_APP_INFO("[cap] the Director ends the take");
    }
}

CaptureStatus CaptureGetStatus() {
    CaptureStatus st;
    st.active = g_take_active.load(std::memory_order_acquire) && !g_still_active.load(std::memory_order_acquire);
    st.still = g_still_active.load(std::memory_order_acquire);
    if (st.active) {
        const Take& t = g_take;
        st.name = t.name;
        st.folder = t.layout.folder;
        st.fps = t.fps;
        st.frames = g_handover.Handed();
        st.failed = g_take_failed.load(std::memory_order_acquire);
        if (st.failed) st.error = FailReason();
    }
    st.finished = g_takes_finished.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lock(g_last_mutex);
        st.last_name = g_last_status.name;
        st.last_folder = g_last_status.folder;
        st.last_state = g_last_status.state;
        st.last_frames = g_last_status.frames;
        st.last_error = g_last_status.error;
    }
    return st;
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
    return core::StepTicksForFps(larecomp::settings::mcla_fixed_step_fps.Get());
}

bool FixedStepActive() {
    return g_take_active.load(std::memory_order_acquire) || larecomp::settings::mcla_fixed_step_fps.Get() > 0.0;
}

uint64_t UpdateCount() {
    return g_updates.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------------------------------
// the scene clock
// ---------------------------------------------------------------------------------------------------------------------
namespace {
bool ClockWanted() {
    return g_take_active.load(std::memory_order_acquire) && !g_still_active.load(std::memory_order_acquire) && larecomp::settings::mcla_capture_virtual_clock.Get() &&
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
    const bool by_update = frame == kNoClockFrame;
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
    return std::clamp<int>(larecomp::settings::mcla_capture_lag.Get(), 0, 2);
}

bool CaptureSyncReadback(uint64_t update) {
    if (CaptureLag() == 0) return true;
    if (!g_take_active.load(std::memory_order_acquire) || g_take.sink != core::SinkKind::kLuma) return false;
    // scene's ending gate drains the sink. The earlier frames may overlap.
    const Take& t = g_take;
    return core::LumaLastFrame(t.scene_clock, update, t.first_update, t.end_scene_t, t.luma_scene_duration);
}

double CaptureDelayMs() {
    return larecomp::settings::mcla_capture_delay_ms.Get();
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
        if (t.sink == core::SinkKind::kLuma) {
            const auto crop = t.luma_crop.width ? t.luma_crop : core::LumaCrop{0, 0, width, height};
            if (!crop.Fits(width, height)) {
                TakeFail("mcla_capture_luma crop is outside the readback");
                return grant;
            }
            // Previous plane, predictor, compressor output/context and the occasional PNG.
            r.fixed_bytes = 4ull * crop.Pixels() + 2ull * width * height + (4ull << 20);
        }
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
    const core::HostRing::Slot slot = s.ring.Acquire(larecomp::settings::mcla_capture_gate_timeout_ms.Get(), &waited, &stuck);
    if (stall_ms) *stall_ms = waited;
    if (waited > 1.0) {
        std::lock_guard<std::mutex> rows(g_rows_mutex);
        ++g_take.ring_stalls;
    }
    if (!slot.data) {
        if (stuck) {
            g_sink_stuck.store(true, std::memory_order_release);
            TakeFail("the writer freed no buffer for " + std::to_string(larecomp::settings::mcla_capture_gate_timeout_ms.Get() / 1000) +
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
    spec.request_scene_t = scene.capture_from;
    CaptureSceneSpec(spec);
}

void ApplierApply(const Scene& scene, double t) {
    if (scene.capture_fps <= 0.0) return;  // a scene with no capture key costs nothing
    const CaptureWindow w = EvaluateCapture(scene, t);
    const SceneTransport tr = GetSceneTransport();
    CaptureSceneRequest(w.on && !tr.holding_for_ready && !tr.held ? w.fps : 0.0);
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
void CaptureCloseNow() {}
void CaptureOnHandOver() {}
void CaptureOnSwap() {}
bool CaptureTakeActive() { return false; }
double CapturePinnedSceneTime(uint64_t*) { return -1.0; }
void CaptureSceneInterrupted() {}
void CaptureSceneRequest(double) {}
void CaptureSceneSpec(const CaptureSpec&) {}
bool CaptureStillRequest(const StillRequest&, std::string* why) {
    if (why) *why = "not in this build";
    return false;
}
bool CaptureStillPoll(uint64_t, StillResult*) { return false; }
void CaptureDirectorRequest(double, const CaptureSpec&) {}
CaptureStatus CaptureGetStatus() { return CaptureStatus{}; }
MemoryGrant CaptureMemoryPlan(uint32_t, uint32_t, uint32_t, uint32_t) { return MemoryGrant{}; }
bool CaptureSyncReadback(uint64_t) { return false; }
uint64_t FixedStepTicks() { return 0; }
bool FixedStepActive() { return false; }
uint64_t UpdateCount() { return 0; }
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
