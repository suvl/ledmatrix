#!/usr/bin/env python3
r"""Shared code of the manifest-signing tools (shared/manifest-signing.md). Python 3.8 or newer, standard library only, plus the
`openssl` command line for the parts that need a private key.

Not a program. tools/sign_manifest.py, tools/gen_signing_key.py, tools/make_test_manifests.py and tools/test_sign_manifest.py load it
by its explicit path (importlib), because `python3 -I` drops the script's folder from sys.path.

What is here:
- the contract's text rules: lowercase hex, key id, the manifest rules, the canonical bytes (section 2), the classification of a
  manifest in the board's order (section 5.1: rules, shape, key, signature);
- a pure-Python ECDSA P-256 verifier, the reference that proves the signer's output and the vectors without trusting only openssl;
- DER <-> raw (r||s) signature conversion and the public key wrapper, with strict parsers;
- parse_keys_header(): reads the pinned keys from arduino/ledmatrix/update_keys.h, strictly;
- the openssl helpers (a private 0700 work folder, 0600 files, signing, public key, verification). The key text is only ever put in
  such a file, never in an argument, never in the environment of a child, never in a message.

Every value in the tests is a throwaway. Nothing here prints a key, a signature or the canonical text."""
import base64
import contextlib
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import tempfile

PREFIX = "ledmatrix-update-v1"          # the domain-separation line
NOTES_MAX = 200                         # characters
CANONICAL_MAX = 1024
MIN_SIZE_EXCLUSIVE = 100 * 1024         # the size must be above 100 KB
MAX_SIZE = 2 * 1024 * 1024              # and at most one app slot
MANIFEST_MAX_BYTES = 2048               # the longest answer a board reads
FIELDS_V1 = ("version", "file", "size", "sha256", "notes")
FIELDS_SIG = ("keyid", "sig")
KEYID_HEX = 8
SIG_HEX = 128
PUB_HEX = 130

MARKER_PREFIX = b"\x01LEDMATRIX-FW-SIGNING\x02"   # the firmware embeds PREFIX + "required" or "off" + END (section 4)
MARKER_END = b"\x03"
MARKER_RE = re.compile(re.escape(MARKER_PREFIX) + rb"([a-z]{1,16})" + re.escape(MARKER_END))

OK, UNSIGNED, MALFORMED, UNKNOWN_KEY, BAD_SIGNATURE, MANIFEST_INVALID = (
    "ok", "unsigned", "malformed", "unknown_key", "bad_signature", "manifest_invalid")

VERSION_RE = re.compile(r"[0-9]{1,4}\.[0-9]{1,4}\.[0-9]{1,4}")
VERSION_STRICT_RE = re.compile(r"(0|[1-9][0-9]{0,3})\.(0|[1-9][0-9]{0,3})\.(0|[1-9][0-9]{0,3})")


class Refusal(Exception):
    """A plain-words reason to stop. The tools print it and exit 1."""


# ---- text rules ----------------------------------------------------------------------------------------------------------------

_HEX_DIGITS = frozenset("0123456789abcdef")


def is_lower_hex(text, length=None):
    """True for a str made only of 0-9 a-f (and of exactly `length` characters when given). Upper case is not hex here."""
    return (isinstance(text, str) and bool(text) and (length is None or len(text) == length)
            and all(c in _HEX_DIGITS for c in text))


def from_lower_hex(text, nbytes):
    """The `nbytes` bytes of lowercase hex `text`. Raises ValueError on anything else (case, length, characters)."""
    if not is_lower_hex(text, 2 * nbytes):
        raise ValueError(f"not exactly {2 * nbytes} lowercase hex characters")
    return bytes.fromhex(text)


def has_control(text):
    """True when text holds a character below 0x20 or the DEL character: the contract forbids them in notes."""
    return any(ord(c) < 0x20 or ord(c) == 0x7F for c in text)


def manifest_problem(m, release=False):
    """None when the manifest's five fields follow the rules of firmware-update.md and manifest-signing.md section 2, otherwise one
    plain sentence. `release=True` is the signer's stricter reading: the version has no leading zeros (the board and the service
    accept them; make_manifest.py never writes them)."""
    if not isinstance(m, dict):
        return "the manifest is not a JSON object"
    ver = m.get("version")
    if not isinstance(ver, str) or not VERSION_RE.fullmatch(ver):
        return "the version is not three numbers of 1 to 4 digits"
    if release and not VERSION_STRICT_RE.fullmatch(ver):
        return "the version has a leading zero (a release is MAJOR.MINOR.PATCH without leading zeros)"
    if m.get("file") != f"ledmatrix-{ver}.bin":
        return "the file name is not ledmatrix-<version>.bin"
    size = m.get("size")
    if isinstance(size, bool) or not isinstance(size, int) or not MIN_SIZE_EXCLUSIVE < size <= MAX_SIZE:
        return "the size is not a whole number above 100 KB and at most 2097152 bytes"
    if not is_lower_hex(m.get("sha256"), 64):
        return "the sha256 is not 64 lowercase hex characters"
    notes = m.get("notes")
    if notes is not None:
        if not isinstance(notes, str):
            return "the notes are not text"
        if len(notes) > NOTES_MAX:
            return "the notes are longer than 200 characters"
        if has_control(notes):
            return "the notes hold a control character"
        try:
            notes.encode("utf-8")
        except UnicodeEncodeError:
            return "the notes are not valid text (a lone surrogate)"
    return None


def canonical_bytes(m):
    """The bytes that are signed (section 2): six lines joined by one line feed, no line feed after the last, UTF-8. Built from the
    manifest's decoded values only. Raises ValueError for a manifest that breaks the rules, so nothing odd is ever signed."""
    why = manifest_problem(m)
    if why:
        raise ValueError(why)
    notes = m.get("notes") or ""
    text = "\n".join([PREFIX, "version=" + m["version"], "file=" + m["file"], "size=" + str(m["size"]),
                      "sha256=" + m["sha256"], "notes=" + notes])
    data = text.encode("utf-8")
    if len(data) >= CANONICAL_MAX:
        raise ValueError("the canonical text is too long")
    return data


def key_id(pub):
    """The first 4 bytes of SHA-256 of the 65-byte public point, as 8 lowercase hex characters."""
    return hashlib.sha256(pub).hexdigest()[:KEYID_HEX]


def signature_fields(m):
    """What the manifest says about its signature: (state, keyid, sig) with state 'none' (neither field), 'both' or 'one'. A JSON
    null counts as absent."""
    keyid, sig = m.get("keyid"), m.get("sig")
    present = [v is not None for v in (keyid, sig)]
    if not any(present):
        return "none", None, None
    return ("both" if all(present) else "one"), keyid, sig


def classify(m, pinned, verify=None):
    """The board's order (section 5.1), as far as it can be run on a laptop: returns (result, keyid) where result is one of OK,
    UNSIGNED, MALFORMED, UNKNOWN_KEY, BAD_SIGNATURE, MANIFEST_INVALID and keyid is the manifest's (or None).
    `pinned` is the list of 65-byte public keys; `verify(pub, digest32, sig64)` defaults to the pure-Python verifier."""
    verify = verify or p256_verify
    if manifest_problem(m):
        return MANIFEST_INVALID, None
    state, keyid, sig = signature_fields(m)
    if state == "none":
        return UNSIGNED, None
    if state == "one" or not is_lower_hex(keyid, KEYID_HEX) or not is_lower_hex(sig, SIG_HEX):
        return MALFORMED, None
    candidates = [p for p in pinned if key_id(p) == keyid]
    if not candidates:
        return UNKNOWN_KEY, keyid
    digest = hashlib.sha256(canonical_bytes(m)).digest()
    raw = bytes.fromhex(sig)
    if any(verify(p, digest, raw) for p in candidates):
        return OK, keyid
    return BAD_SIGNATURE, keyid


# ---- ECDSA on NIST P-256, pure Python (the reference verifier) ------------------------------------------------------------------

P = 0xffffffff00000001000000000000000000000000ffffffffffffffffffffffff
A = P - 3
B = 0x5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b
N = 0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551
G = (0x6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296,
     0x4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5)


def _add(p1, p2):
    """Affine point addition; None is the point at infinity."""
    if p1 is None:
        return p2
    if p2 is None:
        return p1
    x1, y1 = p1
    x2, y2 = p2
    if x1 == x2:
        if (y1 + y2) % P == 0:
            return None
        lam = (3 * x1 * x1 + A) * pow(2 * y1, -1, P) % P
    else:
        lam = (y2 - y1) * pow(x2 - x1, -1, P) % P
    x3 = (lam * lam - x1 - x2) % P
    return x3, (lam * (x1 - x3) - y1) % P


def on_curve(point):
    if not (isinstance(point, tuple) and len(point) == 2):
        return False
    x, y = point
    return 0 <= x < P and 0 <= y < P and (y * y - (x * x * x + A * x + B)) % P == 0


def parse_public_point(pub):
    """(x, y) of a 65-byte uncompressed point that is on the curve, or None."""
    if not isinstance(pub, (bytes, bytearray)) or len(pub) != 65 or pub[0] != 4:
        return None
    point = (int.from_bytes(pub[1:33], "big"), int.from_bytes(pub[33:], "big"))
    return point if on_curve(point) else None


def p256_verify(pub, digest, sig):
    """True only for a valid ECDSA P-256 signature: `pub` 65 bytes (04 || X || Y), `digest` the 32-byte SHA-256 of the message,
    `sig` 64 bytes r || s big endian. 1 <= r < n and 1 <= s < n are required; s and n-s both verify (the contract accepts that)."""
    q = parse_public_point(pub)
    if q is None or not isinstance(digest, (bytes, bytearray)) or len(digest) != 32 \
            or not isinstance(sig, (bytes, bytearray)) or len(sig) != 64:
        return False
    r, s = int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:], "big")
    if not (1 <= r < N and 1 <= s < N):
        return False
    w = pow(s, -1, N)
    u1, u2 = int.from_bytes(digest, "big") * w % N, r * w % N
    # Shamir's trick: one pass over the bits of u1 and u2 together.
    both = _add(G, q)
    acc = None
    for i in range(max(u1.bit_length(), u2.bit_length()) - 1, -1, -1):
        acc = _add(acc, acc)
        b1, b2 = (u1 >> i) & 1, (u2 >> i) & 1
        if b1 and b2:
            acc = _add(acc, both)
        elif b1:
            acc = _add(acc, G)
        elif b2:
            acc = _add(acc, q)
    return acc is not None and acc[0] % N == r


def verify_message(pub, message, sig):
    return p256_verify(pub, hashlib.sha256(message).digest(), sig)


# ---- DER and the public key wrapper (strict) ------------------------------------------------------------------------------------

# The fixed start of SubjectPublicKeyInfo for an EC key on prime256v1 with an uncompressed point: SEQUENCE(89) { SEQUENCE(19)
# { OID id-ecPublicKey, OID prime256v1 }, BIT STRING(66) { 0 unused bits, then the point }.
SPKI_PREFIX = bytes.fromhex("3059301306072a8648ce3d020106082a8648ce3d030107034200")


def spki_to_point(der):
    """The 65-byte point inside an openssl public key (DER). Refuses anything that is not exactly an EC P-256 key."""
    if len(der) != len(SPKI_PREFIX) + 65 or not der.startswith(SPKI_PREFIX) or der[len(SPKI_PREFIX)] != 4:
        raise Refusal("the key is not an EC key on the curve P-256 (prime256v1)")
    return bytes(der[len(SPKI_PREFIX):])


def point_to_spki(pub):
    return SPKI_PREFIX + pub


def point_to_pem(pub):
    body = base64.b64encode(point_to_spki(pub)).decode("ascii")
    lines = [body[i:i + 64] for i in range(0, len(body), 64)]
    return "-----BEGIN PUBLIC KEY-----\n" + "\n".join(lines) + "\n-----END PUBLIC KEY-----\n"


def _read_der_len(der, pos):
    if pos >= len(der):
        raise ValueError("DER ends early")
    first = der[pos]
    pos += 1
    if first < 0x80:
        return first, pos
    count = first & 0x7F
    if count == 0 or count > 2 or pos + count > len(der) or der[pos] == 0:
        raise ValueError("DER length is not in its shortest form")
    n = int.from_bytes(der[pos:pos + count], "big")
    if n < 0x80:
        raise ValueError("DER length is not in its shortest form")
    return n, pos + count


def _read_der_int(der, pos):
    if pos >= len(der) or der[pos] != 0x02:
        raise ValueError("DER: expected an INTEGER")
    n, pos = _read_der_len(der, pos + 1)
    body = der[pos:pos + n]
    if n == 0 or len(body) != n:
        raise ValueError("DER: bad INTEGER length")
    if body[0] & 0x80:
        raise ValueError("DER: negative INTEGER")
    if n > 1 and body[0] == 0 and not body[1] & 0x80:
        raise ValueError("DER: INTEGER is not minimal")
    return int.from_bytes(body, "big"), pos + n


def der_to_raw(der):
    """An ECDSA signature as openssl writes it, SEQUENCE { INTEGER r, INTEGER s }, to 64 bytes r || s. Strict DER; raises
    ValueError for anything else (and for values that do not fit 32 bytes)."""
    der = bytes(der)
    if not der or der[0] != 0x30:
        raise ValueError("DER: expected a SEQUENCE")
    n, pos = _read_der_len(der, 1)
    if pos + n != len(der):
        raise ValueError("DER: the SEQUENCE does not span the data")
    r, pos = _read_der_int(der, pos)
    s, pos = _read_der_int(der, pos)
    if pos != len(der):
        raise ValueError("DER: trailing bytes")
    if r >= 1 << 256 or s >= 1 << 256:
        raise ValueError("DER: a value is wider than 32 bytes")
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


def _der_int(value):
    body = value.to_bytes(max(1, (value.bit_length() + 7) // 8), "big")
    if body[0] & 0x80:
        body = b"\x00" + body
    return b"\x02" + bytes([len(body)]) + body


def raw_to_der(raw):
    """64 bytes r || s to the DER SEQUENCE openssl reads."""
    if len(raw) != 64:
        raise ValueError("a raw signature is 64 bytes")
    body = _der_int(int.from_bytes(raw[:32], "big")) + _der_int(int.from_bytes(raw[32:], "big"))
    return b"\x30" + bytes([len(body)]) + body        # at most 2 * 35 bytes: a short length is enough


# ---- the pinned keys: update_keys.h -----------------------------------------------------------------------------------------------

class PinnedKeys:
    """What arduino/ledmatrix/update_keys.h pins: `keys` (a list of 65-byte points, the current key first), `ids` (their key ids)
    and `require_default` (the header's default for UPDATE_REQUIRE_SIGNATURE, 1 or 0, or None when it does not say)."""

    def __init__(self, keys, require_default):
        self.keys = list(keys)
        self.ids = [key_id(k) for k in keys]
        self.require_default = require_default


_COUNT_RE = re.compile(r"#[ \t]*define[ \t]+UPDATE_KEY_COUNT[ \t]+([0-9]+)")
_KEY_RE = re.compile(r'#[ \t]*define[ \t]+UPDATE_KEY_([0-9]+)[ \t]+"([^"]*)"')
_REQ_RE = re.compile(r"#[ \t]*define[ \t]+UPDATE_REQUIRE_SIGNATURE[ \t]+([0-9]+)")
_CONDITIONAL_RE = re.compile(r"#[ \t]*(if|ifdef|ifndef|elif|elifdef|elifndef|else|endif|undef)\b")


def parse_keys_header_text(text, name="update_keys.h"):
    """Strict reader of the pinned keys. A header it cannot be sure about is refused, never guessed at: conditional compilation
    around the keys (other than the one #ifndef UPDATE_REQUIRE_SIGNATURE ... #endif), a line that mentions a key or the flag in any
    other form, a repeated or missing definition, a key that is not exactly 130 lowercase hex characters starting 04, a point that is
    not on the curve, two equal keys, a count other than 1 or 2 that disagrees with the keys that are defined."""
    def refuse(why):
        raise Refusal(f"{name}: {why}")

    if "\\\n" in text or "\\\r" in text:
        refuse("a line continuation (backslash at the end of a line) is not supported")
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    if "/*" in text:
        refuse("a block comment is not closed")
    count_lines, keys, require = [], {}, []
    depth = 0
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.split("//", 1)[0].strip()
        if not line:
            continue
        where = f"line {lineno}"
        if _CONDITIONAL_RE.match(line):
            if line.replace(" ", "").replace("\t", "") == "#ifndefUPDATE_REQUIRE_SIGNATURE" and depth == 0:
                depth = 1
            elif re.fullmatch(r"#[ \t]*endif", line) and depth == 1:
                depth = 0
            else:
                refuse(f"{where}: conditional compilation or #undef is not supported here (only one #ifndef UPDATE_REQUIRE_SIGNATURE "
                       "block around the default of the flag)")
            continue
        m = _COUNT_RE.fullmatch(line)
        if m:
            if depth:
                refuse(f"{where}: UPDATE_KEY_COUNT must not sit inside a conditional")
            count_lines.append(int(m.group(1)))
            continue
        m = _KEY_RE.fullmatch(line)
        if m:
            if depth:
                refuse(f"{where}: a key must not sit inside a conditional")
            idx = int(m.group(1))
            if idx in keys:
                refuse(f"{where}: UPDATE_KEY_{idx} is defined twice")
            keys[idx] = (m.group(2), where)
            continue
        m = _REQ_RE.fullmatch(line)
        if m:
            require.append((int(m.group(1)), depth))
            continue
        if "UPDATE_KEY" in line or "UPDATE_REQUIRE_SIGNATURE" in line:
            refuse(f"{where}: a line about the keys or the flag that is not in the expected form")
    if depth:
        refuse("the #ifndef UPDATE_REQUIRE_SIGNATURE block is not closed")
    if len(count_lines) != 1:
        refuse(f"UPDATE_KEY_COUNT must be defined exactly once (found {len(count_lines)})")
    count = count_lines[0]
    if count not in (1, 2):
        refuse(f"UPDATE_KEY_COUNT is {count}; it must be 1 or 2")
    for idx in sorted(keys):
        if idx >= count:
            refuse(f"{keys[idx][1]}: UPDATE_KEY_{idx} is defined but UPDATE_KEY_COUNT is {count}")
    points = []
    for idx in range(count):
        if idx not in keys:
            refuse(f"UPDATE_KEY_{idx} is not defined (UPDATE_KEY_COUNT is {count})")
        value, where = keys[idx]
        if not is_lower_hex(value, PUB_HEX) or not value.startswith("04"):
            refuse(f"{where}: UPDATE_KEY_{idx} must be exactly 130 lowercase hex characters starting 04")
        pub = bytes.fromhex(value)
        if parse_public_point(pub) is None:
            refuse(f"{where}: UPDATE_KEY_{idx} is not a point on the curve P-256")
        points.append(pub)
    if len(set(points)) != len(points):
        refuse("the two keys are the same key")
    if len(require) > 1 or (require and require[0][1] != 1) or (require and require[0][0] not in (0, 1)):
        refuse("UPDATE_REQUIRE_SIGNATURE must be defined once, as 0 or 1, inside its #ifndef block")
    return PinnedKeys(points, require[0][0] if require else None)


def parse_keys_header(path):
    try:
        with open(path, "rb") as f:
            raw = f.read(64 * 1024 + 1)
    except OSError as e:
        raise Refusal(f"cannot read the keys header {path}: {e.strerror or e}")
    if len(raw) > 64 * 1024:
        raise Refusal(f"{os.path.basename(path)}: the file is far too large for a keys header")
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        raise Refusal(f"{os.path.basename(path)}: the file is not UTF-8 text")
    return parse_keys_header_text(text, os.path.basename(path))


# ---- the manifest file --------------------------------------------------------------------------------------------------------------

def _no_duplicates(pairs):
    out = {}
    for k, v in pairs:
        if k in out:
            raise ValueError(f"the key {k!r} appears twice")
        out[k] = v
    return out


def _no_constant(name):
    raise ValueError(f"{name} is not allowed")


def load_manifest(path):
    """The manifest.json at `path` as a dict. Strict: UTF-8 without a byte order mark, one JSON object, no repeated key, no NaN, no
    field other than the seven of the contract. Raises Refusal."""
    try:
        with open(path, "rb") as f:
            raw = f.read(MANIFEST_MAX_BYTES * 4 + 1)
    except OSError as e:
        raise Refusal(f"cannot read the manifest {path}: {e.strerror or e}")
    if len(raw) > MANIFEST_MAX_BYTES * 4:
        raise Refusal("the manifest is far larger than a manifest can be")
    try:
        m = json.loads(raw.decode("utf-8"), object_pairs_hook=_no_duplicates, parse_constant=_no_constant)
    except (UnicodeDecodeError, ValueError) as e:
        raise Refusal(f"the manifest is not valid JSON: {e}")
    if not isinstance(m, dict):
        raise Refusal("the manifest is not a JSON object")
    extra = sorted(set(m) - set(FIELDS_V1) - set(FIELDS_SIG))
    if extra:
        raise Refusal(f"the manifest has a field that is not part of the contract: {extra[0]!r}")
    return m


def manifest_text(m):
    """The bytes of manifest.json: compact JSON, the five v1 keys first and then keyid and sig when present, one final line feed."""
    out = {k: m.get(k) for k in FIELDS_V1}
    if out["notes"] is None:
        out["notes"] = ""
    for k in FIELDS_SIG:
        if m.get(k) is not None:
            out[k] = m[k]
    return (json.dumps(out, separators=(",", ":"), ensure_ascii=False) + "\n").encode("utf-8")


def write_atomic(path, data, mode=0o644):
    d = os.path.dirname(os.path.abspath(path))
    fd, tmp = tempfile.mkstemp(prefix=".tmp-", dir=d)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        os.chmod(tmp, mode)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def read_image(manifest_path, m):
    """The image next to the manifest and whether it matches size and SHA-256. Returns (bytes or None, problem or None): None bytes
    when the file is not there. Raises nothing for a mismatch; the caller decides."""
    path = os.path.join(os.path.dirname(os.path.abspath(manifest_path)), m["file"])
    try:
        with open(path, "rb") as f:
            data = f.read(MAX_SIZE + 1)
    except FileNotFoundError:
        return None, None
    except OSError as e:
        return None, f"cannot read the image {m['file']}: {e.strerror or e}"
    if len(data) != m["size"]:
        return data, f"the image {m['file']} is {len(data)} bytes but the manifest says {m['size']}"
    if hashlib.sha256(data).hexdigest() != m["sha256"]:
        return data, f"the SHA-256 of the image {m['file']} is not the one in the manifest"
    return data, None


def signing_marker(data):
    """The list of signing markers an image carries, as text ('required', 'off', ...): the prefix, 1 to 16 lowercase letters, the
    end byte. The bare prefix the firmware holds as a constant (followed by anything else) is not a marker."""
    return [m.group(1).decode("ascii") for m in MARKER_RE.finditer(data)]


# ---- openssl ------------------------------------------------------------------------------------------------------------------------

def find_openssl():
    """The openssl command line, or None."""
    return shutil.which("openssl")


_REDACT_RE = re.compile(r"[A-Za-z0-9+/=_-]{24,}")


def safe_text(text, limit=200):
    """One short line of a tool's complaint with long runs of key-like characters removed, so that even a surprising message cannot
    carry key material into a log."""
    text = _REDACT_RE.sub("<removed>", text or "")
    text = " ".join(text.split())
    return text[:limit]


def run_openssl(args, stdin=b"", openssl=None, timeout=60):
    """Run `openssl <args>` with a clean environment (PATH only: the signing key variable is never passed on) and no terminal, and
    return its stdout as bytes. Raises Refusal with a short, redacted sentence when it fails."""
    exe = openssl or find_openssl()
    if not exe:
        raise Refusal("the openssl command line is not installed (it is needed for the private key operations)")
    env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LC_ALL": "C"}
    try:
        r = subprocess.run([exe, *args], input=stdin, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, timeout=timeout)
    except subprocess.TimeoutExpired:
        raise Refusal(f"openssl {args[0]} did not finish in {timeout} seconds")
    except OSError as e:
        raise Refusal(f"cannot run openssl: {e.strerror or e}")
    if r.returncode != 0:
        raise Refusal(f"openssl {args[0]} failed (exit {r.returncode}): {safe_text(r.stderr.decode('utf-8', 'replace'))}")
    return r.stdout


@contextlib.contextmanager
def private_workdir():
    """A fresh folder mode 0700 (under $RUNNER_TEMP when that is set and exists, otherwise the system's temporary folder) that is
    removed, with everything in it, when the block ends, whatever happens. Key material goes only in files made by write_private()."""
    base = os.environ.get("RUNNER_TEMP")
    if not base or not os.path.isabs(base) or not os.path.isdir(base):
        base = None
    path = tempfile.mkdtemp(prefix="ledmatrix-sign-", dir=base)     # mkdtemp makes it 0700
    os.chmod(path, 0o700)
    try:
        yield path
    finally:
        failed = []
        for name in os.listdir(path):
            full = os.path.join(path, name)
            try:
                size = os.lstat(full).st_size
                with open(full, "r+b") as f:                    # best effort: overwrite before unlinking
                    f.write(b"\0" * size)
                    f.flush()
                    os.fsync(f.fileno())
            except OSError:
                pass
            try:
                os.unlink(full)
            except OSError:
                failed.append(name)
        try:
            os.rmdir(path)
        except OSError:
            failed.append("(the folder)")
        if failed:
            raise Refusal(f"could not remove the temporary signing folder {path}: remove it by hand")


def write_private(path, data):
    """Create a new file mode 0600 (never overwrites) and write bytes."""
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(data)


_PEM_LABEL_RE = re.compile(r"-----BEGIN ([A-Z0-9 ]+)-----")
_PEM_OK_LABELS = ("EC PRIVATE KEY", "PRIVATE KEY", "EC PARAMETERS")


def key_text_to_pem(text):
    """The PEM text of an EC private key from what a person or a secret holds: the PEM itself, or the same PEM as one line of base64
    (the form of the GitHub secret). Refuses anything else, an empty text, and a key protected by a password (openssl would wait for
    the password on a terminal). The message never contains any of the text."""
    if not isinstance(text, str) or not text.strip():
        raise Refusal("the key text is empty")
    if len(text) > 16 * 1024:
        raise Refusal("the key text is far too long for an EC key")
    t = text.strip()
    if "-----BEGIN" not in t:
        try:
            decoded = base64.b64decode("".join(t.split()), validate=True)
            t = decoded.decode("ascii").strip()
        except (ValueError, UnicodeDecodeError):
            raise Refusal("the key is neither PEM text nor one line of base64 of PEM text")
        if "-----BEGIN" not in t:
            raise Refusal("the key is neither PEM text nor one line of base64 of PEM text")
    t = t.replace("\r\n", "\n").replace("\r", "\n")
    if "ENCRYPTED" in t or "Proc-Type" in t:
        raise Refusal("the key is protected by a password; the signing key must be an unprotected PEM (it is kept in a secret instead)")
    labels = _PEM_LABEL_RE.findall(t)
    if not labels or any(label not in _PEM_OK_LABELS for label in labels) \
            or not any(label in ("EC PRIVATE KEY", "PRIVATE KEY") for label in labels):
        raise Refusal("the key text is not the PEM of an EC private key")
    if not t.startswith("-----BEGIN") or not t.endswith("-----"):
        raise Refusal("the key text is not the PEM of an EC private key")
    return t + "\n"


def public_point_from_key_file(keyfile, openssl=None):
    """The 65-byte public point of the private key in `keyfile`; Refusal unless it is an EC key on P-256."""
    try:
        der = run_openssl(["ec", "-in", keyfile, "-pubout", "-outform", "DER", "-passin", "pass:"], openssl=openssl)
    except Refusal:
        raise Refusal("openssl could not read the key as an EC private key")
    pub = spki_to_point(der)
    if parse_public_point(pub) is None:
        raise Refusal("the public key derived from the private key is not a point on the curve P-256")
    return pub


def sign_with_key_file(keyfile, message, openssl=None):
    """ECDSA P-256 / SHA-256 signature of `message` made by openssl with the key in `keyfile`, as 64 bytes r || s."""
    der = run_openssl(["dgst", "-sha256", "-sign", keyfile, "-passin", "pass:"], stdin=message, openssl=openssl)
    try:
        return der_to_raw(der)
    except ValueError:
        raise Refusal("openssl returned a signature that is not a DER ECDSA signature")


def openssl_verify(pub, message, raw, workdir, openssl=None):
    """True when openssl itself verifies the signature `raw` (r || s) over `message` with the public point `pub`."""
    pubfile, sigfile = os.path.join(workdir, "verify-pub.pem"), os.path.join(workdir, "verify-sig.der")
    for path, data in ((pubfile, point_to_pem(pub).encode("ascii")), (sigfile, raw_to_der(raw))):
        if os.path.exists(path):
            os.unlink(path)
        write_private(path, data)
    exe = openssl or find_openssl()
    if not exe:
        raise Refusal("the openssl command line is not installed")
    env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LC_ALL": "C"}
    r = subprocess.run([exe, "dgst", "-sha256", "-verify", pubfile, "-signature", sigfile], input=message,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, timeout=60)
    return r.returncode == 0 and b"Verified OK" in r.stdout


def generate_key_pem(openssl=None, curve="prime256v1"):
    """A new EC private key as PEM text, from `openssl ecparam -genkey -noout`, kept in memory (never given a path by openssl)."""
    out = run_openssl(["ecparam", "-name", curve, "-genkey", "-noout"], openssl=openssl)
    try:
        return key_text_to_pem(out.decode("ascii"))
    except UnicodeDecodeError:
        raise Refusal("openssl returned a key that is not text")


def mode_of(path):
    return stat.S_IMODE(os.stat(path).st_mode)
