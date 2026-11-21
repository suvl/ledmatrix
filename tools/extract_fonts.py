#!/usr/bin/env python3
"""Extract the Adafruit GFX default font and Tom Thumb from the installed Arduino library into
shared/fonts.json, so the preview page and the portal draw with the exact fonts the firmware uses.
Usage: python3 tools/extract_fonts.py [path to Adafruit_GFX_Library]"""
import json, os, re, sys
lib = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser('~/Documents/Arduino/libraries/Adafruit_GFX_Library')
src = open(lib + '/glcdfont.c').read()
body = src[src.index('{', src.index('font[]')):src.rindex('}')]
glcd = [int(x, 16) for x in re.findall(r'0x[0-9A-Fa-f]{2}', body)][:1280]
tt = open(lib + '/Fonts/TomThumb.h').read()
bm = re.search(r'TomThumbBitmaps\[\][^{]*\{(.*?)\n\};', tt, re.S).group(1)
bitmaps = [int(x, 16) for x in re.findall(r'0x[0-9A-Fa-f]{2}', re.sub(r'/\*.*?\*/', '', bm))]
gl = re.search(r'TomThumbGlyphs\[\][^{]*\{(.*?)\n\};', tt, re.S).group(1)
glyphs = {}
for m in re.finditer(r'\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\}\s*,?\s*/\*\s*0x([0-9A-Fa-f]+)', gl):
    glyphs[str(int(m.group(7), 16))] = list(map(int, m.groups()[:6]))
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'shared', 'fonts.json')
json.dump({'glcd': glcd, 'tt': {'bitmaps': bitmaps, 'glyphs': glyphs}}, open(out, 'w'), separators=(',', ':'))
print('wrote', os.path.normpath(out), len(glcd), 'glcd bytes,', len(glyphs), 'Tom Thumb glyphs')
