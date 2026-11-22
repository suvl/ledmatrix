#!/usr/bin/env python3
"""Build the private airline logo set for the ledmatrix-api service.

Each logo is 16x16 pixels, RGB565 big-endian, 512 bytes, in logos/<IATA>.bin.
The logos are trademarks of their airlines: logos/ is git-ignored and never goes into the repository
or the public container image. The service reads them from a mounted folder (LOGO_DIR).

  python3 tools/make_logos.py            fetch what is missing, convert, write logos/ and logos/_sheet.png
  python3 tools/make_logos.py --from-cache   convert again from logos/_src without the network
"""
import os, sys, urllib.request, io
from PIL import Image, ImageEnhance

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "logos")
SRC = os.path.join(ROOT, "_src")
URL = "https://images.kiwi.com/airlines/64x64/{}.png"

# Airlines most likely to pass over a European location, plus the big world carriers (IATA codes).
CODES = """
TP FR U2 EZY LH BA AF KL IB VY UX LX OS SN SK AY AZ EI TK LO OK RO JU FB BT OU A3 BE W6 W4 HV TO V7 NT PC
EW DE X3 4U TB TV PS BG YM JP S4 NO HG ZB LS MT OR TOM EN WF DY D8 N0 7S AT RJ MS SV QR EK EY GF WY KU
ET SA KQ MS AH TU RAM LY EL AI 6E UK SG IX UL BG PK BI MH SQ TR TG VN PR GA CX KA CA CZ MU HU FM HO MF
JL NH KE OZ 7C BR CI AE NZ QF JQ VA DL UA AA WN B6 AS NK F9 HA G4 AC WS TS PD QX AM CM AV LA JJ G3 AR AD
H2 CU DM XL FX 5X QY PO CV 3V GB NX D0 LD SU S7 FV U6 UT KC HY J2 A9 B2 PW BM LG UM JU 9U WZ BS SW
""".split()
CODES += "DS EC RK I2 WA CL A5 LG QS FZ SP WI 5K YU NI 5Y W9 5W DU CJ WK 3S BY LW HF 2L X7 VF XQ JI".split()
CODES = [c for c in dict.fromkeys(CODES) if len(c) == 2]

# Sister airlines and regional subsidiaries that fly under another code but wear the parent's logo.
# Used only when the source has no logo of its own for the code.
ALIAS = {
    "DS": "U2", "EC": "U2",        # easyJet Switzerland, easyJet Europe
    "RK": "FR", "FR": "FR",        # Ryanair UK
    "WA": "KL", "KLM": "KL",       # KLM Cityhopper
    "A5": "AF",                    # HOP! (Air France)
    "NI": "TP",                    # Portugalia (TAP Express)
    "W9": "W6", "5W": "W6",        # Wizz Air UK, Wizz Air Abu Dhabi
    "CL": "LH",                    # Lufthansa CityLine
    "CJ": "BA",                    # BA CityFlyer
    "DU": "DY",                    # Norwegian (Norway)
    "I2": "IB",                    # Iberia Express
    "BY": "X3",                    # TUI Airways
}

def fetch(code):
    p = os.path.join(SRC, code + ".png")
    if not os.path.exists(p):
        try:
            req = urllib.request.Request(URL.format(code), headers={"User-Agent": "ledmatrix-logo-builder"})
            data = urllib.request.urlopen(req, timeout=15).read()
            Image.open(io.BytesIO(data)).verify()
            open(p, "wb").write(data)
        except Exception as e:
            print("  skip", code, e)
            return None
    return p

def convert(path):
    im = Image.open(path).convert("RGBA")
    bg = Image.new("RGBA", im.size, (0, 0, 0, 255))
    bg.alpha_composite(im)
    im = bg.convert("RGB").resize((16, 16), Image.LANCZOS)
    im = ImageEnhance.Color(im).enhance(1.35)       # LEDs wash colours out
    im = ImageEnhance.Contrast(im).enhance(1.15)
    return im

def rgb565(im):
    out = bytearray()
    for r, g, b in list(im.getdata()):
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        out += bytes((v >> 8, v & 255))
    return bytes(out)

def main():
    os.makedirs(SRC, exist_ok=True)
    done = []
    ims = {}
    for c in CODES:
        p = os.path.join(SRC, c + ".png") if "--from-cache" in sys.argv else fetch(c)
        if p and os.path.exists(p):
            ims[c] = convert(p)
    # drop the source's grey "no logo" plane before aliases look for a parent
    from collections import Counter
    count = Counter(rgb565(im) for im in ims.values())
    for c in [c for c, im in ims.items() if count[rgb565(im)] >= 3]:
        del ims[c]
    for c, parent in ALIAS.items():
        if c not in ims and parent in ims:
            ims[c] = ims[parent]
    done = []
    for c, im in ims.items():
        open(os.path.join(ROOT, c + ".bin"), "wb").write(rgb565(im))
        done.append((c, im))
    for f in os.listdir(ROOT):  # forget files of codes that no longer have a logo
        if f.endswith(".bin") and f[:-4] not in ims:
            os.remove(os.path.join(ROOT, f))
    cols = 12
    rows = (len(done) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * 20 * 4, rows * 24 * 4), (0, 0, 0))
    for i, (c, im) in enumerate(done):
        sheet.paste(im.resize((64, 64), Image.NEAREST), ((i % cols) * 80 + 8, (i // cols) * 96 + 4))
    sheet.save(os.path.join(ROOT, "_sheet.png"))
    print(len(done), "logos in", os.path.abspath(ROOT))

main()
