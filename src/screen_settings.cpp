#include "screen_settings.h"
#include "settings.h"
#include "ui_common.h"
#include "ui.h"
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
    PAGE_BRIGHTNESS,
};

static SettingsPage g_page        = PAGE_MENU;
static int          g_menuScroll  = 0;      // menu first-visible row
static int          g_tzScroll    = 0;      // timezone list first-visible row
static int          g_scrScroll   = 0;      // screens list first-visible row
static char         g_editGrid[8];          // working copy for location editor
static char         g_editCall[14];         // working copy for callsign editor
static bool         g_tzApplied   = false;  // true once an arrived tz-lookup result has been saved

// ─── Timezone display order — alphabetical, decoupled from TZ_LIST's own
// index order (which TZ_ALIASES and tzFindByIana() depend on staying fixed) ──
static int  g_tzOrder[TZ_COUNT];
static bool g_tzOrderReady = false;

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
static const int MNU_SB_W    = 22;
static const int MNU_ARROW_H = 22;
static const int MNU_LIST_W  = SCREEN_W - MNU_SB_W;
static const int MNU_ROW_H   = 28;
static const int MNU_NROWS   = 11;  // 8 sub-page rows + 3 inline toggles

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

    // Alphabetical row order (11 rows):
    //  0 Brightness  1 Callsign  2 Distance Units  3 ISS Pass Alert
    //  4 Location    5 Mode Filter  6 Screens  7 Temperature Units
    //  8 Timezone    9 Touch Calibrate  10 Tracker
    // Rows 2, 3, 7 are inline toggles; the rest navigate to a sub-page.
    // Sub-page label index: li = ri < 2 ? ri : (ri < 4 ? -1 : (ri < 7 ? ri-2 : (ri==7 ? -1 : ri-3)))
    static const char* labels[8] = {
        "Brightness", "Callsign", "Location", "Mode Filter",
        "Screens", "Timezone", "Touch Calibrate", "Tracker"
    };

    // Helper to draw a two-option selector button pair
    // left button highlighted green when leftSel==true, right highlighted green otherwise
    auto drawSelector = [&](int bx, int ry, const char* leftLbl, const char* rightLbl, bool leftSel) {
        int lw = spr.textWidth(leftLbl)  + 12;
        int rw = spr.textWidth(rightLbl) + 12;
        spr.fillRoundRect(bx,      ry + 5, lw, 18, 3,  leftSel ? C(COL_GREEN)  : C(COL_BORDER));
        spr.drawRoundRect(bx,      ry + 5, lw, 18, 3,  C(COL_GREY));
        spr.setTextColor(C(COL_WHITE));
        spr.setCursor(bx + 6, ry + 8);
        spr.print(leftLbl);
        spr.fillRoundRect(bx + lw + 4, ry + 5, rw, 18, 3, leftSel ? C(COL_BORDER) : C(COL_GREEN));
        spr.drawRoundRect(bx + lw + 4, ry + 5, rw, 18, 3, C(COL_GREY));
        spr.setCursor(bx + lw + 10, ry + 8);
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
            drawSelector(MNU_LIST_W - 88, ry, "mi", "km", !g_settings.useKm);

        } else if (ri == 3) {
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

        } else if (ri == 7) {
            // Temperature Units: [°C] [°F]
            spr.setTextColor(C(COL_WHITE));
            spr.setCursor(14, ry + 8);
            spr.print("Temperature");
            drawSelector(MNU_LIST_W - 80, ry, "C", "F", g_settings.useCelsius);

        } else {
            // Sub-page navigation row
            int li = ri < 2 ? ri : (ri < 7 ? ri - 2 : ri - 3);
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

// ─── PAGE_BRIGHTNESS ─────────────────────────────────────────────────────────
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
        case PAGE_MODES:       drawModes();       break;
        case PAGE_TZ_DETECT:   drawTzDetect();   break;
        case PAGE_BRIGHTNESS:  drawBrightness(); break;
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
                // Buttons drawn starting at MNU_LIST_W-88; boundary between them ≈ MNU_LIST_W-60
                bool nowKm = (sx >= MNU_LIST_W - 60);
                if (nowKm != g_settings.useKm) {
                    g_settings.useKm = nowKm;
                    settingsSave();
                }
            } else if (ri == 3) {
                // ISS Pass Alert ON/OFF toggle — only if Grey Line screen is on
                if (g_settings.screenEnabled[SCR_GREYLINE]) {
                    g_settings.issJumpEnabled = (sx < MNU_LIST_W - 40);
                    settingsSave();
                }
            } else if (ri == 7) {
                // Temperature: [C] left button | [F] right button
                // Buttons drawn starting at MNU_LIST_W-80; boundary between them ≈ MNU_LIST_W-60
                bool nowCelsius = (sx < MNU_LIST_W - 60);
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
                    case 4: g_page = PAGE_LOCATION; break;
                    case 5: g_page = PAGE_MODES; break;
                    case 6: g_page = PAGE_SCREENS;  g_scrScroll = 0; break;
                    case 8: g_page = PAGE_TIMEZONE; break;
                    case 9: g_page = PAGE_CALIBRATE; break;
                    case 10: g_page = PAGE_TRACKER;  break;
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
    }

    return false;
}
