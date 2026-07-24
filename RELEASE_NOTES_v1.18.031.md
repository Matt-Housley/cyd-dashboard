# v1.18.031

Everything since **v1.18.000** — new amateur-radio features, an on-device unit-test suite, full source documentation, and an MIT license.

## ✨ New features
- **FT8 Spots band filter** — tap the band legend to toggle individual bands on/off; hidden bands drop out of the map and the choice persists across reboots.
- **Lightning alert** — when a thunderstorm is forecast within the hour, a red banner appears on the Weather screen and the onboard LED flashes red; tap the banner to silence it. During an ISS pass the LED alternates red/green.
- **ISS-visible LED** — the onboard LED flashes green when the ISS is optically visible overhead (sunlit ISS + dark local sky). Toggle in Settings.
- **Startup LED** — the onboard LED flashes blue while data loads at boot, stopping once every screen (including PSK Reporter and FT8 Spots) is populated. Toggle in Settings.
- **Configurable ISS frequencies** — set the SSTV / APRS / repeater in/out frequencies used by the ISS Tracker Doppler panel via a digit spinner (Settings → ISS Frequencies).
- **News headline popups** — tap a truncated BBC or Apple/Tech headline to read it in full; auto-closes after 30 s.
- **SD-card screenshots** — with Settings → Service enabled, a 3-second press saves a 24-bit BMP of the current screen to a microSD card. The Service menu can also stage a fake lightning alert or ISS pass for documentation shots.
- **Extended fonts** — £, €, ©, °, accented letters, and typographic punctuation (curly quotes, en/em dashes, ellipsis) now render as real glyphs across news, DX, and contest text instead of blank boxes.

## 🔧 Changes & improvements
- Status bar streamlined to **Play/Pause · Jump-to-screen (☰) · WiFi · Settings cog**; the old Rewind / Fast-Forward icons are gone (swipes and the jump menu cover navigation).
- Degree symbols now use the real ° glyph on the Weather and ISS Tracker screens.
- "Tracker" renamed **Stocks Tracker**, with an animated multi-year sparkline.
- PSK Reporter and FT8 Spots band legends now list bands high→low frequency.
- Settings polish — redrawn cog icon, right-justified unit selectors, alphabetical menu ordering.
- Removed the unreliable 2-second long-press for Settings (use the cog instead).

## 🐛 Fixes
- **Stock price formatting** — thousands separators were mis-placed (e.g. `5123.45` rendered as `,5123.45`); now correct (`5,123.45`). Caught by the new test suite.
- **SD screenshots now reliable** — resolved a chain of issues (VSPI bus contention, TLS memory starvation, byte-swapped colours) by bit-banging the touch controller and using the lean SdFat library on hardware VSPI.
- Minor layout fixes — the lightning banner and news popup no longer clip descenders.

## 🧰 Project
- **Full source documentation** — every function across the headers and sources now carries a Doxygen block (`@brief` / `@param` / `@return`).
- **Unit tests** — a PlatformIO/Unity suite runs on the ESP32 against the real firmware (`pio test -e test`), covering Maidenhead grid conversion, mode classification, timezone lookup, callsign→country, and price formatting.
- Now released under the **MIT license**.

---
**Building/flashing:** compile and upload with PlatformIO — see the [README](README.md) for setup (note the one-time `setuptools<81` fix). No prebuilt binary is attached.
