#pragma once

/**
 * @brief Render the DX Spots screen into `spr`.
 *
 * Shows recent DX cluster spots (callsign, frequency, spotter, bearing) from
 * the cached g_dxSpots data. Draws directly and returns nothing.
 */
void drawScreenDXSpots();

/**
 * @brief Look up the country/entity name for an amateur callsign.
 *
 * Matches the callsign prefix against the built-in DXCC prefix table.
 * @param call  NUL-terminated callsign (e.g. "G4ABC"). Not modified.
 * @return Country name (e.g. "England"), or an empty string "" if the prefix
 *         is not recognised. The returned pointer is a static string literal.
 */
const char* callCountry(const char* call);
