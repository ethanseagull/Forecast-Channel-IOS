// The GX renderer on SDL3 GPU (D3D12 on Windows, Metal on iOS).
//
// Draws are recorded as the command processor decodes them: gx_xform.cpp
// transforms each primitive into a frame-wide vertex list, and the draw keeps
// a copy of the pixel state it needs (pipeline, textures, scissor, the TEV
// registers). The pixel shader is the TEV ubershader, or once it is compiled
// a shader specialized for the draw's TEV configuration (gx_spec.cpp).
// Textures are decoded by gx_tex.cpp and uploaded once per version. The copy
// to the display (GXCopyDisp) ends the frame: one copy pass uploads the
// vertices and new textures, one render pass replays the draws into the EFB
// (a render target at a multiple of the Wii's resolution), and the EFB is
// scaled onto the window (with the screen fill beside it on screens wider than
// 16:9: "screen fill" below).
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>

#include "gx_internal.h"
#include "gx_spec.h"
#include "gx_tex_decode.h"
#include "gx_texcodec.h"
// Shaders: DXIL (D3D12) on Windows, committed; MSL (Metal) on Apple,
// generated from the same HLSL at build time (tools/build_shaders_msl.sh,
// header in the build tree).
#ifdef __APPLE__
#include "gx_shaders_msl.h"
#define GX_SHADER_FORMAT SDL_GPU_SHADERFORMAT_MSL
#define GX_SHADER_ENTRY "main0"
#define GX_VS_CODE gx_vs_msl
#define GX_VS_SIZE (sizeof(gx_vs_msl) - 1)
#define GX_PS_CODE gx_ps_msl
#define GX_PS_SIZE (sizeof(gx_ps_msl) - 1)
#define GX_PRESENT_PS_CODE gx_present_ps_msl
#define GX_PRESENT_PS_SIZE (sizeof(gx_present_ps_msl) - 1)
#else
#include "shaders/gx_shaders_dxil.h"
#define GX_SHADER_FORMAT SDL_GPU_SHADERFORMAT_DXIL
#define GX_SHADER_ENTRY "main"
#define GX_VS_CODE gx_vs_dxil
#define GX_VS_SIZE sizeof(gx_vs_dxil)
#define GX_PS_CODE gx_ps_dxil
#define GX_PS_SIZE sizeof(gx_ps_dxil)
#define GX_PRESENT_PS_CODE gx_present_ps_dxil
#define GX_PRESENT_PS_SIZE sizeof(gx_present_ps_dxil)
#endif
#include <port_log.h>
#include <port_platform.h>

#define WARN_ONCE(...)                                                         \
    do {                                                                       \
        static bool sWarned_;                                                  \
        if (!sWarned_) {                                                       \
            sWarned_ = true;                                                   \
            port_log(__VA_ARGS__);                                             \
        }                                                                      \
    } while (0)

// The Wii's display is 16:9 (SCGetAspectRatio says wide): the EFB is drawn
// anamorphically and stretched, as the TV would, into the frame
// port_display_layout (port/sdk/platform.c) places on the window.

static SDL_GPUDevice* sDev;
static SDL_GPUShader* sVS;
static SDL_GPUShader* sPS;
static SDL_GPUTextureFormat sDepthFmt;

// The EFB: colour and depth targets at sScale times the Wii's size, plus
// sMargin EFB pixels left and right of it when the screen is wider than 16:9
// (see "Screen fill" below). The game's EFB starts at x = sMargin.
static SDL_GPUTexture* sEfbColor;
static SDL_GPUTexture* sEfbDepth;
static u32 sEfbW, sEfbH, sScale;
static u32 sMargin;
static f32 sWideK = 1.0f; // sEfbW / (sEfbW + 2 * sMargin)
// The EFB texture in EFB pixels (times sScale on the GPU): landscape the
// frame and its margins (sEfbW + 2 * sMargin by sEfbH); portrait the canvas
// (see "Portrait" below).
static u32 sCanvasW, sCanvasH;
static bool sPortrait;   // the EFB is a portrait canvas
static bool sCanvasMode; // portrait, this frame: laid out for the canvas (else letterboxed)
static bool sFrameOpen;  // this frame's first draw or copy has decided sCanvasMode
static bool sClearNext = true;
static SDL_FColor sClearColor;
static f32 sClearDepth = 1.0f;

static SDL_GPUBuffer* sVtxBuf;
static u32 sVtxBufSize;
static SDL_GPUTransferBuffer* sXfer;
static u32 sXferSize;
static SDL_GPUTexture* sWhite;

static u32 sFrame;
// The frame's command buffer, acquired when first needed.
static SDL_GPUCommandBuffer* sCmd;

// EFB copies to textures (GXCopyTex), by destination address: a draw that
// samples that address gets the copy instead of the (never written) memory.
struct EfbCopy {
    SDL_GPUTexture* tex;
    u32 w, h; // GPU size (EFB pixels times the scale)
};
static std::unordered_map<u32, EfbCopy> sEfbCopies;

// --- pipelines ----------------------------------------------------------------------

// Everything a pipeline depends on, packed from the BP registers.
union PipeKey {
    u32 bits;
    struct {
        u32 blend : 1;
        u32 srcFactor : 3;
        u32 dstFactor : 3;
        u32 subtract : 1;
        u32 colorWrite : 1;
        u32 alphaWrite : 1;
        u32 efbAlpha : 1;
        u32 zEnable : 1;
        u32 zFunc : 3;
        u32 zWrite : 1;
        u32 cull : 2;
    };
};

// By shader (0 the ubershader, else a specialized shader's id, gx_spec.h) and
// PipeKey bits.
static std::unordered_map<u64, SDL_GPUGraphicsPipeline*> sPipelines;

static SDL_GPUBlendFactor blend_factor(u32 f, bool dst, bool efbAlpha) {
    switch (f) {
    case 0:
        return SDL_GPU_BLENDFACTOR_ZERO;
    case 1:
        return SDL_GPU_BLENDFACTOR_ONE;
    case 2:
        return dst ? SDL_GPU_BLENDFACTOR_SRC_COLOR : SDL_GPU_BLENDFACTOR_DST_COLOR;
    case 3:
        return dst ? SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR : SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
    case 4:
        return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    case 5:
        return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    case 6:
        return efbAlpha ? SDL_GPU_BLENDFACTOR_DST_ALPHA : SDL_GPU_BLENDFACTOR_ONE;
    default:
        return efbAlpha ? SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA : SDL_GPU_BLENDFACTOR_ZERO;
    }
}

static PipeKey pipe_key(void) {
    u32 cmode0 = gx.bp[0x41];
    u32 zmode = gx.bp[0x40];
    PipeKey k;
    k.bits = 0;
    k.efbAlpha = gx_bits(gx.bp[0x43], 0, 3) == 1; // RGBA6_Z24
    bool blend = cmode0 & 1;
    bool logic = (cmode0 >> 1) & 1;
    if (blend) {
        k.blend = 1;
        k.subtract = (cmode0 >> 11) & 1;
        k.srcFactor = gx_bits(cmode0, 8, 3);
        k.dstFactor = gx_bits(cmode0, 5, 3);
    } else if (logic && gx_bits(cmode0, 12, 4) != 3) {
        WARN_ONCE("gx: logic op %lu drawn as copy\n", (unsigned long)gx_bits(cmode0, 12, 4));
    }
    k.colorWrite = (cmode0 >> 3) & 1;
    k.alphaWrite = (cmode0 >> 4) & 1;
    k.zEnable = zmode & 1;
    if (k.zEnable) {
        k.zFunc = gx_bits(zmode, 1, 3);
        k.zWrite = (zmode >> 4) & 1;
    }
    k.cull = gx_bits(gx.bp[0x00], 14, 2);
    return k;
}

// The vertex input of gx_vs.hlsl: one buffer of GXVtxOut.
static void vertex_input(SDL_GPUVertexBufferDescription* vb, SDL_GPUVertexAttribute attrs[11]) {
    *vb = {};
    vb->slot = 0;
    vb->pitch = sizeof(GXVtxOut);
    vb->input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    attrs[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, (Uint32)offsetof(GXVtxOut, pos)};
    attrs[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, (Uint32)offsetof(GXVtxOut, clr[0])};
    attrs[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, (Uint32)offsetof(GXVtxOut, clr[1])};
    for (int i = 0; i < 8; i++) {
        attrs[3 + i] = {(Uint32)(3 + i), 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
                        (Uint32)(offsetof(GXVtxOut, tex) + i * 3 * sizeof(f32))};
    }
}

// The pipeline for state k with pixel shader ps: the ubershader (specId 0)
// or specialized shader specId. Specialized ones are created within the
// main thread's budget (gx_spec_budget_left): NULL when it is spent (the draw
// falls back to the ubershader; asked again later) or creation failed.
static SDL_GPUGraphicsPipeline* get_pipeline(PipeKey k, u32 specId = 0, SDL_GPUShader* ps = NULL,
                                             bool remember = true) {
    u64 mapKey = (u64)specId << 32 | k.bits;
    auto it = sPipelines.find(mapKey);
    if (it != sPipelines.end()) {
        return it->second;
    }
    if (specId != 0 && !gx_spec_budget_left()) {
        return NULL;
    }
    Uint64 start = SDL_GetTicksNS();

    SDL_GPUVertexBufferDescription vb;
    SDL_GPUVertexAttribute attrs[11];
    vertex_input(&vb, attrs);

    SDL_GPUColorTargetDescription ct = {};
    ct.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    SDL_GPUColorTargetBlendState& bs = ct.blend_state;
    bs.enable_color_write_mask = true;
    bs.color_write_mask = (k.colorWrite ? SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B : 0) |
                          (k.alphaWrite && k.efbAlpha ? SDL_GPU_COLORCOMPONENT_A : 0);
    if (k.blend) {
        bs.enable_blend = true;
        if (k.subtract) {
            bs.color_blend_op = bs.alpha_blend_op = SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
            bs.src_color_blendfactor = bs.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            bs.src_alpha_blendfactor = bs.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        } else {
            bs.color_blend_op = bs.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
            bs.src_color_blendfactor = bs.src_alpha_blendfactor = blend_factor(k.srcFactor, false, k.efbAlpha);
            bs.dst_color_blendfactor = bs.dst_alpha_blendfactor = blend_factor(k.dstFactor, true, k.efbAlpha);
        }
    }

    SDL_GPUGraphicsPipelineCreateInfo pi = {};
    pi.vertex_shader = sVS;
    pi.fragment_shader = specId != 0 ? ps : sPS;
    pi.vertex_input_state.vertex_buffer_descriptions = &vb;
    pi.vertex_input_state.num_vertex_buffers = 1;
    pi.vertex_input_state.vertex_attributes = attrs;
    pi.vertex_input_state.num_vertex_attributes = 11;
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    // GX: hardware cull mode 1 culls back faces, 2 front faces; front faces
    // wind clockwise on screen.
    pi.rasterizer_state.cull_mode = k.cull == 1 ? SDL_GPU_CULLMODE_BACK
                                    : k.cull == 2 ? SDL_GPU_CULLMODE_FRONT
                                                  : SDL_GPU_CULLMODE_NONE;
    pi.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
    static const SDL_GPUCompareOp cmp[8] = {
        SDL_GPU_COMPAREOP_NEVER,   SDL_GPU_COMPAREOP_LESS,      SDL_GPU_COMPAREOP_EQUAL,
        SDL_GPU_COMPAREOP_LESS_OR_EQUAL, SDL_GPU_COMPAREOP_GREATER, SDL_GPU_COMPAREOP_NOT_EQUAL,
        SDL_GPU_COMPAREOP_GREATER_OR_EQUAL, SDL_GPU_COMPAREOP_ALWAYS};
    pi.depth_stencil_state.enable_depth_test = k.zEnable;
    pi.depth_stencil_state.enable_depth_write = k.zEnable && k.zWrite;
    pi.depth_stencil_state.compare_op = k.zEnable ? cmp[k.zFunc] : SDL_GPU_COMPAREOP_ALWAYS;
    pi.target_info.color_target_descriptions = &ct;
    pi.target_info.num_color_targets = 1;
    pi.target_info.depth_stencil_format = sDepthFmt;
    pi.target_info.has_depth_stencil_target = true;

    SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(sDev, &pi);
    if (p == NULL) {
        port_log("gx: pipeline %08lx (shader %lu) failed: %s\n", (unsigned long)k.bits, (unsigned long)specId,
                 SDL_GetError());
    }
    sPipelines[mapKey] = p;
    if (specId != 0) {
        gx_spec_budget_charge(SDL_GetTicksNS() - start);
        if (p != NULL && remember) {
            gx_spec_pipeline_created(specId, k.bits);
        }
    }
    return p;
}

// --- samplers and textures ---------------------------------------------------------

static SDL_GPUSampler* sSamplers[3][3][2][2];

static SDL_GPUSampler* get_sampler(const GXTexDesc& d) {
    u32 ws = d.wrapS < 3 ? d.wrapS : 1, wt = d.wrapT < 3 ? d.wrapT : 1;
    SDL_GPUSampler*& s = sSamplers[ws][wt][d.minLinear ? 1 : 0][d.magLinear ? 1 : 0];
    if (s == NULL) {
        static const SDL_GPUSamplerAddressMode wrap[3] = {SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                                                          SDL_GPU_SAMPLERADDRESSMODE_REPEAT,
                                                          SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT};
        SDL_GPUSamplerCreateInfo si = {};
        si.min_filter = d.minLinear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
        si.mag_filter = d.magLinear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
        si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        si.address_mode_u = wrap[ws];
        si.address_mode_v = wrap[wt];
        si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        s = SDL_CreateGPUSampler(sDev, &si);
    }
    return s;
}

struct BackendTex {
    u32 version;
    SDL_GPUTexture* tex;
    u32 lastFrame;
    s32 waiting = -1; // texture pack replacement still being loaded
    u64 bytes = 0;    // GPU memory of tex
};

struct PendingUpload {
    SDL_GPUTexture* tex;
    u32 offset; // into sUploadBytes
    u32 w, h;
};

static std::unordered_map<u64, BackendTex> sTextures;
static std::vector<SDL_GPUTexture*> sRetired; // released after this frame is submitted
static std::vector<PendingUpload> sUploads;
static std::vector<u8> sUploadBytes;

static SDL_GPUTexture* create_texture(u32 w, u32 h, SDL_GPUTextureFormat fmt, SDL_GPUTextureUsageFlags usage) {
    SDL_GPUTextureCreateInfo ti = {};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = fmt;
    ti.usage = usage;
    ti.width = w;
    ti.height = h;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
    return SDL_CreateGPUTexture(sDev, &ti);
}

// Queues `bytes` of texel data (RGBA8 rows or compressed blocks, packed) for
// a w x h texture.
static void queue_upload_bytes(SDL_GPUTexture* tex, const u8* data, size_t bytes, u32 w, u32 h) {
    PendingUpload u;
    u.tex = tex;
    u.offset = (u32)sUploadBytes.size();
    u.w = w;
    u.h = h;
    sUploadBytes.insert(sUploadBytes.end(), data, data + bytes);
    sUploads.push_back(u);
}

static void queue_upload(SDL_GPUTexture* tex, const u8* rgba, u32 w, u32 h) {
    queue_upload_bytes(tex, rgba, (size_t)w * h * 4, w, h);
}

// --- compressed textures ----------------------------------------------------------
//
// GPU memory: the globe's 28 CMPR tiles are 112 MB as RGBA8 (448 MB with the
// 2x texture pack). CMPR is DXT1, so where BC1 works (PCs, Macs) it is
// uploaded as BC1 unchanged (4 bits a pixel); where it does not (iPhones:
// BC support there is unreliable) its blocks are transcoded to ASTC 4x4
// (8 bits a pixel). Large opaque pack images are encoded the same way on the
// loader threads. FORECAST_TEXCOMP=none|bc1|astc overrides the choice.

static int sTexComp = GX_TEXCOMP_NONE;
static u64 sTexBytes; // size of the texture last created by upload_*
static std::vector<u8> sAstcScratch;

static void choose_texcomp(void) {
    bool bc1 = SDL_GPUTextureSupportsFormat(sDev, SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM, SDL_GPU_TEXTURETYPE_2D,
                                            SDL_GPU_TEXTUREUSAGE_SAMPLER);
    bool astc = SDL_GPUTextureSupportsFormat(sDev, SDL_GPU_TEXTUREFORMAT_ASTC_4x4_UNORM, SDL_GPU_TEXTURETYPE_2D,
                                             SDL_GPU_TEXTUREUSAGE_SAMPLER);
#ifdef SDL_PLATFORM_IOS
    // Reported as supported on some iPhones yet drawn black: prefer ASTC.
    if (astc) {
        bc1 = false;
    }
#endif
    sTexComp = bc1 ? GX_TEXCOMP_BC1 : astc ? GX_TEXCOMP_ASTC : GX_TEXCOMP_NONE;
    const char* force = SDL_getenv("FORECAST_TEXCOMP");
    if (force != NULL) {
        if (SDL_strcmp(force, "none") == 0) {
            sTexComp = GX_TEXCOMP_NONE;
        } else if (SDL_strcmp(force, "bc1") == 0 && bc1) {
            sTexComp = GX_TEXCOMP_BC1;
        } else if (SDL_strcmp(force, "astc") == 0 && astc) {
            sTexComp = GX_TEXCOMP_ASTC;
        } else {
            port_log("gx: FORECAST_TEXCOMP=%s not available here\n", force);
        }
    }
    static const char* const names[] = {"none (RGBA8)", "BC1", "ASTC 4x4"};
    port_log("gx: texture compression: %s (BC1 %s, ASTC %s)\n", names[sTexComp], bc1 ? "yes" : "no",
             astc ? "yes" : "no");
    gx_texpack_set_compression(sTexComp);
}

// Uploads BC1 blocks (bw x bh) as the device's compressed format.
static SDL_GPUTexture* upload_bc1(const u8* bc1, u32 bw, u32 bh) {
    u32 w = bw * 4, h = bh * 4;
    size_t blocks = (size_t)bw * bh;
    if (sTexComp == GX_TEXCOMP_BC1) {
        SDL_GPUTexture* t = create_texture(w, h, SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER);
        if (t != NULL) {
            queue_upload_bytes(t, bc1, blocks * 8, w, h);
            sTexBytes = blocks * 8;
        }
        return t;
    }
    sAstcScratch.resize(blocks * 16);
    gx_bc1_to_astc4x4(bc1, blocks, sAstcScratch.data());
    SDL_GPUTexture* t = create_texture(w, h, SDL_GPU_TEXTUREFORMAT_ASTC_4x4_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    if (t != NULL) {
        queue_upload_bytes(t, sAstcScratch.data(), blocks * 16, w, h);
        sTexBytes = blocks * 16;
    }
    return t;
}

// A new RGBA8 texture holding w x h pixels (rows pitch bytes apart).
static SDL_GPUTexture* upload_rgba(const u8* px, u32 w, u32 h, u32 pitch) {
    SDL_GPUTexture* t = create_texture(w, h, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    if (t == NULL) {
        port_log("gx: texture %lux%lu failed: %s\n", (unsigned long)w, (unsigned long)h, SDL_GetError());
    } else if (pitch == w * 4) {
        queue_upload(t, px, w, h);
    } else {
        // Padded rows: copy them packed.
        std::vector<u8> packed((size_t)w * h * 4);
        for (u32 y = 0; y < h; y++) {
            memcpy(&packed[(size_t)y * w * 4], px + (size_t)y * pitch, (size_t)w * 4);
        }
        queue_upload(t, packed.data(), w, h);
    }
    if (t != NULL) {
        sTexBytes = (u64)w * h * 4;
    }
    return t;
}

static void set_gpu_texture(BackendTex& t, SDL_GPUTexture* tex) {
    if (t.tex != NULL) {
        sRetired.push_back(t.tex);
    }
    t.tex = tex;
    t.bytes = sTexBytes;
}

// The GPU copy of the texture in map `map` (described by d), uploading the
// version the GPU lacks: CMPR as BC1 / ASTC where available, the rest as
// RGBA8. A texture pack replacement is loaded on a worker thread: the Wii
// image is drawn until it arrives, then swapped for it (any size: sampling
// is normalized).
static SDL_GPUTexture* get_texture(int map, const GXTexDesc& d) {
    BackendTex& t = sTextures[d.key];
    if (t.tex == NULL || t.version != d.version) {
        SDL_GPUTexture* nt = NULL;
        if (sTexComp != GX_TEXCOMP_NONE && d.format == GX_TEXFMT_CMPR) {
            u32 bw, bh;
            const u8* bc1 = gx_tex_bc1(map, &bw, &bh);
            if (bc1 != NULL) {
                nt = upload_bc1(bc1, bw, bh);
            }
        }
        if (nt == NULL) {
            const u8* px = gx_tex_pixels(map);
            if (px == NULL) {
                return NULL;
            }
            nt = upload_rgba(px, d.width, d.height, d.width * 4);
        }
        set_gpu_texture(t, nt);
        t.version = d.version;
        t.waiting = d.replace;
        if (d.replace >= 0) {
            gx_texpack_request(d.replace);
        }
    }
    if (t.waiting >= 0) {
        GXPackImage* img = gx_texpack_take(t.waiting);
        if (img != NULL) {
            // w == 0: the PNG could not be loaded; keep the Wii image.
            SDL_GPUTexture* nt = img->w == 0 ? NULL
                                 : img->bc1  ? upload_bc1(img->data.data(), img->w / 4, img->h / 4)
                                             : upload_rgba(img->data.data(), img->w, img->h, img->w * 4);
            if (nt != NULL) {
                set_gpu_texture(t, nt);
            }
            delete img;
            t.waiting = -1;
        }
    }
    t.lastFrame = sFrame;
    return t.tex;
}

// --- recorded draws --------------------------------------------------------------------

struct Draw {
    SDL_GPUGraphicsPipeline* pipe;
    u32 first, count;
    bool wide;        // drawn across the margins too (x compressed by sWideK)
    SDL_Rect scissor; // in EFB pixels of the EFB texture (game x + sMargin)
    SDL_GPUTextureSamplerBinding tex[8];
    GXTevUniforms tev;
};

static std::vector<GXVtxOut> sVerts;
static std::vector<Draw> sDraws;

static void fill_tev(GXTevUniforms* u, u32* usedMaps) {
    u32 stages = gx_bits(gx.bp[0x00], 10, 4) + 1;
    *usedMaps = 0;
    for (int i = 0; i < 16; i++) {
        u->colorEnv[i] = gx.bp[0xC0 + 2 * i];
        u->alphaEnv[i] = gx.bp[0xC1 + 2 * i];
    }
    for (int i = 0; i < 8; i++) {
        u->tevOrder[i] = gx.bp[0x28 + i];
        u->ksel[i] = gx.bp[0xF6 + i];
    }
    for (u32 s = 0; s < stages; s++) {
        u32 order = gx_bits(u->tevOrder[s / 2], (s & 1) * 12, 12);
        if ((order >> 6) & 1) {
            *usedMaps |= 1u << (order & 7);
        }
    }
    // Indirect texturing: the indirect stages' maps (BP 0x27, 6 bits a
    // stage, map in the low 3) are sampled too.
    u32 indStages = gx_bits(gx.bp[0x00], 16, 3);
    for (u32 s = 0; s < indStages && s < 4; s++) {
        *usedMaps |= 1u << gx_bits(gx.bp[0x27], s * 6, 3);
    }
    for (int i = 0; i < 16; i++) {
        u->tevInd[i] = gx.bp[0x10 + i];
    }
    for (int i = 0; i < 12; i++) {
        u->indMtx[i] = i < 9 ? gx.bp[0x06 + i] : 0;
    }
    u->indScale[0] = gx.bp[0x25];
    u->indScale[1] = gx.bp[0x26];
    u->indRef = gx.bp[0x27];
    u->pad2 = 0;
    for (int n = 0; n < 8; n++) {
        u->tcScale[n] = gx_bits(gx.bp[0x30 + 2 * n], 0, 16) | gx_bits(gx.bp[0x31 + 2 * n], 0, 16) << 16;
        u->texImage0[n] = gx_bits(gx.bp[n < 4 ? 0x88 + n : 0xA8 + (n - 4)], 0, 20);
    }
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            u->tevReg[r][c] = gx.tevReg[r][c];
            u->konst[r][c] = gx.konst[r][c];
        }
    }
    u->genMode = gx.bp[0x00];
    u->alphaCompare = gx.bp[0xF3];
    u->texBound = 0;
    u->pad = 0;
}

// --- screen fill ------------------------------------------------------------------------
//
// A screen wider than 16:9 (an iPhone is 19.5:9) shows the game's frame where
// it always was (PortDisplay, port_platform.h) and fills the strips beside it,
// up to 21:9:
// - 3D scenes (the globe): draws whose XF viewport is the whole EFB are drawn
//   into an EFB widened by sMargin on both sides, their clip-space x
//   compressed by sWideK so the frame lands exactly where it was; what lay
//   outside the original view shows in the margins (Earth, stars). Their
//   scissor is widened when it covered the frame's width, else it stays in
//   the frame. This includes 2D (ortho) draws, so the city labels, which are
//   2D panes placed over the globe, continue past the frame edge instead of
//   being cut in mid-screen; the buttons and texts of the layouts end at the
//   frame edge, so they stay where they were (FORECAST_FILL_2D=0 keeps 2D
//   draws in the frame). Untextured 2D draws over the whole frame (fades,
//   dimming) are stretched over the margins instead.
// - 2D screens: the frame's edge columns are stretched over the strips
//   (clamp to edge), darkened toward the screen edge.
// A frame is "3D" when it has a widened perspective draw that no later
// textured 2D draw over the whole frame hides (the forecast pages draw the
// globe, then a full-screen page over it), counted from the last EFB clear;
// else "2D". The present cross-fades between the two over FILL_FADE_FRAMES
// frames, so a scene change does not pop. With a 16:9 window sMargin is 0 and
// none of this runs: the EFB, viewport and scissors are as before.
#define FILL_FADE_FRAMES 8
static bool sFrameWide;  // this frame so far: a widened draw since the last cover / clear
static f32 sWideness;    // as presented: 0 edge extension .. 1 the 3D margins

// Applies the fill to the draw just recorded (its vertices transformed, its
// game scissor left / top / right / bottom, the texture maps its TEV reads
// usedMaps): widens it or keeps it in the frame, and notes what the frame
// looks like.
// --- portrait -----------------------------------------------------------------------------
//
// A window taller than wide (port_platform.h, PortPortrait): the EFB is a
// canvas as large as the window, in EFB pixels at the frame's landscape size
// (360 wide: the 640-pixel frame is 16:9 as high as the window is wide). Each
// frame is either
// - laid out for the canvas (the game called port_portrait_canvas): the frame
//   is centred on the canvas at that size, wider than it, and every draw is
//   mapped there, so whatever the game puts outside the frame (layout parts
//   it moved there, the globe's 3D beyond the old view) lands on the canvas
//   around it. Ortho draws exactly over the frame (a fade, a background no
//   layout moved) are stretched over the whole canvas instead;
// - or letterboxed: the frame as wide as the canvas, centred.
// The mode is decided at the frame's first draw, copy or clear (the game asks
// for it before it draws).
struct FramePlace {
    f32 x, y, w, h; // the frame in canvas pixels
};

static FramePlace frame_place(void) {
    FramePlace f;
    if (sCanvasMode) {
        f.w = (f32)sEfbW;
        f.h = (f32)sEfbH;
    } else {
        f.w = (f32)sCanvasW;
        f.h = (f32)sEfbH * (f32)sCanvasW / (f32)sEfbW;
    }
    f.x = ((f32)sCanvasW - f.w) * 0.5f;
    f.y = ((f32)sCanvasH - f.h) * 0.5f;
    return f;
}

static void frame_open(void) {
    if (!sFrameOpen) {
        sFrameOpen = true;
        bool canvas = port_portrait_take_canvas();
        // Testing: FORECAST_PORTRAIT_CANVAS=n puts every frame from frame n on
        // on the canvas.
        static const char* sForce = SDL_getenv("FORECAST_PORTRAIT_CANVAS");
        if (sForce != NULL && SDL_atoi(sForce) > 0 && sFrame + 1 >= (u32)SDL_atoi(sForce)) {
            canvas = true;
        }
        sCanvasMode = sPortrait && canvas;
    }
}

// A game rectangle (EFB pixels of the frame) in canvas pixels, clipped to the canvas.
static SDL_Rect canvas_rect(const FramePlace& f, f32 left, f32 top, f32 right, f32 bottom) {
    f32 kx = f.w / (f32)sEfbW, ky = f.h / (f32)sEfbH;
    int x0 = (int)SDL_floorf(f.x + left * kx), y0 = (int)SDL_floorf(f.y + top * ky);
    int x1 = (int)SDL_ceilf(f.x + right * kx), y1 = (int)SDL_ceilf(f.y + bottom * ky);
    x0 = SDL_max(x0, 0);
    y0 = SDL_max(y0, 0);
    x1 = SDL_min(x1, (int)sCanvasW);
    y1 = SDL_min(y1, (int)sCanvasH);
    SDL_Rect r = {x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0};
    return r;
}

static void portrait_draw(Draw& d, int left, int top, int right, int bottom) {
    int efbW = (int)sEfbW, efbH = (int)sEfbH;
    FramePlace f = frame_place();
    bool fullScissor = left <= 0 && top <= 0 && right >= efbW && bottom >= efbH;
    bool stretch = false;
    if (sCanvasMode && fullScissor && gx.xfReg[0x26] != 0) {
        f32 x0 = 2.0f, x1 = -2.0f, y0 = 2.0f, y1 = -2.0f;
        const GXVtxOut* v = &sVerts[d.first];
        for (u32 i = 0; i < d.count; i++) {
            x0 = SDL_min(x0, v[i].pos[0]);
            x1 = SDL_max(x1, v[i].pos[0]);
            y0 = SDL_min(y0, v[i].pos[1]);
            y1 = SDL_max(y1, v[i].pos[1]);
        }
        const f32 e = 0.01f;
        stretch = fabsf(x0 + 1.0f) < e && fabsf(x1 - 1.0f) < e && fabsf(y0 + 1.0f) < e && fabsf(y1 - 1.0f) < e;
    }
    d.wide = true;
    if (!stretch) {
        // Frame clip space -> canvas clip space.
        f32 cw = (f32)sCanvasW, ch = (f32)sCanvasH;
        f32 ax = f.w / cw, bx = (2.0f * f.x + f.w) / cw - 1.0f;
        f32 ay = f.h / ch, by = 1.0f - (2.0f * f.y + f.h) / ch;
        GXVtxOut* v = &sVerts[d.first];
        for (u32 i = 0; i < d.count; i++) {
            v[i].pos[0] = v[i].pos[0] * ax + v[i].pos[3] * bx;
            v[i].pos[1] = v[i].pos[1] * ay + v[i].pos[3] * by;
        }
    }
    SDL_Rect sc;
    if (fullScissor && (stretch || sCanvasMode)) {
        sc = {0, 0, (int)sCanvasW, (int)sCanvasH};
    } else {
        sc = canvas_rect(f, (f32)left, (f32)top, (f32)right, (f32)bottom);
    }
    d.scissor = sc;
}

static void fill_draw(Draw& d, int left, int top, int right, int bottom, u32 usedMaps) {
    int efbW = (int)sEfbW, efbH = (int)sEfbH, m = (int)sMargin;
    d.wide = false;
    if (sPortrait) {
        portrait_draw(d, left, top, right, bottom);
        return;
    }
    if (m == 0) {
        d.scissor.x = left;
        d.scissor.w = right > left ? right - left : 0;
        return;
    }
    static int sWiden2D = -1;
    if (sWiden2D < 0) {
        const char* e = SDL_getenv("FORECAST_FILL_2D");
        sWiden2D = e == NULL || e[0] != '0';
    }
    bool ortho = gx.xfReg[0x26] != 0;
    bool fullScissor = left <= 0 && right >= efbW;
    // The XF viewport (as gx_xform.cpp decodes it) spans the EFB width.
    f32 sx = gx_xf_float(0x101A), ox = gx_xf_float(0x101D) - 342.0f;
    bool fullViewport = fabsf(2.0f * sx - (f32)efbW) < 1.0f && fabsf(ox - 0.5f * (f32)efbW) < 1.0f;
    bool stretch = false;
    if (ortho && fullScissor && top <= 0 && bottom >= efbH) {
        f32 x0 = 1.0f, x1 = -1.0f, y0 = 1.0f, y1 = -1.0f;
        const GXVtxOut* v = &sVerts[d.first];
        for (u32 i = 0; i < d.count; i++) {
            x0 = SDL_min(x0, v[i].pos[0]);
            x1 = SDL_max(x1, v[i].pos[0]);
            y0 = SDL_min(y0, v[i].pos[1]);
            y1 = SDL_max(y1, v[i].pos[1]);
        }
        const f32 e = 0.995f;
        if (x0 <= -e && x1 >= e && y0 <= -e && y1 >= e) {
            if (usedMaps == 0) {
                // Untextured over the whole frame: a fade or dimming layer.
                // Stretched over the margins too (it is one colour, or a
                // gradient), so the 3D margins fade with the frame.
                stretch = true;
            } else {
                // A 2D picture over the whole frame (a page's background)
                // hides the 3D under it: edge extension.
                sFrameWide = false;
            }
        }
    }
    if (stretch) {
        d.wide = true;
    } else if ((!ortho || sWiden2D) && fullViewport) {
        d.wide = true;
        GXVtxOut* v = &sVerts[d.first];
        for (u32 i = 0; i < d.count; i++) {
            v[i].pos[0] *= sWideK;
        }
        if (!ortho) {
            sFrameWide = true;
        }
    }
    if (d.wide && fullScissor) {
        d.scissor.x = 0;
        d.scissor.w = efbW + 2 * m;
    } else {
        d.scissor.x = left + m;
        d.scissor.w = right > left ? right - left : 0;
    }
}

// Adds the time from construction to destruction to a port_perf_add bucket.
struct PerfScope {
    int bucket;
    Uint64 start;
    explicit PerfScope(int b) : bucket(b), start(SDL_GetTicksNS()) {}
    ~PerfScope() { port_perf_add(bucket, SDL_GetTicksNS() - start); }
};

void gx_draw(u32 primitive, const GXVertexIn* verts, u32 count) {
    PerfScope perf(PORT_PERF_RECORD);
    if (gx_bits(gx.bp[0x00], 14, 2) == 3) {
        return; // cull all
    }
    frame_open();
    Draw d;
    d.first = (u32)sVerts.size();
    d.count = gx_xform_primitive(primitive, verts, count, sVerts);
    if (d.count == 0) {
        return;
    }
    u32 used;
    fill_tev(&d.tev, &used);
    for (int m = 0; m < 8; m++) {
        GXTexDesc td;
        if (used & (1u << m)) {
            u32 b = m < 4 ? 0x80 + m : 0xA0 + (m - 4);
            auto copy = sEfbCopies.find(gx_bits(gx.bp[b + 0x14], 0, 24) << 5);
            if (copy != sEfbCopies.end()) {
                u32 mode0 = gx.bp[b];
                GXTexDesc cd = {};
                cd.wrapS = (u8)gx_bits(mode0, 0, 2);
                cd.wrapT = (u8)gx_bits(mode0, 2, 2);
                cd.magLinear = cd.minLinear = (u8)((mode0 >> 4) & 1);
                d.tex[m].texture = copy->second.tex;
                d.tex[m].sampler = get_sampler(cd);
                d.tev.texBound |= 1u << m;
                continue;
            }
        }
        if ((used & (1u << m)) && gx_tex_get(m, &td)) {
            SDL_GPUTexture* t = get_texture(m, td);
            if (t != NULL) {
                d.tex[m].texture = t;
                d.tex[m].sampler = get_sampler(td);
                d.tev.texBound |= 1u << m;
                continue;
            }
        }
        GXTexDesc white = {};
        d.tex[m].texture = sWhite;
        d.tex[m].sampler = get_sampler(white);
    }

    // The pixel shader: the specialized one for this TEV state once it is
    // ready (and its pipeline exists), else the ubershader.
    PipeKey pk = pipe_key();
    SDL_GPUShader* specPS = NULL;
    u32 specId = gx_spec_lookup(d.tev, &specPS);
    d.pipe = specPS != NULL ? get_pipeline(pk, specId, specPS) : NULL;
    if (specId != 0) {
        gx_spec_count_draw(specId, d.pipe != NULL);
    }
    if (d.pipe == NULL) {
        d.pipe = get_pipeline(pk);
    }
    if (d.pipe == NULL) {
        sVerts.resize(d.first);
        return;
    }

    // Scissor: BP 0x20 top (bits 0-10) / left (12-22), 0x21 bottom / right,
    // all + 342 and inclusive.
    int top = (int)gx_bits(gx.bp[0x20], 0, 11) - 342;
    int left = (int)gx_bits(gx.bp[0x20], 12, 11) - 342;
    int bottom = (int)gx_bits(gx.bp[0x21], 0, 11) - 342 + 1;
    int right = (int)gx_bits(gx.bp[0x21], 12, 11) - 342 + 1;
    // Portrait: scissors may start above or left of the frame (on the
    // canvas around it); portrait_draw clips them to the canvas.
    if (left < 0 && !sPortrait) {
        left = 0;
    }
    if (top < 0 && !sPortrait) {
        top = 0;
    }
    d.scissor.y = top;
    d.scissor.h = bottom > top ? bottom - top : 0;
    fill_draw(d, left, top, right, bottom, used); // sets the scissor's x and w

    if (gx_bits(gx.bp[0xF1], 21, 3) != 0) {
        WARN_ONCE("gx: fog not supported yet\n");
    }
    if (gx_bits(gx.bp[0xF5], 0, 2) != 0) {
        WARN_ONCE("gx: z textures not supported\n");
    }
    // Debugging: FORECAST_LOG_DRAWS=n logs every draw of frame n.
    static const char* sLogFrame = SDL_getenv("FORECAST_LOG_DRAWS");
    if (sLogFrame != NULL && (u32)SDL_atoi(sLogFrame) == sFrame + 1) {
        const GXVtxOut& v0 = sVerts[d.first];
        port_log("draw %u: prim %02x n %u pipe %08x stages %u tex %02x/%02x chans %u gens %u "
                 "clr0 %.2f %.2f %.2f %.2f pos %.3f %.3f %.3f %.3f\n",
                 (unsigned)sDraws.size(), (unsigned)primitive, (unsigned)count, (unsigned)pipe_key().bits,
                 (unsigned)(gx_bits(gx.bp[0x00], 10, 4) + 1), (unsigned)used, (unsigned)d.tev.texBound,
                 (unsigned)(gx.xfReg[0x09] & 3), (unsigned)(gx.xfReg[0x3F] & 15), v0.clr[0][0], v0.clr[0][1],
                 v0.clr[0][2], v0.clr[0][3], v0.pos[0], v0.pos[1], v0.pos[2], v0.pos[3]);
        for (u32 s = 0; s <= gx_bits(gx.bp[0x00], 10, 4); s++) {
            port_log("    stage %u: order %03x color %06x alpha %06x ksel %06x ind %06x\n", (unsigned)s,
                     (unsigned)gx_bits(d.tev.tevOrder[s / 2], (s & 1) * 12, 12), (unsigned)d.tev.colorEnv[s],
                     (unsigned)d.tev.alphaEnv[s], (unsigned)d.tev.ksel[s / 2], (unsigned)d.tev.tevInd[s]);
        }
        if (gx_bits(gx.bp[0x00], 16, 3) != 0) {
            port_log("    ind stages %u: ref %06x scale %06x %06x mtx0 %06x %06x %06x\n",
                     (unsigned)gx_bits(gx.bp[0x00], 16, 3), (unsigned)d.tev.indRef, (unsigned)d.tev.indScale[0],
                     (unsigned)d.tev.indScale[1], (unsigned)d.tev.indMtx[0], (unsigned)d.tev.indMtx[1],
                     (unsigned)d.tev.indMtx[2]);
        }
        port_log("    regs prev %d %d %d %d c0 %d %d %d %d c1 %d %d %d %d k0 %d %d %d %d k1 %d %d %d %d "
                 "chan0 ctrl %06x/%06x mat %08x amb %08x\n",
                 d.tev.tevReg[0][0], d.tev.tevReg[0][1], d.tev.tevReg[0][2], d.tev.tevReg[0][3], d.tev.tevReg[1][0],
                 d.tev.tevReg[1][1], d.tev.tevReg[1][2], d.tev.tevReg[1][3], d.tev.tevReg[2][0], d.tev.tevReg[2][1],
                 d.tev.tevReg[2][2], d.tev.tevReg[2][3], d.tev.konst[0][0], d.tev.konst[0][1], d.tev.konst[0][2],
                 d.tev.konst[0][3], d.tev.konst[1][0], d.tev.konst[1][1], d.tev.konst[1][2], d.tev.konst[1][3],
                 (unsigned)gx.xfReg[0x0E], (unsigned)gx.xfReg[0x10], (unsigned)gx.xfReg[0x0C],
                 (unsigned)gx.xfReg[0x0A]);
    }
    sDraws.push_back(d);
}

// --- frames ---------------------------------------------------------------------------

static void ensure_efb(void) {
    u32 w, h;
    gx_efb_size(&w, &h);
    // iOS: render at the largest whole multiple of the Wii's EFB that fits the
    // window's height (the frame is then scaled up a little). Covering the
    // window instead rendered far more pixels than are shown: 3x on an
    // iPhone 15 Pro (1920x1368 for a 1179-pixel-high screen) left it GPU-bound
    // at ~30 fps. FORECAST_EFB_SCALE=n overrides it.
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(port_window(), &pw, &ph);
    {
        // The laid-out screen's pixels (all of the window, unless it has
        // turned and the game has not rebuilt its screens yet).
        PortDisplay dp;
        port_display_layout((f32)pw, (f32)ph, &dp);
        if (dp.vw > 0.0f && dp.vh > 0.0f) {
            pw = (int)dp.vw;
            ph = (int)dp.vh;
        }
    }
#ifdef SDL_PLATFORM_IOS
    u32 scale = ph > 0 ? (u32)ph / h : 2;
#else
    // Desktop GPUs have room to spare: cover the window.
    u32 scale = ph > 0 ? (u32)((ph + h - 1) / h) : 2;
#endif
    const char* forced = SDL_getenv("FORECAST_EFB_SCALE");
    if (forced != NULL && SDL_atoi(forced) > 0) {
        scale = (u32)SDL_atoi(forced);
    }
    if (scale < 1) {
        scale = 1;
    }
    if (scale > 4) {
        scale = 4;
    }
    // Screen fill: margins as wide as the wider strip beside the frame, in
    // EFB pixels (the frame shows w EFB pixels across d.w).
    u32 margin = 0;
    int ww = 0, wh = 0;
    SDL_GetWindowSize(port_window(), &ww, &wh);
    PortDisplay d;
    port_display_layout((f32)ww, (f32)wh, &d);
    if (d.portrait) {
        // The canvas: the frame (w EFB pixels across 16:9 of the window's
        // width) and the screen around it, in EFB pixels, for the shape the
        // game's screens are laid out for.
        int lw = 0, lh = 0;
        port_layout_size(&lw, &lh);
        f32 fw = (f32)lw * (16.0f / 9.0f), fh = (f32)lw;
        u32 cw = (u32)SDL_lroundf((f32)w * (f32)lw / fw), ch = (u32)SDL_ceilf((f32)h * (f32)lh / fh);
        // As many pixels as the landscape EFB at most: a 3x canvas on an
        // iPhone (1080x2967) would be past what kept the GPU at 60 fps.
#ifdef SDL_PLATFORM_IOS
        u32 pscale = pw > 0 ? (u32)pw / cw : 2;
#else
        u32 pscale = pw > 0 ? (u32)((pw + cw - 1) / cw) : 2;
#endif
        while (pscale > 1 && cw * ch * pscale * pscale > w * (u32)(h * 21 / 16) * 4 * 1u) {
            pscale--;
        }
        if (forced != NULL && SDL_atoi(forced) > 0) {
            pscale = SDL_min((u32)SDL_atoi(forced), 4u);
        }
        pscale = SDL_max(pscale, 1u);
        if (sEfbColor != NULL && sPortrait && w == sEfbW && h == sEfbH && pscale == sScale && cw == sCanvasW &&
            ch == sCanvasH) {
            return;
        }
        if (sEfbColor != NULL) {
            SDL_ReleaseGPUTexture(sDev, sEfbColor);
            SDL_ReleaseGPUTexture(sDev, sEfbDepth);
        }
        sPortrait = true;
        sEfbW = w;
        sEfbH = h;
        sScale = pscale;
        sMargin = 0;
        sWideK = 1.0f;
        sCanvasW = cw;
        sCanvasH = ch;
        sEfbColor = create_texture(cw * pscale, ch * pscale, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                   SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
        sEfbDepth = create_texture(cw * pscale, ch * pscale, sDepthFmt, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
        sClearNext = true;
        port_log("gx: portrait canvas %lux%lu at %lux (%lux%lu) around the %lux%lu frame\n", (unsigned long)cw,
                 (unsigned long)ch, (unsigned long)pscale, (unsigned long)(cw * pscale),
                 (unsigned long)(ch * pscale), (unsigned long)w, (unsigned long)h);
        return;
    }
    if (d.w > 0.0f) {
        f32 side = SDL_max(d.fillLeft, d.fillRight);
        margin = (u32)SDL_ceilf(side / d.w * (f32)w);
        // 21:9 at most (port_display_layout caps the fill there already).
        u32 maxMargin = (u32)SDL_ceilf((f32)w * (21.0f / 16.0f - 1.0f) * 0.5f);
        if (margin > maxMargin) {
            margin = maxMargin;
        }
    }
    if (sEfbColor != NULL && !sPortrait && w == sEfbW && h == sEfbH && scale == sScale && margin == sMargin) {
        return;
    }
    if (sEfbColor != NULL) {
        SDL_ReleaseGPUTexture(sDev, sEfbColor);
        SDL_ReleaseGPUTexture(sDev, sEfbDepth);
    }
    sPortrait = false;
    sCanvasMode = false;
    sEfbW = w;
    sEfbH = h;
    sScale = scale;
    sMargin = margin;
    sWideK = (f32)w / (f32)(w + 2 * margin);
    sCanvasW = w + 2 * margin;
    sCanvasH = h;
    u32 tw = (w + 2 * margin) * scale;
    sEfbColor = create_texture(tw, h * scale, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                               SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    sEfbDepth = create_texture(tw, h * scale, sDepthFmt, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
    sClearNext = true;
    port_log("gx: EFB %lux%lu at %lux (%lux%lu), screen fill margins %lu\n", (unsigned long)w, (unsigned long)h,
             (unsigned long)scale, (unsigned long)tw, (unsigned long)(h * scale), (unsigned long)margin);
}

static void upload(SDL_GPUCommandBuffer* cb) {
    u32 vtxBytes = (u32)(sVerts.size() * sizeof(GXVtxOut));
    u32 texBytes = (u32)sUploadBytes.size();
    if (vtxBytes == 0 && texBytes == 0) {
        return;
    }
    if (vtxBytes > sVtxBufSize) {
        if (sVtxBuf != NULL) {
            SDL_ReleaseGPUBuffer(sDev, sVtxBuf);
        }
        sVtxBufSize = vtxBytes * 2;
        SDL_GPUBufferCreateInfo bi = {};
        bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        bi.size = sVtxBufSize;
        sVtxBuf = SDL_CreateGPUBuffer(sDev, &bi);
    }
    u32 total = vtxBytes + texBytes;
    if (total > sXferSize) {
        if (sXfer != NULL) {
            SDL_ReleaseGPUTransferBuffer(sDev, sXfer);
        }
        sXferSize = total * 2;
        SDL_GPUTransferBufferCreateInfo ti = {};
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        ti.size = sXferSize;
        sXfer = SDL_CreateGPUTransferBuffer(sDev, &ti);
    }
    u8* map = (u8*)SDL_MapGPUTransferBuffer(sDev, sXfer, true);
    if (map == NULL) {
        port_log("gx: map transfer buffer failed: %s\n", SDL_GetError());
        return;
    }
    memcpy(map, sVerts.data(), vtxBytes);
    if (texBytes != 0) {
        memcpy(map + vtxBytes, sUploadBytes.data(), texBytes);
    }
    SDL_UnmapGPUTransferBuffer(sDev, sXfer);

    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
    if (vtxBytes != 0) {
        SDL_GPUTransferBufferLocation src = {sXfer, 0};
        SDL_GPUBufferRegion dst = {sVtxBuf, 0, vtxBytes};
        SDL_UploadToGPUBuffer(cp, &src, &dst, true);
    }
    for (const PendingUpload& u : sUploads) {
        SDL_GPUTextureTransferInfo src = {};
        src.transfer_buffer = sXfer;
        src.offset = vtxBytes + u.offset;
        src.pixels_per_row = u.w;
        src.rows_per_layer = u.h;
        SDL_GPUTextureRegion dst = {};
        dst.texture = u.tex;
        dst.w = u.w;
        dst.h = u.h;
        dst.d = 1;
        SDL_UploadToGPUTexture(cp, &src, &dst, false);
    }
    SDL_EndGPUCopyPass(cp);
}

static SDL_GPUCommandBuffer* get_cmd(void) {
    if (sCmd == NULL) {
        sCmd = SDL_AcquireGPUCommandBuffer(sDev);
        if (sCmd == NULL) {
            port_log("gx: no command buffer: %s\n", SDL_GetError());
        }
    }
    return sCmd;
}

static void render(SDL_GPUCommandBuffer* cb);

static u32 sFrameDraws, sFrameVerts, sFrameUploads;

// Runs the draws recorded so far into the EFB.
static void flush(void) {
    if (sDraws.empty() && sUploads.empty() && !sClearNext) {
        return;
    }
    SDL_GPUCommandBuffer* cb = get_cmd();
    if (cb != NULL) {
        upload(cb);
        render(cb);
    }
    sFrameDraws += (u32)sDraws.size();
    sFrameVerts += (u32)sVerts.size();
    sFrameUploads += (u32)sUploads.size();
    sVerts.clear();
    sDraws.clear();
    sUploads.clear();
    sUploadBytes.clear();
}

static void render(SDL_GPUCommandBuffer* cb) {
    SDL_GPUColorTargetInfo ct = {};
    ct.texture = sEfbColor;
    ct.load_op = sClearNext ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    ct.clear_color = sClearColor;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo dt = {};
    dt.texture = sEfbDepth;
    dt.load_op = sClearNext ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    dt.clear_depth = sClearDepth;
    dt.store_op = SDL_GPU_STOREOP_STORE;
    dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    sClearNext = false;

    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cb, &ct, 1, &dt);
    // The game's frame, and with the screen fill the whole widened EFB.
    const SDL_GPUViewport frameVp = {(f32)(sMargin * sScale), 0.0f, (f32)(sEfbW * sScale), (f32)(sEfbH * sScale),
                                     0.0f, 1.0f};
    const SDL_GPUViewport wideVp = {0.0f, 0.0f, (f32)(sCanvasW * sScale), (f32)(sCanvasH * sScale), 0.0f, 1.0f};
    SDL_SetGPUViewport(rp, &frameVp);
    bool wideVpSet = false;
    SDL_GPUBufferBinding vb = {sVtxBuf, 0};
    if (sVtxBuf != NULL) {
        SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
    }
    SDL_GPUGraphicsPipeline* bound = NULL;
    for (const Draw& d : sDraws) {
        SDL_Rect sc = {d.scissor.x * (int)sScale, d.scissor.y * (int)sScale, d.scissor.w * (int)sScale,
                       d.scissor.h * (int)sScale};
        // Right edge: the frame's, or the widened EFB's for widened draws.
        int maxW = (int)((d.wide ? sCanvasW : sEfbW + sMargin) * sScale), maxH = (int)(sCanvasH * sScale);
        if (sc.x + sc.w > maxW) {
            sc.w = maxW - sc.x;
        }
        if (sc.y + sc.h > maxH) {
            sc.h = maxH - sc.y;
        }
        if (sc.w <= 0 || sc.h <= 0) {
            continue;
        }
        if (d.pipe != bound) {
            SDL_BindGPUGraphicsPipeline(rp, d.pipe);
            bound = d.pipe;
        }
        if (d.wide != wideVpSet) {
            SDL_SetGPUViewport(rp, d.wide ? &wideVp : &frameVp);
            wideVpSet = d.wide;
        }
        SDL_SetGPUScissor(rp, &sc);
        SDL_BindGPUFragmentSamplers(rp, 0, d.tex, 8);
        SDL_PushGPUFragmentUniformData(cb, 0, &d.tev, sizeof(d.tev));
        SDL_DrawGPUPrimitives(rp, d.count, 1, d.first, 0);
    }
    SDL_EndGPURenderPass(rp);
}

static Uint64 sAcquireNS; // this frame's wait for the window's image
static Uint32 sSwapW, sSwapH; // the window's image, last acquired

// --- present ---------------------------------------------------------------------------
//
// Without the screen fill the EFB is blitted into the frame (as always).
// With it, gx_present_ps.hlsl draws textured quads (vertex colour times the
// EFB): the frame, and in each strip beside it the frame's edge column
// stretched (darker toward the screen edge) and / or the EFB's margin with
// the 3D scene, the latter blended in by sWideness.

// The edge extension's colour at the screen edge (1 at the frame).
#define FILL_EDGE_SHADE 0.8f

static SDL_GPUShader* sPresentPS;
static SDL_GPUSampler* sPresentSmp;
static std::unordered_map<int, SDL_GPUGraphicsPipeline*> sPresentPipes; // by target format
static SDL_GPUBuffer* sPresentBuf;
static SDL_GPUTransferBuffer* sPresentXfer;
enum { PRESENT_MAX_VERTS = 5 * 6 };

static SDL_GPUGraphicsPipeline* present_pipeline(SDL_GPUTextureFormat fmt) {
    if (sPresentPS == NULL) {
        return NULL;
    }
    auto it = sPresentPipes.find((int)fmt);
    if (it != sPresentPipes.end()) {
        return it->second;
    }
    SDL_GPUVertexBufferDescription vb;
    SDL_GPUVertexAttribute attrs[11];
    vertex_input(&vb, attrs);
    SDL_GPUColorTargetDescription ct = {};
    ct.format = fmt;
    SDL_GPUColorTargetBlendState& bs = ct.blend_state;
    // Colour only: the window's alpha stays as cleared (opaque).
    bs.enable_color_write_mask = true;
    bs.color_write_mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B;
    bs.enable_blend = true;
    bs.color_blend_op = bs.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    bs.src_color_blendfactor = bs.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    bs.dst_color_blendfactor = bs.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    SDL_GPUGraphicsPipelineCreateInfo pi = {};
    pi.vertex_shader = sVS;
    pi.fragment_shader = sPresentPS;
    pi.vertex_input_state.vertex_buffer_descriptions = &vb;
    pi.vertex_input_state.num_vertex_buffers = 1;
    pi.vertex_input_state.vertex_attributes = attrs;
    pi.vertex_input_state.num_vertex_attributes = 11;
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pi.target_info.color_target_descriptions = &ct;
    pi.target_info.num_color_targets = 1;
    SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(sDev, &pi);
    if (p == NULL) {
        port_log("gx: present pipeline (format %d) failed: %s\n", (int)fmt, SDL_GetError());
    }
    sPresentPipes[(int)fmt] = p;
    return p;
}

// A quad from target pixels (x0, y0)-(x1, y1), EFB texture coordinates
// (u0, v0)-(u1, v1) with u clamped to [umin, umax], colour cl on the left
// edge and cr on the right, alpha a.
static void present_quad(GXVtxOut* out, f32 tw, f32 th, f32 x0, f32 y0, f32 x1, f32 y1, f32 u0, f32 v0, f32 u1,
                         f32 v1, f32 umin, f32 umax, f32 cl, f32 cr, f32 a) {
    GXVtxOut c[4];
    memset(c, 0, sizeof(c));
    const f32 xs[4] = {x0, x1, x1, x0}, ys[4] = {y0, y0, y1, y1};
    const f32 us[4] = {u0, u1, u1, u0}, vs[4] = {v0, v0, v1, v1};
    const f32 cs[4] = {cl, cr, cr, cl};
    for (int i = 0; i < 4; i++) {
        c[i].pos[0] = xs[i] / tw * 2.0f - 1.0f;
        c[i].pos[1] = 1.0f - ys[i] / th * 2.0f;
        c[i].pos[2] = 0.0f;
        c[i].pos[3] = 1.0f;
        c[i].clr[0][0] = c[i].clr[0][1] = c[i].clr[0][2] = cs[i];
        c[i].clr[0][3] = a;
        c[i].tex[0][0] = us[i];
        c[i].tex[0][1] = vs[i];
        c[i].tex[0][2] = 1.0f;
        c[i].tex[1][0] = umin;
        c[i].tex[1][1] = umax;
    }
    static const int idx[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; i++) {
        out[i] = c[idx[i]];
    }
}

// Draws the EFB onto target (tw x th pixels, format fmt), cleared to black.
static void compose(SDL_GPUCommandBuffer* cb, SDL_GPUTexture* target, Uint32 tw, Uint32 th,
                    SDL_GPUTextureFormat fmt) {
    PortDisplay d;
    port_display_layout((f32)tw, (f32)th, &d);
    SDL_GPUColorTargetInfo ct = {};
    ct.texture = target;
    ct.load_op = SDL_GPU_LOADOP_CLEAR;
    ct.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
    ct.store_op = SDL_GPU_STOREOP_STORE;
    if (sPortrait) {
        // The canvas is the screen.
        SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cb, &ct, 1, NULL));
        SDL_GPUBlitInfo bi = {};
        bi.source.texture = sEfbColor;
        bi.source.w = sCanvasW * sScale;
        bi.source.h = sCanvasH * sScale;
        bi.destination.texture = target;
        bi.destination.x = (Uint32)d.vx;
        bi.destination.y = (Uint32)d.vy;
        bi.destination.w = (Uint32)d.vw;
        bi.destination.h = (Uint32)d.vh;
        bi.load_op = SDL_GPU_LOADOP_LOAD;
        bi.filter = SDL_GPU_FILTER_LINEAR;
        SDL_BlitGPUTexture(cb, &bi);
        return;
    }
    bool fill = sMargin != 0 && (d.fillLeft > 0.0f || d.fillRight > 0.0f);
    SDL_GPUGraphicsPipeline* pipe = fill ? present_pipeline(fmt) : NULL;
    if (pipe == NULL) {
        // Clear to black, then scale the EFB's frame into the 16:9 rectangle.
        SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cb, &ct, 1, NULL));
        SDL_GPUBlitInfo bi = {};
        bi.source.texture = sEfbColor;
        bi.source.x = sMargin * sScale;
        bi.source.w = sEfbW * sScale;
        bi.source.h = sEfbH * sScale;
        bi.destination.texture = target;
        bi.destination.x = (Uint32)d.x;
        bi.destination.y = (Uint32)d.y;
        bi.destination.w = (Uint32)d.w;
        bi.destination.h = (Uint32)d.h;
        bi.load_op = SDL_GPU_LOADOP_LOAD;
        bi.filter = SDL_GPU_FILTER_LINEAR;
        SDL_BlitGPUTexture(cb, &bi);
        return;
    }

    // EFB texture coordinates: the frame spans [uf0, uf1]; a strip of width
    // s pixels beside it shows s / d.w * sEfbW EFB pixels of the margin.
    f32 W = (f32)(sEfbW + 2 * sMargin), TW = W * (f32)sScale;
    f32 uf0 = (f32)sMargin / W, uf1 = (f32)(sMargin + sEfbW) / W;
    f32 uEdge0 = ((f32)(sMargin * sScale) + 0.5f) / TW, uEdge1 = ((f32)((sMargin + sEfbW) * sScale) - 0.5f) / TW;
    f32 fw = (f32)tw, fh = (f32)th;
    f32 x0 = d.x, x1 = d.x + d.w, y0 = d.y, y1 = d.y + d.h;
    f32 lx = x0 - d.fillLeft, rx = x1 + d.fillRight;
    f32 ul = uf0 - d.fillLeft / d.w * (f32)sEfbW / W, ur = uf1 + d.fillRight / d.w * (f32)sEfbW / W;
    GXVtxOut v[PRESENT_MAX_VERTS];
    u32 n = 0;
    present_quad(&v[n], fw, fh, x0, y0, x1, y1, uf0, 0.0f, uf1, 1.0f, uEdge0, uEdge1, 1.0f, 1.0f, 1.0f);
    n += 6;
    f32 wide = sWideness;
    if (wide < 1.0f) {
        if (d.fillLeft > 0.0f) {
            present_quad(&v[n], fw, fh, lx, y0, x0, y1, uEdge0, 0.0f, uEdge0, 1.0f, 0.0f, 1.0f, FILL_EDGE_SHADE, 1.0f,
                         1.0f);
            n += 6;
        }
        if (d.fillRight > 0.0f) {
            present_quad(&v[n], fw, fh, x1, y0, rx, y1, uEdge1, 0.0f, uEdge1, 1.0f, 0.0f, 1.0f, 1.0f, FILL_EDGE_SHADE,
                         1.0f);
            n += 6;
        }
    }
    if (wide > 0.0f) {
        if (d.fillLeft > 0.0f) {
            present_quad(&v[n], fw, fh, lx, y0, x0, y1, ul, 0.0f, uf0, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, wide);
            n += 6;
        }
        if (d.fillRight > 0.0f) {
            present_quad(&v[n], fw, fh, x1, y0, rx, y1, uf1, 0.0f, ur, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, wide);
            n += 6;
        }
    }

    u32 bytes = n * (u32)sizeof(GXVtxOut);
    void* map = SDL_MapGPUTransferBuffer(sDev, sPresentXfer, true);
    if (map == NULL) {
        return;
    }
    memcpy(map, v, bytes);
    SDL_UnmapGPUTransferBuffer(sDev, sPresentXfer);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
    SDL_GPUTransferBufferLocation src = {sPresentXfer, 0};
    SDL_GPUBufferRegion dst = {sPresentBuf, 0, bytes};
    SDL_UploadToGPUBuffer(cp, &src, &dst, true);
    SDL_EndGPUCopyPass(cp);

    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cb, &ct, 1, NULL);
    SDL_BindGPUGraphicsPipeline(rp, pipe);
    SDL_GPUBufferBinding vb = {sPresentBuf, 0};
    SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
    SDL_GPUTextureSamplerBinding tb = {sEfbColor, sPresentSmp};
    SDL_BindGPUFragmentSamplers(rp, 0, &tb, 1);
    SDL_DrawGPUPrimitives(rp, n, 1, 0, 0);
    SDL_EndGPURenderPass(rp);
}

static void present(SDL_GPUCommandBuffer* cb) {
    SDL_GPUTexture* sw = NULL;
    Uint32 sw_w = 0, sw_h = 0;
    Uint64 waitStart = SDL_GetTicksNS();
    bool ok = SDL_WaitAndAcquireGPUSwapchainTexture(cb, port_window(), &sw, &sw_w, &sw_h);
    Uint64 waited = SDL_GetTicksNS() - waitStart;
    port_perf_add(PORT_PERF_ACQUIRE, waited);
    sAcquireNS += waited;
    if (!ok || sw == NULL) {
        return; // minimized
    }
    sSwapW = sw_w;
    sSwapH = sw_h;
    compose(cb, sw, sw_w, sw_h, SDL_GetGPUSwapchainTextureFormat(sDev, port_window()));
}

// Debugging: FORECAST_DUMP_FRAME=n[,m...] saves the EFB of those frames as
// efb_<n>.bmp in the working directory: the game's 640x456 frame (times the
// scale), without the screen fill's margins. With the fill, screen_<n>.bmp is
// what the window shows (the frame and the strips beside it).
static bool dump_requested(u32 frame) {
    static const char* sList = SDL_getenv("FORECAST_DUMP_FRAME");
    if (sList == NULL) {
        return false;
    }
    for (const char* p = sList; *p != '\0';) {
        char* end;
        unsigned long n = strtoul(p, &end, 10);
        if (end == p) {
            break;
        }
        if (n == frame) {
            return true;
        }
        p = *end == ',' ? end + 1 : end;
    }
    return false;
}

static void save_bmp(SDL_GPUTransferBuffer* tb, u32 w, u32 h, const char* name) {
    void* px = SDL_MapGPUTransferBuffer(sDev, tb, false);
    SDL_Surface* s = px != NULL ? SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_ABGR8888, px, (int)(w * 4))
                                : NULL;
    if (s == NULL || !SDL_SaveBMP(s, name)) {
        port_log("gx: dump %s failed: %s\n", name, SDL_GetError());
    } else {
        port_log("gx: dumped %s\n", name);
    }
    SDL_DestroySurface(s);
    if (px != NULL) {
        SDL_UnmapGPUTransferBuffer(sDev, tb);
    }
}

static SDL_GPUTransferBuffer* download(SDL_GPUCopyPass* cp, SDL_GPUTexture* tex, u32 x, u32 w, u32 h) {
    SDL_GPUTransferBufferCreateInfo ti = {};
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    ti.size = w * h * 4;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(sDev, &ti);
    SDL_GPUTextureRegion src = {};
    src.texture = tex;
    src.x = x;
    src.w = w;
    src.h = h;
    src.d = 1;
    SDL_GPUTextureTransferInfo dst = {};
    dst.transfer_buffer = tb;
    SDL_DownloadFromGPUTexture(cp, &src, &dst);
    return tb;
}

static void dump_efb(SDL_GPUCommandBuffer* cb, u32 frame) {
    u32 w = sEfbW * sScale, h = sEfbH * sScale, x = sMargin * sScale;
    if (sPortrait) {
        // Portrait: the whole canvas (what the window shows).
        w = sCanvasW * sScale;
        h = sCanvasH * sScale;
        x = 0;
    }
    // The screen as the window shows it (its last size, else the window's).
    SDL_GPUTexture* screen = NULL;
    u32 sw = sSwapW, sh = sSwapH;
    if (sMargin != 0) {
        if (sw == 0 || sh == 0) {
            int pw = 0, ph = 0;
            SDL_GetWindowSizeInPixels(port_window(), &pw, &ph);
            sw = (u32)pw;
            sh = (u32)ph;
        }
        if (sw > 0 && sh > 0) {
            screen = create_texture(sw, sh, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                    SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
        }
        if (screen == NULL) {
            port_log("gx: no screen dump (%lux%lu): %s\n", (unsigned long)sw, (unsigned long)sh, SDL_GetError());
        }
        if (screen != NULL) {
            compose(cb, screen, sw, sh, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
        }
    }
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
    SDL_GPUTransferBuffer* tb = download(cp, sEfbColor, x, w, h);
    SDL_GPUTransferBuffer* sb = screen != NULL ? download(cp, screen, 0, sw, sh) : NULL;
    SDL_EndGPUCopyPass(cp);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
    SDL_WaitForGPUFences(sDev, true, &fence, 1);
    SDL_ReleaseGPUFence(sDev, fence);
    char name[64];
    SDL_snprintf(name, sizeof(name), "efb_%u.bmp", (unsigned)frame);
    save_bmp(tb, w, h, name);
    SDL_ReleaseGPUTransferBuffer(sDev, tb);
    if (sb != NULL) {
        SDL_snprintf(name, sizeof(name), "screen_%u.bmp", (unsigned)frame);
        save_bmp(sb, sw, sh, name);
        SDL_ReleaseGPUTransferBuffer(sDev, sb);
    }
    if (screen != NULL) {
        SDL_ReleaseGPUTexture(sDev, screen);
    }
}

// Testing: FORECAST_GPU_TIME=1 waits for each frame's command buffer and
// logs the average time from submitting it to its completion (the GPU's
// work for the frame, as the CPU no longer overlaps it) every 120 frames.
static bool sGpuTime;
static Uint64 sGpuNS, sGpuMaxNS;
static u32 sGpuFrames;

static void submit_timed(SDL_GPUCommandBuffer* cb) {
    Uint64 t0 = SDL_GetTicksNS();
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
    if (fence != NULL) {
        SDL_WaitForGPUFences(sDev, true, &fence, 1);
        SDL_ReleaseGPUFence(sDev, fence);
    }
    Uint64 ns = SDL_GetTicksNS() - t0;
    sGpuNS += ns;
    if (ns > sGpuMaxNS) {
        sGpuMaxNS = ns;
    }
    if (++sGpuFrames == 120) {
        port_log("gx: gpu frame %lu: %.3f ms average, %.3f ms max (submit to completion, %u frames)\n",
                 (unsigned long)sFrame + 1, (double)sGpuNS / sGpuFrames / 1e6, (double)sGpuMaxNS / 1e6,
                 (unsigned)sGpuFrames);
        sGpuNS = sGpuMaxNS = 0;
        sGpuFrames = 0;
    }
}

// Pre-creates a pipeline remembered from an earlier run (gx_spec_frame_end).
static bool make_spec_pipe(u32 id, SDL_GPUShader* ps, u32 bits) {
    PipeKey k;
    k.bits = bits;
    get_pipeline(k, id, ps, false);
    return true;
}

extern "C" void port_gx_present(void) {
    // Submit time, without the wait for the window's image (reported apart).
    Uint64 submitStart = SDL_GetTicksNS();
    sAcquireNS = 0;
    struct SubmitTimer {
        Uint64 start;
        ~SubmitTimer() { port_perf_add(PORT_PERF_SUBMIT, SDL_GetTicksNS() - start - sAcquireNS); }
    } submitTimer{submitStart};
    flush();
    // Screen fill: move toward this frame's look (3D margins or edge
    // extension) over FILL_FADE_FRAMES frames.
    f32 target = sFrameWide ? 1.0f : 0.0f;
    if (sWideness < target) {
        sWideness = SDL_min(target, sWideness + 1.0f / FILL_FADE_FRAMES);
    } else if (sWideness > target) {
        sWideness = SDL_max(target, sWideness - 1.0f / FILL_FADE_FRAMES);
    }
    static const char* sLogFill = SDL_getenv("FORECAST_LOG_FILL");
    static bool sLastWide;
    if (sLogFill != NULL && sMargin != 0 && sFrameWide != sLastWide) {
        port_log("gx: frame %lu: screen fill %s\n", (unsigned long)sFrame + 1, sFrameWide ? "3D" : "edges");
    }
    sLastWide = sFrameWide;
    SDL_GPUCommandBuffer* cb = get_cmd();
    // Portrait: the pointer maps onto what this frame shows; the next frame
    // decides its own mode at its first draw.
    port_portrait_presented(sCanvasMode);
    sFrameOpen = false;
    if (cb != NULL) {
        present(cb);
        if (dump_requested(sFrame + 1)) {
            dump_efb(cb, sFrame + 1);
        } else if (sGpuTime) {
            submit_timed(cb);
        } else {
            SDL_SubmitGPUCommandBuffer(cb);
        }
        sCmd = NULL;
    }
    for (SDL_GPUTexture* t : sRetired) {
        SDL_ReleaseGPUTexture(sDev, t);
    }
    sRetired.clear();

    sFrame++;
    gx_spec_frame_end(sFrame, make_spec_pipe, dump_requested(sFrame));
    // Testing: FORECAST_QUIT_FRAME=n quits after frame n.
    static const char* sQuit = SDL_getenv("FORECAST_QUIT_FRAME");
    if (sQuit != NULL && (u32)SDL_atoi(sQuit) == sFrame) {
        port_log("gx: quitting after frame %lu (FORECAST_QUIT_FRAME)\n", (unsigned long)sFrame);
        port_platform_quit();
        exit(0);
    }
    if (sFrame <= 3 || sFrame % 600 == 0) {
        u64 texBytes = 0;
        for (const auto& kv : sTextures) {
            texBytes += kv.second.bytes;
        }
        port_log("gx: frame %lu: %lu draws, %lu vertices, %lu texture uploads, %lu pipelines, %lu EFB copies, "
                 "%lu textures in %lu KB\n",
                 (unsigned long)sFrame, (unsigned long)sFrameDraws, (unsigned long)sFrameVerts,
                 (unsigned long)sFrameUploads, (unsigned long)sPipelines.size(), (unsigned long)sEfbCopies.size(),
                 (unsigned long)sTextures.size(), (unsigned long)(texBytes / 1024));
    }
    sFrameDraws = sFrameVerts = sFrameUploads = 0;
    // The EFB is resized only between frames.
    ensure_efb();

    // Drop GPU copies of textures that have not been used for a while.
    if (sFrame % 120 == 0) {
        for (auto it = sTextures.begin(); it != sTextures.end();) {
            if (sFrame - it->second.lastFrame > 600) {
                if (it->second.tex != NULL) {
                    SDL_ReleaseGPUTexture(sDev, it->second.tex);
                }
                it = sTextures.erase(it);
            } else {
                ++it;
            }
        }
    }
    gx_tex_frame_end();
}

// GXCopyTex: copies an EFB rectangle into a texture kept on the GPU.
static void copy_to_texture(u32 command) {
    frame_open();
    flush();
    SDL_GPUCommandBuffer* cb = get_cmd();
    if (cb == NULL) {
        return;
    }
    // BP 0x49: source top-left (x bits 0-9, y 10-19); 0x4A: size - 1;
    // 0x4B: destination address >> 5.
    u32 x = gx_bits(gx.bp[0x49], 0, 10), y = gx_bits(gx.bp[0x49], 10, 10);
    u32 w = gx_bits(gx.bp[0x4A], 0, 10) + 1, h = gx_bits(gx.bp[0x4A], 10, 10) + 1;
    u32 addr = gx_bits(gx.bp[0x4B], 0, 24) << 5;
    if (x + w > sEfbW || y + h > sEfbH) {
        WARN_ONCE("gx: EFB copy %lu,%lu %lux%lu outside the EFB\n", (unsigned long)x, (unsigned long)y,
                  (unsigned long)w, (unsigned long)h);
        return;
    }
    u32 fmt = gx_bits(command, 4, 3) | gx_bits(command, 3, 1) << 3;
    if (fmt != 4 && fmt != 5 && fmt != 6) { // RGB565, RGB5A3, RGBA8
        WARN_ONCE("gx: EFB copy to texture format %lu copied as RGBA\n", (unsigned long)fmt);
    }
    // Portrait: the rectangle on the canvas; the whole frame on a canvas frame
    // is the whole canvas (what a full-frame quad drawing the copy covers).
    SDL_Rect src = {(int)(x + sMargin), (int)y, (int)w, (int)h};
    if (sPortrait) {
        if (sCanvasMode && x == 0 && y == 0 && w == sEfbW && h == sEfbH) {
            src = {0, 0, (int)sCanvasW, (int)sCanvasH};
        } else {
            src = canvas_rect(frame_place(), (f32)x, (f32)y, (f32)(x + w), (f32)(y + h));
        }
        if (src.w <= 0 || src.h <= 0) {
            return;
        }
    }
    EfbCopy& c = sEfbCopies[addr];
    u32 gw = (u32)src.w * sScale, gh = (u32)src.h * sScale;
    if (c.tex == NULL || c.w != gw || c.h != gh) {
        if (c.tex != NULL) {
            sRetired.push_back(c.tex);
        }
        c.tex = create_texture(gw, gh, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER);
        c.w = gw;
        c.h = gh;
    }
    SDL_GPUBlitInfo bi = {};
    bi.source.texture = sEfbColor;
    bi.source.x = (u32)src.x * sScale; // game coordinates: the frame, not the margins
    bi.source.y = (u32)src.y * sScale;
    bi.source.w = gw;
    bi.source.h = gh;
    bi.destination.texture = c.tex;
    bi.destination.w = gw;
    bi.destination.h = gh;
    bi.load_op = SDL_GPU_LOADOP_DONT_CARE;
    bi.filter = SDL_GPU_FILTER_NEAREST;
    SDL_BlitGPUTexture(cb, &bi);
}

void gx_efb_copy(u32 command) {
    bool clear = (command >> 11) & 1;
    bool toXfb = (command >> 14) & 1;
    if (!toXfb) {
        frame_open();
    }
    if (toXfb) {
        port_gx_present();
    } else {
        copy_to_texture(command);
    }
    if (clear) {
        // Copy clear colour: BP 0x4F = A:R, 0x50 = G:B; 0x51 = Z (24-bit).
        u32 ar = gx.bp[0x4F], gb = gx.bp[0x50];
        sClearColor.r = (ar & 0xFF) / 255.0f;
        sClearColor.g = (gb >> 8 & 0xFF) / 255.0f;
        sClearColor.b = (gb & 0xFF) / 255.0f;
        // Without an alpha channel in the EFB (pixel format RGB8_Z24),
        // alpha reads as 255.
        sClearColor.a = gx_bits(gx.bp[0x43], 0, 3) == 1 ? (ar >> 8 & 0xFF) / 255.0f : 1.0f;
        sClearDepth = (gx.bp[0x51] & 0xFFFFFF) / 16777215.0f;
        sClearNext = true;
        sFrameWide = false; // the margins start empty again
    }
}

// --- bring-up ------------------------------------------------------------------------

static SDL_GPUShader* make_shader(const u8* code, size_t size, SDL_GPUShaderStage stage, u32 samplers,
                                  u32 uniforms) {
    SDL_GPUShaderCreateInfo si = {};
    si.code = code;
    si.code_size = size;
    si.entrypoint = GX_SHADER_ENTRY;
    si.format = GX_SHADER_FORMAT;
    si.stage = stage;
    si.num_samplers = samplers;
    si.num_uniform_buffers = uniforms;
    SDL_GPUShader* s = SDL_CreateGPUShader(sDev, &si);
    if (s == NULL) {
        port_log("gx: shader failed: %s\n", SDL_GetError());
    }
    return s;
}

extern "C" void port_gx_backend_init(void) {
    static u8 tlut[0x80000];
    gx.tmemHi = tlut;

#ifdef NDEBUG
    bool debug = false;
#else
    bool debug = true;
#endif
    sDev = SDL_CreateGPUDevice(GX_SHADER_FORMAT, debug, NULL);
    if (sDev == NULL) {
        port_log("gx: SDL_CreateGPUDevice failed: %s\n", SDL_GetError());
        abort();
    }
    if (!SDL_ClaimWindowForGPUDevice(sDev, port_window())) {
        port_log("gx: SDL_ClaimWindowForGPUDevice failed: %s\n", SDL_GetError());
        abort();
    }
    port_log("gx: SDL GPU backend %s%s\n", SDL_GetGPUDeviceDriver(sDev), debug ? " (debug layer)" : "");

    choose_texcomp();
    sDepthFmt = SDL_GPUTextureSupportsFormat(sDev, SDL_GPU_TEXTUREFORMAT_D24_UNORM, SDL_GPU_TEXTURETYPE_2D,
                                             SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)
                    ? SDL_GPU_TEXTUREFORMAT_D24_UNORM
                    : SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

    sVS = make_shader((const u8*)GX_VS_CODE, GX_VS_SIZE, SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    sPS = make_shader((const u8*)GX_PS_CODE, GX_PS_SIZE, SDL_GPU_SHADERSTAGE_FRAGMENT, 8, 1);
    if (sVS == NULL || sPS == NULL) {
        abort();
    }
    // The screen fill's present (gx_present_ps.hlsl): without it the frame
    // is still shown, with black bars.
    sPresentPS = make_shader((const u8*)GX_PRESENT_PS_CODE, GX_PRESENT_PS_SIZE, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);
    {
        SDL_GPUSamplerCreateInfo si = {};
        si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;
        si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sPresentSmp = SDL_CreateGPUSampler(sDev, &si);
        SDL_GPUBufferCreateInfo bi = {};
        bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        bi.size = PRESENT_MAX_VERTS * sizeof(GXVtxOut);
        sPresentBuf = SDL_CreateGPUBuffer(sDev, &bi);
        SDL_GPUTransferBufferCreateInfo ti = {};
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        ti.size = bi.size;
        sPresentXfer = SDL_CreateGPUTransferBuffer(sDev, &ti);
        if (sPresentPS == NULL || sPresentSmp == NULL || sPresentBuf == NULL || sPresentXfer == NULL) {
            port_log("gx: screen fill present unavailable: %s\n", SDL_GetError());
            sPresentPS = NULL;
        }
    }
    gx_spec_init(sDev);
    sGpuTime = SDL_getenv("FORECAST_GPU_TIME") != NULL && SDL_atoi(SDL_getenv("FORECAST_GPU_TIME")) != 0;

    // The 1x1 white texture bound to unused maps.
    static const u8 white[4] = {255, 255, 255, 255};
    sWhite = create_texture(1, 1, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    queue_upload(sWhite, white, 1, 1);
    ensure_efb();
}
