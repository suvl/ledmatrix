---
type: task
status: open
---

## Question

Does CircuitPython 10.3.2 on this exact board run the appliance within acceptable resources? Measure on the device: free RAM and PSRAM, access point plus station at once, `mdns`, `adafruit_httpserver` serving a settings page while the panel keeps refreshing, and persistent settings (nvm vs filesystem when USB is attached). Acceptable means no visible panel stalls and a comfortable memory margin with the portal running. Findings go in `wayfinder/research/circuitpython-budget.md`.

## Resolution (pending the owner's visual check of the panel)

Measured on the board, 21 Nov 2026: with panel, Wi-Fi station, access point, mDNS, web server and HTTPS fetch all running, free heap stays near 1.94 MB, and even a 195 KB JSON parsed whole with no `MemoryError`. Station plus access point came up together, the portal answers by IP and by `flights.local`. The cost is blocking: a fetch stalls the loop and the portal for 0.3 to 0.6 s at a realistic radius (first fetch 4.4 s) and 7 to 12 s at a huge radius. **Correction:** those stall figures were taken with no antenna attached (WROOM-1U has none onboard). With the antenna on, 15 of 15 fetches succeeded and stalls were 0.3 to 0.7 s (4.4 s on the first). The owner saw no panel flicker. Not measured: a client joining the access point, panel flicker under Wi-Fi traffic (needs eyes on the panel), peak memory during the big parse. Details in `wayfinder/research/circuitpython-budget.md`.
