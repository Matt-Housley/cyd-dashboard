#include "screen_hf_now.h"
#include "ui_common.h"
#include "config.h"
#include "data_store.h"
#include <time.h>
#include <math.h>
#include "fonts/ui_fonts.h"

// ─── Tap-to-info overlay state ────────────────────────────────────────────────
static int8_t   s_hfSelected = -1;   // index into tiles[], -1 = none

bool hfNowHasSelection() { return s_hfSelected >= 0; }

bool hfNowTouchUp(int32_t x, int32_t y) {
    const int tileW = 58, tileH = 40;
    // tileGap must match drawScreenHFNow — computed the same way
    int tileGap = (SCREEN_W - 5 * tileW - 6) / 4;
    int tileTop = CONTENT_Y + 3;
    int tileBot = tileTop + tileH;

    if (s_hfSelected >= 0) {
        // Any tap dismisses the overlay
        s_hfSelected = -1;
        return true;
    }
    if (y >= tileTop && y < tileBot) {
        for (int i = 0; i < 5; i++) {
            int tx = 3 + i * (tileW + tileGap);
            if (x >= tx && x < tx + tileW) {
                s_hfSelected = (int8_t)i;
                return true;
            }
        }
    }
    return false;
}

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

    // ── Tap-to-info overlay ───────────────────────────────────────────────────
    if (s_hfSelected < 0) return;

    struct InfoEntry {
        const char* title;      // full name
        const char* rangeStr;   // "0 – 300+"
        const char* idealStr;   // "150 or above"
        const char* line1;      // explanation line 1
        const char* line2;      // explanation line 2
        const char* line3;      // explanation line 3
        const char* line4;      // effect on signals
    };

    static const InfoEntry INFO[5] = {
        {   // SFI
            "Solar Flux Index",
            "Range: 66 (solar min) to 300+",
            "Ideal: 150 or above",
            "Measures radio emissions from the Sun",
            "at 10.7cm wavelength. Reflects the",
            "ionisation level of the F2 layer.",
            "Higher = better HF, especially 17-10m."
        },
        {   // SSN
            "Sunspot Number",
            "Range: 0 (solar min) to 300+",
            "Ideal: 100 or above",
            "Count of sunspots visible on the solar",
            "disc. Closely tracks SFI. High SSN means",
            "strong F2 ionisation and long skip.",
            "Higher = better conditions on upper bands."
        },
        {   // A-Index
            "A-Index  (Geomagnetic - daily)",
            "0-7 Quiet  8-15 Unsettled  16+ Active",
            "Ideal: below 8  (storm: 30+)",
            "Daily average of geomagnetic activity.",
            "Geomagnetic storms absorb HF signals,",
            "especially at high latitudes.",
            "Lower = more stable, less absorption."
        },
        {   // K-Index
            "K-Index  (Geomagnetic - 3 hour)",
            "0-1 Quiet  2-3 Unsettled  4 Active  5+ Storm",
            "Ideal: 0 or 1",
            "3-hourly snapshot of geomagnetic activity.",
            "More current than A-Index. K 5+ causes",
            "polar cap absorption and HF blackouts.",
            "Lower = better, especially at high latitudes."
        },
        {   // X-Ray
            "Solar X-Ray Flux",
            "Classes: A  B  C  M  X  (increasing)",
            "Ideal: A or B class",
            "Measures solar X-ray output. M and X class",
            "flares cause sudden ionospheric disturbances",
            "(SID), blacking out HF on the sunlit side.",
            "A/B = benign.  M/X = possible HF blackout."
        },
    };

    const InfoEntry& inf = INFO[s_hfSelected];

    // Measure content to size the box
    spr.setFont(UI_FONT_12);
    int titleW = spr.textWidth(inf.title);
    spr.setFont(UI_FONT_9);
    int w = titleW;
    auto mw = [&](const char* s){ int tw = spr.textWidth(s); if (tw > w) w = tw; };
    mw(inf.rangeStr); mw(inf.idealStr);
    mw(inf.line1); mw(inf.line2); mw(inf.line3); mw(inf.line4);

    const int IPX = 6, IPY = 5;   // inner padding
    const int BW  = w + 2 * IPX;

    // Calculate BH by simulating the ty progression, then adding bottom padding.
    // This guarantees the hint line never overlaps content regardless of font sizes.
    const int BH = IPY           // top padding
                 + 14 + 7        // title (UI_FONT_12) + gap (7 = divider at +2, then 5px to text = matches bottom IPY)
                 + 9  + 2        // rangeStr + gap
                 + 9  + 3        // idealStr + section gap
                 + 9  + 2        // line1 + gap
                 + 9  + 2        // line2 + gap
                 + 9  + 4        // line3 + gap before effect
                 + 9  + 5        // line4 (effect) + gap before hint
                 + 9             // hint line
                 + IPY;          // bottom padding

    int BX = (SCREEN_W - BW) / 2;
    int BY = CONTENT_Y + 3 + 40 + 8;   // just below the tile row
    if (BY + BH > SCREEN_H - 2) BY = SCREEN_H - BH - 2;

    // Semi-transparent backdrop
    spr.fillRect(0, CONTENT_Y, SCREEN_W, SCREEN_H - CONTENT_Y, spr.color888(0,0,0) & 0xA000);
    spr.fillRect(BX, BY, BW, BH, spr.color888(12, 14, 28));
    spr.drawRect(BX, BY, BW, BH, spr.color888(80, 100, 160));
    spr.drawFastHLine(BX + 1, BY + IPY + 14 + 2, BW - 2, spr.color888(60, 80, 130));

    int tx = BX + IPX;
    int ty = BY + IPY;

    // Title
    spr.setFont(UI_FONT_12);
    spr.setTextColor(spr.color888(0, 210, 255));
    spr.setCursor(tx, ty); spr.print(inf.title);
    ty += 14 + 7;

    spr.setFont(UI_FONT_9);

    // Range / Ideal
    spr.setTextColor(spr.color888(180, 200, 180));
    spr.setCursor(tx, ty); spr.print(inf.rangeStr); ty += 11;
    spr.setTextColor(spr.color888(120, 220, 120));
    spr.setCursor(tx, ty); spr.print(inf.idealStr); ty += 12;

    // Explanation lines
    spr.setTextColor(spr.color888(190, 190, 200));
    spr.setCursor(tx, ty); spr.print(inf.line1); ty += 11;
    spr.setCursor(tx, ty); spr.print(inf.line2); ty += 11;
    spr.setCursor(tx, ty); spr.print(inf.line3); ty += 13;

    // Effect on signals (highlighted amber)
    spr.setTextColor(spr.color888(255, 180, 80));
    spr.setCursor(tx, ty); spr.print(inf.line4);
    ty += 14;   // 9px text + 5px gap before hint

    // Dismiss hint — centred, drawn after all content
    const char* hint = "Tap to dismiss";
    spr.setTextColor(spr.color888(80, 80, 100));
    spr.setCursor(BX + (BW - spr.textWidth(hint)) / 2, ty);
    spr.print(hint);
}
