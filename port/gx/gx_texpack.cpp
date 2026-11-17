// Texture packs in Dolphin's format: PNG files named
//   tex1_<w>x<h>[_m]_<XXH64 of the texels>[_<XXH64 of the used palette>]_<format>.png
// anywhere under <data>/textures/. A texture whose name matches is drawn from
// the PNG (usually upscaled) instead of the decoded Wii image. The name is
// computed exactly as Dolphin's TextureInfo::CalculateTextureName does, so
// packs made for Dolphin work unchanged.
#include <stdlib.h>
#include <string.h>

#include <string>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>

#include "gx_internal.h"
#include "gx_tex_decode.h"
#include <port_log.h>
#include <port_paths.h>

namespace {

// --- XXH64 (seed 0 in use), after the reference implementation ----------------

const u64 P1 = 0x9E3779B185EBCA87ull, P2 = 0xC2B2AE3D27D4EB4Full, P3 = 0x165667B19E3779F9ull,
          P4 = 0x85EBCA77C2B2AE63ull, P5 = 0x27D4EB2F165667C5ull;

inline u64 rotl(u64 x, int r) {
    return (x << r) | (x >> (64 - r));
}
inline u64 read64(const u8* p) {
    u64 v;
    memcpy(&v, p, 8);
    return v; // little-endian hosts (Windows x64, iOS arm64)
}
inline u32 read32(const u8* p) {
    u32 v;
    memcpy(&v, p, 4);
    return v;
}
inline u64 round(u64 acc, u64 input) {
    acc += input * P2;
    acc = rotl(acc, 31);
    return acc * P1;
}
inline u64 merge(u64 acc, u64 val) {
    acc ^= round(0, val);
    return acc * P1 + P4;
}

u64 xxh64(const u8* p, size_t len, u64 seed) {
    const u8* end = p + len;
    u64 h;
    if (len >= 32) {
        u64 v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
        const u8* limit = end - 32;
        do {
            v1 = round(v1, read64(p));
            v2 = round(v2, read64(p + 8));
            v3 = round(v3, read64(p + 16));
            v4 = round(v4, read64(p + 24));
            p += 32;
        } while (p <= limit);
        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
        h = merge(h, v1);
        h = merge(h, v2);
        h = merge(h, v3);
        h = merge(h, v4);
    } else {
        h = seed + P5;
    }
    h += (u64)len;
    while (p + 8 <= end) {
        h ^= round(0, read64(p));
        h = rotl(h, 27) * P1 + P4;
        p += 8;
    }
    if (p + 4 <= end) {
        h ^= (u64)read32(p) * P1;
        h = rotl(h, 23) * P2 + P3;
        p += 4;
    }
    while (p < end) {
        h ^= (u64)(*p) * P5;
        h = rotl(h, 11) * P1;
        p++;
    }
    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    h ^= h >> 32;
    return h;
}

// --- the pack index --------------------------------------------------------------

std::unordered_map<std::string, std::string> sFiles; // lower-case name without .png -> path
std::vector<std::string> sPaths;                     // by replacement id
std::unordered_map<std::string, s32> sIds;           // path -> replacement id
bool sScanned;

SDL_EnumerationResult SDLCALL scan_dir(void* userdata, const char* dirname, const char* fname) {
    (void)userdata;
    std::string path = std::string(dirname) + fname;
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path.c_str(), &info)) {
        return SDL_ENUM_CONTINUE;
    }
    if (info.type == SDL_PATHTYPE_DIRECTORY) {
        SDL_EnumerateDirectory(path.c_str(), scan_dir, nullptr);
        return SDL_ENUM_CONTINUE;
    }
    size_t n = strlen(fname);
    if (n < 9 || SDL_strncasecmp(fname, "tex1_", 5) != 0 || SDL_strcasecmp(fname + n - 4, ".png") != 0) {
        return SDL_ENUM_CONTINUE;
    }
    std::string key(fname, n - 4);
    for (char& c : key) {
        c = (char)SDL_tolower((unsigned char)c);
    }
    // Dolphin lets the first file found win; so do we.
    sFiles.emplace(key, path);
    return SDL_ENUM_CONTINUE;
}

void scan(void) {
    sScanned = true;
    const char* off = SDL_getenv("FORECAST_NO_TEXPACKS");
    if (off != NULL && off[0] == '1') {
        port_log("texpack: off (FORECAST_NO_TEXPACKS)\n");
        return;
    }
    std::string root = std::string(port_data_dir()) + "textures";
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(root.c_str(), &info) || info.type != SDL_PATHTYPE_DIRECTORY) {
        port_log("texpack: no %s; using the original textures\n", root.c_str());
        return;
    }
    SDL_EnumerateDirectory(root.c_str(), scan_dir, nullptr);
    port_log("texpack: %u replacement textures under %s\n", (unsigned)sFiles.size(), root.c_str());
}

} // namespace

s32 gx_texpack_lookup(u32 fmt, u32 w, u32 h, bool mipmaps, const u8* texels, u32 size, const u8* pal,
                      u32 palEntries) {
    if (!sScanned) {
        scan();
    }
    if (sFiles.empty()) {
        return -1;
    }
    // The palette part covers only the entries the texels use (Dolphin).
    u64 palHash = 0;
    size_t palBytes = 0;
    if (pal != nullptr && palEntries != 0) {
        u32 lo = 0xFFFF, hi = 0;
        if (palEntries == 16) {
            for (u32 i = 0; i < size; i++) {
                u32 a = texels[i] & 0xF, b = texels[i] >> 4;
                lo = SDL_min(lo, SDL_min(a, b));
                hi = SDL_max(hi, SDL_max(a, b));
            }
        } else if (palEntries == 256) {
            for (u32 i = 0; i < size; i++) {
                lo = SDL_min(lo, (u32)texels[i]);
                hi = SDL_max(hi, (u32)texels[i]);
            }
        } else {
            // CI14: Dolphin reads the first byte of each halfword, swapped
            // (Common::swap16 of a u8), i.e. the byte in the high half.
            for (u32 i = 0; i < size; i += 2) {
                u32 v = ((u32)texels[i] << 8) & 0x3FFF;
                lo = SDL_min(lo, v);
                hi = SDL_max(hi, v);
            }
        }
        if (lo <= hi) {
            palBytes = 2 * (size_t)(hi + 1 - lo);
            palHash = xxh64(pal + 2 * lo, palBytes, 0);
        }
    }
    char name[96];
    u64 texHash = xxh64(texels, size, 0);
    if (palBytes != 0) {
        SDL_snprintf(name, sizeof(name), "tex1_%ux%u%s_%016llx_%016llx_%u", w, h, mipmaps ? "_m" : "",
                     (unsigned long long)texHash, (unsigned long long)palHash, fmt);
    } else {
        SDL_snprintf(name, sizeof(name), "tex1_%ux%u%s_%016llx_%u", w, h, mipmaps ? "_m" : "",
                     (unsigned long long)texHash, fmt);
    }
    auto it = sFiles.find(name);
    if (it == sFiles.end() && palBytes != 0) {
        // Dolphin also accepts '$' for "any palette".
        SDL_snprintf(name, sizeof(name), "tex1_%ux%u%s_%016llx_$_%u", w, h, mipmaps ? "_m" : "",
                     (unsigned long long)texHash, fmt);
        it = sFiles.find(name);
    }
    if (it == sFiles.end()) {
        return -1;
    }
    auto id = sIds.find(it->second);
    if (id != sIds.end()) {
        return id->second;
    }
    sPaths.push_back(it->second);
    s32 n = (s32)sPaths.size() - 1;
    sIds.emplace(it->second, n);
    port_log("texpack: replaced %s\n", name);
    return n;
}

SDL_Surface* gx_texpack_load(s32 id) {
    if (id < 0 || (size_t)id >= sPaths.size()) {
        return nullptr;
    }
    SDL_Surface* s = SDL_LoadPNG(sPaths[id].c_str());
    if (s == nullptr) {
        port_log("texpack: cannot load %s: %s\n", sPaths[id].c_str(), SDL_GetError());
        return nullptr;
    }
    // Bytes R, G, B, A in memory, as the GPU texture expects.
    SDL_Surface* rgba = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ABGR8888);
    SDL_DestroySurface(s);
    return rgba;
}
