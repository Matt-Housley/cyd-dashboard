#pragma once
#include <Arduino.h>

// Call once when the user opens settings — resets to the main menu page
void settingsEnter();

// Draw the current settings page into the shared sprite.
// Called from uiDraw() whenever inSettings is true.
void drawScreenSettings();

// Handle a completed touch gesture from the settings UI.
// startX/Y = where the finger landed; endX/Y = where it lifted.
// dtMs = time finger was held.
// Returns true when the settings screen should close.
bool settingsTouchUp(int32_t startX, int32_t startY,
                     int32_t endX,   int32_t endY,
                     uint32_t dtMs);
