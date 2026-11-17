// Specialized TEV pixel shaders: key normalization and source generation
// (see gx_tevgen.h). Everything emitted here mirrors shaders/gx_ps.hlsl:
// change both together.
#include "gx_tevgen.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- the key ---------------------------------------------------------------------

namespace {

// Combiner input fields (gx_ps.hlsl main): colour a 12-15, b 8-11, c 4-7,
// d 0-3; alpha a 13-15, b 10-12, c 7-9, d 4-6.
uint32_t color_sel(uint32_t cc, int i) {
    static const int shift[4] = {12, 8, 4, 0};
    return (cc >> shift[i]) & 15u;
}
uint32_t alpha_sel(uint32_t ac, int i) {
    static const int shift[4] = {13, 10, 7, 4};
    return (ac >> shift[i]) & 7u;
}

struct StageUse {
    bool texRead, rasRead, kc, ka;
};

StageUse stage_use(uint32_t cc, uint32_t ac) {
    StageUse u = {};
    for (int i = 0; i < 4; i++) {
        uint32_t c = color_sel(cc, i), a = alpha_sel(ac, i);
        u.texRead |= c == 8 || c == 9 || a == 4;
        u.rasRead |= c == 10 || c == 11 || a == 5;
        u.kc |= c == 14;
        u.ka |= a == 6;
    }
    return u;
}

// Alpha test result for one comparison: 0 always false, 1 always true, -1
// depends on the value.
int comp_const(uint32_t comp) {
    return comp == 0 ? 0 : comp == 7 ? 1 : -1;
}

int logic_const(uint32_t logic, int p0, int p1) {
    if (p0 < 0 || p1 < 0) {
        return -1;
    }
    switch (logic) {
    case 0:
        return p0 && p1;
    case 1:
        return p0 || p1;
    case 2:
        return p0 != p1;
    default:
        return p0 == p1;
    }
}

} // namespace

void tevgen_make_key(const TevGenRegs& r, TevKey* k) {
    memset(k, 0, sizeof(*k));
    uint32_t n = ((r.genMode >> 10) & 15u) + 1u;
    uint32_t numInd = (r.genMode >> 16) & 7u;
    if (numInd > 4) {
        numInd = 4; // the ubershader looks up at most 4; more behave the same
    }
    k->numStages = n;
    k->numInd = numInd;

    uint32_t cc[16], ac[16], order[16], ks[16], ti[16];
    StageUse use[16];
    bool texUsed[16];
    for (uint32_t s = 0; s < n; s++) {
        cc[s] = r.colorEnv[s] & 0xFFFFFFu;
        ac[s] = r.alphaEnv[s] & 0xFFFFFFu;
        order[s] = (r.tevOrder[s >> 1] >> ((s & 1) * 12)) & 0xFFFu;
        ks[s] = ((r.ksel[s >> 1] >> ((s & 1) * 10)) >> 4) & 0x3FFu; // kcsel 0-4, kasel 5-9
        ti[s] = r.tevInd[s] & 0x1FFFFFu;
        use[s] = stage_use(cc[s], ac[s]);
        texUsed[s] = ((order[s] >> 6) & 1u) != 0 && use[s].texRead;
    }
    // A stage's texel coordinate is read by the next stage if that one has an
    // indirect command with add-previous and needs its own coordinate.
    bool outNeeded[16];
    outNeeded[n - 1] = false;
    for (int s = (int)n - 2; s >= 0; s--) {
        uint32_t nx = (uint32_t)s + 1;
        outNeeded[s] = ti[nx] != 0 && ((ti[nx] >> 20) & 1u) != 0 && (texUsed[nx] || outNeeded[nx]);
    }

    uint32_t texBound = 0, indUsed = 0, swapUsed = 0;
    for (uint32_t s = 0; s < n; s++) {
        uint32_t o = order[s];
        uint32_t map = o & 7u, coord = (o >> 3) & 7u, chan = (o >> 7) & 7u;
        uint32_t rswap = ac[s] & 3u, tswap = (ac[s] >> 2) & 3u;
        bool coordNeeded = texUsed[s] || outNeeded[s];
        bool bound = ((r.texBound >> map) & 1u) != 0;
        if (!texUsed[s]) {
            map = 0;
            tswap = 0;
            o &= ~0x40u;
        } else if (!bound) {
            tswap = 0;
        } else {
            texBound |= 1u << map;
            swapUsed |= 1u << tswap;
        }
        if (!coordNeeded) {
            coord = 0;
        }
        if (!use[s].rasRead) {
            chan = 0;
            rswap = 0;
        } else if (chan == 0 || chan == 1) {
            swapUsed |= 1u << rswap;
        } else {
            rswap = 0;
            if (chan != 5 && chan != 6) {
                chan = 7; // reads as zero
            }
        }
        uint32_t t = ti[s];
        if (t != 0) {
            uint32_t bs = (t >> 7) & 3u;
            if (!coordNeeded) {
                // Only the alpha bump (stage, format, select) is left.
                t = bs != 0 ? t & 0x18Fu : 0;
            } else {
                t &= ~(1u << 19); // unmodified texcoord for LOD: ignored
                if (((t >> 9) & 3u) == 0) {
                    t &= ~(3u << 11); // no matrix: its type is unused
                }
            }
            if (t != 0 && (t & 3u) < numInd) {
                indUsed |= 1u << (t & 3u);
            }
        }
        uint32_t kc = use[s].kc ? ks[s] & 31u : 0;
        uint32_t ka = use[s].ka ? (ks[s] >> 5) & 31u : 0;
        if (kc >= 8 && kc < 12) {
            kc = 8;
        }
        if (ka >= 8 && ka < 12) {
            ka = 8;
        }
        o = (o & 0x40u) | map | coord << 3 | chan << 7;
        k->stage[s].color = cc[s];
        k->stage[s].alpha = (ac[s] & ~15u) | rswap | tswap << 2;
        k->stage[s].order = o | kc << 12 | ka << 17;
        k->stage[s].ind = t;
    }
    for (uint32_t s = 0; s < numInd; s++) {
        if ((indUsed >> s) & 1u) {
            uint32_t ref = (r.indRef >> (s * 6)) & 63u;
            k->indRef |= ref << (s * 6);
            if ((r.texBound >> (ref & 7u)) & 1u) {
                texBound |= 1u << (ref & 7u);
            }
        }
    }
    k->texBound = texBound;
    for (uint32_t t = 0; t < 4; t++) {
        if ((swapUsed >> t) & 1u) {
            uint32_t rg = r.ksel[t * 2] & 15u, ba = r.ksel[t * 2 + 1] & 15u;
            k->swap |= (rg | ba << 4) << (t * 8);
        }
    }
    uint32_t comp0 = (r.alphaCompare >> 16) & 7u, comp1 = (r.alphaCompare >> 19) & 7u;
    uint32_t logic = (r.alphaCompare >> 22) & 3u;
    int c = logic_const(logic, comp_const(comp0), comp_const(comp1));
    if (c == 1) {
        k->alphaTest = 7u | 7u << 3; // always passes
    } else if (c == 0) {
        k->alphaTest = 0; // never passes
    } else {
        k->alphaTest = comp0 | comp1 << 3 | logic << 6;
    }
}

uint64_t tevgen_hash(const TevKey& k) {
    const uint32_t* w = (const uint32_t*)&k;
    size_t words = 6 + 4 * (size_t)k.numStages;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < words; i++) {
        for (int b = 0; b < 4; b++) {
            h ^= (w[i] >> (b * 8)) & 0xFFu;
            h *= 0x100000001b3ull;
        }
    }
    return h;
}

// --- source generation ---------------------------------------------------------------

namespace {

struct Out {
    std::string s;
    void raw(const char* text) { s += text; }
    void add(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        int len = vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        if (len > 0) {
            s.append(buf, (size_t)len < sizeof(buf) ? (size_t)len : sizeof(buf) - 1);
        }
    }
};

const char kComp[] = "xyzw";
const char kChan[] = "rgba";

// The uniform block, field for field as gx_ps.hlsl's cbuffer (and
// GXTevUniforms in gx_internal.h): the same syntax in HLSL and MSL.
const char kUniforms[] = "struct GXTevUniforms {\n"
                         "    uint4 colorEnv[4];\n"
                         "    uint4 alphaEnv[4];\n"
                         "    uint4 tevOrder[2];\n"
                         "    uint4 ksel[2];\n"
                         "    int4 tevReg[4];\n"
                         "    int4 konst[4];\n"
                         "    uint genMode;\n"
                         "    uint alphaCompare;\n"
                         "    uint texBound;\n"
                         "    uint pad;\n"
                         "    uint4 tevInd[4];\n"
                         "    uint4 indMtx[3];\n"
                         "    uint indScale0;\n"
                         "    uint indScale1;\n"
                         "    uint indRef;\n"
                         "    uint pad2;\n"
                         "    uint4 tcScale[2];\n"
                         "    uint4 texImage0[2];\n"
                         "};\n";

// gx_ps.hlsl FixedCoord and FixedToUv, whose float math is precise (no
// reordering or fusing: the truncation to texels must come out the same in
// both shaders). HLSL says so with precise; the ubershader's MSL gets
// glslang's NoContraction, which SPIRV-Cross writes as these spvFMul helpers
// (an fma with 0 that the optimizer may not touch); the divisions stay plain.
const char kFixedHlsl[] =
    "int2 FixedCoord(float3 stq, uint r) {\n"
    "    precise float2 scale = float2(float((r & 0xFFFFu) + 1u), float((r >> 16) + 1u));\n"
    "    precise float q = stq.z != 0.0 ? stq.z : 1.0;\n"
    "    precise float2 v = stq.xy / q * scale * 128.0;\n"
    "    return int2(v);\n"
    "}\n"
    "float2 FixedToUv(uint r, int2 c) {\n"
    "    precise float2 size = float2(float((r & 0x3FFu) + 1u), float(((r >> 10) & 0x3FFu) + 1u));\n"
    "    precise float2 uv = float2(c) / (size * 128.0);\n"
    "    return uv;\n"
    "}\n";
const char kFixedMsl[] =
    "template<typename T>\n"
    "[[clang::optnone]] T spvFMul(T l, T r) {\n"
    "    return fma(l, r, T(0));\n"
    "}\n"
    "int2 FixedCoord(float3 stq, uint r) {\n"
    "    float2 scale = float2(float((r & 0xFFFFu) + 1u), float((r >> 16) + 1u));\n"
    "    float q = stq.z != 0.0 ? stq.z : 1.0;\n"
    "    float2 v = spvFMul(spvFMul(stq.xy / float2(q), scale), float2(128.0));\n"
    "    return int2(v);\n"
    "}\n"
    "float2 FixedToUv(uint r, int2 c) {\n"
    "    float2 size = float2(float((r & 0x3FFu) + 1u), float(((r >> 10) & 0x3FFu) + 1u));\n"
    "    return float2(c) / spvFMul(size, float2(128.0));\n"
    "}\n";

// The rest of gx_ps.hlsl's helpers, with the decoded fields as arguments.
// ASINT and RNE are the dialect's bit cast and round-half-to-even (HLSL
// round; the ubershader's MSL gets rint from SPIRV-Cross).
const char kHelpers[] =
    "int Sext11(uint v) {\n"
    "    return ASINT(v << 21) >> 21;\n"
    "}\n"
    "int3 TevLerp(int3 a, int3 b, int3 c, int3 d, int bias, bool sub, int scale) {\n"
    "    c += c >> 7;\n"
    "    if (bias == 1)\n"
    "        d += 128;\n"
    "    else if (bias == 2)\n"
    "        d -= 128;\n"
    "    int3 lerp = (a << 8) + (b - a) * c;\n"
    "    if (scale != 3) {\n"
    "        lerp = (lerp << scale) + (sub ? 127 : 128);\n"
    "        d = d << scale;\n"
    "    }\n"
    "    int3 result = lerp >> 8;\n"
    "    result = sub ? d - result : d + result;\n"
    "    if (scale == 3)\n"
    "        result = result >> 1;\n"
    "    return result;\n"
    "}\n";

// Register n (0-3 of a uint4 row array) of a uniform array.
std::string ureg(const char* array, uint32_t n) {
    char buf[64];
    snprintf(buf, sizeof(buf), "U.%s[%u].%c", array, n >> 2, kComp[n & 3]);
    return buf;
}

// Swap table (rg in bits 0-3, ba in 4-7) as a swizzle of v.
std::string swizzle(const char* v, uint32_t table) {
    uint32_t rg = table & 15u, ba = (table >> 4) & 15u;
    char sw[5] = {kChan[rg & 3], kChan[(rg >> 2) & 3], kChan[ba & 3], kChan[(ba >> 2) & 3], 0};
    if (strcmp(sw, "rgba") == 0) {
        return v;
    }
    return std::string(v) + "." + sw;
}

// Konstant select (gx_ps.hlsl KonstSel) as an int3 (rgb) or int (a).
std::string konst_sel(uint32_t sel, bool alpha) {
    char buf[64];
    if (sel < 8) {
        int v = sel < 4 ? 255 - 32 * (int)sel : 256 - 32 * (int)sel;
        if (alpha) {
            snprintf(buf, sizeof(buf), "%d", v);
        } else {
            snprintf(buf, sizeof(buf), "int3(%d, %d, %d)", v, v, v);
        }
    } else if (sel < 12) {
        snprintf(buf, sizeof(buf), alpha ? "0" : "int3(0, 0, 0)");
    } else if (sel < 16) {
        snprintf(buf, sizeof(buf), "U.konst[%u].%s", sel - 12, alpha ? "a" : "rgb");
    } else {
        char c = kChan[(sel - 16) >> 2];
        if (alpha) {
            snprintf(buf, sizeof(buf), "U.konst[%u].%c", sel & 3u, c);
        } else {
            snprintf(buf, sizeof(buf), "U.konst[%u].%c%c%c", sel & 3u, c, c, c);
        }
    }
    return buf;
}

const char* color_in(uint32_t sel) {
    static const char* const in[16] = {"r0.rgb", "r0.aaa", "r1.rgb", "r1.aaa", "r2.rgb", "r2.aaa",
                                       "r3.rgb", "r3.aaa", "tex.rgb", "tex.aaa", "ras.rgb", "ras.aaa",
                                       "int3(255, 255, 255)", "int3(128, 128, 128)", "kc", "int3(0, 0, 0)"};
    return in[sel & 15];
}

const char* alpha_in(uint32_t sel) {
    static const char* const in[8] = {"r0.a", "r1.a", "r2.a", "r3.a", "tex.a", "ras.a", "ka", "0"};
    return in[sel & 7];
}

// gx_ps.hlsl TevCompare for modes 0-5.
const char* tev_compare(uint32_t op) {
    switch (op) {
    case 0:
        return "cA.r > cB.r";
    case 1:
        return "cA.r == cB.r";
    case 2:
        return "(cA.g << 8 | cA.r) > (cB.g << 8 | cB.r)";
    case 3:
        return "cA.r == cB.r && cA.g == cB.g";
    case 4:
        return "(cA.b << 16 | cA.g << 8 | cA.r) > (cB.b << 16 | cB.g << 8 | cB.r)";
    case 5:
        return "cA.r == cB.r && cA.g == cB.g && cA.b == cB.b";
    default:
        return "false";
    }
}

// gx_ps.hlsl AlphaTest.
std::string alpha_test(uint32_t comp, const char* ref) {
    static const char* const op[8] = {"", "<", "==", "<=", ">", "!=", ">=", ""};
    if (comp == 0) {
        return "false";
    }
    if (comp == 7) {
        return "true";
    }
    return std::string("result.a ") + op[comp] + " " + ref;
}

} // namespace

std::string tevgen_source(const TevKey& k, TevGenDialect dialect) {
    bool msl = dialect == TEVGEN_MSL;
    Out o;
    o.add("// Specialized TEV pixel shader (port/gx/gx_tevgen.cpp): %s\n", tevgen_describe(k).c_str());
    if (msl) {
        o.add("#include <metal_stdlib>\nusing namespace metal;\n");
        o.add("#define ASINT(x) as_type<int>(x)\n#define RNE(x) rint(x)\n");
    } else {
        o.add("#define ASINT(x) asint(x)\n#define RNE(x) round(x)\n");
    }
    o.raw(kUniforms);
    if (msl) {
        // gx_vs.hlsl's outputs as SPIRV-Cross names them (glslang numbers the
        // HLSL varyings from 0 in declaration order; SV_Position is not one).
        o.add("struct main0_in {\n"
              "    float4 clr0 [[user(locn0)]];\n"
              "    float4 clr1 [[user(locn1)]];\n");
        for (int t = 0; t < 8; t++) {
            o.add("    float3 tex%d [[user(locn%d)]];\n", t, t + 2);
        }
        o.add("};\nstruct main0_out {\n    float4 color [[color(0)]];\n};\n");
    } else {
        // shaders/gx_io.hlsli
        o.add("struct GXVaryings {\n"
              "    float4 pos : SV_Position;\n"
              "    float4 clr0 : TEXCOORD0;\n"
              "    float4 clr1 : TEXCOORD1;\n");
        for (int t = 0; t < 8; t++) {
            o.add("    float3 tex%d : TEXCOORD%d;\n", t, t + 2);
        }
        o.add("};\nConstantBuffer<GXTevUniforms> U : register(b0, space3);\n");
        for (int t = 0; t < 8; t++) {
            o.add("Texture2D<float4> gxTex%d : register(t%d, space2);\n", t, t);
        }
        for (int t = 0; t < 8; t++) {
            o.add("SamplerState gxSmp%d : register(s%d, space2);\n", t, t);
        }
    }
    o.raw(msl ? kFixedMsl : kFixedHlsl);
    o.raw(kHelpers);

    if (msl) {
        o.add("fragment main0_out main0(main0_in i [[stage_in]], constant GXTevUniforms& U [[buffer(0)]]");
        for (int t = 0; t < 8; t++) {
            o.add(", texture2d<float> gxTex%d [[texture(%d)]]", t, t);
        }
        for (int t = 0; t < 8; t++) {
            o.add(", sampler gxSmp%d [[sampler(%d)]]", t, t);
        }
        o.add(") {\n");
    } else {
        o.add("float4 main(GXVaryings i) : SV_Target0 {\n");
    }
    const char* sampleFn = msl ? "sample" : "Sample";

    uint32_t n = k.numStages;
    bool needRas[2] = {false, false};
    for (uint32_t s = 0; s < n; s++) {
        uint32_t chan = (k.stage[s].order >> 7) & 7u;
        if (stage_use(k.stage[s].color, k.stage[s].alpha).rasRead && chan < 2) {
            needRas[chan] = true;
        }
    }
    for (int c = 0; c < 2; c++) {
        if (needRas[c]) {
            o.add("    int4 ras%d = int4(RNE(saturate(i.clr%d) * 255.0));\n", c, c);
        }
    }
    o.add("    int4 r0 = U.tevReg[0];\n    int4 r1 = U.tevReg[1];\n    int4 r2 = U.tevReg[2];\n"
          "    int4 r3 = U.tevReg[3];\n");
    o.add("    int2 tevCoord = int2(0, 0);\n    int alphaBump = 0;\n");

    // Indirect lookups (gx_ps.hlsl IndLookup), for the stages some TEV stage uses.
    bool indUsed[4] = {false, false, false, false};
    for (uint32_t s = 0; s < n; s++) {
        uint32_t t = k.stage[s].ind;
        if (t != 0 && (t & 3u) < k.numInd) {
            indUsed[t & 3u] = true;
        }
    }
    for (uint32_t s = 0; s < 4; s++) {
        if (!indUsed[s]) {
            continue;
        }
        uint32_t ref = (k.indRef >> (s * 6)) & 63u;
        uint32_t map = ref & 7u, coord = (ref >> 3) & 7u;
        if (((k.texBound >> map) & 1u) == 0) {
            o.add("    int3 ind%u = int3(255, 255, 255);\n", s);
            continue;
        }
        o.add("    int3 ind%u;\n    {\n", s);
        o.add("        uint sc = U.indScale%u >> %uu;\n", s < 2 ? 0u : 1u, (s & 1u) * 8u);
        o.add("        int2 c = FixedCoord(i.tex%u, %s);\n", coord, ureg("tcScale", coord).c_str());
        o.add("        c = int2(c.x >> int(sc & 15u), c.y >> int((sc >> 4) & 15u));\n");
        o.add("        ind%u = int4(RNE(gxTex%u.%s(gxSmp%u, FixedToUv(%s, c)) * 255.0)).abg;\n", s, map, sampleFn,
              map, ureg("texImage0", map).c_str());
        o.add("    }\n");
    }

    uint32_t colorDest = 0, alphaDest = 0;
    for (uint32_t s = 0; s < n; s++) {
        const TevKey::Stage& st = k.stage[s];
        uint32_t cc = st.color, ac = st.alpha, ord = st.order, ti = st.ind;
        StageUse use = stage_use(cc, ac);
        uint32_t map = ord & 7u, coord = (ord >> 3) & 7u, chan = (ord >> 7) & 7u;
        bool texUsed = ((ord >> 6) & 1u) != 0 && use.texRead;
        bool bound = ((k.texBound >> map) & 1u) != 0;
        // Whether a later stage reads this stage's texel coordinate (as in
        // tevgen_make_key; the key has already dropped what nothing reads).
        bool outNeeded = false;
        for (uint32_t nx = s + 1; nx < n; nx++) {
            uint32_t t = k.stage[nx].ind;
            StageUse nu = stage_use(k.stage[nx].color, k.stage[nx].alpha);
            bool nxTex = ((k.stage[nx].order >> 6) & 1u) != 0 && nu.texRead;
            if (t == 0 || ((t >> 20) & 1u) == 0) {
                break; // replaces the coordinate
            }
            if (nxTex) {
                outNeeded = true;
                break;
            }
            // Adds to it and passes it on: keep looking.
        }
        bool coordNeeded = texUsed || outNeeded;

        o.add("    { // stage %u: color %06x alpha %06x order %07x ind %06x\n", s, cc, ac, ord, ti);
        bool needFixed = coordNeeded && (ti != 0 || outNeeded);
        if (coordNeeded) {
            o.add("        float3 stq = i.tex%u;\n", coord);
        }
        if (needFixed) {
            o.add("        int2 fixedUv = FixedCoord(stq, %s);\n", ureg("tcScale", coord).c_str());
        }
        if (texUsed) {
            o.add("        float2 uv;\n");
        }
        if (ti != 0) {
            uint32_t bt = ti & 3u, fmt = (ti >> 2) & 3u, bias = (ti >> 4) & 7u, bs = (ti >> 7) & 3u;
            uint32_t mtx = (ti >> 9) & 3u, mtxType = (ti >> 11) & 3u;
            if (bt < k.numInd) {
                o.add("        int3 ic = ind%u;\n", bt);
            } else {
                o.add("        int3 ic = int3(0, 0, 0);\n");
            }
            if (bs != 0) {
                static const int bumpMask[4] = {0xF8, 0xE0, 0xF0, 0xF8};
                o.add("        alphaBump = ic.%c & %d;\n", kComp[bs - 1], bumpMask[fmt]);
            }
            if (coordNeeded) {
                static const int fmtMask[4] = {255, 31, 15, 7};
                int biasAdd = fmt == 0 ? -128 : 1;
                o.add("        ic = (ic & %d) + int3(%d, %d, %d);\n", fmtMask[fmt], (bias & 1u) ? biasAdd : 0,
                      (bias & 2u) ? biasAdd : 0, (bias & 4u) ? biasAdd : 0);
                o.add("        int2 off = int2(0, 0);\n");
                if (mtx != 0) {
                    uint32_t b = (mtx - 1) * 3;
                    o.add("        uint wa = %s;\n        uint wb = %s;\n        uint wc = %s;\n",
                          ureg("indMtx", b).c_str(), ureg("indMtx", b + 1).c_str(), ureg("indMtx", b + 2).c_str());
                    o.add("        int shift = 17 - int(((wa >> 22) & 3u) | ((wb >> 22) & 3u) << 2 | "
                          "((wc >> 22) & 3u) << 4);\n");
                    if (mtxType == 0) {
                        o.add("        off.x = Sext11(wa) * ic.x + Sext11(wb) * ic.y + Sext11(wc) * ic.z;\n"
                              "        off.y = Sext11(wa >> 11) * ic.x + Sext11(wb >> 11) * ic.y + "
                              "Sext11(wc >> 11) * ic.z;\n"
                              "        off = off >> 3;\n");
                    } else if (mtxType == 1) {
                        o.add("        off = (fixedUv * ic.xx) >> 8;\n");
                    } else if (mtxType == 2) {
                        o.add("        off = (fixedUv * ic.yy) >> 8;\n");
                    }
                    o.add("        shift = clamp(shift, -31, 31);\n"
                          "        off = shift >= 0 ? (off >> shift) : (off << (-shift));\n");
                }
                uint32_t wrap[2] = {(ti >> 13) & 7u, (ti >> 16) & 7u};
                std::string w[2];
                for (int c = 0; c < 2; c++) {
                    char buf[64];
                    if (wrap[c] == 0) {
                        snprintf(buf, sizeof(buf), "fixedUv.%c", kComp[c]);
                    } else if (wrap[c] < 6) {
                        snprintf(buf, sizeof(buf), "(fixedUv.%c & %d)", kComp[c], (int)(0xFFFEu >> wrap[c]));
                    } else {
                        snprintf(buf, sizeof(buf), "0");
                    }
                    w[c] = buf;
                }
                o.add("        int2 wrapped = int2(%s, %s);\n", w[0].c_str(), w[1].c_str());
                if (s == 0 || ((ti >> 20) & 1u) == 0) {
                    o.add("        tevCoord = wrapped + off;\n");
                } else {
                    o.add("        tevCoord += wrapped + off;\n");
                }
                o.add("        tevCoord = (tevCoord << 8) >> 8;\n");
                if (texUsed) {
                    o.add("        uv = FixedToUv(%s, tevCoord);\n", ureg("texImage0", map).c_str());
                }
            }
        } else {
            if (outNeeded) {
                o.add("        tevCoord = fixedUv;\n");
            }
            if (texUsed) {
                o.add("        float q = stq.z != 0.0 ? stq.z : 1.0;\n        uv = stq.xy / q;\n");
            }
        }

        if (use.texRead) {
            if (texUsed && bound) {
                char sample[128];
                snprintf(sample, sizeof(sample), "int4(RNE(gxTex%u.%s(gxSmp%u, uv) * 255.0))", map, sampleFn, map);
                uint32_t table = (k.swap >> (((ac >> 2) & 3u) * 8)) & 0xFFu;
                o.add("        int4 texel = %s;\n", sample);
                o.add("        int4 tex = %s;\n", swizzle("texel", table).c_str());
            } else {
                o.add("        int4 tex = int4(255, 255, 255, 255);\n");
            }
        }
        if (use.rasRead) {
            if (chan < 2) {
                uint32_t table = (k.swap >> ((ac & 3u) * 8)) & 0xFFu;
                o.add("        int4 ras = %s;\n", swizzle(chan == 0 ? "ras0" : "ras1", table).c_str());
            } else if (chan == 5) {
                o.add("        int4 ras = int4(alphaBump, alphaBump, alphaBump, alphaBump);\n");
            } else if (chan == 6) {
                o.add("        int nb = alphaBump | (alphaBump >> 5);\n        int4 ras = int4(nb, nb, nb, nb);\n");
            } else {
                o.add("        int4 ras = int4(0, 0, 0, 0);\n");
            }
        }
        if (use.kc) {
            o.add("        int3 kc = %s;\n", konst_sel((ord >> 12) & 31u, false).c_str());
        }
        if (use.ka) {
            o.add("        int ka = %s;\n", konst_sel((ord >> 17) & 31u, true).c_str());
        }

        o.add("        int3 cA = (%s) & 255;\n", color_in(color_sel(cc, 0)));
        o.add("        int3 cB = (%s) & 255;\n", color_in(color_sel(cc, 1)));
        o.add("        int3 cC = (%s) & 255;\n", color_in(color_sel(cc, 2)));
        o.add("        int3 cD = %s;\n", color_in(color_sel(cc, 3)));
        o.add("        int aA = (%s) & 255;\n", alpha_in(alpha_sel(ac, 0)));
        o.add("        int aB = (%s) & 255;\n", alpha_in(alpha_sel(ac, 1)));
        o.add("        int aC = (%s) & 255;\n", alpha_in(alpha_sel(ac, 2)));
        o.add("        int aD = %s;\n", alpha_in(alpha_sel(ac, 3)));

        uint32_t cBias = (cc >> 16) & 3u, cSub = (cc >> 18) & 1u, cScale = (cc >> 20) & 3u;
        if (cBias != 3) {
            o.add("        int3 color = TevLerp(cA, cB, cC, cD, %u, %s, %u);\n", cBias, cSub ? "true" : "false",
                  cScale);
        } else {
            uint32_t op = cScale << 1 | cSub;
            if (op == 6 || op == 7) {
                const char* cmp = op == 6 ? "cA > cB" : "cA == cB";
                if (msl) {
                    o.add("        int3 color = select(int3(0, 0, 0), cC, %s);\n", cmp);
                } else {
                    o.add("        int3 color = cC * int3(%s);\n", cmp);
                }
            } else {
                o.add("        int3 color = (%s) ? cC : int3(0, 0, 0);\n", tev_compare(op));
            }
            o.add("        color += cD;\n");
        }
        if ((cc >> 19) & 1u) {
            o.add("        color = clamp(color, int3(0, 0, 0), int3(255, 255, 255));\n");
        } else {
            o.add("        color = clamp(color, int3(-1024, -1024, -1024), int3(1023, 1023, 1023));\n");
        }

        uint32_t aBias = (ac >> 16) & 3u, aSub = (ac >> 18) & 1u, aScale = (ac >> 20) & 3u;
        if (aBias != 3) {
            o.add("        int alpha = TevLerp(int3(aA, 0, 0), int3(aB, 0, 0), int3(aC, 0, 0), int3(aD, 0, 0), %u, "
                  "%s, %u).x;\n",
                  aBias, aSub ? "true" : "false", aScale);
        } else {
            uint32_t op = aScale << 1 | aSub;
            const char* pass = op == 6 ? "aA > aB" : op == 7 ? "aA == aB" : tev_compare(op);
            o.add("        int alpha = aD + ((%s) ? aC : 0);\n", pass);
        }
        if ((ac >> 19) & 1u) {
            o.add("        alpha = clamp(alpha, 0, 255);\n");
        } else {
            o.add("        alpha = clamp(alpha, -1024, 1023);\n");
        }
        colorDest = (cc >> 22) & 3u;
        alphaDest = (ac >> 22) & 3u;
        o.add("        r%u.rgb = color;\n        r%u.a = alpha;\n    }\n", colorDest, alphaDest);
    }

    o.add("    int4 result = int4(r%u.rgb, r%u.a) & 255;\n", colorDest, alphaDest);
    uint32_t comp0 = k.alphaTest & 7u, comp1 = (k.alphaTest >> 3) & 7u, logic = (k.alphaTest >> 6) & 3u;
    int always = logic_const(logic, comp_const(comp0), comp_const(comp1));
    const char* discardStmt = msl ? "discard_fragment();" : "discard;";
    if (always == 0) {
        o.add("    %s\n", discardStmt);
    } else if (always < 0) {
        o.add("    bool pass0 = %s;\n", alpha_test(comp0, "int(U.alphaCompare & 255u)").c_str());
        o.add("    bool pass1 = %s;\n", alpha_test(comp1, "int((U.alphaCompare >> 8) & 255u)").c_str());
        static const char* const lg[4] = {"pass0 && pass1", "pass0 || pass1", "pass0 != pass1", "pass0 == pass1"};
        o.add("    if (!(%s))\n        %s\n", lg[logic], discardStmt);
    }
    if (msl) {
        o.add("    main0_out out;\n    out.color = float4(result) / 255.0;\n    return out;\n}\n");
    } else {
        o.add("    return float4(result) / 255.0;\n}\n");
    }
    return o.s;
}

std::string tevgen_describe(const TevKey& k) {
    char buf[160];
    static const char* const logic[4] = {"and", "or", "xor", "xnor"};
    snprintf(buf, sizeof(buf), "%u stages, %u ind, maps %02x, alpha test %u/%u %s, hash %016llx", k.numStages,
             k.numInd, k.texBound, k.alphaTest & 7u, (k.alphaTest >> 3) & 7u, logic[(k.alphaTest >> 6) & 3u],
             (unsigned long long)tevgen_hash(k));
    return buf;
}

std::string tevgen_key_to_text(const TevKey& k) {
    const uint32_t* w = (const uint32_t*)&k;
    size_t words = 6 + 4 * (size_t)k.numStages;
    std::string s;
    for (size_t i = 0; i < words; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), i == 0 ? "%x" : " %x", w[i]);
        s += buf;
    }
    return s;
}

bool tevgen_key_from_text(const char* text, TevKey* k) {
    memset(k, 0, sizeof(*k));
    uint32_t* w = (uint32_t*)k;
    size_t words = sizeof(*k) / 4, i = 0;
    const char* p = text;
    for (;;) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == 0 || *p == '\n' || *p == '\r' || *p == '#') {
            break;
        }
        char* end;
        unsigned long v = strtoul(p, &end, 16);
        if (end == p || i >= words) {
            return false;
        }
        w[i++] = (uint32_t)v;
        p = end;
    }
    return i >= 6 && k->numStages >= 1 && k->numStages <= 16 && i == 6 + 4 * (size_t)k->numStages &&
           k->numInd <= 4;
}
