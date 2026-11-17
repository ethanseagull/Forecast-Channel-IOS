// KPAD (the Wii Remote library) on a touchscreen.
//
// The targets are touch devices: no cursor is drawn, and a touch (the left
// mouse button on Windows) is the Wii Remote pointer. While nothing touches
// the screen the pointer is off screen. A touch shows the pointer at the touch
// point for one frame (so the channel can hover the button under it), then
// holds A until the touch ends; the pointer stays one frame after release so
// the release lands on the same button.
//
// Gestures for what the Remote does without pointing:
// - Tap: the globe's button bars show only while the Wii pointer is on screen
//   and moving or near the top or bottom edge (WeatherAround::UpdateButtonFade).
//   A finger has no hover, so after a tap the pointer is "parked" just off the
//   screen's top-left corner, where it hovers nothing but counts as at the top
//   edge: the bars slide in and stay for PARK_FRAMES. Tapping the middle of the
//   screen again hides them; tapping the bars (a button) keeps them.
// - Pinch: zoom the globe (- / + on the Remote), one step per PINCH_STEP.
// - Two-finger swipe up / down: Up / Down on the Remote's + pad (the globe's
//   tilt; the page above / below on the forecast pages).
// - Mouse wheel (Windows): zoom.
// While two fingers are down the pointer is off screen and A is released, so
// the globe does not spin.
#include <revolution/OS.h>
#include <revolution/KPAD.h>
#include <revolution/SC.h>
#include <revolution/WPAD.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <port_platform.h>

// The displayed EFB: the largest 16:9 rectangle in the window (as in
// port/gx/gx_sdl.cpp).
#define DISPLAY_ASPECT (16.0f / 9.0f)

// The pointer was down last frame / one frame of "hover" has been shown.
static bool sWasDown, sHovered, sShowAfterRelease;
static u32 sHold;

#define PARK_FRAMES 300       // bars shown after a tap (5 s)
#define TAP_FRAMES 20         // a touch at most this long ...
#define TAP_MOVE 0.03f        // ... moving at most this (window heights) is a tap
#define BAR_EDGE 0.72f        // |pointer y| beyond this: the bars (y 63 / 393 of 456)
#define PINCH_STEP 1.18f      // finger distance ratio per zoom step
#define SWIPE_STEP 0.12f      // two-finger travel (window heights) per Up / Down
#define PARK_X (-1.025f)      // ~8 px left of the screen (the channel counts x >= -16)
#define PARK_Y (-1.0f)        // the top edge

static u32 sFrame;
// Single touch: when it started, where, how far it moved; whether the bars
// were parked when it started.
static u32 sTouchStart;
static f32 sTouchX0, sTouchY0, sTouchMove;
static bool sTouchWasParked;
static u32 sParkUntil;
// Two-finger gesture: active until every finger and the mouse button are up.
static bool sGesture;
static f32 sPinchDist0, sSwipeY0;
// Button pulses (one frame down, then at least one frame up).
static u32 sPulseQueue[8];
static int sPulseCount;
static u32 sPulseCooldown;
static f32 sWheel;

static void pulse(u32 button) {
    if (sPulseCount < (int)SDL_arraysize(sPulseQueue)) {
        sPulseQueue[sPulseCount++] = button;
    }
}

static bool SDLCALL wheel_watch(void* user, SDL_Event* e) {
    (void)user;
    if (e->type == SDL_EVENT_MOUSE_WHEEL) {
        sWheel += e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e->wheel.y : e->wheel.y;
    }
    return true;
}

// The fingers on the screen (window-relative 0..1), real or scripted:
// FORECAST_FINGERS=frame:frames:x1:y1:x2:y2:x1':y1':x2':y2'[;...] moves two
// fingers from the first positions to the second over those frames.
static int get_fingers(f32* xy, int max) {
    const char* script = SDL_getenv("FORECAST_FINGERS");
    if (script != NULL && script[0] != '\0') {
        for (const char* t = script; t != NULL && *t != '\0';) {
            unsigned start = 0, len = 0;
            float p[8];
            if (SDL_sscanf(t, "%u:%u:%f:%f:%f:%f:%f:%f:%f:%f", &start, &len, &p[0], &p[1], &p[2], &p[3], &p[4],
                           &p[5], &p[6], &p[7]) == 10 &&
                sFrame >= start && sFrame < start + len && max >= 2) {
                f32 f = len > 1 ? (f32)(sFrame - start) / (f32)(len - 1) : 1.0f;
                for (int i = 0; i < 4; i++) {
                    xy[i] = p[i] + (p[4 + i] - p[i]) * f;
                }
                return 2;
            }
            t = SDL_strchr(t, ';');
            if (t != NULL) {
                t++;
            }
        }
        return 0;
    }
    int n = 0, devices = 0;
    SDL_TouchID* ids = SDL_GetTouchDevices(&devices);
    for (int d = 0; ids != NULL && d < devices; d++) {
        if (ids[d] == SDL_MOUSE_TOUCHID || ids[d] == SDL_PEN_TOUCHID) {
            continue;
        }
        int count = 0;
        SDL_Finger** fingers = SDL_GetTouchFingers(ids[d], &count);
        for (int i = 0; fingers != NULL && i < count && n < max; i++, n++) {
            xy[n * 2] = fingers[i]->x;
            xy[n * 2 + 1] = fingers[i]->y;
        }
        SDL_free(fingers);
    }
    SDL_free(ids);
    return n;
}

// Two fingers: pinch and swipe steps.
static void track_gesture(const f32* xy, bool start) {
    int w = 1, h = 1;
    SDL_GetWindowSize(port_window(), &w, &h);
    f32 aspect = h > 0 ? (f32)w / (f32)h : 1.0f;
    f32 dx = (xy[2] - xy[0]) * aspect, dy = xy[3] - xy[1];
    f32 dist = SDL_sqrtf(dx * dx + dy * dy);
    f32 midY = (xy[1] + xy[3]) * 0.5f;
    if (start || sPinchDist0 <= 0.0f) {
        sPinchDist0 = dist;
        sSwipeY0 = midY;
        return;
    }
    if (dist > sPinchDist0 * PINCH_STEP) {
        pulse(WPAD_BUTTON_PLUS); // spread: closer
        sPinchDist0 *= PINCH_STEP;
    } else if (dist < sPinchDist0 / PINCH_STEP) {
        pulse(WPAD_BUTTON_MINUS); // pinch: farther
        sPinchDist0 /= PINCH_STEP;
    }
    if (midY > sSwipeY0 + SWIPE_STEP) {
        pulse(WPAD_BUTTON_UP); // pulled down: what is above
        sSwipeY0 += SWIPE_STEP;
    } else if (midY < sSwipeY0 - SWIPE_STEP) {
        pulse(WPAD_BUTTON_DOWN);
        sSwipeY0 -= SWIPE_STEP;
    }
}

void KPADInit(void) {}
void KPADSetPosParam(s32 chan, f32 playRadius, f32 sensitivity) {
    (void)chan;
    (void)playRadius;
    (void)sensitivity;
}
void KPADSetDistParam(s32 chan, f32 playRadius, f32 sensitivity) {
    (void)chan;
    (void)playRadius;
    (void)sensitivity;
}
void KPADEnableDPD(s32 chan) { (void)chan; }

// Window coordinates -> KPAD pointer position: -1..1 across the displayed
// frame, x right, y down.
static void window_to_pos(f32 wx, f32 wy, Vec2* pos) {
    int w = 0, h = 0;
    SDL_GetWindowSize(port_window(), &w, &h);
    f32 dw = (f32)w, dh = (f32)h;
    if (dw <= 0.0f || dh <= 0.0f) {
        pos->x = pos->y = 0.0f;
        return;
    }
    if (dw / dh > DISPLAY_ASPECT) {
        dw = dh * DISPLAY_ASPECT;
    } else {
        dh = dw / DISPLAY_ASPECT;
    }
    f32 x0 = ((f32)w - dw) * 0.5f, y0 = ((f32)h - dh) * 0.5f;
    pos->x = (wx - x0) / dw * 2.0f - 1.0f;
    pos->y = (wy - y0) / dh * 2.0f - 1.0f;
}

// The Wii Remote's other buttons on a keyboard (- / + zoom the globe, Esc or Backspace = B,
// Home = HOME, 1 / 2), and for automated runs FORECAST_BUTTON =
// frame:name:frames[;...] with name one of minus plus b home 1 2.
static u32 keyboard_buttons(void) {
    u32 b = 0;
    const bool* k = SDL_GetKeyboardState(NULL);
    if (k != NULL) {
        if (k[SDL_SCANCODE_MINUS] || k[SDL_SCANCODE_KP_MINUS]) {
            b |= WPAD_BUTTON_MINUS;
        }
        if (k[SDL_SCANCODE_EQUALS] || k[SDL_SCANCODE_KP_PLUS]) {
            b |= WPAD_BUTTON_PLUS;
        }
        if (k[SDL_SCANCODE_ESCAPE] || k[SDL_SCANCODE_BACKSPACE]) {
            b |= WPAD_BUTTON_B;
        }
        if (k[SDL_SCANCODE_HOME]) {
            b |= WPAD_BUTTON_HOME;
        }
        if (k[SDL_SCANCODE_1]) {
            b |= WPAD_BUTTON_1;
        }
        if (k[SDL_SCANCODE_2]) {
            b |= WPAD_BUTTON_2;
        }
    }

    static const char* sScript;
    static bool sScriptRead;
    static u32 sCalls;
    if (!sScriptRead) {
        sScript = SDL_getenv("FORECAST_BUTTON");
        sScriptRead = true;
    }
    // KPADRead runs once a frame; frames counted from 1.
    sCalls++;
    for (const char* t = sScript; t != NULL && *t != '\0';) {
        unsigned start = 0, len = 0;
        char name[16] = {0};
        if (SDL_sscanf(t, "%u:%15[^:]:%u", &start, name, &len) == 3 && sCalls >= start && sCalls < start + len) {
            if (SDL_strcmp(name, "minus") == 0) {
                b |= WPAD_BUTTON_MINUS;
            } else if (SDL_strcmp(name, "plus") == 0) {
                b |= WPAD_BUTTON_PLUS;
            } else if (SDL_strcmp(name, "b") == 0) {
                b |= WPAD_BUTTON_B;
            } else if (SDL_strcmp(name, "home") == 0) {
                b |= WPAD_BUTTON_HOME;
            } else if (SDL_strcmp(name, "1") == 0) {
                b |= WPAD_BUTTON_1;
            } else if (SDL_strcmp(name, "2") == 0) {
                b |= WPAD_BUTTON_2;
            }
        }
        t = SDL_strchr(t, ';');
        if (t != NULL) {
            t++;
        }
    }
    return b;
}

s32 KPADRead(s32 chan, KPADStatus* samples, u32 numSamples) {
    if (numSamples == 0) {
        return 0;
    }
    KPADStatus* s = &samples[0];
    memset(s, 0, sizeof(*s));
    if (chan != 0) {
        s->wpad_err = WPAD_ERR_NO_CONTROLLER;
        return 0;
    }
    static bool sWatching;
    if (!sWatching) {
        SDL_AddEventWatch(wheel_watch, NULL);
        sWatching = true;
    }
    sFrame++;

    f32 x, y;
    bool down, inside;
    port_pointer(&x, &y, &down, &inside);
    int ww = 1, wh = 1;
    SDL_GetWindowSize(port_window(), &ww, &wh);
    if (wh <= 0) {
        wh = 1;
    }

    // Two or more fingers: a gesture, until every finger and the button are up.
    f32 fingers[8];
    int nFingers = get_fingers(fingers, 4);
    if (nFingers >= 2) {
        track_gesture(fingers, !sGesture);
        sGesture = true;
        sParkUntil = 0;
    } else if (sGesture && nFingers == 0 && !down) {
        sGesture = false;
        sPinchDist0 = 0.0f;
    }

    bool show;
    u32 hold = 0;
    Vec2 pos;
    window_to_pos(x, y, &pos);
    if (sGesture) {
        show = false;
        sWasDown = down;
        sHovered = sShowAfterRelease = false;
    } else if (down) {
        if (!sWasDown) {
            sHovered = false;
            sTouchStart = sFrame;
            sTouchX0 = x;
            sTouchY0 = y;
            sTouchMove = 0.0f;
            sTouchWasParked = sFrame < sParkUntil;
            sParkUntil = 0;
        }
        f32 mx = (x - sTouchX0) / (f32)wh, my = (y - sTouchY0) / (f32)wh;
        f32 move = SDL_sqrtf(mx * mx + my * my);
        if (move > sTouchMove) {
            sTouchMove = move;
        }
        show = true;
        // First frame of a touch: pointer only; A from the next frame.
        if (sHovered) {
            hold = WPAD_BUTTON_A;
        }
        sHovered = true;
        sShowAfterRelease = true;
        sWasDown = true;
    } else {
        show = sShowAfterRelease;
        sShowAfterRelease = false;
        if (sWasDown) {
            // A tap shows the globe's bars; a tap in the middle while they
            // are shown hides them; a tap on them (a button) keeps them.
            bool tap = sFrame - sTouchStart <= TAP_FRAMES && sTouchMove <= TAP_MOVE;
            bool onBars = pos.y <= -BAR_EDGE || pos.y > BAR_EDGE;
            if (tap && (onBars || !sTouchWasParked)) {
                sParkUntil = sFrame + 1 + PARK_FRAMES;
            }
        }
        sWasDown = false;
    }

    // Wheel notches and gesture steps as one-frame presses.
    while (sWheel >= 1.0f) {
        pulse(WPAD_BUTTON_PLUS);
        sWheel -= 1.0f;
    }
    while (sWheel <= -1.0f) {
        pulse(WPAD_BUTTON_MINUS);
        sWheel += 1.0f;
    }
    if (sPulseCooldown > 0) {
        sPulseCooldown--;
    } else if (sPulseCount > 0) {
        hold |= sPulseQueue[0];
        SDL_memmove(sPulseQueue, sPulseQueue + 1, sizeof(sPulseQueue[0]) * (size_t)--sPulseCount);
        sPulseCooldown = 2;
    }

    hold |= keyboard_buttons();

    s->hold = hold;
    s->trig = hold & ~sHold;
    s->release = sHold & ~hold;
    sHold = hold;
    s->pos = pos;
    if (!show && !sGesture && sFrame < sParkUntil) {
        // Parked: off the top-left corner, at the top edge.
        s->pos.x = PARK_X;
        s->pos.y = PARK_Y;
        show = true;
    } else if (!show) {
        // Off screen.
        s->pos.x = s->pos.y = 2.0f;
    }
    s->horizon.x = 1.0f;
    s->horizon.y = 0.0f;
    s->dist = 1.0f;
    s->acc.y = -1.0f;
    s->acc_value = 1.0f;
    s->dev_type = WPAD_DEV_CORE;
    s->wpad_err = WPAD_ERR_OK;
    s->dpd_valid_fg = show ? 2 : 0;
    s->data_format = 0;
    return 1;
}

// The channel maps pos with this, then multiplies by 1.1666 in 16:9 (the
// Wii's pointer stretch) and adds half the screen. Undo the stretch here so a
// touch lands exactly where the finger is: pos -1..1 spans the whole screen
// rectangle.
void KPADGetProjectionPos(Vec2* out, const Vec2* pos, const Vec2* rect, f32 ratio) {
    (void)ratio;
    f32 stretch = SCGetAspectRatio() == SC_ASPECT_WIDE ? 1.1666666f : 1.0f;
    out->x = pos->x * (rect[1].x - rect[0].x) * 0.5f / stretch;
    out->y = pos->y * (rect[1].y - rect[0].y) * 0.5f / stretch;
}
