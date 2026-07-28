#pragma once
#include "data_store.h"
#include <stddef.h>

// ─── Data fetchers ────────────────────────────────────────────────────────────
// Each runs on the fetch task, performs one HTTPS request, parses the response,
// and stores the result into its global (g_weather, g_solar, ...) under
// g_dataMutex, updating g_fetchState[FSRC_*]. They take no arguments, return
// nothing, and signal success/failure via the data's `.valid` flag and the
// fetch state (FS_OK / FS_FAIL).

/** @brief Fetch the current weather + 7-day forecast (open-meteo) into g_weather. */
void fetchWeather();
/** @brief Fetch HF solar/geomagnetic indices (hamqsl) into g_solar. */
void fetchSolar();
/** @brief Fetch the BBC News RSS feed + hero thumbnail into g_bbcNews. */
void fetchBBCNews();
/** @brief Fetch the Apple/Mac News RSS feed + hero thumbnail into g_appleNews. */
void fetchAppleNews();
/** @brief Fetch the configured stock symbol's price + history into g_tracker. */
void fetchTracker();
/** @brief Fetch the ISS position, ground track, and next pass into g_iss. */
void fetchISS();
/** @brief Fetch recent DX cluster spots into g_dxSpots. */
void fetchDXSpots();
/** @brief Fetch recent POTA activator spots into g_potaSpots. */
void fetchPOTASpots();
/** @brief Fetch recent SOTA activator spots into g_sotaSpots. */
void fetchSOTASpots();
/** @brief Fetch active/upcoming contests into g_contests. */
void fetchContests();
/** @brief Fetch the detail body for the contest requested via g_contestDetailReq. */
void fetchContestDetail();
/** @brief Resolve the requested lat/lon to an IANA timezone (g_tzLookup). */
void fetchTzLookup();
/** @brief Fetch PSK Reporter reception reports for the callsign into g_pskData. */
void fetchPSKReporter();
/** @brief Fetch FT8 DX spots for the configured grid square into g_ft8Spots. */
void fetchFT8Spots();

/**
 * @brief The background fetch FreeRTOS task entry point (pinned to core 0).
 *
 * Performs the initial boot fetch, then loops refreshing each source on its own
 * schedule. Never returns.
 * @param param  A one-shot SemaphoreHandle_t that setup() gives once the SSL
 *               memory reserves are ready; the task blocks on it before its
 *               first fetch, then deletes it. May be nullptr to skip the wait.
 */
void fetchTask(void* param);

// ─── Thumbnail JPEG buffers ───────────────────────────────────────────────────
/// Max thumbnail JPEG size (heap-allocated so .bss cost is zero).
#define THUMB_BUF_LEN 4096

extern uint8_t* g_bbcThumbBuf;    ///< BBC hero-image JPEG bytes (or nullptr).
extern size_t   g_bbcThumbLen;    ///< Valid byte count in g_bbcThumbBuf.
extern uint8_t* g_appleThumbBuf;  ///< Apple hero-image JPEG bytes (or nullptr).
extern size_t   g_appleThumbLen;  ///< Valid byte count in g_appleThumbBuf.

/// Incremented under g_dataMutex whenever the BBC thumbnail buffer changes;
/// the draw code compares against a cached copy to know when to re-decode.
extern uint32_t g_bbcThumbVersion;
/// As g_bbcThumbVersion, for the Apple thumbnail.
extern uint32_t g_appleThumbVersion;

// ─── SSL DRAM pre-reserves (see main.cpp for the full layout rationale) ───────
// Two blocks are allocated in setup() right after the sprite and HELD between
// SSL calls so nothing nibbles their slots. Freed only for the brief window of
// each TLS handshake / cert parse, then immediately reclaimed.
//
// SSL_X509_RESERVE is sized >= TLS_MIN_CONTIG (16717) so that releasing it just
// before a handshake yields a contiguous block big enough for the mbedTLS input
// record buffer ON ITS OWN — without depending on a fragmented neighbour block
// coalescing. That headroom is what stops long-uptime fragmentation from parking
// the largest free block right on the threshold (the transient-skip symptom).
#define SSL_X509_RESERVE 18432   // was 12288; raised for handshake headroom
#define SSL_CTX_RESERVE   6136
extern void* g_sslX509R;  ///< SSL_X509_RESERVE B — freed just before http.GET() (TLS cert chain).
extern void* g_sslCtxR;   ///< SSL_CTX_RESERVE B — freed just before the WiFiClientSecure ctor.
