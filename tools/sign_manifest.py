#!/usr/bin/env python3
r"""Sign the firmware manifest, or check a signed one (shared/manifest-signing.md, sections 2, 3 and 7).

    python3 -I tools/sign_manifest.py MANIFEST --keys-header FILE [--key-file PATH] [--allow-unsigned]
    python3 -I tools/sign_manifest.py --check MANIFEST --keys-header FILE [--allow-unsigned]

SIGN (the build workflow's step, or by hand). The key is the PEM of an EC P-256 private key, or the same PEM as one line of base64
(the form of the GitHub secret). It is read from the environment variable UPDATE_SIGNING_KEY or from --key-file, never from an
argument; giving it both ways is refused. It is written to a file mode 0600 in a fresh folder mode 0700 (under $RUNNER_TEMP when
that is set), used by `openssl dgst -sha256 -sign`, and removed in a `finally`; it is not passed to any child process, and this tool
never prints a key, a signature or the canonical text. It prints one line: `signed version=0.11.0 keyid=f864ea3b`.

Before signing it refuses (one plain sentence on stderr, exit 1, nothing written) unless:
- the manifest follows every rule of the contract (version MAJOR.MINOR.PATCH without leading zeros, file ledmatrix-<version>.bin,
  size above 100 KB and at most one app slot, 64 lowercase hex SHA-256, notes of at most 200 characters without a control
  character, no field outside the contract) and carries no signature yet;
- the image ledmatrix-<version>.bin next to the manifest has exactly the manifest's size and SHA-256;
- the image carries the embedded signing marker \x01LEDMATRIX-FW-SIGNING\x02required\x03 exactly once (a build with enforcement
  off says `off`, and no release may be that);
- the key is an EC key on P-256 whose public point is one of the keys pinned in --keys-header (arduino/ledmatrix/update_keys.h,
  read strictly): a secret that does not match what the firmware pins would give a release no display could verify.
After signing it verifies the signature with the PINNED public key twice, with this tool's own pure-Python P-256 verifier and with
the openssl command line, checks the file it is about to write the way --check does, and writes manifest.json atomically (compact
JSON, the five v1 keys, then keyid and sig). Running it on a manifest that is already signed checks it and changes nothing.

--check needs no key: it verifies a signed manifest against the pinned keys (and the image next to it, when there is one).

THE TRANSITION RULE (--allow-unsigned, used by the workflow while REQUIRE_SIGNING is "0"): when no key is given the manifest is
validated, a loud warning is printed and it is left unsigned, exit 0; with --check an unsigned manifest is accepted and said to be
unsigned. A manifest that is signed must verify either way, and without --allow-unsigned both an empty key and an unsigned
manifest fail. Python 3.8 or newer, standard library plus the openssl command line."""
import argparse
import importlib.util
import json
import os
import stat
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))

_spec = importlib.util.spec_from_file_location("update_signing", os.path.join(HERE, "update_signing.py"))
us = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(us)

KEY_ENV = "UPDATE_SIGNING_KEY"
UNSIGNED_WARNING = ("the manifest is NOT signed: UPDATE_SIGNING_KEY is not set; displays that enforce signatures will refuse this "
                    "release")


def ids_text(pinned):
    return ", ".join(pinned.ids)


def result_problem(result, keyid, pinned):
    """The plain sentence for a classification other than ok."""
    if result == us.UNSIGNED:
        return "the manifest is not signed (it has no keyid and no sig)"
    if result == us.MALFORMED:
        return ("the signature fields are malformed: keyid and sig must both be present, keyid 8 and sig 128 lowercase hex "
                "characters")
    if result == us.UNKNOWN_KEY:
        return f"the manifest is signed with key {keyid}, which is not one of the pinned keys ({ids_text(pinned)})"
    if result == us.BAD_SIGNATURE:
        return f"the signature does not verify with the pinned key {keyid}: the manifest was changed after it was signed, or it is not this key's signature"
    return "the manifest is not valid"


def check_image(path, m, need_marker):
    """Compare the image next to the manifest with it. Returns the image bytes or None (not there). With need_marker the image must
    be there and carry the `required` signing marker exactly once. Raises Refusal."""
    data, problem = us.read_image(path, m)
    if problem:
        raise us.Refusal(problem)
    if need_marker:
        if data is None:
            raise us.Refusal(f"the image {m['file']} is not next to the manifest: it is needed to check size, SHA-256 and the signing marker")
        markers = us.signing_marker(data)
        if not markers:
            raise us.Refusal(f"the image {m['file']} carries no signing marker: it is not a build of this repository's firmware that "
                             "knows about signed updates (the firmware embeds one)")
        if len(markers) > 1:
            raise us.Refusal(f"the image {m['file']} carries {len(markers)} signing markers; a release image has exactly one")
        if markers[0] == "off":
            raise us.Refusal(f"the image {m['file']} was built with signature enforcement off (a development build, "
                             "UPDATE_REQUIRE_SIGNATURE=0): it must not be released")
        if markers[0] != "required":
            raise us.Refusal(f"the image {m['file']} has the signing marker {markers[0]!r}, not 'required'")
    return data


def read_key(key_file):
    """The key PEM from --key-file or $UPDATE_SIGNING_KEY, or None when there is none. Never from an argument. Raises Refusal."""
    env = os.environ.pop(KEY_ENV, None)
    if env is not None and not env.strip():
        env = None
    if env is not None and key_file:
        raise us.Refusal(f"the key is given twice ({KEY_ENV} and --key-file): give it one way only")
    if key_file:
        try:
            st = os.stat(key_file)
        except OSError as e:
            raise us.Refusal(f"cannot read the key file {key_file}: {e.strerror or e}")
        if not stat.S_ISREG(st.st_mode):
            raise us.Refusal(f"the key file {key_file} is not a plain file")
        if st.st_mode & 0o077:
            raise us.Refusal(f"the key file {key_file} is readable by other users (mode {oct(st.st_mode & 0o777)[2:]}): run chmod 600 on it")
        try:
            with open(key_file, "rb") as f:
                raw = f.read(16 * 1024 + 1)
            env = raw.decode("utf-8")
        except (OSError, UnicodeDecodeError):
            raise us.Refusal(f"cannot read the key file {key_file} as text")
    if env is None:
        return None
    return us.key_text_to_pem(env)


def sign(manifest_path, pinned, pem, openssl=None):
    """Sign the manifest at `manifest_path` with the key in `pem`. Returns (version, keyid). Raises Refusal; writes nothing then."""
    m = us.load_manifest(manifest_path)
    why = us.manifest_problem(m, release=True)
    if why:
        raise us.Refusal(f"the manifest is not valid: {why}")
    check_image(manifest_path, m, need_marker=True)
    canonical = us.canonical_bytes(m)
    with us.private_workdir() as work:
        keyfile = os.path.join(work, "signing-key.pem")
        us.write_private(keyfile, pem.encode("ascii"))
        pub = us.public_point_from_key_file(keyfile, openssl)
        kid = us.key_id(pub)
        if pub not in pinned.keys:
            raise us.Refusal(f"the signing key (key id {kid}) is not one of the keys pinned in the keys header ({ids_text(pinned)}): "
                             "a release signed with it could not be verified by any display. Check that the secret holds the "
                             "private half of a pinned key")
        raw = us.sign_with_key_file(keyfile, canonical, openssl)
        if not us.verify_message(pub, canonical, raw):
            raise us.Refusal("the signature made by openssl does not verify with this tool's own P-256 verifier: nothing was written")
        if not us.openssl_verify(pub, canonical, raw, work, openssl):
            raise us.Refusal("the signature made by openssl does not verify with openssl and the pinned public key: nothing was written")
    signed = dict(m)
    signed["notes"] = m.get("notes") or ""
    signed["keyid"], signed["sig"] = kid, raw.hex()
    text = us.manifest_text(signed)
    # What is about to be written must pass the same check as --check, before anything is touched.
    result, _ = us.classify(_parse_bytes(text), pinned.keys)
    if result != us.OK:
        raise us.Refusal("the signed manifest does not pass the final check: nothing was written")
    us.write_atomic(manifest_path, text)
    return m["version"], kid


def _parse_bytes(text):
    return json.loads(text.decode("utf-8"))


def check(manifest_path, pinned, allow_unsigned):
    """Verify the manifest against the pinned keys. Returns a line to print. Raises Refusal."""
    m = us.load_manifest(manifest_path)
    why = us.manifest_problem(m)
    if why:
        raise us.Refusal(f"the manifest is not valid: {why}")
    result, keyid = us.classify(m, pinned.keys)
    if result not in (us.OK, us.UNSIGNED) or (result == us.UNSIGNED and not allow_unsigned):
        raise us.Refusal(result_problem(result, keyid, pinned))     # the signature first: it is the more serious finding
    image = check_image(manifest_path, m, need_marker=False)
    compared = "the image next to it matches" if image is not None else "no image next to it: not compared"
    if result == us.UNSIGNED:
        return (f"version={m['version']} is NOT signed; accepted only because unsigned releases are allowed (REQUIRE_SIGNING is 0): "
                f"displays that enforce signatures will refuse this release ({compared})")
    return f"ok version={m['version']} keyid={keyid}: the signature is valid and the key is pinned ({compared})"


def sign_or_keep(manifest_path, pinned, key_file, allow_unsigned):
    """The sign command. Returns the line to print on stdout; may print the warning of the transition rule on stderr."""
    m = us.load_manifest(manifest_path)
    state, _, _ = us.signature_fields(m)
    if state != "none":                                   # already signed: check, change nothing
        why = us.manifest_problem(m)
        if why:
            raise us.Refusal(f"the manifest is not valid: {why}")
        return "already signed, " + check(manifest_path, pinned, allow_unsigned=False) + "; the manifest was not changed"
    why = us.manifest_problem(m, release=True)
    if why:
        raise us.Refusal(f"the manifest is not valid: {why}")
    check_image(manifest_path, m, need_marker=False)      # size and SHA-256 are checked whatever happens next
    pem = read_key(key_file)
    if pem is None:
        if not allow_unsigned:
            raise us.Refusal(f"{KEY_ENV} is empty or not set (and no --key-file), so the manifest cannot be signed. An unsigned release "
                             "would be refused by every display that checks signatures, so nothing is published")
        print(f"sign_manifest: WARNING: {UNSIGNED_WARNING}", file=sys.stderr)
        return f"unsigned version={m['version']} (left as it is: no key)"
    version, kid = sign(manifest_path, pinned, pem)
    return f"signed version={version} keyid={kid}"


def main(argv=None):
    p = argparse.ArgumentParser(prog="sign_manifest.py", allow_abbrev=False,
                                description="Sign the firmware manifest, or check a signed one (shared/manifest-signing.md).")
    p.add_argument("manifest", nargs="?", help="the manifest.json to sign (the image ledmatrix-<version>.bin must be next to it)")
    p.add_argument("--check", metavar="MANIFEST", help="verify this signed manifest against the pinned keys; needs no key")
    p.add_argument("--keys-header", required=True, metavar="FILE", help="arduino/ledmatrix/update_keys.h: the pinned public keys")
    p.add_argument("--key-file", metavar="PATH", help=f"the private key (PEM, or one line of base64) in a file mode 0600, instead of ${KEY_ENV}")
    p.add_argument("--allow-unsigned", action="store_true",
                   help="the transition rule: with no key, warn loudly and leave the manifest unsigned (exit 0); with --check, accept an "
                        "unsigned manifest. A signed manifest must always verify")
    args = p.parse_args(argv)
    if bool(args.manifest) == bool(args.check):
        p.error("give either a manifest to sign or --check MANIFEST")
    if args.check and args.key_file:
        p.error("--check needs no key: do not give --key-file")
    try:
        pinned = us.parse_keys_header(args.keys_header)
        if args.check:
            line = check(args.check, pinned, args.allow_unsigned)
        else:
            line = sign_or_keep(args.manifest, pinned, args.key_file, args.allow_unsigned)
    except us.Refusal as e:
        print(f"sign_manifest: {e}", file=sys.stderr)
        return 1
    except OSError as e:
        print(f"sign_manifest: {e.strerror or e}", file=sys.stderr)
        return 1
    print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
