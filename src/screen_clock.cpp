#include "screen_clock.h"
#include "ui_common.h"
#include "config.h"
#include "settings.h"
#include <time.h>
#include <math.h>
#include "fonts/ui_fonts.h"

// ─── Moon graphic ─────────────────────────────────────────────────────────────
// 3-D sphere illumination.  phase 0=new, 0.25=first quarter,
// 0.5=full, 0.75=last quarter.
// Formula: lit when  nx*sin(2π·p) - nz*cos(2π·p) > 0
static void drawMoon(int cx, int cy, int R, float phase) {
    const float PI2 = 2.0f * (float)M_PI;
    const float sp  = sinf(PI2 * phase);
    const float cp  = cosf(PI2 * phase);
    float Rf = (float)R;
    for (int dy = -R; dy <= R; dy++) {
        for (int dx = -R; dx <= R; dx++) {
            float nx = (float)dx / Rf;
            float ny = (float)dy / Rf;
            float nz2 = 1.0f - nx*nx - ny*ny;
            if (nz2 < 0.0f) continue;          // outside circle
            float nz  = sqrtf(nz2);
            bool  lit = (nx * sp - nz * cp) > 0.0f;
            uint32_t col = lit
                ? C(0xE8E8D8UL)                // warm white lit surface
                : C(0x16182AUL);               // very dark blue-grey night side
            spr.drawPixel(cx + dx, cy + dy, col);
        }
    }
    // Faint outline so the disc shows against the background
    spr.drawCircle(cx, cy, R, C(0x3A3A4AUL));
}

// ─── Moon phase ───────────────────────────────────────────────────────────────
// Returns phase ∈ [0, 1).  0 = new, 0.5 = full.
static float moonPhase() {
    time_t now = time(nullptr);
    // Julian Day Number from Unix timestamp
    double jd = (double)now / 86400.0 + 2440587.5;
    // Reference new moon: JD 2451549.26 (6 Jan 2000 18:14 UTC)
    double phase = fmod((jd - 2451549.26) / 29.53059, 1.0);
    if (phase < 0.0) phase += 1.0;
    return (float)phase;
}

static const char* moonPhaseName(float p) {
    if (p < 0.025f || p >= 0.975f) return "New Moon";
    if (p < 0.235f)                 return "Waxing Crescent";
    if (p < 0.265f)                 return "First Quarter";
    if (p < 0.485f)                 return "Waxing Gibbous";
    if (p < 0.515f)                 return "Full Moon";
    if (p < 0.735f)                 return "Waning Gibbous";
    if (p < 0.765f)                 return "Last Quarter";
    return "Waning Crescent";
}

// ─── Sunrise / Sunset (USNO algorithm) ───────────────────────────────────────
// Returns true when both events exist (false near poles).
// riseMinUTC / setMinUTC are minutes after midnight UTC.
static bool calcSun(const struct tm& tm, float lat, float lon,
                    int& riseMinUTC, int& setMinUTC) {
    const float DEG = (float)M_PI / 180.0f;
    float lngHour   = lon / 15.0f;
    int   N         = tm.tm_yday + 1;          // day of year 1-based

    for (int pass = 0; pass < 2; pass++) {
        float approxT = (float)N + ((pass == 0 ? 6.0f : 18.0f) - lngHour) / 24.0f;

        float M = (0.9856f * approxT) - 3.289f;

        float L = M + (1.916f * sinf(M * DEG))
                    + (0.020f * sinf(2.0f * M * DEG)) + 282.634f;
        while (L < 0.0f)    L += 360.0f;
        while (L >= 360.0f) L -= 360.0f;

        float RA = (1.0f / DEG) * atanf(0.91764f * tanf(L * DEG));
        while (RA < 0.0f)    RA += 360.0f;
        while (RA >= 360.0f) RA -= 360.0f;
        RA += floorf(L  / 90.0f) * 90.0f
            - floorf(RA / 90.0f) * 90.0f;
        RA /= 15.0f;                            // RA in hours

        float sinDec = 0.39782f * sinf(L * DEG);
        float cosDec = cosf(asinf(sinDec));

        const float ZENITH = 90.833f;
        float cosH = (cosf(ZENITH * DEG) - (sinDec * sinf(lat * DEG)))
                   / (cosDec * cosf(lat * DEG));

        if (fabsf(cosH) > 1.0f) return false;  // polar: no rise or set

        // Sunrise uses 360-H; sunset uses H
        float H = (pass == 0)
            ? (360.0f - (1.0f / DEG) * acosf(cosH)) / 15.0f
            : (          (1.0f / DEG) * acosf(cosH)) / 15.0f;

        float T  = H + RA - (0.06571f * approxT) - 6.622f;
        float UT = T - lngHour;
        while (UT < 0.0f)    UT += 24.0f;
        while (UT >= 24.0f)  UT -= 24.0f;

        int minutes = (int)(UT * 60.0f + 0.5f);
        if (pass == 0) riseMinUTC = minutes;
        else           setMinUTC  = minutes;
    }
    return true;
}

// ─── Main draw function ───────────────────────────────────────────────────────
void drawScreenClock() {
    struct tm ti;
    if (!getLocalTime(&ti)) {
        drawLoader("Waiting for NTP...");
        return;
    }

    // Zone abbreviation (e.g. "EDT", "BST", "JST") — derived from the active
    // POSIX TZ string via the C library, so this works for any timezone the
    // user selects or that gets auto-detected from the grid locator.
    char zone[8];
    strftime(zone, sizeof(zone), "%Z", &ti);

    int y = CONTENT_Y + 4;

    // ── Zone badge ────────────────────────────────────────────────────────────
    char badge[32];
    snprintf(badge, sizeof(badge), "[ %s - LOCAL TIME ]", zone);
    spr.setFont(UI_FONT_9);
    spr.setTextColor(C(COL_ORANGE));
    int bw = spr.textWidth(badge);
    spr.setCursor((SCREEN_W - bw) / 2, y);
    spr.print(badge);
    y += 14;

    // ── HH:MM (DejaVu72) + :SS (DejaVu56) ────────────────────────────────────
    char hhmm[6], ssbuf[4];
    snprintf(hhmm, sizeof(hhmm), "%02d:%02d", ti.tm_hour, ti.tm_min);
    snprintf(ssbuf, sizeof(ssbuf), ":%02d", ti.tm_sec);

    spr.setFont(UI_FONT_72);
    int hmW = spr.textWidth(hhmm);
    spr.setFont(UI_FONT_56);
    int ssW = spr.textWidth(ssbuf);
    int startX = (SCREEN_W - (hmW + ssW)) / 2;

    spr.setFont(UI_FONT_72);
    spr.setTextColor(C(COL_WHITE));
    spr.setCursor(startX, y);
    spr.print(hhmm);

    spr.setFont(UI_FONT_56);
    spr.setTextColor(C(COL_GREY_L));
    spr.setCursor(startX + hmW, y + 15);
    spr.print(ssbuf);
    y += 82;

    // ── Date ──────────────────────────────────────────────────────────────────
    char datebuf[40];
    strftime(datebuf, sizeof(datebuf), "%A  %d %B %Y", &ti);
    spr.setFont(UI_FONT_12);
    spr.setTextColor(C(COL_GREY));
    int dw = spr.textWidth(datebuf);
    spr.setCursor((SCREEN_W - dw) / 2, y);
    spr.print(datebuf);
    y += 18;

    // ── Divider ───────────────────────────────────────────────────────────────
    spr.drawFastHLine(24, y, SCREEN_W - 48, C(COL_BORDER));
    y += 8;

    // ── Bottom section ────────────────────────────────────────────────────────
    // y is now ≈ 142.  Right column: moon graphic; left column: UTC/rise/set.

    const int botY = y;

    // ── Moon (right column) ───────────────────────────────────────────────────
    // Resolve the phase-name width first so the moon and label share the same
    // centre-x, with the label clamped to stay within the right screen edge.
    float phase = moonPhase();
    const char* pname = moonPhaseName(phase);
    spr.setFont(UI_FONT_9);
    int pnw = spr.textWidth(pname);

    // Ideal right-column centre; shift left if the label would overflow.
    int MCX    = SCREEN_W - 30;           // 290 when unconstrained
    int pnx    = MCX - pnw / 2;
    if (pnx + pnw > SCREEN_W - 3) pnx = SCREEN_W - pnw - 3;
    MCX = pnx + pnw / 2;                  // actual centre shared by moon + label

    const int MCY    = botY + 24;         // ≈166
    const int MOON_R = 20;
    drawMoon(MCX, MCY, MOON_R, phase);

    spr.setTextColor(C(COL_GREY));
    spr.setCursor(pnx, MCY + MOON_R + 4);
    spr.print(pname);

    // ── UTC time (left column) ────────────────────────────────────────────────
    time_t rawNow = time(nullptr);
    struct tm utcTm;
    gmtime_r(&rawNow, &utcTm);
    char utcbuf[6];
    snprintf(utcbuf, sizeof(utcbuf), "%02d:%02d", utcTm.tm_hour, utcTm.tm_min);

    int ly = botY + 4;

    // Shared time-value column — aligned to the widest label ("Sunrise")
    spr.setFont(UI_FONT_9);
    const int valX = 10 + (int)spr.textWidth("Sunrise") + 6;

    spr.setTextColor(C(COL_GREY));
    spr.setCursor(10, ly + 8);            // vertically centred alongside DejaVu18
    spr.print("UTC");

    spr.setFont(UI_FONT_18);
    spr.setTextColor(C(COL_GREY_L));
    spr.setCursor(valX, ly + 2);
    spr.print(utcbuf);
    ly += 28;                              // ly ≈ 174

    // ── Sunrise / Sunset ──────────────────────────────────────────────────────
    int riseMinUTC = 0, setMinUTC = 0;
    bool sunOk = calcSun(utcTm, g_settings.lat, g_settings.lon,
                         riseMinUTC, setMinUTC);

    // Derive UTC offset from the real local/UTC difference (handles all timezones)
    int utcOffMin = (ti.tm_hour - utcTm.tm_hour) * 60
                  + (ti.tm_min  - utcTm.tm_min);
    if (utcOffMin >  720) utcOffMin -= 1440;
    if (utcOffMin < -720) utcOffMin += 1440;

    if (sunOk) {
        int riseLoc = ((riseMinUTC + utcOffMin) % (24 * 60) + 24 * 60) % (24 * 60);
        int setLoc  = ((setMinUTC  + utcOffMin) % (24 * 60) + 24 * 60) % (24 * 60);

        char riseBuf[12], setBuf[12];
        snprintf(riseBuf, sizeof(riseBuf), "%02d:%02d %s", riseLoc / 60, riseLoc % 60, zone);
        snprintf(setBuf,  sizeof(setBuf),  "%02d:%02d %s", setLoc  / 60, setLoc  % 60, zone);

        // Rise row
        spr.setFont(UI_FONT_9);
        spr.setTextColor(C(COL_AMBER));
        spr.setCursor(10, ly + 4);
        spr.print("Sunrise");
        spr.setFont(UI_FONT_12);
        spr.setTextColor(C(COL_AMBER));
        spr.setCursor(valX, ly + 1);
        spr.print(riseBuf);
        ly += 22;                          // ly ≈ 196

        // Set row
        spr.setFont(UI_FONT_9);
        spr.setTextColor(C(COL_GREY));
        spr.setCursor(10, ly + 4);
        spr.print("Sunset");
        spr.setFont(UI_FONT_12);
        spr.setTextColor(C(COL_GREY_L));
        spr.setCursor(valX, ly + 1);
        spr.print(setBuf);
        ly += 22;                          // ly ≈ 218
    } else {
        // Polar day/night — no rise or set
        spr.setFont(UI_FONT_9);
        spr.setTextColor(C(COL_GREY));
        spr.setCursor(10, ly + 4);
        spr.print("No sunrise/sunset today");
        ly += 26;
    }

    // ── Timezone footnote ─────────────────────────────────────────────────────
    // Built from the configured timezone name + the live UTC offset, so it's
    // correct for whichever zone is selected (not just London).
    bool negOff  = utcOffMin < 0;
    int  absOff  = abs(utcOffMin);
    int  offH    = absOff / 60;
    int  offM    = absOff % 60;
    char offBuf[10];
    if (offM == 0) snprintf(offBuf, sizeof(offBuf), "UTC%s%d",     negOff ? "-" : "+", offH);
    else           snprintf(offBuf, sizeof(offBuf), "UTC%s%d:%02d", negOff ? "-" : "+", offH, offM);

    char note[48];
    snprintf(note, sizeof(note), "%s  -  %s", g_settings.tzName, offBuf);
    spr.setFont(UI_FONT_9);
    spr.setTextColor(C(COL_GREY));
    int nw = spr.textWidth(note);
    spr.setCursor((SCREEN_W - nw) / 2, ly + 2);
    spr.print(note);
}
