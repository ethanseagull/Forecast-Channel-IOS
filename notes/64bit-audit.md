# 64-bit audit: Forecast Channel decomp on LLP64 / LP64

Read-only audit of the units ForecastOS compiles (`tools/decomp-linked-units.txt` as filtered by
`CMakeLists.txt`): channel code, NW4R lyt/ut/math/g3d/ef/snd, and the SDK pieces built from the
decomp (MEM, ARC, CX, TPL, GX minus GXFifo/GXMisc/GXPerf, MTX/mtx44). Paths are relative to
`forecast-decomp/` unless they start with `port/`.

**Snapshot: 2026-11-16 17:10.** The decomp working tree was being edited (FORECAST_PORT blocks
in ef, macros.h, snd headers, GX) while this audit ran. Line numbers match the files at that
time. Findings already fixed by the in-progress work are listed in the "Already handled" table,
not in the counts.

Method: every finding below was checked by reading the code (five parallel readers for channel,
lyt/ut/math, g3d, snd and ef, plus a direct pass over the SDK). The compile-error list in
`build_log.txt` (17:05) was cross-checked against them.

## Summary

| Subsystem | High | Medium | Low |
|---|---|---|---|
| Cross-cutting | 0 | 1 | 1 |
| SDK GX | 1 | 3 | 1 |
| SDK TPL | 1 | 0 | 0 |
| SDK ARC / CX | 0 | 0 | 2 |
| SDK MEM | 0 | 1 | 1 |
| SDK MTX (mtx44 only) | 0 | 0 | 0 |
| nw4r lyt | 0 | 0 | 2 |
| nw4r ut (fonts and other) | 4 | 2 | 3 |
| nw4r math | 0 | 0 | 0 |
| nw4r g3d | 2 | 1 | 3 |
| nw4r ef | 4 | 1 | 3 |
| nw4r snd | 1 | 5 | 6 |
| Channel code | 4 | 2 | 8 |
| **Total** | **17** | **16** | **30** |

Severity:
- **High:** corrupts data or crashes.
- **Medium:** wrong in some path.
- **Low:** cosmetic, a warning, or a compile error that the build already reports.

A compile-caught cast is Low only if the correct fix is local. Where the fix needs a wider slot
or a layout change, it keeps the severity of the underlying problem.

### The five most important findings

1. **`GXTexObjImpl` (40 bytes) no longer fits the 32-byte public `GXTexObj`.**
   - Location: `include/revolution/GX/GXInternal.h:62-73`, with `GXTexture.h:11` and the `memset` in `GXTexture.c:92`.
   - Every `GXInitTexObj` writes 8 bytes past the object.
   - In lyt, that overwrites the next texmap or the TexSRT array in each Material (`lyt_material.cpp:525-533`).
   - In g3d, it overwrites the next `texObj[]`, and finally the TLUT data, inside the MDL0 file (`g3d_resmat.h:156-158`).
2. **TPL bind writes pointers into the file's 4-byte slots** (`TPL.h:10-43`, `TPL.c:62-107`).
   - The `TPLPalette`, `TPLDescriptor`, `TPLHeader` and `TPLClutHeader` overlays all shift.
   - This hits every layout texture (`lyt_common.cpp:226-252`) and the channel's own TPLs (`SceneBase.cpp:242`, `WeatherScene.cpp:189`).
3. **brfnt / brfna font headers have pointer members, and the code relocates them in place.**
   - Locations: `ut_ResFontBase.h:20-59`, `ut_ResFont.cpp:10-13, 78-108`, `ut_ArchiveFontBase.cpp:355-443`.
   - All text breaks: FINF, TGLP, CWDH and CMAP fields are read and written at 64-bit offsets.
4. **ef relocates breff / breft by writing native pointers into 4-byte slots.**
   - Affected slots: `TextureData` (name/texture/tlut), the `EmitterResource` name, `ParticleParameterDesc::mTexture[3]`, and the anim-curve texture table.
   - Texel bytes get overwritten, and the GPU is handed a misaligned image (details under ef).
5. **The channel code hard-codes 32-bit object layouts.** No compiler flag catches these.
   - `lyt::Pane` offsets at `LayoutObj.cpp:47-59`. Fixed: member access under `FORECAST_PORT`.
   - The `TextBox` color at `+0xD8`: `LayoutObj.cpp:146`, `LayoutButton.cpp:113`, `WeatherAround.cpp:1631`. Fixed the same way.
   - An opaque 0x4C-byte stand-in for ef's `MemoryManager` at `Scene.cpp:21-56`. The real object is about 0x78-0x80 bytes, so constructing it overflows its heap block.

Close behind: g3d `ResTexPlttInfoData` (MDL0) has two pointer members written at bind time; and
`snd::TaskManager` units are 0x40 bytes but the stream-load tasks are larger.

---

## Cross-cutting

### Already handled (as of the snapshot)

These are in place, so they are not counted:

| Was | Now |
|---|---|
| `include/types.h:26-27` u32/s32 = `long` (8 bytes on iOS) | `port/include/types.h` uses `uint32_t`/`int32_t` + static_assert. All "u32 is 8 bytes on iOS" layout issues (forecast.bin/short.bin structs, savedata, DataRef, `sizeof(u32)` steps in g3d/ef, ef LCGs, `F32AsU32`, `ut::Color`, `DragScroll` blob size) go away. |
| `macros.h:48-53` `ROUND_UP_PTR`/`ROUND_DOWN_PTR` via `(u32)` | Redefined with `uintptr_t` in `port/include/types.h:26-30` |
| `ut_algorithm.h:64-106` `GetIntPtr`/`AddOffsetToPtr`/`RoundUp(T*)`/`RoundDown(T*)`/`ComparePtr` via u32 | FORECAST_PORT `uintptr_t` versions. This also fixes snd's `InstancePool`/`StrmFile`/`DvdSoundArchive` RoundUp on static and stack buffers. |
| `OSAddress.h` `OSCachedToPhysical` = `p - 0x80000000` | Returns the `port_gx_addr` value. `sZeroBuffer` and other static data map through the image region. |
| `GX_WRITE(p, …)` / `GXCallDisplayList` / `GXFastCallDisplayList` address writes | `port_gx_pipe.h:69` converts with `port_gx_addr` |
| `wchar_t` 4 bytes on iOS (layout TXT1, forecast.bin strings, `wcs*`) | `-fshort-wchar` on every target + `port/sdk/libc_extra.c` (wcs*, swprintf, vswprintf) |
| ef track tables (`ef_res_emitter.h:356-404`), name-table writers (`ef_resource.cpp:420, 459` → `PTR_TO_U32`), child-curve reader (`ef_animcurve.cpp:2802`), `ForEachParam` (`ef_types.h`, now `uintptr_t`), `ef_memorymanagerimpl.h:165` RoundUp | FORECAST_PORT edits in progress |
| `g3d_obj.h:53` `TypeObj::GetTypeID` | Low 32 bits of a static name pointer, which is safe within one image |
| `lyt_common.cpp:228` "unbound if `descriptorArray < 0x80000000`" | Replaced by a version-number check |

### Findings

- **[Medium]** `port/include/port_mem.h` and the PTR_TO_U32 / GX_ADDR macros in `include/macros.h`:
  - `port_gx_addr` maps any pointer outside the arena or the executable image to 0 and logs it once. Everything that stores a pointer as a 32-bit GX address or handle therefore silently breaks for stack buffers, CRT-heap memory (`malloc`, or `operator new` before `SystemInit`), and SDL-owned memory.
  - The brief says "arena below 4 GB"; `port_mem.h` says iOS cannot map below 4 GB and uses offsets instead. With offsets, a truncating fix such as `(u32)(uintptr_t)p` is never correct; only `port_gx_addr`/`PTR_TO_U32` or a real `uintptr_t` slot are.
  - Fix: use PTR_TO_U32 only for objects that are provably arena- or image-resident. Make `port_gx_addr` assert in debug builds rather than return 0. Use `uintptr_t` for callback/user-data slots (snd `Channel::mCallbackData`, channel `StateFunc` arg).
- **[Low]** Overloads that assumed `u32 == unsigned long` now collide or stop matching:
  - `lyt_arcResourceAccessor.cpp:43,78,142` `GetResourceSub(unsigned long*)` (the build reports "no matching function").
  - `snd_SoundArchivePlayer.h:105` (already being guarded).
  - Fix: declare these with `u32`.
- **Recommendation** (not counted): the build already uses `-Werror=pointer-to-int-cast`. Add:
  - `-Wshorten-64-to-32` (or at least `-Wshorten-64-to-32` on the decomp sources) to catch implicit `uintptr_t → u32` narrowing.
  - A `static_assert(sizeof(X) == N)` on every struct overlaid on file data: TPL, font, ef and g3d Res*Data.

---

## SDK

### GX

- **[High]** `include/revolution/GX/GXInternal.h:62-73` and `include/revolution/GX/GXTexture.h:11` — sizeof.
  - `GXTexObjImpl` has `void* userData` at 0x10. On 64-bit it is 0x28 bytes, but `GX_PUBLIC_STRUCT_DECL(GXTexObj, 32)` (`GXInternal.h:25-29`) declares a fixed 32 bytes and never checks the impl size.
  - `GXInitTexObj` clears and writes `sizeof(GXTexObjImpl)` (`GXTexture.c:92`), and `format`/`tlutName`/`loadCount`/`flags` move to 0x18-0x23.
  - Victims:
    - lyt Material GX blocks, which pack `texMapNum * sizeof(GXTexObj)` (`lyt_material.cpp:530`, `:230`).
    - **MDL0** `ResTexObjData::texObj[8]` *inside the brres* (`g3d_resmat.h:156-158`, written at `g3d_resmat.cpp:1520-1532` on bind and `g3d_anmtexpat.cpp:514-525` every PAT0 frame).
    - `g3d_state.cpp:242-252` `IsEqualTexObj` compares only `dummy[0..7]`.
  - Fix: under FORECAST_PORT, store `userData` as a `u32` GX address (`port_gx_addr` in `GXInitTexObjUserData`, `port_gx_ptr` in `GXGetTexObjUserData`). Its only user is lyt's CLUT header, which lives in the arena or the image. Then add `static_assert(sizeof(GXTexObjImpl) <= 32 && sizeof(GXTlutObjImpl) <= 12)`.
  - Do not widen `GXTexObj`: its size is baked into MDL0 material data.
- **[Medium]** `src/revolution/GX/GXAttr.c:716` — `GXSetArray` writes `(u32)base & ~0xC0000000` into CP ARRAYBASE.
  - The build reports it.
  - Callers pass heap arrays (g3d `res/g3d_resvtx.cpp:13,44,75`, `g3d_state.cpp:847-858`, `GlobeDots.cpp:109`) and static arrays (`GlobeDots.cpp:110-111`; ef `ef_drawbillboardstrategy.cpp:525`, `ef_drawdirectionalstrategy.cpp:330`, `ef_drawfreestrategy.cpp:143`).
  - Fix: `GX_ADDR(base)`. The image region covers the static arrays, so they need no copying.
- **[Medium]** `src/revolution/GX/GXTexture.c:123` (`GXInitTexObj`) and `:341` (`GXInitTlutObj`) — `((u32)image & 0x3FFFFFFF) >> 5` into a 24-bit field.
  - The build reports both.
  - Fix: `GX_ADDR(image) >> 5`.
  - Constraints to assert in `port_gx_addr`'s callers:
    - texture and TLUT addresses must be 32-byte aligned relative to the arena or image base, which makes the arena base and the image base 32-byte aligned;
    - 24 bits × 32 = 512 MB, which is exactly `PORT_GX_ADDR_LIMIT`, so the regions cannot grow past it.
- **[Medium]** `src/revolution/GX/GXFrameBuf.c:372` (`GXCopyDisp`) and `:435` (`GXCopyTex`) — destination address `(u32)dest & 0x3FFFFFFF`.
  - The build reports both.
  - The XFB and capture buffers are arena allocations (`System.cpp:178, 1065-1071`).
  - Fix: `GX_ADDR(dest)`. The EFB-copy emulation then resolves the address back with `port_gx_ptr`.
- **[Low]** `src/revolution/GX/GXInit.c:184` — `EnableWriteGatherPipe` / `PPCMtwpar((u32)OSUncachedToPhysical(0xCC008000))` is hardware-only. Put it under `#ifndef FORECAST_PORT` (the build reports it).
- **Checked:**
  - `GXFifoObjImpl` (0x38 bytes on 64-bit, slot 128), `GXLightObjImpl`, `GXTlutObjImpl`, and the TexRegion/TlutRegion impls all fit their public sizes.
  - `GXFifo.c` (which has many `(u32)ptr` register writes) is excluded by `CMakeLists.txt` and replaced by `port/gx/gx_fifo.c`.
  - No `GXGetTexObjData`-style getters are used.

### TPL

- **[High]** `include/revolution/TPL/TPL.h:10-43` with `src/revolution/TPL/TPL.c:62-107` — overlay with pointer members and in-place relocation (**.tpl**).
  - `TPLPalette::descriptorArray` (file 0x8), `TPLDescriptor{textureHeader, CLUTHeader}` (8 bytes in the file), `TPLHeader::data` (file 0x8) and `TPLClutHeader::data` (file 0x8) are all pointers. On 64-bit:
    - `descriptorArray` reads 8 bytes, the offset plus the first descriptor's word;
    - descriptors become 16 bytes;
    - `wrapS`…`LODBias` and `unpacked` move by 4-8 bytes.
  - `TPLBind` (`:74, :86, :102`) writes 8-byte pointers over the following fields.
  - The FORECAST_PORT byteswap (`TPL.c:23-57`) also assumes the file layout through these types. Its `for (w = &h->format; w <= &h->LODBias; w++)` at `:42` walks the 64-bit layout, and its `(u32)` casts are compile errors.
  - Users:
    - `lyt_common.cpp:226-252` (every lyt texture, 'timg' in each layout arc);
    - `SceneBase.cpp:242` (TPLCommon), `WeatherScene.cpp:189` (TPLWeather);
    - `ForecastData.cpp:759-791` and `Connect.cpp:433-438` (through `gUnk80330B74`);
    - `GlobeDots.cpp` (`gCommonTpl`).
  - Fix:
    - define private file-layout structs (`u32` offsets) and use them for the swap;
    - have `TPLBind` build a native side table (`TPLDescriptor[]`, `TPLHeader`, `TPLClutHeader` copies, allocated from the arena) and store its pointer in `descriptorArray`, overwriting file bytes 0x8-0xF only after reading them;
    - keep the version-number "bound" marker;
    - `TPLGet` and `TPLGetGXTexObjFromPalette` then work unchanged.

### ARC

- **[Low]** `src/revolution/ARC/arc.c:13` — `ADD_PTR` is `((u32)ptr + offset)`. It is used at `:50` (the port swap), `:79, :81, :85` (`ARCInitHandle`) and `:328` (`ARCGetStartAddrInMem`); all five are build errors.
  - Fix: `((u8*)(ptr) + (offset))`.
  - Checked: `ARCHeader` and `FSTEntry` (the overlays on the **U8 arc**) have no pointer members. `ARCHandle` and `ARCFileInfo` are runtime-only.

### CX

- **[Low]** `src/revolution/CX/CXUncomp.c:189, 396, 445` — the Huffman tree walk does `(((u32)treep >> 1) << 1) + …` (build errors).
  - Fix: use `uintptr_t`. The semantics are identical; the walk relies on the tree buffer's parity, not its width.
  - Checked: the LZ and RL paths and the contexts have no width assumptions. `CXiConvertEndian_` is already identity on little-endian (`CX.h`).

### MEM

- **[Medium]** Alignment, in `include/revolution/MEM/mem_expHeap.h:21-34` and `mem_heapCommon.h:22-36`:
  - The heap heads are placed at `ROUND_UP_PTR(start, 4)` (`mem_expHeap.c:313-314`, `mem_frameHeap.c:98-99`, `mem_unitHeap.c:31-32`). `MEMiHeapHead` embeds an `OSMutex`, and `MEMiExpHeapMBlock` is now 24 bytes (u16, u16, u32, prev*, next*) and sits directly before each block.
  - The channel allocates at alignment 4 by default (`System.cpp:69, 79, 114, 181, 717, 721, 1225-1255`).
  - Results:
    - pointers and `s64` land on 4-byte boundaries;
    - an `OSMutex` shim that wraps `SRWLOCK`/`pthread_mutex_t`/atomics can fault on arm64;
    - compiler-generated aligned SSE stores on objects from `operator new` can fault on x64.
  - Fix: under FORECAST_PORT, clamp `|align|` to at least 16 in `MEMAllocFromExpHeapEx` and `MEMAllocFromFrmHeapEx`, and round heap heads to 16.
- **[Low]** `src/revolution/MEM/mem_unitHeap.c:38` — the unit free list stores a pointer in each unit, so units must be at least 8 bytes. The only user is snd TaskManager (0x40), so this is fine today. Add an assert.
- **Checked:** all MEM pointer arithmetic already uses `uintptr_t` (`mem_heapCommon.h:45-80`). `MEMList` uses a `u16` offset. The negative-alignment paths sign-extend correctly (`s32 align`).

### MTX

`mtx44.c` is the only MTX unit built (`port/sdk/mtx.c` supplies the rest). It has no pointer/integer casts and no `long`; nothing to report.

---

## nw4r::lyt

The **brlyt / brlan** `res::*` overlays have no pointer members; cross-references are u32 offsets
resolved by `ConvertOffsToPtr`. lyt's real exposure is through TPL and `GXTexObj` (see SDK).

- **[Low]** `src/nw4r/lyt/lyt_arcResourceAccessor.cpp:43, 78, 142` — `unsigned long* pSize` is passed a `u32*` (see Cross-cutting; this is a build error).
- **[Low]** `src/nw4r/lyt/lyt_textBox.cpp:49-60, 395, 432` and `ut_TextWriterBase.h:158` — the TXT1 text and `TextWriterBase<wchar_t>` are 2-byte only under `-fshort-wchar`.
  - `vswprintf` comes from `port/sdk/libc_extra.c` on iOS. Make sure libc++'s `std::vswprintf` in `<cwchar>` resolves to it and not to libSystem's 4-byte version.
  - Add `static_assert(sizeof(wchar_t) == 2)`.
- **Cross-references (counted elsewhere):**
  - `lyt_common.cpp:226-252` → TPL.
  - `lyt_common.cpp:246`, `lyt_material.cpp:525-533, 1011-1012` → `GXTexObj` userData.
  - Layout allocations at 4-byte alignment → MEM.
- **Checked:**
  - `C:\forecast-3ds\port\res\lyt_swap.cpp` (copied into `port/res/`) writes no pointers and uses `sizeof` only on pointer-free `res::` structs.
  - `AnimTransformBasic` uses `sizeof(void*)` (`lyt_animation.cpp:348-349`).

## nw4r::ut

### Fonts (brfnt / brfna)

- **[High]** `include/nw4r/ut/ut_ResFontBase.h:20-59` — overlay structs with pointer members.

  | Field | File offset | Notes |
  |---|---|---|
  | `FontTextureGlyph::sheetImage` | 0x14 | |
  | `FontWidth::pNext` | 0x4 | |
  | `FontCodeMap::pNext` | 0x8 | |
  | `FontInformation::pGlyph` | 0x8 | |
  | `FontInformation::pWidth` | 0xC | |
  | `FontInformation::pMap` | 0x10 | |

  - On 64-bit, FINF grows from 0x18 to 0x28 bytes, TGLP from 0x18 to 0x20, and CWDH's `widthTable` and CMAP's `mapInfo` move.
  - Every reader in `ut_ResFontBase.cpp:114-217` and `ut_ArchiveFont.cpp:305-333` reads the wrong fields.
  - Fix: replace the members with `u32` offsets plus accessors (base = the font resource), or keep a native side struct built at load.
- **[High]** `src/nw4r/ut/ut_ResFont.cpp:10-13, 78-108` — **brfnt**. `ResolveOffset` reads the offset through `reinterpret_cast<s32>(ptr)` (a build error at `:12`) and writes 8-byte pointers in place:
  - `pGlyph` lands on `pWidth`;
  - `pWidth` lands on `pMap`, height, width and ascent;
  - CWDH `pNext` lands on `widthTable[0..2]`.
  - Every layout TextBox goes through this path (`lyt_textBox.cpp:88-91`), as do `SceneBase.cpp:207, 229`.
  - Fix: no in-place relocation. Resolve offsets in the accessors.
- **[High]** `src/nw4r/ut/ut_ArchiveFontBase.cpp:355, 367-369, 391-393` — **brfna**. The FINF, CWDH and CMAP bodies are copied raw (`header.size - 8`) into the construct buffer and then overlaid.
  - `pFINF->pWidth/pMap` are written at 64-bit offsets, past the copied FINF, into the next block.
  - `pPrev->pNext` overwrites width and map entries.
  - Fix: expand each header into its native struct while constructing, or switch to offsets (as above).
- **[High]** `src/nw4r/ut/ut_ArchiveFontBase.cpp:412-426, 440, 443` — **brfna** TGLP.
  - The code copies `sizeof(FontTextureGlyph)` bytes (0x20 instead of 0x18), which over-reads the stream and shifts the offset used for the sheet skip at `:448-451`.
  - `:440` reads the sheet offset through the pointer field (a build error) and `:443` stores a native pointer.
  - Fix: read the TGLP with explicit file offsets (as `port/res/font_swap.cpp` does) and keep the sheet offset as `u32`.
- **[Medium]** `src/nw4r/ut/ut_ArchiveFont.cpp:118, 139, 144-153` — `GetRequireBufferSize` sizes the brfna buffer from file block sizes plus `sizeof(FontInformation) + sizeof(FontTextureGlyph)`.
  - Any native expansion of the headers makes the caller's buffer (`SceneBase.cpp:442-454`) too small.
  - Fix: add the per-block growth when the layout changes.
- **[Low]** `src/nw4r/ut/ut_ArchiveFontBase.cpp:295-297` — the scratch-table address is computed as `(reinterpret_cast<u32>(pHeader) + …) & ~1` (a build error). Fix: `uintptr_t`.
- **[Low]** `src/nw4r/ut/ut_CharWriter.cpp:261-273` — the glyph sheet address goes to `GXInitTexObj`.
  - Both the brfnt file buffer and the brfna construct buffer come from arena heaps, so `GX_ADDR` covers them.
  - The sheet must stay 32-byte aligned relative to the arena base.
- **[Low]** `include/nw4r/ut/ut_ArchiveFont.h:162` and `ut_ArchiveFont.cpp:278` — the construct context aligns to 4. The FINF overlay and `CXUncompContextHuffman` (`u8* destp, treep`) hold 8-byte pointers at 4-byte alignment. Align to 8.

### Other ut

- **[Medium]** `include/nw4r/ut/ut_LockedCache.h:21-23` — `LC::GetBase()` returns the hard-wired `0xE0000000`.
  - g3d dereferences it (`g3d_calcview.cpp:667, 841`; `g3d_scnmdlsmpl.cpp:97`) whenever `LC::Lock()` succeeds.
  - **Fixed** (milestone 4): `LC::Lock()` fails on the port, so g3d uses main memory.
  - Fix: back it with a static 16 KB buffer, or make `Lock()` always fail on the port.
- **Checked:**
  - `ut_list` uses a `u16` offset and `u8*` arithmetic; `ut_LinkList` uses an `int` offset.
  - The IOStream and DvdFileStream callbacks take a `void*` arg.
  - `C:\forecast-3ds\port\res\font_swap.cpp` swaps the pointer fields by raw byte offset as 32-bit values and writes no pointers. Its offsets are the right file layout for the fix above.

## nw4r::math

Nothing remains: the `F32AsU32` pun is fixed by the u32 typedef, and the paired-single asm is behind FORECAST_PORT.

## nw4r::g3d (brres: MDL0, TEX0, PLT0, anims)

Most of g3d is offset-based (`ofs_to_ptr` with `s32` offsets on a 64-bit base): ResDic, ResFile,
ResMdl, ResNode, ResTex, ResPltt, ResVtx*Data, ResTagDL and the material DLs are all fine.

- **[High]** `include/nw4r/g3d/res/g3d_resmat.h:453-457` — **MDL0** `ResTexPlttInfoData` has `ResTexData* pTexData` (file 0x8) and `ResPlttData* pPlttData` (file 0xC).
  - The struct grows from 0x34 to 0x40 bytes, so `mapID`…`max_aniso` are read at the wrong offsets and `GetResTexPlttInfo` (`g3d_resmat.h:636-639`) strides wrongly.
  - `BindTex_` and `BindPltt_` (`g3d_resmat.cpp:1506, 1540`) and `Release` (`:1587-1588`) write 8 bytes into 4-byte slots.
  - The two fields are only used as a "bound" marker (`:1557, :1567`).
  - Fix: make both `u32` (a GX address, or just a non-zero flag) and add `static_assert(sizeof(ResTexPlttInfoData) == 0x34)`.
  - **Fixed** (milestone 4): both are `PTR32` on the port, with the size assert.
- **[High]** `src/nw4r/g3d/res/g3d_resanmtexpat.cpp:125-160` (Bind), `:67-68, 113, 122` (GetAnmResult), `:182-189` (Release) — **PAT0**.
  - `toResTexArray` and `toResPlttArray` (`g3d_resanmtexpat.h:88-89`) point at 4-byte slots in the file. The code indexes them as `ResTex[]`/`ResPltt[]`, which are pointer-sized, and writes found textures into them.
  - Result: an 8-byte stride overruns the texture array into the palette array.
  - This only matters if a loaded brres contains PAT0; check earth.brres.
  - Fix: treat both arrays as `u32` GX addresses, and wrap them in `ResTex(port_gx_ptr(slot))`.
- **[Medium]** `src/nw4r/g3d/res/g3d_resshp.cpp:146-155` — **MDL0** shape pre-primitive DL.
  - `ResShp::GXSetArray` writes `reinterpret_cast<u32>(OSCachedToPhysical(pBase))` into the CP ARRAYBASE command stored in the file. It is a build error at `:151`. It is called from `ResShp::Init` (`:196-228`) for POS, NRM, CLR and TEX.
  - Fix: `GX_ADDR(pBase)`. The software CP must read ARRAYBASE as a GX address for both immediate and DL paths.
- **[Low]** `src/nw4r/g3d/g3d_state.cpp:242-252` — `IsEqualTexObj`/`IsEqualTlutObj` compare `dummy[]`. They are correct once the `GXTexObjImpl` fix holds; use `memcmp(sizeof)` for robustness.
- **[Low]** `g3d_anmchr.cpp:352`, `g3d_anmclr.cpp:265`, `g3d_anmtexpat.cpp:267`, `g3d_anmtexsrt.cpp:269` — child pointer arrays are placed with `RoundUp(…, 4)` after a `u16` array, so they can be only 4-byte aligned. Align to `alignof(void*)`.
- **[Low]** `src/nw4r/g3d/res/g3d_restev.cpp:202-203` — `ResTev::CopyTo` stores a pointer difference in an `s32`. That is fine within the arena.
- **Cross-references:**
  - `GXTexObj` embedded in MDL0 (`g3d_resmat.h:156-158`) → GX High.
  - `GXInitTexObj`/`GXInitTlutObj`/`GXSetArray` call sites → GX Medium.
  - The LockedCache path → ut.
- **Checked:** the `ScnRoot`/`ScnMdlSimple` construct sizes use `sizeof(T*)`; the `g3d_workmem` union (0x18000) still covers every member; the `Copy32ByteBlocks` port is fine.

## nw4r::ef (breff / breft: the cursor sparkle)

- **[High]** `include/nw4r/ef/ef_res_texture.h:11-27` with `src/nw4r/ef/ef_resource.cpp:197-201` — **breft** `TextureData` has `char* name`, `u8* texture` and `u8* tlut`. It is 0x20 bytes in the file and 0x30 on 64-bit.
  - `name` (8 bytes) overwrites `width`/`height`.
  - `texture`/`tlut` are written at +0x20..+0x30, over the first 16 texel bytes.
  - `texture` is then set to `this + 0x30`, which skips 16 bytes and is not 32-byte aligned.
  - `ef_drawstrategyimpl.cpp:742-765` hands that address to `GXInitTexObj`/`GXInitTlutObj`, so the GPU reads from the clobbered +0x20.
  - Fix: a 32-bit-layout struct. Compute `texture = this + 0x20` and `tlut = texture + dataSize` in accessors, and keep the name as a `u32` GX address or look it up on demand.
- **[High]** `include/nw4r/ef/ef_res_emitter.h:268-283` with `src/nw4r/ef/ef_resource.cpp:78` — **breff** `EmitterResource {char* name; u32 headersize;}`.
  - The struct is 16 bytes on 64-bit instead of 8, and `sizeof(EmitterResource)` drives `GetEmitterDesc`/`SkipEmitterDesc` (`:279, :283`), so every descriptor and track lookup starts 8 bytes late.
  - `:78` writes an 8-byte name over `headersize`.
  - Fix: a `u32` name slot (`PTR_TO_U32`) and a hard-coded 8-byte header size.
- **[High]** `include/nw4r/ef/ef_particle.h:28` with `src/nw4r/ef/ef_resource.cpp:483-490` — **breff** `ParticleParameterDesc::mTexture[3]` is 12 bytes in the file and 24 on 64-bit.
  - `RelocateCommand` writes 64-bit `TextureData*` values over `textureWrap`, `textureReverse`, `mACmpRef*` and `sizeRandom`, and reads `textureNames` 12 bytes late.
  - `ef_particle.cpp:41` copies them out.
  - Fix: `u32` slots in the file struct, decoded in `ParticleParameter::Initialize`, where the runtime copy can stay native.
- **[High]** `src/nw4r/ef/ef_animcurve.cpp:2672-2674, 2760` — **breff**. `AnimCurveExecuteTexture` reads the name table as `AnimCurveTable<TextureData*>` (8-byte stride). The writer at `ef_resource.cpp:420/459` now stores 4-byte `PTR_TO_U32` values into `Name::work`, so reader and writer disagree.
  - Fix: read `u32 work` and `U32_TO_PTR(TextureData*, …)`, the same as the child-curve reader at `:2802`.
- **[Medium]** `include/nw4r/ef/ef_resource.h:16-30` with `ef_resource.cpp:19-20, 255-256, 385-386` — `EffectProject` / `TextureProject` thread a `ut::Link` (two pointers) through the **breff/breft** project header.
  - On 64-bit the link is 16 bytes, so `memset` and `List_Append` overwrite `projectnameLen` and `projectname[0..3]`.
  - `projectname` is never read, so nothing visible breaks today.
  - Fix: keep the project list in host-side nodes.
- **[Low]** The static `*_tex0_u8` arrays given to `GXSetArray` (`ef_drawbillboardstrategy.cpp:525` and others) are image data and are covered by the image region once `GXSetArray` uses `GX_ADDR`.
- **[Low]** The 8-byte pointer stores above land at 4-byte-aligned file addresses. That is undefined behaviour, but x64 and arm64 tolerate it; it goes away with the `u32`-slot fixes.
- **[Low]** `offsetof` on polymorphic classes (`ef_memorymanagertmp.h:280`, `ef_effect.cpp:17-18`, `ef_emitter.cpp:8`, `ef_particlemanager.cpp:14`, `ef_effectsystem.cpp:43`) only produces `-Winvalid-offsetof` warnings.
- **Notes:**
  - `src/nw4r/ef/ef_emform.cpp` and `ef_drawstrategybuilder.cpp` are stale duplicates of the subdirectory units; don't glob them.
  - The random-seed `union {u32; u8[4]}` XORs are byte-order-dependent, so sparkle randomness will differ from the Wii. That is not a 64-bit issue.
  - The name tables are decoded by hand as big-endian (`ef_resource.cpp:60-71`), so the byteswapper must leave them alone.

## nw4r::snd (brsar, brseq, brbnk, brwar/brwav, brwsd, brstm)

No snd file struct has a pointer member, and nothing writes pointers into resource data. Every
cross-reference is a `DataRef` OFFSET or a raw u32 offset, so swap-on-load plus offsets is enough
for the formats.

- **[High]** `include/nw4r/snd/snd_TaskManager.h:26, 41` with `src/nw4r/snd/snd_TaskManager.cpp:18` — sizeof. `TASK_SIZE_MAX = 0x40` is the unit size of `sTaskArea`.
  - On 64-bit, `Task` alone is 32 bytes (vptr plus `LinkListNode` plus id), and `StrmDataLoadTask`, `StrmHeaderLoadTask` and `SeqLoadTask` are larger than 0x40 (`snd_SoundArchivePlayer.h:217-330`).
  - These tasks are placement-new'd at `snd_SoundArchivePlayer.cpp:858, 973, 997`, so each overruns the next unit and corrupts the unit-heap free list on the first stream load.
  - The 0x44 header allowance is also too small for the 64-bit `MEMiHeapHead`.
  - Fix: `TASK_SIZE_MAX = RoundUp(max(sizeof(...Task)), 16)`, a header allowance from `sizeof`, alignment 16, and a `static_assert`.
  - **Fixed** (`FORECAST_PORT`): 0x80-byte units at 16-byte alignment, a 0x100-byte head allowance with a `static_assert` against the MEM heads, and `static_assert`s on the three load tasks in the `SoundArchivePlayer` constructor.
- **[Medium]** `src/nw4r/snd/snd_AxVoice.cpp:196-258` — `GetDspAddressBySample` / `GetSampleByDspAddress` use `static_cast<u32>(PTR_ADDR(OSCachedToPhysical(p)))`. The build flags these at `:208-252`.
  - With the new `OSCachedToPhysical`, the value is a GX address below 0x20000000, so the ADPCM nibble address (`addr*2+2`) still fits in 32 bits.
  - Fix: `(u32)(uintptr_t)OSCachedToPhysical(p)`, or `GX_ADDR(p)` directly.
  - The mixer decodes `port_gx_ptr(addr)` for PCM8, `addr*2` for PCM16 and `addr/2` for ADPCM, against one global mapping and never against the voice's own buffer: `WavePlayer`/`StrmPlayer` loops and `StopAtPoint` jump into other buffers (`snd_WavePlayer.cpp:100-121`, `snd_StrmPlayer.cpp:24-45`, `snd_AxVoice.cpp:95-133, 334`).
  - Wave data must therefore be in the arena or the image.
- **[Medium]** `src/nw4r/snd/snd_AxVoice.cpp:12-42` and `include/nw4r/snd/snd_AxVoice.h:93, 125-145` — snd writes `AXVPB.pb.addr.*AddressHi/Lo`, `loopFlag` and the sync bits directly, and reads `currentAddressHi/Lo` and `state`.
  - Fix: the AX shim keeps a real `AXPB` per voice that the mixer reads every frame, and writes `currentAddress` back as a 32-bit GX-based DSP address.
- **[Medium]** `include/nw4r/snd/snd_Channel.h:236` (`u32 mCallbackData`) and `snd_NoteOnCallback.h:28`, with `snd_SeqTrack.cpp:440 → 333` and `snd_WsdTrack.cpp:212 → 239` (track pointer through the channel callback on every note-on), and `snd_SoundArchivePlayer.cpp:746 → 839` (`SeqSound*` through `mLoadUserData`, `snd_SeqSound.h:91`).
  - All are build errors.
  - Fix: widen to `uintptr_t` (the field, `NoteOnInfo`, the `ChannelCallback` signature and `SeqSound` userData). `PTR_TO_U32` only works if every track and sound lives in the arena.
- **[Medium]** Pointer-bearing objects are carved at 4-byte alignment:
  - `snd_SoundArchivePlayer.cpp:189, 222, 253, 279, 299, 319, 338` (player heaps with an `OSMutex`, the group table, and the sound and track pools);
  - `snd_FrameHeap.cpp:23, 118`;
  - `snd_InstancePool.cpp:11-12`;
  - `snd_TaskManager.cpp:18`.

  Fix: align to 16, as for MEM.
- **[Medium]** `include/nw4r/snd/snd_FrameHeap.h:84` and `snd_SoundArchivePlayer.cpp:96, 172-178, 723` — the per-allocation `Block` header, the MEM frame-heap head and `Section` all grow on 64-bit, but the brsar `PlayerInfo.heapSize` was tuned for the 32-bit overhead. `SetupSoundPlayer` can fail, or playback can return `NOT_ENOUGH_PLAYER_HEAP`.
  - Fix: add the 64-bit overhead difference to `heapSize` in both `GetRequiredMemSize` and `SetupSoundPlayer`.
- **[Low]** `include/nw4r/snd/snd_SoundArchiveLoader.h:69` and `snd_SoundArchivePlayer.h:334` — FileStreams are placement-new'd into fixed 512-byte, alignment-1 buffers.
  - `NandFileStream` probably exceeds 512 bytes on 64-bit, but the game uses `MemorySoundArchive` (`SoundManager.cpp:243`), whose `MemoryFileStream` fits.
  - Fix: `alignas(16)`, a size taken from `sizeof`, and a `static_assert`.
- **[Low]** `include/nw4r/snd/snd_Util.h:55-60` and `snd_Util.cpp:93-95` — the `DataRef` REFTYPE_ADDRESS branch casts the 32-bit value to a pointer. It is unused by these files. Make it an assert.
  - **Fixed:** returns NULL on the port; `port/res/brsar_swap.cpp` reports any non-null address reference.
- **[Low]** `snd_SoundArchivePlayer.cpp:974, 1000, 1016` — a `StrmPlayer*` is used as the u32 task id (compare-only; build errors). Use `PTR_TO_U32` (the players live in the arena) or widen `Task::mTaskId`.
- **[Low]** `snd_AxManager.cpp:358`, `snd_SoundThread.cpp:65, 94`, `snd_TaskThread.cpp:46, 48` — enum values carried in `void*` are read back with `reinterpret_cast<u32>` (build errors). Use `(u32)(uintptr_t)`.
- **[Low]** `include/nw4r/snd/snd_AxVoice.h:128-131` — `&mVpb->pb == NULL` is never true. Test `mVpb == NULL`.
  - **Fixed:** both accessors test `mVpb == NULL` first on the port.
- **[Low]** `snd_SoundSystem.cpp:18-24, 76-109` — the 16 KB sound and DVD thread stacks are tight for 64-bit frames if the thread shim uses the supplied stacks.
- **Checked:**
  - `DisposeCallbackManager` and the `Invalidate*` paths compare real pointers.
  - The MmlParser call stack and `ParserTrackParam` hold `const u8*`.
  - All pool element types are at least 8 bytes.
  - `MemorySoundArchive` resolves offsets via `AddOffsetToPtr`.
- **Byte-order note** (outside scope): RSTM stream blocks and the ADPC table are read from the stream without a load-time swap (`snd_StrmFile.cpp:94-149`, `snd_SoundArchivePlayer.cpp:1199-1246`).

## Channel code (src/*.cpp, include/channel)

**forecast.bin / short.bin are safe:** their overlays (`ForecastData.h:30-167`) hold only
`u32` offsets, and nothing writes pointers back into the buffer.

- **[High]** `src/Scene.cpp:21-56, 94` — `class MemoryManager` is redeclared with an opaque `u8 unk4[0x4C - 0x4]` body.
  - The real class (`ef_memorymanagerimpl.h`, compiled by `ef_memorymanager.cpp` in the same namespace) holds eight pointers plus ints, about 0x78-0x80 bytes on 64-bit.
  - `new MemoryManager(...)` allocates 0x50 bytes and the constructor writes past it: a heap overflow, and an ODR violation.
  - Fix: include the configured `ef_memorymanagerimpl.h` instead of the blob, plus `static_assert(sizeof)`.
- **[High]** `src/LayoutObj.cpp:47-59` — `GetPaneName` uses `pane + 0xB4`, `SetPaneVisible` uses `+ 0xCF` and `GetPaneUserData` uses `+ 0xC4`. These are 32-bit `lyt::Pane` offsets, but the vtable, list node and pointers come first.
  - On 64-bit the visible-flag write corrupts the Pane, and name lookups read garbage.
  - `LayoutButton.cpp:22-28` defines the same inline functions through members. That is an ODR clash: the linker may pick either copy, so the 6 `SetPaneVisible` calls in LayoutButton (`:554-657`) are affected too.
  - Fix: `Pane::GetName()`, `GetUserData()` and `SetVisible()`, with one definition.
  - **Fixed:** the port uses the members (`FORECAST_PORT`; the Wii keeps the offsets, which match). Both copies of each helper now do the same thing on the port.
- **[High]** `src/LayoutObj.cpp:146`, `src/LayoutButton.cpp:113`, `src/WeatherAround.cpp:1631` — `SetTextBoxColors` writes `TextBox::mTextColors` through `(u8*)textBox + 0xD8`, which is the wrong field on 64-bit.
  - Fix: `TextBox::SetTextColor(0/1, c)`, or a small accessor.
  - **Fixed:** `SetTextColor(0/1, c)` under `FORECAST_PORT` in all three.
- **[High]** `src/System.cpp:1245-1255` and `:1223-1235` — the global `operator new`/`new[]` and `MEM1Alloc`/`MEM2Alloc` with `align == 0` use 4-byte alignment.
  - Clang assumes `__STDCPP_DEFAULT_NEW_ALIGNMENT__` (16) for `operator new` results, so aligned SSE/NEON stores into new'd objects can fault. The `new[]` cookie (`size_t`) and the pointers in `new CityForecast[n]` (and 7 similar in `ForecastData.cpp:160-286`) are misaligned.
  - The replacement also catches CRT, libc++ and SDL allocations made before `SystemInit`, while `gMEM1Heap` is NULL.
  - Fix: minimum alignment 16, and fall back to `malloc` while the heap is NULL (or stop replacing the global `new`).
- **[Medium]** `src/WiiConnect24.cpp:593-599` — the size-padding loop writes up to 3 zero bytes at `(char*)&task->mBuffers + outSize`, that is, up to about 100 KB past `gWC24Tasks`.
  - This is an original bug that landed in bss on the Wii; with a different static layout it can hit live globals.
  - Fix: pad `buffers[i]`, or drop the loop.
- **[Medium]** Fixed heap budgets are sized for 32-bit objects and 32-bit heap overhead:
  - `SceneBase.cpp:140-143` (0x700000 / 0x1B00000);
  - `Scene.cpp:93-94` (ef heap 0x20000 for 32/64/64/64 objects, about 62 KB estimated on 64-bit, so it probably still fits);
  - `WiiConnect24.cpp:43-45` (SO heap 0x10000).

  Fix: raise them, and log the high-water marks.
- **[Low]** `src/WeatherScene.cpp:188`, `System.cpp:1311`, `include/channel/System.h:122` — `u32 gUnk80330B74 = (u32)mTPLWeather`.
  - It is cast back at `ForecastData.cpp:759-791`, `Connect.cpp:433-438` and `WeatherScene.cpp:189, 529, 533`; all are build errors.
  - Fix: declare it `TPLPalette*`.
- **[Low]** `src/WeatherAddress.cpp:743 → 890` — an `AddressEntry*` is passed through an `s32` state argument (build error). Fix: `intptr_t` argument, or a member.
- **[Low]** `src/ForecastCheck.cpp:53-92` — alignment checks such as `(u32)p & 3` (build errors). Fix: `(uintptr_t)`.
- **[Low]** `src/System.cpp:65, 67, 75, 77` — the arena size is computed as `(u32)hi - (u32)lo` (build errors). Fix: `(u8*)hi - (u8*)lo`.
- **[Low]** `src/GlobeDots.cpp:110-111` and `src/ErrorLayoutArc.cpp:6` / `Scene.cpp:334` — static arrays and an embedded `.arc` (its TPL textures) are passed as GX addresses. They are covered by the image region once GX uses `GX_ADDR`. The embedded arc's TPL also needs the TPL side-table fix.
- **[Low]** Declarations that don't match their definitions break MSVC-ABI linking:
  - `extern "C" TPLPalette* gCommonTpl` (`GlobeDots.cpp:10`, `Scene.cpp:18`) vs the C++ definition at `SceneBase.cpp:111`;
  - `extern nw4r::ut::Font* gSysFont` in 9 units vs `ArchiveFont*` at `SceneBase.cpp:102`;
  - `extern f32 gDragScroll[]` (`WeatherAddress.cpp:18`) vs the `DragScroll` object at `SceneBase.cpp:76`.
- **[Low]** `include/channel/WorkerThread.h:6` (0x4000) and `WiiConnect24.cpp:1086` (0x8000) — thread stacks sized for PPC.
- **[Low]** wchar residual: forecast.bin/short.bin UTF-16 strings (`ForecastData.h:21-23`, `ForecastNow.cpp:32-49`, `Connect.cpp:73-80`) and about 200 `wcs*`/`swprintf` calls depend on `-fshort-wchar` and `port/sdk/libc_extra.c`. Add `static_assert(sizeof(wchar_t) == 2)` in a channel header.
- **Not applicable:** `src/HomeButton.cpp` (`HomeButtonController`, `HomeButtonInfo` layouts). `CMakeLists.txt` skips it in favour of `port/channel/HomeButton.cpp`.
- **Checked:**
  - No unions punning pointers, no memcpy of pointer structs, and no `OSCachedToPhysical` in channel code.
  - The runtime wrappers (`CityInfo`, `CityForecast`, …) are separate heap objects.
  - `OSMessage`/`OSThread`/NWC24 ids are fine.
  - The XFB, capture and FIFO buffers come from the arena heaps.

---

## Not verified

- The 64-bit sizes of `MemoryManager`, the snd tasks and `NandFileStream` are hand estimates; confirm them with `static_assert`.
- Whether `earth.brres` contains PAT0 (which decides whether the PAT0 finding is live), and whether the cursor breff uses texture or child anim curves.
- How `port/gx/gx_cp.cpp` decodes ARRAYBASE, texture-image and TLUT registers. It must use `port_gx_ptr` on the 32-bit value.
- Whether the `OSMutex` shim inside `MEMiHeapHead` and the snd heaps needs natural alignment. The alignment findings assume it does.
- The ef files and `include/macros.h` changed during the audit; re-check the ef section after those edits settle.
