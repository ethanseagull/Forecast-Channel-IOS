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

#include <vector>

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
    // The 2nd and 3rd vectors of an NBT normal (GX: binormal, tangent), for
    // the emboss texgen. Vertices without them get the last ones decoded, as
    // the hardware keeps them.
    f32 binormal[3];
    f32 tangent[3];
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

// One vertex after the CPU transform (gx_xform.cpp), as the vertex shader
// reads it (shaders/gx_vs.hlsl): clip-space position in SDL GPU's convention
// (NDC +Y up, Z 0..1), the two rasterized colour channels (0..1, already lit
// and clamped), and the 8 texture coordinates (s, t, q; the pixel shader
// samples at (s/q, t/q), normalized, t = 0 at the top row).
// Vertex attribute locations: 0 pos, 1 clr0, 2 clr1, 3..10 tex0..tex7.
struct GXVtxOut {
    f32 pos[4];
    f32 clr[2][4];
    f32 tex[8][3];
};

// The EFB size the game renders at (the display copy's source size).
void gx_efb_size(u32* w, u32* h);

// Transforms a primitive (gx_xform.cpp) and appends it to out as a triangle
// list. Returns the number of vertices appended.
u32 gx_xform_primitive(u32 primitive, const GXVertexIn* in, u32 count, std::vector<GXVtxOut>& out);

// --- the TEV pixel shader's uniform block (shaders/gx_ps.hlsl) -----------------
//
// Fragment uniform slot 0 (HLSL: cbuffer at register(b0, space3)). Raw BP
// register values: the ubershader decodes the bit fields itself. Laid out in
// 16-byte rows to match HLSL cbuffer packing (u32 x[16] <-> uint4 x[4]).
struct GXTevUniforms {
    u32 colorEnv[16]; // BP 0xC0 + 2n: TEV colour combiner of stage n
    u32 alphaEnv[16]; // BP 0xC1 + 2n: TEV alpha combiner of stage n
    u32 tevOrder[8];  // BP 0x28-0x2F: texmap / texcoord / ras channel, 2 stages each
    u32 ksel[8];      // BP 0xF6-0xFD: konst selects and swap tables
    s32 tevReg[4][4]; // [PREV, C0, C1, C2][R, G, B, A], signed 11-bit as on the GPU
    s32 konst[4][4];  // [K0..K3][R, G, B, A], 0-255
    u32 genMode;      // BP 0x00: number of TEV stages (bits 10-13, + 1), indirect stages (16-18) etc.
    u32 alphaCompare; // BP 0xF3
    u32 texBound;     // bit n: texture map n has a real texture bound (else white)
    u32 pad;
    // Indirect texturing (offset 336; everything above keeps its offset).
    u32 tevInd[16];   // BP 0x10-0x1F: indirect command of TEV stage n
    u32 indMtx[12];   // BP 0x06-0x0E: indirect matrices 0-2, 3 words each; 9-11 unused (0)
    u32 indScale[2];  // BP 0x25, 0x26: coordinate scale of indirect stages 0/1, 2/3
    u32 indRef;       // BP 0x27: texmap / texcoord of indirect stages 0-3
    u32 pad2;
    // Texcoord n's SU scale - 1 (the texture size GX set for it): S in bits
    // 0-15 (BP 0x30 + 2n), T in 16-31 (BP 0x31 + 2n). Indirect texturing
    // works in texels; this turns the normalized coordinates into texels.
    u32 tcScale[8];
    // Texture map n's image0 register (BP 0x88-0x8B, 0xA8-0xAB): width - 1
    // in bits 0-9, height - 1 in 10-19. The Wii size, also for EFB copies
    // (which are larger on the GPU); turns texels back into normalized.
    u32 texImage0[8];
};
// 33 rows; the shader's reflection reports the same 528 bytes.
static_assert(sizeof(GXTevUniforms) == 528, "GXTevUniforms must match the HLSL cbuffer");

// --- textures (gx_tex.cpp) ---------------------------------------------------
//
// The texture loaded into a texture map (BP 0x80-0xBB). Images are tracked
// by what the registers describe, with a version per content: after
// GXInvalidateTexAll (BP 0x66) an image is re-hashed on its next use (large
// ones by a sparse sample, fully every 30 frames) and gets a new version if
// its bytes (or its TLUT) changed. No pixels are kept: the renderer decodes
// an image (gx_tex_pixels) only when it uploads a version it lacks.
struct GXTexDesc {
    // Identifies the image and its contents: a new version means new pixels.
    // Versions are unique across all images.
    u64 key;
    u32 version;
    u32 width, height;
    // Sampling state from the map's mode registers (it can differ per use of
    // the same image). Wrap: 0 clamp, 1 repeat, 2 mirror.
    u8 wrapS, wrapT;
    u8 minLinear, magLinear;
    // A texture pack replacement for this version (gx_texpack_load), or -1.
    s32 replace;
};

// --- texture packs (gx_texpack.cpp) ------------------------------------------

struct SDL_Surface;
// Looks a texture up in the packs under <data>/textures by its Dolphin name
// (computed from the texel bytes, and the used palette for CI formats).
// Returns a replacement id or -1.
s32 gx_texpack_lookup(u32 fmt, u32 w, u32 h, bool mipmaps, const u8* texels, u32 size, const u8* pal,
                      u32 palEntries);
// Queues a replacement for loading on a worker thread (no-op if it is
// already queued or loaded).
void gx_texpack_request(s32 id);
// The replacement as RGBA8 (bytes R, G, B, A) once a worker has decoded it,
// else NULL. The caller takes ownership and destroys it.
SDL_Surface* gx_texpack_take(s32 id);

// Looks up the texture in map 0-7. False if the map has no usable texture.
bool gx_tex_get(int map, GXTexDesc* out);
// Decodes the texture in map 0-7 to RGBA8 (width * height pixels, bytes
// R, G, B, A, top row first). Valid until the next call or
// gx_tex_frame_end; NULL on failure.
const u8* gx_tex_pixels(int map);
// Once per frame: drops images unused for a while and logs statistics.
void gx_tex_frame_end(void);
// BP 0x52: an EFB copy. toTexture distinguishes GXCopyTex from GXCopyDisp.
void gx_efb_copy(u32 command);
// BP 0x66: textures in memory may have changed.
void gx_tex_invalidate(void);

#endif // __cplusplus
#endif
