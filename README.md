# LARecomp

> [!CAUTION]
> LARecomp's code is not free to take. It is licensed under GPL-3.0-only with additional terms: if you use any of it in your own recomp, your whole project must be released under the same licence with its source published, and LARecomp must be credited. Copying it into private or uncredited projects is a licence violation. See [License](#license).
> Contributions to the repository are welcome.

A static recompilation of **Midnight Club: Los Angeles (Complete Edition)** for Windows and Linux (x86-64), built on the [ReXGlue SDK](https://github.com/rexglue/rexglue).

Static recompilation translates the Xbox 360 PowerPC code inside the game's `default.xex` into native C++ that compiles and runs on a PC. The game code runs with no CPU emulator and no interpreter in the loop. File I/O, audio and threading go through the ReXGlue runtime; graphics go through LARecomp's own renderer on Direct3D 12 (Windows) or Vulkan, built on [Plume](https://github.com/mzzvxm/plume).

You supply the game data. Nothing from the disc is included here. [How to dump your own disc](https://github.com/mzzvxm/LARecomp/wiki/Dumping-Your-Game).

---

## What you get

The game boots to the menu, free roam and races work, and saves load. On top of that:

**Its own renderer.** It adds HDR, ambient occlusion, upscalers (FSR, DLSS), frame generation, ray traced effects and texture mods; what is offered depends on the graphics card. The old Xenia GPU emulation is kept as a hidden compatibility renderer for PCs where the native one will not start ([documentation/RUNNING.md](documentation/RUNNING.md#pause-menu)).

**Runs properly above 30 FPS.** The original code locks the simulation to a fixed 30 Hz timestep, and the well-known Xenia unlock makes the game run at double speed. LARecomp feeds the simulation the real measured frame time instead, so physics, camera and traffic behave the same at 30, 60, 120 and 144 FPS.

**Smooth frame pacing.** Frame times used to land on a 15.625 ms grid, which is Windows' default timer resolution rather than anything about the display. Raising timer resolution and taking presentation off vsync removed it and gained about 30% throughput.

**Camera and suspension that do not break at high frame rates.** Chase camera lag, chassis roll and the ground depth filter step with continuous-time exponential decay calibrated against the 30 FPS console curve, so they behave identically at any frame rate.

**Ultrawide and tall windows.** A 21:9 or 32:9 window gets a wider view instead of black bars (the vertical field of view stays the game's), and a taller one (16:10 such as 1920x1200 or the Steam Deck's 1280x800, 4:3, 5:4, even a portrait window) shows more above and below instead (the width of the view stays the game's), with the HUD at 16:9 in the middle either way. HUD WIDTH (pause menu, INTERFACE > UI; `hud_width`, applies at once) moves the HUD's corner pieces (minimap, speedometer, race position, lap and time, the phone) out toward the screen edges (the sides of a wide picture, the top and bottom of a tall one): CENTERED (the default) keeps them in the middle, WIDER moves them halfway, FULL puts each as far from its edge as on a 16:9 screen. The file takes any number from 0 to 100 (50 and 100 are WIDER and FULL). ASPECT RATIO (pause menu, DISPLAY; `aspect_ratio`, applies at once) is AUTO by default, the window's own shape; 16:9, 16:10, 21:9 and 32:9 keep that shape whatever the window is and leave pure black bars where the window is not that shape (21:9 on a 16:9 monitor has bars above and below, 16:9 on an ultrawide one at the sides). The Xenia renderer keeps 16:9 with bars in a window taller than that.

**A settings menu inside the game.** Press Start or Escape and open the SETTINGS tab: RECOMP SETTINGS, GRAPHICS, GAMEPLAY, CONTROLLER, INTERFACE and EXTRAS ([documentation/RUNNING.md](documentation/RUNNING.md#pause-menu) lists what each holds). Changes apply immediately and save when you leave the submenu.

**Quality of life.** An ISO install wizard on first launch, save import from Xenia and RPCS3, custom MP3 radio, Discord Rich Presence, a mod loader, a speedometer that switches between mph and km/h, and a language picker for the five translations on the disc: English, Spanish, French, German and Italian (Portuguese too, once a mod supplies its text).

Under the hood there are 420 mid-asm hooks, more than 1,100 settings, and 24,928 function hints driving codegen.

---

## Getting it running

### You need

- ReXGlue SDK 0.11.0 from [LAR-RXSDK](https://github.com/mzzvxm/LAR-RXSDK), branch `LARecomp` (building the SDK also needs the Vulkan SDK)
- CMake 3.25 or newer
- Clang 20.1.8, from LLVM or Visual Studio. MSVC will not build this.
- Ninja
- Your own copy of the game, [dumped from your disc](https://github.com/mzzvxm/LARecomp/wiki/Dumping-Your-Game)

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

Point CMake at the SDK with `-DCMAKE_PREFIX_PATH=<sdk-install>` (an installed SDK) or `-DREXSDK_DIR=<LAR-RXSDK checkout>` (builds the SDK with the project). `generated/` is not in the repository, so run codegen before the first configure:

```powershell
git submodule update --init --recursive
<sdk-install>\bin\rexglue.exe codegen larecomp_manifest.toml
cmake --preset win-amd64-relwithdebinfo -DCMAKE_PREFIX_PATH=<sdk-install>
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

**The first start builds the shaders** from your game files, behind a progress popup: about 15 seconds on an 8-core CPU, up to a minute or two on slower ones. They go in `cache\shaders` next to the executable and are built again only when the game files, the shader compiler or LARecomp's builder change. The Xenia backend does not need them. `larecomp.exe --build_shader_packs` builds them without starting the game (no window; progress on the console, exit code 0 when they are in place): an installer can run it so the first start has nothing to build. After it, `larecomp.exe --precache_pipelines` does the pipelines the game would otherwise compile while the title starts, for your saved MSAA and upscaler choice: it fills your graphics driver's own cache for the game's API (add `--precache_pipelines_backend=vulkan` or `d3d12` for the other one) and, when nothing changed since (same driver, graphics card, settings and files), finishes in about a second.

**Ray traced shadows** (RT SHADOWS, LIGHTING AND SHADOWS; a graphics card with ray queries, on by itself on a fast one) trace the sun's shadows from the city's own triangles, which are built from your game files as well: about 250 MB in `cache\shaders\city`, a few seconds of work once, started by the game in the background the first time they are wanted. `larecomp.exe --build_city_pack` builds it ahead (no window, progress on the console, exit code 0 when it is in place); an installer can run it after `--build_shader_packs`.

### Linux and the Steam Deck

LARecomp runs on Linux and the Steam Deck through Vulkan. Besides a Vulkan driver, a game installed with the LARecomp launcher needs nothing else: the launcher builds it with its own build kit, which brings its own C++ library (libc++). A build made with the system's compiler (by hand, or by the launcher where its kit cannot start) links the system's libstdc++ instead, and that must be **version 13 or newer**. These have it:

- SteamOS 3.5 or newer (the Steam Deck)
- Ubuntu 24.04 or newer
- Fedora 39 or newer
- Arch Linux, kept up to date (and distributions based on it)

Older ones, such as Ubuntu 22.04 and Debian 12, do not. A build that links the system's library then refuses to start and the terminal says something like:

```
./larecomp: /lib/x86_64-linux-gnu/libstdc++.so.6: version `GLIBCXX_3.4.31' not found (required by ./larecomp)
```

The GLIBCXX number in that line can differ. Update the system to a version in the list above, or use a newer distribution.
<!-- CHECK: the exact GLIBCXX number was not read from a Linux binary (none on this PC); on a Linux build, `objdump -T larecomp | grep -o 'GLIBCXX_[0-9.]*' | sort -V | tail -1` gives it. -->

LARecomp needs no GTK. Where it asks for a file (the ISO of the first-run setup, a save to import) it opens the system's file chooser (the desktop portal, or zenity) and, on a system that has neither, shows a box to type the path in.

Online play is not available on Linux yet. The ONLINE row of the SYSTEM LINK menu is greyed out and says so, the server browser (F6) does not open, and `mcla_online_enable` is ignored.

---

## Documentation

The [wiki](https://github.com/mzzvxm/LARecomp/wiki) has the rest:

- [Dumping your game](https://github.com/mzzvxm/LARecomp/wiki/Dumping-Your-Game): getting the game files from your own disc
- [Getting started](https://github.com/mzzvxm/LARecomp/wiki/Getting-Started): first launch, and where settings and logs live
- [Settings reference](https://github.com/mzzvxm/LARecomp/wiki/Settings-Reference): every setting, environment variable and menu row
- [Working on the code](https://github.com/mzzvxm/LARecomp/wiki/Working-on-the-Code): hooks, codegen and the source layout
- [Troubleshooting](https://github.com/mzzvxm/LARecomp/wiki/Troubleshooting)

---

## Credits & Acknowledgments

**[mzzvxm](https://github.com/mzzvxm)**, author and maintainer of **[LARecomp](https://github.com/mzzvxm/larecomp)**, the original static recompilation of Midnight Club: Los Angeles, in development since 2025. This repository is the LARecomp project: the ReXGlue port, the in-game pause menu and settings system, the ISO install wizard, save porting from Xenia and RPCS3, custom music, Discord Rich Presence, the mod loader, and the bulk of the engine hook work.

Also the custom ReXGlue build LARecomp is developed against, published as [LAR-RXSDK](https://github.com/mzzvxm/LAR-RXSDK). Its rendering fixes are what removed the broken car body reflections and the intermittent HUD glitches.

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
- **D3D fence spin-wait throttling (`Patch_FenceSpinThrottle`).** Replaced the unconstrained PowerPC pipeline NOP busy-wait loop (`mr r31, r31`) in `sub_82412F98` (called by `D3DDevice_BlockOnFence`) with host CPU yielding (`YieldProcessor()` and adaptive `SwitchToThread()`), dropping Render Thread CPU load from 98.8% to 82.2% during fence waits.
- **Single-tile un-tiled rendering (`Patch_SingleTile` & `Patch_EdramLimit`).** Raised the internal EDRAM budget clamp from 2,048 tiles (10 MB console limit) to 4,096 tiles (20 MB ReXGlue limit). Guarded `Patch_SingleTile` to strictly target the main scene framebuffer (`r17 == 0`) while leaving offscreen passes native (`r17 != 0`), eliminating 2–3× multi-tile splitting overhead and yielding a 1.5× to 1.6× FPS boost (+15 to +27 FPS in dense areas) without breaking the pause menu or orbital camera. Single tile is now off by default (`single_tile`).
- **Elimination of PM4 profiler overhead in ReXGlue SDK.** Identified and eliminated ~480,000 `QueryPerformanceCounter` calls per second (`PROFILE_SCOPE_US`) issued on every PM4 packet in `rexgpu-xenos`, which consumed 25.1% CPU of the bottleneck `GPU Commands` thread. Compiling out perf counters recovered this overhead, lifting dense area framerates into the 80–88+ FPS bracket.
- **Adaptive texture cache limits & D3D12 primary buffer submission.** Replaced hardcoded texture cache overrides with a universally safe 2560 MB soft / 4096 MB hard baseline that prevents VRAM exhaustion on 6 GB and 8 GB GPUs while respecting user TOML overrides on high-VRAM cards (12 GB–24 GB). Enabled `d3d12_submit_on_primary_buffer_end` to allow asynchronous overlap of GPU rendering with CPU command recording; it was later measured about 7% slower on a GTX 1650 and is now off by default.
- **Guest profiler 144 FPS / GPU Commands call-stack sampling.** Added `profile_gpu_commands.ps1` helper for targeting `GPU Commands` with `RtlVirtualUnwind` stack sampling at 144 FPS / 7.0ms threshold.

**[Foxxyyy](https://github.com/Foxxyyy)**, for reverse-engineering work on **[CodeX.Games.MCLA](https://github.com/Foxxyyy/CodeX.Games.MCLA)**: the RAGE `RSC5` resource format, type layouts and string databases behind the typed structures used here, and the texture tiling and size maths the mod loader ports from its code (`RPF3/Rpf3Crypto.cs`).

**[ReXGlue Team](https://github.com/rexglue/rexglue)**, for the Xbox 360 static recompilation toolkit and runtime.

**NVIDIA DLSS** and **NVIDIA Reflex**. LARecomp uses NVIDIA DLSS Super Resolution through NVIDIA Streamline, and NVIDIA Reflex, on NVIDIA RTX graphics cards. NVIDIA, NVIDIA RTX, DLSS and Reflex are trademarks and/or registered trademarks of NVIDIA Corporation in the U.S. and other countries. NVIDIA's files stay under NVIDIA's licences, which are copied next to the exe with them (`nvngx_dlss_license.txt`, `reflex_license.txt`, `streamline_license.txt`, `streamline_third_party_licenses.md`).

**AMD FidelityFX** and **AMD Anti-Lag 2**. LARecomp uses FidelityFX Super Resolution (FSR 1, FSR 3.1, FSR 4) and CAS for upscaling and sharpening, and AMD Anti-Lag 2. The FidelityFX SDK is under the MIT licence, copied next to the exe with its files (`amd_fidelityfx_license.md`). AMD FidelityFX, FSR and Radeon are trademarks of Advanced Micro Devices, Inc.

**libmspack**, by Stuart Caie (v1.12, [kyz/libmspack](https://github.com/kyz/libmspack)). LARecomp reads the game's compressed files with libmspack's LZX decoder. libmspack is under the GNU LGPL 2.1 and is a separate DLL, `larecomp_mspack.dll` (`liblarecomp_mspack.so` on Linux), next to the exe, so you can replace it with a build of your own. Its licence and a short notice are copied next to it (`larecomp_mspack_license.txt`, `larecomp_mspack_notice.txt`; the repository keeps them in `LICENSES/`), and the library's source is the submodule `thirdparty/libmspack`.

**Zstandard (zstd)**, by Meta Platforms, Inc. and affiliates ([facebook/zstd](https://github.com/facebook/zstd), v1.5.7). LARecomp compresses the ray tracing data it builds on your PC from your game files with zstd, under its BSD 3-Clause licence (Copyright (c) Meta Platforms, Inc. and affiliates; the licence text is `thirdparty/LAR-XenosRecomp/thirdparty/zstd/LICENSE`, and the library is built into the exe).

**ARC DTrade**, who made the LARecomp logo font for the project: the word mark and the LAR app icon.

**Rockstar San Diego**, who made the game.

## License

LARecomp is free software under [GPL-3.0-only](LICENSE) with
[additional terms](LICENSE-ADDITIONAL-TERMS.md).
See [NOTICE](NOTICE) for attribution and third-party license information.
