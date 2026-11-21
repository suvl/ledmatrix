---
type: research
status: open
---

**Open-Meteo research finished 21 Nov 2026** (superseded by the owner's IPMA choice, kept as fallback): one request returns every needed field in about 950 bytes; free, no key, non-commercial, CC BY 4.0 attribution; ECDSA certificate and no IPv6 address.

**IPMA research finished 21 Nov 2026**, awaiting the owner's decision: IPMA covers Portugal only (35 forecast cities including Madeira and the Azores, 227 observation stations) and has **no hourly forecast**, so the +3 h and +6 h of weather layout C cannot come from it. Today's high, low, rain chance and condition come from one 1,152 byte daily file per city (5 days). Current temperature exists only inside `observations.json`, all stations for 24 h, 842 KB (74 KB gzipped): too big for the board, so a cloud service must shrink it. `api.ipma.pt` is IPv4 only, uses an RSA-3072 certificate (fast handshake class), also serves plain HTTP. Terms: free for non-profit use, IPMA must always be cited as the source, no rate limit stated. Open-Meteo would be the fallback outside Portugal and the only source of hourly values.

## Question

Which weather source gives current temperature, condition, today's high and low, chance of rain, and +3 h and +6 h in a small payload, free for a hobby device, with IPv6 and a cheap TLS handshake? Findings go in `wayfinder/research/weather-data.md`.

**Update 21 Nov 2026:** the first research recommended Open-Meteo (ECDSA certificate, no IPv6 address). The owner wants **IPMA** (ipma.pt) as the weather source instead; a second research pass is in `wayfinder/research/ipma-weather.md`. Open-Meteo stays as the fallback for locations outside Portugal, if IPMA covers only Portugal.
