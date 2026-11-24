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
