#include "screen_pskreporter.h"
#include "screen_dxspots.h"
#include "screen_greyline.h"
#include "ui_common.h"
#include "config.h"
#include "data_store.h"
#include "settings.h"
#include "fonts/ui_fonts.h"

// ─── Band colour from frequency in MHz ────────────────────────────────────────
// If-chain keeps everything in flash (.rodata / code) — no DRAM struct arrays.
static uint32_t pskBandColor(float MHz) {
    if (MHz >=  1.8f   && MHz <   2.0f)  return 0x883333UL;  // 160m dark red
    if (MHz >=  3.5f   && MHz <   4.0f)  return 0xE53935UL;  // 80m  red
    if (MHz >=  5.258f && MHz <   5.45f) return 0x607D8BUL;  // 60m  steel blue
    if (MHz >=  7.0f   && MHz <   7.3f)  return 0xFB8C00UL;  // 40m  orange
    if (MHz >= 10.1f   && MHz <  10.15f) return 0xF9A825UL;  // 30m  amber
    if (MHz >= 14.0f   && MHz <  14.35f) return 0x43A047UL;  // 20m  green
    if (MHz >= 18.068f && MHz <  18.168f)return 0x00ACC1UL;  // 17m  teal
    if (MHz >= 21.0f   && MHz <  21.45f) return 0x1E88E5UL;  // 15m  blue
    if (MHz >= 24.89f  && MHz <  24.99f) return 0x8E24AAUL;  // 12m  purple
    if (MHz >= 28.0f   && MHz <  29.7f)  return 0xE040FBUL;  // 10m  violet
    if (MHz >= 50.0f   && MHz <  54.0f)  return 0xF06292UL;  //  6m  pink
    if (MHz >= 70.0f   && MHz <  70.5f)  return 0x00BFA5UL;  //  4m  teal-green
    if (MHz >= 144.0f  && MHz < 148.0f)  return 0xAB47BCUL;  //  2m  mauve
    if (MHz >= 430.0f  && MHz < 440.0f)  return 0x26C6DAUL;  // 70cm cyan
    return 0x888888UL;
}

// Returns band label for the legend, or nullptr if unrecognised.
static const char* pskBandName(float MHz) {
    if (MHz >=  1.8f   && MHz <   2.0f)  return "160m";
    if (MHz >=  3.5f   && MHz <   4.0f)  return "80m";
    if (MHz >=  5.258f && MHz <   5.45f) return "60m";
    if (MHz >=  7.0f   && MHz <   7.3f)  return "40m";
    if (MHz >= 10.1f   && MHz <  10.15f) return "30m";
    if (MHz >= 14.0f   && MHz <  14.35f) return "20m";
    if (MHz >= 18.068f && MHz <  18.168f)return "17m";
    if (MHz >= 21.0f   && MHz <  21.45f) return "15m";
    if (MHz >= 24.89f  && MHz <  24.99f) return "12m";
    if (MHz >= 28.0f   && MHz <  29.7f)  return "10m";
    if (MHz >= 50.0f   && MHz <  54.0f)  return "6m";
    if (MHz >= 70.0f   && MHz <  70.5f)  return "4m";
    if (MHz >= 144.0f  && MHz < 148.0f)  return "2m";
    if (MHz >= 430.0f  && MHz < 440.0f)  return "70cm";
    return nullptr;
}

// ─── Map projection helpers (mirror of screen_greyline.cpp) ───────────────────
static const int   _MAP_Y0  = STATUS_H;
static const int   _MAP_W   = SCREEN_W;
static const int   _MAP_H   = CONTENT_H;

// Set per-frame before drawing spots
static float _mapLonMin = -180.0f;
static float _mapLonRng = 360.0f;
static float _mapLatTop = 75.0f;
static float _mapLatRng = 138.0f;

static inline int _lonToX(float lon) {
    return (int)((lon - _mapLonMin) / _mapLonRng * _MAP_W);
}
static inline int _latToY(float lat) {
    return _MAP_Y0 + (int)((_mapLatTop - lat) / _mapLatRng * _MAP_H);
}

// ─── Touch state for info overlay ─────────────────────────────────────────────
// -1 = nothing selected, 0 = furthest, 1 = loudest
static int   s_pskSelected = -1;
static int   s_farX, s_farY, s_loudX, s_loudY;
static int   s_iFar = -1, s_iLoud = -1;
static float s_farDist = 0;

// ─── Haversine great-circle distance in km ────────────────────────────────────
static float _gcKm(float la1, float lo1, float la2, float lo2) {
    const float R   = 6371.0f;
    const float D2R = (float)M_PI / 180.0f;
    float dlat = (la2 - la1) * D2R;
    float dlon = (lo2 - lo1) * D2R;
    float a = sinf(dlat * 0.5f) * sinf(dlat * 0.5f) +
              cosf(la1 * D2R) * cosf(la2 * D2R) *
              sinf(dlon * 0.5f) * sinf(dlon * 0.5f);
    return R * 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));
}

// ─── Main draw function ───────────────────────────────────────────────────────
void drawScreenPSKReporter() {
    // ── Reception spots ───────────────────────────────────────────────────────
    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    PSKData ds = g_pskData;
    xSemaphoreGive(g_dataMutex);

    // ── Compute zoom bounds from QTH + all spots ─────────────────────────────
    if (ds.valid && ds.count > 0) {
        float lonMin = g_settings.lon, lonMax = g_settings.lon;
        float latMin = g_settings.lat, latMax = g_settings.lat;
        for (int i = 0; i < ds.count; i++) {
            if (ds.reports[i].lon < lonMin) lonMin = ds.reports[i].lon;
            if (ds.reports[i].lon > lonMax) lonMax = ds.reports[i].lon;
            if (ds.reports[i].lat < latMin) latMin = ds.reports[i].lat;
            if (ds.reports[i].lat > latMax) latMax = ds.reports[i].lat;
        }
        float lonPad = (lonMax - lonMin) * 0.15f + 5.0f;
        float latPad = (latMax - latMin) * 0.15f + 5.0f;
        lonMin -= lonPad; lonMax += lonPad;
        latMin -= latPad; latMax += latPad;
        if (lonMin < -180.0f) lonMin = -180.0f;
        if (lonMax >  180.0f) lonMax =  180.0f;
        if (latMin <  -85.0f) latMin =  -85.0f;
        if (latMax >   85.0f) latMax =   85.0f;

        float lonRng = lonMax - lonMin;
        float latRng = latMax - latMin;
        const float aspect = (float)_MAP_W / (float)_MAP_H;

        if (lonRng / latRng > aspect) {
            float need = lonRng / aspect;
            float c = (latMin + latMax) * 0.5f;
            latMin = c - need * 0.5f;
            latMax = c + need * 0.5f;
        } else {
            float need = latRng * aspect;
            float c = (lonMin + lonMax) * 0.5f;
            lonMin = c - need * 0.5f;
            lonMax = c + need * 0.5f;
        }

        if (lonMin < -180.0f) lonMin = -180.0f;
        if (lonMax >  180.0f) lonMax =  180.0f;
        if (latMin <  -85.0f) latMin =  -85.0f;
        if (latMax >   85.0f) latMax =   85.0f;

        _mapLonMin = lonMin;
        _mapLonRng = lonMax - lonMin;
        _mapLatTop = latMax;
        _mapLatRng = latMax - latMin;
        drawWorldMapZoomed(lonMin, lonMax, latMin, latMax);
    } else {
        _mapLonMin = -180.0f;
        _mapLonRng = 360.0f;
        _mapLatTop = 75.0f;
        _mapLatRng = 138.0f;
        drawWorldMap();
    }

    if (!ds.valid) {
        spr.setFont(UI_FONT_9);
        const char* base;
        bool animate = false;
        if (!g_settings.callsign[0])
            base = "Set callsign in Settings";
        else if (ds.fetchFailed)
            base = "PSK Reporter unavailable";
        else {
            base = "Fetching PSK Reporter Data";
            animate = true;
        }
        char buf[40];
        if (animate) {
            int dots = (millis() / 400) % 4;
            snprintf(buf, sizeof(buf), "%s%.*s", base, dots, "...");
        } else {
            strlcpy(buf, base, sizeof(buf));
        }
        spr.setTextColor(spr.color888(180, 180, 180));
        int fullW = spr.textWidth(animate ? "Fetching PSK Reporter..." : buf);
        spr.setCursor((SCREEN_W - fullW) / 2, _MAP_Y0 + _MAP_H / 2 - 4);
        spr.print(buf);
        return;
    }

    // ds.valid but ds.count == 0 → fetch succeeded, nobody heard us recently.
    // Fall through: the QTH marker and badge ("0 RX / 15min") will still draw.

    // Draw a faint line from QTH to each receiver, then the dot on top
    int qthX = _lonToX(g_settings.lon);
    int qthY = _latToY(g_settings.lat);

    for (int i = 0; i < ds.count; i++) {
        const PSKReport& r = ds.reports[i];
        int rx = _lonToX(r.lon);
        int ry = _latToY(r.lat);

        // Clamp to map bounds
        rx = constrain(rx, 0, _MAP_W - 1);
        ry = constrain(ry, _MAP_Y0, _MAP_Y0 + _MAP_H - 1);

        uint32_t col = pskBandColor(r.freqMHz);

        // Dim propagation line
        uint32_t lineCol = spr.color888(
            ((col >> 16) & 0xFF) / 5,
            ((col >>  8) & 0xFF) / 5,
            ( col        & 0xFF) / 5);
        spr.drawLine(qthX, qthY, rx, ry, lineCol);
    }

    // Find furthest spot and loudest (highest SNR) spot
    int iFar = -1, iLoud = -1;
    float maxDist = 0;
    int8_t maxSnr = -128;
    for (int i = 0; i < ds.count; i++) {
        float d = _gcKm(g_settings.lat, g_settings.lon,
                        ds.reports[i].lat, ds.reports[i].lon);
        if (d > maxDist) { maxDist = d; iFar = i; }
        if (ds.reports[i].snr > maxSnr) { maxSnr = ds.reports[i].snr; iLoud = i; }
    }

    // Dots on top of lines
    for (int i = 0; i < ds.count; i++) {
        const PSKReport& r = ds.reports[i];
        int rx = _lonToX(r.lon);
        int ry = _latToY(r.lat);
        rx = constrain(rx, 2, _MAP_W - 3);
        ry = constrain(ry, _MAP_Y0 + 2, _MAP_Y0 + _MAP_H - 3);

        uint32_t col = pskBandColor(r.freqMHz);
        spr.fillCircle(rx, ry, 2, C(col));
    }

    // Pulsing rings on furthest and loudest spots — save positions for touch
    s_iFar = iFar; s_iLoud = iLoud; s_farDist = maxDist;
    s_farX = s_farY = s_loudX = s_loudY = -1;

    if (ds.count > 0) {
        uint32_t now = millis();
        float phase = (now % 1500) / 1500.0f;
        int pulseR = 4 + (int)(phase * 8);
        uint8_t alpha = (uint8_t)(255 * (1.0f - phase));

        auto drawPulse = [&](int idx, uint32_t col, int& sx, int& sy) {
            if (idx < 0) return;
            sx = _lonToX(ds.reports[idx].lon);
            sy = _latToY(ds.reports[idx].lat);
            sx = constrain(sx, 2, _MAP_W - 3);
            sy = constrain(sy, _MAP_Y0 + 2, _MAP_Y0 + _MAP_H - 3);
            uint32_t dimCol = spr.color888(
                ((col >> 16) & 0xFF) * alpha / 255,
                ((col >>  8) & 0xFF) * alpha / 255,
                ( col        & 0xFF) * alpha / 255);
            spr.drawCircle(sx, sy, pulseR, C(dimCol));
            spr.fillCircle(sx, sy, 3, C(col));
        };

        drawPulse(iFar,  COL_ORANGE, s_farX, s_farY);
        if (iLoud != iFar)
            drawPulse(iLoud, COL_GREEN, s_loudX, s_loudY);
    }

    // ── Band legend (left column) ─────────────────────────────────────────────
    // Walk reports once and collect unique (name, colour) pairs in low→high order.
    // All working state is on the stack — no file-scope struct arrays needed.
    {
        // One representative frequency per unique band seen (up to 14 bands).
        // We store just the frequency; name and colour are derived on demand.
        float  seenFreq[14];
        int    seenCount = 0;

        for (int i = 0; i < ds.count && seenCount < 14; i++) {
            float f = ds.reports[i].freqMHz;
            const char* nm = pskBandName(f);
            if (!nm) continue;
            // Dedup by pointer equality — same literal → same band
            bool dup = false;
            for (int j = 0; j < seenCount; j++)
                if (pskBandName(seenFreq[j]) == nm) { dup = true; break; }
            if (!dup) seenFreq[seenCount++] = f;
        }

        if (seenCount > 0) {
            const int ROW_H = 12;
            const int LEG_W = 38;
            int ly = _MAP_Y0 + 3;

            // Dark opaque backing so text is readable over the map
            spr.fillRect(0, ly - 1, LEG_W, seenCount * ROW_H + 2, C(COL_BG));

            spr.setFont(UI_FONT_9);
            for (int j = 0; j < seenCount; j++) {
                uint32_t col = pskBandColor(seenFreq[j]);
                spr.fillCircle(5, ly + 4, 2, C(col));       // same size as map dots
                spr.setTextColor(spr.color888(170, 170, 170));
                spr.setCursor(11, ly);
                spr.print(pskBandName(seenFreq[j]));
                ly += ROW_H;
            }
        }
    }

    // Re-draw QTH marker on top (lines may have covered it)
    spr.fillCircle(qthX, qthY, 3, C(COL_ORANGE));
    spr.drawCircle(qthX, qthY, 5, C(COL_ORANGE));

    // ── Summary badge — top-right ──────────────────────────────────────────────
    char badge[28];
    snprintf(badge, sizeof(badge), "%d RX / 15min", ds.total);
    spr.setFont(UI_FONT_9);
    spr.setTextColor(spr.color888(200, 200, 200));
    int bw = spr.textWidth(badge);
    spr.setCursor(SCREEN_W - bw - 3, _MAP_Y0 + 2);
    spr.print(badge);

    // ── Furthest station — bottom-left ───────────────────────────────────────────
    if (iFar >= 0) {
        const PSKReport& rf = ds.reports[iFar];
        const char* band = pskBandName(rf.freqMHz);
        char farBuf[48];
        if (g_settings.useKm)
            snprintf(farBuf, sizeof(farBuf), "%s %s %dkm",
                     rf.call, band ? band : "?", (int)(maxDist + 0.5f));
        else
            snprintf(farBuf, sizeof(farBuf), "%s %s %dmi",
                     rf.call, band ? band : "?", (int)(maxDist * 0.621371f + 0.5f));
        spr.setFont(UI_FONT_12);
        spr.setTextColor(C(COL_ORANGE));
        spr.setCursor(4, SCREEN_H - 14);
        spr.print(farBuf);
    }

    // ── Callsign — bottom-right ────────────────────────────────────────────────
    if (g_settings.callsign[0]) {
        spr.setFont(UI_FONT_12);
        spr.setTextColor(C(COL_ORANGE));
        int cw = spr.textWidth(g_settings.callsign);
        spr.setCursor(SCREEN_W - cw - 4, SCREEN_H - 14);
        spr.print(g_settings.callsign);
    }

    // ── Info overlay for tapped spot ──────────────────────────────────────────
    if (s_pskSelected >= 0 && ds.count > 0) {
        int idx = (s_pskSelected == 0) ? s_iFar : s_iLoud;
        if (idx >= 0 && idx < ds.count) {
            const PSKReport& r = ds.reports[idx];
            float dist = _gcKm(g_settings.lat, g_settings.lon, r.lat, r.lon);
            const char* band = pskBandName(r.freqMHz);

            const char* country = callCountry(r.call);

            char line1[32], line2[48], line3[32];
            snprintf(line1, sizeof(line1), "%s", r.call);
            snprintf(line2, sizeof(line2), "%s  %s  SNR %ddB",
                     r.grid, band ? band : "?", r.snr);
            if (g_settings.useKm)
                snprintf(line3, sizeof(line3), "%d km  %s",
                         (int)(dist + 0.5f),
                         s_pskSelected == 0 ? "FURTHEST" : "LOUDEST");
            else
                snprintf(line3, sizeof(line3), "%d mi  %s",
                         (int)(dist * 0.621371f + 0.5f),
                         s_pskSelected == 0 ? "FURTHEST" : "LOUDEST");

            spr.setFont(UI_FONT_9);
            int w1 = spr.textWidth(line1);
            int w2 = spr.textWidth(line2);
            int w3 = spr.textWidth(line3);
            int wc = (country[0]) ? spr.textWidth(country) : 0;
            int pw = max(max(max(w1, w2), w3), wc) + 16;
            int ph = country[0] ? 52 : 40;
            int px = (SCREEN_W - pw) / 2;
            int py = _MAP_Y0 + _MAP_H / 2 - ph / 2;

            uint32_t accentCol = (s_pskSelected == 0) ? COL_ORANGE : COL_GREEN;

            spr.fillRect(px, py, pw, ph, C(COL_BG));
            spr.drawRect(px, py, pw, ph, C(accentCol));

            spr.setFont(UI_FONT_12);
            spr.setTextColor(C(accentCol));
            int cw2 = spr.textWidth(line1);
            spr.setCursor(px + (pw - cw2) / 2, py + 3);
            spr.print(line1);

            spr.setFont(UI_FONT_9);
            if (country[0]) {
                spr.setTextColor(spr.color888(200, 200, 200));
                spr.setCursor(px + (pw - wc) / 2, py + 16);
                spr.print(country);
            }

            int yOff = country[0] ? 12 : 0;
            spr.setTextColor(spr.color888(200, 200, 200));
            spr.setCursor(px + (pw - w2) / 2, py + 16 + yOff);
            spr.print(line2);

            spr.setTextColor(C(accentCol));
            spr.setCursor(px + (pw - w3) / 2, py + 28 + yOff);
            spr.print(line3);
        }
    }
}

void pskClearSelection() { s_pskSelected = -1; }

// ─── Touch handler — returns true if tap was consumed ─────────────────────────
bool pskTouchUp(int32_t x, int32_t y) {
    const int HIT = 25;

    if (s_pskSelected >= 0) {
        s_pskSelected = -1;
        return true;
    }

    if (s_farX >= 0 && abs(x - s_farX) < HIT && abs(y - s_farY) < HIT) {
        s_pskSelected = 0;
        return true;
    }
    if (s_loudX >= 0 && s_iFar != s_iLoud &&
        abs(x - s_loudX) < HIT && abs(y - s_loudY) < HIT) {
        s_pskSelected = 1;
        return true;
    }
    return false;
}
