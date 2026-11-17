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
   (all 16 stages), plus a shader per TEV configuration generated and compiled at run time
   (Metal compiles MSL source; "Specialized TEV shaders" below), vertices transformed on the CPU,
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

### Milestone 3: done (2026-11-16)
Real weather from WiiLink on the 2D screens: first-run date check, region and city lists
(names from `forecast.bin`), then the forecast pages (current conditions, today, the week...).
- `port/sdk/nwc24.c`: the NWC24 download-task calls and the VF file calls the channel uses.
  `NWC24ExecDownloadTask` is an HTTP GET (`port/sdk/http.c`, raw sockets, HTTP/1.0, so no
  iOS ATS exception); Nintendo's `weather.wapp.wii.com` becomes `fore.wiilink24.com`
  (`FORECAST_WC24_HOST` overrides). The 0x140-byte signed header is stripped (signature not
  checked) and the LZ payload stored as `<data>/wc24/<id>.bin` (+ `.time`); if a download
  fails, the last good copy is used.
- `port/res/wc24_swap.cpp`: forecast.bin / short.bin to native byte order after the
  channel decompresses them (CRC checked on the original bytes, then recomputed, so the
  channel's own check still holds); UTF-16 strings found through their offsets.
- `Connect.cpp`: the short.bin buffer is 64 KB on the port. WiiLink's USA short.bin
  (21708 bytes decompressed) has outgrown the original 0x5000; the same size sits in the
  user's Dolphin copy, so WiiLink's patched channel must enlarge it too.
- `macros.h` `GXCOLOR_U32`: the channel reads GXColor structs as u32 for GXColor1u32
  (`DrawUtil.cpp`, `System.cpp`); on little-endian hosts that reversed the bytes (the list
  rows came out opaque cyan instead of translucent white).
- `WiiConnect24.cpp`: the original pads the downloaded size by writing up to 3 bytes
  `outSize` bytes past the task struct; skipped on the port.
- The globe is skipped while `earth.brres` cannot be read (milestone 4): `SimpleModel` has no
  scene model and the globe code checks for it.

### Milestone 4: done (2026-11-16): the globe and texture packs
The globe renders as on the Wii (frame dumps; not yet compared with Dolphin side by side):
satellite Earth, ocean, atmosphere rim, stars, city labels; zoom with - / +.
- `port/res/brres_swap.cpp`: BRRES (root and folder dictionaries, MDL0 with every structure
  g3d reads, TEX0 / PLT0 headers) to native byte order at load (`SimpleModel` constructor),
  idempotent through the byte-order mark. GX display lists, texel / palette data and the shape
  vertex-descriptor cache stay big-endian (g3d patches DLs byte by byte); vertex arrays are
  swapped per component, so GX and CPU code read them natively. Animation folders are skipped
  (earth.brres has none).
- g3d 64-bit / endian fixes: `ResTexPlttInfoData` pointer slots are `PTR32` (0x34 bytes, assert);
  `GDSetChanAmbColor` / `GDSetChanMatColor` pack GXColor with `GX_COLOR_AS_U32`; `LC::Lock()`
  fails on the port so g3d takes the main-memory path instead of the locked cache.
- `System.cpp` `PortNew`: blocks of 1 MB and up (and anything MEM1 cannot serve) come from
  `malloc`, so host-side allocations no longer exhaust the 64 MB MEM1.
- Renderer: indirect texturing in the ubershader (BP 0x06-0x0E, 0x10-0x1F, 0x25-0x27; texel
  scale from the SU registers, map sizes from image0; alpha bump); emboss texgen and NBT
  normal decoding in `gx_xform.cpp` / `gx_cp.cpp` (the globe itself uses environment texgens
  plus an indirect stage, no GX_TG_BUMP); call-display-list addresses are GX addresses.
- Texture cache: no decoded pixels kept (`gx_tex_pixels` decodes when the backend uploads a new
  version); large images are checked with a 4 KB sample hash each frame and fully hashed every
  30 frames (the channel invalidates all textures every frame: 17 MB of hashing a frame -> about
  1.3 MB).
- Threads: `OSJoinThread` on a detached thread fails at once, as on the Wii (the WiiConnect24
  downloader; the exit to the menu deadlocked). `OSReturnToMenu` quits the app (Windows).
- Texture packs (`port/gx/gx_texpack.cpp`): Dolphin-format PNGs anywhere under
  `<data>/textures/`, named `tex1_<w>x<h>[_m]_<XXH64>[_<palette XXH64>|_$]_<fmt>` exactly as
  Dolphin's `TextureInfo::CalculateTextureName` computes them (checked against its source in
  `C:\WiiPC\dolphin-src`). Looked up once per texture version; the backend uploads the PNG
  (SDL_LoadPNG) at its own size instead of the decoded image. With the user's WiiPC-Forecast and
  WiiPC-Earth packs installed locally, ~50 textures are replaced in a session, all 28 Earth
  tiles among them. `FORECAST_NO_TEXPACKS=1` turns them off. DDS packs are not read.
  PNGs decode on two worker threads; the original texture is drawn until its replacement is
  ready (no frame-rate cost measured in Release).
- Input: keys for the Remote's other buttons (`kpad.c`); `FORECAST_BUTTON` for scripted runs.

### Milestone 5: done (2026-11-16): sound
- `port/res/brsar_swap.cpp`: the sound archive (RSAR: SYMB, INFO, FILE) and the files inside it
  (RSEQ headers and labels, RBNK instruments and wave info incl. ADPCM coefficients; RWSD handled
  but unused here) to native byte order, from `MemorySoundArchive::Setup`. Wave samples stay
  big-endian (the DSP reads them); sequence bytecode is read byte by byte. rev_wtr.brsar: 51
  sequences, one bank of 42 mono DSP-ADPCM waves (44.1 / 32 kHz), no streams.
- snd 64-bit: TaskManager units 0x80 bytes; AxVoice NULL checks; Wii-address DataRefs return NULL.
- `port/sdk/ax.c`: the AX library. CPU side after the decomp's AX (voice stacks, priorities and
  stealing, AXSetVoice* with sync flags, DSP cycle budget); DSP side after Dolphin's AX HLE
  (ADPCM / PCM16 / PCM8, loops with ADPCM loop context, SRC none / linear / 4-tap polyphase with
  generated coefficients, volume envelope, LPF, biquad, 12 buses with ramps, depop, aux callbacks,
  fold-down, master volume). Volume ramps last one frame (nw4r re-sends them every frame).
- `port/sdk/ai.c`: a 3 ms frame thread calls the AX frame (as the AI DMA interrupt, under the
  interrupts lock) and queues 96-sample frames on an SDL3 audio stream (32 kHz stereo), keeping
  the device buffer plus ~2 frames queued. `axfx.c`: AXFX hooks; the reverb is the decomp's own
  `AXFXReverbHi*.c`, now compiled in. `SCGetSoundMode` says stereo.
- `OSReturnToMenu` releases the interrupts lock before stopping the audio thread (exiting could
  deadlock against the frame thread).

### Texture compression (2026-11-16)
GPU texture memory on the globe: 123 MB -> 22 MB (BC1), 497 MB -> 98 MB with the 2x Earth pack.
- CMPR textures go to the GPU compressed: as BC1 (`gx_tex_bc1`: the Wii's DXT1 blocks reordered,
  no re-encoding) where BC1 works, else as ASTC 4x4 transcoded block for block from BC1
  (`port/gx/gx_texcodec.cpp`). iOS prefers ASTC: BC support on iPhones is unreliable (reported but
  drawn black on some). Choice logged at start; `FORECAST_TEXCOMP=none|bc1|astc` overrides.
- Pack PNGs of 256x256 and up with no soft alpha are encoded to BC1 on the loader threads
  (~0.2 s per 2048x2048), then uploaded like CMPR. Smaller or soft-alpha images stay RGBA8.
- ASTC: 4-colour BC1 blocks -> CEM 8, 2-bit weights (1/3, 2/3 become 21/64, 43/64); 3-colour
  -> trit weights (exact); with transparency -> dual-plane CEM 12; solid -> void extent. Within
  0.1 dB of BC1 on the real textures; checked only against an independent decoder written from
  the spec (`tools/texcodec_test`), not on an ASTC GPU yet.
- The log's frame line reports resident texture memory.

### iOS preparation (2026-11-16)
- No Nintendo data at build time: the tables the decomp compiles in from the DOL (error screen
  layout, globe dots) are read from the user's `00000001.app` at start-up (`port/sdk/import.c`).
- Import: the channel files (`00000001.app`, `00000006.app`-`0000000a.app`) dropped into the data
  folder (iOS: the app's Documents, via the Files app) are unpacked into `content/` at start-up.
- Port threads allocate from the C heap (`port_mark_host_thread`): the channel's MEM heaps are
  single-threaded (a texture loader corrupted one).
- Stubs: `PORT_SYM` for Mach-O names; the MSVC `/alternatename` aliases only on Windows.
- `nus_fetch.ps1` reads the Wii common key from `WII_COMMON_KEY` or `common-key.txt` (ignored).
- iOS target: CMake app bundle (`port/ios/Info.plist.in`), SDL3 built from source, Metal
  shaders generated from the HLSL by `tools/build_shaders_msl.sh` (glslang + SPIRV-Cross),
  GitHub Actions workflow `.github/workflows/ios.yml` -> unsigned `ForecastOS.ipa`.

### Specialized TEV shaders (2026-11-16)
The iPhone was GPU-bound on the ubershader, so each TEV configuration a draw uses now gets its
own pixel shader, as in Dolphin; the ubershader draws until it is ready.
- `port/gx/gx_tevgen.cpp`: key and source generator, HLSL or MSL from one generator. Key: the
  number of TEV / indirect stages, per stage the colour and alpha combiners (all fields), tev
  order, konst selects and indirect command, the swap tables, the alpha test functions and
  logic, the indirect stages' map / coord, which sampled maps are bound; fields nothing reads
  are zeroed. Uniforms: the same `GXTevUniforms` block as the ubershader (TEV registers, konst
  colours, alpha refs, indirect matrices and scales, texcoord / texture sizes). The code is
  the ubershader's math with the fields as constants.
- `port/gx/gx_spec.cpp`: cache (by key), one worker thread (generate; Windows: compile to DXIL
  with the SDK's `dxcompiler.dll`, `gx_dxc.cpp`), SDL objects within 3 ms a frame on the main
  thread (Apple: `SDL_CreateGPUShader`, which compiles the MSL, on the worker), pipelines keyed
  by shader + blend/depth/cull state. Keys and pipelines seen go to `<data>/tev_shaders.bin` and
  are built at the next start. Logs each new shader and every 600 frames the share of draws
  specialized.
- Exactness: `FixedCoord` / `FixedToUv` (texel coordinates for indirect texturing) are now
  `precise` in the ubershader too; without it, ~0.5% of random indirect configurations differed
  by a texel at a pixel or two. `tools\tevgen\build.bat`: all 1011 sample sources (11 real keys
  + random) compile with dxc -WX, and 12,000 draws of real and random register states with
  random inputs are byte-identical between the two shaders on the RTX 5070. Frame dumps of every
  scene (start-up fade, error 107000, date check, region list, forecast pages, globe zoomed in and
  out) are identical with `FORECAST_SPECIALIZED=0` and `=sync`, and the new ubershader matches
  the old one.
- Shaders per scene: start-up 4-5, error screen 4, date / region 5-6, forecast pages 11, globe
  10; 11 in all.
- Windows Release, RTX 5070, GPU time a frame (`FORECAST_GPU_TIME`, submit to completion):
  EFB 2x 0.54 ms ubershader, 0.30-0.44 specialized; EFB 4x 0.85 ms and 0.36-0.46. DXC takes
  5-19 ms a shader on the worker; with the remembered keys every draw is specialized from the
  first frames.
- Metal: compiled and linked only on the CI (`.github/workflows/ios.yml` step "Specialized
  shaders"); not yet run on an iPhone. The ubershader's MSL comes from SPIRV-Cross, and Metal
  compiles both with fast math, so identical pixels there are likely but not proven.

### CPU vertex transform (2026-11-16)
`port/gx/gx_xform.cpp` rewritten for speed with bit-identical output:
- Per-draw setup (channel controls, colours, lights, texgens, post matrices, viewport decoded to
  plain floats) is cached and reused while XF registers 0x09-0x57, BP 0x4A and the XF-memory
  words it read are unchanged. **If the setup ever reads another XF register or XF-memory word,
  widen `XF_REG_FIRST`/`XF_REG_END` or call `note_xf_mem()`**, or stale state is reused.
- Vertices go through in chunks of 48, one stage at a time (clang auto-vectorizes the sqrt /
  divide stages; no intrinsics); each chunk's triangles are emitted at once (no whole-primitive
  staging, no zero-filling resize). Light terms shared between channels; q8 by table.
- Checked out of tree against the old file: 26.9M vertices memcmp-identical (-O2, -mfma like
  arm64's default contraction, AVX2, -O0); only NaN sign/payload differs on nonsense inputs.
  **Never build with `-ffast-math` / `-ffp-contract=fast`** (it breaks the identity).
- x64 Release: a globe frame's transform 1.52 -> 0.82 ms, the Earth draw 2.3x faster.
- Assumes one GX thread (as before) and a 144-byte `GXVtxOut` (static_assert).
- Next floor is memory traffic (144 bytes written a vertex): a smaller `GXVtxOut` or indexed
  output would be the next step.

Off on the port (each marked in the source):
- Cursor: no cursor is drawn (touchscreens). The sparkle effects were never loaded, and the
  plain hand texture (`Cursor::Set` type 5) is skipped under `FORECAST_PORT`.

### Milestone 7: started (2026-11-17): touch gestures and the screen fill
- **Touch** (`port/sdk/kpad.c`, see its header): the globe's bars slide in only while the Wii
  pointer is on screen and moving or at the top / bottom edge, which a finger can't do, so after
  a tap the pointer is parked just off the top-left corner (x -8 px: counts as on screen and at
  the top edge, hovers nothing) for 5 s; a tap in the middle hides the bars, a tap on them keeps
  them. Pinch / spread = - / + (one zoom step per 1.18x of finger distance), two-finger swipe =
  Up / Down (globe tilt; the page above / below on the forecast pages), mouse wheel = zoom.
  While two fingers are down the pointer is off screen and A released. `System.cpp`
  (FORECAST_PORT): no cursor smoothing; easing toward a new touch made taps spin the globe.
- **Screen fill** (`port_display_layout` in `platform.c`, `gx_sdl.cpp`,
  `shaders/gx_present_ps.hlsl`): the 16:9 frame stays where it was (inside
  `SDL_GetWindowSafeArea`'s width; vertical insets ignored); the strips beside it (up to 21:9
  in all) get scenery. The EFB is widened by M = ceil(strip / frame width x 640) on each side
  (71 on an iPhone 15 Pro: 1564x912 at 2x); draws whose viewport spans the EFB get clip-space x
  scaled by 640/(640+2M) and a full-width viewport, scissors covering the frame widen, the rest
  are offset by M; untextured full-frame 2D draws (fades) stretch across. A frame shows the 3D
  margins if a widened perspective draw is not hidden by a later textured full-frame 2D draw;
  otherwise its edge columns are stretched outward (shaded to 0.8 at the screen edge);
  switches cross-fade over 8 frames. EFB copies and `efb_<n>.bmp` stay in game coordinates.
  16:9 windows: dumps byte-identical to before. iOS: home indicator dimmed
  (`SDL_HINT_IOS_HIDE_HOME_INDICATOR` 2). Costs ~22% more EFB pixels on the phone.
- `port_display_rect` gives kpad the frame in window coordinates (points on iOS).

### Next (from `notes/64bit-audit.md`)
- g3d `ResTexPlttInfoData` pointer members (globe materials), PAT0 bind arrays.
- (ef relocation items: moot, the cursor effect is dropped.)
- `port_gx_addr` should assert in debug builds when given a stack / C-heap pointer.

## Repo rules
- Never commit Nintendo files: `HAFE_v7/`, `*.app`, `*.wad`, `cetk`, `title.tmd`, extracted assets,
  texture packs.
- The upstream decomp ships CLAUDE.md / AGENTS.md; if vendored, strip them (same as forecast-3ds).
