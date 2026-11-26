# Signed firmware manifest: design (ticket 42), 26 Nov 2026

Question (owner, 26 Nov 2026): how are firmware updates signed so that a display accepts only updates published by the maintainer, even if someone changes the saved service address? The owner chose **"sign the update manifest"**; this note makes every sub-decision concrete, conservative and defensible, with the alternatives rejected, because the owner was asleep and the lead takes the decisions from it. The contract another agent implements from is `shared/manifest-signing.md`; the known-answer vectors are `shared/manifest-signing-vectors.json`. Related: #21 (firmware update, "no signature for now", reopened here), #37 (device identity: public manifest and download, soft refusals), #33 (the slice that built the update path), L2, L3 and L12 of `docs/security.md`.

Setup of the proofs: arduino-esp32 **3.3.13**, mbedTLS **3.6.7** in the core (`CONFIG_MBEDTLS_ECDSA_C=y`, `ECP_DP_SECP256R1_ENABLED=y`, `ECP_NIST_OPTIM=y`, `HARDWARE_SHA=y`, `HARDWARE_MPI=y`; `ECP_FIXED_POINT_OPTIM` and `ECP_RESTARTABLE` off), FQBN `esp32:esp32:adafruit_matrixportal_esp32s3`, arduino-cli 1.6.0. Nothing was uploaded; the board was not touched.

## Answers in one line each

1. **Algorithm: ECDSA P-256 with SHA-256, verified by the mbedTLS already in the core.** The verify code is already linked into every TLS build (it checks server certificates), so it costs **about 0.5 KB of flash in the real firmware (+540 bytes measured; +1,168 in the small sketch, which includes its test vectors and a printf)**, no extra RAM, and **no new third-party code**. Ed25519 through Monocypher costs +14,260 bytes and a vendored library; through TweetNaCl +6,344 bytes and is far slower. Verify time on the S3 could not be measured: estimated 70 to 400 ms, which is harmless on the data task.
2. **Signed: the values of version, file, size, sha256 and notes**, as six text lines under `ledmatrix-update-v1`, hashed with SHA-256. The manifest gains `keyid` (8 hex) and `sig` (128 hex, r then s). Not the JSON text, not the image.
3. **Key custody:** GitHub Actions secret `UPDATE_SIGNING_KEY` (one line of base64 PEM) plus an offline copy; a second **spare key that never goes into CI**. Signing is one step in the existing workflow with the secret scoped to that step; the key is never in an image or artifact. Created with `tools/gen_signing_key.py` (or four documented openssl commands). Works with the Mac's own LibreSSL.
4. **Rotation and loss:** the firmware pins two keys (current and spare). Rotating means swapping the spare into CI and releasing a version that pins the spare and a fresh spare, signed by the spare. No flag day, no USB, unless both keys are lost.
5. **Own builds:** replace the two lines in `update_keys.h`, sign with your own key; `-DUPDATE_REQUIRE_SIGNATURE=0` (proved to reach the sketch) switches enforcement off for development builds, and a release cannot be built or signed that way by accident.
6. **Replay and downgrade:** the existing "strictly newer, and newer than a failed version" rule is the whole defence. No `expires`, no stored floor; reasons below. Residual risks are listed honestly.
7. **Bootstrap:** 0.10.x boards ignore the two new fields, so the first signed release installs through the unsigned check once; from then on enforcement is on. The owner's own display can skip even that by flashing 0.11.0 over USB. The board's behaviour for every case (unsigned, malformed, unknown key, bad signature, valid signature over other fields, could not check) is a table in the contract with plain-English sentences.
8. **The `.bin` upload stays code-only.** Residual risk listed.
9. **The guarantee holds:** the manifest and the download stay public; verification needs neither the device key nor the service's cooperation.
10. **Verification** runs in `fetchManifest()` on the data task (core 0), no lock held, before the notice is shown and before a single byte is requested; the SHA-256 of the image is compared with the **signed** value; the order of checks before the boot slot changes is in section 5.5 of the contract.

## 1. Evidence

### 1.1 What the board's core can do, proved by compile only

Four throwaway sketches (kept in the lead's scratch folder, `.../scratchpad/overnight/design-signing/sketches/`), each a TLS client (`NetworkClientSecure` with the built-in CA bundle, so the same mbedTLS parts are linked as in the real firmware) plus one verification. Built with `arduino-cli compile --fqbn esp32:esp32:adafruit_matrixportal_esp32s3 --build-path <scratch>/b/<name> <sketch>`:

| Sketch | Program storage | Delta vs baseline | What it adds |
| --- | --- | --- | --- |
| `sig_base` | 688,094 bytes | , | TLS client only |
| `sig_ecdsa` | 689,262 bytes | **+1,168** | P-256 verify with mbedTLS (`mbedtls_ecdsa_verify`, `mbedtls_md` SHA-256) and a vector, `Serial.printf` |
| `sig_mono` | 702,354 bytes | +14,260 | Ed25519 verify, Monocypher 4.0.3 (`crypto_ed25519_check`, with its SHA-512) |
| `sig_nacl` | 694,438 bytes | +6,344 | Ed25519 verify, TweetNaCl 20140427 (`crypto_sign_open`) |
| **real firmware** (commit 4fd536d, unchanged) | 1,479,333 bytes (70%), 106,064 bytes RAM | , | the baseline the project builds today |
| **real firmware + the same P-256 verify** | 1,479,873 bytes (70%), 106,064 bytes RAM | **+540** (RAM +0) | `sigprobe.cpp` added to a copy and called from `setup()` |

Why ECDSA is nearly free: `nm` on the baseline ELF shows `mbedtls_ecdsa_verify_restartable`, `mbedtls_ecp_muladd`, `mbedtls_ecp_group_load`, `mbedtls_ecp_check_pubkey`, `mbedtls_ecp_point_read_binary`, `mbedtls_sha256` already linked, because the TLS and x509 code that is in every build calls them.

Time cannot be measured without the board. Evidence for the estimate: on a laptop (relative only) mbedTLS 3.6.8 verifies a P-256 signature in 0.82 ms, Monocypher an Ed25519 one in 0.10 ms, TweetNaCl in 1.46 ms. The board differs: 32-bit limbs in mbedTLS (about four times more limb work than a 64-bit laptop), a 240 MHz in-order core, no ECC accelerator on the S3, and the hardware MPI unit that may or may not help for 256-bit operands. Published figures for similar stacks range from 66 ms (a vendor benchmark of another library on the S3 at 240 MHz) to about 900 ms (an ESP32 at 160 MHz with mbedTLS, forum report). Estimates for this design: **ECDSA 70 to 400 ms**, Monocypher 10 to 40 ms, TweetNaCl over 1 s. The design tolerates 2 s. **The lead measures it** with the sketch `sig_ecdsa`, which prints the microseconds of one valid and one tampered verification, the free heap and the stack high-water mark.

### 1.2 The vectors were checked by five implementations

`shared/manifest-signing-vectors.json` was made with a throwaway key in the scratch folder, signed with the **macOS system openssl (LibreSSL 3.3.6)**, and every positive and negative claim was re-checked by: OpenSSL 4.1.0 (CLI), LibreSSL 3.3.6 (CLI), the Python `cryptography` package 50.0.2, a pure-Python P-256 verifier written for this purpose, and **mbedTLS 3.6.8** (Homebrew; the board's core carries 3.6.7) through the exact calls of the reference function below. 18 verifiable cases (5 valid, 13 invalid), 0 mismatches in each. The RFC 8032 test vector 2 was checked with both Ed25519 libraries (both accept it and reject a flipped bit).

### 1.3 Tooling on the owner's Mac

`/usr/bin/openssl` is LibreSSL 3.3.6. It generates and signs with P-256 (`ecparam -genkey`, `dgst -sha256 -sign`, `ec -pubout`), and it **cannot do Ed25519 at all** (`genpkey -algorithm ED25519`: "Algorithm ED25519 not found"). The Homebrew openssl 4.1.0 can. The GitHub runner (`ubuntu-latest`, OpenSSL 3) can do both. The Python `cryptography` module exists on this Mac but is not on a stock runner, so nothing relies on it.

### 1.4 The reference verify function (compiles; correct for every vector)

```cpp
#include <mbedtls/bignum.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/md.h>

// pub: 65 bytes (0x04 || X || Y). hash: SHA-256 of the canonical bytes. sig: r (32 bytes) || s (32 bytes), big endian.
static bool p256Verify(const uint8_t pub[65], const uint8_t hash[32], const uint8_t sig[64]) {
  mbedtls_ecp_group grp; mbedtls_ecp_point Q; mbedtls_mpi r, s;
  mbedtls_ecp_group_init(&grp); mbedtls_ecp_point_init(&Q); mbedtls_mpi_init(&r); mbedtls_mpi_init(&s);
  bool ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0
         && mbedtls_ecp_point_read_binary(&grp, &Q, pub, 65) == 0
         && mbedtls_ecp_check_pubkey(&grp, &Q) == 0
         && mbedtls_mpi_read_binary(&r, sig, 32) == 0
         && mbedtls_mpi_read_binary(&s, sig + 32, 32) == 0
         && mbedtls_ecdsa_verify(&grp, hash, 32, &Q, &r, &s) == 0;   // checks 1 <= r,s < n itself
  mbedtls_mpi_free(&r); mbedtls_mpi_free(&s); mbedtls_ecp_point_free(&Q); mbedtls_ecp_group_free(&grp);
  return ok;
}
```

The production function returns three values instead of a bool: valid when `mbedtls_ecdsa_verify` returns 0, invalid when it returns `MBEDTLS_ERR_ECP_VERIFY_FAILED` or `MBEDTLS_ERR_ECP_BAD_INPUT_DATA` (or the point or integers are malformed), and **could not check** for anything else (an allocation failure, an internal error). That way running out of memory is never reported as an attack.

## 2. The decisions, with the alternatives rejected

### D1. Algorithm: ECDSA P-256 + SHA-256 via mbedTLS

Reasons: no new code on the board (mbedTLS is already trusted for every HTTPS connection; if its P-256 verify were wrong, TLS would fail), the smallest flash cost (+540 bytes in the real firmware), kept patched by core updates (ticket #51 / the core-update slice), the tools run on every machine the owner has (LibreSSL included), 128-bit security, fixed 64-byte signatures that fit the 2,048-byte manifest limit that is already in every shipped board.

Rejected:
- **Ed25519 with Monocypher 4.0.3** (BSD-2-Clause or CC0, about 3,000 lines): the nicest scheme on paper (deterministic signatures, no encodings, fast: estimate 10 to 40 ms). But +14 KB flash, 3,000 lines of vendored crypto code the project would have to track (Monocypher 4.0.3 of 15 June 2026 fixed a timing leak in EdDSA signing: exactly the kind of fix a vendored copy needs a firmware release for), a new line in `THIRD_PARTY_NOTICES.md`, and **the Mac's own openssl cannot even generate an Ed25519 key** (the owner would need Homebrew's openssl or a Python package for the one operation that must not go wrong). Speed is not a requirement here (once a day, off the render core). If the owner prefers it anyway, nothing else in the design changes except the algorithm rows of section 1 of the contract (32-byte keys, 64-byte signatures, the same hex fields): that is a veto he can exercise in the morning at the price of about half a day more work.
- **Ed25519 with TweetNaCl** (public domain, 800 lines, compact): +6 KB, but 64-bit integer arithmetic emulated on a 32-bit core makes verification likely over a second, and it is the same "vendored crypto" objection.
- **RSA-2048 or -3072 with mbedTLS (PKCS#1 v1.5 or PSS):** verification is fast (the S3 has an RSA/MPI accelerator) and also already linked, but the signature is 256 bytes (512 hex characters) for RSA-2048 and **768 bytes (1,536 hex characters) for RSA-3072**: the signed manifest would exceed the 2,048-byte limit that the already-shipped 0.10.x boards enforce, and the first signed release must still be readable by them. RSA-2048 fits but gives only about 112-bit security and a 270-byte key in the firmware.
- **HMAC with a shared secret** (the device-key scheme): the verifier holds the signing secret, so anyone who can read a display's flash could forge releases. Public-key signatures are the point.
- **mbedTLS PK API with DER keys and DER signatures** (`mbedtls_pk_parse_public_key`, `mbedtls_pk_verify`): works, but pulls an ASN.1 parser into the path that handles untrusted bytes. The raw 65-byte point and the fixed-width signature avoid it.

### D2. What is signed, and the canonical bytes

Decided: `version`, `file`, `size`, `sha256`, **and `notes`**, as the decoded values in a fixed text layout, prefixed by the line `ledmatrix-update-v1`; hashed with SHA-256; `keyid` and `sig` are not part of it. `sig` is 128 lowercase hex characters (r then s). `keyid` is the first 8 hex of the SHA-256 of the 65-byte public key.

- **Why the values, not the JSON text:** the service re-encodes the manifest (Go escapes `<`, `>`, `&`), and JSON has several valid spellings of the same value. Signing the values removes every canonicalisation question. Each side builds the same six lines from what it has decoded.
- **Why notes are signed:** the portal shows them in a notice that asks the owner to press Update and type the panel code. If they were unsigned, whoever sits between the service and the board could dress a genuine release in a note of their own choosing. The price is one rule (no control characters, which the tools already guarantee), and the vectors cover accents, a check mark and an emoji. The owner may veto: leave `notes` out of the canonical bytes and treat them as display-only text; nothing else changes.
- **Why `file` is signed even though it equals `ledmatrix-<version>.bin`:** the board builds a request path from it. Signing every field the board reads or acts on is a rule that does not need an exception.
- **Why the prefix:** domain separation, so that a signature made for this purpose cannot be taken from, or used for, anything else, and a later scheme cannot be confused with this one.
- **Why hex, and why r||s:** one valid text form per value and a fixed length checked before any crypto runs; hex is already used for the image hash; no base64 decoder, no ASN.1.
- **Why the key id is not signed:** it only selects the key to try; if it is changed the signature fails (wrong key) or the key is unknown. Nothing is gained by signing it.
- **Why no algorithm field:** an algorithm named in attacker-controlled data is how downgrade attacks start. The pinned key fixes the algorithm; a future one gets a new prefix and new field names.
- **Rejected:** signing the image itself (a detached `.sig` of the `.bin`: the board would download 1.4 MB before it could say no, and the version and notes would still be unauthenticated); an ESP-IDF "signed app image" (secure boot v2: needs eFuse secure boot, not planned, and the TinyUF2 bootloader is not ours); pinning the service's TLS key (ties updates to how the host rotates certificates, and the public image's users control their own host).

### D3. Key custody and CI signing

Decided: private key as the repository (or environment) secret `UPDATE_SIGNING_KEY`, stored as one line of base64 of the PEM (GitHub advises against multi-line structured secrets because its log masking can miss them; a single line is masked as a whole); an offline copy the owner backs up; **plus a spare key, offline only**. The workflow step "Sign the manifest" comes right after the manifest is written and before the artifact upload and the image build; it has the secret in its own `env:` only, so no other step or action sees it. `tools/sign_manifest.py` is Python's standard library plus the `openssl` command line (no new dependency): it writes the key to a 0600 file in a 0700 directory, signs, removes the file, verifies the result against the public key **pinned in `update_keys.h`**, and refuses to publish if the key does not match what the firmware pins, if the image does not match the manifest, or if the image was built with enforcement off.

- **Fail closed:** an empty secret fails the workflow; nothing is published. (A release that cannot be verified by anything is worse than no release.)
- **Why not offline-only signing** (the owner signs each release on his laptop): safest against a CI compromise, but every release needs a manual step and the manifest sits inside the image that CI builds, so the flow would have to be redesigned. The spare key gives the recovery property without the weekly chore.
- **Why not a cloud KMS or Sigstore:** a new dependency and account for one display; Sigstore's certificate chain would have to be parsed on the board.
- **What the owner gets for free:** an Environment (`release`) restricted to `main` so that a run from another branch cannot read the secret; optionally a required approval, which turns each signing into one click. Both are listed under things to veto.
- **Honest consequence:** whoever can push to `main` can change the workflow and so read the secret. Key custody is exactly as strong as write access to `main` (L12), but a stolen registry login or a compromised cluster deploy can no longer ship firmware, which is a real improvement over today.

### D4. Rotation and loss: two pinned keys

Decided: `update_keys.h` pins one or two keys. A release must be signed by a key pinned in the release before it and by a key pinned in itself, which the sign tool enforces for the second half. Normal state: `[A, B]`, CI signs with A, B is offline. Rotation: swap B into the secret, pin `[B, C]`, release, retire A. No waiting period and no special flag: B was already pinned by every display. A lost A is the same procedure; a leaked A is the same procedure done at once, with the service checked first (a leaked key only hurts a display that is talking to someone else's server). Both lost: USB flash. A display two rotations behind needs USB, which is the price of keeping the pinned list at two (more slots, more code, a longer window for an old leaked key).

- **Rejected: one key only.** A lost or leaked key would mean a USB flash of every display, which on a mounted display is what the update path is for.
- **Rejected: key id as free text** ("2026a"): can drift from the key; the derived id cannot.
- **Rejected: a signed key list ("key rollover manifest") fetched from the service:** another trust anchor and another parser for a problem that a pinned pair solves.
- **Rejected: pinning N keys:** each extra key is an extra private key someone must guard; two is the smallest number that allows recovery.

### D5. People with their own build

Decided: they replace `update_keys.h` with their own key (one is enough) and sign with it (their fork's secret, or `sign_manifest.py --key-file` by hand); the public service image carries the maintainer's signed firmware, which their display correctly refuses with "signed with a key this board does not know". A build flag `UPDATE_REQUIRE_SIGNATURE` (default 1) disables enforcement for development; it is documented as "never ship this". Safeguards: with enforcement on and no key the build fails (`static_assert`); the image carries a marker `required` or `off` (like the version marker) and `sign_manifest.py` refuses an image that does not say `required`; the portal prints "Development build: updates are installed without checking a signature." so such a display cannot pass for a protected one.

- **Rejected: a runtime setting in the portal to switch the check off:** anyone on the LAN could do it, which would remove the very protection that closes L2.
- **Rejected: a build with an empty key list meaning "off":** too easy to ship by accident.

### D6. Replay and downgrade

Decided: nothing beyond the existing rule. A service-offered update is offered only if its version is strictly newer than the running one and than any version that rolled back. The running version is the floor; the signature adds that nobody can offer a version that was never released.

- **`expires`: no.** It protects only against being shown a stale-but-genuine manifest (the freeze attack), which the version rule already makes harmless to the extent it can be (a stale manifest can only offer something newer than what the board runs). The cost is that every display would refuse the current release if CI stops for a few weeks, and it needs a trustworthy clock, which is plain NTP (L10). A design whose failure mode is "the owner's displays stop updating unless he re-signs every month" is not conservative.
- **A stored version floor in NVS: no.** It adds nothing: the only older images that can run are the previous slot (rollback, the safety net) and an uploaded `.bin` (code-only by design), and a floor in NVS would be erased by exactly the recovery steps that might be needed.
- **Residual risks, stated honestly:** an attacker who controls the service (or the saved address) can **withhold** updates (a freeze) and can offer any genuine release newer than the running one even if a newer one exists (an older-but-still-newer release, with known bugs); there is no per-release revocation (the answer is to publish a newer version); a leaked key plus control of the server defeats the scheme until the rotation reaches the display; a version number `9999.9999.9999` signed with a leaked key would poison a display (it could not be offered a fix because nothing is newer): USB flash.

### D7. The bootstrap, and what the board does for each manifest

Decided: boards on 0.10.x ignore unknown manifest keys (`parseManifest` reads only what it knows), so the first signed release is the same manifest plus two fields, installed once without a signature check. From that release on, enforcement is on. The owner's own display needs no trust at all: flash 0.11.0 over USB. The table in section 5.2 of the contract gives, for each case, the result, the board's action and the plain sentence: **unsigned** (no `keyid`, no `sig`), **malformed** (one field, or a wrong shape), **unknown key**, **bad signature** (which includes a valid signature over other fields), **could not check** (out of memory), and the development build. In every case: nothing downloaded, nothing written, no reboot, the notice withdrawn, the sentence in the problem list and in `error`, and an additive `signing.refused` code in `/api/update` so the portal can show a warning box. The cheap refusals (unsigned, malformed, unknown key) happen before any cryptography.

- **Rejected: accepting unsigned manifests for a grace period:** the whole point is that someone who changed the saved address cannot get an unsigned update installed; a grace period is a hole of known width.
- **Rejected: "trust on first use" of the key** from the first manifest: a manifest that carries its own key proves nothing.

### D8. The `.bin` upload

Decided: unchanged: the 6-digit panel code, the image checks, no signature (there is no manifest to carry one). It is the way out when keys are lost, when a release is bad, and when someone builds their own firmware. **Residual risk:** whoever is both on the LAN and able to read the panel (so, physically present) can install any firmware, exactly as someone with a USB cable can; and an uploaded image can pin other keys. Rejected: requiring a signed manifest alongside the upload (it takes away the way out when the keys are the problem, and the person at the panel could flash by USB anyway); worth revisiting only together with secure boot.

### D9. The "refused key still gets a fix" guarantee

Holds by construction: the manifest and the download routes ignore the device signature headers (unchanged), verification uses only the pinned public keys and no information from the service, and a refusal of any kind never touches the data side. The service does not verify anything and holds no key.

### D10. Verification flow, resources, display

Decided as in the contract, section 5: one place (`fetchManifest()` in `ota.cpp`), which returns only manifests that passed, so no path can forget the check; the data task on core 0 with no lock held; the 16 KB stack that already carries TLS handshakes (which run the same ECDSA verify for certificates) and a few KB of transient heap with no TLS connection open; the render task (core 1) never involved. The check runs **before the notice is shown and before any byte is requested**, and again on the fresh manifest of the download; the SHA-256 of the image is compared with the signed value; the version marker inside the image is compared with the signed version; only then the boot slot changes. `/api/update` gains `signing` and `available.signedBy` (additive); the portal shows the accepted key ids, "Signed with key ...", a warning box for a refusal, and a development-build warning; the panel needs nothing.

## 3. Risks and what may go wrong

| Risk | Likelihood | Mitigation |
| --- | --- | --- |
| Verification is slower than estimated (over 1 s) | low | runs on core 0 off the render task; budget tolerates 2 s; measured first in the bench plan; if it is far worse, the fallback is Monocypher (one decision, one file) |
| A CI secret that does not match the pinned key ships an unverifiable release | medium (human error) | `sign_manifest.py` refuses unless the key's public point is in `update_keys.h`; `--check` re-verifies; the bench plan checks the real release |
| A release built with enforcement off | low | image marker; the sign tool refuses it; the portal warns |
| Heap shortage turns into a false "attack" report | low | the three-valued verifier: "could not check" is a different sentence |
| The owner loses both keys | low | two offline copies in two places; the recovery is a USB flash |
| Notes signed: a corner case in UTF-8 or JSON escapes breaks verification of a genuine release | low | vectors for accents, a check mark and an emoji; the service re-encodes but the values are compared; the signer refuses text that is not valid UTF-8; a mismatch fails closed |
| ECDSA signing depends on the CI runner's random numbers | very low | openssl's CSPRNG; a bad nonce leaks the key, which the rotation procedure covers |
| 0.10.x displays stay unprotected | certain until updated | the bootstrap; the owner's display via USB; docs say so |
| Two rotations behind needs USB | low | documented; one display |

## 4. What the owner may want to veto in the morning

1. **ECDSA P-256 instead of Ed25519.** (Ed25519 is +14 KB, a vendored library and no working keygen on the Mac's system openssl; same plan otherwise.)
2. **Notes are signed.** (Leave them unsigned and treat them as display-only.)
3. **A second, offline spare key.** (One key is simpler; a lost or leaked key then means a USB flash of every display.)
4. **No `expires`, no stored version floor.**
5. **CI fails closed** when the secret is missing: no service image gets published at all, not even a service-only fix, until the secret exists. (Alternative: a repository variable that lets the step skip; rejected because an unsigned firmware is then one misconfiguration away.)
6. **A GitHub Environment restricted to `main`, with or without a required approval per release.** Recommended: the restriction. Optional: the approval.
7. **The owner's own display is flashed by USB with the first signed release** instead of taking it over the air.
8. **The uploaded `.bin` stays unsigned.**
9. **Wording**: "Signed with key f864ea3b", "An update was refused", and the three refusal sentences.
10. **The build flag name** `UPDATE_REQUIRE_SIGNATURE`.

**Morning actions that only the owner (or the lead on his behalf) can do**, once the slice is merged: run `python3 -I tools/gen_signing_key.py --out ~/ledmatrix-keys --names A B` (outside the repository); `gh secret set UPDATE_SIGNING_KEY < ~/ledmatrix-keys/ledmatrix-update-A.github-secret.txt` (reads the file, prints nothing); paste the two `#define UPDATE_KEY_n` lines into `arduino/ledmatrix/update_keys.h`; make two offline copies of both PEM files; (optionally) create the `release` environment. The key is never typed, printed or pasted anywhere else.

## 5. Implementation plan, in order

Slice #50 (signed manifest), after this design. The dependencies run left to right; items inside an area can be done in the order listed. The contract's vectors are the shared test.

### 5.0 contracts (first, small)
`shared/firmware-update.md`: replace the "Where the update comes from (known limit)" paragraph with a pointer to `manifest-signing.md` and the sentence "the integrity check is the signed manifest plus the board's SHA-256 comparison"; add `keyid` and `sig` to the manifest example, and the 2,048-byte answer limit. (Left untouched here: other builders edit that page in parallel.)

### 5.1 tools and CI
1. `tools/update_signing.py`: the module the scripts share (loaded by explicit path because `python3 -I` drops the script's folder from `sys.path`): `canonical_bytes(manifest)`, hex rules, `key_id(pub)`, `der_to_raw` / `raw_to_der`, a pure-Python P-256 verifier (about 40 lines: it is the reference that proves the signer's output and the vectors without trusting only openssl), `parse_keys_header(path)` (strict).
2. `tools/gen_signing_key.py` as in contract section 7.
3. `tools/sign_manifest.py` (sign and `--check`) as in contract section 7, including the image-marker check, the pinned-key check, the 0600 temp file and the `finally`.
4. `tools/test_sign_manifest.py`: reads the vectors (canonical bytes, key ids, every `ok` verifies with the pure-Python verifier, every `bad_signature` does not, every shape case classifies as expected); makes a fresh key at run time with `openssl` (skipped with a plain message when openssl is missing; the vectors are always run), signs a generated manifest and image, verifies, then tampers; checks that the secret never appears on stdout or stderr and the temp file is gone; mutation check: change the prefix, the notes handling or the key id rule and the test fails. No private key is committed.
5. `.github/workflows/service.yml`: the two steps of contract section 7 after "Write the manifest and the image"; add `python3 -I tools/test_sign_manifest.py` and the new host test to the test steps; a "Check the compiled image" extension for the signing marker; add `tools/sign_manifest.py`, `tools/update_signing.py` and `arduino/ledmatrix/update_keys.h` to the `paths:` filter. Optionally `environment: release`.
6. `.gitignore`: nothing needed (the keys never exist in the repository); add a note to `service/firmware/README.md` that the manifest there is signed in CI.

### 5.2 service (Go)
1. `service/firmware.go`: the two fields, the shape checks, the control-character rule for notes, the 2,048-byte answer limit, the log line with the key id (never the signature). The answer built from validated fields in the order of contract section 3.
2. `service/firmware_test.go`: a v1 manifest answers exactly as before (no `keyid`, no `sig`, no nulls); a v2 manifest passes both values through unchanged and in order; each malformed shape from the vectors is "no firmware is published"; notes with a control character; an answer over 2,048 bytes; the routes still ignore the device headers (signed and unsigned requests get the same manifest); the log line carries the key id and no signature. Read `shared/manifest-signing-vectors.json` for the shapes. Run `gofmt -l .`, `go vet ./...`, `go test -count=1 -race ./...`.
3. `service/README.md`: two lines on the new fields and on how to publish your own signed firmware.

### 5.3 firmware
1. `arduino/ledmatrix/update_keys.h` (contract section 4) with the owner's two public keys once generated; until then the test keys are **not** put there: the file holds the real keys or the build fails, which is the point. For development, `-DUPDATE_REQUIRE_SIGNATURE=0`.
2. `arduino/ledmatrix/updatesig.h`: pure logic with no Arduino or ESP includes, like `otalogic.h`: lowercase-hex decode, the shape classification (`Unsigned`, `Malformed`), `canonicalBytes(const Manifest&, char *out, size_t cap)`, key-id derivation, the pinned list, `checkSignature(const Manifest&, const Keys&, Crypto&)` with the result enum of the contract, and the `Crypto` interface (`sha256`, `p256Verify` returning valid / invalid / could-not-check), in the style of `devauth::Signer`.
3. `otalogic.h`: `Manifest` gains `keyid[9]`, `sig[64]` and a `signedBy` text; `parseManifest` reads the two optional fields (it still ignores other unknown keys) and enforces the new "no control character in notes" rule; new `Fail` values `Unsigned`, `BadSignature`, `UnknownKey`, `SignatureCheckFailed` with their `failRaw` texts.
4. `problems.h`: the four `PROBLEM_RAW_UPDATE_*` macros and sentences of contract section 5.2 (each at most 159 characters: the list's `why` field is 160), recognised in the existing update block of `problemWords`, plus the detail phrase for the malformed case.
5. `ota.cpp`: the real `Crypto` (the function of section 1.4 plus `mbedtls_md` for SHA-256); the check inside `fetchManifest()` after `parseManifest`; `failCheck` withdraws the notice for the new failures; `notice` keeps `signedBy`; the image marker `\x01LEDMATRIX-FW-SIGNING\x02required\x03` (or `off`) embedded like the version marker; `buildJson` adds `signing` and `available.signedBy` under the lock (ArduinoJson 7 keeps `const char[N]` by pointer: serialise under the lock; key ids are copied into the notice, which is under the lock).
6. `net.cpp` / `/status`: `"updateSigning":"required"|"off"`.
7. Compile: flash must stay well under 2,097,152 bytes (expected +1 to 3 KB with all of this: the verify itself is +540 bytes); the sketch folder keeps `partitions.csv`.

### 5.4 portal and panel text
1. Portal template (the Firmware section): the key line, the "Signed with key" line in the notice, the refusal warning box, the development-build line (exact strings in contract section 5.3). They must read on a phone. Regenerate with `python3 tools/build_web.py` and commit `arduino/ledmatrix/portal_page.h` and `preview/index.html`; run `python3 tools/check_margins.py` only if a screen or the renderer changed (neither does).
2. Panel: no change.

### 5.5 tests
- Host (C++): `tools/test_updatesig.cpp` reads `shared/manifest-signing-vectors.json` with ArduinoJson, like `test_deviceauth.cpp`: canonical bytes equal the vectors' (and their SHA-256 equals `canonical_sha256`, computed in the test with its own SHA-256), key ids equal, every vector classified as expected through `checkSignature` with a stand-in `Crypto` that says "valid" only for the triples (key, digest, signature) of the `ok` vectors; table of every enum to its raw text to its sentence (extend `test_problems.cpp`); `parseManifest` rule for control characters; the enforcement-off path; "could not check" is not "bad signature"; mutation checks (change the canonical order, drop notes from the bytes, skip the key-id check, swap two key slots) must each fail a test. Optional, where mbedTLS headers exist (the Mac has `brew install mbedtls@3`): the same vectors through the real `p256Verify` (`-DWITH_MBEDTLS`), as the scratch program `host/verify_mbedtls.c` already does.
- Python: `tools/test_sign_manifest.py` (above).
- Go: `service/firmware_test.go` (above).
- Existing tests keep passing: `test_otalogic`, `test_deviceauth`, `test_problems`, `test_cycle`, `test_protect`, `test_make_manifest.py`, `test_provision.py`, `test_burn_key.py`.
- On the board: the bench plan below.

### 5.6 docs (done at the end by the doc writers from the builder's notes)
`docs/security.md` (1.4 "The firmware routes", 1.7 "Update checks", the residual limits L2, L3 and L12 rewritten as "closed for displays with signing, still open for 0.10.x", a new section on the signing key and its rotation, the owner's checklist), `docs/operations.md` (firmware updates, rotation steps), `docs/install.md` and `docs/development.md` (building with your own key, `UPDATE_REQUIRE_SIGNATURE`, the release steps), `docs/glossary.md` (signature, public key, key id, pinned key, ECDSA), `docs/architecture.md` (the update chain), `docs/hosting.md` (the public image carries signed firmware; a fork needs its own key), `README.md` (one line). The facts are in the contract; "tested" claims must come from the bench plan.

## 6. Bench plan (for the lead; nothing here was done on the board)

Do 0 to 2 first: they prove the estimate and the plumbing before the real firmware matters.

0. **Time and memory of the verify** (no network needed). Flash the sketch `.../scratchpad/overnight/design-signing/sketches/sig_ecdsa` (it was only compiled), open the serial monitor at 115200. Good: `valid=1` and `tampered=0` on every loop, a time in microseconds (expect 70,000 to 400,000; worrying above 2,000,000), `stackLeft` well above 2,000 bytes. Optionally the same with `sig_mono` (expect 10,000 to 40,000 microseconds). The way back: flash the real firmware again.
1. **Compile and size** of the real firmware (flash +1 to 3 KB; the proof build of the firmware plus the same function, +540 bytes, is in section 1.1).
2. **Positive path.** Release 0.11.0 signed in CI. A board on 0.10.1 takes it through the **unsigned** check (bootstrap: it must install; the new fields are ignored). Then `GET /api/update` shows `"signing":{"required":true,"keys":["<id A>","<id B>"],"refused":null}`. The next release 0.11.1 (any change): Check now, the notice says "Signed with key <id>", Update, code, install, restart, probation passes, the problem list stays empty. The panel never freezes for more than a second during the check (watch the panel while pressing Check now ten times).
3. **Negative tests with a test image** (a service image whose firmware folder holds a mis-signed manifest; the lead deploys it and rolls back to the known-good image after each; the download route must never be requested: check the service log shows the manifest request and no `ledmatrix-*.bin` request). Each: Check now, then `GET /api/update`, the portal, the problem list:
   - unsigned manifest (v1 fields only) -> no notice, `refused:"unsigned"`, the "not signed" sentence;
   - signed by an outsider's key (a throwaway test key generated by the test tool, never the real one) with that key's id -> `refused:"unknown_key"`;
   - the genuine manifest with one flipped bit in `sig` -> `refused:"bad_signature"`;
   - the genuine signed manifest with `notes` edited, or `size` plus one, or `version` raised -> `bad_signature` (a valid signature over different fields);
   - `sig` of the wrong length, or only `keyid` -> the "firmware details are not valid" sentence;
   - each case: nothing written (the running slot and `otadata` unchanged: `GET /api/update` shows `state: idle`), no reboot, the display still drawing, and the device-signed data fetches unaffected.
   `tools/make_test_manifests.py` (a test-only tool, written in the slice) produces these manifests from the signed one.
4. **A display whose key the service refuses still gets the signed fix**: add the board's ID to `REVOKED_DEVICES`, confirm the data routes answer 401 `revoked`, then Check now: the signed manifest is accepted and the update can proceed; undo the revocation afterwards.
5. **Second key slot**: a test build (flashed over USB) that pins two throwaway test keys `[T1, T2]` and a test service image with a manifest signed by T2: accepted; signed by T3: `unknown_key`. This proves the spare-key path used by the rotation.
6. **Development build**: flashed over USB with `-DUPDATE_REQUIRE_SIGNATURE=0`: `signing.required:false`, the portal shows the warning, an unsigned manifest is offered. Flash a normal build afterwards.
7. **The upload escape route** still works: a `.bin` uploaded with the panel code installs on a signing build.
8. **Rollback** still works for a signed release that fails its probation (the existing test).
Way back for everything: the USB-flashable known-good build (commit 4fd536d, or the last CI build), and the known-good service image (`kubectl set image` to the pinned tag). What could go wrong: verify slower than 2 s (switch to Monocypher); `heap` too low for the verify during the update window (it would show as "could not check", never as an attack); a CI secret that does not match the pinned key (the sign step fails: nothing ships); a board that took 0.11.0 but still has no keys pinned because `UPDATE_KEY_COUNT` was changed after the fact (the build asserts).

## 7. Open points the lead should know

- The vectors' keys are throwaway. They must never be used as release keys; the contract says so, and `update_keys.h` does not contain them.
- If the owner wants the first signed release flashed by USB, the one-time unsigned trust disappears for his display; the bench step 2 then starts from "USB flash 0.11.0" and still proves the signed path with 0.11.1.
- The overnight batch bumps `version.h` to 0.11.0 for all firmware changes (PROGRESS.md); this slice must be in that release, or the bootstrap needs a further release.
- Not decided here, because it does not need a decision: whether to add the same check to the `Check now` button's rate (it already costs a fetch). Nothing is needed.
- No change to `THIRD_PARTY_NOTICES.md`: no third-party code is added.
