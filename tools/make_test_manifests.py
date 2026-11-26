#!/usr/bin/env python3
r"""Make the negative-test manifests for the signed-manifest bench plan (wayfinder/research/signed-manifest-design.md, section 6,
step 3; shared/manifest-signing.md section 5.2). TEST ONLY: never part of a release.

    python3 -I tools/make_test_manifests.py --from DIR --out OUTDIR [--keys-header FILE]

DIR holds a GENUINE signed release: manifest.json signed by a key pinned in --keys-header (default
arduino/ledmatrix/update_keys.h next to this tool) and the image ledmatrix-<version>.bin it describes, for example the folder the
build workflow wrote into service/firmware, or one signed by hand with tools/sign_manifest.py. The tool refuses a folder whose
manifest does not verify.

It writes one sub-folder per case into OUTDIR (which must not exist or must be empty), each holding a manifest.json and the image
next to it, so that any one of them can be copied into service/firmware/ to build a test service image:

    genuine           the manifest as it is: the control, the board offers it when it is newer
    unsigned          the five v1 fields only                                    -> refused "unsigned"
    outsider-key      valid signature by a throwaway key that is NOT pinned, under its own key id   -> "unknown_key"
    flipped-sig-bit   the genuine manifest with one bit of sig flipped                              -> "bad_signature"
    notes-edited      the genuine manifest with its notes changed                                   -> "bad_signature"
    size-plus-one     size + 1 (the image gets one extra byte and the SHA-256 is recomputed,
                      because the service checks the image against the manifest)                    -> "bad_signature"
    version-raised    the version raised by one patch (the image copied under the new name)         -> "bad_signature"
    wrong-length-sig  sig with 126 characters instead of 128                                        -> "malformed"
    only-keyid        keyid without sig                                                             -> "malformed"

The last two are malformed on purpose, and the service of this repository REFUSES to publish them (it checks the shapes, so the
board would see "no firmware is published", not the signature sentence): to put them in front of a real board use a stand-in
HTTPS server, or rely on the host tests. Every other case is served by the service as it is.

The outsider key is made at run time with the openssl command line in a private temporary folder and is gone when the tool ends:
nothing here is a key anyone can reuse, and nothing is ever committed. It prints no key and no signature, only the case table.
Python 3.8 or newer, standard library plus the openssl command line."""
import argparse
import hashlib
import importlib.util
import os
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))

_spec = importlib.util.spec_from_file_location("update_signing", os.path.join(HERE, "update_signing.py"))
us = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(us)

DEFAULT_HEADER = os.path.join(HERE, "..", "arduino", "ledmatrix", "update_keys.h")


class Case:
    """name, the manifest dict as it is written, the files {name: bytes} next to it, what the board must say, whether the service
    serves it, and a note for the person running the bench."""

    def __init__(self, name, manifest, files, expect, served, note):
        self.name, self.manifest, self.files, self.expect, self.served, self.note = name, manifest, files, expect, served, note


def raised_version(version):
    parts = [int(p) for p in version.split(".")]
    for i in (2, 1, 0):
        if parts[i] < 9999:
            parts[i] += 1
            break
    return ".".join(str(p) for p in parts)


def flip_hex_bit(sig_hex, byte_index=40):
    raw = bytearray(bytes.fromhex(sig_hex))
    raw[byte_index] ^= 0x01
    return bytes(raw).hex()


def build_cases(genuine, image, outsider_keyid, outsider_sig):
    """The cases, from the genuine signed manifest and its image. `outsider_*` is a valid signature of the genuine values by a key
    that is not pinned."""
    base = {k: genuine.get(k) if genuine.get(k) is not None else "" for k in us.FIELDS_V1}
    keyid, sig = genuine["keyid"], genuine["sig"]
    file = genuine["file"]

    def signed(**changes):
        m = dict(base)
        m.update(keyid=keyid, sig=sig)
        m.update(changes)
        return m

    cases = [Case("genuine", signed(), {file: image}, us.OK, True, "offered when its version is newer than the running one")]
    cases.append(Case("unsigned", dict(base), {file: image}, us.UNSIGNED, True, "no notice; sentence: the service offers an update that is not signed"))
    cases.append(Case("outsider-key", signed(keyid=outsider_keyid, sig=outsider_sig), {file: image}, us.UNKNOWN_KEY, True,
                      "a valid signature, but by a key this board does not know"))
    cases.append(Case("flipped-sig-bit", signed(sig=flip_hex_bit(sig)), {file: image}, us.BAD_SIGNATURE, True, "one bit of sig changed"))
    notes = base["notes"]
    edited = (notes[:190].rstrip() + " (edited)") if notes else "edited"
    cases.append(Case("notes-edited", signed(notes=edited), {file: image}, us.BAD_SIGNATURE, True, "a valid signature over other notes"))
    bigger = image + b"\x00"
    cases.append(Case("size-plus-one", signed(size=len(bigger), sha256=hashlib.sha256(bigger).hexdigest()), {file: bigger},
                      us.BAD_SIGNATURE, True, "a valid signature over other size and SHA-256 (the image has one more byte)"))
    newver = raised_version(genuine["version"])
    newfile = f"ledmatrix-{newver}.bin"
    cases.append(Case("version-raised", signed(version=newver, file=newfile), {newfile: image}, us.BAD_SIGNATURE, True,
                      f"a valid signature over version {genuine['version']} shown as {newver}"))
    cases.append(Case("wrong-length-sig", signed(sig=sig[:-2]), {file: image}, us.MALFORMED, False,
                      "sig is 126 characters; the service refuses to publish it (404): use a stand-in server"))
    lone = signed()
    del lone["sig"]
    cases.append(Case("only-keyid", lone, {file: image}, us.MALFORMED, False,
                      "keyid without sig; the service refuses to publish it (404): use a stand-in server"))
    return cases


def prepare_out(path):
    path = os.path.abspath(path)
    if os.path.lexists(path):
        if not os.path.isdir(path) or os.path.islink(path):
            raise us.Refusal(f"{path} exists and is not a plain folder")
        if os.listdir(path):
            raise us.Refusal(f"{path} is not empty: give a new folder (this tool never overwrites)")
    else:
        os.makedirs(path)
    return path


def run(src, out, header, openssl=None):
    """Make the case folders. Returns (the Case objects written, the outsider key id). Raises Refusal."""
    pinned = us.parse_keys_header(header)
    manifest_path = os.path.join(src, "manifest.json")
    genuine = us.load_manifest(manifest_path)
    why = us.manifest_problem(genuine, release=True)
    if why:
        raise us.Refusal(f"the manifest in {src} is not valid: {why}")
    result, _ = us.classify(genuine, pinned.keys)
    if result != us.OK:
        raise us.Refusal(f"the manifest in {src} is not a genuine signed release (it classifies as {result}): sign it first with "
                         "tools/sign_manifest.py, with a key pinned in the keys header")
    image, problem = us.read_image(manifest_path, genuine)
    if problem or image is None:
        raise us.Refusal(problem or f"the image {genuine['file']} is not next to the manifest")
    if os.path.realpath(out) == os.path.realpath(src):
        raise us.Refusal("the output folder must not be the source folder")
    out = prepare_out(out)

    with us.private_workdir() as work:                  # the outsider's key lives here, and nowhere else
        pem = us.generate_key_pem(openssl)
        keyfile = os.path.join(work, "outsider.pem")
        us.write_private(keyfile, pem.encode("ascii"))
        pub = us.public_point_from_key_file(keyfile, openssl)
        if pub in pinned.keys:
            raise us.Refusal("the throwaway key equals a pinned key (this cannot happen with a working random source)")
        raw = us.sign_with_key_file(keyfile, us.canonical_bytes(genuine), openssl)
        outsider_id, outsider_sig = us.key_id(pub), raw.hex()
    cases = build_cases(genuine, image, outsider_id, outsider_sig)
    for c in cases:
        folder = os.path.join(out, c.name)
        os.makedirs(folder)
        for name, data in c.files.items():
            us.write_atomic(os.path.join(folder, name), data)
        us.write_atomic(os.path.join(folder, "manifest.json"), us.manifest_text(c.manifest))
        got, _ = us.classify(c.manifest, pinned.keys)
        if got != c.expect:                                # the tool checks its own work against the board's rules
            raise us.Refusal(f"internal error: the case {c.name} classifies as {got}, expected {c.expect}")
    return cases, outsider_id


def main(argv=None):
    p = argparse.ArgumentParser(prog="make_test_manifests.py", allow_abbrev=False,
                                description="Make the negative-test manifests of the signed-manifest bench plan (test only).")
    p.add_argument("--from", dest="src", required=True, metavar="DIR", help="a folder with a genuine signed manifest.json and its image")
    p.add_argument("--out", required=True, metavar="OUTDIR", help="a new (or empty) folder: one sub-folder per case is written into it")
    p.add_argument("--keys-header", default=DEFAULT_HEADER, metavar="FILE", help="the pinned keys (default: arduino/ledmatrix/update_keys.h)")
    args = p.parse_args(argv)
    try:
        cases, outsider_id = run(args.src, args.out, args.keys_header)
    except us.Refusal as e:
        print(f"make_test_manifests: {e}", file=sys.stderr)
        return 1
    except OSError as e:
        print(f"make_test_manifests: {e.strerror or e}", file=sys.stderr)
        return 1
    print(f"Wrote {len(cases)} cases into {os.path.abspath(args.out)} (the throwaway outsider key {outsider_id} is gone).")
    print(f"{'case':18} {'the board must say':24} {'service serves it':18} note")
    for c in cases:
        board = "offers it" if c.expect == us.OK else f"refused: {c.expect}"
        print(f"{c.name:18} {board:24} {'yes' if c.served else 'NO (404)':18} {c.note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
