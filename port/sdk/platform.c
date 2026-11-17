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
    // iOS: the home indicator is dimmed and the first swipe up from the
    // bottom edge only brings it back (a second one goes home), so taps on the
    // bottom button row cannot leave the app by accident ("1" would hide it
    // until any touch, which brings it back at every tap).
    SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "2");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        port_log("SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
#ifdef SDL_PLATFORM_IOS
    // Portrait or landscape is decided when the app starts, by how the phone
    // is held: the layouts are placed once, as they load (port/channel/
    // portrait.cpp). The app then keeps that orientation.
    {
        SDL_DisplayOrientation o = SDL_GetCurrentDisplayOrientation(SDL_GetPrimaryDisplay());
        bool upright = o == SDL_ORIENTATION_PORTRAIT || o == SDL_ORIENTATION_PORTRAIT_FLIPPED;
        SDL_SetHint(SDL_HINT_ORIENTATIONS, upright ? "Portrait" : "LandscapeLeft LandscapeRight");
        port_log("display orientation %d: %s\n", (int)o, upright ? "portrait" : "landscape");
    }
#endif
    // 16:9 at twice the Wii's 608-wide safe frame; resizable.
    // FORECAST_HIDDEN=1 (automated test runs): no visible window; frames are
    // still rendered (and can be dumped with FORECAST_DUMP_FRAME).
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    const char* hidden = SDL_getenv("FORECAST_HIDDEN");
    if (hidden != NULL && hidden[0] == '1') {
        flags |= SDL_WINDOW_HIDDEN;
    }
    // FORECAST_WINDOW=WxH: another initial size (e.g. 1704x786, an iPhone's
    // 19.5:9, to test the screen fill). iOS sizes the window itself.
    int winW = 1280, winH = 720;
    const char* size = SDL_getenv("FORECAST_WINDOW");
    if (size != NULL && SDL_sscanf(size, "%dx%d", &winW, &winH) != 2) {
        winW = 1280;
        winH = 720;
    }
    if (winW < 64 || winH < 64) {
        winW = 1280;
        winH = 720;
    }
    sWindow = SDL_CreateWindow("Forecast Channel", winW, winH, flags);
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

// The Wii's display is 16:9 (SCGetAspectRatio says wide); the fill beside it
// stops at 21:9.
#define DISPLAY_ASPECT (16.0f / 9.0f)
#define FILL_ASPECT (21.0f / 9.0f)

// Portrait: whether it is on, the canvas request for the frame being drawn
// and the way the last frame was shown.
static int sPortraitOn = -1;
static bool sCanvasRequest, sCanvasShown;

static bool portrait_window(float width, float height) {
    if (sPortraitOn < 0) {
        const char* p = SDL_getenv("FORECAST_PORTRAIT");
        sPortraitOn = p != NULL && p[0] == '0' ? 0 : 1;
    }
    return sPortraitOn && height > width;
}

void port_portrait_canvas(void) { sCanvasRequest = true; }

bool port_portrait_take_canvas(void) {
    bool c = sCanvasRequest;
    sCanvasRequest = false;
    return c;
}

void port_portrait_presented(bool canvas) { sCanvasShown = canvas; }

static uint32_t sPagesFrame;
void port_portrait_pages(void) { sPagesFrame = sFrame + 1; }
bool port_portrait_pages_showing(void) { return sPagesFrame != 0 && sFrame + 1 - sPagesFrame <= 2; }

// Safe-area insets in window units: top and bottom. Windows has none;
// FORECAST_SAFE_AREA=top,bottom (window units, e.g. 59,34 for an iPhone 15
// Pro held upright) simulates them.
static void safe_insets(float* top, float* bottom) {
    *top = *bottom = 0.0f;
    const char* e = SDL_getenv("FORECAST_SAFE_AREA");
    if (e != NULL && SDL_sscanf(e, "%f,%f", top, bottom) == 2) {
        return;
    }
    int ww = 0, wh = 0;
    SDL_Rect safe;
    if (sWindow != NULL && SDL_GetWindowSize(sWindow, &ww, &wh) && SDL_GetWindowSafeArea(sWindow, &safe) &&
        safe.h > 0) {
        *top = (float)safe.y;
        *bottom = (float)(wh - (safe.y + safe.h));
    }
}

void port_portrait(PortPortrait* p) {
    SDL_zerop(p);
    int ww = 0, wh = 0;
    if (sWindow == NULL || !SDL_GetWindowSize(sWindow, &ww, &wh) || !portrait_window((float)ww, (float)wh)) {
        return;
    }
    // The frame in the canvas: as high as the window is wide, 16:9.
    float fh = (float)ww, fw = fh * (16.0f / 9.0f);
    float halfW = 832.0f * (float)ww / fw * 0.5f, halfH = 456.0f * (float)wh / fh * 0.5f;
    p->active = true;
    p->left = 416.0f - halfW;
    p->right = 416.0f + halfW;
    p->top = 228.0f - halfH;
    p->bottom = 228.0f + halfH;
    float top, bottom;
    safe_insets(&top, &bottom);
    p->safeTop = top * 456.0f / fh;
    p->safeBottom = bottom * 456.0f / fh;
}

void port_display_layout(float width, float height, PortDisplay* d) {
    SDL_zerop(d);
    if (width <= 0.0f || height <= 0.0f) {
        return;
    }
    if (portrait_window(width, height)) {
        d->portrait = true;
        d->canvas = sCanvasShown;
        if (d->canvas) {
            // The frame at its landscape size, centred; wider than the screen.
            d->h = width;
            d->w = width * (16.0f / 9.0f);
            d->x = (width - d->w) * 0.5f;
            d->y = (height - d->h) * 0.5f;
            return;
        }
        // Letterboxed: the 16:9 frame as wide as the screen, centred.
        d->w = width;
        d->h = width * (9.0f / 16.0f);
        d->x = 0.0f;
        d->y = (height - d->h) * 0.5f;
        return;
    }
    // The safe area's horizontal extent, scaled to the drawable's units.
    float sx0 = 0.0f, sx1 = width;
    int ww = 0, wh = 0;
    SDL_Rect safe;
    if (sWindow != NULL && SDL_GetWindowSize(sWindow, &ww, &wh) && ww > 0 && SDL_GetWindowSafeArea(sWindow, &safe) &&
        safe.w > 0 && (safe.x > 0 || safe.w < ww)) {
        float s = width / (float)ww;
        sx0 = SDL_max(0.0f, (float)safe.x * s);
        sx1 = SDL_min(width, (float)(safe.x + safe.w) * s);
    }
    // The frame, as before: the largest 16:9 rectangle, centred (the same
    // float operations as the old present, so a 16:9 window shows the same
    // pixels), then narrowed to the safe area if needed.
    float dw = width, dh = height;
    if (dw / dh > DISPLAY_ASPECT) {
        dw = dh * DISPLAY_ASPECT;
    } else {
        dh = dw / DISPLAY_ASPECT;
    }
    if (dw > sx1 - sx0) {
        dw = sx1 - sx0;
        dh = dw / DISPLAY_ASPECT;
    }
    float x = (width - dw) * 0.5f;
    if (x < sx0) {
        x = sx0;
    }
    if (x + dw > sx1) {
        x = sx1 - dw;
    }
    d->x = x;
    d->y = (height - dh) * 0.5f;
    d->w = dw;
    d->h = dh;

    static int sFill = -1;
    if (sFill < 0) {
        const char* f = SDL_getenv("FORECAST_FILL");
        sFill = f != NULL && f[0] == '0' ? 0 : 1;
    }
    if (sFill) {
        float maxSide = dh * (FILL_ASPECT - DISPLAY_ASPECT) * 0.5f;
        float left = SDL_min(x, maxSide), right = SDL_min(width - (x + dw), maxSide);
        // Rounding slack of a 16:9 window is no fill.
        float minSide = dh * 0.005f;
        d->fillLeft = left >= minSide ? left : 0.0f;
        d->fillRight = right >= minSide ? right : 0.0f;
    }
}

void port_display_rect(float* x, float* y, float* w, float* h) {
    int ww = 0, wh = 0;
    SDL_GetWindowSize(sWindow, &ww, &wh);
    PortDisplay d;
    port_display_layout((float)ww, (float)wh, &d);
    *x = d.x;
    *y = d.y;
    *w = d.w;
    *h = d.h;
}

void port_pointer(float* x, float* y, bool* down, bool* inside) {
    *x = sPointerX;
    *y = sPointerY;
    *down = sPointerDown;
    *inside = sPointerInside;
}
