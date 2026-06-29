#pragma once

// Draws the base world map: ocean day/night fill, grid lines, continents,
// night stipple, terminator, sun dot, and QTH marker.
// Called by both drawScreenGreyLine() and drawScreenPSKReporter().
void drawWorldMap();

// Zoomed variant: shows only the lon/lat window specified.
// lonMin/lonMax in [-180,180], latMin/latMax in [-90,90].
void drawWorldMapZoomed(float lonMin, float lonMax, float latMin, float latMax);

void drawScreenGreyLine();
