#!/usr/bin/env python3
"""Generates service/pt_outline.go, Portugal's outline for the weather service (service/coverage.go): the service uses IPMA
inside the outline and Open-Meteo outside it. Python 3, standard library only; run by hand when the outline has to change
(it needs the Natural Earth file, which is not in the repository):

    curl -LO https://naciscdn.org/naturalearth/10m/cultural/ne_10m_admin_0_countries.zip
    python3 tools/make_pt_outline.py ne_10m_admin_0_countries.zip --fetched 2026-11-26

The source is Natural Earth 1:10m Cultural Vectors, Admin 0 - Countries (public domain, https://www.naturalearthdata.com/),
the records whose ADM0_A3 is PRT and ESP. Do not use the 1:110m data: its mainland has 26 points and the north-east corner
is off by 10 km or more. The output keeps

  * the mainland: the largest ring of the PRT record, simplified with Douglas-Peucker (default tolerance 0.005 degrees,
    about 0.5 km; the tolerance is applied on a local projection with the longitude scaled by cos(latitude)), and checked
    to be a simple polygon. Every edge is marked as a border edge (it runs along the Spanish border) or a coast edge: a
    vertex of the ring that is also a vertex of the Spanish ring lies on the border (Natural Earth draws a shared border
    once for both countries), and the points where the two kinds meet are kept by the simplification. The service
    uses the marks to count the water within 1.5 km of a coast edge as Portugal: at 1:10m a town on the water front, such
    as Viana do Castelo, Cascais or Figueira da Foz, can lie a few hundred metres outside the land polygon, and the
    border towns (Tui, Badajoz) must stay outside, which a margin on the whole outline could not do; and
  * one box for Madeira (Madeira, Porto Santo and the Desertas) and one for the Azores: the bounding box of the islands'
    rings, widened by 0.1 degree and rounded outwards to 0.1 degree. The script stops if a box would take in any other
    ring of the file that is not an island of that group (the Selvagens, at 30 N, belong to Portugal but have no IPMA
    place and are left out of both boxes on purpose; so are the small islets off the Algarve and in the Tagus, which the
    mainland ring does not hold either).

The output is deterministic: the same file and options give the same bytes (the date in the header comes from --fetched).
Test: python3 -I tools/test_make_pt_outline.py (offline; it builds a small fake file) and, in service/, go test."""
import argparse
import hashlib
import math
import os
import struct
import sys
import zipfile

sys.dont_write_bytecode = True
ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
OUT = os.path.join(ROOT, "service", "pt_outline.go")
STEM = "ne_10m_admin_0_countries"
SOURCE_URL = "https://naciscdn.org/naturalearth/10m/cultural/" + STEM + ".zip"
MARGIN = 0.1  # degrees added to the islands' bounding boxes
# the groups, by a point of the group's rings: Azores west of -24, Madeira group between -18 and -16 east of the Azores
GROUPS = (
    ("Madeira", lambda lon, lat: -18.0 < lon < -16.0 and 32.0 < lat < 34.0),
    ("Azores", lambda lon, lat: lon < -24.0 and lat > 36.0),
)


def read_dbf(data):
    """A dBase III file as a list of dicts of stripped strings."""
    nrec, hlen, rlen = struct.unpack("<IHH", data[4:12])
    fields, off = [], 32
    while data[off] != 0x0D:
        fields.append((data[off:off + 11].split(b"\0")[0].decode("ascii"), data[off + 16]))
        off += 32
    out = []
    for i in range(nrec):
        rec, p = data[hlen + i * rlen:hlen + (i + 1) * rlen], 1
        row = {}
        for name, n in fields:
            row[name] = rec[p:p + n].decode("utf-8", "replace").replace("\0", "").strip()
            p += n
        out.append(row)
    return out


def read_shp(data):
    """A shapefile of polygons as a list, per record, of rings; a ring is a list of (lon, lat). Null shapes give []."""
    pos, out = 100, []
    while pos < len(data):
        _, words = struct.unpack(">ii", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + words * 2]
        pos += 8 + words * 2
        kind = struct.unpack("<i", body[:4])[0]
        if kind == 0:
            out.append([])
            continue
        if kind != 5:
            raise SystemExit("shapefile record of type %d, expected a polygon (5)" % kind)
        nparts, npts = struct.unpack("<ii", body[36:44])
        parts = list(struct.unpack("<%di" % nparts, body[44:44 + 4 * nparts])) + [npts]
        flat = struct.unpack("<%dd" % (2 * npts), body[44 + 4 * nparts:44 + 4 * nparts + 16 * npts])
        pts = [(flat[2 * k], flat[2 * k + 1]) for k in range(npts)]
        out.append([pts[a:b] for a, b in zip(parts, parts[1:])])
    return out


def country_rings(zip_path, a3):
    """Every ring of the record whose ADM0_A3 is a3, each without the repeated closing point."""
    with zipfile.ZipFile(zip_path) as z:
        rows = read_dbf(z.read(STEM + ".dbf"))
        shapes = read_shp(z.read(STEM + ".shp"))
    if len(rows) != len(shapes):
        raise SystemExit("the .dbf and the .shp hold a different number of records")
    hits = [i for i, r in enumerate(rows) if r.get("ADM0_A3") == a3]
    if len(hits) != 1:
        raise SystemExit("expected one record with ADM0_A3 = %s, found %d" % (a3, len(hits)))
    return [r[:-1] if len(r) > 1 and r[0] == r[-1] else r for r in shapes[hits[0]]]


def signed_area(ring):
    """Shoelace area in square degrees; negative for a clockwise ring (an outer ring in a shapefile)."""
    return sum(x1 * y2 - x2 * y1 for (x1, y1), (x2, y2) in zip(ring, ring[1:] + ring[:1])) / 2


def _project(ring):
    k = math.cos(math.radians(sum(y for _, y in ring) / len(ring)))
    return [(x * k, y) for x, y in ring]


def _dp(pts, lo, hi, tol, keep):
    """Douglas-Peucker on pts[lo..hi] (inclusive), marking the points to keep; iterative, the ring has ~1000 points."""
    stack = [(lo, hi)]
    while stack:
        a, b = stack.pop()
        (ax, ay), (bx, by) = pts[a], pts[b]
        dx, dy = bx - ax, by - ay
        norm = math.hypot(dx, dy)
        far, dmax = -1, tol
        for i in range(a + 1, b):
            px, py = pts[i]
            if norm == 0:
                d = math.hypot(px - ax, py - ay)
            else:
                d = abs(dx * (ay - py) - (ax - px) * dy) / norm
            if d > dmax:
                far, dmax = i, d
        if far >= 0:
            keep[far] = True
            stack.append((a, far))
            stack.append((far, b))


def simplify_indices(ring, tol, anchors=()):
    """The indices of the points that Douglas-Peucker keeps. A ring has no end points, so the first point, the point farthest
    from it and the given anchors are always kept and every stretch between two of them is simplified on its own."""
    n = len(ring)
    pts = _project(ring)
    pts.append(pts[0])  # closed: the last stretch runs back to the first point
    far = max(range(n), key=lambda i: math.hypot(pts[i][0] - pts[0][0], pts[i][1] - pts[0][1]))
    keep = [False] * (n + 1)
    keep[0] = keep[far] = keep[n] = True
    for a in anchors:
        keep[a] = True
    marked = [i for i in range(n + 1) if keep[i]]
    for a, b in zip(marked, marked[1:]):
        _dp(pts, a, b, tol, keep)
    return [i for i in range(n) if keep[i]]


def border_segments(ring, other_rings):
    """For every segment i (ring[i] to ring[i+1]): True when it runs along the other country's border, which is when both of
    its end points are also vertices of the other country's rings (Natural Earth draws a shared border once for both)."""
    shared = {(round(x, 5), round(y, 5)) for r in other_rings for x, y in r}
    on = [(round(x, 5), round(y, 5)) in shared for x, y in ring]
    return [on[i] and on[(i + 1) % len(ring)] for i in range(len(ring))]


def simplify_marked(ring, border, tol):
    """The ring simplified, as [(lon, lat, border)]: border is True when the edge to the next point mostly follows the border.
    The points where a border stretch meets a coast stretch are kept."""
    n = len(ring)
    junctions = [i for i in range(n) if border[i - 1] != border[i]]
    kept = simplify_indices(ring, tol, junctions)
    out = []
    for a, b in zip(kept, kept[1:] + [kept[0] + n]):
        segs = [border[j % n] for j in range(a, b)]
        out.append((ring[a][0], ring[a][1], sum(segs) * 2 > len(segs)))
    return out


def _ccw(a, b, c):
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])


def is_simple(ring):
    """True when no two edges of the closed ring cross or touch (except neighbours at their shared point)."""
    n = len(ring)
    for i in range(n):
        a, b = ring[i], ring[(i + 1) % n]
        for j in range(i + 1, n):
            if j == i + 1 or (i == 0 and j == n - 1):
                continue
            c, d = ring[j], ring[(j + 1) % n]
            d1, d2, d3, d4 = _ccw(a, b, c), _ccw(a, b, d), _ccw(c, d, a), _ccw(c, d, b)
            if (d1 > 0) != (d2 > 0) and (d3 > 0) != (d4 > 0) and d1 != 0 and d2 != 0 and d3 != 0 and d4 != 0:
                return False
            if 0 in (d1, d2, d3, d4):  # touching or collinear overlap: treat as not simple
                return False
    return True


def island_boxes(rings, main):
    """[(name, lat_min, lat_max, lon_min, lon_max)] for the island groups, and the rings left in no group."""
    boxes, used = [], {id(main)}
    for name, member in GROUPS:
        group = [r for r in rings if r is not main and all(member(x, y) for x, y in r)]
        if not group:
            raise SystemExit("no ring found for " + name)
        used.update(id(r) for r in group)
        xs = [x for r in group for x, _ in r]
        ys = [y for r in group for _, y in r]
        lo = lambda v: math.floor(round((v - MARGIN) * 10, 6)) / 10
        hi = lambda v: math.ceil(round((v + MARGIN) * 10, 6)) / 10
        box = (name, lo(min(ys)), hi(max(ys)), lo(min(xs)), hi(max(xs)))
        for r in rings:
            if id(r) not in {id(g) for g in group} and any(box[3] <= x <= box[4] and box[1] <= y <= box[2] for x, y in r):
                raise SystemExit("the %s box would take in a ring of another place" % name)
        boxes.append(box)
    return boxes, [r for r in rings if id(r) not in used]


def render(ring, boxes, left, npoints_in, tol, version, file_date, fetched, sha):
    nborder = sum(1 for p in ring if p[2])
    lines = [
        "// Code generated by tools/make_pt_outline.py; DO NOT EDIT.",
        "//",
        "// Portugal's outline, used by coverage.go to choose between IPMA (inside) and Open-Meteo (outside).",
        "//",
        "// Source: Natural Earth 1:10m Cultural Vectors, Admin 0 - Countries, version %s (files dated %s), the records with" % (version, file_date),
        "// ADM0_A3 = PRT and ESP. Natural Earth is in the public domain (https://www.naturalearthdata.com/). Retrieved %s from" % fetched,
        "// %s; SHA-256 of that zip file: %s." % (SOURCE_URL, sha),
        "//",
        "// ptMainland: the mainland's largest ring, %d points simplified to %d with Douglas-Peucker at %s degrees (about %.1f km)." % (npoints_in, len(ring), tol, tol * 111),
        "// Clockwise, not closed (the last point joins the first), as (lat, lon) in degrees. Border is true when the edge to the",
        "// next point runs along the border with Spain (%d edges) and false when it is coast (%d edges)." % (nborder, len(ring) - nborder),
        "// ptIslandBoxes: the bounding boxes of the Madeira and the Azores islands, widened by %s degree and rounded outwards to 0.1" % MARGIN,
        "// degree. Left out on purpose: %d small rings (the Selvagens at 30 N, which have no IPMA place, and the islets in the" % len(left),
        "// Ria Formosa and the Tagus).",
        "",
        "package main",
        "",
        "var ptMainland = []outlinePoint{",
    ]
    lines += ["\t{%.4f, %.4f, %s}," % (y, x, "true" if b else "false") for x, y, b in ring]
    lines += ["}", "", "var ptIslandBoxes = []outlineBox{"]
    lines += ['\t{"%s", %.1f, %.1f, %.1f, %.1f},' % b for b in boxes]
    lines += ["}", ""]
    return "\n".join(lines)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("zip", help="ne_10m_admin_0_countries.zip from Natural Earth")
    ap.add_argument("--out", default=OUT)
    ap.add_argument("--tolerance", type=float, default=0.005, help="Douglas-Peucker tolerance in degrees (default 0.005)")
    ap.add_argument("--fetched", required=True, help="the date the zip was downloaded, YYYY-MM-DD (goes into the header)")
    a = ap.parse_args(argv)
    rings = country_rings(a.zip, "PRT")
    main_ring = max(rings, key=lambda r: abs(signed_area(r)))
    if signed_area(main_ring) >= 0:
        raise SystemExit("the largest ring is not clockwise: not an outer ring")
    border = border_segments(main_ring, country_rings(a.zip, "ESP"))
    if not any(border) or all(border):
        raise SystemExit("expected both border and coast edges on the mainland, found %d border edges of %d" % (sum(border), len(border)))
    simple = simplify_marked(main_ring, border, a.tolerance)
    if not is_simple([(x, y) for x, y, _ in simple]):
        raise SystemExit("the simplified ring crosses itself: use a smaller --tolerance")
    boxes, left = island_boxes(rings, main_ring)
    with zipfile.ZipFile(a.zip) as z:
        version = z.read(STEM + ".VERSION.txt").decode("ascii").strip()
        d = z.getinfo(STEM + ".shp").date_time
    file_date = "%04d-%02d-%02d" % d[:3]
    with open(a.zip, "rb") as f:
        sha = hashlib.sha256(f.read()).hexdigest()
    text = render(simple, boxes, left, len(main_ring), a.tolerance, version, file_date, a.fetched, sha)
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("%s: mainland %d -> %d points (%d border edges), %d boxes, %d rings left out" % (a.out, len(main_ring), len(simple), sum(p[2] for p in simple), len(boxes), len(left)))


if __name__ == "__main__":
    main()
