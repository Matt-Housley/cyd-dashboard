#pragma once
#include "lgfx_config.h"

// Call once from setup() — creates the 320×240 sprite
void uiInit(LGFX* display);

// Render the given screen into the sprite and push to the display.
// autoPlay / paused are reflected in the status bar indicator.
// When inSettings is true, the settings overlay is drawn instead of the screen.
void uiDraw(int screenID, bool autoPlay, bool paused, bool inSettings = false);

// Status bar button X positions for touch hit-testing
void getStatusBarLayout(int& playX, int& advX, int& wifiX, int& cogX);

// WiFi info overlay — shown when the WiFi bars in the status bar are tapped
void showWifiOverlay(const char* ssid, const char* ip, const char* mac, int rssi);
void hideWifiOverlay();
bool wifiOverlayVisible();

// Live touch state — lets a screen's draw function react to an in-progress
// touch (e.g. dragging a scrollbar thumb) rather than only the final tap/swipe
// reported on finger-up.
bool    touchIsActive();
int32_t touchCurrentX();
int32_t touchCurrentY();
