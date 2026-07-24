#pragma once

/**
 * @brief Render the Clock screen into the shared `spr` sprite.
 *
 * Draws the large local time, the date, and sunrise/sunset plus moon-phase
 * information derived from the configured QTH location. Reads global time and
 * g_settings; the caller pushes the sprite to the display afterwards.
 */
void drawScreenClock();
