// The port arena and GX address translation (see port_mem.h).
#include <stdlib.h>
#include <string.h>

#include <port_log.h>
#include <port_mem.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <sys/mman.h>
#endif

uint8_t* port_arena_base;
static const uint8_t* sImageBase;
static size_t sImageSize;

static void find_image(void) {
#ifdef _WIN32
    HMODULE mod = GetModuleHandleW(NULL);
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)mod;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)((const uint8_t*)mod + dos->e_lfanew);
    sImageBase = (const uint8_t*)mod;
    sImageSize = nt->OptionalHeader.SizeOfImage;
#else
    // The main executable's segments are contiguous from its Mach-O header to
    // the end of __DATA (plus the ASLR slide, already in the header address).
    const struct mach_header_64* mh = (const struct mach_header_64*)_dyld_get_image_header(0);
    unsigned long size = 0;
    uint8_t* data = getsegmentdata(mh, "__DATA", &size);
    sImageBase = (const uint8_t*)mh;
    sImageSize = data != NULL ? (size_t)(data + size - (const uint8_t*)mh) : 0;
#endif
}

void port_mem_init(void) {
    if (port_arena_base != NULL) {
        return;
    }
#ifdef _WIN32
    port_arena_base = (uint8_t*)VirtualAlloc(NULL, PORT_ARENA_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* p = mmap(NULL, PORT_ARENA_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    port_arena_base = p == MAP_FAILED ? NULL : (uint8_t*)p;
#endif
    if (port_arena_base == NULL) {
        port_log("mem: cannot reserve the %u MB arena\n", PORT_ARENA_SIZE >> 20);
        abort();
    }
    find_image();
    if (sImageSize > PORT_GX_ADDR_LIMIT - PORT_GX_IMAGE_BASE) {
        port_log("mem: executable image too large for GX addresses (%zu bytes)\n", sImageSize);
        abort();
    }
    port_log("mem: arena %p (%u MB), image %p (%zu KB)\n", (void*)port_arena_base, PORT_ARENA_SIZE >> 20,
             (const void*)sImageBase, sImageSize >> 10);
}

uint32_t port_gx_addr(const void* p) {
    const uint8_t* b = (const uint8_t*)p;
    if (b == NULL) {
        return 0;
    }
    if (b >= port_arena_base && b < port_arena_base + PORT_ARENA_SIZE) {
        return (uint32_t)(b - port_arena_base);
    }
    if (b >= sImageBase && b < sImageBase + sImageSize) {
        return PORT_GX_IMAGE_BASE + (uint32_t)(b - sImageBase);
    }
    port_log("mem: pointer %p is outside the arena and image; GX cannot address it\n", p);
    return 0;
}

const void* port_gx_ptr(uint32_t addr) {
    addr &= PORT_GX_ADDR_LIMIT - 1;
    if (addr == 0) {
        // The first arena page is never handed out, so 0 stays NULL.
        return NULL;
    }
    if (addr < PORT_GX_IMAGE_BASE) {
        return port_arena_base + addr;
    }
    addr -= PORT_GX_IMAGE_BASE;
    return addr < sImageSize ? sImageBase + addr : NULL;
}

int port_mem_readable(const void* p, size_t size) {
    const uint8_t* b = (const uint8_t*)p;
    if (b >= port_arena_base && size <= PORT_ARENA_SIZE && b + size <= port_arena_base + PORT_ARENA_SIZE) {
        return 1;
    }
    return b >= sImageBase && size <= sImageSize && b + size <= sImageBase + sImageSize;
}
