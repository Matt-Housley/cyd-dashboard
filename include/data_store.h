#pragma once
#include <Arduino.h>
#include <freertos/semphr.h>

// ─── Weather ──────────────────────────────────────────────────────────────────
struct DayForecast {
    char     day[4];      // "Mon"
    uint8_t  code;
    int8_t   maxT;
    int8_t   minT;
    float    precip;      // mm
    uint8_t  windMph;     // max wind speed mph
    uint16_t windDir;     // dominant wind direction, degrees (0=N, 90=E, 180=S, 270=W)
};

struct WeatherData {
    bool     valid     = false;
    int8_t   temp      = 0;
    uint8_t  humidity  = 0;
    uint8_t  windMph   = 0;
    uint8_t  code      = 0;
    char     rainIn[16]= "?";
    int8_t   lightningHours = -1;   // hours until first thunder (-1 = none in 24h)
    uint8_t  lightningPct   = 0;    // % of next 24h hourly slots with WMO code ≥ 95
    DayForecast daily[7];
    uint8_t  dayCount  = 0;
};

// ─── Solar / HF propagation ───────────────────────────────────────────────────
struct BandCond {
    char band[5];       // "20m"
    char day[12];       // "Good"
    char night[12];     // "Fair"
};

struct SolarData {
    bool  valid        = false;
    int   sfi          = 0;
    int   aIndex       = 0;
    int   kIndex       = 0;
    char  xray[4]      = "?";
    int   sunspots     = 0;
    char  geoField[16] = "?";
    char  signalNoise[16] = "?";
    BandCond bands[7];
    uint8_t  bandCount = 0;
};

// ─── News ─────────────────────────────────────────────────────────────────────
struct NewsItem {
    char title[100];
    char thumbUrl[160];
};

struct NewsData {
    bool     valid = false;
    NewsItem items[4];   // screens show 1 hero + 3 sub-rows = 4 max
    uint8_t  count = 0;
};

// ─── Stock / index ────────────────────────────────────────────────────────────
#define TRACKER_POINTS 220

struct TrackerData {
    bool    valid     = false;
    float   price     = 0.0f;
    float   change    = 0.0f;
    float   changePct = 0.0f;
    float   history[TRACKER_POINTS];
    uint8_t histCount = 0;
};

// ─── ISS position + upcoming orbit track ─────────────────────────────────────
struct ISSData {
    bool  valid      = false;
    float lat        = 0.0f;
    float lon        = 0.0f;
    // 32 points × 180 s ≈ one full orbit (~96 min) of ground-track prediction
    float trackLat[32];
    float trackLon[32];
    int   trackCount = 0;

    // Next visible pass from QTH (MIN_EL = 5°), computed in fetchISS()
    bool    passValid      = false;
    bool    passNow        = false;   // ISS is currently above horizon
    int32_t passRiseSec    = 0;       // UNIX time of rise (or now if passNow)
    int32_t passSetSec     = 0;       // UNIX time of set
    int16_t passRiseAz     = 0;       // rise azimuth, degrees 0-359
    int16_t passSetAz      = 0;       // set azimuth, degrees 0-359
    int8_t  passMaxEl      = 0;       // peak elevation during pass, degrees
};

// ─── DX Spots ─────────────────────────────────────────────────────────────────
// Spots are sourced from dxlite.g7vjr.org every 60 s and sorted by the
// great-circle distance from the user's QTH to the spotter's location, so the
// screen shows "what DX is being heard near me right now."
#define DX_SPOTS_MAX 5

struct DXSpot {
    char     dx[12];       // spotted (DX) callsign,  e.g. "VK9DX"
    char     spotter[12];  // spotter callsign,        e.g. "G4ZFE"
    float    freq;         // frequency in kHz
    char     comment[28];  // mode / free-text comment (truncated)
    char     time[8];      // UTC time string,         e.g. "14:03Z"
    uint16_t dist_km;      // spotter distance from QTH; 0 = unknown
    uint16_t dx_dist_km;   // DX station distance from QTH; 0 = unknown
    uint16_t dx_bearing;   // bearing to DX (degrees 0–359); 0xFFFF = unknown
};

struct DXSpotsData {
    bool    valid = false;
    DXSpot  spots[DX_SPOTS_MAX];
    uint8_t count = 0;
};

// ─── POTA Spots ───────────────────────────────────────────────────────────────
// Sourced from api.pota.app/spot/activator every 60 s.
// The API returns full lat/lon per spot, so distance is calculated directly.
#define POTA_SPOTS_MAX 5

struct POTASpot {
    char     activator[14];  // activator callsign, e.g. "DL/HB9XQH"
    char     reference[10];  // park reference,     e.g. "DE-0421"
    char     parkName[32];   // park name, truncated
    float    freq;           // frequency in kHz
    char     mode[6];        // "FT8", "CW", "SSB" …
    char     time[8];        // UTC, e.g. "11:07Z"
    uint16_t dist_km;        // distance from QTH in km; 0 = unknown
};

struct POTASpotsData {
    bool     valid = false;
    POTASpot spots[POTA_SPOTS_MAX];
    uint8_t  count = 0;
};

// ─── SOTA Spots ───────────────────────────────────────────────────────────────
// Sourced from api2.sota.org.uk every 60 s.  No lat/lon in the response, so
// distance is estimated from the activator callsign prefix via pfxToLatLon().
#define SOTA_SPOTS_MAX 5

struct SOTASpot {
    char     activator[14];  // activator callsign,  e.g. "HB9TNF/P"
    char     summit[12];     // summit reference,    e.g. "HB/SZ-018"
    char     name[28];       // summit name + alt,   e.g. "Fronalpstock 1921m"
    float    freq;           // frequency in MHz (as supplied by API)
    char     mode[6];        // "SSB", "CW", "FM" …
    char     time[8];        // UTC, e.g. "11:20Z"
    uint16_t dist_km;        // approx distance from QTH; 0 = unknown
};

struct SOTASpotsData {
    bool     valid = false;
    SOTASpot spots[SOTA_SPOTS_MAX];
    uint8_t  count = 0;
};

// ─── PSK Reporter reception spots ────────────────────────────────────────────
// Who received our callsign in the last 15 minutes, sourced from
// retrieve.pskreporter.info.  Deduplicated by receiver callsign; up to
// PSK_REPORTS_MAX unique stations are stored.
#define PSK_REPORTS_MAX 50

struct PSKReport {
    char   call[14];    // receiver callsign
    char   grid[8];     // receiver Maidenhead locator
    float  lat;         // receiver latitude  (derived from grid)
    float  lon;         // receiver longitude (derived from grid)
    float  freqMHz;     // frequency in MHz
    int8_t snr;         // SNR in dB
};

struct PSKData {
    bool      valid = false;
    bool      fetchFailed = false;  // BISECT: keeping, only 1 byte
    uint8_t   total = 0;     // unique receivers parsed before cap
    PSKReport reports[PSK_REPORTS_MAX];
    uint8_t   count = 0;
};

// ─── Contest Calendar ─────────────────────────────────────────────────────────
// Sourced from contestcalendar.com/calendar.rss every 60 min.
// Active contests (now within start–end window) are shown first, then upcoming.
#define CONTEST_MAX 8

struct Contest {
    char  name[42];     // contest title, truncated to fit
    char  times[36];    // raw description, e.g. "0000Z, May 30 to 2359Z, May 31"
    bool  active;       // true = happening right now
    char  ref[16];      // contestcalendar.com detail ref param
};

struct ContestData {
    bool    valid = false;
    Contest items[CONTEST_MAX];
    uint8_t count = 0;
};

struct ContestDetail {
    bool  valid;
    bool  failed;
    char  name[42];
    char  mode[24];
    char  bands[36];
    char  exchange[48];
};

extern ContestDetail    g_contestDetail;
extern volatile int8_t  g_contestDetailReq;

// ─── Timezone auto-detect (looked up from Open-Meteo on location save) ───────
struct TzLookupResult {
    bool  valid  = false;   // result ready (success or failure)
    bool  failed = false;   // true = lookup failed or no match found
    char  tzName[32] = "";  // matched TZ_LIST human-readable name
    char  tzPosix[48] = ""; // matched TZ_LIST POSIX string
};

extern TzLookupResult   g_tzLookup;
extern volatile bool    g_tzLookupReq;   // true = lookup requested
extern volatile float   g_tzLookupLat;
extern volatile float   g_tzLookupLon;

// ─── Globals (defined in main.cpp) ───────────────────────────────────────────
extern WeatherData      g_weather;
extern SolarData        g_solar;
extern NewsData         g_bbcNews;
extern NewsData         g_appleNews;
extern TrackerData      g_tracker;
extern ISSData          g_iss;
extern DXSpotsData      g_dxSpots;
extern PSKData          g_pskData;
extern POTASpotsData    g_potaSpots;
extern SOTASpotsData    g_sotaSpots;
extern ContestData      g_contests;
extern SemaphoreHandle_t g_dataMutex;
