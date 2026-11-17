// The user's channel files: importing them, and reading data out of the DOL.
//
// The port never ships Nintendo data. The user supplies the Forecast Channel
// (HAFE, USA v7) contents: 00000001.app (the DOL) and 00000006.app ..
// 0000000a.app (U8 archives). On iOS they are copied into the app's Documents
// folder with the Files app; on Windows into the data folder. At start-up any
// archive not unpacked yet is unpacked to <data>/content/0000000N/, and the DOL
// is copied to <data>/content/00000001.app.
//
// Data the decomp normally compiles in from the DOL (the error screen's
// layout archive, the globe's dot tables) is read from it here instead.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <port_import.h>
#include <port_log.h>
#include <port_paths.h>

static uint32_t be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static bool exists(const char* path) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info);
}

static bool write_file(const char* path, const void* data, size_t size) {
    SDL_IOStream* io = SDL_IOFromFile(path, "wb");
    if (io == NULL) {
        return false;
    }
    bool ok = SDL_WriteIO(io, data, size) == size;
    return SDL_CloseIO(io) && ok;
}

// --- U8 archives -------------------------------------------------------------------

// Unpacks a U8 archive (magic 55 AA 38 2D) into dir. Nodes are 12 bytes:
// type (0 file, 1 directory), 24-bit name offset, data offset (directory:
// parent index), size (directory: index one past its last child).
static bool unpack_u8(const uint8_t* a, size_t size, const char* dir) {
    if (size < 0x20 || be32(a) != 0x55AA382D) {
        return false;
    }
    uint32_t rootOff = be32(a + 4);
    if (rootOff + 12 > size) {
        return false;
    }
    const uint8_t* root = a + rootOff;
    uint32_t count = be32(root + 8);
    if (count == 0 || rootOff + (uint64_t)count * 12 > size) {
        return false;
    }
    const char* names = (const char*)(root + count * 12);
    size_t namesMax = size - (rootOff + (size_t)count * 12);

    // Directory stack: path and the node index where each directory ends.
    char path[64][512];
    uint32_t end[64];
    int depth = 0;
    snprintf(path[0], sizeof(path[0]), "%s", dir);
    end[0] = count;
    SDL_CreateDirectory(dir);

    for (uint32_t i = 1; i < count; i++) {
        while (depth > 0 && i >= end[depth]) {
            depth--;
        }
        const uint8_t* n = root + i * 12;
        uint32_t nameOff = be32(n) & 0xFFFFFF;
        if (nameOff >= namesMax) {
            return false;
        }
        const char* name = names + nameOff;
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", path[depth], name);
        if (n[0] == 1) {
            SDL_CreateDirectory(full);
            if (depth + 1 >= 64) {
                return false;
            }
            depth++;
            snprintf(path[depth], sizeof(path[depth]), "%s", full);
            end[depth] = be32(n + 8);
        } else {
            uint32_t off = be32(n + 4), len = be32(n + 8);
            if ((uint64_t)off + len > size || !write_file(full, a + off, len)) {
                return false;
            }
        }
    }
    return true;
}

// Looks for a content file in the places a user may put it: the data folder
// itself, or an "import" folder in it.
static bool find_app(const char* name, char* out, size_t len) {
    const char* data = port_data_dir();
    snprintf(out, len, "%s%s", data, name);
    if (exists(out)) {
        return true;
    }
    snprintf(out, len, "%simport/%s", data, name);
    return exists(out);
}

void port_import_contents(void) {
    const char* data = port_data_dir();
    char content[512];
    snprintf(content, sizeof(content), "%scontent", data);
    SDL_CreateDirectory(content);

    static const char* const archives[] = {"00000006", "00000007", "00000008", "00000009", "0000000a"};
    for (size_t i = 0; i < SDL_arraysize(archives); i++) {
        char dir[512], app[512], name[32];
        snprintf(dir, sizeof(dir), "%s/%s", content, archives[i]);
        if (exists(dir)) {
            continue;
        }
        snprintf(name, sizeof(name), "%s.app", archives[i]);
        if (!find_app(name, app, sizeof(app))) {
            port_log("import: %s missing (copy the channel's contents into %s)\n", name, data);
            continue;
        }
        size_t size = 0;
        uint8_t* a = (uint8_t*)SDL_LoadFile(app, &size);
        char tmp[520];
        snprintf(tmp, sizeof(tmp), "%s.part", dir);
        bool ok = a != NULL && unpack_u8(a, size, tmp) && SDL_RenamePath(tmp, dir);
        SDL_free(a);
        port_log("import: %s -> %s%s\n", name, dir, ok ? "" : " FAILED");
    }

    char dol[512], app[512];
    snprintf(dol, sizeof(dol), "%s/00000001.app", content);
    if (!exists(dol)) {
        if (find_app("00000001.app", app, sizeof(app))) {
            bool ok = SDL_CopyFile(app, dol);
            port_log("import: 00000001.app (DOL)%s\n", ok ? "" : " FAILED");
        } else {
            port_log("import: 00000001.app missing (copy the channel's contents into %s)\n", data);
        }
    }
}

// --- the DOL -------------------------------------------------------------------------

static uint8_t* sDol;
static size_t sDolSize;

bool port_dol_read(uint32_t addr, void* dst, uint32_t size) {
    if (sDol == NULL) {
        char path[512];
        snprintf(path, sizeof(path), "%scontent/00000001.app", port_data_dir());
        sDol = (uint8_t*)SDL_LoadFile(path, &sDolSize);
        if (sDol == NULL || sDolSize < 0x100) {
            port_log("dol: cannot read %s\n", path);
            SDL_free(sDol);
            sDol = NULL;
            return false;
        }
    }
    // 7 text and 11 data sections: file offsets at 0x00, load addresses at
    // 0x48, sizes at 0x90.
    for (int s = 0; s < 18; s++) {
        uint32_t off = be32(sDol + s * 4), base = be32(sDol + 0x48 + s * 4), len = be32(sDol + 0x90 + s * 4);
        if (len != 0 && addr >= base && (uint64_t)addr + size <= (uint64_t)base + len) {
            if ((uint64_t)off + (addr - base) + size > sDolSize) {
                break;
            }
            memcpy(dst, sDol + off + (addr - base), size);
            return true;
        }
    }
    port_log("dol: %08x (+%x) not in the DOL\n", (unsigned)addr, (unsigned)size);
    return false;
}

void port_dol_release(void) {
    SDL_free(sDol);
    sDol = NULL;
}

// The DOL data the decomp compiles in on the Wii (addresses from
// forecast-decomp/config/HAFE/symbols.txt).
extern uint8_t gErrorLayoutArc[0x175C0];
extern uint8_t gGlobeDotPositions[0x8E20];
extern uint8_t gGlobeDotSizes[0x2388];
extern uint8_t gGlobeDotColorIndices[0x2388];
extern uint8_t gGlobeDotColors[0x6A98];

bool port_load_dol_data(void) {
    bool ok = port_dol_read(0x8019C6C0, gErrorLayoutArc, sizeof(gErrorLayoutArc)) &&
              port_dol_read(0x8017B508, gGlobeDotPositions, sizeof(gGlobeDotPositions)) &&
              port_dol_read(0x80184328, gGlobeDotSizes, sizeof(gGlobeDotSizes)) &&
              port_dol_read(0x801866B0, gGlobeDotColorIndices, sizeof(gGlobeDotColorIndices)) &&
              port_dol_read(0x80188A38, gGlobeDotColors, sizeof(gGlobeDotColors));
    port_dol_release();
    // The error layout is a U8 archive: a cheap check that the DOL is the
    // right one.
    if (ok && be32(gErrorLayoutArc) != 0x55AA382D) {
        port_log("dol: the error layout is not a U8 archive: wrong DOL (need HAFE v7)?\n");
        ok = false;
    }
    port_log("dol: tables %s\n", ok ? "loaded" : "NOT loaded");
    return ok;
}
