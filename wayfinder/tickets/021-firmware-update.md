---
type: grilling
status: open
---

## Question

Firmware updates (OTA): how does new firmware reach a board that is mounted and not near a USB cable, and how is that made safe on a network with no portal password?

## Notes (22-24 Nov 2026)

Facts gathered:
- The board has two application slots of 2 MB (partition scheme "TinyUF2 8MB", 2 MB app slots); the firmware is 1.35 MB today, so roughly 650 KB of headroom remains. Growth past 2 MB would need a different scheme (a "no OTA" scheme gives 4 MB but one slot).
- arduino-esp32 ships `Update`/`ArduinoOTA`/`HTTPUpdate`; rollback needs the bootloader's app-rollback option or a manual validity check after boot.
- The portal has no password (LAN only). An unauthenticated upload page would let anyone on the LAN replace the firmware, so the design must answer that first.

Decisions pending: upload a `.bin` through the portal vs. pull from a URL (for example a GitHub release); how a flash is authorised (portal password, a device-specific update key, or signed images); whether a failed boot rolls back automatically; how the version is shown on the portal.

### Decided so far (owner, 24 Nov 2026)
- **Main path: the board pulls the update from a URL.** The portal shows "new firmware available" in a highlighted place, with the version, and an Update button.
- **Escape route: forced update from a `.bin` file** uploaded through the portal, for when the normal path is broken (bad release, no internet, a bad certificate store). Both paths get built.

Still to decide: who may trigger an update (the portal has no password), whether images are signed and verified, where the board looks for releases and how often, what a failed update does (rollback), and how the version is named and shown.

### Decided: who may start an update (owner, 24 Nov 2026)
Pressing Update on the portal (either path: pull or `.bin` upload) makes the panel show a 6-digit code that must be typed into the portal to continue. It proves someone is in front of the device, like the setup Wi-Fi password. No stored update password. Build notes: the code is random per attempt, expires after a few minutes, allows a few wrong tries, and the panel returns to its normal screen afterwards.

### Decided: image signing (owner, 24 Nov 2026)
**No signature for now.** The board downloads over HTTPS with the normal certificate checks and verifies a SHA-256 checksum published with the release (and the image must be a valid ESP32 app image for this board). It trusts the owner's GitHub account and repository. Signing (CI key or offline key) can be added later without changing the update flow. The forced `.bin` upload accepts any valid image after the panel code.

### Decided: where releases are found (owner, 24 Nov 2026)
**Through the owner's service.** The service watches a GitHub repository's latest release (the repository is a service setting, so others point it at their own) and gives the board a tiny answer: version, download address, SHA-256 and size. The board talks only to the one host it already trusts. The download is proxied or redirected by the service. If the service is down the notice does not appear; the forced `.bin` upload still works.

Defaults chosen without asking, owner may object: the board checks once a day (plus a "Check now" button on the portal); the version is a semantic version number set at build time and shown on the portal and in `/status`; only a newer version is offered; the highlighted notice shows the version and the release notes' first lines.
