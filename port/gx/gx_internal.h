// Internal interface of the port's GX layer (port/gx).
//
// The decomp's own GX library encodes every call into Wii GPU register writes
// (BP, CP, XF) and vertex data. Those go through port_gx_pipe into a byte
// stream, which gx_cp.cpp parses into the register state below. Draws read
// that state and go to the renderer backend (for now gx_null.cpp, which only
// counts and logs them).
#ifndef PORT_GX_INTERNAL_H
#define PORT_GX_INTERNAL_H

#include <types.h>

#include <port_mem.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- the pipe (gx_fifo.c) ---------------------------------------------------

// Parses everything written to the immediate pipe so far.
void port_gx_sync(void);

// Parses a complete command stream (an immediate batch or a display list)
// (gx_cp.cpp).
void gx_cp_run(const u8* data, u32 size);

// --- backend bring-up and frames ----------------------------------------------

void port_gx_backend_init(void);
// Called once per frame from the copy to the display (GXCopyDisp).
void port_gx_present(void);

#ifdef __cplusplus
}

// --- register state (gx_cp.cpp) ----------------------------------------------

struct GXRegs {
    u32 bp[0x100];
    u32 cp[0x100];
    // XF memory 0x0000-0x0FFF (matrices, lights) as raw words, and the XF
    // registers 0x1000-0x10FF.
    u32 xfMem[0x1000];
    u32 xfReg[0x100];
    // TEV colour registers (signed 11-bit) and konstant colours, indexed by
    // register pair: [0] PREV, [1] C0, [2] C1, [3] C2 / K0..K3.
    s16 tevReg[4][4];
    u8 konst[4][4];
    // The BP mask for the next BP write.
    u32 bpMask;
    // TMEM above 0x80000: texture lookup tables loaded with BP 0x64/0x65.
    u8* tmemHi;
};

extern GXRegs gx;

static inline f32 gx_xf_float(u32 addr) {
    union {
        u32 u;
        f32 f;
    } x;
    x.u = addr < 0x1000 ? gx.xfMem[addr] : gx.xfReg[addr & 0xFF];
    return x.f;
}

static inline u32 gx_bits(u32 v, int shift, int size) {
    return (v >> shift) & ((1u << size) - 1);
}

// Big-endian readers for the command stream and big-endian resources.
static inline u16 gx_be16(const u8* p) {
    return (u16)(p[0] << 8 | p[1]);
}
static inline u32 gx_be32(const u8* p) {
    return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

// Converts an address the Wii GPU would see (a GX address, port_mem.h) back
// to a host pointer.
static inline const u8* gx_mem(u32 physical) {
    return (const u8*)port_gx_ptr(physical);
}

// True if [p, p + size) is memory the program can read (the port arena or the
// executable image). Used to reject addresses decoded from bad register state.
bool gx_addr_ok(const void* p, u32 size);

// --- draws (backend) -----------------------------------------------------------

// One vertex as decoded from the stream, before transformation.
struct GXVertexIn {
    f32 pos[3];
    f32 nrm[3];
    u8 clr[2][4];
    f32 tex[8][2];
    u8 pnMtx;
    u8 texMtx[8];
    u8 hasPnMtx;
    u8 hasTexMtx; // bit per texcoord
    u8 hasClr; // bit per channel
    u8 hasNrm;
};

// Called by the parser with a primitive's decoded vertices.
void gx_draw(u32 primitive, const GXVertexIn* verts, u32 count);
// BP 0x52: an EFB copy. toTexture distinguishes GXCopyTex from GXCopyDisp.
void gx_efb_copy(u32 command);
// BP 0x66: textures in memory may have changed.
void gx_tex_invalidate(void);

#endif // __cplusplus
#endif
