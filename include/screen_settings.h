#pragma once
#include <Arduino.h>

/**
 * @brief Enter the settings UI: reset to the main menu page and load working copies.
 *
 * Call once when the user opens settings (e.g. taps the cog). Seeds the editors
 * (grid, callsign) and scrolls the timezone list to the current zone.
 */
void settingsEnter();

/**
 * @brief Draw the current settings page into the shared `spr` sprite.
 *
 * Called from uiDraw() whenever the app is in settings mode.
 */
void drawScreenSettings();

/**
 * @brief Handle a completed touch gesture on the settings UI (finger-up).
 * @param startX  X where the finger landed (screen pixels).
 * @param startY  Y where the finger landed.
 * @param endX    X where the finger lifted.
 * @param endY    Y where the finger lifted.
 * @param dtMs    Duration the finger was held, in milliseconds.
 * @return true when the settings screen should close (return to the dashboard),
 *         false to stay in settings.
 */
bool settingsTouchUp(int32_t startX, int32_t startY,
                     int32_t endX,   int32_t endY,
                     uint32_t dtMs);
