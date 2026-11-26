#!/usr/bin/env python3
"""Checks that the portal's list of headline editions is the one in arduino/ledmatrix/editions.h. Python 3, standard library only:
    python3 -I tools/test_editions_portal.py
editions.h is the one place the list is kept; tools/build_web.py writes the <option> elements of the portal's <select id="edition">
from it. This test (1) reads the list from editions.h with its own parser, (2) reads the options from the committed
arduino/ledmatrix/portal_page.h, and compares them, (3) runs build_web.py on a copy of its inputs and checks that the committed
generated files are up to date, and (4) checks the portal's script sends and shows the setting."""
import html
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))


def read(path):
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        return f.read()


def editions_from_header():
    """[(label, hl, gl, ceid)] from the LIST in editions.h, read between its first '{' and the closing '};'."""
    src = read("arduino/ledmatrix/editions.h")
    start = src.index("LIST[] = {")
    body = src[start:src.index("\n};", start)]
    rows = re.findall(r'\{"([^"]*)",\s*"([^"]*)",\s*"([^"]*)",\s*"([^"]*)"\}', body)
    assert body.count("{") - 1 == len(rows), "an entry of editions.h is not a plain four-string initializer"
    return rows


def options_of(page):
    m = re.search(r'<select id="edition">(.*?)</select>', page, re.S)
    assert m, "no <select id=\"edition\"> in the page"
    return re.findall(r'<option value="([^"]*)">([^<]*)</option>', m.group(1)), m.group(1)


class Editions(unittest.TestCase):
    def test_the_header_list_is_sane(self):
        rows = editions_from_header()
        self.assertTrue(8 <= len(rows) <= 40, len(rows))
        self.assertEqual(rows[0][1:], ("pt-PT", "PT", "PT:pt-150"))  # index 0 is the service's own default
        self.assertEqual(len({r[0] for r in rows}), len(rows))

    def test_the_portal_page_lists_exactly_the_header_list_in_order(self):
        rows = editions_from_header()
        opts, inner = options_of(read("arduino/ledmatrix/portal_page.h"))
        self.assertEqual(opts, [(str(i), html.escape(r[0])) for i, r in enumerate(rows)])
        self.assertEqual(inner.count("<option"), len(rows))  # nothing else in the select

    def test_the_template_has_the_placeholder_and_no_hand_written_list(self):
        t = read("portal/portal.template.html")
        self.assertEqual(t.count('<select id="edition">__EDITION_OPTIONS__</select>'), 1)
        self.assertNotIn("<option", t)
        self.assertIn('<label for="edition">Headlines language and region</label>', t)

    def test_the_generated_files_are_up_to_date(self):
        # build_web.py finds its inputs relative to itself: run it on a copy of them and compare with the committed outputs
        with tempfile.TemporaryDirectory() as tmp:
            for p in ["tools/build_web.py", "shared/fonts.json", "shared/renderer.js", "shared/tz_zones.json", "preview/index.template.html",
                      "portal/portal.template.html", "arduino/ledmatrix/editions.h"]:
                os.makedirs(os.path.dirname(os.path.join(tmp, p)), exist_ok=True)
                shutil.copy(os.path.join(ROOT, p), os.path.join(tmp, p))
            os.makedirs(os.path.join(tmp, "preview"), exist_ok=True)
            out = subprocess.run([sys.executable, "-I", os.path.join(tmp, "tools/build_web.py")], capture_output=True, text=True)
            self.assertEqual(out.returncode, 0, out.stderr)
            for p in ["arduino/ledmatrix/portal_page.h", "preview/index.html"]:
                with open(os.path.join(tmp, p), encoding="utf-8") as f:
                    self.assertEqual(f.read(), read(p), p + " is stale: run python3 tools/build_web.py")

    def test_the_portal_sends_and_shows_the_setting(self):
        t = read("portal/portal.template.html")
        self.assertIn("headlineEdition: +$('edition').value", t)   # saved as the index
        self.assertIn("c.headlineEdition", t)                      # shown from GET /api/settings
        self.assertIn("selectedIndex < 0", t)                      # an index the page does not know shows the first entry

    def test_a_missing_or_short_list_stops_the_build(self):
        # the build asserts on the number of entries it finds, so a header that no longer parses cannot make an empty select
        with tempfile.TemporaryDirectory() as tmp:
            for p in ["tools/build_web.py", "shared/fonts.json", "shared/renderer.js", "shared/tz_zones.json", "preview/index.template.html", "portal/portal.template.html"]:
                os.makedirs(os.path.dirname(os.path.join(tmp, p)), exist_ok=True)
                shutil.copy(os.path.join(ROOT, p), os.path.join(tmp, p))
            os.makedirs(os.path.join(tmp, "arduino/ledmatrix"), exist_ok=True)
            with open(os.path.join(tmp, "arduino/ledmatrix/editions.h"), "w") as f:
                f.write('inline constexpr Edition LIST[] = {\n    {"Only one", "en", "US", "US:en"},\n};\n')
            out = subprocess.run([sys.executable, "-I", os.path.join(tmp, "tools/build_web.py")], capture_output=True, text=True)
            self.assertNotEqual(out.returncode, 0)
            self.assertIn("editions.h", out.stderr)


if __name__ == "__main__":
    unittest.main()
