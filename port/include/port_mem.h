// Memory for a 64-bit host, and the 32-bit addresses the Wii code expects.
//
// The Wii's GPU registers hold 29-bit physical addresses (texture images, TLUTs,
// vertex arrays, display lists), and the decomp computes them from pointers.
// Host pointers are 64 bits, so the port hands out "GX addresses" instead:
//
//   0x00000000 - 0x0FFFFFFF  offset into the port arena (MEM1/MEM2 heaps)
//   0x10000000 - 0x1FFFFFFF  offset into the executable image (static data)
//
// Everything the channel allocates comes from the arena, so the same GX address
// works on every target (iOS cannot map memory below 4 GB at all).
#ifndef PORT_MEM_H
#define PORT_MEM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PORT_ARENA_SIZE (256u * 1024 * 1024)
#define PORT_GX_IMAGE_BASE 0x10000000u
#define PORT_GX_ADDR_LIMIT 0x20000000u

// Reserves the arena and finds the executable image. Called once at startup.
void port_mem_init(void);

// The arena: [port_arena_base, port_arena_base + PORT_ARENA_SIZE).
extern uint8_t* port_arena_base;

// Pointer -> GX address. Pointers outside the arena and the image are logged
// once per call site and map to 0 (the GPU then reads nothing useful).
uint32_t port_gx_addr(const void* p);
// GX address -> pointer (NULL if it maps to neither region).
const void* port_gx_ptr(uint32_t addr);
// True if [p, p + size) lies in the arena or the image.
int port_mem_readable(const void* p, size_t size);

// Threads the port creates for itself (texture pack loaders, the audio frame
// thread) call port_mark_host_thread. The channel's operator new serves them
// from the C heap: its MEM heaps are not safe to use from several threads.
void port_mark_host_thread(void);
int port_is_host_thread(void);

#ifdef __cplusplus
}
#endif
#endif
