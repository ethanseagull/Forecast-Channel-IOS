// Reference ASTC LDR block decoder; see astc_ref.h. Written from the
// specification's text and tables, deliberately without the transcoder's
// helpers.
#include "astc_ref.h"

#include <string.h>

namespace astc_ref {
namespace {

int get_bit(const uint8_t* blk, int pos) {
    return (blk[pos >> 3] >> (pos & 7)) & 1;
}

int get_bits(const uint8_t* blk, int pos, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) {
        v |= get_bit(blk, pos + i) << i;
    }
    return v;
}

// The ISE ranges: levels, trits, quints, bits ("Integer Sequence Encoding").
struct Range {
    int levels, trits, quints, bits;
};
const Range kRanges[] = {
    {2, 0, 0, 1},    {3, 1, 0, 0},    {4, 0, 0, 2},    {5, 0, 1, 0},    {6, 1, 0, 1},    {8, 0, 0, 3},
    {10, 0, 1, 1},   {12, 1, 0, 2},   {16, 0, 0, 4},   {20, 0, 1, 2},   {24, 1, 0, 3},   {32, 0, 0, 5},
    {40, 0, 1, 3},   {48, 1, 0, 4},   {64, 0, 0, 6},   {80, 0, 1, 4},   {96, 1, 0, 5},   {128, 0, 0, 7},
    {160, 0, 1, 5},  {192, 1, 0, 6},  {256, 0, 0, 8},
};

const Range* find_range(int levels) {
    for (const Range& r : kRanges) {
        if (r.levels == levels) {
            return &r;
        }
    }
    return nullptr;
}

int ise_bit_count(const Range& r, int n) {
    int bits = n * r.bits;
    if (r.trits) {
        bits += (8 * n + 4) / 5;
    }
    if (r.quints) {
        bits += (7 * n + 2) / 3;
    }
    return bits;
}

// A bit source: either the block upward from a position, or the weight area
// (block bit 127 - i is stream bit i). Reads past `limit` give 0.
struct Stream {
    const uint8_t* blk;
    int start;
    bool reversed;
    int limit;
    int pos = 0;

    int read(int n) {
        int v = 0;
        for (int i = 0; i < n; i++, pos++) {
            int b = 0;
            if (pos < limit) {
                b = reversed ? get_bit(blk, 127 - pos) : get_bit(blk, start + pos);
            }
            v |= b << i;
        }
        return v;
    }
};

// Five trits from the 8-bit T, as in the specification's decoding procedure.
void unpack_trits(int T, int t[5]) {
    int T0 = T & 1, T1 = (T >> 1) & 1, T2 = (T >> 2) & 1, T3 = (T >> 3) & 1, T4 = (T >> 4) & 1;
    int T5 = (T >> 5) & 1, T6 = (T >> 6) & 1, T7 = (T >> 7) & 1;
    int C;
    if (T2 && T3 && T4) {
        C = (T7 << 4) | (T6 << 3) | (T5 << 2) | (T1 << 1) | T0;
        t[4] = 2;
        t[3] = 2;
    } else {
        C = (T4 << 4) | (T3 << 3) | (T2 << 2) | (T1 << 1) | T0;
        if (T5 && T6) {
            t[4] = 2;
            t[3] = T7;
        } else {
            t[4] = T7;
            t[3] = (T6 << 1) | T5;
        }
    }
    int C0 = C & 1, C1 = (C >> 1) & 1, C2 = (C >> 2) & 1, C3 = (C >> 3) & 1, C4 = (C >> 4) & 1;
    if (C0 && C1) {
        t[2] = 2;
        t[1] = C4;
        t[0] = (C3 << 1) | (C2 & (C3 ^ 1));
    } else if (C2 && C3) {
        t[2] = 2;
        t[1] = 2;
        t[0] = (C1 << 1) | C0;
    } else {
        t[2] = C4;
        t[1] = (C3 << 1) | C2;
        t[0] = (C1 << 1) | (C0 & (C1 ^ 1));
    }
}

// Decodes n integers of range r. False for ranges this decoder doesn't do.
bool ise_decode(const Range& r, int n, Stream& s, int* out) {
    if (r.quints) {
        return false;
    }
    if (!r.trits) {
        for (int i = 0; i < n; i++) {
            out[i] = s.read(r.bits);
        }
        return true;
    }
    // Per group of five: m0 T[1:0] m1 T[3:2] m2 T[4] m3 T[6:5] m4 T[7].
    static const int tbits[5] = {2, 2, 1, 2, 1};
    for (int g = 0; g < n; g += 5) {
        int m[5], T = 0, shift = 0;
        for (int j = 0; j < 5; j++) {
            m[j] = s.read(r.bits);
            T |= s.read(tbits[j]) << shift;
            shift += tbits[j];
        }
        int t[5];
        unpack_trits(T, t);
        for (int j = 0; j < 5 && g + j < n; j++) {
            out[g + j] = (t[j] << r.bits) | m[j];
        }
    }
    return true;
}

// Replicates the n-bit value v to `to` bits (the most significant first).
int replicate(int v, int n, int to) {
    int r = 0, have = 0;
    while (have < to) {
        r = (r << n) | v;
        have += n;
    }
    return r >> (have - to);
}

// Weight unquantisation to 0..64.
bool unquant_weight(const Range& r, int v, int* out) {
    int w;
    if (!r.trits && !r.quints) {
        w = replicate(v, r.bits, 6);
    } else if (r.trits && r.bits == 0) {
        static const int t[3] = {0, 32, 63};
        w = t[v];
    } else {
        return false;
    }
    *out = w > 32 ? w + 1 : w;
    return true;
}

struct BlockMode {
    int gw, gh, dual, levels;
};

// The 2D block mode table. False for reserved encodings.
bool decode_mode(int mode, BlockMode* bm) {
    auto b = [mode](int i) { return (mode >> i) & 1; };
    int A = (mode >> 5) & 3, H = b(9), D = b(10), R;
    if ((mode & 3) != 0) {
        R = (b(1) << 2) | (b(0) << 1) | b(4);
        int B = (mode >> 7) & 3;
        switch ((mode >> 2) & 3) {
        case 0: bm->gw = B + 4; bm->gh = A + 2; break;
        case 1: bm->gw = B + 8; bm->gh = A + 2; break;
        case 2: bm->gw = A + 2; bm->gh = B + 8; break;
        default:
            if (b(8)) {
                bm->gw = b(7) + 2;
                bm->gh = A + 2;
            } else {
                bm->gw = A + 2;
                bm->gh = b(7) + 6;
            }
            break;
        }
    } else {
        if (((mode >> 2) & 3) == 0) {
            return false;
        }
        R = (b(3) << 2) | (b(2) << 1) | b(4);
        switch ((mode >> 7) & 3) {
        case 0: bm->gw = 12; bm->gh = A + 2; break;
        case 1: bm->gw = A + 2; bm->gh = 12; break;
        case 2:
            bm->gw = A + 6;
            bm->gh = ((mode >> 9) & 3) + 6;
            D = 0;
            H = 0;
            break;
        default:
            if (A == 0) {
                bm->gw = 6;
                bm->gh = 10;
            } else if (A == 1) {
                bm->gw = 10;
                bm->gh = 6;
            } else {
                return false;
            }
            break;
        }
    }
    static const int levels[2][8] = {{0, 0, 2, 3, 4, 5, 6, 8}, {0, 0, 10, 12, 16, 20, 24, 32}};
    if (R < 2) {
        return false;
    }
    bm->levels = levels[H][R];
    bm->dual = D;
    return true;
}

void error_colour(int n, uint8_t* out8, uint8_t* outRound) {
    for (int i = 0; i < n; i++) {
        uint8_t c[4] = {255, 0, 255, 255};
        memcpy(out8 + i * 4, c, 4);
        if (outRound) {
            memcpy(outRound + i * 4, c, 4);
        }
    }
}

uint8_t round16(int c) {
    return (uint8_t)((c * 255 + 32767) / 65535);
}

} // namespace

Info decode_block(const uint8_t blk[16], int bw, int bh, uint8_t* out8, uint8_t* outRound) {
    Info info;
    int n = bw * bh;
    auto fail = [&](const char* why) {
        info.error = why;
        error_colour(n, out8, outRound);
        return info;
    };

    int mode = get_bits(blk, 0, 11);
    if ((mode & 0x1FF) == 0x1FC) {
        info.voidExtent = true;
        if (get_bit(blk, 9)) {
            return fail("HDR void extent");
        }
        if (get_bits(blk, 10, 2) != 3) {
            return fail("void extent reserved bits");
        }
        int c[4];
        for (int k = 0; k < 4; k++) {
            c[k] = get_bits(blk, 64 + 16 * k, 16);
        }
        for (int i = 0; i < n; i++) {
            for (int k = 0; k < 4; k++) {
                out8[i * 4 + k] = (uint8_t)(c[k] >> 8);
                if (outRound) {
                    outRound[i * 4 + k] = round16(c[k]);
                }
            }
        }
        return info;
    }

    BlockMode bm;
    if (!decode_mode(mode, &bm)) {
        return fail("reserved block mode");
    }
    info.gridW = bm.gw;
    info.gridH = bm.gh;
    info.dualPlane = bm.dual != 0;
    info.weightRange = bm.levels;
    if (bm.gw > bw || bm.gh > bh) {
        return fail("weight grid larger than the block");
    }
    int planes = bm.dual ? 2 : 1;
    int nWeights = bm.gw * bm.gh * planes;
    const Range* wr = find_range(bm.levels);
    int wBits = ise_bit_count(*wr, nWeights);
    if (nWeights > 64 || wBits < 24 || wBits > 96) {
        return fail("weight count or bits out of range");
    }
    if (get_bits(blk, 11, 2) != 0) {
        return fail("more than one partition (unsupported)");
    }
    int cem = get_bits(blk, 13, 4);
    info.cem = cem;

    // Colour endpoints: what the weights (and CCS) leave, at the largest range
    // that fits.
    int nValues = 2 * ((cem >> 2) + 1);
    int avail = 128 - 17 - wBits - (bm.dual ? 2 : 0);
    const Range* er = nullptr;
    for (const Range& r : kRanges) {
        if (ise_bit_count(r, nValues) <= avail) {
            er = &r;
        }
    }
    if (er == nullptr || er->levels < 6) {
        return fail("not enough endpoint bits");
    }
    info.endpointRange = er->levels;
    if (er->trits || er->quints) {
        return fail("endpoint range with trits/quints (unsupported)");
    }
    int ccs = bm.dual ? get_bits(blk, 128 - wBits - 2, 2) : -1;

    int ev[18];
    Stream es{blk, 17, false, ise_bit_count(*er, nValues)};
    ise_decode(*er, nValues, es, ev);
    for (int i = 0; i < nValues; i++) {
        ev[i] = replicate(ev[i], er->bits, 8);
    }

    int e0[4], e1[4];
    switch (cem) {
    case 0: // LDR luminance, direct
        e0[0] = e0[1] = e0[2] = ev[0];
        e1[0] = e1[1] = e1[2] = ev[1];
        e0[3] = e1[3] = 255;
        break;
    case 4: // LDR luminance + alpha, direct
        e0[0] = e0[1] = e0[2] = ev[0];
        e1[0] = e1[1] = e1[2] = ev[1];
        e0[3] = ev[2];
        e1[3] = ev[3];
        break;
    case 8: // LDR RGB, direct
    case 12: { // LDR RGBA, direct
        int a0 = cem == 12 ? ev[6] : 255, a1 = cem == 12 ? ev[7] : 255;
        if (ev[1] + ev[3] + ev[5] >= ev[0] + ev[2] + ev[4]) {
            e0[0] = ev[0]; e0[1] = ev[2]; e0[2] = ev[4]; e0[3] = a0;
            e1[0] = ev[1]; e1[1] = ev[3]; e1[2] = ev[5]; e1[3] = a1;
        } else {
            // Swapped, with blue contraction: (r + b) >> 1, (g + b) >> 1, b.
            e0[0] = (ev[1] + ev[5]) >> 1; e0[1] = (ev[3] + ev[5]) >> 1; e0[2] = ev[5]; e0[3] = a1;
            e1[0] = (ev[0] + ev[4]) >> 1; e1[1] = (ev[2] + ev[4]) >> 1; e1[2] = ev[4]; e1[3] = a0;
        }
        break;
    }
    default:
        return fail("colour endpoint mode (unsupported)");
    }

    // Weights: the ISE stream from the top of the block, bits reversed;
    // with two planes they alternate plane 0, plane 1 per grid point.
    int wq[64];
    Stream ws{blk, 0, true, wBits};
    if (!ise_decode(*wr, nWeights, ws, wq)) {
        return fail("weight range (unsupported)");
    }
    int grid[2][64];
    for (int i = 0; i < nWeights; i++) {
        int u;
        if (!unquant_weight(*wr, wq[i], &u)) {
            return fail("weight range (unsupported)");
        }
        grid[i % planes][i / planes] = u;
    }

    // Infill to texels (identity when the grid is the block size).
    int Ds = (1024 + bw / 2) / (bw - 1), Dt = (1024 + bh / 2) / (bh - 1);
    for (int t = 0; t < bh; t++) {
        for (int s = 0; s < bw; s++) {
            int gs = (Ds * s * (bm.gw - 1) + 32) >> 6, gt = (Dt * t * (bm.gh - 1) + 32) >> 6;
            int js = gs >> 4, fs = gs & 15, jt = gt >> 4, ft = gt & 15;
            int w11 = (fs * ft + 8) >> 4, w10 = ft - w11, w01 = fs - w11, w00 = 16 - fs - ft + w11;
            int w[2];
            for (int p = 0; p < planes; p++) {
                auto at = [&](int x, int y) {
                    x = x < bm.gw ? x : bm.gw - 1;
                    y = y < bm.gh ? y : bm.gh - 1;
                    return grid[p][y * bm.gw + x];
                };
                w[p] = (at(js, jt) * w00 + at(js + 1, jt) * w01 + at(js, jt + 1) * w10 + at(js + 1, jt + 1) * w11 +
                        8) >> 4;
            }
            int texel = t * bw + s;
            for (int k = 0; k < 4; k++) {
                int wk = (bm.dual && k == ccs) ? w[1] : w[0];
                int c0 = (e0[k] << 8) | e0[k], c1 = (e1[k] << 8) | e1[k];
                int c = (c0 * (64 - wk) + c1 * wk + 32) >> 6;
                out8[texel * 4 + k] = (uint8_t)(c >> 8);
                if (outRound) {
                    outRound[texel * 4 + k] = round16(c);
                }
            }
        }
    }
    return info;
}

} // namespace astc_ref
