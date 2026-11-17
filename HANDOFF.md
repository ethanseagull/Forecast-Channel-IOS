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
  n); the log prints fps and busy % every 600 frames. Shaders:
  `tools\build_shaders.bat` after editing `port/gx/shaders/*.hlsl`. Sound plays: `rev_wtr.brsar` is converted at load
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

1. **iOS build in CI** (milestone 6): make the workflow green, then sideload
   the .ipa and test on the iPhone: import (copy 00000001.app and
   00000006-0000000a.app into the app's folder in Files), ASTC textures, audio,
   touch, memory. Decide what "Back to the Wii Menu" does on iOS (quits now).
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
5. **Touch design** (milestone 7, with the iPhone layout): replacements for
   hover highlights, B (back), +/- (globe zoom: pinch), 1/2; bigger hit areas.
