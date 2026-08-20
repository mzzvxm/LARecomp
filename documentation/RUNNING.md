# Running LARecomp (Midnight Club Los Angeles Recompiled)

## Quick Start

```powershell
cd out\build\win-amd64-relwithdebinfo
$env:REX_LOG_LEVEL="warn"; Start-Process larecomp.exe
```

The executable locates game data automatically, sets a 60 FPS target with low-latency presentation by default, and maintains simulation accuracy across all frame rates.

**Set `REX_LOG_LEVEL=warn`.** Non-Release builds default to `trace`, which generates roughly 7,500 log lines per second and 1.4 MB/s of synchronous disk writes during gameplay. That disk traffic causes frame stutter.

On first launch, the game presents an ISO installation wizard if assets are missing, and offers to import existing save files from Xenia or RPCS3.

**Visual Studio:** Open the repository folder, select **Windows AMD64 RelWithDebInfo**, and start debugging. Note that launching directly from Visual Studio does not set `REX_LOG_LEVEL`, resulting in trace-level logging.

---

## Frame Rate and Timing Controls

LARecomp runs at 60 FPS by default out of the box (`real_frame_delta = true`, `fps_limit = 60`, `vsync = false`).

Those first two are different things and the pause menu lists them separately:

| Setting | What it does |
|---|---|
| **REAL FRAME DELTA** (`real_frame_delta`) | Feeds the simulation the measured frame time instead of the engine's fixed 30 Hz timestep, and unlocks presentation from every-other-vblank. This is what makes physics, camera and traffic correct above 30 FPS. It is **not** a frame rate setting. |
| **FPS LIMIT** (`fps_limit`) | The actual frame rate cap - 30 / 60 / 120 / 144 / uncapped. A wall-clock limiter, so frame times stay evenly spaced. |

Turning REAL FRAME DELTA off returns the engine to its original 30 Hz fixed
timestep. Three things stay active either way, because they are independent of
it: the per-frame hitch clamp (an unbounded delta after a streaming stall can
reach the physics and audio clocks at any frame rate), the FPS LIMIT cap, and
the CITY LOD scale. Earlier builds gated all three behind this option, so
turning it off silently disabled the frame cap and the LOD slider as well.

The simulation timestep, chase camera lag, suspension travel, and ground-depth damping use continuous-time exponential decay calibrated to the 30 FPS console reference curve. Vehicle handling and camera behaviour remain consistent at 30, 60, 120, and 144 FPS.

### In-Game Pause Menu

Press Start or Escape, navigate to **Options**, and select the ReXGlue / Recomp settings rows to adjust parameters live:
- **REAL FRAME DELTA**: Feeds the simulation the measured frame time instead of the stock 30 Hz fixed timestep. Required for correct physics, camera, and traffic above 30 FPS. This is not a frame rate cap - see FPS LIMIT below.
- **FPS LIMIT**: Sets the wall-clock frame cap. Selectable values are 30, 60, 120, 144, and UNCAPPED.
- **SUSPENSION FIX**: Enables continuous-time chassis depth and suspension damping.
- **DEPTH OF FIELD**: Toggles full-screen DoF blur (disabled by default for clarity and performance).
- **CITY / TRAFFIC LOD**: Adjusts geometry and vehicle draw distance multipliers.
- **CITY AMBIENT CULLING**: Adjusts pedestrian, parked car, and active traffic density limits.

Press **F4** at any time during gameplay to toggle the ReXGlue developer overlay.

### Environment Variable Overrides

```powershell
# Cap frame rate via environment variable
$env:MCLA_FPS_CAP="60"; Start-Process larecomp.exe

# Force stock 30 FPS console behavior
$env:MCLA_FPS_CAP="30"; Start-Process larecomp.exe

# Point at a specific game data directory
$env:MCLA_GAME_DATA="E:\MCLA\MCLA_Game_Files"; Start-Process larecomp.exe

# Raise or lower the per-frame hitch clamp, in ms (clamped to 16-1000)
$env:MCLA_MAX_FRAME_MS="125"; Start-Process larecomp.exe
```

City LOD scale and camera smoothing are **cvars, not environment variables** -
set them from the pause menu (`lod_city_scale`, `chase_cam_smoothing_factor`).
The full list of variables this build actually reads is written to
`logs/effective_config.txt` on every launch, under
`=== env overrides (only vars this build reads) ===`, with the cvar-only
settings listed separately below it.

The internal frame limiter uses thread-pinned wall-clock accumulation with 1 ms timer precision (`timeBeginPeriod(1)`), coarse sleep, and yield-spinning. This avoids the 15.625 ms quantization grid inherent to vblank synchronization.

---

## Building from Source

### Prerequisites

- Visual Studio 2022 (MSVC v143 x64 toolset)
- Clang / LLVM toolchain
- CMake 3.25 or newer
- ReXGlue SDK 0.10.0 (located in `E:\MCLA\rexglue-sdk-0.10.0`)

### Build Command

From the repository root:

```powershell
cmd.exe /c 'call "F:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" && set "PATH=E:\MCLA\llvm\bin;F:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%" && cmake --build out/build/win-amd64-relwithdebinfo'
```

To reconfigure CMake presets:

```powershell
cmake --preset win-amd64-relwithdebinfo .
cmake --build out/build/win-amd64-relwithdebinfo
```

---

## Code Generation Pipeline (`rexglue codegen`)

When updating function declarations, mid-asm hooks, or recompiled regions:

```powershell
E:\MCLA\rexglue-sdk-0.10.0\out\install\win-amd64\bin\rexglue.exe codegen larecomp_manifest.toml
```

**Important rules:**
1. Pass `larecomp_manifest.toml` to codegen, not the config file. The manifest includes `larecomp_config.toml` and establishes module dependency graphs.
2. **Never edit files in `generated/`.** The codegen step overwrites the entire directory.
3. All gameplay, physics, camera, and render patches are declared under `[[midasm_hook]]` in `larecomp_config.toml` and implemented in `src/mc_engine/hooks.cpp`.

---

## Directory Structure and Save Files

- **`assets/` or `MCLA_Game_Files/`**: Game archive files (`default.xex`, `xarchive_cache.rpf`, `xarchive_audio.rpf`).
- **`user_data/`**: Stores save profiles and player progression. Delete the profile folder here to reset save data.
- **`logs/`**: Rolling log files (`logs/larecomp_001.log`, up to 10 rotated 20MB files) and `logs/effective_config.txt`.
- **`stubs.txt`**: Records calls to unmapped guest functions for diagnostic triage.

---

## Diagnostic Logs

- **`logs/larecomp_NNN.log`**: Standard game log output with sequential index rolling.
- **`logs/effective_config.txt`**: Dumps active cvar values, GPU flag results (each marked `ok` or `FAIL`), and environment variable states, written during `OnPostSetup`.
- **`stubs.txt`**: Deduplicated record of unregistered guest function addresses hit at runtime. An empty file means nothing was missed.
- Crash diagnostics: the SEH / SIGABRT handler in `crash_handler.cpp` writes the
  resolved call stack into the **normal log file**, not a separate file.

- **`logs/timing_<date>_<time>_cap<N>.log`**: Written when `MCLA_TIMING_LOG=1`.
  Off by default, costing one already-resolved bool test per frame.

### Frame-time instrumentation

```powershell
$env:MCLA_TIMING_LOG="1"; Start-Process larecomp.exe
```

Once per second it records measured vs engine frame time (a `ratio` far from
1.00 means the simulation and the wall clock disagree), spike counts, a GPU
time breakdown, texture and pipeline cache hit/miss, thread and stall counters,
and the engine's accumulated-time totals with a `<-- FROZEN` marker if they
stop advancing. Every 30 seconds it emits a 1 ms-resolution frame-time
histogram with empty buckets omitted, so clustering is obvious at a glance.

A healthy 60 FPS baseline looks like this - one dominant bucket, thin tails:

```
--- frame-time histogram | window 0.0s..30.0s | 1720 frames | mean 17.44 ms ---
   16 ms |   1625 ################################################
   17 ms |     32 #
```

The GPU counters are single-frame samples taken at the report boundary, not
per-second totals, so treat one line as a spot check and the trend across lines
as the signal. `substep r24` is a rate-invariance check: it must NOT change
with `fps_limit`.

---

## Technical Fixes Reference

### Boot and Platform Initialization

| Problem | Root Cause | Implemented Solution |
| :--- | :--- | :--- |
| Static initializer crash | Static constructors calling unregistered function addresses | Scan static tables (`0x82770010` to `0x827713F0`) and stub unmapped entries in `OnPostSetup` |
| "Dirty Disc" error dialog | Retail disc integrity check in `sub_82130678` | Bypass `0x82130678` with a direct return |
| Unmapped indirect dispatch | Guest script VM using indirect branches (`bctr`/`bctrl`) | Full scan of code region `0x82130000` to `0x827CD054` stubbing unmapped targets; 540+ script natives registered in TOML |
| Missing `t:` drive mapping | Game engine mounting city and art packages to `t:` partition | Register symbolic link `t:` -> `\Device\Harddisk0\Partition1` in VFS |
| Unhandled crash termination | `abort()` terminating without logging guest register state | Dedicated Windows SEH and SIGABRT exception logger in `crash_handler.cpp` |

### Clock Pipeline, Physics, and Camera

| Problem | Root Cause | Implemented Solution |
| :--- | :--- | :--- |
| 2x game speed at 60 FPS | Engine timer overwriting measured delta with 30 Hz constant | Hook `MCLAUseRealDelta` at `0x821BDB58` (jump to `0x821BDC34`) and `MCLAFixedStepPath` at `0x821BDB90` (`[r3+0x58]`) |
| Frozen accumulated time | Bypassing the entire timer block froze `[r3+20]` / `[r3+24]` accumulators | Narrowed hook entry to `0x821BDB58` so time accumulator increments run normally |
| Frame time jitter and racing | Timer function called concurrently across worker threads | Thread-pinned cadence accumulator (`EnforceFrameLimit`) bound to primary thread id |
| Frame burst after streaming pause | Time accumulator building large delta during background load | Bound per-frame tick delta via `MCLAFrameDelta` at `0x821BDAB0` (125 ms default, `MCLA_MAX_FRAME_MS`, clamped to 16-1000 ms) |
| Chase camera snapping at 60 FPS | Camera chase step using per-frame factor instead of continuous time | Continuous-time exponential decay `1.0 - pow(1.0 - k30, dt * 30.0 * scale)` at `0x82320468` and `0x823204F4` |
| Suspension jitter on sharp turns | Ground depth filter stepping discrete alpha at variable frame rates | Continuous-time depth decay `1.0 - pow(0.90, dt * 30.0)` at `0x82563720` |
| Donut animation skipping | Artificial steering sensitivity divisor applied on top of real delta | Removed redundant `Patch_SteeringSensitivity` hook |

### Graphics, Memory, and Ambient Density

| Problem | Root Cause | Implemented Solution |
| :--- | :--- | :--- |
| Excessive DoF blur | Full-screen blur pass active during gameplay and photo mode | Zero Circle-of-Confusion vector at `dofObj + 0xF0` in `Patch_DofComposite` (`0x8260EBB8`). Defaulted to disabled |
| Downtown FPS drops | High draw call count and geometry density in city core | Base LOD distance scaled dynamically via `UpdateCityLODMemory` at `0x827E0DE0` |
| Pedestrian / traffic crowding | Hardcoded spawn caps causing entity queue pressure at 60 FPS | Ambient density tuning hook at `0x826F5CA0` (mcAmbientDensityTuning constructor epilogue, `r3`) scaling spawn, unspawn, cull, pedestrian and parked-car densities across all 32 zones |
| Audio crackle on multicore hosts | Cache flush loop (`FlushDataCache`, `0x821D5510`) skipping memory barrier | Replace 540,000 emulated `dcbf`/`dcbst` loop iterations with a single `std::atomic_thread_fence(memory_order_seq_cst)` |
| Texture cache eviction during driving | Default GPU cache budgets too low for high-resolution rendering | Texture limits increased to 1536MB soft / 2048MB hard in `OnPostSetup` |
