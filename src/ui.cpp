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
#include "screen_bbc.h"
#include "screen_apple.h"
#include "screen_tracker.h"
#include "screen_dxspots.h"
#include "screen_pota.h"
#include "screen_sota.h"
#include "screen_contests.h"
#include "screen_settings.h"
#include "fonts/ui_fonts.h"

// ─── Shared sprite + display pointer ─────────────────────────────────────────
LGFX_Sprite spr;
static LGFX* gDisplay = nullptr;


// ─── Shared primitive implementations ────────────────────────────────────────

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
    int digits = dotPos;          // number of integer digits
    int commaEvery = 3;
    for (int i = 0; raw[i]; i++) {
        if (i < dotPos) {
            int fromRight = dotPos - i - 1;
            if (fromRight > 0 && fromRight % commaEvery == 0) {
                out[oi++] = ',';
            }
        }
        out[oi++] = raw[i];
    }
    out[oi] = '\0';
    strlcpy(buf, out, bufLen);
}

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
    "Grey Line",
    "PSK Reporter",
    "DX Spots",
    "POTA Spots",
    "SOTA Spots",
    "Contests",
    "BBC News",
    "Apple News",
    "Tracker",
};

// Status bar button X positions — shared with touch handler via getStatusBarLayout()
// All four right-side slots use the same width for even spacing.
#define SB_SLOT_W    16

static int s_sbCogX, s_sbWifiX, s_sbAdvX, s_sbPlayX;

void getStatusBarLayout(int& playX, int& advX, int& wifiX, int& cogX) {
    playX = s_sbPlayX;
    advX  = s_sbAdvX;
    wifiX = s_sbWifiX;
    cogX  = s_sbCogX;
}

// ─── WiFi info overlay ────────────────────────────────────────────────────────
static bool s_wifiOverlay = false;
static char s_wifiSSID[33] = {};
static char s_wifiIP[16]   = {};
static char s_wifiMAC[18]  = {};
static int  s_wifiRSSI     = 0;

void showWifiOverlay(const char* ssid, const char* ip, const char* mac, int rssi) {
    strlcpy(s_wifiSSID, ssid, sizeof(s_wifiSSID));
    strlcpy(s_wifiIP,   ip,   sizeof(s_wifiIP));
    strlcpy(s_wifiMAC,  mac,  sizeof(s_wifiMAC));
    s_wifiRSSI    = rssi;
    s_wifiOverlay = true;
}
void hideWifiOverlay()      { s_wifiOverlay = false; }
bool wifiOverlayVisible()   { return s_wifiOverlay; }

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

    // Hamburger menu (settings)
    s_sbCogX = rx - SB_SLOT_W;
    {
        int bx = s_sbCogX + 3, bw = SB_SLOT_W - 6;
        spr.fillRect(bx, IT,          bw, 2, C(COL_GREY));
        spr.fillRect(bx, IMY - 1,     bw, 2, C(COL_GREY));
        spr.fillRect(bx, IB - 2,      bw, 2, C(COL_GREY));
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

    // Advance button (>>)
    s_sbAdvX = rx - SB_SLOT_W;
    {
        int ax = s_sbAdvX + 2;
        spr.fillTriangle(ax,     IT, ax,     IB, ax + 5, IMY, C(COL_GREY));
        spr.fillTriangle(ax + 5, IT, ax + 5, IB, ax + 10, IMY, C(COL_GREY));
    }
    rx = s_sbAdvX - GAP;

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
    char cntBuf[8];
    snprintf(cntBuf, sizeof(cntBuf), "%d/%d", screenID + 1, NUM_SCREENS);
    spr.setTextColor(C(COL_GREY));
    spr.setCursor(4, 3);
    spr.print(cntBuf);
    int cntW = spr.textWidth(cntBuf);

    spr.setTextColor(C(COL_ORANGE));
    spr.setCursor(4 + cntW + 5, 3);
    spr.print(SCREEN_LABELS[screenID]);

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
            case SCR_BBC:         drawScreenBBC();         break;
            case SCR_APPLE:       drawScreenApple();       break;
            case SCR_TRACKER:     drawScreenTracker();     break;
            case SCR_DXSPOTS:     drawScreenDXSpots();     break;
            case SCR_POTASPOTS:   drawScreenPOTA();        break;
            case SCR_SOTASPOTS:   drawScreenSOTA();        break;
            case SCR_CONTESTS:    drawScreenContests();    break;
        }
    }

    if (s_wifiOverlay) drawWifiOverlay();

    if (gDisplay) spr.pushSprite(gDisplay, 0, 0);
}
