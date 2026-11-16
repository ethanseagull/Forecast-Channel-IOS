// OS layer: arenas, time, interrupts and reporting on top of SDL3.
#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdlib.h>

#include <revolution/OS.h>

#include <port_log.h>
#include <port_mem.h>


// MEM1 / MEM2 stand-ins, carved out of the port arena (port_mem.h) so every
// heap allocation has a GX address. The Wii has 24 MB + 64 MB; both are larger
// here: MEM1 also serves host C++ (operator new), and the globe
// (earth.brres) alone is 17 MB unpacked.
#define PORT_MEM1_SIZE (64u * 1024 * 1024)
#define PORT_MEM2_SIZE (128u * 1024 * 1024)

typedef struct {
    u8* lo;
    u8* hi;
} PortArena;

static PortArena sMem1, sMem2;

static void* arena_alloc_hi(PortArena* a, size_t size, u32 align) {
    uintptr_t hi = ((uintptr_t)a->hi - size) & ~(uintptr_t)(align - 1);
    if (hi < (uintptr_t)a->lo) {
        OSPanic(__FILE__, __LINE__, "arena exhausted (%zu bytes)", size);
    }
    a->hi = (u8*)hi;
    return (void*)hi;
}

void OSInit(void) {
    static BOOL sDone = FALSE;
    if (sDone) {
        return;
    }
    sDone = TRUE;
    // The Wii's clocks (the headers define these memory-mapped globals);
    // OS_TIME_SPEED (bus / 4) is derived from the bus clock.
    OS_BUS_CLOCK_SPEED = 243000000;
    OS_CPU_CLOCK_SPEED = 729000000;
    port_mem_init();
    // GX address 0 must stay NULL (port_mem.h), so the heaps start one page in.
    sMem1.lo = port_arena_base + 4096;
    sMem1.hi = sMem1.lo + PORT_MEM1_SIZE;
    sMem2.lo = sMem1.hi;
    sMem2.hi = sMem2.lo + PORT_MEM2_SIZE;
}

void* OSGetMEM1ArenaHi(void) { return sMem1.hi; }
void* OSGetMEM1ArenaLo(void) { return sMem1.lo; }
void* OSGetMEM2ArenaHi(void) { return sMem2.hi; }
void* OSGetMEM2ArenaLo(void) { return sMem2.lo; }
void* OSGetArenaHi(void) { return sMem1.hi; }
void* OSGetArenaLo(void) { return sMem1.lo; }
void OSSetMEM1ArenaHi(void* p) { sMem1.hi = (u8*)p; }
void OSSetMEM1ArenaLo(void* p) { sMem1.lo = (u8*)p; }
void OSSetMEM2ArenaHi(void* p) { sMem2.hi = (u8*)p; }
void OSSetMEM2ArenaLo(void* p) { sMem2.lo = (u8*)p; }
void OSSetArenaHi(void* p) { sMem1.hi = (u8*)p; }
void OSSetArenaLo(void* p) { sMem1.lo = (u8*)p; }
void* OSAllocFromMEM1ArenaHi(size_t size, u32 align) { return arena_alloc_hi(&sMem1, size, align); }
void* OSAllocFromMEM2ArenaHi(size_t size, u32 align) { return arena_alloc_hi(&sMem2, size, align); }

// Time: like the Wii, ticks of OS_TIME_SPEED (60.75 MHz = 243 / 4000 per ns)
// since 2000-01-01 00:00 local time. The wall clock and UTC offset are read
// once; SDL's monotonic clock advances it.
static s64 sEpochNS = -1;

s64 OSGetTime(void) {
    if (sEpochNS < 0) {
        SDL_Time now = 0;
        SDL_DateTime dt;
        s64 offsetNS = 0;
        SDL_GetCurrentTime(&now);
        if (SDL_TimeToDateTime(now, &dt, true)) {
            offsetNS = (s64)dt.utc_offset * 1000000000LL;
        }
        // 946684800 s: 1970-01-01 -> 2000-01-01
        sEpochNS = now + offsetNS - 946684800LL * 1000000000LL - (s64)SDL_GetTicksNS();
    }
    return (sEpochNS + (s64)SDL_GetTicksNS()) / 1000 * 243 / 4;
}

u32 OSGetTick(void) { return (u32)OSGetTime(); }

// Interrupts: the channel uses these as a global lock around shared state.
// SDL mutexes are recursive.
static SDL_Mutex* sIntrLock;
static SDL_AtomicInt sIntrInit;
static _Thread_local int sIntrDepth;

static void intr_lock_init(void) {
    if (SDL_GetAtomicInt(&sIntrInit) == 2) {
        return;
    }
    if (SDL_CompareAndSwapAtomicInt(&sIntrInit, 0, 1)) {
        sIntrLock = SDL_CreateMutex();
        SDL_SetAtomicInt(&sIntrInit, 2);
    }
    while (SDL_GetAtomicInt(&sIntrInit) != 2) {
        SDL_DelayNS(0);
    }
}

BOOL OSDisableInterrupts(void) {
    intr_lock_init();
    SDL_LockMutex(sIntrLock);
    return sIntrDepth++ == 0;
}

BOOL OSEnableInterrupts(void) {
    intr_lock_init();
    BOOL was = sIntrDepth == 0;
    while (sIntrDepth > 0) {
        sIntrDepth--;
        SDL_UnlockMutex(sIntrLock);
    }
    return was;
}

BOOL OSRestoreInterrupts(BOOL status) {
    // status TRUE = interrupts were enabled before the matching Disable.
    if (sIntrDepth > 0) {
        sIntrDepth--;
        SDL_UnlockMutex(sIntrLock);
    }
    return status;
}

void OSReport(const char* msg, ...) {
    va_list ap;
    va_start(ap, msg);
    port_log_v(msg, ap);
    va_end(ap);
}

void OSPanic(const char* file, int line, const char* msg, ...) {
    va_list ap;
    port_log("\n*** OSPanic %s:%d\n", file, line);
    va_start(ap, msg);
    port_log_v(msg, ap);
    va_end(ap);
    port_log("\n");
    SDL_TriggerBreakpoint();
    abort();
}
