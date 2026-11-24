# ledmatrix-api

A small Go service that answers the LedMatrix display with compact JSON, so the display makes one
cheap request instead of fetching and parsing large responses from several servers.

| Endpoint | Returns | Source |
| --- | --- | --- |
| `GET /v1/weather?lat=&lon=` | current temperature, today's high/low and rain chance, an icon for now, +3 h and +6 h (about 155 bytes) | IPMA |
| `GET /v1/flights?lat=&lon=&radius_km=&alt_min=&alt_max=&n=` | planes nearest first, with airline, route, cities, aircraft and telemetry | adsb.fi (positions), adsbdb (routes) |
| `GET /v1/headlines?q=&n=&hl=&gl=&ceid=` | newest headlines for a topic, de-duplicated | Google News RSS |
| `GET /v1/logo/{iata}` | 512 bytes: a 16x16 airline logo, RGB565 big-endian; 404 if none | files in `LOGO_DIR` (not in the image) |
| `GET /healthz` | `ok` | |

Run locally: `docker build -t ledmatrix-api service && docker run -p 8080:8080 ledmatrix-api`.
Test: `docker run --rm -v "$PWD/service":/src -w /src golang:1.25 go test ./...`.
Configuration (environment): `LISTEN` (default `:8080`), `API_TOKEN` (optional, sent as `X-Token`). When it is set, every `/v1/` endpoint, logos included, answers
401 without it, and `/healthz` stays open. Enter the same value in the portal's "Service access token" field;
the display sends it to the service host only.

## Behaviour worth knowing

- **Logos are not in the repository or the image.** They are trademarks. `tools/make_logos.py` builds them
  privately into `logos/` (git-ignored); mount them with a ConfigMap as described in `deploy/ledmatrix-api.yaml`.
  The service works without them (404, and the display draws a badge with the airline code).
- **Upstream answers are cached** (flight positions 15 s, routes 12 h, weather 30 min, headlines 15 min)
  and a failing upstream serves the last good answer for a while, so the display does not blank.
- **Privacy:** coordinates sent upstream are rounded to two decimals (about 1 km), and the access log
  never contains query strings, because they carry the display's location.
- **Weather covers Portugal only** (35 IPMA forecast cities, Madeira and the Azores included). Outside
  it the answer is HTTP 422. The hourly forecast comes from IPMA's `public-data` feed, which is
  not in IPMA's documented list and could change.
- **Routes come from community data** and are missing or wrong for charter, cargo, general aviation
  and military flights; a missing route is simply left out.
- **Headlines** use an unofficial feed whose notice says "personal, non-commercial use".

## Credits (required)

- Weather data: Instituto Português do Mar e da Atmosfera (IPMA), https://www.ipma.pt. IPMA must be
  cited wherever its data is shown.
- Aircraft positions: adsb.fi open data (personal, non-commercial use), https://adsb.fi.
- Routes and airlines: adsbdb, https://www.adsbdb.com.
- Headlines: Google News.

## Deploy

`deploy/ledmatrix-api.yaml` has the Deployment, Service and HTTPRoute for a k3s cluster whose Envoy
Gateway already serves a wildcard certificate. The hostname is a variable, so the repository never
holds the real one:

    API_HOST=apis.your-domain.example IMAGE=ghcr.io/suvl/ledmatrix-api:latest PULL_POLICY=Always \
      envsubst '${API_HOST} ${IMAGE} ${PULL_POLICY}' < service/deploy/ledmatrix-api.yaml | kubectl apply -f -

If the package is private, build the image for the node (`docker buildx build --platform linux/amd64 -t ledmatrix-api:local --load service`),
load it with `docker save ledmatrix-api:local | ssh NODE sudo k3s ctr -n k8s.io images import -`, and deploy with
`IMAGE=ledmatrix-api:local PULL_POLICY=Never`.

The GitHub Actions workflow `.github/workflows/service.yml` tests the code and publishes
`ghcr.io/suvl/ledmatrix-api` (linux/amd64).
