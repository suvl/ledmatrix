#!/usr/bin/env python3
r"""Create the firmware update signing keys (shared/manifest-signing.md, sections 7 and 8).

    python3 -I tools/gen_signing_key.py --out ~/ledmatrix-keys [--names A B]

Makes one ECDSA P-256 key pair per name with the openssl command line (`openssl ecparam -name prime256v1 -genkey -noout`; the
Mac's own LibreSSL works) and writes, per name N, into the output folder:

    ledmatrix-update-N.pem               the private key (PEM), mode 0600
    ledmatrix-update-N.pub.pem           the public key (PEM), mode 0600
    ledmatrix-update-N.pub.hex           the public key as the 130 hex characters update_keys.h pins, mode 0600
    ledmatrix-update-N.github-secret.txt the private key PEM as ONE line of base64, mode 0600: the value of the GitHub secret
                                         UPDATE_SIGNING_KEY  (gh secret set UPDATE_SIGNING_KEY < that file)

and prints ONLY public information: the key ids, the `#define UPDATE_KEY_n "04..."` lines for arduino/ledmatrix/update_keys.h, the
paths and the reminder of the backup rule. It never prints a private key, the base64 line or a signature.

Safety: the output folder is created mode 0700 (an existing one must be yours and closed to others), must not be inside a git work
tree, no file is ever overwritten (if one exists, nothing at all is written), every new key is tested (a message signed with it is
verified by this tool's own pure-Python verifier) before anything is written, and a run that fails half way removes what it made.
The first name becomes UPDATE_KEY_0 (the current key: its private half goes in CI), the second UPDATE_KEY_1 (the spare: offline
only, never in CI). Python 3.8 or newer, standard library plus the openssl command line."""
import argparse
import base64
import importlib.util
import os
import re
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))

_spec = importlib.util.spec_from_file_location("update_signing", os.path.join(HERE, "update_signing.py"))
us = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(us)

NAME_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]{0,31}")
ROLES = ("the current key: its private half is the CI secret UPDATE_SIGNING_KEY",
         "the spare (\"next\"): its private half stays offline and is never in CI")


def file_names(name):
    base = f"ledmatrix-update-{name}"
    return {"pem": base + ".pem", "pub_pem": base + ".pub.pem", "pub_hex": base + ".pub.hex",
            "secret": base + ".github-secret.txt"}


def inside_git_work_tree(path):
    """True when `path` (which may not exist yet) is, or would be, inside a git work tree: a .git file or folder in it or in any
    folder above it."""
    cur = os.path.realpath(path)
    while True:
        if os.path.lexists(os.path.join(cur, ".git")):
            return True
        parent = os.path.dirname(cur)
        if parent == cur:
            return False
        cur = parent


def prepare_folder(path):
    """The output folder, made (mode 0700) or checked. Returns its absolute path."""
    path = os.path.abspath(os.path.expanduser(path))
    if inside_git_work_tree(path):
        raise us.Refusal(f"{path} is inside a git work tree: the private keys must never be near a repository. Pick a folder "
                         "outside it, for example ~/ledmatrix-keys")
    if os.path.lexists(path):
        if os.path.islink(path) or not os.path.isdir(path):
            raise us.Refusal(f"{path} exists and is not a plain folder")
        st = os.stat(path)
        if st.st_uid != os.geteuid():
            raise us.Refusal(f"{path} belongs to another user")
        if st.st_mode & 0o077:
            raise us.Refusal(f"{path} is open to other users (mode {oct(st.st_mode & 0o777)[2:]}): run chmod 700 on it first")
    else:
        os.makedirs(path, mode=0o700)
        os.chmod(path, 0o700)
    return path


def make_one(name, openssl=None):
    """A new key: returns the file contents {role: bytes}, the public point and its key id. Tests the key first."""
    pem = us.generate_key_pem(openssl)
    with us.private_workdir() as wd:
        keyfile = os.path.join(wd, "key.pem")
        us.write_private(keyfile, pem.encode("ascii"))
        pub = us.public_point_from_key_file(keyfile, openssl)
        probe = b"ledmatrix-update key self-test " + name.encode("ascii")
        sig = us.sign_with_key_file(keyfile, probe, openssl)
    if not us.verify_message(pub, probe, sig):
        raise us.Refusal(f"the new key {name} failed its self-test (a message it signed does not verify): nothing was written")
    secret_line = base64.b64encode(pem.encode("ascii")).decode("ascii") + "\n"
    files = {"pem": pem.encode("ascii"), "pub_pem": us.point_to_pem(pub).encode("ascii"),
             "pub_hex": (pub.hex() + "\n").encode("ascii"), "secret": secret_line.encode("ascii")}
    return files, pub, us.key_id(pub)


def generate(out, names, openssl=None):
    """Make the keys and write the files. Returns a list of (name, key id, public hex, {role: path}). Raises Refusal."""
    if not 1 <= len(names) <= 2:
        raise us.Refusal("give one or two names: the firmware pins at most 2 keys (the current key and the spare)")
    for n in names:
        if not NAME_RE.fullmatch(n):
            raise us.Refusal(f"the name {n!r} is not allowed: use letters, digits, '-' and '_' (at most 32, starting with a letter or digit)")
    if len({n.lower() for n in names}) != len(names):
        raise us.Refusal("the names must differ (a file system that ignores upper and lower case would treat A and a as one)")
    if not (openssl or us.find_openssl()):
        raise us.Refusal("the openssl command line is not installed (it makes the keys)")
    folder = prepare_folder(out)
    paths = {n: {role: os.path.join(folder, fn) for role, fn in file_names(n).items()} for n in names}
    for n in names:
        for p in paths[n].values():
            if os.path.lexists(p):
                raise us.Refusal(f"{p} already exists: nothing was written (this tool never overwrites a key; move or remove it yourself "
                                 "if you are sure)")
    made = {n: make_one(n, openssl) for n in names}
    if len({m[1] for m in made.values()}) != len(names):
        raise us.Refusal("two generated keys are identical (a broken random source?): nothing was written")
    created = []
    try:
        for n in names:
            files = made[n][0]
            for role in ("pem", "secret", "pub_pem", "pub_hex"):
                us.write_private(paths[n][role], files[role])
                created.append(paths[n][role])
    except BaseException:
        for p in created:
            try:
                os.unlink(p)
            except OSError:
                pass
        raise
    return [(n, made[n][2], made[n][1].hex(), paths[n]) for n in names]


def report(results, out_stream):
    w = out_stream.write
    folder = os.path.dirname(results[0][3]["pem"])
    w(f"Created {len(results)} key(s) in {folder} (folder mode 700, files mode 600). Nothing secret is printed here.\n\n")
    for i, (name, kid, pub_hex, paths) in enumerate(results):
        w(f"key {name}: key id {kid}\n")
        w(f'  #define UPDATE_KEY_{i} "{pub_hex}"   // {ROLES[i]}\n')
        w(f"  private key:  {paths['pem']}\n")
        w(f"  CI secret:    {paths['secret']}\n")
    w(f"\nIn arduino/ledmatrix/update_keys.h set UPDATE_KEY_COUNT to {len(results)} and paste the line(s) above (public keys only).\n")
    first = results[0][3]["secret"]
    w(f"To use {results[0][0]} for signing in CI:  gh secret set UPDATE_SIGNING_KEY < {first}\n")
    if len(results) == 2:
        w(f"{results[1][0]} is the spare: keep it offline and never put it in CI.\n")
    w("\nBackup rule: copy the .pem files (and the .github-secret.txt line you set as the secret) to two places that are not this\n"
      "computer's repository, the cluster or the build machine (a password manager, offline media). Losing both private halves\n"
      "is recoverable only by flashing every display over USB (shared/manifest-signing.md, section 9). Never paste, print or\n"
      "commit a private key.\n")


def main(argv=None):
    p = argparse.ArgumentParser(prog="gen_signing_key.py", allow_abbrev=False,
                                description="Create the firmware update signing keys (shared/manifest-signing.md).")
    p.add_argument("--out", required=True, metavar="DIR", help="the folder for the key files (outside any git work tree; created mode 0700)")
    p.add_argument("--names", nargs="+", default=["A", "B"], metavar="NAME",
                   help="one or two key names (default: A B). The first is the current key, the second the spare")
    args = p.parse_args(argv)
    try:
        results = generate(args.out, args.names)
    except us.Refusal as e:
        print(f"gen_signing_key: {e}", file=sys.stderr)
        return 1
    except OSError as e:
        print(f"gen_signing_key: {e.strerror or e}", file=sys.stderr)
        return 1
    report(results, sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
