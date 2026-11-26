// The signed firmware manifest, the board's side (ticket 50; the contract is shared/manifest-signing.md, the known-answer vectors are
// shared/manifest-signing-vectors.json). Pure logic with no Arduino, ESP or mbedTLS headers, so it is tested on the laptop
// (tools/test_updatesig.cpp). The cryptography sits behind the small `Crypto` interface: ota.cpp plugs in updatesig_mbedtls.h (the mbedTLS that
// is already in the core), the host test plugs in the same file with a host mbedTLS, or a stand-in that knows the vectors.
//
// What is in here:
//   canonicalBytes   the six lines that are signed, built from the VALUES of a parsed manifest (never from the JSON text)
//   Keys, PinnedIds  the public keys built into the firmware (update_keys.h) and the 8-hex ids derived from them (never typed by hand)
//   Sig              the outcome of looking at one manifest: Ok / NotChecked (a development build) / Unsigned / Malformed / UnknownKey /
//                    BadSignature / CheckFailed. The verifier is three-valued on purpose: a check that could not run (out of memory, an
//                    internal error) is CheckFailed and says so; it is never reported as a bad signature, which would call a fault an attack
//   checkManifest    the order of contract section 5.1: policy, shape, key id (cheap refusals, no cryptography), then one verification per
//                    matching key (at most 2)
//   the words        which Fail a refusal is, the code the portal gets in `signing.refused`, the `signing` JSON, the image marker
//
// Nothing here prints or returns a signature, a key or the canonical text. Key ids are public and may be shown.
#pragma once
#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "otalogic.h"
#include "update_keys.h"

// ---- the build's own settings, checked while compiling ---------------------------------------------------------------------------
#ifndef UPDATE_REQUIRE_SIGNATURE
#error "update_keys.h must define UPDATE_REQUIRE_SIGNATURE (1: updates must be signed, 0: development builds only)"
#endif
#ifndef UPDATE_KEY_COUNT
#error "update_keys.h must define UPDATE_KEY_COUNT (1 or 2) and UPDATE_KEY_0 (and UPDATE_KEY_1)"
#endif
#if UPDATE_KEY_COUNT > 2
#error "UPDATE_KEY_COUNT is at most 2 (the current key and the spare)"
#endif

namespace updatesig {

static const size_t MAX_KEYS = 2;
static const size_t PUBKEY_LEN = 65;       // 0x04, X, Y (32 bytes each): the uncompressed point
static const size_t PUBKEY_HEX_LEN = 130;
static const size_t CANONICAL_MAX = 1024;  // the longest canonical text is under this (the contract's CANONICAL_MAX)
static const char CANONICAL_PREFIX[] = "ledmatrix-update-v1";  // the domain-separation line

// ---- the build's own settings: constexpr, so a bad update_keys.h stops the build ---------------------------------------------------
constexpr size_t textLen(const char *s) {
  size_t n = 0;
  while (s[n]) n++;
  return n;
}
// A key as written in update_keys.h: exactly 130 lowercase hex characters, the first byte 0x04. (That it is a point ON the curve is checked by
// tools/test_updatesig.cpp and the Python tools with real arithmetic, and again by the board before it uses the key.)
constexpr bool keyTextOk(const char *s) {
  if (textLen(s) != PUBKEY_HEX_LEN || s[0] != '0' || s[1] != '4') return false;
  for (size_t i = 0; i < PUBKEY_HEX_LEN; i++)
    if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
  return true;
}
constexpr bool sameText(const char *a, const char *b) {
  size_t i = 0;
  while (a[i] && a[i] == b[i]) i++;
  return a[i] == b[i];
}

static_assert(UPDATE_REQUIRE_SIGNATURE == 0 || UPDATE_REQUIRE_SIGNATURE == 1, "UPDATE_REQUIRE_SIGNATURE is 0 (development builds only) or 1");
static_assert(UPDATE_KEY_COUNT >= 0 && UPDATE_KEY_COUNT <= 2, "UPDATE_KEY_COUNT is 1 or 2");
static_assert(UPDATE_REQUIRE_SIGNATURE == 0 || UPDATE_KEY_COUNT >= 1,
              "a build that requires signed updates needs at least one public key in update_keys.h (a build that could verify nothing must never be made)");
#if UPDATE_KEY_COUNT >= 1
static_assert(keyTextOk(UPDATE_KEY_0), "UPDATE_KEY_0 must be exactly 130 lowercase hex characters starting with 04 (the uncompressed P-256 point)");
#endif
#if UPDATE_KEY_COUNT >= 2
static_assert(keyTextOk(UPDATE_KEY_1), "UPDATE_KEY_1 must be exactly 130 lowercase hex characters starting with 04 (the uncompressed P-256 point)");
static_assert(!sameText(UPDATE_KEY_0, UPDATE_KEY_1), "the two pinned keys must differ");
#endif

// Do updates have to be signed on this build? (update_keys.h: 1 unless a development build says otherwise.)
static const bool REQUIRED = UPDATE_REQUIRE_SIGNATURE != 0;
inline const char *modeName() { return REQUIRED ? "required" : "off"; }  // /status "updateSigning"

// What the image says about it, for tools/sign_manifest.py (shared/manifest-signing.md, section 4): the bytes
//   \x01LEDMATRIX-FW-SIGNING\x02required\x03   (enforcement on)        \x01LEDMATRIX-FW-SIGNING\x02off\x03   (a development build)
// ota.cpp embeds exactly one of them, like the version marker. The firmware holds no other copy of the whole marker, so the one the sign tool
// finds is the one this build carries. CAUTION: it holds control bytes; it is for reading back from the image only, never for JSON or a log.
#define UPDATESIG_TAG_PREFIX "\x01" "LEDMATRIX-FW-SIGNING" "\x02"
#if UPDATE_REQUIRE_SIGNATURE
#define UPDATESIG_TAG_MODE "required"
#else
#define UPDATESIG_TAG_MODE "off"
#endif
#define UPDATESIG_TAG UPDATESIG_TAG_PREFIX UPDATESIG_TAG_MODE "\x03"

// ---- the cryptography, behind a seam ---------------------------------------------------------------------------------------------
enum class Verify : uint8_t {
  Valid,    // the signature checks out
  Invalid,  // it does not (any reason: wrong key, a flipped bit, other fields, r or s out of range, a key that is not on the curve)
  CouldNot  // the library could not run (out of memory, an internal error): says nothing about the signature
};

struct Crypto {
  virtual ~Crypto() {}
  // SHA-256 of msg. False when it could not be computed.
  virtual bool sha256(const uint8_t *msg, size_t len, uint8_t out[32]) = 0;
  // ECDSA on P-256: is `sig` (r then s, 32 bytes each, big endian) a valid signature of the 32-byte digest `hash` by the key `pub` (65 bytes)?
  virtual Verify p256Verify(const uint8_t pub[PUBKEY_LEN], const uint8_t hash[32], const uint8_t sig[otalogic::SIG_RAW_LEN]) = 0;
};

// ---- keys ------------------------------------------------------------------------------------------------------------------------
struct Keys {
  size_t count = 0;                                // 1 or 2
  const char *hex[MAX_KEYS] = {nullptr, nullptr};  // each 130 lowercase hex characters, 04 first
};

// The keys this build pins (update_keys.h).
inline Keys pinnedKeys() {
  Keys k;
#if UPDATE_KEY_COUNT >= 1
  k.hex[k.count++] = UPDATE_KEY_0;
#endif
#if UPDATE_KEY_COUNT >= 2
  k.hex[k.count++] = UPDATE_KEY_1;
#endif
  return k;
}

// A public key as text -> its 65 bytes. Strict: 130 lowercase hex characters, the first byte 0x04.
inline bool decodePublicKey(const char *hex, uint8_t out[PUBKEY_LEN]) {
  if (!hex || strlen(hex) != PUBKEY_HEX_LEN) return false;
  uint8_t tmp[PUBKEY_LEN];
  if (!otalogic::hexDecodeLower(hex, PUBKEY_HEX_LEN, tmp, sizeof(tmp)) || tmp[0] != 0x04) return false;
  memcpy(out, tmp, PUBKEY_LEN);
  return true;
}

// The key id: the first 4 bytes of SHA-256 of the 65-byte public key, as 8 lowercase hex characters. It only selects which pinned key to try;
// the signature decides. Derived, never typed by hand, so the header cannot disagree with itself.
inline bool keyIdOf(Crypto &crypto, const uint8_t pub[PUBKEY_LEN], char out[otalogic::KEYID_HEX_LEN + 1]) {
  uint8_t d[32];
  if (!crypto.sha256(pub, PUBKEY_LEN, d)) return false;
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < otalogic::KEYID_HEX_LEN / 2; i++) {
    out[2 * i] = hex[d[i] >> 4];
    out[2 * i + 1] = hex[d[i] & 15];
  }
  out[otalogic::KEYID_HEX_LEN] = 0;
  return true;
}

// The ids of the pinned keys, for /api/update ("signing": {"keys": [...]}).
struct PinnedIds {
  size_t count = 0;
  char id[MAX_KEYS][otalogic::KEYID_HEX_LEN + 1] = {};
};
inline bool derivePinnedIds(const Keys &keys, Crypto &crypto, PinnedIds &out) {
  PinnedIds p;
  if (keys.count > MAX_KEYS) return false;
  for (size_t i = 0; i < keys.count; i++) {
    uint8_t pub[PUBKEY_LEN];
    if (!decodePublicKey(keys.hex[i], pub) || !keyIdOf(crypto, pub, p.id[i])) return false;
    p.count++;
  }
  out = p;
  return true;
}

// ---- the signature's own range ----------------------------------------------------------------------------------------------------
// r and s must each be in 1 .. n-1, n being the order of the P-256 group (the contract: "1 <= r < n and 1 <= s < n"). mbedTLS checks the same inside
// mbedtls_ecdsa_verify; checking here as well means the answer does not depend on one library version, costs nothing, and is tested without a
// library (tools/test_updatesig.cpp). Both s and n-s are in range: the "high-s twin" of a valid signature is accepted by design.
static const uint8_t P256_ORDER[32] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                       0xBC, 0xE6, 0xFA, 0xAD, 0xA7, 0x17, 0x9E, 0x84, 0xF3, 0xB9, 0xCA, 0xC2, 0xFC, 0x63, 0x25, 0x51};
inline bool scalarInRange(const uint8_t x[32]) {  // 32 bytes, big endian
  bool zero = true;
  for (size_t i = 0; i < 32; i++)
    if (x[i]) zero = false;
  if (zero) return false;
  for (size_t i = 0; i < 32; i++) {
    if (x[i] < P256_ORDER[i]) return true;
    if (x[i] > P256_ORDER[i]) return false;
  }
  return false;  // equal to n
}

// ---- what is signed --------------------------------------------------------------------------------------------------------------
// Six lines joined by a line feed, no line feed after the last, UTF-8 (the values as the manifest holds them after decoding):
//   ledmatrix-update-v1 / version=<v> / file=<f> / size=<decimal> / sha256=<hex> / notes=<text>
// False when it does not fit in `cap` (a NUL is added) or a field holds a control character, which would make the text ambiguous (parseManifest
// refuses such a manifest long before; this is the builder's own guard). `len` is the number of bytes, without the NUL.
inline bool hasControl(const char *s) {
  for (; *s; s++)
    if ((uint8_t)*s < 0x20 || (uint8_t)*s == 0x7F) return true;
  return false;
}
inline bool canonicalBytes(const otalogic::Manifest &m, char *out, size_t cap, size_t &len) {
  len = 0;
  if (!out || cap == 0) return false;
  if (hasControl(m.version) || hasControl(m.file) || hasControl(m.sha256) || hasControl(m.notes)) return false;
  int n = snprintf(out, cap, "%s\nversion=%s\nfile=%s\nsize=%lu\nsha256=%s\nnotes=%s", CANONICAL_PREFIX, m.version, m.file, (unsigned long)m.size, m.sha256, m.notes);
  if (n < 0 || (size_t)n >= cap) return false;
  len = (size_t)n;
  return true;
}

// ---- the verdict -----------------------------------------------------------------------------------------------------------------
enum class Sig : uint8_t {
  Ok,           // a pinned key verified the signature: the manifest is the maintainer's
  NotChecked,   // a development build (UPDATE_REQUIRE_SIGNATURE 0): no signature is looked at
  Unsigned,     // no keyid and no sig
  Malformed,    // only one of the two, or a field of the wrong shape (see SigFields)
  UnknownKey,   // keyid is not a pinned key's id; no verification has run
  BadSignature, // a pinned key and a signature that does not verify
  CheckFailed   // the check could not run: not an attack, tried again at the next check
};

struct SigResult {
  Sig sig = Sig::CheckFailed;
  char keyid[otalogic::KEYID_HEX_LEN + 1] = "";  // the key that verified, when Ok
};

// A refusal is any verdict that stops the update (everything but Ok and NotChecked).
inline bool isRefusal(Sig s) { return s != Sig::Ok && s != Sig::NotChecked; }

// The order of contract section 5.1. `required` is the build's policy (REQUIRED); the tests pass both. Allocation-free except for what `crypto` does.
//  1. not required: NotChecked, nothing else runs (the fields, well formed or not, are ignored)
//  2. shape: Absent -> Unsigned, Malformed -> Malformed
//  3. key id: derive the ids of the pinned keys (SHA-256 of 65 bytes each); none equals keyid -> UnknownKey (the cheap refusals are before any
//     verification)
//  4. r and s in range (1 .. n-1), else BadSignature; canonical bytes, SHA-256, one verification per pinned key that matches (at most 2). Any valid -> Ok. Otherwise any "could not run" ->
//     CheckFailed (a verification that did not run does not say the signature is bad, even if the other said so); otherwise BadSignature.
inline SigResult checkManifest(bool required, const otalogic::Manifest &m, const Keys &keys, Crypto &crypto) {
  SigResult r;
  if (!required) { r.sig = Sig::NotChecked; return r; }
  if (m.sigFields == otalogic::SigFields::Absent) { r.sig = Sig::Unsigned; return r; }
  if (m.sigFields != otalogic::SigFields::Present) { r.sig = Sig::Malformed; return r; }  // Malformed, or a value this code does not know

  if (keys.count > MAX_KEYS) return r;  // CheckFailed: a build that pins more keys than it can hold is a build fault, not a bad signature
  uint8_t pub[MAX_KEYS][PUBKEY_LEN];
  bool match[MAX_KEYS] = {false, false};
  bool any = false;
  for (size_t i = 0; i < keys.count; i++) {
    char id[otalogic::KEYID_HEX_LEN + 1];
    if (!decodePublicKey(keys.hex[i], pub[i]) || !keyIdOf(crypto, pub[i], id)) return r;  // CheckFailed
    match[i] = memcmp(id, m.keyid, otalogic::KEYID_HEX_LEN) == 0;
    any = any || match[i];
  }
  if (!any) { r.sig = Sig::UnknownKey; return r; }
  if (!scalarInRange(m.sig) || !scalarInRange(m.sig + 32)) { r.sig = Sig::BadSignature; return r; }  // r or s outside 1..n-1: no signature, no work to do

  char text[CANONICAL_MAX];
  size_t len = 0;
  uint8_t digest[32];
  if (!canonicalBytes(m, text, sizeof(text), len) || !crypto.sha256((const uint8_t *)text, len, digest)) return r;  // CheckFailed

  bool couldNot = false;
  for (size_t i = 0; i < keys.count; i++) {
    if (!match[i]) continue;
    Verify v = crypto.p256Verify(pub[i], digest, m.sig);
    if (v == Verify::Valid) {
      r.sig = Sig::Ok;
      memcpy(r.keyid, m.keyid, sizeof(r.keyid));
      return r;
    }
    if (v == Verify::CouldNot) couldNot = true;
  }
  r.sig = couldNot ? Sig::CheckFailed : Sig::BadSignature;
  return r;
}

// ---- the words -------------------------------------------------------------------------------------------------------------------
// The Fail (otalogic.h) a refusal is. Malformed is the existing "manifest is not valid", with SIGNATURE_FIELDS_MALFORMED as the detail.
inline otalogic::Fail failFor(Sig s) {
  switch (s) {
    case Sig::Unsigned: return otalogic::Fail::Unsigned;
    case Sig::Malformed: return otalogic::Fail::ManifestInvalid;
    case Sig::UnknownKey: return otalogic::Fail::UnknownKey;
    case Sig::BadSignature: return otalogic::Fail::BadSignature;
    case Sig::CheckFailed: return otalogic::Fail::SignatureCheckFailed;
    case Sig::Ok:
    case Sig::NotChecked: return otalogic::Fail::None;
  }
  return otalogic::Fail::None;
}
inline const char *refusalDetail(Sig s) { return s == Sig::Malformed ? otalogic::SIGNATURE_FIELDS_MALFORMED : ""; }

// The code the portal gets in "signing": {"refused": ...}; nullptr (JSON null) when the verdict is not a refusal.
inline const char *refusedName(Sig s) {
  switch (s) {
    case Sig::Unsigned: return "unsigned";
    case Sig::Malformed: return "malformed";
    case Sig::UnknownKey: return "unknown_key";
    case Sig::BadSignature: return "bad_signature";
    case Sig::CheckFailed: return "check_failed";
    case Sig::Ok:
    case Sig::NotChecked: return nullptr;
  }
  return nullptr;
}

// "signing" in GET /api/update (contract 5.3): {"required":true,"keys":["f864ea3b","db10c343"],"refused":null|"unsigned"|...}. The key ids are public.
// ArduinoJson may keep a pointer into `ids`: call it under the lock that guards the state and serialise before the lock is released.
inline void fillSigningJson(JsonObject o, bool required, const PinnedIds &ids, Sig lastRefusal) {
  o["required"] = required;
  JsonArray keys = o["keys"].to<JsonArray>();
  for (size_t i = 0; i < ids.count && i < MAX_KEYS; i++) keys.add(ids.id[i]);
  const char *refused = refusedName(lastRefusal);
  if (refused) o["refused"] = refused;
  else o["refused"] = nullptr;
}

// "signedBy" in GET /api/update's "available": the id of the key that verified this manifest, null when nothing verified it (a development build).
inline void fillSignedBy(JsonObject available, const char *keyid) {
  if (keyid && keyid[0]) available["signedBy"] = keyid;
  else available["signedBy"] = nullptr;
}

}  // namespace updatesig
