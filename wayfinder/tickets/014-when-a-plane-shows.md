---
type: grilling
status: closed
---

## Question

When does a plane get shown, for how long, and what does the screen look like?

## Resolution (owner, 21 Nov 2026)

- **Once per plane, on entering the radar.** A plane is shown a single time, when it first comes into the area the owner configured. It is not shown again while it stays in range. A plane that goes out of range and comes back later is not yet defined (see open items).
- **Duration:** configurable on the portal, default **10 seconds**.
- **Priority:** a plane always wins the screen. Weather and headlines only show when no new plane is being shown (from the owner's earlier description of the three features).
- **Layout:** similar to the product theflightwall.com. Details to be taken from that product, adapted to 64×32 pixels. See ticket 006 (display layout prototype).

Open items:
- What "once" means over time: a flight seen at 9:00 and again at 9:40 (same callsign, a round trip or a circling aircraft), and across a reboot (the seen list is in RAM unless stored). Suggested: remember a flight by callsign + ICAO hex for a cooldown of, say, 30 minutes, and clear the list on reboot.
- Several planes entering in the same poll: show closest first, one after another, each for the configured time, or only the closest?
- What the idle screen shows when there is no plane, no weather and no headline.
- Which fields appear, and which of them need extra data sources (airline, route and aircraft type are not in raw ADS-B).
