# Firmware stack options for the Matrix Portal S3 appliance (2026-11-21)

Primary sources only (GitHub issues/repos/release pages, ESP-IDF docs, Adafruit Learn). Pages were read through a summarising fetch tool, so treat quotes as paraphrase. "unverified" means I could not confirm it. Rust details are in `rust-feasibility.md` and not repeated.

## Summary
- **Recommendation: Arduino/C++ (arduino-esp32 + Adafruit_Protomatter + FreeRTOS tasks), with a single owner-run HTTPS cloud endpoint for the data.** It is the only option where the panel refresh (timer ISR) and the network (own task, other core) are separate by construction, and where every portal component is shipped or mainstream.
- Biggest risk: Wi-Fi/flash-write interference with Protomatter's bit-banged timer-ISR refresh (issues #76 and #71, both still open). Evidence is mixed but a single 64x32 panel is the lightest case.
- CircuitPython stays blocked on one fact: no threads, so any cold TLS (4.4 s measured) or reconnect freezes the animation. A persistent connection removes it from steady state only.
- Rust: ready with caveats, but flicker issue #24 has had no activity since 2025-08-17 (checked today). More glue code for the owner.

## A. Arduino/C++ and Adafruit_Protomatter
- **Official support: works.** The README says the MatrixPortal S3 was used for hardware testing (128x64 FM6126A panel) and the `abc_panel` example supports "MatrixPortal M4 and S3". Some ESP32-S3-specific code paths exist.
- **Correction (checked in the installed library source, `src/arch/esp32-s3.h`, 21 Nov 2026): on the ESP32-S3, Protomatter uses the LCD_CAM peripheral with GDMA for the data blast, plus a timer. The "no DMA, no LCD_CAM" reading below came from the README and is wrong for the S3. CPU cost is lower than described, and the Wi-Fi interference question still stands.** Original note follows.
- **How it drives the panel: partial for my question.** It is "brute-force GPIO ... not relying on DMA or other specialized peripherals beyond a timer interrupt". No LCD_CAM, no DMA. The ESP32 build uses the legacy `hw_timer_t` API with IRAM-attributed helpers (`src/arch/esp32.h`, master, read today). The README says RAM use is not efficient and PSRAM is not mentioned. So refresh costs CPU in interrupt context.
- **Wi-Fi flicker: partial (known, mixed evidence).**
  - Issue #76 (opened 2024-04-26, open, last comment 2026-10-02): on Matrix Portal S3, ghost pixels/lines worse once on Wi-Fi + Adafruit IO. Setups: 2x chained 64x64. A single 64x32 unit "showed no problems". dhalbert (Adafruit) suspected Wi-Fi interrupts disturbing the data output. Lowering bit depth (to 3 or less) removed it for two users. One user went to ESP32-HUB75-MatrixPanel-DMA and the flicker went away.
  - Latest retest in #76 (2026-10-02, master 0f51ca2, one 64x64, 4-bit, double buffer): arduino-esp32 4.0.0-alpha1/IDF 6 completed 60 HTTP downloads with no stray pixels in sampled frames. 3.3.11/IDF 5.5.5 completed 40, then a timeout. The commenter says this did not reproduce the original two-panel workload.
  - Issue #71 (opened 2024-01-15, open): writing a file to FFat on S3 corrupts/blanks the display; reads and Preferences (NVS) writes did not trigger it in the original report. 2026 analysis: the timer ISR is deferred while flash cache is disabled (`GPTIMER_ISR_CACHE_SAFE` not enabled in the stock SDK). Workaround: `matrix.stop()` before the flash write, `matrix.resume()` after. This matters for saving settings.
  - Issue #56 (closed 2023-01-02): not a library bug (spotty Wi-Fi).
- **Concurrent Wi-Fi/TLS on another task: partial/unverified for this exact case.** The ISR keeps running while a task is blocked in TLS, since it is a hardware timer. The risks are interrupt latency and flash-cache stalls, not task blocking. I found no report of a TLS handshake specifically.
- **Core assignment (my inference, unverified):** arduino-esp32 runs the Wi-Fi/lwIP tasks on core 0 by default. Call `matrix.begin()` and render from a task pinned to core 1, so the timer interrupt is allocated on core 1 (interrupts are allocated on the calling core; not verified for Protomatter). Run the HTTP client and web server tasks on core 0. Keep the ISR code in IRAM and avoid flash writes during refresh (see #71).
- **Toolchain:** arduino-esp32 **4.0.0** is listed as the latest stable (release page, "19 Nov", on ESP-IDF 6.1.1); 3.3.13 (17 Nov, IDF 5.5.6+) is the latest 3.x. Protomatter was tested on 3.3.11 and 4.0.0-alpha1 only. Pin 3.3.x for the prototype. Protomatter issue #88 (open, 2026-01-16) asks for IDF-without-Arduino support. A PlatformIO board id for the MatrixPortal S3 was not found, and PlatformIO's bundled arduino-esp32 may lag. Arduino IDE lists "Adafruit MatrixPortal ESP32-S3" (Adafruit Learn setup guide). Use Arduino CLI or a pinned platform plus `board_build.variant` (unverified).

## B. Arduino building blocks
| Piece | Verdict | Evidence |
|---|---|---|
| Async HTTP server | works | ESP32Async/ESPAsyncWebServer fork: Arduino core 2.x and 3.x, WebSocket and SSE listed, 1,534 commits. No release version found (unverified). Plain HTTP only, which fits a LAN portal. `esp_http_server` ships with ESP-IDF (not re-checked). |
| mDNS | works (not re-checked) | ESPmDNS is part of arduino-esp32. |
| AP+STA provisioning | partial | WiFiManager README: version 1.0.1, ESP32 support "fairly stable", captive portal, custom parameters, but 606 open issues, docs "out of date", and "retaining AP/STA modes (BUGGY)". Hand-roll with `WIFI_AP_STA`, DNSServer and the async server instead. Estimated 150 lines (my estimate). ESP-IDF provisioning manager via Arduino `WiFiProv`: unverified. |
| Flash settings | works | Preferences/NVS. Beware #71: stop the matrix around writes to be safe. |
| Large JSON | partial | ArduinoJson v7 docs: filter works with a stream, but chunked parsing only for arrays. The filtered chunk must fit in RAM. A headline feed that is an array of items is fine. |
| HTTPS client | works | `NetworkClientSecure` (renamed WiFiClientSecure). Release notes: `useBuiltinCACertBundle` (3.3.12), CA bundle kept after stop (3.3.9), `setCiphers` (3.3.13/4.0.0). `esp_http_client` with the cert bundle is the IDF alternative (unverified in Arduino). |

## C. Cold TLS handshake time
- **I found no primary benchmark of a full handshake on ESP32-S3.** Only: a third-party CycloneCRYPTO table (not mbedtls), a 2021 forum post saying `mbedtls_ecdsa_read_signature` took about 900 ms on a 160 MHz ESP32, a 2017 Espressif staff reply that ECC hardware gains are modest, and a blog with 4-6 ms figures that I judge implausible. ESP-IDF docs recommend enabling hardware AES, SHA and MPI (bignum) acceleration.
- **Is 4.4 s typical? Unverified, and I think it is not a pure handshake figure.** It includes DNS and Wi-Fi/socket setup, so the handshake share is unknown. My expectation (not evidence) is roughly 1 s or less for RSA-2048 with MPI acceleration. ECDSA chain verification may be slower than RSA on the S3 because I could not confirm an ECC accelerator on it (some newer chips have one). Measure both before choosing a certificate type. A short chain (leaf only, pinned CA) cuts signature checks.
- **Session resumption:** ESP-IDF documents `CONFIG_MBEDTLS_CLIENT_SSL_SESSION_TICKETS` and `CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS`. Whether the Arduino prebuilt libs enable them: unverified. CircuitPython: no evidence in its `ssl` docs. Do not plan around it.
- **Rust (mbedtls-rs 0.3.0):** the repo page shows no benchmark or hardware-acceleration statement, and resumption is not mentioned. Unverified.

## D. Rust delta
- **Embassy network task alongside esp-hub75 DMA: partial.** The driver is DMA/LCD_CAM, so CPU use is low (README quote in the earlier report). I found no new test of a TLS task concurrent with rendering.
- **Issue #24:** state today is closed (2025-08-14), last comment 2025-08-17 saying the `iram` fix did not solve it. No later comments, linked PRs or labels. The ISR refactor in 2026-09 is not tied to it. **No new evidence.**

## E. Cloud API owned by the owner (revised)
1. **One host, ~1 KB JSON, ETag/304:** a 304 or tiny body is a few hundred bytes, so cost on a kept-alive link is the known 0.3-0.6 s round trip, mostly network and server latency (CircuitPython figure given by the requester). Cold handshake: certificate type matters only through chain verification (see C). Pick the shortest chain and measure RSA vs ECDSA. Resumption is unverified on all stacks. A cold handshake each 30 s is not safe for CircuitPython (4.4 s) and is unproven for Arduino, but harmless there because it runs in its own task.
2. **Persistent connection, push updates:**
   - *CircuitPython:* `adafruit_minimqtt` `loop(timeout=1.0)` is documented as "non-blocking" but is only approximate. It raises `ValueError` if `timeout < socket_timeout` (default 1 s). Each wait can take up to the socket timeout. `ping()` can wait up to `keep_alive`. A partial packet read waits up to `recv_timeout` (default 10 s). QoS 1 acks block similarly. `connect()` retries with `2**attempt` s back-off up to 32 s, plus jitter. Whether `socket_timeout` can be set to 0 safely: unverified. So steady state can be kept near 1 s only if `socket_timeout` is small, which is unverified. Reconnect blocks for the whole TLS handshake (4.4 s measured) plus back-off.
   - *Arduino:* a dedicated task on core 0 owns the MQTT/WebSocket client (e.g. PubSubClient or AsyncMqttClient, library choice not researched). It never blocks the render task on core 1. On reconnect it retries with back-off while the display shows a "reconnecting" state. Fits the budget by design.
3. **Privacy and operations:** plain HTTP exposes the location and headline queries to the LAN and ISP, so use HTTPS (or HTTPS to the cloud only). The owner runs: a small server (a container or serverless function, any host), weather and news API keys kept server-side, per-device rate limiting and caching (one upstream fetch per query per interval serves many devices), a certificate (e.g. Let's Encrypt) and a domain, and hosting cost. Hosting price depends on the provider (not researched).
4. **Does this make CircuitPython viable?** Partially. Steady state yes: MQTT/WebSocket pushes with a tiny payload and short socket timeout. The panel in CircuitPython is refreshed by a timer ISR (dhalbert's comment on #76), so it stays lit during stalls, but animation frames only update when the main loop runs. **Still blocking:** (a) reconnect after every Wi-Fi/server drop stalls the loop for 4.4 s+, (b) the in-loop partial-read and keep-alive waits can exceed 1 s, (c) the web portal, mDNS and AP+STA must also be served from the same single loop, (d) CircuitPython has no threads. The requirement is strict, so I rate it **partial, not safe**.

## F. Comparison
| | Arduino/C++ | Rust (no_std) | CircuitPython + cloud |
|---|---|---|---|
| Concurrency | FreeRTOS tasks, two cores; ISR refresh | Embassy tasks plus DMA; strongest on paper | single loop only; background refresh via ISR |
| Panel/Wi-Fi flicker | known open issues (#76, #71), single 64x32 low risk | #24 unresolved | same Protomatter ISR as Arduino; #76 seen on CircuitPython too |
| Ecosystem maturity | highest; Protomatter tested on this board | betas and 0.x; breaking each release | mature libraries, but TLS/loop blocking |
| Owner glue code | moderate (provisioning, JSON, tasks) | highest (about 40-60% infra, from earlier report) | lowest, but needs the cloud service |
| Flashing on macOS | arduino-cli/esptool; hold DOWN + tap RESET for ROM mode (auto-reset fails per brief), then tap RESET. Command flags not tested | espflash; same manual ROM entry (flags unverified) | drag `.uf2`/copy files, no esptool |
| Hot update of settings | portal writes NVS (stop matrix around write); firmware update via OTA is possible (unverified here) | storage and OTA hand-built | edit `settings.toml` or files over USB; no portal editing without writes |

## Recommendation
**Arduino/C++ on arduino-esp32 3.3.x, with Protomatter on a core-1 task, networking on core 0, and one owner-run HTTPS merged-JSON endpoint (MQTT/WebSocket push as an optional later step).**
Facts that drive it:
1. Network blocking cannot stall the panel in a task design, which CircuitPython cannot give.
2. Protomatter is officially tested on this board, and the single 64x32 case is the lightest of the reported failures.
3. Every portal part exists in Arduino (async server, mDNS, NVS, ArduinoJson filter/stream) without beta-level churn.

**First milestone:** on the real board, run a 64x32 animation at the intended bit depth while a core-0 task does a cold HTTPS fetch every 30 s with `setCiphers`/CA bundle, an NVS write, and portal requests, for several hours. Record any ghosting, dropped frames, and handshake time for RSA vs ECDSA. This retires the interference risk (#76/#71) and the unmeasured handshake cost together.
