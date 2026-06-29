#include "screen_pota.h"
#include "ui_common.h"
#include "config.h"
#include "data_store.h"
#include "settings.h"
#include "fonts/ui_fonts.h"

// ─── Band name from frequency (kHz) ───────────────────────────────────────────
static const char* potaFreqBand(float kHz) {
    if (kHz >=  1800 && kHz <  2000) return "160m";
    if (kHz >=  3500 && kHz <  4000) return  "80m";
    if (kHz >=  5258 && kHz <  5450) return  "60m";
    if (kHz >=  7000 && kHz <  7300) return  "40m";
    if (kHz >= 10100 && kHz < 10150) return  "30m";
    if (kHz >= 14000 && kHz < 14350) return  "20m";
    if (kHz >= 18068 && kHz < 18168) return  "17m";
    if (kHz >= 21000 && kHz < 21450) return  "15m";
    if (kHz >= 24890 && kHz < 24990) return  "12m";
    if (kHz >= 28000 && kHz < 29700) return  "10m";
    if (kHz >= 50000 && kHz < 54000) return   "6m";
    if (kHz >= 70000 && kHz < 70500) return   "4m";
    if (kHz >= 144000&& kHz < 148000)return   "2m";
    if (kHz >= 430000&& kHz < 440000)return  "70c";
    return "??";
}

// ─── Accent colour per band ────────────────────────────────────────────────────
static uint32_t potaBandColor(const char* b) {
    if (!b || !b[0]) return COL_GREY;
    switch (b[0]) {
        case '1':
            if (b[1] == '6') return 0x883333UL;  // 160m — dark red
            if (b[1] == '7') return 0x00ACC1UL;  // 17m  — teal
            if (b[1] == '5') return 0x1E88E5UL;  // 15m  — blue
            if (b[1] == '2') return 0x8E24AAUL;  // 12m  — purple
            if (b[1] == '0') return 0xE040FBUL;  // 10m  — violet
            break;
        case '8': return 0xE53935UL;              // 80m  — red
        case '6':
            if (b[1] == '0') return 0x607D8BUL;  // 60m  — steel blue
            return 0xF06292UL;                    //  6m  — pink
        case '4':
            if (b[1] == '0') return 0xFB8C00UL;  // 40m  — orange
            return 0x00BFA5UL;                    //  4m  — teal-green
        case '3': return 0xF9A825UL;              // 30m  — amber
        case '2':
            if (b[1] == '0') return 0x43A047UL;  // 20m  — green
            return 0xAB47BCUL;                    //  2m  — mauve
        case '7': return 0x26C6DAUL;              // 70cm — cyan
    }
    return COL_GREY_L;
}

// ─── Main draw function ────────────────────────────────────────────────────────
void drawScreenPOTA() {
    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    POTASpotsData ds = g_potaSpots;
    xSemaphoreGive(g_dataMutex);

    // 5 rows fill the entire content area (224 px → 44 px each, 4 px spare)
    const int ROW_H = CONTENT_H / POTA_SPOTS_MAX;   // 44 px

    if (!ds.valid || ds.count == 0) {
        drawLoader("Fetching POTA spots...");
        return;
    }

    POTASpot filtered[POTA_SPOTS_MAX];
    int fc = 0;
    for (int i = 0; i < ds.count && fc < POTA_SPOTS_MAX; i++) {
        if (g_settings.modeFilter & classifyMode(ds.spots[i].mode))
            filtered[fc++] = ds.spots[i];
    }

    if (fc == 0) {
        drawLoader("No spots match filter");
        return;
    }

    for (int i = 0; i < fc; i++) {
        const POTASpot& s = filtered[i];
        int ry = CONTENT_Y + i * ROW_H;

        // Row background — subtle alternation
        spr.fillRect(0, ry, SCREEN_W, ROW_H, C((i & 1) ? 0x0C0C0CUL : COL_BG));
        spr.drawFastHLine(0, ry + ROW_H - 1, SCREEN_W, C(COL_BORDER));

        // Left-edge band colour bar
        const char* band = potaFreqBand(s.freq);
        uint32_t    bc   = potaBandColor(band);
        spr.fillRect(0, ry, 4, ROW_H, C(bc));

        // ── Top line: activator callsign (left) | freq + mode (right) ────────
        spr.setFont(UI_FONT_12);
        spr.setTextColor(C(COL_WHITE));
        spr.setCursor(8, ry + 7);
        spr.print(s.activator);

        // Frequency + mode — e.g. "14.074 FT8" or "7.056 CW"
        char fbuf[20];
        if (s.freq < 30000.0f)
            snprintf(fbuf, sizeof(fbuf), "%.3f %s", s.freq / 1000.0f, s.mode);
        else
            snprintf(fbuf, sizeof(fbuf), "%.2f %s", s.freq / 1000.0f, s.mode);
        spr.setFont(UI_FONT_9);
        spr.setTextColor(C(bc));
        int fw = spr.textWidth(fbuf);
        spr.setCursor(SCREEN_W - fw - 5, ry + 9);
        spr.print(fbuf);

        // ── Bottom line: reference (left) | park name (centre) | dist/time (right)
        spr.setFont(UI_FONT_9);

        // Park reference
        spr.setTextColor(C(COL_AMBER));
        spr.setCursor(8, ry + 27);
        spr.print(s.reference);
        int refW = spr.textWidth(s.reference);

        // Distance in miles when known, else UTC time
        char rbuf[14];
        if (s.dist_km > 0) {
            int miles = (int)(s.dist_km * 0.621371f + 0.5f);
            if (miles >= 1000)
                snprintf(rbuf, sizeof(rbuf), "%d,%03dmi",
                         miles / 1000, miles % 1000);
            else
                snprintf(rbuf, sizeof(rbuf), "%dmi", miles);
        } else {
            strlcpy(rbuf, s.time, sizeof(rbuf));
        }
        spr.setTextColor(C(s.dist_km > 0 ? COL_AMBER : COL_GREY_L));
        int rw = spr.textWidth(rbuf);
        spr.setCursor(SCREEN_W - rw - 5, ry + 27);
        spr.print(rbuf);

        // Park name fitted into the gap between reference and right value
        if (s.parkName[0]) {
            int usedL = 8 + refW + 6;
            int usedR = rw + 10;
            int available = SCREEN_W - usedL - usedR;
            if (available > 16) {
                spr.setTextColor(C(COL_DIM_WHITE));
                spr.setCursor(usedL, ry + 27);
                char pname[sizeof(s.parkName)];
                strlcpy(pname, s.parkName, sizeof(pname));
                int plen = (int)strlen(pname);
                while (plen > 0 && (int)spr.textWidth(pname) > available)
                    pname[--plen] = '\0';
                if (plen > 0) spr.print(pname);
            }
        }
    }

    // ── Source watermark ──────────────────────────────────────────────────────
    spr.setFont(UI_FONT_9);
    spr.setTextColor(C(0x444444UL));
    const char* src = "pota.app";
    int sw = spr.textWidth(src);
    spr.setCursor(SCREEN_W - sw - 4, SCREEN_H - 11);
    spr.print(src);
}
