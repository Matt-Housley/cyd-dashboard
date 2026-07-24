#pragma once
#include <LovyanGFX.hpp>
#include "lgfx_config.h"
#include "config.h"

/// The shared off-screen sprite (defined in ui.cpp) that every screen draws into.
extern LGFX_Sprite spr;

/**
 * @brief Pack an RGB888 constant into the sprite's native colour format.
 * @param rgb888  Colour as 0xRRGGBB.
 * @return The colour converted to the sprite's current depth (16- or 8-bit).
 */
inline uint32_t C(uint32_t rgb888) {
    return spr.color888(
        (rgb888 >> 16) & 0xFF,
        (rgb888 >>  8) & 0xFF,
         rgb888        & 0xFF);
}

// ─── Drawing primitives ───────────────────────────────────────────────────────

/**
 * @brief Draw a filled rectangle with an optional 1-px border.
 * @param x,y,w,h  Rectangle in screen pixels.
 * @param fill     Fill colour (RGB888).
 * @param border   Border colour (RGB888); pass 0 for no border. Defaults to 0.
 */
inline void drawPanel(int x, int y, int w, int h,
                      uint32_t fill, uint32_t border = 0) {
    spr.fillRect(x, y, w, h, C(fill));
    if (border) spr.drawRect(x, y, w, h, C(border));
}

/**
 * @brief Draw a panel with a coloured accent bar down its left edge.
 * @param x,y,w,h   Rectangle in screen pixels.
 * @param accentCol Colour of the 3-px left accent bar (RGB888).
 * @param fill      Panel fill colour (RGB888). Defaults to COL_PANEL.
 */
inline void drawAccentPanel(int x, int y, int w, int h,
                             uint32_t accentCol, uint32_t fill = COL_PANEL) {
    spr.fillRect(x, y, w, h, C(fill));
    spr.drawRect(x, y, w, h, C(COL_BORDER));
    spr.fillRect(x, y, 3, h, C(accentCol));
}

/**
 * @brief Draw a full-width horizontal rule.
 * @param y    Row in screen pixels.
 * @param col  Line colour (RGB888). Defaults to COL_BORDER.
 */
inline void hRule(int y, uint32_t col = COL_BORDER) {
    spr.drawFastHLine(0, y, SCREEN_W, C(col));
}

/**
 * @brief Map a band/geomagnetic condition word to a status colour.
 * @param cond  Condition string (e.g. "Good", "Fair", "Poor"); case-insensitive.
 * @return COL_GREEN for good/excellent, COL_AMBER for fair/normal, else COL_RED.
 */
inline uint32_t condColor(const char* cond) {
    String s = cond; s.toLowerCase();
    if (s.indexOf("good") >= 0 || s.indexOf("excel") >= 0) return COL_GREEN;
    if (s.indexOf("fair") >= 0 || s.indexOf("norm")  >= 0) return COL_AMBER;
    return COL_RED;
}

/**
 * @brief Draw a horizontal band-condition bar (fill proportion from @p cond).
 * @param x,y,w,h   Bar rectangle in screen pixels (w is the full width).
 * @param cond      Condition string; determines fill (good ~90%, fair ~50%, else ~20%).
 * @param progress  Grow-in animation scale, 0.0–1.0 (1.0 = full). Defaults to 1.0.
 */
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

/**
 * @brief Draw a sparkline (line plus translucent fill beneath it).
 * @param x,y,w,h      Plot rectangle in screen pixels.
 * @param vals         Value array; must have at least 2 entries.
 * @param count        Number of entries in @p vals (used to scale the x-axis).
 * @param lineCol      Line colour (RGB888).
 * @param visibleCount How many points to draw left-to-right; -1 draws all.
 *                     X positions always scale against @p count so the axis
 *                     stays stable during a grow-in animation. Defaults to -1.
 */
void drawSparkline(int x, int y, int w, int h,
                   const float* vals, int count, uint32_t lineCol,
                   int visibleCount = -1);

/**
 * @brief Word-wrap and draw text within a bounding box, at the current font.
 *
 * Stops before drawing below @p maxY so text never overflows the box.
 * @param x,y    Top-left of the text in screen pixels.
 * @param text   NUL-terminated text to wrap.
 * @param maxW   Maximum line width in pixels.
 * @param lineH  Line height in pixels. Defaults to 13.
 * @param maxY   Do not draw a line whose bottom would exceed this. Defaults to
 *               SCREEN_H; pass (panelTop + panelH) for a panel.
 * @return The y position just after the last line printed.
 */
int printWrapped(int x, int y, const char* text, int maxW, int lineH = 13,
                 int maxY = SCREEN_H);

/**
 * @brief Format a price with thousands separators (no locale dependency).
 *
 * e.g. 5123.45 -> "5,123.45".
 * @param buf     Destination buffer.
 * @param bufLen  Size of @p buf in bytes.
 * @param price   Value to format.
 */
void fmtPrice(char* buf, size_t bufLen, float price);

/**
 * @brief Draw a centred loader message in the content area.
 * @param msg  Message text; a trailing "..." animates while shown.
 */
void drawLoader(const char* msg);
