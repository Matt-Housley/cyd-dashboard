# v1.18.032

A stability-focused release. The headline: the device now stays up for **many hours** instead of rebooting every 15–20 minutes, and it recovers gracefully from the long-uptime memory fragmentation that used to wedge its HTTPS fetches.

## 🩹 Reliability — no more fragmentation reboots
Over a long run, heap fragmentation would starve the TLS handshake: plain-HTTP sources kept working, but every HTTPS source failed with `SSL - Memory allocation failed`, and eventually the device rebooted. This is fixed on several fronts:
- **News thumbnails** are now decoded into a sprite that's allocated **once and reused**, instead of allocating/freeing ~15 KB on every new hero image — a leading source of the churn.
- A **pre-flight guard** skips a TLS handshake (rather than churning failed allocations) when there isn't enough contiguous heap, and only reboots after *sustained* starvation.
- The **SSL memory reserves recover gracefully** — the critical one is reclaimed first, and a one-off miss is ridden out for a few cycles instead of triggering an immediate reboot.
- The reserve is **sized with headroom** so it reliably clears the TLS buffer threshold on its own.

Result: uptime went from a reboot every ~15–20 min to hours on end.

## 🌞 Solar screen fixed
The HF Conditions / Propagation data (from hamqsl) is the most memory-hungry fetch and was consistently failing the handshake (`BIGNUM - Memory allocation failed`) at tight moments. Its buffer is now allocated **after** the TLS handshake, freeing that memory for the certificate verify — so solar stays green.

## 📡 ISS "Pass" LED — now means *contactable*
The onboard green LED now flashes whenever the **ISS is above your horizon** (a radio pass), day or night — matching what you actually want for working the ISS — instead of only when it's naked-eye visible (sunlit + dark sky). The setting is renamed **"ISS Visible LED" → "ISS Pass LED"**, and the LED now agrees with the ISS Tracker's "VISIBLE NOW" card and the automatic jump to that screen.

## 🤝 Politer to PSK Reporter
PSK Reporter and FT8 Spots now poll every **5 minutes** (was 2). Both hit the same rate-limited endpoint (`retrieve.pskreporter.info`), and the old cadence got the device temporarily blocked with `HTTP 503`. Occasional 503s can still happen when their server is busy — that's external — but the device now stays a well-behaved client and recovers on its own.

## 🧾 Cleaner logs
Serial fetch logs now only show an SSL error string for genuine connection failures — a real server response like `HTTP 503` no longer prints a misleading, stale `CTR_DRBG`/SSL string.

---
**Building/flashing:** compile and upload with PlatformIO — see the [README](README.md) (note the one-time `setuptools<81` fix). No prebuilt binary is attached.
