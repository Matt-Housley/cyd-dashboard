#pragma once
#include <LovyanGFX.hpp>
#include "lgfx_config.h"
#include "config.h"

// ─── Shared sprite (defined in ui.cpp, used by all screen_*.h) ───────────────
extern LGFX_Sprite spr;

// ─── Colour helper — packs RGB888 constant into the sprite's native format ────
inline uint32_t C(uint32_t rgb888) {
    return spr.color888(
        (rgb888 >> 16) & 0xFF,
        (rgb888 >>  8) & 0xFF,
         rgb888        & 0xFF);
}

// ─── Drawing primitives ───────────────────────────────────────────────────────

// Filled rect with optional 1-px border (pass 0 for no border)
inline void drawPanel(int x, int y, int w, int h,
                      uint32_t fill, uint32_t border = 0) {
    spr.fillRect(x, y, w, h, C(fill));
    if (border) spr.drawRect(x, y, w, h, C(border));
}

// Left-accent panel (coloured left bar, dark fill)
inline void drawAccentPanel(int x, int y, int w, int h,
                             uint32_t accentCol, uint32_t fill = COL_PANEL) {
    spr.fillRect(x, y, w, h, C(fill));
    spr.drawRect(x, y, w, h, C(COL_BORDER));
    spr.fillRect(x, y, 3, h, C(accentCol));
}

// Horizontal rule
inline void hRule(int y, uint32_t col = COL_BORDER) {
    spr.drawFastHLine(0, y, SCREEN_W, C(col));
}

// Condition colour for band/geomag strings
inline uint32_t condColor(const char* cond) {
    String s = cond; s.toLowerCase();
    if (s.indexOf("good") >= 0 || s.indexOf("excel") >= 0) return COL_GREEN;
    if (s.indexOf("fair") >= 0 || s.indexOf("norm")  >= 0) return COL_AMBER;
    return COL_RED;
}

// Horizontal band-condition bar
// barW is the total available width; fill proportion is determined by cond.
// progress (0.0–1.0) scales the filled portion for grow-in animation.
inline void drawBandBar(int x, int y, int w, int h, const char* cond,
                        float progress = 1.0f) {
    String s = cond; s.toLowerCase();
    int fill;
    if (s.indexOf("good") >= 0 || s.indexOf("excel") >= 0) fill = (w * 90) / 100;
    else if (s.indexOf("fair") >= 0 || s.indexOf("norm") >= 0) fill = w / 2;
    else fill = w / 5;
    int animFill = (int)(fill * progress);
    spr.fillRoundRect(x, y, w, h, 2, C(COL_BORDER));
    if (animFill > 0) spr.fillRoundRect(x, y, animFill, h, 2, C(condColor(cond)));
}

// Sparkline — draws line + translucent fill underneath
// vals[] must have at least 2 entries.
// visibleCount limits how many points are drawn (left→right); -1 = draw all.
// X positions are always scaled against the full count so the axis is stable
// during a grow-in animation.
void drawSparkline(int x, int y, int w, int h,
                   const float* vals, int count, uint32_t lineCol,
                   int visibleCount = -1);

// Word-wrap text into lines no wider than maxW pixels, printing at (x,y).
// Stops before drawing below maxY (defaults to the screen bottom) so text
// never overflows a bounding box — pass (panelTop + panelH) as maxY.
// Returns the y position after the last line printed.
int printWrapped(int x, int y, const char* text, int maxW, int lineH = 13,
                 int maxY = SCREEN_H);

// Format a float price without locale (ESP32 printf has no %' grouping)
// e.g. 5123.45 → "5,123.45"
void fmtPrice(char* buf, size_t bufLen, float price);

// Loader message centred in the content area
void drawLoader(const char* msg);
