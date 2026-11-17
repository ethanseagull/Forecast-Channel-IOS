// The GX vertex pipeline (XF) on the CPU.
//
// Each primitive's vertices are transformed by the position matrix and the
// projection, mapped through the XF viewport onto the EFB, lit (both colour
// channels) and given their texture coordinates (texgens incl. colour and
// emboss, texture matrices, dual-texture post matrices). The result is a triangle list in SDL GPU clip
// space that the pass-through vertex shader draws as is.
//
// Adapted from the New 3DS port (C:\forecast-3ds port/gx/gx_draw.cpp), minus
// its screen rotation and per-program channel selection.
//
// Speed (this runs for every vertex of every draw):
// - What depends only on XF registers and XF memory (channel controls,
//   material / ambient colours, lights, texgens, post matrices, viewport) is
//   decoded once (xf_setup) and reused by the following primitives while
//   none of the registers / words it was decoded from change.
// - Vertices go through in chunks of XF_CHUNK, one stage at a time over the
//   chunk (see "vertices"), so that each stage's per-draw choices are made
//   outside its loop and the arithmetic vectorizes across vertices.
// - Each chunk's triangles are assembled and appended right away (see
//   "primitives"): nothing larger than a chunk is staged, and the output is
//   written once (no value-initializing resize).
// The arithmetic is the plain per-vertex evaluation's, expression for
// expression (incl. what the compiler may contract into FMAs on arm64), and
// the output is bit-identical to it; work is only shared where it is the same
// expression on the same inputs (a light's terms for several channel
// controls, a colour control and an identical alpha control). The per-draw
// state, the chunk buffers and the scratch arrays are static: like the GX
// layer around it, this runs on one thread.
#include <math.h>
#include <string.h>

#include <vector>

#include "gx_internal.h"
#include <port_log.h>

#define WARN_ONCE(...)                                                         \
    do {                                                                       \
        static bool sWarned_;                                                  \
        if (!sWarned_) {                                                       \
            sWarned_ = true;                                                   \
            port_log(__VA_ARGS__);                                             \
        }                                                                      \
    } while (0)

void gx_efb_size(u32* w, u32* h) {
    // Display copy source size (BP 0x4A: width - 1, height - 1), set by
    // GXSetDispCopySrc. Before the first call, the usual 640x456.
    u32 cw = gx_bits(gx.bp[0x4A], 0, 10) + 1, ch = gx_bits(gx.bp[0x4A], 10, 10) + 1;
    *w = cw > 1 ? cw : 640;
    *h = ch > 1 ? ch : 456;
}

// --- helpers ---------------------------------------------------------------------

// n / 255.0f for n = 0-255: a colour byte as GX lighting reads it, and what
// q8 returns for the integer it floors to.
struct U8Table {
    f32 v[256];
    U8Table() {
        for (int i = 0; i < 256; i++) {
            v[i] = i / 255.0f;
        }
    }
};
static const U8Table kU8;

// XF memory as floats. Every per-vertex XF read (matrices 0x000-0x5FF,
// lights 0x600-0x67F) is below 0x1000, where gx_xf_float reads xfMem.
static inline const f32* xf_mem_f(u32 addr) {
    return (const f32*)&gx.xfMem[addr];
}

static void unpack_rgba(u32 v, f32 out[4]) {
    out[0] = kU8.v[v >> 24];
    out[1] = kU8.v[v >> 16 & 0xFF];
    out[2] = kU8.v[v >> 8 & 0xFF];
    out[3] = kU8.v[v & 0xFF];
}

static inline f32 clamp01(f32 v) {
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

// Quantizes to the 8 bits the rasterizer carries:
// floorf(clamp01(v) * 255.0f + 0.5f) / 255.0f. The floored value is an
// integer 0-255 (or NaN), so the division comes from the table.
static inline f32 q8(f32 v) {
    f32 x = clamp01(v) * 255.0f + 0.5f;
    if (x == x) {
        return kU8.v[(s32)x]; // x in [0.5, 255.5]: truncation is floor
    }
    return floorf(x) / 255.0f;
}

// v[0..2] /= d: three divisions, issued as one 4-lane one (lane 3 is
// padding; each lane is the same IEEE division).
static inline void div3(f32 v[3], f32 d) {
    f32 t[4] = {v[0], v[1], v[2], 0.0f};
    for (int i = 0; i < 4; i++) {
        t[i] = t[i] / d;
    }
    v[0] = t[0];
    v[1] = t[1];
    v[2] = t[2];
}

static inline void mtx3x4(const f32 m[12], const f32 v[4], f32 out[3]) {
    out[0] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2] + m[3] * v[3];
    out[1] = m[4] * v[0] + m[5] * v[1] + m[6] * v[2] + m[7] * v[3];
    out[2] = m[8] * v[0] + m[9] * v[1] + m[10] * v[2] + m[11] * v[3];
}

static inline void mtx2x4(const f32 m[8], const f32 v[4], f32 out[2]) {
    out[0] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2] + m[3] * v[3];
    out[1] = m[4] * v[0] + m[5] * v[1] + m[6] * v[2] + m[7] * v[3];
}

// --- per-draw state ------------------------------------------------------------

// A light as GX lighting reads it (XF 0x600 + 16 n), and which of its
// per-vertex terms this draw's channel controls need.
struct XfLight {
    f32 color[4];
    f32 pos[3], dir[3];
    f32 cosAtt[3]; // A0-A2 (base + 4..6)
    f32 distAtt[3]; // K0-K2 (base + 7..9)
    // Specular lights: pos normalized (0 if it has no length).
    f32 half[3];
    // Needed terms: direction / distance to the vertex, spotlight or
    // specular attenuation, N.L with the direction or the half vector.
    bool geo, spotAttn, specAttn, dotGeo, dotSpec;
};

// One lighting control (XF 0x0E-0x11).
struct XfCtrl {
    bool matVtx, lit, ambVtx;
    bool attnEnable, spot;
    u32 diffuseFn;
    u32 mask;
    f32 mat[4], amb[4];
    u32 numLights;
    u8 lights[8];
};

enum { TG_REGULAR, TG_EMBOSS, TG_COLOR };
enum { SRC_POS, SRC_NRM, SRC_TEX, SRC_NONE };

struct XfTexGen {
    u8 kind;
    // Regular texgens.
    u8 src, texIdx;
    bool stq, abc1;
    bool post, postNormalize;
    u32 defMtx;
    f32 postMtx[12];
    // Emboss.
    u8 srcGen;
    bool srcOk;
    f32 lpos[3];
    // Colour.
    u8 chan;
};

struct XfDraw {
    bool ortho;
    f32 p[6];
    // GX clip -> EFB clip: X = cx * xs + cw * xo, Y = cy * ys + cw * yo,
    // Z = cz * zs + cw * zo (GX's NDC z runs -1 (near) to 0 (far); the
    // viewport maps it onto 0..1 depth).
    f32 xs, xo, ys, yo, zs, zo;
    u32 numChans;
    u32 numTexGens;
    f32 efbW, efbH;

    // Vertex state, decoded from the XF registers once per primitive.
    u32 defPos;
    bool needNormal, needNbt;
    u32 activeChans; // min(numChans, 2)
    u32 activeGens; // min(numTexGens, 8)
    // [channel][0 colour, 1 alpha]; sameCA: both controls compute the same.
    XfCtrl ctrl[2][2];
    bool sameCA[2];
    u32 numLights;
    u8 lightIds[8];
    XfLight light[8];
    XfTexGen gen[8];
};

static void ctrl_setup(u32 ctrl, int chan, XfCtrl* c) {
    c->matVtx = ctrl & 1;
    c->lit = (ctrl & 2) != 0;
    c->ambVtx = (ctrl & 0x40) != 0;
    if (!c->matVtx) {
        unpack_rgba(gx.xfReg[0x0C + chan], c->mat);
    }
    c->mask = gx_bits(ctrl, 2, 4) | gx_bits(ctrl, 11, 4) << 4;
    c->diffuseFn = gx_bits(ctrl, 7, 2);
    c->attnEnable = (ctrl >> 9) & 1;
    c->spot = (ctrl >> 10) & 1;
    c->numLights = 0;
    if (!c->lit) {
        c->mask = 0;
        return;
    }
    if (!c->ambVtx) {
        unpack_rgba(gx.xfReg[0x0A + chan], c->amb);
    }
    for (int l = 0; l < 8; l++) {
        if (c->mask & (1 << l)) {
            c->lights[c->numLights++] = (u8)l;
        }
    }
}

static bool ctrl_same(const XfCtrl& a, const XfCtrl& b) {
    if (a.matVtx != b.matVtx || a.lit != b.lit) {
        return false;
    }
    if (!a.lit) {
        return true;
    }
    return a.ambVtx == b.ambVtx && a.mask == b.mask && a.diffuseFn == b.diffuseFn && a.attnEnable == b.attnEnable &&
           a.spot == b.spot;
}

// The last primitive's decoded state and everything it was decoded from:
// XF registers 0x09-0x57, BP 0x4A (the EFB size) and the XF memory words
// xf_setup read (lights, post matrices). Consecutive draws mostly share it;
// it is reused only while all of those are unchanged, bit for bit.
enum { XF_REG_FIRST = 0x09, XF_REG_END = 0x58, XF_MAX_RANGES = 24 };
static struct {
    bool valid;
    u32 regs[XF_REG_END - XF_REG_FIRST];
    u32 bp4A;
    u32 numRanges, numWords;
    u16 rangeAddr[XF_MAX_RANGES], rangeLen[XF_MAX_RANGES];
    u32 words[XF_MAX_RANGES * 16];
    bool overflow;
} sSetupKey;

// Notes XF memory [addr, addr + len) as read by xf_setup.
static void note_xf_mem(u32 addr, u32 len) {
    if (sSetupKey.numRanges == XF_MAX_RANGES) {
        sSetupKey.overflow = true;
        return;
    }
    sSetupKey.rangeAddr[sSetupKey.numRanges] = (u16)addr;
    sSetupKey.rangeLen[sSetupKey.numRanges] = (u16)len;
    memcpy(&sSetupKey.words[sSetupKey.numWords], &gx.xfMem[addr], len * sizeof(u32));
    sSetupKey.numRanges++;
    sSetupKey.numWords += len;
}

static bool setup_key_matches(void) {
    if (!sSetupKey.valid || sSetupKey.bp4A != gx.bp[0x4A] ||
        memcmp(sSetupKey.regs, &gx.xfReg[XF_REG_FIRST], sizeof(sSetupKey.regs)) != 0) {
        return false;
    }
    const u32* w = sSetupKey.words;
    for (u32 i = 0; i < sSetupKey.numRanges; i++) {
        u32 len = sSetupKey.rangeLen[i];
        if (memcmp(w, &gx.xfMem[sSetupKey.rangeAddr[i]], len * sizeof(u32)) != 0) {
            return false;
        }
        w += len;
    }
    return true;
}

static void light_setup(XfLight* L, int l) {
    u32 base = 0x600 + l * 16;
    note_xf_mem(base + 3, 13);
    unpack_rgba(gx.xfMem[base + 3], L->color);
    const f32* m = xf_mem_f(base);
    f32 lpos[3] = {m[10], m[11], m[12]};
    for (int i = 0; i < 3; i++) {
        L->pos[i] = lpos[i];
        L->dir[i] = m[13 + i];
        L->cosAtt[i] = m[4 + i];
        L->distAtt[i] = m[7 + i];
        L->half[i] = 0;
    }
    f32 len = sqrtf(lpos[0] * lpos[0] + lpos[1] * lpos[1] + lpos[2] * lpos[2]);
    if (len > 0.0f) {
        for (int i = 0; i < 3; i++) {
            L->half[i] = lpos[i] / len;
        }
    }
}

// The viewport terms of the last draw (most draws share the viewport).
static struct {
    u32 key[7];
    bool valid;
    f32 xs, xo, ys, yo, zs, zo, efbW, efbH;
} sViewport;

static void xf_decode(XfDraw* x, bool warn);

static XfDraw sDraw;

// The primitive's decoded XF state: the last one's if nothing it depends on
// changed.
static const XfDraw& xf_setup(void) {
    if (setup_key_matches()) {
        return sDraw;
    }
    sSetupKey.valid = false;
    sSetupKey.numRanges = sSetupKey.numWords = 0;
    sSetupKey.overflow = false;
    xf_decode(&sDraw, true);
    memcpy(sSetupKey.regs, &gx.xfReg[XF_REG_FIRST], sizeof(sSetupKey.regs));
    sSetupKey.bp4A = gx.bp[0x4A];
    sSetupKey.valid = !sSetupKey.overflow;
    return sDraw;
}

static void xf_decode(XfDraw* x, bool warn) {
    x->ortho = gx.xfReg[0x26] != 0;
    for (int i = 0; i < 6; i++) {
        x->p[i] = gx_xf_float(0x1020 + i);
    }
    // Viewport: 0x101A = width / 2, 0x101B = -height / 2, 0x101C = z range,
    // 0x101D/E = centre + 342, 0x101F = far z (24-bit units).
    u32 key[7] = {gx.xfReg[0x1A], gx.xfReg[0x1B], gx.xfReg[0x1C], gx.xfReg[0x1D],
                  gx.xfReg[0x1E], gx.xfReg[0x1F], gx.bp[0x4A]};
    if (!sViewport.valid || memcmp(key, sViewport.key, sizeof(key)) != 0) {
        f32 sx = gx_xf_float(0x101A), sy = gx_xf_float(0x101B);
        f32 ox = gx_xf_float(0x101D) - 342.0f, oy = gx_xf_float(0x101E) - 342.0f;
        u32 w, h;
        gx_efb_size(&w, &h);
        f32 efbW = (f32)w, efbH = (f32)h;
        sViewport.efbW = efbW;
        sViewport.efbH = efbH;
        sViewport.xs = 2.0f * sx / efbW;
        sViewport.xo = 2.0f * ox / efbW - 1.0f;
        sViewport.ys = -2.0f * sy / efbH;
        sViewport.yo = 1.0f - 2.0f * oy / efbH;
        sViewport.zs = gx_xf_float(0x101C) / 16777215.0f;
        sViewport.zo = gx_xf_float(0x101F) / 16777215.0f;
        memcpy(sViewport.key, key, sizeof(key));
        sViewport.valid = true;
    }
    x->efbW = sViewport.efbW;
    x->efbH = sViewport.efbH;
    x->xs = sViewport.xs;
    x->xo = sViewport.xo;
    x->ys = sViewport.ys;
    x->yo = sViewport.yo;
    x->zs = sViewport.zs;
    x->zo = sViewport.zo;
    x->numChans = gx.xfReg[0x09] & 3;
    x->numTexGens = gx.xfReg[0x3F] & 15;

    x->defPos = gx_bits(gx.xfReg[0x18], 0, 6);
    x->activeChans = x->numChans < 2 ? x->numChans : 2;
    x->activeGens = x->numTexGens < 8 ? x->numTexGens : 8;
    // (Up to 3 channels as the register says: channel 2 reads 0x10 / 0x12.)
    x->needNormal = false;
    for (u32 c = 0; c < x->numChans; c++) {
        if ((gx.xfReg[0x0E + c] & 2) || (gx.xfReg[0x10 + c] & 2)) {
            x->needNormal = true;
        }
    }
    u32 lightsUsed = 0;
    for (u32 c = 0; c < x->activeChans; c++) {
        ctrl_setup(gx.xfReg[0x0E + c], (int)c, &x->ctrl[c][0]);
        ctrl_setup(gx.xfReg[0x10 + c], (int)c, &x->ctrl[c][1]);
        x->sameCA[c] = ctrl_same(x->ctrl[c][0], x->ctrl[c][1]);
        lightsUsed |= x->ctrl[c][0].mask | x->ctrl[c][1].mask;
    }
    x->numLights = 0;
    for (int l = 0; l < 8; l++) {
        if (lightsUsed & (1u << l)) {
            XfLight& L = x->light[l];
            light_setup(&L, l);
            L.geo = L.spotAttn = L.specAttn = L.dotGeo = L.dotSpec = false;
            x->lightIds[x->numLights++] = (u8)l;
        }
    }
    for (u32 c = 0; c < x->activeChans; c++) {
        for (int k = 0; k < 2; k++) {
            const XfCtrl& cc = x->ctrl[c][k];
            bool spec = cc.attnEnable && !cc.spot;
            for (u32 i = 0; i < cc.numLights; i++) {
                XfLight& L = x->light[cc.lights[i]];
                if (spec) {
                    L.specAttn = true;
                    L.dotSpec |= cc.diffuseFn != 0;
                } else {
                    L.geo = true;
                    L.spotAttn |= cc.attnEnable;
                    L.dotGeo |= cc.diffuseFn != 0;
                }
            }
        }
    }

    bool dualTex = gx.xfReg[0x12] & 1;
    x->needNbt = false;
    for (u32 g = 0; g < x->activeGens; g++) {
        XfTexGen& t = x->gen[g];
        u32 info = gx.xfReg[0x40 + g];
        u32 type = gx_bits(info, 4, 3);
        u32 row = gx_bits(info, 7, 5);
        t.stq = (info >> 1) & 1;
        t.abc1 = (info >> 2) & 1;
        if (type == 2 || type == 3) {
            // Colour texgen: s, t from the vertex colour's red and green.
            t.kind = TG_COLOR;
            t.chan = (u8)(type - 2);
            continue;
        }
        if (type == 1) {
            // Emboss (GX_TG_BUMP0-7): the source texgen's result (bits
            // 12-14; GX requires a lower-numbered one, already done)
            // moved by the direction to light n (bits 15-17) in the
            // binormal / tangent frame: s += L.binormal, t += L.tangent.
            // Dolphin's VertexShaderGen; its "tangent" / "binormal" are
            // the NBT's 2nd / 3rd vectors, as here. No texture or post
            // matrix applies.
            t.kind = TG_EMBOSS;
            x->needNbt = true;
            t.srcGen = (u8)gx_bits(info, 12, 3);
            t.srcOk = t.srcGen < g;
            const f32* m = xf_mem_f(0x600 + gx_bits(info, 15, 3) * 16);
            note_xf_mem(0x600 + gx_bits(info, 15, 3) * 16 + 10, 3);
            t.lpos[0] = m[10];
            t.lpos[1] = m[11];
            t.lpos[2] = m[12];
            if (!t.srcOk && warn) {
                WARN_ONCE("gx: emboss texgen %lu from texgen %lu (not computed yet)\n", (unsigned long)g,
                          (unsigned long)t.srcGen);
            }
            continue;
        }
        t.kind = TG_REGULAR;
        if (row == 0) {
            t.src = SRC_POS;
        } else if (row == 1) {
            t.src = SRC_NRM;
        } else if (row >= 5 && row < 13) {
            t.src = SRC_TEX;
            t.texIdx = (u8)(row - 5);
        } else {
            t.src = SRC_NONE;
            if (warn) {
                WARN_ONCE("gx: texgen source row %lu not supported\n", (unsigned long)row);
            }
        }
        t.defMtx = g < 4 ? gx_bits(gx.xfReg[0x18], 6 + 6 * g, 6) : gx_bits(gx.xfReg[0x19], 6 * (g - 4), 6);
        t.post = dualTex;
        if (dualTex) {
            u32 post = gx.xfReg[0x50 + g];
            t.postNormalize = (post >> 8) & 1;
            memcpy(t.postMtx, xf_mem_f(0x500 + gx_bits(post, 0, 6) * 4), sizeof(t.postMtx));
            note_xf_mem(0x500 + gx_bits(post, 0, 6) * 4, 12);
        }
    }
}

// Copies a vertex with 16-byte moves (a 144-byte struct copy is a memcpy
// call on some targets).
#if defined(__clang__) || defined(__GNUC__)
typedef f32 XfVec4 __attribute__((vector_size(16), aligned(4), may_alias));
static inline void copy_vtx(GXVtxOut* d, const GXVtxOut* s) {
    static_assert(sizeof(GXVtxOut) == 9 * 16, "GXVtxOut is 9 x 16 bytes");
    const XfVec4* a = (const XfVec4*)s;
    XfVec4* b = (XfVec4*)d;
    XfVec4 t0 = a[0], t1 = a[1], t2 = a[2], t3 = a[3], t4 = a[4], t5 = a[5], t6 = a[6], t7 = a[7], t8 = a[8];
    b[0] = t0;
    b[1] = t1;
    b[2] = t2;
    b[3] = t3;
    b[4] = t4;
    b[5] = t5;
    b[6] = t6;
    b[7] = t7;
    b[8] = t8;
}
#else
static inline void copy_vtx(GXVtxOut* d, const GXVtxOut* s) {
    memcpy(d, s, sizeof(*d));
}
#endif

// --- vertices ------------------------------------------------------------------
//
// Vertices go through in chunks, one stage at a time over the whole chunk
// (position, normal, each light, each channel, each texgen), with the
// intermediates in per-chunk arrays. Every stage's per-draw decisions are
// made once, outside its loop, and the arithmetic stages run over plain
// arrays the compiler vectorizes across vertices: each lane does the same
// IEEE operations as the scalar code, and the scalar code's conditionals
// become selects (both sides computed, one kept).

// A multiple of 4 and 3: chunks hold whole quads and triangles.
enum { XF_CHUNK = 48 };

struct XfScratch {
    f32 ex[XF_CHUNK], ey[XF_CHUNK], ez[XF_CHUNK]; // eye-space position
    f32 nx[XF_CHUNK], ny[XF_CHUNK], nz[XF_CHUNK]; // eye-space normal
    f32 hasN[XF_CHUNK]; // 1: the vertex's normal was transformed (else 0, 0, 1)
    f32 bin[3][XF_CHUNK], tng[3][XF_CHUNK];
    // Per light: spotlight / specular attenuation, N.L (direction / half).
    f32 spotAttn[8][XF_CHUNK], specAttn[8][XF_CHUNK];
    f32 dotGeo[8][XF_CHUNK], dotSpec[8][XF_CHUNK];
    f32 lit[4][XF_CHUNK];
    f32 mat[XF_CHUNK];
    f32 r[3][XF_CHUNK];
    // Position pass with one matrix: the input position / normal, and the
    // clip-space result.
    f32 pin[3][XF_CHUNK], nin[3][XF_CHUNK], clip[4][XF_CHUNK];
};

// Normalizes (x, y, z) where ok and the length is not 0.
static inline void normalize_soa(f32* __restrict x, f32* __restrict y, f32* __restrict z, u32 n) {
    for (u32 i = 0; i < n; i++) {
        f32 a = x[i], b = y[i], c = z[i];
        f32 len = sqrtf(a * a + b * b + c * c);
        bool ok = len > 0.0f;
        x[i] = ok ? a / len : a;
        y[i] = ok ? b / len : b;
        z[i] = ok ? c / len : c;
    }
}

static inline void normalize_soa_if(f32* __restrict x, f32* __restrict y, f32* __restrict z,
                                    const f32* __restrict use, u32 n) {
    for (u32 i = 0; i < n; i++) {
        f32 a = x[i], b = y[i], c = z[i];
        f32 len = sqrtf(a * a + b * b + c * c);
        bool ok = use[i] != 0.0f && len > 0.0f;
        x[i] = ok ? a / len : a;
        y[i] = ok ? b / len : b;
        z[i] = ok ? c / len : c;
    }
}

// Direction / distance to a point light (or spotlight): attenuation and N.L.
template <bool SPOT, bool DOT>
static inline void light_geo(const XfLight& L, const f32* __restrict ex, const f32* __restrict ey,
                             const f32* __restrict ez, const f32* __restrict nx, const f32* __restrict ny,
                             const f32* __restrict nz, f32* __restrict spotAttn, f32* __restrict dotGeo, u32 n) {
    const f32 lp0 = L.pos[0], lp1 = L.pos[1], lp2 = L.pos[2];
    const f32 dv0 = L.dir[0], dv1 = L.dir[1], dv2 = L.dir[2];
    const f32 a0 = L.cosAtt[0], a1 = L.cosAtt[1], a2 = L.cosAtt[2];
    const f32 k0 = L.distAtt[0], k1 = L.distAtt[1], k2 = L.distAtt[2];
    for (u32 i = 0; i < n; i++) {
        f32 d0 = lp0 - ex[i], d1 = lp1 - ey[i], d2 = lp2 - ez[i];
        f32 dist2 = d0 * d0 + d1 * d1 + d2 * d2;
        f32 dist = sqrtf(dist2);
        bool ok = dist > 0.0f;
        f32 l0 = ok ? d0 / dist : 0.0f;
        f32 l1 = ok ? d1 / dist : 0.0f;
        f32 l2 = ok ? d2 / dist : 0.0f;
        if (SPOT) {
            f32 a = l0 * dv0 + l1 * dv1 + l2 * dv2;
            a = a < 0.0f ? 0.0f : a;
            f32 c = a0 + a1 * a + a2 * a * a;
            f32 k = k0 + k1 * dist + k2 * dist2;
            spotAttn[i] = (c > 0.0f ? c : 0.0f) / (k != 0.0f ? k : 1.0f);
        }
        if (DOT) {
            dotGeo[i] = nx[i] * l0 + ny[i] * l1 + nz[i] * l2;
        }
    }
}

// Specular light: the light's position holds a half-angle direction.
static inline void light_spec(const XfLight& L, const f32* __restrict nx, const f32* __restrict ny,
                              const f32* __restrict nz, f32* __restrict specAttn, f32* __restrict dotSpec, u32 n) {
    const f32 h0 = L.half[0], h1 = L.half[1], h2 = L.half[2];
    const f32 dv0 = L.dir[0], dv1 = L.dir[1], dv2 = L.dir[2];
    const f32 a0 = L.cosAtt[0], a1 = L.cosAtt[1], a2 = L.cosAtt[2];
    const f32 k0 = L.distAtt[0], k1 = L.distAtt[1], k2 = L.distAtt[2];
    for (u32 i = 0; i < n; i++) {
        f32 n0 = nx[i], n1 = ny[i], n2 = nz[i];
        f32 nl = n0 * h0 + n1 * h1 + n2 * h2;
        f32 a = nl >= 0.0f ? n0 * dv0 + n1 * dv1 + n2 * dv2 : 0.0f;
        a = a < 0.0f ? 0.0f : a;
        f32 c = a0 + a1 * a + a2 * a * a;
        f32 k = k0 + k1 * a + k2 * a * a;
        specAttn[i] = (c > 0.0f ? c : 0.0f) / (k != 0.0f ? k : 1.0f);
        // N.L with the half vector: the same expression as nl.
        dotSpec[i] = nl;
    }
}

// lit[k] += color[k] * diff * attn over the chunk, for components k0..k1.
// attn / diff NULL: 1 (that factor is off).
static inline void light_accum(const f32 color[4], const f32* __restrict attnP, const f32* __restrict diffP,
                               bool clampDiff, f32 (*__restrict lit)[XF_CHUNK], int k0, int k1, u32 n) {
    for (int k = k0; k <= k1; k++) {
        const f32 col = color[k];
        f32* __restrict l = lit[k];
        if (attnP && diffP) {
            for (u32 i = 0; i < n; i++) {
                f32 diff = diffP[i];
                diff = clampDiff && diff < 0.0f ? 0.0f : diff;
                l[i] += col * diff * attnP[i];
            }
        } else if (attnP) {
            const f32 diff = 1.0f;
            for (u32 i = 0; i < n; i++) {
                l[i] += col * diff * attnP[i];
            }
        } else if (diffP) {
            const f32 attn = 1.0f;
            for (u32 i = 0; i < n; i++) {
                f32 diff = diffP[i];
                diff = clampDiff && diff < 0.0f ? 0.0f : diff;
                l[i] += col * diff * attn;
            }
        } else {
            const f32 diff = 1.0f, attn = 1.0f;
            for (u32 i = 0; i < n; i++) {
                l[i] += col * diff * attn;
            }
        }
    }
}

// A colour component from the vertex (material / ambient source "vertex"):
// its colour channel, or white if it has none.
static inline f32 vtx_comp(const GXVertexIn& v, int chan, int k) {
    return (v.hasClr & (1 << chan)) ? kU8.v[v.clr[chan][k]] : 1.0f;
}

// A lit control: GX lighting for components k0..k1 into out[i].clr[chan].
static void ctrl_lit(const XfDraw& x, const XfCtrl& c, int chan, const GXVertexIn* in, XfScratch& s, int k0, int k1,
                     GXVtxOut* out, u32 n) {
    for (int k = k0; k <= k1; k++) {
        f32* l = s.lit[k];
        if (c.ambVtx) {
            for (u32 i = 0; i < n; i++) {
                l[i] = vtx_comp(in[i], chan, k);
            }
        } else {
            const f32 a = c.amb[k];
            for (u32 i = 0; i < n; i++) {
                l[i] = a;
            }
        }
    }
    bool spec = c.attnEnable && !c.spot;
    for (u32 li = 0; li < c.numLights; li++) {
        u32 l = c.lights[li];
        const f32* attnP = !c.attnEnable ? NULL : spec ? s.specAttn[l] : s.spotAttn[l];
        const f32* diffP = c.diffuseFn == 0 ? NULL : spec ? s.dotSpec[l] : s.dotGeo[l];
        light_accum(x.light[l].color, attnP, diffP, c.diffuseFn == 2, s.lit, k0, k1, n);
    }
    for (int k = k0; k <= k1; k++) {
        f32* __restrict l = s.lit[k];
        f32* __restrict mat = s.mat;
        if (c.matVtx) {
            for (u32 i = 0; i < n; i++) {
                mat[i] = vtx_comp(in[i], chan, k);
            }
        } else {
            const f32 m = c.mat[k];
            for (u32 i = 0; i < n; i++) {
                mat[i] = m;
            }
        }
        // q8(mat * clamp01(lit)), vectorized: floorf of x in [0.5, 255.5]
        // is its truncation; a NaN x is its own floor.
        for (u32 i = 0; i < n; i++) {
            f32 v = mat[i] * clamp01(l[i]);
            f32 xq = clamp01(v) * 255.0f + 0.5f;
            f32 fl = xq == xq ? (f32)(s32)xq : xq;
            l[i] = fl / 255.0f;
        }
        for (u32 i = 0; i < n; i++) {
            out[i].clr[chan][k] = l[i];
        }
    }
}

// Unlit controls: the material colour after q8, components k0..k1 (rgb
// from the colour control, a from the alpha control). From a vertex it is
// already a multiple of 1/255, which q8 returns unchanged
// (n / 255.0f * 255.0f + 0.5f floors to n, fused or not).
static void ctrl_unlit(const XfCtrl& cc, const XfCtrl& ca, int chan, const GXVertexIn* in, int k0, int k1,
                       GXVtxOut* out, u32 n) {
    bool vtx[4];
    f32 q[4];
    bool anyVtx = false;
    for (int k = 0; k < 4; k++) {
        const XfCtrl& c = k < 3 ? cc : ca;
        vtx[k] = c.matVtx;
        q[k] = c.matVtx ? 0.0f : q8(c.mat[k]);
        anyVtx |= k >= k0 && k <= k1 && c.matVtx;
    }
    if (k0 == 0 && k1 == 3) {
        if (!anyVtx) {
            for (u32 i = 0; i < n; i++) {
                memcpy(out[i].clr[chan], q, sizeof(q));
            }
            return;
        }
        for (u32 i = 0; i < n; i++) {
            const GXVertexIn& v = in[i];
            bool has = (v.hasClr & (1 << chan)) != 0;
            f32 c4[4];
            for (int k = 0; k < 4; k++) {
                c4[k] = vtx[k] ? (has ? kU8.v[v.clr[chan][k]] : 1.0f) : q[k];
            }
            memcpy(out[i].clr[chan], c4, sizeof(c4));
        }
        return;
    }
    for (int k = k0; k <= k1; k++) {
        if (vtx[k]) {
            for (u32 i = 0; i < n; i++) {
                out[i].clr[chan][k] = vtx_comp(in[i], chan, k);
            }
        } else {
            for (u32 i = 0; i < n; i++) {
                out[i].clr[chan][k] = q[k];
            }
        }
    }
}

// A regular texgen's source row (src[3] is 1).
template <int SRC>
static inline void texgen_src(const XfTexGen& t, const GXVertexIn& v, f32 src[4]) {
    src[0] = 0;
    src[1] = 0;
    src[2] = 1;
    src[3] = 1;
    if (SRC == SRC_POS) {
        src[0] = v.pos[0];
        src[1] = v.pos[1];
        src[2] = v.pos[2];
    } else if (SRC == SRC_NRM) {
        src[0] = v.nrm[0];
        src[1] = v.nrm[1];
        src[2] = v.nrm[2];
    } else if (SRC == SRC_TEX) {
        src[0] = v.tex[t.texIdx][0];
        src[1] = v.tex[t.texIdx][1];
    }
    if (!t.abc1) {
        src[2] = 1.0f;
    }
}

// A regular texgen's texture matrix product into s.r (r[2] = 1 for 2x4).
template <int SRC>
static void texgen_mtx(const XfTexGen& t, u32 g, bool perVertexMtx, const GXVertexIn* in, XfScratch& s, u32 n) {
    f32* __restrict r0 = s.r[0];
    f32* __restrict r1 = s.r[1];
    f32* __restrict r2 = s.r[2];
    if (!perVertexMtx) {
        f32 m[12];
        memcpy(m, xf_mem_f(t.defMtx * 4), (t.stq ? 12 : 8) * sizeof(f32));
        if (t.stq) {
            for (u32 i = 0; i < n; i++) {
                f32 src[4], o[3];
                texgen_src<SRC>(t, in[i], src);
                mtx3x4(m, src, o);
                r0[i] = o[0];
                r1[i] = o[1];
                r2[i] = o[2];
            }
        } else {
            for (u32 i = 0; i < n; i++) {
                f32 src[4], o[2];
                texgen_src<SRC>(t, in[i], src);
                mtx2x4(m, src, o);
                r0[i] = o[0];
                r1[i] = o[1];
                r2[i] = 1.0f;
            }
        }
        return;
    }
    for (u32 i = 0; i < n; i++) {
        const GXVertexIn& v = in[i];
        f32 src[4], m[12], o[3] = {0, 0, 1};
        texgen_src<SRC>(t, v, src);
        u32 tm = (v.hasTexMtx & (1 << g)) ? v.texMtx[g] : t.defMtx;
        if (t.stq) {
            memcpy(m, xf_mem_f(tm * 4), 12 * sizeof(f32));
            mtx3x4(m, src, o);
        } else {
            memcpy(m, xf_mem_f(tm * 4), 8 * sizeof(f32));
            mtx2x4(m, src, o);
        }
        r0[i] = o[0];
        r1[i] = o[1];
        r2[i] = o[2];
    }
}

// The dual-texture post matrix (rows 0..rows-1) over s.r.
static inline void texgen_post(const f32 pm[12], bool stq, XfScratch& s, u32 n) {
    f32* __restrict r0 = s.r[0];
    f32* __restrict r1 = s.r[1];
    f32* __restrict r2 = s.r[2];
    const f32 m0 = pm[0], m1 = pm[1], m2 = pm[2], m3 = pm[3];
    const f32 m4 = pm[4], m5 = pm[5], m6 = pm[6], m7 = pm[7];
    const f32 m8 = pm[8], m9 = pm[9], m10 = pm[10], m11 = pm[11];
    const f32 one = 1.0f;
    if (stq) {
        for (u32 i = 0; i < n; i++) {
            f32 a = r0[i], b = r1[i], c = r2[i];
            r0[i] = m0 * a + m1 * b + m2 * c + m3 * one;
            r1[i] = m4 * a + m5 * b + m6 * c + m7 * one;
            r2[i] = m8 * a + m9 * b + m10 * c + m11 * one;
        }
    } else {
        // The third row only feeds q, which is 1 here.
        for (u32 i = 0; i < n; i++) {
            f32 a = r0[i], b = r1[i], c = r2[i];
            r0[i] = m0 * a + m1 * b + m2 * c + m3 * one;
            r1[i] = m4 * a + m5 * b + m6 * c + m7 * one;
        }
    }
}

// The position pass for a chunk with one position matrix, over arrays.
template <bool ORTHO>
static inline void pos_uniform(const XfDraw& x, const f32 m[12], const f32* __restrict px, const f32* __restrict py,
                               const f32* __restrict pz, f32* __restrict ex, f32* __restrict ey, f32* __restrict ez,
                               f32* __restrict c0, f32* __restrict c1, f32* __restrict c2, f32* __restrict c3, u32 n) {
    const f32 p0 = x.p[0], p1 = x.p[1], p2 = x.p[2], p3 = x.p[3], p4 = x.p[4], p5 = x.p[5];
    const f32 xs = x.xs, xo = x.xo, ys = x.ys, yo = x.yo, zs = x.zs, zo = x.zo;
    const f32 m0 = m[0], m1 = m[1], m2 = m[2], m3 = m[3], m4 = m[4], m5 = m[5];
    const f32 m6 = m[6], m7 = m[7], m8 = m[8], m9 = m[9], m10 = m[10], m11 = m[11];
    const f32 one = 1.0f;
    for (u32 i = 0; i < n; i++) {
        f32 v0 = px[i], v1 = py[i], v2 = pz[i];
        // mtx3x4 with v[3] = 1.
        f32 e0 = m0 * v0 + m1 * v1 + m2 * v2 + m3 * one;
        f32 e1 = m4 * v0 + m5 * v1 + m6 * v2 + m7 * one;
        f32 e2 = m8 * v0 + m9 * v1 + m10 * v2 + m11 * one;
        ex[i] = e0;
        ey[i] = e1;
        ez[i] = e2;
        f32 cx, cy, cz, cw;
        if (ORTHO) {
            cx = p0 * e0 + p1;
            cy = p2 * e1 + p3;
            cz = p4 * e2 + p5;
            cw = 1.0f;
        } else {
            cx = p0 * e0 + p1 * e2;
            cy = p2 * e1 + p3 * e2;
            cz = p4 * e2 + p5;
            cw = -e2;
        }
        c0[i] = cx * xs + cw * xo;
        c1[i] = cy * ys + cw * yo;
        c2[i] = cz * zs + cw * zo;
        c3[i] = cw;
    }
}

// The normal through one normal matrix, over arrays ((0, 0, 1) where the
// vertex has none).
static inline void nrm_uniform(const f32 m[9], const f32* __restrict ix, const f32* __restrict iy,
                               const f32* __restrict iz, const f32* __restrict has, f32* __restrict nx,
                               f32* __restrict ny, f32* __restrict nz, u32 n) {
    const f32 m0 = m[0], m1 = m[1], m2 = m[2], m3 = m[3], m4 = m[4], m5 = m[5], m6 = m[6], m7 = m[7], m8 = m[8];
    for (u32 i = 0; i < n; i++) {
        f32 v0 = ix[i], v1 = iy[i], v2 = iz[i];
        bool h = has[i] != 0.0f;
        f32 a = m0 * v0 + m1 * v1 + m2 * v2;
        f32 b = m3 * v0 + m4 * v1 + m5 * v2;
        f32 c = m6 * v0 + m7 * v1 + m8 * v2;
        nx[i] = h ? a : 0.0f;
        ny[i] = h ? b : 0.0f;
        nz[i] = h ? c : 1.0f;
    }
}

// Position, projection, viewport; normal and NBT through the normal matrix.
template <bool ORTHO, bool NRM, bool NBT>
static void pos_pass(const XfDraw& x, const GXVertexIn* in, u32 n, GXVtxOut* out, XfScratch& s, u32* texMtxAny) {
    // The draw's terms in locals: stores into the output may alias anything
    // (-fno-strict-aliasing), locals not.
    const f32 p0 = x.p[0], p1 = x.p[1], p2 = x.p[2], p3 = x.p[3], p4 = x.p[4], p5 = x.p[5];
    const f32 xs = x.xs, xo = x.xo, ys = x.ys, yo = x.yo, zs = x.zs, zo = x.zo;
    const u32 defPos = x.defPos;
    u32 curPm = ~0u, tmAny = 0;
    f32 pmtx[12], nmtx[9];
    if (!NBT) {
        // One position matrix for the whole chunk (the usual case): the
        // same arithmetic over arrays.
        u32 pm0 = in[0].hasPnMtx ? in[0].pnMtx : defPos;
        bool uniform = true;
        for (u32 i = 0; i < n; i++) {
            const GXVertexIn& v = in[i];
            tmAny |= v.hasTexMtx;
            uniform &= (v.hasPnMtx ? v.pnMtx : defPos) == pm0;
        }
        if (uniform) {
            *texMtxAny = tmAny;
            for (u32 i = 0; i < n; i++) {
                s.pin[0][i] = in[i].pos[0];
                s.pin[1][i] = in[i].pos[1];
                s.pin[2][i] = in[i].pos[2];
            }
            memcpy(pmtx, xf_mem_f(pm0 * 4), sizeof(pmtx));
            pos_uniform<ORTHO>(x, pmtx, s.pin[0], s.pin[1], s.pin[2], s.ex, s.ey, s.ez, s.clip[0], s.clip[1],
                               s.clip[2], s.clip[3], n);
            for (u32 i = 0; i < n; i++) {
                f32 pos[4] = {s.clip[0][i], s.clip[1][i], s.clip[2][i], s.clip[3][i]};
                memcpy(out[i].pos, pos, sizeof(pos));
            }
            if (NRM) {
                for (u32 i = 0; i < n; i++) {
                    s.nin[0][i] = in[i].nrm[0];
                    s.nin[1][i] = in[i].nrm[1];
                    s.nin[2][i] = in[i].nrm[2];
                    s.hasN[i] = in[i].hasNrm ? 1.0f : 0.0f;
                }
                memcpy(nmtx, xf_mem_f(0x400 + (pm0 & 31) * 3), sizeof(nmtx));
                nrm_uniform(nmtx, s.nin[0], s.nin[1], s.nin[2], s.hasN, s.nx, s.ny, s.nz, n);
            }
            return;
        }
        tmAny = 0;
    }
    for (u32 i = 0; i < n; i++) {
        const GXVertexIn& v = in[i];
        tmAny |= v.hasTexMtx;
        u32 pm = v.hasPnMtx ? v.pnMtx : defPos;
        if (pm != curPm) {
            curPm = pm;
            memcpy(pmtx, xf_mem_f(pm * 4), sizeof(pmtx));
            if (NRM || NBT) {
                memcpy(nmtx, xf_mem_f(0x400 + (pm & 31) * 3), sizeof(nmtx));
            }
        }
        f32 pin[4] = {v.pos[0], v.pos[1], v.pos[2], 1.0f};
        f32 eye[3];
        mtx3x4(pmtx, pin, eye);
        s.ex[i] = eye[0];
        s.ey[i] = eye[1];
        s.ez[i] = eye[2];

        f32 cx, cy, cz, cw;
        if (ORTHO) {
            cx = p0 * eye[0] + p1;
            cy = p2 * eye[1] + p3;
            cz = p4 * eye[2] + p5;
            cw = 1.0f;
        } else {
            cx = p0 * eye[0] + p1 * eye[2];
            cy = p2 * eye[1] + p3 * eye[2];
            cz = p4 * eye[2] + p5;
            cw = -eye[2];
        }
        f32 pos[4];
        pos[0] = cx * xs + cw * xo;
        pos[1] = cy * ys + cw * yo;
        pos[2] = cz * zs + cw * zo;
        pos[3] = cw;
        memcpy(out[i].pos, pos, sizeof(pos));

        if (NRM) {
            f32 nn[3] = {0, 0, 1};
            if (v.hasNrm) {
                for (int r = 0; r < 3; r++) {
                    nn[r] = nmtx[r * 3] * v.nrm[0] + nmtx[r * 3 + 1] * v.nrm[1] + nmtx[r * 3 + 2] * v.nrm[2];
                }
            }
            s.nx[i] = nn[0];
            s.ny[i] = nn[1];
            s.nz[i] = nn[2];
            s.hasN[i] = v.hasNrm ? 1.0f : 0.0f;
        }
        // Binormal and tangent through the normal matrix, not normalized
        // (Dolphin's VertexShaderGen _binormal / _tangent).
        if (NBT) {
            for (int r = 0; r < 3; r++) {
                f32 m0 = nmtx[r * 3], m1 = nmtx[r * 3 + 1], m2 = nmtx[r * 3 + 2];
                s.bin[r][i] = m0 * v.binormal[0] + m1 * v.binormal[1] + m2 * v.binormal[2];
                s.tng[r][i] = m0 * v.tangent[0] + m1 * v.tangent[1] + m2 * v.tangent[2];
            }
        }
    }
    *texMtxAny = tmAny;
}

// The texture coordinates from activeGens on: (0, 0, 1), as fixed-size
// block copies.
template <u32 AG>
static void tex_defaults_n(GXVtxOut* out, u32 n) {
    static const f32 kDef[24] = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    for (u32 i = 0; i < n; i++) {
        memcpy(out[i].tex[AG], kDef, (8 - AG) * 3 * sizeof(f32));
    }
}

static void tex_defaults(u32 activeGens, GXVtxOut* out, u32 n) {
    switch (activeGens) {
    case 0: tex_defaults_n<0>(out, n); break;
    case 1: tex_defaults_n<1>(out, n); break;
    case 2: tex_defaults_n<2>(out, n); break;
    case 3: tex_defaults_n<3>(out, n); break;
    case 4: tex_defaults_n<4>(out, n); break;
    case 5: tex_defaults_n<5>(out, n); break;
    case 6: tex_defaults_n<6>(out, n); break;
    case 7: tex_defaults_n<7>(out, n); break;
    default: break;
    }
}

// Transforms n <= XF_CHUNK vertices into out[0..n).
static void transform_chunk(const XfDraw& x, const GXVertexIn* in, u32 n, GXVtxOut* out, XfScratch& s) {
    u32 texMtxAny = 0;

    // Position, projection, viewport; normal and NBT through the normal
    // matrix; the defaults of the unused texture coordinates.
    {
        static void (*const kPos[8])(const XfDraw&, const GXVertexIn*, u32, GXVtxOut*, XfScratch&, u32*) = {
            pos_pass<false, false, false>, pos_pass<false, false, true>, pos_pass<false, true, false>,
            pos_pass<false, true, true>,   pos_pass<true, false, false>, pos_pass<true, false, true>,
            pos_pass<true, true, false>,   pos_pass<true, true, true>};
        kPos[(x.ortho ? 4 : 0) | (x.needNormal ? 2 : 0) | (x.needNbt ? 1 : 0)](x, in, n, out, s, &texMtxAny);
        tex_defaults(x.activeGens, out, n);
    }
    if (x.needNormal) {
        normalize_soa_if(s.nx, s.ny, s.nz, s.hasN, n);
    }

    // Lights: the terms every control that uses them reads.
    for (u32 li = 0; li < x.numLights; li++) {
        u32 l = x.lightIds[li];
        const XfLight& L = x.light[l];
        if (L.geo) {
            if (L.spotAttn && L.dotGeo) {
                light_geo<true, true>(L, s.ex, s.ey, s.ez, s.nx, s.ny, s.nz, s.spotAttn[l], s.dotGeo[l], n);
            } else if (L.spotAttn) {
                light_geo<true, false>(L, s.ex, s.ey, s.ez, s.nx, s.ny, s.nz, s.spotAttn[l], s.dotGeo[l], n);
            } else if (L.dotGeo) {
                light_geo<false, true>(L, s.ex, s.ey, s.ez, s.nx, s.ny, s.nz, s.spotAttn[l], s.dotGeo[l], n);
            }
        }
        if (L.specAttn) {
            light_spec(L, s.nx, s.ny, s.nz, s.specAttn[l], s.dotSpec[l], n);
        }
    }

    // Colour channels.
    for (u32 c = 0; c < 2; c++) {
        if (c >= x.activeChans) {
            for (u32 i = 0; i < n; i++) {
                memset(out[i].clr[c], 0, sizeof(out[i].clr[c]));
            }
            continue;
        }
        const XfCtrl& cc = x.ctrl[c][0];
        const XfCtrl& ca = x.ctrl[c][1];
        if (!cc.lit && !ca.lit) {
            ctrl_unlit(cc, ca, (int)c, in, 0, 3, out, n);
            continue;
        }
        if (cc.lit) {
            // With an identical alpha control, alpha comes from the same
            // evaluation.
            ctrl_lit(x, cc, (int)c, in, s, 0, x.sameCA[c] ? 3 : 2, out, n);
        } else {
            ctrl_unlit(cc, ca, (int)c, in, 0, 2, out, n);
        }
        if (ca.lit) {
            if (!x.sameCA[c]) {
                ctrl_lit(x, ca, (int)c, in, s, 3, 3, out, n);
            }
        } else {
            ctrl_unlit(cc, ca, (int)c, in, 3, 3, out, n);
        }
    }
    // Texture coordinates.
    for (u32 g = 0; g < x.activeGens; g++) {
        const XfTexGen& t = x.gen[g];
        if (t.kind == TG_REGULAR) {
            bool perVertexMtx = (texMtxAny >> g) & 1;
            switch (t.src) {
            case SRC_POS: texgen_mtx<SRC_POS>(t, g, perVertexMtx, in, s, n); break;
            case SRC_NRM: texgen_mtx<SRC_NRM>(t, g, perVertexMtx, in, s, n); break;
            case SRC_TEX: texgen_mtx<SRC_TEX>(t, g, perVertexMtx, in, s, n); break;
            default: texgen_mtx<SRC_NONE>(t, g, perVertexMtx, in, s, n); break;
            }
            if (t.post) {
                if (t.postNormalize) {
                    normalize_soa(s.r[0], s.r[1], s.r[2], n);
                }
                texgen_post(t.postMtx, t.stq, s, n);
            }
            for (u32 i = 0; i < n; i++) {
                f32* tc = out[i].tex[g];
                tc[0] = s.r[0][i];
                tc[1] = s.r[1][i];
                tc[2] = t.stq ? s.r[2][i] : 1.0f;
            }
        } else if (t.kind == TG_COLOR) {
            // Colour texgen: s, t from the vertex colour's red and green.
            int c = t.chan;
            for (u32 i = 0; i < n; i++) {
                const GXVertexIn& v = in[i];
                bool has = (v.hasClr & (1 << c)) != 0;
                f32* tc = out[i].tex[g];
                tc[0] = has ? kU8.v[v.clr[c][0]] : 0.0f;
                tc[1] = has ? kU8.v[v.clr[c][1]] : 0.0f;
                tc[2] = 1.0f;
            }
        } else {
            for (u32 i = 0; i < n; i++) {
                f32 l[3] = {t.lpos[0] - s.ex[i], t.lpos[1] - s.ey[i], t.lpos[2] - s.ez[i]};
                f32 llen = sqrtf(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
                if (llen > 0.0f) {
                    div3(l, llen);
                }
                static const f32 kNone[3] = {0.0f, 0.0f, 1.0f};
                const f32* src = t.srcOk ? out[i].tex[t.srcGen] : kNone;
                f32 s0 = src[0], s1 = src[1], s2 = src[2];
                f32* tc = out[i].tex[g];
                tc[0] = s0 + l[0] * s.bin[0][i] + l[1] * s.bin[1][i] + l[2] * s.bin[2][i];
                tc[1] = s1 + l[0] * s.tng[0][i] + l[1] * s.tng[1][i] + l[2] * s.tng[2][i];
                tc[2] = s2;
            }
        }
    }
}

// Expands a line or point into two triangles in screen space.
static void emit_quad(GXVtxOut* out, const GXVtxOut& a, const GXVtxOut& b, f32 halfPx, bool point,
                      const XfDraw& x) {
    f32 w = x.efbW, h = x.efbH;
    f32 off[4][2];
    if (point) {
        f32 ox = halfPx * 2.0f / w, oy = halfPx * 2.0f / h;
        f32 o[4][2] = {{-ox, oy}, {ox, oy}, {ox, -oy}, {-ox, -oy}};
        memcpy(off, o, sizeof(off));
    } else {
        f32 ax = a.pos[0] / a.pos[3] * w * 0.5f, ay = a.pos[1] / a.pos[3] * h * 0.5f;
        f32 bx = b.pos[0] / b.pos[3] * w * 0.5f, by = b.pos[1] / b.pos[3] * h * 0.5f;
        f32 dx = bx - ax, dy = by - ay;
        f32 len = sqrtf(dx * dx + dy * dy);
        if (len <= 0.0f) {
            len = 1.0f;
        }
        f32 px = -dy / len * halfPx * 2.0f / w, py = dx / len * halfPx * 2.0f / h;
        f32 o[4][2] = {{px, py}, {-px, -py}, {-px, -py}, {px, py}};
        memcpy(off, o, sizeof(off));
    }
    // Corners: points 0-3 around the centre; lines a+, a-, b-, b+.
    const GXVtxOut* src[4] = {&a, &a, point ? &a : &b, point ? &a : &b};
    GXVtxOut c[4];
    for (int i = 0; i < 4; i++) {
        c[i] = *src[i];
        c[i].pos[0] += off[i][0] * c[i].pos[3];
        c[i].pos[1] += off[i][1] * c[i].pos[3];
    }
    static const int idx[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; i++) {
        out[i] = c[idx[i]];
    }
}

// --- primitives ----------------------------------------------------------------
//
// A primitive is transformed and assembled chunk by chunk: the chunk's
// vertices (after up to two carried over from the previous chunk, for
// strips and fans) go into sBuf, its triangles into sTri, and from there
// onto the output in one copy. Both stay in the L1 cache however large the
// primitive is (a whole primitive staged first would not: the Earth is one
// triangle list of 27288 vertices).

static GXVtxOut sBuf[XF_CHUNK + 2];
static XfScratch sScratch;
static GXVtxOut sTri[XF_CHUNK * 3];
static GXVtxOut sFan0;

static inline void append(std::vector<GXVtxOut>& out, const GXVtxOut* v, u32 n) {
    out.insert(out.end(), v, v + n);
}

u32 gx_xform_primitive(u32 primitive, const GXVertexIn* in, u32 count, std::vector<GXVtxOut>& out) {
    u32 tris;
    switch (primitive) {
    case 0x80: // quads
    case 0x88:
        tris = count / 4 * 2;
        break;
    case 0x90:
        tris = count / 3;
        break;
    case 0x98:
    case 0xA0:
        tris = count >= 3 ? count - 2 : 0;
        break;
    case 0xA8: // lines
        tris = count / 2 * 2;
        break;
    case 0xB0: // line strip
        tris = count >= 2 ? (count - 1) * 2 : 0;
        break;
    case 0xB8: // points
        tris = count * 2;
        break;
    default:
        tris = 0;
        break;
    }
    if (tris == 0) {
        return 0;
    }
    const XfDraw& x = xf_setup();
    XfScratch& s = sScratch;

    u32 n = tris * 3;
    size_t need = out.size() + n;
    if (out.capacity() < need) {
        size_t cap = out.capacity() * 2;
        out.reserve(cap > need ? cap : need);
    }
    // BP 0x22: line width (bits 0-7) and point size (8-15) in 1/6 pixel.
    f32 lineHalf = gx_bits(gx.bp[0x22], 0, 8) / 12.0f;
    f32 pointHalf = gx_bits(gx.bp[0x22], 8, 8) / 12.0f;

    // Vertices used: whole quads / triangles / line pairs; all otherwise.
    u32 used = primitive == 0x90 ? count / 3 * 3
             : primitive <= 0x88 ? count / 4 * 4
             : primitive == 0xA8 ? count / 2 * 2
                                 : count;
    // Vertices carried into the next chunk: strips 2, fans and line strips 1.
    u32 keep = primitive == 0x98 ? 2 : primitive == 0xA0 || primitive == 0xB0 ? 1 : 0;
    u32 carry = 0;
    u32 tri = 0; // strip: index of the next triangle
    GXVtxOut q[6];
    for (u32 base = 0; base < used; base += XF_CHUNK) {
        u32 m = used - base < XF_CHUNK ? used - base : XF_CHUNK;
        transform_chunk(x, in + base, m, sBuf + carry, s);
        // sBuf[j] is vertex first + j.
        u32 have = carry + m, first = base - carry;
        GXVtxOut* t = sTri;
        switch (primitive) {
        case 0x80:
        case 0x88:
            for (u32 i = 0; i + 3 < have; i += 4) {
                copy_vtx(t++, &sBuf[i]);
                copy_vtx(t++, &sBuf[i + 1]);
                copy_vtx(t++, &sBuf[i + 2]);
                copy_vtx(t++, &sBuf[i]);
                copy_vtx(t++, &sBuf[i + 2]);
                copy_vtx(t++, &sBuf[i + 3]);
            }
            append(out, sTri, (u32)(t - sTri));
            break;
        case 0x90:
            append(out, sBuf, have);
            break;
        case 0x98:
            // Keep every strip triangle's winding the same as the first's.
            for (; tri + 2 < first + have; tri++) {
                u32 j = tri - first;
                copy_vtx(t++, &sBuf[(tri & 1) ? j + 1 : j]);
                copy_vtx(t++, &sBuf[(tri & 1) ? j : j + 1]);
                copy_vtx(t++, &sBuf[j + 2]);
            }
            append(out, sTri, (u32)(t - sTri));
            break;
        case 0xA0:
            if (base == 0) {
                copy_vtx(&sFan0, &sBuf[0]);
            }
            for (u32 j = base == 0 ? 1 : 0; j + 1 < have; j++) {
                copy_vtx(t++, &sFan0);
                copy_vtx(t++, &sBuf[j]);
                copy_vtx(t++, &sBuf[j + 1]);
            }
            append(out, sTri, (u32)(t - sTri));
            break;
        case 0xA8:
        case 0xB0: {
            u32 step = primitive == 0xA8 ? 2 : 1;
            for (u32 j = 0; j + 1 < have; j += step) {
                emit_quad(q, sBuf[j], sBuf[j + 1], lineHalf, false, x);
                append(out, q, 6);
            }
            break;
        }
        case 0xB8:
            for (u32 j = 0; j < have; j++) {
                emit_quad(q, sBuf[j], sBuf[j], pointHalf, true, x);
                append(out, q, 6);
            }
            break;
        }
        // Carry the last vertices into the next chunk.
        carry = have < keep ? have : keep;
        for (u32 j = 0; j < carry; j++) {
            copy_vtx(&sBuf[j], &sBuf[have - carry + j]);
        }
    }
    return n;
}
