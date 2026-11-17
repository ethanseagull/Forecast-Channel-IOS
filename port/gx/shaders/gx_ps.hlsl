// GX pixel shader: a TEV (texture environment) ubershader.
//
// One precompiled shader for every TEV configuration: it decodes the raw BP
// register values in GXTevUniforms (gx_internal.h) at runtime, as Dolphin's
// ubershaders do (VideoCommon/UberShaderPixel.cpp), and reproduces the
// hardware's integer precision: 8-bit inputs, signed 11-bit registers, the
// 0..255 -> 0..256 lerp factor and the rounding of the lerp.
//
// Implemented: 1-16 stages (genMode), tev orders (texmap, texcoord, enable,
// ras channel), colour and alpha combiners (all inputs, bias, add/sub, scale,
// clamp, dest register, compare modes), konstant selects, the 4 swap tables
// for ras and tex, alpha test, indirect texturing (0-4 indirect stages, their
// texmap / texcoord order and coordinate scale, the 3 indirect matrices and
// the dynamic S / T matrices, per-stage format, bias, wrap, add-previous) and
// the alpha-bump ras channels 5 / 6.
// Not implemented (yet): fog (BP 0xEE-0xF2), z textures (BP 0xF4, 0xF5),
// destination alpha (BP 0x42), RGBA6 / logic op output formats, the indirect
// "unmodified texcoord for LOD" bit (the port's textures have no mip levels,
// so there is no LOD to pick). Blending, depth and culling are fixed-function
// pipeline state, set by the backend.
//
// Texture coordinates: the varyings are normalized (s/q, t/q). A stage
// without an indirect command samples them as is. Indirect texturing works as
// the hardware does, in texel units with 7 fraction bits (S17.7): the
// normalized coordinate times the texcoord's SU scale (BP 0x30 + 2n, the size
// GX sets for it) times 128, and back to normalized with the size of the
// texture map that is sampled (its BP image0 register).
//
// Bindings follow SDL3 GPU's D3D12 conventions for fragment shaders: sampled
// textures t0-t7 / samplers s0-s7 in space2, uniform buffer slot 0 at b0 in
// space3. Compiled by tools/build_shaders.bat.

#include "gx_io.hlsli"

// struct GXTevUniforms in gx_internal.h, row for row (16 bytes each).
cbuffer GXTevUniforms : register(b0, space3) {
    uint4 colorEnv[4]; // BP 0xC0 + 2n, stage n in [n >> 2][n & 3]
    uint4 alphaEnv[4]; // BP 0xC1 + 2n
    uint4 tevOrder[2]; // BP 0x28-0x2F
    uint4 ksel[2];     // BP 0xF6-0xFD
    int4 tevReg[4];    // PREV, C0, C1, C2 (RGBA, signed 11-bit)
    int4 konst[4];     // K0-K3 (RGBA, 0-255)
    uint genMode;      // BP 0x00
    uint alphaCompare; // BP 0xF3
    uint texBound;     // bit n: map n has a real texture
    uint pad;
    uint4 tevInd[4];    // BP 0x10-0x1F: indirect command of TEV stage n
    uint4 indMtx[3];    // BP 0x06-0x0E: indirect matrices 0-2, 3 words each (+3 unused)
    uint indScale0;     // BP 0x25: coordinate scale of indirect stages 0, 1
    uint indScale1;     // BP 0x26: of indirect stages 2, 3
    uint indRef;        // BP 0x27: texmap / texcoord of indirect stages 0-3
    uint pad2;
    uint4 tcScale[2];   // texcoord n: SU scale - 1, S bits 0-15, T 16-31 (BP 0x30 + 2n, 0x31 + 2n)
    uint4 texImage0[2]; // texture map n: BP image0 (width - 1 bits 0-9, height - 1 bits 10-19)
};

Texture2D<float4> gxTex0 : register(t0, space2);
Texture2D<float4> gxTex1 : register(t1, space2);
Texture2D<float4> gxTex2 : register(t2, space2);
Texture2D<float4> gxTex3 : register(t3, space2);
Texture2D<float4> gxTex4 : register(t4, space2);
Texture2D<float4> gxTex5 : register(t5, space2);
Texture2D<float4> gxTex6 : register(t6, space2);
Texture2D<float4> gxTex7 : register(t7, space2);
SamplerState gxSmp0 : register(s0, space2);
SamplerState gxSmp1 : register(s1, space2);
SamplerState gxSmp2 : register(s2, space2);
SamplerState gxSmp3 : register(s3, space2);
SamplerState gxSmp4 : register(s4, space2);
SamplerState gxSmp5 : register(s5, space2);
SamplerState gxSmp6 : register(s6, space2);
SamplerState gxSmp7 : register(s7, space2);

// --- register words ----------------------------------------------------------

// Word i (0-3) of a row. Selects instead of v[i]: dynamic vector indexing
// compiles to a local array in DXIL.
uint Word(uint4 v, uint i) {
    return i == 0u ? v.x : (i == 1u ? v.y : (i == 2u ? v.z : v.w));
}

uint ColorEnvReg(uint n) { return Word(colorEnv[n >> 2], n & 3u); }
uint AlphaEnvReg(uint n) { return Word(alphaEnv[n >> 2], n & 3u); }
uint TevOrderReg(uint n) { return Word(tevOrder[n >> 2], n & 3u); }
uint KselReg(uint n) { return Word(ksel[n >> 2], n & 3u); }
uint TevIndReg(uint n) { return Word(tevInd[n >> 2], n & 3u); }
uint IndMtxReg(uint n) { return Word(indMtx[n >> 2], n & 3u); }
uint TcScaleReg(uint n) { return Word(tcScale[n >> 2], n & 3u); }
uint TexImage0Reg(uint n) { return Word(texImage0[n >> 2], n & 3u); }

// --- inputs ------------------------------------------------------------------

// Samples texture map 0-7. A switch rather than a resource array: SDL binds
// each texture to its own slot, on Metal too. The selector is uniform across
// the draw, so the implicit derivatives stay valid.
float4 SampleMap(uint map, float2 uv) {
    switch (map) {
    case 0: return gxTex0.Sample(gxSmp0, uv);
    case 1: return gxTex1.Sample(gxSmp1, uv);
    case 2: return gxTex2.Sample(gxSmp2, uv);
    case 3: return gxTex3.Sample(gxSmp3, uv);
    case 4: return gxTex4.Sample(gxSmp4, uv);
    case 5: return gxTex5.Sample(gxSmp5, uv);
    case 6: return gxTex6.Sample(gxSmp6, uv);
    default: return gxTex7.Sample(gxSmp7, uv);
    }
}

// Channel i (0-3) of c. Selects instead of c[i]: dynamic vector indexing
// compiles to a local array in DXIL.
int Chan(int4 c, uint i) {
    return i == 0u ? c.r : (i == 1u ? c.g : (i == 2u ? c.b : c.a));
}

// Swap table 0-3 (ksel words 2t and 2t+1, bits 0-1 / 2-3 of each): the
// source channel of R, G (word 2t) and B, A (word 2t+1).
int4 Swizzle(uint table, int4 c) {
    uint rg = KselReg(table * 2u);
    uint ba = KselReg(table * 2u + 1u);
    return int4(Chan(c, rg & 3u), Chan(c, (rg >> 2) & 3u), Chan(c, ba & 3u), Chan(c, (ba >> 2) & 3u));
}

// Interpolated texture coordinate n (s, t, q).
float3 TexCoord(GXVaryings i, uint n) {
    switch (n) {
    case 0: return i.tex0;
    case 1: return i.tex1;
    case 2: return i.tex2;
    case 3: return i.tex3;
    case 4: return i.tex4;
    case 5: return i.tex5;
    case 6: return i.tex6;
    default: return i.tex7;
    }
}

// --- indirect texturing (Dolphin's UberShaderPixel / PixelShaderGen) -----------

// Texture coordinate n in texels, S17.7 fixed point: the normalized
// coordinate times the texcoord's SU scale, as the hardware's setup unit
// hands it to the TEV. Truncated like Dolphin's int2() conversion.
int2 FixedCoord(float3 stq, uint n) {
    uint r = TcScaleReg(n);
    float2 scale = float2((float)((r & 0xFFFFu) + 1u), (float)((r >> 16) + 1u));
    float q = stq.z != 0.0 ? stq.z : 1.0;
    return int2(stq.xy / q * scale * 128.0);
}

// An S17.7 texel coordinate of texture map 0-7 as a normalized one: divided
// by 128 times the map's size (its image0 register, as the game set it; EFB
// copies are larger on the GPU, normalized coordinates don't care).
float2 FixedToUv(uint map, int2 c) {
    uint r = TexImage0Reg(map);
    float2 size = float2((float)((r & 0x3FFu) + 1u), (float)(((r >> 10) & 0x3FFu) + 1u));
    return float2(c) / (size * 128.0);
}

// Indirect stage s (0-3): looks up its texture (texmap / texcoord from BP
// 0x27, 6 bits a stage: map 0-2, coord 3-5) at its coordinate scaled down by
// 2^n (BP 0x25 / 0x26, 8 bits a stage: S 0-3, T 4-7). The offsets are the
// texel's (A, B, G) as (s, t, u), 0-255; an unbound map reads as white.
int3 IndLookup(GXVaryings i, uint s) {
    uint ref = indRef >> (s * 6u);
    uint map = ref & 7u;
    uint coord = (ref >> 3) & 7u;
    uint sc = (s < 2u ? indScale0 : indScale1) >> ((s & 1u) * 8u);
    int2 c = FixedCoord(TexCoord(i, coord), coord) >> uint2(sc & 15u, (sc >> 4) & 15u);
    int4 t = int4(255, 255, 255, 255);
    if ((texBound >> map & 1u) != 0u)
        t = int4(round(SampleMap(map, FixedToUv(map, c)) * 255.0));
    return t.abg;
}

// Indirect matrix element: signed 11 bits (S0.10).
int Sext11(uint v) {
    return (int)(v << 21) >> 21;
}

// Indirect wrap 0 off, 1-5 repeat every 256, 128, 64, 32, 16 texels, 6 / 7
// zero.
int IndWrap(int c, uint mode) {
    if (mode == 0u)
        return c;
    if (mode < 6u)
        return c & (int)(0xFFFEu >> mode);
    return 0;
}

// Konstant select (kcsel / kasel): 0-7 the fractions 1, 7/8 .. 1/8 (as
// Dolphin rounds them), 8-11 reserved, 12-15 K0-K3, 16-31 one channel of
// K0-K3 (16 K0.r, 17 K1.r .. 20 K0.g .. 31 K3.a).
int4 KonstSel(uint sel) {
    if (sel < 8u) {
        int s = (int)sel;
        int v = sel < 4u ? 255 - 32 * s : 256 - 32 * s;
        return int4(v, v, v, v);
    }
    if (sel < 12u)
        return int4(0, 0, 0, 0);
    if (sel < 16u)
        return konst[sel - 12u];
    int v = Chan(konst[sel & 3u], (sel - 16u) >> 2);
    return int4(v, v, v, v);
}

// Colour combiner input a/b/c/d.
int3 ColorIn(uint sel, int4 r0, int4 r1, int4 r2, int4 r3, int4 tex, int4 ras, int4 k) {
    switch (sel) {
    case 0: return r0.rgb; // CPREV
    case 1: return r0.aaa; // APREV
    case 2: return r1.rgb; // C0
    case 3: return r1.aaa; // A0
    case 4: return r2.rgb; // C1
    case 5: return r2.aaa; // A1
    case 6: return r3.rgb; // C2
    case 7: return r3.aaa; // A2
    case 8: return tex.rgb; // TEXC
    case 9: return tex.aaa; // TEXA
    case 10: return ras.rgb; // RASC
    case 11: return ras.aaa; // RASA
    case 12: return int3(255, 255, 255); // ONE
    case 13: return int3(128, 128, 128); // HALF
    case 14: return k.rgb; // KONST
    default: return int3(0, 0, 0); // ZERO
    }
}

// Alpha combiner input a/b/c/d.
int AlphaIn(uint sel, int4 r0, int4 r1, int4 r2, int4 r3, int4 tex, int4 ras, int4 k) {
    switch (sel) {
    case 0: return r0.a; // APREV
    case 1: return r1.a; // A0
    case 2: return r2.a; // A1
    case 3: return r3.a; // A2
    case 4: return tex.a; // TEXA
    case 5: return ras.a; // RASA
    case 6: return k.a; // KONST
    default: return 0; // ZERO
    }
}

// --- the combiner --------------------------------------------------------------

// d (+/-) lerp(a, b, c), with bias on d and scale (0 x1, 1 x2, 2 x4, 3 /2),
// in the hardware's integer steps (Dolphin's tevLerp): c goes from 0..255 to
// 0..256, the scale moves inside the lerp for precision, the lerp is rounded
// (+128 add, +127 subtract) except for /2, which halves the final sum.
int3 TevLerp(int3 a, int3 b, int3 c, int3 d, uint bias, bool sub, uint scale) {
    c += c >> 7u;
    if (bias == 1u)
        d += 128;
    else if (bias == 2u)
        d -= 128;
    int3 lerp = (a << 8u) + (b - a) * c;
    if (scale != 3u) {
        lerp = (lerp << scale) + (sub ? 127 : 128);
        d = d << scale;
    }
    int3 result = lerp >> 8u;
    result = sub ? d - result : d + result;
    if (scale == 3u)
        result = result >> 1u;
    return result;
}

int TevLerp(int a, int b, int c, int d, uint bias, bool sub, uint scale) {
    return TevLerp(int3(a, 0, 0), int3(b, 0, 0), int3(c, 0, 0), int3(d, 0, 0), bias, sub, scale).x;
}

// Compare modes 0-5 (bias 3, op = scale << 1 | sub): R8, GR16 and BGR24 as
// one value each, greater-than or equal. Modes 6 and 7 (per channel) are
// handled by the caller.
bool TevCompare(uint op, int3 a, int3 b) {
    switch (op) {
    case 0: return a.r > b.r;
    case 1: return a.r == b.r;
    case 2: return (a.g << 8 | a.r) > (b.g << 8 | b.r);
    case 3: return a.r == b.r && a.g == b.g;
    case 4: return (a.b << 16 | a.g << 8 | a.r) > (b.b << 16 | b.g << 8 | b.r);
    case 5: return a.r == b.r && a.g == b.g && a.b == b.b;
    default: return false;
    }
}

bool AlphaTest(int a, int ref, uint comp) {
    switch (comp) {
    case 0: return false; // NEVER
    case 1: return a < ref; // LESS
    case 2: return a == ref; // EQUAL
    case 3: return a <= ref; // LEQUAL
    case 4: return a > ref; // GREATER
    case 5: return a != ref; // NEQUAL
    case 6: return a >= ref; // GEQUAL
    default: return true; // ALWAYS
    }
}

void WriteColor(uint dest, int3 v, inout int4 r0, inout int4 r1, inout int4 r2, inout int4 r3) {
    switch (dest) {
    case 0: r0.rgb = v; break;
    case 1: r1.rgb = v; break;
    case 2: r2.rgb = v; break;
    default: r3.rgb = v; break;
    }
}

void WriteAlpha(uint dest, int v, inout int4 r0, inout int4 r1, inout int4 r2, inout int4 r3) {
    switch (dest) {
    case 0: r0.a = v; break;
    case 1: r1.a = v; break;
    case 2: r2.a = v; break;
    default: r3.a = v; break;
    }
}

int4 ReadReg(uint n, int4 r0, int4 r1, int4 r2, int4 r3) {
    switch (n) {
    case 0: return r0;
    case 1: return r1;
    case 2: return r2;
    default: return r3;
    }
}

// --- main --------------------------------------------------------------------

float4 main(GXVaryings i) : SV_Target0 {
    int4 ras0 = int4(round(saturate(i.clr0) * 255.0));
    int4 ras1 = int4(round(saturate(i.clr1) * 255.0));

    // PREV, C0, C1, C2 start at the values the program set.
    int4 r0 = tevReg[0];
    int4 r1 = tevReg[1];
    int4 r2 = tevReg[2];
    int4 r3 = tevReg[3];

    uint numStages = ((genMode >> 10) & 15u) + 1u;
    uint colorDest = 0u;
    uint alphaDest = 0u;

    // The indirect stages' lookups (genMode bits 16-18: how many), once per
    // pixel; TEV stages pick one by number.
    uint numInd = (genMode >> 16) & 7u;
    int3 ind0 = int3(0, 0, 0);
    int3 ind1 = int3(0, 0, 0);
    int3 ind2 = int3(0, 0, 0);
    int3 ind3 = int3(0, 0, 0);
    if (numInd > 0u)
        ind0 = IndLookup(i, 0u);
    if (numInd > 1u)
        ind1 = IndLookup(i, 1u);
    if (numInd > 2u)
        ind2 = IndLookup(i, 2u);
    if (numInd > 3u)
        ind3 = IndLookup(i, 3u);
    // The texel coordinate of the last stage (S17.7, kept for add-previous)
    // and the alpha bump value, carried from stage to stage.
    int2 tevCoord = int2(0, 0);
    int alphaBump = 0;

    [loop]
    for (uint n = 0u; n < numStages; n++) {
        uint cc = ColorEnvReg(n);
        uint ac = AlphaEnvReg(n);
        // Even stages in bits 0-11, odd in 12-23: texmap 0-2, texcoord 3-5,
        // texture enable 6, ras channel 7-9.
        uint order = TevOrderReg(n >> 1) >> ((n & 1u) * 12u);
        uint map = order & 7u;
        uint coordId = (order >> 3) & 7u;
        float3 stq = TexCoord(i, coordId);
        int2 fixedUv = FixedCoord(stq, coordId);

        // Indirect command: ind stage 0-1, format 2-3, bias 4-6, alpha bump
        // select 7-8, matrix 9-10 (0 none, 1-3 matrix 0-2), matrix type
        // 11-12 (0 the matrix, 1 dynamic S, 2 dynamic T), wrap S 13-15, wrap
        // T 16-18, unmodified texcoord for LOD 19 (ignored), add previous 20.
        uint ti = TevIndReg(n);
        float2 uv;
        if (ti != 0u) {
            uint bt = ti & 3u;
            uint fmt = (ti >> 2) & 3u;
            uint bias = (ti >> 4) & 7u;
            uint bs = (ti >> 7) & 3u;
            uint mtx = (ti >> 9) & 3u;
            uint mtxType = (ti >> 11) & 3u;
            int3 ic = int3(0, 0, 0);
            if (bt < numInd)
                ic = bt == 0u ? ind0 : (bt == 1u ? ind1 : (bt == 2u ? ind2 : ind3));

            // Alpha bump: the selected offset's top bits (the ones the
            // format leaves out of the offset, at most 5).
            if (bs != 0u) {
                int sel = bs == 1u ? ic.x : (bs == 2u ? ic.y : ic.z);
                alphaBump = sel & (fmt == 0u ? 0xF8 : (fmt == 1u ? 0xE0 : (fmt == 2u ? 0xF0 : 0xF8)));
            }
            // Format: 8, 5, 4 or 3 low bits; bias -128 for 8 bits, else +1.
            int fmtMask = fmt == 0u ? 255 : (fmt == 1u ? 31 : (fmt == 2u ? 15 : 7));
            int biasAdd = fmt == 0u ? -128 : 1;
            ic = (ic & fmtMask) + int3((bias & 1u) != 0u ? biasAdd : 0, (bias & 2u) != 0u ? biasAdd : 0,
                                       (bias & 4u) != 0u ? biasAdd : 0);

            // Offset (S17.7): matrix (S0.10) times offsets, or the stage's
            // texcoord times the s / t offset, scaled by 2^(exp) where the
            // registers hold exp + 17 split 2 / 2 / 2 bits across the three
            // words (bits 22-23).
            int2 off = int2(0, 0);
            if (mtx != 0u) {
                uint b = (mtx - 1u) * 3u;
                uint wa = IndMtxReg(b);
                uint wb = IndMtxReg(b + 1u);
                uint wc = IndMtxReg(b + 2u);
                int shift = 17 - (int)(((wa >> 22) & 3u) | ((wb >> 22) & 3u) << 2 | ((wc >> 22) & 3u) << 4);
                if (mtxType == 0u) {
                    // Row 0 (s) in bits 0-10, row 1 (t) in 11-21.
                    off.x = Sext11(wa) * ic.x + Sext11(wb) * ic.y + Sext11(wc) * ic.z;
                    off.y = Sext11(wa >> 11) * ic.x + Sext11(wb >> 11) * ic.y + Sext11(wc >> 11) * ic.z;
                    off = off >> 3;
                } else if (mtxType == 1u) {
                    off = (fixedUv * ic.xx) >> 8;
                } else if (mtxType == 2u) {
                    off = (fixedUv * ic.yy) >> 8;
                }
                shift = clamp(shift, -31, 31);
                off = shift >= 0 ? off >> (uint)shift : off << (uint)(-shift);
            }

            int2 wrapped = int2(IndWrap(fixedUv.x, (ti >> 13) & 7u), IndWrap(fixedUv.y, (ti >> 16) & 7u));
            if (n == 0u || ((ti >> 20) & 1u) == 0u)
                tevCoord = wrapped + off;
            else
                tevCoord += wrapped + off;
            // The hardware's coordinates are 24 bits.
            tevCoord = (tevCoord << 8) >> 8;
            uv = FixedToUv(map, tevCoord);
        } else {
            tevCoord = fixedUv;
            float q = stq.z != 0.0 ? stq.z : 1.0;
            uv = stq.xy / q;
        }

        int4 tex = int4(255, 255, 255, 255);
        if ((order & 0x40u) != 0u && (texBound >> map & 1u) != 0u) {
            tex = int4(round(SampleMap(map, uv) * 255.0));
            tex = Swizzle((ac >> 2) & 3u, tex);
        }

        // Ras channel: 0 colour 0, 1 colour 1, 5 alpha bump, 6 alpha bump
        // normalized (the 5 bits stretched to 8); the rest read as zero.
        uint chan = (order >> 7) & 7u;
        int4 ras = int4(0, 0, 0, 0);
        if (chan < 2u) {
            ras = Swizzle(ac & 3u, chan == 0u ? ras0 : ras1);
        } else if (chan == 5u) {
            ras = int4(alphaBump, alphaBump, alphaBump, alphaBump);
        } else if (chan == 6u) {
            int nb = alphaBump | (alphaBump >> 5);
            ras = int4(nb, nb, nb, nb);
        }

        // ksel word n/2: kcsel 4-8, kasel 9-13 for even stages, 14-18 and
        // 19-23 for odd.
        uint ks = KselReg(n >> 1) >> ((n & 1u) * 10u);
        int4 k = int4(KonstSel((ks >> 4) & 31u).rgb, KonstSel((ks >> 9) & 31u).a);

        // Colour combiner: d 0-3, c 4-7, b 8-11, a 12-15, bias 16-17, sub 18,
        // clamp 19, scale 20-21, dest 22-23. a, b and c are 8 bits; d keeps
        // the register's sign and range.
        int3 cA = ColorIn((cc >> 12) & 15u, r0, r1, r2, r3, tex, ras, k) & 255;
        int3 cB = ColorIn((cc >> 8) & 15u, r0, r1, r2, r3, tex, ras, k) & 255;
        int3 cC = ColorIn((cc >> 4) & 15u, r0, r1, r2, r3, tex, ras, k) & 255;
        int3 cD = ColorIn(cc & 15u, r0, r1, r2, r3, tex, ras, k);
        // Alpha combiner: ras swap 0-1, tex swap 2-3, d 4-6, c 7-9, b 10-12,
        // a 13-15, then as for colour.
        int aA = AlphaIn((ac >> 13) & 7u, r0, r1, r2, r3, tex, ras, k) & 255;
        int aB = AlphaIn((ac >> 10) & 7u, r0, r1, r2, r3, tex, ras, k) & 255;
        int aC = AlphaIn((ac >> 7) & 7u, r0, r1, r2, r3, tex, ras, k) & 255;
        int aD = AlphaIn((ac >> 4) & 7u, r0, r1, r2, r3, tex, ras, k);

        uint cBias = (cc >> 16) & 3u;
        bool cSub = ((cc >> 18) & 1u) != 0u;
        uint cScale = (cc >> 20) & 3u;
        int3 color;
        if (cBias != 3u) {
            color = TevLerp(cA, cB, cC, cD, cBias, cSub, cScale);
        } else {
            // Compare mode: d + (a OP b ? c : 0).
            uint op = cScale << 1 | (cSub ? 1u : 0u);
            if (op == 6u)
                color = cC * int3(cA > cB);
            else if (op == 7u)
                color = cC * int3(cA == cB);
            else
                color = TevCompare(op, cA, cB) ? cC : int3(0, 0, 0);
            color += cD;
        }
        color = ((cc >> 19) & 1u) != 0u ? clamp(color, 0, 255) : clamp(color, -1024, 1023);

        uint aBias = (ac >> 16) & 3u;
        bool aSub = ((ac >> 18) & 1u) != 0u;
        uint aScale = (ac >> 20) & 3u;
        int alpha;
        if (aBias != 3u) {
            alpha = TevLerp(aA, aB, aC, aD, aBias, aSub, aScale);
        } else {
            // Modes 0-5 compare the colour combiner's a and b; 6 and 7 the
            // alpha inputs.
            uint op = aScale << 1 | (aSub ? 1u : 0u);
            bool pass;
            if (op == 6u)
                pass = aA > aB;
            else if (op == 7u)
                pass = aA == aB;
            else
                pass = TevCompare(op, cA, cB);
            alpha = aD + (pass ? aC : 0);
        }
        alpha = ((ac >> 19) & 1u) != 0u ? clamp(alpha, 0, 255) : clamp(alpha, -1024, 1023);

        colorDest = (cc >> 22) & 3u;
        alphaDest = (ac >> 22) & 3u;
        WriteColor(colorDest, color, r0, r1, r2, r3);
        WriteAlpha(alphaDest, alpha, r0, r1, r2, r3);
    }

    // The output is the last stage's result (GX expects it in PREV), wrapped
    // to 8 bits as the hardware does.
    int4 result = int4(ReadReg(colorDest, r0, r1, r2, r3).rgb, ReadReg(alphaDest, r0, r1, r2, r3).a) & 255;

    // Alpha test: ref0 0-7, ref1 8-15, comp0 16-18, comp1 19-21, logic 22-23
    // (AND, OR, XOR, XNOR).
    bool pass0 = AlphaTest(result.a, (int)(alphaCompare & 255u), (alphaCompare >> 16) & 7u);
    bool pass1 = AlphaTest(result.a, (int)((alphaCompare >> 8) & 255u), (alphaCompare >> 19) & 7u);
    bool pass;
    switch ((alphaCompare >> 22) & 3u) {
    case 0: pass = pass0 && pass1; break;
    case 1: pass = pass0 || pass1; break;
    case 2: pass = pass0 != pass1; break;
    default: pass = pass0 == pass1; break;
    }
    if (!pass)
        discard;

    return float4(result) / 255.0;
}
