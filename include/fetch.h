#pragma once
#include "data_store.h"
#include <stddef.h>

void fetchWeather();
void fetchSolar();
void fetchBBCNews();
void fetchAppleNews();
void fetchTracker();
void fetchISS();
void fetchDXSpots();
void fetchPOTASpots();
void fetchSOTASpots();
void fetchContests();
void fetchContestDetail();
void fetchTzLookup();
void fetchPSKReporter();
void fetchFT8Spots();

// FreeRTOS task — pin to core 0
void fetchTask(void* param);

// Thumbnail JPEG buffers — heap pointers so .bss cost is zero.
#define THUMB_BUF_LEN 4096    //  4 KB — sufficient for wsrv.nl 118×66 JPEG at q=75 (~2–3 KB)
extern uint8_t* g_bbcThumbBuf;
extern size_t   g_bbcThumbLen;
extern uint8_t* g_appleThumbBuf;
extern size_t   g_appleThumbLen;


// Incremented under g_dataMutex each time a thumbnail buffer is updated.
// Screen draw functions compare against a cached version to know when to
// re-decode the JPEG into the shared thumbnail sprite.
extern uint32_t g_bbcThumbVersion;
extern uint32_t g_appleThumbVersion;

// SSL DRAM pre-reserves — see main.cpp for full layout description.
//
// Two blocks are allocated in setup() immediately after the sprite and HELD
// between SSL calls so nothing can nibble their slots.  Freed only for the
// brief window of each TLS handshake / cert parse, then immediately reclaimed.
//
//   g_sslX509R   12288 B — freed just before http.GET(); TLS cert-chain here.
//                          When freed it coalesces with the natural ~26 KB
//                          adjacent free block → ~38.9 KB available for
//                          mbedTLS in_buf (16717 B) + out_buf (16717 B) ✓
//   g_sslCtxR     6136 B — freed just before WiFiClientSecure constructor;
//                          sslclient_context() first-fits here.
extern void* g_sslX509R;
extern void* g_sslCtxR;
