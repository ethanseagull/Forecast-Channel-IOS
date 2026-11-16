# ForecastOS: Forecast Channel for iOS

A port of the Wii Forecast Channel to iPhone/iPad, built from the
[doldecomp/forecast](https://github.com/doldecomp/forecast) decompilation (HAFE, USA v7).
No Nintendo code or assets ship with the app: the user imports their own channel files.

## Inputs on this PC

| What | Where | Notes |
|---|---|---|
| Channel contents, unpatched v7 | `HAFE_v7/` (from NUS via `nus_fetch.ps1`) | `00000001.app` SHA-1 `3c3bdec3…` = the decomp's expected DOL |
| 3DS port of the same decomp | `C:\forecast-3ds` | Source of the reusable port layer (see below). Has uncommitted GX work; copy from it, never edit it |
| Upscaled textures | `C:\WiiPC\textures-png-backup\WiiPC-Forecast` (214 PNGs, UI), `...\WiiPC-Earth` (28 PNGs, globe) | Dolphin pack naming `tex1_WxH_<hash>_<fmt>.png` |
| Toolchain | VS Build Tools 2026: clang-cl 22.1.3, MSVC 14.51, bundled CMake + Ninja | Verified: SDL3 GPU test runs on Direct3D 12, 64-bit |
| SDL3 | `third_party/SDL3` (3.4.20 VC devel, SHA-256 checked, gitignored) | |

**Build environment gotcha:** devkitPro's MSYS2 is on PATH and shadows `cmd`, `cmake` and
`ninja`. Windows builds must start from a clean PATH, then `vcvars64.bat`, then CMake with
`-G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl`.

### What each content file is (from forecast-3ds notes)

| app | contents | use |
|---|---|---|
| 01 | main DOL | replaced by our build |
| 02 | Opera browser | drop |
| 03 | system font (.ttc) | maybe |
| 04/05 | icon / banner | drop |
| 06 | Home Menu button layouts | drop (iOS has its own) |
| 07 | font archives | needed |
| 08 | earth.brres (globe model + 29 1024x1024 CMPR tiles) | needed |
| 09 | main layouts, TPLs, fonts, sounds, cursor effect | needed |
| 0a | Opera pages | drop |

## Architecture

The channel code, NW4R and the SDK pieces that are plain C stay as decompiled. Everything the
Wii hardware did goes through a platform layer:

```
channel code + nw4r (decomp, patched)
        |
  SDK shim (OS, thread, MTX, MEM, ARC, CNT/NAND -> files, KPAD, AX, NWC24)
        |
  GX: write-gather pipe -> command processor (BP/CP/XF state, vertex decode)
        |
  renderer: SDL3 GPU  ->  Metal on iOS, Direct3D 12 / Vulkan on Windows
```

**Why SDL3 GPU:** one renderer that runs on Windows (where we can build, debug and test) and on
iOS (Metal) unchanged. iOS then only adds packaging, touch, file import and networking.

### Reused from forecast-3ds
- `patches/decomp-port.patch`: makes the decomp build outside the Wii toolchain.
- `port/res/`: endian swap-on-load for layouts, animations and fonts. ARC/TPL swaps in the SDK shims.
- `port/gx/gx_fifo.c`, `gx_cp.cpp`, `port_gx_pipe.h`: the GX command stream and register state.
- `port/gx/gx_tex.cpp`: Wii texture decoders (the citro3d upload part gets replaced).
- `port/sdk/*`: OS, thread, mtx, cnt, sc, libc extras (3DS calls -> SDL3/pthreads).
- `tools/gen_stubs.py`, `tools/decomp-linked-units.txt`: unit list and auto-stubbing.

### New for this port
1. **64-bit.** iOS is arm64 only; the 3DS port is 32-bit. The GX stream and the decomp store
   addresses in 32-bit fields. Plan: all game heaps live in one reserved arena under 4 GB and
   GX addresses are offsets into it, with a small table for static data. Audit structs that
   overlay file data for pointer members.
2. **Renderer.** One precompiled TEV ubershader that decodes the raw BP registers at run time
   (all 16 stages; no shader compiler on the device), vertices transformed on the CPU,
   EFB copies -> GPU textures. Still to do: indirect/bump texturing for the globe's emboss,
   fog, z textures.
3. **Texture replacement.** Hash each texture the way Dolphin does (XXH64 of the texel data, plus
   the palette for paletted formats) and load the matching PNG from the WiiPC packs.
4. **Input: touch, no cursor.** The channel's UI is built on the Wii pointer (hit tests,
   hover, globe drag, city labels in ~12 files), so a touch drives the pointer underneath:
   finger down = pointer at that spot (KPAD DPD position in layout space) with A held,
   lift = A released, no finger = pointer off screen. Nothing is drawn for it: the cursor
   and its sparkle effect (the only user of `nw4r::ef`) are dropped, not ported.
   Mouse on Windows behaves the same (button down = touch). What touch lacks is designed
   in milestone 7: hover feedback, B (back), +/- (globe zoom), 1/2.
5. **Forecast data.** NWC24 -> HTTP fetch of `forecast.bin` / `short.bin` from WiiLink
   (`http://fore.wiilink24.com/...`). iOS needs an App Transport Security exception for
   plain HTTP. Written where the channel expects its downloaded files.
6. **Audio.** AX voices -> SDL3 audio stream mixer.
7. **Screen shape.** The Wii layout is 4:3 / 16:9; iPhones are about 19.5:9. Start pillarboxed;
   the real fix is its own milestone (iPhone layout, below).
8. **App packaging.** First run imports the channel files (Files app / share sheet) and
   optionally the texture packs; nothing is bundled.

## Building iOS on Windows

No Mac available. Xcode only runs on macOS, so iOS builds run on a **GitHub Actions macOS
runner**: CMake generates an Xcode project, the runner builds an unsigned `.ipa` and uploads it
as an artifact, and the user's sideloading tool signs and installs it from Windows.
- Public repo: macOS runner minutes are free. Private repo: macOS minutes count 10x against the
  free plan's monthly allowance, so keep iOS builds to tagged/manual runs.
- The repo never contains Nintendo files, so the runner never sees them either.
- No iOS Simulator on Windows: everything up to milestone 5 is built and tested on Windows, and
  iOS-only code (touch, file import, networking) stays thin and isolated.

## Milestones
1. **Windows host build.** Decomp + port layer compile with MSVC (64-bit), boots to the first
   layout load with stubs. Needs SDL3 downloaded.
2. **2D layouts draw.** GX interpreter -> SDL3 GPU; the main menu renders and responds to the mouse.
3. **Forecast data** from WiiLink, real weather on screen.
4. **Globe** (g3d brres byte-swap, CMPR, emboss) and the texture packs.
5. **Audio, polish.**
6. **iOS target.** Touch, file import, packaging, sideload.
7. **iPhone layout.** Make it feel native rather than a pillarboxed TV screen:
   - Landscape first: the channel already has a 16:9 mode; extend the layouts' edge-anchored panes
     to ~19.5:9 instead of letterboxing, and keep content out of the notch / Dynamic Island and
     home-indicator safe areas.
   - Touch: Wii buttons were sized for a pointer; enlarge hit areas (and maybe art) for fingers.
     Drag to spin the globe and scroll lists, pinch to zoom (Wii: +/-), a back gesture or
     button (Wii: B), and a pressed state where the Wii relied on hover highlights.
   - Text legibility at phone size (font scale per layout).
   - Portrait mode would need new or rearranged layouts (BRLYT edits or code-side placement);
     decide after landscape works.
   - Done as layout patches applied at load time, so the user's original files stay untouched.

## Status

### Milestone 1: done (2026-11-16)
Windows x64 build (`tools\build_windows.bat`) boots the channel: SystemInit, the weather scene,
5 layouts and 4 fonts load, 60 fps, no crash; it waits on the "connecting" screen because
NWC24 is a stub.

Run it: `tools\extract_assets.ps1` once, then `build\windows\forecast.exe` (log:
`build\windows\data\boot.log`; crashes write a stack to `crash.txt`, symbolize with
`llvm-symbolizer --obj=forecast.exe <offsets>`).

How the 64-bit problems are handled (details in `patches/decomp-port.patch`, all under
`FORECAST_PORT`; the Wii build still matches: `dtk shasum -c config/HAFE/build.sha1` OK):
- u32/s32 are `int`-based (`port/include/types.h`); Clang errors on every pointer<->32-bit cast.
- One 256 MB arena holds MEM1/MEM2; 32-bit "GX addresses" are offsets into it or into the exe
  image (`port_mem.h`). Address 0 stays NULL (heaps start one page in).
- `macros.h`: `PTR_ADDR`, `GX_ADDR`, `PTR_TO_U32`, `U32_TO_PTR`, `PTR32(T)` (identical casts on
  the Wii). `PTR32` (`port_ptr32.h`) keeps pointer members of file-overlay structs 4 bytes:
  TPL, fonts (FINF/TGLP/CWDH/CMAP), GXTexObj userData.
- Windows only: `selectany` for header-defined hardware globals; `/alternatename` for globals
  the decomp declares with different types (`tools/gen_stubs.py`); x86 divide-by-zero is
  emulated with arm64's result (`port/sdk/crash_win.c`).
- 157 unported SDK functions are generated stubs (`port/sdk/autostubs.c`), logged on first use.

- `lyt::Pane` name / user data / visibility and `TextBox` text colors go through the
  members on the port; the Wii build keeps the original hard-coded offsets.

### Milestone 2: renderer done (2026-11-16); main menu waits on milestone 3
The 2D layouts draw on SDL3 GPU (D3D12 on Windows) and respond to touch. Verified on the
screens reachable without forecast data: the WiiConnect24 and "Unable to download" error
screens (FORE000005) render correctly (gradients, rounded panels, fonts) and the button
highlights on touch. The weather scenes themselves need milestone 3's data.
- `port/gx/gx_xform.cpp`: XF on the CPU (matrices, projection, viewport, lighting, texgens,
  dual-tex), primitives -> triangle lists in SDL GPU clip space.
- `port/gx/gx_sdl.cpp`: records draws (pipeline from blend/z/cull state, 8 texture maps,
  scissor, TEV registers), replays them into the EFB (a render target at 1-4x the Wii's size)
  at GXCopyDisp, scales it to a 16:9 rectangle. GXCopyTex -> GPU textures looked up by
  destination address (the scene fades capture the screen this way).
- `port/gx/shaders/gx_ps.hlsl`: the TEV ubershader (Dolphin's integer TEV math, konst, swap
  tables, compare modes, alpha test); `toolsuild_shaders.bat` compiles it with the Windows
  SDK's dxc into the committed `gx_shaders_dxil.h`. iOS (milestone 6) needs a SPIR-V -> MSL
  step on the macOS runner.
- `port/gx/gx_tex.cpp`: Wii texture formats -> RGBA8, cached and re-hashed after
  GXInvalidateTexAll. Every format in the assets checked by eye (`tools/tex_test`); RGB565
  and the CI formats only by synthetic tests (no asset uses them).
- `port/sdk/kpad.c`: a touch (left mouse button) is the Wii pointer: one frame of hover, then
  A held; no touch = pointer off screen. `SCGetIdleMode` reports WiiConnect24 on.
- Debug switches: `FORECAST_DUMP_FRAME=n[,m]` saves the EFB as `efb_<n>.bmp`;
  `FORECAST_TOUCH=frame:x:y:frames` simulates a touch; `FORECAST_HIDDEN=1` hides the window.
- Not yet: indirect texturing, fog, z textures, logic ops (drawn as copy), EFB copy format
  conversion (copied as RGBA), destination alpha. Each logs once when first used.

Off on the port (each marked in the source):
- Cursor: no cursor is drawn (touchscreens). The sparkle effects were never loaded, and the
  plain hand texture (`Cursor::Set` type 5) is skipped under `FORECAST_PORT`.
- Sound, temporarily (brsar not converted, AX not ported; `Sound.cpp`, `SoundManager.cpp`).

### Next (from `notes/64bit-audit.md`)
- g3d `ResTexPlttInfoData` pointer members (globe materials), PAT0 bind arrays.
- snd TaskManager 0x40-byte units vs larger 64-bit tasks (when sound comes back).
- (ef relocation items: moot, the cursor effect is dropped.)
- `port_gx_addr` should assert in debug builds when given a stack / C-heap pointer.

## Repo rules
- Never commit Nintendo files: `HAFE_v7/`, `*.app`, `*.wad`, `cetk`, `title.tmd`, extracted assets,
  texture packs.
- The upstream decomp ships CLAUDE.md / AGENTS.md; if vendored, strip them (same as forecast-3ds).
