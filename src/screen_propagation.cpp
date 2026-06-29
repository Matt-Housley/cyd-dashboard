#include "screen_propagation.h"
#include "ui_common.h"
#include "config.h"
#include "data_store.h"
#include "fonts/ui_fonts.h"

void drawScreenPropagation() {
    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    SolarData sd = g_solar;
    xSemaphoreGive(g_dataMutex);

    if (!sd.valid) { drawLoader("Fetching Propogation Data..."); return; }

    int y = CONTENT_Y + 2;   // 18

    // ── Section header ────────────────────────────────────────────────────────
    spr.setFont(UI_FONT_9);
    spr.setTextColor(C(COL_ORANGE));
    spr.setCursor(4, y);
    spr.print("PROPAGATION DETAIL");
    y += 12;

    // ── Geomag + noise — single line ──────────────────────────────────────────
    spr.setFont(UI_FONT_9);
    spr.setTextColor(C(COL_GREY));
    spr.setCursor(4, y);
    spr.print("Geomag:");
    spr.setTextColor(C(condColor(sd.geoField)));
    spr.setCursor(54, y);
    spr.print(sd.geoField);

    spr.setTextColor(C(COL_GREY));
    spr.setCursor(168, y);
    spr.print("S/N:");
    spr.setTextColor(C(condColor(sd.signalNoise)));
    spr.setCursor(198, y);
    spr.print(sd.signalNoise);
    y += 12;

    spr.drawFastHLine(4, y + 1, SCREEN_W - 8, C(COL_BORDER));
    y += 5;

    // ── Column headers ────────────────────────────────────────────────────────
    // Three equal columns across 320px with 8px outer margins
    const int MARGIN = 8;
    const int COL_W  = (SCREEN_W - 2 * MARGIN) / 3;  // 101px each
    const int xBand  = MARGIN;
    const int xDay   = MARGIN + COL_W;
    const int xNight = MARGIN + COL_W * 2;
    const int rowH   = 24;

    spr.setFont(UI_FONT_12);
    spr.setTextColor(C(COL_GREY));
    {
        int dw = spr.textWidth("Day");
        int nw = spr.textWidth("Night");
        spr.setCursor(xDay   + (COL_W - dw) / 2, y);  spr.print("Day");
        spr.setCursor(xNight + (COL_W - nw) / 2, y);  spr.print("Night");
    }
    y += 20;

    // ── Band rows (DejaVu18, 7 rows × 24 px = 168 px) ────────────────────────
    for (int i = 0; i < sd.bandCount; i++) {
        BandCond& b = sd.bands[i];
        int ry = y + i * rowH;

        spr.setFont(UI_FONT_18);
        spr.setTextColor(C(COL_ORANGE));
        spr.setCursor(xBand, ry);
        spr.print(b.band);

        spr.setFont(UI_FONT_18);
        spr.setTextColor(C(condColor(b.day)));
        int dw = spr.textWidth(b.day);
        spr.setCursor(xDay + (COL_W - dw) / 2, ry);
        spr.print(b.day);

        spr.setTextColor(C(condColor(b.night)));
        int nw = spr.textWidth(b.night);
        spr.setCursor(xNight + (COL_W - nw) / 2, ry);
        spr.print(b.night);
    }

    // ── Footer summary ────────────────────────────────────────────────────────
    int fy = SCREEN_H - 13;
    spr.setFont(UI_FONT_9);
    spr.setTextColor(C(COL_GREY));
    spr.setCursor(4, fy);
    spr.printf("SFI:%d  SSN:%d  A:%d  K:%d  X:%s",
               sd.sfi, sd.sunspots, sd.aIndex, sd.kIndex, sd.xray);
}
