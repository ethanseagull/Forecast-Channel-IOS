// Portrait layouts (milestone 7): where the parts of the channel's layouts go
// on a phone held upright.
//
// The screen is a canvas around the Wii's 16:9 frame (port_platform.h,
// PortPortrait), in the game's units: the frame is 0..832 x 0..456 (y down)
// and an iPhone 15 Pro's canvas about 182..650 x -266..722. The layouts were
// made for 608 x 456 and stretched to 16:9 (a layout unit is 832 / 608 game
// units across); panes marked "location adjust" draw their contents
// unstretched. Positions here are in game units, measured from the canvas's
// edges and its safe area (status bar, home indicator), so they suit any
// phone. tools/lyt_dump.py lists the panes of the user's layout files.
//
// PortraitLayout runs once, when a layout is loaded (ButtonGroup::ApplyPortrait),
// before the channel's code measures the panes (button hit areas, text boxes).
#include <channel_portrait.h>

#include <nw4r/lyt/lyt_pane.h>
#include <nw4r/math.h>

#include <port_log.h>
#include <port_platform.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

BOOL gPortraitCanvas;

void GetPortraitScreen(PortraitScreen* s) {
    PortPortrait p;
    port_portrait(&p);
    if (!p.active) {
        s->active = FALSE;
        s->left = 0.0f;
        s->top = 0.0f;
        s->right = 832.0f;
        s->bottom = 456.0f;
        s->safeTop = 0.0f;
        s->safeBottom = 0.0f;
        return;
    }
    s->active = TRUE;
    s->left = p.left;
    s->top = p.top;
    s->right = p.right;
    s->bottom = p.bottom;
    s->safeTop = p.safeTop;
    s->safeBottom = p.safeBottom;
}

void RequestPortraitCanvas(void) { port_portrait_canvas(); }

void PortraitBand(f32* top, f32* bottom) {
    if (!gPortraitCanvas) {
        return;
    }
    PortraitScreen s;
    GetPortraitScreen(&s);
    *top = s.top - 1.0f;
    *bottom = s.bottom + 1.0f;
}

void PortraitLabelArea(f32* top, f32* bottom, f32* left, f32* right) {
    if (!gPortraitCanvas) {
        return;
    }
    PortraitScreen s;
    GetPortraitScreen(&s);
    if (top != NULL) {
        *top = s.top + s.safeTop + 12.0f;
    }
    if (bottom != NULL) {
        *bottom = s.bottom - s.safeBottom - 12.0f;
    }
    if (left != NULL) {
        *left = s.left + 10.0f;
    }
    if (right != NULL) {
        *right = s.right - 10.0f;
    }
}

void PortraitOnScreen(f32* left, f32* top, f32* right, f32* bottom) {
    if (!gPortraitCanvas) {
        return;
    }
    PortraitScreen s;
    GetPortraitScreen(&s);
    *left = s.left - 50.0f;
    *top = s.top - 50.0f;
    *right = s.right + 50.0f;
    *bottom = s.bottom + 50.0f;
}

namespace {

using nw4r::lyt::Pane;
using nw4r::lyt::PaneList;
using nw4r::lyt::Size;
using nw4r::math::VEC2;
using nw4r::math::VEC3;

// One layout unit across, in game units.
const f32 kWide = 832.0f / 608.0f;

// The canvas, and its part clear of the status bar and home indicator with a
// small margin (where the controls go).
struct Area {
    f32 left, top, right, bottom;
};

struct Ctx {
    PortraitScreen s;
    Area c; // content area
    Pane* root;
    const char* key;
};

// Game coordinates -> a top-level pane's translate (layout units, centre
// origin, y up).
VEC3 ToLayout(f32 gx, f32 gy) { return VEC3((gx - 416.0f) / kWide, 228.0f - gy, 0.0f); }

Pane* Find(Ctx& c, Pane* parent, const char* name) {
    Pane* p = parent->FindPaneByName(name, true);
    if (p == NULL) {
        port_log("portrait: %s: no pane %s\n", c.key, name);
    }
    return p;
}

// A top-level pane centred at (gx, gy).
void Place(Ctx& c, const char* name, f32 gx, f32 gy) {
    Pane* p = Find(c, c.root, name);
    if (p != NULL) {
        p->SetTranslate(ToLayout(gx, gy));
    }
}

// A top-level pane moved by (dx, dy) game units.
void Shift(Ctx& c, const char* name, f32 dx, f32 dy) {
    Pane* p = Find(c, c.root, name);
    if (p != NULL) {
        VEC3 t = p->GetTranslate();
        p->SetTranslate(VEC3(t.x + dx / kWide, t.y - dy, t.z));
    }
}

void CentreChildren(Pane* p) {
    PaneList& list = p->GetChildList();
    for (PaneList::Iterator it = list.GetBeginIter(); it != list.GetEndIter(); ++it) {
        it->SetTranslate(VEC3(0.0f, 0.0f, 0.0f));
    }
}

// Makes the bar button `name` a free-standing tile w x h (game units) centred
// at (gx, gy): its bar art (<name>B, a null pane over the art pieces) scaled
// into the tile, the thin separator pieces hidden; its icon (<name>I) and,
// with text, its label (<name>T) centred on it, else the label hidden. The
// button's hit area is <name>B's size, so that is set to the tile.
void Tile(Ctx& c, const char* name, f32 gx, f32 gy, f32 w, f32 h, bool text) {
    Pane* btn = Find(c, c.root, name);
    if (btn == NULL) {
        return;
    }
    btn->SetTranslate(ToLayout(gx, gy));
    char sub[32];
    snprintf(sub, sizeof(sub), "%sB", name);
    Pane* base = Find(c, btn, sub);
    if (base != NULL) {
        Size size = base->GetSize();
        VEC2 scale = base->GetScale();
        PaneList& list = base->GetChildList();
        for (PaneList::Iterator it = list.GetBeginIter(); it != list.GetEndIter(); ++it) {
            if (it->GetSize().width <= 8.0f) {
                it->SetVisible(false); // a separator between bar buttons
            } else {
                it->SetTranslate(VEC3(0.0f, 0.0f, 0.0f));
                it->SetSize(size);
            }
        }
        f32 lw = w / kWide;
        base->SetTranslate(VEC3(0.0f, 0.0f, 0.0f));
        base->SetScale(VEC2((scale.x < 0.0f ? -1.0f : 1.0f) * lw / size.width,
                            (scale.y < 0.0f ? -1.0f : 1.0f) * h / size.height));
        base->SetSize(Size(lw, h));
    }
    snprintf(sub, sizeof(sub), "%sI", name);
    Pane* icon = btn->FindPaneByName(sub, true);
    if (icon != NULL) {
        icon->SetTranslate(VEC3(0.0f, 0.0f, 0.0f));
        CentreChildren(icon);
    }
    snprintf(sub, sizeof(sub), "%sT", name);
    Pane* label = btn->FindPaneByName(sub, true);
    if (label != NULL) {
        if (text) {
            label->SetTranslate(VEC3(0.0f, 0.0f, 0.0f));
            CentreChildren(label);
        } else {
            label->SetVisible(false);
        }
    }
}

// --- the globe ------------------------------------------------------------------------
//
// Laid out like Google Earth on a phone: the controls float over the globe
// and stay up. Top: End (back to the forecast) at the left, Next (what the
// labels show) at the right with the title belt under it. Right, within the
// thumb's reach: Zoom in / out and the two tilts; Restore at the bottom
// right; the credit line at the bottom left with the temperature legend
// above it.

const f32 kTileH = 46.0f;   // tile height (game units)
const f32 kSquare = 52.0f;  // icon tiles
const f32 kGap = 12.0f;

void Around(Ctx& c) {
    const Area& a = c.c;
    // The top row beside the Dynamic Island (the status bar is hidden): its
    // middle 34 below the screen's top, about the island's.
    f32 rowY = c.s.top + 34.0f;
    Tile(c, "back", a.left + 56.0f, rowY, 112.0f, kTileH, true);
    Tile(c, "next", a.right - kSquare * 0.5f, rowY, kSquare, kTileH, false);
    f32 x = a.right - kSquare * 0.5f;
    f32 restoreY = a.bottom - kTileH * 0.5f;
    Tile(c, "reset", a.right - 64.0f, restoreY, 128.0f, kTileH, true);
    f32 y = restoreY - kTileH * 0.5f - 2.0f * kGap - kSquare * 0.5f;
    const char* column[] = {"rot_b", "rot_a", "zoom_out", "zoom_in"}; // bottom to top
    for (int i = 0; i < 4; i++, y -= kSquare + kGap) {
        Tile(c, column[i], x, y, kSquare, kSquare, false);
    }
}

void AroundBelt(Ctx& c) {
    const Area& a = c.c;
    // The belt's right end at the screen's edge, under the top row.
    Place(c, "belt", c.s.right, c.s.top + 34.0f + kTileH * 0.5f + 26.0f);
    // The temperature legend: bottom left, above the credit line.
    f32 dx = (a.left + 48.0f) - 744.0f, dy = (a.bottom - 150.0f) - 355.0f;
    const char* legend[] = {"kion", "rain", "high", "text_kionH", "text_kionL", "text_ondo"};
    for (int i = 0; i < 6; i++) {
        Shift(c, legend[i], dx, dy);
    }
}

// --- the forecast pages -------------------------------------------------------------------
//
// Top to bottom: the top button row (▲ previous page, Settings) under the
// status bar; the belt (day and city); the page's content; the strip with
// the "As of" time; the bottom row (Wii Menu, ▼ next page, Globe) over the
// home indicator. The pages follow each other vertically, a screen apart
// (PortraitPageStep), as they did 456 apart on the Wii.

const f32 kBarH = 52.0f;   // a button row's visible height
const f32 kBeltH = 50.0f;  // the day / city belt (74..124 on the Wii)

struct PageGeo {
    f32 topBand, beltY, contentTop, contentBottom, footY, bottomBand;
};

PageGeo Geo(const PortraitScreen& s) {
    PageGeo g;
    g.topBand = s.top + s.safeTop;                   // top of the top row's visible part
    g.beltY = g.topBand + kBarH + 12.0f + kBeltH * 0.5f;
    g.contentTop = g.beltY + kBeltH * 0.5f + 12.0f;
    g.bottomBand = s.bottom - s.safeBottom - kBarH;  // top of the bottom row
    g.footY = g.bottomBand - 4.0f - 11.0f;           // the 22-high "As of" strip
    g.contentBottom = g.footY - 11.0f - 8.0f;
    return g;
}

// The bar button `name` across x0..x1 of the top (or bottom) row: its art
// from the screen's edge to the row's inner edge, its label (and icon) in the
// visible part, iconDx / textDx moving the icon / label sideways (game units,
// 0 keeps them).
void BarButton(Ctx& c, const char* name, f32 x0, f32 x1, bool top, f32 iconDx, f32 textDx, f32 textScale = 1.0f) {
    Pane* btn = Find(c, c.root, name);
    if (btn == NULL) {
        return;
    }
    PageGeo g = Geo(c.s);
    f32 bandMid, artTop, artBottom, originY;
    if (top) {
        bandMid = g.topBand + kBarH * 0.5f;
        artTop = c.s.top;
        artBottom = g.topBand + kBarH;
        originY = bandMid - 10.0f; // the label sits 10 below the origin
    } else {
        bandMid = g.bottomBand + kBarH * 0.5f;
        artTop = g.bottomBand;
        artBottom = c.s.bottom;
        originY = bandMid + 14.0f; // ... 14 above it
    }
    f32 cx = 0.5f * (x0 + x1);
    btn->SetTranslate(ToLayout(cx, originY));
    char sub[32];
    snprintf(sub, sizeof(sub), "%sB", name);
    Pane* base = Find(c, btn, sub);
    if (base != NULL) {
        Size size = base->GetSize();
        VEC2 scale = base->GetScale();
        f32 lw = (x1 - x0) / kWide, h = artBottom - artTop;
        base->SetTranslate(VEC3(0.0f, originY - 0.5f * (artTop + artBottom), 0.0f));
        base->SetScale(VEC2((scale.x < 0.0f ? -1.0f : 1.0f) * lw / size.width,
                            (scale.y < 0.0f ? -1.0f : 1.0f) * h / size.height));
        base->SetSize(Size(lw, h));
    }
    if (iconDx != 0.0f) {
        snprintf(sub, sizeof(sub), "%sI", name);
        Pane* icon = btn->FindPaneByName(sub, true);
        if (icon != NULL) {
            VEC3 t = icon->GetTranslate();
            icon->SetTranslate(VEC3(t.x + iconDx / kWide, t.y, t.z));
        }
    }
    if (textDx != 0.0f || textScale != 1.0f) {
        snprintf(sub, sizeof(sub), "%sT", name);
        Pane* label = btn->FindPaneByName(sub, true);
        if (label != NULL) {
            VEC3 t = label->GetTranslate();
            label->SetTranslate(VEC3(t.x + textDx / kWide, t.y, t.z));
            label->SetScale(VEC2(textScale, textScale));
        }
    }
}

void Forecast(Ctx& c) {
    f32 l = c.s.left, r = c.s.right;
    Pane* blank = Find(c, c.root, "blank");
    if (blank != NULL) {
        blank->SetVisible(false);
        blank->SetTranslate(ToLayout(-2000.0f, -2000.0f)); // and out of reach
    }
    f32 split = l + 0.6f * (r - l);
    BarButton(c, "up", l, split, true, 0.0f, 0.0f);
    BarButton(c, "set", split, r, true, 0.0f, 0.0f);
    f32 w = r - l, side = 0.25f * w;
    BarButton(c, "back", l, l + side, false, 0.0f, 0.0f);
    // The middle one is narrower than on the Wii: icon and label closer,
    // the label a little smaller ("5-Day Forecast").
    BarButton(c, "down", l + side, r - side, false, 24.0f, -4.0f, 0.88f);
    BarButton(c, "around", r - side, r, false, 0.0f, 0.0f);
}

void Base(Ctx& c) {
    Pane* bg = Find(c, c.root, "bg");
    if (bg != NULL) {
        f32 cx = 0.5f * (c.s.left + c.s.right), cy = 0.5f * (c.s.top + c.s.bottom);
        bg->SetTranslate(ToLayout(cx, cy));
        bg->SetSize(Size((c.s.right - c.s.left) / kWide + 2.0f, c.s.bottom - c.s.top + 2.0f));
    }
}

void BaseBelt(Ctx& c) {
    PageGeo g = Geo(c.s);
    Place(c, "belt", c.s.left, g.beltY);
    Place(c, "city", c.s.right - 38.0f, g.beltY - 1.0f);
    f32 dy = g.footY - 374.0f;
    Shift(c, "belt_logo", c.s.left, dy);
    Shift(c, "belt_time", c.s.right - 832.0f, dy);
    Shift(c, "timeWW", c.s.right - 832.0f, dy);
    Shift(c, "timeJP", c.s.right - 832.0f, dy);
}

// Current, Today, Tomorrow: the icon, the temperature, the forecast's words
// and the wind, one under the other.
void Day(Ctx& c) {
    PageGeo g = Geo(c.s);
    f32 h = g.contentBottom - g.contentTop, cx = 0.5f * (c.s.left + c.s.right);
    // The icon is drawn about 110 high around its pane.
    Place(c, "icon", cx, g.contentTop + 0.19f * h);
    f32 tempY = g.contentTop + 0.48f * h;
    // °F and °C have their own panes, 10 apart.
    Place(c, "kion_do_cnt", cx, tempY);
    Place(c, "kion_doF_cnt", cx - 10.0f * kWide, tempY);
    Place(c, "telop", cx, g.contentTop + 0.70f * h);
    f32 windY = g.contentTop + 0.90f * h;
    Place(c, "belt_wind", c.s.right, windY);
    Place(c, "wind_telop", c.s.right - 38.0f, windY + 1.0f);
}

// 5-Day Forecast: each day's column becomes a row (day, icon, high, low).
void Week(Ctx& c) {
    PageGeo g = Geo(c.s);
    const f32 gap = 10.0f;
    f32 rowH = (g.contentBottom - g.contentTop - 4.0f * gap) / 5.0f;
    if (rowH > 104.0f) {
        rowH = 104.0f;
    }
    f32 total = 5.0f * rowH + 4.0f * gap;
    f32 y = 0.5f * (g.contentTop + g.contentBottom - total) + 0.5f * rowH;
    f32 cx = 0.5f * (c.s.left + c.s.right), rowW = c.s.right - c.s.left - 24.0f;
    f32 x0 = cx - 0.5f * rowW;
    const f32 dayW = 104.0f;
    const char days[] = "abcde";
    char name[32];
    for (int i = 0; i < 5; i++, y += rowH + gap) {
        char d = days[i];
        // The column's art: the body becomes the row, the header its day cell.
        snprintf(name, sizeof(name), "week_%c", d);
        Pane* col = Find(c, c.root, name);
        if (col != NULL) {
            col->SetTranslate(ToLayout(cx, y));
            const char* parts[] = {"0", "1", "2", "3"};
            for (int k = 0; k < 4; k++) {
                snprintf(name, sizeof(name), "week_%cB%s", d, parts[k]);
                Pane* p = Find(c, col, name);
                if (p == NULL) {
                    continue;
                }
                if (k == 0) { // header -> the day cell at the row's left
                    p->SetTranslate(VEC3((x0 + 0.5f * dayW - cx) / kWide, 0.0f, 0.0f));
                    p->SetSize(Size(dayW / kWide, rowH));
                } else if (k == 1) { // body -> the row
                    p->SetTranslate(VEC3(0.0f, 0.0f, 0.0f));
                    p->SetSize(Size(rowW / kWide, rowH));
                } else { // the header's underline, the column's side line
                    p->SetVisible(false);
                }
            }
        }
        snprintf(name, sizeof(name), "week_%c_youbi", d);
        Place(c, name, x0 + 0.5f * dayW, y);
        snprintf(name, sizeof(name), "week_%c_icon", d);
        Place(c, name, x0 + dayW + 0.30f * (rowW - dayW), y);
        f32 hiX = x0 + dayW + 0.64f * (rowW - dayW), loX = x0 + dayW + 0.88f * (rowW - dayW);
        snprintf(name, sizeof(name), "week_%cH_cnt", d);
        Place(c, name, hiX, y);
        snprintf(name, sizeof(name), "week_%cH_do_cnt", d);
        Place(c, name, hiX + 4.0f * kWide, y);
        snprintf(name, sizeof(name), "week_%cL_cnt", d);
        Place(c, name, loX, y);
        snprintf(name, sizeof(name), "week_%cL_do_cnt", d);
        Place(c, name, loX + 4.0f * kWide, y);
    }
}

// A button's art (<name>B) resized to w x h game units, its parts with it;
// the hit area follows.
void ResizeButton(Ctx& c, const char* name, f32 w, f32 h) {
    Pane* btn = Find(c, c.root, name);
    if (btn == NULL) {
        return;
    }
    char sub[32];
    snprintf(sub, sizeof(sub), "%sB", name);
    Pane* base = Find(c, btn, sub);
    if (base == NULL) {
        return;
    }
    Size size = base->GetSize();
    VEC2 scale = base->GetScale();
    f32 lw = w / kWide;
    base->SetScale(VEC2((scale.x < 0.0f ? -1.0f : 1.0f) * lw / size.width,
                        (scale.y < 0.0f ? -1.0f : 1.0f) * h / size.height));
    base->SetSize(Size(lw, h));
}

void Scale(Ctx& c, const char* name, f32 k) {
    Pane* p = Find(c, c.root, name);
    if (p != NULL) {
        VEC2 s = p->GetScale();
        p->SetScale(VEC2(s.x * k, s.y * k));
    }
}

// UV Index: the sun over the "UV Index" bar, the number and its words in the
// two boxes under it, all within the screen's width.
void Life(Ctx& c) {
    PageGeo g = Geo(c.s);
    f32 cx = 0.5f * (c.s.left + c.s.right), w = c.s.right - c.s.left - 24.0f;
    f32 half = 0.5f * w, numW = 120.0f, gap = 8.0f, textW = w - numW - gap;
    f32 gy = g.contentTop + 0.40f * (g.contentBottom - g.contentTop);
    Pane* grp = Find(c, c.root, "life_b");
    if (grp == NULL) {
        return;
    }
    grp->SetTranslate(ToLayout(cx, gy));
    // Parts in the group's space: layout units across (game / kWide), y up.
    struct Part {
        const char* name;
        f32 x, w; // game units from the group's centre; w 0 keeps the size
    };
    f32 numX = -half + 0.5f * numW, textX = -half + numW + gap + 0.5f * textW;
    const Part parts[] = {
        {"life_bB0", 0.0f, w},           // the "UV Index" bar
        {"life_bB2", 0.0f, w},           // its top line
        {"life_bB3", -half, 0.0f},       // its left line
        {"life_bB1", numX, numW},        // the number's box
        {"life_bB4", numX, numW},        // its top line
        {"life_bB5", -half, 0.0f},       // its left line
        {"life_bB6", textX, textW},      // the words' box
        {"life_bB7", textX, textW},      // its top line
        {"life_bB8", -half + numW + gap, 0.0f}, // its left line
    };
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        Pane* p = Find(c, grp, parts[i].name);
        if (p == NULL) {
            continue;
        }
        VEC3 t = p->GetTranslate();
        p->SetTranslate(VEC3(parts[i].x / kWide, t.y, t.z));
        if (parts[i].w > 0.0f) {
            p->SetSize(Size(parts[i].w / kWide, p->GetSize().height));
        }
    }
    Pane* icon = Find(c, grp, "life_bI"); // the sun: above the bar
    if (icon != NULL) {
        icon->SetTranslate(VEC3(0.0f, 18.0f + 8.0f + 56.0f, 0.0f));
    }
    Pane* label = Find(c, grp, "life_bT");
    if (label != NULL) {
        label->SetTranslate(VEC3(0.0f, 0.0f, 0.0f));
    }
    // The number's digits and the words: where they were in their boxes.
    f32 boxY = gy + 75.0f; // the boxes' middle (75 below the bar)
    Place(c, "life_b_1-1", cx + numX, boxY - 5.0f);
    Place(c, "life_b_1-10", cx + numX + 19.0f * kWide, boxY - 5.0f);
    Place(c, "life_b_1-100", cx + numX + 38.0f * kWide, boxY - 5.0f);
    Place(c, "life_b_telop", cx + textX, boxY);
}

// Settings: the title under the Dynamic Island; for each setting its label
// bar, then its value with the Change button beside it; the Back row at
// the bottom.
void Set(Ctx& c) {
    PageGeo g = Geo(c.s);
    f32 l = c.s.left, r = c.s.right, w = r - l, cx = 0.5f * (l + r);
    f32 y = g.topBand + 18.0f;
    Place(c, "belt", cx, y);
    Pane* belt = Find(c, c.root, "beltB");
    if (belt != NULL) {
        belt->SetSize(Size(w / kWide + 2.0f, belt->GetSize().height));
    }
    const f32 btnW = 170.0f, labelH = 32.0f, valueH = 58.0f, rowGap = 40.0f;
    f32 valueCx = l + 12.0f + 0.5f * (w - 24.0f - btnW - 10.0f);
    const char* rows[] = {"city", "kion", "wind"};
    char name[32];
    y += 15.0f + 36.0f;
    for (int i = 0; i < 3; i++) {
        f32 labelY = y + 0.5f * labelH, valueY = y + labelH + 6.0f + 0.5f * valueH;
        Place(c, rows[i], cx, labelY);
        snprintf(name, sizeof(name), "%sT", rows[i]);
        Pane* t = Find(c, c.root, name);
        if (t != NULL) {
            t->SetTranslate(VEC3(0.0f, t->GetTranslate().y, 0.0f)); // label centred
        }
        snprintf(name, sizeof(name), "%s_set", rows[i]);
        Place(c, name, cx, valueY);
        snprintf(name, sizeof(name), "%s_setT", rows[i]);
        t = Find(c, c.root, name);
        if (t != NULL) {
            t->SetTranslate(VEC3((valueCx - cx) / kWide, t->GetTranslate().y, 0.0f));
        }
        snprintf(name, sizeof(name), "%s_btn", rows[i]);
        Place(c, name, r - 12.0f - 0.5f * btnW, valueY);
        ResizeButton(c, name, btnW, 64.0f);
        y += labelH + 6.0f + valueH + rowGap;
    }
    Place(c, "text", cx, y + 40.0f);
    Scale(c, "text", 0.82f);
    BarButton(c, "back", l, l + 0.3f * w, false, 0.0f, 0.0f);
    BarButton(c, "blank", l + 0.3f * w, r, false, 0.0f, 0.0f);
}

// "One moment, please..." and the connection errors: the background over
// the whole screen, the message box (wider than the screen) made smaller.
void Dialog(Ctx& c) {
    Base(c);
    const f32 k = 0.82f;
    Scale(c, "text", k);
    // Only the error screen (error0) has these.
    if (c.root->FindPaneByName("next", true) != NULL) {
        Scale(c, "error_server", k);
        Scale(c, "error_code", k);
        ResizeButton(c, "next", 300.0f, 84.0f);
    }
}

struct Entry {
    const char* key;
    void (*fn)(Ctx&);
};

const Entry kLayouts[] = {
    {"around", Around},
    {"around_belt", AroundBelt},
    {"forecast", Forecast},
    {"base", Base},
    {"base_belt", BaseBelt},
    {"day_nWW", Day},
    {"week_nWW", Week},
    {"life_nWW", Life},
    {"set", Set},
    {"error0", Dialog},
    {"error1", Dialog},
};

} // namespace

BOOL PortraitLayout(const char* key, Pane* root) {
    Ctx c;
    GetPortraitScreen(&c.s);
    if (!c.s.active || root == NULL) {
        return FALSE;
    }
    const f32 margin = 12.0f;
    c.c.left = c.s.left + margin;
    c.c.right = c.s.right - margin;
    c.c.top = c.s.top + c.s.safeTop + margin * 0.5f;
    c.c.bottom = c.s.bottom - c.s.safeBottom - margin * 0.5f;
    c.root = root;
    c.key = key;
    for (size_t i = 0; i < sizeof(kLayouts) / sizeof(kLayouts[0]); i++) {
        size_t n = strlen(kLayouts[i].key);
        if (strncmp(kLayouts[i].key, key, n) == 0 && (key[n] == '\0' || key[n] == '.')) {
            kLayouts[i].fn(c);
            return TRUE;
        }
    }
    return FALSE;
}

void PortraitCreditPos(f32* x, f32* y) {
    PortraitScreen s;
    GetPortraitScreen(&s);
    // Just above the pages' "As of" strip: the narrow screen has no room
    // for both in it.
    *x = s.left + 16.0f;
    *y = Geo(s).footY - 11.0f - 4.0f - 26.0f;
}

f32 PortraitPageStep(void) {
    PortraitScreen s;
    GetPortraitScreen(&s);
    return s.active ? ceilf(s.bottom - s.top) : 456.0f;
}
