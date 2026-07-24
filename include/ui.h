#pragma once
#include "lgfx_config.h"

/**
 * @brief Initialise the UI: allocate the shared 320x240 off-screen sprite.
 *
 * Call once from setup(). Falls back from 16-bit to 8-bit colour depth if the
 * 16-bit sprite cannot be allocated from DMA-capable RAM.
 * @param display  Pointer to the initialised LGFX display driver.
 */
void uiInit(LGFX* display);

/**
 * @brief Render a screen (or the settings overlay) and push it to the display.
 * @param screenID   Screen to draw (a ScreenID / SCR_* value).
 * @param autoPlay   Whether auto-cycling is on (shown in the status bar).
 * @param paused     Whether cycling is currently paused (status bar indicator).
 * @param inSettings When true, the settings overlay is drawn instead of the
 *                   screen. Defaults to false.
 */
void uiDraw(int screenID, bool autoPlay, bool paused, bool inSettings = false);

/**
 * @brief Get the status-bar button x-positions for touch hit-testing.
 *
 * Left-to-right on the right side: play, circle, wifi, cog. Values are the
 * left edge (in pixels) of each button slot, valid after the last uiDraw.
 * @param[out] playX  Play/Pause button x.
 * @param[out] circX  Circle (jump-to-screen) button x.
 * @param[out] wifiX  WiFi bars x.
 * @param[out] cogX   Settings cog x.
 */
void getStatusBarLayout(int& playX, int& circX, int& wifiX, int& cogX);

/** @brief Open the jump-to-screen picker menu (Circle button). */
void showScreenMenu();
/** @brief Close the jump-to-screen picker menu. */
void hideScreenMenu();
/** @brief Whether the jump-to-screen picker menu is currently open. */
bool screenMenuVisible();

/**
 * @brief Hit-test a tap against the open screen-picker menu.
 * @param x  Touch x in screen pixels.
 * @param y  Touch y in screen pixels.
 * @return The tapped screen's ID (a SCR_* value), or -1 if the tap missed every
 *         cell (in which case the caller closes the menu).
 */
int  screenMenuHitTest(int x, int y);

/**
 * @brief Show the WiFi info overlay (tapping the status-bar WiFi bars).
 * @param ssid  Connected network name.
 * @param ip    Device IP address string.
 * @param mac   Device MAC address string.
 * @param rssi  Signal strength in dBm.
 */
void showWifiOverlay(const char* ssid, const char* ip, const char* mac, int rssi);
/** @brief Hide the WiFi info overlay. */
void hideWifiOverlay();
/** @brief Whether the WiFi info overlay is currently shown. */
bool wifiOverlayVisible();

/**
 * @brief Show the full-text popup (e.g. a complete, tapped news headline).
 *
 * Auto-closes after 30 s; only drawn on the BBC / Apple news screens.
 * @param text  NUL-terminated text to display (copied into an internal buffer).
 */
void showTextPopup(const char* text);
/** @brief Hide the full-text popup immediately. */
void hideTextPopup();
/** @brief Whether the full-text popup is currently visible (and not expired). */
bool textPopupVisible();

/**
 * @brief Show a brief bottom-centre toast message (~2.5 s).
 * @param msg  NUL-terminated message (e.g. a screenshot confirmation).
 */
void showToast(const char* msg);

/**
 * @brief Whether a touch is in progress this frame.
 *
 * Lets a screen's draw function react to an in-progress touch (e.g. dragging a
 * scrollbar thumb) rather than only the final tap/swipe reported on finger-up.
 * @return true if the finger is currently down, else false.
 */
bool    touchIsActive();
/** @brief Current touch x (screen pixels) while a touch is active. */
int32_t touchCurrentX();
/** @brief Current touch y (screen pixels) while a touch is active. */
int32_t touchCurrentY();
