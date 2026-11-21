---
type: research
status: open
---

## Question

Which public flight-data API should feed the tracker (free tier, no key or easy key, rate limits, bounding-box query, payload size on a microcontroller)? Candidates to compare: OpenSky, adsb.lol, adsb.fi, airplanes.live. Payload size matters less now: the firmware is Arduino/C++ and an owner-run cloud API can merge and shrink the data.

**Added 21 Nov 2026:** raw ADS-B has no airline name, origin, destination, aircraft name, airport names or logos. Research where each can come from, free versus keyed. Leads from the TheFlightWall open-source repo (Apache-2.0): OpenSky states/all with OAuth (free account) for positions; FlightAware AeroAPI (key, per-call cost not checked) for airline, aircraft type and route; its CDN lookups `cdn.theflightwall.com/oss/lookup/airline/{ICAO}.json` and `.../aircraft/{ICAO}.json` for display names (no key; terms of use not checked). Also check free route/aircraft databases such as adsbdb and adsb.lol's route lookup. Their repo has no logos (README: "coming soon") and no airport-name lookup.
