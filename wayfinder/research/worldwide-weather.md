# Worldwide weather: which provider outside Portugal? (research for ticket 41, 2026-11-26)

## Question

Weather works for Portugal only today: the service answers `GET /v1/weather` from IPMA and returns HTTP 422 anywhere else
(`errOutsideCoverage`, 150 km from the nearest of 35 IPMA forecast places). Which free provider should the Go service use outside
Portugal, how does it choose between the two, and what has to change around it (attribution, the board's error handling)? The
provider must supply what the three weather screens show: the current temperature and a condition for one of the four icons
(sun, part, cloud, rain), the temperature and icon for +3 h and +6 h (the board writes the hour labels itself, `21h 00h 03h`,
from its own clock), today's high and low, and the chance of rain.

Everything below was checked on 2026-11-26 with real calls (17 places for Open-Meteo, 7 for MET Norway) and by reading the
terms pages. Where a statement was not verified, it says so.

## Answer in five lines

1. Use **Open-Meteo** (free API, no key) for every location that IPMA does not cover; keep IPMA inside Portugal (the owner's decision).
2. One call of 880 bytes (402 gzipped, cost 1.0 against the free limits) gives every field natively: current temperature and WMO code, hourly temperature and code for the +3 h / +6 h slots, the high, low and highest rain chance of the **local** day, with the time zone resolved by `timezone=auto`. Nothing has to be computed except rounding and the code-to-icon table.
3. The terms fit: free for non-commercial use, and they name "Personal home automation purposes"; under 10,000 calls a day, 5,000 an hour, 600 a minute (a display asks twice an hour per place); CC BY 4.0 attribution, a link "Weather data by Open-Meteo.com". The one thing to remember: **commercial or advertising-funded use needs their paid plan**.
4. MET Norway is the runner-up (any use allowed, CC BY 4.0 / NLOD), but it has no high and low for the part of today that has passed, no chance of rain outside the Nordic countries, no time zone, a mandatory User-Agent with contact details, and a 38 KB answer (19 times the parse cost). 7Timer!, WeatherAPI.com and OpenWeatherMap lose on terms, keys or missing fields.
5. The service picks IPMA when the point is inside Portugal's outline (mainland polygon, Madeira and Azores boxes) **and** within 100 km of an IPMA place, otherwise Open-Meteo. The present "nearest place within 150 km" rule is wrong at the border (it answers Vigo, Badajoz and Huelva with Portuguese cities). The 422 path disappears for every real location, the firmware needs no change, and the one visible gap is snow, which both providers' codes map to the "Rain" icon and text (an owner decision, below).

## Comparison table

Two tables: terms and operation, then fields. "Not called" means the provider needs a key, so the column rests on the vendor's pages
and on `wayfinder/research/weather-data.md` (2026-11-21), not on a call I made.

### Terms and operation

| | Open-Meteo (free API) | MET Norway locationforecast 2.0 | 7Timer! civil | WeatherAPI.com | OpenWeatherMap | Pirate Weather |
|---|---|---|---|---|---|---|
| Key | none (a key exists only for the paid `customer-api` host) | none, but a **User-Agent that identifies the app and a way to contact the owner is mandatory** | none | key | key | key |
| Free personal use | yes, non-commercial; "Personal home automation purposes" is an example | yes, any use (no non-commercial clause; NLOD 2.0 and CC BY 4.0) | yes, non-commercial only | yes (personal or commercial per the terms) | yes, free plan | free tier (10,000 calls a month implied); terms not found |
| Limits | 600/min, 5,000/h, 10,000/day (300,000/month listed, "not enforced"); per IP and application | 20 requests/s per application; do not repeat before `Expires` (observed 30 min); 429 when throttled | none stated | 100,000 calls a month (the earlier research note; not re-checked); cache current data at most 60 min, forecast at most 24 h | 60/min, 1,000,000/month | about 10,000/month |
| Attribution | CC BY 4.0, link "Weather data by Open-Meteo.com" next to displayed data | CC BY 4.0: credit, link to the licence, say if changed; suggested «Data from MET Norway» | developers asked to notify the author | "credit WeatherAPI.com by name or brand logo" | not on the pricing page (not checked further) | not found |
| Global | yes (17 places answered, both poles, longitude 180) | yes; Nordic locations updated hourly, the rest of the world every 6 h | yes (GFS, 3 hourly) | yes | yes | yes (NOAA models) |
| SLA | none ("uninterrupted provision ... not guaranteed") | none ("no guarantees of delivery ... or possibilities to obtain an SLA") | none, a one-person research site | plans | plans | none |
| Verdict | **recommended** | runner-up | no (no current temperature, no high/low, no rain chance, hobby site) | no (a key to keep, link-back, caching limits, extra account) | no (a key; no daily high/low or probability except per 3 h) | no (key, terms unclear, small service) |

### Fields the screens need

| Screen field | Open-Meteo | MET Norway (compact or complete) | 7Timer! |
|---|---|---|---|
| Current temperature | `current.temperature_2m`, 15-minute model value | first `timeseries` entry, `air_temperature`, the model's value for the hour (can be hours old outside the Nordic countries) | none (3-hourly forecast steps only) |
| Current condition | `current.weather_code` (WMO) | `next_1_hours.summary.symbol_code` (a string with `_day`, `_night`, `_polartwilight` suffix) | `weather` (15 strings) |
| +3 h and +6 h | `hourly.temperature_2m` and `hourly.weather_code` at the hour 3 and 6 after the current hour | entries 3 and 6 hours on (hourly for about 59 hours, then every 6 hours) | steps at multiples of 3 h from the model start, integer degrees |
| Today's high and low | `daily.temperature_2m_max` and `_min`, the **whole** local day | **not offered**; computed from the future entries only, so the hours of today that have passed are missing (Tokyo at 10:00 local: low 8.2 from MET, 4.1 from Open-Meteo; Sao Paulo at 22:00: three points) | none |
| Chance of rain | `daily.precipitation_probability_max` for every place tested (also mid-Atlantic, poles) | `probability_of_precipitation` only in the 60 KB `complete` answer and only for Nordic places (Oslo yes; Madrid, Tokyo, Sao Paulo, Kolkata, Auckland, mid-Atlantic: none); the `compact` answer has none anywhere, Oslo included | none (`prec_amount` only) |
| Time zone | `timezone=auto` resolves it; `timeformat=unixtime` gives exact instants and the daily date as the local midnight | UTC only; the service would need a coordinate-to-zone lookup it does not have (no dependency in `service/go.mod`) | UTC from the model start |
| Answer size | 880 bytes raw, 402 gzipped (request below) | 38,432 to 38,931 bytes raw, 2,410 to 2,866 gzipped (`compact`); `complete` 60 KB | 15,151 bytes raw (1,111 gzipped) |
| Parse cost in Go (M1 Max, `encoding/json` into a typed struct with only the needed fields) | 13 microseconds, 1.2 KB, 49 allocations | 250 microseconds, 18 KB, 393 allocations | not measured |
| Cost against the limits | `X-RateLimit-Cost: 1.0` (header seen); fewer than 10 variables | one request | one request |

Current temperatures at the same moment (01:06 UTC), Open-Meteo against MET Norway: Madrid 6.1 / 5.9, Tokyo 9.8 / 10.3,
Sao Paulo 21.0 / 21.1, Kolkata 17.8 / 17.8, Auckland 19.1 / 18.9, mid-Atlantic 27.0 / 26.8. The +3 h and +6 h temperatures differ by
at most 1.9 degrees (Tokyo +6 h: 13.6 / 11.7). Both build on the same global models (ECMWF IFS, DWD ICON, NOAA GFS) where there is no
regional model, so this is agreement, not proof of accuracy. I found no independent verification study and did not run one.

## Terms with quotes

### Open-Meteo

Terms of use, https://open-meteo.com/en/terms (read from the page's HTML on 2026-11-26):

- "By using the Free API for non-commercial use you agree to following terms: Less than 10'000 API calls per day, 5'000 per hour and 600 per minute."
- "You may only use the free API services for non-commercial purposes."
- "You accept to the CC-BY 4.0 licence, as specified in the licence conditions."
- "We reserve the right to block applications and IP addresses that misuse our service without prior notice."
- "Rate limits are applied by IP addresses and application."
- "Non-commercial use is defined as elaborated by creative commons. To make it easier to understand, here are some examples that qualify as non-commercial use: Private or non-profit websites or apps that do not have subscriptions or advertising. Personal home automation purposes. ... Voluntary donations, including contributions through a tip jar, are permitted."
- "... the following examples are considered commercial use: Websites or apps that have subscriptions or display advertisements. Commercial products or promotional activities."
- "Open-Meteo assumes no responsibility for any inaccuracies or omissions in the data, their availability, or any losses or damages resulting from their usage. The accuracy and completeness of the data products and their uninterrupted provision are not guaranteed by Open-Meteo, and they are provided without any warranty, either statutory or otherwise."
- Privacy (same page): "For troubleshooting purposes, we keep webserver log files that may contain sensitive information such as geographical coordinates. We do not share this data with any third party. All log files will be deleted after a period of 90 days."

Licence page, https://open-meteo.com/en/licence:

- "API data are offered under Attribution 4.0 International (CC BY 4.0)".
- "Attribution: You must give appropriate credit, provide a link to the licence, and indicate if changes were made. You may do so in any reasonable manner, but not in any way that suggests the licensor endorses you or your use."
- "You must include a link next to any location Open-Meteo data are displayed, for example: `<a href="https://open-meteo.com/">Weather data by Open-Meteo.com</a>`".

Pricing page, https://open-meteo.com/en/pricing:

- "The free API is for non-commercial use, rate-limited to 10,000 calls/day, and carries no uptime guarantee."
- "Requests for data covering more than 10 weather variables or extending over a period of more than 2 weeks for a single location are considered multiple API calls." (The request below has 7 variables and one day.)
- The key belongs to the paid plan: "The key grants access to customer-api.open-meteo.com", and in the API docs, https://open-meteo.com/en/docs: "apikey ... Only required to commercial use to access reserved API resources for customers." The free host needs no key (my calls had none, and the answer headers carry `X-RateLimit-Limit: 600.0`, `X-RateLimit-Window: 60`, `X-RateLimit-Remaining`, `X-RateLimit-Reset`).

Reading for this project: a display on the owner's own service, no subscriptions, no advertising, is "Private ... apps that do not have subscriptions or advertising" and
"Personal home automation". Someone who runs the same open-source service for paying customers is a commercial user and needs the paid
plan (a one-variable change: the base URL and an `apikey` parameter). This belongs in the README next to the existing non-commercial
notice for adsb.fi.

Docs, https://open-meteo.com/en/docs (relevant statements): "The requested timezone's UTC offset at request time is returned as utc_offset_seconds and used for ISO 8601 timestamps and daily boundaries throughout the response. Daylight saving time (DST) changes within the requested range are ignored. For DST-aware local times, request timeformat=unixtime and convert using the returned timezone in your application." and "Precipitation probability is always derived from ensemble models such as the GFS, ICON and ECMWF IFS ensembles." and for the variable: "Probability of precipitation with more than 0.1 mm of the preceding hour. Probability is based on ensemble weather models with 0.25° (~27 km) resolution."

### MET Norway

Terms of service, https://developer.yr.no/doc/TermsOfService/ (read from the HTML):

- "All requests must (if possible) include an identifying User Agent-string (UA) in the request with the application/domain name, optionally version number. You should also include a company email address or a link to the company website where we can find contact information. If we cannot contact you in case of problems, you risk being blocked without warning."
- "Do not ask too often, and don't repeat requests until the time indicated in the Expires response header. Cache data locally and and use the If-Modified-Since request header to avoid repeatedly downloading the same data."
- "When using requests with latitude/longitude, truncate all coordinates to max 4 decimals. ... For new products, requests with 5+ decimals will return a 403 Forbidden."
- "Anything over 20 requests/second per application (total, not per client) requires special agreement."
- "Clients which don't follow the terms are liable to being throttled. This means requests will start getting a 429 HTTP status code instead of any content."
- "Deliberate breach of our TOS ... will result in a permanent ban in the use of our services. This includes fake or random strings as identification in the User-Agent header."
- "There are no guarantees of delivery regarding this service, or possibilities to obtain an SLA."
- "All open data require attribution as specified in the CC BY 4.0 license: You must give appropriate credit, provide a link to the license, and indicate if changes were made."

Licence page, https://api.met.no/doc/License: "Unless specified otherwise, all data and products are licensed under the Norwegian Licence for Open Government Data (NLOD) 2.0 and Creative Commons 4.0 BY International licences. Credit should be given to The Norwegian Meteorological Institute, shortened MET Norway, as the source of data. Some suggestions: «Data from MET Norway», «Based on data from MET Norway»."

Product page, https://api.met.no/weatherapi/locationforecast/2.0/documentation: "Data for locations in the Nordic region are updated every hour; for the rest of the world every 6 hours."

The service's current User-Agent is `ledmatrix-api/0.1` (upstream.go), which has no contact. MET would need a second, configurable
User-Agent carrying the owner's e-mail or web address, read from the environment and never stored in the repository.

### The others

- 7Timer!, http://www.7timer.info/doc.php?lang=en: "The weather data distributed in this site are entirely free - you can use or redistribute them as long as you are not using them for commercial purpose." "They can be used directly without registering an API key." Data "mainly derived from the NOAA/NCEP-based numeric weather model, the Global Forecast System (GFS)". No rate limit and no licence named.
- WeatherAPI.com, https://www.weatherapi.com/terms.aspx: "If you are a Free API user then for all uses of the data, you will credit WeatherAPI.com by name or brand logo as the source of the data." "You may access, view and make copies of the data in the API for your personal or commercial use". "You may cache API responses for performance optimisation subject to the following limits: current conditions data - maximum 60 minutes; forecast data - maximum 24 hours". Needs a key.
- OpenWeatherMap, https://openweathermap.org/price: free plan "Current weather and forecasts 60 API calls/minute, 1,000,000 calls/month: Current Weather API, 3-hour Forecast (5 days)". Needs a key; no daily high or low and a chance of rain only per 3-hour step.
- Pirate Weather, https://docs.pirateweather.net/en/latest/: needs a key; the limit is only implied ("raise your API limit from 10,000 calls/ month to 20,000 calls per month"); no usage terms or attribution on the pages I could read. Not evaluated further.
- Visual Crossing, Tomorrow.io, Meteoblue, NWS (US only), wttr.in: see `weather-data.md`; none was re-checked.

## The icon mapping table

The display has four icons and texts: `sun` "Clear", `part` "Partly cloudy", `cloud` "Cloudy", `rain` "Rain"
(`iconKind`/`iconText` in `arduino/ledmatrix/screens.cpp`, `wxIcon` in `shared/renderer.js`). An unknown string falls to `cloud`.
There is no night variant, no snow, fog or storm. The existing IPMA map (`iconFor` in `service/weather.go`) sends everything wet or
stormy to `rain` and fog to `cloud`; the worldwide map follows the same convention. The Open-Meteo documentation lists 29 codes; the
table covers all of them and the function covers every other integer.

| Icon | Open-Meteo WMO code (name from the docs) | MET Norway `symbol_code` (suffix `_day`, `_night`, `_polartwilight` removed first) | IPMA `idTipoTempo` (existing) |
|---|---|---|---|
| `sun` | 0 Clear sky; 1 Mainly clear | `clearsky`, `fair` | 1, 22 |
| `part` | 2 Partly cloudy | `partlycloudy` | 2, 3, 5, 25 |
| `cloud` | 3 Overcast; 45 Fog; 48 Depositing rime fog | `cloudy`, `fog` | 0, -99, 4, 16, 17, 24, 26, 27 |
| `rain` | 51, 53, 55 drizzle; 56, 57 freezing drizzle; 61, 63, 65 rain; 66, 67 freezing rain; 80, 81, 82 rain showers | `lightrain`, `rain`, `heavyrain`, `lightrainshowers`, `rainshowers`, `heavyrainshowers` | 6 to 15 |
| `rain` (snow and sleet have no icon) | 71, 73, 75 snowfall; 77 snow grains; 85, 86 snow showers | `lightsnow`, `snow`, `heavysnow`, `lightsnowshowers`, `snowshowers`, `heavysnowshowers`, and the `sleet` family (`lightsleet`, `sleet`, `heavysleet`, `...sleetshowers`) | 18, 28, 29, 30 |
| `rain` (storm and hail have no icon) | 95 Thunderstorm; 96 with slight hail; 97 Heavy thunderstorm; 99 with heavy hail | every `...andthunder` name (`lightssleetshowersandthunder` and `lightssnowshowersandthunder` carry a typo in MET's own names, kept by MET for compatibility) | 19, 20, 21, 23 |

The function (it is enough to implement this, not a map of 29 entries; codes 4 to 44 are haze, dust and similar WMO codes, 50 and
above are all wet, so a code the docs add later, like the 97 the docs list now, lands in the right place by itself):

```go
func iconForWMO(c int) string {
    switch {
    case c <= 1:
        return "sun" // 0, 1 (a negative or missing code is handled by the caller as "cloud")
    case c == 2:
        return "part"
    case c < 50:
        return "cloud" // 3 overcast, 45/48 fog, anything unknown below 50
    default:
        return "rain" // 50-99: drizzle, rain, snow, showers, thunder
    }
}
```

Checked in a prototype (kept outside the repository) against the 29 documented codes: 0,1 -> sun; 2 -> part; 3,45,48 -> cloud; the
other 24 -> rain.

Two things to know about this mapping, both for the owner:

- **Snow shows as "Rain".** The text under the icon says "Rain" too. For IPMA this was rare; worldwide it is a normal winter
  picture (Moscow was snowing in my sample today: code 71 in the +3 h to +7 h slots). The fix is a `snow` icon and text, 4 characters
  (`LiveWeather::icon` is `char[6]`, so `snow`, `fog` and `storm` all fit), but it touches `art.h`, `screens.cpp`,
  `shared/renderer.js`, `tools/check_margins.py` and the preview, so it is its own slice, and the firmware must ship before the
  service starts sending `snow` (an old firmware shows an unknown icon string as "Cloudy"). The service would then map WMO 71 to 77 and
  85, 86, and IPMA 18, 28, 29, 30, to `snow`.
- **Open-Meteo's light drizzle (51, 56) is common and weak.** In the mid-Atlantic sample the code is 51 in two hours with a 1% to 2%
  chance of rain, and the `rain` icon looks wrong there. The IPMA map already sends drizzle (15) to `rain`, so I kept the same rule;
  a refinement (51 and 56 to `cloud` when the hourly chance is under 30%) would need `hourly=precipitation_probability`
  (8 variables, still one call). Decide after looking at it on the panel.

## How the service chooses

### The rule

```
Get(lat, lon):
  if ipmaCovers(lat, lon):            // inside Portugal's outline AND nearest IPMA forecast place <= 100 km
      out, err = ipma(lat, lon)       // exactly today's code
      if err == nil: return out
      // IPMA failed and the cache has nothing (not even a stale answer): fall through, do not answer 502
  return openMeteo(lat, lon)          // src "Open-Meteo", loc ""
```

`ipmaCovers` is two tests, both needed:

1. **Inside Portugal's outline.** One polygon for the mainland, one box for Madeira (latitude 32.2 to 33.3, longitude -17.4 to -16.1,
   Madeira and Porto Santo) and one for the Azores (latitude 36.8 to 39.9, longitude -31.5 to -24.9, all nine islands; no other
   land lies in either box). The Selvagens (30.1 N, 15.9 W) are Portuguese but have no IPMA place and are in no box, so they go to
   Open-Meteo.
2. **Nearest IPMA forecast place within 100 km** (`maxPlaceKm`, 150 today). This only guards against an outline that is wrong; on the
   mainland no Portuguese point I tried is farther than 79 km from a place (Barrancos).

Why a distance rule cannot do the job alone. I computed the distance to the nearest of the 35 places (`service/data/distrits-islands.json`)
for towns on both sides of the border:

| Point | In Portugal | Nearest IPMA place | km |
|---|---|---|---|
| Barrancos | yes | Beja | 79 |
| Melgaco | yes | Braga | 65 |
| Alcoutim | yes | Loule | 61 |
| Miranda do Douro | yes | Braganca | 53 |
| Elvas | yes | Portalegre | 51 |
| Tui | no (Spain) | Viana do Castelo | 42 |
| Ayamonte | no | Faro | 52 |
| Verin | no | Braganca | 58 |
| Badajoz | no | Portalegre | 60 |
| Vigo | no | Viana do Castelo | 61 |
| Ciudad Rodrigo | no | Guarda | 62 |
| Huelva, Caceres, Zamora, Ourense | no | Faro, Portalegre, Braganca, Braga | 92, 92, 91, 99 |
| Seville, Tangier, Madrid, Tenerife | no | Faro, Faro, Braganca, Funchal | 178, 234, 299, 469 |

A Portuguese town can be farther from a place (79 km) than a Spanish one (42 km), so no radius separates them. With today's 150 km
rule, Vigo, Badajoz, Huelva, Ayamonte, Caceres and Ourense would all be answered from IPMA and labelled with a Portuguese city
(`loc: "Viana do Castelo"` for Vigo). The outline test is what makes the border honest.

Getting the outline. Take the Portugal polygon from Natural Earth 10m "admin 0 countries" (public domain), simplify it
(Douglas-Peucker, 0.01 to 0.02 degrees, about 1 to 2 km, a few hundred vertices, 3 to 6 KB), and commit the result as
`service/data/pt-outline.json` together with a small script that makes it (`tools/make_pt_outline.py`, it needs the Natural Earth file
and is run by hand, like `tools/make_tz.py`). Do not draw it from memory and do not use the 110m outlines that are easy to find
(`johan/world.geo.json` has 26 points for the mainland: its north-east corner is off by 10 km or more). The service has no third party
dependencies (`service/go.mod`), so the point-in-polygon test is 15 lines of Go, a ray cast on float64 degrees.

### At the border

- A pin within a few kilometres of the line may land on either side if the outline is 1 to 2 km off; both answers are good forecasts, so
  the failure is harmless. What the user sees: `loc` is the IPMA city on one side and empty on the other, and temperatures differ by
  a degree or two between the providers. Nobody moves the pin across the border, so there is no flapping and no hysteresis is needed.
- A pin in the sea off the coast is outside the outline and goes to Open-Meteo, which is as good over the sea.
- A pin in Portugal's territory but far from any place (the Selvagens) is in no box, so it goes to Open-Meteo; the 100 km test is the
  second line of defence for a wrong outline.

### In IPMA's list but far from a station

Two different distances exist in `weather.go`:

- **Forecast place** (35 places, one forecast per district capital or island): if the pin is in Portugal, the nearest place is within
  79 km on the mainland and within a few tens of km on the islands. Within the outline this is the forecast the owner chose; the test
  above (100 km) is only a guard.
- **Observation station** (`nearestStations(lat, lon, 5)`, 227 stations): the code takes the nearest of five stations that has a
  recent valid reading, with **no distance limit**. I recommend a 50 km limit: a station farther than that is skipped, and the
  forecast value for the current hour (the existing fallback) is used. This is a small, separate hardening and can be left out of #44.

### If IPMA is down

`Cache.Get` already serves an answer that expired less than 6 hours ago (forecast) or 3 hours ago (observations). When there is none,
the present code answers 502. Falling through to Open-Meteo gives the display weather in that case (decision D2 below): the temperature
may jump by a degree or two and the `loc` empties, which the board already handles ("WEATHER"). I recommend it, because the cost is
five lines and it removes the only way IPMA's undocumented `public-data` feed (noted in `service/README.md`) can blank the screen.

## Attribution

Open-Meteo's licence asks for a link next to any place where its data is displayed, "in any reasonable manner". An LED panel cannot show
a link, so the credit goes where a person can read it: the README, the service README, THIRD_PARTY_NOTICES and the portal (the
board's own web page, where the weather switch is). The data is changed (rounded to whole degrees, reduced to four icons), and the
licence asks to say so.

Text to use (one sentence, so the portal and the READMEs match):

> Weather data by [Open-Meteo.com](https://open-meteo.com/) (CC BY 4.0), used outside Portugal; rounded and shown as four icons.
> Weather data in Portugal: IPMA (https://www.ipma.pt).

Where each part goes (for the doc writers and the portal builder; this note changes none of these files):

| File | Now | Add |
|---|---|---|
| `README.md` line 16 (banner "Weather: Portugal only today") and line 156 (limitation) | Portugal only | worldwide; IPMA in Portugal, Open-Meteo elsewhere; the non-commercial note |
| `README.md` "Credits" (line 187) | IPMA | the sentence above, with the link |
| `README.md` non-commercial paragraph (near line 180) | adsb.fi | Open-Meteo's free API is for non-commercial use; a hosted or advertised service needs their paid plan |
| `service/README.md` "Credits (required)" and "Behaviour worth knowing" | IPMA must be cited; "Weather covers Portugal only ... HTTP 422" | the same sentence; the rule of this note; "coordinates sent to Open-Meteo are rounded to two decimals and kept in its logs for up to 90 days" |
| `THIRD_PARTY_NOTICES.md` "Data sources" (line 109) | IPMA | Open-Meteo.com (CC BY 4.0, non-commercial free API) |
| Portal (`portal/portal.template.html`, regenerate with `tools/build_web.py`) | no weather credit at all today | a "Data sources" line under the *Weather* switch or in the page footer with the sentence above and a working link to https://open-meteo.com/ |
| `docs/architecture.md` line 657 (what the sources log) | IPMA, adsb.fi, Google News, ... | Open-Meteo: it logs the request with coordinates for 90 days (its privacy section), the service sends two decimals |

If MET Norway were ever added as a second source, the text would be «Data from MET Norway» with a link and the licence (CC BY 4.0).

## Implementation notes for slice #44

### Files

- `service/worldwide.go` (new): Open-Meteo types, request, conversion, `iconForWMO`. Keep `weather.go` as the IPMA code, with `Get`
  renamed `getIPMA` and a new `Get` that applies the rule above. (`WeatherOut`, `Slot`, `roundInt`, the cache and `Upstream` are reused.)
- `service/coverage.go` (new): `ipmaCovers`, the polygon test, the two boxes; `service/data/pt-outline.json` (embedded with
  `//go:embed`, like the other tables; the Dockerfile already copies `data/`); `tools/make_pt_outline.py`.
- `service/main.go`: header comments only ("IPMA weather" becomes "weather"), and the 422 branch (see below).
- `service/service_test.go`: new tests, and two existing ones change (below).
- `service/testdata/openmeteo_madrid.json`, `openmeteo_moscow.json`, `openmeteo_kathmandu.json` and `service/testdata/README.md`:
  **already recorded in this branch** (real answers of 2026-11-26, one README line each).

### The request

```
GET https://api.open-meteo.com/v1/forecast
  ?latitude=40.42&longitude=-3.70
  &current=temperature_2m,weather_code
  &hourly=temperature_2m,weather_code
  &daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max
  &forecast_days=1&forecast_hours=12
  &timezone=auto&timeformat=unixtime
```

- Coordinates are rounded to two decimals (about 1 km; the service README already promises this for upstream calls, and the
  grid cell is larger). Build the query with `url.Values`; the encoded commas (`%2C`) work (tested).
- `forecast_hours=12`, not 7: `hourly` then starts at the **current hour at fetch time**, and the cache serves an answer for up to
  15 minutes (3 hours when the upstream fails), so by the time it is used the current hour may be index 1 to 3; +6 h then needs index 9.
- No key, no extra header. The existing `Upstream` sends `Accept-Encoding: gzip` through Go's transport and unzips; the cap for
  `GetJSON` can be `64 << 10`.
- 7 variables and one day: cost 1.0 per call (`X-RateLimit-Cost: 1.0` in the answers). Error answers are JSON
  (`{"error":true,"reason":"..."}`, HTTP 400); a 429 (limit) is the same `statusError` as any other failure.

### Cache

`w.cache.Get(fmt.Sprintf("om|%.2f|%.2f", lat, lon), 15*time.Minute, 3*time.Hour, load)`. The board asks every 15 minutes
(`WEATHER_EVERY_MS`) and Open-Meteo's `current` has a 15-minute step, so 15 minutes loses nothing. One board makes 96 calls a day,
about one percent of the free daily limit; 50 different places would still stay under it. The key is per rounded place, so two
boards in the same city share one call. Stale answers up to 3 hours old are served when the upstream fails (the same as the
IPMA observations); after that the answer is a 502, or the IPMA-then-Open-Meteo fall-through described above.

### The JSON mapping

The answer stays the existing `WeatherOut`; the firmware's parser in `fetchWeather` (live.cpp) reads `loc`, `i`, `t`, `hi`, `lo`,
`pop`, `h3`, `h6` and ignores the rest, so the board changes nothing.

| `WeatherOut` field | From Open-Meteo | Notes |
|---|---|---|
| `loc` | `""` | Open-Meteo returns no place name and has no reverse geocoding. The board shows `WEATHER` in layout B when `loc` is empty (`weatherB`), and the other layouts do not show it. A later slice can add a name setting or a small table of cities (D3 below). **Do not** use the time zone's city ("Europe/Madrid" for Vigo would be wrong). |
| `t` | `round(current.temperature_2m)` | nil when null; `math.Round`, so -3.5 becomes -4 like `roundInt` today |
| `i` | `iconForWMO(current.weather_code)` | `cloud` when the code is null |
| `code` | `current.weather_code` | the provider's own code; `src` tells which (the board does not read `code`) |
| `hi`, `lo` | `round(daily.temperature_2m_max[0])`, `round(daily.temperature_2m_min[0])` | whole local day, including hours that have passed (same as IPMA's day values) |
| `pop` | `round(daily.precipitation_probability_max[0])` | the highest hourly chance of the day |
| `h3`, `h6` | `{t: round(hourly.temperature_2m[k]), i: iconForWMO(hourly.weather_code[k])}` | k is the entry whose time is `base + 3 h` and `base + 6 h`, see the next list; `null` when missing |
| `src` | `"Open-Meteo"` | |
| `asof` | `current.time` formatted in the response's zone, `2006-01-02T15:04` | the board does not read it |

Hours and zone, in this order:

1. Request `timeformat=unixtime`: every time is an exact instant, so a DST change inside the window cannot shift a slot (the docs say
   the ISO format uses one offset for the whole answer).
2. `base` = the last `hourly.time` value at or before the service's clock (`w.now()`, which tests replace). Do **not** assume index 0.
3. A slot is found by time: `base + 3*3600` and `base + 6*3600`. This is what makes Kathmandu (+5:45) work: its hourly values are
   on the local hour, 15 minutes past a UTC hour (`1795652100`, `1795655700`, ...), and a search for "now + 3 h" in UTC hours would
   miss them. The board's labels (`tm_hour + 3*i` on the board's own clock) match because the slots are the same instants.
4. Today: `daily.time[0]` is the local midnight (an instant). The day values are used only when `daily.time[0] <= now < daily.time[0] + 1 day`
   (computed in the response's zone). A cached answer that crosses local midnight then has no high, low or rain chance, instead of
   yesterday's values (the board prints `-` for a missing value).
5. The zone is `time.LoadLocation(response.timezone)`; fall back to `time.FixedZone("", utc_offset_seconds)`. `main.go` already imports
   `time/tzdata`, so this works in the distroless image. The shared `tz_zones.json` (POSIX rules for the board) is not needed.

### Go types

```go
type omResponse struct {
    Timezone  string `json:"timezone"`
    UTCOffset int    `json:"utc_offset_seconds"`
    Current   struct {
        Time int64    `json:"time"`
        Temp *float64 `json:"temperature_2m"`
        Code *int     `json:"weather_code"`
    } `json:"current"`
    Hourly struct {
        Time []int64    `json:"time"`
        Temp []*float64 `json:"temperature_2m"`
        Code []*int     `json:"weather_code"`
    } `json:"hourly"`
    Daily struct {
        Time []int64    `json:"time"`
        Max  []*float64 `json:"temperature_2m_max"`
        Min  []*float64 `json:"temperature_2m_min"`
        Pop  []*float64 `json:"precipitation_probability_max"`
    } `json:"daily"`
}
```

Pointers because Open-Meteo sends `null` for a value it does not have. Every slice access checks the length. The `*_units` objects
and `generationtime_ms` are ignored (Go skips unknown fields). Measured: this decodes in 13 microseconds with 49 allocations.

A prototype of the whole conversion (not committed; the reference for the numbers below) turns the three recorded answers into these
JSON bodies, at the service clock 2026-11-26 01:20:00 UTC (unix 1795656000), 149 to 158 bytes each (the Lisbon test allows 220):

```
madrid    {"loc":"","t":6,"hi":11,"lo":3,"pop":0,"i":"sun","code":1,"h3":{"t":5,"i":"sun"},"h6":{"t":3,"i":"sun"},"src":"Open-Meteo","asof":"2026-11-26T02:00"}
moscow    {"loc":"","t":-4,"hi":-1,"lo":-5,"pop":90,"i":"cloud","code":3,"h3":{"t":-4,"i":"rain"},"h6":{"t":-2,"i":"rain"},"src":"Open-Meteo","asof":"2026-11-26T04:00"}
kathmandu {"loc":"","t":8,"hi":21,"lo":7,"pop":0,"i":"cloud","code":45,"h3":{"t":19,"i":"sun"},"h6":{"t":21,"i":"sun"},"src":"Open-Meteo","asof":"2026-11-26T06:45"}
```

(At 01:20 UTC Kathmandu is 07:05 local, so its base is the 07:00 entry, index 1; Madrid and Moscow use index 0. Kathmandu's current
temperature 7.5 becomes 8, half away from zero.)

### Test cases

Use the existing `stub` (`service_test.go`): add a handler for `/v1/forecast` that serves a chosen `openmeteo_*.json` and records
the query; give `Weather` a second base URL field for it, like `base` for IPMA.

1. **Three recorded answers** (Madrid, Moscow, Kathmandu), at the clock above: the exact JSON above, byte for byte, and size under 220.
2. **Request shape**: the query holds `timezone=auto`, `timeformat=unixtime`, `forecast_hours=12`, `forecast_days=1`, and the
   coordinates rounded to two decimals (`40.4168` is sent as `40.42`); it holds no `apikey` and no other coordinates.
3. **Hours by time, not index**: Kathmandu at 06:50 local and at 07:05 local pick different bases (`1795652100` then `1795655700`).
   Moscow at the clock 02:05 UTC picks base index 1 and its +6 h is index 7.
4. **Midnight**: the same Madrid answer with the clock after the local midnight that follows `daily.time[0]`: `hi`, `lo`, `pop` are
   absent and the rest is present.
5. **Nulls and short arrays**: a body with `"temperature_2m":null` in `current` gives no `t` (and still an icon); a `hourly` with only
   4 entries gives no `h6`; an empty `daily` gives no `hi`, `lo`, `pop`; a body where `current`, `hourly` and `daily` are all empty is
   `errNoWeather` and the HTTP layer answers 422.
6. **The icon function**: all 29 documented codes in a table (0,1 sun; 2 part; 3,45,48 cloud; 51 to 99 rain), plus unknown codes:
   4 and 44 cloud, 50 and 98 and 120 rain, -1 cloud. Break `case c < 50` to `c < 51` once and watch it fail.
7. **Cache**: three calls for the same place are one upstream hit; two pins 1 km apart that round to the same two decimals share it; a
   second place is a second hit; an upstream 500 after a good answer serves the stale answer; a 500 with no answer is an error (502 at
   HTTP level, no URL or coordinates in the log line).
8. **Choosing**: Lisbon, Porto, Faro, Vila Real, Evora, Funchal, Ponta Delgada, Angra do Heroismo go to IPMA (the stub counts hits on
   both providers); Vigo, Ourense, Salamanca, Badajoz, Huelva, Seville, Madrid, Tenerife, Tangier, Tokyo, Sao Paulo go to Open-Meteo.
   Use points at least 5 km from the border for the pass/fail lists, because the outline is simplified.
   The Selvagens (30.14, -15.86) go to Open-Meteo. The 100 km guard cannot be reached with a real Portuguese point, so make the limit a
   field of `Weather` (not a constant): a test sets it to 10 km and Barrancos (79 km from Beja) must then go to Open-Meteo.
9. **IPMA down**: with the IPMA aggregate answering 500 and no cached answer, a Lisbon pin gets the Open-Meteo answer (`src` is
   `"Open-Meteo"`). With a cached IPMA answer it still gets IPMA.
10. **Existing tests that must change**: `TestWeatherOutsideCoverage` (Madrid must now give `src "Open-Meteo"`, not `errOutsideCoverage`)
    and the 422 check in `TestHTTPLayer` (`/v1/weather?lat=40.4168&lon=-3.7038` must now be 200). Keep a 422 test through the
    `errNoWeather` path (5 above). `TestWeatherForLisbon` and the cache test stay as they are.
11. **Coverage outline** (`coverage_test.go`): the inside and outside lists above; the polygon file parses, is closed, and has
    between 100 and 2000 vertices; the island boxes contain every IPMA place whose id is in the Madeira or Azores groups (a loop over
    `places`), and every one of the 35 places is inside the outline or a box (a stale outline would fail this).
12. **Mutation checks to do by hand**: remove `timeformat=unixtime` from the query (test 2 fails); use index 0 as base (test 3
    fails); drop the midnight check (test 4 fails); put the outline test after the distance test only (test 8 fails for Vigo).

Commands for the slice: in `service/`, `gofmt -l .`, `go vet ./...`, `go test -count=1 -race ./...`.

### The 422 path and the board

What happens today: `Get` returns `errOutsideCoverage`; `wrap` in `main.go` maps it to HTTP 422 with the sentence "location is outside
IPMA's coverage (Portugal, Madeira, Azores)"; the board's `httpsGet` turns any non-200 into `HTTP 422` (`r.error`), `serviceGet` passes it
on, `fetchWeather` calls `setError(ProblemKind::Weather, ...)`, and `problemWords` (problems.h, the `HTTP nnn` branch) words it as
"The service answered with an unexpected status (HTTP 422)." The weather fetch retries every minute, the data stays empty, the panel
shows the idle screen in place of the weather, and after an hour of failures the panel's PROBLEM message appears (`docs/operations.md`,
"No weather").

After #44:

- **Service**: no real location gives 422 any more. Rename `errOutsideCoverage` to `errNoWeather`, message "no weather data for this
  location", still mapped to 422. It fires only when the worldwide provider answered with no usable value at all (never seen in 17
  places, including both poles). A provider outage stays 502.
- **Firmware: no change is needed.** The answer has the same fields. An empty `loc` is already handled (`WEATHER`). Do not add a
  special 422 sentence; the generic one is right for the rare case. (If the owner wants one anyway: a `problemWords` branch for
  weather and 422, "No weather is available for this place.", is one `if` and a line in `tools/test_problems.cpp`.)
- **Docs that become stale** (the doc writers update them from this list): `README.md` lines 16 and 156; `docs/architecture.md`
  line 294 (the errors paragraph) and the data-flow text that names IPMA; `docs/install.md` lines 301 and 506 ("Weather covers
  Portugal only", "outside it, switch Weather off"); `docs/operations.md` lines 128, 642 (the table row "422 on weather means you are
  outside Portugal") and 975 ("No weather"); `docs/development.md` line 437 (the example call); `service/README.md` lines 86 to 88 and
  Credits; the header comments of `service/main.go` and `service/weather.go`.

## Risks

1. **Open-Meteo's free tier is non-commercial.** Fine for a personal service. It becomes a problem if someone hosts this service for
   others for money or with ads. Mitigation: say so in the READMEs; make the base URL and an optional `apikey` come from the environment
   (`OPENMETEO_API_KEY`, never logged) so a commercial user can point at `customer-api.open-meteo.com`.
2. **No SLA, one small company.** The free API "carries no uptime guarantee" and the terms say it can block abusers without notice.
   Mitigations already in the design: the 3-hour stale cache, the board's own stale rules (the weather stays up to 2 hours), and the
   fall-through from IPMA. The server is open source (AGPLv3) and can be self-hosted if the free API ever goes away; MET Norway is the
   documented second source (it would need the zone lookup, the contact User-Agent, and a way to fill high, low and rain chance,
   so its quality would be lower).
3. **IP-based limits.** "Rate limits are applied by IP addresses and application", and the terms warn that shared hosting
   (Vercel, Netlify, Render) is limited earlier. A home or small-cluster egress address is not a serverless platform, so this is
   unlikely; if answers start coming back 429 the 15-minute cache can be raised to 30.
4. **The code set can grow.** The docs now list 97 "Heavy thunderstorm", which older copies of the table lack. The range-based
   `iconForWMO` copes; a fixed map would not.
5. **Snow and drizzle show as rain** (see the mapping notes); first the snow decision, then drizzle after seeing it on the panel.
6. **The outline is a hand-run artifact.** A wrong outline is harmless near the border (both providers give a good forecast) and the
   test that all 35 IPMA places are inside it catches a broken file. Alternatives I rejected: asking Open-Meteo for `timezone` first and
   treating `Europe/Lisbon`, `Atlantic/Madeira`, `Atlantic/Azores` as Portugal (the IPMA path would depend on Open-Meteo being up); a
   tz-lookup library in the service (the repository's service has no dependencies, and such libraries embed megabytes).
7. **Different numbers either side of the border.** IPMA's current temperature is an observation from a station; Open-Meteo's is a model
   value. A Portuguese owner who moves the pin sees a one or two degree change. Acceptable, and documented.
8. **The two providers disagree on "today"'s meaning only at midnight**; both use the location's local day. Open-Meteo's high, low and
   chance of rain include the hours already past, like the IPMA daily values, so late in the evening the high can be hours old.
9. **The undocumented IPMA `public-data` aggregate feed** (already in `service/README.md`) stays a risk for Portugal; the fall-through
   reduces it to "the numbers come from Open-Meteo for a while".
10. **Privacy.** Open-Meteo keeps request logs with coordinates for 90 days. The service sends two decimals (about 1 km) and no
    device identifier, but the owner's home location is about 1 km accurate in those logs. For a stricter setting, round to one decimal
    (about 10 km; the forecast grid is 1 to 11 km anyway). I recommend one decimal if the owner cares: it costs nothing visible in the
    weather screens. (Decision D4 below.) The existing flights call already sends two decimals to adsb.fi.
11. **Parse and size are not a risk** (13 microseconds, 880 bytes), unlike MET Norway's 38 KB per call.

## Decisions for the owner

- **D1** Add `snow` (and perhaps `storm`, `fog`) icons and texts? Today snow is "Rain". It is a firmware slice (art, screens, renderer,
  margin check, preview); the service maps to `snow` only after the firmware is out.
- **D2** When IPMA fails with nothing cached, fall through to Open-Meteo (recommended) or answer 502 as today?
- **D3** The location name for worldwide answers: empty (the board shows `WEATHER` in layout B) for #44; a settings field or a small
  embedded city table later.
- **D4** Coordinates sent to Open-Meteo: two decimals (the existing convention) or one?
- **D5** Light drizzle (WMO 51, 56) as `rain` (like IPMA's drizzle) or as `cloud`?

## Sources

All fetched on 2026-11-26 unless noted.

- Open-Meteo terms, https://open-meteo.com/en/terms
- Open-Meteo licence, https://open-meteo.com/en/licence
- Open-Meteo pricing and FAQ, https://open-meteo.com/en/pricing
- Open-Meteo weather forecast API docs (parameters, `timezone`, `timeformat`, `forecast_hours`, weather codes, `best_match`), https://open-meteo.com/en/docs
- Open-Meteo status page, https://status.open-meteo.com/ (shows no uptime figures; no incident listed)
- MET Norway terms of service, https://developer.yr.no/doc/TermsOfService/
- MET Norway licensing and data policy, https://api.met.no/doc/License
- MET Norway locationforecast 2.0 documentation, https://api.met.no/weatherapi/locationforecast/2.0/documentation
- MET Norway sample calls: `https://api.met.no/weatherapi/locationforecast/2.0/compact?lat=40.4200&lon=-3.7000` and the `complete`
  form for six places, plus Oslo (`lat=59.9100&lon=10.7500`) for the probability fields. The symbol documentation page returned 404, so the
  base names in the icon table come from the answers (11 names seen) and from the MET documentation text on the typo names.
- 7Timer! documentation, http://www.7timer.info/doc.php?lang=en, and a call to `http://www.7timer.info/bin/api.pl?lon=139.65&lat=35.68&product=civil&output=json`
- WeatherAPI.com terms, https://www.weatherapi.com/terms.aspx
- OpenWeatherMap pricing, https://openweathermap.org/price
- Pirate Weather docs, https://docs.pirateweather.net/en/latest/
- Earlier research in this repository: `wayfinder/research/weather-data.md` and `wayfinder/research/ipma-weather.md` (2026-11-21)
- The code read: `service/weather.go`, `service/geo.go`, `service/cache.go`, `service/upstream.go`, `service/main.go`,
  `service/service_test.go`, `service/Dockerfile`, `arduino/ledmatrix/live.cpp` (`fetchWeather`), `live.h` (`LiveWeather`),
  `arduino/ledmatrix/screens.cpp` (`iconKind`, `iconText`, `weatherA/B/C`), `arduino/ledmatrix/problems.h`, `shared/renderer.js`
  (`wxIcon`, `wxA/B/C`), `shared/tz_zones.json`
- Measurements (scratch, not committed): Open-Meteo for Madrid, Tokyo, Sao Paulo, Kolkata, Auckland, Kathmandu, mid-Atlantic (0, -30),
  both poles, longitude 180, the date line near Fiji, Reykjavik, Moscow, Vigo, Lisbon, Nairobi, New York; the Go decode benchmark
  (Go 1.27.4, Apple M1 Max, `go test -bench`), and the border-distance table computed from `service/data/distrits-islands.json`.
- Recorded samples in this branch: `service/testdata/openmeteo_madrid.json`, `openmeteo_moscow.json`, `openmeteo_kathmandu.json`
  (`service/testdata/README.md` says where each is from and when).
