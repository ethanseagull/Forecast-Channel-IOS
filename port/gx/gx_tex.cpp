// Wii textures, decoded to RGBA8 for the renderer.
//
// A texture map's BP registers name an image in memory (address, size, Wii
// format, TLUT). The first draw that samples it decodes it from the Wii's
// tiled formats into a linear RGBA8 copy (gx_tex_decode), kept in a cache
// keyed by what the registers describe. After GXInvalidateTexAll (BP 0x66,
// issued every frame by SystemDraw) each image is re-hashed on its next use
// and decoded again only if its bytes or its palette changed; every decode
// gets a new version number so the renderer knows to re-upload.
//
// Built with GX_TEX_DECODE_ONLY defined, only the decoders are compiled
// (tools/tex_test builds them on their own).
#include <string.h>

#include "gx_tex_decode.h"

// --- decoding ---------------------------------------------------------------------

static u32 block_w(u32 fmt) {
    switch (fmt) {
    case GX_TEXFMT_I4:
    case GX_TEXFMT_I8:
    case GX_TEXFMT_IA4:
    case GX_TEXFMT_CI4:
    case GX_TEXFMT_CI8:
    case GX_TEXFMT_CMPR: return 8;
    default: return 4;
    }
}

static u32 block_h(u32 fmt) {
    switch (fmt) {
    case GX_TEXFMT_I4:
    case GX_TEXFMT_CI4:
    case GX_TEXFMT_CMPR: return 8;
    default: return 4;
    }
}

static bool known_format(u32 fmt) {
    return fmt <= GX_TEXFMT_RGBA8 || fmt == GX_TEXFMT_CI4 || fmt == GX_TEXFMT_CI8 || fmt == GX_TEXFMT_CI14 ||
           fmt == GX_TEXFMT_CMPR;
}

u32 gx_tex_data_size(u32 fmt, u32 w, u32 h) {
    if (!known_format(fmt)) {
        return 0;
    }
    u32 bw = block_w(fmt), bh = block_h(fmt);
    u32 blocks = ((w + bw - 1) / bw) * ((h + bh - 1) / bh);
    // Every tile is 32 bytes, except RGBA8's, which is two (AR, then GB).
    return blocks * (fmt == GX_TEXFMT_RGBA8 ? 64 : 32);
}

u32 gx_tex_tlut_entries(u32 fmt) {
    switch (fmt) {
    case GX_TEXFMT_CI4: return 16;
    case GX_TEXFMT_CI8: return 256;
    case GX_TEXFMT_CI14: return 16384;
    default: return 0;
    }
}

// Texels are packed as u32 R | G << 8 | B << 16 | A << 24 and stored byte by
// byte (put), so the output is bytes R, G, B, A whatever the host's byte order.
static inline u32 pack(u32 r, u32 g, u32 b, u32 a) {
    return r | g << 8 | b << 16 | a << 24;
}

static inline u32 be16(const u8* p) {
    return (u32)p[0] << 8 | p[1];
}

static inline u32 x3to8(u32 v) {
    return v << 5 | v << 2 | v >> 1;
}
static inline u32 x4to8(u32 v) {
    return v * 0x11;
}
static inline u32 x5to8(u32 v) {
    return v << 3 | v >> 2;
}
static inline u32 x6to8(u32 v) {
    return v << 2 | v >> 4;
}

static inline u32 from_rgb565(u32 v) {
    return pack(x5to8(v >> 11), x6to8((v >> 5) & 63), x5to8(v & 31), 255);
}

// Bit 15 set: RGB555, opaque. Clear: 3-bit alpha (14-12), then RGB444.
static inline u32 from_rgb5a3(u32 v) {
    if (v & 0x8000) {
        return pack(x5to8((v >> 10) & 31), x5to8((v >> 5) & 31), x5to8(v & 31), 255);
    }
    return pack(x4to8((v >> 8) & 15), x4to8((v >> 4) & 15), x4to8(v & 15), x3to8((v >> 12) & 7));
}

// Alpha in the first (high) byte, intensity in the second.
static inline u32 from_ia8(u32 v) {
    u32 a = v >> 8, i = v & 0xFF;
    return pack(i, i, i, a);
}

static inline void put(u8* out, u32 w, u32 h, u32 x, u32 y, u32 c) {
    if (x < w && y < h) {
        u8* p = out + ((size_t)y * w + x) * 4;
        p[0] = (u8)c;
        p[1] = (u8)(c >> 8);
        p[2] = (u8)(c >> 16);
        p[3] = (u8)(c >> 24);
    }
}

// One 4x4 DXT1 sub-block of a CMPR tile, as the GPU decodes it: colours are
// big-endian RGB565, the two interpolated colours blend 5/8 + 3/8 (not 2/3 +
// 1/3), and with c0 <= c1 index 3 is the average colour made transparent (not
// transparent black). Indices: one byte per row, leftmost texel in the top
// two bits. Matches Dolphin's DecodeDXTBlock.
static void decode_dxt1(const u8* p, u8* out, u32 w, u32 h, u32 x0, u32 y0) {
    u32 c0 = be16(p), c1 = be16(p + 2);
    u32 r0 = x5to8(c0 >> 11), g0 = x6to8((c0 >> 5) & 63), b0 = x5to8(c0 & 31);
    u32 r1 = x5to8(c1 >> 11), g1 = x6to8((c1 >> 5) & 63), b1 = x5to8(c1 & 31);
    u32 pal[4];
    pal[0] = pack(r0, g0, b0, 255);
    pal[1] = pack(r1, g1, b1, 255);
    if (c0 > c1) {
        pal[2] = pack((r0 * 5 + r1 * 3) >> 3, (g0 * 5 + g1 * 3) >> 3, (b0 * 5 + b1 * 3) >> 3, 255);
        pal[3] = pack((r0 * 3 + r1 * 5) >> 3, (g0 * 3 + g1 * 5) >> 3, (b0 * 3 + b1 * 5) >> 3, 255);
    } else {
        pal[2] = pack((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 255);
        pal[3] = pack((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 0);
    }
    for (u32 y = 0; y < 4; y++) {
        u32 bits = p[4 + y];
        for (u32 x = 0; x < 4; x++) {
            put(out, w, h, x0 + x, y0 + y, pal[(bits >> (6 - x * 2)) & 3]);
        }
    }
}

bool gx_tex_decode(u32 fmt, const u8* src, u32 w, u32 h, const u8* tlut, u32 tlutFmt, u8* outRgba) {
    if (!known_format(fmt) || w == 0 || h == 0) {
        return false;
    }
    u32 entries = gx_tex_tlut_entries(fmt);
    if (entries != 0 && tlut == nullptr) {
        return false;
    }

    // The palette, converted once.
    static u32 sPal[16384];
    for (u32 i = 0; i < entries; i++) {
        u32 v = be16(tlut + i * 2);
        switch (tlutFmt) {
        case GX_TLUTFMT_IA8: sPal[i] = from_ia8(v); break;
        case GX_TLUTFMT_RGB565: sPal[i] = from_rgb565(v); break;
        default: sPal[i] = from_rgb5a3(v); break; // 2, and 3 (invalid)
        }
    }

    u32 bw = block_w(fmt), bh = block_h(fmt);
    u32 nbx = (w + bw - 1) / bw, nby = (h + bh - 1) / bh;
    u8* out = outRgba;

    // Tiles are stored left to right, then top to bottom; texels within a
    // tile likewise.
    for (u32 by = 0; by < nby; by++) {
        for (u32 bx = 0; bx < nbx; bx++) {
            u32 x0 = bx * bw, y0 = by * bh;
            switch (fmt) {
            case GX_TEXFMT_CMPR:
                // An 8x8 tile of four 4x4 sub-blocks: top left, top right,
                // bottom left, bottom right.
                for (u32 sb = 0; sb < 4; sb++) {
                    decode_dxt1(src + sb * 8, out, w, h, x0 + (sb & 1) * 4, y0 + (sb >> 1) * 4);
                }
                src += 32;
                continue;
            case GX_TEXFMT_RGBA8:
                // 32 bytes of A, R pairs, then 32 bytes of G, B pairs.
                for (u32 i = 0; i < 16; i++) {
                    put(out, w, h, x0 + (i & 3), y0 + (i >> 2),
                        pack(src[i * 2 + 1], src[32 + i * 2], src[32 + i * 2 + 1], src[i * 2]));
                }
                src += 64;
                continue;
            default: break;
            }
            for (u32 y = 0; y < bh; y++) {
                for (u32 x = 0; x < bw; x++) {
                    u32 i = y * bw + x;
                    u32 c;
                    switch (fmt) {
                    case GX_TEXFMT_I4: {
                        // Two texels per byte, the left one in the high nibble.
                        u32 v = x4to8((src[i >> 1] >> ((i & 1) ? 0 : 4)) & 15);
                        c = pack(v, v, v, v);
                        break;
                    }
                    case GX_TEXFMT_I8: c = pack(src[i], src[i], src[i], src[i]); break;
                    case GX_TEXFMT_IA4: { // alpha in the high nibble
                        u32 v = src[i], l = x4to8(v & 15);
                        c = pack(l, l, l, x4to8(v >> 4));
                        break;
                    }
                    case GX_TEXFMT_IA8: c = from_ia8(be16(src + i * 2)); break;
                    case GX_TEXFMT_RGB565: c = from_rgb565(be16(src + i * 2)); break;
                    case GX_TEXFMT_RGB5A3: c = from_rgb5a3(be16(src + i * 2)); break;
                    case GX_TEXFMT_CI4: c = sPal[(src[i >> 1] >> ((i & 1) ? 0 : 4)) & 15]; break;
                    case GX_TEXFMT_CI8: c = sPal[src[i]]; break;
                    default: c = sPal[be16(src + i * 2) & 0x3FFF]; break; // CI14
                    }
                    put(out, w, h, x0 + x, y0 + y, c);
                }
            }
            src += 32;
        }
    }
    return true;
}

#ifndef GX_TEX_DECODE_ONLY

// --- the cache --------------------------------------------------------------------

#include <unordered_map>
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

namespace {

struct TexEntry {
    std::vector<u8> rgba;
    u64 hash = 0;
    u32 epoch = 0; // sEpoch when last hashed (or decoded)
    u32 lastFrame = 0;
    u32 version = 0; // 0: not decoded yet
    u32 w = 0, h = 0;
};

// Node-based: entries stay put when others are added.
std::unordered_map<u64, TexEntry> sCache;
u32 sEpoch = 1;
u32 sFrame;
u32 sVersion;
// Since the last statistics line.
u32 sDecoded, sHashed;
u64 sDecodedBytes, sHashedBytes;

const u32 kTmemHiSize = 0x80000;
const u32 kEvictFrames = 600;

u32 next_version() {
    if (++sVersion == 0) {
        sVersion = 1;
    }
    return sVersion;
}

inline u64 mix(u64 h, u64 v) {
    h ^= v;
    h *= 0x9E3779B97F4A7C15ull;
    return h ^ (h >> 29);
}

// A 64-bit hash, four independent lanes so it runs near memory speed (every
// image in use is re-hashed once a frame).
u64 hash_bytes(const u8* p, size_t n, u64 seed) {
    u64 a = seed, b = seed ^ 0x243F6A8885A308D3ull, c = seed ^ 0x13198A2E03707344ull, d = seed ^ 0xA4093822299F31D0ull;
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        u64 v[4];
        memcpy(v, p + i, 32);
        a = mix(a, v[0]);
        b = mix(b, v[1]);
        c = mix(c, v[2]);
        d = mix(d, v[3]);
    }
    u64 h = mix(mix(mix(a, b), c), d);
    for (; i < n; i++) {
        h = mix(h, p[i]);
    }
    return mix(h, n);
}

} // namespace

void gx_tex_invalidate(void) {
    sEpoch++;
}

bool gx_tex_get(int map, GXTexDesc* out) {
    if (map < 0 || map > 7) {
        return false;
    }
    // Maps 0-3: BP 0x80-0x9B, maps 4-7: BP 0xA0-0xBB (mode0, mode1, image0-3,
    // TLUT, 4 registers apart).
    u32 b = map < 4 ? 0x80 + map : 0xA0 + (map - 4);
    u32 mode0 = gx.bp[b], mode1 = gx.bp[b + 0x04], image0 = gx.bp[b + 0x08], image3 = gx.bp[b + 0x14],
        tlut = gx.bp[b + 0x18];
    u32 w = gx_bits(image0, 0, 10) + 1, h = gx_bits(image0, 10, 10) + 1;
    u32 fmt = gx_bits(image0, 20, 4);
    u32 addr = gx_bits(image3, 0, 24);
    if (addr == 0) {
        return false;
    }
    u32 size = gx_tex_data_size(fmt, w, h);
    if (size == 0) {
        WARN_ONCE("gx: texture map %d has unsupported format %u\n", map, fmt);
        return false;
    }
    const u8* src = gx_mem(addr << 5);
    if (!gx_addr_ok(src, size)) {
        static int sBad;
        if (sBad++ < 8) {
            port_log("gx: texture map %d at GX address %08x (%ux%u fmt %u) unmapped\n", map, addr << 5, w, h, fmt);
        }
        return false;
    }
    if (gx_bits(mode1, 8, 8) != 0) {
        WARN_ONCE("gx: texture with mipmaps (%ux%u fmt %u, max LOD %u/16); only level 0 is used\n", w, h, fmt,
                  gx_bits(mode1, 8, 8));
    }

    // Address (24 bits), format (4), size (10 + 10), and for CI formats the
    // TLUT register (TMEM offset 10, format 2).
    u64 key = (u64)addr | (u64)fmt << 24 | (u64)(w - 1) << 28 | (u64)(h - 1) << 38;
    u32 entries = gx_tex_tlut_entries(fmt);
    const u8* pal = nullptr;
    u32 tlutFmt = 0;
    if (entries != 0) {
        if (gx.tmemHi == nullptr) {
            return false;
        }
        u32 off = gx_bits(tlut, 0, 10) << 9;
        u32 bytes = entries * 2;
        tlutFmt = gx_bits(tlut, 10, 2);
        pal = gx.tmemHi + off;
        if (off + bytes > kTmemHiSize) {
            // A CI14 palette running past the end of TMEM: pad with zeros
            // rather than read beyond it.
            static u8 sPadded[16384 * 2];
            memset(sPadded, 0, sizeof(sPadded));
            memcpy(sPadded, pal, kTmemHiSize - off);
            pal = sPadded;
            WARN_ONCE("gx: TLUT at TMEM %05x runs past the end of TMEM\n", off);
        }
        key |= (u64)gx_bits(tlut, 0, 12) << 48;
    }

    TexEntry& e = sCache[key];
    bool fresh = e.version == 0;
    if (fresh || e.epoch != sEpoch) {
        u64 hash = hash_bytes(src, size, 0x6A09E667F3BCC908ull);
        if (pal != nullptr) {
            hash = hash_bytes(pal, entries * 2, hash);
        }
        sHashed++;
        sHashedBytes += size;
        if (fresh || hash != e.hash) {
            e.rgba.resize((size_t)w * h * 4);
            if (!gx_tex_decode(fmt, src, w, h, pal, tlutFmt, e.rgba.data())) {
                sCache.erase(key);
                return false;
            }
            e.w = w;
            e.h = h;
            e.hash = hash;
            e.version = next_version();
            sDecoded++;
            sDecodedBytes += e.rgba.size();
        }
        e.epoch = sEpoch;
    }
    e.lastFrame = sFrame;

    out->key = key;
    out->version = e.version;
    out->width = e.w;
    out->height = e.h;
    out->rgba = e.rgba.data();
    // Sampling state can differ per use of the same image. Wrap: 0 clamp,
    // 1 repeat, 2 mirror (3 is invalid: repeat).
    u32 ws = gx_bits(mode0, 0, 2), wt = gx_bits(mode0, 2, 2);
    out->wrapS = (u8)(ws == 3 ? 1 : ws);
    out->wrapT = (u8)(wt == 3 ? 1 : wt);
    out->magLinear = (u8)gx_bits(mode0, 4, 1);
    // The min filter field (bits 5-7) is in the hardware encoding
    // (GX2HWFiltConv: NEAR 0, NEAR_MIP_NEAR 1, NEAR_MIP_LIN 2, LINEAR 4,
    // LIN_MIP_NEAR 5, LIN_MIP_LIN 6): its top bit is the base filter.
    out->minLinear = (u8)gx_bits(mode0, 7, 1);
    return true;
}

void gx_tex_frame_end(void) {
    sFrame++;
    if (sFrame % 120 == 0) {
        for (auto it = sCache.begin(); it != sCache.end();) {
            if (sFrame - it->second.lastFrame > kEvictFrames) {
                it = sCache.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (sFrame == 1 || sFrame % 600 == 0) {
        size_t bytes = 0;
        for (const auto& kv : sCache) {
            bytes += kv.second.rgba.size();
        }
        port_log("gx: %u textures cached (%u KB RGBA); since last: %u decoded (%u KB), %u re-hashed (%u KB)\n",
                 (u32)sCache.size(), (u32)(bytes / 1024), sDecoded, (u32)(sDecodedBytes / 1024), sHashed,
                 (u32)(sHashedBytes / 1024));
        sDecoded = sHashed = 0;
        sDecodedBytes = sHashedBytes = 0;
    }
}

#endif // GX_TEX_DECODE_ONLY
