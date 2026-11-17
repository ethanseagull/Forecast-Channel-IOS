// Texture block codecs: BC1 -> ASTC 4x4 transcoding, a BC1 encoder and a
// reference BC1 decoder (gx_texcodec.h).
//
// ASTC layout used by the transcoder (Khronos Data Format Specification,
// "ASTC Compressed Texture Image Formats"); every block is one partition with
// a 4x4 weight grid, so weight i belongs to texel i and needs no infill:
//
//   BC1 block                     ASTC block
//   all 16 texels one colour      void extent, the colour in UNORM16 (exact)
//   4 colours (color0 > color1)   mode 0x042: 2-bit weights (0, 21, 43, 64 /64),
//                                 CEM 8 (RGB direct), endpoints 8-bit (exact)
//   3 colours, none transparent   mode 0x051: trit weights (0, 32, 64 /64),
//                                 CEM 8, endpoints 8-bit (exact)
//   3 colours + transparent       mode 0x451: dual plane, trit weights, CEM 12
//                                 (RGBA direct), alpha in plane 2 (CCS 3),
//                                 endpoints 7-bit (128 levels: RGB within 1)
//
// Bit budgets (128 bits; 17 for mode, partition count and CEM; endpoints get
// what the weights leave, at the largest ISE range that fits):
//   0x042: 16 x 2 = 32 weight bits, 79 left: 6 x 8 bits    (range 256)
//   0x051: ceil(16 * 8 / 5) = 26,   85 left: 6 x 8 bits    (range 256)
//   0x451: ceil(32 * 8 / 5) = 52, 2 for the CCS, 57 left: 8 x 7 bits (range 128;
//          range 160 would need 59 and 192 would need 61)
#include "gx_texcodec.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace {

// For the transparent texels of 3-colour blocks the ASTC block can give any
// of the three opaque palette colours (at alpha 0). true: the average colour,
// as the Wii decodes CMPR (and Dolphin does), which also filters without dark
// fringes; false: the palette colour nearest to BC1's transparent black.
const bool kTransparentRgbAverage = true;

#ifndef GX_TEXCODEC_REFINE_PASSES
#define GX_TEXCODEC_REFINE_PASSES 1
#endif
const int kRefinePasses = GX_TEXCODEC_REFINE_PASSES;

// ---- BC1 ---------------------------------------------------------------------

inline unsigned rd16(const uint8_t* p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

inline void wr16(uint8_t* p, unsigned v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

inline void wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

inline int x5to8(unsigned v) {
    return (int)((v << 3) | (v >> 2));
}

inline int x6to8(unsigned v) {
    return (int)((v << 2) | (v >> 4));
}

// RGB565 -> 8-bit RGB by bit replication.
inline void expand565(unsigned c, int out[3]) {
    out[0] = x5to8(c >> 11);
    out[1] = x6to8((c >> 5) & 63);
    out[2] = x5to8(c & 31);
}

// The palette of a BC1 block as RGBA8 (gx_bc1_decode's rounding).
void bc1_palette(unsigned c0, unsigned c1, uint8_t pal[4][4]) {
    int a[3], b[3];
    expand565(c0, a);
    expand565(c1, b);
    for (int k = 0; k < 3; k++) {
        pal[0][k] = (uint8_t)a[k];
        pal[1][k] = (uint8_t)b[k];
        if (c0 > c1) {
            pal[2][k] = (uint8_t)((2 * a[k] + b[k] + 1) / 3);
            pal[3][k] = (uint8_t)((a[k] + 2 * b[k] + 1) / 3);
        } else {
            pal[2][k] = (uint8_t)((a[k] + b[k] + 1) / 2);
            pal[3][k] = 0;
        }
    }
    pal[0][3] = pal[1][3] = pal[2][3] = 255;
    pal[3][3] = c0 > c1 ? 255 : 0;
}

// ---- ASTC writing ------------------------------------------------------------

// A 128-bit block as two little-endian 64-bit halves.
struct Block128 {
    uint64_t lo = 0, hi = 0;

    void put(unsigned pos, unsigned n, uint64_t v) {
        if (pos < 64) {
            lo |= v << pos;
            if (pos + n > 64) {
                hi |= v >> (64 - pos);
            }
        } else {
            hi |= v << (pos - 64);
        }
    }

    void store(uint8_t* out) const {
        for (int i = 0; i < 8; i++) {
            out[i] = (uint8_t)(lo >> (8 * i));
            out[8 + i] = (uint8_t)(hi >> (8 * i));
        }
    }
};

uint64_t reverse64(uint64_t v) {
    v = ((v >> 1) & 0x5555555555555555ull) | ((v & 0x5555555555555555ull) << 1);
    v = ((v >> 2) & 0x3333333333333333ull) | ((v & 0x3333333333333333ull) << 2);
    v = ((v >> 4) & 0x0F0F0F0F0F0F0F0Full) | ((v & 0x0F0F0F0F0F0F0F0Full) << 4);
    v = ((v >> 8) & 0x00FF00FF00FF00FFull) | ((v & 0x00FF00FF00FF00FFull) << 8);
    v = ((v >> 16) & 0x0000FFFF0000FFFFull) | ((v & 0x0000FFFF0000FFFFull) << 16);
    return (v >> 32) | (v << 32);
}

// Weight data is stored from the top of the block down with its bits
// reversed: bit i of the weight ISE stream is block bit 127 - i. Streams of at
// most 64 bits only, which is all this file writes.
void put_weights(Block128& b, uint64_t stream) {
    b.hi |= reverse64(stream);
}

// The 8-bit packing of five trits (t0 + 3 t1 + 9 t2 + 27 t3 + 81 t4), built by
// inverting the specification's unpacking procedure.
struct TritTable {
    uint8_t pack[243];

    TritTable() {
        bool set[243] = {};
        for (unsigned T = 0; T < 256; T++) {
            unsigned t[5], C;
            if (((T >> 2) & 7) == 7) {
                C = (((T >> 5) & 7) << 2) | (T & 3);
                t[4] = 2;
                t[3] = 2;
            } else {
                C = T & 31;
                if (((T >> 5) & 3) == 3) {
                    t[4] = 2;
                    t[3] = (T >> 7) & 1;
                } else {
                    t[4] = (T >> 7) & 1;
                    t[3] = (T >> 5) & 3;
                }
            }
            if ((C & 3) == 3) {
                t[2] = 2;
                t[1] = (C >> 4) & 1;
                t[0] = (((C >> 3) & 1) << 1) | (((C >> 2) & 1) & ~((C >> 3) & 1));
            } else if (((C >> 2) & 3) == 3) {
                t[2] = 2;
                t[1] = 2;
                t[0] = C & 3;
            } else {
                t[2] = (C >> 4) & 1;
                t[1] = (C >> 2) & 3;
                t[0] = ((C >> 1) & 1) << 1 | ((C & 1) & ~((C >> 1) & 1));
            }
            unsigned i = t[0] + 3 * t[1] + 9 * t[2] + 27 * t[3] + 81 * t[4];
            if (!set[i]) {
                set[i] = true;
                pack[i] = (uint8_t)T;
            }
        }
    }
};

const TritTable& trits() {
    static const TritTable table;
    return table;
}

// ISE of n values in 0..2 (trits, no extra bits): each group of five packs
// into 8 bits T, written T bits 0..7 in order; a last partial group of k
// values writes only its first ceil(8k / 5) bits. Returns the bit count.
unsigned encode_trits(const uint8_t* v, unsigned n, uint64_t* stream) {
    static const unsigned partial[6] = {0, 2, 4, 5, 7, 8};
    const TritTable& tt = trits();
    uint64_t s = 0;
    unsigned pos = 0;
    for (unsigned i = 0; i < n; i += 5) {
        unsigned k = n - i < 5 ? n - i : 5;
        unsigned t[5] = {0, 0, 0, 0, 0};
        for (unsigned j = 0; j < k; j++) {
            t[j] = v[i + j];
        }
        uint64_t T = tt.pack[t[0] + 3 * t[1] + 9 * t[2] + 27 * t[3] + 81 * t[4]];
        unsigned bits = partial[k];
        s |= (T & ((1u << bits) - 1)) << pos;
        pos += bits;
    }
    *stream = s;
    return pos;
}

// 8-bit value -> nearest 7-bit ISE value (range 128: unquantised by bit
// replication, (v << 1) | (v >> 6)).
struct Quant7 {
    uint8_t q[256];

    Quant7() {
        for (int x = 0; x < 256; x++) {
            int best = 0, bestErr = 1 << 30;
            for (int v = 0; v < 128; v++) {
                int err = abs(((v << 1) | (v >> 6)) - x);
                if (err < bestErr) {
                    bestErr = err;
                    best = v;
                }
            }
            q[x] = (uint8_t)best;
        }
    }

    static int unq(int v) {
        return (v << 1) | (v >> 6);
    }
};

const Quant7& quant7() {
    static const Quant7 table;
    return table;
}

const unsigned kModeQuant4 = 0x042; // 4x4 grid, weights 0..3, one plane
const unsigned kModeQuant3 = 0x051; // 4x4 grid, weights 0..2, one plane
const unsigned kModeQuant3Dual = 0x451; // the same, dual plane

void put_header(Block128& b, unsigned mode, unsigned cem) {
    b.put(0, 11, mode);
    b.put(11, 2, 0); // one partition
    b.put(13, 4, cem);
}

void void_extent(const uint8_t rgba[4], uint8_t* out) {
    // Bits 0-8 0x1FC, bit 9 0 (LDR), bits 10-11 reserved 1s, 12-63 all 1s
    // (no extent), then R, G, B, A as UNORM16.
    static const uint8_t head[8] = {0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    memcpy(out, head, 8);
    for (int k = 0; k < 4; k++) {
        out[8 + 2 * k] = rgba[k];
        out[9 + 2 * k] = rgba[k];
    }
}

void transcode_block(const uint8_t* in, uint8_t* out) {
    unsigned c0 = rd16(in), c1 = rd16(in + 2);
    uint32_t idx = rd32(in + 4);
    bool four = c0 > c1;

    uint8_t pal[4][4];
    bc1_palette(c0, c1, pal);
    if (!four && kTransparentRgbAverage) {
        memcpy(pal[3], pal[2], 3); // for all-transparent blocks
    }
    unsigned first = idx & 3;
    if (idx == first * 0x55555555u) {
        void_extent(pal[first], out);
        return;
    }
    // Index 3 in 3-colour mode: bits 11 in some 2-bit field.
    bool transparent = !four && ((idx & (idx >> 1) & 0x55555555u) != 0);
    if (!four && c0 == c1 && !transparent) {
        void_extent(pal[0], out);
        return;
    }

    int e0[3], e1[3];
    expand565(c0, e0);
    expand565(c1, e1);
    Block128 b;

    if (!transparent) {
        // ASTC CEM 8 decodes endpoints in the stored order only when
        // r1 + g1 + b1 >= r0 + g0 + b0; else it swaps them (and blue-
        // contracts). Store them in that order, mirroring the weights.
        bool swap = e1[0] + e1[1] + e1[2] < e0[0] + e0[1] + e0[2];
        const int* lo = swap ? e1 : e0;
        const int* hi = swap ? e0 : e1;
        uint64_t stream = 0;
        if (four) {
            // Index 0 = c0, 1 = c1, 2 = 1/3 of the way, 3 = 2/3 -> weights
            // 0, 64, 21, 43 = ISE values 0, 3, 1, 2.
            static const unsigned q4[4] = {0, 3, 1, 2};
            for (int i = 0; i < 16; i++) {
                unsigned q = q4[(idx >> (2 * i)) & 3];
                stream |= (uint64_t)(swap ? 3 - q : q) << (2 * i);
            }
            put_header(b, kModeQuant4, 8);
        } else {
            // Index 0 = c0, 1 = c1, 2 = the middle -> weights 0, 64, 32.
            static const uint8_t q3[3] = {0, 2, 1};
            uint8_t w[16];
            for (int i = 0; i < 16; i++) {
                uint8_t q = q3[(idx >> (2 * i)) & 3];
                w[i] = swap ? (uint8_t)(2 - q) : q;
            }
            encode_trits(w, 16, &stream);
            put_header(b, kModeQuant3, 8);
        }
        // CEM 8 values: r0, r1, g0, g1, b0, b1, at range 256 (8 bits each).
        for (int k = 0; k < 3; k++) {
            b.put(17 + 16 * k, 8, (uint64_t)lo[k]);
            b.put(25 + 16 * k, 8, (uint64_t)hi[k]);
        }
        put_weights(b, stream);
        b.store(out);
        return;
    }

    // 3 colours and transparent texels: dual plane, alpha (CCS 3) on plane 2.
    const Quant7& q7 = quant7();
    int v0[3], v1[3], u0 = 0, u1 = 0;
    for (int k = 0; k < 3; k++) {
        v0[k] = q7.q[e0[k]];
        v1[k] = q7.q[e1[k]];
        u0 += Quant7::unq(v0[k]);
        u1 += Quant7::unq(v1[k]);
    }
    // CEM 12 orders the endpoints by the RGB sums as CEM 8 does.
    bool swap = u1 < u0;
    const int* lo = swap ? v1 : v0;
    const int* hi = swap ? v0 : v1;
    // RGB weight for the transparent texels: the average colour, or the
    // darkest of the three palette colours.
    uint8_t clear = 1;
    if (!kTransparentRgbAverage) {
        int64_t best = -1;
        for (uint8_t q = 0; q < 3; q++) {
            int64_t d = 0;
            for (int k = 0; k < 3; k++) {
                int c = (Quant7::unq(lo[k]) * (64 - 32 * q) + Quant7::unq(hi[k]) * 32 * q + 32) >> 6;
                d += (int64_t)c * c;
            }
            if (best < 0 || d < best) {
                best = d;
                clear = q;
            }
        }
    }
    static const uint8_t q3[3] = {0, 2, 1};
    uint8_t w[32];
    for (int i = 0; i < 16; i++) {
        unsigned ix = (idx >> (2 * i)) & 3;
        if (ix == 3) {
            w[2 * i] = clear;
            w[2 * i + 1] = 2; // alpha endpoint 1 = 0
        } else {
            w[2 * i] = swap ? (uint8_t)(2 - q3[ix]) : q3[ix];
            w[2 * i + 1] = 0; // alpha endpoint 0 = 255
        }
    }
    uint64_t stream;
    unsigned bits = encode_trits(w, 32, &stream);
    put_header(b, kModeQuant3Dual, 12);
    // CEM 12 values: r0, r1, g0, g1, b0, b1, a0, a1, at range 128 (7 bits).
    for (int k = 0; k < 3; k++) {
        b.put(17 + 14 * k, 7, (uint64_t)lo[k]);
        b.put(24 + 14 * k, 7, (uint64_t)hi[k]);
    }
    b.put(17 + 42, 7, 127); // a0 = 255
    b.put(24 + 42, 7, 0); // a1 = 0
    b.put(128 - bits - 2, 2, 3); // colour component selector: alpha
    put_weights(b, stream);
    b.store(out);
}

// ---- BC1 encoding ------------------------------------------------------------

// For each 8-bit value, the 5- or 6-bit endpoint pair whose 1/3 (BC1 index 2
// in 4-colour mode) or 1/2 (index 2 in 3-colour mode) point decodes nearest
// to it, preferring equal endpoints, then close ones.
struct SingleColour {
    uint8_t third5[256][2], third6[256][2], half5[256][2], half6[256][2];

    static void build(uint8_t table[256][2], int bits, bool half) {
        int n = 1 << bits;
        for (int v = 0; v < 256; v++) {
            int bestErr = 1 << 30, bestSpread = 1 << 30;
            for (int a = 0; a < n; a++) {
                int A = bits == 5 ? x5to8((unsigned)a) : x6to8((unsigned)a);
                for (int b = 0; b < n; b++) {
                    int B = bits == 5 ? x5to8((unsigned)b) : x6to8((unsigned)b);
                    int c = half ? (A + B + 1) / 2 : (2 * A + B + 1) / 3;
                    int err = abs(c - v), spread = abs(A - B);
                    if (err < bestErr || (err == bestErr && spread < bestSpread)) {
                        bestErr = err;
                        bestSpread = spread;
                        table[v][0] = (uint8_t)a;
                        table[v][1] = (uint8_t)b;
                    }
                }
            }
        }
    }

    SingleColour() {
        build(third5, 5, false);
        build(third6, 6, false);
        build(half5, 5, true);
        build(half6, 6, true);
    }
};

const SingleColour& single_colour() {
    static const SingleColour table;
    return table;
}

inline unsigned pack565(int r5, int g6, int b5) {
    return ((unsigned)r5 << 11) | ((unsigned)g6 << 5) | (unsigned)b5;
}

inline int clamp_int(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

inline unsigned quantise565(const float c[3]) {
    int r = clamp_int((int)lroundf(c[0] * (31.0f / 255.0f)), 0, 31);
    int g = clamp_int((int)lroundf(c[1] * (63.0f / 255.0f)), 0, 63);
    int b = clamp_int((int)lroundf(c[2] * (31.0f / 255.0f)), 0, 31);
    return pack565(r, g, b);
}

struct Fit {
    unsigned c0, c1; // as they will be stored (mode fixed by the caller)
    uint32_t idx;
    int64_t err;
};

// The colours of a palette for endpoints a, b: 4-colour (thirds) or 3-colour
// (half), with gx_bc1_decode's rounding.
void palette_rgb(unsigned a, unsigned b, bool four, int pal[4][3]) {
    int A[3], B[3];
    expand565(a, A);
    expand565(b, B);
    for (int k = 0; k < 3; k++) {
        pal[0][k] = A[k];
        pal[1][k] = B[k];
        if (four) {
            pal[2][k] = (2 * A[k] + B[k] + 1) / 3;
            pal[3][k] = (A[k] + 2 * B[k] + 1) / 3;
        } else {
            pal[2][k] = (A[k] + B[k] + 1) / 2;
            pal[3][k] = 0;
        }
    }
}

// Nearest palette colour for each opaque texel; transparent texels get index 3.
Fit assign(unsigned a, unsigned b, bool four, const int px[16][4], uint16_t opaque) {
    int pal[4][3];
    palette_rgb(a, b, four, pal);
    int n = four ? 4 : 3;
    Fit f = {a, b, 0, 0};
    for (int i = 0; i < 16; i++) {
        if (!(opaque & (1u << i))) {
            f.idx |= 3u << (2 * i);
            continue;
        }
        int best = 0;
        int64_t bestErr = INT64_MAX;
        for (int j = 0; j < n; j++) {
            int dr = px[i][0] - pal[j][0], dg = px[i][1] - pal[j][1], db = px[i][2] - pal[j][2];
            int64_t e = (int64_t)dr * dr + (int64_t)dg * dg + (int64_t)db * db;
            if (e < bestErr) {
                bestErr = e;
                best = j;
            }
        }
        f.idx |= (uint32_t)best << (2 * i);
        f.err += bestErr;
    }
    return f;
}

// Least-squares endpoints for the current indices (stb_dxt's / squish's
// refinement); false if the system is singular (all texels on one index).
bool least_squares(const Fit& f, bool four, const int px[16][4], uint16_t opaque, float e0[3], float e1[3]) {
    static const float t4[4] = {0.0f, 1.0f, 1.0f / 3.0f, 2.0f / 3.0f};
    static const float t3[4] = {0.0f, 1.0f, 0.5f, 0.0f};
    const float* t = four ? t4 : t3;
    float aa = 0, ab = 0, bb = 0, x[3] = {0, 0, 0}, y[3] = {0, 0, 0};
    for (int i = 0; i < 16; i++) {
        if (!(opaque & (1u << i))) {
            continue;
        }
        float w = t[(f.idx >> (2 * i)) & 3], v = 1.0f - w;
        aa += v * v;
        ab += v * w;
        bb += w * w;
        for (int k = 0; k < 3; k++) {
            x[k] += v * (float)px[i][k];
            y[k] += w * (float)px[i][k];
        }
    }
    float det = aa * bb - ab * ab;
    if (fabsf(det) < 1e-6f) {
        return false;
    }
    float inv = 1.0f / det;
    for (int k = 0; k < 3; k++) {
        e0[k] = (bb * x[k] - ab * y[k]) * inv;
        e1[k] = (aa * y[k] - ab * x[k]) * inv;
    }
    return true;
}

// Endpoints for the opaque texels: the extremes along the principal axis,
// then least-squares refinement.
Fit fit_block(const int px[16][4], uint16_t opaque, bool four) {
    const SingleColour& sc = single_colour();

    // Mean and whether all opaque texels share one colour.
    int count = 0, firstI = -1;
    bool solid = true;
    float mean[3] = {0, 0, 0};
    for (int i = 0; i < 16; i++) {
        if (!(opaque & (1u << i))) {
            continue;
        }
        if (firstI < 0) {
            firstI = i;
        } else if (px[i][0] != px[firstI][0] || px[i][1] != px[firstI][1] || px[i][2] != px[firstI][2]) {
            solid = false;
        }
        count++;
        for (int k = 0; k < 3; k++) {
            mean[k] += (float)px[i][k];
        }
    }
    if (solid) {
        // One colour: the endpoint pair whose index-2 colour matches best.
        const int* p = px[firstI];
        const uint8_t(*r)[2] = four ? sc.third5 : sc.half5;
        const uint8_t(*g)[2] = four ? sc.third6 : sc.half6;
        unsigned a = pack565(r[p[0]][0], g[p[1]][0], r[p[2]][0]);
        unsigned b = pack565(r[p[0]][1], g[p[1]][1], r[p[2]][1]);
        Fit f = assign(a, b, four, px, opaque);
        return f;
    }
    for (int k = 0; k < 3; k++) {
        mean[k] /= (float)count;
    }

    // Covariance and its principal axis by power iteration.
    float cov[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 16; i++) {
        if (!(opaque & (1u << i))) {
            continue;
        }
        float d0 = (float)px[i][0] - mean[0], d1 = (float)px[i][1] - mean[1], d2 = (float)px[i][2] - mean[2];
        cov[0] += d0 * d0;
        cov[1] += d0 * d1;
        cov[2] += d0 * d2;
        cov[3] += d1 * d1;
        cov[4] += d1 * d2;
        cov[5] += d2 * d2;
    }
    float axis[3] = {cov[0] + cov[1] + cov[2], cov[1] + cov[3] + cov[4], cov[2] + cov[4] + cov[5]};
    if (fabsf(axis[0]) + fabsf(axis[1]) + fabsf(axis[2]) < 1e-3f) {
        axis[0] = axis[1] = axis[2] = 1.0f;
    }
    for (int it = 0; it < 4; it++) {
        float n0 = cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2];
        float n1 = cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2];
        float n2 = cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2];
        float m = fmaxf(fabsf(n0), fmaxf(fabsf(n1), fabsf(n2)));
        if (m < 1e-6f) {
            break;
        }
        axis[0] = n0 / m;
        axis[1] = n1 / m;
        axis[2] = n2 / m;
    }

    // The texels at the two ends of the axis.
    float lo = 1e30f, hi = -1e30f;
    int loI = firstI, hiI = firstI;
    for (int i = 0; i < 16; i++) {
        if (!(opaque & (1u << i))) {
            continue;
        }
        float d = (float)px[i][0] * axis[0] + (float)px[i][1] * axis[1] + (float)px[i][2] * axis[2];
        if (d < lo) {
            lo = d;
            loI = i;
        }
        if (d > hi) {
            hi = d;
            hiI = i;
        }
    }
    float e0[3], e1[3];
    for (int k = 0; k < 3; k++) {
        e0[k] = (float)px[hiI][k];
        e1[k] = (float)px[loI][k];
    }
    Fit best = assign(quantise565(e0), quantise565(e1), four, px, opaque);

    for (int it = 0; it < 2 && best.err > 0; it++) {
        if (!least_squares(best, four, px, opaque, e0, e1)) {
            break;
        }
        Fit f = assign(quantise565(e0), quantise565(e1), four, px, opaque);
        if (f.err >= best.err) {
            break;
        }
        best = f;
    }

    // Then a little hill climbing in RGB565: one step up or down in each of
    // the six endpoint components, while that lowers the error.
    static const unsigned kFields[3][2] = {{11, 31}, {5, 63}, {0, 31}};
    for (int pass = 0; pass < kRefinePasses && best.err > 0; pass++) {
        bool better = false;
        for (int e = 0; e < 2; e++) {
            for (int k = 0; k < 3; k++) {
                for (int step = -1; step <= 1; step += 2) {
                    unsigned c = e == 0 ? best.c0 : best.c1;
                    int v = (int)((c >> kFields[k][0]) & kFields[k][1]) + step;
                    if (v < 0 || v > (int)kFields[k][1]) {
                        continue;
                    }
                    c = (c & ~(kFields[k][1] << kFields[k][0])) | ((unsigned)v << kFields[k][0]);
                    Fit f = e == 0 ? assign(c, best.c1, four, px, opaque) : assign(best.c0, c, four, px, opaque);
                    if (f.err < best.err) {
                        best = f;
                        better = true;
                    }
                }
            }
        }
        if (!better) {
            break;
        }
    }
    return best;
}

void encode_block(const int px[16][4], uint8_t* out) {
    uint16_t opaque = 0;
    for (int i = 0; i < 16; i++) {
        if (px[i][3] >= 128) {
            opaque |= (uint16_t)(1u << i);
        }
    }
    unsigned c0, c1;
    uint32_t idx;
    if (opaque == 0) {
        c0 = c1 = 0;
        idx = 0xFFFFFFFFu;
    } else if (opaque == 0xFFFF) {
        Fit f = fit_block(px, opaque, true);
        c0 = f.c0;
        c1 = f.c1;
        idx = f.idx;
        if (c0 < c1) {
            // 4-colour mode needs color0 > color1: swap, indices 0<->1, 2<->3.
            unsigned t = c0;
            c0 = c1;
            c1 = t;
            idx ^= 0x55555555u;
        } else if (c0 == c1) {
            // Equal endpoints select 3-colour mode, where index 0 has the
            // same colour (and 3 would be transparent).
            idx = 0;
        }
    } else {
        Fit f = fit_block(px, opaque, false);
        c0 = f.c0;
        c1 = f.c1;
        idx = f.idx;
        if (c0 > c1) {
            // 3-colour mode needs color0 <= color1: swap, indices 0<->1.
            unsigned t = c0;
            c0 = c1;
            c1 = t;
            // Fields 0 <-> 1 (bit 0 flips where bit 1 is clear); 2 and 3 stay.
            idx ^= ~(idx >> 1) & 0x55555555u;
        }
    }
    wr16(out, c0);
    wr16(out + 2, c1);
    wr32(out + 4, idx);
}

} // namespace

extern "C" {

void gx_bc1_to_astc4x4(const uint8_t* bc1, size_t blocks, uint8_t* astc) {
    trits();
    quant7();
    for (size_t i = 0; i < blocks; i++) {
        transcode_block(bc1 + i * 8, astc + i * 16);
    }
}

void gx_bc1_encode(const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t pitch, uint8_t* bc1) {
    if (w == 0 || h == 0) {
        return;
    }
    single_colour();
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    int px[16][4];
    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            for (uint32_t y = 0; y < 4; y++) {
                uint32_t sy = by * 4 + y < h ? by * 4 + y : h - 1;
                const uint8_t* row = rgba + (size_t)sy * pitch;
                for (uint32_t x = 0; x < 4; x++) {
                    uint32_t sx = bx * 4 + x < w ? bx * 4 + x : w - 1;
                    const uint8_t* p = row + (size_t)sx * 4;
                    int* d = px[y * 4 + x];
                    d[0] = p[0];
                    d[1] = p[1];
                    d[2] = p[2];
                    d[3] = p[3];
                }
            }
            encode_block(px, bc1 + ((size_t)by * bw + bx) * 8);
        }
    }
}

void gx_bc1_decode(const uint8_t* bc1, uint32_t w, uint32_t h, uint8_t* rgba) {
    uint32_t bw = w / 4, bh = h / 4;
    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t* b = bc1 + ((size_t)by * bw + bx) * 8;
            uint8_t pal[4][4];
            bc1_palette(rd16(b), rd16(b + 2), pal);
            uint32_t idx = rd32(b + 4);
            for (uint32_t y = 0; y < 4; y++) {
                for (uint32_t x = 0; x < 4; x++) {
                    unsigned i = (idx >> (2 * (4 * y + x))) & 3;
                    memcpy(rgba + (((size_t)(by * 4 + y) * w) + bx * 4 + x) * 4, pal[i], 4);
                }
            }
        }
    }
}

} // extern "C"
