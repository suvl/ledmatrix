---
type: grilling
status: open
---

## Question

Time zone per location: the clock is fixed to Lisbon time (WET/WEST). How should the device get the right zone for the location the owner picks on the map?

## Notes (22-24 Nov 2026)

Facts gathered:
- Today `configTzTime("WET0WEST,M3.5.0/1,M10.5.0/2", ...)` is hard coded in `live.cpp`.
- The service already has Go's tz database embedded (`time/tzdata`), so it can map coordinates to an IANA zone and return a POSIX TZ string; the board needs only the string.

Decisions pending: where the lookup lives (service, with a zone table in the service, or an offline table on the board), what happens outside the service's coverage, whether the portal lets the owner override the zone by hand, and how the 12/24 h clock and the log timestamps follow it.
