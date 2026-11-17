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

// Where the game's 16:9 frame (the whole 640x456 EFB, stretched to 16:9 as
// a TV shows it) is on the screen, and how much of the screen beside it is
// filled with scenery (gx_sdl.cpp widens the 3D scene or extends the frame's
// edges there; the rest is black).
typedef struct PortDisplay {
    float x, y, w, h;            // the frame
    float fillLeft, fillRight;   // widths of the filled strips left and right of it
} PortDisplay;

// The layout for a drawable of width x height, in that drawable's units: the
// window coordinates' size gives window coordinates, the swapchain's size
// pixels (the safe area is scaled by drawable / window size).
// The frame: as high as the window and 16:9 wide, or narrower (letterboxed)
// when the window or the safe area's width (notch, Dynamic Island) is too
// narrow; centred, moved into the safe area if centring leaves it. Vertical
// safe-area insets are ignored (landscape iPhones: only the home indicator).
// The fill: the screen left / right of the frame, up to 21:9 in all;
// FORECAST_FILL=0 turns it off (black bars as before).
void port_display_layout(float width, float height, PortDisplay* d);

// The game's 16:9 frame in window coordinates: the units of SDL_GetWindowSize
// and of mouse / touch events (points on iOS, not pixels). x, y top-left.
void port_display_rect(float* x, float* y, float* w, float* h);

// Frame-time accounting for the periodic "perf:" log line (platform.c).
enum {
    PORT_PERF_RECORD,  // gx: transforming and recording draws
    PORT_PERF_SUBMIT,  // gx: uploads, render passes, submitting the frame
    PORT_PERF_ACQUIRE, // gx: waiting for the window's next image (display pacing)
    PORT_PERF_COUNT
};
void port_perf_add(int bucket, uint64_t ns);

#ifdef __cplusplus
}
#endif
#endif
