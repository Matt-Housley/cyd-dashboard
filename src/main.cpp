#include <Arduino.h>
#include <WiFiManager.h>
#include <SPIFFS.h>
#include <time.h>
#include <esp_heap_caps.h>
#include "soc/rtc_cntl_reg.h"

#include "lgfx_config.h"
#include "config.h"
#include "data_store.h"
#include "fetch.h"
#include "ui.h"
#include "settings.h"
#include "screen_settings.h"
#include "screen_pskreporter.h"
#include "screen_ft8spots.h"
#include "screen_contests.h"
#include "screen_hf_now.h"
#include "screen_greyline.h"
#include "screen_weather.h"
#include "screen_bbc.h"
#include "screen_apple.h"
#include "service.h"
#include "fonts/ui_fonts.h"
#include <esp_wifi.h>

// ─── Global instances ─────────────────────────────────────────────────────────
LGFX              tft;
WeatherData       g_weather;
SolarData         g_solar;
NewsData          g_bbcNews;
NewsData          g_appleNews;
TrackerData       g_tracker;
ISSData           g_iss;
DXSpotsData       g_dxSpots;
PSKData           g_pskData;
FT8SpotsData      g_ft8Spots;
POTASpotsData     g_potaSpots;
SOTASpotsData     g_sotaSpots;
ContestData       g_contests;
ContestDetail     g_contestDetail = {};
volatile int8_t   g_contestDetailReq = -1;
TzLookupResult    g_tzLookup = {};
volatile bool     g_tzLookupReq = false;
volatile float    g_tzLookupLat = 0.0f;
volatile float    g_tzLookupLon = 0.0f;
SemaphoreHandle_t g_dataMutex;

// ─── Fetch state (declared in data_store.h) ───────────────────────────────────
volatile FetchState g_fetchState[FSRC_COUNT] = {};   // all IDLE at boot

// ─── SSL pre-reserves (declared in fetch.h) ───────────────────────────────────
// Two blocks are allocated in setup() immediately after the sprite, while the
// remaining ~34–51 KB of the largest heap block is still contiguous.
// They are HELD until the last possible moment before each TLS connection, then
// freed and reclaimed by fetch.cpp so nothing can nibble those slots.
//
//   g_sslX509R  18432 B  — (SSL_X509_RESERVE) freed just before http.GET() for
//                           the mbedTLS input record buffer (16717 B) + cert-
//                           chain parse. Sized to exceed TLS_MIN_CONTIG on its
//                           own so it doesn't depend on a fragmented neighbour
//                           coalescing; any adjacent free block adds margin.
//
//   g_sslCtxR     6136 B  — (SSL_CTX_RESERVE) freed just before WiFiClientSecure
//                           constructor; new sslclient_context() first-fits here.
void* g_sslX509R   = nullptr;
void* g_sslCtxR    = nullptr;

// ─── Fetch task — static stack ────────────────────────────────────────────────
// Declared in .bss so the 8 KB stack uses ZERO heap.  Dynamic allocation via
// xTaskCreatePinnedToCore fails after WiFi init because ~50 KB of heap is
// consumed and no contiguous 8 KB block survives.  Static allocation side-steps
// the problem entirely: WiFi sees the same heap it always had.
static StaticTask_t s_fetchTaskBuffer;
static StackType_t  s_fetchTaskStack[12288];  // StackType_t = uint8_t on ESP32

// ─── Navigation state ─────────────────────────────────────────────────────────
static uint8_t       g_screen       = SCR_CLOCK;
static bool          g_autoPlay     = true;
static unsigned long g_lastCycle    = 0;
static unsigned long g_pauseUntil   = 0;
static bool          g_inSettings   = false;
static bool          g_issPassActive = false;  // true while auto-held on Grey Line for ISS pass

// Service-menu runtime "fake" toggles (see service.h). Not persisted.
bool g_fakeLightning = false;
bool g_fakeISSPass   = false;

// ─── Touch / swipe state ──────────────────────────────────────────────────────
static int32_t       g_touchStartX        = 0;
static int32_t       g_touchStartY        = 0;
static int32_t       g_touchLastX         = 0;  // updated every frame while finger is down
static int32_t       g_touchLastY         = 0;
static unsigned long g_touchDownMs        = 0;
static bool          g_touching           = false;
static bool          g_longPressTriggered = false;  // settings opened during hold

// ─── Live touch accessors (declared in ui.h) ──────────────────────────────────
/** @brief Whether a finger is currently down this frame (see ui.h). @return true if touching. */
bool    touchIsActive()  { return g_touching; }
/** @brief Current touch x while active (see ui.h). @return X in screen pixels. */
int32_t touchCurrentX()  { return g_touchLastX; }
/** @brief Current touch y while active (see ui.h). @return Y in screen pixels. */
int32_t touchCurrentY()  { return g_touchLastY; }

// ─── Splash helper ────────────────────────────────────────────────────────────
// All text uses proportional fonts (see fonts/ui_fonts.h); widths via tft.textWidth().
// Layout (all y values assume callsign is present):
//   y= 18  Callsign        — UI_FONT_40, orange
//   y= 66  "CYD Dashboard" — UI_FONT_18, orange
//   y= 90  Grid locator    — UI_FONT_12, grey
//   y=112  line1           — UI_FONT_12, light grey
//   y=128  line2           — UI_FONT_12, orange  (optional)
// If no callsign is set the title/grid/lines drop back to the original positions.
/**
 * @brief Draw the boot splash screen directly to the display (not the sprite).
 *
 * Shows the callsign, "CYD Dashboard", grid locator, and one or two status
 * lines, plus the firmware version. Used during boot before the UI is running.
 * @param line1  Primary status line (e.g. "Connecting to WiFi...").
 * @param line2  Optional second status line; pass nullptr to omit.
 */
static void splash(const char* line1, const char* line2 = nullptr) {
    tft.fillScreen(tft.color888(13, 13, 13));

    bool hasCall = g_settings.callsign[0] != '\0';
    int  yShift  = hasCall ? 0 : 6;   // nudge everything down when no callsign

    // ── Callsign ──────────────────────────────────────────────────────────────
    if (hasCall) {
        tft.setFont(UI_FONT_40);
        tft.setTextColor(tft.color888(0xFF, 0x6B, 0x00));
        int cw = tft.textWidth(g_settings.callsign);
        tft.setCursor((320 - cw) / 2, 18);
        tft.print(g_settings.callsign);
    }

    // ── "CYD Dashboard" subtitle ──────────────────────────────────────────────
    tft.setFont(UI_FONT_18);
    tft.setTextColor(tft.color888(0xFF, 0x6B, 0x00));
    int tw = tft.textWidth("CYD Dashboard");
    tft.setCursor((320 - tw) / 2, 66 + yShift);
    tft.print("CYD Dashboard");

    // ── Grid locator ──────────────────────────────────────────────────────────
    tft.setFont(UI_FONT_12);
    tft.setTextColor(tft.color888(0x88, 0x88, 0x88));
    int gw = tft.textWidth(g_settings.grid);
    tft.setCursor((320 - gw) / 2, 90 + yShift);
    tft.print(g_settings.grid);

    // ── Status lines ──────────────────────────────────────────────────────────
    tft.setTextColor(tft.color888(0xCC, 0xCC, 0xCC));
    int lw = tft.textWidth(line1);
    tft.setCursor((320 - lw) / 2, 112 + yShift);
    tft.print(line1);

    if (line2) {
        tft.setTextColor(tft.color888(0xFF, 0x6B, 0x00));
        int lw2 = tft.textWidth(line2);
        tft.setCursor((320 - lw2) / 2, 128 + yShift);
        tft.print(line2);
    }

    tft.setFont(UI_FONT_9);
    tft.setTextColor(tft.color888(0x44, 0x44, 0x44));
    tft.setCursor(4, 228);
    tft.print("v" VERSION_STR);
}

// ─── WiFi signal helper (called by drawStatusBar in ui.cpp) ──────────────────
/**
 * @brief Map the current WiFi RSSI to a 0–4 signal-bar count.
 *
 * Declared extern in ui.cpp so WiFi.h never needs to enter that translation unit.
 * @return 0 when not connected; 1–4 bars by RSSI when connected.
 */
int getWiFiBars() {
    if (WiFi.status() != WL_CONNECTED) return 0;
    int rssi = WiFi.RSSI();
    if (rssi > -55) return 4;
    if (rssi > -65) return 3;
    if (rssi > -75) return 2;
    return 1;
}

// ─── WiFiManager callbacks ────────────────────────────────────────────────────
/**
 * @brief WiFiManager callback: shown when the captive-portal AP starts.
 *
 * Draws a splash telling the user which AP to join and where to browse.
 * @param wm  The WiFiManager instance entering AP mode (unused).
 */
static void onAPMode(WiFiManager* wm) {
    splash("Connect to WiFi AP:", WIFI_AP_NAME);
    // splash() leaves the font at UI_FONT_12 — use it for the hint line too
    tft.setTextColor(tft.color888(0x88, 0x88, 0x88));
    const char* hint = "then open 192.168.4.1";
    int hw = tft.textWidth(hint);
    tft.setCursor((320 - hw) / 2, 150);
    tft.print(hint);
}

// ─── Next enabled screen ──────────────────────────────────────────────────────
/**
 * @brief Find the next enabled screen in a given direction, wrapping around.
 * @param from  Current screen index.
 * @param dir   +1 for forward, -1 for backward.
 * @return The next enabled screen index; returns @p from if none are enabled.
 */
static int nextEnabledScreen(int from, int dir) {
    int s = from;
    for (int i = 0; i < NUM_SCREENS; i++) {
        s = (s + dir + NUM_SCREENS) % NUM_SCREENS;
        if (g_settings.screenEnabled[s]) return s;
    }
    return from;   // fallback: all disabled (shouldn't happen)
}

// ─── Screen navigation ────────────────────────────────────────────────────────
/**
 * @brief Switch to a screen, clearing per-screen overlay/selection state.
 * @param s        Target screen index (taken modulo NUM_SCREENS).
 * @param pauseMs  Optional auto-cycle pause after switching, in milliseconds.
 */
static void goTo(int s, unsigned long pauseMs = 0) {
    if ((uint8_t)(s % NUM_SCREENS) != g_screen) {
        pskClearSelection();
        ft8ClearSelection();
        contestClearSelection();
        hideTextPopup();
    }
    g_screen    = (uint8_t)(s % NUM_SCREENS);
    g_lastCycle = millis();
    if (pauseMs) g_pauseUntil = millis() + pauseMs;
}

/** @brief Advance to the next enabled screen (swipe-left), pausing auto-cycle. */
static void nextScreen() { goTo(nextEnabledScreen(g_screen, +1), PAUSE_AFTER_SWIPE_MS); }
/** @brief Go to the previous enabled screen (swipe-right), pausing auto-cycle. */
static void prevScreen() { goTo(nextEnabledScreen(g_screen, -1), PAUSE_AFTER_SWIPE_MS); }

// ─── Apply settings after change ─────────────────────────────────────────────
/**
 * @brief Apply settings changes that take effect immediately after leaving the
 *        settings UI: timezone, backlight, jump-off-disabled-screen, and forcing
 *        a weather/tracker refetch.
 */
static void applySettings() {
    // Timezone
    setenv("TZ", g_settings.tz, 1);
    tzset();

    // Backlight
    tft.setBrightness(g_settings.brightness);

    // Jump away if current screen was just disabled
    if (!g_settings.screenEnabled[g_screen])
        g_screen = (uint8_t)nextEnabledScreen(g_screen, +1);

    // Re-fetch data that depends on changed settings
    g_forceFetchWeather = true;
    g_forceFetchTracker = true;
}

// ─── Touch handler ────────────────────────────────────────────────────────────
/**
 * @brief Poll the touch panel and dispatch taps, swipes, and the screenshot hold.
 *
 * Runs once per loop. Tracks finger down/move/up, detects a stationary 3 s hold
 * (screenshot-to-SD when enabled), and on finger-up classifies the gesture:
 * horizontal swipes navigate screens, and taps are routed to the settings UI,
 * open overlays, the status-bar buttons, or the active screen's touch handler.
 *
 * XPT2046 note: when hit==false the point reads (0,0), so the last valid
 * position (g_touchLastX/Y, updated every frame the finger is down) is used as
 * the gesture end-point.
 */
static void handleTouch() {
    lgfx::touch_point_t tp;
    bool hit = tft.getTouch(&tp);

    // X-axis inversion removed — handled by touch calibration

    if (hit && !g_touching) {
        // ── Finger down ──────────────────────────────────────────────────────
        g_touching           = true;
        g_longPressTriggered = false;
        g_touchStartX        = tp.x;
        g_touchStartY        = tp.y;
        g_touchLastX         = tp.x;
        g_touchLastY         = tp.y;
        g_touchDownMs        = millis();
        g_lastCycle          = millis();

    } else if (hit && g_touching) {
        // ── Finger still down — track position and detect long-press ─────
        g_touchLastX = tp.x;
        g_touchLastY = tp.y;

        // Settings is opened from the status-bar cog (not a long-press). The only
        // hold gesture is a stationary 3 s press to save a screenshot, when the
        // Service > Screenshot toggle is enabled.
        if (!g_inSettings && !g_longPressTriggered && g_settings.screenshotHoldEnabled) {
            uint32_t held = millis() - g_touchDownMs;
            bool stationary = abs(g_touchLastX - g_touchStartX) < 20 &&
                              abs(g_touchLastY - g_touchStartY) < 20;
            if (held >= 3000 && stationary) {
                g_longPressTriggered = true;
                char name[24];
                if (screenshotToSD(name, sizeof(name))) {
                    char msg[40];
                    snprintf(msg, sizeof(msg), "Saved %s", name);
                    showToast(msg);
                } else {
                    showToast("No SD card");
                }
            }
        }

    } else if (!hit && g_touching) {
        // ── Finger up — classify gesture using last *valid* position ──────
        g_touching = false;

        // If settings was opened during the hold, swallow this release so it
        // doesn't accidentally fire a button inside the settings page.
        if (g_longPressTriggered) {
            g_longPressTriggered = false;
            return;
        }

        int32_t  dx = g_touchLastX - g_touchStartX;
        int32_t  dy = g_touchLastY - g_touchStartY;
        uint32_t dt = millis() - g_touchDownMs;

        if (g_inSettings) {
            bool close = settingsTouchUp(g_touchStartX, g_touchStartY,
                                         g_touchLastX,  g_touchLastY, dt);
            if (close) {
                g_inSettings = false;
                applySettings();
            }
            return;
        }

        // Screen-picker menu open: a tap on a cell jumps there; any other
        // release (miss or swipe) just closes it.
        if (screenMenuVisible()) {
            if (abs(dx) < 20 && abs(dy) < 20 && dt < 400) {
                int sel = screenMenuHitTest(g_touchStartX, g_touchStartY);
                if (sel >= 0) goTo(sel, PAUSE_AFTER_SWIPE_MS);
            }
            hideScreenMenu();
            return;
        }

        if (abs(dx) > abs(dy) && abs(dx) > SWIPE_THRESH && dt < SWIPE_MAX_MS) {
            // Horizontal swipe → navigate screens
            if (dx < 0) nextScreen();
            else         prevScreen();

        } else if (abs(dx) < 20 && abs(dy) < 20 && dt < 400) {
            if (textPopupVisible()) {
                // Any tap anywhere dismisses the headline popup
                hideTextPopup();
            } else if (wifiOverlayVisible()) {
                // Any tap anywhere dismisses the WiFi overlay
                hideWifiOverlay();
            } else if (g_touchStartY < STATUS_H + 8) {
                // ── Status bar tap — hit-test buttons ────────────────────────
                int playX, circX, wifiX, cogX;
                getStatusBarLayout(playX, circX, wifiX, cogX);

                if (g_touchStartX >= cogX) {
                    settingsEnter();
                    g_inSettings = true;
                    g_pauseUntil = millis() + 60000UL;
                } else if (g_touchStartX >= SCREEN_W/2 - 40 &&
                           g_touchStartX <= SCREEN_W/2 + 40 &&
                           g_touchStartX < playX) {
                    // Time — jump to clock screen
                    goTo(SCR_CLOCK);
                } else if (g_touchStartX >= wifiX) {
                    // WiFi bars — show info overlay
                    char ssid[33], ip[16], mac[18];
                    strlcpy(ssid, WiFi.SSID().c_str(),             sizeof(ssid));
                    strlcpy(ip,   WiFi.localIP().toString().c_str(), sizeof(ip));
                    strlcpy(mac,  WiFi.macAddress().c_str(),        sizeof(mac));
                    showWifiOverlay(ssid, ip, mac, WiFi.RSSI());
                } else if (g_touchStartX >= circX) {
                    // Circle — open the jump-to-screen menu
                    showScreenMenu();
                    g_pauseUntil = millis() + 60000UL;
                } else if (g_touchStartX >= playX) {
                    if (g_issPassActive) {
                        // User ending ISS pass hold early — resume normal auto-play
                        g_issPassActive = false;
                        g_autoPlay      = true;
                        g_lastCycle     = millis();
                    } else {
                        g_autoPlay = !g_autoPlay;
                        if (g_autoPlay) g_lastCycle = millis();
                    }
                }
            } else if (g_screen == SCR_PSKREPORTER &&
                       pskTouchUp(g_touchStartX, g_touchStartY)) {
                g_pauseUntil = millis() + 10000UL;
            } else if (g_screen == SCR_FT8SPOTS &&
                       ft8TouchUp(g_touchStartX, g_touchStartY)) {
                g_pauseUntil = millis() + 10000UL;
            } else if (g_screen == SCR_CONTESTS &&
                       contestTouchUp(g_touchStartX, g_touchStartY)) {
                g_lastCycle = millis();
            } else if (g_screen == SCR_HF_NOW &&
                       hfNowTouchUp(g_touchStartX, g_touchStartY)) {
                g_lastCycle = millis();
            } else if (g_screen == SCR_WEATHER &&
                       weatherTouchUp(g_touchStartX, g_touchStartY)) {
                g_lastCycle = millis();
            } else if (g_screen == SCR_BBC &&
                       bbcTouchUp(g_touchStartX, g_touchStartY)) {
                g_pauseUntil = millis() + 20000UL;
            } else if (g_screen == SCR_APPLE &&
                       appleTouchUp(g_touchStartX, g_touchStartY)) {
                g_pauseUntil = millis() + 20000UL;
            }
        }
    }
}

// ─── Onboard green LED — blinks while the ISS is above the horizon (radio pass) ─
/** @brief Configure the three onboard LED GPIOs as outputs and switch them off. */
static void ledInit() {
    pinMode(PIN_LED_R, OUTPUT);
    pinMode(PIN_LED_G, OUTPUT);
    pinMode(PIN_LED_B, OUTPUT);
    digitalWrite(PIN_LED_R, LED_OFF);
    digitalWrite(PIN_LED_G, LED_OFF);
    digitalWrite(PIN_LED_B, LED_OFF);
}

/**
 * @brief Whether every data source has completed its first fetch.
 *
 * Includes PSK/FT8, which the fetch task defers ~60 s into its loop. Latches
 * true once reached so it never reverts.
 * @return true once all FSRC_* states are FS_OK or FS_FAIL, else false.
 */
static bool startupComplete() {
    static bool done = false;
    if (done) return true;
    for (int i = 0; i < FSRC_COUNT; i++) {
        FetchState st = g_fetchState[i];
        if (st != FS_OK && st != FS_FAIL) return false;
    }
    done = true;
    return true;
}

/**
 * @brief Drive the onboard RGB LED at ~1 Hz based on current app state.
 *
 * Priority of indications (all subject to their settings toggle):
 *   - Blue blink  — startup: initial data still loading.
 *   - Red/green   — a lightning alert while the ISS Tracker screen is active
 *                   (the two colours alternate).
 *   - Red blink   — a thunderstorm is due within the hour (until acknowledged).
 *   - Green blink — the ISS is above the horizon over the QTH (an overhead pass,
 *                    contactable by radio — not gated on optical visibility).
 * Call once per loop; internal timing produces one on/off cycle per second.
 */
static void ledUpdate() {
    static bool          phase = false;
    static unsigned long last  = 0;

    if (millis() - last >= 500) { last = millis(); phase = !phase; }

    // Startup phase: blue blink until all initial fetches finish. Blue is
    // otherwise never used, so keep it out of the red/green logic below.
    if (!startupComplete()) {
        digitalWrite(PIN_LED_R, LED_OFF);
        digitalWrite(PIN_LED_G, LED_OFF);
        digitalWrite(PIN_LED_B,
                     (g_settings.startupLedEnabled && phase) ? LED_ON : LED_OFF);
        return;
    }
    digitalWrite(PIN_LED_B, LED_OFF);

    // Green condition: the ISS is above the horizon over the QTH — i.e. within an
    // active pass window (computed at MIN_EL = 5°), so it's contactable by radio.
    // This is a geometric pass; it does NOT require optical visibility, so a
    // daylight pass (still a radio pass) flashes the LED.
    bool green = false;
    if (g_settings.issLedEnabled) {
        xSemaphoreTake(g_dataMutex, portMAX_DELAY);
        bool    pv   = g_iss.passValid;
        int32_t rise = g_iss.passRiseSec;
        int32_t set  = g_iss.passSetSec;
        xSemaphoreGive(g_dataMutex);
        if (pv) {
            int32_t now32 = (int32_t)time(nullptr);
            green = (now32 >= rise && now32 < set);
        }
    }

    // Red condition: lightning within the hour, not yet acknowledged.
    // Call weatherLightningSoon() unconditionally so its acknowledge re-arm runs.
    bool soon = weatherLightningSoon();
    bool red  = g_settings.lightningLedEnabled && soon && !weatherLightningAcked();

    // "ISS pass" for the alternation = the ISS Tracker screen is being viewed.
    bool onIssScreen = (g_screen == SCR_GREYLINE);

    bool rOn = false, gOn = false;
    if (red && onIssScreen) { if (phase) rOn = true; else gOn = true; }  // alternate
    else if (red)           { rOn = phase; }                            // blink red
    else if (green)         { gOn = phase; }                            // blink green

    digitalWrite(PIN_LED_R, rOn ? LED_ON : LED_OFF);
    digitalWrite(PIN_LED_G, gOn ? LED_ON : LED_OFF);
}

// ─── Fake ISS pass (Service menu) ─────────────────────────────────────────────
/**
 * @brief Inject a plausible overhead ISS pass into g_iss (Service menu test aid).
 *
 * Places the ISS a few degrees south of the observer (high elevation) with a
 * pass window bracketing "now", so the ISS Tracker overlay renders for
 * documentation screenshots.
 */
static void applyFakeISSPass() {
    const float D2R = (float)M_PI / 180.0f;
    const float INC = 51.6f * D2R;
    float latT = constrain(g_settings.lat - 4.0f, -50.0f, 50.0f);
    float lonT = g_settings.lon;
    float u0   = asinf(constrain(sinf(latT * D2R) / sinf(INC), -1.0f, 1.0f));
    float dLam = atan2f(cosf(INC) * sinf(u0), cosf(u0));   // radians
    int32_t now = (int32_t)time(nullptr);

    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    g_iss.valid         = true;
    g_iss.lat           = latT;
    g_iss.lon           = lonT;
    g_iss.trackCount    = 0;
    g_iss.passValid     = true;
    g_iss.passNow       = true;
    g_iss.passRiseSec   = now - 120;
    g_iss.passSetSec    = now + 360;
    g_iss.passRiseAz    = 210;
    g_iss.passSetAz     = 340;
    g_iss.passMaxEl     = 78;
    g_iss.passMidLat    = latT;
    g_iss.passMidLon    = lonT;
    g_iss.currentAz     = 180.0f;
    g_iss.currentEl     = 70.0f;
    g_iss.rangeRateKmps = -3.0f;
    g_iss.orbitEpoch    = now;
    g_iss.orbitU0       = u0;
    g_iss.orbitLambda0  = 0.0f;
    g_iss.orbitLon0     = lonT - dLam * 180.0f / (float)M_PI;
    xSemaphoreGive(g_dataMutex);
}

/**
 * @brief Maintain the Service-menu fake ISS pass each loop.
 *
 * While g_fakeISSPass is set, re-asserts the fake pass (refreshed every ~5 s so
 * the window keeps bracketing "now"); on release, clears the pass flags in g_iss.
 */
static void serviceFakeISSUpdate() {
    static bool          prev      = false;
    static unsigned long lastApply = 0;
    if (g_fakeISSPass) {
        if (!prev || millis() - lastApply > 5000) { applyFakeISSPass(); lastApply = millis(); }
    } else if (prev) {
        xSemaphoreTake(g_dataMutex, portMAX_DELAY);
        g_iss.passValid = false;
        g_iss.passNow   = false;
        xSemaphoreGive(g_dataMutex);
    }
    prev = g_fakeISSPass;
}

// ─── setup() ─────────────────────────────────────────────────────────────────
/**
 * @brief Arduino entry point: one-time boot initialisation.
 *
 * Brings up serial, LEDs, settings, display/UI sprite and the SSL DRAM
 * pre-reserves (allocated right after the sprite while heap is still
 * contiguous), connects WiFi (saved creds or captive portal), syncs NTP, and
 * launches the background fetch task on a static stack.
 */
#if defined(RUN_UNIT_TESTS)
// Under `pio test -e test` the Unity runner provides setup()/loop(); rename the
// firmware entry points (keeping them defined so their static helpers stay
// referenced and the -Werror build stays warning-clean).
void setup_firmware() {
#else
void setup() {
#endif
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);  // disable brownout detector
    Serial.begin(115200);
    Serial.println("\n\n[boot] CYD Dashboard");
    Serial.printf("[boot] heap=%u  largestDMA=%u\n",
                  ESP.getFreeHeap(),
                  heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

    g_dataMutex = xSemaphoreCreateMutex();

    ledInit();

    // Load settings and initialise display first
    settingsLoad();

    tft.init();
    tft.setRotation(1);
    if (g_settings.touchCalValid)
        tft.setTouchCalibrate(g_settings.touchCal);
    tft.setBrightness(g_settings.brightness);
    tft.fillScreen(TFT_BLACK);
    uiInit(&tft);   // allocates 76,800 B sprite from the largest DMA block (~110 KB)
    Serial.printf("[ui] post-sprite lb8bit=%u  heap=%u\n",
                  heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  ESP.getFreeHeap());

    // ── SSL pre-reserves: allocated RIGHT AFTER sprite ────────────────────────
    // g_sslX509R (SSL_X509_RESERVE) holds a contiguous region for the TLS input
    // record buffer. Sized >= TLS_MIN_CONTIG (16717) so that when freed just
    // before http.GET() it yields a block big enough on its own — with headroom —
    // rather than relying on a fragmented neighbour to coalesce (long uptime
    // erodes that neighbour, which parks the largest block on the threshold and
    // causes the transient TLS skips).
    //
    // g_sslCtxR (SSL_CTX_RESERVE) is freed just before each WiFiClientSecure
    // constructor so new sslclient_context() first-fits into the vacated slot.
    g_sslX509R  = heap_caps_malloc(SSL_X509_RESERVE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    g_sslCtxR   = heap_caps_malloc(SSL_CTX_RESERVE,  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    Serial.printf("[boot] SSL reserves: x509=%p ctx=%p  heap=%u  lb8bit=%u\n",
                  g_sslX509R, g_sslCtxR,
                  ESP.getFreeHeap(),
                  heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    // ── Thumbnail buffers ─────────────────────────────────────────────────────
    g_bbcThumbBuf   = (uint8_t*)malloc(THUMB_BUF_LEN);
    g_appleThumbBuf = (uint8_t*)malloc(THUMB_BUF_LEN);
    Serial.printf("[boot] thumbBufs bbc=%p apple=%p  heap=%u\n",
                  g_bbcThumbBuf, g_appleThumbBuf, ESP.getFreeHeap());

    // Let power supply stabilise before WiFi radio draws ~300 mA
    delay(500);
    splash("Connecting to WiFi...");

    // ── Credentials from SPIFFS (data/wifi.txt) ───────────────────────────────
    // If /wifi.txt exists (SSID on line 1, password on line 2) try a direct
    // WiFi.begin() before falling back to the WiFiManager captive portal.
    // The file is listed in .gitignore and never committed.
    {
        // SPIFFS was already mounted by settingsLoad(); just open the file.
        File wf = SPIFFS.open("/wifi.txt");
        if (wf) {
            String ssid = wf.readStringUntil('\n'); ssid.trim();
            String pass = wf.readStringUntil('\n'); pass.trim();
            wf.close();
            if (ssid.length() > 0) {
                Serial.printf("[wifi] trying /wifi.txt  ssid='%s'\n", ssid.c_str());
                WiFi.setTxPower(WIFI_POWER_8_5dBm);
                WiFi.begin(ssid.c_str(), pass.c_str());
                for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
                    delay(500);
                    Serial.print('.');
                }
                Serial.println();
            }
        }
    }

    // WiFiManager — skips the portal if already connected above
    WiFiManager wm;
    wm.setAPCallback(onAPMode);
    wm.setConfigPortalTimeout(180);
    wm.setConnectTimeout(20);

    if (WiFi.status() != WL_CONNECTED && !wm.autoConnect(WIFI_AP_NAME)) {
        splash("WiFi failed.", "Restarting in 5 s...");
        delay(5000);
        ESP.restart();
    }

    Serial.printf("[wifi] %s\n", WiFi.localIP().toString().c_str());
    splash("WiFi OK", "Syncing time...");

    // Apply saved timezone (uses POSIX TZ string from settings)
    configTime(0, 0, NTP_SERVER1, NTP_SERVER2);
    setenv("TZ", g_settings.tz, 1);
    tzset();

    // Block until time valid (up to 10 s)
    struct tm ti;
    for (int i = 0; i < 20 && !getLocalTime(&ti); i++) delay(500);
    Serial.printf("[ntp] %02d:%02d:%02d  tz=%s\n",
                  ti.tm_hour, ti.tm_min, ti.tm_sec, g_settings.tzName);

    splash("Time OK", "Fetching data...");

    // Create fetchTask with a static stack (zero heap cost) so WiFi's earlier
    // ~50 KB fragmentation doesn't prevent the allocation.  The task blocks on
    // fetchReady until we give it below, after the SSL reserves are released.
    SemaphoreHandle_t fetchReady = xSemaphoreCreateBinary();
    TaskHandle_t fetchHandle = xTaskCreateStaticPinnedToCore(
        fetchTask, "fetch",
        sizeof(s_fetchTaskStack),           // depth = 8192 (StackType_t is uint8_t)
        reinterpret_cast<void*>(fetchReady),
        1, s_fetchTaskStack, &s_fetchTaskBuffer, 1);
    if (!fetchHandle) {
        Serial.println("[boot] fetchTask CREATE FAILED");
    } else {
        Serial.printf("[boot] fetchTask created (static)  heap=%u\n", ESP.getFreeHeap());
    }

    // Signal fetchTask: SSL reserves are in place, safe to connect.
    xSemaphoreGive(fetchReady);

    g_lastCycle = millis();
    Serial.println("[boot] Ready");
}

// ─── Screen name table ────────────────────────────────────────────────────────
/** @brief Map a screen ID to a short name for serial logging.
 *  @param id  A SCR_* screen index. @return A static name string, "?" if unknown. */
static const char* screenName(int id) {
    switch (id) {
        case SCR_CLOCK:       return "CLOCK";
        case SCR_WEATHER:     return "WEATHER";
        case SCR_HF_NOW:      return "HF_NOW";
        case SCR_PROPAGATION: return "PROPAGATION";
        case SCR_GREYLINE:    return "GREYLINE";
        case SCR_PSKREPORTER: return "PSKREPORTER";
        case SCR_FT8SPOTS:    return "FT8_SPOTS";
        case SCR_DXSPOTS:     return "DX_SPOTS";
        case SCR_POTASPOTS:   return "POTA_SPOTS";
        case SCR_SOTASPOTS:   return "SOTA_SPOTS";
        case SCR_CONTESTS:    return "CONTESTS";
        case SCR_BBC:         return "BBC_NEWS";
        case SCR_APPLE:       return "APPLE_NEWS";
        case SCR_TRACKER:     return "TRACKER";
        default:              return "?";
    }
}

/** @brief Map a FetchState to a short string for serial logging.
 *  @param s  A FetchState value. @return A static string ("idle"/"ACTIVE"/"ok"/"FAIL"). */
static const char* fsStr(FetchState s) {
    switch (s) {
        case FS_IDLE:   return "idle";
        case FS_ACTIVE: return "ACTIVE";
        case FS_OK:     return "ok";
        case FS_FAIL:   return "FAIL";
        default:        return "?";
    }
}

/**
 * @brief Print a per-source fetch-state and item-count summary to serial.
 *
 * Reads the volatile fetch states directly (single-byte atomic reads) and the
 * item counts under g_dataMutex for consistency.
 */
static void printDataStatus() {
    // Snapshot volatile fetch states first (single-byte reads are atomic)
    FetchState fs[FSRC_COUNT];
    for (int i = 0; i < FSRC_COUNT; i++) fs[i] = g_fetchState[i];

    // Item counts need the mutex
    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    uint8_t bbcN  = g_bbcNews.count;
    uint8_t appN  = g_appleNews.count;
    uint8_t dxN   = g_dxSpots.count;
    uint8_t potaN = g_potaSpots.count;
    uint8_t sotaN = g_sotaSpots.count;
    uint8_t cxN   = g_contests.count;
    uint8_t pskN  = g_pskData.count;
    uint8_t ft8N  = g_ft8Spots.count;
    xSemaphoreGive(g_dataMutex);

    Serial.printf("[screens] weather:%-6s  solar:%-6s  iss:%-6s  tracker:%s\n",
        fsStr(fs[FSRC_WEATHER]), fsStr(fs[FSRC_SOLAR]),
        fsStr(fs[FSRC_ISS]),     fsStr(fs[FSRC_TRACKER]));
    Serial.printf("[screens] bbc:%-6s(%d)  apple:%-6s(%d)\n",
        fsStr(fs[FSRC_BBC]), bbcN, fsStr(fs[FSRC_APPLE]), appN);
    Serial.printf("[screens] dx:%-6s(%d)  pota:%-6s(%d)  sota:%-6s(%d)  contests:%-6s(%d)\n",
        fsStr(fs[FSRC_DX]),       dxN,
        fsStr(fs[FSRC_POTA]),     potaN,
        fsStr(fs[FSRC_SOTA]),     sotaN,
        fsStr(fs[FSRC_CONTESTS]), cxN);
    Serial.printf("[screens] psk:%-6s(%d)  ft8:%-6s(%d)\n",
        fsStr(fs[FSRC_PSK]), pskN, fsStr(fs[FSRC_FT8]), ft8N);
}

// ─── loop() ──────────────────────────────────────────────────────────────────
/**
 * @brief Arduino main loop: input, ISS-pass auto-jump, auto-cycling, LED, redraw.
 *
 * Each iteration polls touch, maintains the Service fake-ISS state, auto-jumps
 * to the Grey Line screen at the start of a real ISS pass (and resumes after),
 * advances the auto-cycle when not paused, updates the onboard LED, and redraws
 * at ~30 fps.
 */
#if defined(RUN_UNIT_TESTS)
void loop_firmware() {
#else
void loop() {
#endif
    handleTouch();

    serviceFakeISSUpdate();   // Service menu: stage/clear a fake ISS pass

    // ── ISS pass detection ────────────────────────────────────────────────────
    // Derive passNow from the clock rather than the stale flag so the jump
    // fires the moment the countdown reaches zero, not up to 30 s later.
    if (!g_inSettings && g_settings.issJumpEnabled) {
        static bool s_prevPassNow = false;
        bool passNow = false;
        xSemaphoreTake(g_dataMutex, portMAX_DELAY);
        if (g_iss.passValid) {
            int32_t now32 = (int32_t)time(nullptr);
            passNow = (now32 >= g_iss.passRiseSec && now32 < g_iss.passSetSec);
        }
        xSemaphoreGive(g_dataMutex);

        if (passNow && !s_prevPassNow) {
            // Pass just started — jump to Grey Line and hold
            g_issPassActive = true;
            g_screen        = SCR_GREYLINE;
            g_lastCycle     = millis();
        }
        if (!passNow && s_prevPassNow && g_issPassActive) {
            // Pass just ended naturally — resume auto-play
            g_issPassActive = false;
            g_autoPlay      = true;
            g_lastCycle     = millis();
        }
        s_prevPassNow = passNow;
    }

    if (!g_inSettings) {
        // Auto-cycle — skip disabled screens
        bool paused = (millis() < g_pauseUntil)
                    || (g_screen == SCR_CONTESTS && contestHasSelection())
                    || (g_screen == SCR_HF_NOW   && hfNowHasSelection())
                    || g_issPassActive
                    || textPopupVisible();   // keep the news screen up while reading
        unsigned long cycleMs = (g_screen == SCR_CLOCK) ? CYCLE_MS * 2 : CYCLE_MS;
        if (g_autoPlay && !paused && (millis() - g_lastCycle >= cycleMs)) {
            g_screen    = (uint8_t)nextEnabledScreen(g_screen, +1);
            g_lastCycle = millis();
        }
    }

    // ── Screen-change log ─────────────────────────────────────────────────────
    {
        static uint8_t s_lastScreen = 0xFF;
        if (g_screen != s_lastScreen) {
            Serial.printf("[ui] screen -> %s (#%d)\n", screenName(g_screen), g_screen);
            s_lastScreen = g_screen;
        }
    }

    // ── Periodic status dump every 30 s ──────────────────────────────────────
    {
        static unsigned long s_lastStatus = 0;
        if (millis() - s_lastStatus >= 30000UL) {
            s_lastStatus = millis();
            uint32_t ms  = millis();
            uint32_t s   = ms / 1000; ms %= 1000;
            uint32_t m   = s  / 60;   s  %= 60;
            Serial.printf("[%02u:%02u.%03u] [status] screen=%s\n", m, s, ms, screenName(g_screen));
            printDataStatus();
        }
    }

    // Flash the onboard green LED during a visible ISS pass
    ledUpdate();

    // Redraw at ~30 fps
    bool paused = (millis() < g_pauseUntil);
    static unsigned long lastDraw = 0;
    if (millis() - lastDraw >= 33) {
        uiDraw(g_screen, g_autoPlay, paused, g_inSettings);
        lastDraw = millis();
    }
}
