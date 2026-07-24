#pragma once
#include <stdint.h>

/**
 * @brief Render the PSK Reporter screen into `spr`.
 *
 * Draws a world map of stations that heard the configured callsign, with a
 * per-band colour legend. Draws directly and returns nothing.
 */
void drawScreenPSKReporter();

/**
 * @brief Handle a tap on the PSK Reporter screen (called on finger-up).
 *
 * Tapping the furthest/loudest markers shows a detail overlay; tapping a spot
 * highlights its band in the legend; tapping the legend opens the band filter.
 * @param x  Touch x in screen pixels.
 * @param y  Touch y in screen pixels.
 * @return true if the tap was consumed, false otherwise.
 */
bool pskTouchUp(int32_t x, int32_t y);

/**
 * @brief Clear any active selection/overlay on the PSK Reporter screen.
 *
 * Called when navigating away so a stale overlay does not persist.
 */
void pskClearSelection();
