#pragma once
#include <Arduino.h>
#include "config.h"

#define SETTINGS_VERSION 1
#define TZ_COUNT         22

// ─── Timezone table (declared here, defined in settings.cpp) ─────────────────
struct TzEntry {
    const char* name;    // human-readable
    const char* posix;   // POSIX TZ string for setenv("TZ",...)
    const char* iana;    // primary IANA zone name for auto-detect matching
};
extern const TzEntry TZ_LIST[TZ_COUNT];

// Match an IANA zone name (as returned by Open-Meteo) to a TZ_LIST entry.
// Returns -1 if no match found.
int tzFindByIana(const char* iana);

// ─── Stock symbol presets ─────────────────────────────────────────────────────
#define SYM_COUNT 8
struct SymEntry {
    const char* name;   // display label
    const char* url;    // URL-encoded Yahoo Finance ticker
};
extern const SymEntry SYM_LIST[SYM_COUNT];

// ─── Mode filter bits ────────────────────────────────────────────────────────
#define SPOT_MODE_CW    (1 << 0)
#define SPOT_MODE_SSB   (1 << 1)
#define SPOT_MODE_FT8   (1 << 2)
#define SPOT_MODE_FT4   (1 << 3)
#define SPOT_MODE_RTTY  (1 << 4)
#define SPOT_MODE_OTHER (1 << 5)
#define SPOT_MODE_ALL   0x3F

// ─── Persistent settings ──────────────────────────────────────────────────────
struct AppSettings {
    uint8_t version;
    char    grid[8];                      // Maidenhead e.g. "IO92js"
    float   lat;
    float   lon;
    char    tz[48];                       // POSIX TZ string
    char    tzName[32];                   // human-readable timezone name
    bool    screenEnabled[NUM_SCREENS];   // per-screen on/off
    char    trackerSymbol[16];           // URL-encoded Yahoo ticker e.g. "%5EGSPC"
    char    trackerName[24];             // display name e.g. "S&P 500"
    uint8_t trackerRangeYears;           // 1-5
    char    callsign[14];                // amateur callsign for PSK Reporter
    uint8_t  modeFilter;                  // bitmask of enabled modes (MODE_*)
    bool     touchCalValid;               // true if touchCal has been set
    uint16_t touchCal[8];                 // LovyanGFX calibration parameters
    bool     issJumpEnabled;              // jump to Grey Line screen during ISS passes
    uint8_t  brightness;                  // backlight level 0-255
};

// Classify a mode string into a MODE_* bit
uint8_t classifyMode(const char* mode);

extern AppSettings g_settings;

// Force-refetch flags — set from main loop, cleared by fetch task
extern volatile bool g_forceFetchWeather;
extern volatile bool g_forceFetchTracker;

// Load from SPIFFS (writes defaults if no file exists)
void settingsLoad();

// Persist g_settings to SPIFFS
void settingsSave();

// Derive lat/lon centre-point from a 4- or 6-char Maidenhead locator
void gridToLatLon(const char* grid, float& lat, float& lon);
