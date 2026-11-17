// Specialized TEV pixel shaders: the key and the source generator.
//
// The ubershader (shaders/gx_ps.hlsl) decodes the TEV's BP registers per
// pixel. For each TEV configuration a draw uses, gx_spec.cpp instead compiles
// a shader generated here with that configuration baked in: the same integer
// math as the ubershader, with every structural register field a constant, so
// the loops, switches and unused inputs are gone. The two must give
// bit-identical results; keep them in sync (the helpers below mirror the
// ubershader's functions one for one).
//
// Key (baked into the code): number of TEV and indirect stages; per stage the
// colour and alpha combiners (inputs, bias, add/sub, clamp, scale or compare
// mode, dest, ras / tex swap selects), the tev order (texmap, texcoord,
// enable, ras channel), the konst selects, the indirect command; the swap
// tables, the alpha test's compare functions and logic, the indirect stages'
// texmap / texcoord, which sampled maps have a real texture. Fields nothing
// reads are zeroed, so that configurations which only differ there share one
// shader.
// Uniforms (per draw, the same GXTevUniforms block the ubershader reads):
// TEV registers, konst colours, alpha test references, indirect matrices and
// coordinate scales, texcoord scales, texture sizes.
//
// Standalone (no SDL, no port headers): tools/tevgen_samples builds it on its
// own to compile-check the generated Metal source on the CI.
#ifndef GX_TEVGEN_H
#define GX_TEVGEN_H

#include <stdint.h>

#include <string>

// The registers the key is made from, as GXTevUniforms holds them.
struct TevGenRegs {
    uint32_t colorEnv[16]; // BP 0xC0 + 2n
    uint32_t alphaEnv[16]; // BP 0xC1 + 2n
    uint32_t tevOrder[8];  // BP 0x28-0x2F
    uint32_t ksel[8];      // BP 0xF6-0xFD
    uint32_t tevInd[16];   // BP 0x10-0x1F
    uint32_t genMode;      // BP 0x00
    uint32_t alphaCompare; // BP 0xF3
    uint32_t texBound;     // bit n: map n has a real texture
    uint32_t indRef;       // BP 0x27
};

// Normalized: compare and hash it as raw bytes (unused stages are zero).
struct TevKey {
    uint32_t numStages; // 1-16
    uint32_t numInd;    // 0-4 indirect stages
    uint32_t alphaTest; // BP 0xF3 bits 16-23 (comp0, comp1, logic); 0x07 0x07 AND if it always passes
    uint32_t texBound;  // bit n: map n is sampled and has a real texture
    uint32_t indRef;    // BP 0x27, indirect stages some TEV stage uses
    uint32_t swap;      // swap table t (rg bits 0-3, ba 4-7 of ksel 2t, 2t+1) in bits 8t..8t+7, used ones
    struct Stage {
        uint32_t color; // BP 0xC0 + 2n, 24 bits
        uint32_t alpha; // BP 0xC1 + 2n, 24 bits
        uint32_t order; // tev order (12 bits) | kcsel << 12 | kasel << 17
        uint32_t ind;   // BP 0x10 + n, 21 bits
    } stage[16];
};

void tevgen_make_key(const TevGenRegs& r, TevKey* k);
uint64_t tevgen_hash(const TevKey& k);

enum TevGenDialect { TEVGEN_HLSL, TEVGEN_MSL };

// The pixel shader for k. HLSL: entry point "main", SDL GPU's D3D12 binding
// spaces, for ps_6_0. MSL: entry point "main0", with the interface
// SPIRV-Cross gives the ubershader (stage-in [[user(locnN)]] from
// gx_vs.hlsl's outputs, gxTexN / gxSmpN at texture(N) / sampler(N), the
// uniform block at buffer(0)).
std::string tevgen_source(const TevKey& k, TevGenDialect dialect);

// One line describing k for the log.
std::string tevgen_describe(const TevKey& k);

// Text form of a key (hex words), for tools/tevgen_samples; parse returns
// false on malformed input.
std::string tevgen_key_to_text(const TevKey& k);
bool tevgen_key_from_text(const char* text, TevKey* k);

#endif
