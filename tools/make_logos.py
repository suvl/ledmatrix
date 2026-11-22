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
CODES = [c for c in dict.fromkeys(CODES) if len(c) == 2]

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
    for c in CODES:
        p = os.path.join(SRC, c + ".png") if "--from-cache" in sys.argv else fetch(c)
        if not p or not os.path.exists(p):
            continue
        im = convert(p)
        open(os.path.join(ROOT, c + ".bin"), "wb").write(rgb565(im))
        done.append((c, im))
    # the source answers with one grey plane icon when it has no logo: drop every copy of it
    from collections import Counter
    count = Counter(rgb565(im) for _, im in done)
    generic = [b for b, n in count.items() if n >= 3]
    for c, im in list(done):
        if rgb565(im) in generic:
            os.remove(os.path.join(ROOT, c + ".bin"))
            done.remove((c, im))
    cols = 12
    rows = (len(done) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * 20 * 4, rows * 24 * 4), (0, 0, 0))
    for i, (c, im) in enumerate(done):
        sheet.paste(im.resize((64, 64), Image.NEAREST), ((i % cols) * 80 + 8, (i // cols) * 96 + 4))
    sheet.save(os.path.join(ROOT, "_sheet.png"))
    print(len(done), "logos in", os.path.abspath(ROOT))

main()
