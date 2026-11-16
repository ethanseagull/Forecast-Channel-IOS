// CNT (channel content) on host files. On the Wii each content is a U8 archive
// in the title's NAND directory; the port reads them unpacked from
// <data dir>/content/<content id as %08x>/<path> (tools/extract_assets.ps1).
#include <stdio.h>
#include <string.h>

#include <revolution/CNT.h>

#include <port_log.h>
#include <port_paths.h>

// CNTFileInfoNAND.startOffset is 32 bits, too small for a FILE*, so it holds
// an index into this table (plus one, so 0 means closed).
#define CNT_MAX_OPEN 64
static FILE* sOpen[CNT_MAX_OPEN];

static FILE* info_file(CNTFileInfoNAND* info) {
    u32 i = info->startOffset;
    return i >= 1 && i <= CNT_MAX_OPEN ? sOpen[i - 1] : NULL;
}

void CNTInit(void) {}

int CNTShutdown(void) { return 0; }

CNTError contentInitHandleNAND(u32 contentId, CNTHandleNAND* handle, MEMAllocator* allocator) {
    memset(handle, 0, sizeof(*handle));
    handle->fd = (ESFd)contentId;
    handle->allocator = allocator;
    return CNT_RESULT_OK;
}

CNTError contentReleaseHandleNAND(CNTHandleNAND* handle) {
    (void)handle;
    return CNT_RESULT_OK;
}

CNTError contentOpenNAND(CNTHandleNAND* handle, const char* path, CNTFileInfoNAND* info) {
    char full[1024];
    int slot;
    while (*path == '/') {
        path++;
    }
    snprintf(full, sizeof(full), "%scontent/%08x/%s", port_data_dir(), (unsigned)handle->fd, path);
    for (slot = 0; slot < CNT_MAX_OPEN && sOpen[slot] != NULL; slot++) {
    }
    if (slot == CNT_MAX_OPEN) {
        port_log("CNT: too many open files (%s)\n", full);
        return CNT_RESULT_OPEN_ERR;
    }
    FILE* f = fopen(full, "rb");
    if (f == NULL) {
        port_log("CNT: cannot open %s\n", full);
        return CNT_RESULT_OPEN_ERR;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    sOpen[slot] = f;

    info->handle = handle;
    info->startOffset = (u32)slot + 1;
    info->length = (u32)len;
    info->readOffset = 0;
    return CNT_RESULT_OK;
}

CNTError contentConvertPathToEntrynumNAND(CNTHandleNAND* handle, const char* path) {
    (void)handle;
    port_log("CNT: contentConvertPathToEntrynumNAND(%s) unsupported\n", path);
    return CNT_RESULT_INVALID;
}

CNTError contentFastOpenNAND(CNTHandleNAND* handle, s32 entryNum, CNTFileInfoNAND* info) {
    (void)handle;
    (void)info;
    port_log("CNT: contentFastOpenNAND(%d) unsupported\n", (int)entryNum);
    return CNT_RESULT_INVALID;
}

BOOL contentOpenDirNAND(CNTHandleNAND* handle, const char* path, ARCDir* dir) {
    (void)handle;
    (void)dir;
    port_log("CNT: contentOpenDirNAND(%s) unsupported\n", path);
    return FALSE;
}

// Reads up to `length` bytes at absolute `offset`; returns the bytes read
// (callers round lengths up to 32, past the end of the file).
CNTError contentReadNAND(CNTFileInfoNAND* info, void* buffer, u32 length, s32 offset) {
    FILE* f = info_file(info);
    if (f == NULL || fseek(f, offset, SEEK_SET) != 0) {
        return CNT_RESULT_READ_ERR;
    }
    size_t n = fread(buffer, 1, length, f);
    info->readOffset = offset + (s32)n;
    return (CNTError)n;
}

s32 contentGetLengthNAND(CNTFileInfoNAND* info) { return (s32)info->length; }

CNTError contentSeekNAND(CNTFileInfoNAND* info, s32 offset, u32 whence) {
    s32 base = whence == CNT_SEEK_CUR ? info->readOffset : whence == CNT_SEEK_END ? (s32)info->length : 0;
    info->readOffset = base + offset;
    return CNT_RESULT_OK;
}

CNTError contentCloseNAND(CNTFileInfoNAND* info) {
    u32 i = info->startOffset;
    if (i >= 1 && i <= CNT_MAX_OPEN && sOpen[i - 1] != NULL) {
        fclose(sOpen[i - 1]);
        sOpen[i - 1] = NULL;
    }
    info->startOffset = 0;
    return CNT_RESULT_OK;
}
