# Handoff

State now and open work, in priority order. Update in place: no dated journal
entries; finished items move out (to `PLAN.md` "Status" if they are a stable
fact about the port). Milestones, architecture and design: `PLAN.md`. 64-bit
hazards found by audit: `notes/64bit-audit.md`.

## What this is

ForecastOS: the Wii Forecast Channel (HAFE, USA v7) ported from the
doldecomp/forecast decompilation to iOS (arm64) through SDL3. Developed as a
Windows x64 build first; iOS comes at milestone 6. No Nintendo code or assets
ship: the user's own channel files are imported.

Sibling project `C:\forecast-3ds` (New 3DS port of the same decomp) was the
starting point. It was only ever run in Azahar, never on a real 3DS, and its
GX code is uncommitted and never drew anything. Copy from it, never edit it.

**User's policy.**
- Make the technical calls and explain them briefly; the user says "you tell
  me" to option lists. Do installs and downloads for them when the tools allow.
- Commit finished work without asking (user's standing go-ahead, 2026-11-16);
  ask before pushing, adding a remote or creating a GitHub repo. Commits use
  the repo-local identity
  `ForecastOS <244728604+ethanseagull@users.noreply.github.com>` and carry no
  AI co-author / attribution lines (same rule as forecast-3ds).
- No Mac: iOS builds will run on a GitHub Actions macOS runner and be
  sideloaded from Windows (the user has a sideloading method).
- Later: an iPhone-native layout pass (milestone 7 in `PLAN.md`).

## State now (2026-11-16)

- **main**: milestones 1-5, texture compression, iOS preparation; pushed to
  the private repo https://github.com/ethanseagull/ForecastOS (remote `origin`).
  History was rewritten once (2026-11-16) to drop the Wii common key; the key
  is in no commit or object. The iOS workflow (`.github/workflows/ios.yml`)
  runs on every push and uploads `ForecastOS-ipa` (unsigned). Private repo:
  macOS minutes count 10x against the free plan's 2,000 a month.
- **Milestones 2-4 done** (`PLAN.md` "Status"). `build\windows\forecast.exe`
  downloads the forecast from WiiLink, runs the first-run setup (date check,
  region, city), shows real weather on the 2D screens and draws the globe
  (`earth.brres` converted at load; upscaled texture packs from `<data>/textures`), on SDL3 GPU (D3D12), 60 fps, with touch
  (left mouse) as the Wii pointer and keys for the other Remote buttons
  (- / + zoom, Esc = B, Home). "Back to the Wii Menu" quits. Setup choices
  persist in the data directory, so later runs start in the weather scene.
  Automated runs: `FORECAST_HIDDEN=1`, `FORECAST_DUMP_FRAME=n[,m]` (writes
  `efb_<n>.bmp`), `FORECAST_TOUCH=frame:x:y:frames[;...]` (the Globe button is
  at 0.85:0.9 on the weather pages), `FORECAST_BUTTON=frame:minus|plus|b|home|1|2:frames[;...]`,
  `FORECAST_LOG_DRAWS=n` (TEV, indirect and blend state of every draw in frame
  n), `FORECAST_QUIT_FRAME=n` (quit after frame n), `FORECAST_GPU_TIME=1`
  (wait for each frame's GPU work and log its time every 120 frames),
  `FORECAST_FINGERS=frame:frames:x1:y1:x2:y2:x1':y1':x2':y2'[;...]` (two scripted
  fingers, window-relative), `FORECAST_WINDOW=WxH` (e.g. 1704x786 for the iPhone's
  shape; hidden runs default to 1280x720), `FORECAST_FILL=0` (black bars instead
  of the screen fill), `FORECAST_FILL_2D=0` (keep 2D draws in the frame),
  `FORECAST_LOG_FILL=1`; with the fill active a dump also writes `screen_<n>.bmp`
  (what the window shows; `efb_<n>.bmp` stays the game's frame); the log
  prints fps and busy % every 600 frames. Shaders:
  `tools\build_shaders.bat` after editing `port/gx/shaders/*.hlsl`.
  Specialized TEV shaders (`PLAN.md` "Status"): a pixel shader per TEV
  configuration, generated (`port/gx/gx_tevgen.cpp`) and compiled at run time
  (Windows: the SDK's `dxcompiler.dll`; Apple: MSL through SDL), the
  ubershader until it is ready. `FORECAST_SPECIALIZED=0` turns them off,
  `=sync` compiles on first use (every draw specialized; for comparing dumps),
  `FORECAST_SPECIALIZED_DUMP=dir` writes the sources and keys. Keys and
  pipelines seen are kept in `<data>/tev_shaders.bin` and built at start-up.
  `tools\tevgen\build.bat <dir>` compiles samples and checks on the GPU that
  they draw exactly what the ubershader draws; change `gx_ps.hlsl` and the
  generator together. Sound plays: `rev_wtr.brsar` is converted at load
  (`port/res/brsar_swap.cpp`) and AX/AI are emulated (`port/sdk/ax.c`, `ai.c`)
  onto an SDL3 audio stream; `FORECAST_MUTE=1` silences it (frames still run),
  `FORECAST_AUDIO_RAW=file` captures the mix (s16 stereo 32 kHz). The cursor effect is
  dropped for good (`Scene.cpp`): the targets are touchscreens, so no cursor
  is drawn; touch drives the Wii pointer underneath (`PLAN.md` item 4).
- **Wii match:** with `patches/decomp-port.patch` applied the decomp's DOL
  still matches (`build\tools\dtk.exe shasum -c config\HAFE\build.sha1`: OK).
- **Local-only, not in git** (rebuild with the commands below):
  - `HAFE_v7/`: clean v7 contents from NUS (`00000001.app` SHA-1 3c3bdec3...).
    The user's Dolphin NAND copy is WiiLink-patched and does not match.
  - `forecast-decomp/`: checkout at 92a5782 with the patch applied and
    `orig/HAFE/00000001.app` in place. It holds the live decomp edits:
    regenerate the patch after changing it (command below).
  - `third_party/SDL3/`: SDL 3.4.20 VC devel (SHA-256 checked).
  - `build/windows/`, `build/windows/data/` (unpacked assets, `boot.log`).
- Upscaled texture packs (Dolphin format) for later: `C:\WiiPC\textures-png-backup\WiiPC-Forecast`
  (UI, 214 PNGs) and `...\WiiPC-Earth` (globe, 28 PNGs), made in an earlier
  WiiPC session.

## Commands

From `C:\ForecastOS`, PowerShell:

```
.\tools\setup_decomp.ps1                      # fresh checkout + patch (offline: -Source C:\forecast-3ds\forecast-decomp)
copy HAFE_v7\00000001.app forecast-decomp\orig\HAFE\
cd forecast-decomp; py configure.py; py -m ninja; cd ..   # also verifies the Wii match
.\tools\extract_assets.ps1                    # HAFE_v7 -> build\windows\data\content
tools\build_windows.bat                       # Debug; "Release" for optimised
build\windows\forecast.exe                    # log: build\windows\data\boot.log
```

- Save decomp edits into the patch:
  `git -C forecast-decomp -c core.safecrlf=false diff --ignore-cr-at-eol > patches\decomp-port.patch`
  (write it as UTF-8; check it applies to a fresh checkout).
- New unported SDK symbols at link: delete `port\sdk\autostubs.c`, build,
  then `py -I tools\gen_stubs.py build_log.txt build\windows <llvm-nm.exe>`
  (redirect the build to `build_log.txt`), build again.
- Crash: `build\windows\crash.txt` has exe offsets;
  `llvm-symbolizer --obj=forecast.exe --relative-address <offsets>`
  (LLVM tools are in the VS Build Tools `VC\Tools\Llvm\x64\bin`).

**Gotchas.**
- devkitPro's MSYS2 on PATH shadows `cmd`, `cmake`, `ninja`: use
  `tools\build_windows.bat` (clean PATH + vcvars64), or `$env:ComSpec` for cmd.
- Every decomp change goes under `FORECAST_PORT` (or the `macros.h` cast
  macros, which are the original casts on the Wii), then re-check the Wii
  match. CodeWarrior compiles signed and unsigned casts differently: keep a
  `(s32)` where the original had one.
- Windows PowerShell 5.1 splits multi-line strings passed to native programs:
  give `git commit` a message file (`-F`).
- The shells sometimes have no network (DNS fails); retry, or give the user a
  script to run. lldb from VS needs python311.dll (not installed): use the
  crash handler instead.

## Open work, in priority order

1. **iOS: works on the user's iPhone 15 Pro (iOS 26.3.1), 2026-11-16.** Their feedback:
   - Lag: fixed. With specialized TEV shaders (MSL, every draw specialized from the first frame
     thanks to `tev_shaders.bin`), the ~2x faster vertex transform and the 2x EFB, the phone
     holds a steady 60 fps on the globe (`boot.log` 2026-11-17; before: 43-48 fps for the first
     ~15 s). If the Metal path ever misbehaves: `gx_spec_init` picks the defaults
     (`FORECAST_SPECIALIZED=main` / `=0`; a sideloaded app has no environment, so change the
     default and rebuild).
   - FORE000006 ("Unable to download the latest information") on some launches: WiiLink's
     `short.bin` carries an expiry time (`ShortHeader::mTime`, UTC minutes since 2000) and the
     server sometimes keeps serving it minutes past that; the channel rejects it ("short check
     -2" in `boot.log`), as a Wii would. It clears when WiiLink regenerates the file. A port-side
     grace period was offered; the user didn't take it up.
   - Milestone 7 (2026-11-17, not yet on the phone): touch gestures (`port/sdk/kpad.c`: tap
     shows the globe's bars, pinch zooms, two-finger swipe = Up/Down) and the screen fill
     (`PLAN.md` "Milestone 7"). The user asked that nothing important sit under the Dynamic
     Island: UI stays in the 16:9 frame, only scenery goes into the side strips.
   - Reinstalling the .ipa wipes the app's Documents (their sideloading tool reinstalls from
     scratch), so the six channel files must be copied in again. Options: an in-app file picker
     (UIDocumentPicker) that imports from anywhere in Files, or keeping a copy outside the app.
   Crashes write `*** crash` + exe offsets to `boot.log`; iOS's own reports (Settings > Privacy
   & Security > Analytics Data) symbolize with the CI's binary.
2. **Milestone 4 loose ends.** Compare with Dolphin (same WiiLink-patched
   channel): the globe's water and relief shading (indirect texturing and
   environment texgens, judged by eye only), and the left part of the forecast
   pages' title belt, which looked empty in dumps.
   Performance (Release, this PC): 60 fps, about 30% of each frame busy in the
   globe scene, with or without texture packs. Measure in Release: Debug (-O0)
   is 2-3x slower and shows the globe near its frame budget.
   Renderer gaps that log once if used: fog, z textures, EFB-copy format
   conversion, logic ops, the indirect "unmodified texcoord for LOD" bit
   (textures have 1 mip level).
   Memory: decoded images are no longer kept (the backend decodes when it
   uploads); the globe's 28 1024x1024 tiles are about 120 MB of RGBA8 on the
   GPU, and with the Earth pack (2048x2048) about 470 MB. iOS will want a
   compressed GPU format (BC1/ASTC) or smaller tiles there.
3. **Rest of `notes/64bit-audit.md`.** PAT0 bind arrays (no PAT0 in
   earth.brres; only if another file needs them); make
   `port_gx_addr` assert on stack / C-heap pointers in debug builds. The ef
   items are moot (cursor effect dropped).
4. **Texture compression: check ASTC on an iPhone** (milestone 6). The GPU-side
   ASTC path cannot run on this PC (no ASTC on desktop GPUs); its blocks were
   verified against an independent decoder (`tools/texcodec_test`). BC1 runs here.
5. **Touch design, rest** (milestone 7): B (back) and 1/2 have no gesture yet; bigger hit
   areas; a pressed state where the Wii relied on hover highlights; text size at phone scale.
