---
type: research
status: closed
---

## Question

Is the Rust ecosystem for the ESP32-S3 mature enough to build the appliance (HUB75 panel + Wi-Fi station and access point + mDNS + HTTP server + persistent settings)? Findings go in `wayfinder/research/rust-feasibility.md`.

## Resolution

Verdict from the research (not independently verified by me beyond reading the file): **ready with caveats.** A maintained S3 HUB75 driver exists (`esp-hub75` 0.17.0, LCD_CAM + DMA, 64x32 tested), and Wi-Fi, HTTP, mDNS, DHCP, TLS and storage each exist as young no_std crates. Roughly half of the infrastructure glue (AP provisioning, DHCP wiring, mDNS wiring, SNTP for certificate dates, bounded JSON, settings layout) would be hand-built, and the stack is beta or 0.x, so expect API breakage every month or two. Biggest risk: Wi-Fi traffic flickering the panel on a Matrix Portal S3 clone (`esp-hub75` issue 24), with no recorded fix. If a Rust path is chosen, the first milestone is panel plus Wi-Fi traffic on this exact board. No Matrix Portal S3 example exists, and brightness control, toolchain steps and espflash no-reset flags are unverified.
