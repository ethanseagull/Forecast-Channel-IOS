// Compresses a Dolphin texture pack for the phone: each tex1_*.png the app
// would compress to BC1 when it loads it (port/gx/gx_texpack.cpp: at least
// 256x256, both sides multiples of 4, no soft alpha) is written as a .dds
// (DXT1, the same encoder), which the app (and Dolphin) load as is; the
// other PNGs are copied unchanged. The phone then shows the same pixels as
// from the PNGs, the pack is about a quarter of the size, and loading skips
// the PNG decoding and BC1 encoding.
//
//   texpack_compress <pack dir> <output dir>
//
// Built by tools/texpack_compress/build.bat; used by tools/pack_data.ps1.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "gx_texcodec.h"

namespace {

int sDds, sPng, sFailed;
uint64_t sInBytes, sOutBytes;

bool binary_alpha(const SDL_Surface* s) {
    for (int y = 0; y < s->h; y++) {
        const uint8_t* row = (const uint8_t*)s->pixels + (size_t)y * s->pitch;
        for (int x = 0; x < s->w; x++) {
            uint8_t a = row[x * 4 + 3];
            if (a > 8 && a < 247) {
                return false;
            }
        }
    }
    return true;
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; i++) {
        v.push_back((uint8_t)(x >> (8 * i)));
    }
}

// A DDS file: the 128-byte header (DXT1, one level) and the blocks.
std::vector<uint8_t> dds_dxt1(uint32_t w, uint32_t h, const std::vector<uint8_t>& blocks) {
    std::vector<uint8_t> v;
    put32(v, 0x20534444); // "DDS "
    put32(v, 124);        // header size
    put32(v, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000); // caps, height, width, pixel format, linear size
    put32(v, h);
    put32(v, w);
    put32(v, (uint32_t)blocks.size());
    put32(v, 0); // depth
    put32(v, 1); // mip levels
    for (int i = 0; i < 11; i++) {
        put32(v, 0);
    }
    put32(v, 32);         // pixel format size
    put32(v, 0x4);        // FOURCC
    put32(v, 0x31545844); // "DXT1"
    for (int i = 0; i < 5; i++) {
        put32(v, 0);
    }
    put32(v, 0x1000); // texture
    for (int i = 0; i < 4; i++) {
        put32(v, 0);
    }
    v.insert(v.end(), blocks.begin(), blocks.end());
    return v;
}

bool write_file(const std::string& path, const void* data, size_t size) {
    return SDL_SaveFile(path.c_str(), data, size);
}

void make_dirs(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos) {
        SDL_CreateDirectory(path.substr(0, slash).c_str());
    }
}

void process(const std::string& in, const std::string& out) {
    size_t inSize = 0;
    void* raw = SDL_LoadFile(in.c_str(), &inSize);
    if (raw == nullptr) {
        fprintf(stderr, "cannot read %s\n", in.c_str());
        sFailed++;
        return;
    }
    sInBytes += inSize;
    SDL_Surface* s = SDL_LoadPNG(in.c_str());
    SDL_Surface* rgba = s != nullptr ? SDL_ConvertSurface(s, SDL_PIXELFORMAT_ABGR8888) : nullptr;
    if (s != nullptr) {
        SDL_DestroySurface(s);
    }
    make_dirs(out);
    bool bc1 = rgba != nullptr && rgba->w >= 256 && rgba->h >= 256 && (rgba->w & 3) == 0 && (rgba->h & 3) == 0 &&
               binary_alpha(rgba);
    if (bc1) {
        uint32_t w = (uint32_t)rgba->w, h = (uint32_t)rgba->h;
        std::vector<uint8_t> blocks((size_t)(w / 4) * (h / 4) * 8);
        gx_bc1_encode((const uint8_t*)rgba->pixels, w, h, (uint32_t)rgba->pitch, blocks.data());
        std::vector<uint8_t> file = dds_dxt1(w, h, blocks);
        std::string dds = out.substr(0, out.size() - 4) + ".dds";
        if (write_file(dds, file.data(), file.size())) {
            sDds++;
            sOutBytes += file.size();
        } else {
            sFailed++;
        }
    } else if (write_file(out, raw, inSize)) {
        sPng++;
        sOutBytes += inSize;
    } else {
        sFailed++;
    }
    if (rgba != nullptr) {
        SDL_DestroySurface(rgba);
    }
    SDL_free(raw);
}

struct Walk {
    std::string inRoot, outRoot;
};

SDL_EnumerationResult SDLCALL walk(void* user, const char* dirname, const char* fname) {
    Walk* w = (Walk*)user;
    std::string path = std::string(dirname) + fname;
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path.c_str(), &info)) {
        return SDL_ENUM_CONTINUE;
    }
    if (info.type == SDL_PATHTYPE_DIRECTORY) {
        SDL_EnumerateDirectory(path.c_str(), walk, user);
        return SDL_ENUM_CONTINUE;
    }
    size_t n = strlen(fname);
    if (n < 9 || SDL_strncasecmp(fname, "tex1_", 5) != 0 || SDL_strcasecmp(fname + n - 4, ".png") != 0) {
        return SDL_ENUM_CONTINUE;
    }
    std::string rel = path.substr(w->inRoot.size());
    process(path, w->outRoot + rel);
    return SDL_ENUM_CONTINUE;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: texpack_compress <pack dir> <output dir>\n");
        return 2;
    }
    Walk w;
    w.inRoot = argv[1];
    w.outRoot = argv[2];
    for (std::string* p : {&w.inRoot, &w.outRoot}) {
        for (char& c : *p) {
            if (c == '\\') {
                c = '/';
            }
        }
        if (!p->empty() && p->back() != '/') {
            *p += '/';
        }
    }
    SDL_CreateDirectory(w.outRoot.c_str());
    if (!SDL_EnumerateDirectory(w.inRoot.c_str(), walk, &w)) {
        fprintf(stderr, "cannot list %s: %s\n", w.inRoot.c_str(), SDL_GetError());
        return 1;
    }
    printf("%d DDS (BC1), %d PNG kept, %d failed; %.1f MB -> %.1f MB\n", sDds, sPng, sFailed,
           (double)sInBytes / 1048576.0, (double)sOutBytes / 1048576.0);
    return sFailed != 0 ? 1 : 0;
}
