#include "screen_settings.h"
#include "settings.h"
#include "ui_common.h"
#include "ui.h"
#include "config.h"
#include "data_store.h"
#include "service.h"
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
    PAGE_BRIGHTNESS,
    PAGE_ISSFREQ,        // list of the 4 ISS frequencies
    PAGE_ISSFREQ_EDIT,   // digit-spinner editor for one frequency
    PAGE_SERVICE,        // screenshot + fake toggles (for documentation)
};

static SettingsPage g_page        = PAGE_MENU;
static int          g_menuScroll  = 0;      // menu first-visible row
static int          g_tzScroll    = 0;      // timezone list first-visible row
static int          g_scrScroll   = 0;      // screens list first-visible row
static char         g_editGrid[8];          // working copy for location editor
static char         g_editCall[14];         // working copy for callsign editor
static bool         g_tzApplied   = false;  // true once an arrived tz-lookup result has been saved
static int          g_editFreqSel = 0;      // which ISS frequency is being edited (0-3)
static uint8_t      g_editDigits[6];         // working digits DDD.DDD for the freq editor

// ─── Timezone display order — alphabetical, decoupled from TZ_LIST's own
// index order (which TZ_ALIASES and tzFindByIana() depend on staying fixed) ──
static int  g_tzOrder[TZ_COUNT];
static bool g_tzOrderReady = false;

/**
 * @brief Build the alphabetical timezone display order once (lazy, cached).
 *
 * Populates g_tzOrder with TZ_LIST indices sorted by name, keeping TZ_LIST's
 * own index order fixed (aliases and tzFindByIana() depend on it).
 */
static void ensureTzOrder() {
    if (g_tzOrderReady) return;
    for (int i = 0; i < TZ_COUNT; i++) g_tzOrder[i] = i;
    for (int i = 1; i < TZ_COUNT; i++) {
        int key = g_tzOrder[i];
        int j = i - 1;
        while (j >= 0 && strcasecmp(TZ_LIST[g_tzOrder[j]].name, TZ_LIST[key].name) > 0) {
            g_tzOrder[j + 1] = g_tzOrder[j];
            j--;
        }
        g_tzOrder[j + 1] = key;
    }
    g_tzOrderReady = true;
}

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

/**
 * @brief Draw a filled rounded button with a centred label.
 * @param x,y,w,h  Button rectangle in screen pixels.
 * @param bg       Fill colour (RGB888).
 * @param border   Border colour (RGB888).
 * @param fg       Label text colour (RGB888).
 * @param label    Button caption.
 * @param font     Label font; defaults to UI_FONT_9.
 */
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

/**
 * @brief Draw a sub-page header bar: a back arrow at the left, title centred.
 * @param title  Sub-page title text.
 */
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
static const int MNU_SB_W    = 22;
static const int MNU_ARROW_H = 22;
static const int MNU_LIST_W  = SCREEN_W - MNU_SB_W;
static const int MNU_ROW_H   = 28;
static const int MNU_NROWS   = 16;  // 10 sub-page rows + 6 inline toggles

/**
 * @brief Draw the main settings menu: a scrollable list of 16 alphabetical rows
 *        (inline toggles and sub-page links) with a draggable sidebar scrollbar.
 */
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

    int visRows  = (SCREEN_H - BODY_Y) / MNU_ROW_H;
    int maxScroll = max(0, MNU_NROWS - visRows);
    if (g_menuScroll > maxScroll) g_menuScroll = maxScroll;

    // Live drag on sidebar track
    if (touchIsActive() && maxScroll > 0) {
        int32_t tx = touchCurrentX();
        int32_t ty = touchCurrentY();
        if (tx >= MNU_LIST_W) {
            int trackY0 = BODY_Y + MNU_ARROW_H + 3;
            int trackY1 = SCREEN_H - MNU_ARROW_H - 3;
            if (ty >= trackY0 && ty <= trackY1 && trackY1 > trackY0) {
                float frac = (float)(ty - trackY0) / (float)(trackY1 - trackY0);
                g_menuScroll = (int)(constrain(frac, 0.0f, 1.0f) * maxScroll + 0.5f);
            }
        }
    }

    // Alphabetical row order (16 rows):
    //  0 Brightness  1 Callsign  2 Distance Units  3 ISS Frequencies
    //  4 ISS Pass Alert  5 ISS Pass LED  6 Lightning LED  7 Location
    //  8 Mode Filter  9 Screens  10 Service  11 Startup LED
    //  12 Stocks Tracker  13 Temperature Units  14 Timezone  15 Touch Calibrate
    // Rows 2, 4, 5, 6, 11, 13 are inline toggles; the rest navigate to a sub-page.
    // Sub-page rows 0,1,3,7,8,9,10,12,14,15 map to labels[] below.
    static const char* labels[10] = {
        "Brightness", "Callsign", "ISS Frequencies", "Location",
        "Mode Filter", "Screens", "Service", "Stocks Tracker",
        "Timezone", "Touch Calibrate"
    };

    // Two-option selector, right-justified and sized to match the ON/OFF toggle
    // buttons (left 32 px, right 36 px). Selected option is green; labels centred.
    // left button highlighted green when leftSel==true, right highlighted otherwise
    auto drawSelector = [&](int ry, const char* leftLbl, const char* rightLbl, bool leftSel) {
        const int LX = MNU_LIST_W - 76, LW = 32;   // matches the "ON" button
        const int RX = MNU_LIST_W - 40, RW = 36;   // matches the "OFF" button
        spr.setTextColor(C(COL_WHITE));
        spr.fillRoundRect(LX, ry + 5, LW, 18, 3, leftSel ? C(COL_GREEN) : C(COL_BORDER));
        spr.drawRoundRect(LX, ry + 5, LW, 18, 3, C(COL_GREY));
        spr.setCursor(LX + (LW - spr.textWidth(leftLbl)) / 2, ry + 8);
        spr.print(leftLbl);
        spr.fillRoundRect(RX, ry + 5, RW, 18, 3, leftSel ? C(COL_BORDER) : C(COL_GREEN));
        spr.drawRoundRect(RX, ry + 5, RW, 18, 3, C(COL_GREY));
        spr.setCursor(RX + (RW - spr.textWidth(rightLbl)) / 2, ry + 8);
        spr.print(rightLbl);
    };

    for (int vi = 0; vi < visRows; vi++) {
        int ri = g_menuScroll + vi;
        if (ri >= MNU_NROWS) break;
        int ry = BODY_Y + vi * MNU_ROW_H;
        spr.drawFastHLine(0, ry, MNU_LIST_W, C(COL_BORDER));
        spr.setFont(UI_FONT_12);

        if (ri == 2) {
            // Distance Units: [mi] [km]
            spr.setTextColor(C(COL_WHITE));
            spr.setCursor(14, ry + 8);
            spr.print("Distance Units");
            drawSelector(ry, "mi", "km", !g_settings.useKm);

        } else if (ri == 4) {
            // ISS Pass Alert — greyed out when Grey Line screen is disabled
            bool greyAvail = g_settings.screenEnabled[SCR_GREYLINE];
            bool on        = greyAvail && g_settings.issJumpEnabled;
            spr.setTextColor(greyAvail ? C(COL_WHITE) : C(COL_GREY));
            spr.setCursor(14, ry + 8);
            spr.print("ISS Pass Alert");
            if (greyAvail) {
                spr.fillRoundRect(MNU_LIST_W - 76, ry + 5, 32, 18, 3, on ? C(COL_GREEN)  : C(COL_BORDER));
                spr.drawRoundRect(MNU_LIST_W - 76, ry + 5, 32, 18, 3, C(COL_GREY));
                spr.setTextColor(C(COL_WHITE));
                spr.setCursor(MNU_LIST_W - 68, ry + 8);
                spr.print("ON");
                spr.fillRoundRect(MNU_LIST_W - 40, ry + 5, 36, 18, 3, on ? C(COL_BORDER) : C(COL_RED));
                spr.drawRoundRect(MNU_LIST_W - 40, ry + 5, 36, 18, 3, C(COL_GREY));
                spr.setCursor(MNU_LIST_W - 34, ry + 8);
                spr.print("OFF");
            } else {
                spr.setTextColor(C(COL_GREY));
                spr.setCursor(150, ry + 8);
                spr.print("(ISS Tracker off)");
            }

        } else if (ri == 5) {
            // ISS Pass LED ON/OFF — flashes the onboard green LED while the ISS is
            // above the horizon (a radio pass). Works independently of the Grey
            // Line screen.
            bool on = g_settings.issLedEnabled;
            spr.setTextColor(C(COL_WHITE));
            spr.setCursor(14, ry + 8);
            spr.print("ISS Pass LED");
            spr.fillRoundRect(MNU_LIST_W - 76, ry + 5, 32, 18, 3, on ? C(COL_GREEN)  : C(COL_BORDER));
            spr.drawRoundRect(MNU_LIST_W - 76, ry + 5, 32, 18, 3, C(COL_GREY));
            spr.setTextColor(C(COL_WHITE));
            spr.setCursor(MNU_LIST_W - 68, ry + 8);
            spr.print("ON");
            spr.fillRoundRect(MNU_LIST_W - 40, ry + 5, 36, 18, 3, on ? C(COL_BORDER) : C(COL_RED));
            spr.drawRoundRect(MNU_LIST_W - 40, ry + 5, 36, 18, 3, C(COL_GREY));
            spr.setCursor(MNU_LIST_W - 34, ry + 8);
            spr.print("OFF");

        } else if (ri == 6) {
            // Lightning LED ON/OFF — flashes the onboard red LED on a lightning alert
            bool on = g_settings.lightningLedEnabled;
            spr.setTextColor(C(COL_WHITE));
            spr.setCursor(14, ry + 8);
            spr.print("Lightning LED");
            spr.fillRoundRect(MNU_LIST_W - 76, ry + 5, 32, 18, 3, on ? C(COL_GREEN)  : C(COL_BORDER));
            spr.drawRoundRect(MNU_LIST_W - 76, ry + 5, 32, 18, 3, C(COL_GREY));
            spr.setTextColor(C(COL_WHITE));
            spr.setCursor(MNU_LIST_W - 68, ry + 8);
            spr.print("ON");
            spr.fillRoundRect(MNU_LIST_W - 40, ry + 5, 36, 18, 3, on ? C(COL_BORDER) : C(COL_RED));
            spr.drawRoundRect(MNU_LIST_W - 40, ry + 5, 36, 18, 3, C(COL_GREY));
            spr.setCursor(MNU_LIST_W - 34, ry + 8);
            spr.print("OFF");

        } else if (ri == 11) {
            // Startup LED ON/OFF — flashes the onboard blue LED while data loads
            bool on = g_settings.startupLedEnabled;
            spr.setTextColor(C(COL_WHITE));
            spr.setCursor(14, ry + 8);
            spr.print("Startup LED");
            spr.fillRoundRect(MNU_LIST_W - 76, ry + 5, 32, 18, 3, on ? C(COL_GREEN)  : C(COL_BORDER));
            spr.drawRoundRect(MNU_LIST_W - 76, ry + 5, 32, 18, 3, C(COL_GREY));
            spr.setTextColor(C(COL_WHITE));
            spr.setCursor(MNU_LIST_W - 68, ry + 8);
            spr.print("ON");
            spr.fillRoundRect(MNU_LIST_W - 40, ry + 5, 36, 18, 3, on ? C(COL_BORDER) : C(COL_RED));
            spr.drawRoundRect(MNU_LIST_W - 40, ry + 5, 36, 18, 3, C(COL_GREY));
            spr.setCursor(MNU_LIST_W - 34, ry + 8);
            spr.print("OFF");

        } else if (ri == 13) {
            // Temperature Units: [°C] [°F]
            spr.setTextColor(C(COL_WHITE));
            spr.setCursor(14, ry + 8);
            spr.print("Temperature");
            drawSelector(ry, "C", "F", g_settings.useCelsius);

        } else {
            // Sub-page navigation row. Sub-page rows: 0,1,3,7,8,9,10,12,14,15.
            int li = ri < 2  ? ri
                   : ri == 3  ? 2
                   : ri < 11  ? ri - 4    // 7,8,9,10 → 3,4,5,6
                   : ri == 12 ? 7         // Stocks Tracker
                              : ri - 6;   // 14,15 → 8,9
            spr.setTextColor(C(COL_WHITE));
            spr.setCursor(14, ry + 8);
            spr.print(labels[li]);
            spr.setTextColor(C(COL_GREY));
            spr.setCursor(MNU_LIST_W - 18, ry + 8);
            spr.print(">");
        }
    }
    spr.drawFastHLine(0, BODY_Y + min(visRows, MNU_NROWS) * MNU_ROW_H, MNU_LIST_W, C(COL_BORDER));

    // ── Right-hand sidebar ────────────────────────────────────────────────────
    const int sbX    = MNU_LIST_W;
    const int upY    = BODY_Y;
    const int dnY    = SCREEN_H - MNU_ARROW_H;
    const int trackY0 = upY + MNU_ARROW_H + 3;
    const int trackY1 = dnY - 3;
    const int trackH  = max(1, trackY1 - trackY0);

    spr.drawFastVLine(sbX, BODY_Y, SCREEN_H - BODY_Y, C(COL_BORDER));

    // Up arrow
    {
        uint32_t col = (g_menuScroll > 0) ? COL_GREY_L : COL_BORDER;
        spr.drawRect(sbX, upY, MNU_SB_W, MNU_ARROW_H, C(COL_BORDER));
        int acx = sbX + MNU_SB_W / 2;
        spr.fillTriangle(acx-5, upY+16, acx+5, upY+16, acx, upY+6, C(col));
    }
    // Down arrow
    {
        uint32_t col = (g_menuScroll < maxScroll) ? COL_GREY_L : COL_BORDER;
        spr.drawRect(sbX, dnY, MNU_SB_W, MNU_ARROW_H, C(COL_BORDER));
        int acx = sbX + MNU_SB_W / 2;
        spr.fillTriangle(acx-5, dnY+6, acx+5, dnY+6, acx, dnY+16, C(col));
    }
    // Scroll thumb
    {
        int thumbH = max(12, trackH * visRows / MNU_NROWS);
        int avail  = trackH - thumbH;
        int thumbY = trackY0 + (maxScroll > 0 ? avail * g_menuScroll / maxScroll : 0);
        spr.fillRoundRect(sbX + 5, thumbY, MNU_SB_W - 10, thumbH, 2, C(COL_GREY));
    }
}

// ─── PAGE_LOCATION ────────────────────────────────────────────────────────────
/** @brief Draw the Location sub-page: a 6-character Maidenhead grid editor with
 *         up/down spinners per column and the resolved lat/lon. */
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
static const int TZ_SB_W    = 22;                    // right-hand sidebar width
static const int TZ_ARROW_H = 22;                    // up/down button height
static const int TZ_LIST_W  = SCREEN_W - TZ_SB_W;     // row content width

/** @brief Draw the Timezone sub-page: a scrollable alphabetical list of zones
 *         with the current one highlighted, plus an auto-detect option. */
static void drawTimezone() {
    drawSubHeader("TIMEZONE");
    ensureTzOrder();

    int y    = BODY_Y;
    int rows = (SCREEN_H - y) / TZ_ROW_H;
    int maxScroll = max(0, TZ_COUNT - rows);
    if (g_tzScroll > maxScroll) g_tzScroll = maxScroll;

    // ── Live scrollbar interaction ─────────────────────────────────────────────
    // Polled every frame (not just on finger-up) so dragging the thumb tracks
    // the finger smoothly, and tapping anywhere in the track jumps straight to
    // that position — both reported as "doing nothing" before this existed.
    if (touchIsActive() && maxScroll > 0) {
        int32_t tx = touchCurrentX();
        int32_t ty = touchCurrentY();
        if (tx >= SCREEN_W - TZ_SB_W) {
            int trackY0 = BODY_Y + TZ_ARROW_H + 3;
            int trackY1 = SCREEN_H - TZ_ARROW_H - 3;
            if (ty >= trackY0 && ty <= trackY1 && trackY1 > trackY0) {
                float frac = (float)(ty - trackY0) / (float)(trackY1 - trackY0);
                if (frac < 0.0f) frac = 0.0f;
                if (frac > 1.0f) frac = 1.0f;
                g_tzScroll = (int)(frac * maxScroll + 0.5f);
            }
        }
    }

    for (int vi = 0; vi < rows; vi++) {
        int pos = g_tzScroll + vi;
        if (pos >= TZ_COUNT) break;
        int ti = g_tzOrder[pos];
        int ry = y + vi * TZ_ROW_H;

        bool sel = (strcmp(TZ_LIST[ti].posix, g_settings.tz) == 0);
        if (sel) spr.fillRect(0, ry, TZ_LIST_W, TZ_ROW_H, C(0x0F1F0FUL));

        spr.fillCircle(10, ry + TZ_ROW_H / 2, 4,
                       C(sel ? COL_GREEN : COL_BORDER));

        spr.setFont(UI_FONT_9);
        spr.setTextColor(C(sel ? COL_WHITE : COL_GREY_L));
        spr.setCursor(22, ry + 6);
        spr.print(TZ_LIST[ti].name);

        spr.drawFastHLine(0, ry + TZ_ROW_H - 1, TZ_LIST_W, C(COL_BORDER));
    }

    // ── Right-hand sidebar: page-up arrow, scroll thumb, page-down arrow ──────
    const int sbX  = SCREEN_W - TZ_SB_W;
    const int upY  = BODY_Y;
    const int dnY  = SCREEN_H - TZ_ARROW_H;
    const int trackY0 = upY + TZ_ARROW_H + 3;
    const int trackY1 = dnY - 3;
    const int trackH   = max(1, trackY1 - trackY0);

    spr.drawFastVLine(sbX, BODY_Y, SCREEN_H - BODY_Y, C(COL_BORDER));

    // Up arrow
    {
        bool enabled = g_tzScroll > 0;
        uint32_t col = enabled ? COL_GREY_L : COL_BORDER;
        spr.drawRect(sbX, upY, TZ_SB_W, TZ_ARROW_H, C(COL_BORDER));
        int acx = sbX + TZ_SB_W / 2;
        spr.fillTriangle(acx - 5, upY + 16, acx + 5, upY + 16, acx, upY + 6, C(col));
    }
    // Down arrow
    {
        bool enabled = g_tzScroll < maxScroll;
        uint32_t col = enabled ? COL_GREY_L : COL_BORDER;
        spr.drawRect(sbX, dnY, TZ_SB_W, TZ_ARROW_H, C(COL_BORDER));
        int acx = sbX + TZ_SB_W / 2;
        spr.fillTriangle(acx - 5, dnY + 6, acx + 5, dnY + 6, acx, dnY + 16, C(col));
    }
    // Track + thumb
    {
        int thumbH = max(12, trackH * rows / TZ_COUNT);
        int avail  = trackH - thumbH;
        int thumbY = trackY0 + (maxScroll > 0 ? avail * g_tzScroll / maxScroll : 0);
        spr.fillRoundRect(sbX + 5, thumbY, TZ_SB_W - 10, thumbH, 2, C(COL_GREY));
    }
}

// ─── PAGE_SCREENS ─────────────────────────────────────────────────────────────
static const int SCR_SB_W    = 22;                    // right-hand sidebar width
static const int SCR_ARROW_H = 22;                     // up/down button height
static const int SCR_LIST_W  = SCREEN_W - SCR_SB_W;    // row content width
static const int SCR_BTN_X   = SCR_LIST_W - 58;        // ON/OFF button x

/** @brief Draw the Screens sub-page: a scrollable list of every screen with a
 *         per-screen enable/disable toggle. */
static void drawScreens() {
    drawSubHeader("SCREENS");

    static const char* names[NUM_SCREENS] = {
        "Clock", "Weather", "HF Conditions", "Propagation",
        "ISS Tracker", "PSK Reporter", "FT8 Spots",
        "DX Spots", "POTA Spots", "SOTA Spots", "Contests",
        "BBC News", "Apple News", "Tracker"
    };

    int visRows = (SCREEN_H - BODY_Y) / SCR_ROW_H;
    int maxScroll = max(0, NUM_SCREENS - visRows);
    if (g_scrScroll > maxScroll) g_scrScroll = maxScroll;

    // ── Live scrollbar interaction (tap-to-jump + drag), same as Timezone ─────
    if (touchIsActive() && maxScroll > 0) {
        int32_t tx = touchCurrentX();
        int32_t ty = touchCurrentY();
        if (tx >= SCREEN_W - SCR_SB_W) {
            int trackY0 = BODY_Y + SCR_ARROW_H + 3;
            int trackY1 = SCREEN_H - SCR_ARROW_H - 3;
            if (ty >= trackY0 && ty <= trackY1 && trackY1 > trackY0) {
                float frac = (float)(ty - trackY0) / (float)(trackY1 - trackY0);
                if (frac < 0.0f) frac = 0.0f;
                if (frac > 1.0f) frac = 1.0f;
                g_scrScroll = (int)(frac * maxScroll + 0.5f);
            }
        }
    }

    for (int vi = 0; vi < visRows; vi++) {
        int si = g_scrScroll + vi;
        if (si >= NUM_SCREENS) break;

        int ry = BODY_Y + vi * SCR_ROW_H;
        spr.drawFastHLine(0, ry, SCR_LIST_W, C(COL_BORDER));
        spr.setFont(UI_FONT_9);
        // Screen number (dim) + name — makes it obvious the list is scrollable
        spr.setTextColor(C(COL_GREY));
        char numBuf[16];
        snprintf(numBuf, sizeof(numBuf), "%d.", si + 1);
        spr.setCursor(6, ry + 7);
        spr.print(numBuf);
        int numW = spr.textWidth(numBuf);
        spr.setTextColor(C(COL_GREY_L));
        spr.setCursor(6 + numW + 4, ry + 7);
        spr.print(names[si]);

        bool en = g_settings.screenEnabled[si];
        drawBtn(SCR_BTN_X, ry + 3, 52, 18,
                en ? 0x0A280AUL : 0x280A0AUL,
                en ? COL_GREEN  : COL_RED,
                en ? COL_GREEN  : COL_RED,
                en ? "ON" : "OFF");
    }
    spr.drawFastHLine(0, BODY_Y + min(visRows, NUM_SCREENS) * SCR_ROW_H,
                      SCR_LIST_W, C(COL_BORDER));

    // ── Right-hand sidebar: page-up arrow, scroll thumb, page-down arrow ──────
    const int sbX  = SCREEN_W - SCR_SB_W;
    const int upY  = BODY_Y;
    const int dnY  = SCREEN_H - SCR_ARROW_H;
    const int trackY0 = upY + SCR_ARROW_H + 3;
    const int trackY1 = dnY - 3;
    const int trackH   = max(1, trackY1 - trackY0);

    spr.drawFastVLine(sbX, BODY_Y, SCREEN_H - BODY_Y, C(COL_BORDER));

    // Up arrow
    {
        bool enabled = g_scrScroll > 0;
        uint32_t col = enabled ? COL_GREY_L : COL_BORDER;
        spr.drawRect(sbX, upY, SCR_SB_W, SCR_ARROW_H, C(COL_BORDER));
        int acx = sbX + SCR_SB_W / 2;
        spr.fillTriangle(acx - 5, upY + 16, acx + 5, upY + 16, acx, upY + 6, C(col));
    }
    // Down arrow
    {
        bool enabled = g_scrScroll < maxScroll;
        uint32_t col = enabled ? COL_GREY_L : COL_BORDER;
        spr.drawRect(sbX, dnY, SCR_SB_W, SCR_ARROW_H, C(COL_BORDER));
        int acx = sbX + SCR_SB_W / 2;
        spr.fillTriangle(acx - 5, dnY + 6, acx + 5, dnY + 6, acx, dnY + 16, C(col));
    }
    // Track + thumb
    {
        int thumbH = max(12, trackH * visRows / NUM_SCREENS);
        int avail  = trackH - thumbH;
        int thumbY = trackY0 + (maxScroll > 0 ? avail * g_scrScroll / maxScroll : 0);
        spr.fillRoundRect(sbX + 5, thumbY, SCR_SB_W - 10, thumbH, 2, C(COL_GREY));
    }
}

// ─── PAGE_TRACKER ─────────────────────────────────────────────────────────────
/** @brief Draw the Stocks Tracker sub-page: preset symbol buttons and a
 *         history-range (years) selector. */
static void drawTracker() {
    drawSubHeader("STOCKS TRACKER");

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
/** @brief Draw the Callsign sub-page: a per-character spinner editor for the
 *         amateur callsign used by PSK Reporter and FT8 Spots. */
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
/**
 * @brief Cycle one character of the callsign editor through its allowed set.
 * @param cs   Callsign working buffer (modified in place).
 * @param pos  Index of the character to change.
 * @param dir  +1 for the next allowed character, -1 for the previous.
 */
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

// ─── PAGE_BRIGHTNESS ─────────────────────────────────────────────────────────
/** @brief Draw the Brightness sub-page: a live backlight-level slider with a
 *         preview swatch. */
static void drawBrightness() {
    drawSubHeader("BRIGHTNESS");

    const int SLX  = 14;                      // slider left edge
    const int SLW  = SCREEN_W - SLX - 14;     // slider track width
    const int SLY  = BODY_Y + 30;             // slider vertical centre
    const int THW  = 14;                      // thumb width
    const int THH  = 30;                      // thumb height

    // Live drag: update brightness while finger is held on the slider
    if (touchIsActive()) {
        int32_t tx = touchCurrentX();
        int32_t ty2 = touchCurrentY();
        if (ty2 >= SLY - 20 && ty2 <= SLY + 20) {
            int raw = (int)((float)(tx - SLX) / SLW * 255.0f + 0.5f);
            g_settings.brightness = (uint8_t)constrain(raw, 10, 255);
            tft.setBrightness(g_settings.brightness);
        }
    }

    uint8_t bv = g_settings.brightness;
    int pct    = (int)((bv * 100 + 127) / 255);
    int thumbX = SLX + (int)((float)bv / 255.0f * (SLW - THW));

    // Track background
    spr.fillRoundRect(SLX, SLY - 4, SLW, 8, 4, C(COL_BORDER));
    // Filled portion (left of thumb)
    uint32_t fillCol = (pct >= 75) ? C(COL_GREEN) : (pct >= 40) ? C(COL_AMBER) : C(COL_RED);
    spr.fillRoundRect(SLX, SLY - 4, thumbX - SLX + THW / 2, 8, 4, fillCol);
    // Thumb
    spr.fillRoundRect(thumbX, SLY - THH / 2, THW, THH, 4, C(COL_WHITE));
    spr.drawRoundRect(thumbX, SLY - THH / 2, THW, THH, 4, C(COL_GREY));

    // Percentage label
    spr.setFont(UI_FONT_18);
    spr.setTextColor(C(COL_WHITE));
    char pbuf[8]; snprintf(pbuf, sizeof(pbuf), "%d%%", pct);
    int pw = spr.textWidth(pbuf);
    spr.setCursor((SCREEN_W - pw) / 2, SLY + 28);
    spr.print(pbuf);

    // Preset buttons: 25 / 50 / 75 / 100
    static const uint8_t PRESETS[]  = { 64, 128, 191, 255 };
    static const char*   PLABELS[]  = { "25%", "50%", "75%", "100%" };
    const int PBW = 60, PBH = 26, PBY = SLY + 62;
    int totalPW = 4 * PBW + 3 * 8;
    int pbx = (SCREEN_W - totalPW) / 2;
    for (int i = 0; i < 4; i++) {
        bool active = (abs((int)bv - (int)PRESETS[i]) <= 2);
        spr.fillRoundRect(pbx, PBY, PBW, PBH, 4,
                          active ? C(COL_ORANGE_D) : C(COL_PANEL));
        spr.drawRoundRect(pbx, PBY, PBW, PBH, 4,
                          active ? C(COL_ORANGE) : C(COL_BORDER));
        spr.setFont(UI_FONT_12);
        spr.setTextColor(C(COL_WHITE));
        int lw = spr.textWidth(PLABELS[i]);
        spr.setCursor(pbx + (PBW - lw) / 2, PBY + 6);
        spr.print(PLABELS[i]);
        pbx += PBW + 8;
    }

    // Hint
    spr.setFont(UI_FONT_9);
    spr.setTextColor(C(COL_GREY));
    const char* hint = "Drag slider or tap a preset";
    int hw = spr.textWidth(hint);
    spr.setCursor((SCREEN_W - hw) / 2, PBY + PBH + 10);
    spr.print(hint);
}

// ─── PAGE_MODES ──────────────────────────────────────────────────────────────
static const int MODE_NROWS = 6;
static const uint8_t MODE_BITS[MODE_NROWS] = {
    SPOT_MODE_CW, SPOT_MODE_SSB, SPOT_MODE_FT8, SPOT_MODE_FT4, SPOT_MODE_RTTY, SPOT_MODE_OTHER
};
static const char* MODE_NAMES[MODE_NROWS] = {
    "CW", "Voice (SSB/FM/AM)", "FT8", "FT4", "Digital (RTTY/PSK)", "Other"
};

/** @brief Draw the Mode Filter sub-page: per-mode (CW/SSB/FT8/…) enable toggles
 *         applied to the spot screens. */
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

// ─── PAGE_ISSFREQ / PAGE_ISSFREQ_EDIT ────────────────────────────────────────
static const int    ISSFREQ_ROW_H = 30;
static const char*  ISSFREQ_LABEL[4] = {
    "SSTV", "APRS", "Repeater In", "Repeater Out"
};
/**
 * @brief Get a pointer to the settings field for one ISS frequency slot.
 * @param i  Slot index: 0=SSTV, 1=APRS, 2=Repeater In, 3=Repeater Out (default).
 * @return Pointer to the corresponding g_settings float field.
 */
static float* issFreqPtr(int i) {
    switch (i) {
        case 0:  return &g_settings.issFreqSstv;
        case 1:  return &g_settings.issFreqAprs;
        case 2:  return &g_settings.issFreqRptIn;
        default: return &g_settings.issFreqRptOut;
    }
}

/**
 * @brief Load a MHz value into the 6 working digits (DDD.DDD, kHz resolution).
 * @param mhz  Frequency in MHz; clamped to the 000.000–999.999 range.
 */
static void freqLoadDigits(float mhz) {
    long kHz = lroundf(mhz * 1000.0f);
    if (kHz < 0)      kHz = 0;
    if (kHz > 999999) kHz = 999999;
    g_editDigits[0] = (kHz / 100000) % 10;
    g_editDigits[1] = (kHz / 10000)  % 10;
    g_editDigits[2] = (kHz / 1000)   % 10;
    g_editDigits[3] = (kHz / 100)    % 10;
    g_editDigits[4] = (kHz / 10)     % 10;
    g_editDigits[5] =  kHz           % 10;
}
/** @brief Assemble the 6 working digits back into a MHz value.
 *  @return The edited frequency in MHz (kHz resolution). */
static float freqFromDigits() {
    long kHz = g_editDigits[0] * 100000L + g_editDigits[1] * 10000L + g_editDigits[2] * 1000L
             + g_editDigits[3] * 100L    + g_editDigits[4] * 10L    + g_editDigits[5];
    return kHz / 1000.0f;
}

/** @brief Draw the ISS Frequencies sub-page: the four configured downlink/uplink
 *         frequencies, each tappable to open the digit editor. */
static void drawIssFreq() {
    drawSubHeader("ISS FREQUENCIES");
    for (int i = 0; i < 4; i++) {
        int ry = BODY_Y + i * ISSFREQ_ROW_H;
        spr.drawFastHLine(0, ry, SCREEN_W, C(COL_BORDER));
        spr.setFont(UI_FONT_12);
        spr.setTextColor(C(COL_GREY_L));
        spr.setCursor(14, ry + 10);
        spr.print(ISSFREQ_LABEL[i]);

        char vb[16];
        snprintf(vb, sizeof(vb), "%.3f MHz", *issFreqPtr(i));
        int vw = spr.textWidth(vb);
        spr.setTextColor(C(COL_ORANGE));
        spr.setCursor(SCREEN_W - vw - 24, ry + 10);
        spr.print(vb);
        spr.setTextColor(C(COL_GREY));
        spr.setCursor(SCREEN_W - 16, ry + 10);
        spr.print(">");
    }
    spr.drawFastHLine(0, BODY_Y + 4 * ISSFREQ_ROW_H, SCREEN_W, C(COL_BORDER));

    spr.setFont(UI_FONT_9);
    spr.setTextColor(C(COL_GREY));
    spr.setCursor(14, BODY_Y + 4 * ISSFREQ_ROW_H + 12);
    spr.print("Tap a frequency to edit");
}

/** @brief Draw the ISS-frequency digit editor: six per-digit up/down spinners
 *         (DDD.DDD MHz) for the selected frequency slot. */
static void drawIssFreqEdit() {
    drawSubHeader(ISSFREQ_LABEL[g_editFreqSel]);

    // ▲ buttons (reusing the LOCATION page's 6-column spinner geometry)
    for (int i = 0; i < 6; i++) {
        int bx = i * LOC_COL_W + (LOC_COL_W - 36) / 2;
        spr.fillRoundRect(bx, LOC_UP_Y, 36, LOC_BTN_H, 3, C(COL_PANEL));
        spr.drawRoundRect(bx, LOC_UP_Y, 36, LOC_BTN_H, 3, C(COL_BORDER));
        spr.setFont(UI_FONT_12);
        spr.setTextColor(C(COL_GREY_L));
        spr.setCursor(bx + 13, LOC_UP_Y + 8);
        spr.print("^");
    }

    // Digits DDD.DDD
    for (int i = 0; i < 6; i++) {
        char ch[2] = { (char)('0' + g_editDigits[i]), 0 };
        spr.setFont(UI_FONT_24);
        int cw = spr.textWidth(ch);
        spr.setTextColor(C(COL_WHITE));
        spr.setCursor(i * LOC_COL_W + (LOC_COL_W - cw) / 2, LOC_CHAR_Y + 4);
        spr.print(ch);
    }
    // Decimal point in the gap between the 3rd and 4th digit
    spr.setFont(UI_FONT_24);
    spr.setTextColor(C(COL_ORANGE));
    spr.setCursor(3 * LOC_COL_W - 6, LOC_CHAR_Y + 4);
    spr.print(".");

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

    // Resulting value + unit
    spr.setFont(UI_FONT_9);
    spr.setTextColor(C(COL_GREY));
    char vb[20];
    snprintf(vb, sizeof(vb), "%.3f MHz", freqFromDigits());
    int vw = spr.textWidth(vb);
    spr.setCursor((SCREEN_W - vw) / 2, LOC_INFO_Y);
    spr.print(vb);
    spr.drawFastHLine(8, LOC_INFO_Y + 14, SCREEN_W - 16, C(COL_BORDER));

    drawBtn((SCREEN_W - 110) / 2, LOC_DONE_Y, 110, 28,
            COL_ORANGE_D, COL_ORANGE, COL_WHITE, "DONE");
}

// ─── PAGE_SERVICE ─────────────────────────────────────────────────────────────
static const int    SVC_ROW_H = 30;
static const char*  SVC_LABELS[3] = { "Screenshot (3s hold)", "Fake Lightning", "Fake ISS Pass" };
/**
 * @brief Read the current on/off state of a Service-menu row.
 * @param i  Row index: 0=screenshot hold, 1=fake lightning, 2=fake ISS pass.
 * @return true if that service option is currently enabled.
 */
static bool serviceState(int i) {
    return i == 0 ? g_settings.screenshotHoldEnabled
         : i == 1 ? g_fakeLightning
                  : g_fakeISSPass;
}

/** @brief Draw the Service sub-page: toggles for the 3-second screenshot hold and
 *         the fake lightning / fake ISS-pass test aids. */
static void drawService() {
    drawSubHeader("SERVICE");
    for (int i = 0; i < 3; i++) {
        int ry = BODY_Y + i * SVC_ROW_H;
        spr.drawFastHLine(0, ry, SCREEN_W, C(COL_BORDER));
        spr.setFont(UI_FONT_12);
        spr.setTextColor(C(COL_WHITE));
        spr.setCursor(14, ry + 10);
        spr.print(SVC_LABELS[i]);

        bool on = serviceState(i);
        spr.fillRoundRect(SCREEN_W - 76, ry + 6, 32, 18, 3, on ? C(COL_GREEN)  : C(COL_BORDER));
        spr.drawRoundRect(SCREEN_W - 76, ry + 6, 32, 18, 3, C(COL_GREY));
        spr.setTextColor(C(COL_WHITE));
        spr.setCursor(SCREEN_W - 68, ry + 9);
        spr.print("ON");
        spr.fillRoundRect(SCREEN_W - 40, ry + 6, 36, 18, 3, on ? C(COL_BORDER) : C(COL_RED));
        spr.drawRoundRect(SCREEN_W - 40, ry + 6, 36, 18, 3, C(COL_GREY));
        spr.setCursor(SCREEN_W - 34, ry + 9);
        spr.print("OFF");
    }
    spr.drawFastHLine(0, BODY_Y + 3 * SVC_ROW_H, SCREEN_W, C(COL_BORDER));

    spr.setFont(UI_FONT_9);
    spr.setTextColor(C(COL_GREY));
    spr.setCursor(14, BODY_Y + 3 * SVC_ROW_H + 12);
    spr.print("Hold the screen 3s to save a BMP screenshot to SD.");
    spr.setCursor(14, BODY_Y + 3 * SVC_ROW_H + 26);
    spr.print("Fakes reset on reboot.");
}

/**
 * @brief Run the interactive touch-calibration routine (blocking).
 *
 * Clears any saved calibration, draws directly to the display, has the user
 * touch the crosshairs, then stores the new calibration to settings and
 * returns to the main settings menu.
 */
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
/** @brief Draw the auto-detect-timezone page: shows progress, the detected zone
 *         (which it applies and saves once), or an error with a manual-set hint. */
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
/** @brief Draw the current settings page, dispatching on the active page enum.
 *         See screen_settings.h. */
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
        case PAGE_MODES:       drawModes();       break;
        case PAGE_TZ_DETECT:   drawTzDetect();   break;
        case PAGE_BRIGHTNESS:  drawBrightness(); break;
        case PAGE_ISSFREQ:      drawIssFreq();     break;
        case PAGE_ISSFREQ_EDIT: drawIssFreqEdit(); break;
        case PAGE_SERVICE:      drawService();     break;
        default: break;
    }
}

// ─── Public: enter ────────────────────────────────────────────────────────────
/** @brief Enter the settings UI: reset to the main menu and seed the editors
 *         (grid, callsign) and timezone scroll position. See screen_settings.h. */
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

    // Scroll to current timezone (position in the alphabetically-sorted list)
    ensureTzOrder();
    g_tzScroll = 0;
    for (int pos = 0; pos < TZ_COUNT; pos++) {
        if (strcmp(TZ_LIST[g_tzOrder[pos]].posix, g_settings.tz) == 0) {
            g_tzScroll = max(0, pos - 3);
            break;
        }
    }
}

// ─── Grid char cycling ────────────────────────────────────────────────────────
/**
 * @brief Cycle one character of the Maidenhead grid editor within its valid set.
 *
 * Fields 0–1 cycle A–R, positions 2–3 cycle 0–9, and positions 4–5 cycle a–x.
 * @param grid  Grid working buffer (modified in place).
 * @param pos   Character index (0–5).
 * @param dir   +1 for the next character, -1 for the previous (wraps).
 */
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
/** @brief Handle a finger-up gesture on the settings UI, dispatching per page
 *         (taps toggle/navigate, swipes/scroll where relevant).
 *         See screen_settings.h. @return true when settings should close. */
bool settingsTouchUp(int32_t sx, int32_t sy,
                     int32_t ex, int32_t ey,
                     uint32_t dtMs) {
    bool isTap   = (abs(ex - sx) < 22 && abs(ey - sy) < 22 && dtMs < 500);
    bool isSwipe = (abs(ey - sy) > 28 && dtMs < SWIPE_MAX_MS);

    switch (g_page) {

    // LovyanGFX's calibrateTouch() blocks and consumes its own input, so there
    // is nothing to handle here.  Listed explicitly rather than left to fall
    // through, so -Wswitch keeps checking this switch for gaps.
    case PAGE_CALIBRATE:
        break;

    // ── MENU ──────────────────────────────────────────────────────────────────
    case PAGE_MENU:
        // Header: close button
        if (isTap && sy < BODY_Y) {
            if (sx > SCREEN_W - 34) return true;   // X tapped
            break;
        }
        // Sidebar: arrow taps and swipe scrolling
        {
            int visRows   = (SCREEN_H - BODY_Y) / MNU_ROW_H;
            int maxScroll = max(0, MNU_NROWS - visRows);
            bool startedInSidebar = (sx >= MNU_LIST_W);

            if (isTap && startedInSidebar) {
                // Arrow buttons
                if (sy >= BODY_Y && sy < BODY_Y + MNU_ARROW_H)
                    g_menuScroll = max(0, g_menuScroll - 1);
                else if (sy >= SCREEN_H - MNU_ARROW_H)
                    g_menuScroll = min(maxScroll, g_menuScroll + 1);
                break;
            }
            if (!startedInSidebar && isSwipe) {
                if (ey < sy) g_menuScroll = min(maxScroll, g_menuScroll + 1);
                else         g_menuScroll = max(0,         g_menuScroll - 1);
                break;
            }
            if (!isTap || startedInSidebar) break;
            if (sx >= MNU_LIST_W) break;   // tap in sidebar but not arrow — ignore
        }
        // List row tap
        {
            int visRows = (SCREEN_H - BODY_Y) / MNU_ROW_H;
            int vi  = (sy - BODY_Y) / MNU_ROW_H;
            int ri  = g_menuScroll + vi;
            if (vi < 0 || vi >= visRows || ri >= MNU_NROWS) break;

            if (ri == 2) {
                // Distance Units: [mi] left button | [km] right button
                // Split at the OFF-button edge, matching the ON/OFF toggles.
                bool nowKm = (sx >= MNU_LIST_W - 40);
                if (nowKm != g_settings.useKm) {
                    g_settings.useKm = nowKm;
                    settingsSave();
                }
            } else if (ri == 4) {
                // ISS Pass Alert ON/OFF toggle — only if Grey Line screen is on
                if (g_settings.screenEnabled[SCR_GREYLINE]) {
                    g_settings.issJumpEnabled = (sx < MNU_LIST_W - 40);
                    settingsSave();
                }
            } else if (ri == 5) {
                // ISS Pass LED ON/OFF toggle (same button geometry as row 4)
                g_settings.issLedEnabled = (sx < MNU_LIST_W - 40);
                settingsSave();
            } else if (ri == 6) {
                // Lightning LED ON/OFF toggle (same button geometry as row 4)
                g_settings.lightningLedEnabled = (sx < MNU_LIST_W - 40);
                settingsSave();
            } else if (ri == 11) {
                // Startup LED ON/OFF toggle (same button geometry as row 4)
                g_settings.startupLedEnabled = (sx < MNU_LIST_W - 40);
                settingsSave();
            } else if (ri == 13) {
                // Temperature: [C] left button | [F] right button
                // Split at the OFF-button edge, matching the ON/OFF toggles.
                bool nowCelsius = (sx < MNU_LIST_W - 40);
                if (nowCelsius != g_settings.useCelsius) {
                    g_settings.useCelsius = nowCelsius;
                    settingsSave();
                }
            } else {
                strlcpy(g_editGrid, g_settings.grid, sizeof(g_editGrid));
                switch (ri) {
                    case 0: g_page = PAGE_BRIGHTNESS; break;
                    case 1:
                        memset(g_editCall, ' ', 6); g_editCall[6] = '\0';
                        { int cl = (int)strlen(g_settings.callsign); if (cl > 6) cl = 6;
                          for (int i = 0; i < cl; i++) g_editCall[i] = g_settings.callsign[i]; }
                        g_page = PAGE_CALLSIGN;
                        break;
                    case 3: g_page = PAGE_ISSFREQ;  break;
                    case 7: g_page = PAGE_LOCATION; break;
                    case 8: g_page = PAGE_MODES; break;
                    case 9: g_page = PAGE_SCREENS;  g_scrScroll = 0; break;
                    case 10: g_page = PAGE_SERVICE; break;
                    case 12: g_page = PAGE_TRACKER;  break;   // Stocks Tracker
                    case 14: g_page = PAGE_TIMEZONE; break;
                    case 15: g_page = PAGE_CALIBRATE; break;
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
    case PAGE_TIMEZONE: {
        ensureTzOrder();
        int visRows   = (SCREEN_H - BODY_Y) / TZ_ROW_H;
        int maxScroll = max(0, TZ_COUNT - visRows);

        // A drag that started in the sidebar is handled live, frame-by-frame,
        // by drawTimezone() — skip the generic swipe/tap handling entirely so
        // release doesn't also apply its own +/-1 nudge on top.
        bool startedInSidebar = (sx >= SCREEN_W - TZ_SB_W);

        if (isSwipe) {
            if (!startedInSidebar) {
                if (ey < sy) g_tzScroll = min(g_tzScroll + 1, maxScroll);
                else         g_tzScroll = max(g_tzScroll - 1, 0);
            }
            break;
        }
        if (!isTap) break;
        if (sy < BODY_Y && sx < 40) { g_page = PAGE_MENU; break; }

        // Sidebar: page-up / page-down arrows
        if (startedInSidebar) {
            if (sy < BODY_Y + TZ_ARROW_H) {
                g_tzScroll = max(0, g_tzScroll - visRows);
            } else if (sy >= SCREEN_H - TZ_ARROW_H) {
                g_tzScroll = min(maxScroll, g_tzScroll + visRows);
            }
            break;
        }

        // Row tap — select timezone
        {
            int vi  = (sy - BODY_Y) / TZ_ROW_H;
            int pos = g_tzScroll + vi;
            if (pos >= 0 && pos < TZ_COUNT) {
                int ti = g_tzOrder[pos];
                strlcpy(g_settings.tz,     TZ_LIST[ti].posix, sizeof(g_settings.tz));
                strlcpy(g_settings.tzName, TZ_LIST[ti].name,  sizeof(g_settings.tzName));
                settingsSave();
            }
        }
        break;
    }

    // ── SCREENS ───────────────────────────────────────────────────────────────
    case PAGE_SCREENS: {
        int visRows   = (SCREEN_H - BODY_Y) / SCR_ROW_H;
        int maxScroll = NUM_SCREENS - visRows;
        if (maxScroll < 0) maxScroll = 0;

        // A drag that started in the sidebar is handled live, frame-by-frame,
        // by drawScreens() — skip the generic swipe/tap nudge on release.
        bool startedInSidebar = (sx >= SCREEN_W - SCR_SB_W);

        if (isSwipe) {
            if (!startedInSidebar) {
                if (ey < sy) g_scrScroll = min(g_scrScroll + 1, maxScroll);
                else         g_scrScroll = max(g_scrScroll - 1, 0);
            }
            break;
        }
        if (!isTap) break;
        if (sy < BODY_Y && sx < 40) { g_page = PAGE_MENU; break; }

        // Sidebar: page-up / page-down arrows
        if (startedInSidebar) {
            if (sy < BODY_Y + SCR_ARROW_H) {
                g_scrScroll = max(0, g_scrScroll - visRows);
            } else if (sy >= SCREEN_H - SCR_ARROW_H) {
                g_scrScroll = min(maxScroll, g_scrScroll + visRows);
            }
            break;
        }
        {
            int vi  = (sy - BODY_Y) / SCR_ROW_H;   // visible row index
            int si  = g_scrScroll + vi;             // actual screen index
            if (vi >= 0 && vi < visRows && si < NUM_SCREENS && sx >= SCR_BTN_X) {
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

    // ── BRIGHTNESS ───────────────────────────────────────────────────────────
    case PAGE_BRIGHTNESS:
        if (isTap && sy < BODY_Y && sx < 40) {
            settingsSave();
            g_page = PAGE_MENU;
            break;
        }
        if (!isTap) break;
        {
            // Preset button tap?
            const int SLY  = BODY_Y + 30;
            const int PBW = 60, PBH = 26, PBY = SLY + 62;
            int totalPW = 4 * PBW + 3 * 8;
            int pbx = (SCREEN_W - totalPW) / 2;
            static const uint8_t PRESETS[] = { 64, 128, 191, 255 };
            if (sy >= PBY && sy < PBY + PBH) {
                for (int i = 0; i < 4; i++) {
                    if (sx >= pbx && sx < pbx + PBW) {
                        g_settings.brightness = PRESETS[i];
                        tft.setBrightness(g_settings.brightness);
                        settingsSave();
                        break;
                    }
                    pbx += PBW + 8;
                }
            }
        }
        break;

    // ── ISS FREQUENCIES (list) ──────────────────────────────────────────────────
    case PAGE_ISSFREQ:
        if (isTap && sy < BODY_Y && sx < 40) { g_page = PAGE_MENU; break; }
        if (!isTap) break;
        if (sy >= BODY_Y) {
            int i = (sy - BODY_Y) / ISSFREQ_ROW_H;
            if (i >= 0 && i < 4) {
                g_editFreqSel = i;
                freqLoadDigits(*issFreqPtr(i));
                g_page = PAGE_ISSFREQ_EDIT;
            }
        }
        break;

    // ── ISS FREQUENCY EDITOR (digit spinner) ────────────────────────────────────
    case PAGE_ISSFREQ_EDIT:
        if (isTap && sy < BODY_Y && sx < 40) { g_page = PAGE_ISSFREQ; break; }  // back, discard
        if (!isTap) break;
        {
            int col = sx / LOC_COL_W;
            if (col < 0) col = 0;
            if (col > 5) col = 5;

            if (sy >= LOC_DONE_Y && sy < LOC_DONE_Y + 28) {
                *issFreqPtr(g_editFreqSel) = freqFromDigits();
                settingsSave();
                g_page = PAGE_ISSFREQ;
                break;
            }
            if (sy >= LOC_UP_Y && sy < LOC_UP_Y + LOC_BTN_H)
                g_editDigits[col] = (uint8_t)((g_editDigits[col] + 1) % 10);
            else if (sy >= LOC_DN_Y && sy < LOC_DN_Y + LOC_BTN_H)
                g_editDigits[col] = (uint8_t)((g_editDigits[col] + 9) % 10);
        }
        break;

    // ── SERVICE ─────────────────────────────────────────────────────────────────
    case PAGE_SERVICE:
        if (isTap && sy < BODY_Y && sx < 40) { g_page = PAGE_MENU; break; }  // back
        if (!isTap) break;
        if (sy >= BODY_Y) {
            int i = (sy - BODY_Y) / SVC_ROW_H;
            if (i >= 0 && i < 3) {
                bool on = (sx < SCREEN_W - 40);   // left = ON, right = OFF
                switch (i) {
                    case 0: g_settings.screenshotHoldEnabled = on; settingsSave(); break;
                    case 1: g_fakeLightning = on; break;
                    case 2: g_fakeISSPass   = on; break;
                }
            }
        }
        break;
    }

    return false;
}
