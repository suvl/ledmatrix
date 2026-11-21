---
type: grilling
status: closed
---

## Resolution (final for now, 21 Nov 2026)

**Arduino/C++ with Adafruit Protomatter (arduino-esp32 3.3.x).** The panel is drawn by a task pinned to core 1, networking and the web portal run in tasks on core 0, and data comes from one HTTPS endpoint on the owner's own cloud API. Chosen after ticket 011 (see `research/firmware-options.md`), because the scope (three switchable features plus an on-device portal, a 1 s display budget and a 500 ms data cycle) needs real concurrency and CircuitPython has no threads.

The owner's reason for Rust was safety and fewer bugs. Acknowledged: Rust removes memory-safety bug classes, which matters for a device that parses untrusted network input. Against that: most of this project's bugs will be integration and timing bugs, and the Rust stack would mean far more hand-written glue on beta crates. Rust stays parked, not rejected. Revisit if the Protomatter plus Wi-Fi flicker milestone fails.

Mitigations to carry over to the C++ build because of that concern: compile with `-Wall -Wextra`, run `cppcheck` or `clang-tidy`, keep tasks talking through queues with no shared mutable state, bounded buffers only, hardware watchdog on, and test the JSON filtering and settings logic on the laptop off the board.

First milestone: ticket 012.

---

## History (earlier reopenings, kept for context)

**Reopened a second time, 21 Nov 2026.** The scope grew to three switchable features (flights polled every 30 s, weather, headlines) plus the portal, with a 1 s display budget and a 500 ms data-cycle cap. At 30 s polling every fetch is a cold DNS and TLS connection, about 4.4 s measured on CircuitPython, and one blocking loop cannot hide that. The tripwire below is effectively tripped by the requirements. Candidates to compare in ticket 011: Arduino/C++ with Adafruit Protomatter, Rust with embassy, CircuitPython plus a relay. CircuitPython stays on the board until a choice is made.

## Previous resolution (retaken 21 Nov 2026, superseded)

**CircuitPython, for now. Rust is parked, not rejected.**

Why: research ticket 008 says Rust is "ready with caveats" but would mean hand-building about half the infrastructure on beta crates, with an unresolved Wi-Fi/panel flicker report. Ticket 009 measured CircuitPython on the real board with the antenna attached: portal, mDNS, Wi-Fi, HTTPS fetch and panel together, no flicker, 1.9 MB heap free, 15 of 15 fetches succeeded, stalls 0.3 to 0.7 s (4.4 s for the first fetch).

The owner is **not fully convinced**: the status line and counter still pause for about 1 to 2 s now and then, and more features will add load. That concern is legitimate. The counter in the test only changes once a second, so a 0.4 to 0.7 s stall can look like 1 to 2 s on the panel, but it is a real blocking cost and the loop has no way to avoid it.

**Tripwire to reopen Rust:** once the real features are in (portal, fetch at the real interval, scrolling), if any visible freeze longer than 1 s remains in normal operation after the cheap mitigations (fetch less often, short timeouts, no in-loop retries, keep-alive connection, a fixed-height render step), or the heap falls under about 500 KB with everything running, reopen this decision and prototype the Rust panel + Wi-Fi coexistence first (see `research/rust-feasibility.md`).

**Reopened 21 Nov 2026.** The first resolution (below) assumed "an API plus some pixels". The requirements grew into an appliance: on-device web portal (LAN only, mDNS name), first-boot Wi-Fi access point, persistent settings, map-based location, radius and altitude filters. Decide again from evidence in tickets 008 and 009.

**Decision rule (anti-over-engineering):** stay on CircuitPython if 009 shows portal + panel refresh + flight fetch run together with no visible stalls and enough free memory. Switch to Rust only if 008 shows the S3 HUB75 driver and the Wi-Fi/HTTP/mDNS stack are mature enough, and 009 shows CircuitPython actually falls short. When in doubt, the cheaper path (CircuitPython) wins.

### Original resolution (superseded until the decision is retaken)

## Question

CircuitPython or Rust for the S3 firmware?

## Resolution

CircuitPython (owner chose it after the comparison).

- The job is HTTPS requests every few seconds plus JSON parsing, so it's network-bound, not CPU-bound.
- CircuitPython's HUB75 driver (Protomatter) is C, so refresh and flicker don't depend on Python.
- Iteration is fast: save a file on `CIRCUITPY` and it reloads. Adafruit libraries cover Wi-Fi, requests and fonts.
- Rust would win on memory (stream-parsing big JSON) and robustness, but the S3 Rust ecosystem (`esp-hal`, `esp-hub75`, `embassy`) is younger and I did not verify HUB75 support.
- Trigger to revisit: `MemoryError` when parsing an API response.
