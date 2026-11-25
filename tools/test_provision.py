#!/usr/bin/env python3
"""Tests for tools/provision.py, against the shared known-answer vectors (shared/device-auth-vectors.json).
Run from anywhere (Python 3, standard library only):
    python3 -I tools/test_provision.py
Every secret, ID and host in here is a placeholder: the vectors' own values and example.com style names. Nothing is flashed and
no serial port is touched (the flash command is only assembled, with subprocess.run replaced)."""
import hashlib
import hmac
import http.server
import importlib.util
import io
import json
import os
import re
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
import zlib
from contextlib import redirect_stderr, redirect_stdout
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
PROVISION = os.path.join(HERE, "provision.py")
VECTORS = os.path.join(HERE, "..", "shared", "device-auth-vectors.json")

sys.dont_write_bytecode = True  # no tools/__pycache__ in the repository
spec = importlib.util.spec_from_file_location("provision", PROVISION)  # by path: `python3 -I` does not put this folder on sys.path
prov = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prov)

with open(VECTORS, encoding="utf-8") as f:
    V = json.load(f)
MASTER = V["master_secret"]
MASTER_PADDED = V["master_secret_with_whitespace"]
DEVICE_ID = V["device_id"]
KEY_HEX = V["device_key_hex"]
KEY = bytes.fromhex(KEY_HEX)


def expected_sector(device_id, key, flags=0):
    """The record as shared/device-auth.md lays it out, written out independently of provision.py."""
    payload = device_id.encode() + bytes([flags]) + b"\x00\x00\x00" + key
    header = b"IDNT" + (1).to_bytes(2, "little") + (48).to_bytes(2, "little") + zlib.crc32(payload).to_bytes(4, "little")
    return header + payload + b"\xff" * (4096 - 60)


class TempDirCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = os.path.realpath(self._tmp.name)

    def path(self, name):
        return os.path.join(self.dir, name)

    def write(self, name, data, mode=0o600):
        p = self.path(name)
        with open(p, "wb") as f:
            f.write(data if isinstance(data, bytes) else data.encode())
        os.chmod(p, mode)
        return p

    def run_main(self, *argv, env=None):
        """prov.main in this process, with stdout and stderr captured; returns (exit code, stdout, stderr)."""
        out, err = io.StringIO(), io.StringIO()
        old = dict(os.environ)
        try:
            if env is not None:
                os.environ.pop(prov.SECRET_FILE_ENV, None)
                os.environ.update(env)
            with redirect_stdout(out), redirect_stderr(err):
                try:
                    rc = prov.main(list(argv))
                except SystemExit as e:  # argparse
                    rc = e.code
        finally:
            os.environ.clear()
            os.environ.update(old)
        return rc, out.getvalue(), err.getvalue()


class VectorsAnchor(unittest.TestCase):
    def test_rfc4231_case2(self):
        a = V["primitive_rfc4231_case2"]
        self.assertEqual(hmac.new(bytes.fromhex(a["key_hex"]), a["message"].encode(), hashlib.sha256).hexdigest(), a["hmac"])

    def test_master_has_at_least_32_characters(self):
        self.assertGreaterEqual(len(MASTER), 32)
        self.assertEqual(MASTER_PADDED.strip(" \t\r\n"), MASTER)
        self.assertNotEqual(MASTER_PADDED, MASTER)


class KeyDerivation(TempDirCase):
    def test_device_key_vector(self):
        self.assertEqual(prov.derive_key(MASTER.encode(), DEVICE_ID).hex(), KEY_HEX)

    def test_whitespace_trimmed_master_gives_the_same_key(self):
        p = self.write("secret", MASTER_PADDED)
        secret = prov.read_secret(p)
        self.assertEqual(secret, MASTER.encode())
        self.assertEqual(prov.derive_key(secret, DEVICE_ID).hex(), KEY_HEX)

    def test_only_space_tab_cr_lf_are_trimmed(self):
        p = self.write("secret", " \t\r\n" + MASTER + "\n\n\t ")
        self.assertEqual(prov.read_secret(p), MASTER.encode())
        # a form feed or a vertical tab is part of the secret (the contract names four characters)
        p = self.write("secret2", "\x0c" + MASTER + "\x0b")
        self.assertEqual(prov.read_secret(p), ("\x0c" + MASTER + "\x0b").encode())
        # whitespace inside stays
        p = self.write("secret3", MASTER[:10] + "\n" + MASTER[10:])
        self.assertEqual(prov.read_secret(p), (MASTER[:10] + "\n" + MASTER[10:]).encode())

    def test_other_ids_give_other_keys(self):
        k = prov.derive_key(MASTER.encode(), "aabbccddee00")
        self.assertNotEqual(k.hex(), KEY_HEX)
        self.assertEqual(len(k), 32)

    def test_bad_device_ids_are_refused(self):
        for bad in V["bad_device_ids"]:
            self.assertFalse(prov.valid_device_id(bad), bad)
            with self.assertRaises(prov.ProvisionError):
                prov.derive_key(MASTER.encode(), bad)
        self.assertFalse(prov.valid_device_id(None))
        self.assertFalse(prov.valid_device_id("aabbccddeeff\n"))
        self.assertFalse(prov.valid_device_id("aabbccddeeff "))
        self.assertTrue(prov.valid_device_id(DEVICE_ID))

    def test_short_secrets_are_refused(self):
        for n in (0, 1, 31):
            text = ("Qx7!" * 8)[:n]
            p = self.write("short", text)
            with self.assertRaises(prov.ProvisionError) as cm:
                prov.read_secret(p)
            if text:
                self.assertNotIn(text, str(cm.exception))
        # 31 characters with whitespace around them: still 31 after the trim
        p = self.write("short2", "  " + "s" * 31 + "\r\n")
        with self.assertRaises(prov.ProvisionError):
            prov.read_secret(p)
        # exactly 32 is fine
        p = self.write("ok", "s" * 32)
        self.assertEqual(prov.read_secret(p), b"s" * 32)
        # characters, not bytes: 16 two-byte characters are 32 bytes but only 16 characters
        p = self.write("wide", "é" * 16)
        with self.assertRaises(prov.ProvisionError):
            prov.read_secret(p)
        p = self.write("wide32", "é" * 32)
        self.assertEqual(len(prov.read_secret(p)), 64)

    def test_missing_unreadable_and_binary_secret_files(self):
        with self.assertRaises(prov.ProvisionError):
            prov.read_secret(self.path("nothing-here"))
        with self.assertRaises(prov.ProvisionError):
            prov.read_secret(self.dir)  # a directory
        p = self.write("binary", b"\xff\xfe" * 40)
        with self.assertRaises(prov.ProvisionError) as cm:
            prov.read_secret(p)
        self.assertNotIn("ff", str(cm.exception).lower().replace("utf-8", ""))
        p = self.write("huge", b"a" * (1 << 16))
        with self.assertRaises(prov.ProvisionError):
            prov.read_secret(p)


class RecordBytes(unittest.TestCase):
    def test_sector_layout(self):
        s = prov.build_sector(DEVICE_ID, KEY)
        self.assertEqual(len(s), 4096)
        self.assertEqual(s, expected_sector(DEVICE_ID, KEY))
        self.assertEqual(s[0:4], b"IDNT")
        self.assertEqual(struct.unpack("<I", s[0:4])[0], 0x544E4449)
        self.assertEqual(struct.unpack("<H", s[4:6])[0], 1)
        self.assertEqual(struct.unpack("<H", s[6:8])[0], 48)
        self.assertEqual(struct.unpack("<I", s[8:12])[0], zlib.crc32(s[12:60]))
        self.assertEqual(s[12:24], DEVICE_ID.encode())
        self.assertEqual(s[24], 0)
        self.assertEqual(s[25:28], b"\x00\x00\x00")
        self.assertEqual(s[28:60], KEY)
        self.assertEqual(s[60:], b"\xff" * 4036)

    def test_image_is_two_identical_copies(self):
        img = prov.build_image(DEVICE_ID, KEY)
        self.assertEqual(len(img), 8192)
        self.assertEqual(img[:4096], img[4096:])
        self.assertEqual(img[:4096], expected_sector(DEVICE_ID, KEY))

    def test_burned_flag(self):
        s = prov.build_sector(DEVICE_ID, b"\x00" * 32, flags=1)
        self.assertEqual(s, expected_sector(DEVICE_ID, b"\x00" * 32, flags=1))
        self.assertEqual(prov.parse_sector(s), DEVICE_ID)

    def test_crc_is_the_plain_zlib_crc32(self):
        self.assertEqual(zlib.crc32(b"123456789"), 0xCBF43926)  # the check value of the standard
        img = prov.build_image(DEVICE_ID, KEY)
        self.assertEqual(prov.image_crc(img), zlib.crc32(img) & 0xFFFFFFFF)

    def test_bad_inputs(self):
        with self.assertRaises(prov.ProvisionError):
            prov.build_sector("AABBCCDDEEFF", KEY)
        with self.assertRaises(prov.ProvisionError):
            prov.build_sector(DEVICE_ID, KEY[:31])


class CheckImage(unittest.TestCase):
    def setUp(self):
        self.img = prov.build_image(DEVICE_ID, KEY)

    def test_good_image(self):
        self.assertEqual(prov.check_image(self.img), DEVICE_ID)

    def refused(self, img):
        with self.assertRaises(prov.ProvisionError) as cm:
            prov.check_image(bytes(img))
        self.assertNotIn(KEY_HEX, str(cm.exception))
        return str(cm.exception)

    def test_wrong_size(self):
        self.refused(self.img[:-1])
        self.refused(self.img + b"\xff")
        self.refused(b"")
        self.refused(self.img[:4096])

    def test_bad_magic_version_length_crc(self):
        for i in (0, 1, 2, 3):
            b = bytearray(self.img)
            b[i] ^= 1
            self.assertIn("magic", self.refused(b))
        b = bytearray(self.img)
        b[4] = 2
        self.assertIn("version", self.refused(b))
        b = bytearray(self.img)
        b[6] = 49
        self.assertIn("length", self.refused(b))
        b = bytearray(self.img)
        b[40] ^= 1
        self.assertIn("checksum", self.refused(b))
        b = bytearray(self.img)
        b[8] ^= 1
        self.assertIn("checksum", self.refused(b))

    def test_second_copy_is_checked_too(self):
        b = bytearray(self.img)
        b[4096 + 40] ^= 1
        self.assertIn("sector 1", self.refused(b))

    def test_copies_must_be_identical(self):
        other = prov.build_sector("112233445566", KEY)
        self.assertIn("identical", self.refused(prov.build_sector(DEVICE_ID, KEY) + other))

    def test_rest_of_the_sector_and_reserved_bytes(self):
        b = bytearray(self.img)
        b[100] = 0
        self.assertIn("0xFF", self.refused(b))
        s = bytearray(prov.build_sector(DEVICE_ID, KEY))
        s[26] = 1
        s[8:12] = zlib.crc32(bytes(s[12:60])).to_bytes(4, "little")  # a valid checksum over a reserved byte that is not zero
        self.assertIn("reserved", self.refused(bytes(s) + bytes(s)))

    def test_flags(self):
        s = bytearray(prov.build_sector(DEVICE_ID, KEY))
        s[24] = 2
        s[8:12] = zlib.crc32(bytes(s[12:60])).to_bytes(4, "little")
        self.assertIn("flag", self.refused(bytes(s) + bytes(s)))
        s = prov.build_sector(DEVICE_ID, KEY, flags=1)  # burned, but the key is not zero
        self.assertIn("burned", self.refused(s + s))
        s = prov.build_sector(DEVICE_ID, b"\x00" * 32, flags=1)
        self.assertEqual(prov.check_image(s + s), DEVICE_ID)

    def test_device_id_in_the_record(self):
        for bad in ("AABBCCDDEEFF", "aa:bb:cc:dd:", "aabbccddeefg", "\xe9" * 6):
            payload = bad.encode("latin-1") + b"\x00" * 4 + KEY
            payload = payload[:12] + b"\x00\x00\x00\x00" + KEY
            record = struct.pack("<IHHI", prov.MAGIC, 1, 48, zlib.crc32(payload)) + payload
            sector = record + b"\xff" * (4096 - 60)
            self.refused(sector + sector)


class Cli(TempDirCase):
    def setUp(self):
        super().setUp()
        self.secret = self.write("master", MASTER_PADDED)
        self.out = self.path("ident.bin")

    def args(self, *extra):
        return ["--device-id", DEVICE_ID, "--secret-file", self.secret, "--out", self.out, *extra]

    def test_writes_the_image_and_prints_only_the_id_and_the_crc(self):
        rc, out, err = self.run_main(*self.args())
        self.assertEqual(rc, 0, err)
        with open(self.out, "rb") as f:
            img = f.read()
        self.assertEqual(img, expected_sector(DEVICE_ID, KEY) * 2)
        self.assertEqual(out, "device=%s crc32=%08x\n" % (DEVICE_ID, zlib.crc32(img)))
        self.assertEqual(err, "")

    def test_file_mode_is_0600(self):
        old = os.umask(0)  # even a umask that would allow more
        try:
            rc, _, err = self.run_main(*self.args())
        finally:
            os.umask(old)
        self.assertEqual(rc, 0, err)
        self.assertEqual(stat.S_IMODE(os.stat(self.out).st_mode), 0o600)

    def test_an_existing_file_is_tightened_and_replaced(self):
        with open(self.out, "wb") as f:
            f.write(b"old" * 5000)
        os.chmod(self.out, 0o644)
        rc, _, err = self.run_main(*self.args())
        self.assertEqual(rc, 0, err)
        self.assertEqual(stat.S_IMODE(os.stat(self.out).st_mode), 0o600)
        self.assertEqual(os.path.getsize(self.out), 8192)

    def test_not_through_a_symbolic_link(self):
        target = self.write("target", b"keep")
        os.symlink(target, self.out)
        rc, _, err = self.run_main(*self.args())
        self.assertEqual(rc, 2)
        with open(target, "rb") as f:
            self.assertEqual(f.read(), b"keep")

    def test_secret_from_the_environment(self):
        rc, out, err = self.run_main("--device-id", DEVICE_ID, "--out", self.out, env={prov.SECRET_FILE_ENV: self.secret})
        self.assertEqual(rc, 0, err)
        self.assertIn(DEVICE_ID, out)
        with open(self.out, "rb") as f:
            self.assertEqual(f.read(), expected_sector(DEVICE_ID, KEY) * 2)

    def test_the_flag_wins_over_the_environment(self):
        other = self.write("other", "x" * 40)
        rc, _, err = self.run_main(*self.args(), env={prov.SECRET_FILE_ENV: other})
        self.assertEqual(rc, 0, err)
        with open(self.out, "rb") as f:
            self.assertEqual(f.read(), expected_sector(DEVICE_ID, KEY) * 2)

    def test_no_secret_file_at_all(self):
        rc, _, err = self.run_main("--device-id", DEVICE_ID, "--out", self.out, env={})
        self.assertEqual(rc, 2)
        self.assertIn(prov.SECRET_FILE_ENV, err)
        self.assertFalse(os.path.exists(self.out))

    def test_the_secret_is_not_a_command_line_argument(self):
        for opt in ("--secret", "--master-secret", "--key"):
            rc, _, err = self.run_main("--device-id", DEVICE_ID, opt, MASTER, "--out", self.out)
            self.assertNotEqual(rc, 0)
            self.assertFalse(os.path.exists(self.out))
            self.assertNotIn(MASTER, err)  # argparse names the unknown option, not its value
        # and an abbreviation of --secret-file is not accepted either
        rc, _, err = self.run_main("--device-id", DEVICE_ID, "--secret", self.secret, "--out", self.out)
        self.assertNotEqual(rc, 0)

    def test_short_secret_is_refused_and_nothing_is_written(self):
        short = self.write("short", "tooshort" * 3)
        rc, out, err = self.run_main("--device-id", DEVICE_ID, "--secret-file", short, "--out", self.out)
        self.assertEqual(rc, 2)
        self.assertEqual(out, "")
        self.assertIn("32", err)
        self.assertNotIn("tooshort", err)
        self.assertFalse(os.path.exists(self.out))

    def test_bad_device_ids_are_refused_and_nothing_is_written(self):
        for bad in V["bad_device_ids"]:
            rc, out, err = self.run_main("--device-id", bad, "--secret-file", self.secret, "--out", self.out)
            self.assertNotEqual(rc, 0, bad)
            self.assertEqual(out, "")
            self.assertFalse(os.path.exists(self.out))

    def test_exactly_one_source_for_the_device_id(self):
        rc, _, _ = self.run_main("--secret-file", self.secret, "--out", self.out)
        self.assertEqual(rc, 2)
        rc, _, _ = self.run_main(*self.args("--from-board", "127.0.0.1"))
        self.assertEqual(rc, 2)

    def test_out_is_needed(self):
        rc, _, err = self.run_main("--device-id", DEVICE_ID, "--secret-file", self.secret)
        self.assertEqual(rc, 2)

    def test_nothing_is_written_inside_the_repository(self):
        inside = os.path.join(prov.REPO, "ident-test-must-not-exist.bin")
        rc, _, err = self.run_main("--device-id", DEVICE_ID, "--secret-file", self.secret, "--out", inside)
        self.assertEqual(rc, 2)
        self.assertFalse(os.path.exists(inside))
        self.assertIn("outside the repository", err)
        rc, _, err = self.run_main("--device-id", DEVICE_ID, "--secret-file", os.path.join(prov.REPO, "README.md"), "--out", self.out)
        self.assertEqual(rc, 2)
        self.assertIn("outside the repository", err)

    def test_check_command(self):
        self.assertEqual(self.run_main(*self.args())[0], 0)
        rc, out, err = self.run_main("--check", self.out)
        self.assertEqual(rc, 0, err)
        with open(self.out, "rb") as f:
            self.assertEqual(out, "device=%s crc32=%08x\n" % (DEVICE_ID, zlib.crc32(f.read())))
        # a damaged image
        with open(self.out, "r+b") as f:
            f.seek(30)
            f.write(b"\x00")
        rc, out, err = self.run_main("--check", self.out)
        self.assertEqual(rc, 2)
        self.assertEqual(out, "")
        self.assertNotIn(KEY_HEX, err)
        rc, _, _ = self.run_main("--check", self.path("nothing-here"))
        self.assertEqual(rc, 2)


class NoSecretInTheOutput(TempDirCase):
    """Runs the real script as a separate process, the way the owner does (python3 -I), and looks at everything it printed."""

    def leaks(self, text):
        secrets = [MASTER, KEY_HEX, KEY_HEX.upper(), "dkVjYWxs"]  # the secret, the key in hex
        secrets.append(__import__("base64").b64encode(KEY).decode())
        secrets.append(__import__("base64").b64encode(MASTER.encode()).decode())
        secrets.append(MASTER.encode().hex())
        return [s for s in secrets if s in text]

    def run_script(self, *argv, env=None):
        e = {k: v for k, v in os.environ.items() if k != prov.SECRET_FILE_ENV}
        e.update(env or {})
        p = subprocess.run([sys.executable, "-I", PROVISION, *argv], capture_output=True, text=True, env=e)
        return p.returncode, p.stdout, p.stderr

    def test_success(self):
        secret = self.write("master", MASTER_PADDED)
        out_path = self.path("ident.bin")
        rc, out, err = self.run_script("--device-id", DEVICE_ID, "--out", out_path, env={prov.SECRET_FILE_ENV: secret})
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.leaks(out), [])
        self.assertEqual(self.leaks(err), [])
        self.assertRegex(out, r"^device=aabbccddeeff crc32=[0-9a-f]{8}\n$")
        self.assertEqual(err, "")
        self.assertEqual(stat.S_IMODE(os.stat(out_path).st_mode), 0o600)
        with open(out_path, "rb") as f:
            self.assertEqual(f.read(), expected_sector(DEVICE_ID, KEY) * 2)
        # and --check says the same
        rc, out2, err2 = self.run_script("--check", out_path)
        self.assertEqual((rc, err2), (0, ""))
        self.assertEqual(out2, out)

    def test_failures_say_nothing_secret(self):
        secret = self.write("master", MASTER_PADDED)
        short = self.write("short", MASTER[:20])
        out_path = self.path("ident.bin")
        cases = [
            ["--device-id", "AABBCCDDEEFF", "--secret-file", secret, "--out", out_path],
            ["--device-id", DEVICE_ID, "--secret-file", short, "--out", out_path],
            ["--device-id", DEVICE_ID, "--secret-file", self.path("nothing-here"), "--out", out_path],
            ["--device-id", DEVICE_ID, "--secret-file", secret, "--out", self.path("no-such-dir/ident.bin")],
            ["--device-id", DEVICE_ID, "--secret", MASTER, "--out", out_path],
            ["--device-id", DEVICE_ID, "--secret-file", secret],
            ["--check", secret],
            ["--help"],
        ]
        for argv in cases:
            rc, out, err = self.run_script(*argv)
            self.assertEqual(self.leaks(out), [], argv)
            self.assertEqual(self.leaks(err), [], argv)
        self.assertFalse(os.path.exists(out_path))

    def test_isolated_mode_needs_nothing_but_the_standard_library(self):
        rc, out, err = self.run_script("--help")
        self.assertEqual(rc, 0, err)
        self.assertIn("--secret-file", out)
        self.assertNotIn("--secret ", out)  # there is no option that takes the secret itself


class StatusHandler(http.server.BaseHTTPRequestHandler):
    reply = (200, b'{"ssid":"example","device":"aabbccddeeff","identity":"missing"}')

    def do_GET(self):
        if self.path != "/status":
            self.send_response(404)
            self.end_headers()
            return
        code, body = type(self).reply
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


class FromBoard(TempDirCase):
    def setUp(self):
        super().setUp()
        self.srv = http.server.HTTPServer(("127.0.0.1", 0), StatusHandler)
        self.addCleanup(self.srv.server_close)
        t = threading.Thread(target=self.srv.serve_forever, daemon=True)
        t.start()
        self.addCleanup(self.srv.shutdown)
        self.host = "127.0.0.1:%d" % self.srv.server_address[1]
        StatusHandler.reply = (200, b'{"ssid":"example","device":"aabbccddeeff","identity":"missing"}')

    def test_reads_the_device_id(self):
        self.assertEqual(prov.device_id_from_board(self.host), DEVICE_ID)

    def test_through_the_cli(self):
        secret = self.write("master", MASTER_PADDED)
        out_path = self.path("ident.bin")
        old = dict(os.environ)
        os.environ["http_proxy"] = "http://127.0.0.1:9"  # a proxy must be ignored for a board on the LAN
        try:
            out, err = io.StringIO(), io.StringIO()
            with redirect_stdout(out), redirect_stderr(err):
                rc = prov.main(["--from-board", self.host, "--secret-file", secret, "--out", out_path])
        finally:
            os.environ.clear()
            os.environ.update(old)
        self.assertEqual(rc, 0, err.getvalue())
        self.assertTrue(out.getvalue().startswith("device=aabbccddeeff crc32="))
        with open(out_path, "rb") as f:
            self.assertEqual(f.read(), expected_sector(DEVICE_ID, KEY) * 2)

    def test_what_the_board_may_answer(self):
        for reply in [(200, b'{"device":"AABBCCDDEEFF"}'), (200, b'{"device":"aabbccddeef"}'), (200, b'{"device":42}'), (200, b'{"ssid":"x"}'),
                      (200, b'not json'), (200, b'[]'), (200, b'\xff\xfe'), (500, b'{"device":"aabbccddeeff"}'), (404, b'{}')]:
            StatusHandler.reply = reply
            with self.assertRaises(prov.ProvisionError, msg=reply):
                prov.device_id_from_board(self.host)

    def test_a_board_that_is_not_there(self):
        srv = http.server.HTTPServer(("127.0.0.1", 0), StatusHandler)
        port = srv.server_address[1]
        srv.server_close()  # nothing listens on this port now
        with self.assertRaises(prov.ProvisionError):
            prov.device_id_from_board("127.0.0.1:%d" % port)

    def test_the_address_is_checked(self):
        for bad in ("", "ho st", "host/path", "host?x=1", "user@host", "host#x", "http://host", "-x y"):
            with self.assertRaises(prov.ProvisionError, msg=bad):
                prov.device_id_from_board(bad)


class Flash(TempDirCase):
    EXTRA = '0x410000 "/example/core/variants/adafruit_matrixportal_esp32s3/tinyuf2.bin"'

    def test_upload_command(self):
        cmd = prov.upload_command("/dev/cu.example", "/example/build", "/example/ident.bin", self.EXTRA)
        self.assertEqual(cmd, [
            "arduino-cli", "upload", "--fqbn", "esp32:esp32:adafruit_matrixportal_esp32s3", "--port", "/dev/cu.example",
            "--input-dir", "/example/build", "--upload-property",
            'upload.extra_flags=0x410000 "/example/core/variants/adafruit_matrixportal_esp32s3/tinyuf2.bin" 0x7FE000 "/example/ident.bin"',
            prov.SKETCH_DIR])
        with self.assertRaises(prov.ProvisionError):
            prov.upload_command("p", "d", '/example/id"ent.bin', self.EXTRA)

    def test_flash_reads_the_boards_own_flags_then_uploads(self):
        calls = []

        def fake_run(cmd, **kw):
            calls.append((list(cmd), kw))
            if "--show-properties" in cmd:
                return subprocess.CompletedProcess(cmd, 0, stdout="build.foo=1\nupload.extra_flags=" + self.EXTRA + "\nupload.flags=\n", stderr="")
            return subprocess.CompletedProcess(cmd, 0)

        image = self.write("ident.bin", b"x" * 8192)
        with mock.patch.object(prov.subprocess, "run", fake_run):
            prov.flash("/dev/cu.example", "/example/build", image)
        self.assertEqual(len(calls), 2)
        self.assertEqual(calls[0][0], ["arduino-cli", "compile", "--fqbn", prov.FQBN, "--show-properties", prov.SKETCH_DIR])
        up = calls[1][0]
        self.assertEqual(up[:2], ["arduino-cli", "upload"])
        self.assertIn("--upload-property", up)
        self.assertEqual(up[up.index("--upload-property") + 1], 'upload.extra_flags=%s %s "%s"' % (self.EXTRA, "0x7FE000", image))
        self.assertEqual(up[up.index("--port") + 1], "/dev/cu.example")
        self.assertEqual(up[up.index("--input-dir") + 1], "/example/build")
        self.assertNotIn("esptool", " ".join(up))  # arduino-cli only, never esptool by hand

    def test_a_failed_upload_is_reported(self):
        def fake_run(cmd, **kw):
            if "--show-properties" in cmd:
                return subprocess.CompletedProcess(cmd, 0, stdout="upload.extra_flags=" + self.EXTRA + "\n", stderr="")
            return subprocess.CompletedProcess(cmd, 3)

        with mock.patch.object(prov.subprocess, "run", fake_run):
            with self.assertRaises(prov.ProvisionError):
                prov.flash("p", "d", self.path("ident.bin"))

    def test_no_extra_flags_property(self):
        def fake_run(cmd, **kw):
            return subprocess.CompletedProcess(cmd, 0, stdout="build.foo=1\n", stderr="")

        with mock.patch.object(prov.subprocess, "run", fake_run):
            with self.assertRaises(prov.ProvisionError):
                prov.current_extra_flags()

    def test_flash_option_checks_and_the_order_in_the_cli(self):
        secret = self.write("master", MASTER_PADDED)
        out_path = self.path("ident.bin")
        rc, _, err = self.run_main("--device-id", DEVICE_ID, "--secret-file", secret, "--out", out_path, "--flash")
        self.assertEqual(rc, 2)
        self.assertIn("--port", err)
        self.assertFalse(os.path.exists(out_path))  # refused before anything is written
        seen = []

        def fake_flash(port, input_dir, image):
            seen.append((port, input_dir, image, os.path.getsize(image)))

        with mock.patch.object(prov, "flash", fake_flash):
            rc, out, err = self.run_main("--device-id", DEVICE_ID, "--secret-file", secret, "--out", out_path,
                                         "--flash", "--port", "/dev/cu.example", "--input-dir", "/example/build")
        self.assertEqual(rc, 0, err)
        self.assertEqual(seen, [("/dev/cu.example", "/example/build", out_path, 8192)])  # the image exists before the upload starts

    def flash_run(self, fake_flash, *extra):
        secret = self.write("master", MASTER_PADDED)
        out_path = self.path("ident.bin")
        with mock.patch.object(prov, "flash", fake_flash):
            rc, out, err = self.run_main("--device-id", DEVICE_ID, "--secret-file", secret, "--out", out_path,
                                         "--flash", "--port", "/dev/cu.example", "--input-dir", "/example/build", *extra)
        return out_path, rc, out, err

    def test_the_image_is_deleted_after_a_successful_flash(self):
        out_path, rc, out, err = self.flash_run(lambda *a: None)
        self.assertEqual(rc, 0, err)
        self.assertFalse(os.path.exists(out_path))  # it held the device key
        lines = out.splitlines()
        self.assertEqual(len(lines), 2)
        self.assertRegex(lines[0], r"^device=aabbccddeeff crc32=[0-9a-f]{8}$")
        self.assertIn("deleted the image file", lines[1])
        self.assertIn("--keep", lines[1])
        self.assertEqual(err, "")

    def test_keep_keeps_the_image(self):
        out_path, rc, out, err = self.flash_run(lambda *a: None, "--keep")
        self.assertEqual(rc, 0, err)
        self.assertEqual(os.path.getsize(out_path), 8192)
        self.assertEqual(stat.S_IMODE(os.stat(out_path).st_mode), 0o600)
        self.assertIn("kept the image file", out.splitlines()[1])
        self.assertNotIn(KEY_HEX, out + err)

    def test_the_image_is_kept_after_a_failed_flash(self):
        def failing(port, input_dir, image):
            raise prov.ProvisionError("arduino-cli upload failed (exit code 3)")

        out_path, rc, out, err = self.flash_run(failing)
        self.assertEqual(rc, 2)
        self.assertEqual(os.path.getsize(out_path), 8192)  # so the flash can be tried again
        self.assertIn("upload failed", err)
        self.assertIn("image file was kept", err)
        self.assertEqual(len(out.splitlines()), 1)  # only the device line: nothing was deleted
        self.assertNotIn(KEY_HEX, out + err)

    def test_a_failed_delete_is_reported(self):
        with mock.patch.object(prov.os, "remove", side_effect=PermissionError(1, "Operation not permitted")):
            out_path, rc, out, err = self.flash_run(lambda *a: None)
        self.assertEqual(rc, 2)
        self.assertIn("could not be deleted", err)
        self.assertIn(out_path, err)  # tells the owner which file to delete by hand
        self.assertTrue(os.path.exists(out_path))

    def test_no_flash_no_delete(self):
        secret = self.write("master", MASTER_PADDED)
        out_path = self.path("ident.bin")
        rc, out, err = self.run_main("--device-id", DEVICE_ID, "--secret-file", secret, "--out", out_path)
        self.assertEqual(rc, 0, err)
        self.assertEqual(os.path.getsize(out_path), 8192)  # without --flash the file is the whole point
        self.assertEqual(len(out.splitlines()), 1)

    def run_main(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            try:
                rc = prov.main(list(argv))
            except SystemExit as e:
                rc = e.code
        return rc, out.getvalue(), err.getvalue()


class Script(unittest.TestCase):
    def test_executable_and_isolated(self):
        self.assertTrue(os.access(PROVISION, os.X_OK))
        with open(PROVISION, encoding="utf-8") as f:
            first = f.readline()
        self.assertTrue(first.startswith("#!"))
        with open(PROVISION, encoding="utf-8") as f:
            text = f.read()
        mods = set(re.findall(r"^\s*(?:import|from)\s+([A-Za-z_][A-Za-z0-9_]*)", text, re.M))
        self.assertTrue(mods <= set(sys.stdlib_module_names), mods - set(sys.stdlib_module_names))  # standard library only


if __name__ == "__main__":
    unittest.main(verbosity=1)
