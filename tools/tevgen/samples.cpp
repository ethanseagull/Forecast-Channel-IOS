// Writes the specialized TEV pixel shaders (port/gx/gx_tevgen.cpp) for a
// list of keys, in MSL and HLSL, so they can be compile-checked where the
// app cannot run: the iOS workflow compiles the .metal files with Apple's
// metal compiler (.github/workflows/ios.yml), build.bat the .hlsl ones with
// the Windows SDK's dxc.
//
//   samples <keys.txt> <outdir> [random count]
//
// keys.txt: one key a line as gx_tevgen's text form (FORECAST_SPECIALIZED_DUMP
// writes the keys a run uses to keys.txt); '#' starts a comment. With a count,
// that many keys made from random registers (fixed seed) are added, to reach
// the generator's rarer branches.
//
// Standard C++ only: builds on macOS, Linux and Windows.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "gx_tevgen.h"

static uint32_t sRand = 12345;

static uint32_t rnd(void) {
    // xorshift32
    sRand ^= sRand << 13;
    sRand ^= sRand >> 17;
    sRand ^= sRand << 5;
    return sRand;
}

static bool write_file(const std::string& path, const std::string& text) {
    FILE* f = fopen(path.c_str(), "wb");
    if (f == NULL) {
        fprintf(stderr, "cannot write %s\n", path.c_str());
        return false;
    }
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    return true;
}

static bool emit(const std::string& dir, int n, const TevKey& k) {
    char name[64];
    snprintf(name, sizeof(name), "/tev_%03d", n);
    return write_file(dir + name + ".metal", tevgen_source(k, TEVGEN_MSL)) &&
           write_file(dir + name + ".hlsl", tevgen_source(k, TEVGEN_HLSL));
}

// Random registers that stay in the ranges the hardware defines (16 stages,
// 4 indirect stages) with every field random, so the key covers everything.
void random_regs(TevGenRegs* r) {
    memset(r, 0, sizeof(*r));
    for (int i = 0; i < 16; i++) {
        r->colorEnv[i] = rnd() & 0xFFFFFF;
        r->alphaEnv[i] = rnd() & 0xFFFFFF;
        r->tevInd[i] = (rnd() & 7) == 0 ? 0 : rnd() & 0x1FFFFF;
    }
    for (int i = 0; i < 8; i++) {
        r->tevOrder[i] = rnd() & 0xFFFFFF;
        r->ksel[i] = rnd() & 0xFFFFFF;
    }
    r->genMode = (rnd() & 15) << 10 | (rnd() % 5) << 16;
    r->alphaCompare = rnd() & 0xFFFFFF;
    r->texBound = rnd() & 0xFF;
    r->indRef = rnd() & 0xFFFFFF;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: samples <keys.txt> <outdir> [random count]\n");
        return 2;
    }
    std::string dir = argv[2];
    int n = 0;
    FILE* f = fopen(argv[1], "rb");
    if (f == NULL) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    char line[4096];
    while (fgets(line, sizeof(line), f) != NULL) {
        const char* p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == 0) {
            continue;
        }
        TevKey k;
        if (!tevgen_key_from_text(p, &k)) {
            fprintf(stderr, "bad key: %s", line);
            fclose(f);
            return 1;
        }
        if (!emit(dir, n++, k)) {
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    int count = argc > 3 ? atoi(argv[3]) : 0;
    for (int i = 0; i < count; i++) {
        TevGenRegs r;
        random_regs(&r);
        TevKey k;
        tevgen_make_key(r, &k);
        if (!emit(dir, n++, k)) {
            return 1;
        }
    }
    printf("%d shaders written to %s\n", n, dir.c_str());
    return 0;
}
