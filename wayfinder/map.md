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

- [Display layouts](tickets/006-display-layout.md): picked from the preview page. Flights A/B/C, weather A/B/C and headlines A/B are all available and selectable on the portal. Idle is the clock or a black screen. Setup mode is one static screen (network name, big password, address): the first cycling version was too fast to type from. All four status screens kept. One active layout per screen, picked on the portal, which reuses the preview page's renderer.

- [Dual stack](tickets/015-dual-stack.md): IPv4 and IPv6 everywhere. Inbound verified on the board (link-local and global IPv6, mDNS publishes it); outbound fetches over both families still to verify in slice 4.
- [Portal location picker](tickets/016-portal-location-picker.md): map with a draggable pin and radius circle, plus a place search (OpenStreetMap) and latitude/longitude fields as a fallback.

- [Flight, weather and headline sources](tickets/005-flight-data-source.md): positions adsb.fi, routes adsbdb, weather IPMA (with its hourly public-data feed), headlines Google News RSS, all fetched by the owner's service (tickets [017](tickets/017-weather-data-source.md), [018](tickets/018-headlines-data-source.md)). A normal CA store on the board: [ticket 019](tickets/019-tls-trust-store.md). The service lives at `apis.example.com` on the owner's k3s: [ticket 020](tickets/020-cloud-service-hosting.md).

## Build progress

- **Slice 1, screens on the real panel** (`arduino/ledmatrix/`): all 15 screens and layouts drawn from example data, ported from the preview page with the same fonts. UP/DOWN buttons step through them, and the demo advances every 8 s. Findings: the 4×6 font's degree sign is disabled in the library header, so degrees are drawn by hand; "American" is cut to "America" beside a logo (7 characters fit); flight layout C's telemetry lines are 1 to 2 characters too wide.

- **Slice 2, Wi-Fi setup flow** (`arduino/ledmatrix/net.cpp`, `settings.cpp`): tested end to end by the owner on a phone, and it works. Boots into setup mode when no network is saved: WPA2 access point `ledmatrix` with a random password, captive page, form with a network list. Hold UP for 5 s (amber progress bar) forgets the network. Findings: the cycling setup screen was too fast to type from, so it is now one static screen; `ledmatrix.local` took exactly 5 s to resolve on macOS until the board got an IPv6 link-local address (`WiFi.enableIPv6()`), now 6 to 15 ms. Known weakness: the form does not verify the password before saving.

- **Slice 3, settings portal** (`portal/`, `shared/`, `tools/`, `arduino/ledmatrix/settings.cpp`, `net.cpp`): the portal is a single page served by the board at `http://ledmatrix.local`. It edits what to show, one layout per screen (thumbnails drawn by the same renderer as the preview page), how long a plane stays (3 to 60 s, default 10), the location on a map with a draggable pin, radius circle, place search and coordinate fields, altitude limits, headline topics, 12/24 h clock, and has a forget-Wi-Fi button. Settings are validated on the device and kept in NVS. Tested with curl: defaults, valid save, four kinds of invalid save (400 with a clear message), persistence across a reboot, reading over IPv6 alone, and the playlist following the saved layouts and dwell. Not yet checked by a human in a browser. The portal has no password (LAN only); the map and place search need the visitor's browser to have internet. `tools/build_web.py` regenerates `preview/index.html` and `arduino/ledmatrix/portal_page.h` from the templates and the shared renderer.

- **ledmatrix-api service** (`service/`, Go 1.25, standard library only): `/v1/weather`, `/v1/flights`, `/v1/headlines`, `/healthz`. Unit tests use a fake upstream serving the real captured responses; a live run against the real servers returned weather in 155 bytes (195 ms, 2 ms cached), three planes with routes in 661 bytes, and headlines in 416 bytes. Image 16.7 MB (distroless). Manifests (Deployment, Service, HTTPRoute) pass a server-side dry run on the cluster. **Deployed** on the owner's k3s cluster (21 Nov 2026): Deployment, Service and HTTPRoute in `default`, served over HTTPS with the existing wildcard certificate (RSA-2048, TLS 1.3, verified), manifests kept in the owner's `k8s/ledmatrix-api/` folder (not committed there). Live checks over HTTPS: weather 155 bytes in 133 ms, flights and headlines answer in about 100 ms. The cluster pulls the CI-built image `ghcr.io/suvl/ledmatrix-api`, pinned to the commit it was built from (an update is a deliberate manifest change). It first ran from an amd64 image loaded onto the node by hand, because the package was private for a while (the registry refused an anonymous token until the owner made it public); that image has been removed. Found and fixed on the way: a non-numeric image user made the pod fail with `CreateContainerConfigError`. No AAAA record yet, so the board-to-service link is IPv4 only for now.

- **Slice 4, live data** (`arduino/ledmatrix/live.cpp`, `plane_view.h`, `screens.cpp`, `ledmatrix.ino`, portal "Data service" section): the board fetches flights every 30 s, weather every 15 min and headlines every 30 min from the owner's service, in a task on core 0, so a slow request never touches the panel. A plane is shown once when it first enters the radar for the dwell time set on the portal (default 10 s) and not again for 30 minutes; a new headline is shown once; between those it shows the weather, or the clock when weather is off or not yet loaded; a blinking dot means data is stale. Uses the standard CA bundle, a synchronised clock, and a connection routine that resolves both IP families and falls back (verified: IPv4-only, IPv6-only and dual-stack hosts all reached; the fallback path itself is untested). Found and fixed: enabling IPv6 let the router announce a link-local DNS server that lwIP cannot use, so all lookups failed until a DNS guard was added. With no service address saved the device shows the demo screens, so a fresh device is useful before configuration. The service address, location and topics are entered on the portal.
- **Slice 5, airline logos** (`service/logos.go`, `tools/make_logos.py`, `live.cpp`, `art.cpp`): the service serves 16x16 RGB565 logos (512 bytes) from a mounted folder; the logo files are git-ignored and live on the owner's cluster as a ConfigMap (157 airlines, about 80 KB), never in the repository or the public image. The board fetches a logo in the data task before the plane that needs it is queued (so the panel never waits), keeps 16 in RAM, remembers a 404 for an hour, and draws the airline code in a coloured box when there is no logo. Artwork source: converted from `images.kiwi.com/airlines`, for this personal device only; their copyright stays with the airlines. Adding an airline: add its code to `tools/make_logos.py`, rebuild, update the ConfigMap (no firmware or image change).
- **Brightness** (`ledmatrix.ino`, portal "Display" section): a slider from 10 to 100 percent, saved with the other settings. Protomatter has no dimming call, so the finished frame is scaled before it is sent; the panel now runs at 6 bits per colour (was 4) so dimmed pictures keep their shades. Tested at 30 percent on the real panel.

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
