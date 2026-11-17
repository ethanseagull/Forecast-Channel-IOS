// Portrait for the channel's own code (milestone 7; port/channel/portrait.cpp).
//
// A phone held upright: the forecast pages and the globe are laid out on a
// canvas around the Wii's frame (port_platform.h, PortPortrait); the other
// screens are letterboxed. The decomp calls these under FORECAST_PORT only.
#ifndef CHANNEL_PORTRAIT_H
#define CHANNEL_PORTRAIT_H

#include <types.h>
#include <port_platform.h>

namespace nw4r {
namespace lyt {
class Pane;
}
} // namespace nw4r

// The screen in game units (the frame is 0..GetScreenWidth() x 0..456, y down).
struct PortraitScreen {
    BOOL active;                 // the window is portrait
    f32 left, top, right, bottom; // the canvas; landscape: the frame
    f32 safeTop, safeBottom;     // the canvas's edges covered by the status bar / home indicator
};
void GetPortraitScreen(PortraitScreen* s);

// The last frame drawn was laid out for the canvas (WeatherScene::Draw sets
// it each frame); code that runs before the draw (input, label fading) asks it.
extern BOOL gPortraitCanvas;

// Asks for the canvas for the frame being drawn (port_portrait_canvas).
void RequestPortraitCanvas(void);

// Moves the panes of the layout `key` ("around", "forecast", ...) to their
// portrait places when the window is portrait. TRUE if it did (the caller's
// buttons must then measure their panes again).
BOOL PortraitLayout(const char* key, nw4r::lyt::Pane* root);

// The credit line's top-left (game units): in the pages' "As of" strip.
void PortraitCreditPos(f32* x, f32* y);

// How far apart the forecast pages are (they scroll vertically): 456 on the
// Wii, the screen's height in portrait.
f32 PortraitPageStep(void);

// On a canvas frame (gPortraitCanvas), these replace the Wii's screen edges
// in the globe's code (they leave the values alone otherwise):
// - PortraitBand: the band between the button bars (y 63..393) -> the whole screen;
// - PortraitLabelArea: where labels stay bright and their boxes go (any may be NULL);
// - PortraitOnScreen: where a city counts as on screen (the screen and 50 around it).
void PortraitBand(f32* top, f32* bottom);
void PortraitLabelArea(f32* top, f32* bottom, f32* left, f32* right);
void PortraitOnScreen(f32* left, f32* top, f32* right, f32* bottom);

#endif
