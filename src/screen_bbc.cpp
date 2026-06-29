#include "screen_bbc.h"
#include "ui_common.h"
#include "config.h"
#include "data_store.h"
#include "fetch.h"
#include "fonts/ui_fonts.h"

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

    int textX   = 8;
    int textMaxW = SCREEN_W - 20;

    // Thumbnail — hold the mutex during decode so fetchThumb cannot overwrite
    // the buffer on the other core while drawJpg/drawPng is reading it.
    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    if (g_bbcThumbLen > 0) {
        int ty = y + (topH - thumbH) / 2;
        bool isPng = (g_bbcThumbLen >= 4 &&
                      g_bbcThumbBuf[0] == 0x89 && g_bbcThumbBuf[1] == 'P');
        if (isPng)
            spr.drawPng(g_bbcThumbBuf, (uint32_t)g_bbcThumbLen, 4, ty, thumbW, thumbH);
        else
            spr.drawJpg(g_bbcThumbBuf, (uint32_t)g_bbcThumbLen, 4, ty, thumbW, thumbH);
        textX    = 4 + thumbW + 4;
        textMaxW = SCREEN_W - textX - 6;
    }
    xSemaphoreGive(g_dataMutex);

    spr.setFont(UI_FONT_12);
    spr.setTextColor(C(COL_WHITE));
    printWrapped(textX, y + 6, nd.items[0].title, textMaxW, 16, y + topH - 4);
    y += topH + 3;

    // ── Stories 2–4 ───────────────────────────────────────────────────────────
    // rowH=40 fits two lines of DejaVu12 (5px top pad + 16 + 16 = 37, 3px margin).
    for (int i = 1; i < nd.count && i < 4; i++) {
        const int rowH = 40;
        drawPanel(2, y, SCREEN_W - 4, rowH, 0x111111, COL_BORDER);

        // Numbered badge
        spr.fillCircle(13, y + rowH / 2, 9, C(COL_BBC_RED));
        spr.setFont(UI_FONT_9);
        spr.setTextColor(C(COL_WHITE));
        char n[3]; snprintf(n, sizeof(n), "%d", i + 1);
        int nw = spr.textWidth(n);
        spr.setCursor(13 - nw / 2, y + rowH / 2 - 5);
        spr.print(n);

        spr.setFont(UI_FONT_12);
        spr.setTextColor(C(COL_DIM_WHITE));
        printWrapped(27, y + 5, nd.items[i].title, SCREEN_W - 38, 16, y + rowH - 3);

        y += rowH + 2;
    }
}
