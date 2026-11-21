---
type: task
status: open
---

## Interim result (21 Nov 2026)

Toolchain installed (`arduino-cli` 1.6.0, esp32 core 3.3.11, Protomatter 1.7.1, ArduinoJson 7.4.3). Milestone sketch runs: a 2.3 s ECDSA handshake does not stall the panel (worst render gap 26 ms), RSA handshakes take about 0.4 s, no flicker seen. Bug found: `matrix.stop()`/`matrix.resume()` around the NVS write blanked the panel for good; writing without pausing is fine. Several-hour soak still to run. Details in `wayfinder/research/arduino-milestone.md`.

## Question

First Arduino/C++ milestone: set up the toolchain and run, on the real board for several hours, a 64×32 animation at the intended bit depth while a core-0 task does a cold HTTPS fetch every 30 s, an NVS settings write and portal requests. Record ghosting or flicker, dropped frames, and the cold TLS handshake time for RSA versus ECDSA certificates. This retires the Protomatter-plus-Wi-Fi interference risk (Adafruit_Protomatter issues 76 and 71) and the unmeasured handshake cost.

Notes from the research: pin arduino-esp32 to 3.3.x (Protomatter was only tested on 3.3.11 and 4.0.0-alpha1); render on core 1, networking on core 0; call `matrix.stop()` and `matrix.resume()` around flash writes; the board enters the ROM bootloader by holding DOWN and tapping RESET, then tap RESET again after flashing. Project code goes in `arduino/`. The CircuitPython stress test on the board gets replaced.
