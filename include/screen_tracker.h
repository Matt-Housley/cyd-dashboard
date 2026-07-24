#pragma once

/**
 * @brief Render the Stocks Tracker screen into `spr`.
 *
 * Shows the configured symbol's current price, change, and a history
 * sparkline from the cached g_tracker data. Draws directly, returns nothing.
 */
void drawScreenTracker();
