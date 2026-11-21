---
type: grilling
status: closed
---

## Question

How do we develop and test the display output?

## Resolution

- Edit in `~/ledmatrix/circuitpython/`, run `./deploy.sh` to copy `code.py` to `CIRCUITPY`. The board auto-reloads.
- The real panel is the source of truth. Keep drawing code separate from networking so each can be tested on its own.
- Optional later: a desktop displayio renderer (Blinka plus `blinka_displayio_pygamedisplay`) for layout work. Not verified, not required.
- Wokwi and `RGBMatrixEmulator` were considered. Wokwi support for this board and panel is unverified. `RGBMatrixEmulator` uses a different API.
