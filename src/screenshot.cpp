#include <Arduino.h>
#include <SPI.h>
#include <SdFat.h>
#include <esp_heap_caps.h>
#include "service.h"
#include "config.h"
#include "lgfx_config.h"

// The shared framebuffer sprite (defined in ui.cpp). Fully drawn every frame,
// so reading it at any time yields a complete screen image.
extern LGFX_Sprite spr;

// SD on hardware VSPI (pins 18/19/23/5) — free because the touch is bit-banged
// (lgfx_config: touch spi_host = -1). SdFat (not the Arduino SD library) is used
// because it is lean: a single ~512-byte cache instead of the ~24 KB esp_vfs_fat
// stack, so it can stay mounted without collapsing the mbedTLS handshake reserve.
// SHARED_SPI => SdFat releases the VSPI bus between accesses.
static SPIClass s_sdSPI(VSPI);
#define SD_CFG SdSpiConfig(SD_CS, SHARED_SPI, SD_SCK_MHZ(20), &s_sdSPI)
static SdFat32 s_sd;
static bool    s_sdReady   = false;
static bool    s_spiBegun  = false;

/**
 * @brief Report the largest free contiguous internal 8-bit heap block.
 *
 * This is the figure that governs whether an mbedTLS handshake can proceed, so
 * it is logged before/after SD mount to prove SD does not starve TLS.
 * @return Size in bytes of the largest allocatable internal block.
 */
static uint32_t lb8() {
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

/**
 * @brief Lazily initialise VSPI and mount the SD card (idempotent, cached).
 *
 * Begins the VSPI bus once and mounts the FAT volume once; subsequent calls
 * return the cached result. Logs heap/largest-block before and after mount.
 * @return true if the card is mounted and ready; false on no card, non-FAT32
 *         formatting, wiring fault, or a stuck card.
 */
static bool ensureSD() {
    if (s_sdReady) return true;
    if (!s_spiBegun) {
        s_sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
        s_spiBegun = true;
    }
    uint32_t heap0 = ESP.getFreeHeap(), lb0 = lb8();
    s_sdReady = s_sd.begin(SD_CFG);
    Serial.printf("[sd] mount %s  heap %u->%u  lb8 %u->%u\n",
                  s_sdReady ? "OK" : "FAIL",
                  heap0, ESP.getFreeHeap(), lb0, lb8());
    if (!s_sdReady) Serial.println("[sd] (no card / not FAT32 / wiring / stuck-card: re-seat it)");
    return s_sdReady;
}

/** @brief Whether an SD card is present and mountable (see service.h).
 *  @return true if the card mounts (mounting it as a side effect), else false. */
bool sdCardPresent() { return ensureSD(); }

/** @brief Write a little-endian uint16 to an open file. */
static inline void wr16(File32& f, uint16_t v) { f.write((uint8_t*)&v, 2); }
/** @brief Write a little-endian uint32 to an open file. */
static inline void wr32(File32& f, uint32_t v) { f.write((uint8_t*)&v, 4); }

/**
 * @brief Save the current framebuffer to the SD card as a 24-bit BMP.
 *
 * Mounts the card if needed, picks the first free /scrNNNN.bmp filename, and
 * writes a bottom-up 24-bit BGR bitmap read from the shared sprite (undoing the
 * byte-swap that readRect() returns). See service.h.
 * @param[out] outName  Optional buffer to receive the written filename (without
 *                      the leading '/'); pass nullptr if not needed.
 * @param outLen        Size of @p outName in bytes.
 * @return true on success; false if the card is unavailable, all 10000 filename
 *         slots are used, the file cannot be opened, or a write fails.
 */
bool screenshotToSD(char* outName, size_t outLen) {
    Serial.printf("[sd] init VSPI  SCK=%d MISO=%d MOSI=%d CS=%d\n",
                  SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (!ensureSD()) return false;

    // Pick the first free scrNNNN.bmp
    char path[24];
    int idx = 0;
    for (; idx < 10000; idx++) {
        snprintf(path, sizeof(path), "/scr%04d.bmp", idx);
        if (!s_sd.exists(path)) break;
    }
    if (idx >= 10000) return false;

    File32 f = s_sd.open(path, O_WRITE | O_CREAT | O_TRUNC);
    if (!f) { Serial.printf("[sd] open %s failed\n", path); return false; }

    const int      W        = SCREEN_W, H = SCREEN_H;
    const uint32_t rowBytes = (uint32_t)W * 3;       // 960 → already 4-byte aligned
    const uint32_t dataSize = rowBytes * H;

    // ── BMP file header (14) ──
    wr16(f, 0x4D42);            // "BM"
    wr32(f, 54 + dataSize);
    wr32(f, 0);
    wr32(f, 54);
    // ── DIB (BITMAPINFOHEADER, 40) ──
    wr32(f, 40);
    wr32(f, (uint32_t)W);
    wr32(f, (uint32_t)H);       // positive height → rows stored bottom-up
    wr16(f, 1);
    wr16(f, 24);
    wr32(f, 0);                 // BI_RGB
    wr32(f, dataSize);
    wr32(f, 2835); wr32(f, 2835);
    wr32(f, 0); wr32(f, 0);

    // Pixel rows, bottom-up, 24-bit BGR
    static uint16_t line[SCREEN_W];       // RGB565 row
    static uint8_t  bgr [SCREEN_W * 3];
    bool ok = true;
    for (int y = H - 1; y >= 0 && ok; y--) {
        spr.readRect(0, y, W, 1, line);   // returns byte-swapped RGB565
        for (int x = 0; x < W; x++) {
            uint16_t px = line[x];
            px = (uint16_t)((px >> 8) | (px << 8));   // undo the byte swap
            uint8_t r = (px >> 11) & 0x1F, g = (px >> 5) & 0x3F, b = px & 0x1F;
            bgr[x * 3 + 0] = (uint8_t)((b << 3) | (b >> 2));
            bgr[x * 3 + 1] = (uint8_t)((g << 2) | (g >> 4));
            bgr[x * 3 + 2] = (uint8_t)((r << 3) | (r >> 2));
        }
        if (f.write(bgr, rowBytes) != (int)rowBytes) ok = false;
    }
    f.close();

    Serial.printf("[sd] wrote %s  %s\n", path, ok ? "OK" : "WRITE ERROR");
    if (ok && outName) strlcpy(outName, path + 1, outLen);
    return ok;
}
