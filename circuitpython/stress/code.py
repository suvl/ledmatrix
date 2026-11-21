# Ticket 009: CircuitPython resource stress test.
# Panel animation + Wi-Fi station + access point + mDNS + web server + HTTPS fetch of a large JSON,
# all in one polling loop. Prints "STAT" lines over serial; GET /status returns the same as JSON.
import gc
import os
import time

import adafruit_connection_manager
import adafruit_requests
import board
import displayio
import framebufferio
import mdns
import rgbmatrix
import socketpool
import terminalio
import wifi
from adafruit_display_text import label
from adafruit_httpserver import Request, Response, Server

# Arbitrary coordinates over central Europe (busy airspace), not the owner's location.
FETCH_URL = "https://api.adsb.lol/v2/lat/48.0/lon/10.0/dist/30"
FETCH_EVERY_S = 10
ENABLE_AP = False
FETCH_TIMEOUT_S = 4
STAT_EVERY_S = 5

def mem():
    gc.collect()
    return gc.mem_free()

print("STAT boot free", mem())

# --- panel -----------------------------------------------------------------
displayio.release_displays()
matrix = rgbmatrix.RGBMatrix(
    width=64, height=32, bit_depth=4,
    rgb_pins=[board.MTX_R1, board.MTX_G1, board.MTX_B1, board.MTX_R2, board.MTX_G2, board.MTX_B2],
    addr_pins=[board.MTX_ADDRA, board.MTX_ADDRB, board.MTX_ADDRC, board.MTX_ADDRD],
    clock_pin=board.MTX_CLK, latch_pin=board.MTX_LAT, output_enable_pin=board.MTX_OE,
)
display = framebufferio.FramebufferDisplay(matrix, auto_refresh=True)
bitmap = displayio.Bitmap(64, 32, 3)
palette = displayio.Palette(3)
palette[0] = 0x000000
palette[1] = 0x00A0FF
palette[2] = 0xFFB000
group = displayio.Group()
group.append(displayio.TileGrid(bitmap, pixel_shader=palette))
counter = label.Label(terminalio.FONT, text="0", color=0xFFFFFF, x=2, y=24)
status = label.Label(terminalio.FONT, text="wifi...", color=0x00FF00, x=2, y=6)
group.append(counter)
group.append(status)
display.root_group = group
print("STAT panel free", mem())

# --- wifi: station + access point ---------------------------------------------
t0 = time.monotonic()
wifi.radio.connect(os.getenv("CIRCUITPY_WIFI_SSID"), os.getenv("CIRCUITPY_WIFI_PASSWORD"))
ip = str(wifi.radio.ipv4_address)
print("STAT station up in %.1fs ip %s free %d" % (time.monotonic() - t0, ip, mem()))
if ENABLE_AP:
    try:
        wifi.radio.start_ap(ssid="flights-setup-test", password="flighttest1")
        print("STAT ap up ip", wifi.radio.ipv4_address_ap, "free", mem())
    except Exception as e:  # noqa: BLE001
        print("STAT ap FAILED", repr(e))
else:
    print("STAT ap disabled")

# --- mdns ------------------------------------------------------------------------
mdns_server = mdns.Server(wifi.radio)
mdns_server.hostname = "flights"
mdns_server.advertise_service(service_type="_http", protocol="_tcp", port=80)
print("STAT mdns up free", mem())

# --- http server -------------------------------------------------------------------
pool = socketpool.SocketPool(wifi.radio)
server = Server(pool, debug=False)
stats = {"free": 0, "max_gap_ms": 0, "fetches": 0, "last_fetch_s": 0, "last_bytes": 0,
         "last_aircraft": 0, "errors": 0, "requests": 0, "min_free": 10**9}

@server.route("/")
def index(request: Request):
    stats["requests"] += 1
    return Response(request, "<h1>flights.local</h1><p>stress test</p>", content_type="text/html")

@server.route("/status")
def status_route(request: Request):
    stats["requests"] += 1
    stats["free"] = mem()
    import json
    return Response(request, json.dumps(stats), content_type="application/json")

server.start("0.0.0.0", 80)
print("STAT server up free", mem())

# --- https client --------------------------------------------------------------------
ssl_ctx = adafruit_connection_manager.get_radio_ssl_context(wifi.radio)
conn_pool = adafruit_connection_manager.get_radio_socketpool(wifi.radio)
requests = adafruit_requests.Session(conn_pool, ssl_ctx)

def fetch():
    t = time.monotonic()
    try:
        r = requests.get(FETCH_URL, headers={"User-Agent": "matrix-stress-test"}, timeout=FETCH_TIMEOUT_S)
        raw = r.content
        stats["last_bytes"] = len(raw)
        import json
        data = json.loads(raw)
        stats["last_aircraft"] = len(data.get("ac", data.get("aircraft", [])))
        r.close()
        del raw, data
    except Exception as e:  # noqa: BLE001
        stats["errors"] += 1
        print("STAT fetch ERROR", repr(e))
    stats["fetches"] += 1
    stats["last_fetch_s"] = round(time.monotonic() - t, 2)
    gc.collect()

# --- main loop ---------------------------------------------------------------------------
x = 0
last_frame = last_stat = last_fetch = time.monotonic()
last_loop = time.monotonic()
status.text = "ok " + ip.split(".")[-1]
while True:
    now = time.monotonic()
    gap = int((now - last_loop) * 1000)
    last_loop = now
    if gap > stats["max_gap_ms"]:
        stats["max_gap_ms"] = gap
    try:
        server.poll()
    except Exception as e:  # noqa: BLE001
        stats["errors"] += 1
        print("STAT poll ERROR", repr(e))
    if now - last_frame >= 0.05:
        last_frame = now
        bitmap[x, 12] = 0
        x = (x + 1) % 64
        bitmap[x, 12] = 2
        counter.text = str(int(now) % 1000)
    if now - last_fetch >= FETCH_EVERY_S:
        last_fetch = now
        fetch()
    if now - last_stat >= STAT_EVERY_S:
        last_stat = now
        f = mem()
        stats["free"] = f
        stats["min_free"] = min(stats["min_free"], f)
        print("STAT", stats)
        stats["max_gap_ms"] = 0
