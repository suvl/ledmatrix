#!/usr/bin/env python3
"""Tests for tools/make_manifest.py (the firmware manifest writer, shared/firmware-update.md). Python 3, standard library only:
    python3 -I tools/test_make_manifest.py
The images here are fakes built in memory with a correct ESP32-S3 header, segments, checksum and appended SHA-256; no compiler,
no board and no network are involved. Every value is a placeholder."""
import hashlib
import importlib.util
import io
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

sys.dont_write_bytecode = True  # no tools/__pycache__ in the repository
HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "make_manifest.py")

_spec = importlib.util.spec_from_file_location("make_manifest", TOOL)
mm = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mm)


TAG_PREFIX = b"\x01LEDMATRIX-FW-VERSION\x02"


def tag(version):
    return TAG_PREFIX + version.encode("ascii") + b"\x03"


def make_image(total, chip=0x0009, magic=0xE9, segments=1, hash_appended=True, fill=0x5A, tags=("1.2.3",), extra=b""):
    """An intact ESP image of exactly `total` bytes (a multiple of 16): header, `segments` segments, padding with the checksum
    byte, and (hash_appended) the 32-byte SHA-256 of everything before it. `tags` are the versions of the version markers put
    into the first segment's data (none: a build without one); `extra` is more raw bytes put there (a decoy, for instance)."""
    tail = 32 if hash_appended else 0
    header = bytearray(24)
    header[0], header[1] = magic, segments
    struct.pack_into("<I", header, 4, 0x40380000)
    struct.pack_into("<H", header, 12, chip)
    header[23] = 1 if hash_appended else 0
    # total = 24 + 8*segments + data + pad(+checksum) + tail, and data + pad is chosen so the sum lands on `total`.
    body = total - tail - 24 - 8 * segments           # data bytes plus the padding and the checksum byte
    assert body > 0 and total % 16 == 0
    # The segments' data ends where the padding starts: pick data so that (24 + 8*segments + data) % 16 == 15 - pad.
    data_len = body - 1
    while (24 + 8 * segments + data_len + 1) % 16 != 0:
        data_len -= 1
    sizes = [data_len // segments] * segments
    sizes[-1] += data_len - sum(sizes)
    out = bytearray(header)
    checksum = 0xEF
    for i, n in enumerate(sizes):
        chunk = bytes((fill + i + k) & 0xFF for k in range(min(n, 251))) * (n // 251 + 1)
        chunk = bytearray(chunk[:n])
        if i == 0:
            at = 16
            for blob in [tag(v) for v in tags] + [extra]:
                assert at + len(blob) < n, "the fake segment is too small for the markers"
                chunk[at:at + len(blob)] = blob
                at += len(blob) + 16
        chunk = bytes(chunk)
        out += struct.pack("<II", 0x3C000000 + i * 0x100000, n) + chunk
        for b in chunk:
            checksum ^= b
    pad = 15 - (len(out) % 16)
    out += bytes(pad) + bytes([checksum])
    assert len(out) % 16 == 0
    if hash_appended:
        out += hashlib.sha256(bytes(out)).digest()
    assert len(out) == total, (len(out), total)
    return bytes(out)


def run_main(argv):
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        try:
            rc = mm.main(argv)
        except SystemExit as e:                       # argparse errors
            rc = e.code
    return rc, out.getvalue(), err.getvalue()


class TempCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = self._tmp.name
        self.out = os.path.join(self.tmp, "firmware")

    def write(self, name, data):
        path = os.path.join(self.tmp, name)
        with open(path, "wb") as f:
            f.write(data if isinstance(data, bytes) else data.encode("utf-8"))
        return path

    def snapshot(self):
        out = {}
        for n in self.listing():
            with open(os.path.join(self.out, n), "rb") as f:
                out[n] = f.read()
        return out

    def listing(self):
        return sorted(os.listdir(self.out)) if os.path.isdir(self.out) else None


class VersionTests(unittest.TestCase):
    def test_accepts_three_numbers(self):
        for v in ("0.0.0", "0.10.0", "1.2.3", "10.20.30", "9999.9999.9999"):
            self.assertEqual(mm.check_version(v), v)

    def test_rejects_everything_else(self):
        for v in ("", "1", "1.2", "1.2.3.4", "1.2.3-rc1", "1.2.3+b", "v1.2.3", "01.2.3", "1.02.3", "1.2.03", "10000.0.0",
                  "1.10000.0", "1.2.x", " 1.2.3", "1.2.3 ", "1.2.3\n", "1..3", "-1.2.3", "1.2.-3", "١.٢.٣",
                  "1.2.3\x00", "1\u0662.3.4", "1.3.4\u0663"):
            with self.assertRaises(mm.Refusal, msg=repr(v)):
                mm.check_version(v)

    def test_non_string(self):
        with self.assertRaises(mm.Refusal):
            mm.check_version(None)


class VersionHTests(TempCase):
    def test_reads_the_define(self):
        p = self.write("version.h", '// comment\n#pragma once\n#define FIRMWARE_VERSION "0.10.0"\n')
        self.assertEqual(mm.read_version_h(p), "0.10.0")

    def test_spacing_and_trailing_comment(self):
        p = self.write("version.h", '  #  define   FIRMWARE_VERSION\t"1.2.3"   // the release\n')
        self.assertEqual(mm.read_version_h(p), "1.2.3")

    def test_crlf(self):
        p = self.write("version.h", '#define FIRMWARE_VERSION "1.2.3"\r\n')
        self.assertEqual(mm.read_version_h(p), "1.2.3")

    def test_missing_define(self):
        p = self.write("version.h", "#pragma once\n")
        with self.assertRaises(mm.Refusal):
            mm.read_version_h(p)

    def test_commented_out_define_is_ignored(self):
        p = self.write("version.h", '// #define FIRMWARE_VERSION "9.9.9"\n#define FIRMWARE_VERSION "1.2.3"\n')
        self.assertEqual(mm.read_version_h(p), "1.2.3")

    def test_two_defines_are_refused(self):
        p = self.write("version.h", '#define FIRMWARE_VERSION "1.2.3"\n#define FIRMWARE_VERSION "1.2.4"\n')
        with self.assertRaises(mm.Refusal):
            mm.read_version_h(p)

    def test_bad_version_inside(self):
        for text in ('#define FIRMWARE_VERSION "1.2"\n', '#define FIRMWARE_VERSION "1.2.3-rc1"\n',
                     '#define FIRMWARE_VERSION ""\n', "#define FIRMWARE_VERSION 1.2.3\n"):
            p = self.write("version.h", text)
            with self.assertRaises(mm.Refusal, msg=text):
                mm.read_version_h(p)

    def test_missing_file(self):
        with self.assertRaises(mm.Refusal):
            mm.read_version_h(os.path.join(self.tmp, "nope.h"))

    def test_the_real_version_h_is_valid(self):
        real = os.path.join(HERE, "..", "arduino", "ledmatrix", "version.h")
        if os.path.exists(real):
            mm.check_version(mm.read_version_h(real))


class NotesTests(unittest.TestCase):
    def test_short_text_is_unchanged(self):
        self.assertEqual(mm.fold_notes("Fixes the clock."), "Fixes the clock.")
        self.assertEqual(mm.fold_notes(""), "")
        self.assertEqual(mm.fold_notes(None), "")

    def test_newlines_and_blanks_fold_to_one_space(self):
        self.assertEqual(mm.fold_notes("first line\n\nsecond\tline\r\n  third   "), "first line second line third")
        self.assertEqual(mm.fold_notes("a\x00b\x07c\x7fd"), "a b c d")

    def test_exactly_200_is_kept(self):
        text = ("word " * 40)[:200].rstrip()
        text = text + "x" * (200 - len(text))
        self.assertEqual(len(text), 200)
        self.assertEqual(mm.fold_notes(text), text)

    def test_201_is_cut_at_a_word_boundary_with_an_ellipsis(self):
        text = " ".join(["alpha"] * 40)                  # 239 characters
        got = mm.fold_notes(text)
        self.assertLessEqual(len(got), 200)
        self.assertTrue(got.endswith("alpha..."), got)
        self.assertTrue(set(got[:-3].split(" ")) == {"alpha"}, got)   # no half word left behind
        self.assertGreater(len(got), 190)

    def test_cut_on_a_space_keeps_the_whole_words(self):
        text = "a" * 196 + " " + "b" * 30                # the 197th character is a space: cut right there
        self.assertEqual(mm.fold_notes(text), "a" * 196 + "...")

    def test_one_long_word_is_hard_cut(self):
        got = mm.fold_notes("x" * 500)
        self.assertEqual(got, "x" * 197 + "...")
        self.assertEqual(len(got), 200)

    def test_length_is_also_held_in_utf8_bytes(self):
        got = mm.fold_notes("éè " * 100)      # two-byte letters: 200 characters would be 400 bytes
        self.assertLessEqual(len(got.encode("utf-8")), 200)
        self.assertTrue(got.endswith("..."))
        got = mm.fold_notes("€" * 100)             # three-byte characters, no space to cut at
        self.assertLessEqual(len(got.encode("utf-8")), 200)
        self.assertTrue(got.endswith("..."))

    def test_result_never_exceeds_the_limit(self):
        for n in range(150, 260):
            for word in ("ab", "abcdefg", "été"):
                got = mm.fold_notes(" ".join([word] * n))
                self.assertLessEqual(len(got), 200)
                self.assertLessEqual(len(got.encode("utf-8")), 200)


class ImageTests(unittest.TestCase):
    SMALL_OK = 102416                                   # the smallest valid size: above 102400 and a multiple of 16

    def refused(self, data):
        with self.assertRaises(mm.Refusal) as cm:
            mm.check_image(data)
        return str(cm.exception)

    def test_a_correct_image_passes(self):
        mm.check_image(make_image(self.SMALL_OK))
        mm.check_image(make_image(1412736))             # the size of a real build
        mm.check_image(make_image(200000, segments=5))

    def test_an_image_without_an_appended_hash_passes(self):
        mm.check_image(make_image(self.SMALL_OK, hash_appended=False))

    def test_size_bounds(self):
        self.assertIn("only", self.refused(make_image(102400)))          # not above 100 KB
        mm.check_image(make_image(102416))
        mm.check_image(make_image(2097152))                              # exactly one app slot
        self.assertIn("larger than one app slot", self.refused(make_image(2097168)))

    def test_a_tiny_fake_with_a_correct_header_is_too_small(self):
        tiny = make_image(112, tags=())                          # header, one segment, checksum, hash: all correct, but tiny
        self.assertEqual(tiny[0], 0xE9)
        self.assertEqual(struct.unpack_from("<H", tiny, 12)[0], 9)
        self.assertIn("only 112 bytes", self.refused(tiny))

    def test_wrong_first_byte(self):
        self.assertIn("0xE9", self.refused(make_image(self.SMALL_OK, magic=0x7F)))
        self.assertIn("0xE9", self.refused(b"\x00" * self.SMALL_OK))
        self.assertIn("0xE9", self.refused(b"<html>" + b" " * self.SMALL_OK))

    def test_wrong_chip(self):
        for chip in (0x0000, 0x0002, 0x0005, 0x000D, 0x0A09, 0xFFFF):  # ESP32, S2, C3, S3 beta lookalikes and junk
            self.assertIn("not the ESP32-S3", self.refused(make_image(self.SMALL_OK, chip=chip)), hex(chip))

    def test_segment_count_out_of_range(self):
        img = bytearray(make_image(self.SMALL_OK))
        for count in (0, 17, 255):
            img[1] = count
            self.assertIn("segments", self.refused(bytes(img)))

    def test_truncated_images(self):
        img = make_image(200016)
        for cut in (1, 16, 31, 32, 33, 4096, 100000):
            msg = self.refused(img[:-cut])
            self.assertTrue("truncated" in msg or "SHA-256" in msg or "only" in msg or "checksum" in msg, msg)

    def test_damaged_content(self):
        img = bytearray(make_image(200016))
        img[5000] ^= 0x01                               # one flipped bit in a segment
        self.assertIn("checksum", self.refused(bytes(img)))
        # fix the checksum byte but not the hash: the appended SHA-256 catches it
        good = bytearray(make_image(200016))
        good[5000] ^= 0x01
        end = len(good) - 32
        good[end - 1] ^= 0x01
        self.assertIn("SHA-256", self.refused(bytes(good)))

    def test_damaged_hash(self):
        img = bytearray(make_image(200016))
        img[-1] ^= 0xFF
        self.assertIn("SHA-256", self.refused(bytes(img)))

    def test_merged_image_with_a_tail_is_refused(self):
        img = make_image(200016)
        self.assertIn("follow", self.refused(img + b"\xff" * 1024))
        self.assertIn("follow", self.refused(make_image(200016, hash_appended=False) + b"\xff" * 4096))

    def test_a_segment_that_runs_past_the_end(self):
        img = bytearray(make_image(200016))
        struct.pack_into("<I", img, 28, 0x7FFFFFFF)     # first segment's length
        self.assertIn("past the end", self.refused(bytes(img)))


class TagTests(unittest.TestCase):
    """The version marker the firmware embeds (otalogic.h VersionTagScanner): exactly one, and it says the version published."""
    OK = 102416

    def refused(self, data, version="1.2.3"):
        with self.assertRaises(mm.Refusal) as cm:
            mm.check_version_tag(data, version)
        return str(cm.exception)

    def test_one_marker_that_matches_passes(self):
        mm.check_version_tag(make_image(self.OK, tags=("1.2.3",)), "1.2.3")
        mm.check_version_tag(make_image(self.OK, tags=("0.10.0",)), "0.10.0")
        mm.check_version_tag(make_image(self.OK, tags=("9999.9999.9999",)), "9999.9999.9999")   # the longest text the board reads

    def test_no_marker(self):
        msg = self.refused(make_image(self.OK, tags=()))
        self.assertIn("no firmware version marker", msg)

    def test_a_marker_with_another_version(self):
        msg = self.refused(make_image(self.OK, tags=("1.2.4",)), "1.2.3")
        self.assertIn("says it is version 1.2.4", msg)
        self.assertIn("published as 1.2.3", msg)

    def test_two_markers_are_refused_even_when_they_agree(self):
        self.assertIn("2 firmware version markers", self.refused(make_image(self.OK, tags=("1.2.3", "1.2.3"))))
        self.assertIn("2 firmware version markers", self.refused(make_image(self.OK, tags=("1.2.3", "1.2.4"))))

    def test_the_bare_prefix_in_the_code_is_not_a_marker(self):
        # the firmware holds the prefix as a constant (followed by a NUL, or by code); the board's scanner skips it, and so does this
        img = make_image(self.OK, tags=("1.2.3",), extra=TAG_PREFIX + b"\x00more code")
        mm.check_version_tag(img, "1.2.3")
        img = make_image(self.OK, tags=("1.2.3",), extra=TAG_PREFIX + b"abc\x03")
        mm.check_version_tag(img, "1.2.3")

    def test_prefix_without_the_end_byte_or_with_too_long_a_text_is_not_a_marker(self):
        self.assertIn("no firmware version marker", self.refused(make_image(self.OK, tags=(), extra=TAG_PREFIX + b"1.2.3\x00")))
        too_long = b"1" * 15
        self.assertIn("no firmware version marker", self.refused(make_image(self.OK, tags=(), extra=TAG_PREFIX + too_long + b"\x03")))

    def test_a_marker_whose_text_is_not_the_version_text(self):
        # same number, other text: the board compares numbers, this tool compares the text and wants the canonical one
        self.assertIn("says it is version 1.2.03", self.refused(make_image(self.OK, tags=("1.2.03",))))

    def test_a_marker_cut_between_two_places_is_not_a_marker(self):
        img = make_image(self.OK, tags=(), extra=TAG_PREFIX[:10] + b"X" + TAG_PREFIX[10:] + b"1.2.3\x03")
        self.assertIn("no firmware version marker", self.refused(img))

    def test_check_image_alone_does_not_look_for_the_marker(self):
        mm.check_image(make_image(self.OK, tags=()))


class PublishTests(TempCase):
    def image(self, name="app.bin", total=102416, fill=0x5A, **kw):
        return self.write(name, make_image(total, fill=fill, **kw))   # its marker says 1.2.3 unless tags= says otherwise

    def test_writes_the_manifest_exactly_and_copies_the_image(self):
        data = make_image(102416, tags=("0.10.1",))
        img = self.write("app.bin", data)
        rc, out, err = run_main([img, "--version", "0.10.1", "--notes", "Fixes the clock.", "--out", self.out])
        self.assertEqual((rc, err), (0, ""))
        sha = hashlib.sha256(data).hexdigest()
        self.assertEqual(out, f"version=0.10.1 size=102416 sha256={sha}\n")
        self.assertEqual(self.listing(), ["ledmatrix-0.10.1.bin", "manifest.json"])
        with open(os.path.join(self.out, "manifest.json"), "rb") as f:
            raw = f.read()
        expected = ('{"version":"0.10.1","file":"ledmatrix-0.10.1.bin","size":102416,"sha256":"%s",'
                    '"notes":"Fixes the clock."}\n' % sha)
        self.assertEqual(raw.decode("utf-8"), expected)
        with open(os.path.join(self.out, "ledmatrix-0.10.1.bin"), "rb") as f:
            self.assertEqual(f.read(), data)

    def test_manifest_matches_the_contract(self):
        img = self.image()
        rc, _, _ = run_main([img, "--version", "1.2.3", "--out", self.out])
        self.assertEqual(rc, 0)
        with open(os.path.join(self.out, "manifest.json"), "rb") as f:
            m = json.loads(f.read())
        self.assertEqual(list(m), ["version", "file", "size", "sha256", "notes"])
        self.assertEqual(m["file"], "ledmatrix-%s.bin" % m["version"])
        self.assertRegex(m["sha256"], r"^[0-9a-f]{64}$")
        self.assertEqual(m["notes"], "")
        self.assertIsInstance(m["size"], int)

    def test_output_is_deterministic(self):
        img = self.image()
        other = os.path.join(self.tmp, "second")
        for d in (self.out, other):
            self.assertEqual(run_main([img, "--version", "1.2.3", "--notes", "n", "--out", d])[0], 0)
        with open(os.path.join(self.out, "manifest.json"), "rb") as a, open(os.path.join(other, "manifest.json"), "rb") as b:
            self.assertEqual(a.read(), b.read())

    def test_notes_are_folded_and_cut(self):
        img = self.image()
        run_main([img, "--version", "1.2.3", "--notes", "line one\nline two\n\n" + "word " * 100, "--out", self.out])
        with open(os.path.join(self.out, "manifest.json"), "rb") as f:
            m = json.loads(f.read())
        self.assertTrue(m["notes"].startswith("line one line two word"))
        self.assertTrue(m["notes"].endswith("word..."))
        self.assertLessEqual(len(m["notes"]), 200)

    def test_notes_with_quotes_and_unicode_stay_valid_json(self):
        img = self.image()
        run_main([img, "--version", "1.2.3", "--notes", 'say "hi" \\ é', "--out", self.out])
        with open(os.path.join(self.out, "manifest.json"), "rb") as f:
            self.assertEqual(json.loads(f.read().decode("utf-8"))["notes"], 'say "hi" \\ é')

    def test_notes_are_written_as_utf8_not_escapes(self):
        run_main([self.image(), "--version", "1.2.3", "--notes", "caf\u00e9", "--out", self.out])
        with open(os.path.join(self.out, "manifest.json"), "rb") as f:
            self.assertIn("caf\u00e9".encode("utf-8"), f.read())

    def test_version_from_version_h(self):
        img = self.image(tags=("0.10.0",))
        vh = self.write("version.h", '#pragma once\n#define FIRMWARE_VERSION "0.10.0"\n')
        rc, out, _ = run_main([img, "--version-from", vh, "--out", self.out])
        self.assertEqual(rc, 0)
        self.assertTrue(out.startswith("version=0.10.0 size=102416 sha256="))
        self.assertEqual(self.listing(), ["ledmatrix-0.10.0.bin", "manifest.json"])

    def test_both_or_neither_version_source_is_a_usage_error(self):
        img = self.image()
        vh = self.write("version.h", '#define FIRMWARE_VERSION "0.10.0"\n')
        self.assertEqual(run_main([img, "--out", self.out])[0], 2)
        self.assertEqual(run_main([img, "--version", "1.2.3", "--version-from", vh, "--out", self.out])[0], 2)
        self.assertIsNone(self.listing())

    def test_a_bad_version_writes_nothing(self):
        img = self.image()
        for v in ("1.2", "1.2.3-rc1", "01.2.3", "1.2.3.4"):
            rc, out, err = run_main([img, "--version", v, "--out", self.out])
            self.assertEqual((rc, out), (1, ""))
            self.assertIn("MAJOR.MINOR.PATCH", err)
        self.assertIsNone(self.listing())                # the folder was not even created

    def test_a_bad_image_writes_nothing(self):
        for data in (b"", b"\xe9" * 10, make_image(102416, chip=0x0002), make_image(102416)[:-5], b"x" * 200000):
            img = self.write("bad.bin", data)
            rc, out, err = run_main([img, "--version", "1.2.3", "--out", self.out])
            self.assertEqual((rc, out), (1, ""), err)
            self.assertTrue(err.startswith("make_manifest: "))
        self.assertIsNone(self.listing())

    def test_an_image_without_or_with_the_wrong_marker_writes_nothing(self):
        for kw, text in (({"tags": ()}, "no firmware version marker"), ({"tags": ("1.2.4",)}, "says it is version 1.2.4"),
                         ({"tags": ("1.2.3", "1.2.3")}, "2 firmware version markers")):
            img = self.image(**kw)
            rc, out, err = run_main([img, "--version", "1.2.3", "--out", self.out])
            self.assertEqual((rc, out), (1, ""), err)
            self.assertIn(text, err)
        self.assertIsNone(self.listing())

    def test_version_from_must_agree_with_the_marker_too(self):
        img = self.image(tags=("0.10.1",))
        vh = self.write("version.h", '#define FIRMWARE_VERSION "0.10.0"\n')
        rc, out, err = run_main([img, "--version-from", vh, "--out", self.out])
        self.assertEqual((rc, out), (1, ""))
        self.assertIn("says it is version 0.10.1", err)
        self.assertIsNone(self.listing())

    def test_force_does_not_skip_the_marker_check(self):
        run_main([self.image("a.bin"), "--version", "1.2.3", "--out", self.out])
        before = self.snapshot()
        for kw in ({"tags": ()}, {"tags": ("1.2.4",)}, {"tags": ("1.2.3", "1.2.3")}):
            img = self.image("b.bin", fill=0x22, **kw)
            rc, out, err = run_main([img, "--version", "1.2.3", "--out", self.out, "--force"])
            self.assertEqual((rc, out), (1, ""), err)
        self.assertEqual(before, self.snapshot())

    def test_a_missing_image(self):
        rc, out, err = run_main([os.path.join(self.tmp, "nope.bin"), "--version", "1.2.3", "--out", self.out])
        self.assertEqual((rc, out), (1, ""))
        self.assertIn("cannot read the image", err)

    def test_out_that_is_a_file_is_refused(self):
        img = self.image()
        blocker = self.write("firmware", b"not a folder")
        self.assertEqual(blocker, self.out)
        rc, _, err = run_main([img, "--version", "1.2.3", "--out", self.out])
        self.assertEqual(rc, 1)
        self.assertIn("not a folder", err)

    def test_the_same_image_again_is_fine(self):
        img = self.image()
        self.assertEqual(run_main([img, "--version", "1.2.3", "--notes", "one", "--out", self.out])[0], 0)
        self.assertEqual(run_main([img, "--version", "1.2.3", "--notes", "two", "--out", self.out])[0], 0)
        with open(os.path.join(self.out, "manifest.json"), "rb") as f:
            self.assertEqual(json.loads(f.read())["notes"], "two")

    def test_a_release_is_immutable(self):
        first = self.image("a.bin", fill=0x11)
        second = self.image("b.bin", fill=0x22)
        self.assertEqual(run_main([first, "--version", "1.2.3", "--out", self.out])[0], 0)
        before = self.snapshot()
        rc, out, err = run_main([second, "--version", "1.2.3", "--out", self.out])
        self.assertEqual((rc, out), (1, ""))
        self.assertIn("immutable", err)
        after = self.snapshot()
        self.assertEqual(before, after)

    def test_an_image_under_the_same_version_with_other_bytes_is_refused_even_without_a_manifest(self):
        self.assertEqual(run_main([self.image("a.bin", fill=0x11), "--version", "1.2.3", "--out", self.out])[0], 0)
        os.unlink(os.path.join(self.out, "manifest.json"))
        rc, _, err = run_main([self.image("b.bin", fill=0x22), "--version", "1.2.3", "--out", self.out])
        self.assertEqual(rc, 1)
        self.assertIn("already holds a different image", err)

    def test_a_manifest_for_the_same_version_with_another_hash_is_refused(self):
        first = self.image("a.bin", fill=0x11)
        second = self.image("b.bin", fill=0x22)
        self.assertEqual(run_main([first, "--version", "1.2.3", "--out", self.out])[0], 0)
        os.unlink(os.path.join(self.out, "ledmatrix-1.2.3.bin"))        # the image is gone, the manifest still names it
        rc, _, err = run_main([second, "--version", "1.2.3", "--out", self.out])
        self.assertEqual(rc, 1)
        self.assertIn("immutable", err)

    def test_force_replaces_a_release(self):
        first = self.image("a.bin", fill=0x11)
        second_data = make_image(102416, fill=0x22)
        second = self.write("b.bin", second_data)
        run_main([first, "--version", "1.2.3", "--out", self.out])
        rc, out, _ = run_main([second, "--version", "1.2.3", "--out", self.out, "--force"])
        self.assertEqual(rc, 0)
        self.assertIn(hashlib.sha256(second_data).hexdigest(), out)
        with open(os.path.join(self.out, "ledmatrix-1.2.3.bin"), "rb") as f:
            self.assertEqual(f.read(), second_data)

    def test_a_new_version_leaves_the_old_image_in_place(self):
        run_main([self.image("a.bin", fill=0x11), "--version", "1.2.3", "--out", self.out])
        run_main([self.image("b.bin", fill=0x22, tags=("1.2.4",)), "--version", "1.2.4", "--out", self.out])
        self.assertEqual(self.listing(), ["ledmatrix-1.2.3.bin", "ledmatrix-1.2.4.bin", "manifest.json"])
        with open(os.path.join(self.out, "manifest.json"), "rb") as f:
            self.assertEqual(json.loads(f.read())["version"], "1.2.4")

    def test_no_temporary_files_are_left(self):
        run_main([self.image(), "--version", "1.2.3", "--out", self.out])
        self.assertEqual(self.listing(), ["ledmatrix-1.2.3.bin", "manifest.json"])

    def test_nested_out_folder_is_created(self):
        nested = os.path.join(self.tmp, "a", "b", "firmware")
        self.assertEqual(run_main([self.image(), "--version", "1.2.3", "--out", nested])[0], 0)
        self.assertTrue(os.path.isfile(os.path.join(nested, "manifest.json")))


class CommandLineTests(TempCase):
    """The real program, started the way the workflow starts it: python3 -I, no imports from the current folder."""

    def run_tool(self, *args):
        return subprocess.run([sys.executable, "-I", TOOL, *args], capture_output=True, text=True, cwd=self.tmp)

    def test_one_line_on_stdout(self):
        data = make_image(102416, tags=("0.10.1",))
        img = self.write("app.bin", data)
        r = self.run_tool(img, "--version", "0.10.1", "--notes=- starts with a dash", "--out", self.out)
        self.assertEqual((r.returncode, r.stderr), (0, ""))
        self.assertEqual(r.stdout, f"version=0.10.1 size=102416 sha256={hashlib.sha256(data).hexdigest()}\n")
        with open(os.path.join(self.out, "manifest.json"), "rb") as f:
            self.assertEqual(json.loads(f.read())["notes"], "- starts with a dash")

    def test_version_from_and_a_refusal(self):
        img = self.write("app.bin", make_image(102416, tags=("0.10.0",)))
        vh = self.write("version.h", '#define FIRMWARE_VERSION "0.10.0"\n')
        r = self.run_tool(img, "--version-from", vh, "--out", self.out)
        self.assertEqual(r.returncode, 0)
        other = self.write("other.bin", make_image(102416, fill=0x77, tags=("0.10.0",)))
        r = self.run_tool(other, "--version-from", vh, "--out", self.out)
        self.assertEqual((r.returncode, r.stdout), (1, ""))
        self.assertIn("immutable", r.stderr)

    def test_no_bytecode_is_written_next_to_the_tool(self):
        cache = os.path.join(HERE, "__pycache__")

        def mine():
            return {n for n in (os.listdir(cache) if os.path.isdir(cache) else []) if n.startswith("make_manifest.")}

        before = mine()
        self.write("app.bin", make_image(102416))
        self.run_tool(os.path.join(self.tmp, "app.bin"), "--version", "1.2.3", "--out", self.out)
        self.assertEqual(mine(), before)


if __name__ == "__main__":
    unittest.main(verbosity=1)
