# ledmatrix-api

A small Go service that answers the LedMatrix display with compact JSON, so the display makes one
cheap request instead of fetching and parsing large responses from several servers.

| Endpoint | Returns | Source |
| --- | --- | --- |
| `GET /v1/weather?lat=&lon=` | current temperature, today's high/low and rain chance, an icon for now, +3 h and +6 h (about 155 bytes) | IPMA |
| `GET /v1/flights?lat=&lon=&radius_km=&alt_min=&alt_max=&n=` | planes nearest first, with airline, route, cities, aircraft and telemetry | adsb.fi (positions), adsbdb (routes) |
| `GET /v1/headlines?q=&n=&hl=&gl=&ceid=` | newest headlines for a topic, de-duplicated | Google News RSS |
| `GET /v1/logo/{iata}` | 512 bytes: a 16x16 airline logo, RGB565 big-endian; 404 if none | files in `LOGO_DIR` (not in the image) |
| `GET /v1/firmware/manifest` | the newest firmware's version, file name, size, SHA-256 and notes; 404 when none is published | the `/firmware` folder in the image |
| `GET /v1/firmware/{file}` | the firmware image named in the manifest, byte ranges supported | the `/firmware` folder in the image |
| `GET /healthz` | `ok` | |

Run locally (no secret needed, nothing is checked; for your own computer only):
`docker build -t ledmatrix-api service && docker run -e AUTH_OFF=1 -p 8080:8080 ledmatrix-api`.
Test: `docker run --rm -v "$PWD":/src -w /src/service golang:1.27 go test ./...` (mount the whole repository: the tests read
`shared/device-auth-vectors.json`).

## Authentication

There is no token. The display signs every request with a key of its own, and every `/v1/` endpoint, logos included,
answers `401` unless the signature is valid. `GET /healthz` stays open. The scheme is written down in
[`shared/device-auth.md`](../shared/device-auth.md) (headers `X-Device`, `X-Timestamp`, `X-Signature`; known-answer
vectors in `shared/device-auth-vectors.json`); the code is `auth.go`.

Configuration (environment):

| Variable | Meaning |
| --- | --- |
| `LISTEN` | listen address, default `:8080` |
| `MASTER_SECRET` | **Exactly one of `MASTER_SECRET`, `MASTER_SECRET_FILE` and `AUTH_OFF=1` is required.** Text only you know, at least 32 characters; whitespace around it (space, tab, CR, LF) is removed. Every device key is derived from it: key = HMAC-SHA256(master secret, device ID). The service keeps no device keys, only this secret. |
| `MASTER_SECRET_FILE` | The same secret, read from a file instead of the environment, so a Docker or Kubernetes secret mounted as a file works (for example `/run/secrets/master-secret`) and the secret never appears in the environment, `docker inspect` or `kubectl describe`. The value is the file's absolute path; the file holds only the secret (at most 4 KB; a trailing newline and other whitespace around it are removed; at least 32 characters after that). It is read once, at start: change the secret, restart the service. A missing, unreadable or oversized file, or a directory, stops the service from starting with a message that names the path and never the content. |
| `REVOKED_DEVICES` | Optional. Comma-separated device IDs (12 lowercase hex characters, the board's MAC) that are refused with `revoked` even with a valid signature. A bad ID stops the service from starting. Remove an ID to let the device back in: nothing is ever banned for good. |
| `PUBLIC_HOST` | Optional. The host name the display uses, for when a proxy rewrites the `Host` header. When set it is used instead of the request's `Host` in the signed text (lower case, no port). |
| `AUTH_OFF` | `1` turns the check off, for local development only; the service logs a loud warning at start. It refuses to start if `MASTER_SECRET` or `MASTER_SECRET_FILE` is also set, and it refuses to start with none of the three (it never runs open by accident); more than one of the three is also refused, because it would be ambiguous which one is meant. |
| `LOGO_DIR` | Optional folder of `<IATA>.bin` logo files. |

Make a master secret with `openssl rand -hex 32` and keep it out of the repository, tickets, logs and chat. **Back it up
offline** (a password manager, or paper in a safe place): the display's key is derived from it, and once the key has
been burned into the display's chip a lost master secret cannot be recovered, so the display would be locked out of this
service for good. Changing the master secret locks out every display that was provisioned with the old one.

A refusal is `401` with `{"error": "<a plain sentence>", "reason": "<code>"}`; the codes are `missing`, `malformed`,
`bad_signature`, `revoked` and `clock_skew` (the last also carries `serverTime`, Unix seconds, so the display can correct
its clock). The service logs one line per refusal with the device ID and the reason, never a key, a signature or the
secret, and at most once per device and reason per minute (and 30 lines per minute in all), so a scanner cannot flood
the log.

The body is part of the signature. The display sends only `GET`, so a `GET` or `HEAD` that carries a body (a
`Content-Length` other than 0, or any `Transfer-Encoding`) is refused as `malformed` and its body is never read; any
other method may carry at most 4 KB, read before the signature is checked (`413` above that), so a caller who cannot
sign cannot make the service hold much memory.

## Firmware updates

The image carries the firmware: the build workflow compiles it, writes `manifest.json` and `ledmatrix-<version>.bin` into
`service/firmware/` (git-ignored; `tools/make_manifest.py` does the writing) and builds the image with that folder as
`/firmware`. A new firmware reaches a display by deploying a new image: there is no storage, token or secret to add, and
the manifest and the image are never written by the service. The contract is [`shared/firmware-update.md`](../shared/firmware-update.md).

- `GET /v1/firmware/manifest` answers `{"version","file","size","sha256","notes"}` with `Cache-Control: no-cache`, or `404
  {"error":"no firmware is published"}`.
- `GET /v1/firmware/{file}` streams the image (`Content-Length` exact, `Range` works). The name is only compared with the
  manifest's `file`, never used as a path; any other name is `404 {"error":"no such firmware"}`.
- **Both are open to unsigned callers on purpose**: they ignore the signature headers completely, so a display whose key
  is refused can still fetch a fix. The integrity check is the display's SHA-256 against the manifest, over HTTPS.
- The manifest is checked when the service starts and again at most once a minute: the version is three numbers of 1 to 4
  digits, `file` is `ledmatrix-<version>.bin`, the size is above 100 KB and at most 2,097,152 bytes and equals the real
  file's, the SHA-256 equals the real file's (hashed once per change of the file), the notes are at most 200 characters.
  Anything else is "no firmware is published", logged once per change (not per request) with the reason.
- Downloads have their own limit (8 at once, then `503`), apart from the limit of the data routes, and lift the server's
  20 s write timeout (10 minutes instead), so a slow display cannot starve the signed routes or be cut off half way.
- Folder: `FIRMWARE_DIR` (default `/firmware`). Without a manifest the service works as before.

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
