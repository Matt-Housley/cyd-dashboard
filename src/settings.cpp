#include "settings.h"
#include <SPIFFS.h>
#include <ArduinoJson.h>

// ─── Globals ─────────────────────────────────────────────────────────────────
AppSettings      g_settings;
volatile bool    g_forceFetchWeather = false;
volatile bool    g_forceFetchTracker = false;

// ─── Timezone table ───────────────────────────────────────────────────────────
const TzEntry TZ_LIST[TZ_COUNT] = {
    { "London (GMT/BST)",       "GMT0BST,M3.5.0/1,M10.5.0",      "Europe/London"    },
    { "Dublin (GMT/IST)",       "GMT0IST,M3.5.0/1,M10.5.0",      "Europe/Dublin"    },
    { "Lisbon (WET/WEST)",      "WET0WEST,M3.5.0/1,M10.5.0",     "Europe/Lisbon"    },
    { "Paris (CET/CEST)",       "CET-1CEST,M3.5.0,M10.5.0/3",    "Europe/Paris"     },
    { "Berlin (CET/CEST)",      "CET-1CEST,M3.5.0,M10.5.0/3",    "Europe/Berlin"    },
    { "Athens (EET/EEST)",      "EET-2EEST,M3.5.0/3,M10.5.0/4",  "Europe/Athens"    },
    { "Moscow (MSK)",           "MSK-3",                          "Europe/Moscow"    },
    { "Dubai (GST)",            "GST-4",                          "Asia/Dubai"       },
    { "India (IST)",            "IST-5:30",                       "Asia/Kolkata"     },
    { "Bangkok (ICT)",          "ICT-7",                          "Asia/Bangkok"     },
    { "Singapore (SGT)",        "SGT-8",                          "Asia/Singapore"   },
    { "Tokyo (JST)",            "JST-9",                          "Asia/Tokyo"       },
    { "Sydney (AEST/AEDT)",     "AEST-10AEDT,M10.1.0,M4.1.0/3",  "Australia/Sydney" },
    { "Auckland (NZST/NZDT)",   "NZST-12NZDT,M9.5.0,M4.1.0/3",  "Pacific/Auckland" },
    { "UTC",                    "UTC0",                           "UTC"              },
    { "New York (EST/EDT)",     "EST5EDT,M3.2.0,M11.1.0",        "America/New_York" },
    { "Chicago (CST/CDT)",      "CST6CDT,M3.2.0,M11.1.0",        "America/Chicago"  },
    { "Denver (MST/MDT)",       "MST7MDT,M3.2.0,M11.1.0",        "America/Denver"   },
    { "Los Angeles (PST/PDT)",  "PST8PDT,M3.2.0,M11.1.0",        "America/Los_Angeles" },
    { "Anchorage (AKST/AKDT)", "AKST9AKDT,M3.2.0,M11.1.0",      "America/Anchorage" },
    { "Honolulu (HST)",         "HST10",                          "Pacific/Honolulu" },
    { "Sao Paulo (BRT/BRST)",  "BRT3BRST,M10.3.0/0,M2.3.0/0",   "America/Sao_Paulo" },
};

// Aliases — IANA zones that share identical rules with a TZ_LIST entry but
// have a different canonical name (common for nearby cities/regions).
struct TzAlias { const char* iana; int tzIndex; };
static const TzAlias TZ_ALIASES[] = {
    { "Europe/Belfast",       0 },  { "Europe/Jersey",        0 },
    { "Europe/Guernsey",      0 },  { "Europe/Isle_of_Man",   0 },
    { "Europe/Madrid",        3 },  { "Europe/Amsterdam",     3 },
    { "Europe/Brussels",      3 },  { "Europe/Rome",          3 },
    { "Europe/Vienna",        3 },  { "Europe/Zurich",        3 },
    { "Europe/Stockholm",     3 },  { "Europe/Oslo",          3 },
    { "Europe/Copenhagen",    3 },  { "Europe/Warsaw",        3 },
    { "Europe/Prague",        3 },  { "Europe/Budapest",      3 },
    { "Europe/Helsinki",      5 },  { "Europe/Bucharest",     5 },
    { "Europe/Kiev",          5 },  { "Europe/Sofia",         5 },
    { "Asia/Kuwait",          7 },  { "Asia/Muscat",          7 },
    { "Asia/Colombo",         8 },
    { "Asia/Jakarta",        10 },
    { "Asia/Kuala_Lumpur",   10 },  { "Asia/Manila",         10 },
    { "Asia/Hong_Kong",      10 },  { "Asia/Shanghai",       10 },
    { "Asia/Taipei",         10 },
    { "Asia/Seoul",          11 },
    { "Australia/Brisbane",  12 },  { "Australia/Melbourne", 12 },
    { "America/Detroit",     15 },  { "America/Toronto",     15 },
    { "America/Indianapolis",15 },  { "America/Nassau",      15 },
    { "America/Indiana/Indianapolis", 15 },
    { "America/Winnipeg",    16 },  { "America/Mexico_City", 16 },
    { "America/Edmonton",    17 },  { "America/Boise",       17 },
    { "America/Vancouver",   18 },  { "America/Tijuana",     18 },
    { "America/Juneau",      19 },
};
static const int TZ_ALIAS_COUNT = sizeof(TZ_ALIASES) / sizeof(TZ_ALIASES[0]);

int tzFindByIana(const char* iana) {
    if (!iana || !iana[0]) return -1;
    for (int i = 0; i < TZ_COUNT; i++)
        if (strcmp(TZ_LIST[i].iana, iana) == 0) return i;
    for (int i = 0; i < TZ_ALIAS_COUNT; i++)
        if (strcmp(TZ_ALIASES[i].iana, iana) == 0) return TZ_ALIASES[i].tzIndex;
    return -1;
}

// ─── Stock symbol table ───────────────────────────────────────────────────────
const SymEntry SYM_LIST[SYM_COUNT] = {
    { "S&P 500",   "%5EGSPC" },
    { "NASDAQ",    "%5EIXIC" },
    { "Dow Jones", "%5EDJI"  },
    { "FTSE 100",  "%5EFTSE" },
    { "Bitcoin",   "BTC-USD" },
    { "Gold",      "GC%3DF"  },
    { "Apple",     "AAPL"    },
    { "Tesla",     "TSLA"    },
};

// ─── Maidenhead → lat/lon ─────────────────────────────────────────────────────
void gridToLatLon(const char* grid, float& lat, float& lon) {
    if (!grid || strlen(grid) < 4) { lat = 0.0f; lon = 0.0f; return; }
    char g[7];
    strlcpy(g, grid, sizeof(g));
    g[0] = toupper((uint8_t)g[0]);
    g[1] = toupper((uint8_t)g[1]);
    if (strlen(g) >= 6) {
        g[4] = tolower((uint8_t)g[4]);
        g[5] = tolower((uint8_t)g[5]);
    }
    float lo = (g[0] - 'A') * 20.0f - 180.0f + (g[2] - '0') * 2.0f;
    float la = (g[1] - 'A') * 10.0f -  90.0f + (g[3] - '0') * 1.0f;
    if (strlen(g) >= 6) {
        lo += (g[4] - 'a') * (2.0f / 24.0f) + (1.0f / 24.0f);
        la += (g[5] - 'a') * (1.0f / 24.0f) + (0.5f / 24.0f);
    } else {
        lo += 1.0f;
        la += 0.5f;
    }
    lat = la;
    lon = lo;
}

// ─── Mode classification ──────────────────────────────────────────────────────
uint8_t classifyMode(const char* m) {
    if (!m || !m[0]) return SPOT_MODE_OTHER;
    if (strcasecmp(m, "CW")   == 0) return SPOT_MODE_CW;
    if (strcasecmp(m, "SSB")  == 0) return SPOT_MODE_SSB;
    if (strcasecmp(m, "LSB")  == 0) return SPOT_MODE_SSB;
    if (strcasecmp(m, "USB")  == 0) return SPOT_MODE_SSB;
    if (strcasecmp(m, "AM")   == 0) return SPOT_MODE_SSB;
    if (strcasecmp(m, "FM")   == 0) return SPOT_MODE_SSB;
    if (strcasecmp(m, "FT8")  == 0) return SPOT_MODE_FT8;
    if (strcasecmp(m, "FT4")  == 0) return SPOT_MODE_FT4;
    if (strcasecmp(m, "RTTY") == 0) return SPOT_MODE_RTTY;
    if (strcasecmp(m, "PSK")  == 0) return SPOT_MODE_RTTY;
    if (strcasecmp(m, "PSK31")== 0) return SPOT_MODE_RTTY;
    if (strcasecmp(m, "JT65") == 0) return SPOT_MODE_RTTY;
    if (strcasecmp(m, "JT9")  == 0) return SPOT_MODE_RTTY;
    if (strcasecmp(m, "JS8")  == 0) return SPOT_MODE_RTTY;
    if (strcasecmp(m, "WSPR") == 0) return SPOT_MODE_RTTY;
    return SPOT_MODE_OTHER;
}

// ─── Defaults ─────────────────────────────────────────────────────────────────
static void applyDefaults(AppSettings& s) {
    s.version = SETTINGS_VERSION;
    strlcpy(s.grid,        QTH_GRID,           sizeof(s.grid));
    s.lat = QTH_LAT;
    s.lon = QTH_LON;
    strlcpy(s.tz,          TZ_LONDON,          sizeof(s.tz));
    strlcpy(s.tzName,      "London (GMT/BST)", sizeof(s.tzName));
    for (int i = 0; i < NUM_SCREENS; i++) s.screenEnabled[i] = true;
    strlcpy(s.trackerSymbol, "%5EGSPC",        sizeof(s.trackerSymbol));
    strlcpy(s.trackerName,   "S&P 500",        sizeof(s.trackerName));
    s.trackerRangeYears = 4;
    strlcpy(s.callsign,    "M0KGO",            sizeof(s.callsign));
    s.modeFilter = SPOT_MODE_ALL;
    s.touchCalValid = false;
    memset(s.touchCal, 0, sizeof(s.touchCal));
}

// ─── Load ─────────────────────────────────────────────────────────────────────
void settingsLoad() {
    applyDefaults(g_settings);

    if (!SPIFFS.begin(true)) {
        Serial.println("[settings] SPIFFS mount failed — using defaults");
        return;
    }
    if (!SPIFFS.exists("/settings.json")) {
        Serial.println("[settings] no saved settings — using defaults");
        return;
    }

    File f = SPIFFS.open("/settings.json", "r");
    if (!f) { Serial.println("[settings] open failed"); return; }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        Serial.printf("[settings] JSON parse error: %s\n", err.c_str());
        return;
    }

    strlcpy(g_settings.grid,        doc["grid"]   | QTH_GRID,           sizeof(g_settings.grid));
    g_settings.lat =                 doc["lat"]    | (double)QTH_LAT;
    g_settings.lon =                 doc["lon"]    | (double)QTH_LON;
    strlcpy(g_settings.tz,          doc["tz"]     | TZ_LONDON,          sizeof(g_settings.tz));
    strlcpy(g_settings.tzName,      doc["tzName"] | "London (GMT/BST)", sizeof(g_settings.tzName));
    strlcpy(g_settings.trackerSymbol, doc["sym"]   | "%5EGSPC",          sizeof(g_settings.trackerSymbol));
    strlcpy(g_settings.trackerName,   doc["sname"] | "S&P 500",          sizeof(g_settings.trackerName));
    g_settings.trackerRangeYears =    doc["range"] | 4;
    strlcpy(g_settings.callsign,    doc["call"]   | "M0KGO", sizeof(g_settings.callsign));
    g_settings.modeFilter =          doc["modes"]  | (int)SPOT_MODE_ALL;

    JsonArray tc = doc["tcal"].as<JsonArray>();
    if (tc.size() == 8) {
        for (int i = 0; i < 8; i++) g_settings.touchCal[i] = tc[i].as<uint16_t>();
        g_settings.touchCalValid = true;
    }

    JsonArray en = doc["screens"].as<JsonArray>();
    for (int i = 0; i < NUM_SCREENS && i < (int)en.size(); i++)
        g_settings.screenEnabled[i] = en[i].as<bool>();

    // Ensure at least one screen is always enabled
    bool any = false;
    for (int i = 0; i < NUM_SCREENS; i++) if (g_settings.screenEnabled[i]) { any = true; break; }
    if (!any) for (int i = 0; i < NUM_SCREENS; i++) g_settings.screenEnabled[i] = true;

    Serial.printf("[settings] Loaded: grid=%s  tz=%s  sym=%s  %dy\n",
                  g_settings.grid, g_settings.tzName,
                  g_settings.trackerName, g_settings.trackerRangeYears);
}

// ─── Save ─────────────────────────────────────────────────────────────────────
void settingsSave() {
    if (!SPIFFS.begin(true)) { Serial.println("[settings] SPIFFS unavailable"); return; }

    JsonDocument doc;
    doc["grid"]   = g_settings.grid;
    doc["lat"]    = g_settings.lat;
    doc["lon"]    = g_settings.lon;
    doc["tz"]     = g_settings.tz;
    doc["tzName"] = g_settings.tzName;
    doc["sym"]    = g_settings.trackerSymbol;
    doc["sname"]  = g_settings.trackerName;
    doc["range"]  = g_settings.trackerRangeYears;
    doc["call"]   = g_settings.callsign;
    doc["modes"]  = g_settings.modeFilter;

    JsonArray en = doc["screens"].to<JsonArray>();
    for (int i = 0; i < NUM_SCREENS; i++) en.add(g_settings.screenEnabled[i]);

    if (g_settings.touchCalValid) {
        JsonArray tc = doc["tcal"].to<JsonArray>();
        for (int i = 0; i < 8; i++) tc.add(g_settings.touchCal[i]);
    }

    File f = SPIFFS.open("/settings.json", "w");
    if (!f) { Serial.println("[settings] write open failed"); return; }
    serializeJson(doc, f);
    f.close();
    Serial.println("[settings] Saved OK");
}
