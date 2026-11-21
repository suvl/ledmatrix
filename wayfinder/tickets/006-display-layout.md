---
type: prototype
status: closed
---

## Resolution (owner, 21 Nov 2026)

Chosen from the screen preview page (`preview/index.html`, published privately; candidate layouts A/B/C per screen). Layouts are **selectable on the portal**:

- **Flight card:** all three (A logo + three lines + speed strip, B logo + three lines + destination name, C no logo + full telemetry).
- **Weather:** all three (A icon + big temperature + high/low, B location + big temperature, C now / +3 h / +6 h).
- **Headlines:** both (A scrolling line, B paged four lines).
- **Idle:** the clock and date (A), or a clean black screen. Radar and dots dropped.
- **Setup mode:** B was first chosen as one item at a time, 3 s each. Testing on the real board showed that is too fast to read and type a password, so it is now **one static screen** with the network name, the password in the big font, and the address (owner's call, 21 Nov 2026).
- **Status screens:** all four as previewed (booting, connected, stale-data dot, no Wi-Fi).

Data these layouts need, passed to ticket 005: airline name, route (IATA/ICAO codes), aircraft type, airport names, altitude, speed, heading, vertical rate, a 16×16 logo, weather (temperature, condition, high/low, rain chance, +3 h and +6 h), headline text. Pixel limits: 10 characters per line at 6×8, 16 at 4×6, logo 16×16.

**One active layout per screen**, chosen on the portal (owner, 21 Nov 2026). No rotation between layouts. The portal reuses the preview page's pixel renderer to show the layout choices; it must work with no internet (setup mode), so no web fonts or CDN scripts: the page and its font data are embedded in the firmware.

Open: the idle clock's timezone and 12/24 h format; fonts and colours are as previewed and can still change.

## Original question (kept for context)

## Question

What does one flight look like on 64×32 pixels (callsign, route, altitude, distance, aircraft type)? One flight at a time or several? Fixed colours or by altitude? Reference (owner's four photos of the commercial FlightWall, 21 Nov 2026): airline logo block at left, then three text lines to its right (airline name, route as `LAX-ICN` or `JFK > SEA`, aircraft type such as `747-8`); under that, either two telemetry lines (`Alt:9.9kft,Spd:330kts` / `Trk:305deg,Vr:+1ft/s`) or two lines with a phrase and airport names (`Departing to / Seoul Incheon`, `John F Kennedy Intl / Seattle Tacoma Intl`). Their LED walls are larger than 64×32, so the content has to be cut down. Target: 64×32 pixels, about 10 characters per line at 6×8, about 16 at a 4×6 font, logo about 16×16.
