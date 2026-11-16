// The window, events and 60 Hz frame pacing on SDL3.
#include <SDL3/SDL.h>
#include <stdlib.h>

#include <port_log.h>
#include <port_platform.h>

static SDL_Window* sWindow;
static Uint64 sNextFrameNS;
static uint32_t sFrame;
static float sPointerX, sPointerY;
static bool sPointerDown, sPointerInside;

// The Wii's 60 Hz (59.94 for NTSC; 60 is close enough for a channel).
#define FRAME_NS (1000000000ull / 60)

bool port_platform_init(void) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        port_log("SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    // 16:9 at twice the Wii's 608-wide safe frame; resizable.
    // FORECAST_HIDDEN=1 (automated test runs): no visible window; frames are
    // still rendered (and can be dumped with FORECAST_DUMP_FRAME).
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    const char* hidden = SDL_getenv("FORECAST_HIDDEN");
    if (hidden != NULL && hidden[0] == '1') {
        flags |= SDL_WINDOW_HIDDEN;
    }
    sWindow = SDL_CreateWindow("Forecast Channel", 1280, 720, flags);
    if (sWindow == NULL) {
        port_log("SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
    sNextFrameNS = SDL_GetTicksNS() + FRAME_NS;
    return true;
}

void port_platform_quit(void) {
    SDL_DestroyWindow(sWindow);
    SDL_Quit();
}

SDL_Window* port_window(void) { return sWindow; }

uint32_t port_platform_frame(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            port_log("quit after %u frames\n", sFrame);
            port_platform_quit();
            exit(0);
        case SDL_EVENT_MOUSE_MOTION:
            sPointerX = e.motion.x;
            sPointerY = e.motion.y;
            sPointerInside = true;
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.button == SDL_BUTTON_LEFT) {
                sPointerDown = e.button.down;
                sPointerX = e.button.x;
                sPointerY = e.button.y;
            }
            break;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            sPointerInside = false;
            break;
        default:
            break;
        }
    }

    // Debugging: FORECAST_TOUCH=frame:x:y:frames[;...] touches the window at (x, y)
    // (0-1 of the window size) for that many frames.
    static const char* sTouch;
    static bool sTouchRead;
    if (!sTouchRead) {
        sTouch = SDL_getenv("FORECAST_TOUCH");
        sTouchRead = true;
    }
    // Several touches: separated by ';'.
    for (const char* t = sTouch; t != NULL && *t != '\0';) {
        unsigned start = 0, len = 0;
        float tx = 0, ty = 0;
        if (SDL_sscanf(t, "%u:%f:%f:%u", &start, &tx, &ty, &len) == 4 && sFrame + 1 >= start &&
            sFrame + 1 <= start + len) {
            int w = 0, h = 0;
            SDL_GetWindowSize(sWindow, &w, &h);
            sPointerX = tx * (float)w;
            sPointerY = ty * (float)h;
            sPointerInside = true;
            sPointerDown = sFrame + 1 < start + len;
        }
        t = SDL_strchr(t, ';');
        if (t != NULL) {
            t++;
        }
    }

    Uint64 now = SDL_GetTicksNS();
    if (now < sNextFrameNS) {
        SDL_DelayPrecise(sNextFrameNS - now);
        sNextFrameNS += FRAME_NS;
    } else {
        // Running late: don't try to catch up with a burst of frames.
        sNextFrameNS = now + FRAME_NS;
    }
    sFrame++;
    if (sFrame == 1 || sFrame % 600 == 0) {
        port_log("frame %u\n", sFrame);
    }
    return sFrame;
}

void port_pointer(float* x, float* y, bool* down, bool* inside) {
    *x = sPointerX;
    *y = sPointerY;
    *down = sPointerDown;
    *inside = sPointerInside;
}
