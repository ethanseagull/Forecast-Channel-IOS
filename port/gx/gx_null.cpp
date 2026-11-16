// Milestone 1 backend: no rendering. Counts what the command processor
// decodes, logs a per-frame summary for the first frames, and paces nothing
// (VIWaitForRetrace does that). Replaced by the SDL3 GPU backend in milestone 2.
#include "gx_internal.h"
#include <port_log.h>

static u32 sFrame;
static u32 sDraws, sVerts, sCopies;
static u32 sPrimCount[8];

extern "C" void port_gx_backend_init(void) {
    static u8 tlut[0x80000];
    gx.tmemHi = tlut;
    port_log("gx: null backend (draws are counted, not rendered)\n");
}

extern "C" void port_gx_present(void) {
    sFrame++;
    if (sFrame <= 5 || sFrame % 300 == 0) {
        port_log("gx: frame %u: %u draws, %u vertices, %u copies "
                 "(quads %u, tris %u, strips %u, fans %u, lines %u, points %u)\n",
                 sFrame, sDraws, sVerts, sCopies, sPrimCount[0], sPrimCount[2], sPrimCount[3], sPrimCount[4],
                 sPrimCount[5] + sPrimCount[6], sPrimCount[7]);
    }
    sDraws = sVerts = sCopies = 0;
    for (u32& c : sPrimCount) {
        c = 0;
    }
}

void gx_draw(u32 primitive, const GXVertexIn* verts, u32 count) {
    (void)verts;
    sDraws++;
    sVerts += count;
    // GX_QUADS 0x80, GX_TRIANGLES 0x90, STRIP 0x98, FAN 0xA0, LINES 0xA8,
    // LINESTRIP 0xB0, POINTS 0xB8.
    sPrimCount[(primitive >> 3) & 7]++;
}

void gx_efb_copy(u32 command) {
    sCopies++;
    // Bit 14: copy to the external frame buffer (GXCopyDisp) = end of frame.
    if ((command >> 14) & 1) {
        port_gx_present();
    }
}

void gx_tex_invalidate(void) {}
