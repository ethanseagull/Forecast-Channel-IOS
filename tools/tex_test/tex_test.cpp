// Standalone test of the GX texture decoders (port/gx/gx_tex.cpp built with
// GX_TEX_DECODE_ONLY).
//
//   tex_test --self
//       Synthetic checks: hand-encoded texels with known values for every
//       format, and a round trip through an independent tiler (linear image
//       -> Wii tiles -> gx_tex_decode) at sizes that are not whole tiles.
//   tex_test [--checker] <png dir> <file.tpl | dir>...
//       Decodes every image of every TPL into <png dir> as
//       <tpl name>_<index>_<format>_<w>x<h>.png and prints a count per format.
//       --checker composites the images over a grey checkerboard (opaque
//       PNGs, easier to look at; alpha shows as the checkerboard).
//
// Writes PNGs with a built-in encoder (stored deflate blocks), so it needs
// nothing but the C++ standard library.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include <filesystem>

#include "gx_tex_decode.h"

namespace fs = std::filesystem;

static const char* fmt_name(u32 fmt) {
    switch (fmt) {
    case GX_TEXFMT_I4: return "I4";
    case GX_TEXFMT_I8: return "I8";
    case GX_TEXFMT_IA4: return "IA4";
    case GX_TEXFMT_IA8: return "IA8";
    case GX_TEXFMT_RGB565: return "RGB565";
    case GX_TEXFMT_RGB5A3: return "RGB5A3";
    case GX_TEXFMT_RGBA8: return "RGBA8";
    case GX_TEXFMT_CI4: return "CI4";
    case GX_TEXFMT_CI8: return "CI8";
    case GX_TEXFMT_CI14: return "CI14";
    case GX_TEXFMT_CMPR: return "CMPR";
    default: return "unknown";
    }
}

static const char* tlut_name(u32 fmt) {
    switch (fmt) {
    case GX_TLUTFMT_IA8: return "IA8";
    case GX_TLUTFMT_RGB565: return "RGB565";
    case GX_TLUTFMT_RGB5A3: return "RGB5A3";
    default: return "?";
    }
}

// --- PNG ----------------------------------------------------------------------------

static u32 sCrcTable[256];

static u32 crc32(const u8* p, size_t n, u32 crc) {
    if (sCrcTable[1] == 0) {
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            sCrcTable[i] = c;
        }
    }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc = sCrcTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

static void put32(std::vector<u8>& v, u32 x) {
    v.push_back((u8)(x >> 24));
    v.push_back((u8)(x >> 16));
    v.push_back((u8)(x >> 8));
    v.push_back((u8)x);
}

static void chunk(std::vector<u8>& png, const char* type, const std::vector<u8>& data) {
    put32(png, (u32)data.size());
    size_t start = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    put32(png, crc32(png.data() + start, png.size() - start, 0));
}

static bool write_png(const std::string& path, const u8* rgba, u32 w, u32 h) {
    // Raw scanlines, filter 0.
    std::vector<u8> raw;
    raw.reserve((size_t)(w * 4 + 1) * h);
    for (u32 y = 0; y < h; y++) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba + (size_t)y * w * 4, rgba + (size_t)(y + 1) * w * 4);
    }
    // zlib stream of stored blocks.
    std::vector<u8> z = {0x78, 0x01};
    size_t pos = 0;
    do {
        size_t n = raw.size() - pos < 65535 ? raw.size() - pos : 65535;
        z.push_back(pos + n == raw.size() ? 1 : 0);
        z.push_back((u8)n);
        z.push_back((u8)(n >> 8));
        z.push_back((u8)~n);
        z.push_back((u8)(~n >> 8));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    } while (pos < raw.size());
    u32 a = 1, b = 0;
    for (u8 c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    put32(z, b << 16 | a);

    std::vector<u8> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<u8> ihdr;
    put32(ihdr, w);
    put32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0}); // 8-bit RGBA
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    FILE* f = fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    fwrite(png.data(), 1, png.size(), f);
    fclose(f);
    return true;
}

// --- self test ------------------------------------------------------------------------

static int sFailures;

static void expect_px(const char* what, const std::vector<u8>& img, u32 w, u32 x, u32 y, u32 r, u32 g, u32 b,
                      u32 a) {
    const u8* p = &img[((size_t)y * w + x) * 4];
    if (p[0] != r || p[1] != g || p[2] != b || p[3] != a) {
        printf("FAIL %s (%u,%u): got %u,%u,%u,%u want %u,%u,%u,%u\n", what, x, y, p[0], p[1], p[2], p[3], r, g, b, a);
        sFailures++;
    }
}

static std::vector<u8> decode(u32 fmt, const std::vector<u8>& src, u32 w, u32 h, const u8* tlut = nullptr,
                              u32 tlutFmt = 0) {
    std::vector<u8> out((size_t)w * h * 4, 0xCD);
    if (gx_tex_data_size(fmt, w, h) > src.size()) {
        printf("FAIL %s %ux%u: source too small\n", fmt_name(fmt), w, h);
        sFailures++;
        return out;
    }
    if (!gx_tex_decode(fmt, src.data(), w, h, tlut, tlutFmt, out.data())) {
        printf("FAIL %s: decode returned false\n", fmt_name(fmt));
        sFailures++;
    }
    return out;
}

// Hand-encoded texels. Values follow the GX format descriptions (YAGCD /
// Dolphin), written out independently of the decoder.
static void self_known_values() {
    // I4, 8x8 tiles, high nibble first. 16x8: the second tile starts at 32.
    {
        std::vector<u8> s(64, 0);
        s[0] = 0x3C;  // (0,0)=3 (1,0)=C
        s[4] = 0xF0;  // (0,1)=F
        s[31] = 0x07; // (7,7)=7
        s[32] = 0xA0; // (8,0)=A
        auto o = decode(GX_TEXFMT_I4, s, 16, 8);
        expect_px("I4", o, 16, 0, 0, 0x33, 0x33, 0x33, 0x33);
        expect_px("I4", o, 16, 1, 0, 0xCC, 0xCC, 0xCC, 0xCC);
        expect_px("I4", o, 16, 0, 1, 0xFF, 0xFF, 0xFF, 0xFF);
        expect_px("I4", o, 16, 7, 7, 0x77, 0x77, 0x77, 0x77);
        expect_px("I4", o, 16, 8, 0, 0xAA, 0xAA, 0xAA, 0xAA);
    }
    // I8, 8x4 tiles. 8x8: the second tile row starts at 32.
    {
        std::vector<u8> s(64, 0);
        s[1] = 0x12;
        s[8] = 0x34;
        s[32] = 0x56;
        auto o = decode(GX_TEXFMT_I8, s, 8, 8);
        expect_px("I8", o, 8, 1, 0, 0x12, 0x12, 0x12, 0x12);
        expect_px("I8", o, 8, 0, 1, 0x34, 0x34, 0x34, 0x34);
        expect_px("I8", o, 8, 0, 4, 0x56, 0x56, 0x56, 0x56);
    }
    // IA4, 8x4 tiles: alpha in the high nibble.
    {
        std::vector<u8> s(32, 0);
        s[9] = 0x5A;
        auto o = decode(GX_TEXFMT_IA4, s, 8, 4);
        expect_px("IA4", o, 8, 1, 1, 0xAA, 0xAA, 0xAA, 0x55);
    }
    // IA8, 4x4 tiles: alpha byte first.
    {
        std::vector<u8> s(32, 0);
        s[10] = 0x80; // texel 5 = (1,1)
        s[11] = 0xFF;
        auto o = decode(GX_TEXFMT_IA8, s, 4, 4);
        expect_px("IA8", o, 4, 1, 1, 0xFF, 0xFF, 0xFF, 0x80);
    }
    // RGB565, big-endian.
    {
        std::vector<u8> s(32, 0);
        s[0] = 0xF8; // red
        s[1] = 0x00;
        s[2] = 0x07; // green
        s[3] = 0xE0;
        s[4] = 0x00; // blue
        s[5] = 0x1F;
        s[6] = 0x84; // r=16 g=33 b=16 -> 132, 134, 132
        s[7] = 0x30;
        auto o = decode(GX_TEXFMT_RGB565, s, 4, 4);
        expect_px("RGB565", o, 4, 0, 0, 255, 0, 0, 255);
        expect_px("RGB565", o, 4, 1, 0, 0, 255, 0, 255);
        expect_px("RGB565", o, 4, 2, 0, 0, 0, 255, 255);
        expect_px("RGB565", o, 4, 3, 0, 132, 134, 132, 255);
    }
    // RGB5A3: bit 15 set = RGB555 opaque, clear = A3 RGB444.
    {
        std::vector<u8> s(32, 0);
        s[0] = 0xFC; // 1 11111 00000 00000
        s[1] = 0x00;
        s[2] = 0x3A; // 0 011 1010 1011 1100
        s[3] = 0xBC;
        s[4] = 0x70; // a=7 r=0 g=0 b=F
        s[5] = 0x0F;
        auto o = decode(GX_TEXFMT_RGB5A3, s, 4, 4);
        expect_px("RGB5A3", o, 4, 0, 0, 255, 0, 0, 255);
        expect_px("RGB5A3", o, 4, 1, 0, 0xAA, 0xBB, 0xCC, 0x6D); // 3 -> 011 011 01
        expect_px("RGB5A3", o, 4, 2, 0, 0, 0, 0xFF, 0xFF);
    }
    // RGBA8: 32 bytes of AR pairs, then 32 of GB pairs.
    {
        std::vector<u8> s(64, 0);
        s[6] = 0x11; // texel 3: A
        s[7] = 0x22; //          R
        s[38] = 0x33; //         G
        s[39] = 0x44; //         B
        auto o = decode(GX_TEXFMT_RGBA8, s, 4, 4);
        expect_px("RGBA8", o, 4, 3, 0, 0x22, 0x33, 0x44, 0x11);
    }
    // CI4 / CI8 / CI14 with each TLUT format.
    {
        std::vector<u8> tlut(16384 * 2, 0);
        tlut[2 * 3] = 0x80; // entry 3: IA8 a=0x80 i=0x40 / RGB5A3 opaque
        tlut[2 * 3 + 1] = 0x40;
        tlut[2 * 0x1234] = 0xF8; // entry 0x1234: RGB565 red
        std::vector<u8> s4(32, 0);
        s4[0] = 0x03; // (1,0) = 3
        auto o = decode(GX_TEXFMT_CI4, s4, 8, 8, tlut.data(), GX_TLUTFMT_IA8);
        expect_px("CI4/IA8", o, 8, 1, 0, 0x40, 0x40, 0x40, 0x80);
        std::vector<u8> s8(32, 0);
        s8[9] = 3;
        o = decode(GX_TEXFMT_CI8, s8, 8, 4, tlut.data(), GX_TLUTFMT_RGB5A3);
        // 0x8040: opaque, r=0 g=2 b=0
        expect_px("CI8/RGB5A3", o, 8, 1, 1, 0, 16, 0, 255);
        std::vector<u8> s14(32, 0);
        s14[2] = 0xD2; // top two bits ignored -> 0x1234
        s14[3] = 0x34;
        o = decode(GX_TEXFMT_CI14, s14, 4, 4, tlut.data(), GX_TLUTFMT_RGB565);
        expect_px("CI14/RGB565", o, 4, 1, 0, 255, 0, 0, 255);
    }
    // CMPR: 8x8 tiles of four DXT1 blocks (TL, TR, BL, BR), big-endian
    // colours, 5/8-3/8 blends, and the 3-colour mode when c0 <= c1.
    {
        std::vector<u8> s(32, 0);
        // TL: c0 red > c1 blue, row 0 indices 0,1,2,3.
        s[0] = 0xF8, s[1] = 0x00, s[2] = 0x00, s[3] = 0x1F;
        s[4] = 0x1B; // 00 01 10 11
        // TR: c0 blue <= c1 red: index 2 average, 3 average + transparent.
        s[8] = 0x00, s[9] = 0x1F, s[10] = 0xF8, s[11] = 0x00;
        s[12 + 1] = 0xB0; // row 1: 10 11 00 00
        // BL: c0 == c1 (green): 3-colour mode.
        s[16] = 0x07, s[17] = 0xE0, s[18] = 0x07, s[19] = 0xE0;
        s[20 + 3] = 0x03; // row 3: (3,3) index 3
        // BR: white / black, row 2 col 1 = index 1.
        s[24] = 0xFF, s[25] = 0xFF, s[26] = 0x00, s[27] = 0x00;
        s[28 + 2] = 0x10;
        auto o = decode(GX_TEXFMT_CMPR, s, 8, 8);
        expect_px("CMPR", o, 8, 0, 0, 255, 0, 0, 255);
        expect_px("CMPR", o, 8, 1, 0, 0, 0, 255, 255);
        expect_px("CMPR", o, 8, 2, 0, 159, 0, 95, 255);
        expect_px("CMPR", o, 8, 3, 0, 95, 0, 159, 255);
        expect_px("CMPR", o, 8, 4, 1, 127, 0, 127, 255);
        expect_px("CMPR", o, 8, 5, 1, 127, 0, 127, 0);
        expect_px("CMPR", o, 8, 6, 1, 0, 0, 255, 255);
        expect_px("CMPR", o, 8, 3, 7, 0, 255, 0, 0);
        expect_px("CMPR", o, 8, 0, 7, 0, 255, 0, 255);
        expect_px("CMPR", o, 8, 5, 6, 0, 0, 0, 255);
        expect_px("CMPR", o, 8, 4, 6, 255, 255, 255, 255);
    }
}

// An independent tiler: places texel (x, y) of a linear image into Wii tile
// order for formats with one value per texel, then checks the decoder puts it
// back. Covers tile order and partial tiles at the right and bottom edges.
static void self_round_trip() {
    struct F {
        u32 fmt, bw, bh, bits;
    };
    const F fmts[] = {{GX_TEXFMT_I4, 8, 8, 4},     {GX_TEXFMT_I8, 8, 4, 8},     {GX_TEXFMT_IA4, 8, 4, 8},
                      {GX_TEXFMT_IA8, 4, 4, 16},   {GX_TEXFMT_RGB565, 4, 4, 16}, {GX_TEXFMT_RGB5A3, 4, 4, 16},
                      {GX_TEXFMT_RGBA8, 4, 4, 32}, {GX_TEXFMT_CI4, 8, 8, 4},    {GX_TEXFMT_CI8, 8, 4, 8},
                      {GX_TEXFMT_CI14, 4, 4, 16}};
    const u32 sizes[][2] = {{1, 1}, {13, 7}, {8, 8}, {33, 17}, {100, 3}, {4, 29}};
    // A palette whose entry i is RGB565 i, so CI output identifies the index.
    std::vector<u8> tlut(16384 * 2);
    for (u32 i = 0; i < 16384; i++) {
        tlut[i * 2] = (u8)(i >> 8);
        tlut[i * 2 + 1] = (u8)i;
    }
    srand(1234);
    for (const F& f : fmts) {
        for (const auto& sz : sizes) {
            u32 w = sz[0], h = sz[1];
            u32 tx = (w + f.bw - 1) / f.bw;
            std::vector<u32> val((size_t)w * h);
            std::vector<u8> src(gx_tex_data_size(f.fmt, w, h), 0);
            for (u32 y = 0; y < h; y++) {
                for (u32 x = 0; x < w; x++) {
                    u32 v = (u32)rand() ^ (u32)rand() << 11 ^ (u32)rand() << 22;
                    if (f.bits < 32) {
                        v &= (1u << f.bits) - 1;
                    }
                    if (f.fmt == GX_TEXFMT_CI14) {
                        v &= 0x3FFF;
                    }
                    val[(size_t)y * w + x] = v;
                    u32 tile = (y / f.bh) * tx + x / f.bw;
                    u32 i = (y % f.bh) * f.bw + x % f.bw;
                    u32 tileBytes = f.fmt == GX_TEXFMT_RGBA8 ? 64 : 32;
                    u8* t = &src[(size_t)tile * tileBytes];
                    switch (f.bits) {
                    case 4: t[i / 2] |= (u8)(v << ((i & 1) ? 0 : 4)); break;
                    case 8: t[i] = (u8)v; break;
                    case 16:
                        t[i * 2] = (u8)(v >> 8);
                        t[i * 2 + 1] = (u8)v;
                        break;
                    default: // RGBA8 as A R | G B
                        t[i * 2] = (u8)(v >> 24);
                        t[i * 2 + 1] = (u8)v;
                        t[32 + i * 2] = (u8)(v >> 8);
                        t[32 + i * 2 + 1] = (u8)(v >> 16);
                        break;
                    }
                }
            }
            auto o = decode(f.fmt, src, w, h, tlut.data(), GX_TLUTFMT_RGB565);
            int bad = 0;
            for (u32 y = 0; y < h && bad < 3; y++) {
                for (u32 x = 0; x < w && bad < 3; x++) {
                    u32 v = val[(size_t)y * w + x];
                    u32 r, g, b, a;
                    auto x4 = [](u32 n) { return n * 0x11; };
                    auto x5 = [](u32 n) { return n << 3 | n >> 2; };
                    auto x6 = [](u32 n) { return n << 2 | n >> 4; };
                    switch (f.fmt) {
                    case GX_TEXFMT_I4: r = g = b = a = x4(v); break;
                    case GX_TEXFMT_I8: r = g = b = a = v; break;
                    case GX_TEXFMT_IA4: r = g = b = x4(v & 15), a = x4(v >> 4); break;
                    case GX_TEXFMT_IA8: r = g = b = v & 0xFF, a = v >> 8; break;
                    case GX_TEXFMT_RGB5A3:
                        if (v & 0x8000) {
                            r = x5((v >> 10) & 31), g = x5((v >> 5) & 31), b = x5(v & 31), a = 255;
                        } else {
                            u32 a3 = (v >> 12) & 7;
                            r = x4((v >> 8) & 15), g = x4((v >> 4) & 15), b = x4(v & 15);
                            a = a3 << 5 | a3 << 2 | a3 >> 1;
                        }
                        break;
                    case GX_TEXFMT_RGBA8: r = v & 0xFF, g = (v >> 8) & 0xFF, b = (v >> 16) & 0xFF, a = v >> 24; break;
                    default: // RGB565, and CI through the RGB565 palette
                        r = x5(v >> 11), g = x6((v >> 5) & 63), b = x5(v & 31), a = 255;
                        break;
                    }
                    const u8* p = &o[((size_t)y * w + x) * 4];
                    if (p[0] != r || p[1] != g || p[2] != b || p[3] != a) {
                        printf("FAIL round trip %s %ux%u at (%u,%u): got %u,%u,%u,%u want %u,%u,%u,%u\n",
                               fmt_name(f.fmt), w, h, x, y, p[0], p[1], p[2], p[3], r, g, b, a);
                        sFailures++;
                        bad++;
                    }
                }
            }
        }
    }
}

// --- TPL files ----------------------------------------------------------------------

static u32 rd16(const std::vector<u8>& d, size_t o) {
    return o + 2 <= d.size() ? (u32)d[o] << 8 | d[o + 1] : 0;
}
static u32 rd32(const std::vector<u8>& d, size_t o) {
    return o + 4 <= d.size() ? (u32)d[o] << 24 | (u32)d[o + 1] << 16 | (u32)d[o + 2] << 8 | d[o + 3] : 0;
}

struct Stats {
    std::map<std::string, int> images;  // per format (CI: with TLUT format)
    std::map<std::string, std::set<std::string>> unique;
    int errors = 0;
};

static u64 fnv(const u8* p, size_t n) {
    u64 h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 1099511628211ull;
    }
    return h;
}

static void do_tpl(const fs::path& path, const fs::path& outDir, bool checker, Stats& st) {
    FILE* f = fopen(path.string().c_str(), "rb");
    if (f == nullptr) {
        printf("%s: cannot open\n", path.string().c_str());
        st.errors++;
        return;
    }
    std::vector<u8> d;
    fseek(f, 0, SEEK_END);
    d.resize((size_t)ftell(f));
    fseek(f, 0, SEEK_SET);
    size_t got = fread(d.data(), 1, d.size(), f);
    fclose(f);
    if (got != d.size() || rd32(d, 0) != 0x0020AF30) {
        printf("%s: not a TPL\n", path.string().c_str());
        st.errors++;
        return;
    }
    u32 n = rd32(d, 4), table = rd32(d, 8);
    for (u32 i = 0; i < n; i++) {
        u32 hdr = rd32(d, table + i * 8), palHdr = rd32(d, table + i * 8 + 4);
        u32 h = rd16(d, hdr), w = rd16(d, hdr + 2), fmt = rd32(d, hdr + 4), data = rd32(d, hdr + 8);
        u32 maxLod = hdr + 0x22 < d.size() ? d[hdr + 0x22] : 0;
        u32 size = gx_tex_data_size(fmt, w, h);
        std::string name = path.stem().string() + "_" + std::to_string(i) + "_" + fmt_name(fmt) + "_" +
                           std::to_string(w) + "x" + std::to_string(h);
        if (size == 0 || w == 0 || h == 0 || (size_t)data + size > d.size()) {
            printf("%s: bad image (fmt %u, %ux%u, data %x + %x, file %zx)\n", name.c_str(), fmt, w, h, data, size,
                   d.size());
            st.errors++;
            continue;
        }
        std::vector<u8> tlut;
        u32 tlutFmt = 0;
        std::string key = fmt_name(fmt);
        u32 entries = gx_tex_tlut_entries(fmt);
        if (entries != 0) {
            if (palHdr == 0) {
                printf("%s: CI image without a palette\n", name.c_str());
                st.errors++;
                continue;
            }
            u32 count = rd16(d, palHdr);
            tlutFmt = rd32(d, palHdr + 4);
            u32 palData = rd32(d, palHdr + 8);
            tlut.assign(16384 * 2, 0);
            for (u32 k = 0; k < count && k < 16384 && palData + k * 2 + 1 < d.size(); k++) {
                tlut[k * 2] = d[palData + k * 2];
                tlut[k * 2 + 1] = d[palData + k * 2 + 1];
            }
            key += std::string("/") + tlut_name(tlutFmt);
            name += std::string("_") + tlut_name(tlutFmt);
        }
        std::vector<u8> rgba((size_t)w * h * 4);
        if (!gx_tex_decode(fmt, d.data() + data, w, h, tlut.empty() ? nullptr : tlut.data(), tlutFmt, rgba.data())) {
            printf("%s: decode failed\n", name.c_str());
            st.errors++;
            continue;
        }
        st.images[key]++;
        st.unique[key].insert(std::to_string(fnv(rgba.data(), rgba.size())) + "_" + std::to_string(w));
        if (maxLod != 0) {
            name += "_mips";
        }
        if (checker) {
            for (u32 y = 0; y < h; y++) {
                for (u32 x = 0; x < w; x++) {
                    u8* p = &rgba[((size_t)y * w + x) * 4];
                    u32 bg = ((x / 8) ^ (y / 8)) & 1 ? 0x60 : 0xA0;
                    for (int c = 0; c < 3; c++) {
                        p[c] = (u8)((p[c] * p[3] + bg * (255 - p[3]) + 127) / 255);
                    }
                    p[3] = 255;
                }
            }
        }
        if (!write_png((outDir / (name + ".png")).string(), rgba.data(), w, h)) {
            printf("%s: cannot write PNG\n", name.c_str());
            st.errors++;
        }
    }
}

int main(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "--self") == 0) {
        self_known_values();
        self_round_trip();
        printf(sFailures ? "self test: %d failures\n" : "self test: all passed\n", sFailures);
        return sFailures ? 1 : 0;
    }
    int a = 1;
    bool checker = false;
    if (a < argc && strcmp(argv[a], "--checker") == 0) {
        checker = true;
        a++;
    }
    if (argc - a < 2) {
        fprintf(stderr, "usage: tex_test --self | tex_test [--checker] <png dir> <file.tpl | dir>...\n");
        return 2;
    }
    fs::path outDir = argv[a++];
    fs::create_directories(outDir);
    Stats st;
    int files = 0;
    for (; a < argc; a++) {
        fs::path p = argv[a];
        if (fs::is_directory(p)) {
            for (const auto& e : fs::directory_iterator(p)) {
                if (e.path().extension() == ".tpl") {
                    do_tpl(e.path(), outDir, checker, st);
                    files++;
                }
            }
        } else {
            do_tpl(p, outDir, checker, st);
            files++;
        }
    }
    printf("%d TPL files\n%-14s %7s %7s\n", files, "format", "images", "unique");
    for (const auto& kv : st.images) {
        printf("%-14s %7d %7zu\n", kv.first.c_str(), kv.second, st.unique[kv.first].size());
    }
    printf("%d errors\n", st.errors);
    return st.errors ? 1 : 0;
}
