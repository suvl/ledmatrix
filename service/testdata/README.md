# Recorded upstream answers

Real answers, saved as they came, so the service tests need no network. The `openmeteo_*` files were recorded for
`wayfinder/research/worldwide-weather.md` (ticket 41) and are the input of the worldwide weather slice. The other files in this
folder were recorded earlier for the existing tests.

- `openmeteo_madrid.json`: Open-Meteo `https://api.open-meteo.com/v1/forecast` for latitude 40.42, longitude -3.70, fetched 2026-11-26 01:11 UTC (the first hourly value is 02:00 local, the zone is Europe/Madrid, +1 h). The request is in the research note, section "Implementation notes". A clear night in winter time.
- `openmeteo_moscow.json`: the same request for latitude 55.75, longitude 37.62, fetched 2026-11-26 01:11 UTC (zone Europe/Moscow, +3 h). Weather code 71 (snow) in the +3 h and +6 h slots, and negative temperatures (-3.5 in the +3 h slot tests the rounding).
- `openmeteo_kathmandu.json`: the same request for latitude 27.72, longitude 85.32, fetched 2026-11-26 01:11 UTC (zone Asia/Kathmandu, +5:45). The hourly times are on the local hour, 15 minutes past a UTC hour, so a slot must be found by time and not by an index of whole UTC hours. Weather code 45 (fog) now.
