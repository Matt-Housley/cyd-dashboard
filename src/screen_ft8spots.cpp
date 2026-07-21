#include "screen_ft8spots.h"
#include "screen_greyline.h"   // drawWorldMap / drawWorldMapZoomed
#include "screen_dxspots.h"    // callCountry
#include "ui_common.h"
#include "config.h"
#include "data_store.h"
#include "settings.h"
#include "fonts/ui_fonts.h"

// Band colour / label — shared logic with PSK Reporter (duplicated to avoid
// cross-screen coupling; the compiler will inline and eliminate dead code).
static uint32_t ft8BandColor(float MHz) {
    if (MHz >=  1.8f   && MHz <   2.0f)  return 0x883333UL;
    if (MHz >=  3.5f   && MHz <   4.0f)  return 0xE53935UL;
    if (MHz >=  5.258f && MHz <   5.45f) return 0x607D8BUL;
    if (MHz >=  7.0f   && MHz <   7.3f)  return 0xFB8C00UL;
    if (MHz >= 10.1f   && MHz <  10.15f) return 0xF9A825UL;
    if (MHz >= 14.0f   && MHz <  14.35f) return 0x43A047UL;
    if (MHz >= 18.068f && MHz <  18.168f)return 0x00ACC1UL;
    if (MHz >= 21.0f   && MHz <  21.45f) return 0x1E88E5UL;
    if (MHz >= 24.89f  && MHz <  24.99f) return 0x8E24AAUL;
    if (MHz >= 28.0f   && MHz <  29.7f)  return 0xE040FBUL;
    if (MHz >= 50.0f   && MHz <  54.0f)  return 0xF06292UL;
    if (MHz >= 70.0f   && MHz <  70.5f)  return 0x00BFA5UL;
    if (MHz >= 144.0f  && MHz < 148.0f)  return 0xAB47BCUL;
    if (MHz >= 430.0f  && MHz < 440.0f)  return 0x26C6DAUL;
    return 0x888888UL;
}

static const char* ft8BandName(float MHz) {
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

// ─── Map projection ────────────────────────────────────────────────────────────
static const int   _F_Y0 = STATUS_H;
static const int   _F_W  = SCREEN_W;
static const int   _F_H  = CONTENT_H;

static float _fLonMin = -180.0f;
static float _fLonRng = 360.0f;
static float _fLatTop = 75.0f;
static float _fLatRng = 138.0f;

static inline int _fLonToX(float lon) {
    return (int)((lon - _fLonMin) / _fLonRng * _F_W);
}
static inline int _fLatToY(float lat) {
    return _F_Y0 + (int)((_fLatTop - lat) / _fLatRng * _F_H);
}

// ─── Touch / overlay state ─────────────────────────────────────────────────────
static int   s_sel    = -1;   // -1 = none, 0 = furthest, 1 = loudest
static int   s_farX   = -1, s_farY  = -1;
static int   s_loudX  = -1, s_loudY = -1;
static int   s_iFar   = -1, s_iLoud = -1;
static float s_farDist = 0;

// Band highlighted in the legend after tapping an ordinary spot.  Points at a
// string literal from ft8BandName(), so it needs no storage of its own.
static const char* s_selBand = nullptr;

// ─── Haversine ─────────────────────────────────────────────────────────────────
static float _fGcKm(float la1, float lo1, float la2, float lo2) {
    const float R   = 6371.0f;
    const float D2R = (float)M_PI / 180.0f;
    float dlat = (la2 - la1) * D2R;
    float dlon = (lo2 - lo1) * D2R;
    float a = sinf(dlat * 0.5f) * sinf(dlat * 0.5f) +
              cosf(la1 * D2R) * cosf(la2 * D2R) *
              sinf(dlon * 0.5f) * sinf(dlon * 0.5f);
    return R * 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));
}

// ─── Main draw ─────────────────────────────────────────────────────────────────
void drawScreenFT8Spots() {
    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    FT8SpotsData ds = g_ft8Spots;
    xSemaphoreGive(g_dataMutex);

    // ── Compute zoom bounds from QTH + all DX spots ───────────────────────────
    if (ds.valid && ds.count > 0) {
        float lonMin = g_settings.lon, lonMax = g_settings.lon;
        float latMin = g_settings.lat, latMax = g_settings.lat;
        for (int i = 0; i < ds.count; i++) {
            if (ds.spots[i].lon < lonMin) lonMin = ds.spots[i].lon;
            if (ds.spots[i].lon > lonMax) lonMax = ds.spots[i].lon;
            if (ds.spots[i].lat < latMin) latMin = ds.spots[i].lat;
            if (ds.spots[i].lat > latMax) latMax = ds.spots[i].lat;
        }
        float lonPad = (lonMax - lonMin) * 0.15f + 5.0f;
        float latPad = (latMax - latMin) * 0.15f + 5.0f;
        lonMin -= lonPad; lonMax += lonPad;
        latMin -= latPad; latMax += latPad;
        lonMin = max(lonMin, -180.0f); lonMax = min(lonMax, 180.0f);
        latMin = max(latMin,  -85.0f); latMax = min(latMax,  85.0f);

        float lonRng = lonMax - lonMin;
        float latRng = latMax - latMin;
        const float aspect = (float)_F_W / (float)_F_H;
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
        lonMin = max(lonMin, -180.0f); lonMax = min(lonMax, 180.0f);
        latMin = max(latMin,  -85.0f); latMax = min(latMax,  85.0f);

        _fLonMin = lonMin; _fLonRng = lonMax - lonMin;
        _fLatTop = latMax; _fLatRng = latMax - latMin;
        drawWorldMapZoomed(lonMin, lonMax, latMin, latMax);
    } else {
        _fLonMin = -180.0f; _fLonRng = 360.0f;
        _fLatTop = 75.0f;   _fLatRng = 138.0f;
        drawWorldMap();
    }

    // ── Not-ready / error message ─────────────────────────────────────────────
    if (!ds.valid) {
        spr.setFont(UI_FONT_9);
        const char* base;
        bool animate = false;
        if (!g_settings.grid[0])
            base = "Set grid locator in Settings";
        else if (ds.fetchFailed)
            base = "FT8 Spots unavailable";
        else {
            base = "Fetching FT8 Spots";
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
        int fw = spr.textWidth(animate ? "Fetching FT8 Spots..." : buf);
        spr.setCursor((SCREEN_W - fw) / 2, _F_Y0 + _F_H / 2 - 4);
        spr.print(buf);
        return;
    }

    int qthX = _fLonToX(g_settings.lon);
    int qthY = _fLatToY(g_settings.lat);

    // ── Faint propagation lines from QTH to each DX station ──────────────────
    for (int i = 0; i < ds.count; i++) {
        const FT8Spot& s = ds.spots[i];
        int dx = _fLonToX(s.lon);
        int dy = _fLatToY(s.lat);
        dx = constrain(dx, 0, _F_W - 1);
        dy = constrain(dy, _F_Y0, _F_Y0 + _F_H - 1);
        uint32_t col = ft8BandColor(s.freqMHz);
        uint32_t lineCol = spr.color888(
            ((col >> 16) & 0xFF) / 5,
            ((col >>  8) & 0xFF) / 5,
            ( col        & 0xFF) / 5);
        spr.drawLine(qthX, qthY, dx, dy, lineCol);
    }

    // ── Find furthest and highest-SNR spots ───────────────────────────────────
    int iFar = -1, iLoud = -1;
    float maxDist = 0;
    int8_t maxSnr = -128;
    for (int i = 0; i < ds.count; i++) {
        float d = _fGcKm(g_settings.lat, g_settings.lon,
                         ds.spots[i].lat, ds.spots[i].lon);
        if (d > maxDist) { maxDist = d; iFar = i; }
        if (ds.spots[i].snr > maxSnr) { maxSnr = ds.spots[i].snr; iLoud = i; }
    }

    // ── Dots on top of lines ──────────────────────────────────────────────────
    for (int i = 0; i < ds.count; i++) {
        const FT8Spot& s = ds.spots[i];
        int dx = _fLonToX(s.lon);
        int dy = _fLatToY(s.lat);
        dx = constrain(dx, 2, _F_W - 3);
        dy = constrain(dy, _F_Y0 + 2, _F_Y0 + _F_H - 3);
        spr.fillCircle(dx, dy, 2, C(ft8BandColor(s.freqMHz)));
    }

    // ── Pulsing rings on furthest (orange) and loudest (green) ───────────────
    s_iFar = iFar; s_iLoud = iLoud; s_farDist = maxDist;
    s_farX = s_farY = s_loudX = s_loudY = -1;

    if (ds.count > 0) {
        uint32_t now = millis();
        float phase = (now % 1500) / 1500.0f;
        int pulseR = 4 + (int)(phase * 8);
        uint8_t alpha = (uint8_t)(255 * (1.0f - phase));

        auto drawPulse = [&](int idx, uint32_t col, int& sx, int& sy) {
            if (idx < 0) return;
            sx = _fLonToX(ds.spots[idx].lon);
            sy = _fLatToY(ds.spots[idx].lat);
            sx = constrain(sx, 2, _F_W - 3);
            sy = constrain(sy, _F_Y0 + 2, _F_Y0 + _F_H - 3);
            uint32_t dimCol = spr.color888(
                ((col >> 16) & 0xFF) * alpha / 255,
                ((col >>  8) & 0xFF) * alpha / 255,
                ( col        & 0xFF) * alpha / 255);
            spr.drawCircle(sx, sy, pulseR, C(dimCol));
            spr.fillCircle(sx, sy, 3, C(col));
        };

        drawPulse(iFar,  COL_ORANGE, s_farX,  s_farY);
        if (iLoud != iFar)
            drawPulse(iLoud, COL_GREEN, s_loudX, s_loudY);
    }

    // ── Band legend (left column) ─────────────────────────────────────────────
    {
        float seenFreq[14];
        int   seenCount = 0;
        for (int i = 0; i < ds.count && seenCount < 14; i++) {
            float f = ds.spots[i].freqMHz;
            const char* nm = ft8BandName(f);
            if (!nm) continue;
            bool dup = false;
            for (int j = 0; j < seenCount; j++)
                if (ft8BandName(seenFreq[j]) == nm) { dup = true; break; }
            if (!dup) seenFreq[seenCount++] = f;
        }
        if (seenCount > 0) {
            const int ROW_H = 12;
            const int LEG_W = 38;
            const int legH  = seenCount * ROW_H + 2;

            // Candidate corners.  Top-right sits below the "N DX / 15min"
            // badge; the bottom pair sit above the furthest-DX and grid labels.
            struct Cand { int x, y; };
            const Cand cands[4] = {
                { 0,                _F_Y0 + 3            },   // top-left
                { SCREEN_W - LEG_W, _F_Y0 + 14           },   // top-right
                { 0,                SCREEN_H - 16 - legH },   // bottom-left
                { SCREEN_W - LEG_W, SCREEN_H - 16 - legH },   // bottom-right
            };

            // Score each corner by how much it would cover, then take the
            // clearest.  Ties resolve to the earliest candidate, so the
            // familiar top-left position wins when nothing is in the way.
            int bestI = 0, bestScore = 0x7FFFFFFF;
            for (int c = 0; c < 4; c++) {
                int x0 = cands[c].x - 3, x1 = cands[c].x + LEG_W + 3;
                int y0 = cands[c].y - 3, y1 = cands[c].y + legH  + 3;
                int score = 0;
                for (int i = 0; i < ds.count; i++) {
                    int dx = constrain(_fLonToX(ds.spots[i].lon), 2, _F_W - 3);
                    int dy = constrain(_fLatToY(ds.spots[i].lat),
                                       _F_Y0 + 2, _F_Y0 + _F_H - 3);
                    if (dx >= x0 && dx <= x1 && dy >= y0 && dy <= y1) score++;
                }
                // The QTH marker matters more than any single spot.
                if (qthX >= x0 && qthX <= x1 && qthY >= y0 && qthY <= y1) score += 3;
                if (score < bestScore) { bestScore = score; bestI = c; }
            }

            const int lx = cands[bestI].x;
            int       ly = cands[bestI].y;

            spr.fillRect(lx, ly - 1, LEG_W, legH, C(COL_BG));
            spr.setFont(UI_FONT_9);
            for (int j = 0; j < seenCount; j++) {
                const char* nm  = ft8BandName(seenFreq[j]);
                uint32_t    col = ft8BandColor(seenFreq[j]);

                // The tapped station's band is emphasised with a larger dot and
                // its own colour, then redrawn 1 px right for a faux-bold
                // weight — the DejaVu set has no bold variant.
                bool hot = (s_selBand && nm && strcmp(s_selBand, nm) == 0);

                spr.fillCircle(lx + 5, ly + 4, hot ? 3 : 2, C(col));
                spr.setTextColor(hot ? C(col) : spr.color888(170, 170, 170));
                spr.setCursor(lx + 11, ly);
                spr.print(nm);
                if (hot) { spr.setCursor(lx + 12, ly); spr.print(nm); }
                ly += ROW_H;
            }
        }
    }

    // ── QTH marker ───────────────────────────────────────────────────────────
    spr.fillCircle(qthX, qthY, 3, C(COL_ORANGE));
    spr.drawCircle(qthX, qthY, 5, C(COL_ORANGE));

    // ── Badge — top-right: "N DX / 15min" ────────────────────────────────────
    char badge[28];
    snprintf(badge, sizeof(badge), "%d DX / 15min", ds.total);
    spr.setFont(UI_FONT_9);
    spr.setTextColor(spr.color888(200, 200, 200));
    int bw = spr.textWidth(badge);
    spr.setCursor(SCREEN_W - bw - 3, _F_Y0 + 2);
    spr.print(badge);

    // ── Furthest DX — bottom-left ─────────────────────────────────────────────
    if (iFar >= 0) {
        const FT8Spot& rf = ds.spots[iFar];
        const char* band = ft8BandName(rf.freqMHz);
        char farBuf[48];
        if (g_settings.useKm)
            snprintf(farBuf, sizeof(farBuf), "%s %s %dkm",
                     rf.senderCall, band ? band : "?", (int)(maxDist + 0.5f));
        else
            snprintf(farBuf, sizeof(farBuf), "%s %s %dmi",
                     rf.senderCall, band ? band : "?",
                     (int)(maxDist * 0.621371f + 0.5f));
        spr.setFont(UI_FONT_12);
        spr.setTextColor(C(COL_ORANGE));
        spr.setCursor(4, SCREEN_H - 14);
        spr.print(farBuf);
    }

    // ── Grid square — bottom-right ────────────────────────────────────────────
    if (g_settings.grid[0]) {
        spr.setFont(UI_FONT_12);
        spr.setTextColor(C(COL_ORANGE));
        // Show first 4 chars of grid (the area we searched)
        char g4[5] = {};
        strlcpy(g4, g_settings.grid, 5);
        int gw = spr.textWidth(g4);
        spr.setCursor(SCREEN_W - gw - 4, SCREEN_H - 14);
        spr.print(g4);
    }

    // ── Info overlay ──────────────────────────────────────────────────────────
    if (s_sel >= 0 && ds.count > 0) {
        int idx = (s_sel == 0) ? s_iFar : s_iLoud;
        if (idx >= 0 && idx < ds.count) {
            const FT8Spot& r = ds.spots[idx];
            float dist = _fGcKm(g_settings.lat, g_settings.lon, r.lat, r.lon);
            const char* band    = ft8BandName(r.freqMHz);
            const char* country = callCountry(r.senderCall);

            char line1[20], line2[48], line3[32], line4[32];
            snprintf(line1, sizeof(line1), "%s", r.senderCall);
            snprintf(line2, sizeof(line2), "%s  %s  SNR %ddB",
                     r.senderGrid, band ? band : "?", r.snr);
            if (g_settings.useKm)
                snprintf(line3, sizeof(line3), "%d km  %s",
                         (int)(dist + 0.5f),
                         s_sel == 0 ? "FURTHEST" : "LOUDEST");
            else
                snprintf(line3, sizeof(line3), "%d mi  %s",
                         (int)(dist * 0.621371f + 0.5f),
                         s_sel == 0 ? "FURTHEST" : "LOUDEST");
            snprintf(line4, sizeof(line4), "via %s", r.rxCall);

            spr.setFont(UI_FONT_9);
            int w1 = spr.textWidth(line1);
            int w2 = spr.textWidth(line2);
            int w3 = spr.textWidth(line3);
            int w4 = spr.textWidth(line4);
            int wc = country[0] ? (int)spr.textWidth(country) : 0;
            int pw = max(max(max(max(w1, w2), w3), w4), wc) + 16;
            int ph = 52 + (country[0] ? 12 : 0);
            int px = (SCREEN_W - pw) / 2;
            int py = _F_Y0 + _F_H / 2 - ph / 2;

            uint32_t accentCol = (s_sel == 0) ? COL_ORANGE : COL_GREEN;

            spr.fillRect(px, py, pw, ph, C(COL_BG));
            spr.drawRect(px, py, pw, ph, C(accentCol));

            // Line 1: DX callsign
            spr.setFont(UI_FONT_12);
            spr.setTextColor(C(accentCol));
            int cw2 = spr.textWidth(line1);
            spr.setCursor(px + (pw - cw2) / 2, py + 3);
            spr.print(line1);

            spr.setFont(UI_FONT_9);
            int yy = py + 16;

            // Line 2: country (optional)
            if (country[0]) {
                spr.setTextColor(spr.color888(200, 200, 200));
                spr.setCursor(px + (pw - wc) / 2, yy);
                spr.print(country);
                yy += 12;
            }

            // Line 3: grid / band / SNR
            spr.setTextColor(spr.color888(200, 200, 200));
            spr.setCursor(px + (pw - w2) / 2, yy);
            spr.print(line2);
            yy += 12;

            // Line 4: distance + label
            spr.setTextColor(C(accentCol));
            spr.setCursor(px + (pw - w3) / 2, yy);
            spr.print(line3);
            yy += 12;

            // Line 5: "via RX_CALLSIGN"
            spr.setTextColor(spr.color888(140, 140, 160));
            spr.setCursor(px + (pw - w4) / 2, yy);
            spr.print(line4);
        }
    }
}

void ft8ClearSelection() { s_sel = -1; s_selBand = nullptr; }

bool ft8TouchUp(int32_t x, int32_t y) {
    const int HIT = 25;

    // Anything already showing? Tap dismisses it.
    if (s_sel >= 0 || s_selBand) { s_sel = -1; s_selBand = nullptr; return true; }

    if (s_farX >= 0 && abs(x - s_farX) < HIT && abs(y - s_farY) < HIT) {
        s_sel = 0; return true;
    }
    if (s_loudX >= 0 && s_iFar != s_iLoud &&
        abs(x - s_loudX) < HIT && abs(y - s_loudY) < HIT) {
        s_sel = 1; return true;
    }

    // Otherwise find the nearest ordinary spot and highlight its band in the
    // legend.  Screen positions are recomputed from the projection statics the
    // last draw left behind, so no per-spot coordinates need storing.
    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    FT8SpotsData ds = g_ft8Spots;
    xSemaphoreGive(g_dataMutex);
    if (!ds.valid || ds.count == 0) return false;

    int  bestI  = -1;
    long bestD2 = (long)HIT * HIT;   // squared, so no sqrt needed
    for (int i = 0; i < ds.count; i++) {
        if (i == s_iFar || i == s_iLoud) continue;   // those have their own overlay
        int  dx = constrain(_fLonToX(ds.spots[i].lon), 2, _F_W - 3);
        int  dy = constrain(_fLatToY(ds.spots[i].lat),
                            _F_Y0 + 2, _F_Y0 + _F_H - 3);
        long d2 = (long)(x - dx) * (x - dx) + (long)(y - dy) * (y - dy);
        if (d2 < bestD2) { bestD2 = d2; bestI = i; }
    }
    if (bestI < 0) return false;

    const char* nm = ft8BandName(ds.spots[bestI].freqMHz);
    if (!nm) return false;      // out-of-band spot has no legend row to bolden
    s_selBand = nm;
    return true;
}
