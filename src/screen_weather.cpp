#include "screen_weather.h"
#include "ui_common.h"
#include "config.h"
#include "data_store.h"
#include <math.h>
#include "fonts/ui_fonts.h"

// ─── Wind direction helpers ───────────────────────────────────────────────────

// Draws a full-length directional arrow centred at (cx,cy).
// The shaft runs from TAIL to TIP (total length = 2 × r px), which fills the
// wind-row height and is twice as visible as a centre→tip half-arrow.
// r=5 keeps the tip safely inside the tile border for all eight cardinal
// directions (worst case: due-south tip at cy+5, tile border at cy+7).
static void drawWindArrow(int cx, int cy, uint16_t windDeg, uint32_t col) {
    const float r  = 5.0f;   // half-length; total shaft = 10 px ≈ DejaVu9 height
    const float br = 3.0f;   // arrowhead barb length
    float rad = windDeg * (float)M_PI / 180.0f;
    float sr  = sinf(rad), cr = cosf(rad);

    // Full shaft: tail → tip  (previously only centre → tip was drawn)
    int tipX  = cx + (int)(r * sr);
    int tipY  = cy - (int)(r * cr);
    int tailX = cx - (int)(r * sr);
    int tailY = cy + (int)(r * cr);
    spr.drawLine(tailX, tailY, tipX, tipY, col);

    // Arrowhead barbs at the tip
    float back = rad + (float)M_PI;
    spr.drawLine(tipX, tipY,
                 tipX + (int)(br * sinf(back + 0.7f)),
                 tipY - (int)(br * cosf(back + 0.7f)), col);
    spr.drawLine(tipX, tipY,
                 tipX + (int)(br * sinf(back - 0.7f)),
                 tipY - (int)(br * cosf(back - 0.7f)), col);
}

// ─── WMO code helpers ─────────────────────────────────────────────────────────
static const char* wmoLabel(uint8_t code) {
    if (code == 0)  return "Clear sky";
    if (code <= 2)  return "Partly cloudy";
    if (code == 3)  return "Overcast";
    if (code <= 48) return "Foggy";
    if (code <= 55) return "Drizzle";
    if (code <= 65) return "Rain";
    if (code <= 75) return "Snow";
    if (code <= 82) return "Showers";
    if (code <= 86) return "Snow showers";
    return "Thunderstorm";
}

// ─── Weather icon drawing ─────────────────────────────────────────────────────
static void drawCloudShape(int cx, int cy, int r, uint32_t col) {
    int r2 = r * 2 / 3;
    spr.fillCircle(cx,        cy,       r2,  col);
    spr.fillCircle(cx - r/2,  cy + r/4, r/2, col);
    spr.fillCircle(cx + r/2,  cy + r/4, r/2, col);
    // Fill the gap between the three circles.  Width and height are kept
    // inside the circle edges at every y level so no rectangle corners poke out.
    spr.fillRect(cx - r*2/3, cy + r/4, r*4/3, r/3, col);
}

// t = millis(), used to drive frame-by-frame animations.
static void drawWeatherIcon(int cx, int cy, int r, uint8_t code, uint32_t t) {
    uint32_t sunC   = spr.color888(255, 200, 0);
    uint32_t cloudC = spr.color888(160, 160, 185);
    uint32_t rainC  = spr.color888(80,  140, 255);
    uint32_t snowC  = spr.color888(200, 230, 255);
    uint32_t boltC  = spr.color888(255, 230, 0);

    if (code == 0) {
        // ── Rotating sun ──────────────────────────────────────────────────────
        spr.fillCircle(cx, cy, r * 3 / 5, sunC);
        float angle = (float)(t % 8000) / 8000.0f * 2.0f * (float)M_PI;
        int ri = r * 3 / 5 + 2, ro = r;
        for (int ray = 0; ray < 8; ray++) {
            float a  = angle + ray * (float)M_PI / 4.0f;
            float ca = cosf(a), sa = sinf(a);
            spr.drawLine(cx + (int)(ri * ca), cy - (int)(ri * sa),
                         cx + (int)(ro * ca), cy - (int)(ro * sa), sunC);
        }

    } else if (code <= 2) {
        // ── Partly cloudy: rotating sun + gently drifting cloud ───────────────
        int sx = cx - r / 3, sy = cy - r / 3;
        spr.fillCircle(sx, sy, r / 2, sunC);
        float angle = (float)(t % 8000) / 8000.0f * 2.0f * (float)M_PI;
        int ri = r / 2 - 1, ro = r / 2 + r / 6;
        for (int ray = 0; ray < 8; ray++) {
            float a  = angle + ray * (float)M_PI / 4.0f;
            float ca = cosf(a), sa = sinf(a);
            spr.drawLine(sx + (int)(ri * ca), sy - (int)(ri * sa),
                         sx + (int)(ro * ca), sy - (int)(ro * sa), sunC);
        }
        int dx = (int)(sinf((float)t / 1200.0f) * (r / 8));
        drawCloudShape(cx + r / 4 + dx, cy + r / 6, r * 3 / 4, cloudC);

    } else if (code <= 3) {
        // ── Overcast: cloud drifts gently left/right ──────────────────────────
        int dx = (int)(sinf((float)t / 1500.0f) * (r / 6));
        drawCloudShape(cx + dx, cy, r, cloudC);

    } else if (code <= 48) {
        // ── Fog: lines breathe in and out ─────────────────────────────────────
        uint32_t fogC = spr.color888(140, 140, 160);
        for (int i = -1; i <= 1; i++) {
            float wave = sinf((float)t / 900.0f + i * 1.1f);
            int len = r + (int)(wave * r / 5);
            if (len < r / 2) len = r / 2;
            spr.drawFastHLine(cx - len, cy + i * (r / 3), len * 2, fogC);
        }

    } else if (code <= 55) {
        // ── Drizzle: three thin drops fall continuously ────────────────────────
        drawCloudShape(cx, cy - r / 4, r * 3 / 4, cloudC);
        int dropH = max(2, r / 3);
        int range = r / 2 + r / 4;
        for (int i = 0; i < 3; i++) {
            int dropX = cx - r / 2 + i * (r / 2);
            int dy    = (int)((t / 100 + (uint32_t)(i * range / 3)) % (uint32_t)range);
            spr.fillRect(dropX, cy + r / 4 + dy, 1, dropH, rainC);
        }

    } else if (code <= 65) {
        // ── Rain: three angled drops fall faster ──────────────────────────────
        drawCloudShape(cx, cy - r / 4, r * 3 / 4, cloudC);
        int dropH  = max(2, r / 3);
        int slant  = r / 6;
        int range  = r / 2 + r / 4;
        for (int i = 0; i < 3; i++) {
            int dropX = cx - r / 2 + i * (r / 2);
            int dy    = (int)((t / 60 + (uint32_t)(i * range / 3)) % (uint32_t)range);
            int dropY = cy + r / 4 + dy;
            spr.drawLine(dropX, dropY, dropX - slant, dropY + dropH, rainC);
        }

    } else if (code <= 75) {
        // ── Snow: three crosses drift slowly downward ──────────────────────────
        drawCloudShape(cx, cy - r / 4, r * 3 / 4, cloudC);
        int sf    = max(2, r / 10 + 1);   // snowflake arm half-length
        int range = r / 2;
        for (int i = 0; i < 3; i++) {
            int sx = cx - r / 2 + i * (r / 2);
            int dy = (int)((t / 200 + (uint32_t)(i * range / 3)) % (uint32_t)range);
            int sy = cy + r / 2 + dy;
            spr.drawFastHLine(sx - sf, sy,      sf * 2 + 1, snowC);
            spr.drawFastVLine(sx,      sy - sf, sf * 2 + 1, snowC);
        }

    } else if (code <= 82) {
        // ── Showers: four heavier angled drops fall quickly ───────────────────
        drawCloudShape(cx, cy - r / 4, r * 3 / 4, cloudC);
        int dropH = max(2, r / 3);
        int slant = r / 6;
        int range = r / 2 + r / 4;
        for (int i = 0; i < 4; i++) {
            int dropX = cx - r * 3 / 4 + i * (r / 2);
            int dy    = (int)((t / 50 + (uint32_t)(i * range / 4)) % (uint32_t)range);
            int dropY = cy + r / 4 + dy;
            spr.drawLine(dropX, dropY, dropX - slant, dropY + dropH, rainC);
        }

    } else {
        // ── Thunderstorm: lightning bolt double-flashes every ~2 s ───────────
        drawCloudShape(cx, cy - r / 4, r * 3 / 4, cloudC);
        uint32_t phase = t % 2000;
        bool flash = (phase < 120) || (phase > 240 && phase < 360);
        if (flash) {
            int lx = cx + r / 4, ly = cy + r / 3;
            spr.drawLine(lx,       ly,           lx - r/3, ly + r/4,   boltC);
            spr.drawLine(lx - r/3, ly + r/4,     lx + r/8, ly + r/4,   boltC);
            spr.drawLine(lx + r/8, ly + r/4,     lx - r/4, ly + r*2/3, boltC);
        }
    }
}

// ─── Screen ───────────────────────────────────────────────────────────────────
void drawScreenWeather() {
    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    WeatherData wd = g_weather;
    xSemaphoreGive(g_dataMutex);

    if (!wd.valid) { drawLoader("Fetching Weather..."); return; }

    uint32_t t = millis();   // shared animation clock for all icons this frame
    int y = CONTENT_Y + 2;

    // ── Large temperature ─────────────────────────────────────────────────────
    // Centre the temperature block in the band between the status bar and the
    // condition label (at y+48).  Band = 50 px; DejaVu40 digit cap ≈ 36 px
    // → 7 px top margin keeps it visually centred.
    const int tempTop = STATUS_H + ((y + 48 - STATUS_H) - 36) / 2;

    char tbuf[8];
    snprintf(tbuf, sizeof(tbuf), "%d", (int)wd.temp);

    spr.setFont(UI_FONT_40);
    spr.setTextColor(C(COL_WHITE));
    int tempW = spr.textWidth(tbuf);
    spr.setCursor(6, tempTop);
    spr.print(tbuf);

    // Degree circle + "C"
    int degX = 6 + tempW + 6;
    int degY = tempTop + 10;
    spr.drawCircle(degX, degY, 3, C(COL_GREY_L));
    spr.setFont(UI_FONT_18);
    spr.setTextColor(C(COL_GREY_L));
    spr.setCursor(degX + 7, tempTop + 8);
    spr.print("C");

    // ── Current condition label ───────────────────────────────────────────────
    spr.setFont(UI_FONT_12);
    spr.setTextColor(C(COL_GREY));
    spr.setCursor(6, y + 48);
    spr.print(wmoLabel(wd.code));

    // ── Stats (right column) ──────────────────────────────────────────────────
    spr.setFont(UI_FONT_9);
    int rx = 162;
    spr.setTextColor(C(COL_GREY_L));
    spr.setCursor(rx, y + 6);
    spr.printf("Hum: %d%%", wd.humidity);
    spr.setCursor(rx, y + 20);
    spr.printf("Wind: %d mph", wd.windMph);
    spr.setTextColor(C(COL_AMBER));
    spr.setCursor(rx, y + 34);
    spr.printf("Rain: %s", wd.rainIn);

    // Lightning row — shares the y+48 band with the condition label (x=6, left side)
    if (wd.lightningHours < 0) {
        spr.setTextColor(C(COL_GREY));
        spr.setCursor(rx, y + 48);
        spr.print("Ltng: None");
    } else {
        spr.setTextColor(spr.color888(255, 210, 0));   // bright yellow
        char lbuf[20];
        if (wd.lightningHours == 0)
            snprintf(lbuf, sizeof(lbuf), "Ltng: Now %d%%", (int)wd.lightningPct);
        else
            snprintf(lbuf, sizeof(lbuf), "Ltng: ~%dh %d%%",
                     (int)wd.lightningHours, (int)wd.lightningPct);
        spr.setCursor(rx, y + 48);
        spr.print(lbuf);
    }

    // ── Current conditions icon ───────────────────────────────────────────────
    // Centre vertically between the status bar and the separator line (y+66).
    // Band = (y+66) - STATUS_H = 68 px → centre at STATUS_H + 34 = 50 = y+32.
    const int iconCY = STATUS_H + ((y + 66) - STATUS_H) / 2;
    drawWeatherIcon(284, iconCY, 20, wd.code, t);

    y += 66;
    spr.drawFastHLine(4, y, SCREEN_W - 8, C(COL_BORDER));
    y += 4;

    // ── 7-day label ───────────────────────────────────────────────────────────
    spr.setFont(UI_FONT_9);
    spr.setTextColor(C(COL_GREY));
    spr.setCursor(4, y);
    spr.print("7-DAY FORECAST");
    y += 13;

    // ── Day tiles ─────────────────────────────────────────────────────────────
    if (wd.dayCount == 0) return;

    int colW  = (SCREEN_W - 6) / wd.dayCount;   // ~44 px for 7 days
    int tileH = SCREEN_H - y - 2;                // ~137 px

    // ── Compute weekly temperature range for the bar ──────────────────────────
    int8_t wMin = wd.daily[0].minT, wMax = wd.daily[0].maxT;
    for (int i = 1; i < wd.dayCount; i++) {
        if (wd.daily[i].minT < wMin) wMin = wd.daily[i].minT;
        if (wd.daily[i].maxT > wMax) wMax = wd.daily[i].maxT;
    }
    int wRange = (int)wMax - (int)wMin;
    if (wRange < 2) wRange = 2;   // avoid divide-by-zero / single-pixel bars

    // Bar geometry (tile-relative offsets).
    // Reduced by 8 px vs original to make room for the wind row at the bottom.
    const int barRelTop = 48;   // px from tile top
    const int barRelBot = 100;  // px from tile top  → 52 px tall (was 108/60)
    const int barTotalH = barRelBot - barRelTop;
    const int barW      = 6;

    for (int i = 0; i < wd.dayCount; i++) {
        int cx = 3 + i * colW;
        DayForecast& d = wd.daily[i];

        drawPanel(cx, y, colW - 2, tileH, COL_PANEL, COL_BORDER);

        // Day name
        spr.setFont(UI_FONT_9);
        spr.setTextColor(i == 0 ? C(COL_ORANGE) : C(COL_GREY));
        int tw = spr.textWidth(d.day);
        spr.setCursor(cx + (colW - 2 - tw) / 2, y + 2);
        spr.print(d.day);

        // Weather icon centred in tile (animated)
        drawWeatherIcon(cx + (colW - 2) / 2, y + 24, 10, d.code, t);

        // Max temp
        spr.setFont(UI_FONT_9);
        spr.setTextColor(C(COL_WHITE));
        char maxbuf[8];
        snprintf(maxbuf, sizeof(maxbuf), "%d", d.maxT);
        int mw   = spr.textWidth(maxbuf) + 5;   // +5 for degree circle
        int mxOff = cx + (colW - 2 - mw) / 2;
        spr.setCursor(mxOff, y + 37);
        spr.print(maxbuf);
        spr.drawCircle(mxOff + spr.textWidth(maxbuf) + 2, y + 38, 2, C(COL_WHITE));

        // ── Temperature bar ───────────────────────────────────────────────────
        int bx      = cx + (colW - 2 - barW) / 2;
        int absTop  = y + barRelTop;
        int absBot  = y + barRelBot;

        spr.fillRect(bx, absTop, barW, barTotalH, C(COL_BORDER));

        int fBot = absBot - ((int)(d.minT - wMin) * barTotalH / wRange);
        int fTop = absBot - ((int)(d.maxT - wMin) * barTotalH / wRange);
        fTop = max(fTop, absTop);
        fBot = min(fBot, absBot);
        int fH = fBot - fTop;
        if (fH < 2) fH = 2;

        uint32_t barCol = (d.maxT >= 20) ? COL_RED    :
                          (d.maxT >= 15) ? COL_ORANGE :
                          (d.maxT >= 10) ? COL_AMBER  : COL_BLUE;
        spr.fillRect(bx, fTop, barW, fH, C(barCol));

        // Min temp  (moved up 8 px to make room for wind row)
        spr.setFont(UI_FONT_9);
        spr.setTextColor(C(COL_GREY));
        char minbuf[8];
        snprintf(minbuf, sizeof(minbuf), "%d", d.minT);
        int mnw   = spr.textWidth(minbuf) + 5;
        int mnOff = cx + (colW - 2 - mnw) / 2;
        spr.setCursor(mnOff, y + 102);
        spr.print(minbuf);
        spr.drawCircle(mnOff + spr.textWidth(minbuf) + 2, y + 103, 2, C(COL_GREY));

        // Precipitation (optional, moved up 8 px)
        if (d.precip >= 0.1f) {
            spr.setTextColor(C(COL_BLUE));
            char pbuf[8];
            snprintf(pbuf, sizeof(pbuf),
                     d.precip < 10.0f ? "%.1f" : "%.0f", d.precip);
            strlcat(pbuf, "mm", sizeof(pbuf));
            int pw = spr.textWidth(pbuf);
            spr.setCursor(cx + (colW - 2 - pw) / 2, y + 114);
            spr.print(pbuf);
        }

        // Wind direction arrow + speed  ─────────────────────────────────────
        // Arrow + speed number centred as a group in the tile.
        // "widget" width ≈ 9 px (arrow) + 3 px gap + up to 18 px (speed) = 30 px.
        {
            uint32_t windCol  = C(COL_GREY);
            const int windTextY = y + 124;          // top of DejaVu9 text row
            const int windCY    = windTextY + 6;    // vertical centre for arrow

            char wsBuf[6];
            snprintf(wsBuf, sizeof(wsBuf), "%d", d.windMph);
            int speedW  = spr.textWidth(wsBuf);
            int groupW  = 17 + 3 + speedW;          // arrow(17) + gap(3) + speed
            int leftX   = cx + (colW - 2 - groupW) / 2;

            // Arrow centred in its 17 px slot
            drawWindArrow(leftX + 8, windCY, d.windDir, windCol);

            // Speed number immediately to the right
            spr.setFont(UI_FONT_9);
            spr.setTextColor(windCol);
            spr.setCursor(leftX + 20, windTextY);
            spr.print(wsBuf);
        }
    }
}
