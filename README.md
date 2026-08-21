<p align="center">
  <img src="assets/larecomplogo.png" alt="LARecomp" width="480">
</p>

# LARecomp

A static recompilation of **Midnight Club: Los Angeles (Complete Edition)** for Windows x86-64, built on the [ReXGlue SDK](https://github.com/rexglue/rexglue).

Static recompilation translates the Xbox 360 PowerPC code inside the game's `default.xex` into native C++ that compiles and runs on a PC. There is no emulator and no interpreter in the loop. File I/O, GPU commands, audio and threading go through the ReXGlue runtime.

You supply the game data. Nothing from the disc is included here.

---

## What you get

The game boots to the menu, free roam and races work, and saves load. On top of that:

**Runs properly above 30 FPS.** The original code locks the simulation to a fixed 30 Hz timestep, and the well-known Xenia unlock makes the game run at double speed. LARecomp feeds the simulation the real measured frame time instead, so physics, camera and traffic behave the same at 30, 60, 120 and 144 FPS.

**Smooth frame pacing.** Frame times used to land on a 15.625 ms grid, which is Windows' default timer resolution rather than anything about the display. Raising timer resolution and taking presentation off vsync removed it and gained about 30% throughput.

**Camera and suspension that do not break at high frame rates.** Chase camera lag, chassis roll and the ground depth filter step with continuous-time exponential decay calibrated against the 30 FPS console curve, so they behave identically at any frame rate.

**A settings menu inside the game.** Press Start, open Options, and there are eight tabs: ReXGlue Settings, Recomp Settings, Performance, Fidelity FX, Debug Camera, Time of Day, Carbon Fiber and Languages. Changes apply immediately and save when you leave the submenu.

**Quality of life.** An ISO install wizard on first launch, save import from Xenia and RPCS3, custom MP3 radio, Discord Rich Presence, a mod loader, a speedometer that switches between mph and km/h, and a language picker that reaches the German and Italian translations the retail NTSC/U build region-gates out.

Under the hood there are 112 mid-asm hooks, 155 settings, and 24,927 named function hints driving codegen.

## Known issues

| Issue | Detail |
|---|---|
| Dithered alpha on shadows and foliage | Visible as a dither pattern on shadow edges and on vegetation leaves. Cause not pinned down: either the GPU plugin's handling of the dither pattern, or the game's own shaders dithering at a pattern scale that assumes the 1280x720 console output and does not hold at higher resolutions. This is the only rendering issue left; car body reflections and the occasional HUD glitches were both fixed by the custom ReXGlue build LARecomp uses. |
| Intro movies play fast | Playback pacing follows the frame rate unlock. Capping to 30, 45 or 60 FPS produces identical speed, so a frame cap does not help. Press A to skip, or turn on `skip_intro`. |
| Frame rate collapses below 10 FPS after long sessions | Traced to a bug in the SDK's vblank timer, not to this project. See below. |

### The frame rate collapse

After anywhere from 7 to 30 minutes, the frame rate could fall under 10 FPS and stay there for the rest of the session. It is fixed by a workaround while the SDK issue is addressed upstream:

```powershell
Start-Process larecomp.exe -ArgumentList "--clock_no_scaling=true"
```

The cause is an unsigned underflow in the SDK's vblank catch-up loop. Once the guest tick clock steps backwards by even one tick, the loop's exit condition can never be satisfied again, and it dispatches guest interrupts continuously. Measured: 1,010 vblank interrupts per second while healthy, 11,766,658 per second once stuck. `clock_no_scaling=true` makes the guest tick count a plain function of the host clock, which removes the backwards step. A 33 minute test run with it set never dropped below 24 FPS.

Whether the flag costs frame rate is **not settled**. It does not change the interrupt load: vblank rate under it averaged 1,014 per second against a 1,010 baseline. It does change what `QueryGuestSystemTime` returns, so anything pacing off guest system time behaves differently. If you want to know for certain, run the same route twice with `MCLA_TIMING_LOG=1` and compare the histograms.

Full write-up in [`TECHNICAL_NOTES.md`](documentation/TECHNICAL_NOTES.md).

---

## Getting it running

### You need

- ReXGlue SDK 0.10.0
- CMake 3.25 or newer
- Clang, from LLVM or Visual Studio. MSVC will not build this.
- Ninja
- Your own copy of the game, extracted from disc

### Game data

Point LARecomp at a folder holding `default.xex` and the `xarchive_*.rpf` archives:

```
MCLA_Game_Files/
  default.xex
  xarchive_cache.rpf
  xarchive_audio.rpf
  ...
```

It looks for that folder on its own, in this order:

1. `--game_data_root` on the command line
2. The `MCLA_GAME_DATA` environment variable
3. Walking up from the executable, checking `MCLA_Game_Files/`, `game/`, `assets/` and the directory itself at each level

A directory only counts if it has `default.xex` **and** at least one archive next to it. A half-finished install is rejected at startup rather than failing later with something unhelpful. If nothing is found, the ISO install wizard offers to set it up.

Codegen is separate: `larecomp_manifest.toml` points at `../MCLA_Game_Files/default.xex`, relative to the repo. That path is only read by `rexglue codegen`, never at runtime.

### Build

```powershell
cmake --preset win-amd64-relwithdebinfo .
cmake --build out/build/win-amd64-relwithdebinfo
```

The executable lands in `out/build/win-amd64-relwithdebinfo/larecomp.exe`.

In Visual Studio, open the repo folder, pick **Windows AMD64 RelWithDebInfo**, and run.

### Play

```powershell
cd out\build\win-amd64-relwithdebinfo
$env:REX_LOG_LEVEL="warn"; Start-Process larecomp.exe
```

**Set `REX_LOG_LEVEL=warn`.** Non-Release builds log at `trace`, which is roughly 7,500 lines and 1.4 MB of synchronous disk writes per second during gameplay. That is enough to cause stutter on its own.

Everything else is in the pause menu. It defaults to 60 FPS with real frame delta on and vsync off.

---

## Settings

Most settings are cvars, not environment variables. Set them in the pause menu, on the command line as `--name=value`, or in `larecomp.toml` beside the executable.

Two rows that sound alike but are not:

| Setting | What it does |
|---|---|
| **REAL FRAME DELTA** | Feeds the simulation the measured frame time instead of the fixed 30 Hz timestep. This is what makes high frame rates correct. It is not a frame rate setting. |
| **FPS LIMIT** | The actual cap: 30, 60, 120, 144 or uncapped. |

Turning REAL FRAME DELTA off puts the engine back on its original 30 Hz timestep. The hitch clamp, the FPS cap and the city LOD scale keep working either way.

These are the environment variables the build reads. Everything else moved to cvars:

| Variable | Default | Purpose |
|---|---|---|
| `REX_LOG_LEVEL` | `trace` on non-Release | Set to `warn`. See above. |
| `MCLA_GAME_DATA` | auto-detect | Folder holding `default.xex`. |
| `MCLA_FPS_CAP` | unset | Overrides the `fps_limit` cvar. |
| `MCLA_MAX_FRAME_MS` | `125` | Caps how far one frame can advance the clock. Clamped to 16-1000 ms; cannot be turned off. |
| `MCLA_TIMING_LOG` | off | `1` writes frame-time stats and a histogram. |
| `MCLA_TEX_SOFT` / `MCLA_TEX_HARD` / `MCLA_TEX_RTT` | `1536` / `2048` / `64` | Texture cache limits in MB. |
| `MCLA_VSYNC` | `false` | Leave off. On costs about 30% throughput. |
| `MCLA_REFRESH_RATE` | `60` | Guest video mode refresh rate. |
| `MCLA_ALLOW_INVALID_FETCH` | `true` | Set `false` if HUD or minimap glitches appear. |
| `MCLA_NO_STUB_SWEEP` | `0` | `1` skips stubbing unmapped addresses at startup, saving about 400 ms. Only safe while `stubs.txt` stays empty. |
| `LARECOMP_LOG_FILE` | auto | Log file path. |

`logs/effective_config.txt` is written every launch and lists what actually took effect, including an `ok` or `FAIL` line for each GPU flag. Check it first when a setting seems to do nothing.

---

## Working on the code

Engine patches are `[[midasm_hook]]` entries in `larecomp_config.toml`, implemented in `src/mc_engine/hooks.cpp`:

```toml
[[midasm_hook]]
name = "MCLAFixedStepPath"
address = 0x821BDB90
after_instruction = true
registers = ["r3", "f11"]
```

The hook function takes only the registers named in `registers`, by reference, with ordinary C++ linkage. Not `extern "C"`, not `(ctx, base)`. Float registers arrive as `PPCRegister&` with the value in `.f64`. Returning `bool` drives `jump_address_on_true`.

After changing hooks or hints:

```powershell
E:\MCLA\rexglue-sdk-0.10.0\out\install\win-amd64\bin\rexglue.exe codegen larecomp_manifest.toml
cmake --build out/build/win-amd64-relwithdebinfo
```

Pass the **manifest**, not the config. The manifest includes the config and sets up module dependencies.

**Never edit anything in `generated/`.** Codegen rewrites the whole directory. Hooks survive regeneration because they live in the config and in `hooks.cpp`; hand edits do not.

### Layout

```
larecomp/
  README.md
  documentation/
    RUNNING.md              # run and debug quick reference
    TECHNICAL_NOTES.md      # engine findings, corrections, gotchas
    MCLA_WORKPLAN.md        # investigation log and open work
  src/
    larecomp_app.h          # paths, cvars, GPU flags, stub sweep
    mc_engine/              # hooks, pause menu, HUD, music, mod loader
    isoinstaller/           # first-run ISO install wizard
    saveporter/             # Xenia and RPCS3 save import
    discord_rpc/
    mcla_rage_types.h       # big-endian RAGE structs, offsets static_asserted
  assets/
    gamecontrollerdb.txt    # SDL controller mappings, copied next to the exe
  generated/                # REGENERATED BY CODEGEN, never hand-edit
  larecomp_manifest.toml    # codegen entry point
  larecomp_config.toml      # function hints and hook declarations
```

---

## Credits & Acknowledgments

**[mzzvxm](https://github.com/mzzvxm)**, author and maintainer of **[LARecomp](https://github.com/mzzvxm/larecomp)**, the original static recompilation of Midnight Club: Los Angeles, in development since 2025. This repository is the LARecomp project: the ReXGlue port, the in-game pause menu and settings system, the ISO install wizard, save porting from Xenia and RPCS3, custom music, Discord Rich Presence, the mod loader, and the bulk of the engine hook work.

Also the custom ReXGlue build LARecomp is developed against, which is not published yet. Its rendering fixes are what removed the broken car body reflections and the intermittent HUD glitches, leaving dithered alpha as the only rendering issue outstanding.

**[BadassBaboon](https://github.com/BadassBaboon)** and the **[midnightclub fork](https://github.com/BadassBaboon/midnightclub)**.

Frame timing and high-frame-rate work, developed in the fork and carried into LARecomp:

- Real frame delta injection. The 2x speed bug needed hooks on **two** fixed-timestep paths, not the one the Xenia patch addresses, and it had to keep the timer reset guard that the Xenia patch skips. Dropping that guard caused a loud audio blowout.
- Frame pacing. Identifying the 15.625 ms grid as Windows timer granularity, and establishing that both `timeBeginPeriod(1)` and vsync off are required.
- The wall-clock frame limiter, deliberately not vblank-based so pacing stays continuous.
- Continuous-time chase camera, chassis roll and ground depth smoothing.
- Ambient traffic and pedestrian density tuning, with the corrected `mcAmbientDensityTuning` field offsets.
- Texture cache sizing that takes `texture_cache_misses` to zero.

Fixes made directly in LARecomp:

- **Unclickable setup wizards.** ReXGlue moved windowing and input to SDL3, but the ISO installer and the save import wizard still pumped messages with Win32 `PeekMessageW`, which never drains SDL3's event queue. ImGui received no mouse events at all, so the buttons did nothing.
- **Access violation on the aspect ratio patch.** `flt_8201E7EC` sits in `.rdata`, which the XEX loader maps read-only. Once `g_guest_mem` was actually populated, the patch stopped exiting early and wrote straight into a read-only page. It now unprotects the page first.
- **Ambient density tuning did nothing.** The hook was placed after the `density_tuning.xml` parse, on a code path that never executes. Moved to the constructor epilogue at `0x826F5CA0`, where all 32 zones apply.
- **Pause menu crash on SAVE SETTINGS.** The custom row installer clears the list's table-source pointer, and the game's menu update path dereferences it without a null check.
- **Settings that disabled each other.** The FPS cap and the city LOD slider were gated behind the frame delta option, so turning that off silently switched both off too. The option was also named "60 FPS" while sitting directly above "FPS LIMIT"; it is now REAL FRAME DELTA.
- **Save button replaced with auto-save**, since every setting already applied immediately and the button existed only to write the file.
- **Log noise.** A routine "entry not found, create it" branch logged at warning level, producing about 180 warnings per session and burying the real ones. Now 10.
- **Clean builds were broken.** `enable_language(RC)` picked Microsoft `rc.exe`, which cannot parse clang's line markers and fails with `RC2019`.
- Frame-time instrumentation, and the diagnosis of the vblank interrupt storm behind the long-session frame rate collapse.

**[Foxxyyy](https://github.com/Foxxyyy)**, for reverse-engineering work on **[CodeX.Games.MCLA](https://github.com/Foxxyyy/CodeX.Games.MCLA)**: the RAGE `RSC5` resource format, type layouts and string databases behind the typed structures used here.

**[ReXGlue Team](https://github.com/rexglue/rexglue)**, for the Xbox 360 static recompilation toolkit and runtime.

**Rockstar San Diego**, who made the game.
