#include "screen_apple.h"
#include "ui_common.h"
#include "ui.h"
#include "config.h"
#include "data_store.h"
#include "fetch.h"
#include "fonts/ui_fonts.h"

// Headline hit-boxes recorded during draw, so a tap can open the full text.
static int s_hlY[4], s_hlH[4], s_hlN = 0;

static LGFX_Sprite s_thumbSpr;
static uint32_t    s_thumbVersion = 0xFFFFFFFF;
static bool        s_thumbReady   = false;
static bool        s_thumbAlloc   = false;        // sprite allocated once, then reused
static int         s_thumbW       = 0;
static int         s_thumbH       = 0;

/** @brief Draw the Apple/Mac News screen: masthead, a top-story panel with a
 *         cached hero thumbnail (decoded only when a new image arrives), and
 *         secondary headlines. Records per-headline hit-boxes so appleTouchUp()
 *         can open the full text. See screen_apple.h. */
void drawScreenApple() {
    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    NewsData nd = g_appleNews;
    xSemaphoreGive(g_dataMutex);

    int y = CONTENT_Y + 2;

    // ── Masthead ──────────────────────────────────────────────────────────────
    spr.fillRoundRect(3, y, 112, 14, 3, C(COL_APPLE_GREY));
    spr.setFont(UI_FONT_9);
    spr.setTextColor(C(COL_BG));
    spr.setCursor(7, y + 2);
    spr.print("APPLE / MAC NEWS");
    y += 18;

    s_hlN = 0;   // reset headline hit-boxes for this frame

    if (!nd.valid || nd.count == 0) {
        drawLoader("Fetching Apple News...");
        return;
    }

    // ── Top story panel ───────────────────────────────────────────────────────
    const int topH   = 80;
    const int thumbW = 118, thumbH = 66;
    drawAccentPanel(2, y, SCREEN_W - 4, topH, COL_APPLE_GREY, 0x1A1A1A);

    int textX    = 8;
    int textMaxW = SCREEN_W - 20;

    {
        xSemaphoreTake(g_dataMutex, portMAX_DELAY);
        uint32_t ver = g_appleThumbVersion;
        bool     hasData = (g_appleThumbLen > 0);
        xSemaphoreGive(g_dataMutex);

        if (hasData && ver != s_thumbVersion) {
            // Allocate the decode sprite ONCE and reuse it (see screen_bbc.cpp) —
            // repeated create/deleteSprite() churns ~15 KB of DMA-capable heap and
            // fragments the pool the TLS handshake needs.
            if (!s_thumbAlloc) {
                s_thumbSpr.setColorDepth(16);
                s_thumbAlloc = s_thumbSpr.createSprite(thumbW, thumbH);
            }
            if (s_thumbAlloc) {
                s_thumbReady = false;
                s_thumbSpr.fillSprite(0);
                xSemaphoreTake(g_dataMutex, portMAX_DELAY);
                bool isPng = (g_appleThumbLen >= 4 &&
                              g_appleThumbBuf[0] == 0x89 && g_appleThumbBuf[1] == 'P');
                bool ok;
                if (isPng)
                    ok = s_thumbSpr.drawPng(g_appleThumbBuf, (uint32_t)g_appleThumbLen,
                                            0, 0, thumbW, thumbH);
                else
                    ok = s_thumbSpr.drawJpg(g_appleThumbBuf, (uint32_t)g_appleThumbLen,
                                            0, 0, thumbW, thumbH);
                xSemaphoreGive(g_dataMutex);

                if (ok) {
                    s_thumbVersion = ver;
                    s_thumbW = thumbW;
                    s_thumbH = thumbH;
                    s_thumbReady = true;
                }
                // On decode failure keep the sprite allocated (no churn).
            }
        }
    }

    if (s_thumbReady) {
        int ty = y + (topH - s_thumbH) / 2;
        s_thumbSpr.pushSprite(&spr, 4, ty);
        textX    = 4 + s_thumbW + 4;
        textMaxW = SCREEN_W - textX - 6;
    }

    spr.setFont(UI_FONT_12);
    spr.setTextColor(C(COL_WHITE));
    printWrapped(textX, y + 6, nd.items[0].title, textMaxW, 16, y + topH - 4);
    s_hlY[0] = y; s_hlH[0] = topH; s_hlN = 1;
    y += topH + 3;

    // ── Stories 2–4 ───────────────────────────────────────────────────────────
    for (int i = 1; i < nd.count && i < 4; i++) {
        const int rowH = 40;
        s_hlY[i] = y; s_hlH[i] = rowH; s_hlN = i + 1;
        drawPanel(2, y, SCREEN_W - 4, rowH, COL_PANEL, COL_BORDER);
        spr.fillRect(2, y, 2, rowH, C(COL_APPLE_GREY));

        spr.setFont(UI_FONT_12);
        spr.setTextColor(C(COL_DIM_WHITE));
        printWrapped(10, y + 5, nd.items[i].title, SCREEN_W - 22, 16, y + rowH - 3);

        y += rowH + 2;
    }
}

/**
 * @brief Handle a tap on the Apple News screen (finger-up).
 *
 * If the tap falls within a headline's recorded hit-box, opens the full
 * headline text in a popup (useful when a title was truncated).
 * @param x  Touch x in screen pixels (unused; headlines span the width).
 * @param y  Touch y in screen pixels.
 * @return true if a headline was tapped and its popup shown, false otherwise.
 */
bool appleTouchUp(int32_t x, int32_t y) {
    for (int i = 0; i < s_hlN; i++) {
        if (y >= s_hlY[i] && y < s_hlY[i] + s_hlH[i]) {
            char t[100] = {};
            xSemaphoreTake(g_dataMutex, portMAX_DELAY);
            if (i < g_appleNews.count) strlcpy(t, g_appleNews.items[i].title, sizeof(t));
            xSemaphoreGive(g_dataMutex);
            if (t[0]) { showTextPopup(t); return true; }
        }
    }
    return false;
}
