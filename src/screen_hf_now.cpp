#include "screen_hf_now.h"
#include "ui_common.h"
#include "config.h"
#include "data_store.h"
#include <time.h>
#include <math.h>
#include "fonts/ui_fonts.h"

// ─── Bar grow-in animation state ──────────────────────────────────────────────
static uint32_t s_entryTime  = 0;
static uint32_t s_lastCallMs = 0;

// Cubic ease-out: fast start, decelerates to a stop
static inline float easeOutCubic(float t) {
    if (t >= 1.0f) return 1.0f;
    if (t <= 0.0f) return 0.0f;
    float u = 1.0f - t;
    return 1.0f - u * u * u;
}

void drawScreenHFNow() {
    // Detect screen entry: if more than 500 ms have passed since the last draw
    // call, the screen was just navigated to — reset the animation clock.
    uint32_t now = millis();
    if (now - s_lastCallMs > 500) s_entryTime = now;
    s_lastCallMs = now;

    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    SolarData sd = g_solar;
    xSemaphoreGive(g_dataMutex);

    if (!sd.valid) { drawLoader("Fetching solar data..."); return; }

    int y = CONTENT_Y + 3;

    // ── Solar index tiles ─────────────────────────────────────────────────────
    struct IndexTile {
        const char* label;
        int         value;
        uint32_t    col;
    } tiles[5] = {
        { "SFI",  sd.sfi,      sd.sfi > 150     ? COL_GREEN : sd.sfi > 100     ? COL_AMBER : COL_RED },
        { "SSN",  sd.sunspots, sd.sunspots > 100 ? COL_GREEN : sd.sunspots > 50 ? COL_AMBER : COL_RED },
        { "A",    sd.aIndex,   sd.aIndex < 8     ? COL_GREEN : sd.aIndex < 20   ? COL_AMBER : COL_RED },
        { "K",    sd.kIndex,   sd.kIndex < 2     ? COL_GREEN : sd.kIndex < 4    ? COL_AMBER : COL_RED },
        { "XRAY", 0,           COL_GREY },
    };
    char xr = sd.xray[0];
    tiles[4].col = (xr == 'A') ? COL_GREEN : (xr == 'B') ? COL_AMBER : COL_RED;

    const int tileW = 58, tileH = 40;
    int tileGap = (SCREEN_W - 5 * tileW - 6) / 4;

    for (int i = 0; i < 5; i++) {
        int tx = 3 + i * (tileW + tileGap);
        drawPanel(tx, y, tileW, tileH, COL_PANEL, COL_BORDER);

        spr.setFont(UI_FONT_9);
        spr.setTextColor(C(COL_GREY));
        spr.setCursor(tx + 4, y + 3);
        spr.print(tiles[i].label);

        spr.setFont(UI_FONT_18);
        spr.setTextColor(C(tiles[i].col));
        char vbuf[8];
        if (i == 4)
            strlcpy(vbuf, sd.xray, sizeof(vbuf));
        else
            snprintf(vbuf, sizeof(vbuf), "%d", tiles[i].value);
        int vw = spr.textWidth(vbuf);
        spr.setCursor(tx + (tileW - vw) / 2, y + 18);
        spr.print(vbuf);
    }

    y += tileH + 5;
    spr.drawFastHLine(4, y, SCREEN_W - 8, C(COL_BORDER));
    y += 4;

    // ── Day / Night header ────────────────────────────────────────────────────
    struct tm ti;
    getLocalTime(&ti);
    bool isDay = (ti.tm_hour >= 6 && ti.tm_hour < 20);

    spr.setFont(UI_FONT_9);
    spr.setTextColor(C(COL_GREY));
    spr.setCursor(4, y);
    spr.printf("BAND CONDITIONS  [%s]", isDay ? "DAY" : "NIGHT");
    y += 13;

    // ── Band bars ─────────────────────────────────────────────────────────────
    // Stretch 7 rows to fill the remaining screen height.
    // Available: SCREEN_H - y - 4 (bottom margin) spread across bandCount rows.
    const int available = SCREEN_H - y - 4;
    const int rowH  = (sd.bandCount > 0) ? available / sd.bandCount : 22;
    const int barH  = rowH - 4;

    const int labelW = 36;
    const int condW  = 68;
    const int barX   = labelW + 4;   // 40
    const int barW   = SCREEN_W - barX - condW - 6;   // ~206

    // Animation: each bar grows in with a 50 ms stagger between rows.
    // Each bar's own grow takes 400 ms (cubic ease-out).
    const uint32_t BAR_DURATION_MS = 400;
    const uint32_t BAR_STAGGER_MS  = 50;
    uint32_t elapsed = now - s_entryTime;

    for (int i = 0; i < sd.bandCount; i++) {
        BandCond& b = sd.bands[i];
        const char* cond = isDay ? b.day : b.night;

        // Compute 0→1 progress for this bar's grow-in
        uint32_t barStart = (uint32_t)i * BAR_STAGGER_MS;
        float t = (elapsed > barStart)
                      ? (float)(elapsed - barStart) / (float)BAR_DURATION_MS
                      : 0.0f;
        float progress = easeOutCubic(t);

        spr.setFont(UI_FONT_12);
        spr.setTextColor(C(COL_ORANGE));
        spr.setCursor(4, y + (barH - 14) / 2);
        spr.print(b.band);

        drawBandBar(barX, y, barW, barH, cond, progress);

        spr.setFont(UI_FONT_12);
        spr.setTextColor(C(condColor(cond)));
        spr.setCursor(barX + barW + 4, y + (barH - 14) / 2);
        spr.print(cond);

        y += rowH;
    }
}
