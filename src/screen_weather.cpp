#include "screen_weather.h"
#include "ui_common.h"
#include "config.h"
#include "data_store.h"
#include "settings.h"
#include <math.h>
#include "service.h"
#include "fonts/ui_fonts.h"

// ─── Lightning alert state ────────────────────────────────────────────────────
static bool s_lightningAcked = false;                 // red LED silenced by tap
static int  s_alertX = -1, s_alertY, s_alertW, s_alertH;   // alert-box hit rect

/**
 * @brief Whether a thunderstorm is forecast within the next hour.
 *
 * True when g_weather.lightningHours is 0 or 1, or when a fake alert is staged
 * from the Service menu (g_fakeLightning). Re-arms the tap-to-silence
 * acknowledgement whenever the alert clears.
 * @return true if a lightning alert should be active this frame.
 */
bool weatherLightningSoon() {
    if (g_fakeLightning) return true;   // Service menu: staged lightning alert
    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    int8_t lh = g_weather.valid ? g_weather.lightningHours : -1;
    xSemaphoreGive(g_dataMutex);
    bool soon = (lh >= 0 && lh <= 1);
    if (!soon) s_lightningAcked = false;
    return soon;
}
/** @brief Whether the user has tapped the alert box to silence the red LED.
 *  @return true if the current lightning alert has been acknowledged. */
bool weatherLightningAcked() { return s_lightningAcked; }

/**
 * @brief Handle a tap on the Weather screen (finger-up).
 *
 * A tap inside the lightning-alert box acknowledges it, silencing the flashing
 * red LED (the warning box itself remains visible).
 * @param x  Touch x in screen pixels.
 * @param y  Touch y in screen pixels.
 * @return true if the alert box was tapped and acknowledged, false otherwise.
 */
bool weatherTouchUp(int32_t x, int32_t y) {
    if (s_alertX >= 0 &&
        x >= s_alertX && x < s_alertX + s_alertW &&
        y >= s_alertY && y < s_alertY + s_alertH) {
        s_lightningAcked = true;   // silence the red LED (box stays as a warning)
        return true;
    }
    return false;
}

// ─── Wind direction helpers ───────────────────────────────────────────────────

/**
 * @brief Draw a full-length wind-direction arrow centred at (cx,cy).
 *
 * The shaft runs tail→tip (total length 2×r px) pointing in the wind's
 * "flowing-to" direction, with barbs at the tip.
 * @param cx,cy    Arrow centre in screen pixels.
 * @param windDeg  Wind direction in degrees (0 = up/north, clockwise).
 * @param col      Arrow colour (already packed to the sprite's format).
 */
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
/**
 * @brief Map a WMO weather-interpretation code to a short label.
 * @param code  WMO code as supplied by Open-Meteo (0–99).
 * @return A static label such as "Clear sky", "Rain", or "Thunderstorm".
 */
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
/**
 * @brief Draw a filled cloud shape (three circles plus a connecting rectangle).
 * @param cx,cy  Cloud centre in screen pixels.
 * @param r      Nominal cloud radius in pixels (overall size scales with this).
 * @param col    Fill colour (already packed to the sprite's format).
 */
static void drawCloudShape(int cx, int cy, int r, uint32_t col) {
    int r2 = r * 2 / 3;
    spr.fillCircle(cx,        cy,       r2,  col);
    spr.fillCircle(cx - r/2,  cy + r/4, r/2, col);
    spr.fillCircle(cx + r/2,  cy + r/4, r/2, col);
    // Fill the gap between the three circles.  Width and height are kept
    // inside the circle edges at every y level so no rectangle corners poke out.
    spr.fillRect(cx - r*2/3, cy + r/4, r*4/3, r/3, col);
}

/**
 * @brief Draw an animated weather icon for a WMO code.
 *
 * Selects and renders one animated glyph (rotating sun, drifting cloud, falling
 * rain/snow, lightning flash, etc.) matching the condition.
 * @param cx,cy  Icon centre in screen pixels.
 * @param r      Icon radius in pixels (draws scale with this).
 * @param code   WMO weather code (0–99).
 * @param t      Animation clock in milliseconds (typically millis()).
 */
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
/** @brief Draw the Weather screen: large current temperature (°C/°F per
 *         settings), condition, humidity/wind/rain/lightning stats, an animated
 *         condition icon, a tap-to-silence lightning-alert banner, and a 7-day
 *         forecast strip with temperature-range bars and wind arrows.
 *         See screen_weather.h. */
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
    float dispTemp = g_settings.useCelsius ? wd.temp : wd.temp * 9.0f / 5.0f + 32.0f;
    snprintf(tbuf, sizeof(tbuf), "%d", (int)dispTemp);

    spr.setFont(UI_FONT_40);
    spr.setTextColor(C(COL_WHITE));
    int tempW = spr.textWidth(tbuf);
    spr.setCursor(6, tempTop);
    spr.print(tbuf);

    // Degree symbol + "C"/"F" — "\xC2\xB0" is U+00B0, now a real glyph in DejaVu18
    spr.setFont(UI_FONT_18);
    spr.setTextColor(C(COL_GREY_L));
    spr.setCursor(6 + tempW + 6, tempTop + 8);
    spr.print(g_settings.useCelsius ? "\xC2\xB0" "C" : "\xC2\xB0" "F");

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
    int dispWind = g_settings.useKm ? (int)(wd.windMph * 1.60934f + 0.5f) : wd.windMph;
    spr.printf("Wind: %d %s", dispWind, g_settings.useKm ? "km/h" : "mph");
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

    // ── Lightning alert banner (only when thunder is due within the hour) ──────
    if (weatherLightningSoon()) {
        const int BH = 30;
        s_alertX = 4; s_alertY = y; s_alertW = SCREEN_W - 8; s_alertH = BH;
        spr.fillRoundRect(s_alertX, y, s_alertW, BH, 3, C(0xC62828UL));
        spr.drawRoundRect(s_alertX, y, s_alertW, BH, 3, C(0xFF5252UL));

        spr.setFont(UI_FONT_12);
        spr.setTextColor(C(COL_WHITE));
        const char* msg = "! LIGHTNING ALERT !";
        spr.setCursor((SCREEN_W - spr.textWidth(msg)) / 2, y + 4);
        spr.print(msg);

        spr.setFont(UI_FONT_9);
        bool acked = weatherLightningAcked();
        const char* hint = acked ? "LED silenced" : "Tap to silence LED";
        spr.setTextColor(acked ? spr.color888(255, 190, 190) : C(COL_WHITE));
        spr.setCursor((SCREEN_W - spr.textWidth(hint)) / 2, y + 17);
        spr.print(hint);

        y += BH + 4;
    } else {
        s_alertX = -1;   // no box this frame
    }

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
    // Bar bottom anchored 37 px above the tile bottom (room for min-temp, precip
    // and wind rows). Equals 100 at full height; shrinks if the lightning banner
    // has pushed the forecast down, keeping the bottom rows on-screen.
    const int barRelBot = tileH - 37;
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

        // Max temp (convert from stored °C)
        int dispMaxT = g_settings.useCelsius ? d.maxT : (int)(d.maxT * 9.0f / 5.0f + 32.0f);
        spr.setFont(UI_FONT_9);
        spr.setTextColor(C(COL_WHITE));
        char maxbuf[10];
        snprintf(maxbuf, sizeof(maxbuf), "%d\xC2\xB0", dispMaxT);   // \xC2\xB0 = U+00B0
        int mw   = spr.textWidth(maxbuf);
        int mxOff = cx + (colW - 2 - mw) / 2;
        spr.setCursor(mxOff, y + 37);
        spr.print(maxbuf);

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

        // Min temp (convert from stored °C)
        int dispMinT = g_settings.useCelsius ? d.minT : (int)(d.minT * 9.0f / 5.0f + 32.0f);
        spr.setFont(UI_FONT_9);
        spr.setTextColor(C(COL_GREY));
        char minbuf[10];
        snprintf(minbuf, sizeof(minbuf), "%d\xC2\xB0", dispMinT);   // \xC2\xB0 = U+00B0
        int mnw   = spr.textWidth(minbuf);
        int mnOff = cx + (colW - 2 - mnw) / 2;
        spr.setCursor(mnOff, y + barRelBot + 2);
        spr.print(minbuf);

        // Precipitation (optional, moved up 8 px)
        if (d.precip >= 0.1f) {
            spr.setTextColor(C(COL_BLUE));
            char pbuf[8];
            snprintf(pbuf, sizeof(pbuf),
                     d.precip < 10.0f ? "%.1f" : "%.0f", d.precip);
            strlcat(pbuf, "mm", sizeof(pbuf));
            int pw = spr.textWidth(pbuf);
            spr.setCursor(cx + (colW - 2 - pw) / 2, y + barRelBot + 14);
            spr.print(pbuf);
        }

        // Wind direction arrow + speed  ─────────────────────────────────────
        // Arrow + speed number centred as a group in the tile.
        // "widget" width ≈ 9 px (arrow) + 3 px gap + up to 18 px (speed) = 30 px.
        {
            uint32_t windCol  = C(COL_GREY);
            const int windTextY = y + barRelBot + 24; // top of DejaVu9 text row
            const int windCY    = windTextY + 6;    // vertical centre for arrow

            char wsBuf[6];
            int tileWind = g_settings.useKm ? (int)(d.windMph * 1.60934f + 0.5f) : d.windMph;
            snprintf(wsBuf, sizeof(wsBuf), "%d", tileWind);
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
