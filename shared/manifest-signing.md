# Signed firmware manifest, scheme v1 (contract)

Decision: ticket #42 (reopens "no signature for now" of #21; the update flow of `firmware-update.md` and the device identity of `device-auth.md` are unchanged). Build: the signed-manifest slice (#50). This page is the contract between the firmware (C++), the service (Go), the build workflow, `tools/` (Python) and the portal. The known-answer vectors in `manifest-signing-vectors.json` are the test for all of them: same bytes, same answer. Reasons and rejected alternatives are in `wayfinder/research/signed-manifest-design.md`.

Every example value is a placeholder. The keys in the vectors are throwaway test keys. The real signing key is never in the repository, a ticket, a log, a commit, an image or a workflow artifact.

## What it does, in five sentences

The maintainer's CI signs the firmware manifest (version, file name, size, SHA-256 of the image, notes) with an ECDSA P-256 key. The firmware has up to two matching public keys built in (`update_keys.h`). A display installs a service-offered update only when the manifest it just fetched carries a valid signature by one of those keys, and only then compares the downloaded image's SHA-256 with the signed one. So changing the saved service address, controlling DNS or a certificate, or deploying a doctored service image no longer gets anything installed except genuine, signed releases. The manifest and the image stay public and unsigned-request-friendly, so a display whose key the service refuses still gets a signed fix.

## Decisions in one table

| Question | Decision |
| --- | --- |
| Algorithm | ECDSA on NIST P-256 (secp256r1, prime256v1) with SHA-256, verified with the mbedTLS that is already in the board's core (`mbedtls_ecdsa_verify`). No new third-party code |
| What is signed | The values of `version`, `file`, `size`, `sha256`, `notes` as the canonical bytes of section 2, under the domain-separation line `ledmatrix-update-v1`. Not the JSON text |
| Where the signature is | Two extra fields in `manifest.json`: `keyid` (8 lowercase hex) and `sig` (128 lowercase hex, r then s). The service passes the values through |
| Keys on the board | Up to 2 public keys in `arduino/ledmatrix/update_keys.h`: the current key (private half is the GitHub Actions secret `UPDATE_SIGNING_KEY`) and a spare ("next", private half offline, never in CI) |
| Rotation, loss, leak | Swap the spare into CI, release a version that pins the spare and a fresh spare; no flag day. Both lost: USB flash |
| Own builds | Replace `update_keys.h` and sign with your own key; `-DUPDATE_REQUIRE_SIGNATURE=0` (development builds only) switches enforcement off |
| Downgrade, replay | Only a strictly newer version than the running one (and than any version that rolled back) is offered. No `expires`, no stored floor |
| Bootstrap | 0.10.x boards ignore the new fields and install the first signed release from an unsigned check, once. From then on enforcement is on |
| `.bin` upload | Still code-only (6-digit panel code). It cannot be signed. Residual risk, section 10 |
| Fail-safe | A refusal is soft (nothing written, nothing rebooted), says why in plain words, and never affects the data side. Manifest and download stay public |

## 1. Algorithm and encodings

| Item | Value |
| --- | --- |
| Signature algorithm | ECDSA, curve NIST P-256 (also called secp256r1 and prime256v1), hash SHA-256 |
| Message hashed | The canonical bytes of section 2, hashed once with SHA-256 (the usual "ECDSA with SHA-256"); the 32-byte digest is what `mbedtls_ecdsa_verify` takes |
| Signature on the wire | 64 bytes: `r` then `s`, each 32 bytes, big endian, fixed width (left padded with zeros), written as **128 lowercase hexadecimal characters**. Never DER, never base64, never upper case |
| Public key | 65 bytes: the uncompressed point (the byte `0x04`, then X, then Y, 32 bytes each), written as 130 lowercase hex characters in `update_keys.h`. Compressed or DER forms are not used |
| Key id | The first 4 bytes of SHA-256 of the 65-byte public key, as 8 lowercase hex characters. It only selects which pinned key to try; the signature decides. Derived, never typed by hand |
| Accepted signatures | `1 <= r < n` and `1 <= s < n` (mbedTLS checks). Both `s` and `n-s` verify; that is accepted by design (the vectors include the "high-s twin"). Nothing depends on a signature being unique |
| Determinism | ECDSA signatures from the openssl CLI use a random nonce, so two signatures of the same bytes differ. Verification does not care. The tests use fixed signatures (the vectors) |
| Randomness needed on the board | None: verification needs no random numbers |
| No algorithm field | The pinned key fixes the algorithm and `ledmatrix-update-v1` fixes the format. A later algorithm means a new prefix (`-v2`) and new field names; older boards then ignore the new fields and see "unsigned" (they keep the keys they have) |

Why not base64 or DER on the wire: exactly one valid text form per value, no decoder on the board beyond hex (already used for the SHA-256), no ASN.1 parser in the path that handles untrusted bytes, and a fixed length that is checked before any crypto runs.

## 2. The canonical bytes (what is signed)

Six lines joined by a single line feed (`0x0A`), **no line feed after the last line**, UTF-8:

```
ledmatrix-update-v1
version=<version>
file=<file>
size=<size>
sha256=<sha256>
notes=<notes>
```

- `ledmatrix-update-v1` is the domain-separation line: a signature made for this purpose cannot be mistaken for a signature of anything else, and this scheme cannot be confused with a later one.
- `<version>`, `<file>`, `<sha256>` are the manifest's strings exactly as they are in the JSON after decoding (they have already passed the rules of `firmware-update.md`: version is three numbers, file is `ledmatrix-<version>.bin`, sha256 is 64 lowercase hex).
- `<size>` is the decimal number with no sign, no leading zeros and no exponent: `1412537`.
- `<notes>` is the decoded notes text as UTF-8 bytes; a missing field, `null` and `""` are all the empty text (the line is then exactly `notes=`). Rules: at most 200 characters, **no control character** (no byte below `0x20`, no `0x7F`; the signer folds them to spaces already). Notes are signed so that nothing the portal shows from the manifest can be changed by whoever sits between the service and the board.
- The signature covers the **values**, not the JSON text. The service may re-encode the JSON (Go escapes `<`, `>`, `&`, U+2028) and a board may decode it differently as long as the decoded values are the same; the canonical bytes are built from the decoded values only. `keyid` and `sig` are not in the canonical bytes.
- The longest possible canonical text is under 1,024 bytes (`CANONICAL_MAX = 1024`).

Worked example (vector `ok-basic`, test key A, key id `f864ea3b`). The bytes, with `\n` for the line feed:

```
ledmatrix-update-v1\nversion=0.11.0\nfile=ledmatrix-0.11.0.bin\nsize=1412537\nsha256=2ba21a04f86b6da51b269a6eca124355345286cb4efff64048bda4518b05d2fa\nnotes=Test release: signed manifest vectors.
```

SHA-256 of those bytes: `91b4fbe9836bc6e61310702685cc434205535088b56a761757a9671cab827e0f`. The signature is in the vectors file.

## 3. The manifest, version 2

`GET /v1/firmware/manifest` answers the five fields of `firmware-update.md` and, for a signed release, two more, in this order:

```json
{"version":"0.11.0","file":"ledmatrix-0.11.0.bin","size":1412537,"sha256":"<64 hex>","notes":"...","keyid":"f864ea3b","sig":"<128 hex>"}
```

- **Version 1 stays valid**: a manifest without `keyid` and `sig` is answered exactly as before (no `keyid` or `sig` keys, no nulls). Boards that do not know signing (0.10.x) ignore the two new fields (their parser ignores unknown keys), so one manifest serves both generations of boards.
- `keyid` and `sig` come as a pair: both or neither. A manifest with only one, or with a value of the wrong shape (not a string, wrong length, upper case, not hex), is **malformed**, not merely unsigned. `null` counts as absent.
- The whole answer must be at most **2,048 bytes** (the limit already built into every board, `MANIFEST_MAX_BYTES`). With the longest notes (200 characters, up to 800 bytes) the answer stays under 1.2 KB; the service refuses to publish an answer over 2,048 bytes.
- The signature is created over the final values; `make_manifest.py` writes the five v1 fields and `sign_manifest.py` (section 6) appends the two new ones, so the order and the immutability rules of the v1 tools are unchanged.

## 4. What the board holds: `update_keys.h`

```c
// arduino/ledmatrix/update_keys.h: the public keys that updates may be signed with. Public by nature; edit only to rotate (docs).
#pragma once
#ifndef UPDATE_REQUIRE_SIGNATURE   // 1 (the default): an update is installed only when its manifest carries a valid signature by a key below
#define UPDATE_REQUIRE_SIGNATURE 1 // 0: development builds only, no signature is looked at (the portal says so)
#endif
#define UPDATE_KEY_COUNT 2         // 1 or 2
#define UPDATE_KEY_0 "04<128 hex>"   // the current key: its private half is the CI secret UPDATE_SIGNING_KEY
#define UPDATE_KEY_1 "04<128 hex>"   // the spare ("next"): its private half is offline and never in CI
```

Rules (compile-time and test-time):
- `UPDATE_KEY_COUNT` is 1 or 2 and each `UPDATE_KEY_n` is exactly 130 lowercase hex characters starting `04`, a point on the curve (the Python test checks, with its pure-Python P-256 code; the board checks again with `mbedtls_ecp_check_pubkey`). The two keys differ.
- With `UPDATE_REQUIRE_SIGNATURE` 1 the build **fails** (a `static_assert`) if there is no key. A release build can therefore never be a build that cannot verify anything.
- Development builds pass `--build-property "compiler.cpp.extra_flags=-DUPDATE_REQUIRE_SIGNATURE=0"` to `arduino-cli compile` (tested: it reaches the sketch). The release workflow never passes it, and `sign_manifest.py` refuses an image that does not carry the marker `\x01LEDMATRIX-FW-SIGNING\x02required\x03` (the firmware embeds it only when enforcement is on, like the version marker; with enforcement off the marker says `off`).
- The key ids are derived at run time (section 1), so the header cannot disagree with itself. The ids are shown in `/api/update`.
- The file holds public keys only. Whoever builds their own firmware replaces the two lines (section 8).

## 5. What the board does with a manifest

The check runs in `fetchManifest()` of `ota.cpp`, after the existing manifest rules and **before** the function returns a manifest to its caller. Both the daily or "Check now" check and the fresh look that starts a download go through it, so there is no path that uses an unchecked manifest. It runs on the data task (core 0), with no lock held; the render task never waits for it.

### 5.1 Order

1. The existing rules (`parseManifest`): the five fields, plus the one new rule that **notes hold no control character** (a manifest that breaks it is `ManifestInvalid`, as for any other broken field).
2. `UPDATE_REQUIRE_SIGNATURE` is 0: the signature fields are ignored, the result is `NotChecked`, the update goes on, and the portal says so (5.3). Nothing below runs.
3. Shape: `keyid` and `sig` both absent -> `Unsigned`. Exactly one present, or either not an 8 / 128 lowercase hex string -> `Malformed`.
4. Key: derive the ids of the pinned keys; none equals `keyid` -> `UnknownKey` (no crypto has run: the cheap refusals come first).
5. Build the canonical bytes from the parsed values, hash them (SHA-256, hardware), verify with the pinned key whose id matches (try each match; at most 2): valid -> `Ok(keyid)`; the library says "invalid signature" (any reason: wrong key, a flipped bit, other fields, `r` or `s` out of range) -> `BadSignature`; the library could not run (out of memory, an internal error) -> `CheckFailed`. The verify function is three-valued on purpose: running out of memory must not be reported as an attack, or an attack as a fault.
6. Only an `Ok` or `NotChecked` manifest goes on to `isOffered` (strictly newer, not a failed version).

### 5.2 Table (enforcement on)

| The manifest | Result | The board | `Fail` / raw text (`problems.h`) | Plain sentence (problem list `why`, portal `error`) |
| --- | --- | --- | --- | --- |
| Valid signature by a pinned key | `Ok` | Offers the update if newer; shows "Signed with key `<id>`" | none | none |
| No `keyid`, no `sig` | `Unsigned` | No notice, no download. Withdraws a notice shown earlier. Soft | `Fail::Unsigned`, `PROBLEM_RAW_UPDATE_UNSIGNED` = "firmware manifest is not signed" | "The service offers a firmware update that is not signed, so it was not offered. This board accepts only updates signed with the key built into it." |
| Only one of the two fields, or a field of the wrong shape | `Malformed` | As above | `Fail::ManifestInvalid`, detail "the signature fields are malformed" | "The firmware details from the service are not valid, so no update is offered." (the existing sentence, plus the detail) |
| `keyid` not among the pinned ids | `UnknownKey` | As above | `Fail::UnknownKey`, `PROBLEM_RAW_UPDATE_UNKNOWN_KEY` = "firmware signed with an unknown key" | "The firmware update is signed with a key this board does not know, so it was refused and nothing was changed. A USB flash may be needed." |
| Known `keyid`, signature does not verify (also: valid signature over different fields, a bit flipped anywhere, another key's signature under this id, `r` or `s` out of range) | `BadSignature` | As above | `Fail::BadSignature`, `PROBLEM_RAW_UPDATE_BAD_SIGNATURE` = "firmware signature is not valid" | "The signature on the firmware update does not check out, so it was refused and nothing was changed. Check that the service address is yours." |
| The check could not run | `CheckFailed` | As above; tried again at the next check | `Fail::SignatureCheckFailed`, `PROBLEM_RAW_UPDATE_CHECK_FAILED` = "could not check the firmware signature" | "The board could not check the signature on the firmware update (it ran short of memory). Nothing was changed. It tries again at the next check." |
| Any manifest, enforcement off (development build) | `NotChecked` | Offers the update if newer | none | none; the portal shows "Development build: updates are installed without checking a signature." |

"A valid signature over different fields" and "a corrupted signature" are the same case on purpose: the board cannot and need not tell them apart. In every refusal nothing is downloaded (the check happens before the first byte is requested), nothing is written to flash, nothing reboots, the data side is untouched, and the refusal is an ordinary failed check (the existing schedule retries it after an hour; the hourly retries are more than 10 minutes apart, so the panel's "see the portal" message of `problems.h` never starts from them). The recognising phrases for `problemWords` (so a sentence fed back in is recognised as itself, like the other update sentences) are: "update that is not signed", "signed with a key this board does not know", "signature on the firmware update does not check out", "could not check the signature on the firmware update".

### 5.3 Where the result is shown

`GET /api/update` gains two additive members (nothing is removed or renamed; older portals ignore them):

```
"signing": {"required": true,
            "keys": ["f864ea3b", "db10c343"],                  the ids of the pinned keys (public)
            "refused": null | "unsigned" | "malformed" | "unknown_key" | "bad_signature" | "check_failed"},
"available": {..., "signedBy": "f864ea3b" | null}              null only when "required" is false
```

- `refused` holds the result of the **last** check that was refused, and is cleared by the next check that is not refused. The sentence is in `error` (and in the problem list under `ProblemKind::Update`).
- `/status` gains `"updateSigning":"required"` or `"off"`.
- No signature, key bytes or canonical text is ever returned, logged or printed by the board; the portal shows key ids only. (They are public, but one rule is easier to keep than three.)
- Portal text (exact): under the Firmware heading, `Updates must be signed. Accepted keys: f864ea3b, db10c343.`; in a development build `Development build: updates are installed without checking a signature.`; under the version in the notice `Signed with key f864ea3b.` (development build: `Signature not checked (development build).`); when `refused` is set, a warning box titled `An update was refused` with the `error` sentence. The existing "From: <host>" line stays.
- The panel shows nothing new: a refused update never gets as far as the code screen, and no panel message is added for it.

### 5.4 Resources

- Where: the data task (16 KB stack, `live.cpp`), core 0; the render task is on core 1 and is not involved. The 5-second fetch timeouts do not include the verification.
- Time: software ECDSA on the S3 (it has no ECC accelerator): estimated 70 to 400 ms at 240 MHz (published figures for other stacks go from 66 ms to 900 ms; the compile-only proof cannot measure). Budget: up to 2 s without harm (core 0 shares only the portal's task and the Wi-Fi tasks; the idle task's 5 s watchdog is not reached). **The lead measures it** (bench plan in the design note).
- Stack: the check needs about 1.1 KB (the manifest) plus 1 KB (canonical bytes) plus about 2 KB inside mbedTLS, on a task whose TLS handshake already runs the same `mbedtls_ecdsa_verify` through certificate checks. Heap: a few KB, freed before the function returns, and no TLS connection is open at that moment (the manifest download has ended).
- At most 2 verifications per manifest, 1 or 2 manifests per update, 1 daily check plus presses of "Check now". Cheap refusals (steps 3 and 4) cost no crypto.
- Flash: the verification adds about 0.5 KB to the real firmware (measured by compile: 1,479,333 to 1,479,873 bytes, RAM unchanged), because the TLS code already links all of mbedTLS's P-256 verification.
- The reference verification (compiles and links for `esp32:esp32:adafruit_matrixportal_esp32s3`, and gives the right answers for all vectors with mbedTLS 3.6.8 on a laptop) is the function `p256Verify` quoted in the design note. It uses only `mbedtls_ecp_group_load`, `mbedtls_ecp_point_read_binary`, `mbedtls_ecp_check_pubkey`, `mbedtls_mpi_read_binary`, `mbedtls_ecdsa_verify` and `mbedtls_md`.

### 5.5 The order of every check before the boot slot changes

1. Manifest fetched (public route, over HTTPS, at most 2,048 bytes).
2. Manifest rules (`parseManifest`), then signature (5.1). **Nothing is requested or written before this passes.**
3. Strictly newer than the running version and than any version that rolled back (`isOffered`).
4. The panel code (unchanged), then a fresh manifest fetch for the download, **checked again from step 1**. The manifest that governs a download is the one just verified, never an earlier copy.
5. The image streams into the other slot; its SHA-256 is computed while it streams.
6. The byte count equals the signed `size`.
7. The SHA-256 equals the signed `sha256` (this is where the image becomes "the maintainer's image").
8. ESP-IDF image validation (`esp_ota_end`: header, segments, checksum, appended hash) and the app description.
9. The version marker inside the image equals the signed `version` (already done today).
10. Only then `esp_ota_set_boot_partition`.

Because the signed values include `sha256` and the image carries its own version marker, the signature transitively authenticates the image, its version and its size. The release notes and file name are authenticated too.

## 6. The service (Go)

Behaviour, in addition to `firmware-update.md`:

- `firmwareManifest` gains `KeyID string json:"keyid,omitempty"` and `Sig string json:"sig,omitempty"`, after `notes`. The answer is still built from the validated fields (never copied from the file), so the values pass through unchanged and nothing else can slip in.
- Checks (a manifest that fails is "no firmware is published", logged once, as for the other rules): `keyid` and `sig` both empty or both set; `keyid` is 8 lowercase hex; `sig` is 128 lowercase hex; the notes hold no control character; the answer is at most 2,048 bytes.
- **It does not verify the signature** and holds no key: it cannot do more harm than a service that serves an unsigned manifest, and a second verifier would be a second thing to keep in step. The board's check is the only one that counts. The start-up log line says `firmware: version 0.11.0 is published (1412537 bytes), signed with key f864ea3b` or `..., not signed` (the key id is public; never the signature).
- A manifest without the two fields is served exactly as before (v1). The routes stay public: they ignore the device signature headers, so a display whose device key is refused still gets the signed fix. The download route is unchanged.
- Deploying: the manifest is inside the service image, so an old service never serves a new manifest.

## 7. Signing in the build workflow

The image is built in one job. The new steps sit **after** "Write the manifest and the image into service/firmware" and **before** "Keep the firmware as a workflow artifact" and the image build:

```yaml
      - name: Sign the manifest
        env:
          UPDATE_SIGNING_KEY: ${{ secrets.UPDATE_SIGNING_KEY }}   # this step only: no other step, action or artifact sees it
        run: python3 -I tools/sign_manifest.py service/firmware/manifest.json --keys-header arduino/ledmatrix/update_keys.h
      - name: Check the signed manifest (public keys only)
        run: python3 -I tools/sign_manifest.py --check service/firmware/manifest.json --keys-header arduino/ledmatrix/update_keys.h
```

`tools/sign_manifest.py` (Python 3, standard library, plus the `openssl` command line that every runner and Mac has; the macOS system LibreSSL 3.3.6 and Homebrew's OpenSSL 4.1.0 were both tested for key generation, signing and verification, in both directions: signatures made by one verify under the other, under mbedTLS 3.6.8 and under Python's `cryptography`; the runner's OpenSSL 3 uses the same commands):

- Reads the key from the environment variable `UPDATE_SIGNING_KEY` (the PEM text of an EC P-256 private key, or the same PEM as one line of base64: GitHub advises against multi-line structured secrets because its log masking can miss them; one line is masked as a whole) or from `--key-file PATH`; **never from an argument**. It writes the key to a file mode 0600 in a fresh directory mode 0700 (under `$RUNNER_TEMP` when set), runs `openssl dgst -sha256 -sign`, and removes the file in a `finally`. It never prints the key, the signature or the canonical text; it prints one line: `signed version=0.11.0 keyid=f864ea3b`.
- Before signing it checks (one plain sentence and exit 1 on any failure, nothing written): the manifest passes every rule of this page; it carries no signature yet; the image `ledmatrix-<version>.bin` next to it has exactly the manifest's size and SHA-256; the image carries the signing marker `required`; the key is an EC key on P-256; **the key's public point is one of the keys in `update_keys.h`** (the parser reads the `UPDATE_KEY_n` lines strictly). That last rule catches the likeliest mistake: a CI secret that does not match what the firmware pins would produce a release that no display could verify.
- After signing it verifies the signature itself with the public key from the header, using code of its own (a pure-Python P-256 check, a few dozen lines) and the `openssl` command line, writes `manifest.json` atomically with `keyid` and `sig` appended (compact JSON, the five v1 keys first), and leaves the image alone. Running it again on a manifest that is already signed checks it and changes nothing; if it does not verify, it refuses.
- `--check MANIFEST --keys-header FILE` needs no key: it verifies a signed manifest against the pinned keys (used in the workflow right after signing, and by anyone locally).
- If `UPDATE_SIGNING_KEY` is empty or missing the step fails and the workflow publishes nothing. An unsigned release would be refused by every display that checks signatures; failing loudly is the safe choice. (Forks: create your own key and secret, section 8.)
- The private key never reaches the service image (the build context is `service/`; the key is only in `$RUNNER_TEMP`), the artifact (`manifest.json` and the image only) or the log.
- A manifest signed by a key that the previous release did not pin cannot be installed by the displays that run it. The tool cannot know the previous release's keys; the rotation procedure (section 9) keeps one key in common between consecutive releases, and the test bench checks it.

`tools/gen_signing_key.py --out DIR --names A B` creates the keys (it runs `openssl ecparam -name prime256v1 -genkey -noout`; nothing is printed that is secret):
- per name, `DIR/ledmatrix-update-<name>.pem` (mode 0600) and `DIR/ledmatrix-update-<name>.github-secret.txt` (mode 0600, the PEM as one base64 line, the value for `gh secret set UPDATE_SIGNING_KEY < file`);
- it prints, per name, the key id and the `#define UPDATE_KEY_n "04..."` line to paste into `update_keys.h`;
- it refuses an output folder inside a git work tree, refuses to overwrite a file, and reminds the owner of the backup rule (section 8).
- The same by hand: `openssl ecparam -name prime256v1 -genkey -noout -out key.pem`; the public key: `openssl ec -in key.pem -pubout -outform DER | tail -c 65 | xxd -p -c 130`; the key id: `openssl ec -in key.pem -pubout -outform DER | tail -c 65 | shasum -a 256 | cut -c1-8` (all three were run on the Mac's LibreSSL and give the same answers as the vectors' key A).

## 8. Key handling

- **Two keys, two homes.** Key A (current): the GitHub Actions secret `UPDATE_SIGNING_KEY` (repository or an environment secret) plus an offline copy. Key B (spare): **offline only, never in CI**. Both public halves are pinned in every release.
- **Offline copies**: the PEM files and the `.github-secret.txt` lines, in the owner's password manager or on offline media, two places. They are not in the repository, in the cluster, or on the build machine. Losing both private halves is recoverable only by a USB flash (section 9).
- **Hardening that costs nothing**: put the secret in a GitHub Environment (`release`) that only the `main` branch may use, so a branch or a manual run from another branch cannot read it. Optional, a little more friction: require the owner's approval for that environment so that every signing waits for one click. Whoever can push to `main` can change the workflow and so read the secret: key custody is exactly as strong as write access to `main` (this narrows limit L12: a compromised deploy step or registry alone can no longer ship firmware).
- **Never**: print the key (no `set -x`, no `echo`), pass it as an argument, store it in an artifact, an image layer, a log, a comment or a ticket; sign on an untrusted machine.
- **Own builds** (someone who builds their own display): generate your own key with `gen_signing_key.py` (one key is enough: `UPDATE_KEY_COUNT 1`), replace the lines in `update_keys.h` with your public key, set the secret `UPDATE_SIGNING_KEY` in your fork (or run `sign_manifest.py --key-file` by hand when you publish), build your own service image or mount your own `FIRMWARE_DIR`. The public service image carries the maintainer's signed firmware, which a display with your key correctly refuses (`UnknownKey`). For experiments that never leave your desk: `-DUPDATE_REQUIRE_SIGNATURE=0`.

## 9. Rotation, loss and leaks

All with the invariant that consecutive releases share at least one key: **a release must be signed by a key pinned in the release before it AND by a key pinned in itself** (the tool enforces the second half; the procedure the first).

State at the start: releases pin `[A, B]`; CI signs with A; B is offline.

| Situation | What it means | What to do |
| --- | --- | --- |
| **Routine rotation** (say yearly, or on suspicion with no proof of leak) | A gets retired | 1. Generate a new key C offline. 2. `update_keys.h` becomes `[B, C]`. 3. Replace the CI secret with B's key. 4. Release: signed by B, which every display already pins; the release pins `[B, C]`. 5. Offline copy of C; destroy A. B is now the current key (in CI), C the spare. Displays that skipped a release still verify (they pin A and B; releases are signed by B) |
| **A is lost** (CI secret gone, offline copy gone) | No release can be signed by A | The same steps as the routine rotation. No flag day, no USB |
| **A is leaked** | Whoever has A can sign. Dangerous only together with the ability to serve it to a display: the display must be talking to their server (they control your service, DNS or a certificate, or the saved address was changed) | 1. Delete the GitHub secret; check the service, the registry and the GitHub account. 2. The routine rotation, at once. Until a display has installed the release that drops A it still accepts A. **A leaked key does not let anyone attack a display that talks to your own, uncompromised service.** If an attacker has already published a higher version signed by A and a display installed it, that display cannot be offered your fix (not newer): USB flash |
| **B is lost or leaked** (the spare) | You lose the recovery path for A | Release signed by A that pins `[A, C]` (a new spare). Check that no copy of B is in CI |
| **A and B are both lost** | No installed display accepts any update | USB flash a build with new keys (`install.md`), every display |
| **A display two rotations behind** | It pins `[A, B]` and the newest releases are signed by C | Not recoverable over the air: USB flash. The service offers only the newest release |
| **A bad (but genuine) release** | Not a key problem | Publish a newer version; displays that rolled back skip the bad one (existing rule). There is no per-release revocation |

The key ids appear in `/api/update`, so the owner can see which keys a display trusts.

## 10. Downgrade, replay, the escape route, and what remains

- **Offered versions**: a service-offered update is offered only when its version is strictly newer than the running version, and strictly newer than any version that already failed here (`isOffered`, unchanged). So a replayed old manifest, or a genuine old release, is never installed over a newer one. The running version is the floor; nothing is stored.
- **No `expires` field.** An expiry would only help against a stale manifest being served to hide a newer release, and a stale manifest is still a genuine one. The price is real: if the maintainer's CI stops for a few weeks, every display would refuse the current release, and the clock it is checked against is plain NTP (L10). The downgrade rule is what matters, and it does not need a clock.
- **No stored version floor** (NVS). It would add nothing: the only older images that can run are the previous slot (rollback, which is the safety net) and one uploaded by hand (code-only by design). A persistent floor would also lock the owner out of a legitimate re-flash path.
- **The `.bin` upload stays code-only**: it needs the 6-digit panel code and the image checks, and it cannot be signed (there is no manifest). It accepts any valid ESP32-S3 app image, including one with other keys. Residual: someone who is at the panel and also on the LAN can install any firmware, as someone with a USB cable can. This is the escape route the owner asked for; signing it would take away the way out when keys are lost.
- **Residual risks** (stated plainly in the docs): displays still on 0.10.x have no check until they install the first signed release; whoever holds the CI secret can sign (so the GitHub account and write access to `main` are the root of trust, L12); a genuine older release that is newer than what a display runs can be offered to it, and an attacker who controls the service can withhold updates (freeze) or show an older release; a leaked key plus control of the service defeats the scheme until the rotation reaches the display; a thief with USB access can read and rewrite flash anyway (L9); the pinned keys sit in unprotected flash (no secure boot); randomised ECDSA signatures depend on the signer's random numbers (the openssl CLI's, a standard, audited source).
- **What this closes**: L2 (changing the saved service host gets nothing installed but genuine releases) and L3 (DNS, a certificate authority or the deploy step). Both only for displays that already run a signing build.

## 11. The bootstrap

0.10.x boards (no key, no check) read the new manifest with their existing parser: the two extra fields are ignored. So the first signed release (0.11.0) is offered by the same manifest, installed through the unsigned check **once**, and from 0.11.0 on the board enforces. What is needed before that release is built: both public keys pasted into `update_keys.h` (the build fails without them), the secret set in GitHub, the offline copies made. The owner's own display can skip the one-time trust entirely by flashing 0.11.0 over USB. The signed path is then proved by the next release (0.11.1, any change).

## 12. Constraints

- Public repository: placeholders only; test keys only, in the vectors file; no real key, ever; the private half of the vector keys was deleted.
- The display never stalls more than 1 s: verification on the data task, no lock held while it runs, no HTTPS, no flash write on the render task.
- The refusal is soft: nothing wiped, nothing rebooted, no effect on the device-key side (`device-auth.md`). The manifest and the download stay open to signed and unsigned requests.
- ArduinoJson 7 stores `const char[N]` members by pointer: serialise `signing` and `available.signedBy` under the lock that guards the state. No string with a control character goes into JSON (key ids are hex; notes are validated).
- Never log or print a key, a signature, the canonical bytes, the update code or the master secret.

## 13. Vectors

`manifest-signing-vectors.json`: three throwaway test keys (A and B are the "pinned" ones, C is an outsider), the scheme in words, and 32 vectors. Each has the full manifest as the service would answer it, the expected result (`ok`, `unsigned`, `malformed`, `unknown_key`, `bad_signature`, `manifest_invalid`) and, where a signature exists over it, the canonical bytes, their hex and their SHA-256. Negative cases: a wrong key id, a signature of an outsider that claims a pinned id, one flipped bit in `r`, in `s`, in `sha256`, a different version, size, hash and notes (one letter, one trailing space), `r = 0, s = 0`, `r = n`, `s = n`, malformed fields of every kind, a missing field. The Go test, the C++ host test (with a stand-in verifier keyed on the vector triples) and the Python test all read this file; the bench test on the board uses the same bytes. How they were checked: openssl (4.1.0 and LibreSSL 3.3.6) and the Python `cryptography` package 50.0.2 agree on every one; so does a pure-Python verifier, and so does mbedTLS 3.6.8 (the board's core carries 3.6.7) through exactly the calls of `p256Verify`.
