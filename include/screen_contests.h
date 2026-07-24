#pragma once
#include <stdint.h>

/**
 * @brief Render the Contests screen into `spr`.
 *
 * Lists active and upcoming amateur-radio contests from the cached g_contests
 * data; a contest can be tapped for a detail view. Draws directly, no return.
 */
void drawScreenContests();

/**
 * @brief Handle a tap on the Contests screen (called on finger-up).
 * @param x  Touch x in screen pixels.
 * @param y  Touch y in screen pixels.
 * @return true if the tap selected a contest or dismissed the detail view,
 *         false otherwise.
 */
bool contestTouchUp(int32_t x, int32_t y);

/**
 * @brief Clear any active contest selection/detail view.
 *
 * Called when navigating away so a stale selection does not persist.
 */
void contestClearSelection();

/**
 * @brief Whether a contest is currently selected (detail view open).
 * @return true if a selection is active (auto-cycling should pause), else false.
 */
bool contestHasSelection();
