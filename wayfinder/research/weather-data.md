# Weather data source for the LED display (research, 2026-11-21)

## Summary / recommendation
- Best: **Open-Meteo** (free, no key, one ~0.95 KB JSON response with every field). Fetch it from the owner's cloud API, which returns a ~100-byte payload over the RSA host.
- Why via cloud: api.open-meteo.com has an ECDSA cert (about 2.3 s handshake) and **no AAAA record**. Open-Meteo also serves plain HTTP (verified HTTP 200), so direct HTTP is a possible device fallback.
- Fallback: **MET Norway locationforecast** (no key, RSA cert, no AAAA). It returns 39 KB, so only the cloud service can call it. It has no daily high/low or rain probability, which the service must compute.
- Direct from the device, the only reasonable choice is Open-Meteo, over HTTP (no TLS cost) or HTTPS (2.3 s, IPv4 only).
- Exact request (all values arbitrary: lat 48.0, lon 10.0):
  `https://api.open-meteo.com/v1/forecast?latitude=48.0&longitude=10.0&current=temperature_2m,weather_code&hourly=temperature_2m,weather_code&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max&forecast_hours=7&forecast_days=1&timezone=auto`
- Caveat: the free tier is non-commercial only (fine for one hobby device) and needs CC BY 4.0 attribution, for example a "Weather data by Open-Meteo.com" line in the project README.

## Measured table
| Source | Key | AAAA | Cert leaf / chain | HTTP | Size for our fields |
|---|---|---|---|---|---|
| Open-Meteo | no | **no** | ECDSA P-256, 4 certs (LE YE2, Root YE, ISRG X2) | yes (200) | 948 B raw, 406 B gzip |
| MET Norway | no | **no** | RSA-2048, 3 certs (GEANT RSA, HARICA root) | 301 to HTTPS | 38,973 B raw, 2,834 B gzip |
| OpenWeatherMap | yes | **no** (CNAME eu-api only) | RSA-2048, 4 certs (Sectigo) | unverified | not fetched (no key) |
| WeatherAPI.com | yes | yes (via CDN) | RSA-2048, 4 certs (LE YR2) | unverified | not fetched |
| Visual Crossing | yes | **no** | ECDSA P-256, 4 certs | unverified | not fetched |
| Tomorrow.io | yes | yes | ECDSA P-256, 3 certs (GTS) | unverified | not fetched |
| Meteoblue | yes | yes | ECDSA P-256, 3 certs (GTS) | unverified | not fetched |
| Pirate Weather | yes | **no** | RSA-2048, 3 certs (Amazon) | unverified | not fetched |
| NWS | no | yes | RSA-2048, 3 certs | unverified | not fetched |
| wttr.in | no | **no** | RSA-2048, 3 certs (LE YR1) | yes (200) | 39.5 KB (`format=j1`); 9 B for `?format=%t+%C` |

All TLS and DNS data came from `dig AAAA` and `openssl s_client -showcerts` run on 2026-11-21. Sizes come from real GETs, except the key-gated APIs, which I did not call.

## Open-Meteo (https://open-meteo.com/en/docs, /en/pricing)
1. Request as above. Real response: 948 B raw, 406 B gzip. Sample (2026-11-21 23:45 local):
   `current:{temperature_2m:2.7,weather_code:45}`, `hourly.temperature_2m:[3.0,2.6,2.7,3.3,...]` and `hourly.weather_code:[2,45,45,45,...]` (7 hourly values, index 0 is the current hour), `daily:{temperature_2m_max:[6.4],temperature_2m_min:[2.6],precipitation_probability_max:[88]}`.
   - `forecast_hours=7` counts from the current hour, so +3 h is index 3 and +6 h is index 6.
   - `timezone=auto` makes "today" the location's local day. For the high/low to cover the whole local day, check that forecast_days=1 still returns the full day. It did here.
   - The docs say rain probability comes from ensemble models (GFS, ICON, ECMWF IFS ensembles). They do not state worldwide availability. Unverified outside Europe.
2. Free tier: 600/min, 5,000/h, 10,000/day, 300,000/month (monthly not enforced yet). 2 calls/h is far below this. Non-commercial only. License is CC BY 4.0, so attribution is required. No uptime guarantee.
3. No key, no account, free.
4. No AAAA. Leaf cert ECDSA P-256 under an all-ECDSA chain of 4 certs. This is the worst case for the ESP32 (about 2.3 s).
5. `weather_code` is a WMO code. 0-1 clear, 2 partly cloudy, 3 overcast, 45/48 fog, 51-67 and 80-82 rain/drizzle, 71-77 and 85-86 snow, 95-99 thunderstorm. Icon map:
   - Sun: 0-1.
   - Part-cloud: 2.
   - Cloud: 3, 45, 48.
   - Rain: 51 and above (drizzle, rain, snow, thunderstorm).
   - If one of the four icons should be "snow" or "storm", split on 71-77/85-86 and 95-99.
6. Plain `http://` works (HTTP 200) with no redirect, so a fallback without TLS is possible. The data is unauthenticated, but the risk is low for a display. Gzip is supported. The service runs on its own servers and has no SLA. Accuracy is model-based. Day/night is not in the code, so use `is_day` if needed.

## MET Norway locationforecast 2.0 (https://developer.yr.no/doc/TermsOfService/)
1. `https://api.met.no/weatherapi/locationforecast/2.0/compact?lat=48.0&lon=10.0` returns 89 hourly timeseries entries, 38,973 B raw and 2,834 B gzip. The `compact` variant is already the smallest, and there is no field selection, so the device cannot parse this.
   - Per entry: `air_temperature`, `next_1_hours.summary.symbol_code` (for example `partlycloudy_night`), and `next_6_hours`/`next_12_hours` summaries with `precipitation_amount`.
   - No rain probability is exposed in `compact`. Daily high/low must be computed from the series (cloud service only).
2. Terms (read): a unique identifying User-Agent is required, ideally with a contact. Cache until the `Expires` header (last response Expires about 8 minutes ahead) and use If-Modified-Since. Coordinates must be at most 4 decimals (5+ decimals can return 403). CC BY 4.0 attribution is required. The page does not mention commercial use. Ceiling is 20 req/s per application (429 when throttled).
3. No key, free.
4. No AAAA. RSA-2048 leaf, 3-cert chain (about 0.4 s handshake).
5. `symbol_code` strings: `clearsky`, `fair`, `partlycloudy`, `cloudy`, `rain`, `lightrain`, `heavyrain`, `snow`, `fog` and others, with `_day`, `_night` or `_polartwilight` suffixes. Strip the suffix and match the prefix.
6. HTTP gets a 301 to HTTPS, so HTTPS is required. Very reliable (national met service). Excellent in Europe, global coverage is model-based.

## OpenWeatherMap (https://openweathermap.org/price)
1. Free plan includes Current Weather and the 3-hour 5-day forecast (`/data/2.5/forecast`, 40 entries, tens of KB). One Call 3.0 is a separate pay-per-call product with 1,000 calls/day free; I could not confirm whether a card is required. Unverified: exact responses and field selection (there is no daily high/low or rain-probability field in the free plan except `pop` per 3-hour block).
2. 60 calls/min and 1,000,000/month. Terms and attribution not read. Unverified.
3. Key required (account). Unauthenticated test returned 401 `Invalid API key`.
4. No AAAA. RSA-2048 leaf, 4-cert chain.
5. Numeric condition id (2xx storm, 3xx drizzle, 5xx rain, 6xx snow, 7xx fog, 800 clear, 801-802 partial, 803-804 cloud). The mapping is easy.
6. Not recommended: the payload is large, no IPv6, and it needs a key.

## WeatherAPI.com (https://www.weatherapi.com/pricing.aspx)
1. `/v1/forecast.json?key=KEY&q=48.0,10.0&days=1` includes current, day high/low and `daily_chance_of_rain` plus hourly. Unverified size (large without the `hour` filter; a `hour=` parameter exists, unverified).
2. Free: 100,000 calls/month, 3-day forecast, hourly. The page says commercial light use is allowed. A link back is requested.
3. Key and account required, free.
4. AAAA yes (CDN). RSA-2048 leaf, 4-cert chain (about 0.4 s).
5. Numeric `condition.code` (1000 clear, 1003 partly cloudy, 1006/1009 cloudy, 1063+ rain, ...). Mapping is straightforward.
6. HTTP is probably supported (unverified). A credible fallback if a key is acceptable. It is the only candidate with IPv6 and RSA together.

## Visual Crossing (https://www.visualcrossing.com/weather-data-editions/)
1. Timeline API with `elements=` and `include=` filters can narrow the response. Unverified, not called (needs a key).
2. Free: 30,000 records/month. A daily cap exists but the page gives no figure. Commercial use is listed as allowed on the free plan. Attribution required: "Weather Data Provided by Visual Crossing" with a link.
3. Account and key needed.
4. No AAAA. ECDSA P-256 leaf, 4-cert chain (about 2.3 s).
5. `icon` strings (`clear-day`, `partly-cloudy-day`, `cloudy`, `rain`, ...) and `conditions` text.
6. Not recommended: ECDSA, no IPv6, and records are counted per hour and day.

## Tomorrow.io, Meteoblue, Pirate Weather, NWS, wttr.in
- **Tomorrow.io**: key required. Free limits and terms are not published on the pages I could read (the rate-limits page returned 404). AAAA yes. ECDSA P-256 leaf, 3-cert chain. Numeric `weatherCode`. Timeline endpoint with `fields=` is small. Unverified, so skip.
- **Meteoblue**: registration needed. The page offers a free API for one year ("without any obligation"), and quotas and non-commercial terms are not stated. AAAA yes. ECDSA leaf. Unverified, so skip because of the one-year limit.
- **Pirate Weather**: key signup. About 10,000 calls/month free (docs imply it; terms, attribution, coverage unverified). The data comes from NOAA models (GFS, HRRR, NBM). No AAAA. RSA-2048 leaf, 3-cert chain (about 0.4 s). Dark-Sky-like JSON with an `exclude=` parameter and an `icon` string. A workable keyed fallback, but small-account service and global quality is unverified.
- **NWS (api.weather.gov)**: US only, so it fails the requirement. No key. A User-Agent is required (read). AAAA yes. RSA-2048 leaf. Not usable.
- **wttr.in**: unofficial service built on other upstream data, with no guarantees or documented limits. `?format=%t+%C` gave `+2°C Fog` in 9 bytes, but it has no today high/low, no rain chance and no +3 h/+6 h values. `format=j1` is 39.5 KB. No AAAA. Plain HTTP works. Not suitable.

## Direct vs cloud service
- Device cost per Open-Meteo HTTPS refresh: about 2.3 s of handshake, plus parsing ~950 B. At one refresh every 15 to 30 minutes this is acceptable but ties up the radio and the main loop.
- Cloud advantages: it can hold an RSA cert and keep a warm connection (about 0.4 s), serve over IPv6, call MET Norway (which needs the 39 KB handling, caching by `Expires`, and a proper User-Agent), merge sources with failover, map WMO codes to the 4 icons, and return about 100 B such as `{"t":2.7,"c":2,"hi":6.4,"lo":2.6,"p":88,"t3":[3,2],"t6":[3,2]}`.
- Per-device polling stays tiny, and all upstream rate limits and attribution are handled in one place.
- The cloud service does not need to be commercial: one device polling every 15 minutes is well under the Open-Meteo free limits.
- Direct fallback: if the cloud service is down, the device can use the exact Open-Meteo URL above with `http://`.

## Unverified items
- Rain-probability coverage outside Europe for Open-Meteo.
- Key-gated API response sizes (OpenWeatherMap, WeatherAPI, Visual Crossing, Tomorrow.io, Meteoblue, Pirate Weather), because I did not call them.
- HTTP-fallback behavior for the keyed APIs.
- Free-tier terms for Tomorrow.io, Meteoblue and Pirate Weather beyond the notes above.
