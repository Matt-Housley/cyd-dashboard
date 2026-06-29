#pragma once
#include <stdint.h>   // uint8_t needed by enum ScreenID

// ─── Version ─────────────────────────────────────────────────────────────────
#define VERSION_MAJOR  1
#define VERSION_MINOR  15
#define VERSION_PATCH  5
#define VERSION_STR    "1.15.005"

// ─── Display ──────────────────────────────────────────────────────────────────
#define SCREEN_W  320
#define SCREEN_H  240

// Status bar occupies the top strip; content sits below it
#define STATUS_H   16
#define CONTENT_Y  STATUS_H
#define CONTENT_H  (SCREEN_H - STATUS_H)

// ─── Palette (RGB888) ─────────────────────────────────────────────────────────
#define COL_BG          0x0D0D0DUL
#define COL_PANEL       0x141414UL
#define COL_BORDER      0x222222UL
#define COL_ORANGE      0xFF6B00UL
#define COL_ORANGE_D    0xCC5500UL
#define COL_AMBER       0xFFB300UL
#define COL_GREEN       0x4CAF50UL
#define COL_RED         0xF44336UL
#define COL_BLUE        0x2196F3UL
#define COL_GREY        0x888888UL
#define COL_GREY_L      0xAAAAAAUL
#define COL_WHITE       0xF0F0F0UL
#define COL_DIM_WHITE   0xCCCCCCUL
#define COL_APPLE_GREY  0xA2AAADul
#define COL_BBC_RED     0xBB1919UL
#define COL_AMSAT_BLUE  0x1565C0UL
#define COL_OCEAN       0x0A1628UL
#define COL_LAND        0x1A3A1AUL
#define COL_LAND_EDGE   0x2D6020UL
#define COL_SUN         0xFFE040UL

// ─── Location — IO92js ───────────────────────────────────────────────────────
#define QTH_LAT   52.77f
#define QTH_LON   (-1.21f)
#define QTH_GRID  "IO92js"

// ─── Timing ───────────────────────────────────────────────────────────────────
#define CYCLE_MS              8000UL    // Auto-advance interval
#define PAUSE_AFTER_SWIPE_MS 15000UL   // Resume auto after manual nav
#define PAUSE_ON_TAP_MS      30000UL   // Tap to pause

#define SWIPE_THRESH          40        // px
#define SWIPE_MAX_MS         800        // ignore slow drags

// Data refresh intervals (milliseconds)
#define REFRESH_WEATHER_MS   600000UL   // 10 min
#define REFRESH_SOLAR_MS     900000UL   // 15 min
#define REFRESH_NEWS_MS      600000UL   // 10 min
#define REFRESH_TRACKER_MS   120000UL   //  2 min
#define REFRESH_ISS_MS        30000UL   // 30 s  (ISS orbits in ~92 min)
#define REFRESH_DXSPOTS_MS    60000UL   // 60 s
#define REFRESH_POTASPOTS_MS  60000UL   // 60 s
#define REFRESH_SOTASPOTS_MS  60000UL   // 60 s
#define REFRESH_CONTESTS_MS 3600000UL   // 60 min
#define REFRESH_PSK_MS       120000UL   //  2 min

// ─── Network ──────────────────────────────────────────────────────────────────
#define WIFI_AP_NAME  "CYD-Dashboard"
#define NTP_SERVER1   "pool.ntp.org"
#define NTP_SERVER2   "time.google.com"
// POSIX TZ string for Europe/London — handles BST/GMT automatically
#define TZ_LONDON     "GMT0BST,M3.5.0/1,M10.5.0"

// API endpoints
// ISS uses plain HTTP (api.open-notify.org has no HTTPS); everything else uses HTTPS.
#define API_SOLAR     "https://www.hamqsl.com/solarxml.php"
#define API_BBC_RSS   "https://feeds.bbci.co.uk/news/rss.xml"
#define API_APPLE_RSS "https://feeds.macrumors.com/MacRumors-Main"
#define API_TRACKER   "https://query1.finance.yahoo.com/v8/finance/chart/%5EGSPC?interval=1wk&range=4y"
#define API_ISS       "http://api.open-notify.org/iss-now.json"
// plain HTTP — dxlite.g7vjr.org does not serve HTTPS
// noft8=1 suppresses FT4/FT8 clutter so phone/CW/data spots dominate
#define API_DXSPOTS     "http://dxlite.g7vjr.org/?xml=1&noft8=1&limit=50"
#define API_POTASPOTS   "https://api.pota.app/spot/activator"
#define API_SOTASPOTS   "https://api2.sota.org.uk/api/spots/15"
#define API_CONTESTS    "https://www.contestcalendar.com/calendar.rss"
// %s is replaced with the sender callsign at runtime.
// lastSeconds=900 = 15-minute window (correct parameter; NOT lastDuration).
// rronly=1 omits the large <activeReceiver> section — reception reports only.
// NOTE: http:// does NOT work — the server redirects to https:// and the plain
//       HTTP path in httpGet() cannot follow an HTTP→HTTPS redirect.
#define API_PSKREPORTER "https://retrieve.pskreporter.info/query?senderCallsign=%s&lastSeconds=900&rronly=1"
// ─── Screen IDs ───────────────────────────────────────────────────────────────
#define NUM_SCREENS  13

enum ScreenID : uint8_t {
    SCR_CLOCK = 0,
    SCR_WEATHER,
    SCR_HF_NOW,
    SCR_PROPAGATION,
    SCR_GREYLINE,
    SCR_PSKREPORTER,
    SCR_DXSPOTS,
    SCR_POTASPOTS,
    SCR_SOTASPOTS,
    SCR_CONTESTS,
    SCR_BBC,
    SCR_APPLE,
    SCR_TRACKER,
};
