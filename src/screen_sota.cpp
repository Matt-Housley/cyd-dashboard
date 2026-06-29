#include "screen_sota.h"
#include "ui_common.h"
#include "config.h"
#include "data_store.h"
#include "settings.h"
#include "fonts/ui_fonts.h"

// ─── Band name from frequency (MHz as supplied by SOTA API) ───────────────────
static const char* sotaFreqBand(float MHz) {
    if (MHz >=  1.8f   && MHz <   2.0f)  return "160m";
    if (MHz >=  3.5f   && MHz <   4.0f)  return  "80m";
    if (MHz >=  5.258f && MHz <   5.45f) return  "60m";
    if (MHz >=  7.0f   && MHz <   7.3f)  return  "40m";
    if (MHz >= 10.1f   && MHz <  10.15f) return  "30m";
    if (MHz >= 14.0f   && MHz <  14.35f) return  "20m";
    if (MHz >= 18.068f && MHz <  18.168f)return  "17m";
    if (MHz >= 21.0f   && MHz <  21.45f) return  "15m";
    if (MHz >= 24.89f  && MHz <  24.99f) return  "12m";
    if (MHz >= 28.0f   && MHz <  29.7f)  return  "10m";
    if (MHz >= 50.0f   && MHz <  54.0f)  return   "6m";
    if (MHz >= 70.0f   && MHz <  70.5f)  return   "4m";
    if (MHz >= 144.0f  && MHz < 148.0f)  return   "2m";
    if (MHz >= 430.0f  && MHz < 440.0f)  return  "70c";
    return "??";
}

// ─── Accent colour per band ────────────────────────────────────────────────────
static uint32_t sotaBandColor(const char* b) {
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
void drawScreenSOTA() {
    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    SOTASpotsData ds = g_sotaSpots;
    xSemaphoreGive(g_dataMutex);

    // 5 rows fill the entire content area (224 px → 44 px each, 4 px spare)
    const int ROW_H = CONTENT_H / SOTA_SPOTS_MAX;   // 44 px

    if (!ds.valid || ds.count == 0) {
        drawLoader("Fetching SOTA Spots...");
        return;
    }

    SOTASpot filtered[SOTA_SPOTS_MAX];
    int fc = 0;
    for (int i = 0; i < ds.count && fc < SOTA_SPOTS_MAX; i++) {
        if (g_settings.modeFilter & classifyMode(ds.spots[i].mode))
            filtered[fc++] = ds.spots[i];
    }

    if (fc == 0) {
        drawLoader("No spots match filter");
        return;
    }

    for (int i = 0; i < fc; i++) {
        const SOTASpot& s = filtered[i];
        int ry = CONTENT_Y + i * ROW_H;

        // Row background — subtle alternation
        spr.fillRect(0, ry, SCREEN_W, ROW_H, C((i & 1) ? 0x0C0C0CUL : COL_BG));
        spr.drawFastHLine(0, ry + ROW_H - 1, SCREEN_W, C(COL_BORDER));

        // Left-edge band colour bar (SOTA API gives MHz)
        const char* band = sotaFreqBand(s.freq);
        uint32_t    bc   = sotaBandColor(band);
        spr.fillRect(0, ry, 4, ROW_H, C(bc));

        // ── Top line: activator callsign (left) | freq + mode (right) ────────
        spr.setFont(UI_FONT_12);
        spr.setTextColor(C(COL_WHITE));
        spr.setCursor(8, ry + 7);
        spr.print(s.activator);

        // Frequency shown in MHz (as received from API)
        char fbuf[20];
        snprintf(fbuf, sizeof(fbuf), "%.3f %s", s.freq, s.mode);
        spr.setFont(UI_FONT_9);
        spr.setTextColor(C(bc));
        int fw = spr.textWidth(fbuf);
        spr.setCursor(SCREEN_W - fw - 5, ry + 9);
        spr.print(fbuf);

        // ── Bottom line: summit ref (left) | name (centre) | dist/time (right)
        spr.setFont(UI_FONT_9);

        // Summit reference
        spr.setTextColor(C(COL_AMBER));
        spr.setCursor(8, ry + 27);
        spr.print(s.summit);
        int refW = spr.textWidth(s.summit);

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

        // Summit name fitted into the gap between ref and right value
        if (s.name[0]) {
            int usedL     = 8 + refW + 6;
            int usedR     = rw + 10;
            int available = SCREEN_W - usedL - usedR;
            if (available > 16) {
                spr.setTextColor(C(COL_DIM_WHITE));
                spr.setCursor(usedL, ry + 27);
                char nbuf[sizeof(s.name)];
                strlcpy(nbuf, s.name, sizeof(nbuf));
                int nlen = (int)strlen(nbuf);
                while (nlen > 0 && (int)spr.textWidth(nbuf) > available)
                    nbuf[--nlen] = '\0';
                if (nlen > 0) spr.print(nbuf);
            }
        }
    }

    // ── Source watermark ──────────────────────────────────────────────────────
    spr.setFont(UI_FONT_9);
    spr.setTextColor(C(0x444444UL));
    const char* src = "sota.org.uk";
    int sw = spr.textWidth(src);
    spr.setCursor(SCREEN_W - sw - 4, SCREEN_H - 11);
    spr.print(src);
}
