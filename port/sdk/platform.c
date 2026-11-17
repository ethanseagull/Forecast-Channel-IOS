// The window, events and 60 Hz frame pacing on SDL3.
#include <SDL3/SDL.h>
#include <stdlib.h>

#include <port_log.h>
#include <port_audio.h>
#include <port_platform.h>

static SDL_Window* sWindow;
// Time per frame spent in parts of the frame (port_perf_add), summed between log lines.
static uint64_t sPerf[PORT_PERF_COUNT];
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

// "Back to the Wii Menu": on the Wii this never returns. On Windows the app
// quits. (iOS apps must not quit themselves; milestone 6 decides what it
// does there.)
int OSEnableInterrupts(void);

void OSReturnToMenu(void) {
    port_log("OSReturnToMenu: quitting after %u frames\n", sFrame);
    // The channel calls this with interrupts disabled (the port's global
    // lock held). The audio frame thread takes that lock every frame, so
    // release it before port_platform_quit waits for that thread.
    OSEnableInterrupts();
    port_platform_quit();
    exit(0);
}

void port_platform_quit(void) {
    // Stop the audio frame thread before SDL goes away (port/sdk/ai.c).
    port_audio_shutdown();
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

    // Frame pacing. Windows: a fixed 60 Hz grid (monitors may run at any
    // rate). iOS: the display's own refresh paces the game (presenting waits
    // for a free drawable), and the timer only enforces a minimum interval:
    // two independent 60 Hz clocks drifting against each other made frames
    // land a refresh late every so often (visible stutter).
    static Uint64 sFrameStartNS, sLogNS, sBusyNS, sSleepNS, sMaxNS;
    static uint32_t sHitches, sLogFrames;
    Uint64 now = SDL_GetTicksNS();
    Uint64 interval = sFrameStartNS != 0 ? now - sFrameStartNS : 0;
    if (sFrameStartNS != 0) {
        sBusyNS += interval;
    }
#ifdef SDL_PLATFORM_IOS
    // 15.5 ms: under a 60 Hz refresh, so the display decides when frames
    // happen; a 120 Hz display still cannot run the game faster than 60.
    const Uint64 minInterval = 15500000;
    if (sFrameStartNS != 0 && interval < minInterval) {
        SDL_DelayPrecise(minInterval - interval);
        sSleepNS += minInterval - interval;
    }
#else
    if (now < sNextFrameNS) {
        SDL_DelayPrecise(sNextFrameNS - now);
        sSleepNS += sNextFrameNS - now;
        sNextFrameNS += FRAME_NS;
    } else {
        // Running late: don't try to catch up with a burst of frames.
        sNextFrameNS = now + FRAME_NS;
    }
#endif
    sFrame++;
    now = SDL_GetTicksNS();
    if (sFrameStartNS != 0) {
        Uint64 full = now - sFrameStartNS;
        if (full > sMaxNS) {
            sMaxNS = full;
        }
        if (full > 20000000) {
            sHitches++;
        }
    }
    sFrameStartNS = now;
    sLogFrames++;
    // Every 2 seconds: frame rate, slowest frame, hitches (frames over 20 ms)
    // and where the time went (port_perf_add buckets: per-frame averages).
    if (sLogNS == 0) {
        sLogNS = now;
    } else if (now - sLogNS >= 2000000000ull) {
        double secs = (double)(now - sLogNS) / 1e9;
        double perFrame = 1.0 / (double)sLogFrames / 1e6; // ns total -> ms per frame
        port_log("perf: frame %u: %.1f fps, slowest %.1f ms, %u hitches; per frame: game %.1f ms "
                 "(gx record %.1f, gx submit %.1f, screen wait %.1f), sleep %.1f ms\n",
                 sFrame, (double)sLogFrames / secs, (double)sMaxNS / 1e6, sHitches,
                 (double)sBusyNS * perFrame, (double)sPerf[PORT_PERF_RECORD] * perFrame,
                 (double)sPerf[PORT_PERF_SUBMIT] * perFrame, (double)sPerf[PORT_PERF_ACQUIRE] * perFrame,
                 (double)sSleepNS * perFrame);
        sLogNS = now;
        sBusyNS = sSleepNS = sMaxNS = 0;
        sHitches = sLogFrames = 0;
        SDL_memset(sPerf, 0, sizeof(sPerf));
    }
    return sFrame;
}

void port_perf_add(int bucket, uint64_t ns) {
    if (bucket >= 0 && bucket < PORT_PERF_COUNT) {
        sPerf[bucket] += ns;
    }
}

void port_pointer(float* x, float* y, bool* down, bool* inside) {
    *x = sPointerX;
    *y = sPointerY;
    *down = sPointerDown;
    *inside = sPointerInside;
}
