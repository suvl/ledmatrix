---
type: task
status: closed
---

## Question

How do we get CircuitPython onto the S3, and is it flashed?

## Resolution

Done 21 Nov 2026.

- Board identified over USB as "MatrixPortal ESP32_S3" (Adafruit vendor 0x239A), MAC `aa:bb:cc:dd:ee:ff`, 8 MB flash, quad. It shipped with a stock demo, confirmed on the panel.
- The board has no reachable auto-reset. Bootloader entry is manual for CircuitPython: hold a boot button and tap **RESET**. Correction: this note first said DOWN is GPIO0, but the Arduino pin definitions give DOWN = GPIO7, and GPIO0 is the separate small **Boot** button. Which one worked for the owner was not recorded. With the Arduino firmware, `arduino-cli upload` resets into the bootloader by itself, so no buttons are needed. It then shows as "USB JTAG_serial debug unit".
- `uvx esptool erase-flash`, then `write-flash 0x0` of `adafruit-circuitpython-adafruit_matrixportal_s3-en_US-10.3.2.bin` (from downloads.circuitpython.org, 1.9 MB, sha256 `80ed9122…7c31`). Hash verified. The old demo is gone.
- esptool's software reset does not leave the bootloader on this chip: tap RESET (alone) afterwards.
- Result: `Adafruit CircuitPython 10.3.2 on 2026-11-11`, drive `/Volumes/CIRCUITPY`, serial `/dev/cu.usbmodem*` (name changed after flashing).
- Libraries installed with `uvx circup --path /Volumes/CIRCUITPY install adafruit_display_text`.
- Chose 10.3.2 stable over the 11.0.0 alpha.
