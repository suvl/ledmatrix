# LedMatrix

A flight tracker, weather and news display for an Adafruit Matrix Portal S3 driving a 64×32 HUB75 LED
panel. New planes near the display are shown once (airline, route, aircraft, telemetry); between planes
it shows the weather and headlines. Everything is configured on a web page the display serves itself.

| Folder | What |
| --- | --- |
| `arduino/ledmatrix/` | The firmware (Arduino C++, arduino-esp32 3.3.x, Adafruit Protomatter): screens, Wi-Fi setup, settings portal |
| `service/` | `ledmatrix-api`, the Go service that gives the display compact weather, flights and headlines |
| `portal/`, `preview/`, `shared/`, `tools/` | The settings page, the screen preview page, the shared pixel renderer and fonts, and the script that builds them (`tools/build_web.py`) |
| `wayfinder/` | The decision map: tickets, research and build progress |
| `arduino/milestone/`, `circuitpython/` | Earlier experiments (Wi-Fi/TLS interference test, CircuitPython smoke tests) |

Build the firmware: `arduino-cli compile --fqbn esp32:esp32:adafruit_matrixportal_esp32s3 arduino/ledmatrix`
(core pinned to 3.3.11 for now). Wi-Fi credentials are never stored in this repository.

## Credits

- Weather data: Instituto Português do Mar e da Atmosfera (IPMA), https://www.ipma.pt
- Aircraft positions: adsb.fi open data, https://adsb.fi (personal, non-commercial use)
- Routes and airlines: adsbdb, https://www.adsbdb.com
- Headlines: Google News
- Map: © OpenStreetMap contributors, Leaflet
