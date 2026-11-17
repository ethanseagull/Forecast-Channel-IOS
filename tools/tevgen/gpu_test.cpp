// Checks that the specialized TEV shaders (port/gx/gx_tevgen.cpp) draw exactly
// what the ubershader (port/gx/shaders/gx_ps.hlsl, the committed DXIL) draws,
// on this PC's GPU through SDL3 GPU (D3D12), for many register states:
//   - every key in a keys file (FORECAST_SPECIALIZED_DUMP's keys.txt: the
//     configurations the channel really uses), turned back into registers;
//   - random registers (every field random).
// Each is drawn with random vertex colours, texture coordinates (with q),
// textures, samplers and uniforms (TEV registers, konst colours, alpha
// references, indirect matrices and scales, texcoord and texture sizes),
// once with each shader, into a 64x64 target cleared to a marker colour (so
// discarded pixels show), and the two images must be byte-identical.
//
//   gpu_test <keys.txt or -> [random count] [seed]
//
// Built by tools\tevgen\build.bat. Windows only (DXC from the Windows SDK).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "gx_dxc.h"
#include "gx_tevgen.h"
#include "shaders/gx_shaders_dxil.h"

typedef uint8_t u8;
typedef uint32_t u32;
typedef int32_t s32;

// port/gx/gx_internal.h GXTevUniforms, copied (that header needs the port's).
struct GXTevUniforms {
    u32 colorEnv[16];
    u32 alphaEnv[16];
    u32 tevOrder[8];
    u32 ksel[8];
    s32 tevReg[4][4];
    s32 konst[4][4];
    u32 genMode;
    u32 alphaCompare;
    u32 texBound;
    u32 pad;
    u32 tevInd[16];
    u32 indMtx[12];
    u32 indScale[2];
    u32 indRef;
    u32 pad2;
    u32 tcScale[8];
    u32 texImage0[8];
};
static_assert(sizeof(GXTevUniforms) == 528, "GXTevUniforms");

struct Vtx {
    float pos[4];
    float clr[2][4];
    float tex[8][3];
};

static const u32 kSize = 64;
static u32 sRand = 1;

static u32 rnd(void) {
    sRand ^= sRand << 13;
    sRand ^= sRand >> 17;
    sRand ^= sRand << 5;
    return sRand;
}
static float frand(float lo, float hi) {
    return lo + (hi - lo) * (float)(rnd() & 0xFFFFFF) / 16777215.0f;
}

static SDL_GPUDevice* sDev;
static SDL_GPUShader* sVS;
static SDL_GPUTexture* sTarget;
static SDL_GPUTexture* sTex[8];
static SDL_GPUSampler* sSmp[8];
static SDL_GPUBuffer* sVB;
static SDL_GPUTransferBuffer* sDown;

static SDL_GPUShader* make_shader(const void* code, size_t size, SDL_GPUShaderStage stage, u32 samplers, u32 ubs) {
    SDL_GPUShaderCreateInfo si = {};
    si.code = (const Uint8*)code;
    si.code_size = size;
    si.entrypoint = "main";
    si.format = SDL_GPU_SHADERFORMAT_DXIL;
    si.stage = stage;
    si.num_samplers = samplers;
    si.num_uniform_buffers = ubs;
    return SDL_CreateGPUShader(sDev, &si);
}

static SDL_GPUGraphicsPipeline* make_pipe(SDL_GPUShader* ps) {
    SDL_GPUVertexBufferDescription vb = {};
    vb.pitch = sizeof(Vtx);
    vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute attrs[11] = {};
    attrs[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, (Uint32)offsetof(Vtx, pos)};
    attrs[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, (Uint32)offsetof(Vtx, clr[0])};
    attrs[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, (Uint32)offsetof(Vtx, clr[1])};
    for (int i = 0; i < 8; i++) {
        attrs[3 + i] = {(Uint32)(3 + i), 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
                        (Uint32)(offsetof(Vtx, tex) + i * 3 * sizeof(float))};
    }
    SDL_GPUColorTargetDescription ct = {};
    ct.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    SDL_GPUGraphicsPipelineCreateInfo pi = {};
    pi.vertex_shader = sVS;
    pi.fragment_shader = ps;
    pi.vertex_input_state.vertex_buffer_descriptions = &vb;
    pi.vertex_input_state.num_vertex_buffers = 1;
    pi.vertex_input_state.vertex_attributes = attrs;
    pi.vertex_input_state.num_vertex_attributes = 11;
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.target_info.color_target_descriptions = &ct;
    pi.target_info.num_color_targets = 1;
    return SDL_CreateGPUGraphicsPipeline(sDev, &pi);
}

static SDL_GPUTexture* make_texture(u32 w, u32 h, SDL_GPUTextureUsageFlags usage) {
    SDL_GPUTextureCreateInfo ti = {};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.usage = usage;
    ti.width = w;
    ti.height = h;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    return SDL_CreateGPUTexture(sDev, &ti);
}

// Uploads bytes to a texture or buffer.
static void upload(SDL_GPUTexture* tex, u32 w, u32 h, SDL_GPUBuffer* buf, const void* data, u32 size) {
    SDL_GPUTransferBufferCreateInfo ti = {};
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    ti.size = size;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(sDev, &ti);
    memcpy(SDL_MapGPUTransferBuffer(sDev, tb, false), data, size);
    SDL_UnmapGPUTransferBuffer(sDev, tb);
    SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(sDev);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
    if (tex != NULL) {
        SDL_GPUTextureTransferInfo src = {};
        src.transfer_buffer = tb;
        SDL_GPUTextureRegion dst = {};
        dst.texture = tex;
        dst.w = w;
        dst.h = h;
        dst.d = 1;
        SDL_UploadToGPUTexture(cp, &src, &dst, false);
    } else {
        SDL_GPUTransferBufferLocation src = {tb, 0};
        SDL_GPUBufferRegion dst = {buf, 0, size};
        SDL_UploadToGPUBuffer(cp, &src, &dst, false);
    }
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cb);
    SDL_WaitForGPUIdle(sDev);
    SDL_ReleaseGPUTransferBuffer(sDev, tb);
}

// Random inputs for one test: vertices, textures and samplers.
static void randomize_inputs(void) {
    // A quad over the target: corners bottom-left, bottom-right, top-left,
    // top-right, each with its own random attributes.
    static const float cornerPos[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    Vtx quad[4];
    for (int c = 0; c < 4; c++) {
        Vtx& q = quad[c];
        q.pos[0] = cornerPos[c][0];
        q.pos[1] = cornerPos[c][1];
        q.pos[2] = 0.5f;
        q.pos[3] = 1.0f;
        for (int ch = 0; ch < 2; ch++) {
            for (int i = 0; i < 4; i++) {
                q.clr[ch][i] = frand(-0.2f, 1.2f);
            }
        }
        for (int t = 0; t < 8; t++) {
            q.tex[t][0] = frand(-3.0f, 3.0f);
            q.tex[t][1] = frand(-3.0f, 3.0f);
            q.tex[t][2] = (rnd() & 7) == 0 ? 1.0f : frand(0.25f, 2.0f);
        }
    }
    static const int idx[6] = {0, 1, 2, 1, 3, 2};
    std::vector<Vtx> v(6);
    for (int i = 0; i < 6; i++) {
        v[i] = quad[idx[i]];
    }
    upload(NULL, 0, 0, sVB, v.data(), (u32)(v.size() * sizeof(Vtx)));

    for (int m = 0; m < 8; m++) {
        if (sTex[m] != NULL) {
            SDL_ReleaseGPUTexture(sDev, sTex[m]);
        }
        u32 w = 1u << (rnd() % 6), h = 1u << (rnd() % 6);
        sTex[m] = make_texture(w, h, SDL_GPU_TEXTUREUSAGE_SAMPLER);
        std::vector<u8> px(w * h * 4);
        for (auto& b : px) {
            // Mostly random, with the extremes common.
            u32 r = rnd();
            b = (r & 7) == 0 ? 0 : (r & 7) == 1 ? 255 : (u8)(r >> 8);
        }
        upload(sTex[m], w, h, NULL, px.data(), (u32)px.size());
        if (sSmp[m] != NULL) {
            SDL_ReleaseGPUSampler(sDev, sSmp[m]);
        }
        static const SDL_GPUSamplerAddressMode wrap[3] = {SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                                                          SDL_GPU_SAMPLERADDRESSMODE_REPEAT,
                                                          SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT};
        SDL_GPUSamplerCreateInfo si = {};
        si.min_filter = si.mag_filter = (rnd() & 1) ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
        si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        si.address_mode_u = wrap[rnd() % 3];
        si.address_mode_v = wrap[rnd() % 3];
        si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sSmp[m] = SDL_CreateGPUSampler(sDev, &si);
    }
}

// Random uniforms (everything that is not in the key).
static void randomize_uniforms(GXTevUniforms* u) {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            u->tevReg[r][c] = (int)(rnd() % 2048) - 1024;
            u->konst[r][c] = (int)(rnd() & 255);
        }
    }
    u->alphaCompare = (u->alphaCompare & 0xFF0000) | (rnd() & 0xFFFF);
    for (int i = 0; i < 12; i++) {
        u->indMtx[i] = i < 9 ? rnd() & 0xFFFFFF : 0;
    }
    u->indScale[0] = rnd() & 0xFFFF;
    u->indScale[1] = rnd() & 0xFFFF;
    for (int n = 0; n < 8; n++) {
        u->tcScale[n] = (rnd() % 1024) | (rnd() % 1024) << 16;
        u->texImage0[n] = rnd() & 0xFFFFF;
    }
}

// Registers whose key is k (gx_tevgen's key is register fields, normalized),
// with random values where the key has none.
static void regs_from_key(const TevKey& k, GXTevUniforms* u) {
    memset(u, 0, sizeof(*u));
    u->genMode = (k.numStages - 1) << 10 | k.numInd << 16;
    for (u32 s = 0; s < 16; s++) {
        const TevKey::Stage& st = k.stage[s];
        u->colorEnv[s] = s < k.numStages ? st.color : rnd() & 0xFFFFFF;
        u->alphaEnv[s] = s < k.numStages ? st.alpha : rnd() & 0xFFFFFF;
        u->tevInd[s] = s < k.numStages ? st.ind : rnd() & 0x1FFFFF;
        u->tevOrder[s / 2] |= (st.order & 0xFFF) << ((s & 1) * 12);
        u->ksel[s / 2] |= ((st.order >> 12) & 0x3FF) << (4 + (s & 1) * 10);
    }
    for (u32 t = 0; t < 4; t++) {
        u32 sw = (k.swap >> (t * 8)) & 0xFF;
        u->ksel[t * 2] |= sw & 15;
        u->ksel[t * 2 + 1] |= sw >> 4;
    }
    u->alphaCompare = (k.alphaTest & 63) << 16 | ((k.alphaTest >> 6) & 3) << 22;
    u->texBound = k.texBound;
    u->indRef = k.indRef;
}

static void random_regs(GXTevUniforms* u) {
    memset(u, 0, sizeof(*u));
    for (int i = 0; i < 16; i++) {
        u->colorEnv[i] = rnd() & 0xFFFFFF;
        u->alphaEnv[i] = rnd() & 0xFFFFFF;
        u->tevInd[i] = (rnd() & 3) == 0 ? 0 : rnd() & 0x1FFFFF;
    }
    for (int i = 0; i < 8; i++) {
        u->tevOrder[i] = rnd() & 0xFFFFFF;
        u->ksel[i] = rnd() & 0xFFFFFF;
    }
    u->genMode = (rnd() & 15) << 10 | (rnd() & 7) << 16;
    u->alphaCompare = (rnd() & 0xFF) << 16;
    u->texBound = rnd() & 0xFF;
    u->indRef = rnd() & 0xFFFFFF;
}

static TevGenRegs to_regs(const GXTevUniforms& u) {
    TevGenRegs r;
    memcpy(r.colorEnv, u.colorEnv, sizeof(r.colorEnv));
    memcpy(r.alphaEnv, u.alphaEnv, sizeof(r.alphaEnv));
    memcpy(r.tevOrder, u.tevOrder, sizeof(r.tevOrder));
    memcpy(r.ksel, u.ksel, sizeof(r.ksel));
    memcpy(r.tevInd, u.tevInd, sizeof(r.tevInd));
    r.genMode = u.genMode;
    r.alphaCompare = u.alphaCompare;
    r.texBound = u.texBound;
    r.indRef = u.indRef;
    return r;
}

static bool draw(SDL_GPUGraphicsPipeline* p, const GXTevUniforms& u, std::vector<u8>* out) {
    SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(sDev);
    SDL_GPUColorTargetInfo ct = {};
    ct.texture = sTarget;
    ct.load_op = SDL_GPU_LOADOP_CLEAR;
    ct.clear_color = {0.2f, 0.4f, 0.6f, 0.8f};
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cb, &ct, 1, NULL);
    SDL_BindGPUGraphicsPipeline(rp, p);
    SDL_GPUBufferBinding vb = {sVB, 0};
    SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
    SDL_GPUTextureSamplerBinding tb[8];
    for (int m = 0; m < 8; m++) {
        tb[m].texture = sTex[m];
        tb[m].sampler = sSmp[m];
    }
    SDL_BindGPUFragmentSamplers(rp, 0, tb, 8);
    SDL_PushGPUFragmentUniformData(cb, 0, &u, sizeof(u));
    SDL_DrawGPUPrimitives(rp, 6, 1, 0, 0);
    SDL_EndGPURenderPass(rp);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
    SDL_GPUTextureRegion src = {};
    src.texture = sTarget;
    src.w = kSize;
    src.h = kSize;
    src.d = 1;
    SDL_GPUTextureTransferInfo dst = {};
    dst.transfer_buffer = sDown;
    SDL_DownloadFromGPUTexture(cp, &src, &dst);
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cb);
    SDL_WaitForGPUIdle(sDev);
    const u8* px = (const u8*)SDL_MapGPUTransferBuffer(sDev, sDown, false);
    out->assign(px, px + kSize * kSize * 4);
    SDL_UnmapGPUTransferBuffer(sDev, sDown);
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: gpu_test <keys.txt or -> [random count] [seed]\n");
        return 2;
    }
    int randomCount = argc > 2 ? atoi(argv[2]) : 0;
    sRand = argc > 3 ? (u32)strtoul(argv[3], NULL, 0) : 1;
    if (sRand == 0) {
        sRand = 1;
    }
    std::string where;
    if (!gx_dxc_load(&where)) {
        fprintf(stderr, "dxcompiler.dll not found\n");
        return 1;
    }
    printf("DXC from %s\n", where.c_str());
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s", SDL_GetError());
        return 1;
    }
    sDev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_DXIL, false, NULL);
    if (sDev == NULL) {
        fprintf(stderr, "SDL_CreateGPUDevice: %s\n", SDL_GetError());
        return 1;
    }
    sVS = make_shader(gx_vs_dxil, gx_vs_dxil_size, SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    SDL_GPUShader* uberPS = make_shader(gx_ps_dxil, gx_ps_dxil_size, SDL_GPU_SHADERSTAGE_FRAGMENT, 8, 1);
    SDL_GPUGraphicsPipeline* uber = make_pipe(uberPS);
    sTarget = make_texture(kSize, kSize, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
    SDL_GPUBufferCreateInfo bi = {};
    bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    bi.size = 6 * sizeof(Vtx);
    sVB = SDL_CreateGPUBuffer(sDev, &bi);
    SDL_GPUTransferBufferCreateInfo ti = {};
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    ti.size = kSize * kSize * 4;
    sDown = SDL_CreateGPUTransferBuffer(sDev, &ti);
    if (uber == NULL || sTarget == NULL || sVB == NULL || sDown == NULL) {
        fprintf(stderr, "setup failed: %s\n", SDL_GetError());
        return 1;
    }

    // The register states to test.
    std::vector<GXTevUniforms> cases;
    std::vector<std::string> names;
    if (strcmp(argv[1], "-") != 0) {
        FILE* f = fopen(argv[1], "rb");
        if (f == NULL) {
            fprintf(stderr, "cannot read %s\n", argv[1]);
            return 1;
        }
        char line[4096];
        while (fgets(line, sizeof(line), f) != NULL) {
            TevKey k;
            if (line[0] == '#' || !tevgen_key_from_text(line, &k)) {
                continue;
            }
            GXTevUniforms u;
            regs_from_key(k, &u);
            cases.push_back(u);
            names.push_back("key " + std::to_string(cases.size()));
        }
        fclose(f);
    }
    for (int i = 0; i < randomCount; i++) {
        GXTevUniforms u;
        random_regs(&u);
        cases.push_back(u);
        names.push_back("random " + std::to_string(i));
    }

    // Each case several times with new random inputs and uniforms.
    const int kRounds = 4;
    int failures = 0, draws = 0, keyMismatch = 0;
    std::vector<u8> a, b;
    for (size_t c = 0; c < cases.size(); c++) {
        TevKey key;
        tevgen_make_key(to_regs(cases[c]), &key);
        std::string src = tevgen_source(key, TEVGEN_HLSL);
        std::vector<unsigned char> dxil;
        std::string err;
        if (!gx_dxc_compile(src, &dxil, &err)) {
            printf("%s: compile failed:\n%s\n%s\n", names[c].c_str(), err.c_str(), src.c_str());
            failures++;
            continue;
        }
        SDL_GPUShader* ps = make_shader(dxil.data(), dxil.size(), SDL_GPU_SHADERSTAGE_FRAGMENT, 8, 1);
        SDL_GPUGraphicsPipeline* spec = ps != NULL ? make_pipe(ps) : NULL;
        if (spec == NULL) {
            printf("%s: pipeline failed: %s\n", names[c].c_str(), SDL_GetError());
            failures++;
            continue;
        }
        for (int round = 0; round < kRounds; round++) {
            GXTevUniforms u = cases[c];
            randomize_uniforms(&u);
            // The key ignores the uniforms: check it stays the same.
            TevKey k2;
            tevgen_make_key(to_regs(u), &k2);
            if (memcmp(&k2, &key, sizeof(key)) != 0) {
                keyMismatch++;
            }
            randomize_inputs();
            draw(uber, u, &a);
            draw(spec, u, &b);
            draws++;
            if (a != b) {
                int diffs = 0, maxd = 0, first = -1;
                for (u32 i = 0; i < kSize * kSize * 4; i++) {
                    int d = abs((int)a[i] - (int)b[i]);
                    if (d != 0) {
                        diffs++;
                        if (first < 0) {
                            first = (int)i;
                        }
                    }
                    maxd = d > maxd ? d : maxd;
                }
                printf("%s round %d: MISMATCH %d bytes, max %d; first at pixel %d: uber %d %d %d %d spec %d %d %d %d\n"
                       "  key %s\n",
                       names[c].c_str(), round, diffs, maxd, first / 4, a[first & ~3], a[(first & ~3) + 1],
                       a[(first & ~3) + 2], a[(first & ~3) + 3], b[first & ~3], b[(first & ~3) + 1],
                       b[(first & ~3) + 2], b[(first & ~3) + 3], tevgen_key_to_text(key).c_str());
                failures++;
                break;
            }
        }
        SDL_ReleaseGPUGraphicsPipeline(sDev, spec);
        SDL_ReleaseGPUShader(sDev, ps);
    }
    printf("%zu cases, %d draws compared, %d failures, %d uniform-only changes that changed the key\n", cases.size(),
           draws, failures, keyMismatch);
    return failures == 0 && keyMismatch == 0 ? 0 : 1;
}
