#pragma once
#include <stdint.h>

/**
 * @brief Render the Weather screen into `spr`.
 *
 * Draws the current temperature (°C/°F), condition, humidity/wind/rain/lightning
 * stats, a lightning-alert banner when due, and a 7-day forecast strip from the
 * cached g_weather data. Draws directly and returns nothing.
 */
void drawScreenWeather();

// ─── Lightning alert (shared with the LED logic in main.cpp) ─────────────────

/**
 * @brief Whether a thunderstorm is forecast within the next hour.
 *
 * Also returns true when the Service-menu "Fake Lightning" toggle is on. As a
 * side effect, re-arms the acknowledge flag whenever the alert clears.
 * @return true if a lightning alert is currently active, else false.
 */
bool weatherLightningSoon();

/**
 * @brief Whether the red flashing LED has been silenced for the current alert.
 * @return true if the user tapped the alert box to silence the LED, else false.
 */
bool weatherLightningAcked();

/**
 * @brief Handle a tap on the Weather screen (called on finger-up).
 * @param x  Touch x in screen pixels.
 * @param y  Touch y in screen pixels.
 * @return true if the tap hit the lightning-alert box (silencing the red LED),
 *         false otherwise.
 */
bool weatherTouchUp(int32_t x, int32_t y);
