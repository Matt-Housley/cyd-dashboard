#pragma once
#include <time.h>

/**
 * @brief Draw the base world map into `spr` at full extent.
 *
 * Renders ocean day/night fill, grid lines, continents, night stipple, the
 * solar terminator, the sub-solar dot, and the QTH marker. Shared by
 * drawScreenGreyLine() and drawScreenPSKReporter().
 */
void drawWorldMap();

/**
 * @brief Draw the world map zoomed to a lon/lat window.
 * @param lonMin  Western edge, degrees in [-180, 180].
 * @param lonMax  Eastern edge, degrees in [-180, 180] (> lonMin).
 * @param latMin  Southern edge, degrees in [-90, 90].
 * @param latMax  Northern edge, degrees in [-90, 90] (> latMin).
 */
void drawWorldMapZoomed(float lonMin, float lonMax, float latMin, float latMax);

/**
 * @brief Render the Grey Line / ISS Tracker screen into `spr`.
 *
 * Draws the world map with the day/night terminator, the ISS ground track and
 * marker, and (during a pass) a "next pass" / "ISS visible now" info panel with
 * Doppler-corrected frequencies. Draws directly and returns nothing.
 */
void drawScreenGreyLine();

/**
 * @brief Test whether the ISS is optically visible from an observer.
 *
 * Visible means the ISS is sunlit while the observer's sky is dark (sun below
 * the horizon) — matching the "Sunlit, dark sky / twilight" cases.
 * @param issLat  ISS sub-point latitude, degrees.
 * @param issLon  ISS sub-point longitude, degrees.
 * @param obsLat  Observer latitude, degrees.
 * @param obsLon  Observer longitude, degrees.
 * @param t       Time (UNIX/UTC) at which to evaluate.
 * @return true if the ISS is sunlit and the observer's sky is dark, else false.
 */
bool issOpticallyVisible(float issLat, float issLon,
                         float obsLat, float obsLon, time_t t);
