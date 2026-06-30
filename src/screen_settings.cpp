#include "screen_settings.h"
#include "settings.h"
#include "ui_common.h"
#include "config.h"
#include "data_store.h"
#include <cstring>
#include "fonts/ui_fonts.h"

// ─── Page IDs ─────────────────────────────────────────────────────────────────
enum SettingsPage : uint8_t {
    PAGE_MENU = 0,
    PAGE_LOCATION,
    PAGE_TIMEZONE,
    PAGE_SCREENS,
    PAGE_TRACKER,
    PAGE_CALLSIGN,
    PAGE_MODES,
    PAGE_CALIBRATE,
    PAGE_TZ_DETECT,
};

static SettingsPage g_page        = PAGE_MENU;
static int          g_tzScroll    = 0;      // timezone list first-visible row
static int          g_scrScroll   = 0;      // screens list first-visible row
static char         g_editGrid[8];          // working copy for location editor
static char         g_editCall[14];         // working copy for callsign editor
static bool         g_tzApplied   = false;  // true once an arrived tz-lookup result has been saved

// ─── Shared layout constants ──────────────────────────────────────────────────
static const int HDR_Y    = CONTENT_Y;      // 16
static const int HDR_H    = 26;
static const int BODY_Y   = HDR_Y + HDR_H;  // 42

// Location page
static const int LOC_COL_W  = SCREEN_W / 6; // 53 px per character column
static const int LOC_UP_Y   = BODY_Y + 4;   // ▲ button top  (46)
static const int LOC_BTN_H  = 28;
static const int LOC_CHAR_Y = LOC_UP_Y + LOC_BTN_H + 2; // char top (76)
static const int LOC_CHAR_H = 34;
static const int LOC_DN_Y   = LOC_CHAR_Y + LOC_CHAR_H;  // ▼ button top (110)
static const int LOC_INFO_Y = LOC_DN_Y + LOC_BTN_H + 8; // lat/lon text (146)
static const int LOC_DONE_Y = LOC_INFO_Y + 22;          // DONE button top (168)

// Timezone page
static const int TZ_ROW_H   = 22;

// Screens page
static const int SCR_ROW_H  = 24;

// Tracker page
static const int TRK_SYM_Y  = BODY_Y + 14;  // symbol section top
static const int TRK_BTN_H  = 30;
static const int TRK_BTN_W  = SCREEN_W / 4; // 80 px, 4 per row
static const int TRK_RNG_Y  = TRK_SYM_Y + 14 + 2 * (TRK_BTN_H + 4) + 12 + 14; // range row top

// ─── Drawing helpers ──────────────────────────────────────────────────────────

// Filled rounded button, label centred
static void drawBtn(int x, int y, int w, int h,
                    uint32_t bg, uint32_t border, uint32_t fg,
                    const char* label,
                    const lgfx::IFont* font = UI_FONT_9) {
    spr.fillRoundRect(x, y, w, h, 3, C(bg));
    spr.drawRoundRect(x, y, w, h, 3, C(border));
    spr.setFont(font);
    int tw = spr.textWidth(label);
    spr.setTextColor(C(fg));
    int tx = x + (w - tw) / 2;
    int ty = y + (h - 9) / 2;
    spr.setCursor(tx < x + 2 ? x + 2 : tx, ty);
    spr.print(label);
}

// Sub-page header with back arrow left, title centred
static void drawSubHeader(const char* title) {
    spr.fillRect(0, HDR_Y, SCREEN_W, HDR_H, C(0x1A1A1AUL));
    spr.drawFastHLine(0, HDR_Y + HDR_H - 1, SCREEN_W, C(COL_BORDER));
    spr.setFont(UI_FONT_12);
    spr.setTextColor(C(COL_GREY));
    spr.setCursor(6, HDR_Y + 7);
    spr.print("<");
    spr.setTextColor(C(COL_ORANGE));
    int tw = spr.textWidth(title);
    spr.setCursor((SCREEN_W - tw) / 2, HDR_Y + 7);
    spr.print(title);
}

// ─── PAGE_MENU ────────────────────────────────────────────────────────────────
static void drawMenu() {
    // Header
    spr.fillRect(0, HDR_Y, SCREEN_W, HDR_H, C(0x1A1A1AUL));
    spr.drawFastHLine(0, HDR_Y + HDR_H - 1, SCREEN_W, C(COL_BORDER));
    spr.setFont(UI_FONT_12);
    spr.setTextColor(C(COL_ORANGE));
    spr.setCursor(8, HDR_Y + 7);
    spr.print("SETTINGS");
    spr.setTextColor(C(COL_GREY));
    spr.setCursor(SCREEN_W - 22, HDR_Y + 7);
    spr.print("X");

    static const int rowH  = 28;
    static const int NROWS = 7;

    static const char* labels[NROWS] = {
        "Location", "Timezone", "Screens", "Tracker", "Callsign",
        "Mode Filter", "Touch Calibrate"
    };

    for (int i = 0; i < NROWS; i++) {
        int ry = BODY_Y + i * rowH;
        spr.drawFastHLine(0, ry, SCREEN_W, C(COL_BORDER));

        // Label
        spr.setFont(UI_FONT_12);
        spr.setTextColor(C(COL_WHITE));
        spr.setCursor(14, ry + 8);
        spr.print(labels[i]);

        // Arrow
        spr.setFont(UI_FONT_12);
        spr.setTextColor(C(COL_GREY));
        spr.setCursor(SCREEN_W - 18, ry + 8);
        spr.print(">");
    }
    spr.drawFastHLine(0, BODY_Y + NROWS * rowH, SCREEN_W, C(COL_BORDER));
}

// ─── PAGE_LOCATION ────────────────────────────────────────────────────────────
static void drawLocation() {
    drawSubHeader("LOCATION");

    // ▲ buttons
    for (int i = 0; i < 6; i++) {
        int bx = i * LOC_COL_W + (LOC_COL_W - 36) / 2;
        spr.fillRoundRect(bx, LOC_UP_Y, 36, LOC_BTN_H, 3, C(COL_PANEL));
        spr.drawRoundRect(bx, LOC_UP_Y, 36, LOC_BTN_H, 3, C(COL_BORDER));
        spr.setFont(UI_FONT_12);
        spr.setTextColor(C(COL_GREY_L));
        spr.setCursor(bx + 13, LOC_UP_Y + 8);
        spr.print("^");
    }

    // Characters (DejaVu24)
    for (int i = 0; i < 6; i++) {
        char ch[2] = { g_editGrid[i], 0 };
        spr.setFont(UI_FONT_24);
        int cw = spr.textWidth(ch);
        spr.setTextColor(C(COL_WHITE));
        spr.setCursor(i * LOC_COL_W + (LOC_COL_W - cw) / 2, LOC_CHAR_Y + 4);
        spr.print(ch);
    }

    // ▼ buttons
    for (int i = 0; i < 6; i++) {
        int bx = i * LOC_COL_W + (LOC_COL_W - 36) / 2;
        spr.fillRoundRect(bx, LOC_DN_Y, 36, LOC_BTN_H, 3, C(COL_PANEL));
        spr.drawRoundRect(bx, LOC_DN_Y, 36, LOC_BTN_H, 3, C(COL_BORDER));
        spr.setFont(UI_FONT_12);
        spr.setTextColor(C(COL_GREY_L));
        spr.setCursor(bx + 13, LOC_DN_Y + 8);
        spr.print("v");
    }

    // Derived lat/lon
    float la, lo;
    gridToLatLon(g_editGrid, la, lo);
    spr.setFont(UI_FONT_9);
    spr.setTextColor(C(COL_GREY));
    spr.setCursor(8, LOC_INFO_Y);
    spr.printf("Lat: %.4f    Lon: %.4f", la, lo);

    spr.drawFastHLine(8, LOC_INFO_Y + 14, SCREEN_W - 16, C(COL_BORDER));

    // DONE button
    drawBtn((SCREEN_W - 110) / 2, LOC_DONE_Y, 110, 28,
            COL_ORANGE_D, COL_ORANGE, COL_WHITE, "DONE");
}

// ─── PAGE_TIMEZONE ────────────────────────────────────────────────────────────
static void drawTimezone() {
    drawSubHeader("TIMEZONE");

    int y    = BODY_Y;
    int rows = (SCREEN_H - y) / TZ_ROW_H;

    for (int vi = 0; vi < rows; vi++) {
        int ti = g_tzScroll + vi;
        if (ti >= TZ_COUNT) break;
        int ry = y + vi * TZ_ROW_H;

        bool sel = (strcmp(TZ_LIST[ti].posix, g_settings.tz) == 0);
        if (sel) spr.fillRect(0, ry, SCREEN_W, TZ_ROW_H, C(0x0F1F0FUL));

        spr.fillCircle(10, ry + TZ_ROW_H / 2, 4,
                       C(sel ? COL_GREEN : COL_BORDER));

        spr.setFont(UI_FONT_9);
        spr.setTextColor(C(sel ? COL_WHITE : COL_GREY_L));
        spr.setCursor(22, ry + 6);
        spr.print(TZ_LIST[ti].name);

        spr.drawFastHLine(0, ry + TZ_ROW_H - 1, SCREEN_W, C(COL_BORDER));
    }

    // Scroll hints
    if (g_tzScroll > 0) {
        spr.setFont(UI_FONT_9);
        spr.setTextColor(C(COL_GREY));
        spr.setCursor(SCREEN_W - 12, BODY_Y + 2);
        spr.print("^");
    }
    if (g_tzScroll + rows < TZ_COUNT) {
        spr.setFont(UI_FONT_9);
        spr.setTextColor(C(COL_GREY));
        spr.setCursor(SCREEN_W - 12, SCREEN_H - 12);
        spr.print("v");
    }
}

// ─── PAGE_SCREENS ─────────────────────────────────────────────────────────────
static void drawScreens() {
    drawSubHeader("SCREENS");

    static const char* names[NUM_SCREENS] = {
        "Clock", "Weather", "HF Conditions", "Propagation",
        "Grey Line", "PSK Reporter",
        "DX Spots", "POTA Spots", "SOTA Spots", "Contests",
        "BBC News", "Apple News", "Tracker"
    };

    int visRows = (SCREEN_H - BODY_Y) / SCR_ROW_H;   // 8 rows visible at once

    for (int vi = 0; vi < visRows; vi++) {
        int si = g_scrScroll + vi;
        if (si >= NUM_SCREENS) break;

        int ry = BODY_Y + vi * SCR_ROW_H;
        spr.drawFastHLine(0, ry, SCREEN_W, C(COL_BORDER));
        spr.setFont(UI_FONT_9);
        // Screen number (dim) + name — makes it obvious the list is scrollable
        spr.setTextColor(C(COL_GREY));
        char numBuf[4];
        snprintf(numBuf, sizeof(numBuf), "%d.", si + 1);
        spr.setCursor(6, ry + 7);
        spr.print(numBuf);
        int numW = spr.textWidth(numBuf);
        spr.setTextColor(C(COL_GREY_L));
        spr.setCursor(6 + numW + 4, ry + 7);
        spr.print(names[si]);

        bool en = g_settings.screenEnabled[si];
        drawBtn(SCREEN_W - 58, ry + 3, 52, 18,
                en ? 0x0A280AUL : 0x280A0AUL,
                en ? COL_GREEN  : COL_RED,
                en ? COL_GREEN  : COL_RED,
                en ? "ON" : "OFF");
    }
    spr.drawFastHLine(0, BODY_Y + min(visRows, NUM_SCREENS) * SCR_ROW_H,
                      SCREEN_W, C(COL_BORDER));

    // Scroll hints
    if (g_scrScroll > 0) {
        spr.setFont(UI_FONT_9);
        spr.setTextColor(C(COL_GREY));
        spr.setCursor(SCREEN_W - 12, BODY_Y + 2);
        spr.print("^");
    }
    if (g_scrScroll + visRows < NUM_SCREENS) {
        spr.setFont(UI_FONT_9);
        spr.setTextColor(C(COL_GREY));
        spr.setCursor(SCREEN_W - 12, SCREEN_H - 12);
        spr.print("v");
    }
}

// ─── PAGE_TRACKER ─────────────────────────────────────────────────────────────
static void drawTracker() {
    drawSubHeader("TRACKER");

    // Symbol label
    spr.setFont(UI_FONT_9);
    spr.setTextColor(C(COL_GREY));
    spr.setCursor(8, BODY_Y + 4);
    spr.print("SYMBOL");

    // Symbol grid: 4 per row, 2 rows
    for (int i = 0; i < SYM_COUNT; i++) {
        int row = i / 4, col = i % 4;
        int bx = col * TRK_BTN_W + 2;
        int by = TRK_SYM_Y + row * (TRK_BTN_H + 4);
        bool sel = (strcmp(SYM_LIST[i].url, g_settings.trackerSymbol) == 0);
        drawBtn(bx, by, TRK_BTN_W - 4, TRK_BTN_H,
                sel ? COL_ORANGE_D : COL_PANEL,
                sel ? COL_ORANGE   : COL_BORDER,
                sel ? COL_WHITE    : COL_GREY_L,
                SYM_LIST[i].name);
    }

    // Range label
    spr.setFont(UI_FONT_9);
    spr.setTextColor(C(COL_GREY));
    spr.setCursor(8, TRK_RNG_Y - 14);
    spr.print("RANGE");

    // Range row: 5 buttons
    const int rngW = SCREEN_W / 5;
    static const char* rngLbls[] = { "1 Year", "2 Years", "3 Years", "4 Years", "5 Years" };
    for (int i = 0; i < 5; i++) {
        bool sel = (g_settings.trackerRangeYears == i + 1);
        drawBtn(i * rngW + 2, TRK_RNG_Y, rngW - 4, TRK_BTN_H,
                sel ? COL_ORANGE_D : COL_PANEL,
                sel ? COL_ORANGE   : COL_BORDER,
                sel ? COL_WHITE    : COL_GREY_L,
                rngLbls[i]);
    }
}

// ─── PAGE_CALLSIGN ────────────────────────────────────────────────────────────
// Reuses the same 6-column ▲/char/▼ layout as the location editor.
// Characters cycle through: space, A-Z, 0-9, / (38 options total).
// The callsign is stored upper-case; trailing spaces are stripped on DONE.
static void drawCallsign() {
    drawSubHeader("CALLSIGN");

    // ▲ buttons
    for (int i = 0; i < 6; i++) {
        int bx = i * LOC_COL_W + (LOC_COL_W - 36) / 2;
        spr.fillRoundRect(bx, LOC_UP_Y, 36, LOC_BTN_H, 3, C(COL_PANEL));
        spr.drawRoundRect(bx, LOC_UP_Y, 36, LOC_BTN_H, 3, C(COL_BORDER));
        spr.setFont(UI_FONT_12);
        spr.setTextColor(C(COL_GREY_L));
        spr.setCursor(bx + 13, LOC_UP_Y + 8);
        spr.print("^");
    }

    // Characters
    for (int i = 0; i < 6; i++) {
        char ch[2] = { g_editCall[i] ? g_editCall[i] : ' ', 0 };
        spr.setFont(UI_FONT_24);
        int cw = spr.textWidth(ch);
        spr.setTextColor(C(COL_WHITE));
        spr.setCursor(i * LOC_COL_W + (LOC_COL_W - cw) / 2, LOC_CHAR_Y + 4);
        spr.print(ch);
    }

    // ▼ buttons
    for (int i = 0; i < 6; i++) {
        int bx = i * LOC_COL_W + (LOC_COL_W - 36) / 2;
        spr.fillRoundRect(bx, LOC_DN_Y, 36, LOC_BTN_H, 3, C(COL_PANEL));
        spr.drawRoundRect(bx, LOC_DN_Y, 36, LOC_BTN_H, 3, C(COL_BORDER));
        spr.setFont(UI_FONT_12);
        spr.setTextColor(C(COL_GREY_L));
        spr.setCursor(bx + 13, LOC_DN_Y + 8);
        spr.print("v");
    }

    // Help hint
    spr.setFont(UI_FONT_9);
    spr.setTextColor(C(COL_GREY));
    spr.setCursor(8, LOC_INFO_Y);
    spr.print("Tap ^ / v to change each letter");
    spr.drawFastHLine(8, LOC_INFO_Y + 14, SCREEN_W - 16, C(COL_BORDER));

    // DONE button
    drawBtn((SCREEN_W - 110) / 2, LOC_DONE_Y, 110, 28,
            COL_ORANGE_D, COL_ORANGE, COL_WHITE, "DONE");
}

// Cycle a callsign character: space → A-Z → 0-9 → / → space
static void cycleCallChar(char* cs, int pos, int dir) {
    // Charset: ' '(0), A-Z(1-26), 0-9(27-36), '/'(37) — 38 entries
    static const char CALL_CHARS[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/";
    const int N = (int)(sizeof(CALL_CHARS) - 1);
    char c = cs[pos] ? cs[pos] : ' ';
    int idx = 0;
    for (int i = 0; i < N; i++) if (CALL_CHARS[i] == c) { idx = i; break; }
    idx = (idx + dir % N + N) % N;
    cs[pos] = CALL_CHARS[idx];
}

// ─── PAGE_MODES ──────────────────────────────────────────────────────────────
static const int MODE_NROWS = 6;
static const uint8_t MODE_BITS[MODE_NROWS] = {
    SPOT_MODE_CW, SPOT_MODE_SSB, SPOT_MODE_FT8, SPOT_MODE_FT4, SPOT_MODE_RTTY, SPOT_MODE_OTHER
};
static const char* MODE_NAMES[MODE_NROWS] = {
    "CW", "Voice (SSB/FM/AM)", "FT8", "FT4", "Digital (RTTY/PSK)", "Other"
};

static void drawModes() {
    drawSubHeader("MODE FILTER");

    static const int ROW_H = 28;
    for (int i = 0; i < MODE_NROWS; i++) {
        int ry = BODY_Y + i * ROW_H;
        spr.drawFastHLine(0, ry, SCREEN_W, C(COL_BORDER));
        spr.setFont(UI_FONT_9);
        spr.setTextColor(C(COL_GREY_L));
        spr.setCursor(14, ry + 9);
        spr.print(MODE_NAMES[i]);

        bool en = g_settings.modeFilter & MODE_BITS[i];
        drawBtn(SCREEN_W - 58, ry + 5, 52, 18,
                en ? 0x0A280AUL : 0x280A0AUL,
                en ? COL_GREEN  : COL_RED,
                en ? COL_GREEN  : COL_RED,
                en ? "ON" : "OFF");
    }
    spr.drawFastHLine(0, BODY_Y + MODE_NROWS * ROW_H, SCREEN_W, C(COL_BORDER));
}

static void runCalibration() {
    // Clear any existing calibration so the crosshairs use raw defaults
    g_settings.touchCalValid = false;
    memset(g_settings.touchCal, 0, sizeof(g_settings.touchCal));
    settingsSave();

    tft.fillScreen(TFT_BLACK);
    tft.setFont(UI_FONT_12);
    tft.setTextColor(tft.color888(0xFF, 0x6B, 0x00));
    const char* msg = "Touch each crosshair";
    int mw = tft.textWidth(msg);
    tft.setCursor((SCREEN_W - mw) / 2, SCREEN_H / 2 - 8);
    tft.print(msg);
    delay(1500);

    uint16_t cal[8];
    tft.calibrateTouch(cal, TFT_WHITE, TFT_BLACK, 15);

    memcpy(g_settings.touchCal, cal, sizeof(cal));
    g_settings.touchCalValid = true;
    tft.setTouchCalibrate(g_settings.touchCal);
    settingsSave();

    tft.fillScreen(TFT_BLACK);
    tft.setFont(UI_FONT_12);
    tft.setTextColor(tft.color888(0x4C, 0xAF, 0x50));
    const char* ok = "Calibration saved!";
    int ow = tft.textWidth(ok);
    tft.setCursor((SCREEN_W - ow) / 2, SCREEN_H / 2 - 8);
    tft.print(ok);
    delay(1000);

    g_page = PAGE_MENU;
}

// ─── PAGE_TZ_DETECT ───────────────────────────────────────────────────────────
static void drawTzDetect() {
    drawSubHeader("TIMEZONE");

    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    TzLookupResult r = g_tzLookup;
    xSemaphoreGive(g_dataMutex);

    const int cy = BODY_Y + (SCREEN_H - BODY_Y) / 2 - 8;

    if (!r.valid) {
        char msg[28];
        int dots = (millis() / 400) % 4;
        snprintf(msg, sizeof(msg), "Detecting timezone%.*s", dots, "...");
        spr.setFont(UI_FONT_12);
        spr.setTextColor(C(COL_AMBER));
        int fullW = spr.textWidth("Detecting timezone...");
        spr.setCursor((SCREEN_W - fullW) / 2, cy);
        spr.print(msg);
    } else if (r.failed) {
        spr.setFont(UI_FONT_12);
        spr.setTextColor(C(COL_RED));
        const char* msg = "Could not detect timezone";
        int mw = spr.textWidth(msg);
        spr.setCursor((SCREEN_W - mw) / 2, cy - 10);
        spr.print(msg);

        spr.setFont(UI_FONT_9);
        spr.setTextColor(C(COL_GREY));
        const char* hint = "Set it manually, or tap to continue";
        int hw = spr.textWidth(hint);
        spr.setCursor((SCREEN_W - hw) / 2, cy + 14);
        spr.print(hint);
    } else {
        if (!g_tzApplied) {
            strlcpy(g_settings.tz,     r.tzPosix, sizeof(g_settings.tz));
            strlcpy(g_settings.tzName, r.tzName,  sizeof(g_settings.tzName));
            settingsSave();
            g_tzApplied = true;
        }

        spr.setFont(UI_FONT_12);
        spr.setTextColor(C(COL_GREEN));
        const char* msg = "Timezone set:";
        int mw = spr.textWidth(msg);
        spr.setCursor((SCREEN_W - mw) / 2, cy - 10);
        spr.print(msg);

        spr.setTextColor(C(COL_WHITE));
        int nw = spr.textWidth(r.tzName);
        spr.setCursor((SCREEN_W - nw) / 2, cy + 10);
        spr.print(r.tzName);
    }
}

// ─── Public: draw ─────────────────────────────────────────────────────────────
void drawScreenSettings() {
    if (g_page == PAGE_CALIBRATE) {
        runCalibration();
        return;
    }
    switch (g_page) {
        case PAGE_MENU:       drawMenu();      break;
        case PAGE_LOCATION:   drawLocation();  break;
        case PAGE_TIMEZONE:   drawTimezone();  break;
        case PAGE_SCREENS:    drawScreens();   break;
        case PAGE_TRACKER:    drawTracker();   break;
        case PAGE_CALLSIGN:   drawCallsign();  break;
        case PAGE_MODES:      drawModes();     break;
        case PAGE_TZ_DETECT:  drawTzDetect();  break;
        default: break;
    }
}

// ─── Public: enter ────────────────────────────────────────────────────────────
void settingsEnter() {
    g_page     = PAGE_MENU;
    g_scrScroll = 0;
    strlcpy(g_editGrid, g_settings.grid, sizeof(g_editGrid));

    // Pad callsign to 6 chars with spaces for the picker
    memset(g_editCall, ' ', 6);
    g_editCall[6] = '\0';
    int clen = (int)strlen(g_settings.callsign);
    if (clen > 6) clen = 6;
    for (int i = 0; i < clen; i++) g_editCall[i] = g_settings.callsign[i];

    // Scroll to current timezone
    g_tzScroll = 0;
    for (int i = 0; i < TZ_COUNT; i++) {
        if (strcmp(TZ_LIST[i].posix, g_settings.tz) == 0) {
            g_tzScroll = max(0, i - 3);
            break;
        }
    }
}

// ─── Grid char cycling ────────────────────────────────────────────────────────
static void cycleChar(char* grid, int pos, int dir) {
    char c = grid[pos];
    if (pos < 2) {
        // Field letters A-R (18)
        c = 'A' + (char)((c - 'A' + dir + 18) % 18);
    } else if (pos < 4) {
        // Digit 0-9
        c = '0' + (char)((c - '0' + dir + 10) % 10);
    } else {
        // Subsquare letters a-x (24)
        c = 'a' + (char)((c - 'a' + dir + 24) % 24);
    }
    grid[pos] = c;
}

// ─── Public: touch ────────────────────────────────────────────────────────────
bool settingsTouchUp(int32_t sx, int32_t sy,
                     int32_t ex, int32_t ey,
                     uint32_t dtMs) {
    bool isTap   = (abs(ex - sx) < 22 && abs(ey - sy) < 22 && dtMs < 500);
    bool isSwipe = (abs(ey - sy) > 28 && dtMs < SWIPE_MAX_MS);

    switch (g_page) {

    // ── MENU ──────────────────────────────────────────────────────────────────
    case PAGE_MENU:
        if (!isTap) break;
        // Header: close button
        if (sy < BODY_Y) {
            if (sx > SCREEN_W - 34) return true;   // X tapped
            break;
        }
        {
            int row = (sy - BODY_Y) / 28;
            if (row >= 0 && row < 7) {
                strlcpy(g_editGrid, g_settings.grid, sizeof(g_editGrid));
                switch (row) {
                    case 0: g_page = PAGE_LOCATION; break;
                    case 1: g_page = PAGE_TIMEZONE; break;
                    case 2: g_page = PAGE_SCREENS;  g_scrScroll = 0; break;
                    case 3: g_page = PAGE_TRACKER;  break;
                    case 4:
                        memset(g_editCall, ' ', 6); g_editCall[6] = '\0';
                        { int cl = (int)strlen(g_settings.callsign); if (cl > 6) cl = 6;
                          for (int i = 0; i < cl; i++) g_editCall[i] = g_settings.callsign[i]; }
                        g_page = PAGE_CALLSIGN;
                        break;
                    case 5: g_page = PAGE_MODES; break;
                    case 6: g_page = PAGE_CALIBRATE; break;
                }
            }
        }
        break;

    // ── LOCATION ──────────────────────────────────────────────────────────────
    case PAGE_LOCATION:
        if (isTap && sy < BODY_Y && sx < 40) {
            // Back — discard changes
            g_page = PAGE_MENU;
            break;
        }
        if (!isTap) break;
        {
            int col = sx / LOC_COL_W;
            if (col < 0) col = 0;
            if (col > 5) col = 5;

            // DONE button
            if (sy >= LOC_DONE_Y && sy < LOC_DONE_Y + 28) {
                strlcpy(g_settings.grid, g_editGrid, sizeof(g_settings.grid));
                gridToLatLon(g_settings.grid, g_settings.lat, g_settings.lon);
                settingsSave();

                // Location changed — reset weather so the old location's data
                // doesn't linger; the loader shows until the fresh fetch lands.
                xSemaphoreTake(g_dataMutex, portMAX_DELAY);
                g_weather.valid = false;
                xSemaphoreGive(g_dataMutex);
                g_forceFetchWeather = true;

                // Kick off async timezone auto-detect from the new location
                g_tzLookup.valid  = false;
                g_tzLookup.failed = false;
                g_tzLookupLat = g_settings.lat;
                g_tzLookupLon = g_settings.lon;
                g_tzLookupReq = true;
                g_tzApplied   = false;
                g_page = PAGE_TZ_DETECT;
                break;
            }
            // ▲ row
            if (sy >= LOC_UP_Y && sy < LOC_UP_Y + LOC_BTN_H) {
                cycleChar(g_editGrid, col, +1);
            }
            // ▼ row
            else if (sy >= LOC_DN_Y && sy < LOC_DN_Y + LOC_BTN_H) {
                cycleChar(g_editGrid, col, -1);
            }
        }
        break;

    // ── TZ AUTO-DETECT (result page) ────────────────────────────────────────────
    case PAGE_TZ_DETECT:
        if (!isTap) break;
        if (g_tzLookup.valid) g_page = PAGE_MENU;   // tap anywhere to dismiss once ready
        break;

    // ── TIMEZONE ──────────────────────────────────────────────────────────────
    case PAGE_TIMEZONE:
        if (isSwipe) {
            int visRows = (SCREEN_H - BODY_Y) / TZ_ROW_H;
            int maxScroll = TZ_COUNT - visRows;
            if (maxScroll < 0) maxScroll = 0;
            if (ey < sy) g_tzScroll = min(g_tzScroll + 1, maxScroll);
            else         g_tzScroll = max(g_tzScroll - 1, 0);
            break;
        }
        if (!isTap) break;
        if (sy < BODY_Y && sx < 40) { g_page = PAGE_MENU; break; }
        {
            int vi = (sy - BODY_Y) / TZ_ROW_H;
            int ti = g_tzScroll + vi;
            if (ti >= 0 && ti < TZ_COUNT) {
                strlcpy(g_settings.tz,     TZ_LIST[ti].posix, sizeof(g_settings.tz));
                strlcpy(g_settings.tzName, TZ_LIST[ti].name,  sizeof(g_settings.tzName));
                settingsSave();
            }
        }
        break;

    // ── SCREENS ───────────────────────────────────────────────────────────────
    case PAGE_SCREENS: {
        int visRows   = (SCREEN_H - BODY_Y) / SCR_ROW_H;
        int maxScroll = NUM_SCREENS - visRows;
        if (maxScroll < 0) maxScroll = 0;

        if (isSwipe) {
            if (ey < sy) g_scrScroll = min(g_scrScroll + 1, maxScroll);
            else         g_scrScroll = max(g_scrScroll - 1, 0);
            break;
        }
        if (!isTap) break;
        if (sy < BODY_Y && sx < 40) { g_page = PAGE_MENU; break; }
        {
            int vi  = (sy - BODY_Y) / SCR_ROW_H;   // visible row index
            int si  = g_scrScroll + vi;             // actual screen index
            if (vi >= 0 && vi < visRows && si < NUM_SCREENS && sx >= SCREEN_W - 58) {
                bool next = !g_settings.screenEnabled[si];
                // Must keep at least one screen active
                if (!next) {
                    int active = 0;
                    for (int i = 0; i < NUM_SCREENS; i++)
                        if (g_settings.screenEnabled[i]) active++;
                    if (active <= 1) break;   // can't disable last screen
                }
                g_settings.screenEnabled[si] = next;
                settingsSave();
            }
        }
        break;
    }

    // ── CALLSIGN ──────────────────────────────────────────────────────────────
    case PAGE_CALLSIGN:
        if (isTap && sy < BODY_Y && sx < 40) {
            g_page = PAGE_MENU;
            break;
        }
        if (!isTap) break;
        {
            // DONE button
            if (sy >= LOC_DONE_Y && sy < LOC_DONE_Y + 28) {
                // Copy to settings, trimming trailing spaces
                int last = 5;
                while (last > 0 && g_editCall[last] == ' ') last--;
                char trimmed[14] = {};
                for (int i = 0; i <= last; i++) trimmed[i] = g_editCall[i];
                trimmed[last + 1] = '\0';
                strlcpy(g_settings.callsign, trimmed, sizeof(g_settings.callsign));
                settingsSave();
                g_page = PAGE_MENU;
                break;
            }
            int col = sx / LOC_COL_W;
            if (col < 0) col = 0;
            if (col > 5) col = 5;
            // ▲ row
            if (sy >= LOC_UP_Y && sy < LOC_UP_Y + LOC_BTN_H)
                cycleCallChar(g_editCall, col, +1);
            // ▼ row
            else if (sy >= LOC_DN_Y && sy < LOC_DN_Y + LOC_BTN_H)
                cycleCallChar(g_editCall, col, -1);
        }
        break;

    // ── TRACKER ───────────────────────────────────────────────────────────────
    case PAGE_TRACKER:
        if (!isTap) break;
        if (sy < BODY_Y && sx < 40) { g_page = PAGE_MENU; break; }
        // Symbol buttons
        for (int i = 0; i < SYM_COUNT; i++) {
            int row = i / 4, col = i % 4;
            int bx = col * TRK_BTN_W;
            int by = TRK_SYM_Y + row * (TRK_BTN_H + 4);
            if (sx >= bx && sx < bx + TRK_BTN_W &&
                sy >= by && sy < by + TRK_BTN_H) {
                strlcpy(g_settings.trackerSymbol, SYM_LIST[i].url,  sizeof(g_settings.trackerSymbol));
                strlcpy(g_settings.trackerName,   SYM_LIST[i].name, sizeof(g_settings.trackerName));
                settingsSave();
                break;
            }
        }
        // Range buttons
        if (sy >= TRK_RNG_Y && sy < TRK_RNG_Y + TRK_BTN_H) {
            int col = sx * 5 / SCREEN_W;
            if (col >= 0 && col < 5) {
                g_settings.trackerRangeYears = (uint8_t)(col + 1);
                settingsSave();
            }
        }
        break;

    // ── MODES ────────────────────────────────────────────────────────────────
    case PAGE_MODES:
        if (!isTap) break;
        if (sy < BODY_Y && sx < 40) { g_page = PAGE_MENU; break; }
        {
            int row = (sy - BODY_Y) / 28;
            if (row >= 0 && row < MODE_NROWS && sx >= SCREEN_W - 58) {
                g_settings.modeFilter ^= MODE_BITS[row];
                settingsSave();
            }
        }
        break;
    }

    return false;
}
