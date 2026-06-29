#include "screen_contests.h"
#include "ui_common.h"
#include "config.h"
#include "data_store.h"
#include "fonts/ui_fonts.h"

static int8_t s_cxSelected = -1;

void contestClearSelection() { s_cxSelected = -1; }
bool contestHasSelection()   { return s_cxSelected >= 0; }

bool contestTouchUp(int32_t x, int32_t y) {
    if (s_cxSelected >= 0) {
        s_cxSelected = -1;
        return true;
    }

    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    int count = g_contests.count;
    xSemaphoreGive(g_dataMutex);

    const int ROWS = 6;
    const int ROW_H = CONTENT_H / ROWS;
    int show = min(count, ROWS);

    int row = (y - CONTENT_Y) / ROW_H;
    if (row >= 0 && row < show && y >= CONTENT_Y) {
        s_cxSelected = (int8_t)row;
        g_contestDetail.valid  = false;
        g_contestDetail.failed = false;
        g_contestDetailReq = row;
        return true;
    }
    return false;
}

void drawScreenContests() {
    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    ContestData cd = g_contests;
    xSemaphoreGive(g_dataMutex);

    if (!cd.valid || cd.count == 0) {
        drawLoader("Fetching Contest Calendar...");
        return;
    }

    // 6 rows fill the content area (224 px → 37 px each, 2 px spare)
    const int ROWS  = 6;
    const int ROW_H = CONTENT_H / ROWS;   // 37 px
    const int SHOW  = min((int)cd.count, ROWS);

    // Pre-calculate LIVE badge width (DejaVu9) for name-truncation maths.
    spr.setFont(UI_FONT_9);
    const int BADGE_W = spr.textWidth("LIVE") + 10;   // text + horizontal padding

    for (int i = 0; i < SHOW; i++) {
        const Contest& c = cd.items[i];
        int ry = CONTENT_Y + i * ROW_H;

        // Row background — subtle alternation
        spr.fillRect(0, ry, SCREEN_W, ROW_H, C((i & 1) ? 0x0C0C0CUL : COL_BG));
        spr.drawFastHLine(0, ry + ROW_H - 1, SCREEN_W, C(COL_BORDER));

        // Left colour bar: green = active, amber = upcoming
        uint32_t barCol = c.active ? COL_GREEN : COL_AMBER;
        spr.fillRect(0, ry, 4, ROW_H, C(barCol));

        // "LIVE" badge for active contests (right-aligned)
        if (c.active) {
            int bx = SCREEN_W - BADGE_W - 4;
            spr.fillRoundRect(bx, ry + 5, BADGE_W, 13, 3, C(COL_GREEN));
            spr.setFont(UI_FONT_9);
            spr.setTextColor(C(0x001800UL));   // very dark green text on green badge
            int tw = spr.textWidth("LIVE");
            spr.setCursor(bx + (BADGE_W - tw) / 2, ry + 7);
            spr.print("LIVE");
        }

        // Contest name — top line, truncated to avoid overrunning the badge
        int nameMaxW = (SCREEN_W - 14) - (c.active ? BADGE_W + 8 : 0);
        spr.setFont(UI_FONT_12);
        spr.setTextColor(C(c.active ? COL_WHITE : COL_DIM_WHITE));
        char nameBuf[sizeof(c.name)];
        strlcpy(nameBuf, c.name, sizeof(nameBuf));
        int nlen = (int)strlen(nameBuf);
        while (nlen > 0 && (int)spr.textWidth(nameBuf) > nameMaxW)
            nameBuf[--nlen] = '\0';
        spr.setCursor(8, ry + 7);
        spr.print(nameBuf);

        // Time string — bottom line
        spr.setFont(UI_FONT_9);
        spr.setTextColor(C(c.active ? COL_GREEN : COL_GREY));
        spr.setCursor(8, ry + 24);
        spr.print(c.times);
    }

    // Source watermark — bottom-right corner
    spr.setFont(UI_FONT_9);
    spr.setTextColor(C(0x444444UL));
    const char* src = "contestcalendar.com";
    int sw = spr.textWidth(src);
    spr.setCursor(SCREEN_W - sw - 4, SCREEN_H - 11);
    spr.print(src);

    // ── Detail overlay ───────────────────────────────────────────────────────
    if (s_cxSelected >= 0 && s_cxSelected < cd.count) {
        const int PX = 8, PW = SCREEN_W - 16;
        const int PY = CONTENT_Y + 10;

        xSemaphoreTake(g_dataMutex, portMAX_DELAY);
        ContestDetail det = g_contestDetail;
        xSemaphoreGive(g_dataMutex);

        if (!det.valid) {
            int ph = 30;
            spr.fillRect(PX, PY, PW, ph, C(COL_BG));
            spr.drawRect(PX, PY, PW, ph, C(det.failed ? COL_RED : COL_AMBER));

            spr.setFont(UI_FONT_9);
            if (det.failed) {
                spr.setTextColor(C(COL_RED));
                const char* err = "Could not load details";
                int ew = spr.textWidth(err);
                spr.setCursor(PX + (PW - ew) / 2, PY + 10);
                spr.print(err);
            } else {
                uint32_t now = millis();
                int dotCount = (now / 400) % 4;
                char msg[16];
                strlcpy(msg, "Loading", sizeof(msg));
                for (int d = 0; d < dotCount; d++) strlcat(msg, ".", sizeof(msg));

                spr.setTextColor(C(COL_AMBER));
                int mw = spr.textWidth("Loading...");
                spr.setCursor(PX + (PW - mw) / 2, PY + 10);
                spr.print(msg);
            }
        } else {
            const int lineH = 13;
            const int maxW  = PW - 14;
            const int leftX = PX + 6;
            const int rightLim = PX + PW - 6;
            spr.setFont(UI_FONT_9);

            // Word-wrap helper: counts lines the exchange text needs
            int labelW = det.exchange[0] ? spr.textWidth("Exch: ") + 4 : 0;
            int exchLines = 0;
            if (det.exchange[0]) {
                exchLines = 1;
                int cx = leftX + labelW;
                const char* p = det.exchange;
                while (*p) {
                    const char* sp = strchr(p, ' ');
                    int wlen = sp ? (int)(sp - p + 1) : (int)strlen(p);
                    char word[64];
                    strlcpy(word, p, min(wlen + 1, (int)sizeof(word)));
                    int ww = spr.textWidth(word);
                    if (cx + ww > rightLim && cx > leftX + labelW) {
                        exchLines++;
                        cx = leftX;
                    }
                    cx += ww;
                    p += wlen;
                }
            }

            int lines = 1;  // name
            if (det.mode[0])  lines++;
            if (det.bands[0]) lines++;
            lines += exchLines;
            int ph = 8 + lines * lineH + 4;
            spr.fillRect(PX, PY, PW, ph, C(COL_BG));
            spr.drawRect(PX, PY, PW, ph, C(COL_AMBER));

            int cy = PY + 5;
            spr.setFont(UI_FONT_12);
            spr.setTextColor(C(COL_AMBER));
            spr.setCursor(leftX, cy);
            spr.print(det.name);
            cy += lineH + 1;

            auto drawField = [&](int y, const char* label, const char* val) {
                spr.setFont(UI_FONT_9);
                spr.setTextColor(C(COL_GREY));
                spr.setCursor(leftX, y);
                spr.print(label);
                int lw = spr.textWidth(label);
                spr.setTextColor(C(COL_WHITE));
                spr.setCursor(leftX + lw + 4, y);
                spr.print(val);
            };

            if (det.mode[0])  { drawField(cy, "Mode:", det.mode);   cy += lineH; }
            if (det.bands[0]) { drawField(cy, "Bands:", det.bands); cy += lineH; }

            if (det.exchange[0]) {
                spr.setFont(UI_FONT_9);
                spr.setTextColor(C(COL_GREY));
                spr.setCursor(leftX, cy);
                spr.print("Exch:");

                spr.setTextColor(C(COL_WHITE));
                int cx = leftX + labelW;
                const char* p = det.exchange;
                while (*p) {
                    const char* sp = strchr(p, ' ');
                    int wlen = sp ? (int)(sp - p + 1) : (int)strlen(p);
                    char word[64];
                    strlcpy(word, p, min(wlen + 1, (int)sizeof(word)));
                    int ww = spr.textWidth(word);
                    if (cx + ww > rightLim && cx > leftX + labelW) {
                        cy += lineH;
                        cx = leftX;
                    }
                    spr.setCursor(cx, cy);
                    spr.print(word);
                    cx += ww;
                    p += wlen;
                }
            }
        }
    }
}
