// The window, events and frame pacing (port/sdk/platform.c).
#ifndef PORT_PLATFORM_H
#define PORT_PLATFORM_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct SDL_Window SDL_Window;

bool port_platform_init(void);
void port_platform_quit(void);
SDL_Window* port_window(void);

// Pumps events and waits for the next 60 Hz frame. Exits the process when the
// window is closed. Returns the frame number.
uint32_t port_platform_frame(void);

// The pointer in window pixels, and whether the primary button is down.
void port_pointer(float* x, float* y, bool* down, bool* inside);

#ifdef __cplusplus
}
#endif
#endif
