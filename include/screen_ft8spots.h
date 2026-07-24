#pragma once
#include <stdint.h>

/**
 * @brief Render the FT8 Spots screen into `spr`.
 *
 * Draws a world map of DX stations heard by receivers in the configured grid
 * square, with a per-band colour legend and furthest/loudest markers.
 */
void drawScreenFT8Spots();

/**
 * @brief Clear any active selection/overlay on the FT8 Spots screen.
 *
 * Called when navigating away so a stale overlay does not persist.
 */
void ft8ClearSelection();

/**
 * @brief Handle a tap on the FT8 Spots screen (called on finger-up).
 *
 * May open/dismiss the furthest/loudest overlay, highlight a band in the
 * legend, or open the band-filter options screen.
 * @param x  Touch x in screen pixels.
 * @param y  Touch y in screen pixels.
 * @return true if the tap was consumed (so the caller can extend the auto-play
 *         pause), false otherwise.
 */
bool ft8TouchUp(int32_t x, int32_t y);
