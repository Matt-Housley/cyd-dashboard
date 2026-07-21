#pragma once
#include <stdint.h>
void drawScreenFT8Spots();
void ft8ClearSelection();
// Returns true if the tap was consumed (overlay opened or dismissed, or a band
// highlighted in the legend), so the caller can extend the auto-play pause.
bool ft8TouchUp(int32_t x, int32_t y);
