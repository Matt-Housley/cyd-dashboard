# v1.18.033

A small feature release: the **FT8 Spots** screen can now show **FT4** spots instead — and it renames itself to match.

## 📡 FT8 / FT4 mode switch
The FTx Spots screen gained a mode selector. **Tap the band legend** to open the options overlay (now titled **"FTx Options"**), where a new **Mode [FT8] [FT4]** toggle sits above the per-band filter.

- The **screen name follows the mode** — the status bar and the jump-to-screen picker read **"FT8 Spots"** or **"FT4 Spots"** to match your choice. The generic term **"FTx"** is used only where the mode is ambiguous (the options overlay title).
- The **PSK Reporter query is filtered** to the selected mode (`mode=FT8` / `mode=FT4`), so the map shows genuine FT4 activity rather than everything lumped together.
- Switching mode **clears the stale spots and refetches straight away** — you don't wait out the 5-minute refresh cycle — via a new force-fetch path that mirrors the existing weather/tracker one.
- Your choice **persists across reboots** alongside the per-band filter.

The band filter, furthest/loudest markers, auto-zoom, and tap-to-info overlay all work exactly as before, now against whichever mode is selected.

---
**Building/flashing:** compile and upload with PlatformIO — see the [README](README.md) (note the one-time `setuptools<81` fix). No prebuilt binary is attached.
