// Test of gx_tex.cpp's cache (gx_tex_get, gx_tex_invalidate, gx_tex_frame_end)
// against stub register state and memory, built with the project's headers
// (see build.bat). The GX address space is a local buffer here.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "gx_internal.h"
#include <port_log.h>

GXRegs gx;

static u8 sMem[1 << 20];
static int sLogLines;

extern "C" const void* port_gx_ptr(uint32_t addr) {
    return addr < sizeof(sMem) ? sMem + addr : nullptr;
}

bool gx_addr_ok(const void* p, u32 size) {
    const u8* b = (const u8*)p;
    return b != nullptr && b >= sMem && b + size <= sMem + sizeof(sMem);
}

extern "C" void port_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("  log: ");
    vprintf(fmt, ap);
    va_end(ap);
    sLogLines++;
}

static int sFailures;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                                                \
            sFailures++;                                                                                               \
        }                                                                                                              \
    } while (0)

// Binds an image to a map the way GXLoadTexObj's register writes do.
static void bind(int map, u32 addr, u32 fmt, u32 w, u32 h, u32 mode0 = 0, u32 mode1 = 0, u32 tlut = 0) {
    u32 b = map < 4 ? 0x80 + map : 0xA0 + (map - 4);
    gx.bp[b] = mode0;
    gx.bp[b + 0x04] = mode1;
    gx.bp[b + 0x08] = (w - 1) | (h - 1) << 10 | fmt << 20;
    gx.bp[b + 0x14] = addr >> 5;
    gx.bp[b + 0x18] = tlut;
}

int main() {
    static u8 tmem[0x80000];
    gx.tmemHi = tmem;
    GXTexDesc d, d2;

    // An 8x4 I8 image at 0x1000: texel (1,0) = 0x40.
    sMem[0x1001] = 0x40;
    bind(0, 0x1000, 1, 8, 4);
    CHECK(gx_tex_get(0, &d));
    CHECK(d.width == 8 && d.height == 4 && d.version != 0);
    CHECK(d.rgba[4] == 0x40 && d.rgba[7] == 0x40);
    u32 v1 = d.version;

    // Same frame, same epoch: no change.
    CHECK(gx_tex_get(0, &d2) && d2.version == v1 && d2.key == d.key);

    // Bytes change without an invalidate: the cache does not look (as the
    // GPU's texture cache would not).
    sMem[0x1001] = 0x80;
    CHECK(gx_tex_get(0, &d2) && d2.version == v1);
    // After an invalidate: re-hashed, re-decoded, new version.
    gx_tex_invalidate();
    CHECK(gx_tex_get(0, &d2) && d2.version != v1 && d2.rgba[4] == 0x80);
    u32 v2 = d2.version;
    // Invalidate with unchanged bytes: same version.
    gx_tex_invalidate();
    CHECK(gx_tex_get(0, &d2) && d2.version == v2);

    // The same address with another size is another image with its own
    // version; the first one keeps its own.
    bind(1, 0x1000, 1, 8, 8);
    CHECK(gx_tex_get(1, &d2) && d2.key != d.key && d2.version != v2 && d2.height == 8);
    CHECK(gx_tex_get(0, &d) && d.version == v2);

    // Sampling state: wrap S repeat, T mirror, mag linear, min LIN_MIP_LIN
    // (hardware 6). Then clamp/clamp(3 -> repeat), nearest, NEAR_MIP_LIN (2).
    bind(2, 0x1000, 1, 8, 4, 1 | 2 << 2 | 1 << 4 | 6 << 5);
    CHECK(gx_tex_get(2, &d2) && d2.wrapS == 1 && d2.wrapT == 2 && d2.magLinear == 1 && d2.minLinear == 1);
    CHECK(d2.version == v2); // same image as map 0
    bind(2, 0x1000, 1, 8, 4, 0 | 3 << 2 | 0 << 4 | 2 << 5);
    CHECK(gx_tex_get(2, &d2) && d2.wrapS == 0 && d2.wrapT == 1 && d2.magLinear == 0 && d2.minLinear == 0);
    bind(2, 0x1000, 1, 8, 4, 4 << 5); // GX_LINEAR
    CHECK(gx_tex_get(2, &d2) && d2.minLinear == 1);

    // Mipmaps: logged once.
    int before = sLogLines;
    bind(3, 0x1000, 1, 8, 4, 0, 0x10 << 8);
    CHECK(gx_tex_get(3, &d2));
    CHECK(gx_tex_get(3, &d2));
    CHECK(sLogLines == before + 1);

    // CI8 with an RGB565 TLUT at TMEM line 2 (offset 0x400): texel (0,0) is
    // index 5.
    sMem[0x2000] = 5;
    tmem[0x400 + 10] = 0xF8; // entry 5 = red
    bind(4, 0x2000, 9, 8, 4, 0, 0, 2 | 1 << 10);
    CHECK(gx_tex_get(4, &d2) && d2.rgba[0] == 255 && d2.rgba[1] == 0 && d2.rgba[3] == 255);
    u32 vci = d2.version;
    // Palette change + invalidate: re-decoded.
    tmem[0x400 + 10] = 0x00;
    tmem[0x400 + 11] = 0x1F; // blue
    gx_tex_invalidate();
    CHECK(gx_tex_get(4, &d2) && d2.version != vci && d2.rgba[0] == 0 && d2.rgba[2] == 255);
    // Same image with another TLUT register: another key.
    u64 k = d2.key;
    bind(4, 0x2000, 9, 8, 4, 0, 0, 3 | 1 << 10);
    CHECK(gx_tex_get(4, &d2) && d2.key != k);
    // CI14 with its TLUT near the end of TMEM: padded, not read past.
    bind(5, 0x2000, 10, 4, 4, 0, 0, 1023 | 2 << 10);
    CHECK(gx_tex_get(5, &d2));

    // Unmapped or empty: false.
    bind(6, 0, 1, 8, 4);
    CHECK(!gx_tex_get(6, &d2));
    bind(6, 0xFFFE0, 6, 64, 64); // runs past the end of memory
    CHECK(!gx_tex_get(6, &d2));
    bind(6, 0x1000, 7, 8, 8); // format 7 does not exist
    CHECK(!gx_tex_get(6, &d2));
    CHECK(!gx_tex_get(8, &d2) && !gx_tex_get(-1, &d2));

    // Eviction: map 0's image unused for > 600 frames is dropped; using it
    // again decodes it anew with a new version.
    for (int i = 0; i < 300; i++) {
        gx_tex_get(1, &d2); // keep map 1's image alive
        gx_tex_frame_end();
    }
    CHECK(gx_tex_get(0, &d) && d.version == v2); // still cached
    for (int i = 0; i < 800; i++) {
        gx_tex_get(1, &d2);
        gx_tex_frame_end();
    }
    u32 v1b = d2.version;
    CHECK(gx_tex_get(0, &d) && d.version != v2 && d.rgba[4] == 0x80);
    CHECK(gx_tex_get(1, &d2) && d2.version == v1b);

    printf(sFailures ? "cache test: %d failures\n" : "cache test: all passed\n", sFailures);
    return sFailures ? 1 : 0;
}
