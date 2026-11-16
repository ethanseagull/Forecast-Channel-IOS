// The Wii's write-gather pipe, replaced. GX_WRITE (revolution/GX/GXHardware.h)
// appends each value big-endian to the current command buffer, exactly as the
// pipe would have put it on the bus. port/gx/gx_cp.cpp parses the buffer:
// immediate commands at sync points, display lists when they are called.
#ifndef PORT_GX_PIPE_H
#define PORT_GX_PIPE_H
#include <types.h>

#include <port_mem.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    u8* cur;
    u8* end;
} PortGXPipe;

extern PortGXPipe port_gx_pipe;

// Makes room for need bytes (parses the immediate buffer, or marks a display
// list as overflowed).
void port_gx_pipe_full(u32 need);

static inline u8* port_gx_reserve(u32 n) {
    u8* p;
    if ((u32)(port_gx_pipe.end - port_gx_pipe.cur) < n) {
        port_gx_pipe_full(n);
    }
    p = port_gx_pipe.cur;
    port_gx_pipe.cur = p + n;
    return p;
}

static inline void port_gx_w8(u32 v) {
    u8* p = port_gx_reserve(1);
    p[0] = (u8)v;
}

static inline void port_gx_w16(u32 v) {
    u8* p = port_gx_reserve(2);
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}

static inline void port_gx_w32(u32 v) {
    u8* p = port_gx_reserve(4);
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

static inline void port_gx_wf32(f32 f) {
    union {
        f32 f;
        u32 u;
    } x;
    x.f = f;
    port_gx_w32(x.u);
}

#define port_gx_write_c(v) port_gx_w8((u32)(u8)(v))
#define port_gx_write_uc(v) port_gx_w8((u32)(u8)(v))
#define port_gx_write_s(v) port_gx_w16((u32)(u16)(v))
#define port_gx_write_us(v) port_gx_w16((u32)(u16)(v))
#define port_gx_write_i(v) port_gx_w32((u32)(v))
#define port_gx_write_ui(v) port_gx_w32((u32)(v))
#define port_gx_write_f(v) port_gx_wf32((f32)(v))
// Addresses written into the stream become GX addresses (port_mem.h).
#define port_gx_write_p(v) port_gx_w32(port_gx_addr((const void*)(v)))

#define GX_WRITE(member, value) port_gx_write_##member(value)

#ifdef __cplusplus
}
#endif
#endif
