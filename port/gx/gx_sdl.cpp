// The GX renderer on SDL3 GPU (D3D12 on Windows, Metal on iOS).
//
// Draws are recorded as the command processor decodes them: gx_xform.cpp
// transforms each primitive into a frame-wide vertex list, and the draw keeps
// a copy of the pixel state it needs (pipeline, textures, scissor, the TEV
// registers for the ubershader). Textures are decoded by gx_tex.cpp and
// uploaded once per version. The copy to the display (GXCopyDisp) ends the
// frame: one copy pass uploads the vertices and new textures, one render pass
// replays the draws into the EFB (a render target at a multiple of the Wii's
// resolution), and the EFB is scaled onto the window.
#include <stdlib.h>
#include <string.h>

#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>

#include "gx_internal.h"
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
#else
#include "shaders/gx_shaders_dxil.h"
#define GX_SHADER_FORMAT SDL_GPU_SHADERFORMAT_DXIL
#define GX_SHADER_ENTRY "main"
#define GX_VS_CODE gx_vs_dxil
#define GX_VS_SIZE sizeof(gx_vs_dxil)
#define GX_PS_CODE gx_ps_dxil
#define GX_PS_SIZE sizeof(gx_ps_dxil)
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
// anamorphically and stretched, as the TV would.
#define DISPLAY_ASPECT (16.0f / 9.0f)

static SDL_GPUDevice* sDev;
static SDL_GPUShader* sVS;
static SDL_GPUShader* sPS;
static SDL_GPUTextureFormat sDepthFmt;

// The EFB: colour and depth targets at sScale times the Wii's size.
static SDL_GPUTexture* sEfbColor;
static SDL_GPUTexture* sEfbDepth;
static u32 sEfbW, sEfbH, sScale;
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

static std::unordered_map<u32, SDL_GPUGraphicsPipeline*> sPipelines;

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

static SDL_GPUGraphicsPipeline* get_pipeline(PipeKey k) {
    auto it = sPipelines.find(k.bits);
    if (it != sPipelines.end()) {
        return it->second;
    }

    SDL_GPUVertexBufferDescription vb = {};
    vb.slot = 0;
    vb.pitch = sizeof(GXVtxOut);
    vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute attrs[11] = {};
    attrs[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, (Uint32)offsetof(GXVtxOut, pos)};
    attrs[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, (Uint32)offsetof(GXVtxOut, clr[0])};
    attrs[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, (Uint32)offsetof(GXVtxOut, clr[1])};
    for (int i = 0; i < 8; i++) {
        attrs[3 + i] = {(Uint32)(3 + i), 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
                        (Uint32)(offsetof(GXVtxOut, tex) + i * 3 * sizeof(f32))};
    }

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
    pi.fragment_shader = sPS;
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
        port_log("gx: pipeline %08lx failed: %s\n", (unsigned long)k.bits, SDL_GetError());
    }
    sPipelines[k.bits] = p;
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
    SDL_Rect scissor; // in EFB pixels
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
    Draw d;
    d.first = (u32)sVerts.size();
    d.count = gx_xform_primitive(primitive, verts, count, sVerts);
    if (d.count == 0) {
        return;
    }
    d.pipe = get_pipeline(pipe_key());
    if (d.pipe == NULL) {
        sVerts.resize(d.first);
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

    // Scissor: BP 0x20 top (bits 0-10) / left (12-22), 0x21 bottom / right,
    // all + 342 and inclusive.
    int top = (int)gx_bits(gx.bp[0x20], 0, 11) - 342;
    int left = (int)gx_bits(gx.bp[0x20], 12, 11) - 342;
    int bottom = (int)gx_bits(gx.bp[0x21], 0, 11) - 342 + 1;
    int right = (int)gx_bits(gx.bp[0x21], 12, 11) - 342 + 1;
    if (left < 0) {
        left = 0;
    }
    if (top < 0) {
        top = 0;
    }
    d.scissor.x = left;
    d.scissor.y = top;
    d.scissor.w = right > left ? right - left : 0;
    d.scissor.h = bottom > top ? bottom - top : 0;

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
    if (sEfbColor != NULL && w == sEfbW && h == sEfbH && scale == sScale) {
        return;
    }
    if (sEfbColor != NULL) {
        SDL_ReleaseGPUTexture(sDev, sEfbColor);
        SDL_ReleaseGPUTexture(sDev, sEfbDepth);
    }
    sEfbW = w;
    sEfbH = h;
    sScale = scale;
    sEfbColor = create_texture(w * scale, h * scale, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                               SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    sEfbDepth = create_texture(w * scale, h * scale, sDepthFmt, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
    sClearNext = true;
    port_log("gx: EFB %lux%lu at %lux (%lux%lu)\n", (unsigned long)w, (unsigned long)h, (unsigned long)scale,
             (unsigned long)(w * scale), (unsigned long)(h * scale));
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
    SDL_GPUViewport vp = {0.0f, 0.0f, (f32)(sEfbW * sScale), (f32)(sEfbH * sScale), 0.0f, 1.0f};
    SDL_SetGPUViewport(rp, &vp);
    SDL_GPUBufferBinding vb = {sVtxBuf, 0};
    if (sVtxBuf != NULL) {
        SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
    }
    SDL_GPUGraphicsPipeline* bound = NULL;
    for (const Draw& d : sDraws) {
        SDL_Rect sc = {d.scissor.x * (int)sScale, d.scissor.y * (int)sScale, d.scissor.w * (int)sScale,
                       d.scissor.h * (int)sScale};
        int maxW = (int)(sEfbW * sScale), maxH = (int)(sEfbH * sScale);
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
        SDL_SetGPUScissor(rp, &sc);
        SDL_BindGPUFragmentSamplers(rp, 0, d.tex, 8);
        SDL_PushGPUFragmentUniformData(cb, 0, &d.tev, sizeof(d.tev));
        SDL_DrawGPUPrimitives(rp, d.count, 1, d.first, 0);
    }
    SDL_EndGPURenderPass(rp);
}

static Uint64 sAcquireNS; // this frame's wait for the window's image

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
    // Clear to black, then scale the EFB into the largest 16:9 rectangle.
    SDL_GPUColorTargetInfo ct = {};
    ct.texture = sw;
    ct.load_op = SDL_GPU_LOADOP_CLEAR;
    ct.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cb, &ct, 1, NULL));

    f32 dw = (f32)sw_w, dh = (f32)sw_h;
    if (dw / dh > DISPLAY_ASPECT) {
        dw = dh * DISPLAY_ASPECT;
    } else {
        dh = dw / DISPLAY_ASPECT;
    }
    SDL_GPUBlitInfo bi = {};
    bi.source.texture = sEfbColor;
    bi.source.w = sEfbW * sScale;
    bi.source.h = sEfbH * sScale;
    bi.destination.texture = sw;
    bi.destination.x = (Uint32)((sw_w - dw) * 0.5f);
    bi.destination.y = (Uint32)((sw_h - dh) * 0.5f);
    bi.destination.w = (Uint32)dw;
    bi.destination.h = (Uint32)dh;
    bi.load_op = SDL_GPU_LOADOP_LOAD;
    bi.filter = SDL_GPU_FILTER_LINEAR;
    SDL_BlitGPUTexture(cb, &bi);
}

// Debugging: FORECAST_DUMP_FRAME=n[,m...] saves the EFB of those frames as
// efb_<n>.bmp in the working directory.
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

static void dump_efb(SDL_GPUCommandBuffer* cb, u32 frame) {
    u32 w = sEfbW * sScale, h = sEfbH * sScale;
    SDL_GPUTransferBufferCreateInfo ti = {};
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    ti.size = w * h * 4;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(sDev, &ti);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
    SDL_GPUTextureRegion src = {};
    src.texture = sEfbColor;
    src.w = w;
    src.h = h;
    src.d = 1;
    SDL_GPUTextureTransferInfo dst = {};
    dst.transfer_buffer = tb;
    SDL_DownloadFromGPUTexture(cp, &src, &dst);
    SDL_EndGPUCopyPass(cp);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
    SDL_WaitForGPUFences(sDev, true, &fence, 1);
    SDL_ReleaseGPUFence(sDev, fence);
    void* px = SDL_MapGPUTransferBuffer(sDev, tb, false);
    SDL_Surface* s = SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_ABGR8888, px, (int)(w * 4));
    char name[64];
    SDL_snprintf(name, sizeof(name), "efb_%u.bmp", (unsigned)frame);
    if (s == NULL || !SDL_SaveBMP(s, name)) {
        port_log("gx: dump %s failed: %s\n", name, SDL_GetError());
    } else {
        port_log("gx: dumped %s\n", name);
    }
    SDL_DestroySurface(s);
    SDL_UnmapGPUTransferBuffer(sDev, tb);
    SDL_ReleaseGPUTransferBuffer(sDev, tb);
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
    SDL_GPUCommandBuffer* cb = get_cmd();
    if (cb != NULL) {
        present(cb);
        if (dump_requested(sFrame + 1)) {
            dump_efb(cb, sFrame + 1);
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
    EfbCopy& c = sEfbCopies[addr];
    u32 gw = w * sScale, gh = h * sScale;
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
    bi.source.x = x * sScale;
    bi.source.y = y * sScale;
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

    // The 1x1 white texture bound to unused maps.
    static const u8 white[4] = {255, 255, 255, 255};
    sWhite = create_texture(1, 1, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    queue_upload(sWhite, white, 1, 1);
    ensure_efb();
}
