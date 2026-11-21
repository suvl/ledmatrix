---
type: research
status: open
---

**Open-Meteo research finished 21 Nov 2026** (superseded by the owner's IPMA choice, kept as fallback): one request returns every needed field in about 950 bytes; free, no key, non-commercial, CC BY 4.0 attribution; ECDSA certificate and no IPv6 address.

## Question

Which weather source gives current temperature, condition, today's high and low, chance of rain, and +3 h and +6 h in a small payload, free for a hobby device, with IPv6 and a cheap TLS handshake? Findings go in `wayfinder/research/weather-data.md`.

**Update 21 Nov 2026:** the first research recommended Open-Meteo (ECDSA certificate, no IPv6 address). The owner wants **IPMA** (ipma.pt) as the weather source instead; a second research pass is in `wayfinder/research/ipma-weather.md`. Open-Meteo stays as the fallback for locations outside Portugal, if IPMA covers only Portugal.
