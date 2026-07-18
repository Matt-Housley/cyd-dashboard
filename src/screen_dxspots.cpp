#include "screen_dxspots.h"
#include "ui_common.h"
#include "config.h"
#include "data_store.h"
#include "settings.h"
#include "fonts/ui_fonts.h"

// ─── Callsign prefix → country name ───────────────────────────────────────────
// Tries prefix lengths 3 → 2 → 1.  Strips any /suffix before matching.
// Returns "" for unknown prefixes (nothing shown).
struct PfxCty { const char pfx[5]; const char name[16]; };
static const PfxCty PFX_CTY[] = {
    // ── True 3-char prefixes (tried first) ───────────────────────────────────
    {"EA8", "Canary Is."},   {"EA9", "Ceuta"},        {"HB0", "Liechtenstein"},
    {"IS0", "Sardinia"},     {"KH0", "Mariana Is."},  {"KH2", "Guam"},
    {"KH6", "Hawaii"},       {"KP2", "US V.I."},      {"KP4", "Puerto Rico"},
    {"OH0", "Aland Is."},    {"OJ0", "Market Reef"},  {"SV5", "Dodecanese"},
    {"SV9", "Crete"},        {"UA2", "Kaliningrad"},  {"UA9", "Asiatic Russia"},
    {"VK9", "Aus. Terr."},   {"VP2", "Br. Caribbean"},{"VP5", "Turks&Caicos"},
    {"VP8", "Falklands"},    {"VP9", "Bermuda"},      {"VQ9", "Chagos"},
    {"XX9", "Macao"},        {"ZD7", "St Helena"},    {"ZD8", "Ascension I."},
    {"ZD9", "Tristan"},
    // ── 2-char prefixes ───────────────────────────────────────────────────────
    // UK — G-series (old-style licences)
    {"GD",  "Isle of Man"},  {"GI",  "N. Ireland"},   {"GJ",  "Jersey"},
    {"GM",  "Scotland"},     {"GU",  "Guernsey"},     {"GW",  "Wales"},
    // UK — M-series (post-2000 full licences)
    {"MD",  "Isle of Man"},  {"MI",  "N. Ireland"},   {"MJ",  "Jersey"},
    {"MM",  "Scotland"},     {"MU",  "Guernsey"},     {"MW",  "Wales"},
    // UK — 2-series (Foundation / Intermediate licences)
    {"2D",  "Isle of Man"},  {"2E",  "England"},      {"2I",  "N. Ireland"},
    {"2J",  "Jersey"},       {"2M",  "Scotland"},     {"2U",  "Guernsey"},
    {"2W",  "Wales"},
    // USA — A-series calls (AA–AK)
    {"AA",  "USA"},          {"AB",  "USA"},          {"AC",  "USA"},
    {"AD",  "USA"},          {"AE",  "USA"},          {"AF",  "USA"},
    {"AG",  "USA"},          {"AI",  "USA"},          {"AK",  "USA"},
    // Germany — full DA–DR allocation
    {"DA",  "Germany"},      {"DB",  "Germany"},      {"DC",  "Germany"},
    {"DD",  "Germany"},      {"DE",  "Germany"},      {"DF",  "Germany"},
    {"DG",  "Germany"},      {"DH",  "Germany"},      {"DI",  "Germany"},
    {"DJ",  "Germany"},      {"DK",  "Germany"},      {"DL",  "Germany"},
    {"DM",  "Germany"},      {"DN",  "Germany"},      {"DO",  "Germany"},
    {"DP",  "Germany"},      {"DQ",  "Germany"},      {"DR",  "Germany"},
    // Japan — full JA–JS allocation
    {"JA",  "Japan"},        {"JE",  "Japan"},        {"JF",  "Japan"},
    {"JG",  "Japan"},        {"JH",  "Japan"},        {"JI",  "Japan"},
    {"JJ",  "Japan"},        {"JK",  "Japan"},        {"JL",  "Japan"},
    {"JM",  "Japan"},        {"JN",  "Japan"},        {"JO",  "Japan"},
    {"JP",  "Japan"},        {"JQ",  "Japan"},        {"JR",  "Japan"},
    {"JS",  "Japan"},
    // Netherlands — PA–PI allocation
    {"PA",  "Netherlands"},  {"PB",  "Netherlands"},  {"PC",  "Netherlands"},
    {"PD",  "Netherlands"},  {"PE",  "Netherlands"},  {"PF",  "Netherlands"},
    {"PG",  "Netherlands"},  {"PH",  "Netherlands"},  {"PI",  "Netherlands"},
    // Brazil — PP–PY allocation
    {"PP",  "Brazil"},       {"PQ",  "Brazil"},       {"PR",  "Brazil"},
    {"PS",  "Brazil"},       {"PT",  "Brazil"},       {"PU",  "Brazil"},
    {"PV",  "Brazil"},       {"PW",  "Brazil"},       {"PX",  "Brazil"},
    {"PY",  "Brazil"},
    // Argentina — LO–LW allocation
    {"LO",  "Argentina"},    {"LP",  "Argentina"},    {"LQ",  "Argentina"},
    {"LR",  "Argentina"},    {"LS",  "Argentina"},    {"LT",  "Argentina"},
    {"LU",  "Argentina"},    {"LV",  "Argentina"},    {"LW",  "Argentina"},
    // Canada
    {"VA",  "Canada"},       {"VE",  "Canada"},       {"VO",  "Canada"},
    {"VY",  "Canada"},
    // Ukraine — full allocation
    {"UR",  "Ukraine"},      {"US",  "Ukraine"},      {"UT",  "Ukraine"},
    {"UV",  "Ukraine"},      {"UW",  "Ukraine"},      {"UX",  "Ukraine"},
    {"UY",  "Ukraine"},      {"UZ",  "Ukraine"},
    // Russia (European) & related
    {"UA",  "Russia"},       {"RA",  "Russia"},       {"RK",  "Russia"},
    // Poland
    {"SP",  "Poland"},       {"SQ",  "Poland"},       {"SR",  "Poland"},
    // Spain
    {"EA",  "Spain"},        {"EB",  "Spain"},
    // Czech Republic
    {"OK",  "Czech Rep."},   {"OL",  "Czech Rep."},
    // Romania
    {"YO",  "Romania"},      {"YP",  "Romania"},      {"YQ",  "Romania"},
    {"YR",  "Romania"},
    // Serbia / ex-YU
    {"YT",  "Serbia"},       {"YU",  "Serbia"},       {"YZ",  "Serbia"},
    // Rest of 2-char world
    {"3A",  "Monaco"},       {"3B",  "Mauritius"},    {"3C",  "Eq. Guinea"},
    {"3D",  "Swaziland"},    {"3V",  "Tunisia"},      {"4J",  "Azerbaijan"},
    {"4K",  "Azerbaijan"},   {"4L",  "Georgia"},      {"4O",  "Montenegro"},
    {"4S",  "Sri Lanka"},    {"4X",  "Israel"},       {"4Z",  "Israel"},
    {"5A",  "Libya"},        {"5B",  "Cyprus"},       {"5H",  "Tanzania"},
    {"5N",  "Nigeria"},      {"5R",  "Madagascar"},   {"5T",  "Mauritania"},
    {"5U",  "Niger"},        {"5V",  "Togo"},         {"5W",  "Samoa"},
    {"5X",  "Uganda"},       {"5Z",  "Kenya"},        {"6W",  "Senegal"},
    {"6Y",  "Jamaica"},      {"7Q",  "Malawi"},       {"7X",  "Algeria"},
    {"8P",  "Barbados"},     {"8Q",  "Maldives"},     {"8R",  "Guyana"},
    {"9A",  "Croatia"},      {"9G",  "Ghana"},        {"9H",  "Malta"},
    {"9J",  "Zambia"},       {"9K",  "Kuwait"},       {"9M",  "Malaysia"},
    {"9N",  "Nepal"},        {"9Q",  "DR Congo"},     {"9V",  "Singapore"},
    {"9X",  "Rwanda"},       {"9Y",  "Trinidad"},
    {"A2",  "Botswana"},     {"A3",  "Tonga"},        {"A4",  "Oman"},
    {"A5",  "Bhutan"},       {"A6",  "UAE"},          {"A7",  "Qatar"},
    {"A9",  "Bahrain"},      {"AP",  "Pakistan"},
    {"BV",  "Taiwan"},       {"BY",  "China"},        {"C3",  "Andorra"},
    {"CE",  "Chile"},        {"CM",  "Cuba"},         {"CN",  "Morocco"},
    {"CP",  "Bolivia"},      {"CT",  "Portugal"},     {"CU",  "Azores"},
    {"CX",  "Uruguay"},      {"D2",  "Angola"},       {"D4",  "Cape Verde"},
    {"DS",  "S. Korea"},     {"DU",  "Philippines"},
    {"E7",  "Bosnia"},       {"EI",  "Ireland"},      {"EK",  "Armenia"},
    {"EL",  "Liberia"},      {"EP",  "Iran"},         {"ER",  "Moldova"},
    {"ES",  "Estonia"},      {"ET",  "Ethiopia"},     {"EU",  "Belarus"},
    {"EW",  "Belarus"},      {"EX",  "Kyrgyzstan"},   {"EY",  "Tajikistan"},
    {"EZ",  "Turkmenistan"},
    {"FK",  "New Caledonia"},{"FM",  "Martinique"},   {"FO",  "Fr. Polynesia"},
    {"FP",  "St Pierre"},    {"FR",  "Reunion"},      {"FT",  "Fr. S. Terr."},
    {"FY",  "Fr. Guiana"},
    {"HA",  "Hungary"},      {"HB",  "Switzerland"},  {"HC",  "Ecuador"},
    {"HG",  "Hungary"},      {"HH",  "Haiti"},        {"HI",  "Dom. Republic"},
    {"HK",  "Colombia"},     {"HL",  "S. Korea"},     {"HP",  "Panama"},
    {"HR",  "Honduras"},     {"HS",  "Thailand"},     {"HV",  "Vatican"},
    {"HZ",  "Saudi Arabia"},
    {"IS",  "Sardinia"},     {"JT",  "Mongolia"},     {"JW",  "Svalbard"},
    {"JX",  "Jan Mayen"},    {"JY",  "Jordan"},
    {"KL",  "Alaska"},       {"LA",  "Norway"},       {"LX",  "Luxembourg"},
    {"LY",  "Lithuania"},    {"LZ",  "Bulgaria"},
    {"OA",  "Peru"},         {"OD",  "Lebanon"},      {"OE",  "Austria"},
    {"OH",  "Finland"},      {"OM",  "Slovakia"},     {"ON",  "Belgium"},
    {"OX",  "Greenland"},    {"OY",  "Faroe Is."},    {"OZ",  "Denmark"},
    {"P2",  "Papua NG"},     {"P4",  "Aruba"},        {"PJ",  "Dutch Caribbean"},
    {"PZ",  "Suriname"},
    {"S2",  "Bangladesh"},   {"S5",  "Slovenia"},     {"S7",  "Seychelles"},
    {"S9",  "Sao Tome"},     {"SM",  "Sweden"},       {"SU",  "Egypt"},
    {"SV",  "Greece"},
    {"T2",  "Tuvalu"},       {"T3",  "Kiribati"},     {"T7",  "San Marino"},
    {"T9",  "Bosnia"},       {"TA",  "Turkey"},       {"TF",  "Iceland"},
    {"TG",  "Guatemala"},    {"TI",  "Costa Rica"},   {"TJ",  "Cameroon"},
    {"TK",  "Corsica"},      {"TL",  "C. Africa"},    {"TN",  "Congo"},
    {"TR",  "Gabon"},        {"TT",  "Chad"},         {"TU",  "Ivory Coast"},
    {"TY",  "Benin"},        {"TZ",  "Mali"},
    {"UA",  "Russia"},       {"UK",  "Uzbekistan"},   {"UN",  "Kazakhstan"},
    {"UP",  "Kazakhstan"},
    {"V2",  "Antigua"},      {"V3",  "Belize"},       {"V4",  "St Kitts"},
    {"V5",  "Namibia"},      {"V6",  "Micronesia"},   {"V7",  "Marshall Is."},
    {"V8",  "Brunei"},       {"VK",  "Australia"},    {"VR",  "Hong Kong"},
    {"VU",  "India"},
    {"XE",  "Mexico"},       {"XT",  "Burkina Faso"}, {"XU",  "Cambodia"},
    {"XW",  "Laos"},         {"XZ",  "Myanmar"},
    {"YA",  "Afghanistan"},  {"YB",  "Indonesia"},    {"YI",  "Iraq"},
    {"YJ",  "Vanuatu"},      {"YK",  "Syria"},        {"YL",  "Latvia"},
    {"YN",  "Nicaragua"},    {"YS",  "El Salvador"},  {"YV",  "Venezuela"},
    {"Z2",  "Zimbabwe"},     {"Z3",  "N. Macedonia"}, {"Z6",  "Kosovo"},
    {"Z8",  "S. Sudan"},     {"ZA",  "Albania"},      {"ZB",  "Gibraltar"},
    {"ZF",  "Cayman Is."},   {"ZK",  "Tokelau"},      {"ZL",  "New Zealand"},
    {"ZP",  "Paraguay"},     {"ZS",  "South Africa"},
    // ── 1-char prefixes (last resort) ────────────────────────────────────────
    {"F",   "France"},       {"G",   "England"},      {"I",   "Italy"},
    {"K",   "USA"},          {"M",   "England"},      {"N",   "USA"},
    {"R",   "Russia"},       {"W",   "USA"},
};
static const int PFX_CTY_LEN = sizeof(PFX_CTY) / sizeof(PFX_CTY[0]);

// Returns the country name for a callsign, or "" if unknown.
// Strips /suffix first, then tries prefixes of length 3, 2, 1.
const char* callCountry(const char* call) {
    char buf[14];
    strlcpy(buf, call, sizeof(buf));
    char* sl = strchr(buf, '/');
    if (sl) *sl = '\0';               // strip /P /B /M etc.
    int clen = (int)strlen(buf);
    for (int len = 3; len >= 1; len--) {
        if (clen < len) continue;
        for (int i = 0; i < PFX_CTY_LEN; i++) {
            if (strlen(PFX_CTY[i].pfx) == (size_t)len &&
                strncasecmp(buf, PFX_CTY[i].pfx, len) == 0)
                return PFX_CTY[i].name;
        }
    }
    return "";
}

// ─── Compass point from bearing (degrees 0–359) ───────────────────────────────
static const char* compassPt(uint16_t deg) {
    // 8 points, each 45°.  Offset by 22.5° so North is centred on 0/360.
    static const char* pts[8] = { "N","NE","E","SE","S","SW","W","NW" };
    return pts[((deg + 22) % 360) / 45];
}

// ─── Band name from frequency (kHz) ───────────────────────────────────────────
static const char* freqBand(float kHz) {
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
static uint32_t bandColor(const char* b) {
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
            if (b[1] == '0') return 0x43A047UL;  // 20m  — green (primary DX band)
            return 0xAB47BCUL;                    //  2m  — mauve
        case '7': return 0x26C6DAUL;              // 70cm — cyan
    }
    return COL_GREY_L;
}

static uint8_t classifyDXComment(const char* cmt) {
    if (!cmt || !cmt[0]) return SPOT_MODE_OTHER;
    char word[8];
    int j = 0;
    for (int i = 0; cmt[i] && cmt[i] != ' ' && j < 7; i++) word[j++] = cmt[i];
    word[j] = '\0';
    return classifyMode(word);
}

// ─── Main draw function ────────────────────────────────────────────────────────
void drawScreenDXSpots() {
    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    DXSpotsData ds = g_dxSpots;
    xSemaphoreGive(g_dataMutex);

    const int ROW_H = CONTENT_H / DX_SPOTS_MAX;

    if (!ds.valid || ds.count == 0) {
        drawLoader("Fetching DX Spots...");
        return;
    }

    // Apply mode filter
    DXSpot filtered[DX_SPOTS_MAX];
    int fc = 0;
    for (int i = 0; i < ds.count && fc < DX_SPOTS_MAX; i++) {
        if (g_settings.modeFilter & classifyDXComment(ds.spots[i].comment))
            filtered[fc++] = ds.spots[i];
    }

    if (fc == 0) {
        drawLoader("No spots match filter");
        return;
    }

    // ── Spot rows ─────────────────────────────────────────────────────────────
    for (int i = 0; i < fc; i++) {
        const DXSpot& s = filtered[i];
        int ry = CONTENT_Y + i * ROW_H;

        // Row background — subtle alternation
        spr.fillRect(0, ry, SCREEN_W, ROW_H, C((i & 1) ? 0x0C0C0CUL : COL_BG));
        spr.drawFastHLine(0, ry + ROW_H - 1, SCREEN_W, C(COL_BORDER));

        // Left-edge band colour bar
        const char* band = freqBand(s.freq);
        uint32_t    bc   = bandColor(band);
        spr.fillRect(0, ry, 4, ROW_H, C(bc));

        // ── Top line: DX callsign (left) | frequency + band (right) ──────────
        // Calculate freq string + width first so we know how much room is left.
        char fbuf[18];
        if (s.freq < 30000.0f)
            snprintf(fbuf, sizeof(fbuf), "%.3f %s", s.freq / 1000.0f, band);
        else
            snprintf(fbuf, sizeof(fbuf), "%.2f %s", s.freq / 1000.0f, band);
        spr.setFont(UI_FONT_9);
        int fw = spr.textWidth(fbuf);

        // Callsign
        spr.setFont(UI_FONT_12);
        spr.setTextColor(C(COL_WHITE));
        spr.setCursor(8, ry + 7);
        spr.print(s.dx);
        int callW = spr.textWidth(s.dx);

        // Country name + bearing/distance in smaller grey text, if it fits
        const char* cty = callCountry(s.dx);
        if (cty[0]) {
            char ctyBuf[36];
            if (s.dx_dist_km > 0 && s.dx_bearing != 0xFFFF) {
                const char* dir = compassPt(s.dx_bearing);
                if (g_settings.useKm) {
                    int km = (int)(s.dx_dist_km + 0.5f);
                    if (km >= 1000)
                        snprintf(ctyBuf, sizeof(ctyBuf), "(%s %d,%03dkm %s)",
                                 cty, km / 1000, km % 1000, dir);
                    else
                        snprintf(ctyBuf, sizeof(ctyBuf), "(%s %dkm %s)",
                                 cty, km, dir);
                } else {
                    int miles = (int)(s.dx_dist_km * 0.621371f + 0.5f);
                    if (miles >= 1000)
                        snprintf(ctyBuf, sizeof(ctyBuf), "(%s %d,%03dmi %s)",
                                 cty, miles / 1000, miles % 1000, dir);
                    else
                        snprintf(ctyBuf, sizeof(ctyBuf), "(%s %dmi %s)",
                                 cty, miles, dir);
                }
            } else {
                snprintf(ctyBuf, sizeof(ctyBuf), "(%s)", cty);
            }
            spr.setFont(UI_FONT_9);
            int ctyw = spr.textWidth(ctyBuf);
            int ctyX = 8 + callW + 4;
            if (ctyX + ctyw < SCREEN_W - fw - 10) {
                spr.setTextColor(C(COL_GREY));
                spr.setCursor(ctyX, ry + 10);   // +10 aligns DejaVu9 baseline with DejaVu12
                spr.print(ctyBuf);
            }
        }

        // Freq + band (right-aligned)
        spr.setFont(UI_FONT_9);
        spr.setTextColor(C(bc));
        spr.setCursor(SCREEN_W - fw - 5, ry + 9);
        spr.print(fbuf);

        // ── Bottom line: spotter (left) | comment (centre) | dist/time (right)
        spr.setFont(UI_FONT_9);

        char spBuf[18];
        snprintf(spBuf, sizeof(spBuf), "de %s", s.spotter);
        spr.setTextColor(C(COL_GREY));
        spr.setCursor(8, ry + 27);
        spr.print(spBuf);

        // Right: distance when known, else UTC time
        char rbuf[14];
        if (s.dist_km > 0) {
            if (g_settings.useKm) {
                int km = (int)(s.dist_km + 0.5f);
                if (km >= 1000) snprintf(rbuf, sizeof(rbuf), "%d,%03dkm", km / 1000, km % 1000);
                else            snprintf(rbuf, sizeof(rbuf), "%dkm", km);
            } else {
                int miles = (int)(s.dist_km * 0.621371f + 0.5f);
                if (miles >= 1000) snprintf(rbuf, sizeof(rbuf), "%d,%03dmi", miles / 1000, miles % 1000);
                else               snprintf(rbuf, sizeof(rbuf), "%dmi", miles);
            }
        } else {
            strlcpy(rbuf, s.time, sizeof(rbuf));
        }
        spr.setTextColor(C(s.dist_km > 0 ? COL_AMBER : COL_GREY_L));
        int rw = spr.textWidth(rbuf);
        spr.setCursor(SCREEN_W - rw - 5, ry + 27);
        spr.print(rbuf);

        // Comment fitted into the gap between spotter and right value
        if (s.comment[0]) {
            int usedL = spr.textWidth(spBuf) + 8;
            int usedR = rw + 10;
            int available = SCREEN_W - 8 - usedL - usedR;
            if (available > 16) {
                spr.setTextColor(C(COL_DIM_WHITE));
                spr.setCursor(8 + usedL, ry + 27);
                char cmt[sizeof(s.comment)];
                strlcpy(cmt, s.comment, sizeof(cmt));
                int clen = (int)strlen(cmt);
                while (clen > 0 && (int)spr.textWidth(cmt) > available)
                    cmt[--clen] = '\0';
                if (clen > 0) spr.print(cmt);
            }
        }
    }

    // ── Source watermark — bottom right corner ────────────────────────────────
    spr.setFont(UI_FONT_9);
    spr.setTextColor(C(0x444444UL));
    const char* src = "dxlite.g7vjr.org";
    int sw = spr.textWidth(src);
    spr.setCursor(SCREEN_W - sw - 4, SCREEN_H - 11);
    spr.print(src);
}
