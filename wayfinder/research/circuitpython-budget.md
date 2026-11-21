# CircuitPython resource budget (ticket 009), partial

Measured 21 Nov 2026 on the actual board over the REPL, CircuitPython 10.3.2, with only the REPL running (no panel, no Wi-Fi connection).

| Item | Measured |
| --- | --- |
| Free Python heap | 2,071,472 bytes (about 2.0 MB) after `gc.collect()` |
| ESP-IDF heap total / free | 2,376,092 / 2,273,484 bytes |
| CPU | 240 MHz |
| NVM (persistent settings) | 8,192 bytes |
| Filesystem (`CIRCUITPY`) | 1 KB blocks: 5,796 total, 5,682 free (about 5.7 MB) |
| Built in | `mdns`, `wifi`, `espidf` import fine |
| Not built in | `asyncio` (needs the Adafruit library from the bundle) |

Reading: the heap size matches a 2 MB PSRAM module on top of internal RAM. Memory is not tiny for a microcontroller, but TLS, a web server and the display framebuffer all draw on it at once, so the margin under load is the open question.

## Stress test (21 Nov 2026)

Code: `circuitpython/stress/code.py`. One polling loop with: panel animation (20 fps), Wi-Fi station, access point (`flights-setup-test`), mDNS name `flights.local`, `adafruit_httpserver` on port 80, and an HTTPS fetch of a flight-data JSON (adsb.lol, arbitrary coordinates over central Europe, not the owner's location). I curl'd `/status` from the laptop during the run.

| Result | Value |
| --- | --- |
| Free heap at boot | 1,981,888 bytes |
| Free heap with everything running, idle | about 1.94 MB |
| Free heap after parsing a 195 KB payload (314 aircraft, 250 nm radius) | about 1.60 MB, no `MemoryError` (peak during the parse not sampled) |
| Station + access point together | both came up (AP 192.168.4.1). No client was connected to the AP in this test. |
| Portal reachable by IP | yes, 0.1 to 1.6 s per request |
| Portal reachable as `flights.local` | yes (first lookup took 2.4 s) |
| Realistic fetch, 30 nm radius (2 to 3 KB, 3 to 4 aircraft) | 0.3 to 0.6 s, after a first fetch of 4.4 s |
| Loop stall while fetching | 0.3 to 0.6 s typical, 4.4 s on the first fetch |
| Worst-case fetch, 195 KB payload | 7 to 12 s, during which the loop and the portal do not respond (two curl requests timed out) |
| Errors | 0 in both runs |

Reading:
- Memory is not the constraint. Even a 195 KB payload parsed whole fit with 1.6 MB to spare.
- The real cost is **blocking**. A fetch stops the loop (and the portal) for its whole duration. With a realistic radius this is under 1 s, apart from the first fetch (TLS handshake). With a huge radius it is 7 to 12 s.
- The panel stays lit during stalls (the refresh runs from an interrupt). The on-screen animation freezes. Whether Wi-Fi traffic makes the panel visibly flicker needs a human to look; the test cannot see it.
- Mitigations if the stalls matter: keep the radius small, let the portal respond between fetches, or move the fetch to an asyncio task with a non-blocking socket.

## Correction: the antenna (later the same day)

The runs above were made **with no antenna attached.** The module is an ESP32-S3-WROOM-**1U**, which has no onboard antenna, only the u.FL socket. The owner noticed the network errors and attached the VGOL patch antenna.

| Run | Fetches | Errors | Loop stalls |
| --- | --- | --- | --- |
| No antenna, access point on, 15 s timeout | 56 | 10 | 5 to 25 s, matching the timeout |
| No antenna, access point off, 4 s timeout | 13 | 4 | 5.5 to 11 s (DNS and connect phases do not honour the timeout, and `adafruit_requests` retries once) |
| **Antenna attached**, access point off, 4 s timeout | 15 | **0** | 0.3 to 0.7 s, plus 4.4 s on the first fetch |

Signal strength with the antenna: -58 to -60 dBm on channel 1. The laptop reaches the same API in 0.2 to 0.5 s every time, so the API was never the problem.

Conclusions:
- The long freezes the owner saw on the panel came from Wi-Fi errors with no antenna, not from CircuitPython itself.
- With the antenna, the single-threaded loop is acceptable: a fetch of a realistic radius stalls it for under a second.
- It stays fragile. A weak signal (or a failing API) would bring back 10 s or longer freezes, because DNS and connection setup block. Plan on short timeouts, no retries inside the loop, and a "last good data" display during failures.
- The first fetch after boot takes about 4.4 s (TLS handshake). Do it before showing flights, or behind a "connecting" screen.
- Caveat: one run of about 2 minutes, 15 fetches. Treat it as strong evidence, not proof.

## Not measured

- A client actually joining the access point and loading the portal over it.
- The peak memory during the large parse.
- Panel flicker under Wi-Fi traffic (visual).
- Settings persistence: NVM (8 KB, fine for a few numbers) versus the filesystem. Code can only write files when no computer has the drive mounted, so NVM is the safer default.
