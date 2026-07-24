#pragma once
#include <stddef.h>

// Service menu / manual-screenshot helpers.

/// Runtime toggle: force a lightning alert (red LED + weather banner). Defined
/// in main.cpp, not persisted (resets on reboot). Used to stage documentation
/// screenshots.
extern bool g_fakeLightning;

/// Runtime toggle: force a synthetic ISS pass overhead (ISS Tracker overlay).
/// Defined in main.cpp, not persisted (resets on reboot).
extern bool g_fakeISSPass;

/**
 * @brief Capture the current framebuffer to a 24-bit BMP on the SD card.
 *
 * Reads the shared `spr` sprite and writes /scrNNNN.bmp (next free index) via
 * SdFat on the hardware VSPI bus. Blocks briefly while writing.
 * @param outName  Optional buffer to receive the bare filename written
 *                 (e.g. "scr0007.bmp"); pass nullptr to ignore.
 * @param outLen   Size of @p outName in bytes.
 * @return true on success; false if the SD card is absent/unmounted, the file
 *         cannot be created, or a write fails.
 */
bool screenshotToSD(char* outName = nullptr, size_t outLen = 0);

/**
 * @brief Probe whether a microSD card can be mounted.
 *
 * Mounts lazily; the mount is cached for subsequent calls.
 * @return true if the SD card mounted successfully, else false.
 */
bool sdCardPresent();
