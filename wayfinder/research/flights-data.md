# Flight data sources for the ESP32-S3 LED display (research, 2026-11-21)

## Summary and recommendation

- Positions: adsb.fi open data (`/api/v3/lat/{lat}/lon/{lon}/dist/{nm}`) is the best keyless source for a 30 s poll. Documented limit is 1 req/s, no daily cap, no key, personal non-commercial use only, attribution required. It returns `desc` (full type name), `t`, `r`, `flight`, `gs`, `track`, `baro_rate`.
- adsb.lol (same readsb JSON, ODbL, AAAA present) is the fallback, but keyless access is capped at 500 req/IP/day (seen in `x-ratelimit-limit: 500, 500;w=86400`). 30 s polling needs 2880/day. That needs a free feeder key (7,200/day) or a cloud proxy that backs off when nothing is nearby.
- Enrichment: adsbdb.com (`/v0/callsign/{cs}` gives airline, IATA+ICAO route and airport names in one call; `/v0/aircraft/{hex}` gives the full type name). hexdb.io is the fallback (Cloudflare, AAAA, no documented limits). Route data is community-sourced and imperfect (see section 3).
- Put a small own cloud API in the middle for everything. The device then makes one RSA-certificate TLS call (about 0.4 s) and gets about 60-100 B per plane, with route, names and logo already merged. Static tables (airlines, airports, type names) can also live in flash as a fallback.
- Not suitable for this device: ADS-B Exchange ($10/month = 10,000 requests, a 30 s poll needs about 86,000/month), FlightAware AeroAPI ($0.05 per position search, about $144/day at 30 s), OpenSky (400 credits/day anonymous, no type or registration field, no IPv6).
- Every ADS-B API gives ft, kt, ft/min and degrees, and no route. Radius parameter is nautical miles: 15 km = 8 nm, 100 km = 54 nm, 250 km = 135 nm (the APIs cap at 250 nm).

## 1. Position sources

### Test method
Few GET requests, User-Agent `ledmatrix-research`, lat 48.0 lon 10.0, 2 s apart. Sizes are for that area at test time (sparse airspace, unrepresentative of busy areas). Per-aircraft cost is about 650-700 B raw JSON (adsb.lol: 4 aircraft = 2,721 B raw, 954 B gzip; adsb.fi v2: 2,795 B raw, 1,076 B gzip). The API has no field filter, so busy areas scale linearly: 50 planes at 100 km is about 35 KB raw, about 12 KB gzip.

| Item | adsb.lol | adsb.fi open data | airplanes.live | OpenSky | ADS-B Exchange | FlightAware AeroAPI |
|---|---|---|---|---|---|---|
| Point+radius endpoint | `https://api.adsb.lol/v2/point/{lat}/{lon}/{nm}` (also `/v2/lat/../lon/../dist/..`) | `https://opendata.adsb.fi/api/v3/lat/{lat}/lon/{lon}/dist/{nm}` (v2 deprecated, still answers) | `https://api.airplanes.live/v2/point/{lat}/{lon}/{nm}` | `GET https://opensky-network.org/api/states/all?lamin=&lomin=&lamax=&lomax=` (bounding box only) | RapidAPI paid; lat/lon/dist (unverified details) | `GET /flights/search/positions` (query syntax) |
| Fields | hex, flight, r, t, alt_baro, alt_geom, gs, track, baro_rate, geom_rate, lat, lon, dst, dir, squawk, category (no `desc` seen in my sample) | same readsb fields plus `desc` (e.g. "DORNIER 328"); `alt_baro` is `"ground"` when on ground | readsb format (unverified, my requests got 403) | array of 17: icao24, callsign, country, lon, lat, baro alt (m), on_ground, velocity (m/s), true_track, vertical_rate (m/s), geo alt, squawk, category. No type code, no registration | readsb format | JSON, per-flight; includes route (ident, origin, destination) |
| Rate limit / terms | 500 req/day/IP without key; free adhoc key 1,000/day; free feeder key 7,200/day; paid up to 360,000. 429 + Retry-After; resets 00:00 UTC. ODbL 1.0 | 1 req/s on public endpoints; bad requests count; abuse leads to IP block. Personal, non-commercial only; attribute adsb.fi; contact for higher rates | Third parties say 1 req/s, non-commercial, no SLA, key not needed "currently". My requests got 403: "contact us at contact@airplanes.live... include links, description of project" | OAuth2 client credentials only. Anonymous 400 credits/day; account 4,000/day; active feeder 8,000/day. 1 credit per query up to 25 sq deg; 2 up to 100 sq deg; 3 up to 400 sq deg. Commercial use needs licence | Community API $10/month, 10,000 requests, non-commercial | Personal tier: no minimum, $5/month free ($10 for feeders), then per query; 10 result sets/min |
| Key / cost | None (feeder key optional, free) | None, free | None documented; see 403 note | Free account required (OAuth) | Paid via RapidAPI | Account and API key; paid beyond free credit |
| AAAA | Yes `2a0a:4cc0:c1:f5e6::` | No (Cloudflare, `dig AAAA` empty) | No (Cloudflare) | No | No | No at the hostname (CNAME to Apigee; AAAA output empty) |
| Cert key / chain | ECDSA P-256 leaf, 4 certs sent (LE YE2, Root YE, ISRG Root X2). Cold handshake about 2.3 s | ECDSA P-256, 3 certs (Google WE1, GTS Root R4) | ECDSA P-256, 3 certs (Google) | RSA 2048, 3 certs (LE YR1, Root YR) | ECDSA P-256, 3 certs | RSA 2048, 3 certs (SSL.com). No IPv6, so not a fit |
| 15 km / 100 km size (test) | 101 B (0 planes) / 2,721 B raw, 954 B gzip (4 planes) | 75 B / 2,795 B raw, 1,076 B gzip | blocked | 156 B for 1.4 x 1.0 deg box (1 plane, raw ~150 B per plane) | n/a | n/a |
| Caveats | `cache-control: no-store`, sticky cookie. Rate limit may tighten (page mentions keys for feeders). Routes via `routeset` | Cloudflare only (IPv4). v3 returned 200 in test | Cloudflare challenge on web pages; API gatekeeping unclear | Box is not a circle, filter client-side. No type/registration; `/states/all` only has the latest data for anonymous callers | Cost and quota rule it out | Cost rules it out for positions |

Sources read:
- https://www.adsb.lol/docs/open-data/api/
- https://api.adsb.lol/api/openapi.json
- https://github.com/adsbfi/opendata (fetched through WebFetch, README text)
- https://openskynetwork.github.io/opensky-api/rest.html
- https://www.flightaware.com/commercial/aeroapi/
- http://www.adsbexchange.com/data-products/
- https://flyitalyadsb.com/en/dati/confronto/ (third party, dated 2026-10-28, used for airplanes.live and ADSBx terms)

TLS cost note: only adsb.lol, adsbdb and hexdb have AAAA. The device is dual-stack, so Cloudflare-fronted hosts without AAAA are still reachable over IPv4. The ECDSA handshake cost (about 2.3 s) applies to all candidates except OpenSky and FlightAware (RSA). Mitigation if direct: keep the TLS connection alive between polls, or use TLS session tickets (unverified for these hosts). Your own cloud API with an RSA-2048 certificate removes this cost.

## 2. Enrichment sources (tested with arbitrary data)

| Item | adsbdb.com | hexdb.io | adsb.lol routes |
|---|---|---|---|
| Route from callsign | `GET https://api.adsbdb.com/v0/callsign/UAE6` returned callsign_icao and callsign_iata (EK6), airline (name, icao, iata, country), origin and destination (icao, iata, name, municipality, lat/lon, elevation) | `GET https://hexdb.io/api/v1/route/icao/UAE6` returned `{"route":"EGLL-OMDB"}`. ICAO codes only, no names. v2: `/api/v2/routes/{callsign}` (unverified response) | `POST https://api.adsb.lol/api/0/routeset` with `{"planes":[{"callsign":"..","lat":..,"lng":..}]}` returned HTTP 201, empty body in my test (unverified whether it works). The old `GET /api/0/route/...` gave 500 |
| Aircraft by hex | `GET /v0/aircraft/89647f` returned type "A380 842", icao_type A388, manufacturer, registration, owner, photo URLs | `GET /api/v1/aircraft/89647f` returned Type "A380 842", ICAOTypeCode, Manufacturer, RegisteredOwners, OperatorFlagCode | n/a |
| Airline by code | `GET /v0/airline/UAE` returned name "Emirates", icao, iata | n/a | n/a |
| Airport by code | in route response | `GET /api/v2/airports/EGLL` returned name "Heathrow Airport", iata, lat/lon | `GET /api/0/airport/{icao}` (in OpenAPI, not tested) |
| Combined | `/v0/aircraft/{hex}?callsign={cs}` returns aircraft and route in one call | no | no |
| Rate limit / terms | README states none. MIT code, but route data (David J Taylor, Jim Mason) "may not be copied, published or incorporated into other databases" without permission. Data from PlaneBase, airport-data.com | None stated; says free, no key. Data courtesy of PlaneBase, Jim Mason, Steve Hibberd, ip2location | Source is VRS standing data; adsb.lol is ODbL |
| AAAA / cert | Yes `2a01:4f8:1c1c:ff7f::1`; ECDSA P-256, 4 certs sent | Yes (Cloudflare); ECDSA P-256, 3 certs; replies have `cache-control: max-age=14400` | see adsb.lol |

Sources read: https://github.com/mrjackwills/adsbdb, https://hexdb.io/, live responses above.

### 3. Where does the route come from, and how accurate is it?
- ADS-B carries no origin or destination. Every route here is a lookup from callsign (not hex): a table of "callsign -> origin-destination" built by volunteers from schedules and observed flights (VRS standing data, Jim Mason / David Taylor / PlaneBase).
- Accuracy: good for scheduled airline flights that reuse a callsign every day (UAE6, RYR26DG returned a plausible answer). Wrong or absent for charter, cargo, business jets, GA (callsign = registration), military, and airlines that reuse flight numbers on varied legs or multi-leg flight numbers. Table age is unknown (hexdb says updated 2026-11-21; one adsb.lol issue reports stale routes). A wrong route is a real risk: show nothing when there is no match and consider checking that the plane's position is near the great circle between origin and destination (cheap on a server, fiddly on the device).
- Callsign gives the airline: first 3 letters are the ICAO airline code (UAE -> Emirates). IATA flight number (EK6) is returned by adsbdb.
- Paid alternative: FlightAware `/flights/{ident}` ($0.005 per call). Only for rare lookups; personal tier gives about 1,000 lookups per month under the free credit. Not needed.
- Cache per callsign for hours (a route changes at most daily). With a cloud service the cache is shared and enrichment calls disappear from the device.

### 4. Static tables on the device flash (8 MB)
- Airlines: OpenFlights `airlines.dat` is 396,897 B uncompressed (checked via HTTP header, `https://raw.githubusercontent.com/jpatokal/openflights/master/data/airlines.dat`). It is old and partly stale. A trimmed table of 500-1000 airlines (ICAO 3 letters, IATA 2, 7-char short name) is about 20-30 KB.
- Airports: OurAirports `airports.csv` is 12.7 MB (`https://davidmegginson.github.io/ourairports-data/airports.csv`, public domain per the project, unverified here). Keep only large and medium airports with an IATA code: about 4,000 rows x 40 B = about 160 KB. Fits easily in LittleFS or PROGMEM.
- Aircraft types: ICAO type code -> short name ("B38M" -> "737 MAX 8") is a table of 300-500 rows, about 10 KB. Build it from the ICAO type designators list (data source for the table not checked; unverified).
- Verdict: static tables in flash are fine and make the device work even if the enrichment API is down, but a cloud service keeps them current without reflashing. Best design: cloud returns names; device holds a small flash table as fallback.

## 5. Airline logos (16x16)
- No free API with a clear licence for use as bitmap data was found. All logos are trademarks of the airlines.
- Candidate collections (verify licence files yourself):
  - spydogenesis/airlines-logo (GitHub, by IATA code, served via jsDelivr; licence not seen).
  - bchoubert/airline-logos (880+ airlines, by ICAO code; licence null in metadata).
  - soaring-symbols (npm, MIT code, SVG, by IATA code; the README says the logos remain the airlines' IP, "informational and reference purposes only").
  - Wikimedia Commons logos: many are PD-textlogo, others are not free; check each file. Airhex and Duffel are paid.
- For a one-device non-commercial hobby display the trademark risk is low, but do not redistribute a public bitmap pack. Keep the generated bitmaps private.
- Producing 16x16: do it offline once. Take each SVG or PNG, crop to the tail or symbol (full logos with text are unreadable at 16 px), resize with area averaging, quantise to a 16-colour palette or RGB565, check by eye and fix by hand for the top 100-200 airlines. Store as 512 B each in RGB565 (16x16x2), about 100 KB for 200 airlines, plus a fallback generic plane icon and a coloured square (airline brand colour) for the rest. The cloud service can return the bitmap by ICAO code (base64, 512 B) if you prefer not to reflash.

## 6. Where each data type should be fetched
| Data | Direct or cloud |
|---|---|
| Position/radius list | Cloud preferred (one RSA-cert call, JSON cut to about 60 B/plane, server filters by radius and de-duplicates by hex, holds feeder key, backs off). Direct to adsb.fi works but costs 2.3 s TLS plus up to tens of KB |
| Route (callsign) | Cloud (cache, validate, merge adsbdb and hexdb) |
| Airline name, type name, airport names | Cloud, with a flash table fallback |
| Logos | Cloud or flash; offline conversion either way |

## 7. Fallback chain
1. Cloud API (adsb.fi primary, adsb.lol secondary with a feeder key if you set up a feeder; or adsb.lol keyless if your own poll count stays under 500/day).
2. If the cloud is unreachable: device direct to adsb.fi `/api/v3`, then adsb.lol (note the 500/day cap).
3. Enrichment fallback chain on the cloud: adsbdb -> hexdb -> flash tables.

## Unverified or not done
- airplanes.live API behaviour: my 403 may come from IP or user-agent; terms taken from a search summary and a third-party page; official `/api-guide` returned a Cloudflare challenge.
- ADS-B Exchange endpoint details and the $10/10,000 price come from a search summary of the official developer page; I did not open the RapidAPI listing.
- adsb.lol routeset output (201, empty body) and whether `desc` appears for adsb.lol (not in my sample).
- TLS session resumption on the candidate hosts; RSA/ECDSA key types may change at certificate renewal.
- Response sizes are for a quiet area (4 planes at 100 km); no 250 km test was run.
- OurAirports licence text and the ICAO type list source were not read.
