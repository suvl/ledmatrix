---
type: grilling
status: closed
---

## Question

How does the device decide which HTTPS servers to trust?

## Resolution (owner, 21 Nov 2026)

**A normal CA store, like any computer, not per-host pinned roots.** The milestone sketch (`arduino/milestone/`) pinned one root certificate per test host only so the timings included real chain verification. Production does not do that.

Plan:
- Use the CA bundle that ships with the ESP-IDF libraries (the Mozilla root set at that release). arduino-esp32 3.3.12 and later expose it as `useBuiltinCACertBundle()`; the project pins 3.3.11, so slice 4 moves to the latest 3.3.x (3.3.13) and re-checks that Protomatter still renders cleanly.
- Never pin a leaf or an intermediate certificate. A host changing CA, or Let's Encrypt changing its chain, then needs no firmware change as long as the new root is in the bundle.
- The device needs the right time for certificate dates: SNTP must succeed before the first HTTPS request (slice 4 adds it anyway for the idle clock).
- The bundle is fixed at build time, so a firmware that is never updated eventually misses new roots (typical roots live 15 to 25 years, but a CA can change its hierarchy sooner). Mitigations: a firmware update path (OTA, a later slice and its own ticket), the portal showing the last TLS error with its reason, the panel showing the stale-data dot rather than failing silently, and the owner's own cloud service using a long-established widely trusted root.
- Insecure TLS (skipping verification) is never used, unlike the open-source FlightWall firmware.

Open: the firmware update mechanism (OTA from the portal, or from a URL), and whether to allow the owner to upload an extra CA certificate through the portal as an override.
