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
#include "gx_texcodec.h"
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

// --- loading on worker threads ---------------------------------------------------
//
// Decoding a 2048x2048 PNG takes tens of milliseconds, so replacements are
// loaded by worker threads: the renderer asks for one (gx_texpack_request),
// keeps drawing the original texture, and picks the replacement up once it
// is ready (gx_texpack_take).

namespace {

const int kWorkers = 2;

SDL_Mutex* sLock;
SDL_Condition* sWake;
std::vector<std::pair<s32, std::string>> sQueue; // (id, path) waiting for a worker
std::unordered_map<s32, GXPackImage*> sReady;    // loaded, not yet taken
std::unordered_map<s32, bool> sInFlight;         // queued or being decoded
SDL_AtomicInt sComp;                             // GX_TEXCOMP_*
SDL_AtomicInt sCompressed;                       // statistics

// Images at least this large, a multiple of 4 in both directions and with no
// soft alpha are compressed (to BC1) when the GPU takes compressed formats.
const u32 kCompressMin = 256;

// True if every alpha value is (close to) 0 or 255: BC1's 1-bit alpha keeps it.
bool binary_alpha(const SDL_Surface* s) {
    for (int y = 0; y < s->h; y++) {
        const u8* row = (const u8*)s->pixels + (size_t)y * s->pitch;
        for (int x = 0; x < s->w; x++) {
            u8 a = row[x * 4 + 3];
            if (a > 8 && a < 247) {
                return false;
            }
        }
    }
    return true;
}

GXPackImage* load_png(const std::string& path) {
    SDL_Surface* s = SDL_LoadPNG(path.c_str());
    if (s == nullptr) {
        port_log("texpack: cannot load %s: %s\n", path.c_str(), SDL_GetError());
        return nullptr;
    }
    // Bytes R, G, B, A in memory, as the GPU texture expects.
    SDL_Surface* rgba = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ABGR8888);
    SDL_DestroySurface(s);
    if (rgba == nullptr) {
        return nullptr;
    }
    GXPackImage* img = new GXPackImage;
    img->w = (u32)rgba->w;
    img->h = (u32)rgba->h;
    img->bc1 = SDL_GetAtomicInt(&sComp) != GX_TEXCOMP_NONE && img->w >= kCompressMin && img->h >= kCompressMin &&
               (img->w & 3) == 0 && (img->h & 3) == 0 && binary_alpha(rgba);
    if (img->bc1) {
        img->data.resize((size_t)(img->w / 4) * (img->h / 4) * 8);
        gx_bc1_encode((const u8*)rgba->pixels, img->w, img->h, (u32)rgba->pitch, img->data.data());
        SDL_AddAtomicInt(&sCompressed, 1);
    } else {
        img->data.resize((size_t)img->w * img->h * 4);
        for (u32 y = 0; y < img->h; y++) {
            memcpy(&img->data[(size_t)y * img->w * 4], (const u8*)rgba->pixels + (size_t)y * rgba->pitch,
                   (size_t)img->w * 4);
        }
    }
    SDL_DestroySurface(rgba);
    return img;
}

int SDLCALL worker(void* unused) {
    (void)unused;
    for (;;) {
        SDL_LockMutex(sLock);
        while (sQueue.empty()) {
            SDL_WaitCondition(sWake, sLock);
        }
        s32 id = sQueue.front().first;
        std::string path = sQueue.front().second;
        sQueue.erase(sQueue.begin());
        SDL_UnlockMutex(sLock);

        GXPackImage* img = load_png(path);

        SDL_LockMutex(sLock);
        sInFlight.erase(id);
        if (img != nullptr) {
            sReady[id] = img;
        }
        SDL_UnlockMutex(sLock);
    }
    return 0;
}

void start_workers(void) {
    sLock = SDL_CreateMutex();
    sWake = SDL_CreateCondition();
    for (int i = 0; i < kWorkers; i++) {
        SDL_Thread* t = SDL_CreateThread(worker, "texpack", nullptr);
        if (t == nullptr) {
            port_log("texpack: worker thread failed: %s\n", SDL_GetError());
        } else {
            SDL_DetachThread(t);
        }
    }
}

} // namespace

void gx_texpack_set_compression(int comp) {
    SDL_SetAtomicInt(&sComp, comp);
}

void gx_texpack_request(s32 id) {
    if (id < 0 || (size_t)id >= sPaths.size()) {
        return;
    }
    if (sLock == nullptr) {
        start_workers();
    }
    SDL_LockMutex(sLock);
    if (sReady.find(id) == sReady.end() && sInFlight.find(id) == sInFlight.end()) {
        sInFlight[id] = true;
        sQueue.emplace_back(id, sPaths[id]);
        SDL_SignalCondition(sWake);
    }
    SDL_UnlockMutex(sLock);
}

GXPackImage* gx_texpack_take(s32 id) {
    if (sLock == nullptr) {
        return nullptr;
    }
    GXPackImage* img = nullptr;
    SDL_LockMutex(sLock);
    auto it = sReady.find(id);
    if (it != sReady.end()) {
        img = it->second;
        sReady.erase(it);
    }
    SDL_UnlockMutex(sLock);
    return img;
}
