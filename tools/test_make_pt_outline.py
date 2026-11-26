#!/usr/bin/env python3
"""Tests for tools/make_pt_outline.py (Portugal's outline for the weather service). Python 3, standard library only:
    python3 -I tools/test_make_pt_outline.py
Offline: the Natural Earth file is faked in memory (a .dbf, a .shp of polygons and a VERSION file in a zip) with a square
"Portugal" next to a square "Spain", so that the border, the coast, the islands and the simplification can be checked by hand."""
import hashlib
import importlib.util
import io
import os
import struct
import sys
import tempfile
import unittest
import zipfile
from contextlib import redirect_stderr, redirect_stdout

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("make_pt_outline", os.path.join(HERE, "make_pt_outline.py"))
mp = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mp)

# clockwise rings as (lon, lat). Mainland: north edge with a bump of 0.003 degree (below the tolerance), the east edge
# (the border: shared with Spain) with collinear points, the south edge, the west coast with a bump of 0.05 degree.
PT = [(-9.0, 42.0), (-8.0, 42.003), (-7.0, 42.0), (-7.0, 41.0), (-7.0, 40.0), (-7.0, 39.0), (-7.0, 38.0), (-7.0, 37.0), (-9.0, 37.0), (-9.05, 40.0)]
ES = [(-7.0, 42.0), (-5.0, 42.0), (-5.0, 37.0), (-7.0, 37.0), (-7.0, 38.0), (-7.0, 39.0), (-7.0, 40.0), (-7.0, 41.0)]
MADEIRA = [(-17.0, 32.8), (-16.8, 32.8), (-16.8, 32.6), (-17.0, 32.6)]
AZORES_1 = [(-25.6, 37.8), (-25.4, 37.8), (-25.4, 37.6), (-25.6, 37.6)]
AZORES_2 = [(-28.1, 38.7), (-27.9, 38.7), (-27.9, 38.5), (-28.1, 38.5)]
SELVAGENS = [(-15.95, 30.15), (-15.85, 30.15), (-15.85, 30.05), (-15.95, 30.05)]
STEM = mp.STEM


def closed(ring):
    return ring + [ring[0]]  # a shapefile ring repeats its first point


def fake_dbf(rows):
    fields = [("ADM0_A3", 3), ("ADMIN", 12)]
    rlen = 1 + sum(n for _, n in fields)
    hlen = 32 + 32 * len(fields) + 1
    out = bytearray(32)
    out[0] = 3
    struct.pack_into("<IHH", out, 4, len(rows), hlen, rlen)
    for name, n in fields:
        d = bytearray(32)
        d[:len(name)] = name.encode()
        d[11] = ord("C")
        d[16] = n
        out += d
    out.append(0x0D)
    for r in rows:
        out.append(0x20)
        for (name, n) in fields:
            out += r[name].encode().ljust(n, b" ")
    return bytes(out)


def fake_shp(records):
    """records: a list of lists of rings (each ring a list of (lon, lat), not yet closed); None is a null shape."""
    out = bytearray(100)
    for num, rings in enumerate(records, 1):
        if rings is None:
            body = struct.pack("<i", 0)
        else:
            parts, pts = [], []
            for ring in rings:
                parts.append(len(pts))
                pts += closed(ring)
            xs, ys = [p[0] for p in pts], [p[1] for p in pts]
            body = struct.pack("<i4dii", 5, min(xs), min(ys), max(xs), max(ys), len(parts), len(pts))
            body += struct.pack("<%di" % len(parts), *parts)
            body += b"".join(struct.pack("<2d", x, y) for x, y in pts)
        out += struct.pack(">ii", num, len(body) // 2) + body
    return bytes(out)


def fake_zip(path, records=None, version="9.9.9"):
    records = records if records is not None else [
        ("XXX", "Nowhere", [[(0.0, 0.0), (0.0, 1.0), (1.0, 1.0), (1.0, 0.0)]]),
        ("ESP", "Spain", [ES]),
        None,
        ("PRT", "Portugal", [PT, MADEIRA, AZORES_1, AZORES_2, SELVAGENS]),
    ]
    rows, shapes = [], []
    for r in records:
        if r is None:
            rows.append({"ADM0_A3": "-99", "ADMIN": "Null Island"})
            shapes.append(None)
        else:
            rows.append({"ADM0_A3": r[0], "ADMIN": r[1]})
            shapes.append(r[2])
    with zipfile.ZipFile(path, "w") as z:
        z.writestr(STEM + ".dbf", fake_dbf(rows))
        z.writestr(STEM + ".shp", fake_shp(shapes))
        z.writestr(STEM + ".VERSION.txt", version + "\n")


class Reading(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.zip = os.path.join(self.tmp.name, "ne.zip")
        fake_zip(self.zip)

    def test_rings_of_a_country(self):
        rings = mp.country_rings(self.zip, "PRT")
        self.assertEqual(len(rings), 5)
        self.assertEqual(rings[0], PT)  # the repeated closing point is dropped
        self.assertEqual(mp.country_rings(self.zip, "ESP"), [ES])

    def test_unknown_or_repeated_country_stops(self):
        with self.assertRaises(SystemExit):
            mp.country_rings(self.zip, "FRA")
        fake_zip(self.zip, [("PRT", "a", [PT]), ("PRT", "b", [PT])])
        with self.assertRaises(SystemExit):
            mp.country_rings(self.zip, "PRT")

    def test_a_shapefile_of_lines_is_refused(self):
        data = bytearray(fake_shp([[PT]]))
        struct.pack_into("<i", data, 108, 3)  # the record's shape type: a polyline
        with self.assertRaises(SystemExit):
            mp.read_shp(bytes(data))

    def test_orientation(self):
        self.assertLess(mp.signed_area(PT), 0)  # clockwise
        self.assertGreater(mp.signed_area(PT[::-1]), 0)
        self.assertAlmostEqual(mp.signed_area([(0, 0), (0, 1), (1, 1), (1, 0)]), -1.0)


class Simplifying(unittest.TestCase):
    def test_collinear_points_go_and_a_big_bump_stays(self):
        ring = [(0.0, 0.0), (0.5, 0.0), (1.0, 0.0), (1.0, 1.0), (0.5, 1.0), (0.0, 1.0)]
        self.assertEqual(mp.simplify_indices(ring, 0.01), [0, 2, 3, 5])
        bumped = [(0.0, 0.0), (0.5, 0.3), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
        self.assertEqual(mp.simplify_indices(bumped, 0.01), [0, 1, 2, 3, 4])

    def test_the_tolerance_is_in_degrees_of_latitude(self):
        ring = [(0.0, 40.0), (0.5, 40.004), (1.0, 40.0), (1.0, 41.0), (0.0, 41.0)]
        self.assertEqual(len(mp.simplify_indices(ring, 0.005)), 4)   # a bump of 0.004 is below it
        self.assertEqual(len(mp.simplify_indices(ring, 0.003)), 5)   # and above this one

    def test_longitude_is_scaled_by_the_cosine_of_the_latitude(self):
        # a 0.01 degree bump of longitude at 60 N is 0.005 degree of ground: below the tolerance 0.006, above 0.004
        ring = [(0.0, 60.0), (0.01, 60.5), (0.0, 61.0), (-1.0, 61.0), (-1.0, 60.0)]
        self.assertEqual(len(mp.simplify_indices(ring, 0.006)), 4)
        self.assertEqual(len(mp.simplify_indices(ring, 0.004)), 5)

    def test_anchors_are_kept(self):
        ring = [(0.0, 0.0), (0.5, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
        self.assertEqual(mp.simplify_indices(ring, 0.01, anchors=[1]), [0, 1, 2, 3, 4])

    def test_is_simple(self):
        self.assertTrue(mp.is_simple([(0, 0), (0, 2), (2, 2), (2, 0)]))
        self.assertFalse(mp.is_simple([(0, 0), (2, 2), (0, 2), (2, 0)]))  # a bow tie
        self.assertFalse(mp.is_simple([(0, 0), (4, 0), (4, 4), (2, 0), (0, 4)]))  # a corner of the ring touches an edge


class Marking(unittest.TestCase):
    def test_border_segments_are_those_with_both_ends_on_the_other_country(self):
        border = mp.border_segments(PT, [ES])
        # PT[2] (-7,42) to PT[7] (-7,37) are shared: segments 2..6; segment 1 ends on a shared point but starts on the bump
        self.assertEqual(border, [False, False, True, True, True, True, True, False, False, False])

    def test_the_junctions_are_kept_and_the_flags_follow_the_edges(self):
        border = mp.border_segments(PT, [ES])
        out = mp.simplify_marked(PT, border, 0.005)
        self.assertEqual(out, [(-9.0, 42.0, False), (-7.0, 42.0, True), (-7.0, 37.0, False), (-9.0, 37.0, False), (-9.05, 40.0, False)])

    def test_the_border_is_still_marked_when_the_simplification_would_drop_a_junction(self):
        # the border run is two segments long and straight: only the junctions keep it
        ring = [(0.0, 1.0), (1.0, 1.0), (1.0, 0.5), (1.0, 0.0), (0.0, 0.0)]
        out = mp.simplify_marked(ring, [False, True, True, False, False], 0.01)
        self.assertEqual([(x, y, b) for x, y, b in out if b], [(1.0, 1.0, True)])
        self.assertEqual([(x, y) for x, y, _ in out], [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)])


class Boxes(unittest.TestCase):
    def test_boxes_are_widened_and_rounded_outwards(self):
        boxes, left = mp.island_boxes([PT, MADEIRA, AZORES_1, AZORES_2, SELVAGENS], PT)
        self.assertEqual(boxes[0], ("Madeira", 32.5, 32.9, -17.1, -16.7))
        # both Azores rings are in one box: lon -28.1..-25.4, lat 37.6..38.7, widened by 0.1 and rounded outwards
        self.assertEqual(boxes[1], ("Azores", 37.5, 38.8, -28.2, -25.3))
        self.assertEqual(left, [SELVAGENS])  # Portuguese but in no box, on purpose

    def test_a_box_that_would_take_in_another_ring_stops(self):
        intruder = [(-16.9, 31.9), (-16.9, 32.7), (-16.85, 32.7)]  # a point of it is outside the Madeira group's area, the others inside the box
        with self.assertRaises(SystemExit):
            mp.island_boxes([PT, MADEIRA, AZORES_1, intruder], PT)

    def test_a_missing_group_stops(self):
        with self.assertRaises(SystemExit):
            mp.island_boxes([PT, MADEIRA], PT)


class Whole(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.zip = os.path.join(self.tmp.name, "ne.zip")
        self.out = os.path.join(self.tmp.name, "pt_outline.go")
        fake_zip(self.zip)

    def run_tool(self, *extra):
        buf = io.StringIO()
        with redirect_stdout(buf):
            mp.main([self.zip, "--out", self.out, "--fetched", "2026-11-26"] + list(extra))
        with open(self.out, encoding="utf-8") as f:
            return buf.getvalue(), f.read()

    def test_the_generated_file(self):
        said, text = self.run_tool()
        with open(self.zip, "rb") as f:
            sha = hashlib.sha256(f.read()).hexdigest()
        self.assertIn("mainland 10 -> 5 points (1 border edges), 2 boxes, 1 rings left out", said)
        first = text.splitlines()[0]
        self.assertEqual(first, "// Code generated by tools/make_pt_outline.py; DO NOT EDIT.")  # the marker Go tools look for
        for want in ("Natural Earth 1:10m", "version 9.9.9", "ADM0_A3 = PRT and ESP", "public domain", "Retrieved 2026-11-26", sha, mp.SOURCE_URL):
            self.assertIn(want, text)
        self.assertIn("package main\n\nvar ptMainland = []outlinePoint{\n", text)
        self.assertIn("\t{42.0000, -9.0000, false},\n\t{42.0000, -7.0000, true},\n\t{37.0000, -7.0000, false},\n\t{37.0000, -9.0000, false},\n\t{40.0000, -9.0500, false},\n}", text)
        self.assertIn('var ptIslandBoxes = []outlineBox{\n\t{"Madeira", 32.5, 32.9, -17.1, -16.7},\n\t{"Azores", 37.5, 38.8, -28.2, -25.3},\n}\n', text)
        self.assertTrue(text.endswith("\n") and "\r" not in text)

    def test_same_input_same_bytes(self):
        _, a = self.run_tool()
        _, b = self.run_tool()
        self.assertEqual(a, b)

    def test_a_smaller_tolerance_keeps_the_small_bump(self):
        _, text = self.run_tool("--tolerance", "0.001")
        self.assertIn("{42.0030, -8.0000, false}", text)

    def test_the_fetch_date_is_required(self):
        with self.assertRaises(SystemExit), redirect_stderr(io.StringIO()):
            mp.main([self.zip, "--out", self.out])

    def test_a_ring_that_is_not_clockwise_stops(self):
        fake_zip(self.zip, [("ESP", "Spain", [ES]), ("PRT", "Portugal", [PT[::-1], MADEIRA, AZORES_1])])
        with self.assertRaises(SystemExit):
            self.run_tool()

    def test_no_border_stops(self):
        fake_zip(self.zip, [("ESP", "Spain", [[(10.0, 10.0), (10.0, 11.0), (11.0, 11.0), (11.0, 10.0)]]), ("PRT", "Portugal", [PT, MADEIRA, AZORES_1])])
        with self.assertRaises(SystemExit):
            self.run_tool()

    def test_the_committed_file_is_what_the_service_expects(self):
        # the generated file in the repository: marker, source, a few hundred points, the two flags and the two boxes
        path = os.path.join(HERE, "..", "service", "pt_outline.go")
        with open(path, encoding="utf-8") as f:
            text = f.read()
        self.assertTrue(text.startswith("// Code generated by tools/make_pt_outline.py; DO NOT EDIT.\n"))
        self.assertIn("Natural Earth 1:10m", text)
        self.assertIn("Retrieved 20", text)
        points = [l for l in text.splitlines() if l.startswith("\t{") and l.endswith(("true},", "false},"))]
        self.assertTrue(100 <= len(points) <= 600, len(points))
        self.assertTrue(any(l.endswith("true},") for l in points) and any(l.endswith("false},") for l in points))
        self.assertIn('{"Madeira", ', text)
        self.assertIn('{"Azores", ', text)


if __name__ == "__main__":
    unittest.main()
