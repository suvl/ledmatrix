# Map: Flight tracker on a 64×32 HUB75 panel

Tracker: local markdown (no git repo, no issue tracker). Tickets live in `tickets/`; a ticket is open until its file has a `## Resolution` section and `status: closed`.

## Destination

A Matrix Portal S3 driving a Waveshare 64×32 P4 panel that shows nearby flights live, fetched over Wi-Fi from a public flight-data API, powered from one 5V 6A supply. Firmware is Arduino/C++ (see Decisions so far). Planned board features, each switchable on the portal: (1) nearby flights, polled every 30 s; (2) weather for the owner's location, shown when no new plane is passing; (3) top headlines from owner-set queries, cycled about every 30 minutes when there is a new headline and no new plane is passing. The panel may never go more than 1 s without updating, and each data cycle (DNS, TLS, request, response, parsing) must finish within 500 ms. Boot is exempt.

## Notes

- Hardware: Adafruit Matrix Portal S3 (#6475, ESP32-S3-WROOM-1U, 8 MB flash), Waveshare 64×32 P4 panel (256×128 mm), Velain 5V 6A supply, KUOQIY DC jacks, Greluma DC→USB-C adapter, VGOL patch antenna. Wiring: `wiring-rev-a.png` plan (REV A, 16 Nov 2026), not stored here.
- Effort override: **this map carries execution.** The owner asked to build as we go, so code lands in `~/ledmatrix/circuitpython/` alongside the decisions.
- Never connect the laptop USB and the Greluma to the S3 at the same time.
- Secrets (Wi-Fi) go in `settings.toml` on the board only. Only `settings.toml.example` is kept here.

## Decisions so far

- [Firmware language](tickets/001-firmware-language.md): **decided: Arduino/C++ with Adafruit Protomatter, render task on core 1 and network tasks on core 0 (21 Nov 2026).** Rust parked, CircuitPython retired for this build. Why, in short: the flights + weather + headlines + toggles scope, 30 s polling (cold TLS each time, about 4.4 s measured) and the 500 ms cap trip the tripwire below. Candidates: Arduino/C++ with Adafruit Protomatter on FreeRTOS, Rust with embassy, or CircuitPython plus a relay. Previous status: CircuitPython 10.3.2 for now, Rust parked. Measured on the real board: no flicker, 1.9 MB free, stalls under 1 s with the antenna on. Owner not fully convinced, so the ticket has a tripwire (a freeze over 1 s in the real app, or heap under about 500 KB) that reopens Rust. Evidence: [Rust feasibility](tickets/008-rust-feasibility.md) (ready with caveats), [CircuitPython budget](tickets/009-circuitpython-budget.md).
- [Power and wiring architecture](tickets/002-power-wiring.md): the panel runs from its own cable on the 6A supply. The S3 runs from a separate 20 cm lead through the Greluma. Screw posts and HUB75 OUT unused. Data goes over the ribbon cable to HUB75 IN: the IN header is unkeyed, and seating the S3 rotated 180° garbles the display. Measure 5.0–5.2 V at the Greluma before connecting.
- [Flashing method](tickets/003-flashing-method.md): esptool in the ROM bootloader (hold DOWN, tap RESET), full erase, write the `.bin` at 0x0, then tap RESET alone. The demo firmware was replaced.
- [Dev loop: panel first, simulator optional](tickets/004-dev-loop.md): edit on the laptop, `deploy.sh` copies to `CIRCUITPY`. A desktop displayio renderer is optional later.

- [Stall budget](tickets/010-stall-budget.md): the panel may never go more than 1 second without updating, total. The first fetch after boot (4.4 s) and failures (5 to 25 s) don't fit as measured; boot/reconnect exemption still open. Measure again once real features exist.

- [Setup trigger and device name](tickets/013-setup-trigger-and-name.md): a long press on the UP button (not DOWN, which is the boot pin) wipes the Wi-Fi credentials only and starts setup mode; the setup network and the mDNS name are both `ledmatrix` (`ledmatrix.local`). The setup network is WPA2 with the password shown on the panel.

- [When a plane shows](tickets/014-when-a-plane-shows.md): each plane is shown once, when it enters the radar, for a duration set on the portal (default 10 s). A plane always takes priority over weather and headlines. Layout modeled on theflightwall.com, adapted to 64×32.

- [Display layouts](tickets/006-display-layout.md): picked from the preview page. Flights A/B/C, weather A/B/C and headlines A/B are all available and selectable on the portal. Idle is the clock or a black screen. Setup mode uses layout B. All four status screens kept. One active layout per screen, picked on the portal, which reuses the preview page's renderer.

## Build progress

- **Slice 1, screens on the real panel** (`arduino/ledmatrix/`): all 15 screens and layouts drawn from example data, ported from the preview page with the same fonts. UP/DOWN buttons step through them, and the demo advances every 8 s. Findings: the 4×6 font's degree sign is disabled in the library header, so degrees are drawn by hand; "American" is cut to "America" beside a logo (7 characters fit); flight layout C's telemetry lines are 1 to 2 characters too wide.

## Not yet specified

- Wi-Fi/time-of-fetch behaviour: reconnect, retry, offline screen.
- Smoothness: scroll vs page flip, brightness, night dimming.
- Panel orientation and mounting, enclosure, antenna placement.
- Memory strategy if the API payload is large.
- How we guarantee the 1 s budget (fetch interval, timeouts, keep-alive, a loop that never blocks for long).
- Portal security: LAN-only means anyone on the Wi-Fi can change settings. Password or not?
- Random versus fixed setup password, the press length, a separate full factory reset.
- Idle clock timezone and 12/24 h.
- Where logos, airline/route/aircraft names and airport names come from (ticket 005), and how logos reach the device (16×16 bitmaps from the cloud API vs stored on the device).
- The portal's own layout: pages and fields (map, radius, altitude, dwell time, layouts, toggles, headline queries, Wi-Fi).
- Settings persistence layout (NVS keys, versioning).
- Map tiles need the visitor's browser to have internet, which it won't while on the setup access point.

## Out of scope

- Chaining a second panel (HUB75 OUT).
- Rewriting in Rust.
