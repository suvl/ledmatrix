---
type: research
status: open
---

Findings go in `wayfinder/research/flights-data.md` (research started 21 Nov 2026, together with tickets 017 weather and 018 headlines).

**Research finished 21 Nov 2026**, awaiting the owner's decision: positions from adsb.fi open data (keyless, 1 req/s, non-commercial, no IPv6), fallback adsb.lol (IPv6, but 500 requests/day without a key); route, airline and airport names from adsbdb.com (community data, wrong or missing for charter, cargo, general aviation and military flights); full aircraft type from adsbdb or the `desc` field; logos converted offline to 16×16 for the top 100 to 200 airlines (trademark caveat: keep them private). Everything through the owner's own cloud API, which would shrink each plane to about 60 bytes and answer over an RSA certificate.

## Question

Which public flight-data API should feed the tracker (free tier, no key or easy key, rate limits, bounding-box query, payload size on a microcontroller)? Candidates to compare: OpenSky, adsb.lol, adsb.fi, airplanes.live. Payload size matters less now: the firmware is Arduino/C++ and an owner-run cloud API can merge and shrink the data.

**Added 21 Nov 2026:** raw ADS-B has no airline name, origin, destination, aircraft name, airport names or logos. Research where each can come from, free versus keyed. Leads from the TheFlightWall open-source repo (Apache-2.0): OpenSky states/all with OAuth (free account) for positions; FlightAware AeroAPI (key, per-call cost not checked) for airline, aircraft type and route; its CDN lookups `cdn.theflightwall.com/oss/lookup/airline/{ICAO}.json` and `.../aircraft/{ICAO}.json` for display names (no key; terms of use not checked). Also check free route/aircraft databases such as adsbdb and adsb.lol's route lookup. Their repo has no logos (README: "coming soon") and no airport-name lookup.
