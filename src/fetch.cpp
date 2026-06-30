#include "fetch.h"
#include "config.h"
#include "settings.h"
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <Arduino.h>
#include <math.h>
#include <esp_heap_caps.h>
#include <freertos/semphr.h>

// ─── Thumbnail buffers ────────────────────────────────────────────────────────
// Allocated in fetchTask() after the semaphore fires — NOT global arrays — so
// that 24 KB stays out of .bss and the full boot heap is available for WiFi
// RF calibration and the sprite DMA allocation.
uint8_t* g_bbcThumbBuf   = nullptr;
size_t   g_bbcThumbLen   = 0;
uint8_t* g_appleThumbBuf = nullptr;
size_t   g_appleThumbLen = 0;

uint32_t g_bbcThumbVersion   = 0;
uint32_t g_appleThumbVersion = 0;


// ─── SSL DRAM reserve helpers ─────────────────────────────────────────────────
//
// g_sslX509R (12288 B):
//   Released just before http.GET() so the TLS cert-chain parser (~10 KB for
//   BBC's 3-cert chain) has a clean region to use.  When freed it coalesces
//   with the natural ~26 KB adjacent free block, giving ~38.9 KB for mbedTLS
//   in_buf (16717 B) + out_buf (16717 B) — well above the 33434 B minimum.
//   Reclaimed AFTER the inner scope closes (http + sc both destroyed) so the
//   slot is completely empty when malloc(12288) runs.
//
// g_sslCtxR (6136 B):
//   Released just before each WiFiClientSecure constructor.  The constructor's
//   very first heap allocation is  new sslclient_context() (6136 B); first-fit
//   picks the freshly-freed slot.  Held as a global so nothing (WiFi stack,
//   lwIP, etc.) can nibble it between setup() and the first fetch.
//   Reclaimed after the WiFiClientSecure destructor runs.

static void sslX509Release()  { if (g_sslX509R)  { heap_caps_free(g_sslX509R);  g_sslX509R  = nullptr; } }
static void sslX509Reclaim()  { if (!g_sslX509R)   g_sslX509R  = heap_caps_malloc(12288, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
static void sslCtxRelease()   { if (g_sslCtxR)   { heap_caps_free(g_sslCtxR);   g_sslCtxR   = nullptr; } }
static void sslCtxReclaim()   { if (!g_sslCtxR)    g_sslCtxR   = heap_caps_malloc(6136,  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }

// Release x509 slot just before http.GET() (ctx freed earlier, before the
// WiFiClientSecure constructor).  Freeing x509 coalesces with the natural
// ~26 KB free block → ~38.9 KB available for mbedTLS in_buf + out_buf.
static void sslDataRelease() {
    sslX509Release();
    Serial.printf("[ssl] data-released  lb8bit=%u  heap=%u\n",
                  heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  ESP.getFreeHeap());
}
// Reclaim all slots after the WiFiClientSecure destructor has run.
static void sslAllReclaim() {
    sslX509Reclaim();
    sslCtxReclaim();
    Serial.printf("[ssl] reclaimed  x509=%s ctx=%s  lb8bit=%u\n",
                  g_sslX509R  ? "OK" : "FAIL",
                  g_sslCtxR   ? "OK" : "FAIL",
                  heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

// ─── Stream-read helpers ──────────────────────────────────────────────────────
// streamReadText: incremental reader for responses that carry Content-Length.
// Uses body.reserve(4096) + repeated small realloc so the String grows in-place
// within whatever contiguous block was available at first allocation — no large
// upfront reserve required.  Works even when lb8bit ≈ 12 KB (during SSL when
// in_buf + out_buf have consumed most of the available heap).
//
// NOT suitable for chunked Transfer-Encoding (it sees raw "HEX\r\n" headers).
// For chunked responses, use http.getString() — with getSize() == -1 it skips
// the Content-Length-based reserve() call and grows the StreamString the same
// incremental way, while also stripping chunk headers automatically.
static bool streamReadText(HTTPClient& http, String& body, size_t maxBytes) {
    int       contentLen = http.getSize();
    WiFiClient* stream   = http.getStreamPtr();
    body.reserve(4096);
    char     cbuf[512];
    size_t   total = 0;
    uint32_t t0    = millis();
    while ((millis() - t0) < 20000) {
        if (contentLen > 0 && (int)total >= contentLen) break;
        if (total >= maxBytes) break;
        int avail = stream->available();
        if (avail > 0) {
            size_t toRead = min((size_t)avail, min(sizeof(cbuf), maxBytes - total));
            int n = stream->readBytes(cbuf, toRead);
            if (n > 0) { body.concat(cbuf, n); total += n; }
            vTaskDelay(pdMS_TO_TICKS(1));
        } else if (!stream->connected()) {
            break;
        } else {
            delay(5);
        }
    }
    http.end();
    return total > 0;
}


static size_t streamReadBytes(HTTPClient& http, uint8_t* buf, size_t maxLen,
                               unsigned long timeoutMs) {
    WiFiClient* stream = http.getStreamPtr();
    size_t   got = 0;
    uint32_t t0  = millis();
    while (got < maxLen && (millis() - t0) < timeoutMs) {
        if (stream->available()) {
            size_t n = stream->readBytes(buf + got, min((size_t)512, maxLen - got));
            got += n;
            vTaskDelay(pdMS_TO_TICKS(1));
        } else if (!stream->connected()) {
            break;
        } else {
            delay(5);
        }
    }
    http.end();
    return got;
}

// ─── WiFi guard ───────────────────────────────────────────────────────────────
static bool ensureWiFi() {
    if (WiFi.status() == WL_CONNECTED) return true;
    Serial.println("[fetch] WiFi lost — reconnecting");
    WiFi.reconnect();
    for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) delay(500);
    bool ok = (WiFi.status() == WL_CONNECTED);
    Serial.printf("[fetch] WiFi %s\n", ok ? "reconnected" : "reconnect FAILED");
    return ok;
}

// ─── HTTP GET (text, stream-read with size cap) ───────────────────────────────
// HTTPS path memory layout per connection:
//   1. sslCtxRelease()    — frees ctx slot (6136 B)
//   2. WiFiClientSecure   — sslclient_context (6136 B) → ctx slot ✓
//   3. sslDataRelease()   — frees x509 slot (12288 B); coalesces with natural
//                           ~26 KB adjacent block → lb8bit ≈ 38.9 KB ✓
//   4. http.GET()         — in_buf  (16717 B) ✓  out_buf (16717 B) ✓
//                           x509 cert (~10 KB) fits in remaining space ✓
//   5. scopes close       — http destroyed, sc destroyed → ctx slot freed
//   6. sslAllReclaim()    — x509 and ctx slots reclaimed
static String httpGet(const char* url, size_t maxBytes = 40960) {
    const int MAX_TRIES = 3;
    for (int attempt = 1; attempt <= MAX_TRIES; attempt++) {
        if (!ensureWiFi()) {
            if (attempt < MAX_TRIES) delay(2000);
            continue;
        }
        bool isHttps = (strncmp(url, "https://", 8) == 0);
        Serial.printf("[fetch] heap=%u lb8bit=%u  attempt %d/%d  %s\n",
                      ESP.getFreeHeap(),
                      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                      attempt, MAX_TRIES, url);

        bool   gotBody = false;
        String body;

        if (isHttps) {
            sslCtxRelease();                    // ctx slot free → sslclient_context goes here
            {
                WiFiClientSecure sc;            // sslclient_context → ctx slot ✓
                sslDataRelease();               // x509 slot freed → coalesces → lb8bit ≈ 38.9 KB
                sc.setInsecure();
                {
                    HTTPClient http;
                    http.begin(sc, url);
                    http.setTimeout(20000);
                    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
                    http.addHeader("User-Agent", "CYD-Dashboard/1.0 ESP32");
                    http.addHeader("Connection", "close");
                    int code = http.GET();
                    if (code != HTTP_CODE_OK) {
                        char sslErr[64] = "";
                        sc.lastError(sslErr, sizeof(sslErr));
                        Serial.printf("[fetch] %s => HTTP %d  ssl='%s'  lb8bit=%u\n",
                                      url, code, sslErr,
                                      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
                        http.end();
                    } else {
                        gotBody = streamReadText(http, body, maxBytes);
                    }
                }                               // http destroyed
            }                                   // sc destroyed → ctx slot freed
            sslAllReclaim();                    // x509 + ctx slots reclaimed
        } else {
            // Plain HTTP — no SSL bookkeeping needed
            HTTPClient http;
            http.begin(url);
            http.setTimeout(20000);
            http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
            http.addHeader("User-Agent", "CYD-Dashboard/1.0 ESP32");
            http.addHeader("Connection", "close");
            int code = http.GET();
            if (code != HTTP_CODE_OK) {
                Serial.printf("[fetch] %s => HTTP %d\n", url, code);
                http.end();
            } else {
                gotBody = streamReadText(http, body, maxBytes);
            }
        }

        if (!gotBody) {
            if (attempt < MAX_TRIES) delay(2000);
            continue;
        }
        return body;
    }
    return "";
}

// ─── HTTP GET (binary) ────────────────────────────────────────────────────────
static size_t httpGetBytes(const char* url, uint8_t* buf, size_t maxLen,
                           int maxTries = 3, unsigned long timeoutMs = 15000) {
    for (int attempt = 1; attempt <= maxTries; attempt++) {
        if (!ensureWiFi()) {
            if (attempt < maxTries) delay(2000);
            continue;
        }
        bool isHttps = (strncmp(url, "https://", 8) == 0);
        Serial.printf("[fetch-bytes] heap=%u lb8bit=%u  attempt %d/%d  %s\n",
                      ESP.getFreeHeap(),
                      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                      attempt, maxTries, url);

        size_t got = 0;

        if (isHttps) {
            sslCtxRelease();
            {
                WiFiClientSecure sc;        // sslclient_context → ctx slot ✓
                sslDataRelease();           // x509 slot freed → coalesces → lb8bit ≈ 38.9 KB
                sc.setInsecure();
                {
                    HTTPClient http;
                    http.begin(sc, url);
                    http.setTimeout(timeoutMs);
                    http.addHeader("User-Agent", "CYD-Dashboard/1.0 ESP32");
                    int code = http.GET();
                    if (code != HTTP_CODE_OK) {
                        Serial.printf("[fetch-bytes] %s => HTTP %d\n", url, code);
                        http.end();
                    } else {
                        got = streamReadBytes(http, buf, maxLen, timeoutMs);
                    }
                }                           // http destroyed
            }                               // sc destroyed
            sslAllReclaim();
        } else {
            HTTPClient http;
            http.begin(url);
            http.setTimeout(timeoutMs);
            http.addHeader("User-Agent", "CYD-Dashboard/1.0 ESP32");
            int code = http.GET();
            if (code != HTTP_CODE_OK) {
                Serial.printf("[fetch-bytes] %s => HTTP %d\n", url, code);
                http.end();
            } else {
                got = streamReadBytes(http, buf, maxLen, timeoutMs);
            }
        }

        if (got == 0) {
            if (attempt < maxTries) delay(2000);
            continue;
        }
        Serial.printf("[fetch-bytes] got %u bytes\n", (unsigned)got);
        return got;
    }
    return 0;
}

// ─── XML helpers ─────────────────────────────────────────────────────────────
static String xmlGet(const String& xml, const char* tag) {
    String open  = String('<') + tag + '>';
    String close = String("</") + tag + '>';
    int s = xml.indexOf(open);
    if (s < 0) return "";
    s += open.length();
    int e = xml.indexOf(close, s);
    return (e < 0) ? "" : xml.substring(s, e);
}

static String xmlAttr(const String& tag, const char* attr) {
    String needle = String(attr) + "=\"";
    int s = tag.indexOf(needle);
    if (s < 0) return "";
    s += needle.length();
    int e = tag.indexOf('"', s);
    return (e < 0) ? "" : tag.substring(s, e);
}

// band is the exact name= attribute value, e.g. "80m-40m"
static String xmlGetBand(const String& xml, const char* band, const char* time) {
    String nameAttr = String("name=\"") + band + '"';
    String timeAttr = String("time=\"") + time + '"';
    int pos = 0;
    while (true) {
        int bi = xml.indexOf("<band", pos); if (bi < 0) break;
        int be = xml.indexOf('>',    bi);   if (be < 0) break;
        String tag = xml.substring(bi, be);
        if (tag.indexOf(nameAttr) >= 0 && tag.indexOf(timeAttr) >= 0) {
            int ve = xml.indexOf("</band>", be + 1);
            if (ve >= 0) return xml.substring(be + 1, ve);
        }
        pos = be + 1;
    }
    return "";
}

// ─── HTML entity decoder ──────────────────────────────────────────────────────
// Maps both named (&amp;) and numeric (&#36; / &#x24;) entities to ASCII.
// Non-ASCII codepoints are mapped to the nearest printable ASCII equivalent.
static String decodeHtmlEntities(const String& s) {
    String out;
    out.reserve(s.length());
    int i = 0, n = (int)s.length();
    while (i < n) {
        if (s[i] != '&') { out += s[i++]; continue; }
        int semi = s.indexOf(';', i + 1);
        if (semi < 0 || semi - i > 12) { out += s[i++]; continue; }
        String ent = s.substring(i + 1, semi);
        char rep = 0;
        if      (ent.equalsIgnoreCase("amp"))   rep = '&';
        else if (ent.equalsIgnoreCase("lt"))    rep = '<';
        else if (ent.equalsIgnoreCase("gt"))    rep = '>';
        else if (ent.equalsIgnoreCase("quot"))  rep = '"';
        else if (ent.equalsIgnoreCase("apos"))  rep = '\'';
        else if (ent.equalsIgnoreCase("nbsp"))  rep = ' ';
        else if (ent.equalsIgnoreCase("pound")) rep = 'L';   // £
        else if (ent.startsWith("#x") || ent.startsWith("#X")) {
            long code = strtol(ent.c_str() + 2, nullptr, 16);
            if      (code >= 0x20 && code <= 0x7E) rep = (char)code;
            else if (code == 0xA0)                  rep = ' ';
            else if (code == 0xA3)                  rep = 'L';  // £
            else if (code == 0x2013 || code == 0x2014) rep = '-';
            else if (code == 0x2018 || code == 0x2019) rep = '\'';
            else if (code == 0x201C || code == 0x201D) rep = '"';
            else if (code == 0x2026)                rep = '.';
            else if (code == 0x2022)                rep = '*';
            else                                    rep = '?';
        } else if (ent.startsWith("#")) {
            long code = ent.substring(1).toInt();
            if      (code >= 32 && code <= 126)     rep = (char)code;
            else if (code == 160)                   rep = ' ';
            else if (code == 163)                   rep = 'L';  // £
            else if (code == 8211 || code == 8212)  rep = '-';
            else if (code == 8216 || code == 8217)  rep = '\'';
            else if (code == 8220 || code == 8221)  rep = '"';
            else if (code == 8230)                  rep = '.';
            else if (code == 8226)                  rep = '*';
            else                                    rep = '?';
        }
        if (rep) { out += rep; i = semi + 1; }
        else     { out += s[i++]; }   // unknown entity — keep the '&'
    }
    return out;
}

// ─── Day abbreviation ─────────────────────────────────────────────────────────
static const char* dayAbbr(int wday) {
    static const char* D[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    return D[wday % 7];
}

// ─── WEATHER ─────────────────────────────────────────────────────────────────
// open-meteo uses HTTP/1.1 chunked transfer encoding.
// getStreamPtr() returns raw bytes including chunk-size headers ("3a4\r\n…").
// http.getString() de-chunks automatically — use that instead.
void fetchWeather() {
    char url[420];
    // HTTPS — open-meteo supports it and it is more reliable than their HTTP endpoint.
    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast"
        "?latitude=%.4f&longitude=%.4f"
        "&current=temperature_2m,weather_code,relative_humidity_2m,wind_speed_10m"
        "&hourly=weather_code&forecast_hours=24"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min"
              ",precipitation_sum,wind_speed_10m_max,wind_direction_10m_dominant"
        "&wind_speed_unit=mph&timezone=auto&forecast_days=7",
        g_settings.lat, g_settings.lon);

    const int MAX_TRIES = 3;
    String body;
    for (int attempt = 1; attempt <= MAX_TRIES; attempt++) {
        if (!ensureWiFi()) { if (attempt < MAX_TRIES) delay(2000); continue; }
        Serial.printf("[weather] attempt %d/%d\n", attempt, MAX_TRIES);

        sslCtxRelease();
        {
            WiFiClientSecure sc;            // sslclient_context → ctx slot ✓
            sslDataRelease();               // x509 slot freed → coalesces → lb8bit ≈ 38.9 KB
            sc.setInsecure();
            {
                HTTPClient http;
                http.begin(sc, url);
                http.setTimeout(20000);
                http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
                http.addHeader("User-Agent", "CYD-Dashboard/1.0 ESP32");
                http.addHeader("Connection", "close");
                int code = http.GET();
                Serial.printf("[weather] HTTP %d\n", code);
                if (code != HTTP_CODE_OK) {
                    http.end();
                } else {
                    // open-meteo uses chunked Transfer-Encoding; getString()
                    // de-chunks automatically (getStreamPtr() would expose raw
                    // chunk headers).  Response is ~3-5 KB — no heap risk.
                    body = http.getString();
                    http.end();
                }
            }                               // http destroyed
        }                                   // sc destroyed
        sslAllReclaim();

        if (!body.isEmpty()) break;
        if (attempt < MAX_TRIES) delay(2000);
    }
    Serial.printf("[weather] body=%u\n", body.length());
    if (body.isEmpty()) return;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);

    if (err) { Serial.printf("[weather] JSON: %s\n", err.c_str()); return; }

    WeatherData wd;
    wd.valid    = true;
    wd.temp     = (int8_t)doc["current"]["temperature_2m"].as<float>();
    wd.humidity = doc["current"]["relative_humidity_2m"] | 0;
    wd.windMph  = (uint8_t)doc["current"]["wind_speed_10m"].as<float>();
    wd.code     = doc["current"]["weather_code"] | 0;

    auto times  = doc["daily"]["time"].as<JsonArray>();
    auto codes  = doc["daily"]["weather_code"].as<JsonArray>();
    auto maxTs  = doc["daily"]["temperature_2m_max"].as<JsonArray>();
    auto minTs  = doc["daily"]["temperature_2m_min"].as<JsonArray>();
    auto precs  = doc["daily"]["precipitation_sum"].as<JsonArray>();
    auto winds  = doc["daily"]["wind_speed_10m_max"].as<JsonArray>();
    auto windDs = doc["daily"]["wind_direction_10m_dominant"].as<JsonArray>();

    wd.dayCount = (uint8_t)min((int)times.size(), 7);
    Serial.printf("[weather] dayCount=%d  times.size=%d\n", wd.dayCount, (int)times.size());

    for (int i = 0; i < wd.dayCount; i++) {
        String ds = times[i].as<String>();
        int yr = ds.substring(0, 4).toInt();
        int mo = ds.substring(5, 7).toInt();
        int dy = ds.substring(8, 10).toInt();
        static const int t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
        int y2 = (mo < 3) ? yr - 1 : yr;
        int wday = (y2 + y2/4 - y2/100 + y2/400 + t[mo-1] + dy) % 7;
        strlcpy(wd.daily[i].day, dayAbbr(wday), 4);
        wd.daily[i].code    = codes[i] | 0;
        wd.daily[i].maxT    = (int8_t)maxTs[i].as<float>();
        wd.daily[i].minT    = (int8_t)minTs[i].as<float>();
        wd.daily[i].precip  = precs[i].as<float>();
        wd.daily[i].windMph = (uint8_t)winds[i].as<float>();
        wd.daily[i].windDir = (uint16_t)windDs[i].as<int>();
    }

    // Rain prediction from daily precipitation_sum
    strlcpy(wd.rainIn, "Not soon", sizeof(wd.rainIn));
    for (int i = 0; i < wd.dayCount; i++) {
        if (wd.daily[i].precip >= 0.5f) {
            if      (i == 0) strlcpy(wd.rainIn, "Today",    sizeof(wd.rainIn));
            else if (i == 1) strlcpy(wd.rainIn, "Tomorrow", sizeof(wd.rainIn));
            else             snprintf(wd.rainIn, sizeof(wd.rainIn), "~%s", wd.daily[i].day);
            break;
        }
    }

    // Lightning: scan the 24-hour hourly weather_code array.
    // forecast_hours=24 starts from the current hour, so index h = h hours from now.
    // WMO codes 95–99 = thunderstorm / lightning.
    {
        auto hcodes = doc["hourly"]["weather_code"].as<JsonArray>();
        int  total  = min((int)hcodes.size(), 24);
        int  thunder = 0;
        int8_t firstH = -1;
        for (int h = 0; h < total; h++) {
            int hc = hcodes[h] | 0;
            if (hc >= 95) {
                if (firstH < 0) firstH = (int8_t)h;
                thunder++;
            }
        }
        wd.lightningHours = firstH;
        wd.lightningPct   = (total > 0) ? (uint8_t)(thunder * 100 / total) : 0;
    }

    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    g_weather = wd;
    xSemaphoreGive(g_dataMutex);
    Serial.printf("[weather] OK  %dC  rain:%s  ltng:%dh(%d%%)  days:%d\n",
                  wd.temp, wd.rainIn,
                  (int)wd.lightningHours, (int)wd.lightningPct,
                  wd.dayCount);
}

// ─── SOLAR ────────────────────────────────────────────────────────────────────
void fetchSolar() {
    String body = httpGet(API_SOLAR);
    if (body.isEmpty()) return;

    SolarData sd;
    sd.valid    = true;
    sd.sfi      = xmlGet(body, "solarflux").toInt();
    sd.aIndex   = xmlGet(body, "aindex").toInt();
    sd.kIndex   = xmlGet(body, "kindex").toInt();
    sd.sunspots = xmlGet(body, "sunspots").toInt();
    strlcpy(sd.xray,        xmlGet(body, "xray").c_str(),        sizeof(sd.xray));
    strlcpy(sd.geoField,    xmlGet(body, "geomagfield").c_str(), sizeof(sd.geoField));
    strlcpy(sd.signalNoise, xmlGet(body, "signalnoise").c_str(), sizeof(sd.signalNoise));

    // hamqsl.com solarxml.php uses range-pair band names.
    // Map 7 individual display bands to the 4 XML range pairs.
    static const char* DISP[]  = { "80m", "40m", "20m", "17m", "15m", "12m", "10m" };
    static const char* XML[]   = { "80m-40m","80m-40m","30m-20m","17m-15m","17m-15m","12m-10m","12m-10m" };
    sd.bandCount = 7;
    for (int i = 0; i < 7; i++) {
        strlcpy(sd.bands[i].band, DISP[i], sizeof(sd.bands[i].band));
        String dv = xmlGetBand(body, XML[i], "day");
        String nv = xmlGetBand(body, XML[i], "night");
        strlcpy(sd.bands[i].day,   dv.length() ? dv.c_str() : "?", sizeof(sd.bands[i].day));
        strlcpy(sd.bands[i].night, nv.length() ? nv.c_str() : "?", sizeof(sd.bands[i].night));
    }

    // Debug: show what the first <band> tag actually looks like
    {
        int ccPos = body.indexOf("<calculatedconditions>");
        if (ccPos < 0) {
            Serial.printf("[solar] WARNING: no <calculatedconditions> in body (len=%u)\n",
                          body.length());
        } else {
            int bpos = body.indexOf("<band", ccPos);
            if (bpos >= 0) {
                int bend = body.indexOf("</band>", bpos);
                String sample = body.substring(bpos, bend + 7);
                Serial.printf("[solar] band sample: %s\n", sample.c_str());
            }
        }
    }

    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    g_solar = sd;
    xSemaphoreGive(g_dataMutex);
    Serial.printf("[solar] OK  SFI:%d K:%d A:%d  80m-day:%s\n",
                  sd.sfi, sd.kIndex, sd.aIndex, sd.bands[0].day);
}

// ─── RSS PARSER ───────────────────────────────────────────────────────────────

// Find the real </item> end, correctly skipping any </item> that appears
// inside a CDATA section (e.g. inside <description><![CDATA[...]]></description>).
// itemStart points to '<' of "<item>" or "<item ...>".
static int findItemEnd(const String& body, int itemStart) {
    // Skip to the end of the opening tag (could be "<item>" or "<item rdf:about=...>")
    int tagClose = body.indexOf('>', itemStart);
    int pos = (tagClose >= 0) ? tagClose + 1 : itemStart + 6;
    while (true) {
        int cdataPos = body.indexOf("<![CDATA[", pos);
        int endPos   = body.indexOf("</item>",   pos);
        if (endPos < 0) return -1;
        if (cdataPos < 0 || endPos < cdataPos) return endPos;
        // A CDATA section starts before the next </item> — skip past it
        int cdataEnd = body.indexOf("]]>", cdataPos + 9);
        if (cdataEnd < 0) return -1;
        pos = cdataEnd + 3;
    }
}

static void parseRSS(const String& body, NewsData& nd, int maxItems) {
    nd.count = 0;
    nd.valid = true;
    int pos  = 0;

    while (nd.count < maxItems) {
        // Match both "<item>" (RSS 2.0) and "<item " (RSS 1.0 with rdf:about attr)
        int s1 = body.indexOf("<item>", pos);
        int s2 = body.indexOf("<item ", pos);
        int start;
        if      (s1 < 0 && s2 < 0) break;
        else if (s1 < 0)            start = s2;
        else if (s2 < 0)            start = s1;
        else                        start = min(s1, s2);

        // Use CDATA-aware end search to avoid false </item> matches.
        // If </item> is absent (body truncated mid-item, common with large chunked
        // feeds like MacRumors-All), fall through with whatever content we have so
        // at least the title and link can be extracted from the partial item.
        int end = findItemEnd(body, start);
        bool truncated = (end < 0);
        String item = truncated ? body.substring(start)
                                : body.substring(start, end + 7);

        String title = xmlGet(item, "title");
        title.replace("<![CDATA[", ""); title.replace("]]>", ""); title.trim();
        title = decodeHtmlEntities(title);

        // Thumbnail URL from <media:thumbnail url="..."/> or <enclosure url="..."/>
        // Search for <media:thumbnail> OUTSIDE any CDATA block: advance past any CDATA
        // that starts before the tag position to avoid false matches in descriptions.
        String thumbUrl;
        {
            int mt = -1;
            int searchFrom = 0;
            while (true) {
                int candidate = item.indexOf("<media:thumbnail", searchFrom);
                if (candidate < 0) break;
                // Check whether this candidate is inside a CDATA section
                int cdS = item.lastIndexOf("<![CDATA[", candidate);
                int cdE = (cdS >= 0) ? item.indexOf("]]>", cdS + 9) : -1;
                if (cdS >= 0 && (cdE < 0 || cdE > candidate)) {
                    // Inside CDATA — skip past it
                    searchFrom = (cdE >= 0) ? cdE + 3 : item.length();
                    continue;
                }
                mt = candidate;
                break;
            }
            if (mt >= 0) {
                int me = item.indexOf("/>", mt);
                if (me < 0) me = item.indexOf('>', mt);
                if (me >= 0) {
                    String mtag = item.substring(mt, me);
                    thumbUrl = xmlAttr(mtag, "url");
                    thumbUrl.replace("/208/", "/96/");
                    thumbUrl.replace("/240/", "/96/");
                    thumbUrl.replace("/400/", "/96/");
                    // BBC's ichef CDN serves the same image as JPEG or PNG.
                    // Request JPEG — it's ~5 KB vs ~20 KB for PNG, safely fits
                    // the 6 KB buffer and doesn't need a second draw path.
                    if (thumbUrl.indexOf("ichef.bbci.co.uk") >= 0)
                        thumbUrl.replace(".png", ".jpg");
                }
            }
        }
        if (thumbUrl.isEmpty()) {
            int enc = item.indexOf("<enclosure");
            if (enc >= 0) {
                int ence = item.indexOf("/>", enc);
                if (ence < 0) ence = item.indexOf('>', enc);
                if (ence >= 0) {
                    String etag = item.substring(enc, ence);
                    String type = xmlAttr(etag, "type");
                    if (type.indexOf("image") >= 0 || type.isEmpty())
                        thumbUrl = xmlAttr(etag, "url");
                }
            }
        }
        // Third fallback: <img src="..."> embedded inside description CDATA.
        // Feeds like MacRumors/9to5Mac only provide images this way.
        // We pick the first img whose src ends in a known image extension.
        if (thumbUrl.isEmpty()) {
            int imgPos = 0;
            while (thumbUrl.isEmpty()) {
                int it = item.indexOf("<img", imgPos);
                if (it < 0) break;
                int ie = item.indexOf('>', it);
                if (ie < 0) break;
                String itag = item.substring(it, ie + 1);
                String src  = xmlAttr(itag, "src");
                if (src.length() > 0) {
                    String sl = src; sl.toLowerCase();
                    if (sl.indexOf(".jpg")  >= 0 || sl.indexOf(".jpeg") >= 0 ||
                        sl.indexOf(".png")  >= 0 || sl.indexOf(".webp") >= 0)
                        thumbUrl = src;
                }
                imgPos = ie + 1;
            }
        }
        // Sanity-check the URL: discard if it contains spaces, newlines, or XML
        // syntax — a sign the extraction ran past the actual attribute value
        // (e.g. matched <media:thumbnail> inside a description CDATA section).
        for (int i = 0; i < (int)thumbUrl.length(); i++) {
            char c = thumbUrl[i];
            if (c == ' ' || c == '<' || c == '>' || c == '\n' || c == '\r') {
                thumbUrl = "";
                break;
            }
        }

        if (title.length() > 0) {
            strlcpy(nd.items[nd.count].title,    title.c_str(),    sizeof(nd.items[0].title));
            strlcpy(nd.items[nd.count].thumbUrl, thumbUrl.c_str(), sizeof(nd.items[0].thumbUrl));
            nd.count++;
        }
        if (truncated) break;   // no more complete items after a body-truncated one
        pos = end + 7;
    }
}

static void fetchThumb(const char* thumbUrl, uint8_t* buf, size_t& len, uint32_t& version) {
    if (!thumbUrl || thumbUrl[0] == '\0' || !buf) { len = 0; return; }

    // Download into a temporary buffer so the shared buf is never partially
    // written while the draw task is decoding it from a concurrent core.
    static uint8_t* tmpBuf = nullptr;
    if (!tmpBuf) tmpBuf = (uint8_t*)malloc(THUMB_BUF_LEN);
    if (!tmpBuf) { len = 0; return; }

    size_t got = httpGetBytes(thumbUrl, tmpBuf, THUMB_BUF_LEN, 1, 8000);

    if (got == 0 && strncmp(thumbUrl, "https://", 8) == 0) {
        char httpUrl[256];
        snprintf(httpUrl, sizeof(httpUrl), "http://%s", thumbUrl + 8);
        got = httpGetBytes(httpUrl, tmpBuf, THUMB_BUF_LEN, 1, 8000);
    }

    // Atomically swap the buffer under the mutex and bump the version so the
    // draw task knows to re-decode the JPEG into its cached thumbnail sprite.
    // If got == THUMB_BUF_LEN the image was truncated — keep the old thumbnail.
    if (got > 0 && got < THUMB_BUF_LEN) {
        xSemaphoreTake(g_dataMutex, portMAX_DELAY);
        memcpy(buf, tmpBuf, got);
        len = got;
        ++version;
        xSemaphoreGive(g_dataMutex);
    }
}

// ─── RSS streaming helpers ────────────────────────────────────────────────────
//
// Scan stream for the next <item> or <item attr...> opening tag.
// Consumes and discards everything up to and including the tag opener.
// Returns true if found before deadline / disconnect.
static bool streamSkipToItem(WiFiClient* s, uint32_t deadline) {
    const char NEEDLE[] = "<item";
    const int  NLEN = 5;
    int match = 0;
    while (millis() < deadline) {
        if (!s->available()) {
            if (!s->connected()) return false;
            vTaskDelay(pdMS_TO_TICKS(2)); continue;
        }
        char c = (char)s->read();
        if (c == NEEDLE[match]) {
            if (++match == NLEN) {
                // Peek next char: must be '>' or ' ' to be a real <item>/<item ...>
                uint32_t t2 = millis() + 300;
                while (millis() < t2 && !s->available()) vTaskDelay(pdMS_TO_TICKS(1));
                if (!s->available()) return false;
                char nc = (char)s->read();
                if (nc == '>' || nc == ' ') return true;
                // e.g. "<items>" — not an item tag; continue (nc already consumed)
                match = (nc == NEEDLE[0]) ? 1 : 0;
            }
        } else {
            match = (c == NEEDLE[0]) ? 1 : 0;
        }
    }
    return false;
}

// Read the body of one RSS item (from just after the opening <item...> tag)
// into buf[0..bufLen-1], searching for the </item> closing tag.
// Prepends "<item>" so that parseRSS can locate the opening tag.
// On buffer overflow: continues draining the stream until </item> is found.
// Returns bytes written to buf (>= PLEN if any content), or -1 on timeout.
static int streamReadItem(WiFiClient* s, char* buf, int bufLen, uint32_t deadline) {
    const char PREFIX[] = "<item>";
    const int  PLEN = 6;
    memcpy(buf, PREFIX, PLEN);

    const char NEEDLE[] = "</item>";
    const int  NLEN = 7;
    int  n    = PLEN;
    int  match = 0;
    bool over = false;

    while (millis() < deadline) {
        if (!s->available()) {
            if (!s->connected()) return -1;
            vTaskDelay(pdMS_TO_TICKS(2)); continue;
        }
        char c = (char)s->read();

        if (!over) {
            if (n < bufLen - 1) buf[n++] = c;
            else                 over = true;
        }

        if (c == NEEDLE[match]) {
            if (++match == NLEN) {
                buf[over ? bufLen - 1 : n] = '\0';
                return n;
            }
        } else {
            match = (c == NEEDLE[0]) ? 1 : 0;
        }
    }
    return -1;
}

// ─── RSS news fetch helper ────────────────────────────────────────────────────
// Streaming item-by-item parser — NO large static buffer in .bss.
//
// Each <item>...</item> block is read into a 1 KB stack buffer (one at a time).
// After the buffer is filled the stream is drained to </item> without storing.
// This captures title (always near the top of an item) and media:thumbnail /
// enclosure for feeds that place them before a long <description> block
// (BBC, NASA).  For feeds that only embed images deep inside description
// CDATA (some MacRumors items), the thumbnail may not be captured, but the
// title and all 4 items are always extracted.
//
// itemBuf (1 KB) is static — lives in .bss, not on the task stack.
// This avoids blowing the 8 KB FreeRTOS task stack during deep SSL call chains.
// During SSL in_buf+out_buf (~33 KB) are allocated; the ~5 KB remaining heap
// is enough for one small String(itemBuf) + parseRSS substrings per item.
//
// Returns true if nd.count > 0.
static bool fetchRSSNews(const char* url, const char* tag,
                         NewsData& nd, int maxItems) {
    for (int attempt = 1; attempt <= 3; attempt++) {
        if (!ensureWiFi()) { if (attempt < 3) delay(2000); continue; }
        Serial.printf("[fetch] heap=%u lb8bit=%u  attempt %d/3  %s\n",
                      ESP.getFreeHeap(),
                      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                      attempt, url);

        nd.count = 0;
        nd.valid = false;
        bool connected = false;

        sslCtxRelease();
        {
            WiFiClientSecure sc;
            sslDataRelease();   // x509 freed → coalesces → lb8bit ≈ 38.9 KB
            sc.setInsecure();
            {
                HTTPClient http;
                http.begin(sc, url);
                http.setTimeout(20000);
                http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
                http.addHeader("User-Agent", "CYD-Dashboard/1.0 ESP32");
                http.addHeader("Connection", "close");
                int code = http.GET();
                if (code != HTTP_CODE_OK) {
                    char sslErr[64] = "";
                    sc.lastError(sslErr, sizeof(sslErr));
                    Serial.printf("[fetch] %s => HTTP %d  ssl='%s'  lb8bit=%u\n",
                                  url, code, sslErr,
                                  heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
                    http.end();
                } else {
                    connected = true;
                    WiFiClient* stream = http.getStreamPtr();
                    uint32_t deadline = millis() + 20000;
                    // Static so it lives in .bss, not on the task stack.
                    // fetchRSSNews is always called sequentially (never re-entrant),
                    // so sharing one buffer across invocations is safe.
                    static char itemBuf[1024];

                    while (nd.count < maxItems && millis() < deadline) {
                        if (!streamSkipToItem(stream, deadline)) break;
                        int len = streamReadItem(stream, itemBuf, sizeof(itemBuf), deadline);
                        if (len < 0) break;
                        // Wrap the buffered item content in a temporary String for
                        // parseRSS.  ~1 KB alloc while in_buf+out_buf hold ~33 KB;
                        // the remaining ~5 KB is enough for this and its substrings.
                        String itemStr(itemBuf);
                        NewsData one;
                        parseRSS(itemStr, one, 1);
                        if (one.count > 0)
                            nd.items[nd.count++] = one.items[0];
                    }   // itemStr freed each iteration
                    nd.valid = true;
                    http.end();
                }
            }   // http destroyed
        }   // sc destroyed → in_buf + out_buf freed
        sslAllReclaim();

        Serial.printf("[%s] %d items  connected=%d\n", tag, nd.count, (int)connected);
        if (nd.count > 0) break;
        if (attempt < 3) delay(2000);
    }
    return nd.count > 0;
}

void fetchBBCNews() {
    NewsData nd;
    if (!fetchRSSNews(API_BBC_RSS, "bbc", nd, 4)) return;
    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    g_bbcNews = nd;
    xSemaphoreGive(g_dataMutex);
    fetchThumb(nd.items[0].thumbUrl, g_bbcThumbBuf, g_bbcThumbLen, g_bbcThumbVersion);
}

void fetchAppleNews() {
    NewsData nd;
    if (!fetchRSSNews(API_APPLE_RSS, "apple", nd, 4)) return;
    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    g_appleNews = nd;
    xSemaphoreGive(g_dataMutex);

    // MacRumors images are full-resolution (2000+ px) — far too large for the
    // 6 KB thumb buffer.  Route through wsrv.nl, a free image-resize CDN proxy,
    // to get a 118×66 JPEG (~3–5 KB) that fits the buffer exactly.
    // wsrv.nl accepts the URL without scheme in the url= parameter.
    if (nd.items[0].thumbUrl[0]) {
        const char* src = nd.items[0].thumbUrl;
        if (strncmp(src, "https://", 8) == 0) src += 8;
        else if (strncmp(src, "http://",  7) == 0) src += 7;
        char proxyUrl[320];
        snprintf(proxyUrl, sizeof(proxyUrl),
                 "https://wsrv.nl/?url=%s&w=118&h=66&output=jpg&fit=cover&q=75",
                 src);
        Serial.printf("[apple] thumb via wsrv.nl: %s\n", proxyUrl);
        fetchThumb(proxyUrl, g_appleThumbBuf, g_appleThumbLen, g_appleThumbVersion);
    }
}


// ─── S&P 500 — Yahoo Finance v8, byte-level stream search ────────────────────
// Streams the response and locates the "close":[ array without loading the
// entire ~50 KB JSON into RAM.  No ArduinoJson involved (the filter approach
// silently discarded the data on this endpoint).
void fetchTracker() {
    char trackerUrl[128];
    snprintf(trackerUrl, sizeof(trackerUrl),
             "https://query1.finance.yahoo.com/v8/finance/chart/%s?interval=1wk&range=%dy",
             g_settings.trackerSymbol, (int)g_settings.trackerRangeYears);

    float    closes[TRACKER_POINTS];
    int      closeCount = 0;
    size_t   total      = 0;

    sslCtxRelease();
    {
        WiFiClientSecure sc;            // sslclient_context → ctx slot ✓
        sslDataRelease();               // in/out/x509 slots freed
        sc.setInsecure();
        {
            HTTPClient http;
            http.begin(sc, trackerUrl);
            http.setTimeout(25000);
            http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
            http.addHeader("User-Agent",
                "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                "AppleWebKit/537.36 (KHTML, like Gecko) "
                "Chrome/124.0.0.0 Safari/537.36");
            http.addHeader("Accept", "*/*");
            http.addHeader("Connection", "close");
            int code = http.GET();
            Serial.printf("[tracker] HTTP %d\n", code);
            if (code != HTTP_CODE_OK) {
                http.end();
            } else {
                WiFiClient* stream = http.getStreamPtr();

                // State machine: scan for  "close":[  then parse ASCII floats.
                static const char NEEDLE[]  = "\"close\":[";
                const int         NEEDLE_LEN = 9;
                int      matchPos = 0;
                bool     inArray  = false;
                bool     done     = false;
                char     numBuf[20];
                int      numLen   = 0;
                uint32_t t0       = millis();
                uint8_t  cbuf[256];

                while (!done && (millis() - t0) < 25000 && total < 300000UL) {
                    int av = stream->available();
                    if (av <= 0) {
                        if (!stream->connected()) break;
                        delay(5);
                        continue;
                    }
                    size_t n = stream->readBytes(cbuf,
                                   (size_t)min(av, (int)sizeof(cbuf)));
                    total += n;
                    for (size_t i = 0; i < n && !done; i++) {
                        char ch = (char)cbuf[i];
                        if (!inArray) {
                            if (ch == NEEDLE[matchPos]) {
                                if (++matchPos == NEEDLE_LEN) { inArray = true; matchPos = 0; }
                            } else {
                                matchPos = (ch == NEEDLE[0]) ? 1 : 0;
                            }
                        } else {
                            if ((ch >= '0' && ch <= '9') || ch == '.') {
                                if (numLen < 18) numBuf[numLen++] = ch;
                            } else if (ch == '-' && numLen == 0) {
                                numBuf[numLen++] = ch;
                            } else {
                                if (numLen > 0) {
                                    numBuf[numLen] = '\0';
                                    float v = atof(numBuf);
                                    if (v > 100.0f && closeCount < TRACKER_POINTS)
                                        closes[closeCount++] = v;
                                    numLen = 0;
                                }
                                if (ch == ']') done = true;
                            }
                        }
                    }
                }
                if (numLen > 0 && closeCount < TRACKER_POINTS) {
                    numBuf[numLen] = '\0';
                    float v = atof(numBuf);
                    if (v > 100.0f) closes[closeCount++] = v;
                }
                http.end();
            }
        }                               // http destroyed
    }                                   // sc destroyed
    sslAllReclaim();
    Serial.printf("[tracker] read=%u  closes=%d\n", (unsigned)total, closeCount);

    if (closeCount < 2) {
        Serial.println("[tracker] insufficient data — check URL/network");
        return;
    }

    // Yahoo Finance returns data in chronological order (oldest first)
    TrackerData sd;
    sd.valid     = true;
    sd.price     = closes[closeCount - 1];
    sd.change    = closes[closeCount - 1] - closes[closeCount - 2];
    sd.changePct = (sd.change / closes[closeCount - 2]) * 100.0f;
    sd.histCount = (uint8_t)min(closeCount, TRACKER_POINTS);
    for (int i = 0; i < sd.histCount; i++)
        sd.history[i] = closes[closeCount - sd.histCount + i];

    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    g_tracker = sd;
    xSemaphoreGive(g_dataMutex);
    Serial.printf("[tracker] OK  $%.2f  %+.2f%%  pts=%d\n",
                  sd.price, sd.changePct, sd.histCount);
}

// ─── DX SPOTS ─────────────────────────────────────────────────────────────────
// Sorted by great-circle distance from the user's QTH to the SPOTTER's location,
// so the screen shows "what DX is being heard near me right now."
//
// Spotter location is derived from the callsign prefix using a compact lookup
// table (~50 common DXCC entities).  If the API ever adds lat/lon fields to the
// JSON response (possible with full=1) those take priority over the table.
//
// Duplicate spotted callsigns are collapsed: if the same DX station is spotted
// by two nearby stations, we keep whichever spotter is closer to the user.

struct PfxEntry { char pfx[5]; int8_t lat; int16_t lon; };
static const PfxEntry PFX_LOC[] = {
    // 3-char prefixes first — must appear before shorter prefixes for longest-match
    {"HB0", 47,   9}, // Liechtenstein
    {"HB9", 47,   8}, // Switzerland
    {"IS0", 40,   9}, // Sardinia
    {"KH6", 21,-157}, // Hawaii
    {"KL7", 64,-153}, // Alaska
    {"UA9", 60,  80}, // Asiatic Russia
    // 2-char prefixes
    {"A4",  22,  58}, // Oman
    {"A6",  24,  54}, // UAE
    {"BV",  24, 121}, // Taiwan
    {"BY",  36, 104}, // China
    {"C6",  25, -77}, // Bahamas
    {"CE", -33, -71}, // Chile
    {"CT",  39,  -9}, // Portugal
    {"D4",  15, -24}, // Cape Verde
    {"DL",  51,  10}, // Germany
    {"EA",  40,  -4}, // Spain
    {"EI",  53,  -8}, // Ireland
    {"HA",  47,  19}, // Hungary
    {"HZ",  24,  45}, // Saudi Arabia
    {"JA",  36, 138}, // Japan
    {"LA",  60,  10}, // Norway
    {"LU", -34, -64}, // Argentina
    {"LY",  56,  24}, // Lithuania
    {"LZ",  43,  25}, // Bulgaria
    {"OA", -12, -77}, // Peru
    {"OE",  47,  14}, // Austria
    {"OH",  61,  26}, // Finland
    {"OK",  50,  15}, // Czech Republic
    {"OM",  49,  19}, // Slovakia
    {"ON",  51,   4}, // Belgium
    {"OZ",  56,  10}, // Denmark
    {"PA",  52,   5}, // Netherlands
    {"PY", -15, -47}, // Brazil
    {"S5",  46,  14}, // Slovenia
    {"SM",  59,  15}, // Sweden
    {"SP",  52,  20}, // Poland
    {"SV",  38,  24}, // Greece
    {"UA",  55,  37}, // Russia (European)
    {"UR",  49,  32}, // Ukraine
    {"VE",  56, -96}, // Canada
    {"VK", -25, 133}, // Australia
    {"VU",  23,  80}, // India
    {"YB",  -6, 107}, // Indonesia
    {"YO",  45,  25}, // Romania
    {"YU",  44,  21}, // Serbia
    {"Z2", -20,  30}, // Zimbabwe
    {"ZL", -41, 172}, // New Zealand
    {"ZS", -30,  25}, // South Africa
    // 1-char prefixes (catch-all after longer matches fail)
    {"F",   46,   2}, // France
    {"G",   53,  -2}, // UK
    {"I",   42,  14}, // Italy
    {"K",   39, -98}, // USA
    {"N",   39, -98}, // USA
    {"W",   39, -98}, // USA
};
static const int PFX_N = (int)(sizeof(PFX_LOC) / sizeof(PFX_LOC[0]));

// Longest-match prefix lookup: tries lengths 3, 2, 1 in order.
static bool pfxToLatLon(const char* call, float& lat, float& lon) {
    char base[12];
    strlcpy(base, call, sizeof(base));
    char* sl = strchr(base, '/');
    if (sl) *sl = '\0';                    // strip /P /M /suffix
    int blen = (int)strlen(base);
    for (int plen = 3; plen >= 1; plen--) {
        if (blen < plen) continue;
        char p[5] = {};
        strncpy(p, base, plen);
        for (int i = 0; i < PFX_N; i++) {
            if (strcasecmp(PFX_LOC[i].pfx, p) == 0) {
                lat = (float)PFX_LOC[i].lat;
                lon = (float)PFX_LOC[i].lon;
                return true;
            }
        }
    }
    return false;
}

// Haversine great-circle distance in km.
static float gcKm(float la1, float lo1, float la2, float lo2) {
    const float R   = 6371.0f;
    const float D2R = (float)M_PI / 180.0f;
    float dlat = (la2 - la1) * D2R;
    float dlon = (lo2 - lo1) * D2R;
    float a = sinf(dlat * 0.5f) * sinf(dlat * 0.5f) +
              cosf(la1 * D2R) * cosf(la2 * D2R) *
              sinf(dlon * 0.5f) * sinf(dlon * 0.5f);
    return R * 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));
}

// Initial bearing from (la1,lo1) toward (la2,lo2), degrees 0–360.
static float gcBearing(float la1, float lo1, float la2, float lo2) {
    const float D2R = (float)M_PI / 180.0f;
    float dlon = (lo2 - lo1) * D2R;
    float y    = sinf(dlon) * cosf(la2 * D2R);
    float x    = cosf(la1 * D2R) * sinf(la2 * D2R)
               - sinf(la1 * D2R) * cosf(la2 * D2R) * cosf(dlon);
    float b    = atan2f(y, x) / D2R;
    if (b < 0.0f) b += 360.0f;
    return b;
}

void fetchDXSpots() {
    String body = httpGet(API_DXSPOTS, 24576);
    if (body.isEmpty()) return;

    if (body.indexOf("<spot") < 0) {
        Serial.println("[dx] XML: no <spot> elements found");
        return;
    }

    // Candidate pool — static to stay off the 8 KB task stack.
    struct Cand { DXSpot spot; float dist; };
    static Cand cands[50];
    int nc = 0;

    // Walk through every <spot ...>...</spot> block.
    int searchFrom = 0;
    while (nc < 50) {
        int tagStart = body.indexOf("<spot", searchFrom);
        if (tagStart < 0) break;
        int blockEnd = body.indexOf("</spot>", tagStart);
        if (blockEnd < 0) break;
        blockEnd += 7;  // include closing tag
        String block = body.substring(tagStart, blockEnd);
        searchFrom = blockEnd;

        String dxStr  = xmlGet(block, "dx");
        String sptStr = xmlGet(block, "spotter");
        String frStr  = xmlGet(block, "frequency");
        String cmtStr = xmlGet(block, "comment");
        String tmStr  = xmlGet(block, "time");   // "2026-05-26 10:28:00"

        if (dxStr.isEmpty() || frStr.isEmpty()) continue;
        float fr = frStr.toFloat();
        if (fr < 1.0f) continue;

        // Trim whitespace from callsigns.
        dxStr.trim();
        sptStr.trim();

        // Decode HTML entities in comment (&lt; &gt; &amp; &apos; etc.)
        cmtStr = decodeHtmlEntities(cmtStr);
        cmtStr.trim();

        // Convert "YYYY-MM-DD HH:MM:SS" → "HH:MMZ"
        char tmBuf[8] = "";
        {
            int sp = tmStr.indexOf(' ');
            if (sp >= 0 && sp + 6 <= (int)tmStr.length()) {
                String hhmm = tmStr.substring(sp + 1, sp + 6);  // "HH:MM"
                snprintf(tmBuf, sizeof(tmBuf), "%sZ", hhmm.c_str());
            } else {
                strlcpy(tmBuf, tmStr.c_str(), sizeof(tmBuf));
            }
        }

        const char* dx  = dxStr.c_str();
        const char* spt = sptStr.c_str();
        const char* cmt = cmtStr.c_str();

        // Spotter location from prefix table.
        float sLat = 0.0f, sLon = 0.0f;
        bool  gotLoc = pfxToLatLon(spt, sLat, sLon);
        float dist = gotLoc ? gcKm(g_settings.lat, g_settings.lon, sLat, sLon)
                            : 1e9f;   // unknowns sort to end

        // DX station location, distance, and bearing from QTH.
        float dxLat = 0.0f, dxLon = 0.0f;
        bool  gotDxLoc = pfxToLatLon(dx, dxLat, dxLon);
        uint16_t dxDistKm = 0;
        uint16_t dxBear   = 0xFFFF;
        if (gotDxLoc) {
            float d = gcKm(g_settings.lat, g_settings.lon, dxLat, dxLon);
            float b = gcBearing(g_settings.lat, g_settings.lon, dxLat, dxLon);
            dxDistKm = (d < 65535.0f) ? (uint16_t)d : 0;
            dxBear   = (uint16_t)(b + 0.5f) % 360;
        }

        // Deduplicate by spotted callsign — keep the closer spotter.
        bool dup = false;
        for (int i = 0; i < nc; i++) {
            if (strcasecmp(cands[i].spot.dx, dx) == 0) {
                if (dist < cands[i].dist) {
                    cands[i].dist = dist;
                    strlcpy(cands[i].spot.spotter, spt,   sizeof(cands[i].spot.spotter));
                    strlcpy(cands[i].spot.time,    tmBuf, sizeof(cands[i].spot.time));
                    strlcpy(cands[i].spot.comment, cmt,   sizeof(cands[i].spot.comment));
                    cands[i].spot.dist_km = (gotLoc && dist < 65535.0f)
                                          ? (uint16_t)dist : 0;
                }
                dup = true;
                break;
            }
        }
        if (dup) continue;

        DXSpot& s = cands[nc].spot;
        strlcpy(s.dx,      dx,    sizeof(s.dx));
        strlcpy(s.spotter, spt,   sizeof(s.spotter));
        s.freq = fr;
        strlcpy(s.comment, cmt,   sizeof(s.comment));
        strlcpy(s.time,    tmBuf, sizeof(s.time));
        s.dist_km    = (gotLoc && dist < 65535.0f) ? (uint16_t)dist : 0;
        s.dx_dist_km = dxDistKm;
        s.dx_bearing = dxBear;
        cands[nc].dist = dist;
        nc++;
    }

    if (nc == 0) { Serial.println("[dx] no valid spots"); return; }

    // Insertion-sort by distance (nearest first; unknowns at end).
    for (int i = 1; i < nc; i++) {
        Cand tmp = cands[i];
        int  j   = i - 1;
        while (j >= 0 && cands[j].dist > tmp.dist) {
            cands[j + 1] = cands[j];
            j--;
        }
        cands[j + 1] = tmp;
    }

    DXSpotsData ds;
    ds.valid = true;
    ds.count = (uint8_t)min(nc, DX_SPOTS_MAX);
    for (int i = 0; i < ds.count; i++) ds.spots[i] = cands[i].spot;

    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    g_dxSpots = ds;
    xSemaphoreGive(g_dataMutex);
    Serial.printf("[dx] OK  %d spots (from %d unique)\n", ds.count, nc);
}

// ─── ISS position + orbit track ───────────────────────────────────────────────
// api.open-notify.org  (plain HTTP, ~130-byte JSON response)
// {"iss_position": {"longitude": "-12.34", "latitude": "45.67"}, ...}
//
// After getting the current position we compute the upcoming ground-track
// analytically using classical orbital mechanics:
//   Inclination  i  = 51.6°
//   Orbital period  = 5561 s  (~92.7 min)
//   Earth rotation  = 360° / 86400 s
//
// Two consecutive lat readings determine ascending/descending so the correct
// quadrant of the argument-of-latitude u₀ can be selected.
void fetchISS() {
    String body = httpGet(API_ISS, 512);
    if (body.isEmpty()) return;

    int latIdx = body.indexOf("\"latitude\"");
    int lonIdx = body.indexOf("\"longitude\"");
    if (latIdx < 0 || lonIdx < 0) return;

    auto parseVal = [&](int keyIdx) -> float {
        int colon = body.indexOf(':', keyIdx);
        if (colon < 0) return 0.0f;
        int q1 = body.indexOf('"', colon);
        if (q1 < 0) return 0.0f;
        int q2 = body.indexOf('"', q1 + 1);
        if (q2 < 0) return 0.0f;
        return body.substring(q1 + 1, q2).toFloat();
    };

    const float lat = parseVal(latIdx);
    const float lon = parseVal(lonIdx);

    // ── Ascending / descending from previous reading ──────────────────────────
    static float s_prevLat = 999.0f;   // 999 = no previous reading yet
    bool ascending = (s_prevLat > 90.0f) ? true : (lat >= s_prevLat);
    s_prevLat = lat;

    // ── Orbital constants ─────────────────────────────────────────────────────
    const float INC     = 51.6f * (float)M_PI / 180.0f;   // inclination, rad
    const float PERIOD  = 5561.0f;                          // orbital period, s
    const float N       = 2.0f * (float)M_PI / PERIOD;     // mean motion, rad/s
    const float EARTH_W = 360.0f / 86400.0f;               // Earth rotation, °/s

    // ── Solve for argument of latitude u₀ ────────────────────────────────────
    // sin(lat) = sin(i) × sin(u)  →  u = arcsin(sin(lat) / sin(i))
    float sinU = sinf(lat * (float)M_PI / 180.0f) / sinf(INC);
    sinU = constrain(sinU, -1.0f, 1.0f);
    float u0 = asinf(sinU);   // result in [-π/2, π/2]

    // Place u₀ in the correct quadrant:
    //  ascending,  lat ≥ 0:  u ∈ [0,   π/2]  → keep u0
    //  ascending,  lat < 0:  u ∈ [3π/2, 2π]  → u0 = 2π + u0
    //  descending, any lat:  u ∈ [π/2,  3π/2] → u0 = π  - u0
    if (!ascending) {
        u0 = (float)M_PI - u0;
    } else if (lat < 0.0f) {
        u0 = 2.0f * (float)M_PI + u0;
    }

    // Orbital longitude angle at t=0 (for computing Δλ each step)
    const float lambda0 = atan2f(cosf(INC) * sinf(u0), cosf(u0));

    // ── Generate 32-point ground track at 180 s intervals (~96 min) ──────────
    ISSData iss;
    iss.valid      = true;
    iss.lat        = lat;
    iss.lon        = lon;
    iss.trackCount = 32;

    for (int k = 0; k < iss.trackCount; k++) {
        const float dt = k * 180.0f;
        const float u  = u0 + N * dt;

        float tlat = asinf(constrain(sinf(INC) * sinf(u), -1.0f, 1.0f))
                     * 180.0f / (float)M_PI;
        float dLambda = atan2f(cosf(INC) * sinf(u), cosf(u)) - lambda0;
        float tlon = lon + dLambda * 180.0f / (float)M_PI - EARTH_W * dt;

        // Normalise to [-180, 180]
        while (tlon >  180.0f) tlon -= 360.0f;
        while (tlon < -180.0f) tlon += 360.0f;

        iss.trackLat[k] = tlat;
        iss.trackLon[k] = tlon;
    }

    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    g_iss = iss;
    xSemaphoreGive(g_dataMutex);
    Serial.printf("[iss] lat=%.2f  lon=%.2f  %s  track=%d pts\n",
                  lat, lon, ascending ? "asc" : "desc", iss.trackCount);
}

// ─── SOTA Spots ───────────────────────────────────────────────────────────────
// api2.sota.org.uk/api/spots/30 — last 30 SOTA activations.
// No lat/lon in the response; distance is estimated by stripping the portable
// suffix (/P, /M, /QRP…) from the activator callsign and using pfxToLatLon().
void fetchSOTASpots() {
    String body = httpGet(API_SOTASPOTS, 16384);
    if (body.isEmpty()) return;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) { Serial.printf("[sota] JSON: %s\n", err.c_str()); return; }

    if (!doc.is<JsonArray>()) { Serial.println("[sota] expected array"); return; }
    JsonArray arr = doc.as<JsonArray>();

    struct SOTACand { SOTASpot spot; float dist; };
    static SOTACand cands[30];
    int nc = 0;

    for (JsonObject obj : arr) {
        if (nc >= 30) break;

        const char* act   = obj["activatorCallsign"] | "";
        const char* aCode = obj["associationCode"]   | "";
        const char* sCode = obj["summitCode"]        | "";
        const char* sdet  = obj["summitDetails"]     | "";
        const char* freq  = obj["frequency"]         | "0";
        const char* mode  = obj["mode"]              | "";
        const char* tm    = obj["timeStamp"]         | "";

        float fr = String(freq).toFloat();
        if (!act[0] || fr < 0.1f) continue;

        // Strip portable/mobile suffix for callsign-prefix lookup.
        char pfxBuf[14];
        strlcpy(pfxBuf, act, sizeof(pfxBuf));
        char* slash = strchr(pfxBuf, '/');
        if (slash) *slash = '\0';

        float sLat = 0.0f, sLon = 0.0f;
        bool  gotLoc = pfxToLatLon(pfxBuf, sLat, sLon);
        float dist   = gotLoc
                     ? gcKm(g_settings.lat, g_settings.lon, sLat, sLon)
                     : 1e9f;

        // Build summit ref "AA/BB-NNN" and extract name + altitude from summitDetails.
        char summitRef[12] = "";
        if (aCode[0] && sCode[0])
            snprintf(summitRef, sizeof(summitRef), "%s/%s", aCode, sCode);
        else
            strlcpy(summitRef, sCode, sizeof(summitRef));

        // summitDetails: "Fronalpstock, 1921m, 6 points"
        // Build display name: "Fronalpstock 1921m"
        char nameBuf[28] = "";
        {
            String sd(sdet);
            int c1 = sd.indexOf(',');
            String sname  = (c1 > 0) ? sd.substring(0, c1) : sd;
            int    c2     = (c1 > 0) ? sd.indexOf(',', c1 + 1) : -1;
            String sheight = "";
            if (c1 > 0) {
                String ht = (c2 > 0) ? sd.substring(c1 + 2, c2)
                                     : sd.substring(c1 + 2);
                ht.trim();
                if (ht.length() > 0 && ht.endsWith("m")) sheight = " " + ht;
            }
            String full = sname + sheight;
            strlcpy(nameBuf, full.c_str(), sizeof(nameBuf));
        }

        // Convert "2026-05-26T11:20:07" → "11:20Z"
        char tmBuf[8] = "";
        {
            const char* t = strchr(tm, 'T');
            if (t && strlen(t) >= 6)
                snprintf(tmBuf, sizeof(tmBuf), "%.5sZ", t + 1);
            else
                strlcpy(tmBuf, tm, sizeof(tmBuf));
        }

        // Deduplicate by activator — keep the closest entry.
        bool dup = false;
        for (int i = 0; i < nc; i++) {
            if (strcasecmp(cands[i].spot.activator, act) == 0) {
                if (dist < cands[i].dist) {
                    cands[i].dist          = dist;
                    cands[i].spot.dist_km  = (gotLoc && dist < 65535.0f)
                                           ? (uint16_t)dist : 0;
                    strlcpy(cands[i].spot.time, tmBuf, sizeof(cands[i].spot.time));
                }
                dup = true; break;
            }
        }
        if (dup) continue;

        SOTASpot& s = cands[nc].spot;
        strlcpy(s.activator, act,       sizeof(s.activator));
        strlcpy(s.summit,    summitRef, sizeof(s.summit));
        strlcpy(s.name,      nameBuf,   sizeof(s.name));
        s.freq = fr;
        strlcpy(s.mode, mode,  sizeof(s.mode));
        strlcpy(s.time, tmBuf, sizeof(s.time));
        s.dist_km = (gotLoc && dist < 65535.0f) ? (uint16_t)dist : 0;
        cands[nc].dist = dist;
        nc++;
    }

    if (nc == 0) { Serial.println("[sota] no valid spots"); return; }

    // Insertion-sort nearest first.
    for (int i = 1; i < nc; i++) {
        SOTACand tmp = cands[i];
        int j = i - 1;
        while (j >= 0 && cands[j].dist > tmp.dist) { cands[j+1] = cands[j]; j--; }
        cands[j+1] = tmp;
    }

    SOTASpotsData ds;
    ds.valid = true;
    ds.count = (uint8_t)min(nc, SOTA_SPOTS_MAX);
    for (int i = 0; i < ds.count; i++) ds.spots[i] = cands[i].spot;

    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    g_sotaSpots = ds;
    xSemaphoreGive(g_dataMutex);
    Serial.printf("[sota] OK  %d spots (from %d)\n", ds.count, nc);
}

// ─── POTA Spots ───────────────────────────────────────────────────────────────
// api.pota.app/spot/activator returns a JSON array of active POTA activations.
// Each entry includes lat/lon, so we calculate the Haversine distance directly
// rather than using the callsign-prefix table.  Deduplicate by activator
// callsign keeping the closest entry, then sort nearest-first.
//
// Memory strategy: hand-rolled streaming parser — reads one JSON object at a
// time from the TCP stream into a small static buffer, extracts the 8 fields
// we need, then immediately processes and discards it.  Peak extra allocation
// is ~2 KB regardless of response size (was 40+ KB with ArduinoJson).

// Read the next `{…}` object from a stream into buf.
// Returns object length (>0), 0 at end of array (']' seen), or -1 on timeout.
// Objects larger than bufLen-1 are consumed but return -2 (caller should skip).
static int potaReadObj(WiFiClient* s, char* buf, int bufLen) {
    int  depth = 0, len = 0;
    bool inStr = false, esc = false, started = false, overflow = false;
    unsigned long deadline = millis() + 15000UL;

    while (millis() < deadline) {
        if (!s->available()) { delay(1); continue; }
        char c = (char)s->read();
        if (!started) {
            if (c == '{')         { started = true; depth = 1; buf[0] = c; len = 1; }
            else if (c == ']')    return 0;   // clean end of top-level array
            continue;
        }
        if (len < bufLen - 1)     buf[len++] = c;
        else                      overflow = true;

        if (esc)                  { esc = false; continue; }
        if (c == '\\' && inStr)   { esc = true;  continue; }
        if (c == '"')             { inStr = !inStr; continue; }
        if (!inStr) {
            if      (c == '{') depth++;
            else if (c == '}') { if (--depth == 0) { buf[len] = '\0'; return overflow ? -2 : len; } }
        }
    }
    return -1;  // timeout
}

// Extract the value for `key` from a flat JSON object string into out[outLen].
// Handles both quoted strings and unquoted scalars (numbers, null, bool).
static void potaGetField(const char* obj, const char* key, char* out, int outLen) {
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(obj, pat);
    if (!p) { out[0] = '\0'; return; }
    p += strlen(pat);
    while (*p == ' ') p++;
    if (*p != ':') { out[0] = '\0'; return; }
    p++;
    while (*p == ' ') p++;
    if (*p == '"') {
        p++;
        int i = 0;
        while (*p && *p != '"' && i < outLen - 1) {
            if (*p == '\\' && *(p + 1)) p++;   // skip escape char
            out[i++] = *p++;
        }
        out[i] = '\0';
    } else {
        // unquoted value (number, null, bool)
        int i = 0;
        while (*p && *p != ',' && *p != '}' && *p != ' ' && i < outLen - 1)
            out[i++] = *p++;
        out[i] = '\0';
        if (strcmp(out, "null") == 0) out[0] = '\0';
    }
}

void fetchPOTASpots() {
    struct POTACand { POTASpot spot; float dist; };
    static POTACand cands[100];
    static char     objBuf[2048];
    int  nc      = 0;
    bool streamOk = false;

    for (int attempt = 1; attempt <= 3; attempt++) {
        if (!ensureWiFi()) { if (attempt < 3) delay(2000); continue; }
        Serial.printf("[fetch] heap=%u lb8bit=%u  attempt %d/3  %s\n",
                      ESP.getFreeHeap(),
                      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                      attempt, API_POTASPOTS);

        nc = 0;
        sslCtxRelease();
        {
            WiFiClientSecure sc;                // sslclient_context → ctx slot ✓
            sslDataRelease();                   // in/out/x509 slots freed
            sc.setInsecure();
            {
                HTTPClient http;
                http.begin(sc, API_POTASPOTS);
                http.setTimeout(20000);
                http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
                http.addHeader("User-Agent", "CYD-Dashboard/1.0 ESP32");
                http.addHeader("Connection", "close");
                int code = http.GET();
                if (code != HTTP_CODE_OK) {
                    Serial.printf("[pota] HTTP %d\n", code);
                    http.end();
                } else {
                    // Stream parse: one object at a time — no large heap alloc.
                    WiFiClient* stream = http.getStreamPtr();
                    int objLen;
                    while ((objLen = potaReadObj(stream, objBuf, sizeof(objBuf))) != -1) {
                        if (objLen == 0) { streamOk = true; break; }  // ']' seen
                        if (objLen < 0) continue;                      // oversized, skip

                        char act[14], ref[10], name[32], freq[12], mode[6];
                        char tm[24], latS[14], lonS[14];
                        potaGetField(objBuf, "activator", act,  sizeof(act));
                        potaGetField(objBuf, "reference", ref,  sizeof(ref));
                        potaGetField(objBuf, "name",      name, sizeof(name));
                        potaGetField(objBuf, "frequency", freq, sizeof(freq));
                        potaGetField(objBuf, "mode",      mode, sizeof(mode));
                        potaGetField(objBuf, "spotTime",  tm,   sizeof(tm));
                        potaGetField(objBuf, "latitude",  latS, sizeof(latS));
                        potaGetField(objBuf, "longitude", lonS, sizeof(lonS));

                        float fr = atof(freq);
                        if (!act[0] || fr < 1.0f) continue;

                        float lat = atof(latS), lon = atof(lonS);
                        bool  gotLoc = (latS[0] && lonS[0] && (lat != 0.0f || lon != 0.0f));
                        float dist   = gotLoc
                                     ? gcKm(g_settings.lat, g_settings.lon, lat, lon)
                                     : 1e9f;

                        // Convert "2026-05-26T11:07:54" → "11:07Z"
                        char tmBuf[8] = "";
                        const char* tptr = strchr(tm, 'T');
                        if (tptr && strlen(tptr) >= 6)
                            snprintf(tmBuf, sizeof(tmBuf), "%.5sZ", tptr + 1);
                        else
                            strlcpy(tmBuf, tm, sizeof(tmBuf));

                        // Deduplicate by activator — keep closest.
                        bool dup = false;
                        for (int i = 0; i < nc; i++) {
                            if (strcasecmp(cands[i].spot.activator, act) == 0) {
                                if (dist < cands[i].dist) {
                                    cands[i].dist = dist;
                                    strlcpy(cands[i].spot.time, tmBuf, sizeof(cands[i].spot.time));
                                    cands[i].spot.dist_km = (gotLoc && dist < 65535.0f)
                                                          ? (uint16_t)dist : 0;
                                }
                                dup = true; break;
                            }
                        }
                        if (dup || nc >= 100) continue;

                        POTASpot& s = cands[nc].spot;
                        strlcpy(s.activator, act,  sizeof(s.activator));
                        strlcpy(s.reference, ref,  sizeof(s.reference));
                        strlcpy(s.parkName,  name, sizeof(s.parkName));
                        s.freq = fr;
                        strlcpy(s.mode, mode, sizeof(s.mode));
                        strlcpy(s.time, tmBuf, sizeof(s.time));
                        s.dist_km = (gotLoc && dist < 65535.0f) ? (uint16_t)dist : 0;
                        cands[nc].dist = dist;
                        nc++;
                    }
                    http.end();
                }
            }                                   // http destroyed
        }                                       // sc destroyed
        sslAllReclaim();

        if (streamOk || nc > 0) break;
        if (attempt < 3) delay(2000);
    }

    if (nc == 0) { Serial.println("[pota] no valid spots"); return; }

    // Insertion-sort nearest first.
    for (int i = 1; i < nc; i++) {
        POTACand tmp = cands[i];
        int j = i - 1;
        while (j >= 0 && cands[j].dist > tmp.dist) { cands[j + 1] = cands[j]; j--; }
        cands[j + 1] = tmp;
    }

    POTASpotsData ds;
    ds.valid = true;
    ds.count = (uint8_t)min(nc, POTA_SPOTS_MAX);
    for (int i = 0; i < ds.count; i++) ds.spots[i] = cands[i].spot;

    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    g_potaSpots = ds;
    xSemaphoreGive(g_dataMutex);
    Serial.printf("[pota] OK  %d spots (from %d)\n", ds.count, nc);
}

// ─── Contest Calendar ─────────────────────────────────────────────────────────
// contestcalendar.com/calendar.rss — standard RSS feed.
// Each <item> has:
//   <title>  contest name
//   <description>  one of two formats:
//     Same-day:  "0100Z-0159Z, May 26"
//     Multi-day: "0000Z, May 30 to 2359Z, May 31"
//
// We parse start/end as UTC timestamps, classify contests as active (ongoing)
// or upcoming (not started), discard those that have already ended, and store
// active ones first followed by upcoming in chronological order.

static int cxMonthNum(const String& s) {
    const char* mo[12] = {"Jan","Feb","Mar","Apr","May","Jun",
                          "Jul","Aug","Sep","Oct","Nov","Dec"};
    for (int i = 0; i < 12; i++) if (s.startsWith(mo[i])) return i + 1;
    return -1;
}

// Self-contained UTC epoch — avoids platform timezone dependencies.
static time_t cxUtcEpoch(int year, int mo, int dy, int hh, int mm) {
    static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    long days = (long)(year - 1970) * 365L;
    for (int y = 1970; y < year; y++)
        if ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) days++;
    bool leap = ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
    for (int m = 1; m < mo; m++) {
        days += mdays[m - 1];
        if (m == 2 && leap) days++;
    }
    days += dy - 1;
    return (time_t)(days * 86400L + hh * 3600L + mm * 60L);
}

// Parse "HHMM" at position pos in s.  Returns true and sets h/m on success.
static bool cxParseHHMM(const String& s, int pos, int& h, int& m) {
    if (pos + 4 > (int)s.length()) return false;
    h = (s[pos]-'0')*10 + (s[pos+1]-'0');
    m = (s[pos+2]-'0')*10 + (s[pos+3]-'0');
    return (h >= 0 && h <= 23 && m >= 0 && m <= 59);
}

// Skip over 'Z', ',', ' ' chars starting at pos; then parse "Mon DD".
static bool cxParseMonDD(const String& s, int pos, int& month, int& day) {
    while (pos < (int)s.length() &&
           (s[pos]=='Z' || s[pos]==',' || s[pos]==' ')) pos++;
    if (pos + 5 > (int)s.length()) return false;
    month = cxMonthNum(s.substring(pos));
    if (month < 0) return false;
    pos += 3;
    while (pos < (int)s.length() && s[pos] == ' ') pos++;
    day = s.substring(pos).toInt();
    return (day >= 1 && day <= 31);
}

// Parse description string into start/end UTC epoch values.
// Returns false if the string cannot be parsed.
static bool cxParseTimes(const String& desc, time_t& tStart, time_t& tEnd) {
    tStart = tEnd = 0;

    time_t now = time(NULL);
    struct tm gm; gmtime_r(&now, &gm);
    int curYear = gm.tm_year + 1900;
    int curMo   = gm.tm_mon + 1;

    int sh, sm, eh, em, sMo, sDy, eMo, eDy;
    int toIdx = desc.indexOf(" to ");

    if (toIdx > 0) {
        // Multi-day: "0000Z, May 30 to 2359Z, May 31"
        if (!cxParseHHMM(desc,  0, sh, sm)) return false;
        if (!cxParseMonDD(desc, 4, sMo, sDy)) return false;
        if (!cxParseHHMM(desc,  toIdx + 4, eh, em)) return false;
        if (!cxParseMonDD(desc, toIdx + 9, eMo, eDy)) return false;
    } else {
        // Same-day: "0100Z-0159Z, May 26"
        if (!cxParseHHMM(desc,  0, sh, sm)) return false;
        if (!cxParseHHMM(desc,  6, eh, em)) return false;
        if (!cxParseMonDD(desc, 10, sMo, sDy)) return false;
        eMo = sMo; eDy = sDy;
    }

    // Resolve year — if start month is far behind current, assume next year.
    int sYear = curYear;
    if (sMo < curMo - 3) sYear++;

    int eYear = sYear;
    if (eMo < sMo) eYear++;  // e.g. Dec → Jan wrap

    tStart = cxUtcEpoch(sYear, sMo, sDy, sh, sm);
    tEnd   = cxUtcEpoch(eYear, eMo, eDy, eh, em);

    // Same-day contest crossing midnight: end is next day.
    if (toIdx < 0 && tEnd < tStart) tEnd += 86400;

    return (tStart > 0 && tEnd >= tStart);
}

void fetchContests() {
    String body = httpGet(API_CONTESTS, 32768);
    if (body.isEmpty()) return;

    if (body.indexOf("<item>") < 0) {
        Serial.println("[cx] no <item> elements");
        return;
    }

    time_t now = time(NULL);

    // Two buckets: active (up to CONTEST_MAX) and upcoming (up to CONTEST_MAX).
    // We merge them (active first) before storing.
    struct CxEntry { Contest c; time_t tStart; };
    static CxEntry active[CONTEST_MAX], upcoming[CONTEST_MAX];
    int nActive = 0, nUpcoming = 0;

    int searchFrom = 0;
    while (nActive + nUpcoming < CONTEST_MAX * 2) {
        int s = body.indexOf("<item>", searchFrom);
        if (s < 0) break;
        int e = body.indexOf("</item>", s);
        if (e < 0) break;
        e += 7;
        String blk = body.substring(s, e);
        searchFrom = e;

        String title = decodeHtmlEntities(xmlGet(blk, "title"));
        String desc  = decodeHtmlEntities(xmlGet(blk, "description"));
        String link  = xmlGet(blk, "link");
        title.trim(); desc.trim(); link.trim();
        if (title.isEmpty() || desc.isEmpty()) continue;

        time_t tStart, tEnd;
        if (!cxParseTimes(desc, tStart, tEnd)) continue;
        if (tEnd <= now) continue;  // already finished

        bool isActive = (now >= tStart && now < tEnd);

        // Fill a Contest entry.
        CxEntry cx;
        strlcpy(cx.c.name,  title.c_str(), sizeof(cx.c.name));
        strlcpy(cx.c.times, desc.c_str(),  sizeof(cx.c.times));
        cx.c.ref[0] = '\0';
        { int ri = link.indexOf("ref=");
          if (ri >= 0) strlcpy(cx.c.ref, link.c_str() + ri + 4, sizeof(cx.c.ref)); }
        cx.c.active = isActive;
        cx.tStart   = tStart;

        if (isActive && nActive < CONTEST_MAX) {
            active[nActive++] = cx;
        } else if (!isActive && nUpcoming < CONTEST_MAX) {
            upcoming[nUpcoming++] = cx;
        }
    }

    // Insertion-sort upcoming by start time (RSS is roughly chronological,
    // but sort to be safe).
    for (int i = 1; i < nUpcoming; i++) {
        CxEntry tmp = upcoming[i];
        int j = i - 1;
        while (j >= 0 && upcoming[j].tStart > tmp.tStart) {
            upcoming[j + 1] = upcoming[j];
            j--;
        }
        upcoming[j + 1] = tmp;
    }

    int total = nActive + nUpcoming;
    if (total == 0) { Serial.println("[cx] no current/upcoming contests"); return; }

    ContestData cd;
    cd.valid = true;
    cd.count = (uint8_t)min(total, CONTEST_MAX);
    for (int i = 0; i < nActive   && i < CONTEST_MAX; i++) cd.items[i]          = active[i].c;
    for (int i = 0; i < nUpcoming && nActive + i < CONTEST_MAX; i++) cd.items[nActive + i] = upcoming[i].c;

    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    g_contests = cd;
    xSemaphoreGive(g_dataMutex);
    Serial.printf("[cx] OK  %d active  %d upcoming\n", nActive, min(nUpcoming, CONTEST_MAX - nActive));
}

static void extractTableField(const String& html, const char* label, char* out, int outLen) {
    out[0] = '\0';
    String pat = String(label) + ":</td><td>";
    int idx = html.indexOf(pat);
    if (idx < 0) { pat = String(label) + ":</td>\n<td>"; idx = html.indexOf(pat); }
    if (idx < 0) return;
    int start = idx + pat.length();
    int end = html.indexOf("</td>", start);
    if (end < 0 || end - start > 256) return;
    String val = html.substring(start, end);
    val.replace("<br>", " / ");
    val.replace("<br/>", " / ");
    val.replace("<br />", " / ");
    val.trim();
    strlcpy(out, val.c_str(), outLen);
}

void fetchContestDetail() {
    int8_t idx = g_contestDetailReq;
    if (idx < 0) return;
    g_contestDetailReq = -1;

    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    Contest c = (idx < g_contests.count) ? g_contests.items[idx] : Contest{};
    xSemaphoreGive(g_dataMutex);

    auto setFailed = [&](const char* reason) {
        Serial.printf("[cx-detail] %s\n", reason);
        xSemaphoreTake(g_dataMutex, portMAX_DELAY);
        g_contestDetail.valid  = false;
        g_contestDetail.failed = true;
        xSemaphoreGive(g_dataMutex);
    };

    if (!c.ref[0]) { setFailed("no ref"); return; }

    char url[96];
    snprintf(url, sizeof(url),
             "https://www.contestcalendar.com/weeklycontdetails.php?ref=%s", c.ref);
    Serial.printf("[cx-detail] fetching %s\n", url);
    String html = httpGet(url, 8192);
    if (html.isEmpty()) { setFailed("fetch failed"); return; }

    ContestDetail d = {};
    strlcpy(d.name, c.name, sizeof(d.name));
    extractTableField(html, "Mode",     d.mode,     sizeof(d.mode));
    extractTableField(html, "Bands",    d.bands,    sizeof(d.bands));
    extractTableField(html, "Exchange", d.exchange, sizeof(d.exchange));
    d.valid = true;

    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    g_contestDetail = d;
    xSemaphoreGive(g_dataMutex);

    Serial.printf("[cx-detail] OK  mode=%s  bands=%s\n", d.mode, d.bands);
}

// ─── Timezone auto-detect ─────────────────────────────────────────────────────
// Reuses Open-Meteo's timezone=auto resolver (the same API used for weather)
// to find the IANA zone name for a lat/lon, then matches it against TZ_LIST.
void fetchTzLookup() {
    if (!g_tzLookupReq) return;
    g_tzLookupReq = false;
    float lat = g_tzLookupLat, lon = g_tzLookupLon;

    auto setResult = [&](bool failed, int idx) {
        TzLookupResult r;
        r.valid  = true;
        r.failed = failed;
        if (!failed) {
            strlcpy(r.tzName,  TZ_LIST[idx].name,  sizeof(r.tzName));
            strlcpy(r.tzPosix, TZ_LIST[idx].posix, sizeof(r.tzPosix));
        }
        xSemaphoreTake(g_dataMutex, portMAX_DELAY);
        g_tzLookup = r;
        xSemaphoreGive(g_dataMutex);
    };

    char url[160];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast"
             "?latitude=%.4f&longitude=%.4f&current=temperature_2m&timezone=auto",
             lat, lon);
    Serial.printf("[tz-lookup] fetching %s\n", url);

    // Open-Meteo always responds with chunked Transfer-Encoding, so this can't
    // use the shared httpGet() helper (its streamReadText() assumes
    // Content-Length framing and would corrupt the body with raw chunk-size
    // headers). http.getString() de-chunks automatically — same approach as
    // fetchWeather() below.
    String body;
    {
        if (!ensureWiFi()) { setResult(true, -1); return; }
        sslCtxRelease();
        {
            WiFiClientSecure sc;
            sslDataRelease();
            sc.setInsecure();
            {
                HTTPClient http;
                http.begin(sc, url);
                http.setTimeout(20000);
                http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
                http.addHeader("User-Agent", "CYD-Dashboard/1.0 ESP32");
                http.addHeader("Connection", "close");
                int code = http.GET();
                if (code == HTTP_CODE_OK) body = http.getString();
                http.end();
            }
        }
        sslAllReclaim();
    }
    if (body.isEmpty()) { Serial.println("[tz-lookup] fetch failed"); setResult(true, -1); return; }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) { Serial.printf("[tz-lookup] JSON: %s\n", err.c_str()); setResult(true, -1); return; }

    const char* iana = doc["timezone"] | "";
    Serial.printf("[tz-lookup] iana=%s\n", iana);
    int idx = tzFindByIana(iana);
    if (idx < 0) { Serial.println("[tz-lookup] no match"); setResult(true, -1); return; }

    Serial.printf("[tz-lookup] matched %s\n", TZ_LIST[idx].name);
    setResult(false, idx);
}

// ─── PSK Reporter ─────────────────────────────────────────────────────────────
// Fetches who received our callsign in the last 15 minutes from pskreporter.info.
// XML is streamed into a String (≤32 KB) then parsed tag-by-tag with strstr().

static bool pskGetAttr(const char* tag, const char* attr, char* out, int outLen) {
    char pat[48];
    snprintf(pat, sizeof(pat), " %s=\"", attr);
    const char* p = strstr(tag, pat);
    if (!p) return false;
    p += strlen(pat);
    int i = 0;
    while (*p && *p != '"' && i < outLen - 1) out[i++] = *p++;
    out[i] = '\0';
    return i > 0;
}

void fetchPSKReporter() {
    if (!g_settings.callsign[0]) return;

    char url[160];
    snprintf(url, sizeof(url), API_PSKREPORTER, g_settings.callsign);

    Serial.printf("[psk] fetching for %s\n", g_settings.callsign);
    String xml = httpGet(url, 32768);
    Serial.printf("[psk] body=%d bytes\n", xml.length());
    if (xml.isEmpty()) {
        Serial.println("[psk] fetch failed or empty");
        xSemaphoreTake(g_dataMutex, portMAX_DELAY);
        g_pskData.fetchFailed = true;
        xSemaphoreGive(g_dataMutex);
        return;
    }
    // Print first 120 chars so the serial log shows whether we got XML or an
    // error/redirect page — helps diagnose server-side 503/redirect issues.
    {
        char preview[121];
        int plen = xml.length() < 120 ? xml.length() : 120;
        strncpy(preview, xml.c_str(), plen);
        preview[plen] = '\0';
        // Replace newlines/CRs with spaces for compact log output
        for (int i = 0; i < plen; i++) if (preview[i] < 0x20) preview[i] = ' ';
        Serial.printf("[psk] start: %s\n", preview);
    }

    PSKData nd = {};

    const char* TAG = "<receptionReport ";
    const char* p = xml.c_str();

    while ((p = strstr(p, TAG)) != nullptr) {
        p += 17; // skip past "<receptionReport "

        // Copy the tag body into a bounded buffer so pskGetAttr() can't
        // stray into the next tag.  Each PSK report tag is ~200–300 bytes.
        // Prepend a space so pskGetAttr's " attr=\"" pattern matches even the
        // very first attribute in the tag.
        char tagBuf[512] = {};
        tagBuf[0] = ' ';
        int  len = 1;
        const char* q = p;
        bool inStr = false; char strC = 0;
        while (*q && len < (int)(sizeof(tagBuf) - 1)) {
            if (inStr) {
                if (*q == strC) inStr = false;
            } else {
                if (*q == '"' || *q == '\'') { inStr = true; strC = *q; }
                else if (*q == '>') break;
            }
            tagBuf[len++] = *q++;
        }
        tagBuf[len] = '\0';

        // Inner reports have receiverLocator; the outer wrapper element does not
        char locator[8] = "";
        if (!pskGetAttr(tagBuf, "receiverLocator", locator, sizeof(locator))) continue;

        nd.total++;

        if (nd.count < PSK_REPORTS_MAX) {
            PSKReport& r = nd.reports[nd.count];
            memset(&r, 0, sizeof(r));

            char freqStr[16] = "";
            char snrStr[8]   = "";

            pskGetAttr(tagBuf, "receiverCallsign", r.call,    sizeof(r.call));
            pskGetAttr(tagBuf, "frequency",        freqStr,   sizeof(freqStr));
            pskGetAttr(tagBuf, "sNR",              snrStr,    sizeof(snrStr));

            strlcpy(r.grid, locator, sizeof(r.grid));
            gridToLatLon(locator, r.lat, r.lon);
            r.freqMHz = (float)atof(freqStr) / 1e6f;
            r.snr     = (int8_t)atoi(snrStr);
            nd.count++;
        }
    }

    // Mark valid as soon as we get any successful HTTP response — even with
    // zero reports.  The screen uses valid=false to mean "never fetched yet"
    // and shows "Fetching…"; valid=true with count=0 just shows a blank map.
    nd.valid = true;
    if (nd.total > 0) {
        Serial.printf("[psk] OK  %d reports total, %d stored\n", nd.total, nd.count);
    } else {
        // Either callsign wasn't heard in the last 15 min, or the XML has a
        // different structure than expected.  The "[psk] start:" line above
        // shows the raw response for diagnosis.
        Serial.printf("[psk] 0 <receptionReport receiverLocator=...> tags found"
                      " in %d-byte body\n", xml.length());
    }

    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    g_pskData = nd;
    xSemaphoreGive(g_dataMutex);
}

// ─── Background fetch task (core 1) ──────────────────────────────────────────
// News and stocks are fetched FIRST so they appear on screen as soon as
// possible.  Weather and solar follow.  Each source tracks its own last-fetch
// time and retries quickly (90 s) if the previous attempt failed.
//
// param: SemaphoreHandle_t fetchReady — blocks here until setup() signals that
//   the SSL reserve blocks are in place (see main.cpp).  This guarantees that
//   g_sslX509R and g_sslCtxR are held before the first TLS connection.
void fetchTask(void* param) {
    // Wait for setup() to free the SSL memory reserves
    if (param) {
        SemaphoreHandle_t ready = reinterpret_cast<SemaphoreHandle_t>(param);
        xSemaphoreTake(ready, portMAX_DELAY);
        vSemaphoreDelete(ready);   // one-shot; release the handle
    }
    Serial.printf("[fetch] started  largestDMA=%u  heap=%u\n",
                  heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                  ESP.getFreeHeap());

    // Thumbnail buffers are pre-allocated in setup() (before the SSL reserves)
    // so they don't land in the C+D coalesced block that mbedTLS needs.
    // These guards are a last-resort fallback only.
    if (!g_bbcThumbBuf)   g_bbcThumbBuf   = (uint8_t*)malloc(THUMB_BUF_LEN);
    if (!g_appleThumbBuf) g_appleThumbBuf = (uint8_t*)malloc(THUMB_BUF_LEN);
    Serial.printf("[fetch] started  bbc=%p apple=%p  largestDMA=%u  heap=%u\n",
                  g_bbcThumbBuf, g_appleThumbBuf,
                  heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                  ESP.getFreeHeap());

    bool weatherOk = false, solarOk    = false;
    bool newsOk    = false, stockOk    = false;
    bool issOk     = false, dxOk       = false;
    bool potaOk    = false, sotaOk     = false;
    bool contestOk = false, pskOk      = false;

    // ── Boot fetch: news + stocks first, then weather + solar + ISS + DX + POTA + SOTA + contests + PSK ──
    fetchBBCNews();   fetchAppleNews();   fetchTracker();
    fetchWeather();   fetchSolar();       fetchISS();
    fetchDXSpots();   fetchPOTASpots();   fetchSOTASpots();   fetchContests();
    fetchPSKReporter();

    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    newsOk    = g_bbcNews.valid;
    stockOk   = g_tracker.valid;
    weatherOk = g_weather.valid;
    solarOk   = g_solar.valid;
    issOk     = g_iss.valid;
    dxOk      = g_dxSpots.valid;
    potaOk    = g_potaSpots.valid;
    sotaOk    = g_sotaSpots.valid;
    contestOk = g_contests.valid;
    pskOk     = g_pskData.valid;
    xSemaphoreGive(g_dataMutex);

    unsigned long lastNews    = millis();
    unsigned long lastStock   = millis();
    unsigned long lastWeather = millis();
    unsigned long lastSolar   = millis();
    unsigned long lastISS     = millis();
    unsigned long lastDX      = millis();
    unsigned long lastPOTA    = millis();
    unsigned long lastSOTA    = millis();
    unsigned long lastContest = millis();
    unsigned long lastPSK     = millis();

    for (;;) {
        unsigned long now = millis();

        // Retry every 90 s after failure; use the normal interval after success
        if (now - lastNews >= (newsOk ? REFRESH_NEWS_MS : 90000UL)) {
            fetchBBCNews(); fetchAppleNews();
            xSemaphoreTake(g_dataMutex, portMAX_DELAY);
            newsOk = g_bbcNews.valid;
            xSemaphoreGive(g_dataMutex);
            lastNews = millis();
        }
        if (g_forceFetchTracker ||
            now - lastStock >= (stockOk ? REFRESH_TRACKER_MS : 90000UL)) {
            fetchTracker();
            xSemaphoreTake(g_dataMutex, portMAX_DELAY);
            stockOk = g_tracker.valid;
            xSemaphoreGive(g_dataMutex);
            lastStock = millis();
            g_forceFetchTracker = false;
        }
        if (g_forceFetchWeather ||
            now - lastWeather >= (weatherOk ? REFRESH_WEATHER_MS : 30000UL)) {
            fetchWeather();
            xSemaphoreTake(g_dataMutex, portMAX_DELAY);
            weatherOk = g_weather.valid;
            xSemaphoreGive(g_dataMutex);
            lastWeather = millis();
            g_forceFetchWeather = false;
        }
        if (now - lastSolar >= (solarOk ? REFRESH_SOLAR_MS : 90000UL)) {
            fetchSolar();
            xSemaphoreTake(g_dataMutex, portMAX_DELAY);
            solarOk = g_solar.valid;
            xSemaphoreGive(g_dataMutex);
            lastSolar = millis();
        }
        if (now - lastISS >= (issOk ? REFRESH_ISS_MS : 30000UL)) {
            fetchISS();
            xSemaphoreTake(g_dataMutex, portMAX_DELAY);
            issOk = g_iss.valid;
            xSemaphoreGive(g_dataMutex);
            lastISS = millis();
        }
        if (now - lastDX >= (dxOk ? REFRESH_DXSPOTS_MS : 30000UL)) {
            fetchDXSpots();
            xSemaphoreTake(g_dataMutex, portMAX_DELAY);
            dxOk = g_dxSpots.valid;
            xSemaphoreGive(g_dataMutex);
            lastDX = millis();
        }
        if (now - lastPOTA >= (potaOk ? REFRESH_POTASPOTS_MS : 30000UL)) {
            fetchPOTASpots();
            xSemaphoreTake(g_dataMutex, portMAX_DELAY);
            potaOk = g_potaSpots.valid;
            xSemaphoreGive(g_dataMutex);
            lastPOTA = millis();
        }
        if (now - lastSOTA >= (sotaOk ? REFRESH_SOTASPOTS_MS : 30000UL)) {
            fetchSOTASpots();
            xSemaphoreTake(g_dataMutex, portMAX_DELAY);
            sotaOk = g_sotaSpots.valid;
            xSemaphoreGive(g_dataMutex);
            lastSOTA = millis();
        }
        if (now - lastContest >= (contestOk ? REFRESH_CONTESTS_MS : 90000UL)) {
            fetchContests();
            xSemaphoreTake(g_dataMutex, portMAX_DELAY);
            contestOk = g_contests.valid;
            xSemaphoreGive(g_dataMutex);
            lastContest = millis();
        }
        if (now - lastPSK >= (pskOk ? REFRESH_PSK_MS : 60000UL)) {
            fetchPSKReporter();
            xSemaphoreTake(g_dataMutex, portMAX_DELAY);
            pskOk = g_pskData.valid;
            xSemaphoreGive(g_dataMutex);
            lastPSK = millis();
        }
        if (g_contestDetailReq >= 0) {
            fetchContestDetail();
        }
        if (g_tzLookupReq) {
            fetchTzLookup();
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
