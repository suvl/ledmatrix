#!/usr/bin/env python3
"""Provisions a board's identity (tickets 37 and 39): derives the board's device key from the owner's master secret and the
board's device ID, and writes the 8 KB identity image for the read-only `ident` partition at 0x7FE000.

The contract is shared/device-auth.md; the vectors that prove this file are shared/device-auth-vectors.json (tools/test_provision.py).

  device ID    the board's factory MAC as 12 lowercase hex characters: --device-id, or asked from the board with --from-board.
               With --from-board the ID comes from the network, not from the USB board: with two boards on the LAN, check that the
               host is the one on --port. (The firmware refuses a record made for another board: identity "other_board".)
  master secret  a text of at least 32 characters in a FILE that lives outside the repository: --secret-file, or the environment
               variable LEDMATRIX_MASTER_SECRET_FILE (a path). Never a command-line argument: it would show in the process list.
               Surrounding space, tab, CR and LF are removed, the rest is used as the raw bytes of the HMAC key.
  device key   HMAC-SHA256(key = master secret, message = device ID text), 32 bytes

Examples (placeholders):
  python3 -I tools/provision.py --device-id aabbccddeeff --secret-file ~/secrets/ledmatrix-master --out ~/secrets/ident.bin
  python3 -I tools/provision.py --from-board ledmatrix.local --secret-file ~/secrets/ledmatrix-master --out ~/secrets/ident.bin \\
      --flash --port /dev/cu.usbmodem1234 --input-dir <build dir of the sketch>
  python3 -I tools/provision.py --check ~/secrets/ident.bin

What it prints: the line `device=<id> crc32=<hex>` (the CRC-32 of the whole image), and with --flash one more line about the image
file. Never the secret, never a key.
The image file holds the device key, so it is created with mode 0600 and must be written outside the repository. After a successful
--flash the file is deleted (--keep keeps it); after a failed one it is kept, so the flash can be tried again.
The board is flashed only with arduino-cli (never esptool by hand): --flash runs `arduino-cli upload` with the image added to the
board's own extra upload files, so the bootloader, partition table and firmware are written as usual.
Python 3 standard library only."""
import argparse
import hashlib
import hmac
import json
import os
import re
import struct
import subprocess
import sys
import urllib.request
import zlib

SECTOR = 4096
IMAGE_SIZE = 2 * SECTOR
MAGIC = 0x544E4449  # the bytes "IDNT"
VERSION = 1
PAYLOAD_LEN = 48
HEADER_LEN = 12
RECORD_LEN = HEADER_LEN + PAYLOAD_LEN
FLAG_BURNED = 1
MIN_SECRET_CHARS = 32
SECRET_FILE_ENV = "LEDMATRIX_MASTER_SECRET_FILE"
FLASH_OFFSET = "0x7FE000"
FQBN = "esp32:esp32:adafruit_matrixportal_esp32s3"
REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
SKETCH_DIR = os.path.join(REPO, "arduino", "ledmatrix")
BOARD_TIMEOUT_S = 5


class ProvisionError(Exception):
    """A problem to tell the owner about. The text never holds a secret or a key."""


def valid_device_id(s):
    """Exactly 12 lowercase hexadecimal characters: no upper case, no colons, no other length."""
    return isinstance(s, str) and re.fullmatch(r"[0-9a-f]{12}", s) is not None


def read_secret(path):
    """The master secret from a file, as bytes: surrounding space, tab, CR and LF removed, at least 32 characters left."""
    try:
        with open(path, "rb") as f:
            raw = f.read(1 << 16)
    except OSError as e:
        raise ProvisionError("cannot read the secret file (%s)" % (e.strerror or type(e).__name__))
    if len(raw) >= 1 << 16:
        raise ProvisionError("the secret file is too large to be a secret")
    secret = raw.strip(b" \t\r\n")
    try:
        text = secret.decode("utf-8")
    except UnicodeDecodeError:
        raise ProvisionError("the secret file is not text (UTF-8)")  # the message must not quote the bytes
    if len(text) < MIN_SECRET_CHARS:
        raise ProvisionError("the master secret is shorter than %d characters" % MIN_SECRET_CHARS)
    return secret


def derive_key(secret, device_id):
    """The 32-byte device key: HMAC-SHA256(key = master secret bytes, message = device ID text)."""
    if not valid_device_id(device_id):
        raise ProvisionError("the device ID must be 12 lowercase hexadecimal characters")
    return hmac.new(secret, device_id.encode("ascii"), hashlib.sha256).digest()


def build_sector(device_id, key, flags=0):
    """One 4 KB sector: the 60-byte record, then 0xFF. Layout (little-endian): magic u32, version u16, payload length u16,
    CRC-32 of the payload u32, then the payload: device ID (12), flags (1), reserved zero (3), key (32)."""
    if not valid_device_id(device_id) or len(key) != 32:
        raise ProvisionError("not a valid device ID or key")
    payload = device_id.encode("ascii") + bytes([flags, 0, 0, 0]) + key
    assert len(payload) == PAYLOAD_LEN
    record = struct.pack("<IHHI", MAGIC, VERSION, PAYLOAD_LEN, zlib.crc32(payload) & 0xFFFFFFFF) + payload
    return record + b"\xff" * (SECTOR - len(record))


def build_image(device_id, key, flags=0):
    """The 8192-byte `ident` image: two identical copies of the record."""
    sector = build_sector(device_id, key, flags)
    return sector + sector


def image_crc(image):
    return zlib.crc32(image) & 0xFFFFFFFF


def parse_sector(sector):
    """Checks one sector and returns its device ID; raises ProvisionError (without quoting any bytes) when it is not valid."""
    if len(sector) != SECTOR:
        raise ProvisionError("a sector is not 4096 bytes")
    magic, version, plen, crc = struct.unpack("<IHHI", sector[:HEADER_LEN])
    if magic != MAGIC:
        raise ProvisionError("bad magic")
    if version != VERSION:
        raise ProvisionError("unknown record version")
    if plen != PAYLOAD_LEN:
        raise ProvisionError("bad payload length")
    payload = sector[HEADER_LEN:RECORD_LEN]
    if zlib.crc32(payload) & 0xFFFFFFFF != crc:
        raise ProvisionError("bad checksum")
    raw_id = payload[:12]
    flags = payload[12]
    reserved = payload[13:16]
    key = payload[16:48]
    try:
        device_id = raw_id.decode("ascii")
    except UnicodeDecodeError:
        raise ProvisionError("the device ID is not valid")
    if not valid_device_id(device_id):
        raise ProvisionError("the device ID is not valid")
    if reserved != b"\x00\x00\x00":
        raise ProvisionError("the reserved bytes are not zero")
    if flags & ~FLAG_BURNED:
        raise ProvisionError("unknown flag bits")
    if flags & FLAG_BURNED and key != b"\x00" * 32:
        raise ProvisionError("the key is marked as burned but is not zero")
    if sector[RECORD_LEN:] != b"\xff" * (SECTOR - RECORD_LEN):
        raise ProvisionError("the rest of the sector is not 0xFF")
    return device_id


def check_image(image):
    """Validates a whole image (both copies valid and identical) and returns its device ID."""
    if len(image) != IMAGE_SIZE:
        raise ProvisionError("an identity image is exactly %d bytes" % IMAGE_SIZE)
    ids = []
    for i in (0, 1):
        try:
            ids.append(parse_sector(image[i * SECTOR:(i + 1) * SECTOR]))
        except ProvisionError as e:
            raise ProvisionError("sector %d: %s" % (i, e))
    if image[:SECTOR] != image[SECTOR:]:
        raise ProvisionError("the two copies are not identical")
    return ids[0]


def inside_repo(path):
    real = os.path.realpath(path)
    repo = os.path.realpath(REPO)
    return os.path.commonpath([real, repo]) == repo


def write_private(path, data):
    """Creates (or replaces the content of) a file with mode 0600, whatever the umask, and never through a symbolic link."""
    flags = os.O_WRONLY | os.O_CREAT | os.O_TRUNC | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags, 0o600)
    except OSError as e:
        raise ProvisionError("cannot write the output file (%s)" % (e.strerror or type(e).__name__))
    try:
        os.fchmod(fd, 0o600)  # a file that already existed keeps its old mode otherwise
        with os.fdopen(fd, "wb") as f:
            fd = -1
            f.write(data)
    finally:
        if fd >= 0:
            os.close(fd)


def device_id_from_board(host):
    """Asks the board (GET http://<host>/status, 5 s) for its device ID."""
    if not re.fullmatch(r"[A-Za-z0-9.\-]+(:[0-9]{1,5})?|\[[0-9A-Fa-f:.]+\](:[0-9]{1,5})?", host or ""):
        raise ProvisionError("the board address must be a host name or an IP address")
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))  # a board on the LAN is never reached through a proxy
    try:
        with opener.open("http://%s/status" % host, timeout=BOARD_TIMEOUT_S) as resp:
            body = resp.read(1 << 14)
        status = json.loads(body.decode("utf-8"))
        device_id = status.get("device") if isinstance(status, dict) else None
    except (OSError, ValueError) as e:  # URLError, timeouts and HTTP errors are OSErrors; bad JSON and bad text are ValueErrors
        if hasattr(e, "close"):
            e.close()  # an HTTPError holds the connection
        raise ProvisionError("could not read the device ID from the board (is it on, and is the firmware up to date?)")
    if not valid_device_id(device_id):
        raise ProvisionError("the board did not report a valid device ID")
    return device_id


def current_extra_flags():
    """The board's own upload.extra_flags (the TinyUF2 file at 0x410000), read from arduino-cli."""
    cmd = ["arduino-cli", "compile", "--fqbn", FQBN, "--show-properties", SKETCH_DIR]
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        raise ProvisionError("arduino-cli could not show the board's upload properties")
    for line in out.splitlines():
        if line.startswith("upload.extra_flags="):
            return line[len("upload.extra_flags="):]
    raise ProvisionError("the board has no upload.extra_flags property")


def upload_command(port, input_dir, image_path, extra_flags):
    """arduino-cli upload with the identity image added to the extra files: `<offset> "<file>"` after what is already there.
    Every other part of the upload (bootloader, partition table, firmware, TinyUF2) is the normal one."""
    if '"' in image_path:
        raise ProvisionError("the image path must not contain a double quote")
    prop = "upload.extra_flags=%s %s \"%s\"" % (extra_flags, FLASH_OFFSET, image_path)
    return ["arduino-cli", "upload", "--fqbn", FQBN, "--port", port, "--input-dir", input_dir, "--upload-property", prop, SKETCH_DIR]


def flash(port, input_dir, image_path):
    cmd = upload_command(port, input_dir, os.path.abspath(image_path), current_extra_flags())
    try:
        rc = subprocess.run(cmd).returncode
    except OSError:
        raise ProvisionError("arduino-cli could not be started")
    if rc != 0:
        raise ProvisionError("arduino-cli upload failed (exit code %d)" % rc)


def run(args):
    if args.check:
        try:
            with open(args.check, "rb") as f:
                image = f.read(IMAGE_SIZE + 1)
        except OSError as e:
            raise ProvisionError("cannot read the image (%s)" % (e.strerror or type(e).__name__))
        device_id = check_image(image)
        print("device=%s crc32=%08x" % (device_id, image_crc(image)))
        return 0

    if bool(args.device_id) == bool(args.from_board):
        raise ProvisionError("give exactly one of --device-id and --from-board")
    if not args.out:
        raise ProvisionError("--out is needed (where to write the 8 KB image)")
    if args.flash and not (args.port and args.input_dir):
        raise ProvisionError("--flash needs --port and --input-dir")
    secret_path = args.secret_file or os.environ.get(SECRET_FILE_ENV)
    if not secret_path:
        raise ProvisionError("give the master secret file with --secret-file or the %s variable (a path, not the secret itself)" % SECRET_FILE_ENV)
    if inside_repo(args.out):
        raise ProvisionError("the image holds the device key: write it outside the repository")
    if inside_repo(secret_path):
        raise ProvisionError("the master secret must be kept outside the repository")

    device_id = args.device_id if args.device_id else device_id_from_board(args.from_board)
    if not valid_device_id(device_id):
        raise ProvisionError("the device ID must be 12 lowercase hexadecimal characters")
    secret = read_secret(secret_path)
    key = derive_key(secret, device_id)
    image = build_image(device_id, key)
    write_private(args.out, image)
    print("device=%s crc32=%08x" % (device_id, image_crc(image)))
    sys.stdout.flush()
    if args.flash:
        try:
            flash(args.port, args.input_dir, args.out)
        except ProvisionError:
            print("the image file was kept (it holds the device key): %s" % args.out, file=sys.stderr)
            raise
        if args.keep:
            print("kept the image file (it holds the device key): delete it when you are done: %s" % args.out)
        else:
            try:
                os.remove(args.out)
            except OSError as e:
                raise ProvisionError("flashed, but the image file could not be deleted (%s): delete it by hand: %s"
                                     % (e.strerror or type(e).__name__, args.out))
            print("deleted the image file (it held the device key); use --keep to keep it")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog="provision.py", description=__doc__, allow_abbrev=False,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--device-id", metavar="ID", help="the board's device ID: 12 lowercase hex characters")
    ap.add_argument("--from-board", metavar="HOST", help="ask the board for its device ID (GET http://HOST/status)")
    ap.add_argument("--secret-file", metavar="PATH", help="file with the master secret (or set %s)" % SECRET_FILE_ENV)
    ap.add_argument("--out", metavar="FILE", help="where to write the 8192-byte identity image (mode 0600)")
    ap.add_argument("--flash", action="store_true", help="also write the image to the board with arduino-cli upload")
    ap.add_argument("--port", metavar="PORT", help="serial port of the board, for --flash")
    ap.add_argument("--input-dir", metavar="DIR", help="build directory with the compiled firmware, for --flash")
    ap.add_argument("--keep", action="store_true", help="with --flash: keep the image file after a successful flash (it is deleted by default)")
    ap.add_argument("--check", metavar="FILE", help="validate an identity image and print its device ID")
    args, extra = ap.parse_known_args(argv)
    if extra:
        # argparse would print the unknown arguments, values included. Someone who typed the secret as an option must not see it
        # echoed (into a terminal log, a CI log): say that something was not understood, and that no option takes the secret.
        ap.error("unrecognized arguments (not shown: one of them may be a secret). The master secret is never a command-line "
                 "argument: give the path of a file with --secret-file")
    try:
        return run(args)
    except ProvisionError as e:
        print("error: %s" % e, file=sys.stderr)
        return 2
    except Exception as e:  # only the kind of failure is shown: a message could quote something it should not
        print("error: unexpected failure (%s)" % type(e).__name__, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
