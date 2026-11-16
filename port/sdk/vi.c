// VI (video interface): the channel paces itself on VIWaitForRetrace, which
// maps to the host's 60 Hz frame (port/sdk/platform.c). Frame buffers are
// handled by the GX layer.
#include <revolution/VI.h>

#include <port_platform.h>

static u32 sRetraceCount;

void VIInit(void) {}

void VIConfigure(const GXRenderModeObj* rm) { (void)rm; }

void VIFlush(void) {}

void VISetNextFrameBuffer(void* fb) { (void)fb; }

void VISetBlack(BOOL black) { (void)black; }

s32 VIGetRetraceCount(void) { return (s32)sRetraceCount; }

void VIWaitForRetrace(void) { sRetraceCount = port_platform_frame(); }
