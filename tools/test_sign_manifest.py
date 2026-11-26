#!/usr/bin/env python3
"""Tests for the manifest-signing tools (shared/manifest-signing.md): tools/update_signing.py, tools/sign_manifest.py,
tools/gen_signing_key.py and tools/make_test_manifests.py. Python 3.8 or newer, standard library only:
    python3 -I tools/test_sign_manifest.py

Two layers. The VECTOR tests need nothing but Python: they read shared/manifest-signing-vectors.json and check the canonical bytes,
the key ids, that every `ok` vector verifies and every bad one does not (with the pure-Python P-256 verifier), and that every shape
case classifies as the contract says. They always run. The OPENSSL tests make fresh throwaway keys at run time with the openssl
command line (skipped with a plain message when it is not installed), sign a generated manifest and a fake image, verify, tamper,
and check that no secret reaches stdout or stderr and that no temporary key file is left. The MUTATION tests break the tools on
purpose (the prefix, the notes line, the key id rule, the range check, the pinned-key check, the marker check...) and require the
checks to notice. No private key is committed or printed: the keys here live in a temporary folder that is removed at the end."""
import base64
import hashlib
import importlib.util
import io
import json
import os
import re
import stat
import subprocess
import sys
import tempfile
import unittest
import unittest.mock
from contextlib import redirect_stderr, redirect_stdout

sys.dont_write_bytecode = True  # no tools/__pycache__ in the repository
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
VECTORS = os.path.join(ROOT, "shared", "manifest-signing-vectors.json")
KEYS_HEADER = os.path.join(ROOT, "arduino", "ledmatrix", "update_keys.h")
WORKFLOW = os.path.join(ROOT, ".github", "workflows", "service.yml")


def load(name, filename=None, source=None, path=None):
    """Import tools/<name>.py by its path (python3 -I has no tools/ on sys.path). With `source`, a modified copy of the code."""
    path = path or os.path.join(HERE, (filename or name) + ".py")
    if source is None:
        with open(path, "r", encoding="utf-8") as f:
            source = f.read()
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    exec(compile(source, path, "exec"), mod.__dict__)
    return mod


def source_of(name):
    with open(os.path.join(HERE, name + ".py"), "r", encoding="utf-8") as f:
        return f.read()


def mutated(name, *pairs):
    """The module `name` with each (old, new) pair of texts replaced (every replacement must happen: a mutation that changes nothing
    proves nothing)."""
    src = source_of(name)
    for old, new in pairs:
        assert src.count(old) >= 1, f"the mutation target {old!r} is not in {name}.py"
        src = src.replace(old, new)
    return load(name + "_mutated", filename=name, source=src)


us = load("update_signing")
sm = load("sign_manifest")
gk = load("gen_signing_key")
mt = load("make_test_manifests")
mm = load("make_manifest")
tmm = load("test_make_manifest")          # for make_image: fake ESP images with the right structure

OPENSSL = us.find_openssl()
NO_OPENSSL = "openssl is not installed: the tests that make a key at run time are skipped (the vector tests still run)"
needs_openssl = unittest.skipUnless(OPENSSL, NO_OPENSSL)

with open(VECTORS, "r", encoding="utf-8") as _f:
    VEC = json.load(_f)
PINNED = [bytes.fromhex(k["public_key_hex"]) for k in VEC["keys"] if k["pinned"]]
MARKER_REQUIRED = b"\x01LEDMATRIX-FW-SIGNING\x02required\x03"
MARKER_OFF = b"\x01LEDMATRIX-FW-SIGNING\x02off\x03"
SIGN_ENV = "UPDATE_SIGNING_KEY"


# ---- the checks that the mutation tests re-run ------------------------------------------------------------------------------------

def contract_checks(u, d):
    """Everything the vectors and the contract say about the shared module `u`; returns a list of failures (empty when it is right).
    An exception counts as a failure, so a mutation that crashes is noticed too."""
    fails = []
    pubs = {k["name"]: bytes.fromhex(k["public_key_hex"]) for k in d["keys"]}
    pinned = [pubs[k["name"]] for k in d["keys"] if k["pinned"]]
    for k in d["keys"]:
        try:
            if u.key_id(pubs[k["name"]]) != k["id"]:
                fails.append(f"key {k['name']}: the key id is not {k['id']}")
            if u.parse_public_point(pubs[k["name"]]) is None:
                fails.append(f"key {k['name']}: not recognised as a point on the curve")
        except Exception as e:
            fails.append(f"key {k['name']}: raised {e!r}")
    for v in d["vectors"]:
        name, m = v["name"], v["manifest"]
        try:
            got, _ = u.classify(m, pinned)
            if got != v["expect"]:
                fails.append(f"{name}: classified {got}, expected {v['expect']}")
            if "canonical" in v:
                cb = u.canonical_bytes(m)
                if cb != v["canonical"].encode("utf-8") or cb.hex() != v["canonical_hex"] \
                        or hashlib.sha256(cb).hexdigest() != v["canonical_sha256"]:
                    fails.append(f"{name}: the canonical bytes differ from the vector's")
        except Exception as e:
            fails.append(f"{name}: raised {e!r}")
    # the rules that no vector exercises on its own
    try:
        a = pubs["A"]
        off_curve = a[:-1] + bytes([a[-1] ^ 1])
        if u.parse_public_point(off_curve) is not None:
            fails.append("a public key that is not on the curve is accepted")
        if u.parse_public_point(a[:-1]) is not None or u.parse_public_point(b"\x02" + a[1:33] + bytes(32)) is not None:
            fails.append("a public key of the wrong shape is accepted")
    except Exception as e:
        fails.append(f"the key checks raised {e!r}")
    base = dict(next(v for v in d["vectors"] if v["name"] == "ok-basic")["manifest"])
    try:
        for label, change in (("a DEL in the notes", {"notes": "a\x7fb"}), ("an upper case sha256", {"sha256": base["sha256"].upper()}),
                              ("a size of 100 KB", {"size": 102400}), ("a size of 2 MiB + 1", {"size": 2097153}),
                              ("201 characters of notes", {"notes": "n" * 201})):
            if u.manifest_problem(dict(base, **change)) is None:
                fails.append(f"{label} is accepted")
        for label, change in (("200 characters of notes", {"notes": "n" * 200}), ("a size of 100 KB + 1", {"size": 102401})):
            if u.manifest_problem(dict(base, **change)) is not None:
                fails.append(f"{label} is refused")
        if u.classify(dict(base, size=1412537.0), pinned)[0] != "manifest_invalid":
            fails.append("a size that is not an integer is accepted")
        if u.canonical_bytes(dict(base, notes=None)) != u.canonical_bytes(dict(base, notes="")):
            fails.append("null notes are not the empty text")
        if u.classify(dict(base, keyid=None, sig=None), pinned)[0] != "unsigned":
            fails.append("null keyid and sig are not 'unsigned'")
    except Exception as e:
        fails.append(f"the rule checks raised {e!r}")
    return fails


# ---- 1. the vectors (always run) --------------------------------------------------------------------------------------------------

class VectorTests(unittest.TestCase):
    def test_the_file_is_what_the_contract_says(self):
        self.assertEqual(VEC["scheme"]["canonical_prefix"], "ledmatrix-update-v1")
        self.assertEqual([k["name"] for k in VEC["keys"]], ["A", "B", "C"])
        self.assertEqual([k["pinned"] for k in VEC["keys"]], [True, True, False])
        kinds = {}
        for v in VEC["vectors"]:
            kinds[v["expect"]] = kinds.get(v["expect"], 0) + 1
        self.assertEqual(sorted(kinds), ["bad_signature", "malformed", "manifest_invalid", "ok", "unknown_key", "unsigned"])
        self.assertEqual(len(VEC["vectors"]), 32)

    def test_all_contract_checks_pass_on_the_real_module(self):
        self.assertEqual(contract_checks(us, VEC), [])

    def test_key_ids_and_points(self):
        for k in VEC["keys"]:
            pub = bytes.fromhex(k["public_key_hex"])
            self.assertEqual(len(pub), 65)
            self.assertEqual(us.key_id(pub), k["id"], k["name"])
            self.assertIsNotNone(us.parse_public_point(pub), k["name"])

    def test_canonical_bytes_hash_and_hex(self):
        n = 0
        for v in VEC["vectors"]:
            if "canonical" not in v:
                continue
            n += 1
            cb = us.canonical_bytes(v["manifest"])
            self.assertEqual(cb, v["canonical"].encode("utf-8"), v["name"])
            self.assertEqual(cb.hex(), v["canonical_hex"], v["name"])
            self.assertEqual(hashlib.sha256(cb).hexdigest(), v["canonical_sha256"], v["name"])
            self.assertFalse(cb.endswith(b"\n"), v["name"])
            self.assertEqual(cb.count(b"\n"), 5, v["name"])
        self.assertEqual(n, 6)          # the vectors that carry their canonical bytes

    def test_every_vector_classifies_as_expected(self):
        for v in VEC["vectors"]:
            self.assertEqual(us.classify(v["manifest"], PINNED)[0], v["expect"], v["name"])

    def test_every_ok_vector_verifies_and_every_bad_one_does_not(self):
        pubs = {k["id"]: bytes.fromhex(k["public_key_hex"]) for k in VEC["keys"]}
        seen = {"ok": 0, "bad": 0}
        for v in VEC["vectors"]:
            m = v["manifest"]
            if v["expect"] == "ok":
                seen["ok"] += 1
                self.assertTrue(us.verify_message(pubs[m["keyid"]], us.canonical_bytes(m), bytes.fromhex(m["sig"])), v["name"])
            elif v["expect"] == "bad_signature":
                seen["bad"] += 1
                self.assertFalse(us.verify_message(pubs[m["keyid"]], us.canonical_bytes(m), bytes.fromhex(m["sig"])), v["name"])
        self.assertEqual(seen["ok"], 5)
        self.assertEqual(seen["bad"], 13)

    def test_valid_signatures_that_are_refused_for_another_reason(self):
        by_name = {v["name"]: v["manifest"] for v in VEC["vectors"]}
        a, c = (bytes.fromhex(k["public_key_hex"]) for k in (VEC["keys"][0], VEC["keys"][2]))
        outsider = by_name["unknown-key-valid-signature"]            # a real signature by C, but C is not pinned
        self.assertTrue(us.verify_message(c, us.canonical_bytes(outsider), bytes.fromhex(outsider["sig"])))
        relabelled = by_name["unknown-key-id-relabelled"]            # a real signature by A under an id nobody pins
        self.assertTrue(us.verify_message(a, us.canonical_bytes(relabelled), bytes.fromhex(relabelled["sig"])))
        claim = by_name["bad-signature-unpinned-signer-claims-a"]    # C's signature under A's id
        self.assertFalse(us.verify_message(a, us.canonical_bytes(claim), bytes.fromhex(claim["sig"])))

    def test_the_high_s_twin_is_accepted_by_design(self):
        m = next(v["manifest"] for v in VEC["vectors"] if v["name"] == "ok-high-s-twin")
        s = int(m["sig"][64:], 16)
        self.assertGreater(s, us.N // 2)
        self.assertEqual(us.classify(m, PINNED)[0], "ok")

    def test_the_two_real_keys_are_not_the_test_keys(self):
        pinned = us.parse_keys_header(KEYS_HEADER)
        for k in VEC["keys"]:
            self.assertNotIn(bytes.fromhex(k["public_key_hex"]), pinned.keys, "a test key must never be pinned in the firmware")


# ---- 2. the pure-Python verifier, DER and hex -------------------------------------------------------------------------------------

class VerifierTests(unittest.TestCase):
    def setUp(self):
        v = next(v for v in VEC["vectors"] if v["name"] == "ok-basic")
        self.m = v["manifest"]
        self.pub = bytes.fromhex(VEC["keys"][0]["public_key_hex"])
        self.digest = hashlib.sha256(us.canonical_bytes(self.m)).digest()
        self.sig = bytes.fromhex(self.m["sig"])

    def test_the_good_signature(self):
        self.assertTrue(us.p256_verify(self.pub, self.digest, self.sig))

    def test_range_checks(self):
        r, s = self.sig[:32], self.sig[32:]
        zero, n = bytes(32), us.N.to_bytes(32, "big")
        for label, sig in (("r=0", zero + s), ("s=0", r + zero), ("r=n", n + s), ("s=n", r + n), ("r=n+1", (us.N + 1).to_bytes(32, "big") + s),
                           ("r and s max", b"\xff" * 64)):
            self.assertFalse(us.p256_verify(self.pub, self.digest, sig), label)

    def test_wrong_shapes_are_false_not_errors(self):
        self.assertFalse(us.p256_verify(self.pub, self.digest, self.sig[:-1]))
        self.assertFalse(us.p256_verify(self.pub, self.digest, self.sig + b"\x00"))
        self.assertFalse(us.p256_verify(self.pub, self.digest[:-1], self.sig))
        self.assertFalse(us.p256_verify(self.pub[:-1], self.digest, self.sig))
        self.assertFalse(us.p256_verify(b"\x04" + bytes(64), self.digest, self.sig))     # (0, 0) is not on the curve
        self.assertFalse(us.p256_verify(b"\x02" + self.pub[1:33] + bytes(32), self.digest, self.sig))
        self.assertFalse(us.p256_verify(self.pub[:-1] + bytes([self.pub[-1] ^ 1]), self.digest, self.sig))   # off the curve
        self.assertFalse(us.p256_verify("not bytes", self.digest, self.sig))

    def test_any_flipped_bit_breaks_it(self):
        for i in (0, 31, 32, 63):
            bad = bytearray(self.sig)
            bad[i] ^= 0x01
            self.assertFalse(us.p256_verify(self.pub, self.digest, bytes(bad)), i)
        bad = bytearray(self.digest)
        bad[5] ^= 0x80
        self.assertFalse(us.p256_verify(self.pub, bytes(bad), self.sig))
        other = bytes.fromhex(VEC["keys"][1]["public_key_hex"])
        self.assertFalse(us.p256_verify(other, self.digest, self.sig))

    def test_generator_arithmetic(self):
        self.assertTrue(us.on_curve(us.G))
        self.assertIsNone(us._add(us.G, (us.G[0], us.P - us.G[1])))        # P + (-P) is the point at infinity
        twice = us._add(us.G, us.G)
        self.assertTrue(us.on_curve(twice))
        self.assertEqual(twice, (0x7cf27b188d034f7e8a52380304b51ac3c08969e277f21b35a60b48fc47669978,
                                 0x07775510db8ed040293d9ac69f7430dbba7dade63ce982299e04b79d227873d1))   # 2G, a published value

    def test_der_round_trip_and_edges(self):
        self.assertEqual(us.der_to_raw(us.raw_to_der(self.sig)), self.sig)
        for r, s in ((1, 1), (1, us.N - 1), (1 << 255, 1 << 255), (0x80, 0x7F), (0xFF, 0x100), (1 << 248, 2)):
            raw = r.to_bytes(32, "big") + s.to_bytes(32, "big")
            self.assertEqual(us.der_to_raw(us.raw_to_der(raw)), raw, (r, s))
        self.assertEqual(us.raw_to_der(bytes(31) + b"\x01" + bytes(31) + b"\x01"), bytes.fromhex("3006020101020101"))
        self.assertEqual(us.raw_to_der(b"\x80" + bytes(31) + bytes(31) + b"\x01"), bytes.fromhex("3026022100" + "80" + "00" * 31 + "020101"))

    def test_der_parser_is_strict(self):
        good = us.raw_to_der(self.sig)
        for label, der in (("empty", b""), ("not a sequence", b"\x31" + good[1:]), ("trailing byte", good + b"\x00"),
                           ("truncated", good[:-1]), ("wrong total length", good[:1] + bytes([good[1] + 1]) + good[2:]),
                           ("negative r", bytes.fromhex("3006020180020101")), ("non-minimal r", bytes.fromhex("300702020001020101")),
                           ("empty integer", bytes.fromhex("3005020002010102")[:4]), ("not an integer", bytes.fromhex("3006030101020101")),
                           ("long length form for a short one", bytes.fromhex("30810602010102 0101".replace(" ", ""))),
                           ("33 byte r", bytes.fromhex("3026022101") + bytes(32) + bytes.fromhex("020101"))):
            with self.assertRaises(ValueError, msg=label):
                us.der_to_raw(der)
        with self.assertRaises(ValueError):
            us.raw_to_der(bytes(63))

    def test_spki_wrapper(self):
        der = us.point_to_spki(self.pub)
        self.assertEqual(len(der), 91)
        self.assertEqual(us.spki_to_point(der), self.pub)
        for bad in (der[:-1], der + b"\x00", b"\x00" + der[1:], der[:26] + b"\x02" + der[27:],
                    der.replace(bytes.fromhex("2a8648ce3d030107"), bytes.fromhex("2b81040022")[:8].ljust(8, b"\x00"))):
            with self.assertRaises(us.Refusal):
                us.spki_to_point(bad)
        pem = us.point_to_pem(self.pub)
        self.assertTrue(pem.startswith("-----BEGIN PUBLIC KEY-----\n") and pem.endswith("-----END PUBLIC KEY-----\n"))
        self.assertEqual(base64.b64decode("".join(pem.splitlines()[1:-1])), der)

    def test_hex_rules(self):
        self.assertTrue(us.is_lower_hex("0123456789abcdef"))
        self.assertTrue(us.is_lower_hex("abcd", 4))
        for bad in ("", "ABCD", "abcg", "abc ", " abc", "abc\n", "ａｂ", None, 12, b"ab"):
            self.assertFalse(us.is_lower_hex(bad), repr(bad))
        self.assertFalse(us.is_lower_hex("abcd", 3))
        self.assertEqual(us.from_lower_hex("0a0b", 2), b"\x0a\x0b")
        for bad in ("0A0B", "0a0", "0a0b0c", "zz00"):
            with self.assertRaises(ValueError):
                us.from_lower_hex(bad, 2)


# ---- 3. the shared module: rules, canonical bytes, classification, the keys header --------------------------------------------------

BASE = {"version": "0.11.0", "file": "ledmatrix-0.11.0.bin", "size": 1412537,
        "sha256": "2ba21a04f86b6da51b269a6eca124355345286cb4efff64048bda4518b05d2fa", "notes": "Test release."}


class RuleTests(unittest.TestCase):
    def problem(self, **change):
        m = dict(BASE)
        for k, v in change.items():
            if v is KeyError:
                m.pop(k, None)
            else:
                m[k] = v
        return us.manifest_problem(m)

    def test_the_base_manifest_is_fine(self):
        self.assertIsNone(us.manifest_problem(BASE))
        self.assertIsNone(us.manifest_problem(BASE, release=True))

    def test_versions(self):
        for v in ("0.0.0", "1.2.3", "9999.9999.9999", "0001.0.0"):
            self.assertIsNone(self.problem(version=v, file=f"ledmatrix-{v}.bin"), v)
        for v in ("1.2", "1.2.3.4", "1.2.x", "v1.2.3", "1.2.3-rc1", "12345.0.0", "", " 1.2.3", "1.2.3\n", "١.2.3", None, 123):
            self.assertIsNotNone(self.problem(version=v, file=f"ledmatrix-{v}.bin"), repr(v))
        self.assertIsNone(us.manifest_problem(dict(BASE, version="01.2.3", file="ledmatrix-01.2.3.bin")))
        self.assertIn("leading zero", us.manifest_problem(dict(BASE, version="01.2.3", file="ledmatrix-01.2.3.bin"), release=True))

    def test_file_names(self):
        for f in ("ledmatrix-0.11.1.bin", "ledmatrix-0.11.0.BIN", "../ledmatrix-0.11.0.bin", "ledmatrix-0.11.0.bin ", "", None, "firmware.bin"):
            self.assertIsNotNone(self.problem(file=f), repr(f))
        self.assertIsNotNone(self.problem(file=KeyError))

    def test_sizes(self):
        for n in (102401, 1412537, 2097152):
            self.assertIsNone(self.problem(size=n), n)
        for n in (102400, 0, -1, 2097153, 1412537.0, "1412537", True, None):
            self.assertIsNotNone(self.problem(size=n), repr(n))

    def test_sha256(self):
        for s in ("", "A" * 64, "a" * 63, "a" * 65, "g" * 64, None, 5):
            self.assertIsNotNone(self.problem(sha256=s), repr(s))

    def test_notes(self):
        for n in (None, "", "n" * 200, "Atualização ✓ \U0001F680", "a\u0085b", "a b"):
            self.assertIsNone(self.problem(notes=n), repr(n))
        self.assertIsNone(self.problem(notes=KeyError))
        for n in ("n" * 201, "a\nb", "a\tb", "a\rb", "a\x00b", "a\x1fb", "a\x7fb", 5, ["a"], "a\ud800b"):
            self.assertIsNotNone(self.problem(notes=n), repr(n))

    def test_not_an_object(self):
        for m in (None, [], "text", 5):
            self.assertIsNotNone(us.manifest_problem(m))

    def test_canonical_text(self):
        got = us.canonical_bytes(BASE)
        self.assertEqual(got.decode("utf-8"), "ledmatrix-update-v1\nversion=0.11.0\nfile=ledmatrix-0.11.0.bin\nsize=1412537\n"
                         "sha256=2ba21a04f86b6da51b269a6eca124355345286cb4efff64048bda4518b05d2fa\nnotes=Test release.")
        for notes in (None, "", KeyError):
            m = dict(BASE)
            if notes is KeyError:
                m.pop("notes")
            else:
                m["notes"] = notes
            self.assertTrue(us.canonical_bytes(m).endswith(b"\nnotes="), repr(notes))
        # keyid and sig are not part of it
        self.assertEqual(us.canonical_bytes(dict(BASE, keyid="f864ea3b", sig="0" * 128)), got)
        # the longest possible text is below the contract's bound
        longest = dict(BASE, version="9999.9999.9999", file="ledmatrix-9999.9999.9999.bin", size=2097152, notes="\U0001F680" * 200)
        self.assertLess(len(us.canonical_bytes(longest)), us.CANONICAL_MAX)

    def test_nothing_odd_is_ever_turned_into_bytes_to_sign(self):
        for change in ({"notes": "a\nb"}, {"version": "1.2.3\nx"}, {"size": "1412537"}, {"sha256": "A" * 64}, {"file": "x"}):
            with self.assertRaises(ValueError, msg=str(change)):
                us.canonical_bytes(dict(BASE, **change))

    def test_classification_order(self):
        pub = bytes.fromhex(VEC["keys"][0]["public_key_hex"])
        kid = us.key_id(pub)
        sig = "0" * 128
        yes = lambda p, d, s: True       # a verifier that says yes to everything: only the order of the other rules is shown
        no = lambda p, d, s: False
        cases = [
            (dict(BASE, notes="a\nb", keyid=kid, sig=sig), "manifest_invalid"),    # the manifest rules come first
            (dict(BASE), "unsigned"),
            (dict(BASE, keyid=None, sig=None), "unsigned"),
            (dict(BASE, keyid=kid), "malformed"),
            (dict(BASE, sig=sig), "malformed"),
            (dict(BASE, keyid=kid, sig=None), "malformed"),
            (dict(BASE, keyid=kid.upper(), sig=sig), "malformed"),
            (dict(BASE, keyid=kid, sig=5), "malformed"),
            (dict(BASE, keyid="00000000", sig=sig), "unknown_key"),                # no crypto has run: the verifier is never asked
            (dict(BASE, keyid=kid, sig=sig), "ok"),
        ]
        for m, want in cases:
            self.assertEqual(us.classify(m, [pub], yes)[0], want, m)
        self.assertEqual(us.classify(dict(BASE, keyid=kid, sig=sig), [pub], no)[0], "bad_signature")
        asked = []
        us.classify(dict(BASE, keyid="00000000", sig=sig), [pub], lambda *a: asked.append(a) or True)
        self.assertEqual(asked, [])

    def test_load_manifest_is_strict(self):
        with tempfile.TemporaryDirectory() as tmp:
            def put(data):
                path = os.path.join(tmp, "manifest.json")
                with open(path, "wb") as f:
                    f.write(data if isinstance(data, bytes) else data.encode("utf-8"))
                return path

            good = json.dumps(BASE)
            self.assertEqual(us.load_manifest(put(good)), BASE)
            for label, data in (("a BOM", b"\xef\xbb\xbf" + good.encode()), ("not JSON", "<html>"), ("an array", "[]"), ("null", "null"),
                                ("a repeated key", '{"version":"1.2.3","version":"1.2.4"}'), ("NaN", '{"size":NaN}'),
                                ("not UTF-8", b'{"notes":"\xff"}'), ("an extra field", good[:-1] + ',"token":"x"}'),
                                ("trailing text", good + "x")):
                with self.assertRaises(us.Refusal, msg=label):
                    us.load_manifest(put(data))
            with self.assertRaises(us.Refusal):
                us.load_manifest(os.path.join(tmp, "missing.json"))

    def test_manifest_text_is_compact_and_ordered(self):
        m = dict(BASE, keyid="f864ea3b", sig="ab" * 64)
        text = us.manifest_text({k: m[k] for k in ("sig", "keyid", "notes", "sha256", "size", "file", "version")})
        self.assertEqual(text.decode("utf-8"), json.dumps(m, separators=(",", ":")) + "\n")
        self.assertEqual(list(json.loads(text)), ["version", "file", "size", "sha256", "notes", "keyid", "sig"])
        self.assertNotIn(b"keyid", us.manifest_text(BASE))
        self.assertTrue(us.manifest_text({k: v for k, v in BASE.items() if k != "notes"}).endswith(b'"notes":""}\n'))
        self.assertIn("✓".encode("utf-8"), us.manifest_text(dict(BASE, notes="✓")))          # UTF-8, not \u escapes


class KeysHeaderTests(unittest.TestCase):
    A = VEC["keys"][0]["public_key_hex"]
    B = VEC["keys"][1]["public_key_hex"]

    def header(self, count=2, a=None, b=None, extra="", flag="#ifndef UPDATE_REQUIRE_SIGNATURE\n#define UPDATE_REQUIRE_SIGNATURE 1\n#endif\n"):
        a = self.A if a is None else a
        b = self.B if b is None else b
        keys = f'#define UPDATE_KEY_0 "{a}"\n' + (f'#define UPDATE_KEY_1 "{b}"\n' if b else "")
        return f"#pragma once\n{flag}#define UPDATE_KEY_COUNT {count}\n{keys}{extra}"

    def test_the_real_header(self):
        pinned = us.parse_keys_header(KEYS_HEADER)
        self.assertEqual(len(pinned.keys), 2)
        self.assertEqual(pinned.ids, ["eb6a67ad", "dfcb5742"])          # as the comments of the header say
        self.assertEqual(pinned.require_default, 1)

    def test_a_good_header(self):
        p = us.parse_keys_header_text(self.header())
        self.assertEqual(p.keys, [bytes.fromhex(self.A), bytes.fromhex(self.B)])
        self.assertEqual(p.ids, ["f864ea3b", "db10c343"])
        p = us.parse_keys_header_text(self.header(count=1, b=""))
        self.assertEqual(len(p.keys), 1)

    def test_comments_spacing_and_unknown_lines(self):
        text = ("// UPDATE_KEY_5 is only mentioned here\n/* #define UPDATE_KEY_COUNT 7 */\n#pragma once\n" + self.header(flag="").replace(
            "#define UPDATE_KEY_COUNT 2", "#  define\tUPDATE_KEY_COUNT   2   // two keys") + 'static_assert(true, "x");\n#include <stdint.h>\n')
        self.assertEqual(len(us.parse_keys_header_text(text).keys), 2)
        text = self.header(flag="").replace('#define UPDATE_KEY_1 "', '// #define UPDATE_KEY_9 "\n#define UPDATE_KEY_1 "')
        self.assertEqual(len(us.parse_keys_header_text(text).keys), 2)

    def test_refused(self):
        wrong_point = self.A[:-1] + ("0" if self.A[-1] != "0" else "1")
        cases = {
            "count 3": self.header(count=3), "count 0": self.header(count=0), "count 1 with two keys": self.header(count=1),
            "count 2 with one key": self.header(count=2, b=""), "no count": self.header().replace("#define UPDATE_KEY_COUNT 2\n", ""),
            "two counts": self.header(extra="#define UPDATE_KEY_COUNT 2\n"),
            "a key twice": self.header(extra=f'#define UPDATE_KEY_0 "{self.A}"\n'),
            "the same key twice": self.header(b=self.A), "a third key": self.header(extra=f'#define UPDATE_KEY_2 "{self.B}"\n'),
            "key in upper case": self.header(a=self.A.upper()), "key too short": self.header(a=self.A[:-2]), "key too long": self.header(a=self.A + "00"),
            "key not starting 04": self.header(a="02" + self.A[2:]), "key not hex": self.header(a="zz" + self.A[2:]),
            "key off the curve": self.header(a=wrong_point), "key all zero": self.header(a="04" + "0" * 128),
            "compressed key": self.header(a="02" + self.A[2:66]),
            "an #if around": self.header(extra="").replace('#define UPDATE_KEY_1', '#if 0\n#define UPDATE_KEY_1') + "#endif\n",
            "an #else": self.header(extra="#else\n"), "an #ifdef": "#ifdef X\n" + self.header(), "an #undef": self.header(extra="#undef UPDATE_KEY_1\n"),
            "an #elif": self.header(extra="#elif 1\n"),
            "key inside the flag block": self.header(flag=f'#ifndef UPDATE_REQUIRE_SIGNATURE\n#define UPDATE_KEY_0 "{self.A}"\n#endif\n'),
            "unclosed flag block": self.header(flag="#ifndef UPDATE_REQUIRE_SIGNATURE\n#define UPDATE_REQUIRE_SIGNATURE 1\n"),
            "stray #endif": self.header(extra="#endif\n"),
            "flag value 2": self.header(flag="#ifndef UPDATE_REQUIRE_SIGNATURE\n#define UPDATE_REQUIRE_SIGNATURE 2\n#endif\n"),
            "flag twice": self.header(extra="#define UPDATE_REQUIRE_SIGNATURE 0\n"),
            "flag outside its block": self.header(flag="#define UPDATE_REQUIRE_SIGNATURE 1\n"),
            "a line continuation": self.header().replace('"\n#define UPDATE_KEY_1', '" \\\n#define UPDATE_KEY_1'),
            "an odd line about a key": self.header(extra='#define UPDATE_KEY_0_ALT "x"\n'),
            "key with trailing code": self.header().replace(f'"{self.A}"', f'"{self.A}" "00"'),
            "an unclosed block comment": self.header(extra="/* oops\n"),
        }
        for label, text in cases.items():
            with self.assertRaises(us.Refusal, msg=label):
                us.parse_keys_header_text(text)

    def test_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "update_keys.h")
            with open(path, "w", encoding="utf-8") as f:
                f.write(self.header())
            self.assertEqual(len(us.parse_keys_header(path).keys), 2)
            for label, data in (("not utf-8", b"\xff\xfe"), ("too big", b"//" + b"x" * 70000)):
                with open(path, "wb") as f:
                    f.write(data)
                with self.assertRaises(us.Refusal, msg=label):
                    us.parse_keys_header(path)
            with self.assertRaises(us.Refusal):
                us.parse_keys_header(os.path.join(tmp, "missing.h"))


# ---- 4. key generation, signing, checking: with openssl ---------------------------------------------------------------------------

_KEYDIR = None
KEYS = {}       # name -> {"pem": text, "secret": one base64 line, "pub": bytes, "id": text, "pem_path": path}
HEADER2 = HEADER1 = None


def setUpModule():
    global _KEYDIR, HEADER2, HEADER1
    if not OPENSSL:
        return
    _KEYDIR = tempfile.TemporaryDirectory()
    folder = os.path.join(_KEYDIR.name, "keys")
    for names in (["A", "B"], ["C"], ["D"]):
        for name, kid, pub_hex, paths in gk.generate(folder, names):
            with open(paths["pem"], "r") as f:
                pem = f.read()
            with open(paths["secret"], "r") as f:
                secret = f.read()
            KEYS[name] = {"pem": pem, "secret": secret, "pub": bytes.fromhex(pub_hex), "id": kid, "pem_path": paths["pem"]}
    HEADER2 = write_header(os.path.join(_KEYDIR.name, "update_keys.h"), [KEYS["A"]["pub"], KEYS["B"]["pub"]])
    HEADER1 = write_header(os.path.join(_KEYDIR.name, "update_keys_one.h"), [KEYS["A"]["pub"]])


def tearDownModule():
    if _KEYDIR is not None:
        _KEYDIR.cleanup()


def write_header(path, pubs):
    lines = ["#pragma once", "#ifndef UPDATE_REQUIRE_SIGNATURE", "#define UPDATE_REQUIRE_SIGNATURE 1", "#endif", f"#define UPDATE_KEY_COUNT {len(pubs)}"]
    lines += [f'#define UPDATE_KEY_{i} "{p.hex()}"' for i, p in enumerate(pubs)]
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    return path


def make_image(version="0.11.0", marker=MARKER_REQUIRED, tags=None, total=120000):
    return tmm.make_image(total, tags=(version,) if tags is None else tags, extra=marker)


class Run:
    def __init__(self, rc, out, err):
        self.rc, self.out, self.err = rc, out, err


class ToolCase(unittest.TestCase):
    """A scratch folder per test, a runner folder that must be empty after every run, and a way to run the tools as the workflow does."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = self._tmp.name
        self.runner = os.path.join(self.tmp, "runner")
        os.mkdir(self.runner)

    def tool(self, script, *args, secret=None, cwd=None):
        env = {"PATH": os.environ.get("PATH", ""), "RUNNER_TEMP": self.runner}
        if secret is not None:
            env[SIGN_ENV] = secret
        r = subprocess.run([sys.executable, "-I", os.path.join(HERE, script), *args], capture_output=True, text=True, env=env, cwd=cwd or self.tmp)
        self.assertEqual(os.listdir(self.runner), [], "a temporary signing folder or key file was left behind")
        return Run(r.returncode, r.stdout, r.stderr)

    def sign(self, manifest, header=None, secret=None, *extra):
        return self.tool("sign_manifest.py", manifest, "--keys-header", header or HEADER2, *extra, secret=secret)

    def check(self, manifest, header=None, *extra):
        return self.tool("sign_manifest.py", "--check", manifest, "--keys-header", header or HEADER2, *extra)

    def release(self, name="rel", version="0.11.0", notes="Test release.", marker=MARKER_REQUIRED, tags=None, image=None):
        """A folder with an UNSIGNED manifest.json (made by make_manifest.py) and its image. Returns the manifest path."""
        folder = os.path.join(self.tmp, name)
        os.makedirs(folder, exist_ok=True)
        app = os.path.join(self.tmp, name + "-app.bin")
        with open(app, "wb") as f:
            f.write(image if image is not None else make_image(version, marker, tags))
        mm.make_manifest(app, version, notes, folder, force=True)
        return os.path.join(folder, "manifest.json")

    @staticmethod
    def read(path):
        with open(path, "rb") as f:
            return f.read()

    def jload(self, path):
        return json.loads(self.read(path))

    def jsave(self, path, m):
        with open(path, "wb") as f:
            f.write((json.dumps(m, separators=(",", ":"), ensure_ascii=False) + "\n").encode("utf-8"))

    def secrets_absent(self, run, key):
        """None of the secret forms, nor any signature or canonical text, is in what the tool printed."""
        pem_lines = [ln for ln in key["pem"].splitlines() if not ln.startswith("-----")]
        for text in (run.out, run.err):
            self.assertNotIn("PRIVATE KEY", text)
            self.assertNotIn(key["secret"].strip(), text)
            self.assertNotIn(key["pem"].strip(), text)
            for ln in pem_lines:
                self.assertNotIn(ln, text)
            self.assertNotIn("ledmatrix-update-v1", text)
            self.assertIsNone(re.search(r"[0-9a-f]{128}", text), "a 128 hex signature appears in the output")


@needs_openssl
class GenerateKeyTests(ToolCase):
    def gen(self, *args):
        return self.tool("gen_signing_key.py", *args)

    def test_two_keys_files_modes_and_output(self):
        out = os.path.join(self.tmp, "keys")
        r = self.gen("--out", out, "--names", "A", "B")
        self.assertEqual((r.rc, r.err), (0, ""), r.err)
        self.assertEqual(stat.S_IMODE(os.stat(out).st_mode), 0o700)
        want = {f"ledmatrix-update-{n}.{ext}" for n in "AB" for ext in ("pem", "pub.pem", "pub.hex", "github-secret.txt")}
        self.assertEqual(set(os.listdir(out)), want)
        for name in want:
            self.assertEqual(stat.S_IMODE(os.stat(os.path.join(out, name)).st_mode), 0o600, name)
        for i, n in enumerate("AB"):
            pem = self.read(os.path.join(out, f"ledmatrix-update-{n}.pem")).decode()
            secret = self.read(os.path.join(out, f"ledmatrix-update-{n}.github-secret.txt")).decode()
            pub_hex = self.read(os.path.join(out, f"ledmatrix-update-{n}.pub.hex")).decode()
            pub_pem = self.read(os.path.join(out, f"ledmatrix-update-{n}.pub.pem")).decode()
            self.assertTrue(pem.startswith("-----BEGIN EC PRIVATE KEY-----\n") or pem.startswith("-----BEGIN PRIVATE KEY-----\n"))
            self.assertEqual(secret.count("\n"), 1)                                    # one line
            self.assertEqual(base64.b64decode(secret.strip(), validate=True).decode(), pem)
            self.assertRegex(pub_hex, r"^04[0-9a-f]{128}\n$")
            pub = bytes.fromhex(pub_hex.strip())
            self.assertIsNotNone(us.parse_public_point(pub))
            self.assertEqual(base64.b64decode("".join(pub_pem.splitlines()[1:-1])), us.point_to_spki(pub))
            # the printed key id and define line are the key's, and nothing secret is printed
            self.assertIn(f"key {n}: key id {us.key_id(pub)}", r.out)
            self.assertIn(f'#define UPDATE_KEY_{i} "{pub.hex()}"', r.out)
            self.assertNotIn("PRIVATE", r.out)
            self.assertNotIn(secret.strip(), r.out)
            for ln in pem.splitlines()[1:-1]:
                self.assertNotIn(ln, r.out)
        self.assertIn("UPDATE_KEY_COUNT to 2", r.out)
        self.assertIn(f"gh secret set UPDATE_SIGNING_KEY < {os.path.join(out, 'ledmatrix-update-A.github-secret.txt')}", r.out)
        self.assertIn("Backup rule", r.out)
        # the printed lines make a header the strict parser accepts
        defines = re.findall(r'#define UPDATE_KEY_\d "04[0-9a-f]{128}"', r.out)
        header = write_header(os.path.join(self.tmp, "h.h"), [])
        with open(header, "w") as f:
            f.write("#define UPDATE_KEY_COUNT 2\n" + "\n".join(defines) + "\n")
        self.assertEqual(len(us.parse_keys_header(header).keys), 2)

    def test_default_names_and_one_name(self):
        r = self.gen("--out", os.path.join(self.tmp, "k1"))
        self.assertEqual(r.rc, 0, r.err)
        self.assertEqual(len(os.listdir(os.path.join(self.tmp, "k1"))), 8)
        r = self.gen("--out", os.path.join(self.tmp, "k2"), "--names", "Solo")
        self.assertEqual(r.rc, 0, r.err)
        self.assertIn("UPDATE_KEY_COUNT to 1", r.out)
        self.assertNotIn("UPDATE_KEY_1", r.out)

    def test_two_keys_differ(self):
        out = os.path.join(self.tmp, "keys")
        self.assertEqual(self.gen("--out", out).rc, 0)
        a = self.read(os.path.join(out, "ledmatrix-update-A.pub.hex"))
        b = self.read(os.path.join(out, "ledmatrix-update-B.pub.hex"))
        self.assertNotEqual(a, b)

    def test_never_overwrites(self):
        out = os.path.join(self.tmp, "keys")
        self.assertEqual(self.gen("--out", out, "--names", "A", "B").rc, 0)
        before = {n: self.read(os.path.join(out, n)) for n in os.listdir(out)}
        r = self.gen("--out", out, "--names", "A", "B")
        self.assertEqual((r.rc, r.out), (1, ""))
        self.assertIn("already exists", r.err)
        self.assertEqual({n: self.read(os.path.join(out, n)) for n in os.listdir(out)}, before)
        # one of four files missing: still refused, and the missing one is not made
        os.unlink(os.path.join(out, "ledmatrix-update-B.pub.hex"))
        r = self.gen("--out", out, "--names", "A", "B")
        self.assertEqual(r.rc, 1)
        self.assertFalse(os.path.exists(os.path.join(out, "ledmatrix-update-B.pub.hex")))
        # another name in the same folder is fine
        self.assertEqual(self.gen("--out", out, "--names", "C").rc, 0)

    def test_refuses_a_folder_inside_a_git_work_tree(self):
        work = os.path.join(self.tmp, "work")
        os.makedirs(os.path.join(work, ".git"))
        for out in (os.path.join(work, "keys"), os.path.join(work, "deep", "er", "keys"), work):
            r = self.gen("--out", out)
            self.assertEqual((r.rc, r.out), (1, ""), out)
            self.assertIn("git work tree", r.err)
        self.assertEqual(os.listdir(work), [".git"])
        with open(os.path.join(self.tmp, "wt-file"), "w"):
            pass
        os.makedirs(os.path.join(self.tmp, "wt2"))
        with open(os.path.join(self.tmp, "wt2", ".git"), "w") as f:                  # a linked work tree has a .git FILE
            f.write("gitdir: elsewhere\n")
        self.assertEqual(self.gen("--out", os.path.join(self.tmp, "wt2", "keys")).rc, 1)

    def test_refuses_an_open_or_odd_folder(self):
        open_dir = os.path.join(self.tmp, "open")
        os.mkdir(open_dir, 0o755)
        os.chmod(open_dir, 0o755)
        r = self.gen("--out", open_dir)
        self.assertEqual(r.rc, 1)
        self.assertIn("chmod 700", r.err)
        self.assertEqual(os.listdir(open_dir), [])
        a_file = os.path.join(self.tmp, "afile")
        with open(a_file, "w"):
            pass
        self.assertEqual(self.gen("--out", a_file).rc, 1)
        closed = os.path.join(self.tmp, "closed")
        os.mkdir(closed, 0o700)
        self.assertEqual(self.gen("--out", closed).rc, 0)          # an existing folder that is yours and closed is fine

    def test_names(self):
        out = os.path.join(self.tmp, "keys")
        for names in (["a/b"], [".hidden"], ["x" * 33], ["has space"], [""], ["A", "a"], ["A", "A"], ["A", "B", "C"], ["é"]):
            r = self.gen("--out", out, "--names", *names)
            self.assertEqual((r.rc, r.out), (1, ""), names)
            self.assertFalse(os.path.exists(out), names)
        self.assertEqual(self.gen("--out", out, "--names", "x" * 32, "My_key-2").rc, 0)

    def test_a_failure_half_way_leaves_nothing(self):
        out = os.path.join(self.tmp, "keys")
        real = us.write_private
        calls = []

        def fail_on_fifth(path, data):
            calls.append(path)
            if len(calls) == 5:
                raise OSError(28, "No space left on device")
            return real(path, data)

        with unittest.mock.patch.object(gk.us, "write_private", fail_on_fifth):
            with self.assertRaises(OSError):
                gk.generate(out, ["A", "B"], None)
        self.assertEqual([n for n in os.listdir(out)], [])
        # and the private work folder of the self-test is gone too (it is the system temporary folder here, nothing to list):
        self.assertEqual(os.listdir(self.runner), [])

    def test_the_self_test_catches_a_broken_openssl(self):
        out = os.path.join(self.tmp, "keys")
        with unittest.mock.patch.object(gk.us, "sign_with_key_file", lambda *a, **k: bytes(64)):
            with self.assertRaises(gk.us.Refusal) as cm:
                gk.generate(out, ["A"])
        self.assertIn("self-test", str(cm.exception))
        self.assertEqual(os.listdir(out), [])


@needs_openssl
class SignTests(ToolCase):
    def assert_signed(self, path, key_name, header_pubs=None):
        m = self.jload(path)
        self.assertEqual(list(m), ["version", "file", "size", "sha256", "notes", "keyid", "sig"])
        self.assertEqual(m["keyid"], KEYS[key_name]["id"])
        self.assertRegex(m["sig"], r"^[0-9a-f]{128}$")
        pinned = header_pubs or [KEYS["A"]["pub"], KEYS["B"]["pub"]]
        self.assertEqual(us.classify(m, pinned)[0], "ok")
        # and openssl itself says yes, with the pinned public key
        with us.private_workdir() as wd:
            self.assertTrue(us.openssl_verify(KEYS[key_name]["pub"], us.canonical_bytes(m), bytes.fromhex(m["sig"]), wd))
        return m

    def test_sign_with_the_secret_as_base64_pem_and_key_file(self):
        for label, kw in (("base64 secret", {"secret": KEYS["A"]["secret"]}), ("PEM text", {"secret": KEYS["A"]["pem"]}),
                          ("one line PEM with spaces", {"secret": " " + KEYS["A"]["secret"].strip() + " \n\n"}),
                          ("key file", {"keyfile": KEYS["A"]["pem_path"]}),
                          ("key file with the one-line base64 secret", {"keyfile": KEYS["A"]["pem_path"].replace(".pem", ".github-secret.txt")})):
            path = self.release(name="r-" + label.replace(" ", "-"))
            image = self.read(os.path.join(os.path.dirname(path), "ledmatrix-0.11.0.bin"))
            args = ["--key-file", kw["keyfile"]] if "keyfile" in kw else []
            run = self.sign(path, None, kw.get("secret"), *args)
            self.assertEqual((run.rc, run.err), (0, ""), (label, run.err))
            self.assertEqual(run.out, f"signed version=0.11.0 keyid={KEYS['A']['id']}\n", label)
            self.assert_signed(path, "A")
            self.assertEqual(self.read(os.path.join(os.path.dirname(path), "ledmatrix-0.11.0.bin")), image, "the image is left alone")
            self.secrets_absent(run, KEYS["A"])

    def test_the_spare_key_signs_too(self):
        path = self.release()
        run = self.sign(path, None, KEYS["B"]["secret"])
        self.assertEqual(run.rc, 0, run.err)
        self.assert_signed(path, "B")

    def test_one_pinned_key_only(self):
        path = self.release()
        self.assertEqual(self.sign(path, HEADER1, KEYS["A"]["secret"]).rc, 0)
        self.assert_signed(path, "A", [KEYS["A"]["pub"]])
        other = self.release(name="b")
        r = self.sign(other, HEADER1, KEYS["B"]["secret"])
        self.assertEqual(r.rc, 1)
        self.assertIn("not one of the keys pinned", r.err)

    def test_signatures_are_random_and_both_verify(self):
        one, two = self.release(name="one"), self.release(name="two")
        self.assertEqual(self.sign(one, None, KEYS["A"]["secret"]).rc, 0)
        self.assertEqual(self.sign(two, None, KEYS["A"]["secret"]).rc, 0)
        a, b = self.jload(one), self.jload(two)
        self.assertEqual({k: a[k] for k in a if k != "sig"}, {k: b[k] for k in b if k != "sig"})
        self.assertNotEqual(a["sig"], b["sig"], "ECDSA with a random nonce: two signatures of the same bytes differ")
        self.assert_signed(one, "A")
        self.assert_signed(two, "A")

    def test_notes_with_unicode_and_the_empty_text(self):
        for notes in ("", "Atualização: ecrã e fuso horário (✓) \U0001F680", "<b>&amp;</b> \"q\" \\", "x" * 200):
            path = self.release(name="n" + str(abs(hash(notes)) % 10 ** 6), notes=notes)
            run = self.sign(path, None, KEYS["A"]["secret"])
            self.assertEqual(run.rc, 0, (notes, run.err))
            self.assertEqual(self.assert_signed(path, "A")["notes"], mm.fold_notes(notes))

    def test_a_manifest_without_a_notes_field_is_signed_over_empty_notes(self):
        path = self.release()
        m = self.jload(path)
        del m["notes"]
        self.jsave(path, m)
        self.assertEqual(self.sign(path, None, KEYS["A"]["secret"]).rc, 0)
        signed = self.assert_signed(path, "A")
        self.assertEqual(signed["notes"], "")

    def test_the_secret_never_reaches_a_child_process_or_the_terminal(self):
        path = self.release()
        key = KEYS["A"]
        pinned = us.parse_keys_header(HEADER2)
        real = us.subprocess.run
        seen = []

        def spy(cmd, **kw):
            entry = {"cmd": list(cmd), "env": dict(kw.get("env") or {})}
            if "-sign" in cmd:
                keyfile = cmd[cmd.index("-sign") + 1]
                entry["key_mode"] = stat.S_IMODE(os.stat(keyfile).st_mode)
                entry["dir_mode"] = stat.S_IMODE(os.stat(os.path.dirname(keyfile)).st_mode)
                entry["key_ok"] = self.read(keyfile).decode() == key["pem"]
                entry["dir"] = os.path.dirname(keyfile)
            seen.append(entry)
            return real(cmd, **kw)

        with unittest.mock.patch.object(us.subprocess, "run", spy), unittest.mock.patch.dict(os.environ, {"RUNNER_TEMP": self.runner}):
            version, kid = sm.sign(path, pinned, key["pem"])
        self.assertEqual((version, kid), ("0.11.0", key["id"]))
        self.assertGreaterEqual(len(seen), 3)                      # ec (public key), dgst -sign, dgst -verify
        signs = [e for e in seen if "key_mode" in e]
        self.assertEqual(len(signs), 1)
        self.assertEqual((signs[0]["key_mode"], signs[0]["dir_mode"], signs[0]["key_ok"]), (0o600, 0o700, True))
        self.assertEqual(os.path.dirname(signs[0]["dir"]), self.runner, "the work folder is made under RUNNER_TEMP")
        body = [ln for ln in key["pem"].splitlines() if not ln.startswith("-----")]
        for e in seen:
            self.assertNotIn(SIGN_ENV, e["env"])
            self.assertLessEqual(set(e["env"]), {"PATH", "LC_ALL"}, "a child gets a clean environment")
            joined = "\0".join(e["cmd"])
            for ln in body:
                self.assertNotIn(ln, joined, "the key is never an argument")
            self.assertNotIn(key["secret"].strip(), joined)
        self.assertEqual(os.listdir(self.runner), [])

    def test_the_environment_variable_is_dropped_after_it_is_read(self):
        with unittest.mock.patch.dict(os.environ, {SIGN_ENV: KEYS["A"]["secret"]}):
            pem = sm.read_key(None)
            self.assertNotIn(SIGN_ENV, os.environ)
        self.assertEqual(pem.strip(), KEYS["A"]["pem"].strip())

    def test_nothing_is_left_when_signing_fails(self):
        path = self.release()
        before = self.read(path)
        run = self.sign(path, None, KEYS["C"]["secret"])                      # an outsider's key (the helper checks the runner folder)
        self.assertEqual(run.rc, 1)
        self.assertEqual(self.read(path), before)
        self.assertEqual(sorted(os.listdir(os.path.dirname(path))), ["ledmatrix-0.11.0.bin", "manifest.json"])

    # ---- refusals: one plain sentence, exit 1, nothing written, nothing printed on stdout, no secret on stderr ----

    def refused(self, path, secret, header=None, *extra, want=None, key=None):
        before = {n: self.read(os.path.join(os.path.dirname(path), n)) for n in os.listdir(os.path.dirname(path))}
        run = self.sign(path, header, secret, *extra)
        self.assertEqual(run.rc, 1, run.err)
        self.assertEqual(run.out, "")
        self.assertEqual(run.err.count("\n"), 1, "one sentence")
        self.assertTrue(run.err.startswith("sign_manifest: "))
        if want:
            self.assertIn(want, run.err)
        self.secrets_absent(run, key or KEYS["A"])
        after = {n: self.read(os.path.join(os.path.dirname(path), n)) for n in os.listdir(os.path.dirname(path))}
        self.assertEqual(before, after, "nothing may be written")
        return run

    def test_a_key_that_is_not_pinned(self):
        run = self.refused(self.release(), KEYS["C"]["secret"], want="not one of the keys pinned", key=KEYS["C"])
        self.assertIn(KEYS["C"]["id"], run.err)
        self.assertIn(KEYS["A"]["id"], run.err)
        self.refused(self.release(name="d"), KEYS["D"]["secret"], want="not one of the keys pinned", key=KEYS["D"])

    def test_no_key_without_the_transition_flag(self):
        path = self.release()
        for secret in (None, "", "   \n"):
            self.refused(path, secret, want="UPDATE_SIGNING_KEY is empty or not set")

    def test_bad_keys(self):
        path = self.release()
        p384 = us.generate_key_pem(curve="secp384r1")
        cases = {
            "not base64 nor pem": ("this is not a key!", "neither PEM text nor one line of base64"),
            "base64 of something else": (base64.b64encode(b"hello world").decode(), "neither PEM text nor one line of base64"),
            "an encrypted key": ("-----BEGIN ENCRYPTED PRIVATE KEY-----\nAAAA\n-----END ENCRYPTED PRIVATE KEY-----\n", "protected by a password"),
            "an old style encrypted key": ("-----BEGIN EC PRIVATE KEY-----\nProc-Type: 4,ENCRYPTED\nDEK-Info: AES-256-CBC,00\n\nAAAA\n"
                                           "-----END EC PRIVATE KEY-----\n", "protected by a password"),
            "a public key": (us.point_to_pem(KEYS["A"]["pub"]), "not the PEM of an EC private key"),
            "a certificate": ("-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n", "not the PEM of an EC private key"),
            "a key of the wrong curve": (p384, "P-256"),
            "PEM that is not a key": ("-----BEGIN EC PRIVATE KEY-----\nAAAAAAAA\n-----END EC PRIVATE KEY-----\n", "could not read the key"),
            "text before the PEM": ("hello\n" + KEYS["A"]["pem"], "not the PEM of an EC private key"),
            "way too long": ("A" * 20000, "far too long"),
        }
        for label, (secret, want) in cases.items():
            run = self.refused(path, secret, want=want)
            self.assertNotIn(secret.strip()[:40], run.err, label) if len(secret.strip()) > 40 else None

    def test_the_key_given_twice(self):
        self.refused(self.release(), KEYS["A"]["secret"], None, "--key-file", KEYS["A"]["pem_path"], want="one way only")

    def test_a_key_file_open_to_others_or_odd(self):
        path = self.release()
        loose = os.path.join(self.tmp, "loose.pem")
        with open(loose, "w") as f:
            f.write(KEYS["A"]["pem"])
        os.chmod(loose, 0o644)
        self.refused(path, None, None, "--key-file", loose, want="chmod 600")
        os.chmod(loose, 0o600)
        run = self.sign(path, None, None, "--key-file", loose)
        self.assertEqual(run.rc, 0, run.err)
        self.refused(self.release(name="m"), None, None, "--key-file", os.path.join(self.tmp, "missing.pem"), want="cannot read the key file")
        self.refused(self.release(name="x"), None, None, "--key-file", self.tmp, want="not a plain file")

    def test_a_manifest_that_breaks_the_rules(self):
        edits = {
            "a newline in the notes": ({"notes": "a\nb"}, "control character"),
            "a leading zero in the version": ({"version": "01.1.0", "file": "ledmatrix-01.1.0.bin"}, "leading zero"),
            "a file that is not the version's": ({"file": "ledmatrix-0.11.9.bin"}, "file name"),
            "a size that is a string": ({"size": "200016"}, "size"),
            "a short sha256": ({"sha256": "abc"}, "sha256"),
            "notes of 201 characters": ({"notes": "n" * 201}, "200 characters"),
            "an unknown field": ({"extra": 1}, "not part of the contract"),
        }
        for label, (change, want) in edits.items():
            path = self.release(name="e" + str(abs(hash(label)) % 10 ** 6))
            m = self.jload(path)
            m.update(change)
            self.jsave(path, m)
            self.refused(path, KEYS["A"]["secret"], want=want)
        broken = self.release(name="broken")
        with open(broken, "wb") as f:
            f.write(b"<html>")
        self.refused(broken, KEYS["A"]["secret"], want="not valid JSON")
        emptydir = os.path.join(self.tmp, "emptydir")
        os.mkdir(emptydir)
        self.refused(os.path.join(emptydir, "manifest.json"), KEYS["A"]["secret"], want="cannot read the manifest")

    def test_the_image_must_match_the_manifest(self):
        path = self.release()
        image = os.path.join(os.path.dirname(path), "ledmatrix-0.11.0.bin")
        data = self.read(image)
        with open(image, "wb") as f:
            f.write(data + b"\x00")
        self.refused(path, KEYS["A"]["secret"], want="bytes but the manifest says")
        flipped = bytearray(data)
        flipped[5000] ^= 0x01
        with open(image, "wb") as f:
            f.write(bytes(flipped))
        self.refused(path, KEYS["A"]["secret"], want="SHA-256")
        os.unlink(image)
        self.refused(path, KEYS["A"]["secret"], want="not next to the manifest")

    def test_the_image_must_carry_the_signing_marker(self):
        cases = {
            "no marker": (b"", "no signing marker"),
            "a development build": (MARKER_OFF, "enforcement off"),
            "two markers": (MARKER_REQUIRED + b"\x00" * 16 + MARKER_REQUIRED, "2 signing markers"),
            "another word": (b"\x01LEDMATRIX-FW-SIGNING\x02maybe\x03", "'maybe'"),
            "the prefix alone": (b"\x01LEDMATRIX-FW-SIGNING\x02" + b"\x00more code", "no signing marker"),
            "the marker without its end byte": (b"\x01LEDMATRIX-FW-SIGNING\x02required\x00", "no signing marker"),
            "upper case": (b"\x01LEDMATRIX-FW-SIGNING\x02REQUIRED\x03", "no signing marker"),
            "the word cut by another byte": (b"\x01LEDMATRIX-FW-SIGNING\x02requ\x00ired\x03", "no signing marker"),
            "required and off together": (MARKER_REQUIRED + b"\x00" * 16 + MARKER_OFF, "2 signing markers"),
        }
        for label, (marker, want) in cases.items():
            path = self.release(name="m" + str(abs(hash(label)) % 10 ** 6), marker=marker)
            self.refused(path, KEYS["A"]["secret"], want=want)
        ok = self.release(name="fine")
        self.assertEqual(self.sign(ok, None, KEYS["A"]["secret"]).rc, 0)

    def test_a_bad_keys_header(self):
        path = self.release()
        bad = os.path.join(self.tmp, "bad.h")
        with open(bad, "w") as f:
            f.write("#define UPDATE_KEY_COUNT 3\n")
        self.refused(path, KEYS["A"]["secret"], bad, want="UPDATE_KEY_COUNT")
        self.refused(path, KEYS["A"]["secret"], os.path.join(self.tmp, "missing.h"), want="cannot read the keys header")

    # ---- an already signed manifest ----

    def test_signing_again_checks_and_changes_nothing(self):
        path = self.release()
        self.assertEqual(self.sign(path, None, KEYS["A"]["secret"]).rc, 0)
        signed = self.read(path)
        for secret in (KEYS["A"]["secret"], KEYS["B"]["secret"], None):
            run = self.sign(path, None, secret)
            self.assertEqual((run.rc, run.err), (0, ""))
            self.assertIn("already signed", run.out)
            self.assertIn("not changed", run.out)
            self.assertEqual(self.read(path), signed)

    def test_an_already_signed_manifest_that_does_not_verify_is_refused(self):
        path = self.release()
        self.assertEqual(self.sign(path, None, KEYS["A"]["secret"]).rc, 0)
        m = self.jload(path)
        m["notes"] = "changed after signing"
        self.jsave(path, m)
        bad = self.read(path)
        run = self.sign(path, None, KEYS["A"]["secret"])
        self.assertEqual(run.rc, 1)
        self.assertIn("does not verify", run.err)
        self.assertEqual(self.read(path), bad)
        half = self.release(name="half")
        m = self.jload(half)
        m["keyid"] = KEYS["A"]["id"]
        self.jsave(half, m)
        run = self.sign(half, None, KEYS["A"]["secret"])
        self.assertEqual(run.rc, 1)
        self.assertIn("malformed", run.err)

    # ---- --check ----

    def test_check_accepts_a_signed_manifest_and_needs_no_key(self):
        path = self.release()
        self.assertEqual(self.sign(path, None, KEYS["A"]["secret"]).rc, 0)
        run = self.check(path)
        self.assertEqual((run.rc, run.err), (0, ""))
        self.assertTrue(run.out.startswith(f"ok version=0.11.0 keyid={KEYS['A']['id']}: "), run.out)
        self.assertIn("the image next to it matches", run.out)
        self.assertEqual(self.check(path, HEADER1).rc, 0)
        lonely = os.path.join(self.tmp, "lonely")
        os.mkdir(lonely)
        copy = os.path.join(lonely, "manifest.json")
        with open(copy, "wb") as f:
            f.write(self.read(path))
        run = self.check(copy)                                                   # no image next to it: still checked, and says so
        self.assertEqual(run.rc, 0, run.err)
        self.assertIn("not compared", run.out)

    def test_check_refuses_every_kind_of_tampering(self):
        path = self.release()
        self.assertEqual(self.sign(path, None, KEYS["A"]["secret"]).rc, 0)
        genuine = self.jload(path)
        folder = os.path.dirname(path)

        def flip(sig, i=40):
            raw = bytearray(bytes.fromhex(sig))
            raw[i] ^= 0x01
            return bytes(raw).hex()

        edits = {
            "notes edited": ({"notes": genuine["notes"] + " "}, "does not verify"),
            "size plus one": ({"size": genuine["size"] + 1}, "does not verify"),
            "another sha256": ({"sha256": flip(genuine["sha256"], 3)}, "does not verify"),
            "a flipped bit in r": ({"sig": flip(genuine["sig"], 3)}, "does not verify"),
            "a flipped bit in s": ({"sig": flip(genuine["sig"], 40)}, "does not verify"),
            "the other key's id": ({"keyid": KEYS["B"]["id"]}, "does not verify"),
            "an unknown key id": ({"keyid": "00000000"}, "not one of the pinned keys"),
            "an outsider's id": ({"keyid": KEYS["C"]["id"]}, "not one of the pinned keys"),
            "a short sig": ({"sig": genuine["sig"][:-2]}, "malformed"),
            "an upper case sig": ({"sig": genuine["sig"].upper()}, "malformed"),
            "an upper case keyid": ({"keyid": genuine["keyid"].upper()}, "malformed"),
            "a sig that is a number": ({"sig": 5}, "malformed"),
            "a control character in the notes": ({"notes": "a\nb"}, "control character"),
        }
        for label, (change, want) in edits.items():
            m = dict(genuine)
            m.update(change)
            tampered = os.path.join(self.tmp, "t-" + str(abs(hash(label)) % 10 ** 6) + ".json")
            os.makedirs(os.path.dirname(tampered), exist_ok=True)
            self.jsave(tampered, m)
            for flag in ((), ("--allow-unsigned",)):
                run = self.check(tampered, None, *flag)
                self.assertEqual(run.rc, 1, (label, flag, run.out))
                self.assertIn(want, run.err, label)
                self.assertEqual(run.out, "")
        for only in ("keyid", "sig"):
            m = dict(genuine)
            del m[only]
            tampered = os.path.join(self.tmp, "only-" + only + ".json")
            self.jsave(tampered, m)
            run = self.check(tampered, None, "--allow-unsigned")
            self.assertEqual(run.rc, 1, only)
            self.assertIn("malformed", run.err)
        # a real signature, but the image next to the manifest is not the signed one
        with open(os.path.join(folder, "ledmatrix-0.11.0.bin"), "ab") as f:
            f.write(b"\x00")
        run = self.check(path)
        self.assertEqual(run.rc, 1)
        self.assertIn("bytes but the manifest says", run.err)

    def test_check_with_the_wrong_header(self):
        path = self.release()
        self.assertEqual(self.sign(path, None, KEYS["B"]["secret"]).rc, 0)
        run = self.check(path, HEADER1)                                       # only A is pinned here
        self.assertEqual(run.rc, 1)
        self.assertIn("not one of the pinned keys", run.err)
        self.assertIn(KEYS["B"]["id"], run.err)

    # ---- the transition rule of the workflow (REQUIRE_SIGNING) ----

    def test_unsigned_is_allowed_only_with_the_flag(self):
        path = self.release()
        before = self.read(path)
        # REQUIRE_SIGNING=1: an empty secret fails and nothing is written
        run = self.sign(path, None, "")
        self.assertEqual(run.rc, 1)
        # REQUIRE_SIGNING=0: it warns loudly, leaves the manifest unsigned and succeeds
        run = self.sign(path, None, "", "--allow-unsigned")
        self.assertEqual(run.rc, 0)
        self.assertIn("the manifest is NOT signed: UPDATE_SIGNING_KEY is not set; displays that enforce signatures will refuse this release", run.err)
        self.assertTrue(run.out.startswith("unsigned version=0.11.0"), run.out)
        self.assertEqual(self.read(path), before)
        run = self.sign(path, None, None, "--allow-unsigned")                    # a missing variable is the same as an empty one
        self.assertEqual(run.rc, 0)
        # --check of the unsigned manifest: refused without the flag, accepted with it, and says what it is
        run = self.check(path)
        self.assertEqual(run.rc, 1)
        self.assertIn("not signed", run.err)
        run = self.check(path, None, "--allow-unsigned")
        self.assertEqual(run.rc, 0)
        self.assertIn("NOT signed", run.out)
        self.assertIn("REQUIRE_SIGNING is 0", run.out)

    def test_with_a_secret_the_flag_still_signs(self):
        path = self.release()
        run = self.sign(path, None, KEYS["A"]["secret"], "--allow-unsigned")
        self.assertEqual((run.rc, run.err), (0, ""))
        self.assertTrue(run.out.startswith("signed version=0.11.0"))
        self.assert_signed(path, "A")

    def test_the_flag_never_excuses_a_bad_key_or_a_bad_manifest(self):
        path = self.release()
        self.refused(path, KEYS["C"]["secret"], None, "--allow-unsigned", want="not one of the keys pinned", key=KEYS["C"])
        self.refused(path, "garbage!", None, "--allow-unsigned", want="neither PEM text nor one line of base64")
        bad = self.release(name="badmanifest")
        m = self.jload(bad)
        m["notes"] = "a\nb"
        self.jsave(bad, m)
        self.refused(bad, "", None, "--allow-unsigned", want="control character")
        img = self.release(name="badimage")
        with open(os.path.join(os.path.dirname(img), "ledmatrix-0.11.0.bin"), "ab") as f:
            f.write(b"\x00")
        self.refused(img, "", None, "--allow-unsigned", want="bytes but the manifest says")

    def test_usage_errors(self):
        for args in ([], ["--keys-header", HEADER2], ["a.json", "--check", "b.json", "--keys-header", HEADER2],
                     ["--check", "a.json", "--keys-header", HEADER2, "--key-file", "k.pem"], ["a.json"]):
            run = self.tool("sign_manifest.py", *args)
            self.assertEqual(run.rc, 2, args)


# ---- 5. make_test_manifests.py -----------------------------------------------------------------------------------------------------

@needs_openssl
class TestManifestsTests(ToolCase):
    CASES = ["genuine", "unsigned", "outsider-key", "flipped-sig-bit", "notes-edited", "size-plus-one", "version-raised", "wrong-length-sig", "only-keyid"]
    EXPECT = {"genuine": "ok", "unsigned": "unsigned", "outsider-key": "unknown_key", "flipped-sig-bit": "bad_signature",
              "notes-edited": "bad_signature", "size-plus-one": "bad_signature", "version-raised": "bad_signature",
              "wrong-length-sig": "malformed", "only-keyid": "malformed"}

    def genuine(self):
        path = self.release(name="genuine")
        run = self.tool("sign_manifest.py", path, "--keys-header", HEADER2, secret=KEYS["A"]["secret"])
        self.assertEqual(run.rc, 0, run.err)
        return os.path.dirname(path)

    def make(self, src, out, *extra):
        return self.tool("make_test_manifests.py", "--from", src, "--out", out, "--keys-header", HEADER2, *extra)

    def test_every_case_is_what_the_table_says(self):
        src = self.genuine()
        out = os.path.join(self.tmp, "cases")
        run = self.make(src, out)
        self.assertEqual((run.rc, run.err), (0, ""))
        self.assertEqual(sorted(os.listdir(out)), sorted(self.CASES))
        pinned = [KEYS["A"]["pub"], KEYS["B"]["pub"]]
        genuine = us.load_manifest(os.path.join(src, "manifest.json"))
        for name in self.CASES:
            folder = os.path.join(out, name)
            m = us.load_manifest(os.path.join(folder, "manifest.json"))
            self.assertEqual(us.classify(m, pinned)[0], self.EXPECT[name], name)
            self.assertIn(m["file"] if name != "wrong-length-sig" else genuine["file"], os.listdir(folder), name)
            if name not in ("wrong-length-sig", "only-keyid"):
                # what the service would check: the image next to it has the size and SHA-256 the manifest says
                data, problem = us.read_image(os.path.join(folder, "manifest.json"), m)
                self.assertIsNone(problem, name)
                self.assertIsNotNone(data, name)
                self.assertIsNone(us.manifest_problem(m, release=True), name)
        self.assertEqual(self.read(os.path.join(out, "genuine", "manifest.json")), self.read(os.path.join(src, "manifest.json")))
        self.assertNotIn("keyid", self.jload(os.path.join(out, "unsigned", "manifest.json")))
        sig = genuine["sig"]
        self.assertEqual(sum(a != b for a, b in zip(sig, self.jload(os.path.join(out, "flipped-sig-bit", "manifest.json"))["sig"])), 1)
        self.assertEqual(self.jload(os.path.join(out, "wrong-length-sig", "manifest.json"))["sig"], sig[:-2])
        self.assertNotIn("sig", self.jload(os.path.join(out, "only-keyid", "manifest.json")))
        self.assertEqual(self.jload(os.path.join(out, "size-plus-one", "manifest.json"))["size"], genuine["size"] + 1)
        self.assertEqual(os.path.getsize(os.path.join(out, "size-plus-one", genuine["file"])), genuine["size"] + 1)
        self.assertEqual(self.jload(os.path.join(out, "version-raised", "manifest.json"))["version"], "0.11.1")
        self.assertEqual(self.jload(os.path.join(out, "outsider-key", "manifest.json"))["keyid"] not in (KEYS["A"]["id"], KEYS["B"]["id"]), True)
        self.assertNotEqual(self.jload(os.path.join(out, "notes-edited", "manifest.json"))["notes"], genuine["notes"])

    def test_the_output_holds_no_private_key_and_prints_no_signature(self):
        src = self.genuine()
        out = os.path.join(self.tmp, "cases")
        run = self.make(src, out)
        self.assertEqual(run.rc, 0, run.err)
        genuine = us.load_manifest(os.path.join(src, "manifest.json"))
        self.assertNotIn(genuine["sig"], run.out)
        self.assertIsNone(re.search(r"[0-9a-f]{128}", run.out))
        self.assertNotIn("PRIVATE", run.out + run.err)
        for root, _, files in os.walk(out):
            for f in files:
                if f.endswith(".bin"):
                    continue
                self.assertNotIn(b"PRIVATE KEY", self.read(os.path.join(root, f)), f)
        for case in self.CASES:
            self.assertIn(case, run.out)
        self.assertIn("NO (404)", run.out)                          # the two shapes the service refuses to publish are said so

    def test_the_outsider_key_is_throwaway(self):
        src = self.genuine()
        ids = set()
        for i in range(2):
            out = os.path.join(self.tmp, f"cases{i}")
            self.assertEqual(self.make(src, out).rc, 0)
            ids.add(self.jload(os.path.join(out, "outsider-key", "manifest.json"))["keyid"])
        self.assertEqual(len(ids), 2, "a new key every run")

    def test_refusals(self):
        src = self.genuine()
        out = os.path.join(self.tmp, "cases")
        self.assertEqual(self.make(src, out).rc, 0)
        run = self.make(src, out)                                     # the folder is not empty now
        self.assertEqual(run.rc, 1)
        self.assertIn("not empty", run.err)
        self.assertEqual(self.make(src, src).rc, 1)                   # not into itself
        unsigned = os.path.dirname(self.release(name="unsigned-src"))
        run = self.make(unsigned, os.path.join(self.tmp, "o2"))
        self.assertEqual(run.rc, 1)
        self.assertIn("not a genuine signed release", run.err)
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "o2")))
        # a signed manifest of another key than the header pins
        other = os.path.dirname(self.release(name="outsider-src"))
        self.assertEqual(self.tool("sign_manifest.py", os.path.join(other, "manifest.json"), "--keys-header", HEADER2, secret=KEYS["B"]["secret"]).rc, 0)
        run = self.tool("make_test_manifests.py", "--from", other, "--out", os.path.join(self.tmp, "o3"), "--keys-header", HEADER1)
        self.assertEqual(run.rc, 1)
        self.assertIn("not a genuine signed release", run.err)
        # no image next to the manifest
        lonely = os.path.join(self.tmp, "lonely")
        os.mkdir(lonely)
        with open(os.path.join(lonely, "manifest.json"), "wb") as f:
            f.write(self.read(os.path.join(src, "manifest.json")))
        self.assertEqual(self.make(lonely, os.path.join(self.tmp, "o4")).rc, 1)

    def test_the_default_header_is_the_real_one(self):
        self.assertTrue(os.path.samefile(mt.DEFAULT_HEADER, KEYS_HEADER))


# ---- 6. mutation checks: the tests must notice when the rules are broken -------------------------------------------------------------

class MutationTests(unittest.TestCase):
    """Break the shared module on purpose, one rule at a time; the contract checks must fail for each. (An untouched copy passes.)"""

    def mutant(self, old, new):
        return mutated("update_signing", (old, new))

    def failures(self, module):
        return contract_checks(module, VEC)

    def test_an_untouched_copy_passes(self):
        self.assertEqual(self.failures(load("update_signing_copy", filename="update_signing")), [])

    def test_each_mutation_is_noticed(self):
        mutations = {
            "the domain prefix": ('PREFIX = "ledmatrix-update-v1"', 'PREFIX = "ledmatrix-update-v2"'),
            "the notes are left out": ('"notes=" + notes]', '"notes="]'),
            "the size is padded": ('"size=" + str(m["size"])', '"size=%08d" % m["size"]'),
            "a trailing line feed": ('text = "\\n".join([PREFIX,', 'text = "\\n".join(["", PREFIX,'),
            "CRLF line ends": ('text = "\\n".join([', 'text = "\\r\\n".join(['),
            "the key id slice": ("hexdigest()[:KEYID_HEX]", "hexdigest()[1:KEYID_HEX + 1]"),
            "the key id is not looked at": ("candidates = [p for p in pinned if key_id(p) == keyid]", "candidates = list(pinned)"),
            "the signature is not looked at": ("if any(verify(p, digest, raw) for p in candidates):", "if True:"),
            "upper case hex is accepted": ('frozenset("0123456789abcdef")', 'frozenset("0123456789abcdefABCDEF")'),
            "r and s are not range checked": ("if not (1 <= r < N and 1 <= s < N):", "if False:"),
            "s must be below n/2": ("if not (1 <= r < N and 1 <= s < N):", "if not (1 <= r < N and 1 <= s < N // 2):"),
            "the point is not checked": ("return point if on_curve(point) else None", "return point"),
            "control characters are allowed": ("return any(ord(c) < 0x20 or ord(c) == 0x7F for c in text)", "return False"),
            "DEL is allowed": ("or ord(c) == 0x7F for c in text)", "for c in text)"),
            "notes may be longer": ("if len(notes) > NOTES_MAX:", "if len(notes) > 2 * NOTES_MAX:"),
            "a size of exactly 100 KB is fine": ("MIN_SIZE_EXCLUSIVE < size <= MAX_SIZE", "MIN_SIZE_EXCLUSIVE <= size <= MAX_SIZE"),
            "a float size is fine": ("or not isinstance(size, int) or", "or not isinstance(size, (int, float)) or"),
            "a sig of the wrong shape is fine": ("or not is_lower_hex(sig, SIG_HEX):", ":"),
            "null is not absent": ("present = [v is not None for v in (keyid, sig)]", "present = [v is not None or k in m for k, v in (('keyid', keyid), ('sig', sig))]"),
            "the manifest rules come after": ("    if manifest_problem(m):\n        return MANIFEST_INVALID, None\n", ""),
            "wrong curve constant": ("N = 0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551", "N = 0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632552"),
        }
        for label, (old, new) in mutations.items():
            self.assertNotEqual(self.failures(self.mutant(old, new)), [], f"the mutation '{label}' was not noticed")


@needs_openssl
class SignerMutationTests(ToolCase):
    """The same for the signer: its refusals are the point of it, so each is removed once and a small battery must notice."""

    def battery(self, signer, shared=None):
        """Run the signer's main promises in process; return the list of failures. `signer` is a (possibly mutated) sign_manifest module."""
        if shared is not None:
            signer.us = shared
        fails = []
        pinned = signer.us.parse_keys_header(HEADER2)
        keyfile = KEYS["A"]["pem_path"]
        outsider = KEYS["C"]["pem_path"]
        env = {"RUNNER_TEMP": self.runner}

        def attempt(label, name, key_path, expect_ok, edit=None, image_edit=None, marker=MARKER_REQUIRED):
            path = self.release(name=name, marker=marker)
            if edit:
                m = self.jload(path)
                m.update(edit)
                self.jsave(path, m)
            if image_edit:
                image_edit(os.path.join(os.path.dirname(path), "ledmatrix-0.11.0.bin"))
            before = self.read(path)
            err = io.StringIO()
            with unittest.mock.patch.dict(os.environ, env), redirect_stderr(err), redirect_stdout(io.StringIO()):
                try:
                    signer.sign_or_keep(path, pinned, key_path, False)
                    ok = True
                except signer.us.Refusal:
                    ok = False
                except Exception as e:                    # a crash is not a refusal
                    fails.append(f"{label}: raised {e!r}")
                    return
            if ok != expect_ok:
                fails.append(f"{label}: {'signed' if ok else 'refused'}, expected the opposite")
            after = self.read(path)
            if not ok and after != before:
                fails.append(f"{label}: refused but wrote the manifest")
            if ok:
                m = signer.us.load_manifest(path)
                if signer.us.classify(m, pinned.keys)[0] != "ok":
                    fails.append(f"{label}: what it wrote does not verify")

        def flip_byte(p):
            with open(p, "r+b") as f:
                f.seek(5000)
                b = f.read(1)
                f.seek(5000)
                f.write(bytes([b[0] ^ 1]))

        attempt("a good release", "b-good", keyfile, True)
        attempt("an unpinned key", "b-outsider", outsider, False)
        attempt("a development image", "b-off", keyfile, False, marker=MARKER_OFF)
        attempt("an image without the marker", "b-none", keyfile, False, marker=b"")
        attempt("a changed image", "b-flip", keyfile, False, image_edit=flip_byte)
        attempt("a manifest with a newline in the notes", "b-nl", keyfile, False, edit={"notes": "a\nb"})
        attempt("a leading zero", "b-zero", keyfile, False, edit={"version": "00.11.0", "file": "ledmatrix-00.11.0.bin"})
        return fails

    def test_an_untouched_signer_passes_the_battery(self):
        self.assertEqual(self.battery(load("sign_manifest_copy", filename="sign_manifest")), [])

    def test_each_mutation_is_noticed(self):
        # The signer checks the same things in more than one place (defence in depth), so a layer that is removed alone is not visible:
        # the layers that back each other up are removed together here.
        final = ('if result != us.OK:\n        raise us.Refusal("the signed manifest does not pass the final check',
                 'if False:\n        raise us.Refusal("the signed manifest does not pass the final check')
        selfcheck = ("if not us.verify_message(pub, canonical, raw):", "if False:")
        opensslcheck = ("if not us.openssl_verify(pub, canonical, raw, work, openssl):", "if False:")
        zeros = ('signed["keyid"], signed["sig"] = kid, raw.hex()', 'signed["keyid"], signed["sig"] = kid, "0" * 128')
        rules = ('why = us.manifest_problem(m, release=True)\n    if why:\n        raise us.Refusal(f"the manifest is not valid: {why}")\n    check_image(manifest_path, m, need_marker=%s)',
                 'check_image(manifest_path, m, need_marker=%s)')
        sign_mutations = {
            "the pinned-key check and the final check are removed": (("if pub not in pinned.keys:", "if False:"), final),
            "the development marker is accepted": (('if markers[0] == "off":', "if False:"), ('if markers[0] != "required":', "if False:")),
            "the marker is not required": (("if need_marker:", "if False:"),),
            "no signature is checked anywhere and a bad one is written": (selfcheck, opensslcheck, final, zeros),
            "a bad signature is written (the final check refuses it, so no manifest at all)": (zeros,),
            "the manifest rules are skipped": ((rules[0] % "True", rules[1] % "True"), (rules[0] % "False", rules[1] % "False")),
        }
        for label, pairs in sign_mutations.items():
            signer = mutated("sign_manifest", *pairs)
            self.assertNotEqual(self.battery(signer), [], f"the mutation '{label}' was not noticed")
        shared_mutations = {
            "the image hash is not compared": (('if hashlib.sha256(data).hexdigest() != m["sha256"]:', "if False:"),),
            "neither the image size nor its hash is compared": (('if hashlib.sha256(data).hexdigest() != m["sha256"]:', "if False:"),
                                                                ('if len(data) != m["size"]:', "if False:")),
        }
        for label, pairs in shared_mutations.items():
            shared = mutated("update_signing", *pairs)
            self.assertNotEqual(self.battery(load("sign_manifest_copy", filename="sign_manifest"), shared), [],
                                f"the mutation '{label}' was not noticed")


# ---- 7. the workflow and the command lines --------------------------------------------------------------------------------------------

class WorkflowTests(unittest.TestCase):
    """A light check of .github/workflows/service.yml (no YAML library: lines): where the secret is, and the transition rule."""

    def setUp(self):
        if not os.path.exists(WORKFLOW):
            self.skipTest("the workflow file is not in this checkout")
        with open(WORKFLOW, "r", encoding="utf-8") as f:
            self.text = f.read()
        self.lines = self.text.splitlines()

    def step_index(self, name):
        for i, ln in enumerate(self.lines):
            if ln.strip() == f"- name: {name}":
                return i
        self.fail(f"no step named {name!r}")

    def test_the_secret_appears_once_and_only_in_the_sign_step(self):
        hits = [i for i, ln in enumerate(self.lines) if "secrets.UPDATE_SIGNING_KEY" in ln]
        self.assertEqual(len(hits), 1)
        start = self.step_index("Sign the manifest")
        end = min(i for i in range(start + 1, len(self.lines)) if self.lines[i].lstrip().startswith("- "))        # the next step
        self.assertTrue(start < hits[0] < end)
        self.assertRegex(self.lines[hits[0]].strip(), r"^UPDATE_SIGNING_KEY: \$\{\{ secrets\.UPDATE_SIGNING_KEY \}\}")
        self.assertTrue(self.lines[hits[0] - 1].strip().startswith("#") or self.lines[hits[0] - 1].strip() == "env:")
        # the secret is not echoed, expanded in a script, or put in a job or workflow level env
        self.assertNotRegex(self.text, r"(?m)^env:\n(?:  .*\n)*?  UPDATE_SIGNING_KEY")
        for ln in self.lines:
            if ln.strip().startswith("echo") or ln.strip().startswith("set -x"):
                self.assertNotIn("UPDATE_SIGNING_KEY", ln.replace("UPDATE_SIGNING_KEY is not set", ""))

    def test_the_order_of_the_steps(self):
        write = self.step_index("Write the manifest and the image into service/firmware")
        sign = self.step_index("Sign the manifest")
        check = self.step_index("Check the signed manifest (public keys only)")
        keep = self.step_index("Keep the firmware as a workflow artifact")
        self.assertTrue(write < sign < check < keep)
        self.assertGreater(self.text.index("docker/build-push-action"), self.text.index("Check the signed manifest"))
        self.assertGreater(self.text.index("actions/upload-artifact"), self.text.index("Check the signed manifest"))

    def test_the_transition_rule(self):
        self.assertRegex(self.text, r'(?m)^env:\n(?:  .*\n)*?  REQUIRE_SIGNING: "0"\n')
        self.assertIn("Set to \"1\" once the secret UPDATE_SIGNING_KEY exists", self.text)
        sign = "\n".join(self.lines[self.step_index("Sign the manifest"):self.step_index("Check the signed manifest (public keys only)")])
        self.assertIn("the manifest is NOT signed: UPDATE_SIGNING_KEY is not set; displays that enforce signatures will refuse this release", sign)
        self.assertIn("--allow-unsigned", sign)
        self.assertIn("python3 -I tools/sign_manifest.py service/firmware/manifest.json --keys-header arduino/ledmatrix/update_keys.h", sign)
        check = "\n".join(self.lines[self.step_index("Check the signed manifest (public keys only)"):self.step_index("Keep the firmware as a workflow artifact")])
        self.assertIn("--check service/firmware/manifest.json --keys-header arduino/ledmatrix/update_keys.h", check)
        self.assertIn('"$REQUIRE_SIGNING" = "0"', check)

    def test_the_tests_and_the_paths(self):
        self.assertIn("python3 -I tools/test_sign_manifest.py", self.text)
        head = self.text.split("permissions:")[0]
        for p in ("tools/update_signing.py", "tools/sign_manifest.py", "tools/gen_signing_key.py", "tools/make_test_manifests.py",
                  "tools/test_sign_manifest.py", "tools/make_manifest.py", "shared/**", "arduino/**"):
            self.assertIn(f'"{p}"', head, p)

    def test_actions_stay_pinned_by_hash(self):
        for ln in self.lines:
            m = re.match(r"\s*- uses: (\S+)@(\S+)", ln) or re.match(r"\s*uses: (\S+)@(\S+)", ln)
            if m:
                self.assertRegex(m.group(2), r"^[0-9a-f]{40}$", ln)


class CommandLineTests(unittest.TestCase):
    TOOLS = ("sign_manifest.py", "gen_signing_key.py", "make_test_manifests.py")

    def test_help_runs_under_python_dash_I(self):
        for tool in self.TOOLS:
            r = subprocess.run([sys.executable, "-I", os.path.join(HERE, tool), "--help"], capture_output=True, text=True)
            self.assertEqual((r.returncode, r.stderr), (0, ""), tool)
            self.assertIn("usage:", r.stdout)

    def test_no_bytecode_is_written_next_to_the_tools(self):
        cache = os.path.join(HERE, "__pycache__")

        def mine():
            return {n for n in (os.listdir(cache) if os.path.isdir(cache) else [])
                    if n.startswith(("update_signing.", "sign_manifest.", "gen_signing_key.", "make_test_manifests."))}

        before = mine()
        for tool in self.TOOLS:
            subprocess.run([sys.executable, "-I", os.path.join(HERE, tool), "--help"], capture_output=True)
        self.assertEqual(mine(), before)

    def test_a_missing_openssl_is_said_plainly(self):
        with tempfile.TemporaryDirectory() as tmp:
            r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "gen_signing_key.py"), "--out", os.path.join(tmp, "k")],
                               capture_output=True, text=True, env={"PATH": tmp})
            self.assertEqual(r.returncode, 1)
            self.assertIn("openssl command line is not installed", r.stderr)
            self.assertFalse(os.path.exists(os.path.join(tmp, "k")))


if __name__ == "__main__":
    if not OPENSSL:
        print("NOTE: " + NO_OPENSSL, file=sys.stderr)
    unittest.main(verbosity=1)
