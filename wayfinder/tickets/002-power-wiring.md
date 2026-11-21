---
type: grilling
status: closed
---

## Question

How is the panel and the S3 powered, and what must never be done?

## Resolution

Plan REV A (16 Nov 2026):

- One Velain 5V 6A supply into DC female jack A. The panel's own power cable and a 20 cm lead share jack A's terminals (red +, black −).
- The lead goes to DC male plug B, then into the Greluma DC→USB-C adapter, then the S3's USB-C port. The S3 never carries panel current.
- The S3 connects to the panel's HUB75 **IN** (ribbon or seated). HUB75 carries signals only. HUB75 OUT stays empty.
- The S3's screw posts are unused. Adafruit's note, as quoted in the plan: they are outputs fed from USB, and 4A through the board would damage it.
- Budget: panel up to 4A at full white, S3 with Wi-Fi about 0.3A, supply 6A.
- Before connecting the Greluma to the S3: multimeter on the Greluma's VBUS or plug B's + screw. Expect +5.0 to 5.2 V. If −5V, swap the wires at plug B. Also check the panel header for reversed terminals.
- Never both USB sources on the S3 (laptop and Greluma) at once.
- Open checks: the Greluma must accept 5V input and pass it through. The module is a WROOM-**1U**: it has no onboard antenna, so the external antenna is **required**, not optional. Without it about a third of HTTPS fetches failed and the loop froze for 5 to 25 s. With the VGOL patch antenna on the u.FL socket: 0 errors in 15 fetches, RSSI about -59 dBm (see `wayfinder/research/circuitpython-budget.md`). No jumper was needed.
- Data link decision: use the **ribbon cable** from the panel's HUB75 IN to the S3, not the S3 seated directly on the header. The panel's IN header is unkeyed, so the S3 fits in both orientations. Seated 180° rotated (the orientation that hides it behind the panel) the pins are reversed and the display is garbled, and the S3's outputs land on the panel's ground pins. The only correct direct orientation leaves the S3 overhanging the panel edge, which the owner rejected. Software can't fix a swapped pinout. When seating, match the ribbon's red stripe to pin 1 on the IN header.
- Observed 21 Nov 2026: the panel ran the S3's stock demo from the Velain supply with the S3 on laptop USB. This confirms panel power, the HUB75 link, and the panel's scan mapping.
