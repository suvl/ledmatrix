---
type: grilling
status: closed
---

## Question

Time zone per location: the clock is fixed to Lisbon time (WET/WEST). How should the device get the right zone for the location the owner picks on the map?

## Notes (22-24 Nov 2026)

Facts gathered:
- Today `configTzTime("WET0WEST,M3.5.0/1,M10.5.0/2", ...)` is hard coded in `live.cpp`.
- The service already has Go's tz database embedded (`time/tzdata`), so it can map coordinates to an IANA zone and return a POSIX TZ string; the board needs only the string.

Decisions pending: where the lookup lives (service, with a zone table in the service, or an offline table on the board), what happens outside the service's coverage, whether the portal lets the owner override the zone by hand, and how the 12/24 h clock and the log timestamps follow it.

## Resolution (owner, 24 Nov 2026)

- **The portal decides, in the browser.** When the pin is placed on the map, a small offline lookup library (about 70 KB, pinned with an integrity hash like Leaflet) maps the coordinates to an IANA zone, and the portal stores both the zone name and its POSIX rule string on the board. The service is not involved and the board needs no internet to know its zone.
- **A dropdown overrides it by hand** (all zones, searchable), and it is the only way when the browser has no internet.
- The default stays Europe/Lisbon (`WET0WEST,M3.5.0/1,M10.5.0/2`) until a location is set.
- Build notes: (1) a table zone name to POSIX string is generated at build time from the tz database and kept in the repository (about 400 entries, about 15 KB), so a rule change means regenerating and releasing, which the update path now supports; (2) the board applies it with `configTzTime` as today, and live data refresh does not need to restart; (3) the 12/24 hour clock, the recent planes log, the weather hours (ticket 026) and night blanking (ticket 024) all use it; (4) the portal shows the zone and the board's current local time so a wrong zone is obvious; (5) the preview page needs no change.
