#pragma once
#include <stdint.h>

/**
 * @brief Render the Apple/Mac News screen into `spr`.
 *
 * Draws a hero story with thumbnail plus up to three headline rows from the
 * cached g_appleNews data. Also records each headline's hit-box for appleTouchUp.
 */
void drawScreenApple();

/**
 * @brief Handle a tap on the Apple News screen (called on finger-up).
 *
 * Tapping a headline opens the full-text popup (useful when it is truncated).
 * @param x  Touch x in screen pixels.
 * @param y  Touch y in screen pixels.
 * @return true if a headline was hit (popup opened), false otherwise.
 */
bool appleTouchUp(int32_t x, int32_t y);
