// Tests port/gx/gx_texcodec.cpp: BC1 -> ASTC 4x4 transcoding against an
// independent ASTC decoder (astc_ref.cpp), and the BC1 encoder on real images.
//   texcodec_test <output dir> [image.png ...]
// Writes crops (original | BC1 | ASTC | BC1 error x8) as PNG into the output
// directory. Keep it, and the images, out of the repository.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <random>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "astc_ref.h"
#include "gx_texcodec.h"

namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
}

struct ErrStats {
    int maxErr[4] = {0, 0, 0, 0};
    double sum[4] = {0, 0, 0, 0};
    long long count = 0;

    void add(const uint8_t* a, const uint8_t* b, int channels) {
        for (int k = 0; k < channels; k++) {
            int e = abs((int)a[k] - (int)b[k]);
            if (e > maxErr[k]) {
                maxErr[k] = e;
            }
            sum[k] += e;
        }
        count++;
    }

    void print(const char* what) const {
        if (count == 0) {
            printf("  %-34s (none)\n", what);
            return;
        }
        printf("  %-34s max R %d G %d B %d, mean R %.4f G %.4f B %.4f  (%lld texels)\n", what, maxErr[0], maxErr[1],
               maxErr[2], sum[0] / count, sum[1] / count, sum[2] / count, count);
    }
};

// ---- Synthetic round trip ----------------------------------------------------

struct RoundTrip {
    ErrStats endpoints, interpolated, threeColour, threeColourDual, transparentVsAverage, transparentVsBlack;
    ErrStats endpointsRound, interpolatedRound;
    long long blocks = 0, alphaMismatch = 0, errors = 0;
    long long nVoid = 0, n4 = 0, n3 = 0, nDual = 0, nOther = 0;
    const char* firstError = nullptr;

    void run(const uint8_t bc1[8]) {
        uint8_t ref[64], astc[16], got[64], gotRound[64];
        gx_bc1_decode(bc1, 4, 4, ref);
        gx_bc1_to_astc4x4(bc1, 1, astc);
        astc_ref::Info info = astc_ref::decode_block(astc, 4, 4, got, gotRound);
        blocks++;
        if (info.error) {
            errors++;
            if (!firstError) {
                firstError = info.error;
            }
            return;
        }
        if (info.voidExtent) {
            nVoid++;
        } else if (info.dualPlane && info.cem == 12 && info.weightRange == 3 && info.endpointRange == 128) {
            nDual++;
        } else if (!info.dualPlane && info.cem == 8 && info.weightRange == 4 && info.endpointRange == 256) {
            n4++;
        } else if (!info.dualPlane && info.cem == 8 && info.weightRange == 3 && info.endpointRange == 256) {
            n3++;
        } else {
            nOther++;
        }
        unsigned c0 = bc1[0] | (bc1[1] << 8), c1 = bc1[2] | (bc1[3] << 8);
        uint32_t idx = (uint32_t)bc1[4] | ((uint32_t)bc1[5] << 8) | ((uint32_t)bc1[6] << 16) |
                       ((uint32_t)bc1[7] << 24);
        bool four = c0 > c1;
        // The 3-colour mode's average colour (gx_bc1_decode's index 2).
        uint8_t avg[4];
        {
            uint8_t pal[64];
            uint8_t blk[8];
            memcpy(blk, bc1, 4);
            uint32_t two = 0xAAAAAAAAu;
            memcpy(blk + 4, &two, 4);
            gx_bc1_decode(blk, 4, 4, pal);
            memcpy(avg, pal, 4);
        }
        for (int i = 0; i < 16; i++) {
            const uint8_t* r = ref + i * 4;
            const uint8_t* g = got + i * 4;
            const uint8_t* gr = gotRound + i * 4;
            unsigned ix = (idx >> (2 * i)) & 3;
            if (r[3] != g[3] || r[3] != gr[3]) {
                alphaMismatch++;
            }
            if (r[3] == 0) {
                transparentVsAverage.add(avg, g, 3);
                transparentVsBlack.add(r, g, 3);
            } else if (!four && info.dualPlane) {
                threeColourDual.add(r, g, 3);
            } else if (!four) {
                threeColour.add(r, g, 3);
            } else if (ix < 2) {
                endpoints.add(r, g, 3);
                endpointsRound.add(r, gr, 3);
            } else {
                interpolated.add(r, g, 3);
                interpolatedRound.add(r, gr, 3);
            }
        }
    }

    bool report() const {
        printf("Round trip: BC1 -> gx_bc1_decode vs BC1 -> gx_bc1_to_astc4x4 -> reference ASTC decoder\n");
        printf("  %lld blocks: void extent %lld, 4-colour (CEM 8, 4 weights) %lld, 3-colour (CEM 8, 3 weights) %lld,\n"
               "  dual plane (CEM 12, 3 weights, 7-bit endpoints) %lld, other %lld, decode errors %lld%s%s\n",
               blocks, nVoid, n4, n3, nDual, nOther, errors, firstError ? ": " : "", firstError ? firstError : "");
        endpoints.print("4-colour, index 0/1 texels");
        interpolated.print("4-colour, index 2/3 texels");
        threeColour.print("3-colour, opaque (CEM 8)");
        threeColourDual.print("3-colour, opaque (dual plane)");
        transparentVsAverage.print("transparent texels' RGB vs average");
        transparentVsBlack.print("transparent texels' RGB vs black");
        printf("  with the 16-bit result rounded to 8 bits instead of truncated:\n");
        endpointsRound.print("4-colour, index 0/1 texels");
        interpolatedRound.print("4-colour, index 2/3 texels");
        printf("  alpha mismatches: %lld\n", alphaMismatch);
        return errors == 0 && alphaMismatch == 0 && nOther == 0;
    }
};

void put_block(uint8_t* b, unsigned c0, unsigned c1, uint32_t idx) {
    b[0] = (uint8_t)c0;
    b[1] = (uint8_t)(c0 >> 8);
    b[2] = (uint8_t)c1;
    b[3] = (uint8_t)(c1 >> 8);
    b[4] = (uint8_t)idx;
    b[5] = (uint8_t)(idx >> 8);
    b[6] = (uint8_t)(idx >> 16);
    b[7] = (uint8_t)(idx >> 24);
}

bool synthetic() {
    RoundTrip rt;
    std::mt19937 rng(12345);
    uint8_t b[8];

    // Edge cases.
    static const unsigned special[] = {0x0000, 0xFFFF, 0xFFFE, 0x0001, 0xF800, 0x07E0, 0x001F, 0x8410,
                                       0x7BEF, 0xF81F, 0x07FF, 0xFFE0, 0x0020, 0x0800, 0x8000, 0x7FFF};
    for (unsigned a : special) {
        for (unsigned c : special) {
            for (unsigned i = 0; i < 4; i++) {
                put_block(b, a, c, i * 0x55555555u); // all indices equal
                rt.run(b);
            }
            put_block(b, a, c, 0xE4E4E4E4u); // 0, 1, 2, 3 per row
            rt.run(b);
            put_block(b, a, c, 0x24242424u); // 0, 1, 2, 0 (no index 3)
            rt.run(b);
            put_block(b, a, c, rng());
            rt.run(b);
        }
    }
    // Equal endpoints with random indices (3-colour mode, often transparent).
    for (int i = 0; i < 20000; i++) {
        unsigned c = rng() & 0xFFFF;
        put_block(b, c, c, rng());
        rt.run(b);
    }
    // Random blocks, each mode, with and without index 3 in 3-colour mode.
    for (int i = 0; i < 2000000; i++) {
        unsigned c0 = rng() & 0xFFFF, c1 = rng() & 0xFFFF;
        uint32_t idx = rng();
        switch (i & 3) {
        case 0: // 4-colour
            if (c0 < c1) {
                unsigned t = c0;
                c0 = c1;
                c1 = t;
            }
            break;
        case 1: // 3-colour, opaque: replace index 3 by 0..2
            if (c0 > c1) {
                unsigned t = c0;
                c0 = c1;
                c1 = t;
            }
            for (int k = 0; k < 16; k++) {
                if (((idx >> (2 * k)) & 3) == 3) {
                    idx &= ~(3u << (2 * k));
                    idx |= (uint32_t)(rng() % 3) << (2 * k);
                }
            }
            break;
        case 2: // 3-colour, transparent texels
            if (c0 > c1) {
                unsigned t = c0;
                c0 = c1;
                c1 = t;
            }
            break;
        default: // as drawn
            break;
        }
        put_block(b, c0, c1, idx);
        rt.run(b);
    }
    return rt.report();
}

// ---- Images ------------------------------------------------------------------

double psnr_from_mse(double mse) {
    return mse <= 0 ? 99.0 : 10.0 * log10(255.0 * 255.0 / mse);
}

// RGB mean squared error over the texels opaque in `orig` (alpha >= 128).
double rgb_mse(const uint8_t* orig, const uint8_t* test, uint32_t w, uint32_t h, uint32_t testW,
               long long* alphaWrong) {
    double sum = 0;
    long long n = 0, wrong = 0;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t* a = orig + ((size_t)y * w + x) * 4;
            const uint8_t* b = test + ((size_t)y * testW + x) * 4;
            bool opaque = a[3] >= 128;
            if (opaque != (b[3] == 255) || (!opaque && b[3] != 0)) {
                wrong++;
            }
            if (!opaque) {
                continue;
            }
            for (int k = 0; k < 3; k++) {
                double d = (double)a[k] - (double)b[k];
                sum += d * d;
            }
            n++;
        }
    }
    if (alphaWrong) {
        *alphaWrong = wrong;
    }
    return n ? sum / (3.0 * n) : 0;
}

// A 3x nearest-neighbour crop strip: original | BC1 | ASTC | |BC1 - original| x 8,
// transparent texels over a grey checkerboard.
void write_crop(const std::string& path, const uint8_t* orig, uint32_t w, const uint8_t* bc1, const uint8_t* astc,
                uint32_t padW, uint32_t cx, uint32_t cy, uint32_t cw, uint32_t ch) {
    const int S = 3;
    int outW = (int)cw * S * 4 + 3 * 4, outH = (int)ch * S;
    SDL_Surface* s = SDL_CreateSurface(outW, outH, SDL_PIXELFORMAT_ABGR8888);
    if (!s) {
        return;
    }
    SDL_FillSurfaceRect(s, nullptr, 0xFF000000u);
    uint8_t* dst = (uint8_t*)s->pixels;
    for (int panel = 0; panel < 4; panel++) {
        for (uint32_t y = 0; y < ch; y++) {
            for (uint32_t x = 0; x < cw; x++) {
                const uint8_t* o = orig + ((size_t)(cy + y) * w + cx + x) * 4;
                const uint8_t* b = bc1 + ((size_t)(cy + y) * padW + cx + x) * 4;
                const uint8_t* a = astc + ((size_t)(cy + y) * padW + cx + x) * 4;
                uint8_t c[4];
                if (panel == 3 && o[3] < 128) {
                    // Transparent: dark blue, or red if the decode isn't.
                    c[0] = b[3] != 0 ? 255 : 0;
                    c[1] = 0;
                    c[2] = b[3] != 0 ? 0 : 64;
                    c[3] = 255;
                } else if (panel == 3) {
                    for (int k = 0; k < 3; k++) {
                        int d = abs((int)o[k] - (int)b[k]) * 8;
                        c[k] = (uint8_t)(d > 255 ? 255 : d);
                    }
                    c[3] = 255;
                } else {
                    const uint8_t* p = panel == 0 ? o : panel == 1 ? b : a;
                    int alpha = panel == 0 ? (p[3] >= 128 ? 255 : 0) : p[3];
                    int bg = (((x / 4) + (y / 4)) & 1) ? 160 : 96;
                    for (int k = 0; k < 3; k++) {
                        c[k] = (uint8_t)((p[k] * alpha + bg * (255 - alpha)) / 255);
                    }
                    c[3] = 255;
                }
                for (int sy = 0; sy < S; sy++) {
                    for (int sx = 0; sx < S; sx++) {
                        int X = panel * ((int)cw * S + 4) + (int)x * S + sx, Y = (int)y * S + sy;
                        memcpy(dst + (size_t)Y * s->pitch + (size_t)X * 4, c, 4);
                    }
                }
            }
        }
    }
    if (!SDL_SavePNG(s, path.c_str())) {
        printf("  cannot write %s: %s\n", path.c_str(), SDL_GetError());
    }
    SDL_DestroySurface(s);
}

// The busiest crop (largest luma variance) on a coarse grid.
void busiest(const uint8_t* px, uint32_t w, uint32_t h, uint32_t cw, uint32_t ch, uint32_t* bx, uint32_t* by) {
    double best = -1;
    *bx = *by = 0;
    for (uint32_t y = 0; y + ch <= h; y += ch / 2) {
        for (uint32_t x = 0; x + cw <= w; x += cw / 2) {
            double s = 0, s2 = 0;
            for (uint32_t j = 0; j < ch; j += 2) {
                for (uint32_t i = 0; i < cw; i += 2) {
                    const uint8_t* p = px + ((size_t)(y + j) * w + x + i) * 4;
                    double l = 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
                    s += l;
                    s2 += l * l;
                }
            }
            double n = (double)(cw / 2) * (ch / 2), v = s2 / n - (s / n) * (s / n);
            if (v > best) {
                best = v;
                *bx = x & ~3u;
                *by = y & ~3u;
            }
        }
    }
}

struct Totals {
    double encodeSeconds = 0, transcodeSeconds = 0;
    double megapixels = 0;
};

void image(const std::string& path, const std::string& outDir, int number, Totals* totals) {
    SDL_Surface* loaded = SDL_LoadPNG(path.c_str());
    if (!loaded) {
        printf("%s: cannot load: %s\n", path.c_str(), SDL_GetError());
        return;
    }
    SDL_Surface* s = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_ABGR8888); // bytes R, G, B, A
    SDL_DestroySurface(loaded);
    if (!s) {
        return;
    }
    uint32_t w = (uint32_t)s->w, h = (uint32_t)s->h;
    std::vector<uint8_t> orig((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++) {
        memcpy(&orig[(size_t)y * w * 4], (const uint8_t*)s->pixels + (size_t)y * s->pitch, (size_t)w * 4);
    }
    // Encode straight from the surface (its pitch) as gx_texpack.cpp does.
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, padW = bw * 4, padH = bh * 4;
    size_t blocks = (size_t)bw * bh;
    std::vector<uint8_t> bc1(blocks * 8), astc(blocks * 16);
    double enc = 1e30;
    int runs = w * h >= 1024 * 1024 ? 3 : 20;
    for (int r = 0; r < runs; r++) {
        auto t = Clock::now();
        gx_bc1_encode((const uint8_t*)s->pixels, w, h, (uint32_t)s->pitch, bc1.data());
        double d = seconds_since(t);
        enc = d < enc ? d : enc;
    }
    SDL_DestroySurface(s);
    double tc = 1e30;
    for (int r = 0; r < runs; r++) {
        auto t = Clock::now();
        gx_bc1_to_astc4x4(bc1.data(), blocks, astc.data());
        double d = seconds_since(t);
        tc = d < tc ? d : tc;
    }
    totals->encodeSeconds += enc;
    totals->transcodeSeconds += tc;
    totals->megapixels += (double)w * h / 1e6;

    std::vector<uint8_t> dBc1((size_t)padW * padH * 4), dAstc((size_t)padW * padH * 4);
    gx_bc1_decode(bc1.data(), padW, padH, dBc1.data());
    long long errors = 0;
    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            uint8_t texels[64];
            astc_ref::Info info = astc_ref::decode_block(&astc[(by * bw + bx) * 16], 4, 4, texels);
            if (info.error) {
                errors++;
            }
            for (int y = 0; y < 4; y++) {
                memcpy(&dAstc[(((size_t)by * 4 + y) * padW + bx * 4) * 4], texels + y * 16, 16);
            }
        }
    }
    long long alphaBc1 = 0, alphaAstc = 0;
    double mseBc1 = rgb_mse(orig.data(), dBc1.data(), w, h, padW, &alphaBc1);
    double mseAstc = rgb_mse(orig.data(), dAstc.data(), w, h, padW, &alphaAstc);
    // ASTC vs BC1 decode (BC1 output as the "original").
    double mseBetween = 0;
    {
        double sum = 0;
        long long n = 0;
        for (size_t i = 0; i < (size_t)padW * padH; i++) {
            if (dBc1[i * 4 + 3] == 0) {
                continue;
            }
            for (int k = 0; k < 3; k++) {
                double d = (double)dBc1[i * 4 + k] - dAstc[i * 4 + k];
                sum += d * d;
            }
            n++;
        }
        mseBetween = n ? sum / (3.0 * n) : 0;
    }
    const char* name = strrchr(path.c_str(), '\\');
    const char* name2 = strrchr(path.c_str(), '/');
    name = name2 > name ? name2 : name;
    name = name ? name + 1 : path.c_str();
    printf("%s (%ux%u): encode %.1f ms (%.0f ns/block), transcode %.1f ms; PSNR BC1 %.2f dB, ASTC %.2f dB "
           "(%+.2f), ASTC vs BC1 %.2f dB; alpha wrong BC1 %lld ASTC %lld; ASTC decode errors %lld\n",
           name, w, h, enc * 1e3, enc * 1e9 / blocks, tc * 1e3, psnr_from_mse(mseBc1), psnr_from_mse(mseAstc),
           psnr_from_mse(mseAstc) - psnr_from_mse(mseBc1), psnr_from_mse(mseBetween), alphaBc1, alphaAstc, errors);

    // A size that isn't a multiple of 4 (edge blocks repeat the last row and
    // column): (w - 1) x (h - 3) of the same pixels, pitch w * 4.
    if (w > 4 && h > 4) {
        uint32_t ow = w - 1, oh = h - 3, obw = (ow + 3) / 4, obh = (oh + 3) / 4;
        std::vector<uint8_t> ob((size_t)obw * obh * 8), od((size_t)obw * obh * 64);
        gx_bc1_encode(orig.data(), ow, oh, w * 4, ob.data());
        gx_bc1_decode(ob.data(), obw * 4, obh * 4, od.data());
        std::vector<uint8_t> sub((size_t)ow * oh * 4);
        for (uint32_t y = 0; y < oh; y++) {
            memcpy(&sub[(size_t)y * ow * 4], &orig[(size_t)y * w * 4], (size_t)ow * 4);
        }
        long long wrong = 0;
        double mse = rgb_mse(sub.data(), od.data(), ow, oh, obw * 4, &wrong);
        printf("  %ux%u (not a multiple of 4): PSNR BC1 %.2f dB, alpha wrong %lld\n", ow, oh, psnr_from_mse(mse),
               wrong);
    }

    uint32_t cw = w < 96 ? w & ~3u : 96, ch = h < 96 ? h & ~3u : 96, cx, cy;
    if (cw >= 4 && ch >= 4) {
        busiest(orig.data(), w, h, cw, ch, &cx, &cy);
        char file[64];
        snprintf(file, sizeof file, "crop%02d.png", number);
        write_crop(outDir + "/" + file, orig.data(), w, dBc1.data(), dAstc.data(), padW, cx, cy, cw, ch);
        printf("  crop %s at (%u, %u) %ux%u\n", file, cx, cy, cw, ch);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: texcodec_test <output dir> [image.png ...]\n");
        return 2;
    }
    bool ok = synthetic();
    printf("\n");
    Totals totals;
    for (int i = 2; i < argc; i++) {
        image(argv[i], argv[1], i - 1, &totals);
    }
    if (argc > 2) {
        printf("\nTotal: %.1f Mpixel, encode %.0f ms (%.1f ns/pixel), transcode %.0f ms\n", totals.megapixels,
               totals.encodeSeconds * 1e3, totals.encodeSeconds * 1e3 / (totals.megapixels * 1e6) * 1e6,
               totals.transcodeSeconds * 1e3);
    }
    printf("%s\n", ok ? "synthetic round trip: OK" : "synthetic round trip: FAILED");
    return ok ? 0 : 1;
}
