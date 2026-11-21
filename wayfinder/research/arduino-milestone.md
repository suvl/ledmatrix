# Arduino milestone (ticket 012), interim results, 21 Nov 2026

Code: `arduino/milestone/milestone.ino`. arduino-esp32 3.3.11, Adafruit Protomatter 1.7.1, Matrix Portal S3 with the VGOL antenna on, one 64x32 panel at 4-bit depth. Render task on core 1; fetch and portal tasks on core 0. Cold HTTPS fetch every 30 s with a new client each time (full handshake), rotating three hosts, then an NVS write.

## Findings so far

| Question | Result |
| --- | --- |
| Does a 2 to 3 s TLS handshake stall the panel? | **No.** Render loop worst frame gap stayed at 26 ms (the 20 ms frame delay plus jitter) during every fetch. This is the main thing the task design was meant to buy. |
| Panel flicker with Wi-Fi/TLS traffic | None seen by the owner so far. Short observation only (about 2 to 3 minutes of the final build). |
| Cold handshake, **ECDSA** P-256 hosts (api.adsb.lol, api.open-meteo.com) | **about 2.2 to 2.4 s** connect time (DNS excluded), 7 samples |
| Cold handshake, **RSA-2048** host (www.amazon.com) | **about 0.38 to 0.39 s**, 3 samples |
| DNS | 1 to 11 ms (cached by the router/lwIP after the first lookup) |
| Request + response for a small JSON | 50 to 140 ms over an established TLS session |
| JSON parse (filtered, 0.4 to 0.8 KB) | 1 ms |
| NVS write | 2 to 3 ms |
| Free heap during fetches | about 152 KB internal, 2.09 MB PSRAM free |
| `adsb.lol` with HTTP/1.0 | returns 426 Upgrade Required. Use HTTP/1.1 with `Connection: close` and decode chunked bodies. |

Conclusions:
- **Use an RSA-2048 certificate on the owner's own cloud endpoint.** On the S3, ECDSA chain verification is about six times slower than RSA (no ECC acceleration, RSA uses the hardware big-number unit). Third-party hosts (flights, weather, news) are mostly ECDSA, so those fetches cost 2.3 s each. That is invisible in a background task but would blow the 500 ms cap if done inline. Caveat: one RSA host as the sample, and it measured a different server and network path.
- The 4.4 s cold fetch measured earlier on CircuitPython was slower than this (2.3 s connect plus request). CircuitPython's figure includes more overhead.
- Cold fetch every 30 s is workable in this architecture. A persistent connection is not needed to protect the display.

## Bug found: `matrix.stop()` / `matrix.resume()` blanks the panel for good

The research suggested pausing the matrix around flash writes (Protomatter issue 71). In this build that blanked the panel permanently: black within about 5 seconds of boot, which is when the first settings write happens, while the CPU and render loop kept running (render gap 26 ms, fetches continuing). With the pause removed (`PAUSE_FOR_NVS 0`) and the same NVS write running while the matrix is live, the panel stayed lit and smooth. A/B observed by the owner, one run each. Not investigated: why `resume()` fails on the S3 LCD_CAM path, and whether an NVS write can still cause a brief glitch (none visible so far, writes are 2 to 3 ms). Settings writes are rare (only when the owner changes a setting), so a rare tiny glitch would be acceptable.

## Not done yet

- Several-hour soak with the real log (running logger started after this note).
- RSA vs ECDSA with more RSA hosts, and with a larger sample.
- Wi-Fi access point plus station mode, the real portal, streaming a larger JSON, TLS session resumption.
