#!/usr/bin/env python3
r"""Write the firmware manifest and the published copy of an image (shared/firmware-update.md).

    python3 -I tools/make_manifest.py <image.bin> --version <x.y.z> [--notes "<text>"] --out <dir> [--force]
    python3 -I tools/make_manifest.py <image.bin> --version-from arduino/ledmatrix/version.h --out <dir>

Writes <dir>/manifest.json (compact JSON, keys in this order: version, file, size, sha256, notes) and copies the image to
<dir>/ledmatrix-<version>.bin. Prints one line: version=<v> size=<n> sha256=<hex>. Python 3, standard library only; it reads
nothing but its arguments, the image, and (with --version-from) the one version.h it is pointed at.

What it checks before it writes anything (a failure prints one plain sentence on stderr and exits 1, nothing is written):
- the version is MAJOR.MINOR.PATCH, three numbers of 1 to 4 digits each, no leading zeros (0.10.0, not 0.010.0);
- the image is a plausible ESP32-S3 app image: first byte 0xE9, chip id 0x0009 at header offset 12, more than 100 KB and at
  most 2,097,152 bytes (one app slot), and its own structure is intact (segments add up to the file length, checksum byte,
  and the SHA-256 the build appends when the header says so), which also rejects a truncated file or a merged flash image;
- the image carries exactly one version marker (the bytes \x01LEDMATRIX-FW-VERSION\x02 <version> \x03 that the firmware embeds,
  arduino/ledmatrix/otalogic.h and ota.cpp) and it says the version being published: a release is self-consistent, because the
  board reads the marker back after writing the image and refuses one that disagrees with the manifest. --force does not skip
  this check;
- the notes are at most 200 characters (and 200 UTF-8 bytes, so any reader's count agrees): newlines and runs of blanks fold
  to one space, a longer text is cut at a word boundary and ends in "...";
- a release is immutable: if <dir> already holds ledmatrix-<version>.bin with other bytes, or a manifest for the same version
  with another hash, it refuses unless --force. The same image again is fine and changes nothing.

The image is copied first and manifest.json is replaced last, so a run that stops half way never advertises a missing file.
Pass the notes as --notes="..." when the text can start with a dash.

The manifest is the unsigned form (version 1, five fields): in the build workflow tools/sign_manifest.py runs next and appends
keyid and sig (shared/manifest-signing.md). The notes are therefore valid UTF-8 text without a control character (they are signed
as text), and running this tool again on a folder whose manifest is already signed writes the unsigned form again: sign it again."""
import argparse
import hashlib
import json
import os
import re
import struct
import sys
import tempfile

sys.dont_write_bytecode = True

MIN_SIZE_EXCLUSIVE = 100 * 1024     # "above 100 KB" in the contract
MAX_SIZE = 2 * 1024 * 1024          # one app slot (2,097,152)
NOTES_MAX = 200
ELLIPSIS = "..."
CHIP_ID_ESP32_S3 = 0x0009
IMAGE_MAGIC = 0xE9
HEADER_LEN = 24
# The version marker the firmware embeds (otalogic.h VersionTagScanner): prefix, 1 to VERSION_TEXT_MAX characters of digits and dots,
# end byte. The firmware also holds the bare prefix as a constant in the code that looks for it; that is not followed by a version
# and is not a marker.
TAG_PREFIX = b"\x01LEDMATRIX-FW-VERSION\x02"
TAG_END = b"\x03"
VERSION_TEXT_MAX = 14               # "9999.9999.9999"
TAG_RE = re.compile(re.escape(TAG_PREFIX) + rb"([0-9.]{1,%d})" % VERSION_TEXT_MAX + re.escape(TAG_END))
VERSION_RE = re.compile(r"(0|[1-9][0-9]{0,3})\.(0|[1-9][0-9]{0,3})\.(0|[1-9][0-9]{0,3})")
DEFINE_RE = re.compile(r'^[ \t]*#[ \t]*define[ \t]+FIRMWARE_VERSION[ \t]+"([^"\n]*)"[ \t]*(?://.*)?$', re.M)


class Refusal(Exception):
    """A plain-words reason to stop; main() prints it and exits 1."""


def check_version(text):
    if not isinstance(text, str) or not VERSION_RE.fullmatch(text):
        raise Refusal(f"the version {text!r} is not MAJOR.MINOR.PATCH (three numbers of 1 to 4 digits, no leading zeros, "
                      "no pre-release tags)")
    return text


def read_version_h(path):
    try:
        with open(path, "r", encoding="utf-8") as f:
            text = f.read()
    except (OSError, UnicodeDecodeError) as e:
        raise Refusal(f"cannot read {path}: {e}")
    found = DEFINE_RE.findall(text)
    if len(found) != 1:
        raise Refusal(f"{path} must hold exactly one line like #define FIRMWARE_VERSION \"0.10.0\" (found {len(found)})")
    return check_version(found[0])


def fold_notes(text):
    """Fold every control character and whitespace run to one space, then cut to NOTES_MAX at a word boundary."""
    text = re.sub(r"[\x00-\x1f\x7f]", " ", text or "")
    text = " ".join(text.split())

    def fits(s):
        return len(s) <= NOTES_MAX and len(s.encode("utf-8")) <= NOTES_MAX

    if fits(text):
        return text
    # The longest prefix that leaves room for the ellipsis, then back to the last word boundary.
    keep = len(text)
    while keep > 0 and not fits(text[:keep] + ELLIPSIS):
        keep -= 1
    head = text[:keep]
    if keep < len(text) and text[keep] != " ":      # the cut landed inside a word: drop the broken word if there is another
        space = head.rfind(" ")
        if space > 0:
            head = head[:space]
    return head.rstrip() + ELLIPSIS


def check_image(data):
    """Raise Refusal unless data is an intact ESP32-S3 application image of an allowed size. Returns nothing."""
    n = len(data)
    if n > MAX_SIZE:
        raise Refusal(f"the image is larger than one app slot ({MAX_SIZE} bytes): is it the app .bin and not a merged flash image?")
    if n <= MIN_SIZE_EXCLUSIVE:
        raise Refusal(f"the image is only {n} bytes; a firmware image is more than {MIN_SIZE_EXCLUSIVE} bytes")
    if data[0] != IMAGE_MAGIC:
        raise Refusal(f"the file does not start with the ESP image byte 0xE9 (it starts with 0x{data[0]:02X}): "
                      "is it the app .bin and not another file?")
    chip_id = struct.unpack_from("<H", data, 12)[0]
    if chip_id != CHIP_ID_ESP32_S3:
        raise Refusal(f"the image is for chip id 0x{chip_id:04X}, not the ESP32-S3 (0x{CHIP_ID_ESP32_S3:04X})")
    segments, hash_appended = data[1], data[23]
    if not 1 <= segments <= 16:
        raise Refusal(f"the image header claims {segments} segments (1 to 16 are possible)")
    pos, checksum = HEADER_LEN, 0xEF
    for i in range(segments):
        if pos + 8 > n:
            raise Refusal(f"the image ends inside the header of segment {i + 1}: truncated?")
        length = struct.unpack_from("<I", data, pos + 4)[0]
        pos += 8
        if length > n - pos:
            raise Refusal(f"segment {i + 1} runs past the end of the file: truncated?")
        for b in data[pos:pos + length]:
            checksum ^= b
        pos += length
    pos += 15 - (pos % 16)                          # padding, the last byte of which is the checksum
    if pos >= n:
        raise Refusal("the image ends before its checksum byte: truncated?")
    if data[pos] != checksum:
        raise Refusal("the image's own checksum does not match its contents: damaged?")
    pos += 1
    if hash_appended == 1:
        if n - pos != 32:
            raise Refusal(f"the image should end with a 32-byte SHA-256 after its checksum but {n - pos} bytes follow it "
                          "(a merged flash image, a truncated file or an extra tail)")
        if hashlib.sha256(data[:pos]).digest() != data[pos:]:
            raise Refusal("the SHA-256 the build appended to the image does not match its contents: damaged?")
    elif n != pos:
        raise Refusal(f"{n - pos} bytes follow the end of the image (a merged flash image, or an extra tail)")


def check_version_tag(data, version):
    """Raise Refusal unless the image holds exactly one version marker and it says `version`."""
    tags = [m.group(1).decode("ascii") for m in TAG_RE.finditer(data)]
    if not tags:
        raise Refusal("the image carries no firmware version marker (the firmware embeds one, arduino/ledmatrix/ota.cpp): is it a "
                      "build of this repository's firmware?")
    if len(tags) > 1:
        raise Refusal(f"the image carries {len(tags)} firmware version markers ({', '.join(tags)}); a release image has exactly one")
    if tags[0] != version:
        raise Refusal(f"the image says it is version {tags[0]} but it would be published as {version}, and the board refuses an "
                      "image whose version differs from the manifest's: rebuild after changing version.h, or give the right version")


def read_existing_manifest(path):
    try:
        with open(path, "rb") as f:
            m = json.loads(f.read().decode("utf-8"))
        return m if isinstance(m, dict) else None
    except (OSError, ValueError):
        return None


def write_atomic(path, data):
    d = os.path.dirname(path)
    fd, tmp = tempfile.mkstemp(prefix=".tmp-", dir=d)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        os.chmod(tmp, 0o644)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def make_manifest(image_path, version, notes, out_dir, force=False):
    """Validate, then publish. Returns the manifest dict. Raises Refusal."""
    check_version(version)
    try:
        with open(image_path, "rb") as f:
            data = f.read(MAX_SIZE + 1)               # one byte past the limit is enough to know it is too big
    except OSError as e:
        raise Refusal(f"cannot read the image {image_path}: {e}")
    check_image(data)
    check_version_tag(data, version)               # never skipped, not even by force
    try:
        (notes or "").encode("utf-8")   # the notes are signed as UTF-8 text: a lone surrogate (a command line that was not UTF-8) is no text
    except UnicodeEncodeError:
        raise Refusal("the notes are not valid UTF-8 text (was the command line in another encoding?)")
    notes = fold_notes(notes)

    name = f"ledmatrix-{version}.bin"
    manifest = {"version": version, "file": name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest(), "notes": notes}

    if os.path.exists(out_dir) and not os.path.isdir(out_dir):
        raise Refusal(f"{out_dir} exists and is not a folder")
    target = os.path.join(out_dir, name)
    if not force:
        if os.path.exists(target):
            try:
                with open(target, "rb") as f:
                    same = f.read() == data
            except OSError as e:
                raise Refusal(f"cannot read the published {target}: {e}")
            if not same:
                raise Refusal(f"{target} already holds a different image: a release is immutable, so give this build a "
                              "new version (or pass --force to replace it)")
        old = read_existing_manifest(os.path.join(out_dir, "manifest.json"))
        if old and old.get("version") == version and old.get("sha256") != manifest["sha256"]:
            raise Refusal(f"manifest.json already publishes version {version} with another hash: a release is immutable, "
                          "so give this build a new version (or pass --force to replace it)")

    os.makedirs(out_dir, exist_ok=True)
    write_atomic(target, data)
    text = json.dumps(manifest, separators=(",", ":"), ensure_ascii=False) + "\n"
    write_atomic(os.path.join(out_dir, "manifest.json"), text.encode("utf-8"))
    return manifest


def main(argv=None):
    p = argparse.ArgumentParser(prog="make_manifest.py", description="Write manifest.json and ledmatrix-<version>.bin "
                                "(shared/firmware-update.md).", allow_abbrev=False)
    p.add_argument("image", help="the app image, the sketch's .ino.bin (not the merged, bootloader or partitions file)")
    v = p.add_mutually_exclusive_group(required=True)
    v.add_argument("--version", help="MAJOR.MINOR.PATCH, for example 0.10.1")
    v.add_argument("--version-from", metavar="VERSION_H", help="read it from #define FIRMWARE_VERSION in this version.h")
    p.add_argument("--notes", default="", help="release notes, at most 200 characters (longer is cut at a word boundary); "
                   "use --notes=\"...\" if the text can start with a dash")
    p.add_argument("--out", required=True, metavar="DIR", help="the folder to publish into (created if missing)")
    p.add_argument("--force", action="store_true", help="replace a different image already published under this version (it skips "
                   "that one rule only, never the image or version-marker checks)")
    args = p.parse_args(argv)
    try:
        version = check_version(args.version) if args.version is not None else read_version_h(args.version_from)
        m = make_manifest(args.image, version, args.notes, args.out, args.force)
    except Refusal as e:
        print(f"make_manifest: {e}", file=sys.stderr)
        return 1
    except OSError as e:
        print(f"make_manifest: {e}", file=sys.stderr)
        return 1
    print(f"version={m['version']} size={m['size']} sha256={m['sha256']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
