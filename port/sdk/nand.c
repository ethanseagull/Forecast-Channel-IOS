// NAND on host files: the Wii path /a/b/c maps to <data dir>/nand/a/b/c.
// The channel uses it for its saved settings (location, units).
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include <revolution/NAND.h>

#include <port_log.h>
#include <port_paths.h>

#define NAND_MAX_OPEN 16
static FILE* sOpen[NAND_MAX_OPEN];

static void host_path(const char* path, char* out, size_t size) {
    SDL_snprintf(out, size, "%snand%s%s", port_data_dir(), path[0] == '/' ? "" : "/", path);
}

static void make_parent_dirs(const char* full) {
    char buf[1024];
    SDL_strlcpy(buf, full, sizeof(buf));
    char* slash = SDL_strrchr(buf, '/');
    if (slash != NULL) {
        *slash = 0;
        SDL_CreateDirectory(buf); // creates intermediate directories too
    }
}

s32 NANDCreate(const char* path, u8 perm, u8 attr) {
    char full[1024];
    (void)perm;
    (void)attr;
    host_path(path, full, sizeof(full));
    if (SDL_GetPathInfo(full, NULL)) {
        return NAND_RESULT_EXISTS;
    }
    make_parent_dirs(full);
    FILE* f = fopen(full, "wb");
    if (f == NULL) {
        port_log("NAND: cannot create %s\n", full);
        return NAND_RESULT_ACCESS;
    }
    fclose(f);
    return NAND_RESULT_OK;
}

s32 NANDCreateDir(const char* path, u8 perm, u8 attr) {
    char full[1024];
    (void)perm;
    (void)attr;
    host_path(path, full, sizeof(full));
    if (SDL_GetPathInfo(full, NULL)) {
        return NAND_RESULT_EXISTS;
    }
    return SDL_CreateDirectory(full) ? NAND_RESULT_OK : NAND_RESULT_ACCESS;
}

s32 NANDOpen(const char* path, NANDFileInfo* info, u8 mode) {
    char full[1024];
    int slot;
    const char* fmode = mode == NAND_ACCESS_READ ? "rb" : "r+b";
    host_path(path, full, sizeof(full));
    for (slot = 0; slot < NAND_MAX_OPEN && sOpen[slot] != NULL; slot++) {
    }
    if (slot == NAND_MAX_OPEN) {
        return NAND_RESULT_MAXFD;
    }
    FILE* f = fopen(full, fmode);
    if (f == NULL) {
        return NAND_RESULT_NOEXISTS;
    }
    sOpen[slot] = f;
    memset(info, 0, sizeof(*info));
    info->fd = slot;
    info->access = mode;
    SDL_strlcpy(info->openPath, path, sizeof(info->openPath));
    return NAND_RESULT_OK;
}

static FILE* info_file(NANDFileInfo* info) {
    return info->fd >= 0 && info->fd < NAND_MAX_OPEN ? sOpen[info->fd] : NULL;
}

s32 NANDClose(NANDFileInfo* info) {
    FILE* f = info_file(info);
    if (f == NULL) {
        return NAND_RESULT_INVALID;
    }
    fclose(f);
    sOpen[info->fd] = NULL;
    info->fd = -1;
    return NAND_RESULT_OK;
}

s32 NANDRead(NANDFileInfo* info, void* buf, u32 len) {
    FILE* f = info_file(info);
    return f == NULL ? NAND_RESULT_INVALID : (s32)fread(buf, 1, len, f);
}

s32 NANDWrite(NANDFileInfo* info, const void* buf, u32 len) {
    FILE* f = info_file(info);
    if (f == NULL) {
        return NAND_RESULT_INVALID;
    }
    s32 n = (s32)fwrite(buf, 1, len, f);
    fflush(f);
    return n;
}

// Referenced only through nw4r::ut::NandFileStream's vtable, which the channel
// never uses: fail.
s32 NANDGetLength(NANDFileInfo* info, u32* length) {
    (void)info;
    *length = 0;
    return NAND_RESULT_FATAL_ERROR;
}

s32 NANDReadAsync(NANDFileInfo* info, void* buf, u32 len, NANDAsyncCallback callback, NANDCommandBlock* block) {
    (void)info;
    (void)buf;
    (void)len;
    (void)callback;
    (void)block;
    return NAND_RESULT_FATAL_ERROR;
}

s32 NANDWriteAsync(NANDFileInfo* info, const void* buf, u32 len, NANDAsyncCallback callback,
                   NANDCommandBlock* block) {
    (void)info;
    (void)buf;
    (void)len;
    (void)callback;
    (void)block;
    return NAND_RESULT_FATAL_ERROR;
}
