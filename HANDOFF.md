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
- The user decides commits and pushes. Commits use the repo-local identity
  `ForecastOS <244728604+ethanseagull@users.noreply.github.com>` and carry no
  AI co-author / attribution lines (same rule as forecast-3ds).
- No Mac: iOS builds will run on a GitHub Actions macOS runner and be
  sideloaded from Windows (the user has a sideloading method).
- Later: an iPhone-native layout pass (milestone 7 in `PLAN.md`).

## State now (2026-11-16)

- **main = `33cf20c`** (first commit), working tree clean apart from this file.
  No remote, nothing pushed.
- **Milestone 1 done.** `build\windows\forecast.exe` boots: SystemInit, weather
  scene, 5 layouts + 4 fonts, 60 fps, no crash; waits on the "connecting"
  screen (NWC24 is a stub). Draws are counted, not rendered
  (`port/gx/gx_null.cpp`). Sound and the cursor effect are off on purpose
  (files not converted yet; marked `FORECAST_PORT` in `Sound.cpp`,
  `SoundManager.cpp`, `Scene.cpp`, `WeatherScene.cpp`).
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

1. **Hard-coded Wii offsets into `lyt::Pane` / `TextBox`** (`LayoutObj.cpp:47-59`
   name +0xB4, user data +0xC4, visibility flag +0xCF; `LayoutButton.cpp:22-28`
   duplicates them; TextBox color +0xD8 in `LayoutObj.cpp:146`,
   `LayoutButton.cpp:113`, `WeatherAround.cpp:1631`). Wrong on 64-bit and
   writes into the wrong field. Replace with real member access under
   `FORECAST_PORT`. Do before or at the start of milestone 2.
2. **Milestone 2: draw the 2D layouts.** Replace `gx_null.cpp` with an SDL3 GPU
   backend: vertex transform from XF state, TEV stages -> generated shaders,
   texture decode (forecast-3ds `port/gx/gx_tex.cpp` has decoders for RGB5A3,
   I4/I8/IA4/IA8, CMPR; its citro3d upload is not reusable), EFB copies.
   Present on GXCopyDisp (`gx_efb_copy`). Mouse as the Wii pointer through
   KPADRead (currently a stub; `port_pointer()` in `platform.c` is ready).
3. **`nus_fetch.ps1` embeds the Wii common key.** Before any public push (the
   free macOS runner needs a public repo), make it read the key from a local
   file or prompt for it, and drop it from history (it is in `33cf20c`).
4. **Milestone 3: forecast data.** NWC24/VF are stubs; fetch `forecast.bin` /
   `short.bin` from `http://fore.wiilink24.com/...` and feed them where the
   channel's VF/NWC24 reads expect them. `forecast.bin` parsing is 64-bit safe
   (offsets only, per the audit).
5. **Rest of `notes/64bit-audit.md`.** g3d `ResTexPlttInfoData` and PAT0 bind
   arrays (globe, milestone 4); snd TaskManager unit size (with sound); ef
   relocation (cursor effect); make `port_gx_addr` assert on stack / C-heap
   pointers in debug builds.
6. **Sound and cursor effect** (milestone 5): convert brsar / breff / breft at
   load, port AX to SDL3 audio, then remove the port skips.
7. **iOS** (milestone 6): CMake Xcode generator on a GitHub Actions macOS
   runner, unsigned .ipa artifact, touch -> pointer, Files-app import of the
   channel contents, ATS exception for WiiLink's plain HTTP. Watch for `long`
   (64-bit on iOS, 32 on Windows) in anything the audit missed.
