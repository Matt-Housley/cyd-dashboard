#include "screen_tracker.h"
#include "ui_common.h"
#include "config.h"
#include "data_store.h"
#include "settings.h"
#include <time.h>
#include <math.h>
#include "fonts/ui_fonts.h"

// ─── Graph grow-in animation state ───────────────────────────────────────────
static uint32_t s_entryTime  = 0;
static uint32_t s_lastCallMs = 0;

static inline float easeOutCubic(float t) {
    if (t >= 1.0f) return 1.0f;
    if (t <= 0.0f) return 0.0f;
    float u = 1.0f - t;
    return 1.0f - u * u * u;
}

// ─── Dynamic year-tick computation ───────────────────────────────────────────
// Computes tick positions based on the ACTUAL data span (histCount × 7 days)
// rather than the requested range years.  Yahoo Finance sometimes returns fewer
// bars than requested (rate limits, symbol history length, etc.), so using
// histCount * 7 days as the data-start estimate keeps the ticks consistent with
// whatever data we actually have.
static int computeYearTicks(int totalPoints,
                             int    ticks[],
                             char   labels[][6],
                             int    maxTicks) {
    if (totalPoints < 2) return 0;

    time_t nowT      = time(nullptr);
    // Estimate the actual start of the data: each bar represents ~7 days and
    // the most-recent bar is the current week.
    time_t dataStartT = nowT - (time_t)(totalPoints - 1) * 7 * 86400;

    struct tm startTm;
    gmtime_r(&dataStartT, &startTm);
    int startYear = startTm.tm_year + 1900;

    // Iterate far enough to span the full data; the wk<totalPoints guard stops
    // us from emitting ticks beyond the data.
    int count = 0;
    for (int yr = startYear + 1;
         yr <= startYear + g_settings.trackerRangeYears + 2 && count < maxTicks;
         yr++) {
        struct tm janTm = {};
        janTm.tm_year = yr - 1900;
        janTm.tm_mon  = 0;
        janTm.tm_mday = 1;
        time_t janT   = mktime(&janTm);
        int wk = (int)((janT - dataStartT) / (7 * 86400));
        if (wk >= 0 && wk < totalPoints) {
            ticks[count] = wk;
            snprintf(labels[count], 6, "'%02d", yr % 100);
            count++;
        }
    }
    return count;
}

void drawScreenTracker() {
    // Detect screen entry: a gap > 500 ms since last draw means we just arrived
    uint32_t now = millis();
    if (now - s_lastCallMs > 500) s_entryTime = now;
    s_lastCallMs = now;

    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    TrackerData sd = g_tracker;
    xSemaphoreGive(g_dataMutex);

    int y = CONTENT_Y + 4;

    // ── Header row ────────────────────────────────────────────────────────────
    spr.setFont(UI_FONT_9);
    spr.setTextColor(C(COL_GREY));
    spr.setCursor(4, y);
    spr.print(g_settings.trackerName);

    // Period label — "Since <Mon> '<YY>"
    // Use the actual data span (histCount weeks) rather than the requested range
    // so this label stays consistent with the year ticks in computeYearTicks().
    time_t nowT   = time(nullptr);
    int    hc     = sd.valid ? sd.histCount : 0;
    time_t startT = nowT - (time_t)(hc > 1 ? hc - 1 : 0) * 7 * 86400;
    struct tm startTm;
    gmtime_r(&startT, &startTm);
    static const char* MON[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                 "Jul","Aug","Sep","Oct","Nov","Dec"};
    char periodLabel[24];
    snprintf(periodLabel, sizeof(periodLabel), "Since %s '%02d",
             MON[startTm.tm_mon], (startTm.tm_year + 1900) % 100);

    int plw = spr.textWidth(periodLabel);
    spr.setCursor(SCREEN_W - plw - 4, y);
    spr.print(periodLabel);
    y += 14;

    if (!sd.valid) { drawLoader("Fetching Market Data..."); return; }

    bool     up   = (sd.change >= 0.0f);
    uint32_t chgC = up ? COL_GREEN : COL_RED;

    // ── Price ─────────────────────────────────────────────────────────────────
    char priceBuf[16];
    fmtPrice(priceBuf, sizeof(priceBuf), sd.price);

    spr.setFont(UI_FONT_24);
    spr.setTextColor(C(COL_WHITE));
    spr.setCursor(4, y);
    spr.print(priceBuf);

    char chgBuf[24];
    snprintf(chgBuf, sizeof(chgBuf), "%+.1f%%", sd.changePct);
    spr.setFont(UI_FONT_12);
    spr.setTextColor(C(chgC));
    int cw = spr.textWidth(chgBuf);
    spr.setCursor(SCREEN_W - cw - 4, y + 8);
    spr.print(chgBuf);
    y += 30;

    // ── Chart box ─────────────────────────────────────────────────────────────
    const int chartH = 148;
    const int labelW = 42;
    const int xAxisH = 14;

    drawPanel(2, y, SCREEN_W - 4, chartH, COL_PANEL, COL_BORDER);

    if (sd.histCount >= 2) {
        int spX = 2 + labelW;
        int spY = y + 3;
        int spW = SCREEN_W - 4 - labelW - 4;
        int spH = chartH - xAxisH - 6;

        // ── Animated visible count (cubic ease-out, 800 ms) ───────────────────
        const uint32_t ANIM_MS = 800;
        float progress    = easeOutCubic((float)(now - s_entryTime) / (float)ANIM_MS);
        int visibleCount  = (int)(sd.histCount * progress);
        if (visibleCount < 2)            visibleCount = 2;
        if (visibleCount > sd.histCount) visibleCount = sd.histCount;

        drawSparkline(spX, spY, spW, spH, sd.history, sd.histCount, chgC, visibleCount);

        float mn = sd.history[0], mx = sd.history[0];
        for (int i = 1; i < sd.histCount; i++) {
            mn = min(mn, sd.history[i]);
            mx = max(mx, sd.history[i]);
        }
        float mid = (mn + mx) * 0.5f;

        // Y-axis labels — always visible (scale is fixed throughout animation)
        spr.setFont(UI_FONT_9);
        spr.setTextColor(C(COL_GREY));
        char mb[12];

        fmtPrice(mb, sizeof(mb), mx);
        spr.setCursor(2 + labelW - spr.textWidth(mb) - 3, spY + 1);
        spr.print(mb);

        fmtPrice(mb, sizeof(mb), mid);
        spr.setCursor(2 + labelW - spr.textWidth(mb) - 3, spY + spH / 2 - 4);
        spr.print(mb);

        fmtPrice(mb, sizeof(mb), mn);
        spr.setCursor(2 + labelW - spr.textWidth(mb) - 3, spY + spH - 10);
        spr.print(mb);

        // X-axis year labels — only show ticks that are already revealed
        int    ticks[6];
        char   labels[6][6];
        int    nTicks  = computeYearTicks(sd.histCount, ticks, labels, 6);
        int    xLabelY = y + chartH - xAxisH + 1;

        for (int i = 0; i < nTicks; i++) {
            if (ticks[i] >= visibleCount) continue;   // not yet reached by animation
            int px = spX + (ticks[i] * (spW - 1)) / (sd.histCount - 1);
            spr.drawFastVLine(px, spY + spH, 3, C(COL_GREY));
            int lw2 = spr.textWidth(labels[i]);
            int lx  = px - lw2 / 2;
            lx = constrain(lx, 2 + labelW, SCREEN_W - 4 - lw2);
            spr.setCursor(lx, xLabelY);
            spr.print(labels[i]);
        }
    }
    y += chartH + 4;

    // ── Info tiles ────────────────────────────────────────────────────────────
    char absChg[12];
    snprintf(absChg, sizeof(absChg), "%+.0f", sd.change);

    const char* tileLabels[] = { "EXCHANGE", "CURRENCY", "CHANGE", "POINTS" };
    char        tileValues[4][12];
    strlcpy(tileValues[0], "NYSE",   sizeof(tileValues[0]));
    strlcpy(tileValues[1], "USD",    sizeof(tileValues[1]));
    strlcpy(tileValues[2], absChg,   sizeof(tileValues[2]));
    snprintf(tileValues[3], sizeof(tileValues[3]), "%d", sd.histCount);

    int tileW = (SCREEN_W - 6) / 4;
    for (int i = 0; i < 4; i++) {
        int tx = 2 + i * (tileW + 1);
        drawPanel(tx, y, tileW, 24, COL_PANEL, COL_BORDER);
        spr.setFont(UI_FONT_9);
        spr.setTextColor(C(COL_GREY));
        spr.setCursor(tx + 2, y + 2);
        spr.print(tileLabels[i]);
        spr.setTextColor(i == 2 ? C(chgC) : C(COL_WHITE));
        spr.setCursor(tx + 2, y + 13);
        spr.print(tileValues[i]);
    }
}
