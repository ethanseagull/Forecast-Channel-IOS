// KPAD (the Wii Remote library) on a touchscreen.
//
// The targets are touch devices: no cursor is drawn, and a touch (the left
// mouse button on Windows) is the Wii Remote pointer. While nothing touches
// the screen the pointer is off screen. A touch shows the pointer at the touch
// point for one frame (so the channel can hover the button under it), then
// holds A until the touch ends; the pointer stays one frame after release so
// the release lands on the same button.
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
    f32 x, y;
    bool down, inside;
    port_pointer(&x, &y, &down, &inside);

    bool show;
    u32 hold = 0;
    if (down) {
        if (!sWasDown) {
            sHovered = false;
        }
        show = true;
        // First frame of a touch: pointer only; A from the next frame.
        if (sHovered) {
            hold = WPAD_BUTTON_A;
        }
        sHovered = true;
        sShowAfterRelease = true;
    } else {
        show = sShowAfterRelease;
        sShowAfterRelease = false;
    }
    sWasDown = down;

    s->hold = hold;
    s->trig = hold & ~sHold;
    s->release = sHold & ~hold;
    sHold = hold;
    window_to_pos(x, y, &s->pos);
    if (!show) {
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
