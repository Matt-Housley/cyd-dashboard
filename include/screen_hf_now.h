#pragma once
#include <stdint.h>

/**
 * @brief Render the "HF Now" band-conditions screen into `spr`.
 *
 * Shows current real-time HF band openings; a band can be tapped to show a
 * detail overlay (see hfNowTouchUp). Draws directly and returns nothing.
 */
void drawScreenHFNow();

/**
 * @brief Whether a band detail is currently selected/expanded on this screen.
 * @return true if an overlay/selection is active (auto-cycling should pause),
 *         false otherwise.
 */
bool hfNowHasSelection();

/**
 * @brief Handle a tap on the HF Now screen (called on finger-up).
 * @param x  Touch x in screen pixels (0..SCREEN_W-1).
 * @param y  Touch y in screen pixels (0..SCREEN_H-1).
 * @return true if the tap was consumed (a band was selected or an overlay
 *         dismissed), false if it hit nothing interactive.
 */
bool hfNowTouchUp(int32_t x, int32_t y);
