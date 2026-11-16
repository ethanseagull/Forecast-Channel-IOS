// GX FIFO objects and synchronisation, replacing GXFifo.c and GXMisc.c.
//
// The CPU FIFO is either the immediate pipe (a private buffer parsed at sync
// points) or a display list being recorded (GXBeginDisplayList points it at
// the list's memory). Both receive the same big-endian command bytes.
#include <stdlib.h>
#include <string.h>

#include <revolution/GX.h>
#include <revolution/OS.h>

#include "gx_internal.h"
#include <port_gx_pipe.h>
#include <port_log.h>

#define PIPE_SIZE (256 * 1024)

PortGXPipe port_gx_pipe;

static u8* sPipeBuf;
static u32 sPipeCap;
// The FIFO GXInit set up; any other CPU FIFO is a display list.
static void* sMainBase;
static GXFifoObjImpl* sCPUFifo;
static GXFifoObjImpl sMainFifo;
static GXBool sRecording;
static u8 sOverflowSink[64];
static GXBool sOverflowed;
static GXDrawDoneCallback sDrawDoneCB;
static GXDrawSyncCallback sTokenCB;

void port_gx_sync(void) {
    u32 size;
    if (sRecording || sPipeBuf == NULL) {
        return;
    }
    size = (u32)(port_gx_pipe.cur - sPipeBuf);
    port_gx_pipe.cur = sPipeBuf;
    if (size != 0) {
        gx_cp_run(sPipeBuf, size);
    }
}

void port_gx_pipe_full(u32 need) {
    if (sRecording) {
        // The list is full: like the hardware, flag the wrap and drop the rest.
        if (!sOverflowed) {
            port_log("gx: display list overflow (%lu bytes)\n", (unsigned long)sCPUFifo->size);
        }
        sOverflowed = TRUE;
        if (sCPUFifo != NULL) {
            sCPUFifo->wrap = TRUE;
        }
        port_gx_pipe.cur = sOverflowSink;
        port_gx_pipe.end = sOverflowSink + sizeof(sOverflowSink);
        return;
    }
    // Immediate pipe full. A command may straddle the end, so parsing waits
    // for a sync point; grow instead (rare: one huge immediate primitive).
    {
        u32 used = (u32)(port_gx_pipe.cur - sPipeBuf);
        u32 cap = (u32)(port_gx_pipe.end - sPipeBuf);
        u32 newCap = cap * 2;
        u8* grown;
        while (newCap - used < need) {
            newCap *= 2;
        }
        grown = (u8*)realloc(sPipeBuf, newCap);
        if (grown == NULL) {
            OSPanic(__FILE__, __LINE__, "gx: pipe cannot grow to %lu bytes", (unsigned long)newCap);
        }
        port_log("gx: immediate pipe grown to %lu KB\n", (unsigned long)(newCap / 1024));
        sPipeBuf = grown;
        sPipeCap = newCap;
        port_gx_pipe.cur = grown + used;
        port_gx_pipe.end = grown + newCap;
    }
}

void __GXFifoInit(void) {
    if (sPipeBuf == NULL) {
        sPipeBuf = (u8*)malloc(PIPE_SIZE);
        port_gx_pipe.cur = sPipeBuf;
        port_gx_pipe.end = sPipeBuf + PIPE_SIZE;
        sPipeCap = PIPE_SIZE;
        port_gx_backend_init();
    }
}

void GXInitFifoBase(GXFifoObj* fifo, void* base, u32 size) {
    GXFifoObjImpl* impl = (GXFifoObjImpl*)fifo;
    impl->base = base;
    impl->end = (u8*)base + size - 4;
    impl->size = size;
    impl->count = 0;
    impl->wrap = FALSE;
    impl->readPtr = base;
    impl->writePtr = base;
    if (sMainBase == NULL) {
        sMainBase = base;
    }
}

void GXInitFifoPtrs(GXFifoObj* fifo, void* readPtr, void* writePtr) {
    GXFifoObjImpl* impl = (GXFifoObjImpl*)fifo;
    impl->readPtr = readPtr;
    impl->writePtr = writePtr;
    impl->count = (s32)((u8*)writePtr - (u8*)readPtr);
}

void GXInitFifoLimits(GXFifoObj* fifo, u32 hiWatermark, u32 loWatermark) {
    GXFifoObjImpl* impl = (GXFifoObjImpl*)fifo;
    impl->hiWatermark = hiWatermark;
    impl->loWatermark = loWatermark;
}

void GXSetCPUFifo(GXFifoObj* fifo) {
    GXFifoObjImpl* impl = (GXFifoObjImpl*)fifo;

    if (sRecording) {
        // Leaving a display list: record how much was written.
        sCPUFifo->writePtr = sOverflowed ? sCPUFifo->writePtr : port_gx_pipe.cur;
        sCPUFifo->count = (s32)((u8*)sCPUFifo->writePtr - (u8*)sCPUFifo->base);
        sRecording = FALSE;
    }

    if (impl->base == sMainBase) {
        sMainFifo = *impl;
        sCPUFifo = &sMainFifo;
        port_gx_pipe.cur = sPipeBuf;
        port_gx_pipe.end = sPipeBuf + sPipeCap;
        return;
    }

    // Recording into a display list: flush what the pipe holds first.
    port_gx_sync();
    sCPUFifo = impl;
    sRecording = TRUE;
    sOverflowed = FALSE;
    impl->wrap = FALSE;
    port_gx_pipe.cur = (u8*)impl->writePtr;
    port_gx_pipe.end = (u8*)impl->base + impl->size;
}

void GXSetGPFifo(GXFifoObj* fifo) { (void)fifo; }

BOOL GXGetCPUFifo(GXFifoObj* fifo) {
    GXFifoObjImpl* impl = (GXFifoObjImpl*)fifo;
    if (sCPUFifo == NULL) {
        return FALSE;
    }
    if (sRecording) {
        if (!sOverflowed) {
            sCPUFifo->writePtr = port_gx_pipe.cur;
        }
        sCPUFifo->count = (s32)((u8*)sCPUFifo->writePtr - (u8*)sCPUFifo->base);
    }
    if (impl != sCPUFifo) {
        *impl = *sCPUFifo;
    }
    return TRUE;
}

BOOL GXGetGPFifo(GXFifoObj* fifo) {
    *(GXFifoObjImpl*)fifo = sMainFifo;
    return TRUE;
}

void GXGetFifoPtrs(const GXFifoObj* fifo, void** readPtr, void** writePtr) {
    const GXFifoObjImpl* impl = (const GXFifoObjImpl*)fifo;
    *readPtr = impl->readPtr;
    *writePtr = impl->writePtr;
}

u32 GXGetFifoCount(GXFifoObj* fifo) { return (u32)((GXFifoObjImpl*)fifo)->count; }

u8 GXGetFifoWrap(GXFifoObj* fifo) { return ((GXFifoObjImpl*)fifo)->wrap; }

GXBool CPGPLinkCheck(void) { return TRUE; }

GXBool __GXIsGPFifoReady(void) { return TRUE; }

void __GXCleanGPFifo(void) {}

GXBreakPtCallback GXSetBreakPtCallback(GXBreakPtCallback callback) {
    (void)callback;
    return NULL;
}

void GXResetWriteGatherPipe(void) {}

// --- GXMisc -------------------------------------------------------------------

void GXSetMisc(GXMiscToken token, u32 val) {
    switch (token) {
    case GX_MT_XF_FLUSH:
        gxdt->vNum = val;
        gxdt->vNumNot = !gxdt->vNum;
        gxdt->lastWriteWasXF = TRUE;
        if (gxdt->vNum) {
            gxdt->gxDirtyFlags |= GX_DIRTY_VCD;
        }
        break;
    case GX_MT_DL_SAVE_CONTEXT:
        gxdt->dlistSave = val > 0;
        break;
    case GX_MT_ABORT_WAIT_COPYOUT:
        gxdt->abortWaitCopyOut = val > 0;
        break;
    default:
        break;
    }
}

void GXFlush(void) {
    if (gxdt->gxDirtyFlags) {
        __GXSetDirtyState();
    }
    port_gx_sync();
}

void GXAbortFrame(void) { port_gx_pipe.cur = sPipeBuf; }

void GXSetDrawDone(void) { GXFlush(); }

void GXWaitDrawDone(void) {}

void GXDrawDone(void) {
    GXFlush();
    if (sDrawDoneCB != NULL) {
        sDrawDoneCB();
    }
}

void GXSetDrawSync(u16 token) {
    GXFlush();
    if (sTokenCB != NULL) {
        sTokenCB(token);
    }
}

void GXPixModeSync(void) {
    GX_BP_LOAD_REG(gxdt->zControl);
    gxdt->lastWriteWasXF = FALSE;
}

GXDrawSyncCallback GXSetDrawSyncCallback(GXDrawSyncCallback callback) {
    GXDrawSyncCallback old = sTokenCB;
    sTokenCB = callback;
    return old;
}

GXDrawDoneCallback GXSetDrawDoneCallback(GXDrawDoneCallback callback) {
    GXDrawDoneCallback old = sDrawDoneCB;
    sDrawDoneCB = callback;
    return old;
}

void __GXPEInit(void) {}

// Direct EFB pokes: there is no CPU-visible EFB; ignored.
void GXPokeAlphaMode(GXCompare func, u8 threshold) { (void)func; (void)threshold; }
void GXPokeAlphaRead(GXAlphaReadMode mode) { (void)mode; }
void GXPokeAlphaUpdate(GXBool update) { (void)update; }
void GXPokeBlendMode(GXBlendMode type, GXBlendFactor src, GXBlendFactor dst, GXLogicOp op) {
    (void)type;
    (void)src;
    (void)dst;
    (void)op;
}
void GXPokeColorUpdate(GXBool update) { (void)update; }
void GXPokeDstAlpha(GXBool enable, u8 alpha) {
    (void)enable;
    (void)alpha;
}
void GXPokeDither(GXBool dither) { (void)dither; }
void GXPokeZMode(GXBool compareEnable, GXCompare func, GXBool updateEnable) {
    (void)compareEnable;
    (void)func;
    (void)updateEnable;
}
