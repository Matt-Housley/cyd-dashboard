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
#include <lwip/sockets.h>   // lwip_setsockopt / SO_LINGER

// ─── Debug timestamp helper ───────────────────────────────────────────────────
/** @brief Print an "[MM:SS.mmm] " elapsed-since-boot prefix on the serial log. */
static void logTs() {
    uint32_t ms = millis();
    uint32_t s  = ms / 1000; ms %= 1000;
    uint32_t m  = s  / 60;   s  %= 60;
    Serial.printf("[%02u:%02u.%03u] ", m, s, ms);
}
#define LOG(...) do { logTs(); Serial.printf(__VA_ARGS__); } while(0)

// Diagnostic: skip contestcalendar.com.  Its TLS handshake aborts on the
// hardware-AES misalignment (PADLOCK) and leaves ~30 KB of mbedTLS allocations
// behind, collapsing lb8bit from 34804 to ~4084 permanently — after which every
// HTTPS fetch fails until reboot.  Set to 0 once mbedTLS is rebuilt with
// hardware AES disabled for real.
#define DIAG_NO_CONTESTS 0

// Largest SINGLE contiguous block mbedTLS needs for a handshake.
//
// Was 33434 (= in_buf 16717 + out_buf 16717), because the stock Arduino build
// shipped a prebuilt mbedTLS with symmetric 16384-byte content buffers and no
// way to change them.  sdkconfig.defaults now sets
// CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN with OUT=4096, so the two buffers are
// 16717 and 4373 and are allocated separately — only the larger one has to be
// contiguous.  Keeping the old 33434 here made this check reject connections
// that would have succeeded (observed: "lb8bit=27648 < 33434 — skipping TLS").
//
// Keep in step with CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN in sdkconfig.defaults.
#define TLS_MIN_CONTIG 16717U

// Consecutive HTTPS attempts skipped for want of a contiguous TLS_MIN_CONTIG
// block before the device reboots to defragment the heap.  Long-uptime heap
// fragmentation can drop the largest free block below TLS_MIN_CONTIG even with
// plenty of free heap; once wedged it never recovers on its own (HTTP sources
// keep working, every HTTPS source fails "SSL - Memory allocation failed").
// The boot-time SSL reserve guarantees a contiguous block again, so a reboot
// restores all sources.  The streak resets on any attempt that finds enough
// contiguous heap, so only sustained wedging triggers a reboot.
#define TLS_STARVE_REBOOT 12

// Consecutive cycles the ctx reserve may fail to reclaim before rebooting.
// Losing ctx dooms all HTTPS, but a single miss is usually a transient
// fragmentation spike that clears within a cycle (the reserve is re-attempted
// every fetch, and the pre-flight guard skips HTTPS meanwhile so nothing makes
// it worse).  Only sustained failure — a genuinely wedged heap — reboots.
#define SSL_CTX_MISS_REBOOT 4

// Fine-grained heap probe — prints free heap and largest 8-bit block at a
// labelled point.  Set DIAG_HEAP_PROBES to 1 to localise where heap is lost
// across a fetch; the probes proved that the drop across sslAllReclaim() is the
// reserves being re-taken (by design), not a leak on the TLS failure path.
#define DIAG_HEAP_PROBES 0

#if DIAG_HEAP_PROBES
#define HEAPPROBE(label) LOG("  [probe] %-22s heap=%u lb8bit=%u\n", (label), \
    ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT))
#else
#define HEAPPROBE(label) do {} while(0)
#endif

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
// g_sslX509R (SSL_X509_RESERVE, 18432 B):
//   Released just before http.GET() so the mbedTLS input record buffer (16717 B)
//   and the cert-chain parser have a clean, contiguous region. Sized on its own
//   to exceed TLS_MIN_CONTIG with headroom, so it no longer depends on a
//   fragmented neighbour coalescing (long uptime erodes that neighbour). Any
//   adjacent free block still merges in for extra margin. Reclaimed AFTER the
//   inner scope closes (http + sc both destroyed) so the slot is empty first.
//
// g_sslCtxR (6136 B):
//   Released just before each WiFiClientSecure constructor.  The constructor's
//   very first heap allocation is  new sslclient_context() (6136 B); first-fit
//   picks the freshly-freed slot.  Held as a global so nothing (WiFi stack,
//   lwIP, etc.) can nibble it between setup() and the first fetch.
//   Reclaimed after the WiFiClientSecure destructor runs.

/** @brief Free the 12 KB x509 reserve so the TLS cert parser can use its slot. */
static void sslX509Release()  { if (g_sslX509R)  { heap_caps_free(g_sslX509R);  g_sslX509R  = nullptr; } }
/** @brief Re-take the 12 KB x509 reserve after TLS work completes. */
static void sslX509Reclaim()  { if (!g_sslX509R)   g_sslX509R  = heap_caps_malloc(SSL_X509_RESERVE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
/** @brief Free the 6 KB SSL-context reserve so the TLS client ctor can use its slot. */
static void sslCtxRelease()   { if (g_sslCtxR)   { heap_caps_free(g_sslCtxR);   g_sslCtxR   = nullptr; } }
/** @brief Re-take the 6 KB SSL-context reserve after the TLS client is destroyed. */
static void sslCtxReclaim()   { if (!g_sslCtxR)    g_sslCtxR   = heap_caps_malloc(SSL_CTX_RESERVE,  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }

/** @brief Release the x509 reserve just before http.GET(), logging the resulting
 *         largest-block/heap; coalesces to ~38.9 KB for the mbedTLS buffers. */
static void sslDataRelease() {
    sslX509Release();
    LOG("[ssl] data-released  lb8bit=%u  heap=%u\n",
                  heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  ESP.getFreeHeap());
}
/** @brief Re-take both SSL reserves after a TLS connection is fully torn down.
 *
 * Reclaims the CRITICAL ctx reserve FIRST: losing it dooms all future HTTPS,
 * whereas losing x509 only costs coalesce headroom (and is retried every cycle),
 * so giving ctx first dibs on the freed memory means a fragmentation spike
 * sacrifices the harmless reserve, not the fatal one.  A single ctx miss is
 * ridden out — it is re-attempted next fetch and the pre-flight guard skips
 * HTTPS meanwhile — and only sustained failure across SSL_CTX_MISS_REBOOT cycles
 * (a genuinely wedged heap) reboots to defragment. */
static void sslAllReclaim() {
    sslCtxReclaim();     // critical reserve first
    sslX509Reclaim();    // harmless if this one misses; retried every cycle
    bool ctxOk = (g_sslCtxR != nullptr);
    LOG("[ssl] reclaimed  ctx=%s x509=%s  lb8bit=%u\n",
                  ctxOk      ? "OK" : "FAIL",
                  g_sslX509R ? "OK" : "FAIL",
                  heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    static int s_ctxMissStreak = 0;
    if (ctxOk) {
        s_ctxMissStreak = 0;
    } else if (++s_ctxMissStreak >= SSL_CTX_MISS_REBOOT) {
        LOG("[ssl] ctx slot unrecoverable x%d — rebooting to defragment\n",
            s_ctxMissStreak);
        delay(2000);
        ESP.restart();
    } else {
        LOG("[ssl] ctx reclaim missed (streak %d/%d) — retrying next cycle\n",
            s_ctxMissStreak, SSL_CTX_MISS_REBOOT);
    }
}

// ─── Stream-read helpers ──────────────────────────────────────────────────────


/**
 * @brief Read up to @p maxLen bytes from an HTTP response stream, then drain/close.
 * @param http       Active HTTPClient with a live response.
 * @param buf        Destination buffer.
 * @param maxLen     Maximum bytes to read into @p buf.
 * @param timeoutMs  Overall read timeout in milliseconds.
 * @return Number of bytes actually read (may be less than @p maxLen).
 */
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
    // Drain as per streamReadText — same orphan-buffer risk for byte streams.
    {
        uint32_t drainT0 = millis();
        size_t   drained = 0;
        while ((millis() - drainT0) < 2000 && drained < 65536) {
            if (stream->available() > 0) { stream->read(); drained++; }
            else if (!stream->connected()) { break; }
            else { delay(5); }
        }
    }
    http.end();
    return got;
}

// ─── WiFi guard ───────────────────────────────────────────────────────────────
/**
 * @brief Ensure WiFi is connected, attempting a reconnect if it has dropped.
 * @return true if connected (already or after reconnect), false if reconnect
 *         failed within the retry window.
 */
static bool ensureWiFi() {
    if (WiFi.status() == WL_CONNECTED) return true;
    Serial.println("[fetch] WiFi lost — reconnecting");
    WiFi.reconnect();
    for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) delay(500);
    bool ok = (WiFi.status() == WL_CONNECTED);
    LOG("[fetch] WiFi %s\n", ok ? "reconnected" : "reconnect FAILED");
    return ok;
}

// ─── Streaming response consumption ───────────────────────────────────────────
// httpGet()+streamReadText() force the whole response into one contiguous heap
// block.  Arduino's String grows by exactly the requested size on every concat
// (WString.cpp: newSize = (maxStrLen + 16) & ~0xf — no geometric growth), so a
// 32 KB body costs ~56 reallocs, and the last one needs the old and new buffers
// live at the same time.  That happens while mbedTLS in_buf + out_buf (33434 B)
// are also allocated; this board does not have ~65 KB spare.  Worse, concat()'s
// failure is silent — streamReadText ignored the return value, so an OOM showed
// up later as a truncated body (the [sota] IncompleteInput).
//
// These helpers consume the response as it arrives, so no full-body buffer ever
// exists and memory use is independent of response size.

/**
 * @brief Drain any unread response bytes, then close the connection.
 *
 * Reading to EOF lets the server send FIN first so lwIP does not park the
 * remainder in a permanent receive buffer.
 * @param http  The HTTPClient to drain and end.
 */
static void drainAndEnd(HTTPClient& http) {
    WiFiClient* s = http.getStreamPtr();
    if (s) {
        uint32_t t0 = millis();
        size_t   drained = 0;
        while ((millis() - t0) < 2000 && drained < 65536) {
            if (s->available() > 0) { s->read(); drained++; }
            else if (!s->connected()) break;
            else delay(5);
        }
    }
    http.end();
}

// Consumer callback: reads from the live stream, returns true on success.
typedef bool (*StreamConsumer)(HTTPClient& http, void* ctx);

/**
 * @brief GET a URL and, on HTTP 200, hand the live stream to a consumer callback.
 *
 * Performs the SSL reserve release/reclaim dance for HTTPS, retries transient
 * failures, and treats allocation/PADLOCK/BIGNUM SSL errors as fatal (no retry).
 * Replaces a buffer-the-whole-body approach for sources that parse incrementally.
 * @param url        Target URL (http:// or https://).
 * @param consume    Callback invoked with the live stream; returns true on success.
 * @param ctx        Opaque context pointer passed through to @p consume.
 * @param who        Short label for log lines.
 * @param maxTries   Maximum attempts. Defaults to 3.
 * @param userAgent  Overrides the default UA where a server demands it (e.g.
 *                   Yahoo Finance). Defaults to nullptr (use the built-in UA).
 * @return true if @p consume reported success on an HTTP 200 response, else false.
 */
static bool httpStream(const char* url, StreamConsumer consume, void* ctx,
                       const char* who, int maxTries = 3,
                       const char* userAgent = nullptr) {
    const char* UA = userAgent ? userAgent : "CYD-Dashboard/1.0 ESP32";
    // Consecutive HTTPS attempts skipped for lack of contiguous heap; a sustained
    // streak triggers a defragmenting reboot (see the pre-flight guard below).
    static int s_tlsStarveStreak = 0;
    for (int attempt = 1; attempt <= maxTries; attempt++) {
        if (!ensureWiFi()) { if (attempt < maxTries) delay(2000); continue; }

        bool isHttps = (strncmp(url, "https://", 8) == 0);
        LOG("[%s] heap=%u lb8bit=%u  attempt %d/%d\n", who,
            ESP.getFreeHeap(),
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            attempt, maxTries);

        bool ok = false, fatal = false;

        if (isHttps) {
            sslCtxRelease();
            {
                WiFiClientSecure sc;
                sslDataRelease();
                sc.setHandshakeTimeout(30);
                sc.setTimeout(15000);
                sc.setInsecure();

                // Pre-flight contiguous-heap guard.  mbedTLS needs one contiguous
                // TLS_MIN_CONTIG block for its inbound record buffer; on a
                // fragmented long-uptime heap the largest block can fall below that
                // even with lots of free heap, and attempting the handshake then
                // only churns failed partial allocations (worsening fragmentation).
                // Skip it, and reboot once the device is confidently wedged.
                uint32_t lb8 = heap_caps_get_largest_free_block(
                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                if (lb8 < TLS_MIN_CONTIG) {
                    if (++s_tlsStarveStreak >= TLS_STARVE_REBOOT) {
                        LOG("[ssl] HTTPS heap-starved x%d (lb8bit=%u < %u) — "
                            "rebooting to defragment\n",
                            s_tlsStarveStreak, lb8, (unsigned)TLS_MIN_CONTIG);
                        delay(1000);
                        ESP.restart();
                    }
                    LOG("[%s] lb8bit=%u < %u — skipping TLS (fragmented, streak %d)\n",
                        who, lb8, (unsigned)TLS_MIN_CONTIG, s_tlsStarveStreak);
                    fatal = true;   // retrying won't defragment the heap this call
                } else {
                    s_tlsStarveStreak = 0;   // enough contiguous heap this attempt
                    HTTPClient http;
                    http.begin(sc, url);
                    http.setTimeout(20000);
                    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
                    http.addHeader("User-Agent", UA);
                    http.addHeader("Accept", "*/*");
                    http.addHeader("Connection", "close");
                    int code = http.GET();
                    // Force RST on close so the server stops transmitting at once.
                    {
                        int fd = sc.fd();
                        if (fd >= 0) {
                            struct linger sl = {1, 0};
                            lwip_setsockopt(fd, SOL_SOCKET, SO_LINGER, &sl, sizeof(sl));
                        }
                    }
                    if (code != HTTP_CODE_OK) {
                        if (code < 0) {
                            // Connection-level failure (handshake/socket) — the SSL
                            // error is meaningful. Memory/BIGNUM/PADLOCK errors get
                            // worse with retries, so mark them fatal.
                            char sslErr[64] = "";
                            sc.lastError(sslErr, sizeof(sslErr));
                            LOG("[%s] connect failed %d  ssl='%s'  lb8bit=%u\n",
                                who, code, sslErr,
                                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
                            if (strstr(sslErr, "BIGNUM")     || strstr(sslErr, "Allocation") ||
                                strstr(sslErr, "allocation") || strstr(sslErr, "PADLOCK")) fatal = true;
                        } else {
                            // A real HTTP status from the server (e.g. 503, 404) —
                            // the TLS layer worked, so don't print a stale/misleading
                            // SSL error string.
                            LOG("[%s] HTTP %d  lb8bit=%u\n", who, code,
                                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
                        }
                        http.end();
                    } else {
                        ok = consume(http, ctx);
                        drainAndEnd(http);
                    }
                }                               // http / guard done
            }                                   // sc destroyed
            sslAllReclaim();
        } else {
            HTTPClient http;
            http.begin(url);
            http.setTimeout(20000);
            http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
            http.addHeader("User-Agent", UA);
            http.addHeader("Accept", "*/*");
            http.addHeader("Connection", "close");
            int code = http.GET();
            if (code != HTTP_CODE_OK) { LOG("[%s] HTTP %d\n", who, code); http.end(); }
            else { ok = consume(http, ctx); drainAndEnd(http); }
        }

        if (ok)    return true;
        if (fatal) break;
        if (attempt < maxTries) delay(2000);
    }
    return false;
}

// Sliding-window XML element scanner.  Finds each occurrence of `open` (e.g.
// "<receptionReport ") in the stream and passes that element's attribute text
// to onTag().  Memory use is fixed at tagCap bytes regardless of body size, so
// a 200 KB response costs the same as a 2 KB one.
typedef void (*TagHandler)(const char* tagBody, void* ctx);

/**
 * @brief Sliding-window scanner that delivers each XML element's attribute text.
 *
 * Finds every occurrence of @p open (e.g. "<receptionReport ") in the stream and
 * passes that element's attributes to @p onTag. Memory use is fixed at @p tagCap
 * regardless of body size.
 * @param http       Active HTTPClient with a live response.
 * @param open       Opening-tag prefix to match.
 * @param onTag      Callback invoked with each matched element's attribute text.
 * @param ctx        Opaque context pointer passed to @p onTag.
 * @param tagBuf     Scratch buffer for the current tag's attributes.
 * @param tagCap     Size of @p tagBuf in bytes.
 * @param timeoutMs  Overall scan timeout. Defaults to 20000.
 * @return Number of matched elements delivered.
 */
static int scanTags(HTTPClient& http, const char* open, TagHandler onTag,
                    void* ctx, char* tagBuf, size_t tagCap,
                    uint32_t timeoutMs = 20000) {
    WiFiClient* s = http.getStreamPtr();
    if (!s) return 0;

    const size_t openLen = strlen(open);
    size_t   m     = 0;          // chars of `open` matched so far
    bool     inTag = false;
    bool     inStr = false; char strC = 0;
    size_t   n     = 0;          // bytes held in tagBuf
    int      found = 0;
    char     chunk[256];
    uint32_t t0    = millis();

    while ((millis() - t0) < timeoutMs) {
        int avail = s->available();
        if (avail <= 0) {
            if (!s->connected()) break;
            delay(5);
            continue;
        }
        int got = s->readBytes(chunk, min((size_t)avail, sizeof(chunk)));
        for (int i = 0; i < got; i++) {
            char c = chunk[i];
            if (!inTag) {
                if (c == open[m]) {
                    if (++m == openLen) {
                        // Lead with a space so attr lookups matching " name=\""
                        // find the very first attribute too.
                        inTag = true; inStr = false; m = 0;
                        n = 0; tagBuf[n++] = ' ';
                    }
                } else {
                    m = (c == open[0]) ? 1 : 0;
                }
            } else {
                if (inStr) {
                    if (c == strC) inStr = false;
                } else if (c == '"' || c == '\'') {
                    inStr = true; strC = c;
                } else if (c == '>') {
                    tagBuf[n] = '\0';
                    onTag(tagBuf, ctx);
                    found++;
                    inTag = false;
                    continue;
                }
                if (n < tagCap - 1) tagBuf[n++] = c;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return found;
}

// Sliding-window element scanner.  Where scanTags() hands back a tag's
// ATTRIBUTES, this hands back the whole element INCLUDING its inner text —
// "<spot>...</spot>", "<item>...</item>" — so the existing xmlGet()-style
// parsers can run against one small bounded block instead of a whole-body
// String.  Memory use is fixed at blockCap regardless of response size.
//
// onBlock receives a NUL-terminated copy of the element, opening tag included.
// Returns the number of complete blocks seen (blocks longer than blockCap are
// truncated, still delivered, and still counted).
typedef void (*BlockHandler)(const char* block, void* ctx);

/**
 * @brief Sliding-window scanner that delivers each whole XML element (with text).
 *
 * Where scanTags() yields attributes, this yields the full element including
 * inner text ("<spot>...</spot>"), so xmlGet()-style parsers can run on one
 * small bounded block. Memory use is fixed at @p blockCap regardless of size.
 * @param http       Active HTTPClient with a live response.
 * @param open       Opening tag to match (replayed into the buffer).
 * @param close      Closing tag that ends each block.
 * @param onBlock    Callback given a NUL-terminated copy of each element.
 * @param ctx        Opaque context pointer passed to @p onBlock.
 * @param buf        Scratch buffer for the current block.
 * @param blockCap   Size of @p buf; over-long blocks are truncated but delivered.
 * @param timeoutMs  Overall scan timeout. Defaults to 20000.
 * @return Number of complete blocks seen.
 */
static int scanBlocks(HTTPClient& http, const char* open, const char* close,
                      BlockHandler onBlock, void* ctx,
                      char* buf, size_t blockCap, uint32_t timeoutMs = 20000) {
    WiFiClient* s = http.getStreamPtr();
    if (!s) return 0;

    const size_t openLen  = strlen(open);
    const size_t closeLen = strlen(close);
    size_t   mOpen = 0, mClose = 0;
    bool     inBlock = false;
    size_t   n = 0;
    int      found = 0;
    char     chunk[256];
    uint32_t t0 = millis();

    while ((millis() - t0) < timeoutMs) {
        int avail = s->available();
        if (avail <= 0) {
            if (!s->connected()) break;
            delay(5);
            continue;
        }
        int got = s->readBytes(chunk, min((size_t)avail, sizeof(chunk)));
        for (int i = 0; i < got; i++) {
            char c = chunk[i];

            if (!inBlock) {
                if (c == open[mOpen]) {
                    if (++mOpen == openLen) {
                        inBlock = true; mClose = 0;
                        n = 0;
                        // Replay the opening tag so xmlGet() sees a whole element
                        for (size_t k = 0; k < openLen && n < blockCap - 1; k++)
                            buf[n++] = open[k];
                        mOpen = 0;
                    }
                } else {
                    mOpen = (c == open[0]) ? 1 : 0;
                }
                continue;
            }

            if (n < blockCap - 1) buf[n++] = c;

            if (c == close[mClose]) {
                if (++mClose == closeLen) {
                    buf[n] = '\0';
                    onBlock(buf, ctx);
                    found++;
                    inBlock = false; mOpen = 0; mClose = 0;
                }
            } else {
                mClose = (c == close[0]) ? 1 : 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return found;
}

// Bounded whole-body read, for responses small enough that a fixed buffer is
// clearer than a streaming parser (ISS ~200 B, solar ~3 KB, contest detail).
// Unlike the old streamReadText() this writes into caller-owned storage, so
// there is no incremental String growth and no silent truncation on OOM.
struct TextSink { char* buf; size_t cap; size_t len; };

/**
 * @brief StreamConsumer that reads a whole (small) body into a TextSink buffer.
 * @param http  Active HTTPClient with a live response.
 * @param ctx   Pointer to a TextSink describing the destination buffer.
 * @return true if at least one byte was read, false otherwise.
 */
static bool textConsume(HTTPClient& http, void* ctx) {
    TextSink& t = *(TextSink*)ctx;
    WiFiClient* s = http.getStreamPtr();
    if (!s) return false;
    uint32_t t0 = millis();
    while (t.len < t.cap - 1 && (millis() - t0) < 20000) {
        int avail = s->available();
        if (avail > 0) {
            int n = s->readBytes(t.buf + t.len,
                                 min((size_t)avail, t.cap - 1 - t.len));
            if (n > 0) t.len += n;
            vTaskDelay(pdMS_TO_TICKS(1));
        } else if (!s->connected()) {
            break;
        } else {
            delay(5);
        }
    }
    t.buf[t.len] = '\0';
    return t.len > 0;
}

/**
 * @brief Fetch a whole (small) response body into a caller-owned buffer.
 * @param url       Target URL.
 * @param buf       Destination buffer (NUL-terminated on return).
 * @param cap       Size of @p buf in bytes.
 * @param who       Short label for log lines.
 * @param maxTries  Maximum attempts. Defaults to 3.
 * @return Bytes read, or 0 on failure.
 */
static size_t httpFetchText(const char* url, char* buf, size_t cap,
                            const char* who, int maxTries = 3) {
    TextSink t = { buf, cap, 0 };
    buf[0] = '\0';
    if (!httpStream(url, textConsume, &t, who, maxTries)) return 0;
    return t.len;
}

// As httpFetchText, but the destination buffer is malloc'd INSIDE the stream
// consumer — i.e. only after the TLS handshake has already succeeded.  That
// keeps the buffer out of the heap during the handshake, so the RSA/BIGNUM
// certificate verify has room for its scratch.  Used for the memory-hungriest
// source (solar), which otherwise failed the handshake ("BIGNUM - Memory
// allocation failed") whenever it landed at a low-heap, fragmented moment.
struct TextSinkAlloc { char* buf; size_t cap; size_t len; };

/**
 * @brief StreamConsumer that lazily allocates its buffer, then reads the body.
 * @param http  Active HTTPClient with a live response (handshake already done).
 * @param ctx   Pointer to a TextSinkAlloc (buf starts null; cap is the size).
 * @return true if at least one byte was read, false otherwise.
 */
static bool textConsumeAlloc(HTTPClient& http, void* ctx) {
    TextSinkAlloc& t = *(TextSinkAlloc*)ctx;
    if (!t.buf) {                      // allocate now — the handshake is done
        t.buf = (char*)malloc(t.cap);
        if (!t.buf) return false;
    }
    t.len = 0;                         // reset (a retry may reuse the buffer)
    WiFiClient* s = http.getStreamPtr();
    if (!s) return false;
    uint32_t t0 = millis();
    while (t.len < t.cap - 1 && (millis() - t0) < 20000) {
        int avail = s->available();
        if (avail > 0) {
            int n = s->readBytes(t.buf + t.len,
                                 min((size_t)avail, t.cap - 1 - t.len));
            if (n > 0) t.len += n;
            vTaskDelay(pdMS_TO_TICKS(1));
        } else if (!s->connected()) {
            break;
        } else {
            delay(5);
        }
    }
    t.buf[t.len] = '\0';
    return t.len > 0;
}

/**
 * @brief Fetch a whole (small) body into a heap buffer allocated post-handshake.
 *
 * The buffer is malloc'd inside the consumer (after TLS is up), so it doesn't
 * compete with the handshake's RSA/BIGNUM scratch for contiguous heap.
 * @param url          Target URL.
 * @param cap          Buffer size to allocate.
 * @param who          Short label for log lines.
 * @param[out] outLen  Receives the bytes read (0 on failure).
 * @param maxTries     Maximum attempts. Defaults to 3.
 * @return The allocated NUL-terminated buffer (caller must free), or nullptr on
 *         failure.
 */
static char* httpFetchTextAlloc(const char* url, size_t cap, const char* who,
                                size_t& outLen, int maxTries = 3) {
    TextSinkAlloc t = { nullptr, cap, 0 };
    outLen = 0;
    if (!httpStream(url, textConsumeAlloc, &t, who, maxTries)) {
        if (t.buf) free(t.buf);
        return nullptr;
    }
    outLen = t.len;
    return t.buf;
}


// ─── HTTP GET (binary) ────────────────────────────────────────────────────────
/**
 * @brief GET a binary resource (e.g. a JPEG thumbnail) into a fixed buffer.
 * @param url        Target URL (http:// or https://).
 * @param buf        Destination buffer.
 * @param maxLen     Maximum bytes to read.
 * @param maxTries   Maximum attempts. Defaults to 3.
 * @param timeoutMs  Per-attempt read timeout. Defaults to 15000.
 * @return Bytes read, or 0 on failure.
 */
static size_t httpGetBytes(const char* url, uint8_t* buf, size_t maxLen,
                           int maxTries = 3, unsigned long timeoutMs = 15000) {
    for (int attempt = 1; attempt <= maxTries; attempt++) {
        if (!ensureWiFi()) {
            if (attempt < maxTries) delay(2000);
            continue;
        }
        bool isHttps = (strncmp(url, "https://", 8) == 0);
        LOG("[fetch-bytes] heap=%u lb8bit=%u  attempt %d/%d  %s\n",
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
                        LOG("[fetch-bytes] %s => HTTP %d\n", url, code);
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
                LOG("[fetch-bytes] %s => HTTP %d\n", url, code);
                http.end();
            } else {
                got = streamReadBytes(http, buf, maxLen, timeoutMs);
            }
        }

        if (got == 0) {
            if (attempt < maxTries) delay(2000);
            continue;
        }
        LOG("[fetch-bytes] got %u bytes\n", (unsigned)got);
        return got;
    }
    return 0;
}

// ─── XML helpers ─────────────────────────────────────────────────────────────
/**
 * @brief Extract the inner text of the first <tag>...</tag> element.
 * @param xml  XML/RSS text to search.
 * @param tag  Element name (without angle brackets).
 * @return The element's inner text, or "" if the tag is not found.
 */
static String xmlGet(const String& xml, const char* tag) {
    String open  = String('<') + tag + '>';
    String close = String("</") + tag + '>';
    int s = xml.indexOf(open);
    if (s < 0) return "";
    s += open.length();
    int e = xml.indexOf(close, s);
    return (e < 0) ? "" : xml.substring(s, e);
}

/**
 * @brief Extract a double-quoted attribute value from an opening tag string.
 * @param tag   The tag text to search (e.g. `<enclosure url="..." />`).
 * @param attr  Attribute name (without the `="`).
 * @return The attribute value, or "" if the attribute is not present.
 */
static String xmlAttr(const String& tag, const char* attr) {
    String needle = String(attr) + "=\"";
    int s = tag.indexOf(needle);
    if (s < 0) return "";
    s += needle.length();
    int e = tag.indexOf('"', s);
    return (e < 0) ? "" : tag.substring(s, e);
}

/**
 * @brief Extract a <band> element's value matching both name= and time= attrs.
 * @param xml   Solar-conditions XML.
 * @param band  Exact name= attribute value (e.g. "80m-40m").
 * @param time  Exact time= attribute value (e.g. "day" or "night").
 * @return The matching band's condition text, or "" if not found.
 */
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

// Codepoints the extended DejaVu fonts (src/fonts/dejavu_ext.cpp) contain: ASCII,
// Latin-1, the General-Punctuation block (dashes, curly quotes, bullet, ellipsis)
// and the Euro sign. These render as real glyphs; anything else must be folded.
/**
 * @brief Whether the extended DejaVu fonts contain a glyph for a codepoint.
 * @param cp  Unicode codepoint.
 * @return true for ASCII, Latin-1, the covered General-Punctuation range, and
 *         the Euro sign; false for everything else (which must be folded).
 */
static bool fontHasGlyph(uint32_t cp) {
    return (cp >= 0x20   && cp <= 0x7E)
        || (cp >= 0xA0   && cp <= 0xFF)
        || (cp >= 0x2013 && cp <= 0x2026)
        || (cp == 0x20AC);
}

// ─── Unicode → ASCII fold ─────────────────────────────────────────────────────
// For codepoints the fonts DON'T contain, map to a printable-ASCII equivalent
// (which may be several chars, or "" to drop it). Covered codepoints are handled
// by appendCodepoint() and never reach here.
/**
 * @brief Fold a codepoint the fonts lack down to a printable-ASCII equivalent.
 *
 * Covered codepoints are handled by appendCodepoint() and never reach here.
 * @param cp  Unicode codepoint.
 * @return A static ASCII replacement (may be several chars, "" to drop it, or
 *         "?" for an unmapped codepoint).
 */
static const char* foldCodepoint(uint32_t cp) {
    if (cp <= 0x1F)                return "";  // C0 controls
    if (cp >= 0x7F && cp <= 0x9F)  return "";  // C1 controls
    switch (cp) {
        case 0x2007: case 0x2009: case 0x200A: case 0x202F: return " ";  // spaces
        case 0x200B: case 0x200C: case 0x200D: case 0xFEFF: return "";   // zero-width
        case 0x2010: case 0x2011: case 0x2012: case 0x2212: return "-";  // hyphens / minus
        case 0x2032: case 0x02BC: return "'";                 // primes
        case 0x2033: return "\"";
        case 0x2027: case 0x2219: return "*";                 // other bullets
        case 0x2122: return "(TM)";                           // ™
        case 0x0152: return "OE"; case 0x0153: return "oe";
        case 0x0160: return "S";  case 0x0161: return "s";
        case 0x017D: return "Z";  case 0x017E: return "z";
        case 0x0178: return "Y";
    }
    return "?";
}

/**
 * @brief Append a codepoint to a String as UTF-8 (up to 3 bytes / whole BMP).
 * @param out  Destination String, appended in place.
 * @param cp   Unicode codepoint to encode.
 */
static void appendUtf8(String& out, uint32_t cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

/**
 * @brief Append a codepoint as bytes the display can render.
 *
 * Glyphs the font has pass through as UTF-8 (LovyanGFX maps them back);
 * everything else is folded to an ASCII approximation via foldCodepoint().
 * @param out  Destination String, appended in place.
 * @param cp   Unicode codepoint.
 */
static void appendCodepoint(String& out, uint32_t cp) {
    if (cp >= 0x20 && cp <= 0x7E) { out += (char)cp; return; }
    if (fontHasGlyph(cp)) { appendUtf8(out, cp); return; }
    out += foldCodepoint(cp);
}

// ─── HTML entity + UTF-8 normaliser ───────────────────────────────────────────
// Rewrites named (&amp;) and numeric (&#36; / &#x24;) HTML entities to their
// characters, and folds any codepoint the fonts can't render down to ASCII.
// ASCII and Latin-1 (incl. £ and accented letters) survive; higher Unicode
// (curly quotes, dashes, ellipsis, emoji) becomes an ASCII approximation or '?'.
/**
 * @brief Decode HTML entities and normalise text for the display fonts.
 *
 * Rewrites named (&amp;) and numeric (&#36; / &#x24;) entities to their
 * characters and folds any codepoint the fonts can't render to ASCII. ASCII and
 * Latin-1 (incl. £ and accents) survive; higher Unicode becomes an ASCII
 * approximation or '?'.
 * @param s  Raw text (UTF-8 with possible HTML entities).
 * @return The decoded, font-safe text.
 */
static String decodeHtmlEntities(const String& s) {
    String out;
    out.reserve(s.length());
    int i = 0, n = (int)s.length();
    while (i < n) {
        uint8_t b = (uint8_t)s[i];

        // Raw UTF-8: any byte with the high bit set starts a multi-byte sequence
        if (b >= 0x80) {
            uint32_t cp; int len;
            if      ((b & 0xE0) == 0xC0) { cp = b & 0x1F; len = 2; }
            else if ((b & 0xF0) == 0xE0) { cp = b & 0x0F; len = 3; }
            else if ((b & 0xF8) == 0xF0) { cp = b & 0x07; len = 4; }
            else { i++; continue; }                    // stray continuation byte
            if (i + len > n) { i++; continue; }         // truncated sequence
            bool ok = true;
            for (int k = 1; k < len; k++) {
                uint8_t bc = (uint8_t)s[i + k];
                if ((bc & 0xC0) != 0x80) { ok = false; break; }
                cp = (cp << 6) | (bc & 0x3F);
            }
            if (!ok) { i++; continue; }
            appendCodepoint(out, cp);
            i += len;
            continue;
        }

        if (b != '&') { out += (char)b; i++; continue; }

        // HTML entity
        int semi = s.indexOf(';', i + 1);
        if (semi < 0 || semi - i > 12) { out += s[i++]; continue; }
        String ent = s.substring(i + 1, semi);
        bool handled = true;
        if      (ent.equalsIgnoreCase("amp"))   out += '&';
        else if (ent.equalsIgnoreCase("lt"))    out += '<';
        else if (ent.equalsIgnoreCase("gt"))    out += '>';
        else if (ent.equalsIgnoreCase("quot"))  out += '"';
        else if (ent.equalsIgnoreCase("apos"))  out += '\'';
        else if (ent.equalsIgnoreCase("nbsp"))  appendCodepoint(out, 0x00A0);
        else if (ent.equalsIgnoreCase("pound")) appendCodepoint(out, 0x00A3);
        else if (ent.startsWith("#x") || ent.startsWith("#X"))
            appendCodepoint(out, (uint32_t)strtol(ent.c_str() + 2, nullptr, 16));
        else if (ent.startsWith("#"))
            appendCodepoint(out, (uint32_t)strtol(ent.c_str() + 1, nullptr, 10));
        else handled = false;

        if (handled) i = semi + 1;
        else         out += s[i++];   // unknown entity — keep the '&'
    }
    return out;
}

// ─── Day abbreviation ─────────────────────────────────────────────────────────
/** @brief Map a weekday index to its 3-letter abbreviation.
 *  @param wday  Day of week, 0=Sunday (taken modulo 7).
 *  @return A static string ("Sun".."Sat"). */
static const char* dayAbbr(int wday) {
    static const char* D[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    return D[wday % 7];
}

// ─── WEATHER ─────────────────────────────────────────────────────────────────
/** @brief Fetch current weather + 7-day forecast from Open-Meteo into g_weather
 *         (see fetch.h). Always fetched in °C/mph; unit conversion happens at
 *         display time. Uses getString() to de-chunk the response. */
void fetchWeather() {
    g_fetchState[FSRC_WEATHER] = FS_ACTIVE;
    char url[420];
    // Always fetch in °C and mph — unit conversion to °F / km/h is done at display time
    // so toggling units takes effect instantly without waiting for a re-fetch.
    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast"
        "?latitude=%.4f&longitude=%.4f"
        "&current=temperature_2m,weather_code,relative_humidity_2m,wind_speed_10m"
        "&hourly=weather_code&forecast_hours=24"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min"
              ",precipitation_sum,wind_speed_10m_max,wind_direction_10m_dominant"
        "&wind_speed_unit=mph&timezone=auto&forecast_days=7",
        g_settings.lat, g_settings.lon);

    // open-meteo sends chunked Transfer-Encoding.  getStreamPtr() exposes raw
    // "HEX\r\n" chunk headers, so this source cannot use the streaming scanners
    // the others do — getString() is what de-chunks.  The body is only ~3-5 KB,
    // so the single allocation is not a heap risk; what matters here is that the
    // hand-rolled SSL release/reclaim block is gone in favour of httpStream().
    String body;
    httpStream(url,
        [](HTTPClient& http, void* ctx) -> bool {
            String& b = *(String*)ctx;
            b = http.getString();
            return b.length() > 0;
        },
        &body, "weather");
    LOG("[weather] body=%u\n", body.length());
    if (body.isEmpty()) { g_fetchState[FSRC_WEATHER] = FS_FAIL; return; }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);

    if (err) { LOG("[weather] JSON: %s\n", err.c_str()); g_fetchState[FSRC_WEATHER] = FS_FAIL; return; }

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
    LOG("[weather] dayCount=%d  times.size=%d\n", wd.dayCount, (int)times.size());

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

#if FORCE_LIGHTNING
    wd.lightningHours = 0;    // TEST: pretend thunder is imminent
    wd.lightningPct   = 99;
#endif

    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    g_weather = wd;
    xSemaphoreGive(g_dataMutex);
    g_fetchState[FSRC_WEATHER] = FS_OK;
    LOG("[weather] OK  %dC  rain:%s  ltng:%dh(%d%%)  days:%d\n",
                  wd.temp, wd.rainIn,
                  (int)wd.lightningHours, (int)wd.lightningPct,
                  wd.dayCount);
}

// ─── SOLAR ────────────────────────────────────────────────────────────────────
/** @brief Fetch HF solar/geomagnetic indices and band conditions from hamqsl
 *         into g_solar (see fetch.h). */
void fetchSolar() {
    g_fetchState[FSRC_SOLAR] = FS_ACTIVE;
    // hamqsl solarxml.php is ~3 KB.  This is the memory-hungriest fetch (big
    // buffer + a cert that needs more RSA/BIGNUM scratch than the others), so it
    // was failing the handshake at low-heap moments.  httpFetchTextAlloc defers
    // the buffer's allocation until AFTER the handshake, freeing that heap for
    // the RSA/BIGNUM cert verify.  Parsed in place, then released.
    const size_t SOLAR_CAP = 6144;
    size_t slen = 0;
    char* sbuf = httpFetchTextAlloc(API_SOLAR, SOLAR_CAP, "solar", slen);
    if (!sbuf) { g_fetchState[FSRC_SOLAR] = FS_FAIL; return; }
    String body(sbuf);
    free(sbuf);

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
            LOG("[solar] WARNING: no <calculatedconditions> in body (len=%u)\n",
                          body.length());
        } else {
            int bpos = body.indexOf("<band", ccPos);
            if (bpos >= 0) {
                int bend = body.indexOf("</band>", bpos);
                String sample = body.substring(bpos, bend + 7);
                LOG("[solar] band sample: %s\n", sample.c_str());
            }
        }
    }

    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    g_solar = sd;
    xSemaphoreGive(g_dataMutex);
    g_fetchState[FSRC_SOLAR] = FS_OK;
    LOG("[solar] OK  SFI:%d K:%d A:%d  80m-day:%s\n",
                  sd.sfi, sd.kIndex, sd.aIndex, sd.bands[0].day);
}

// ─── RSS PARSER ───────────────────────────────────────────────────────────────

/**
 * @brief Find the real end of an RSS <item>, skipping CDATA-embedded </item>.
 * @param body       Full RSS body.
 * @param itemStart  Index of the '<' of the opening "<item>"/"<item ...>" tag.
 * @return Index of the matching "</item>" (its '<'), or -1 if none before EOF.
 */
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

/**
 * @brief Parse an RSS body into NewsData, extracting titles and thumbnail URLs.
 *
 * Handles CDATA, decodes HTML entities in titles, and resolves a thumbnail from
 * media:thumbnail, enclosure, or an embedded <img> (in that order).
 * @param body      Full RSS body (may be truncated mid-item).
 * @param[out] nd   Destination NewsData (count/valid/items populated).
 * @param maxItems  Maximum items to extract.
 */
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

/**
 * @brief Download a thumbnail image and publish it to a shared buffer atomically.
 *
 * Fetches into a private temp buffer (falling back from HTTPS to HTTP once),
 * then swaps it into @p buf under g_dataMutex and bumps @p version so the draw
 * task re-decodes. A truncated image is discarded (old thumbnail kept).
 * @param thumbUrl      Image URL; a null/empty URL clears @p len to 0.
 * @param buf           Shared destination buffer (THUMB_BUF_LEN bytes).
 * @param[in,out] len   Receives the new byte count (0 if nothing was published).
 * @param[in,out] version  Bumped when a new image is published.
 */
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
/**
 * @brief Advance a stream to just past the next "<item" opening tag.
 * @param s         Live response stream.
 * @param deadline  Absolute millis() deadline.
 * @return true if an item opener was found before the deadline/disconnect.
 */
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

/**
 * @brief Read one RSS item's body into a buffer, up to its </item>.
 *
 * Prepends "<item>" so parseRSS can find the opening tag. On overflow it keeps
 * draining the stream until </item> so the next item still aligns.
 * @param s         Live response stream (positioned just past "<item...>").
 * @param buf       Destination buffer.
 * @param bufLen    Size of @p buf in bytes.
 * @param deadline  Absolute millis() deadline.
 * @return Bytes written to @p buf (>= 6), or -1 on timeout/disconnect.
 */
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
struct RSSCtx { NewsData* nd; int maxItems; };

// The RSS reader already consumed one <item> at a time off the socket; this
// keeps that loop verbatim and only replaces the hand-rolled SSL choreography
// around it with httpStream().
/**
 * @brief StreamConsumer that parses an RSS feed item-by-item into NewsData.
 * @param http  Active HTTPClient with a live response.
 * @param ctx   Pointer to an RSSCtx (destination NewsData + max item count).
 * @return true if at least one item was parsed (0 items => httpStream retries).
 */
static bool rssConsume(HTTPClient& http, void* ctx) {
    RSSCtx&   c  = *(RSSCtx*)ctx;
    NewsData& nd = *c.nd;

    WiFiClient* stream   = http.getStreamPtr();
    uint32_t    deadline = millis() + 20000;

    // Static so it lives in .bss, not on the task stack.  fetchRSSNews is
    // always called sequentially (never re-entrant), so one shared buffer
    // across invocations is safe.
    static char itemBuf[1024];

    while (nd.count < c.maxItems && millis() < deadline) {
        if (!streamSkipToItem(stream, deadline)) break;
        int len = streamReadItem(stream, itemBuf, sizeof(itemBuf), deadline);
        if (len < 0) break;
        // Wrap the buffered item in a temporary String for parseRSS — ~1 KB,
        // freed each iteration.
        String itemStr(itemBuf);
        NewsData one;
        parseRSS(itemStr, one, 1);
        if (one.count > 0)
            nd.items[nd.count++] = one.items[0];
    }
    nd.valid = true;
    return nd.count > 0;   // zero items => httpStream retries
}

/**
 * @brief Fetch and parse an RSS feed into NewsData (shared by BBC/Apple).
 * @param url       Feed URL.
 * @param tag       Short label for log lines.
 * @param[out] nd   Destination NewsData.
 * @param maxItems  Maximum items to keep.
 * @return true if at least one item was parsed.
 */
static bool fetchRSSNews(const char* url, const char* tag,
                         NewsData& nd, int maxItems) {
    nd.count = 0;
    nd.valid = false;

    RSSCtx c = { &nd, maxItems };
    httpStream(url, rssConsume, &c, tag);

    Serial.printf("[%s] %d items\n", tag, nd.count);
    return nd.count > 0;
}

/** @brief Fetch the BBC News RSS feed + hero thumbnail into g_bbcNews (see fetch.h). */
void fetchBBCNews() {
    g_fetchState[FSRC_BBC] = FS_ACTIVE;
    NewsData nd;
    if (!fetchRSSNews(API_BBC_RSS, "bbc", nd, 4)) { g_fetchState[FSRC_BBC] = FS_FAIL; return; }
    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    g_bbcNews = nd;
    xSemaphoreGive(g_dataMutex);
    g_fetchState[FSRC_BBC] = FS_OK;
    fetchThumb(nd.items[0].thumbUrl, g_bbcThumbBuf, g_bbcThumbLen, g_bbcThumbVersion);
}

/** @brief Fetch the Apple/Mac News RSS feed + hero thumbnail into g_appleNews
 *         (see fetch.h). Routes the hero image through a resize proxy to fit the
 *         thumbnail buffer. */
void fetchAppleNews() {
    g_fetchState[FSRC_APPLE] = FS_ACTIVE;
    NewsData nd;
    if (!fetchRSSNews(API_APPLE_RSS, "apple", nd, 4)) { g_fetchState[FSRC_APPLE] = FS_FAIL; return; }
    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    g_appleNews = nd;
    xSemaphoreGive(g_dataMutex);
    g_fetchState[FSRC_APPLE] = FS_OK;

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
        LOG("[apple] thumb via wsrv.nl: %s\n", proxyUrl);
        fetchThumb(proxyUrl, g_appleThumbBuf, g_appleThumbLen, g_appleThumbVersion);
    }
}


// ─── S&P 500 — Yahoo Finance v8, byte-level stream search ────────────────────
// Streams the response and locates the "close":[ array without loading the
// entire ~50 KB JSON into RAM.  No ArduinoJson involved (the filter approach
// silently discarded the data on this endpoint).
// Yahoo streams a large JSON chart payload; we only need the "close" array, so
// this scans for it byte-by-byte and never buffers the body.  Unchanged from the
// previous implementation apart from losing its hand-rolled SSL block.
struct TrackerCtx { float* closes; int closeCount; size_t total; };

/**
 * @brief StreamConsumer that scrapes the "close" price array from Yahoo's chart JSON.
 *
 * Scans byte-by-byte for the `"close":[` array so the large payload is never
 * buffered; collects up to TRACKER_POINTS values > 100.
 * @param http  Active HTTPClient with a live response.
 * @param ctx   Pointer to a TrackerCtx (destination array + counters).
 * @return true if at least one close value was captured.
 */
static bool trackerConsume(HTTPClient& http, void* ctx) {
    TrackerCtx& st = *(TrackerCtx*)ctx;
    WiFiClient* stream = http.getStreamPtr();

    static const char NEEDLE[]  = "\"close\":[";
    const int         NEEDLE_LEN = 9;
    int      matchPos = 0;
    bool     inArray  = false;
    bool     done     = false;
    char     numBuf[20];
    int      numLen   = 0;
    uint32_t t0       = millis();
    uint8_t  cbuf[256];

    while (!done && (millis() - t0) < 25000 && st.total < 300000UL) {
        int av = stream->available();
        if (av <= 0) {
            if (!stream->connected()) break;
            delay(5);
            continue;
        }
        size_t n = stream->readBytes(cbuf, (size_t)min(av, (int)sizeof(cbuf)));
        st.total += n;
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
                        if (v > 100.0f && st.closeCount < TRACKER_POINTS)
                            st.closes[st.closeCount++] = v;
                        numLen = 0;
                    }
                    if (ch == ']') done = true;
                }
            }
        }
    }
    if (numLen > 0 && st.closeCount < TRACKER_POINTS) {
        numBuf[numLen] = '\0';
        float v = atof(numBuf);
        if (v > 100.0f) st.closes[st.closeCount++] = v;
    }
    return st.closeCount > 0;
}

/** @brief Fetch the configured stock symbol's weekly price history from Yahoo
 *         Finance into g_tracker (see fetch.h). */
void fetchTracker() {
    char trackerUrl[128];
    snprintf(trackerUrl, sizeof(trackerUrl),
             "https://query1.finance.yahoo.com/v8/finance/chart/%s?interval=1wk&range=%dy",
             g_settings.trackerSymbol, (int)g_settings.trackerRangeYears);

    g_fetchState[FSRC_TRACKER] = FS_ACTIVE;
    float closes[TRACKER_POINTS];

    TrackerCtx st = { closes, 0, 0 };
    // Yahoo rejects non-browser user agents.
    httpStream(trackerUrl, trackerConsume, &st, "tracker", 3,
               "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
               "AppleWebKit/537.36 (KHTML, like Gecko) "
               "Chrome/124.0.0.0 Safari/537.36");

    const int    closeCount = st.closeCount;
    const size_t total      = st.total;

    LOG("[tracker] read=%u  closes=%d\n", (unsigned)total, closeCount);

    if (closeCount < 2) {
        Serial.println("[tracker] insufficient data — check URL/network");
        g_fetchState[FSRC_TRACKER] = FS_FAIL;
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
    g_fetchState[FSRC_TRACKER] = FS_OK;
    LOG("[tracker] OK  $%.2f  %+.2f%%  pts=%d\n",
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

/**
 * @brief Estimate a station's location from its callsign prefix (DXCC table).
 *
 * Strips any "/suffix" then tries prefix lengths 3, 2, 1 in order (longest match).
 * @param call      Callsign string.
 * @param[out] lat  Receives the entity's approximate latitude in degrees.
 * @param[out] lon  Receives the entity's approximate longitude in degrees.
 * @return true if the prefix was matched (out-params set), false otherwise.
 */
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

/**
 * @brief Great-circle distance between two lat/lon points (haversine).
 * @param la1,lo1  First point's latitude/longitude in degrees.
 * @param la2,lo2  Second point's latitude/longitude in degrees.
 * @return Distance in kilometres.
 */
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

/**
 * @brief Initial great-circle bearing from one point toward another.
 * @param la1,lo1  Start latitude/longitude in degrees.
 * @param la2,lo2  Destination latitude/longitude in degrees.
 * @return Initial bearing in degrees (0–360, 0 = north).
 */
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

// Candidate pool — file scope so the streaming block handler can reach it, and
// static so 50 x DXSpot stays off the fetch task stack.
struct DXCand { DXSpot spot; float dist; };
static DXCand cands[50];

/**
 * @brief BlockHandler for one DX-cluster <spot> element.
 *
 * Parses the spot, derives spotter/DX locations from the prefix table, and adds
 * it to the candidate pool — deduplicating by spotted callsign (keeping the
 * closer spotter).
 * @param blockStr  NUL-terminated "<spot>...</spot>" element text.
 * @param ctx       Pointer to the running candidate count (int*).
 */
static void dxOnBlock(const char* blockStr, void* ctx) {
    int& nc = *(int*)ctx;
    if (nc >= 50) return;

    String block(blockStr);

    String dxStr  = xmlGet(block, "dx");
    String sptStr = xmlGet(block, "spotter");
    String frStr  = xmlGet(block, "frequency");
    String cmtStr = xmlGet(block, "comment");
    String tmStr  = xmlGet(block, "time");   // "2026-05-26 10:28:00"

    if (dxStr.isEmpty() || frStr.isEmpty()) return;
    float fr = frStr.toFloat();
    if (fr < 1.0f) return;

    // Trim whitespace from callsigns.
    dxStr.trim();
    sptStr.trim();

    // Decode HTML entities in comment (&lt; &gt; &amp; &apos; etc.)
    cmtStr = decodeHtmlEntities(cmtStr);
    cmtStr.trim();

    // Convert "YYYY-MM-DD HH:MM:SS" -> "HH:MMZ"
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
            return;
        }
    }

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

/** @brief StreamConsumer that scans DX-cluster XML for <spot> elements.
 *  @param http  Active HTTPClient. @param ctx  Pointer to the candidate count.
 *  @return true if at least one spot was parsed. */
static bool dxConsume(HTTPClient& http, void* ctx) {
    char blockBuf[512];          // one <spot> element; DX comments are short
    int n = scanBlocks(http, "<spot", "</spot>", dxOnBlock, ctx,
                       blockBuf, sizeof(blockBuf));
    if (n == 0) Serial.println("[dx] XML: no <spot> elements found");
    return n > 0;
}

/** @brief Fetch recent DX-cluster spots into g_dxSpots, sorted by distance from
 *         the QTH to the spotter (nearest first). See fetch.h. */
void fetchDXSpots() {
    g_fetchState[FSRC_DX] = FS_ACTIVE;
    int nc = 0;
    if (!httpStream(API_DXSPOTS, dxConsume, &nc, "dx")) {
        g_fetchState[FSRC_DX] = FS_FAIL;
        return;
    }


    if (nc == 0) { Serial.println("[dx] no valid spots"); g_fetchState[FSRC_DX] = FS_FAIL; return; }

    // Insertion-sort by distance (nearest first; unknowns at end).
    for (int i = 1; i < nc; i++) {
        DXCand tmp = cands[i];
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
    g_fetchState[FSRC_DX] = FS_OK;
    LOG("[dx] OK  %d spots (from %d unique)\n", ds.count, nc);
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
/** @brief Fetch the current ISS position from open-notify, then analytically
 *         compute its upcoming ground track and the next visible pass into g_iss
 *         (see fetch.h). Uses two consecutive readings to resolve ascending vs
 *         descending. */
void fetchISS() {
    g_fetchState[FSRC_ISS] = FS_ACTIVE;
    // ~200-byte response — a fixed stack buffer beats any streaming parser.
    char issBuf[512];
    if (!httpFetchText(API_ISS, issBuf, sizeof(issBuf), "iss")) {
        g_fetchState[FSRC_ISS] = FS_FAIL; return;
    }
    String body(issBuf);

    int latIdx = body.indexOf("\"latitude\"");
    int lonIdx = body.indexOf("\"longitude\"");
    // Was a bare return, which left the source stuck ACTIVE forever on a
    // malformed response instead of retrying at the failure interval.
    if (latIdx < 0 || lonIdx < 0) { g_fetchState[FSRC_ISS] = FS_FAIL; return; }

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
    iss.valid        = true;
    iss.lat          = lat;
    iss.lon          = lon;
    iss.trackCount   = 32;
    iss.orbitEpoch   = (int32_t)time(nullptr);
    iss.orbitU0      = u0;
    iss.orbitLambda0 = lambda0;
    iss.orbitLon0    = lon;

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

    // ── Next visible pass prediction ──────────────────────────────────────────
    // Scan forward in 30 s steps for up to 3 orbits (~16800 s).
    // Elevation formula:  slant² = Re² + (Re+h)² – 2·Re·(Re+h)·cos(ρ)
    //                     el     = arcsin( ((Re+h)·cos(ρ) – Re) / slant )
    // Azimuth: bearing from QTH to sub-satellite point.
    {
        const float RE    = 6371.0f;       // Earth radius, km
        const float ALT   = 408.0f;        // ISS altitude, km
        const float MINEL = 5.0f;          // minimum useful elevation, °
        const float D2R   = (float)M_PI / 180.0f;

        float qLat = g_settings.lat * D2R;
        float qLon = g_settings.lon * D2R;
        time_t nowT = time(nullptr);

        bool    inPass   = false;
        int32_t riseStep = 0;
        float   riseAz   = 0.0f;
        float   maxEl    = -90.0f;

        iss.passValid = false;

        for (int step = 0; step <= 432000; step += 30) {   // up to 5 days
            float dt  = (float)step;
            float u   = u0 + N * dt;

            float sLatR = asinf(constrain(sinf(INC) * sinf(u), -1.0f, 1.0f));
            float dLam  = atan2f(cosf(INC) * sinf(u), cosf(u)) - lambda0;
            float sLonD = lon + dLam * 180.0f / (float)M_PI - EARTH_W * dt;
            while (sLonD >  180.0f) sLonD -= 360.0f;
            while (sLonD < -180.0f) sLonD += 360.0f;
            float sLonR = sLonD * D2R;

            float cosRho = constrain(
                sinf(qLat)*sinf(sLatR) + cosf(qLat)*cosf(sLatR)*cosf(sLonR - qLon),
                -1.0f, 1.0f);
            float slant = sqrtf(RE*RE + (RE+ALT)*(RE+ALT) - 2.0f*RE*(RE+ALT)*cosRho);
            float el = asinf(((RE+ALT)*cosRho - RE) / slant) * 180.0f / (float)M_PI;

            float dLon = sLonR - qLon;
            float az   = atan2f(sinf(dLon)*cosf(sLatR),
                                cosf(qLat)*sinf(sLatR) - sinf(qLat)*cosf(sLatR)*cosf(dLon))
                         * 180.0f / (float)M_PI;
            if (az < 0.0f) az += 360.0f;

            if (!inPass && el >= MINEL) {
                inPass   = true;
                riseStep = step;
                riseAz   = az;
                maxEl    = el;
            } else if (inPass && el >= MINEL) {
                if (el > maxEl) maxEl = el;
            } else if (inPass && el < MINEL) {
                iss.passValid   = true;
                iss.passNow     = (riseStep == 0);
                iss.passRiseSec = (int32_t)nowT + riseStep;
                iss.passSetSec  = (int32_t)nowT + step;
                iss.passRiseAz  = (int16_t)roundf(riseAz);
                iss.passSetAz   = (int16_t)roundf(az);
                iss.passMaxEl   = (int8_t)roundf(maxEl);

                // Recompute ISS position at pass midpoint for illumination check
                {
                    float mDt  = (float)((riseStep + step) / 2);
                    float mU   = u0 + N * mDt;
                    float mLat = asinf(constrain(sinf(INC)*sinf(mU), -1.0f, 1.0f));
                    float mDLam = atan2f(cosf(INC)*sinf(mU), cosf(mU)) - lambda0;
                    float mLonD = lon + mDLam * 180.0f / (float)M_PI - EARTH_W * mDt;
                    while (mLonD >  180.0f) mLonD -= 360.0f;
                    while (mLonD < -180.0f) mLonD += 360.0f;
                    iss.passMidLat = mLat * 180.0f / (float)M_PI;
                    iss.passMidLon = mLonD;
                }
                break;
            }
        }

        // ── Current azimuth/elevation + range rate (valid when passNow) ─────
        {
            float issLatR = iss.lat * D2R;
            float issLonR = iss.lon * D2R;
            float cosRho  = constrain(
                sinf(qLat)*sinf(issLatR) + cosf(qLat)*cosf(issLatR)*cosf(issLonR - qLon),
                -1.0f, 1.0f);
            float slant0 = sqrtf(RE*RE + (RE+ALT)*(RE+ALT) - 2.0f*RE*(RE+ALT)*cosRho);
            iss.currentEl = asinf(((RE+ALT)*cosRho - RE) / slant0) * 180.0f / (float)M_PI;
            float dLon    = issLonR - qLon;
            float az      = atan2f(sinf(dLon)*cosf(issLatR),
                                   cosf(qLat)*sinf(issLatR) - sinf(qLat)*cosf(issLatR)*cosf(dLon))
                            * 180.0f / (float)M_PI;
            if (az < 0.0f) az += 360.0f;
            iss.currentAz = az;

            // Range rate via 1-second numerical differentiation (km/s, +ve = approaching)
            {
                const float dt1 = 1.0f;
                float u1   = u0 + N * dt1;
                float lat1 = asinf(constrain(sinf(INC)*sinf(u1), -1.0f, 1.0f));
                float dLam1 = atan2f(cosf(INC)*sinf(u1), cosf(u1)) - lambda0;
                float lon1D = lon + dLam1 * 180.0f / (float)M_PI - EARTH_W * dt1;
                while (lon1D >  180.0f) lon1D -= 360.0f;
                while (lon1D < -180.0f) lon1D += 360.0f;
                float lon1R = lon1D * D2R;
                float cosRho1 = constrain(
                    sinf(qLat)*sinf(lat1) + cosf(qLat)*cosf(lat1)*cosf(lon1R - qLon),
                    -1.0f, 1.0f);
                float slant1 = sqrtf(RE*RE + (RE+ALT)*(RE+ALT) - 2.0f*RE*(RE+ALT)*cosRho1);
                iss.rangeRateKmps = (slant0 - slant1) / dt1;   // +ve = approaching
            }
        }
    }

    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    g_iss = iss;
    xSemaphoreGive(g_dataMutex);
    g_fetchState[FSRC_ISS] = FS_OK;
    LOG("[iss] lat=%.2f  lon=%.2f  %s  track=%d pts\n",
                  lat, lon, ascending ? "asc" : "desc", iss.trackCount);
}

// ─── SOTA Spots ───────────────────────────────────────────────────────────────
// api2.sota.org.uk/api/spots/30 — last 30 SOTA activations.
// No lat/lon in the response; distance is estimated by stripping the portable
// suffix (/P, /M, /QRP…) from the activator callsign and using pfxToLatLon().
// Parses straight off the socket.  The raw response is ~20 KB, but the filter
// keeps only the seven fields actually used, so the document stays a few KB and
// no whole-body buffer ever exists.  This is what the old 24576-byte String was
// for — and what silently truncated into "IncompleteInput" when it could not grow.
/**
 * @brief StreamConsumer that parses the SOTA spots JSON (with a field filter).
 *
 * Deserialises directly from the stream keeping only the seven used fields, so
 * no whole-body buffer is needed.
 * @param http  Active HTTPClient with a live response.
 * @param ctx   Pointer to the destination JsonDocument.
 * @return true on successful parse, false on a JSON error.
 */
static bool sotaConsume(HTTPClient& http, void* ctx) {
    JsonDocument& doc = *(JsonDocument*)ctx;

    JsonDocument filter;
    JsonObject f = filter.add<JsonObject>();   // array-of-objects filter shape
    f["activatorCallsign"] = true;
    f["associationCode"]   = true;
    f["summitCode"]        = true;
    f["summitDetails"]     = true;
    f["frequency"]         = true;
    f["mode"]              = true;
    f["timeStamp"]         = true;

    DeserializationError err = deserializeJson(
        doc, *http.getStreamPtr(), DeserializationOption::Filter(filter));
    if (err) { LOG("[sota] JSON: %s\n", err.c_str()); return false; }
    return true;
}

/** @brief Fetch recent SOTA activator spots into g_sotaSpots, sorted by distance
 *         from the QTH (nearest first). See fetch.h. */
void fetchSOTASpots() {
    g_fetchState[FSRC_SOTA] = FS_ACTIVE;

    JsonDocument doc;
    if (!httpStream(API_SOTASPOTS, sotaConsume, &doc, "sota")) {
        g_fetchState[FSRC_SOTA] = FS_FAIL;
        return;
    }

    if (!doc.is<JsonArray>()) { Serial.println("[sota] expected array"); g_fetchState[FSRC_SOTA] = FS_FAIL; return; }
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

    if (nc == 0) { Serial.println("[sota] no valid spots"); g_fetchState[FSRC_SOTA] = FS_FAIL; return; }

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
    g_fetchState[FSRC_SOTA] = FS_OK;
    LOG("[sota] OK  %d spots (from %d)\n", ds.count, nc);
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

/**
 * @brief Read the next JSON `{...}` object from a stream into a buffer.
 *
 * Tracks brace depth and string escaping so nested braces don't end it early.
 * @param s       Live response stream.
 * @param buf     Destination buffer (NUL-terminated on a complete object).
 * @param bufLen  Size of @p buf in bytes.
 * @return Object length (>0); 0 at the end of the top-level array (']' seen);
 *         -1 on timeout; -2 if the object overflowed @p buf (caller should skip).
 */
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

/**
 * @brief Extract a field's value from a flat JSON object string.
 *
 * Handles quoted strings and unquoted scalars; a literal "null" yields "".
 * @param obj         NUL-terminated JSON object text.
 * @param key         Field name to look up.
 * @param[out] out    Destination buffer (empty string if the key is absent).
 * @param outLen      Size of @p out in bytes.
 */
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

// File scope so the streaming consumer can reach it; static so 100 x POTASpot
// stays off the fetch task stack.
struct POTACand { POTASpot spot; float dist; };
static POTACand potaCands[100];
struct POTACtx  { int nc; bool streamOk; };

// POTA already parsed one JSON object at a time; this keeps that loop verbatim
// and only replaces the hand-rolled SSL choreography around it with httpStream.
/**
 * @brief StreamConsumer that parses the POTA spots JSON one object at a time.
 *
 * Uses lat/lon from each object directly for distance, deduplicating by
 * activator (keeping the closest).
 * @param http  Active HTTPClient with a live response.
 * @param ctx   Pointer to a POTACtx (candidate count + clean-EOF flag).
 * @return true if the array was consumed to its closing ']'.
 */
static bool potaConsume(HTTPClient& http, void* ctx) {
    POTACtx& st = *(POTACtx*)ctx;
    static char objBuf[2048];

    WiFiClient* stream = http.getStreamPtr();
    int objLen;
    while ((objLen = potaReadObj(stream, objBuf, sizeof(objBuf))) != -1) {
        if (objLen == 0) { st.streamOk = true; break; }  // ']' seen
        if (objLen < 0) continue;                        // oversized, skip

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

        // Convert "2026-05-26T11:07:54" -> "11:07Z"
        char tmBuf[8] = "";
        const char* tptr = strchr(tm, 'T');
        if (tptr && strlen(tptr) >= 6)
            snprintf(tmBuf, sizeof(tmBuf), "%.5sZ", tptr + 1);
        else
            strlcpy(tmBuf, tm, sizeof(tmBuf));

        // Deduplicate by activator — keep closest.
        bool dup = false;
        for (int i = 0; i < st.nc; i++) {
            if (strcasecmp(potaCands[i].spot.activator, act) == 0) {
                if (dist < potaCands[i].dist) {
                    potaCands[i].dist = dist;
                    strlcpy(potaCands[i].spot.time, tmBuf, sizeof(potaCands[i].spot.time));
                    potaCands[i].spot.dist_km = (gotLoc && dist < 65535.0f)
                                              ? (uint16_t)dist : 0;
                }
                dup = true; break;
            }
        }
        if (dup || st.nc >= 100) continue;

        POTASpot& s = potaCands[st.nc].spot;
        strlcpy(s.activator, act,  sizeof(s.activator));
        strlcpy(s.reference, ref,  sizeof(s.reference));
        strlcpy(s.parkName,  name, sizeof(s.parkName));
        s.freq = fr;
        strlcpy(s.mode, mode, sizeof(s.mode));
        strlcpy(s.time, tmBuf, sizeof(s.time));
        s.dist_km = (gotLoc && dist < 65535.0f) ? (uint16_t)dist : 0;
        potaCands[st.nc].dist = dist;
        st.nc++;
    }
    return st.streamOk || st.nc > 0;
}

/** @brief Fetch recent POTA activator spots into g_potaSpots, sorted by distance
 *         from the QTH (nearest first). See fetch.h. */
void fetchPOTASpots() {
    g_fetchState[FSRC_POTA] = FS_ACTIVE;

    POTACtx st = { 0, false };
    httpStream(API_POTASPOTS, potaConsume, &st, "pota");
    const int nc = st.nc;


    if (nc == 0) { Serial.println("[pota] no valid spots"); g_fetchState[FSRC_POTA] = FS_FAIL; return; }

    // Insertion-sort nearest first.
    for (int i = 1; i < nc; i++) {
        POTACand tmp = potaCands[i];
        int j = i - 1;
        while (j >= 0 && potaCands[j].dist > tmp.dist) { potaCands[j + 1] = potaCands[j]; j--; }
        potaCands[j + 1] = tmp;
    }

    POTASpotsData ds;
    ds.valid = true;
    ds.count = (uint8_t)min(nc, POTA_SPOTS_MAX);
    for (int i = 0; i < ds.count; i++) ds.spots[i] = potaCands[i].spot;

    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    g_potaSpots = ds;
    xSemaphoreGive(g_dataMutex);
    g_fetchState[FSRC_POTA] = FS_OK;
    LOG("[pota] OK  %d spots (from %d)\n", ds.count, nc);
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

/** @brief Parse a leading 3-letter English month name.
 *  @param s  String beginning with a month abbreviation (e.g. "May 26").
 *  @return Month number 1–12, or -1 if unrecognised. */
static int cxMonthNum(const String& s) {
    const char* mo[12] = {"Jan","Feb","Mar","Apr","May","Jun",
                          "Jul","Aug","Sep","Oct","Nov","Dec"};
    for (int i = 0; i < 12; i++) if (s.startsWith(mo[i])) return i + 1;
    return -1;
}

/**
 * @brief Convert a UTC calendar date/time to a Unix epoch (timezone-independent).
 * @param year,mo,dy  Year, month (1–12), day (1–31).
 * @param hh,mm       Hour (0–23) and minute (0–59).
 * @return Seconds since the Unix epoch.
 */
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

/**
 * @brief Parse a "HHMM" time at a given offset.
 * @param s        Source string.
 * @param pos      Offset of the first digit.
 * @param[out] h   Receives the hour.
 * @param[out] m   Receives the minute.
 * @return true if a valid 24-hour time was parsed.
 */
static bool cxParseHHMM(const String& s, int pos, int& h, int& m) {
    if (pos + 4 > (int)s.length()) return false;
    h = (s[pos]-'0')*10 + (s[pos+1]-'0');
    m = (s[pos+2]-'0')*10 + (s[pos+3]-'0');
    return (h >= 0 && h <= 23 && m >= 0 && m <= 59);
}

/**
 * @brief Parse a "Mon DD" date, skipping any leading 'Z', ',', or spaces.
 * @param s           Source string.
 * @param pos         Offset to start scanning from.
 * @param[out] month  Receives the month (1–12).
 * @param[out] day    Receives the day (1–31).
 * @return true if a valid month/day was parsed.
 */
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

/**
 * @brief Parse a contest description's time range into UTC epoch values.
 *
 * Handles same-day ("0100Z-0159Z, May 26") and multi-day ("0000Z, May 30 to
 * 2359Z, May 31") formats, inferring the year and handling year wrap.
 * @param desc          Contest <description> time text.
 * @param[out] tStart   Receives the start epoch.
 * @param[out] tEnd     Receives the end epoch.
 * @return true if the range parsed, false otherwise.
 */
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

// Two buckets: active (up to CONTEST_MAX) and upcoming (up to CONTEST_MAX).
// File scope so the streaming block handler can reach them, static so they stay
// off the fetch task stack.
struct CxEntry { Contest c; time_t tStart; };
static CxEntry cxActive[CONTEST_MAX], cxUpcoming[CONTEST_MAX];
struct CxCtx  { time_t now; int nActive; int nUpcoming; };

/**
 * @brief BlockHandler for one contest-calendar RSS <item>.
 *
 * Parses the times, discards finished contests, classifies active vs upcoming,
 * and files the entry into the appropriate bucket.
 * @param blockStr  NUL-terminated "<item>...</item>" element text.
 * @param ctx       Pointer to a CxCtx (now-time and per-bucket counters).
 */
static void cxOnBlock(const char* blockStr, void* ctx) {
    CxCtx& st = *(CxCtx*)ctx;
    if (st.nActive + st.nUpcoming >= CONTEST_MAX * 2) return;

    String blk(blockStr);

    String title = decodeHtmlEntities(xmlGet(blk, "title"));
    String desc  = decodeHtmlEntities(xmlGet(blk, "description"));
    String link  = xmlGet(blk, "link");
    title.trim(); desc.trim(); link.trim();
    if (title.isEmpty() || desc.isEmpty()) return;

    time_t tStart, tEnd;
    if (!cxParseTimes(desc, tStart, tEnd)) return;
    if (tEnd <= st.now) return;  // already finished

    bool isActive = (st.now >= tStart && st.now < tEnd);

    // Fill a Contest entry.
    CxEntry cx;
    strlcpy(cx.c.name,  title.c_str(), sizeof(cx.c.name));
    strlcpy(cx.c.times, desc.c_str(),  sizeof(cx.c.times));
    cx.c.ref[0] = '\0';
    { int ri = link.indexOf("ref=");
      if (ri >= 0) strlcpy(cx.c.ref, link.c_str() + ri + 4, sizeof(cx.c.ref)); }
    cx.c.active = isActive;
    cx.tStart   = tStart;

    if (isActive && st.nActive < CONTEST_MAX) {
        cxActive[st.nActive++] = cx;
    } else if (!isActive && st.nUpcoming < CONTEST_MAX) {
        cxUpcoming[st.nUpcoming++] = cx;
    }
}

/** @brief StreamConsumer that scans the contest-calendar RSS for <item> elements.
 *  @param http  Active HTTPClient. @param ctx  Pointer to a CxCtx.
 *  @return true if at least one item was parsed. */
static bool cxConsume(HTTPClient& http, void* ctx) {
    char blockBuf[1024];        // one RSS <item>, description included
    int n = scanBlocks(http, "<item>", "</item>", cxOnBlock, ctx,
                       blockBuf, sizeof(blockBuf));
    if (n == 0) Serial.println("[cx] no <item> elements");
    return n > 0;
}

/** @brief Fetch active/upcoming contests into g_contests, active first then
 *         upcoming in chronological order (see fetch.h). */
void fetchContests() {
    g_fetchState[FSRC_CONTESTS] = FS_ACTIVE;

    CxCtx st = { time(NULL), 0, 0 };
    if (!httpStream(API_CONTESTS, cxConsume, &st, "cx")) {
        g_fetchState[FSRC_CONTESTS] = FS_FAIL;
        return;
    }
    const int nActive   = st.nActive;
    int       nUpcoming = st.nUpcoming;


    // Insertion-sort upcoming by start time (RSS is roughly chronological,
    // but sort to be safe).
    for (int i = 1; i < nUpcoming; i++) {
        CxEntry tmp = cxUpcoming[i];
        int j = i - 1;
        while (j >= 0 && cxUpcoming[j].tStart > tmp.tStart) {
            cxUpcoming[j + 1] = cxUpcoming[j];
            j--;
        }
        cxUpcoming[j + 1] = tmp;
    }

    int total = nActive + nUpcoming;
    if (total == 0) { Serial.println("[cx] no current/upcoming contests"); g_fetchState[FSRC_CONTESTS] = FS_FAIL; return; }

    ContestData cd;
    cd.valid = true;
    cd.count = (uint8_t)min(total, CONTEST_MAX);
    for (int i = 0; i < nActive   && i < CONTEST_MAX; i++) cd.items[i]          = cxActive[i].c;
    for (int i = 0; i < nUpcoming && nActive + i < CONTEST_MAX; i++) cd.items[nActive + i] = cxUpcoming[i].c;

    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    g_contests = cd;
    xSemaphoreGive(g_dataMutex);
    g_fetchState[FSRC_CONTESTS] = FS_OK;
    LOG("[cx] OK  %d active  %d upcoming\n", nActive, min(nUpcoming, CONTEST_MAX - nActive));
}

/**
 * @brief Extract a labelled field value from a contest-detail HTML table.
 * @param html        The detail page HTML.
 * @param label       Row label to find (e.g. "Mode"); matched as "label:</td><td>".
 * @param[out] out    Destination buffer (empty if not found); <br> becomes " / ".
 * @param outLen      Size of @p out in bytes.
 */
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

/** @brief Fetch the detail body (mode/bands/exchange) for the contest requested
 *         via g_contestDetailReq into g_contestDetail (see fetch.h). */
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
    // Single detail page — one bounded allocation, released once parsed.
    const size_t CX_CAP = 8192;
    char* hbuf = (char*)malloc(CX_CAP);
    if (!hbuf) { setFailed("out of memory"); return; }
    if (!httpFetchText(url, hbuf, CX_CAP, "cx-detail")) {
        free(hbuf);
        setFailed("fetch failed");
        return;
    }
    String html(hbuf);
    free(hbuf);

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
/** @brief Resolve the requested lat/lon (g_tzLookupLat/Lon) to a TZ_LIST entry
 *         via Open-Meteo's timezone=auto resolver, into g_tzLookup (see fetch.h). */
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
    LOG("[tz-lookup] fetching %s\n", url);

    // Open-Meteo always responds with chunked Transfer-Encoding, so this cannot
    // use the streaming scanners — getStreamPtr() would expose raw "HEX\r\n"
    // chunk headers.  getString() de-chunks; same treatment as fetchWeather().
    String body;
    httpStream(url,
        [](HTTPClient& http, void* ctx) -> bool {
            String& b = *(String*)ctx;
            b = http.getString();
            return b.length() > 0;
        },
        &body, "tz-lookup", 1);   // one-shot: user is waiting on this
    if (body.isEmpty()) { Serial.println("[tz-lookup] fetch failed"); setResult(true, -1); return; }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) { LOG("[tz-lookup] JSON: %s\n", err.c_str()); setResult(true, -1); return; }

    const char* iana = doc["timezone"] | "";
    LOG("[tz-lookup] iana=%s\n", iana);
    int idx = tzFindByIana(iana);
    if (idx < 0) { Serial.println("[tz-lookup] no match"); setResult(true, -1); return; }

    LOG("[tz-lookup] matched %s\n", TZ_LIST[idx].name);
    setResult(false, idx);
}

// ─── PSK Reporter ─────────────────────────────────────────────────────────────
// Fetches who received our callsign in the last 15 minutes from pskreporter.info.
// XML is streamed into a String (≤32 KB) then parsed tag-by-tag with strstr().

/**
 * @brief Extract a double-quoted attribute value from a raw tag string.
 * @param tag         Tag attribute text (leading space expected).
 * @param attr        Attribute name to find.
 * @param[out] out    Destination buffer.
 * @param outLen      Size of @p out in bytes.
 * @return true if the attribute was found and non-empty.
 */
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

/**
 * @brief TagHandler for one PSK Reporter <receptionReport> element.
 *
 * Skips the outer wrapper (no receiverLocator), counts every inner report, and
 * stores up to PSK_REPORTS_MAX of them (call, grid→lat/lon, frequency, SNR).
 * @param tag  The element's attribute text.
 * @param ctx  Pointer to the destination PSKData.
 */
static void pskOnTag(const char* tag, void* ctx) {
    PSKData& nd = *(PSKData*)ctx;

    // Inner reports have receiverLocator; the outer wrapper element does not
    char locator[8] = "";
    if (!pskGetAttr(tag, "receiverLocator", locator, sizeof(locator))) return;

    nd.total++;
    if (nd.count >= PSK_REPORTS_MAX) return;

    PSKReport& r = nd.reports[nd.count];
    memset(&r, 0, sizeof(r));

    char freqStr[16] = "";
    char snrStr[8]   = "";
    pskGetAttr(tag, "receiverCallsign", r.call,  sizeof(r.call));
    pskGetAttr(tag, "frequency",        freqStr, sizeof(freqStr));
    pskGetAttr(tag, "sNR",              snrStr,  sizeof(snrStr));

    strlcpy(r.grid, locator, sizeof(r.grid));
    gridToLatLon(locator, r.lat, r.lon);
    r.freqMHz = (float)atof(freqStr) / 1e6f;
    r.snr     = (int8_t)atoi(snrStr);
    nd.count++;
}

/** @brief StreamConsumer that scans PSK Reporter XML for <receptionReport> tags.
 *  @param http  Active HTTPClient. @param ctx  Pointer to the destination PSKData.
 *  @return true always (HTTP 200 with zero reports is still valid). */
static bool pskConsume(HTTPClient& http, void* ctx) {
    char tagBuf[512];
    scanTags(http, "<receptionReport ", pskOnTag, ctx, tagBuf, sizeof(tagBuf));
    return true;   // HTTP 200 with zero reports is still a valid answer
}

/** @brief Fetch stations that heard the configured callsign in the last 15 min
 *         from PSK Reporter into g_pskData (see fetch.h). No-op if no callsign. */
void fetchPSKReporter() {
    if (!g_settings.callsign[0]) return;

    char url[160];
    snprintf(url, sizeof(url), API_PSKREPORTER, g_settings.callsign);

    g_fetchState[FSRC_PSK] = FS_ACTIVE;
    LOG("[psk] fetching for %s\n", g_settings.callsign);

    PSKData nd = {};
    if (!httpStream(url, pskConsume, &nd, "psk")) {
        Serial.println("[psk] fetch failed");
        xSemaphoreTake(g_dataMutex, portMAX_DELAY);
        g_pskData.fetchFailed = true;
        xSemaphoreGive(g_dataMutex);
        g_fetchState[FSRC_PSK] = FS_FAIL;
        return;
    }

    // Mark valid as soon as we get any successful HTTP response — even with
    // zero reports.  The screen uses valid=false to mean "never fetched yet"
    // and shows "Fetching…"; valid=true with count=0 just shows a blank map.
    nd.valid = true;
    LOG("[psk] OK  %d reports total, %d stored\n", nd.total, nd.count);

    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    g_pskData = nd;
    xSemaphoreGive(g_dataMutex);
    g_fetchState[FSRC_PSK] = FS_OK;
}

// ─── FT8 Spots — what local stations are hearing ─────────────────────────────
// Queries PSK Reporter by receiver grid square. Each <receptionReport> means a
// local station heard the sender. We deduplicate by senderCallsign, keeping the
// highest-SNR instance, so the map shows unique DX stations heard in the area.
/**
 * @brief TagHandler for one FT8 <receptionReport> element.
 *
 * Requires a sender locator (to place the map dot), counts every report, and
 * deduplicates by senderCallsign keeping the highest-SNR instance.
 * @param tag  The element's attribute text.
 * @param ctx  Pointer to the destination FT8SpotsData.
 */
static void ft8OnTag(const char* tag, void* ctx) {
    FT8SpotsData& nd = *(FT8SpotsData*)ctx;

    // Must have a sender locator to place the dot on the map
    char senderLoc[8] = "";
    if (!pskGetAttr(tag, "senderLocator", senderLoc, sizeof(senderLoc))) return;

    nd.total++;

    char senderCall[14] = "";
    char rxCall[14]     = "";
    char freqStr[16]    = "";
    char snrStr[8]      = "";
    pskGetAttr(tag, "senderCallsign",   senderCall, sizeof(senderCall));
    pskGetAttr(tag, "receiverCallsign", rxCall,     sizeof(rxCall));
    pskGetAttr(tag, "frequency",        freqStr,    sizeof(freqStr));
    pskGetAttr(tag, "sNR",              snrStr,     sizeof(snrStr));

    if (!senderCall[0]) return;

    float  freqMHz = (float)atof(freqStr) / 1e6f;
    int8_t snr     = (int8_t)atoi(snrStr);

    for (int i = 0; i < nd.count; i++) {
        if (strcmp(nd.spots[i].senderCall, senderCall) == 0) {
            if (snr > nd.spots[i].snr) {
                nd.spots[i].snr = snr;
                strlcpy(nd.spots[i].rxCall, rxCall, sizeof(nd.spots[i].rxCall));
                nd.spots[i].freqMHz = freqMHz;
            }
            return;
        }
    }

    if (nd.count < FT8_SPOTS_MAX) {
        FT8Spot& s = nd.spots[nd.count];
        memset(&s, 0, sizeof(s));
        strlcpy(s.senderCall, senderCall, sizeof(s.senderCall));
        strlcpy(s.senderGrid, senderLoc,  sizeof(s.senderGrid));
        strlcpy(s.rxCall,     rxCall,     sizeof(s.rxCall));
        gridToLatLon(senderLoc, s.lat, s.lon);
        s.freqMHz = freqMHz;
        s.snr     = snr;
        nd.count++;
    }
}

/** @brief StreamConsumer that scans FT8 PSK-Reporter XML for <receptionReport> tags.
 *  @param http  Active HTTPClient. @param ctx  Pointer to the destination FT8SpotsData.
 *  @return true always (HTTP 200 with zero reports is still valid). */
static bool ft8Consume(HTTPClient& http, void* ctx) {
    char tagBuf[512];
    scanTags(http, "<receptionReport ", ft8OnTag, ctx, tagBuf, sizeof(tagBuf));
    return true;
}

/** @brief Fetch FT8 DX stations heard in the configured grid square (first 4
 *         chars) from PSK Reporter into g_ft8Spots (see fetch.h). No-op if no grid. */
void fetchFT8Spots() {
    if (!g_settings.grid[0]) return;

    // Use only the first 4 characters of the grid (e.g. "IO92" from "IO92js")
    char grid4[5] = {};
    strlcpy(grid4, g_settings.grid, 5);

    const char* mode = g_settings.ft8ModeFt4 ? "FT4" : "FT8";
    char url[160];
    snprintf(url, sizeof(url), API_FT8SPOTS, grid4, mode);
    g_fetchState[FSRC_FT8] = FS_ACTIVE;
    LOG("[ft8] fetching %s for grid %s\n", mode, grid4);

    FT8SpotsData nd = {};
    if (!httpStream(url, ft8Consume, &nd, "ft8")) {
        xSemaphoreTake(g_dataMutex, portMAX_DELAY);
        g_ft8Spots.fetchFailed = true;
        xSemaphoreGive(g_dataMutex);
        g_fetchState[FSRC_FT8] = FS_FAIL;
        return;
    }

    nd.valid = true;
    g_fetchState[FSRC_FT8] = FS_OK;
    LOG("[ft8] %d unique DX stations from %d reports\n", nd.count, nd.total);

    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    g_ft8Spots = nd;
    xSemaphoreGive(g_dataMutex);
}

// ─── Background fetch task (core 1) ──────────────────────────────────────────
/** @brief Background fetch FreeRTOS task entry point (see fetch.h).
 *
 * Waits on the one-shot ready semaphore (so the SSL reserves are held before the
 * first TLS connection), performs the initial boot fetch, then loops refreshing
 * each source on its own schedule — news/stocks first, with quick (~90 s)
 * retries after a failure. Never returns. */
void fetchTask(void* param) {
    // Wait for setup() to free the SSL memory reserves
    if (param) {
        SemaphoreHandle_t ready = reinterpret_cast<SemaphoreHandle_t>(param);
        xSemaphoreTake(ready, portMAX_DELAY);
        vSemaphoreDelete(ready);   // one-shot; release the handle
    }
    LOG("[fetch] started  largestDMA=%u  heap=%u\n",
                  heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                  ESP.getFreeHeap());

    // Thumbnail buffers are pre-allocated in setup() (before the SSL reserves)
    // so they don't land in the C+D coalesced block that mbedTLS needs.
    // These guards are a last-resort fallback only.
    if (!g_bbcThumbBuf)   g_bbcThumbBuf   = (uint8_t*)malloc(THUMB_BUF_LEN);
    if (!g_appleThumbBuf) g_appleThumbBuf = (uint8_t*)malloc(THUMB_BUF_LEN);
    LOG("[fetch] started  bbc=%p apple=%p  largestDMA=%u  heap=%u\n",
                  g_bbcThumbBuf, g_appleThumbBuf,
                  heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                  ESP.getFreeHeap());

    bool weatherOk = false, solarOk    = false;
    bool newsOk    = false, stockOk    = false;
    bool issOk     = false, dxOk       = false;
    bool potaOk    = false, sotaOk     = false;
    bool contestOk = false, pskOk      = false;
    bool ft8Ok     = false;

    // ── Boot fetch ────────────────────────────────────────────────────────────
    fetchBBCNews();   fetchAppleNews();   fetchTracker();
    fetchWeather();   fetchSolar();       fetchISS();
    fetchDXSpots();   fetchPOTASpots();
    // fetchPSKReporter() / fetchFT8Spots() deferred to task loop: each successful
    // pskreporter.info connection leaves a 16 KB orphan TCP receive buffer ~30 s
    // after close (server continues sending after the client reads and disconnects).
    // Running them at boot fragments lb8bit from 34804 → 18420, which is below the
    // 33434 B mbedTLS minimum, breaking all subsequent task-loop HTTPS connections.
    fetchSOTASpots();
#if !DIAG_NO_CONTESTS
    fetchContests();
#endif
    // ContestCalendar re-enabled: CONFIG_MBEDTLS_HARDWARE_AES=n (platformio.ini)
    // forces software AES, eliminating the PADLOCK misalignment that previously
    // caused the orphan-buffer / ctx=FAIL crash.

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
    ft8Ok     = g_ft8Spots.valid;
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
    unsigned long lastFT8     = millis();

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
        if (now - lastSOTA >= (sotaOk ? REFRESH_SOTASPOTS_MS : 90000UL)) {
            fetchSOTASpots();
            xSemaphoreTake(g_dataMutex, portMAX_DELAY);
            sotaOk = g_sotaSpots.valid;
            xSemaphoreGive(g_dataMutex);
            lastSOTA = millis();
        }
        if (!DIAG_NO_CONTESTS &&
            now - lastContest >= (contestOk ? REFRESH_CONTESTS_MS : 90000UL)) {
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
            vTaskDelay(pdMS_TO_TICKS(500)); // let SSL cleanup before next HTTPS
        }
        if (g_forceFetchFT8 ||
            now - lastFT8 >= (ft8Ok ? REFRESH_FT8_MS : 60000UL)) {
            fetchFT8Spots();
            xSemaphoreTake(g_dataMutex, portMAX_DELAY);
            ft8Ok = g_ft8Spots.valid;
            xSemaphoreGive(g_dataMutex);
            lastFT8 = millis();
            g_forceFetchFT8 = false;
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
