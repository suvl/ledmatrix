---
type: research
status: closed
---

## Question

Which firmware stack can keep the panel updating within 1 s while a network call (cold DNS + TLS, up to several seconds) runs in the background? Compare Arduino/C++ (Adafruit Protomatter, FreeRTOS tasks), Rust (embassy), and CircuitPython plus a relay. Judged on: panel/Wi-Fi coexistence (flicker), true concurrency, access-point provisioning, mDNS, on-device web server, streaming JSON for a large headlines feed, realistic cold TLS handshake time on the S3, maturity, and effort. Findings go in `wayfinder/research/firmware-options.md`.

## Resolution

Research recommends **Arduino/C++ (arduino-esp32 3.3.x, Adafruit Protomatter), render task pinned to core 1, networking tasks on core 0, data from one owner-run HTTPS endpoint (about 1 KB merged JSON), MQTT/WebSocket push optional later.** Reasons: tasks keep a slow TLS call from stalling the panel; Protomatter is officially tested on this board; every portal part exists without beta churn. CircuitPython plus a cloud API is rated partial, not safe: no threads, reconnects block for the whole handshake, and the portal, mDNS and AP+STA share the one loop. Rust is ready with caveats (more glue code, unresolved flicker report).

Open and unverified, per the research: cold TLS handshake time on the S3 (no primary benchmark found; the 4.4 s figure includes DNS and connection setup), RSA vs ECDSA speed, session resumption, Wi-Fi/flash-write interference with Protomatter's timer-ISR refresh (issues 76 and 71; a single 64x32 panel is the lightest case). Flash writes can blank the display, so stop and resume the matrix around settings writes. The recommended first milestone is a several-hour run on the real board: animation at the intended bit depth, plus a core-0 task doing a cold HTTPS fetch every 30 s, an NVS write and portal requests, recording ghosting, dropped frames and handshake time (RSA vs ECDSA). The choice itself is the owner's, recorded in ticket 001.
