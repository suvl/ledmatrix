# Rust on ESP32-S3 for a Matrix Portal S3 appliance: feasibility (2026-11-21)

Sources: crates.io API (versions/dates), docs.rs, GitHub repos/issues/changelogs, Adafruit CircuitPython board files. Facts I could not confirm are marked **unverified**. Fetches were summarised by a tool, so a few odd outputs are flagged inline.

## Summary
- Panel driver, Wi-Fi, TLS, HTTP, mDNS, DHCP and flash storage each exist as maintained no_std crates, but nearly all are young (1.0 betas, 0.x) and break API every release or two.
- **Biggest risk: Wi-Fi activity flickering a HUB75 panel on this very board family.** Issue liebman/esp-hub75 #24 (Aug 2025, a Matrix Portal S3 clone) ended unresolved for the reporter. Later refactors may have fixed it, but I found no evidence either way.
- No Matrix Portal S3 example exists. The driver is not board-specific, but pins and timings must be hand-configured.
- The AP provisioning / captive / mDNS / HTTP glue is hand-assembled from parts. No turnkey provisioning crate exists on the no_std side.
- The std path (esp-idf-svc) has the easy services but has **no HUB75 driver** (esp-hub75 PR #45 for esp-idf support was closed unmerged).
- **Verdict: ready with caveats.** Prototype the Wi-Fi + panel coexistence first.

## Board facts (primary: adafruit/circuitpython, ports/espressif/boards/adafruit_matrixportal_s3)
- Flash 8 MB QIO 80 MHz. PSRAM **2 MB, quad (QIO)**, so the board is probably the N8R2 module (my inference). This answers the "PSRAM variant unknown" question.
- HUB75 GPIOs: R1=42 G1=41 B1=40 R2=38 G2=39 B2=37, A=45 B=36 C=48 D=35 E=21, CLK=2 LAT=47 OE=14. Buttons: UP=6, DOWN=7. Not contiguous, so the LCD_CAM pins must be routed through the GPIO matrix (an assumption about the driver API; **unverified** against esp-hub75 source).
- The CircuitPython board file disables ParallelDisplayBus with the comment "Not enough pins".

## 1. HUB75 driver: partial (works on S3 and 64x32, but nothing for this board and a Wi-Fi flicker risk)
- **esp-hub75 0.17.0** (2026-09-14; 13 releases since 2025-06; MSRV 1.95, edition 2024, depends on esp-hal ~1.2.0). Repo liebman/esp-hub75, 261 commits, active. Its Releases page is empty, so tags are not published there.
- S3 uses **LCD_CAM + DMA**. Also supports ESP32 (I2S), C6/C5 (PARL_IO). Features include `circular-dma`, `iram`, blanking tuning, and invert-clock/OE/blank.
- **hub75-framebuffer 0.13.0** (2026-10-07): DMA-friendly bitplane and standard framebuffers that implement embedded-graphics (per esp-hub75 README).
- 64x32 1/16-scan: README lists a "Waveshare P3-64x32 (1/16)" and a "generic 64x32 (1/16)" as tested. The Matrix Portal's E line is not needed for this panel.
- Brightness: I found only BCM bitplanes and blanking features, no obvious brightness API. Brightness control is **unverified**.
- Matrix Portal S3 is not named in the README, and no example for it exists. Closest examples: gradient, gradient-embassy.
- Recent maintenance: ISR/DMA refactor (PR #65, 2026-09-09), HIL tests (#67), a ghosting issue (#51) closed. Ghosting is described as panel-specific and tunable.
- **Wi-Fi flicker: issue #24, "Flickering on wifi use"** (opened 2025-08-07, closed 2025-08-14 although the reporter said it was not fixed). It happened on S3 / Matrix Portal S3 clone and recurred on each HTTP request. The maintainer's hypothesis was instruction-cache eviction while Wi-Fi runs. `--release` helped him but not the reporter. The `iram` feature, core pinning and disabling logging did not help the reporter. The thread ends with an untested `#[ram]` suggestion for esp-hal's lcd_cam. No resolution recorded. The reporter's repro repo (lostb1t/flicker_test) has 3 commits, all from 2025-08-09.
- Whether the Sept 2026 refactor (interrupt-driven continuous rendering PR #49, ISR rework #65) fixed this: **unverified**.

## 2. Wi-Fi STA / AP / both, DHCP server: partial
- **no_std: esp-radio** (replaces esp-wifi). Latest **1.0.0-beta.1** (2026-09-16 on crates.io; GitHub release 2026-09-22). crates.io shows beta.2 (2026-09-17) as **yanked**, while the changelog lists beta.2 as an S31 build fix. The sources conflict, so use beta.1 or check before pinning. Earlier: 0.18.0 (2026-04-16). It needs esp-hal `unstable`.
- `Config::AccessPointStation(StationConfig, AccessPointConfig)` exists (renamed Mixed -> ApSta -> AccessPointStation across releases). Official example esp-hal/examples/wifi/embassy_access_point_with_sta. Code comments: a FIXME about dropped simultaneous connections, a panic-prone request parser, and a static AP IP. Android may show "no internet".
- **The AP side has no DHCP server in that example.** The AP is fixed at 192.168.2.1 and clients must set a static IP. A DHCP server needs **edge-dhcp 0.8.0** (2026-06-25, no_std) wired on top of embassy-net. Whether it works well with esp-radio's AP is **unverified**; no maintained example found.
- WPA3 not supported (esp-radio docs). Heap use is about 47-57 KB (STA) and 53-63 KB (AP), per esp-radio docs. Some AP setters (`max_connections`, `dtim_period`) are behind `unstable`.
- **std: esp-idf-svc 0.53.0** (2026-09-15; the previous release was 0.52.1 on 2026-03-10; the docs.rs build for 0.53.0 was reported failed). It wraps ESP-IDF Wi-Fi (STA, AP, mixed), including the C DHCP server and netif. This is the more battle-tested stack, but it "might lag behind the latest stable ESP-IDF". The ESP-IDF version it targets is **unverified**.
- Comparison: std gives DHCP, mDNS, HTTP, NVS and TLS as built-ins at the cost of a much heavier build (ESP-IDF toolchain) and **no HUB75 driver**. no_std gives the panel driver but you assemble the network services.

## 3. mDNS: partial (no_std) / works (std)
- no_std: **edge-mdns 0.8.0** (2026-06-25; previous 0.7.0 on 2026-01-01). It is async, no-alloc, runs over embassy-net UDP. I did not find a first-party esp-radio example. Behaviour on the esp-radio AP/STA interfaces (multicast join, with both interfaces) is **unverified**.
- std: `esp_idf_svc::mdns` exists, gated on the ESP-IDF mdns component (lib.rs on master). Works.

## 4. HTTP server: works (no_std)
- **picoserve 0.21.0** (2026-11-14; 0.20.1 on 2026-09-21): async no_std router, static pages and JSON. Active (4 releases in 5 months).
- Alternative: **edge-http 0.8.0** (2026-06-25). std alternative: esp-idf-svc `EspHttpServer` (its own example does AP + JSON POST).
- Whether picoserve has an esp-hal sample: **unverified**. The esp-hal Wi-Fi example uses hand-rolled TCP reads.
- Captive portal DNS: **edge-captive 0.8.0** (2026-06-25). The full captive-style OS popup behaviour is **unverified**.

## 5. HTTPS client + JSON: partial
- no_std TLS: **mbedtls-rs 0.3.0** (2026-09-14; 0.2.0 on 2026-08-20; 0.1.0 on 2026-06-25) in esp-rs/mbedtls-rs. It replaces esp-mbedtls (the old name was not on crates.io under that name). Needs `alloc`. Async HTTPS example exists.
- **Certificate validity dates are NOT checked by default**. You get chain, signature and hostname checks, but expired certs are accepted. Enabling date checks needs the `hook-wall-clock` feature plus an SNTP clock (esp-hal has an embassy_sntp example). CA bundle handling and S3 hardware acceleration: **unverified**.
- HTTP client: **reqwless 0.14.0** (2026-01-12). Its docs say it supports TLS via embedded-tls (TLS 1.3 only) or esp-mbedtls; whether it already supports mbedtls-rs is **unverified**.
- JSON: **serde-json-core 0.7.0** (2026-10-08; previous release 0.6.0 in Aug 2024). It is no_std, no-alloc, but it deserializes from a complete slice (not streaming). A large API response must be buffered, or filtered with a hand-rolled scanner. Aggregator responses can be tens of KB, which is a real constraint (the payload size of the intended API is unverified).
- std: esp-idf-svc `tls` / HTTP client with the ESP-IDF cert bundle. Easier.

## 6. Persistent settings: works
- no_std: **esp-storage 0.10.0** (2026-08-26) provides flash access, with **sequential-storage 8.0.2** (2026-10-01) as a wear-aware key/value map on a partition. Partition table handling through esp-bootloader-esp-idf: **unverified** details. No NVS-compatible crate for no_std was found.
- std: esp-idf-svc `nvs`, which is the real NVS. Works.

## 7. Concurrency with panel refresh, memory pressure
- esp-hub75's DMA design means near-zero CPU use for refresh (README: "almost no CPU involvement"). With Embassy, panel and network can share a core or be split across the two cores. esp-rtos is used in the esp-hal examples.
- Memory budget (rough, my estimate from published numbers): the S3 has about 512 KB SRAM total, of which Wi-Fi takes about 50-60 KB heap plus TLS handshake buffers (mbedtls TLS record buffers are commonly 16-30 KB, **unverified for this stack**), plus a 64x32 bitplane framebuffer in the tens of KB. Tight but feasible without PSRAM; the 2 MB quad PSRAM is probably not needed.
- DMA from PSRAM: the 2 MB is quad PSRAM, and I found no evidence esp-hub75 can feed LCD_CAM from PSRAM. Keep the framebuffer in internal RAM. esp-hal's PSRAM-fed LCD_CAM work (RGB bounce-buffer, #5262 draft, #6744 tracking) concerns the RGB/DPI mode, not i8080.
- No LCD_CAM + Wi-Fi interaction issue found in esp-hal open issues apart from esp-hub75 #24. I did not read every esp-hal issue (the search returned partial pages).

## 8. Toolchain and flashing on macOS
- S3 is Xtensa, so no_std needs the espup toolchain (the esp Rust fork). **espup 0.19.0** (2026-10-27; 0.18.0 yanked; 0.17.1 was 2026-04-17). **espflash 4.7.0** (2026-10-29).
- esp-hal 1.2.2 (2026-09-18); esp-hub75 needs rust-version 1.95 (the esp fork's version vs 1.95: **unverified**).
- I could not load the esp-rs book install page (404), so exact first-time steps and build times are **unverified**. Expect a first build of a Wi-Fi + TLS project to take minutes; I did not measure.
- Flashing: Adafruit documents ROM bootloader entry with BOOT/DOWN held plus RESET. espflash with `--before no-reset` / `--after no-reset` style options is the likely workaround for no auto-reset (flag names **unverified**). The S3 native USB means the port re-enumerates between app and ROM mode; flash while in ROM mode, then tap RESET.

## Hand-assembled vs maintained (rough estimate, my judgement)
- Maintained crates available: panel driver, Wi-Fi stack, HTTP server, mDNS, DHCP, captive DNS, TLS, flash KV storage. About 8 of ~10 blocks exist.
- Hand-assembled glue: roughly **40-60% of the infrastructure code** is yours. AP provisioning state machine (STA fail -> AP fallback), AP+STA coexistence handling, DHCP server wiring, wall-clock/SNTP for certificate dates, bounded JSON handling, flash layout and settings versioning, captive-portal redirect logic, and API tuning for each beta bump. None of these have a single drop-in crate.
- Pin versions and expect migration work every 1-2 months (esp-hal, esp-radio, esp-hub75 all released several times in the last 4 months).

## Blockers and open risks
1. Wi-Fi-induced panel flicker on the Matrix Portal S3 family (esp-hub75 #24), no recorded fix.
2. esp-radio is still 1.0.0-beta, with a yanked beta.2 and a conflicting changelog.
3. TLS certificate date checking is off by default.
4. AP-mode DHCP server and mDNS on esp-radio: no maintained end-to-end example found.
5. No std HUB75 driver, so the easy network path cannot drive the panel.

## Verdict: ready with caveats
Facts driving it:
- A maintained S3 LCD_CAM+DMA HUB75 driver supports 64x32 and embedded-graphics (esp-hub75 0.17.0), and the rest of the stack exists as no_std crates.
- The one reported Wi-Fi flicker on this hardware family is unresolved in the record; the first milestone must be panel + Wi-Fi traffic on the real board.
- Everything is beta or 0.x and the provisioning and network glue is largely hand-built (about half the infrastructure).
- If the flicker test fails, the fallback is a std-ESP-IDF build with a custom HUB75 driver or CircuitPython, not a drop-in Rust alternative.
