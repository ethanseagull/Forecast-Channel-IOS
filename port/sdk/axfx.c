// AXFX allocation hooks (the SDK's AXFXHooks.c, whose default allocator is the
// OS heap, which the port does not have). The effects themselves are the
// decomp's own AXFXReverbHi.c and AXFXReverbHiExp.c, built from the decomp
// (CMakeLists.txt lets the revolution/AXFX/AXFXReverbHi* units through).
// nw4r always installs its own allocator around the AXFX calls; these
// defaults are only a fallback.
#include <stdlib.h>

#include <revolution/AXFX.h>

static void* default_alloc(size_t size) { return malloc(size); }
static void default_free(void* block) { free(block); }

AXFXAllocHook __AXFXAlloc = default_alloc;
AXFXFreeHook __AXFXFree = default_free;

void AXFXSetHooks(AXFXAllocHook alloc, AXFXFreeHook free) {
    __AXFXAlloc = alloc;
    __AXFXFree = free;
}

void AXFXGetHooks(AXFXAllocHook* alloc, AXFXFreeHook* free) {
    *alloc = __AXFXAlloc;
    *free = __AXFXFree;
}
