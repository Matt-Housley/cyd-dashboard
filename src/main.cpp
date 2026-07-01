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
#include "screen_contests.h"
#include "screen_hf_now.h"
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

// ─── SSL pre-reserves (declared in fetch.h) ───────────────────────────────────
// Two blocks are allocated in setup() immediately after the sprite, while the
// remaining ~34–51 KB of the largest heap block is still contiguous.
// They are HELD until the last possible moment before each TLS connection, then
// freed and reclaimed by fetch.cpp so nothing can nibble those slots.
//
//   g_sslX509R   12288 B  — freed just before http.GET(); TLS cert-chain parser
//                           (~10 KB for BBC's 3-cert chain) lands here.
//                           When freed it coalesces with the natural ~26 KB
//                           adjacent free block → ~38.9 KB for mbedTLS
//                           in_buf (16717 B) + out_buf (16717 B) ✓
//
//   g_sslCtxR     6136 B  — freed just before WiFiClientSecure constructor;
//                           new sslclient_context() (6136 B) first-fits here.
void* g_sslX509R   = nullptr;
void* g_sslCtxR    = nullptr;

// ─── Fetch task — static stack ────────────────────────────────────────────────
// Declared in .bss so the 8 KB stack uses ZERO heap.  Dynamic allocation via
// xTaskCreatePinnedToCore fails after WiFi init because ~50 KB of heap is
// consumed and no contiguous 8 KB block survives.  Static allocation side-steps
// the problem entirely: WiFi sees the same heap it always had.
static StaticTask_t s_fetchTaskBuffer;
static StackType_t  s_fetchTaskStack[8192];   // StackType_t = uint8_t on ESP32

// ─── Navigation state ─────────────────────────────────────────────────────────
static uint8_t       g_screen       = SCR_CLOCK;
static bool          g_autoPlay     = true;
static unsigned long g_lastCycle    = 0;
static unsigned long g_pauseUntil   = 0;
static bool          g_inSettings   = false;
static bool          g_issPassActive = false;  // true while auto-held on Grey Line for ISS pass

// ─── Touch / swipe state ──────────────────────────────────────────────────────
static int32_t       g_touchStartX        = 0;
static int32_t       g_touchStartY        = 0;
static int32_t       g_touchLastX         = 0;  // updated every frame while finger is down
static int32_t       g_touchLastY         = 0;
static unsigned long g_touchDownMs        = 0;
static bool          g_touching           = false;
static bool          g_longPressTriggered = false;  // settings opened during hold

// ─── Live touch accessors (declared in ui.h) ──────────────────────────────────
bool    touchIsActive()  { return g_touching; }
int32_t touchCurrentX()  { return g_touchLastX; }
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
// Returns 0 when not connected, 1–4 bars by RSSI when connected.
// Kept here so WiFi.h never needs to be included in ui.cpp.
int getWiFiBars() {
    if (WiFi.status() != WL_CONNECTED) return 0;
    int rssi = WiFi.RSSI();
    if (rssi > -55) return 4;
    if (rssi > -65) return 3;
    if (rssi > -75) return 2;
    return 1;
}

// ─── WiFiManager callbacks ────────────────────────────────────────────────────
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
static int nextEnabledScreen(int from, int dir) {
    int s = from;
    for (int i = 0; i < NUM_SCREENS; i++) {
        s = (s + dir + NUM_SCREENS) % NUM_SCREENS;
        if (g_settings.screenEnabled[s]) return s;
    }
    return from;   // fallback: all disabled (shouldn't happen)
}

// ─── Screen navigation ────────────────────────────────────────────────────────
static void goTo(int s, unsigned long pauseMs = 0) {
    if ((uint8_t)(s % NUM_SCREENS) != g_screen) {
        pskClearSelection();
        contestClearSelection();
    }
    g_screen    = (uint8_t)(s % NUM_SCREENS);
    g_lastCycle = millis();
    if (pauseMs) g_pauseUntil = millis() + pauseMs;
}

static void nextScreen() { goTo(nextEnabledScreen(g_screen, +1), PAUSE_AFTER_SWIPE_MS); }
static void prevScreen() { goTo(nextEnabledScreen(g_screen, -1), PAUSE_AFTER_SWIPE_MS); }

// ─── Apply settings after change ─────────────────────────────────────────────
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
// XPT2046 note: when hit==false the tp struct contains (0,0) — the resistive
// panel has nothing to measure.  We therefore maintain g_touchLastX/Y which
// is updated every frame the finger is down and use *that* as the end-position
// when computing gesture deltas.
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

        // Open settings after 2 s of stationary hold (not already in settings)
        if (!g_inSettings && !g_longPressTriggered) {
            uint32_t held = millis() - g_touchDownMs;
            if (held >= 2000 &&
                abs(g_touchLastX - g_touchStartX) < 20 &&
                abs(g_touchLastY - g_touchStartY) < 20) {
                g_longPressTriggered = true;
                settingsEnter();
                g_inSettings = true;
                g_pauseUntil = millis() + 60000UL;
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

        if (abs(dx) > abs(dy) && abs(dx) > SWIPE_THRESH && dt < SWIPE_MAX_MS) {
            // Horizontal swipe → navigate screens
            if (dx < 0) nextScreen();
            else         prevScreen();

        } else if (abs(dx) < 20 && abs(dy) < 20 && dt < 400) {
            if (wifiOverlayVisible()) {
                // Any tap anywhere dismisses the WiFi overlay
                hideWifiOverlay();
            } else if (g_touchStartY < STATUS_H + 8) {
                // ── Status bar tap — hit-test buttons ────────────────────────
                int playX, advX, wifiX, cogX;
                getStatusBarLayout(playX, advX, wifiX, cogX);

                if (g_touchStartX >= cogX) {
                    settingsEnter();
                    g_inSettings = true;
                    g_pauseUntil = millis() + 60000UL;
                } else if (g_touchStartX >= SCREEN_W/2 - 40 &&
                           g_touchStartX <= SCREEN_W/2 + 40 &&
                           g_touchStartX < wifiX) {
                    // Time — jump to clock screen
                    goTo(SCR_CLOCK);
                } else if (g_touchStartX >= wifiX) {
                    // WiFi bars — show info overlay
                    char ssid[33], ip[16], mac[18];
                    strlcpy(ssid, WiFi.SSID().c_str(),             sizeof(ssid));
                    strlcpy(ip,   WiFi.localIP().toString().c_str(), sizeof(ip));
                    strlcpy(mac,  WiFi.macAddress().c_str(),        sizeof(mac));
                    showWifiOverlay(ssid, ip, mac, WiFi.RSSI());
                } else if (g_touchStartX >= advX) {
                    goTo(nextEnabledScreen(g_screen, +1));
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
            } else if (g_screen == SCR_CONTESTS &&
                       contestTouchUp(g_touchStartX, g_touchStartY)) {
                g_lastCycle = millis();
            } else if (g_screen == SCR_HF_NOW &&
                       hfNowTouchUp(g_touchStartX, g_touchStartY)) {
                g_lastCycle = millis();
            }
        }
    }
}

// ─── setup() ─────────────────────────────────────────────────────────────────
void setup() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);  // disable brownout detector
    Serial.begin(115200);
    Serial.println("\n\n[boot] CYD Dashboard");
    Serial.printf("[boot] heap=%u  largestDMA=%u\n",
                  ESP.getFreeHeap(),
                  heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

    g_dataMutex = xSemaphoreCreateMutex();

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
    // g_sslX509R (12288 B) holds the region that will be used by the TLS cert-
    // chain parser.  When freed just before http.GET() it coalesces with the
    // natural ~26 KB adjacent free block, yielding ~38.9 KB for mbedTLS's
    // in_buf (16717 B) + out_buf (16717 B) — well above the 33434 B minimum.
    //
    // g_sslCtxR (6136 B) is freed just before each WiFiClientSecure constructor
    // so new sslclient_context() first-fits into the vacated slot.
    g_sslX509R  = heap_caps_malloc(12288, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    g_sslCtxR   = heap_caps_malloc(6136,  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
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

// ─── loop() ──────────────────────────────────────────────────────────────────
void loop() {
    handleTouch();

    // ── ISS pass detection ────────────────────────────────────────────────────
    // Check passNow flag (updated every ~30 s by fetchISS) and jump to/from
    // the Grey Line screen when a pass starts or ends.
    if (!g_inSettings && g_settings.issJumpEnabled) {
        static bool s_prevPassNow = false;
        bool passNow = false;
        xSemaphoreTake(g_dataMutex, portMAX_DELAY);
        passNow = g_iss.passValid && g_iss.passNow;
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
                    || g_issPassActive;
        unsigned long cycleMs = (g_screen == SCR_CLOCK) ? CYCLE_MS * 2 : CYCLE_MS;
        if (g_autoPlay && !paused && (millis() - g_lastCycle >= cycleMs)) {
            g_screen    = (uint8_t)nextEnabledScreen(g_screen, +1);
            g_lastCycle = millis();
        }
    }

    // Redraw at ~30 fps
    bool paused = (millis() < g_pauseUntil);
    static unsigned long lastDraw = 0;
    if (millis() - lastDraw >= 33) {
        uiDraw(g_screen, g_autoPlay, paused, g_inSettings);
        lastDraw = millis();
    }
}
