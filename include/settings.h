#pragma once
#include <Arduino.h>
#include "config.h"

#define SETTINGS_VERSION 1
#define TZ_COUNT         22

/// A selectable timezone (table declared here, defined in settings.cpp).
struct TzEntry {
    const char* name;    ///< Human-readable name (e.g. "London (GMT/BST)").
    const char* posix;   ///< POSIX TZ string for setenv("TZ", ...).
    const char* iana;    ///< Primary IANA zone name for auto-detect matching.
};
extern const TzEntry TZ_LIST[TZ_COUNT];

/**
 * @brief Find the TZ_LIST index for an IANA zone name.
 * @param iana  IANA zone name (e.g. "Europe/London") as returned by Open-Meteo.
 * @return Index into TZ_LIST, or -1 if no match is found.
 */
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
/// All user-configurable settings, serialised to /settings.json on SPIFFS.
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
    bool     issLedEnabled;               // flash onboard green LED when ISS is visible
    bool     lightningLedEnabled;         // flash onboard red LED on a lightning alert
    bool     startupLedEnabled;           // flash onboard blue LED while data loads at boot
    bool     screenshotHoldEnabled;       // 3-second press saves a screenshot to SD
    uint8_t  brightness;                  // backlight level 0-255
    bool     useCelsius;                  // true = °C, false = °F
    bool     useKm;                       // true = km / km/h, false = miles / mph
    uint16_t ft8BandFilter;               // FT8 Spots: 1 bit per band (see screen_ft8spots)
    float    issFreqSstv;                 // ISS SSTV downlink (MHz)
    float    issFreqAprs;                 // ISS APRS downlink (MHz)
    float    issFreqRptIn;                // ISS voice repeater input / uplink (MHz)
    float    issFreqRptOut;               // ISS voice repeater output / downlink (MHz)
};

// FT8 Spots band filter: 14 amateur bands, one bit each. All-on default.
#define FT8_BAND_COUNT 14
#define FT8_BAND_ALL   0x3FFF

/**
 * @brief Classify a mode string into a SPOT_MODE_* bit.
 * @param mode  Mode label (e.g. "CW", "FT8", "SSB"); case-insensitive.
 * @return The matching SPOT_MODE_* bit, or SPOT_MODE_OTHER if unrecognised.
 */
uint8_t classifyMode(const char* mode);

/// The one global settings instance (loaded at boot, saved on change).
extern AppSettings g_settings;

/// Set true from the main loop to request an out-of-schedule weather refetch;
/// cleared by the fetch task once serviced.
extern volatile bool g_forceFetchWeather;
/// As g_forceFetchWeather, for the stock tracker.
extern volatile bool g_forceFetchTracker;

/**
 * @brief Load g_settings from SPIFFS, applying built-in defaults first.
 *
 * If no saved file exists (or it fails to parse) the defaults are kept.
 */
void settingsLoad();

/** @brief Persist g_settings to SPIFFS as JSON. */
void settingsSave();

/**
 * @brief Convert a Maidenhead locator to its centre-point latitude/longitude.
 * @param grid      4- or 6-character Maidenhead locator (e.g. "IO92js").
 * @param[out] lat  Receives the centre latitude in degrees.
 * @param[out] lon  Receives the centre longitude in degrees.
 */
void gridToLatLon(const char* grid, float& lat, float& lon);
