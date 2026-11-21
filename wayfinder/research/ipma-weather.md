# IPMA open data as weather source for the LED gadget

Researched 2026-11-21 with plain GETs (UA `ledmatrix-research`, ~25 requests total). Sizes are raw bytes unless marked gz.

## Summary
- IPMA has **no hourly forecast** in open data (the API doc page lists none; guessed `.../cities/hourly/` returns 404). Only a **daily** forecast, 5 days, per 35 district capitals/islands. So **+3 h / +6 h cannot come from IPMA**. Closest substitute: today's daily weather type plus tMin/tMax, or Open-Meteo hourly.
- Today's high/low, rain probability and condition: **yes**, from one 1152 B file per city (`cities/daily/{globalIdLocal}.json`), 324 B gzipped.
- Current temperature: **yes, but only through a heavy file**. `observations.json` is all stations x last 24 h = 841,982 B raw (73,894 B gz). There is no per-station endpoint. Not parseable on the ESP32, so it needs a cloud service to shrink it.
- api.ipma.pt is **IPv4 only (no AAAA)**, serves plain HTTP too, and uses an **RSA-3072 cert** (fast handshake, about 0.4 s class). The chain is 3 certs, root included.
- Coverage is Portugal only, including Madeira and Azores. Fallback is Open-Meteo.
- Terms: non-profit use is free but **IPMA must always be cited as the source**. No rate limit is stated.
- Recommendation: use a cloud service. The device calls one small endpoint, and the service merges IPMA (current, daily) with Open-Meteo (hourly). See the end.

## 1. Endpoints (base `https://api.ipma.pt/open-data/`)
Doc page read: https://api.ipma.pt/ (87 KB HTML, Portuguese).

| Purpose | URL (suffix) | Raw size | gz |
|---|---|---|---|
| Daily forecast, one city, 5 days | `forecast/meteorology/cities/daily/1110600.json` (Lisboa) | 1,152 B | 324 B |
| Daily forecast, all 35 locations, one day | `forecast/meteorology/cities/daily/hp-daily-forecast-day0.json` (day1, day2 also exist; day1 = 6,334 B) | 6,815 B | 836 B |
| Locations list (districts/islands, IDs, lat/lon) | `distrits-islands.json` (spelled this way in the API) | 10,071 B | 1,315 B |
| Weather type descriptions | `weather-type-classe.json` (at root; `configs/...` is 404) | 4,954 B | n/a |
| Observations, all stations, last 24 h hourly | `observation/meteorology/stations/observations.json` | 841,982 B | 73,894 B |
| Station list (GeoJSON, 227 stations) | `observation/meteorology/stations/stations.json` | 49,864 B | 6,240 B |
| Hourly forecast | none found (`.../cities/hourly/` is 404; no such service on the doc page) | | |

The server honours `Accept-Encoding: gzip`. Responses carry `Last-Modified`, `ETag` and `Access-Control-Allow-Origin: *`, so conditional GETs are possible. Other services exist (warnings, UV, fire risk, sea, seismic) but are not needed.

Daily file fields (verified): `forecastDate`, `tMin`, `tMax` (strings), `precipitaProb` (percent string), `idWeatherType`, `predWindDir`, `classWindSpeed`, `classPrecInt`, `latitude`, `longitude`, top-level `globalIdLocal`, `dataUpdate`. In the all-locations files tMin/tMax are integers.

## 2. Fields
- **Current temperature**: `observations.json`, `{ "YYYY-mm-ddThh:mi": { "{idEstacao}": {temperatura, humidade, pressao, intensidadeVentoKM, precAcumulada, ...} } }`. 24 hourly keys, 227 station slots each. The doc says refresh is hourly and times are UTC. Latest key at fetch time was 22:00 (fetched ~23:00 UTC). In the latest slot 57 of 227 stations were `null`, and 162 had a real temperature, so some stations are missing. `-99.0` means no data. It is per-all-stations only, with no filter parameter. Expect an age of 0 to 2 h.
- **Today's high/low**: `tMin`, `tMax` of the `forecastDate == today` entry. These are forecast values, not observed extremes. Note that the first entry is still "today" late in the evening.
- **Rain probability**: `precipitaProb`, a daily percent (0 to 100), not hourly.
- **+3 h / +6 h**: not available. A weak substitute is the daily type and tMax/tMin. Use Open-Meteo hourly instead (section 5).

## 3. Mapping lat/lon to a location
- Forecast: 35 locations in `distrits-islands.json` (18 mainland district capitals plus Madeira: Funchal, Porto Santo; Azores: Vila do Porto, Ponta Delgada, Angra do Heroísmo, Santa Cruz da Graciosa, Velas, Madalena, Horta, Santa Cruz das Flores, Vila do Corvo). Each has `globalIdLocal`, lat, lon, name and `idAreaAviso`. Choose the nearest by haversine distance. Mainland ones are only district capitals, so the forecast can be about 50 km or more from the user, and only coarse regional detail is available.
- Observations: 227 stations (all in Portugal, including islands), nearest by distance, skipping `null` and -99 values.
- Flash: 35 rows of (id, lat, lon) is about 35 x 12 B = about 0.5 KB. Adding names is about 1 KB. 227 stations (id, lat, lon) is about 2.7 KB, or 5 KB with names. Both fit easily in flash. The better design is for the cloud service to hold the tables and do the mapping, so the device only sends lat/lon.
- Use only well-known IDs for tests: Lisboa 1110600, Porto 1131200 (unverified, I only requested Lisboa and the all-locations files).

## 4. idWeatherType (from weather-type-classe.json)
-99 no data; 0 no info; 1 clear; 2 partly cloudy; 3 sunny intervals; 4 cloudy; 5 cloudy (high cloud); 6 showers/rain; 7 light showers/rain; 8 heavy showers/rain; 9 rain/showers; 10 light rain; 11 heavy rain/showers; 12 intermittent rain; 13 intermittent light rain; 14 intermittent heavy rain; 15 drizzle; 16 mist; 17 fog; 18 snow; 19 thunderstorms; 20 showers and thunderstorms; 21 hail; 22 frost; 23 rain and thunderstorms; 24 convective clouds; 25 partly cloudy; 26 fog; 27 cloudy; 28 snow showers; 29 and 30 rain and snow.

The codes are the same by day and night, and the code does not encode night. IPMA ships day (`w_ic_d_NN.svg`) and night (`w_ic_n_NN.svg`) icon sets, so night is chosen by the client.

Suggested 4-icon mapping (my own choice):
- Sun: 1 (night: moon variant if available), also 3.
- Part cloud: 2, 3, 24, 25.
- Cloud: 0, -99, 4, 5, 16, 17, 22, 26, 27.
- Rain: 6 to 15, 18, 19, 20, 21, 23, 28, 29, 30. Snow, hail and thunder have no icon, so they fall in rain.

## 5. Coverage and fallback
Only Portugal. `country: "PT"` is in every file. There is no endpoint for coordinates outside Portugal, so the nearest-city lookup would return a wrong far-away city unless the service enforces a distance limit (suggest a limit of about 60 km mainland, with a bounding box for islands). Outside that, use **Open-Meteo** (current, hourly and daily in one request, no key, non-commercial free). I did not fetch Open-Meteo or read its terms (unverified here). Its terms require attribution (unverified). Using it for all of Portugal as well is an option, since it covers +3 h and +6 h, which IPMA cannot.

## 6. Terms
Read: https://api.ipma.pt/ section "termos e condições / Condições de Utilização" (https://www.ipma.pt/pt/siteinfo/termosutilizacao.html is a 404 and no separate licence page was found).
- Users may copy and use information and symbols free of charge, for personal or public use, **as long as there are no profit-making or offensive purposes**. The user **must always cite the source** (IPMA); for documents the doc text says to link to the IPMA site. No named open licence (no CC) was found.
- Data is "general and indicative" with no guarantees. IPMA may change or restrict access without notice and may block abusive users.
- No numeric rate limit is stated. Files refresh hourly, so poll every 30 to 60 min at most.
- A hobby device fits the non-profit condition. Show "IPMA" on the device or in a README.

## 7. Network and TLS
- `dig AAAA api.ipma.pt` returned nothing. A returns 193.137.20.123. The device would need IPv4 for this host.
- Certificate: leaf `CN=*.ipma.pt`, **RSA 3072**, sha256WithRSA. Chain served: leaf, GEANT TLS RSA 1 (RSA 3072), HARICA TLS RSA Root CA 2021 (RSA 4096, root sent too). Negotiated TLS 1.2, ECDHE-RSA-AES256-GCM-SHA384. RSA-3072 will be somewhat slower than the measured RSA-2048 0.4 s (unverified, expected under 1 s); ECDHE key exchange still has no acceleration. Pinning the HARICA root is possible, with a 4 KB cert in flash.
- Plain HTTP on port 80 works (`http://api.ipma.pt/...` returned 200 and the same bytes, no redirect). Fine for public data (the device saves the handshake) but anyone could alter the data, so prefer HTTPS if the time allows.
- Server: nginx; `Content-Length` given, keep-alive.

## 8. Reliability and caveats
- Daily forecast `dataUpdate` was 22:31 on 2026-11-21 (hourly refresh per docs). Last-Modified matches.
- Observation holes (null stations, -99 fields) are common; pick the nearest valid station and fall back to the next nearest.
- Times are UTC, and the forecast date is Lisbon-local, so handle summer time. Azores are UTC-1.
- Typos and oddities in the API (`distrits-islands`, "ligth") mean the schema is unversioned. Parse defensively. Types can differ (tMin string vs int).
- No SLA; IPMA reserves the right to change or remove the service.

## Recommendation
Use a small cloud API. The device sends lat/lon (or a saved key) and receives about 150 B of compact JSON.
1. **Cloud service, every 30 min**: fetch `observations.json` (74 KB gz) and keep only the latest valid temperature for the nearest station (fall back to the next nearest if it has -99/null). Fetch `cities/daily/{id}.json` (324 B gz) for tMin/tMax, precipitaProb and idWeatherType.
2. **+3 h / +6 h and any location outside Portugal**: Open-Meteo hourly (current temperature plus `weather_code` and precipitation probability). Map to the 4 icons in the service. Optionally use Open-Meteo for everything so there is one code path, and IPMA only as the primary source in Portugal.
3. Output shape: `{t, ic, hi, lo, rain, t3, ic3, t6, ic6, src}`. `src` carries the attribution ("IPMA" or "Open-Meteo") for the display.
4. Direct from the device only a fallback: `cities/daily/{id}.json` (1152 B, or 324 B gz) is small enough, but current temperature and hourly are not. RSA-3072 handshake, IPv4 only.
5. Show the source credit "IPMA" on the device or in the README to comply with the terms.
