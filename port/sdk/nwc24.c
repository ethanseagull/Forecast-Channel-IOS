// WiiConnect24 downloads (NWC24) and their virtual file system (VF).
//
// On the Wii, the channel registers download tasks with NWC24 (URL, file
// name, interval), NWC24 fetches each URL, checks the RSA signature, strips
// the 0x140-byte header and stores the LZ-compressed payload as a file in a
// FAT image (the VF), and the channel reads that file back
// (forecast-decomp/src/WiiConnect24.cpp).
//
// Here the task table lives in memory, NWC24ExecDownloadTask is a plain HTTP
// GET, and the VF is a directory: <data>/wc24/<file> plus <file>.time (the
// OSTime of the download). A failed download keeps the last good file, so the
// channel still has (older) data offline. The signature is not checked.
//
// The channel builds Nintendo's URLs (weather.wapp.wii.com); they are sent to
// WiiLink's replacement service instead (FORECAST_WC24_HOST overrides it).
#include <revolution/OS.h>
#include <revolution/NWC24/NWC24Download.h>
#include <revolution/NWC24/NWC24Manage.h>
#include <revolution/NWC24/internal/NWC24iDownload.h>
#include <revolution/NWC24/internal/NWC24iManage.h>
#include <revolution/VF/d_vf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <port_http.h>
#include <port_log.h>
#include <port_paths.h>

#define WC24_HEADER_SIZE 0x140
#define NINTENDO_HOST "weather.wapp.wii.com"
#define WIILINK_HOST "fore.wiilink24.com"

// --- download tasks ----------------------------------------------------------------

// What a task holds; kept inside NWC24DlTask's opaque 0x200 bytes while the
// channel edits it, and in the table once added.
typedef struct PortDlTask {
    u32 magic;
    NWC24DlId id;
    u16 interval; // minutes
    s16 count;
    u8 type;
    u8 pad;
    s64 lastUpdate;
    s64 nextTime;
    char url[0x100];
    char filename[0x20];
} PortDlTask;

#define TASK_MAGIC 0x50444C54 // 'PDLT'
#define MAX_TASKS 120

typedef char check_task_size[sizeof(PortDlTask) <= NWC24_DL_TASK_PUBLIC_SIZE ? 1 : -1];

static PortDlTask sTasks[MAX_TASKS];
static bool sUsed[MAX_TASKS];
static s32 sErrorCode;

static PortDlTask* T(NWC24DlTask* t) {
    return (PortDlTask*)t->data;
}

static int find_task(NWC24DlId id) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (sUsed[i] && sTasks[i].id == id) {
            return i;
        }
    }
    return -1;
}

NWC24Err NWC24OpenLib(NWC24Work* work) {
    (void)work;
    return NWC24_OK;
}
NWC24Err NWC24CloseLib(void) { return NWC24_OK; }
NWC24Err NWC24Check(u32 usage) {
    (void)usage;
    return NWC24_OK;
}
s32 NWC24GetErrorCode(void) { return sErrorCode; }
NWC24Err NWC24iCheckDlTask(NWC24DlTask* t) {
    (void)t;
    return NWC24_OK;
}

NWC24Err NWC24GetDlTask(NWC24DlTask* t, NWC24DlId id) {
    int i = find_task(id);
    if (i < 0) {
        return NWC24_ERR_NOT_FOUND;
    }
    memset(t, 0, sizeof(*t));
    *T(t) = sTasks[i];
    return NWC24_OK;
}

NWC24Err NWC24InitDlTask(NWC24DlTask* t, NWC24DlType type) {
    memset(t, 0, sizeof(*t));
    T(t)->magic = TASK_MAGIC;
    T(t)->type = (u8)type;
    T(t)->interval = 60;
    return NWC24_OK;
}

NWC24Err NWC24SetDlId(NWC24DlTask* t, NWC24DlId id) {
    T(t)->id = id;
    return NWC24_OK;
}
NWC24Err NWC24GetDlId(NWC24DlTask* t, NWC24DlId* id) {
    *id = T(t)->id;
    return NWC24_OK;
}
NWC24Err NWC24SetDlPriority(NWC24DlTask* t, u8 priority) {
    (void)t;
    (void)priority;
    return NWC24_OK;
}
NWC24Err NWC24SetDlInterval(NWC24DlTask* t, u16 interval) {
    T(t)->interval = interval;
    return NWC24_OK;
}
NWC24Err NWC24GetDlInterval(NWC24DlTask* t, u16* interval) {
    *interval = T(t)->interval;
    return NWC24_OK;
}
NWC24Err NWC24SetDlRetryMargin(NWC24DlTask* t, u16 margin) {
    (void)t;
    (void)margin;
    return NWC24_OK;
}
NWC24Err NWC24SetDlFlags(NWC24DlTask* t, u32 flags) {
    (void)t;
    (void)flags;
    return NWC24_OK;
}
NWC24Err NWC24SetDlCount(NWC24DlTask* t, s16 count) {
    T(t)->count = count;
    return NWC24_OK;
}

static NWC24Err copy_out(const char* src, char* dst, u32 len) {
    size_t n = strlen(src);
    if (n + 1 > len) {
        return NWC24_ERR_OVERFLOW;
    }
    memcpy(dst, src, n + 1);
    return NWC24_OK;
}

NWC24Err NWC24SetDlUrl(NWC24DlTask* t, const char* url) {
    if (strlen(url) >= sizeof(T(t)->url)) {
        return NWC24_ERR_OVERFLOW;
    }
    strcpy(T(t)->url, url);
    return NWC24_OK;
}
NWC24Err NWC24GetDlUrl(NWC24DlTask* t, char* url, u32 len) { return copy_out(T(t)->url, url, len); }
NWC24Err NWC24SetDlFilename(NWC24DlTask* t, const char* name) {
    if (strlen(name) >= sizeof(T(t)->filename)) {
        return NWC24_ERR_OVERFLOW;
    }
    strcpy(T(t)->filename, name);
    return NWC24_OK;
}
NWC24Err NWC24GetDlFilename(NWC24DlTask* t, char* name, u32 len) { return copy_out(T(t)->filename, name, len); }
NWC24Err NWC24GetDlLastUpdate(NWC24DlTask* t, s64* time) {
    *time = T(t)->lastUpdate;
    return NWC24_OK;
}
NWC24Err NWC24GetDlNextTime(NWC24DlTask* t, s64* time) {
    *time = T(t)->nextTime;
    return NWC24_OK;
}
NWC24Err NWC24GetDlVfName(NWC24DlTask* t, char* name, u32 len) {
    (void)t;
    // The channel's own data directory, as on the Wii; only used as a name.
    return copy_out("/title/00010002/48414645/data/wc24dl.vff", name, len);
}
NWC24Err NWC24CreateDlVf(NWC24DlTask* t, u32 size) {
    (void)t;
    (void)size;
    return NWC24_OK;
}

NWC24Err NWC24AddDlTask(NWC24DlTask* t) {
    int i = find_task(T(t)->id);
    if (i < 0) {
        for (i = 0; i < MAX_TASKS && sUsed[i]; i++) {
        }
        if (i == MAX_TASKS) {
            return NWC24_ERR_FULL;
        }
    }
    sTasks[i] = *T(t);
    sUsed[i] = true;
    return NWC24_OK;
}

NWC24Err NWC24UpdateDlTask(NWC24DlTask* t) {
    int i = find_task(T(t)->id);
    if (i < 0) {
        return NWC24_ERR_NOT_FOUND;
    }
    sTasks[i] = *T(t);
    return NWC24_OK;
}

NWC24Err NWC24DeleteDlTask(NWC24DlTask* t) {
    int i = find_task(T(t)->id);
    if (i < 0) {
        return NWC24_ERR_NOT_FOUND;
    }
    sUsed[i] = false;
    return NWC24_OK;
}

// --- the VF directory ---------------------------------------------------------------

static void vf_path(const char* name, const char* suffix, char* out, size_t len) {
    snprintf(out, len, "%swc24/%s%s", port_data_dir(), name, suffix);
}

static bool vf_write(const char* name, const u8* data, u32 size, s64 time) {
    char dir[512], path[512], tmp[512];
    snprintf(dir, sizeof(dir), "%swc24", port_data_dir());
    SDL_CreateDirectory(dir);
    vf_path(name, "", path, sizeof(path));
    vf_path(name, ".part", tmp, sizeof(tmp));
    FILE* f = fopen(tmp, "wb");
    if (f == NULL) {
        return false;
    }
    bool ok = fwrite(data, 1, size, f) == size;
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        remove(tmp);
        return false;
    }
    remove(path);
    if (rename(tmp, path) != 0) {
        return false;
    }
    vf_path(name, ".time", path, sizeof(path));
    f = fopen(path, "w");
    if (f != NULL) {
        fprintf(f, "%lld\n", (long long)time);
        fclose(f);
    }
    return true;
}

static bool vf_time(const char* name, s64* time) {
    char path[512];
    vf_path(name, ".time", path, sizeof(path));
    FILE* f = fopen(path, "r");
    if (f == NULL) {
        return false;
    }
    long long t = 0;
    bool ok = fscanf(f, "%lld", &t) == 1;
    fclose(f);
    *time = (s64)t;
    return ok;
}

// Replaces Nintendo's host with WiiLink's (or $FORECAST_WC24_HOST); also
// collapses the run of slashes WiiLink's patched channel leaves after it.
static void map_url(const char* in, char* out, size_t len) {
    const char* host = SDL_getenv("FORECAST_WC24_HOST");
    if (host == NULL || host[0] == '\0') {
        host = WIILINK_HOST;
    }
    const char* prefix = "http://" NINTENDO_HOST;
    if (strncmp(in, prefix, strlen(prefix)) == 0) {
        const char* rest = in + strlen(prefix);
        while (rest[0] == '/' && rest[1] == '/') {
            rest++;
        }
        snprintf(out, len, "http://%s%s", host, rest);
    } else {
        snprintf(out, len, "%s", in);
    }
}

NWC24Err NWC24ExecDownloadTask(s32 mode, u16 id, s32 flags) {
    (void)mode;
    (void)flags;
    int i = find_task(id);
    if (i < 0) {
        return NWC24_ERR_NOT_FOUND;
    }
    PortDlTask* t = &sTasks[i];
    char url[0x140];
    map_url(t->url, url, sizeof(url));

    u8* data = NULL;
    unsigned size = 0;
    int status = 0;
    NWC24Err err = NWC24_OK;
    if (!port_http_get(url, &data, &size, &status)) {
        err = NWC24_ERR_NETWORK;
        sErrorCode = -107000; // the Wii's "could not connect" family
    } else if (status != 200) {
        port_log("wc24: %s: HTTP %d\n", url, status);
        err = NWC24_ERR_SERVER;
        sErrorCode = -117000 - status;
    } else if (size <= WC24_HEADER_SIZE || (data[WC24_HEADER_SIZE] & 0xF0) != 0x10) {
        port_log("wc24: %s: %u bytes, not a WiiConnect24 LZ file\n", url, size);
        err = NWC24_ERR_FILE_BROKEN;
        sErrorCode = -107245;
    } else {
        s64 now = OSGetTime();
        if (!vf_write(t->filename, data + WC24_HEADER_SIZE, size - WC24_HEADER_SIZE, now)) {
            port_log("wc24: cannot store %s\n", t->filename);
            err = NWC24_ERR_FILE_OTHER;
        } else {
            port_log("wc24: %s -> %s (%u bytes)\n", url, t->filename, size - WC24_HEADER_SIZE);
            t->lastUpdate = now;
            t->nextTime = now + OS_SEC_TO_TICKS((s64)t->interval * 60);
            sErrorCode = 0;
        }
    }
    free(data);

    if (err != NWC24_OK) {
        // Keep showing the last good download, if there is one.
        s64 time;
        if (vf_time(t->filename, &time)) {
            port_log("wc24: download failed (%d); using the copy from an earlier run\n", (int)err);
            t->lastUpdate = time;
            t->nextTime = OSGetTime() + OS_SEC_TO_TICKS((s64)t->interval * 60);
            return NWC24_OK;
        }
        port_log("wc24: download of %s failed (%d)\n", url, (int)err);
    }
    return err;
}

// --- VF file access -------------------------------------------------------------------

typedef struct PortVfFile {
    u8* data;
    u32 size;
    u32 pos;
} PortVfFile;

void VFInit(void) {}
s32 VFMountDriveNANDFlashEx(const char* drive, const char* sysFile) {
    (void)drive;
    (void)sysFile;
    return 0;
}
s32 VFUnmountDrive(const char* drive) {
    (void)drive;
    return 0;
}
// Directory listing: the channel only walks it to the end; report no entries.
s32 VFFileSearchFirst(void* dta, const char* path, u8 attr) {
    (void)dta;
    (void)path;
    (void)attr;
    return 1;
}
s32 VFFileSearchNext(void* dta) {
    (void)dta;
    return 1;
}

void* VFOpenFile(const char* path, const char* mode, u32 attr) {
    (void)attr;
    if (mode[0] != 'r') {
        port_log("wc24: VF write access (%s) not supported\n", path);
        return NULL;
    }
    // Drop a drive prefix ("@24:/").
    const char* name = strrchr(path, '/');
    name = name != NULL ? name + 1 : path;
    const char* colon = strrchr(name, ':');
    if (colon != NULL) {
        name = colon + 1;
    }
    char full[512];
    vf_path(name, "", full, sizeof(full));
    size_t size = 0;
    void* data = SDL_LoadFile(full, &size);
    if (data == NULL) {
        return NULL;
    }
    PortVfFile* f = (PortVfFile*)malloc(sizeof(PortVfFile));
    f->data = (u8*)data;
    f->size = (u32)size;
    f->pos = 0;
    return f;
}

s32 VFReadFile(void* file, void* buf, u32 size, u32* readSize) {
    PortVfFile* f = (PortVfFile*)file;
    u32 n = f->size - f->pos;
    if (n > size) {
        n = size;
    }
    memcpy(buf, f->data + f->pos, n);
    f->pos += n;
    if (readSize != NULL) {
        *readSize = n;
    }
    return 0;
}

s32 VFGetFileSizeByFd(void* file) { return (s32)((PortVfFile*)file)->size; }

s32 VFCloseFile(void* file) {
    PortVfFile* f = (PortVfFile*)file;
    SDL_free(f->data);
    free(f);
    return 0;
}
