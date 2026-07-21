#include "screen_bbc.h"
#include "ui_common.h"
#include "config.h"
#include "data_store.h"
#include "fetch.h"
#include "fonts/ui_fonts.h"

// Decoded thumbnail sprite — rebuilt only when g_bbcThumbVersion changes.
// Decoding JPEG on every draw frame (30fps) causes intermittent malloc failures
// inside the JPEG decoder which leave the thumbnail area blank for that frame,
// producing a visible flash. Caching the decoded pixels eliminates this.
static LGFX_Sprite s_thumbSpr;
static uint32_t    s_thumbVersion = 0xFFFFFFFF;   // force decode on first call
static bool        s_thumbReady   = false;
static int         s_thumbW       = 0;
static int         s_thumbH       = 0;

void drawScreenBBC() {
    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    NewsData nd = g_bbcNews;
    xSemaphoreGive(g_dataMutex);

    int y = CONTENT_Y + 2;

    // ── Masthead ──────────────────────────────────────────────────────────────
    spr.fillRoundRect(3, y, 64, 14, 3, C(COL_BBC_RED));
    spr.setFont(UI_FONT_9);
    spr.setTextColor(C(COL_WHITE));
    spr.setCursor(7, y + 2);
    spr.print("BBC NEWS");
    y += 18;

    if (!nd.valid || nd.count == 0) {
        drawLoader("Fetching BBC News...");
        return;
    }

    // ── Top story panel ───────────────────────────────────────────────────────
    const int topH  = 80;
    const int thumbW = 118, thumbH = 66;
    drawAccentPanel(2, y, SCREEN_W - 4, topH, COL_BBC_RED, COL_PANEL);

    int textX    = 8;
    int textMaxW = SCREEN_W - 20;

    // Rebuild cached sprite only when a new thumbnail has been downloaded.
    // Decoding under the mutex keeps the compressed buffer stable; on success
    // the sprite is self-contained and the mutex is not needed at draw time.
    {
        xSemaphoreTake(g_dataMutex, portMAX_DELAY);
        uint32_t ver = g_bbcThumbVersion;
        bool     hasData = (g_bbcThumbLen > 0);
        xSemaphoreGive(g_dataMutex);

        if (hasData && ver != s_thumbVersion) {
            s_thumbReady = false;
            s_thumbSpr.deleteSprite();
            s_thumbSpr.setColorDepth(16);
            if (s_thumbSpr.createSprite(thumbW, thumbH)) {
                s_thumbSpr.fillSprite(0);
                xSemaphoreTake(g_dataMutex, portMAX_DELAY);
                bool isPng = (g_bbcThumbLen >= 4 &&
                              g_bbcThumbBuf[0] == 0x89 && g_bbcThumbBuf[1] == 'P');
                bool ok;
                if (isPng)
                    ok = s_thumbSpr.drawPng(g_bbcThumbBuf, (uint32_t)g_bbcThumbLen,
                                            0, 0, thumbW, thumbH);
                else
                    ok = s_thumbSpr.drawJpg(g_bbcThumbBuf, (uint32_t)g_bbcThumbLen,
                                            0, 0, thumbW, thumbH);
                xSemaphoreGive(g_dataMutex);

                if (ok) {
                    s_thumbVersion = ver;
                    s_thumbW = thumbW;
                    s_thumbH = thumbH;
                    s_thumbReady = true;
                } else {
                    s_thumbSpr.deleteSprite();
                }
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
    y += topH + 3;

    // ── Stories 2–4 ───────────────────────────────────────────────────────────
    for (int i = 1; i < nd.count && i < 4; i++) {
        const int rowH = 40;
        drawPanel(2, y, SCREEN_W - 4, rowH, 0x111111, COL_BORDER);

        spr.fillCircle(13, y + rowH / 2, 9, C(COL_BBC_RED));
        spr.setFont(UI_FONT_9);
        spr.setTextColor(C(COL_WHITE));
        // 12 = widest an int can print, so truncation is impossible.
        char n[12]; snprintf(n, sizeof(n), "%d", i + 1);
        int nw = spr.textWidth(n);
        spr.setCursor(13 - nw / 2, y + rowH / 2 - 5);
        spr.print(n);

        spr.setFont(UI_FONT_12);
        spr.setTextColor(C(COL_DIM_WHITE));
        printWrapped(27, y + 5, nd.items[i].title, SCREEN_W - 38, 16, y + rowH - 3);

        y += rowH + 2;
    }
}
