#ifndef REXGLUE_HAS_XEO3_TARGET
//
// Front-end camera (the city shot behind the boot / initial menus).
//
// RE map (default.xex), and why the obvious targets are the wrong ones:
//
//   sub_822C0320   The camera system's per-frame update. It builds the final
//                  camera through one of FOUR branches, picked by the byte
//                  mcUILogic+332672 (UILogic_SetCameraControl), by whether a
//                  cutscene camera supplied a matrix, and by whether the
//                  debug/menu camera manager at camsys+0x33C exists:
//                    0x822C065C  manager path (sub_82502E18, then read the
//                                matrix back out of the active camera)
//                    0x822C0714  fallback camera when there is no manager
//                    0x822C07A4  mcUILogic::UpdateCamera (sub_8220AA68)
//                    cutscene/replay matrix from earlier in the function
//
//                  Hooking any single producer is a coin flip — measured with
//                  menu_cam_diag, the boot menu does NOT take the
//                  mcUILogic::UpdateCamera branch. So we hook the point where
//                  all four converge instead:
//
//                    0x822C0868 (loc_822C0868), a branch target, one instruction
//                    before the result is copied and handed to the renderer:
//                      r1+0x50  fov (degrees)   r1+0x54 far   r1+0x58 near
//                      r1+0x60  matrix row 0    +0x70 row 1   +0x80 row 2
//                      r1+0x90  row 3 = world position
//                      r24      camera mode id; 6 = a UI/menu camera is driving,
//                               which is the game's own way of saying "this
//                               frame is a menu shot", so we gate on it and
//                               leave the gameplay camera alone.
//                    The copy at 0x822C0868-0x822C08AC, sub_822E5188 (matrix)
//                    and sub_822E5E10 (fov/near/far) all read those slots.
//
//   sub_82654960   setMenuCam's slot picker (nav.setMenuCam(N) in
//                  tune/ui/*.sc.xml, FlashNavigator vtable 0x82023AB8 ->
//                  sub_82220FE0 case 13). Forces 0 in cutscene/replay, else
//                  probes the shot (sub_82654738 camera->lookat sweep,
//                  sub_822082B0 player->camera sweep) and falls back to idx+10.
//                  Hooked only to read/lock the slot number.
//
// Dead ends, recorded so nobody re-walks them:
//   - The slot table sub_82654960 probes (UILogic+0x13210 position, +0x13530
//     lookat, +0x13E10 fov) is zeroed by the mcUILogic ctor (sub_8220E080) with
//     fov 25.0, and nothing in the XEX ever writes it. All 50 entries stay
//     (0,0,0) at runtime.
//   - sub_82205FD8 reads that table, and is only the FSCommand:SetPolarCam
//     handler, registered in the ctor. Not the menu backdrop.
//   - mcUILogic::UpdateCamera's own override slot (+332674 flag, +332688 matrix,
//     +332752 fov) works, but the function is skipped in menus.
//
// Registered as midasm hooks in larecomp_config.toml:
//   0x82654960  Hook_MenuCameraPick   (r3, return_on_true)
//   0x822C0868  Hook_MenuCameraFinal  (r1, r24)
//

#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include "larecomp_log.h"
#include "menu_camera.h"
#include "time_of_day.h"

// ---------------------------------------------------------------- cvars

REXCVAR_DEFINE_BOOL(menu_cam_lock, false, "MCLA/Menu Camera",
    "Always show the same shot instead of letting the game rotate. Off = the "
    "game picks (setMenuCam plus its own visibility probes) and menu_cam_slot is "
    "ignored; on = menu_cam_slot every time. This is the on/off switch — the "
    "slot number by itself never locks anything.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(menu_cam_slot, 0, "MCLA/Menu Camera",
    "Which shot menu_cam_lock pins (0-63). Ignored while menu_cam_lock is off, "
    "so parking a number here is harmless.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(menu_cam_custom, false, "MCLA/Menu Camera",
    "Place the menu camera at the shot from <exe dir>/menu_cameras.txt matching "
    "the current slot. Slots with no line keep the stock camera. The file is "
    "re-read when it changes, so you can tune it with the menu on screen.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(menu_cam_only_custom, false, "MCLA/Menu Camera",
    "Show only your own shots. Whatever slot the game picks is remapped onto the "
    "lines present in menu_cameras.txt, so the stock cameras never appear. The "
    "game's rotation still decides which of your shots comes up; turn on "
    "menu_cam_lock to pin one. Needs menu_cam_custom.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(menu_cam_cycle, 0.0, "MCLA/Menu Camera",
    "Seconds between shots, rotating through every line in menu_cameras.txt. "
    "0 = no rotation. The game does not rotate the front-end camera itself — it "
    "only calls setMenuCam for the network and garage menus, so without this the "
    "boot screen sits on one shot forever. Ignored while menu_cam_lock is on.")
    .range(0.0, 600.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(menu_cam_record, false, "MCLA/Menu Camera",
    "Capture the game's own menu shots into menu_cameras.txt as 'cam' lines. "
    "There is no table of stock camera spots in the XEX — they only exist as "
    "runtime state — so this samples the real camera whenever it moves somewhere "
    "new (25+ units) and appends it with the next free slot number. Leave it on "
    "while the menus cycle, then turn it off: every shot, stock and custom, is "
    "now a line you own. Works with menu_cam_custom on; it always records what "
    "the game produced, never what this placed.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(menu_cam_diag, false, "MCLA/Menu Camera",
    "Log the camera the game hands to the renderer (mode id, position, matrix "
    "rows) once per mode change, and every shot this places. Turn on if a shot "
    "does not show up or points the wrong way.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Forward decl so the command cvar below can name it.
static void MenuCamDump();

REXCVAR_DEFINE_COMMAND(menu_cam_dump, MenuCamDump, "MCLA/Menu Camera",
    "Append the current spot to <exe dir>/menu_cameras.txt as a ready-to-use "
    "'cam' line. Fly there with debug_cam = free first (the free-fly pose gives "
    "both the position and the look direction); without it the player car's "
    "position is used. Also records the menu camera currently on screen.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// ---------------------------------------------------------------- guest memory

// Game object; player position sits at +0x10A0 (sub_822082B0 loads it there).
constexpr uint32_t kGameObjPtr = 0x8286D804;

// sub_822C0320 stack slots, relative to r1 at loc_822C0868.
constexpr uint32_t kSlotFov = 0x50;
constexpr uint32_t kSlotFar = 0x54;
constexpr uint32_t kSlotNear = 0x58;
constexpr uint32_t kSlotRow0 = 0x60;
constexpr uint32_t kSlotRow1 = 0x70;
constexpr uint32_t kSlotRow2 = 0x80;
constexpr uint32_t kSlotPos = 0x90;

// r24 at that point: the game's camera mode id. 6 = a UI/menu camera is driving.
constexpr uint32_t kModeMenu = 6;

// Sanity check only. Do NOT narrow this to the XEX image range: the guest stack
// and heap live far outside it (the vinyl hook logs work areas at 0xBD3C0D60),
// so an image-range test silently rejects every stack pointer and object handed
// to these hooks — which is exactly why the first three attempts logged nothing
// at all. The guest address space is a full 4 GiB reservation off
// virtual_membase, so any non-null 32-bit address is addressable.
static bool IsGuestAddr(uint32_t ea) {
    return ea >= 0x10000u;
}

static uint32_t ReadU32(const uint8_t* base, uint32_t addr) {
    uint32_t v;
    std::memcpy(&v, base + addr, sizeof(v));
    return __builtin_bswap32(v);
}

static void WriteU32(uint8_t* base, uint32_t addr, uint32_t v) {
    v = __builtin_bswap32(v);
    std::memcpy(base + addr, &v, sizeof(v));
}

static float ReadF32(const uint8_t* base, uint32_t addr) {
    uint32_t v = ReadU32(base, addr);
    float f;
    std::memcpy(&f, &v, sizeof(f));
    return f;
}

static void WriteF32(uint8_t* base, uint32_t addr, float f) {
    uint32_t v;
    std::memcpy(&v, &f, sizeof(v));
    WriteU32(base, addr, v);
}

// ---------------------------------------------------------------- custom table

// The game only ever asks for slots 0-19; the extra room lets you park spare
// shots in the file and switch between them with menu_cam_slot.
constexpr int kMaxSlots = 64;

// mcUILogic's ctor seeds every slot's fov with this, so it is the sane default
// for a line that leaves the field at 0.
constexpr float kDefaultFovDeg = 25.0f;

struct CamEntry {
    bool used = false;
    // A "stock" entry takes a slot in the rotation but places nothing, leaving
    // the game's own front-end camera on screen for that step. That is how you
    // mix the original shots in with your own.
    bool stock = false;
    float pos[3] = {0.0f, 0.0f, 0.0f};
    float look[3] = {0.0f, 0.0f, 0.0f};
    float fovy = 0.0f;  // 0 = keep the game's fov for this frame
    float tod = -1.0f;  // hour 0-24 to light the shot with; <0 = leave the clock
    int weather = -1;   // engine weather index for the shot; <0 = leave the sky
};

static std::mutex g_table_mutex;
static std::array<CamEntry, kMaxSlots> g_table{};
static std::filesystem::file_time_type g_table_stamp{};
static bool g_table_loaded = false;
static int64_t g_next_stat_ms = 0;

static std::filesystem::path CamFilePath() {
    std::error_code ec;
    return std::filesystem::current_path(ec) / "menu_cameras.txt";
}

static int64_t NowMs() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Parses lines of the form
//   cam   <slot> <px> <py> <pz> <lx> <ly> <lz> [fov_degrees] [hour] [weather]
//   stock <slot>
// weather is one of nice / cloudy / stormy / foggy.
// '#' and ';' start a comment; everything else is ignored, so the dump command
// can interleave commented context blocks with the real lines.
static void LoadTableLocked() {
    std::array<CamEntry, kMaxSlots> parsed{};
    std::ifstream in(CamFilePath());
    if (!in) {
        g_table = parsed;
        return;
    }

    auto claim = [&](int slot, const CamEntry& e, const char* what) {
        if (slot < 0 || slot >= kMaxSlots) {
            LARECOMP_APP_INFO("[MenuCam] {} slot {} out of range (0-{}), skipped",
                              what, slot, kMaxSlots - 1);
            return false;
        }
        parsed[static_cast<size_t>(slot)] = e;
        return true;
    };

    std::string line;
    int cams = 0;
    int stocks = 0;
    while (std::getline(in, line)) {
        auto cut = line.find_first_of("#;");
        if (cut != std::string::npos) line.resize(cut);

        int slot = 0;
        CamEntry e{};
        char wname[16] = {0};
        int n = std::sscanf(line.c_str(), " cam %d %f %f %f %f %f %f %f %f %15s",
                            &slot, &e.pos[0], &e.pos[1], &e.pos[2], &e.look[0],
                            &e.look[1], &e.look[2], &e.fovy, &e.tod, wname);
        if (n >= 7) {
            if (n < 8) e.fovy = 0.0f;
            if (n < 9) e.tod = -1.0f;
            e.weather = (n >= 10) ? Weather_NameToIndex(wname) : -1;
            e.used = true;
            if (claim(slot, e, "cam")) ++cams;
            continue;
        }

        // A stock step: reserves its place in the rotation and shows the game's
        // own camera when it comes up.
        if (std::sscanf(line.c_str(), " stock %d", &slot) == 1) {
            CamEntry s{};
            s.used = true;
            s.stock = true;
            if (claim(slot, s, "stock")) ++stocks;
        }
    }

    g_table = parsed;
    LARECOMP_APP_INFO(
        "[MenuCam] loaded {} custom shot(s) + {} stock step(s) from menu_cameras.txt",
        cams, stocks);
}

// Reloads when the file's write time changed. Stat'd at most once a second so
// the per-frame hook stays cheap.
static void RefreshTableLocked() {
    int64_t now = NowMs();
    if (g_table_loaded && now < g_next_stat_ms) return;
    g_next_stat_ms = now + 1000;

    std::error_code ec;
    auto stamp = std::filesystem::last_write_time(CamFilePath(), ec);
    if (ec) {
        // File gone: drop the table so the stock shots come back.
        if (g_table_loaded) {
            g_table = {};
            g_table_stamp = {};
        }
        g_table_loaded = true;
        return;
    }
    if (g_table_loaded && stamp == g_table_stamp) return;

    g_table_stamp = stamp;
    g_table_loaded = true;
    LoadTableLocked();
}

// ---------------------------------------------------------------- dump sources

static std::mutex g_sample_mutex;
// Free-fly camera pose, fed by Patch_DebugCam.
static bool g_freecam_valid = false;
static float g_freecam[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};  // x y z yaw pitch
// When the free-fly camera last drove the shot; we stand down while it is active.
static int64_t g_freecam_ms = 0;
// Slot the game last asked for through setMenuCam.
static int g_last_pick = 0;
// Last camera the renderer actually got, for the dump.
static bool g_last_menu_valid = false;
static uint32_t g_last_mode = 0;
static float g_last_menu_pos[3] = {0.0f, 0.0f, 0.0f};
static float g_last_menu_fov = 0.0f;

void MenuCam_NoteFreecam(float x, float y, float z, float yaw, float pitch) {
    std::lock_guard<std::mutex> lock(g_sample_mutex);
    g_freecam_ms = NowMs();
    g_freecam[0] = x;
    g_freecam[1] = y;
    g_freecam[2] = z;
    g_freecam[3] = yaw;
    g_freecam[4] = pitch;
    g_freecam_valid = true;
}

static void MenuCamDump() {
    float fc[5];
    bool have_fc;
    int pick;
    bool have_menu;
    uint32_t mode;
    float mpos[3];
    float mfov;
    {
        std::lock_guard<std::mutex> lock(g_sample_mutex);
        have_fc = g_freecam_valid;
        std::memcpy(fc, g_freecam, sizeof(fc));
        pick = g_last_pick;
        have_menu = g_last_menu_valid;
        mode = g_last_mode;
        std::memcpy(mpos, g_last_menu_pos, sizeof(mpos));
        mfov = g_last_menu_fov;
    }

    auto* base = rex::Runtime::instance()->virtual_membase();
    float ppos[3] = {0.0f, 0.0f, 0.0f};
    bool have_player = false;
    if (base) {
        uint32_t game = ReadU32(base, kGameObjPtr);
        if (IsGuestAddr(game)) {
            ppos[0] = ReadF32(base, game + 0x10A0);
            ppos[1] = ReadF32(base, game + 0x10A4);
            ppos[2] = ReadF32(base, game + 0x10A8);
            have_player = true;
        }
    }

    // Pick the sample point. The free-fly camera is preferred: it carries a look
    // direction, which a bare position does not. Orientation convention matches
    // Patch_DebugCam (yaw about +Y, Y up).
    float pos[3];
    float look[3];
    const char* origin;
    if (have_fc) {
        float cy = std::cos(fc[3]), sy = std::sin(fc[3]);
        float cp = std::cos(fc[4]), sp = std::sin(fc[4]);
        pos[0] = fc[0];
        pos[1] = fc[1];
        pos[2] = fc[2];
        look[0] = pos[0] + cp * sy * 25.0f;
        look[1] = pos[1] + sp * 25.0f;
        look[2] = pos[2] + cp * cy * 25.0f;
        origin = "free-fly camera";
    } else if (have_player) {
        pos[0] = ppos[0];
        pos[1] = ppos[1] + 6.0f;
        pos[2] = ppos[2];
        look[0] = ppos[0];
        look[1] = ppos[1];
        look[2] = ppos[2];
        origin = "player position (no free-fly pose; set debug_cam = free for a look direction)";
    } else {
        LARECOMP_APP_INFO("[MenuCam] dump: no position available (load a game first)");
        return;
    }

    auto path = CamFilePath();
    std::ofstream out(path, std::ios::app);
    if (!out) {
        LARECOMP_APP_INFO("[MenuCam] dump: cannot write {}", path.string());
        return;
    }

    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char stamp[64] = {0};
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);

    char buf[256];
    out << "\n# ---- dumped " << stamp << " (" << origin << ")\n";
    if (have_player) {
        std::snprintf(buf, sizeof(buf), "#   player pos  %.3f %.3f %.3f\n",
                      ppos[0], ppos[1], ppos[2]);
        out << buf;
    }
    if (have_menu) {
        std::snprintf(buf, sizeof(buf),
                      "#   last camera to renderer: mode %u  pos %.3f %.3f %.3f"
                      "  fov %.2f  (last setMenuCam slot %d)\n",
                      mode, mpos[0], mpos[1], mpos[2], mfov, pick);
        out << buf;
    }
    out << "# set the slot number, then menu_cam_custom = true"
           " (menu_cam_slot to lock it)\n";

    std::snprintf(buf, sizeof(buf),
                  "cam 0  %.3f %.3f %.3f  %.3f %.3f %.3f  %.2f\n", pos[0],
                  pos[1], pos[2], look[0], look[1], look[2],
                  have_menu && mfov > 0.0f ? mfov : kDefaultFovDeg);
    out << buf;

    LARECOMP_APP_INFO("[MenuCam] dumped {} -> {}", origin, path.string());
}

// ---------------------------------------------------------------- recorder

// Appends the camera the game itself produced as a 'cam' line, so the stock
// shots become slots like any other. Called from the hook with the values read
// before any override, so what lands in the file is always the game's.
static void RecordStockShot(const float pos[3], const float fwd[3], float fov,
                            float tod, int weather) {
    static float s_last[3] = {0.0f, 0.0f, 0.0f};
    static bool s_have_last = false;
    static int64_t s_next_ms = 0;
    static int s_next_slot = -1;

    int64_t now = NowMs();
    if (now < s_next_ms) return;

    if (s_have_last) {
        float dx = pos[0] - s_last[0];
        float dy = pos[1] - s_last[1];
        float dz = pos[2] - s_last[2];
        if ((dx * dx + dy * dy + dz * dz) < (25.0f * 25.0f)) return;
    }
    s_next_ms = now + 500;
    std::memcpy(s_last, pos, sizeof(s_last));
    s_have_last = true;

    // First write of the session: start after the highest slot already in the
    // file so nothing existing is clobbered.
    {
        std::lock_guard<std::mutex> lock(g_table_mutex);
        RefreshTableLocked();
        if (s_next_slot < 0) {
            s_next_slot = 0;
            for (int i = 0; i < kMaxSlots; ++i) {
                if (g_table[static_cast<size_t>(i)].used) s_next_slot = i + 1;
            }
        }
    }
    if (s_next_slot >= kMaxSlots) return;

    auto path = CamFilePath();
    std::ofstream out(path, std::ios::app);
    if (!out) {
        LARECOMP_APP_INFO("[MenuCam] record: cannot open {} for append", path.string());
        return;
    }

    const char* wname = Weather_IndexToName(weather);
    char buf[260];
    std::snprintf(buf, sizeof(buf),
                  "cam %d  %.3f %.3f %.3f  %.3f %.3f %.3f  %.2f  %.2f  %s"
                  "  # recorded stock shot\n",
                  s_next_slot, pos[0], pos[1], pos[2], pos[0] + fwd[0] * 25.0f,
                  pos[1] + fwd[1] * 25.0f, pos[2] + fwd[2] * 25.0f, fov,
                  tod >= 0.0f ? tod : 12.0f, wname ? wname : "nice");
    out << buf;
    out.flush();
    if (!out.good()) {
        LARECOMP_APP_INFO("[MenuCam] record: write to {} failed", path.string());
        return;
    }
    LARECOMP_APP_INFO("[MenuCam] recorded stock shot as slot {} at {:.1f} {:.1f} {:.1f} -> {}",
                      s_next_slot, pos[0], pos[1], pos[2], path.string());
    ++s_next_slot;
}

// ---------------------------------------------------------------- resolution

// Which shot to place this frame, if any. Returns false to leave the camera
// alone (custom off, no line for the slot, or free-fly camera in charge).
static bool ResolveEntry(CamEntry* out, int* out_slot) {
    if (!REXCVAR_GET(menu_cam_custom)) return false;

    bool locked = REXCVAR_GET(menu_cam_lock);
    int32_t forced = REXCVAR_GET(menu_cam_slot);
    int pick;
    {
        std::lock_guard<std::mutex> lock(g_sample_mutex);
        if (NowMs() - g_freecam_ms < 500) return false;  // free-fly owns it
        pick = locked ? forced : g_last_pick;
    }

    std::lock_guard<std::mutex> lock(g_table_mutex);
    RefreshTableLocked();

    // Every slot that has a line, in slot order.
    int defined[kMaxSlots];
    int count = 0;
    for (int i = 0; i < kMaxSlots; ++i) {
        if (g_table[static_cast<size_t>(i)].used) defined[count++] = i;
    }
    if (count == 0) return false;

    // Rotation. The game never rotates the front-end camera on its own — it only
    // calls setMenuCam for the network and garage menus — so g_last_pick sits at
    // whatever it was and the boot screen would stay on one shot. Drive it off a
    // free-running clock instead.
    double cycle = locked ? 0.0 : REXCVAR_GET(menu_cam_cycle);
    if (cycle > 0.0) {
        int64_t step = static_cast<int64_t>(cycle * 1000.0);
        if (step < 1) step = 1;
        int idx = static_cast<int>((NowMs() / step) % count);
        const CamEntry& e = g_table[static_cast<size_t>(defined[idx])];
        if (e.stock) return false;  // this step belongs to the game
        *out = e;
        *out_slot = defined[idx];
        return true;
    }

    if (pick >= 0 && pick < kMaxSlots && g_table[static_cast<size_t>(pick)].used) {
        const CamEntry& e = g_table[static_cast<size_t>(pick)];
        if (e.stock) return false;
        *out = e;
        *out_slot = pick;
        return true;
    }

    if (!REXCVAR_GET(menu_cam_only_custom)) return false;

    // Only-custom: fold whatever the game asked for onto the lines that exist,
    // so a slot without a line still shows one of your shots instead of the
    // stock camera.
    int idx = pick < 0 ? 0 : pick % count;
    const CamEntry& e = g_table[static_cast<size_t>(defined[idx])];
    if (e.stock) return false;
    *out = e;
    *out_slot = defined[idx];
    return true;
}

// ---------------------------------------------------------------- hooks

// sub_82654960, the slot picker behind setMenuCam. r3 = pointer to the index.
// Returning true skips the original body, so the visibility probes, the idx+10
// fallback and the cutscene reset to 0 are all bypassed and the requested slot
// always wins.
bool Hook_MenuCameraPick(PPCRegister& r3) {
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return false;
    uint32_t p = static_cast<uint32_t>(r3.u64);
    if (!IsGuestAddr(p)) return false;

    int32_t slot = REXCVAR_GET(menu_cam_slot);
    if (!REXCVAR_GET(menu_cam_lock) || slot < 0 || slot >= kMaxSlots) {
        // Not locking: just remember what the game asked for, for the dump and
        // for picking which custom line to place.
        std::lock_guard<std::mutex> lock(g_sample_mutex);
        g_last_pick = static_cast<int32_t>(ReadU32(base, p));
        return false;
    }

    // Whether this module is going to place the shot ITSELF this frame. When it
    // is, the guest index does not need forcing at all: ResolveEntry reads
    // menu_cam_slot directly while locked and never looks at the guest value,
    // and Hook_MenuCameraFinal overwrites the camera the game produced anyway.
    bool placed_here = false;
    if (REXCVAR_GET(menu_cam_custom)) {
        std::lock_guard<std::mutex> lock(g_table_mutex);
        RefreshTableLocked();
        const CamEntry& e = g_table[static_cast<size_t>(slot)];
        placed_here = e.used && !e.stock;
    }
    // Writing a slot the GAME does not own is a crash, not a cosmetic miss.
    // sub_82654960 indexes its tables straight off this value with no bounds
    // check (dword_8286D8D4 + 16*(slot + 4897) and + 16*(slot + 4907), the two
    // groups its own `slot += 10` steps between), and the pause menu's camera
    // compare (sub_82653450) looks the index up in "camera_coord", gets null
    // back and calls a vfunc through it. Reported with menu_cam_slot = 51 and
    // menu_cam_custom turned OFF: "Unhandled guest access violation: read of
    // guest 0x00000000", inside sub_82653450 -> sub_8265F190 -> the pause menu
    // open path. With custom off nothing placed the shot, so the forced 51 went
    // straight into the game's own camera code.
    constexpr int32_t kMaxGuestSlot = 19;
    if (placed_here || slot > kMaxGuestSlot) {
        std::lock_guard<std::mutex> lock(g_sample_mutex);
        g_last_pick = slot;
        return false;  // let the game pick its own valid slot
    }

    WriteU32(base, p, static_cast<uint32_t>(slot));
    {
        std::lock_guard<std::mutex> lock(g_sample_mutex);
        g_last_pick = slot;
    }
    return true;
}

// loc_822C0868 in sub_822C0320: every branch has produced its camera and the
// result is one instruction away from being copied and handed to the renderer.
// r1 = stack pointer, r24 = camera mode id (6 = UI/menu camera).
//
// Writing the source slots here beats hooking any single producer, which is what
// the two earlier attempts got wrong: the boot menu does not take the
// mcUILogic::UpdateCamera branch.
void Hook_MenuCameraFinal(PPCRegister& r1, PPCRegister& r24) {
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    uint32_t sp = static_cast<uint32_t>(r1.u64);
    if (!IsGuestAddr(sp)) return;

    uint32_t mode = static_cast<uint32_t>(r24.u64);

    // Record what the renderer is about to get, for the dump and the diag.
    float cur_pos[3] = {ReadF32(base, sp + kSlotPos + 0),
                        ReadF32(base, sp + kSlotPos + 4),
                        ReadF32(base, sp + kSlotPos + 8)};
    float cur_fov = ReadF32(base, sp + kSlotFov);
    {
        std::lock_guard<std::mutex> lock(g_sample_mutex);
        g_last_mode = mode;
        std::memcpy(g_last_menu_pos, cur_pos, sizeof(cur_pos));
        g_last_menu_fov = cur_fov;
        g_last_menu_valid = true;
    }

    if (REXCVAR_GET(menu_cam_diag)) {
        static uint32_t s_logged_mode = 0xFFFFFFFFu;
        if (mode != s_logged_mode) {
            s_logged_mode = mode;
            LARECOMP_APP_INFO(
                "[MenuCam] renderer camera: mode={} pos {:.1f} {:.1f} {:.1f} fov {:.2f}"
                " row0 {:.3f} {:.3f} {:.3f} | row1 {:.3f} {:.3f} {:.3f}"
                " | row2 {:.3f} {:.3f} {:.3f}",
                mode, cur_pos[0], cur_pos[1], cur_pos[2], cur_fov,
                ReadF32(base, sp + kSlotRow0 + 0), ReadF32(base, sp + kSlotRow0 + 4),
                ReadF32(base, sp + kSlotRow0 + 8),
                ReadF32(base, sp + kSlotRow1 + 0), ReadF32(base, sp + kSlotRow1 + 4),
                ReadF32(base, sp + kSlotRow1 + 8),
                ReadF32(base, sp + kSlotRow2 + 0), ReadF32(base, sp + kSlotRow2 + 4),
                ReadF32(base, sp + kSlotRow2 + 8));
        }
    }

    // Capture the game's own shot before anything is overwritten — row 2 is the
    // forward vector, which is what turns a position into a lookat. This runs
    // ahead of the menu-mode gate so the log can say why nothing is being
    // captured when the camera is not a menu one.
    if (REXCVAR_GET(menu_cam_record)) {
        static uint32_t s_seen_mode = 0xFFFFFFFFu;
        if (mode != s_seen_mode) {
            s_seen_mode = mode;
            LARECOMP_APP_INFO("[MenuCam] record armed, camera mode {} — {}", mode,
                              mode == kModeMenu ? "capturing"
                                                : "ignored, not a menu camera");
        }
        if (mode == kModeMenu) {
            float fwd[3] = {ReadF32(base, sp + kSlotRow2 + 0),
                            ReadF32(base, sp + kSlotRow2 + 4),
                            ReadF32(base, sp + kSlotRow2 + 8)};
            RecordStockShot(cur_pos, fwd, cur_fov, TimeOfDay_GetHour(),
                            Weather_GetIndex());
        }
    }

    // Only menu shots. The gameplay camera keeps its own mode id, so it is never
    // touched even with the cvars on.
    if (mode != kModeMenu) {
        TimeOfDay_RequestHour(-1.0f);
        Weather_RequestIndex(-1);
        return;
    }

    CamEntry e;
    int slot = 0;
    if (!ResolveEntry(&e, &slot)) {
        TimeOfDay_RequestHour(-1.0f);
        Weather_RequestIndex(-1);  // let the clock go
        return;
    }

    // A shot can carry the hour and sky it was framed under. Both requests are
    // renewed every frame and lapse on their own, so leaving the menu hands the
    // clock and the weather straight back without any unwinding.
    TimeOfDay_RequestHour(e.tod);
    Weather_RequestIndex(e.weather);

    // RAGE keeps the rows as right / up / forward / position, and this world is
    // Y-up (same convention the free-fly camera uses).
    float f[3] = {e.look[0] - e.pos[0], e.look[1] - e.pos[1],
                  e.look[2] - e.pos[2]};
    float flen = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (flen < 1e-4f) {
        f[0] = 0.0f;
        f[1] = 0.0f;
        f[2] = 1.0f;
    } else {
        f[0] /= flen;
        f[1] /= flen;
        f[2] /= flen;
    }

    // World up, swapped when the shot looks straight up or down.
    float up[3] = {0.0f, 1.0f, 0.0f};
    if (std::fabs(f[1]) > 0.999f) {
        up[1] = 0.0f;
        up[2] = 1.0f;
    }

    // right = up x forward
    float r[3] = {up[1] * f[2] - up[2] * f[1], up[2] * f[0] - up[0] * f[2],
                  up[0] * f[1] - up[1] * f[0]};
    float rlen = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (rlen < 1e-4f) {
        r[0] = 1.0f;
        r[1] = 0.0f;
        r[2] = 0.0f;
    } else {
        r[0] /= rlen;
        r[1] /= rlen;
        r[2] /= rlen;
    }

    // up = forward x right
    float u[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2],
                  f[0] * r[1] - f[1] * r[0]};

    auto put = [&](uint32_t slot_off, const float v[3], float w) {
        WriteF32(base, sp + slot_off + 0, v[0]);
        WriteF32(base, sp + slot_off + 4, v[1]);
        WriteF32(base, sp + slot_off + 8, v[2]);
        WriteF32(base, sp + slot_off + 12, w);
    };
    put(kSlotRow0, r, 0.0f);
    put(kSlotRow1, u, 0.0f);
    put(kSlotRow2, f, 0.0f);
    put(kSlotPos, e.pos, 1.0f);

    if (e.fovy > 0.0f) WriteF32(base, sp + kSlotFov, e.fovy);

    if (REXCVAR_GET(menu_cam_diag)) {
        static int s_last_slot = -1;
        if (slot != s_last_slot) {
            s_last_slot = slot;
            LARECOMP_APP_INFO("[MenuCam] placing slot {} at {:.1f} {:.1f} {:.1f}",
                              slot, e.pos[0], e.pos[1], e.pos[2]);
        }
    }
}

#else  // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

#include "menu_camera.h"

void MenuCam_NoteFreecam(float x, float y, float z, float yaw, float pitch) {}
bool Hook_MenuCameraPick(PPCRegister& r3) { return false; }
void Hook_MenuCameraFinal(PPCRegister& r1, PPCRegister& r24) {}

#endif  // REXGLUE_HAS_XEO3_TARGET
