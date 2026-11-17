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

// --- per-draw state ------------------------------------------------------------

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
};

static void xf_setup(XfDraw* x) {
    x->ortho = gx.xfReg[0x26] != 0;
    for (int i = 0; i < 6; i++) {
        x->p[i] = gx_xf_float(0x1020 + i);
    }
    // Viewport: 0x101A = width / 2, 0x101B = -height / 2, 0x101C = z range,
    // 0x101D/E = centre + 342, 0x101F = far z (24-bit units).
    f32 sx = gx_xf_float(0x101A), sy = gx_xf_float(0x101B);
    f32 ox = gx_xf_float(0x101D) - 342.0f, oy = gx_xf_float(0x101E) - 342.0f;
    u32 w, h;
    gx_efb_size(&w, &h);
    x->efbW = (f32)w;
    x->efbH = (f32)h;
    x->xs = 2.0f * sx / x->efbW;
    x->xo = 2.0f * ox / x->efbW - 1.0f;
    x->ys = -2.0f * sy / x->efbH;
    x->yo = 1.0f - 2.0f * oy / x->efbH;
    x->zs = gx_xf_float(0x101C) / 16777215.0f;
    x->zo = gx_xf_float(0x101F) / 16777215.0f;
    x->numChans = gx.xfReg[0x09] & 3;
    x->numTexGens = gx.xfReg[0x3F] & 15;
}

static void mtx_row_mul(u32 base, const f32 v[4], int rows, f32* out) {
    for (int r = 0; r < rows; r++) {
        u32 a = base + r * 4;
        out[r] = gx_xf_float(a) * v[0] + gx_xf_float(a + 1) * v[1] + gx_xf_float(a + 2) * v[2] +
                 gx_xf_float(a + 3) * v[3];
    }
}

static void unpack_rgba(u32 v, f32 out[4]) {
    out[0] = (v >> 24) / 255.0f;
    out[1] = (v >> 16 & 0xFF) / 255.0f;
    out[2] = (v >> 8 & 0xFF) / 255.0f;
    out[3] = (v & 0xFF) / 255.0f;
}

static f32 clamp01(f32 v) {
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

// Quantizes to the 8 bits the rasterizer carries.
static f32 q8(f32 v) {
    return floorf(clamp01(v) * 255.0f + 0.5f) / 255.0f;
}

// One colour channel (ctrl = XF 0x0E + chan for colour, 0x10 + chan for
// alpha), as GX lighting computes it. All four components are computed; the
// caller keeps rgb from the colour control and a from the alpha control.
static void light_part(u32 ctrl, int chan, const GXVertexIn& v, const f32 eye[3], const f32 n[3],
                       f32 out[4]) {
    f32 mat[4], amb[4];
    if (ctrl & 1) {
        for (int i = 0; i < 4; i++) {
            mat[i] = (v.hasClr & (1 << chan)) ? v.clr[chan][i] / 255.0f : 1.0f;
        }
    } else {
        unpack_rgba(gx.xfReg[0x0C + chan], mat);
    }
    if (!(ctrl & 2)) {
        for (int i = 0; i < 4; i++) {
            out[i] = mat[i];
        }
        return;
    }
    if (ctrl & 0x40) {
        for (int i = 0; i < 4; i++) {
            amb[i] = (v.hasClr & (1 << chan)) ? v.clr[chan][i] / 255.0f : 1.0f;
        }
    } else {
        unpack_rgba(gx.xfReg[0x0A + chan], amb);
    }
    f32 lit[4] = {amb[0], amb[1], amb[2], amb[3]};
    u32 mask = gx_bits(ctrl, 2, 4) | gx_bits(ctrl, 11, 4) << 4;
    u32 diffuseFn = gx_bits(ctrl, 7, 2);
    bool attnEnable = (ctrl >> 9) & 1;
    bool spot = (ctrl >> 10) & 1;
    for (int l = 0; l < 8; l++) {
        if (!(mask & (1 << l))) {
            continue;
        }
        u32 base = 0x600 + l * 16;
        f32 color[4];
        unpack_rgba(gx.xfMem[base + 3], color);
        f32 lpos[3] = {gx_xf_float(base + 10), gx_xf_float(base + 11), gx_xf_float(base + 12)};
        f32 ldirv[3] = {gx_xf_float(base + 13), gx_xf_float(base + 14), gx_xf_float(base + 15)};
        f32 d[3] = {lpos[0] - eye[0], lpos[1] - eye[1], lpos[2] - eye[2]};
        f32 dist2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        f32 dist = sqrtf(dist2);
        f32 ldir[3] = {0, 0, 0};
        f32 attn = 1.0f;
        if (attnEnable && !spot) {
            // Specular: the light's position holds a half-angle direction.
            f32 len = sqrtf(lpos[0] * lpos[0] + lpos[1] * lpos[1] + lpos[2] * lpos[2]);
            if (len > 0.0f) {
                for (int i = 0; i < 3; i++) {
                    ldir[i] = lpos[i] / len;
                }
            }
            f32 nl = n[0] * ldir[0] + n[1] * ldir[1] + n[2] * ldir[2];
            f32 a = nl >= 0.0f ? n[0] * ldirv[0] + n[1] * ldirv[1] + n[2] * ldirv[2] : 0.0f;
            if (a < 0.0f) {
                a = 0.0f;
            }
            f32 c = gx_xf_float(base + 4) + gx_xf_float(base + 5) * a + gx_xf_float(base + 6) * a * a;
            f32 k = gx_xf_float(base + 7) + gx_xf_float(base + 8) * a + gx_xf_float(base + 9) * a * a;
            attn = (c > 0.0f ? c : 0.0f) / (k != 0.0f ? k : 1.0f);
        } else {
            if (dist > 0.0f) {
                for (int i = 0; i < 3; i++) {
                    ldir[i] = d[i] / dist;
                }
            }
            if (attnEnable) {
                f32 a = ldir[0] * ldirv[0] + ldir[1] * ldirv[1] + ldir[2] * ldirv[2];
                if (a < 0.0f) {
                    a = 0.0f;
                }
                f32 c = gx_xf_float(base + 4) + gx_xf_float(base + 5) * a + gx_xf_float(base + 6) * a * a;
                f32 k = gx_xf_float(base + 7) + gx_xf_float(base + 8) * dist + gx_xf_float(base + 9) * dist2;
                attn = (c > 0.0f ? c : 0.0f) / (k != 0.0f ? k : 1.0f);
            }
        }
        f32 diff = 1.0f;
        if (diffuseFn != 0) {
            diff = n[0] * ldir[0] + n[1] * ldir[1] + n[2] * ldir[2];
            if (diffuseFn == 2 && diff < 0.0f) {
                diff = 0.0f;
            }
        }
        for (int i = 0; i < 4; i++) {
            lit[i] += color[i] * diff * attn;
        }
    }
    for (int i = 0; i < 4; i++) {
        out[i] = mat[i] * clamp01(lit[i]);
    }
}

// --- vertices ------------------------------------------------------------------

static void transform(const XfDraw& x, const GXVertexIn* in, u32 count, GXVtxOut* outv) {
    u32 defPos = gx_bits(gx.xfReg[0x18], 0, 6);
    u32 defTex[8];
    for (int i = 0; i < 4; i++) {
        defTex[i] = gx_bits(gx.xfReg[0x18], 6 + 6 * i, 6);
        defTex[4 + i] = gx_bits(gx.xfReg[0x19], 6 * i, 6);
    }
    bool dualTex = gx.xfReg[0x12] & 1;
    bool needNormal = false;
    for (u32 c = 0; c < x.numChans; c++) {
        if ((gx.xfReg[0x0E + c] & 2) || (gx.xfReg[0x10 + c] & 2)) {
            needNormal = true;
        }
    }
    // Emboss texgens (type 1) need the binormal and tangent in eye space.
    bool needNbt = false;
    for (u32 g = 0; g < x.numTexGens && g < 8; g++) {
        if (gx_bits(gx.xfReg[0x40 + g], 4, 3) == 1) {
            needNbt = true;
        }
    }

    for (u32 vi = 0; vi < count; vi++) {
        const GXVertexIn& v = in[vi];
        GXVtxOut& o = outv[vi];
        u32 pm = v.hasPnMtx ? v.pnMtx : defPos;
        f32 p4[4] = {v.pos[0], v.pos[1], v.pos[2], 1.0f};
        f32 eye[3];
        mtx_row_mul(pm * 4, p4, 3, eye);

        f32 cx, cy, cz, cw;
        if (x.ortho) {
            cx = x.p[0] * eye[0] + x.p[1];
            cy = x.p[2] * eye[1] + x.p[3];
            cz = x.p[4] * eye[2] + x.p[5];
            cw = 1.0f;
        } else {
            cx = x.p[0] * eye[0] + x.p[1] * eye[2];
            cy = x.p[2] * eye[1] + x.p[3] * eye[2];
            cz = x.p[4] * eye[2] + x.p[5];
            cw = -eye[2];
        }
        o.pos[0] = cx * x.xs + cw * x.xo;
        o.pos[1] = cy * x.ys + cw * x.yo;
        o.pos[2] = cz * x.zs + cw * x.zo;
        o.pos[3] = cw;

        f32 n[3] = {0, 0, 1};
        if (needNormal && v.hasNrm) {
            u32 nb = 0x400 + (pm & 31) * 3;
            for (int r = 0; r < 3; r++) {
                n[r] = gx_xf_float(nb + r * 3) * v.nrm[0] + gx_xf_float(nb + r * 3 + 1) * v.nrm[1] +
                       gx_xf_float(nb + r * 3 + 2) * v.nrm[2];
            }
            f32 len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (len > 0.0f) {
                n[0] /= len;
                n[1] /= len;
                n[2] /= len;
            }
        }
        // Binormal and tangent through the normal matrix, not normalized
        // (Dolphin's VertexShaderGen _binormal / _tangent).
        f32 bin[3] = {0, 0, 0}, tng[3] = {0, 0, 0};
        if (needNbt) {
            u32 nb = 0x400 + (pm & 31) * 3;
            for (int r = 0; r < 3; r++) {
                f32 m0 = gx_xf_float(nb + r * 3), m1 = gx_xf_float(nb + r * 3 + 1), m2 = gx_xf_float(nb + r * 3 + 2);
                bin[r] = m0 * v.binormal[0] + m1 * v.binormal[1] + m2 * v.binormal[2];
                tng[r] = m0 * v.tangent[0] + m1 * v.tangent[1] + m2 * v.tangent[2];
            }
        }

        for (int c = 0; c < 2; c++) {
            if ((u32)c < x.numChans) {
                f32 col[4], alp[4];
                light_part(gx.xfReg[0x0E + c], c, v, eye, n, col);
                light_part(gx.xfReg[0x10 + c], c, v, eye, n, alp);
                o.clr[c][0] = q8(col[0]);
                o.clr[c][1] = q8(col[1]);
                o.clr[c][2] = q8(col[2]);
                o.clr[c][3] = q8(alp[3]);
            } else {
                o.clr[c][0] = o.clr[c][1] = o.clr[c][2] = o.clr[c][3] = 0.0f;
            }
        }

        for (u32 g = 0; g < 8; g++) {
            f32* tc = o.tex[g];
            tc[0] = tc[1] = 0.0f;
            tc[2] = 1.0f;
            if (g >= x.numTexGens) {
                continue;
            }
            u32 info = gx.xfReg[0x40 + g];
            u32 type = gx_bits(info, 4, 3);
            u32 row = gx_bits(info, 7, 5);
            bool stq = (info >> 1) & 1;
            bool abc1 = (info >> 2) & 1;
            if (type == 2 || type == 3) {
                // Colour texgen: s, t from the vertex colour's red and green.
                int c = type - 2;
                tc[0] = (v.hasClr & (1 << c)) ? v.clr[c][0] / 255.0f : 0.0f;
                tc[1] = (v.hasClr & (1 << c)) ? v.clr[c][1] / 255.0f : 0.0f;
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
                u32 srcGen = gx_bits(info, 12, 3);
                u32 lbase = 0x600 + gx_bits(info, 15, 3) * 16;
                f32 l[3] = {gx_xf_float(lbase + 10) - eye[0], gx_xf_float(lbase + 11) - eye[1],
                            gx_xf_float(lbase + 12) - eye[2]};
                f32 llen = sqrtf(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
                if (llen > 0.0f) {
                    l[0] /= llen;
                    l[1] /= llen;
                    l[2] /= llen;
                }
                static const f32 kNone[3] = {0.0f, 0.0f, 1.0f};
                const f32* s = srcGen < g ? o.tex[srcGen] : kNone;
                if (srcGen >= g) {
                    WARN_ONCE("gx: emboss texgen %lu from texgen %lu (not computed yet)\n", (unsigned long)g,
                              (unsigned long)srcGen);
                }
                tc[0] = s[0] + l[0] * bin[0] + l[1] * bin[1] + l[2] * bin[2];
                tc[1] = s[1] + l[0] * tng[0] + l[1] * tng[1] + l[2] * tng[2];
                tc[2] = s[2];
                continue;
            }
            f32 src[4] = {0, 0, 1, 1};
            if (row == 0) {
                src[0] = v.pos[0];
                src[1] = v.pos[1];
                src[2] = v.pos[2];
            } else if (row == 1) {
                src[0] = v.nrm[0];
                src[1] = v.nrm[1];
                src[2] = v.nrm[2];
            } else if (row >= 5 && row < 13) {
                src[0] = v.tex[row - 5][0];
                src[1] = v.tex[row - 5][1];
            } else {
                WARN_ONCE("gx: texgen source row %lu not supported\n", (unsigned long)row);
            }
            if (!abc1) {
                src[2] = 1.0f;
            }
            u32 tm = (v.hasTexMtx & (1 << g)) ? v.texMtx[g] : defTex[g];
            f32 r3[3] = {0, 0, 1};
            mtx_row_mul(tm * 4, src, stq ? 3 : 2, r3);
            if (dualTex) {
                u32 post = gx.xfReg[0x50 + g];
                if ((post >> 8) & 1) {
                    f32 len = sqrtf(r3[0] * r3[0] + r3[1] * r3[1] + r3[2] * r3[2]);
                    if (len > 0.0f) {
                        r3[0] /= len;
                        r3[1] /= len;
                        r3[2] /= len;
                    }
                }
                f32 p4b[4] = {r3[0], r3[1], r3[2], 1.0f};
                mtx_row_mul(0x500 + gx_bits(post, 0, 6) * 4, p4b, 3, r3);
            }
            tc[0] = r3[0];
            tc[1] = r3[1];
            tc[2] = stq ? r3[2] : 1.0f;
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

static std::vector<GXVtxOut> sX;

u32 gx_xform_primitive(u32 primitive, const GXVertexIn* in, u32 count, std::vector<GXVtxOut>& out) {
    if (sX.size() < count) {
        sX.resize(count);
    }
    XfDraw x;
    xf_setup(&x);
    transform(x, in, count, sX.data());

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
    size_t first = out.size();
    out.resize(first + tris * 3);
    GXVtxOut* o = out.data() + first;
    const GXVtxOut* X = sX.data();
    switch (primitive) {
    case 0x80:
    case 0x88:
        for (u32 i = 0; i + 3 < count; i += 4) {
            *o++ = X[i];
            *o++ = X[i + 1];
            *o++ = X[i + 2];
            *o++ = X[i];
            *o++ = X[i + 2];
            *o++ = X[i + 3];
        }
        break;
    case 0x90:
        for (u32 i = 0; i + 2 < count; i += 3) {
            *o++ = X[i];
            *o++ = X[i + 1];
            *o++ = X[i + 2];
        }
        break;
    case 0x98:
        // Keep every strip triangle's winding the same as the first's.
        for (u32 i = 0; i + 2 < count; i++) {
            *o++ = X[(i & 1) ? i + 1 : i];
            *o++ = X[(i & 1) ? i : i + 1];
            *o++ = X[i + 2];
        }
        break;
    case 0xA0:
        for (u32 i = 1; i + 1 < count; i++) {
            *o++ = X[0];
            *o++ = X[i];
            *o++ = X[i + 1];
        }
        break;
    case 0xA8:
    case 0xB0: {
        // BP 0x22: line width (bits 0-7) and point size (8-15) in 1/6 pixel.
        f32 half = gx_bits(gx.bp[0x22], 0, 8) / 12.0f;
        u32 step = primitive == 0xA8 ? 2 : 1;
        for (u32 i = 0; i + 1 < count; i += step) {
            emit_quad(o, X[i], X[i + 1], half, false, x);
            o += 6;
        }
        break;
    }
    case 0xB8: {
        f32 half = gx_bits(gx.bp[0x22], 8, 8) / 12.0f;
        for (u32 i = 0; i < count; i++) {
            emit_quad(o, X[i], X[i], half, true, x);
            o += 6;
        }
        break;
    }
    }
    u32 n = (u32)(o - (out.data() + first));
    out.resize(first + n);
    return n;
}
