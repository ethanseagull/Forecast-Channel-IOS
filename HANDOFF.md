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

- **main**: milestones 1-3; `git log` for the tip. No remote, nothing pushed.
- **Milestones 2 and 3 done** (`PLAN.md` "Status"). `build\windows\forecast.exe`
  downloads the forecast from WiiLink, runs the first-run setup (date check,
  region, city) and shows real weather on the 2D screens, drawn on SDL3 GPU
  (D3D12), with touch (left mouse) as the Wii pointer. The globe is not drawn:
  `earth.brres` is big-endian and nothing converts it yet (milestone 4); the
  scene runs without it. Setup choices persist in the data directory, so later
  runs start in the weather scene. Automated runs: `FORECAST_HIDDEN=1`,
  `FORECAST_DUMP_FRAME=n[,m]` (writes `efb_<n>.bmp`),
  `FORECAST_TOUCH=frame:x:y:frames[;...]`, `FORECAST_LOG_DRAWS=n` (TEV and
  blend state of every draw in frame n). Shaders:
  `tools\build_shaders.bat` after editing `port/gx/shaders/*.hlsl`. Sound is off until its archive is converted
  (`Sound.cpp`, `SoundManager.cpp`, `WeatherScene.cpp`). The cursor effect is
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

1. **`nus_fetch.ps1` embeds the Wii common key.** Before any public push (the
   free macOS runner needs a public repo), make it read the key from a local
   file or prompt for it, and drop it from history (it is in `33cf20c`).
2. **Milestone 4: the globe.** Convert `earth.brres` (MDL0, TEX0, PLT0 and the
   animations) to native byte order at load, like `port/res/lyt_swap.cpp`;
   then remove the "no globe" guards (`SimpleModel.cpp`, `PORT_HAS_EARTH_MDL`
   in `SimpleGlobe.cpp`). Includes the audit's g3d items (item 3).
   The texture packs come with it. Compare frames with Dolphin, which has the
   same WiiLink-patched channel.
   Renderer gaps that may show up there: indirect texturing
   (emboss), fog, z textures, EFB-copy format conversion, logic ops. Each logs
   once. Every image in use is also re-hashed each frame, because the channel
   calls GXInvalidateTexAll every frame (about 80 KB a frame now); profile
   that once the globe draws.
3. **Rest of `notes/64bit-audit.md`.** g3d `ResTexPlttInfoData` and PAT0 bind
   arrays (globe, milestone 4); snd TaskManager unit size (with sound); make
   `port_gx_addr` assert on stack / C-heap pointers in debug builds. The ef
   items are moot (cursor effect dropped).
4. **Sound** (milestone 5): convert brsar at load, port AX to SDL3 audio, then
   remove the port skips.
5. **iOS** (milestone 6): CMake Xcode generator on a GitHub Actions macOS
   runner, unsigned .ipa artifact, SDL touch events into the same pointer path
   as the mouse, Files-app import of the channel contents, ATS exception for
   WiiLink's plain HTTP. Watch for `long` (64-bit on iOS, 32 on Windows) in
   anything the audit missed.
6. **Touch design** (milestone 7, with the iPhone layout): replacements for
   hover highlights, B (back), +/- (globe zoom: pinch), 1/2; bigger hit areas.
