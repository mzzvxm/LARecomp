// cam_trace: what the gameplay camera did each update, and when each frame got
// to the screen. The measurement behind the camera judder work: whether a jump
// comes from the simulation (the camera itself moves unevenly in game time), from
// the camera shake, or from frames reaching the screen at other intervals than
// the time steps they were simulated with.
//
// One CSV line per event in cam_trace.csv (working directory), QPC ticks
// throughout -- the clock DXGI reports in:
//
//   U  one per game update, after the camera system's tick (sub_822C0320):
//      seq, the QPC of the game clock's timebase read that started the update
//      (sub_821BDA90 on 0x827D7500), the QPC before and after the camera tick,
//      the clock's +8 (scaled dt, what the camera and the frame-rate checks use),
//      +0x58 (unscaled) and +0x5C (raw) deltas, the published camera matrix
//      (*0x8286D804 + 4272: position, forward), the player car's position, and
//      the shake (sub_82315000) seen this update: the camera state's position and
//      forward before and after it, the effect slots that were live, and the sum
//      of their magnitudes.
//   S  one per guest swap (render thread, after the FPS limiter's hold): QPC,
//      timed (the engine timer ran since the previous swap), the newest U seq.
//   P  one per guest present on the host (src/gpu render thread, right after the
//      swap chain's present): QPC, the guest present's ordinal, DXGI's present
//      count, DXGI's frame statistics (the newest present on screen and the QPC
//      of the refresh it went out on; zeros while the desktop composes the
//      window), and with frame_pacing the frame's planned time and when it was
//      ready. Needs cam_trace on at startup.
//
// Off (cam_trace = false, the default) every hook is one relaxed load.

#include "camera_trace.h"

#ifndef REXGLUE_HAS_XEO3_TARGET

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

#include <rex/cvar.h>
#include <rex/ppc.h>

#include "hooks/hooks_internal.h"
#include "logging.h"

#if defined(LARECOMP_HAS_GPU)
#include "gpu/gpu.h"
#endif

REXCVAR_DEFINE_BOOL(cam_trace, false, "MCLA/Diagnostics",
    "Write cam_trace.csv: every game update's camera (position, direction, shake), the player "
    "car's position and the clock's time step, every guest swap, and when each frame was "
    "presented and reached the screen. For measuring camera judder.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__rex_sub_822C0320);  // camera system tick
REX_EXTERN(__imp__rex_sub_822B2AB0);  // mcPlayerCamera::Update
REX_EXTERN(__imp__rex_sub_82315000);  // camera shake (mcCameraEffect), applied to a matrix

namespace {

constexpr uint32_t kVhsmGlobal = 0x8286D804;      // camera publish block owner
constexpr uint32_t kPublishedCamera = 4272;       // + right, up, forward, position rows
constexpr uint32_t kVehicleList = 0x82874374;

int64_t Qpc() {
#if defined(_WIN32)
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
#endif
}

int64_t QpcFrequency() {
#if defined(_WIN32)
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
#else
    return 1000000000;
#endif
}

bool Tracing() {
    return REXCVAR_GET(cam_trace);
}

// Lines go to a buffer; a thread of its own writes it out, so no game thread
// waits on the disk.
class Writer {
 public:
    void Append(const char* text, int length) {
        if (length <= 0) return;
        std::call_once(start_, [this] { Start(); });
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.append(text, static_cast<size_t>(length));
    }

 private:
    void Start() {
        std::error_code ec;
        const auto path = std::filesystem::current_path(ec) / "cam_trace.csv";
        file_ = std::fopen(path.string().c_str(), "wb");
        if (!file_) {
            MC_ERROR("[cam_trace] cannot open {}", path.string());
            return;
        }
        std::fprintf(file_, "#cam_trace,qpc_frequency=%lld\n",
                     static_cast<long long>(QpcFrequency()));
        std::fprintf(file_,
                     "#U,seq,t_clock,t_cam0,t_cam1,dt8,dt58,dt5c,px,py,pz,fx,fy,fz,carx,cary,carz,"
                     "sh_ran,sh0px,sh0py,sh0pz,sh0fx,sh0fy,sh0fz,sh1px,sh1py,sh1pz,sh1fx,sh1fy,"
                     "sh1fz,sh_mask,sh_mag,cs_vtable,plan_target,plan_latency_ms\n");
        std::fprintf(file_, "#S,t,timed,useq\n");
        std::fprintf(file_,
                     "#P,t,guest_present,present_count,stats,on_screen_present,present_refresh,"
                     "sync_refresh,sync_qpc,target,ready,due\n");
        std::fflush(file_);
        MC_INFO("[cam_trace] writing {}", path.string());
        std::thread([this] { Run(); }).detach();
    }

    void Run() {
        std::string local;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            {
                std::lock_guard<std::mutex> lock(mutex_);
                local.swap(pending_);
            }
            if (!local.empty()) {
                std::fwrite(local.data(), 1, local.size(), file_);
                std::fflush(file_);
                local.clear();
            }
        }
    }

    std::once_flag start_;
    std::mutex mutex_;
    std::string pending_;
    FILE* file_ = nullptr;
};

Writer g_writer;

template <typename... Args>
void Line(const char* format, Args... args) {
    char buffer[768];
    const int n = std::snprintf(buffer, sizeof(buffer), format, args...);
    if (n > 0) g_writer.Append(buffer, n < int(sizeof(buffer)) ? n : int(sizeof(buffer)) - 1);
}

// The update thread's state for the U line being built.
int64_t g_clock_qpc = 0;
uint32_t g_player_camera = 0;
struct Shake {
    bool ran = false;
    float before[6] = {};  // position, forward of the matrix handed in
    float after[6] = {};
    uint32_t mask = 0;     // effect slots live this update
    float magnitude = 0;   // their magnitudes summed
};
Shake g_shake;
std::atomic<uint64_t> g_update_seq{0};

void ReadPose(const uint8_t* base, uint32_t matrix, float* out) {
    out[0] = ReadGuestF32(base, matrix + 48);
    out[1] = ReadGuestF32(base, matrix + 52);
    out[2] = ReadGuestF32(base, matrix + 56);
    out[3] = ReadGuestF32(base, matrix + 32);
    out[4] = ReadGuestF32(base, matrix + 36);
    out[5] = ReadGuestF32(base, matrix + 40);
}

// The player car's matrix, the way the game loop finds it for the audio listener
// (sub_822A3998(list, 0) then sub_822A8108): a list kept twice, picked by a byte
// of the thread's block (*r13 + 72). 0 when there is no car.
uint32_t PlayerCarMatrix(const uint8_t* base, uint32_t r13) {
    const uint32_t list = ReadGuestU32(base, kVehicleList);
    if (!list) return 0;
    const uint32_t block = r13 ? ReadGuestU32(base, r13) : 0;
    const bool second = block && base[block + 72] != 0;
    const int32_t count = static_cast<int32_t>(ReadGuestU32(base, list + (second ? 260 : 132)));
    if (count <= 0) return 0;
    const uint32_t vehicle = ReadGuestU32(base, list + 4 * (second ? 34 : 2));
    if (!vehicle) return 0;
    const uint32_t inst = ReadGuestU32(base, vehicle + 48);
    if (!inst) return vehicle + 288;
    if (second) return inst + 272;
    const uint32_t entry = ReadGuestU32(base, inst + 28);
    return entry ? entry + 16 : 0;
}

#if defined(LARECOMP_HAS_GPU)
void OnPresent(const mcla::gpu::PresentObservation& o) {
    if (!Tracing()) return;
    Line("P,%lld,%llu,%u,%d,%u,%u,%u,%lld,%lld,%lld,%lld\n", static_cast<long long>(Qpc()),
         static_cast<unsigned long long>(o.guest_present), o.present_count, o.statistics ? 1 : 0,
         o.on_screen_present, o.present_refresh, o.sync_refresh,
         static_cast<long long>(o.sync_qpc), static_cast<long long>(o.target_qpc),
         static_cast<long long>(o.ready_qpc), static_cast<long long>(o.due_qpc));
}
#endif

}  // namespace

// mcPlayerCamera::Update: which camera owns the shake slots below.
extern "C" REX_FUNC(rex_sub_822B2AB0) {
    if (Tracing()) g_player_camera = ctx.r3.u32;
    __imp__rex_sub_822B2AB0(ctx, base);
}

// The shake: r3 the effect tune (mcPlayerCamera + 24's object, slots at +288,
// 224 bytes each), r4 the camera state's matrix it turns in place, r5 the car's
// position it turns about, f1 the time step.
extern "C" REX_FUNC(rex_sub_82315000) {
    if (!Tracing()) {
        __imp__rex_sub_82315000(ctx, base);
        return;
    }
    const uint32_t effects = ctx.r3.u32;
    const uint32_t matrix = ctx.r4.u32;
    Shake s;
    s.ran = true;
    if (matrix) ReadPose(base, matrix, s.before);
    if (effects) {
        for (uint32_t i = 0; i < 5; ++i) {
            const uint32_t slot = effects + 288 + 224 * i;
            if (ReadGuestU32(base, slot + 20) == 0) continue;
            const float m = ReadGuestF32(base, slot + 0) * ReadGuestF32(base, slot + 4) *
                            ReadGuestF32(base, slot + 8);
            if (m != 0.0f) {
                s.mask |= 1u << i;
                s.magnitude += m;
            }
        }
    }
    __imp__rex_sub_82315000(ctx, base);
    if (matrix) ReadPose(base, matrix, s.after);
    g_shake = s;
}

// The camera system's tick: the frame's camera is published when it returns.
extern "C" REX_FUNC(rex_sub_822C0320) {
    if (!Tracing()) {
        __imp__rex_sub_822C0320(ctx, base);
        return;
    }
    const uint32_t r13 = ctx.r13.u32;
    const int64_t t0 = Qpc();
    __imp__rex_sub_822C0320(ctx, base);
    const int64_t t1 = Qpc();

    float cam[6] = {};
    const uint32_t vhsm = ReadGuestU32(base, kVhsmGlobal);
    if (vhsm) ReadPose(base, vhsm + kPublishedCamera, cam);
    float car[3] = {};
    if (const uint32_t m = PlayerCarMatrix(base, r13)) {
        car[0] = ReadGuestF32(base, m + 48);
        car[1] = ReadGuestF32(base, m + 52);
        car[2] = ReadGuestF32(base, m + 56);
    }
    uint32_t cs_vtable = 0;
    if (g_player_camera) {
        const uint32_t view = ReadGuestU32(base, g_player_camera + 16);
        const uint32_t cs = view ? ReadGuestU32(base, view + 48) : 0;
        cs_vtable = cs ? ReadGuestU32(base, cs) : 0;
    }
    const Shake& s = g_shake;
    const uint64_t seq = g_update_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    int64_t plan_target = 0;
    double plan_latency_ms = 0.0;
    mc::pacing::TraceState(plan_target, plan_latency_ms);
    Line("U,%llu,%lld,%lld,%lld,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.6g,%.6g,%.6g,%.7g,%.7g,%.7g,"
         "%d,%.7g,%.7g,%.7g,%.6g,%.6g,%.6g,%.7g,%.7g,%.7g,%.6g,%.6g,%.6g,%u,%.5g,%08X,%lld,%.3f\n",
         static_cast<unsigned long long>(seq), static_cast<long long>(g_clock_qpc),
         static_cast<long long>(t0), static_cast<long long>(t1),
         ReadGuestF32(base, kGuestFrameDelta), ReadGuestF32(base, 0x827D7558),
         ReadGuestF32(base, 0x827D755C), cam[0], cam[1], cam[2], cam[3], cam[4], cam[5], car[0],
         car[1], car[2], s.ran ? 1 : 0, s.before[0], s.before[1], s.before[2], s.before[3],
         s.before[4], s.before[5], s.after[0], s.after[1], s.after[2], s.after[3], s.after[4],
         s.after[5], s.mask, s.magnitude, cs_vtable, static_cast<long long>(plan_target),
         plan_latency_ms);
    g_shake = Shake{};
}

namespace mc::camtrace {

void OnGuestSwap(bool timed) {
    if (!Tracing()) return;
    Line("S,%lld,%d,%llu\n", static_cast<long long>(Qpc()), timed ? 1 : 0,
         static_cast<unsigned long long>(g_update_seq.load(std::memory_order_relaxed)));
}

void NoteGameClock(int64_t qpc) {
    g_clock_qpc = qpc;
}

void Install() {
#if defined(LARECOMP_HAS_GPU)
    // The P lines need the trace on at startup: the renderer asks the
    // presentation for its statistics after every present while an observer
    // is set.
    if (Tracing()) mcla::gpu::SetPresentObserver(&OnPresent);
#endif
}

}  // namespace mc::camtrace

#else  // REXGLUE_HAS_XEO3_TARGET

namespace mc::camtrace {
void OnGuestSwap(bool) {}
void Install() {}
void NoteGameClock(int64_t) {}
}  // namespace mc::camtrace

#endif  // REXGLUE_HAS_XEO3_TARGET
