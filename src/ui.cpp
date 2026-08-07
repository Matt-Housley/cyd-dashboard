#include "ui_common.h"
#include "config.h"
#include <esp_heap_caps.h>
#include <time.h>

// WiFi helpers — implemented in main.cpp so WiFi.h never enters this TU
extern int getWiFiBars();
#include "screen_clock.h"
#include "screen_weather.h"
#include "screen_hf_now.h"
#include "screen_propagation.h"
#include "screen_greyline.h"
#include "screen_pskreporter.h"
#include "screen_ft8spots.h"
#include "screen_bbc.h"
#include "screen_apple.h"
#include "screen_tracker.h"
#include "screen_dxspots.h"
#include "screen_pota.h"
#include "screen_sota.h"
#include "screen_contests.h"
#include "screen_settings.h"
#include "settings.h"
#include "fonts/ui_fonts.h"

// ─── Shared sprite + display pointer ─────────────────────────────────────────
LGFX_Sprite spr;
static LGFX* gDisplay = nullptr;


// ─── Shared primitive implementations ────────────────────────────────────────

/** @brief Draw a sparkline with translucent fill (see ui_common.h). */
void drawSparkline(int x, int y, int w, int h,
                   const float* vals, int count, uint32_t lineCol,
                   int visibleCount) {
    if (!vals || count < 2) return;
    if (visibleCount < 0 || visibleCount > count) visibleCount = count;
    if (visibleCount < 2) visibleCount = 2;

    // Min/max always derived from the FULL dataset so the Y-scale stays fixed
    // while the animation reveals points left-to-right.
    float mn = vals[0], mx2 = vals[0];
    for (int i = 1; i < count; i++) {
        if (vals[i] < mn)  mn  = vals[i];
        if (vals[i] > mx2) mx2 = vals[i];
    }
    float range = (mx2 - mn < 0.01f) ? 1.0f : (mx2 - mn);

    uint32_t fillCol = spr.color888(
        ((lineCol >> 16) & 0xFF) / 4,
        ((lineCol >>  8) & 0xFF) / 4,
        ( lineCol        & 0xFF) / 4);
    uint32_t lc = spr.color888(
        (lineCol >> 16) & 0xFF,
        (lineCol >>  8) & 0xFF,
         lineCol        & 0xFF);

    int baseY = y + h - 2;

    // X positions use (count - 1) so the scale is stable throughout animation
    for (int i = 0; i < visibleCount; i++) {
        int cx = x + (i * (w - 1)) / (count - 1);
        int cy = y + (h - 4) - (int)(((vals[i] - mn) / range) * (h - 8)) - 2;
        if (cy < baseY)
            spr.drawFastVLine(cx, cy, baseY - cy, fillCol);
    }
    for (int i = 1; i < visibleCount; i++) {
        int x0 = x + ((i-1) * (w - 1)) / (count - 1);
        int y0 = y + (h - 4) - (int)(((vals[i-1] - mn) / range) * (h - 8)) - 2;
        int x1 = x + (i     * (w - 1)) / (count - 1);
        int y1 = y + (h - 4) - (int)(((vals[i]   - mn) / range) * (h - 8)) - 2;
        spr.drawLine(x0, y0, x1, y1, lc);
    }
}

/** @brief Word-wrap and draw text within a box (see ui_common.h).
 *  @return The y position just after the last line printed. */
int printWrapped(int x, int y, const char* text, int maxW, int lineH, int maxY) {
    String src(text);
    String line;
    int pos = 0;
    int slen = (int)src.length();

    while (pos <= slen) {
        int sp = src.indexOf(' ', pos);
        if (sp < 0) sp = slen;

        String word = src.substring(pos, sp);
        if (word.isEmpty() && sp == slen) break;

        String trial = line.isEmpty() ? word : line + ' ' + word;

        if ((int)spr.textWidth(trial.c_str()) <= maxW || line.isEmpty()) {
            line = trial;
        } else {
            spr.setCursor(x, y);
            spr.print(line.c_str());
            y += lineH;
            line = word;
            if (y + lineH > maxY) break;
        }
        pos = sp + 1;
    }
    if (!line.isEmpty() && y + lineH <= maxY) {
        spr.setCursor(x, y);
        spr.print(line.c_str());
        y += lineH;
    }
    return y;
}

/** @brief Format a price with thousands separators (see ui_common.h). */
void fmtPrice(char* buf, size_t bufLen, float price) {
    // Build without locale grouping, then insert commas
    char raw[16];
    snprintf(raw, sizeof(raw), "%.2f", price);
    // Find decimal point position
    int dotPos = -1;
    for (int i = 0; raw[i]; i++) if (raw[i] == '.') { dotPos = i; break; }
    if (dotPos < 0) { strlcpy(buf, raw, bufLen); return; }

    // Insert commas every 3 digits from the decimal point leftward
    char out[20] = {};
    int oi = 0;
    int commaEvery = 3;
    for (int i = 0; raw[i]; i++) {
        // Comma before digit i when a whole number of 3-digit groups follows it
        // within the integer part (and i isn't the leading digit).
        if (i > 0 && i < dotPos && (dotPos - i) % commaEvery == 0) {
            out[oi++] = ',';
        }
        out[oi++] = raw[i];
    }
    out[oi] = '\0';
    strlcpy(buf, out, bufLen);
}

/** @brief Draw a centred loader message with animated dots (see ui_common.h). */
void drawLoader(const char* msg) {
    spr.setFont(UI_FONT_12);
    spr.setTextColor(C(COL_GREY));

    int len = (int)strlen(msg);
    if (len >= 3 && msg[len-1] == '.' && msg[len-2] == '.' && msg[len-3] == '.') {
        char buf[64];
        int base = len - 3;
        if (base >= (int)sizeof(buf)) base = sizeof(buf) - 4;
        memcpy(buf, msg, base);
        int dots = (millis() / 400) % 4;
        for (int d = 0; d < dots; d++) buf[base + d] = '.';
        buf[base + dots] = '\0';

        int fullW = spr.textWidth(msg);
        spr.setCursor((SCREEN_W - fullW) / 2, SCREEN_H / 2 - 8);
        spr.print(buf);
    } else {
        int tw = spr.textWidth(msg);
        spr.setCursor((SCREEN_W - tw) / 2, SCREEN_H / 2 - 8);
        spr.print(msg);
    }
}

// ─── Status bar ──────────────────────────────────────────────────────────────
static const char* SCREEN_LABELS[NUM_SCREENS] = {
    "Clock",
    "Weather",
    "HF Conditions",
    "Propagation",
    "ISS Tracker",
    "PSK Reporter",
    "FT8 Spots",   // dynamic — see screenLabel() (FT8/FT4 per settings)
    "DX Spots",
    "POTA Spots",
    "SOTA Spots",
    "Contests",
    "BBC News",
    "Apple News",
    "Tracker",
};

/** @brief The display label for a screen. Usually SCREEN_LABELS[id], but the
 *         FTx Spots screen's label tracks the FT8/FT4 mode setting. */
static const char* screenLabel(int id) {
    if (id == SCR_FT8SPOTS) return ft8ScreenTitle();
    return SCREEN_LABELS[id];
}

// Status bar button X positions — shared with touch handler via getStatusBarLayout()
// All right-side slots use the same width for even spacing.
#define SB_SLOT_W    16

static int s_sbCogX, s_sbWifiX, s_sbCircX, s_sbPlayX;

/** @brief Report the status-bar button x-positions for touch hit-testing
 *         (see ui.h). Valid after the most recent uiDraw(). */
void getStatusBarLayout(int& playX, int& circX, int& wifiX, int& cogX) {
    playX = s_sbPlayX;
    circX = s_sbCircX;
    wifiX = s_sbWifiX;
    cogX  = s_sbCogX;
}

// ─── WiFi info overlay ────────────────────────────────────────────────────────
static bool s_wifiOverlay = false;
static char s_wifiSSID[33] = {};
static char s_wifiIP[16]   = {};
static char s_wifiMAC[18]  = {};
static int  s_wifiRSSI     = 0;

/** @brief Populate and show the WiFi info overlay (see ui.h). */
void showWifiOverlay(const char* ssid, const char* ip, const char* mac, int rssi) {
    strlcpy(s_wifiSSID, ssid, sizeof(s_wifiSSID));
    strlcpy(s_wifiIP,   ip,   sizeof(s_wifiIP));
    strlcpy(s_wifiMAC,  mac,  sizeof(s_wifiMAC));
    s_wifiRSSI    = rssi;
    s_wifiOverlay = true;
}
/** @brief Hide the WiFi info overlay (see ui.h). */
void hideWifiOverlay()      { s_wifiOverlay = false; }
/** @brief Whether the WiFi info overlay is showing (see ui.h). @return true if visible. */
bool wifiOverlayVisible()   { return s_wifiOverlay; }

/** @brief Draw the WiFi info overlay box (SSID/IP/MAC/signal) from cached state. */
static void drawWifiOverlay() {
    spr.setFont(UI_FONT_9);

    // Measure to fit box width to the widest line
    char ssidLine[48], ipLine[32], macLine[32], sigLine[32];
    snprintf(ssidLine, sizeof(ssidLine), "SSID:   %s", s_wifiSSID);
    snprintf(ipLine,   sizeof(ipLine),   "IP:     %s", s_wifiIP);
    snprintf(macLine,  sizeof(macLine),  "MAC:    %s", s_wifiMAC);
    int bars = (s_wifiRSSI > -55) ? 4 : (s_wifiRSSI > -65) ? 3 :
               (s_wifiRSSI > -75) ? 2 : 1;
    snprintf(sigLine,  sizeof(sigLine),  "Signal: %d dBm", s_wifiRSSI);

    spr.setFont(UI_FONT_12);
    int titleW = spr.textWidth("WiFi Information");
    spr.setFont(UI_FONT_9);
    int w = titleW;
    auto mw = [&](const char* s){ int tw = spr.textWidth(s); if (tw > w) w = tw; };
    mw(ssidLine); mw(ipLine); mw(macLine); mw(sigLine);

    const int IPX    = 7, IPY = 5;
    const int LH     = 11;
    const int DIVGAP = 7;   // gap from title baseline to first data line
                            // divider drawn at +2, leaving 5px to text — matches bottom IPY
    const int BW  = w + 2 * IPX;
    const int BH  = IPY + 14 + DIVGAP + LH + LH + LH + LH + IPY;
    const int BX  = (SCREEN_W - BW) / 2;
    const int BY  = STATUS_H + 6;

    spr.fillRect(BX, BY, BW, BH, spr.color888(10, 12, 26));
    spr.drawRect(BX, BY, BW, BH, spr.color888(60, 120, 200));
    spr.drawFastHLine(BX + 1, BY + IPY + 14 + 2, BW - 2, spr.color888(40, 80, 150));

    int tx = BX + IPX;
    int ty = BY + IPY;

    spr.setFont(UI_FONT_12);
    spr.setTextColor(spr.color888(0, 210, 255));
    spr.setCursor(tx, ty); spr.print("WiFi Information");
    ty += 14 + DIVGAP;

    spr.setFont(UI_FONT_9);

    spr.setTextColor(spr.color888(180, 180, 200));
    spr.setCursor(tx, ty); spr.print(ssidLine); ty += LH;
    spr.setCursor(tx, ty); spr.print(ipLine);   ty += LH;
    spr.setCursor(tx, ty); spr.print(macLine);  ty += LH;

    // Signal line: text + small bar graphic
    spr.setCursor(tx, ty);
    spr.print(sigLine);
    int barX = tx + spr.textWidth(sigLine) + 4;
    for (int b = 0; b < 4; b++) {
        int bh = 3 + b * 2;
        uint32_t col = (b < bars) ? spr.color888(0, 200, 80) : spr.color888(40, 40, 50);
        spr.fillRect(barX + b * 5, ty + (8 - bh), 4, bh, col);
    }
}

// ─── Toast (brief bottom-centre message) ─────────────────────────────────────
static char          s_toastMsg[40] = {};
static unsigned long s_toastMs      = 0;
/** @brief Show a brief bottom-centre toast message (~2.5 s) (see ui.h). */
void showToast(const char* msg) { strlcpy(s_toastMsg, msg, sizeof(s_toastMsg)); s_toastMs = millis(); }

/** @brief Draw the toast message if one is active and not yet expired. */
static void drawToast() {
    if (s_toastMs == 0 || millis() - s_toastMs > 2500) return;
    spr.setFont(UI_FONT_12);
    int tw = spr.textWidth(s_toastMsg);
    int bw = tw + 20, bh = 22;
    int bx = (SCREEN_W - bw) / 2, by = SCREEN_H - bh - 8;
    spr.fillRoundRect(bx, by, bw, bh, 4, spr.color888(20, 30, 20));
    spr.drawRoundRect(bx, by, bw, bh, 4, C(COL_GREEN));
    spr.setTextColor(C(COL_WHITE));
    spr.setCursor(bx + 10, by + 5);
    spr.print(s_toastMsg);
}

// ─── Full-text popup (tapped news headline) ──────────────────────────────────
static bool          s_textPopup = false;
static unsigned long s_popupMs   = 0;
static char          s_popupText[128] = {};

/** @brief Show the full-text popup (see ui.h). */
void showTextPopup(const char* text) {
    strlcpy(s_popupText, text, sizeof(s_popupText));
    s_textPopup = true;
    s_popupMs   = millis();
}
/** @brief Hide the full-text popup immediately (see ui.h). */
void hideTextPopup() { s_textPopup = false; }
/** @brief Whether the full-text popup is visible, auto-closing after 30 s (see ui.h).
 *  @return true if the popup should be drawn this frame. */
bool textPopupVisible() {
    if (s_textPopup && millis() - s_popupMs > 30000) s_textPopup = false;  // auto-close
    return s_textPopup;
}

/**
 * @brief Count how many lines @p text wraps to at a given width.
 *
 * Mirrors printWrapped()'s wrapping so a popup can be sized before drawing.
 * The current font must already be set.
 * @param text  Text to measure.
 * @param maxW  Maximum line width in pixels.
 * @return Number of wrapped lines (at least 1).
 */
static int wrapLineCount(const char* text, int maxW) {
    String src(text), line;
    int pos = 0, slen = (int)src.length(), lines = 0;
    while (pos <= slen) {
        int sp = src.indexOf(' ', pos); if (sp < 0) sp = slen;
        String word = src.substring(pos, sp);
        if (word.isEmpty() && sp == slen) break;
        String trial = line.isEmpty() ? word : line + ' ' + word;
        if ((int)spr.textWidth(trial.c_str()) <= maxW || line.isEmpty()) line = trial;
        else { lines++; line = word; }
        pos = sp + 1;
    }
    if (!line.isEmpty()) lines++;
    return lines < 1 ? 1 : lines;
}

/** @brief Draw the full-text popup box (auto-sized to the wrapped text). */
static void drawTextPopup() {
    const int PAD   = 10;
    const int BW    = SCREEN_W - 24;
    const int textW = BW - 2 * PAD;
    const int lineH = 16;
    const int hintH = 12;

    spr.setFont(UI_FONT_12);
    int textH = wrapLineCount(s_popupText, textW) * lineH;
    int BH = PAD + textH + 6 + hintH + 6;
    if (BH > SCREEN_H - STATUS_H - 8) BH = SCREEN_H - STATUS_H - 8;
    int BX = (SCREEN_W - BW) / 2;
    int BY = (SCREEN_H - BH) / 2;
    if (BY < STATUS_H + 4) BY = STATUS_H + 4;

    spr.fillRoundRect(BX, BY, BW, BH, 5, spr.color888(16, 22, 32));
    spr.drawRoundRect(BX, BY, BW, BH, 5, C(COL_ORANGE));

    spr.setFont(UI_FONT_12);
    spr.setTextColor(C(COL_WHITE));
    printWrapped(BX + PAD, BY + PAD, s_popupText, textW, lineH, BY + PAD + textH + 2);

    spr.setFont(UI_FONT_9);
    spr.setTextColor(C(COL_GREY));
    const char* hint = "tap to close";
    spr.setCursor(BX + BW - spr.textWidth(hint) - 8, BY + BH - hintH - 3);
    spr.print(hint);
}

// ─── Screen-picker menu ───────────────────────────────────────────────────────
// Opened by the Circle button. Lists every enabled screen in a two-column grid;
// tapping a cell jumps straight to that screen. The cell rects are recorded on
// each draw so the touch handler can hit-test against exactly what's on screen.
static bool s_menuOpen = false;

#define MENU_MAX  NUM_SCREENS
static int s_menuCount = 0;              // number of cells currently laid out
static int s_menuId[MENU_MAX];           // screen ID behind each cell
static int s_menuX[MENU_MAX], s_menuY[MENU_MAX], s_menuW[MENU_MAX], s_menuH[MENU_MAX];

/** @brief Open the jump-to-screen picker menu (see ui.h). */
void showScreenMenu()    { s_menuOpen = true; }
/** @brief Close the jump-to-screen picker menu (see ui.h). */
void hideScreenMenu()    { s_menuOpen = false; }
/** @brief Whether the jump-to-screen picker is open (see ui.h). @return true if open. */
bool screenMenuVisible() { return s_menuOpen; }

/** @brief Recompute the enabled-screen list and per-cell grid geometry into the
 *         s_menu* static arrays, ready for drawing and hit-testing. */
static void buildScreenMenu() {
    s_menuCount = 0;
    for (int i = 0; i < NUM_SCREENS && s_menuCount < MENU_MAX; i++)
        if (g_settings.screenEnabled[i]) s_menuId[s_menuCount++] = i;

    if (s_menuCount == 0) return;

    const int COLS = 2;
    const int rows = (s_menuCount + COLS - 1) / COLS;   // left column filled first

    const int MX   = 4;                 // outer margin
    const int GY0  = STATUS_H + 22;      // grid top (below the title)
    const int GAP  = 4;
    const int gridH = SCREEN_H - GY0 - MX;
    const int colW  = (SCREEN_W - 2 * MX - GAP) / COLS;
    const int rowH  = (gridH - (rows - 1) * GAP) / rows;

    for (int k = 0; k < s_menuCount; k++) {
        int col = k / rows;
        int row = k % rows;
        s_menuX[k] = MX + col * (colW + GAP);
        s_menuY[k] = GY0 + row * (rowH + GAP);
        s_menuW[k] = colW;
        s_menuH[k] = rowH;
    }
}

/**
 * @brief Draw the jump-to-screen picker grid, highlighting the current screen.
 * @param screenID  The currently displayed screen (drawn as selected).
 */
static void drawScreenMenu(int screenID) {
    buildScreenMenu();

    // Dim the content area behind the menu
    spr.fillRect(0, STATUS_H, SCREEN_W, SCREEN_H - STATUS_H, C(COL_BG));

    spr.setFont(UI_FONT_12);
    spr.setTextColor(C(COL_ORANGE));
    spr.setCursor(6, STATUS_H + 4);
    spr.print("Jump to screen");

    spr.setFont(UI_FONT_9);
    for (int k = 0; k < s_menuCount; k++) {
        int id  = s_menuId[k];
        bool cur = (id == screenID);
        int x = s_menuX[k], y = s_menuY[k], w = s_menuW[k], h = s_menuH[k];

        spr.fillRoundRect(x, y, w, h, 3, C(COL_BG));
        spr.drawRoundRect(x, y, w, h, 3, C(cur ? COL_ORANGE : COL_BORDER));

        // Marker dot + label, vertically centred
        int cy = y + h / 2;
        spr.fillCircle(x + 8, cy, 2, C(cur ? COL_ORANGE : COL_GREY));
        spr.setTextColor(C(cur ? COL_ORANGE : COL_WHITE));
        spr.setCursor(x + 16, cy - 5);
        spr.print(screenLabel(id));
    }
}

/** @brief Hit-test a tap against the open screen-picker menu (see ui.h).
 *  @return The tapped screen's ID, or -1 if the tap missed every cell. */
int screenMenuHitTest(int x, int y) {
    for (int k = 0; k < s_menuCount; k++)
        if (x >= s_menuX[k] && x < s_menuX[k] + s_menuW[k] &&
            y >= s_menuY[k] && y < s_menuY[k] + s_menuH[k])
            return s_menuId[k];
    return -1;
}

/**
 * @brief Draw the top status bar: screen counter/label, centre clock, and the
 *        right-side play/pause, jump-to-screen, WiFi, and cog icons.
 *
 * Records each right-side icon's x-position into the s_sb* statics for
 * getStatusBarLayout() to expose to the touch handler.
 * @param screenID  Currently displayed screen (for the label and counter).
 * @param autoPlay  Whether auto-cycling is on (selects play vs pause icon).
 * @param paused    Whether cycling is paused (reserved; currently unused here).
 */
static void drawStatusBar(int screenID, bool autoPlay, bool paused) {
    spr.fillRect(0, 0, SCREEN_W, STATUS_H, C(COL_BG));
    spr.drawFastHLine(0, STATUS_H - 1, SCREEN_W, C(COL_BORDER));
    spr.setFont(UI_FONT_9);

    // ── Right side layout (right to left) ────────────────────────────────────
    // All icons drawn within a consistent bounding box: SB_SLOT_W × 10px,
    // vertically centred in the STATUS_H bar.
    const int GAP  = 6;
    const int IT   = 3;                // icon top (3px from top of bar)
    const int IB   = STATUS_H - 3;     // icon bottom
    const int IH   = IB - IT;          // icon height = 10px
    const int IMY  = (IT + IB) / 2;    // icon vertical midpoint
    int rx = SCREEN_W - 3;

    // Settings cog — 11x11 8-tooth gear bitmap with a centre hole
    s_sbCogX = rx - SB_SLOT_W;
    {
        static const uint16_t GEAR11[11] = {
            0x0D8, 0x0F8, 0x1FC, 0x7FF, 0x78F, 0x38E, 0x78F, 0x7FF, 0x1FC, 0x0F8, 0x0D8
        };
        int gx = s_sbCogX + 3, gy = IT;   // 11px tall fills the icon band (IT..IB)
        uint32_t col = C(COL_GREY);
        for (int r = 0; r < 11; r++)
            for (int cbit = 0; cbit < 11; cbit++)
                if (GEAR11[r] & (1 << (10 - cbit)))
                    spr.drawPixel(gx + cbit, gy + r, col);
    }
    rx = s_sbCogX - GAP;

    // WiFi signal bars
    s_sbWifiX = rx - SB_SLOT_W;
    {
        int bars = getWiFiBars();
        int wfX = s_sbWifiX + 1;
        for (int b = 0; b < 4; b++) {
            int bh  = (IH * (b + 1) + 2) / 4;
            uint32_t col = (b < bars) ? C(COL_GREEN) : C(COL_BORDER);
            spr.fillRect(wfX + b * 4, IB - bh, 3, bh, col);
        }
    }
    rx = s_sbWifiX - GAP;

    // Jump-to-screen menu — hamburger (three horizontal bars)
    s_sbCircX = rx - SB_SLOT_W;
    {
        int bx = s_sbCircX + 3, bw = SB_SLOT_W - 6;
        spr.fillRect(bx, IT,      bw, 2, C(COL_GREY));
        spr.fillRect(bx, IMY - 1, bw, 2, C(COL_GREY));
        spr.fillRect(bx, IB - 2,  bw, 2, C(COL_GREY));
    }
    rx = s_sbCircX - GAP;

    // Play/Pause button
    s_sbPlayX = rx - SB_SLOT_W;
    {
        uint32_t iconCol = autoPlay ? COL_GREY : COL_ORANGE;
        if (autoPlay) {
            int px = s_sbPlayX + 3;
            spr.fillRect(px,     IT, 3, IH, C(iconCol));
            spr.fillRect(px + 6, IT, 3, IH, C(iconCol));
        } else {
            int px = s_sbPlayX + 3;
            spr.fillTriangle(px, IT, px, IB, px + 9, IMY, C(iconCol));
        }
    }

    // ── Left side: screen counter + label ────────────────────────────────────
    char cntBuf[24];
    snprintf(cntBuf, sizeof(cntBuf), "%d/%d", screenID + 1, NUM_SCREENS);
    spr.setTextColor(C(COL_GREY));
    spr.setCursor(4, 3);
    spr.print(cntBuf);
    int cntW = spr.textWidth(cntBuf);

    spr.setTextColor(C(COL_ORANGE));
    spr.setCursor(4 + cntW + 5, 3);
    spr.print(screenLabel(screenID));

    // ── Local time (centre) ───────────────────────────────────────────────────
    struct tm ti;
    if (getLocalTime(&ti)) {
        char tbuf[6];
        snprintf(tbuf, sizeof(tbuf), "%02d:%02d", ti.tm_hour, ti.tm_min);
        spr.setTextColor(C(COL_WHITE));
        int tmw = spr.textWidth(tbuf);
        spr.setCursor((SCREEN_W - tmw) / 2, 3);
        spr.print(tbuf);
    }
}

// ─── Public API ───────────────────────────────────────────────────────────────
/** @brief Allocate the shared framebuffer sprite, falling back to 8-bit depth
 *         (and halting with an on-screen error if even that fails). See ui.h. */
void uiInit(LGFX* display) {
    gDisplay = display;
    spr.setColorDepth(16);
    bool ok = spr.createSprite(SCREEN_W, SCREEN_H);
    if (!ok) {
        // 16-bit needs 150 KB DMA-contiguous; fall back to 8-bit (76 KB)
        spr.setColorDepth(8);
        ok = spr.createSprite(SCREEN_W, SCREEN_H);
    }
    Serial.printf("[ui] sprite %dx%d %d-bit alloc %s  free heap=%u\n",
                  SCREEN_W, SCREEN_H, spr.getColorDepth(),
                  ok ? "OK" : "FAILED", esp_get_free_heap_size());
    if (!ok) {
        display->fillScreen(display->color888(0x80, 0, 0));
        display->setFont(UI_FONT_9);
        display->setTextColor(display->color888(0xFF,0xFF,0xFF));
        display->setCursor(4, 80);
        display->printf("Sprite alloc failed!\nFree heap: %u bytes\nLargest DMA block: %u",
                        esp_get_free_heap_size(),
                        heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
        for(;;) delay(1000);
    }
}

/** @brief Render a screen (or the settings overlay) plus any active overlays
 *         into the sprite and push it to the display. See ui.h. */
void uiDraw(int screenID, bool autoPlay, bool paused, bool inSettings) {
    spr.fillSprite(C(COL_BG));
    drawStatusBar(screenID, autoPlay, paused);

    if (inSettings) {
        drawScreenSettings();
    } else {
        switch ((ScreenID)screenID) {
            case SCR_CLOCK:       drawScreenClock();       break;
            case SCR_WEATHER:     drawScreenWeather();     break;
            case SCR_HF_NOW:      drawScreenHFNow();       break;
            case SCR_PROPAGATION: drawScreenPropagation(); break;
            case SCR_GREYLINE:      drawScreenGreyLine();      break;
            case SCR_PSKREPORTER:  drawScreenPSKReporter();  break;
            case SCR_FT8SPOTS:     drawScreenFT8Spots();     break;
            case SCR_BBC:         drawScreenBBC();         break;
            case SCR_APPLE:       drawScreenApple();       break;
            case SCR_TRACKER:     drawScreenTracker();     break;
            case SCR_DXSPOTS:     drawScreenDXSpots();     break;
            case SCR_POTASPOTS:   drawScreenPOTA();        break;
            case SCR_SOTASPOTS:   drawScreenSOTA();        break;
            case SCR_CONTESTS:    drawScreenContests();    break;
        }
    }

    if (textPopupVisible() && (screenID == SCR_BBC || screenID == SCR_APPLE))
        drawTextPopup();
    if (s_menuOpen)   drawScreenMenu(screenID);
    if (s_wifiOverlay) drawWifiOverlay();
    drawToast();

    if (gDisplay) spr.pushSprite(gDisplay, 0, 0);
}
