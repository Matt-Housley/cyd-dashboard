#pragma once
#include "lgfx_config.h"

// Call once from setup() — creates the 320×240 sprite
void uiInit(LGFX* display);

// Render the given screen into the sprite and push to the display.
// autoPlay / paused are reflected in the status bar indicator.
// When inSettings is true, the settings overlay is drawn instead of the screen.
void uiDraw(int screenID, bool autoPlay, bool paused, bool inSettings = false);

// Status bar button X positions for touch hit-testing
void getStatusBarLayout(int& playX, int& advX, int& cogX);
