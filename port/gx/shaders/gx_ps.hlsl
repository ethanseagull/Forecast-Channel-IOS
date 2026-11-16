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
// for ras and tex, alpha test.
// Not implemented (yet): indirect texturing (BP 0x10-0x27; the texcoords are
// used as is), fog (BP 0xEE-0xF2), z textures (BP 0xF4, 0xF5), the alpha-bump
// ras channels 5/6 (read as zero, like 2-4 and 7), destination alpha (BP
// 0x42), RGBA6 / logic op output formats. Blending, depth and culling are
// fixed-function pipeline state, set by the backend.
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

    [loop]
    for (uint n = 0u; n < numStages; n++) {
        uint cc = ColorEnvReg(n);
        uint ac = AlphaEnvReg(n);
        // Even stages in bits 0-11, odd in 12-23: texmap 0-2, texcoord 3-5,
        // texture enable 6, ras channel 7-9.
        uint order = TevOrderReg(n >> 1) >> ((n & 1u) * 12u);

        int4 tex = int4(255, 255, 255, 255);
        if ((order & 0x40u) != 0u) {
            uint map = order & 7u;
            if ((texBound >> map & 1u) != 0u) {
                float3 stq = TexCoord(i, (order >> 3) & 7u);
                float q = stq.z != 0.0 ? stq.z : 1.0;
                tex = int4(round(SampleMap(map, stq.xy / q) * 255.0));
                tex = Swizzle((ac >> 2) & 3u, tex);
            }
        }

        // Ras channel: 0 colour 0, 1 colour 1; 5/6 alpha bump (indirect
        // texturing, not implemented) and the rest read as zero.
        uint chan = (order >> 7) & 7u;
        int4 ras = int4(0, 0, 0, 0);
        if (chan < 2u)
            ras = Swizzle(ac & 3u, chan == 0u ? ras0 : ras1);

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
